/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

/* KMS/DRM context, running without any window manager.
 * Based on kmscube example by Rob Clark.
 */

#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

#include <sched.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>

#include <libdrm/drm.h>
#include <gbm.h>

#include <lists/dir_list.h>
#include <string/stdstring.h>

#include "../../configuration.h"
#include "../../verbosity.h"
#include "../../frontend/frontend_driver.h"
#include "../common/drm_common.h"

#include <go2/display.h>
#include <drm/drm_fourcc.h>

#ifdef HAVE_EGL
#include "../common/egl_common.h"
#endif

#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
#include "../common/gl_common.h"
#endif

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#ifdef HAVE_OPENGLES

#ifndef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT_KHR 0x0040
#endif

#endif

#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif

static enum gfx_ctx_api drm_api           = GFX_CTX_NONE;


typedef struct gfx_ctx_drm_data
{
#ifdef HAVE_EGL
   egl_ctx_data_t egl;
#endif
   go2_display_t* display;
   go2_presenter_t* presenter;
   go2_context_t* context;
   int interval;
   unsigned fb_width;
   unsigned fb_height;

   bool core_hw_context_enable;
} gfx_ctx_drm_data_t;


static void gfx_ctx_drm_input_driver(void *data,
      const char *joypad_name,
      input_driver_t **input, void **input_data)
{
#ifdef HAVE_X11
   settings_t *settings = config_get_ptr();

   /* We cannot use the X11 input driver for DRM/KMS */
   if (string_is_equal(settings->arrays.input_driver, "x"))
   {
#ifdef HAVE_UDEV
      {
         /* Try to set it to udev instead */
         void *udev = input_udev.init(joypad_name);
         if (udev)
         {
            *input       = &input_udev;
            *input_data  = udev;
            return;
         }
      }
#endif
#if defined(__linux__) && !defined(ANDROID)
      {
         /* Try to set it to linuxraw instead */
         void *linuxraw = input_linuxraw.init(joypad_name);
         if (linuxraw)
         {
            *input       = &input_linuxraw;
            *input_data  = linuxraw;
            return;
         }
      }
#endif
   }
#endif

   *input      = NULL;
   *input_data = NULL;
}

static gfx_ctx_proc_t gfx_ctx_drm_get_proc_address(const char *symbol)
{
   switch (drm_api)
   {
      case GFX_CTX_OPENGL_API:
      case GFX_CTX_OPENGL_ES_API:
      case GFX_CTX_OPENVG_API:
#ifdef HAVE_EGL
         return egl_get_proc_address(symbol);
#endif
      case GFX_CTX_NONE:
      default:
         break;
   }

   return NULL;
}

static void *gfx_ctx_drm_init(video_frame_info_t *video_info, void *video_driver)
{
   gfx_ctx_drm_data_t *drm              = (gfx_ctx_drm_data_t*)
      calloc(1, sizeof(gfx_ctx_drm_data_t));

   if (!drm)
      return NULL;

   drm->display = go2_display_create();
   drm->presenter = go2_presenter_create(drm->display, DRM_FORMAT_RGB565, 0xff080808);

   return drm;
}

static void gfx_ctx_drm_destroy(void *data)
{
   gfx_ctx_drm_data_t *drm = (gfx_ctx_drm_data_t*)data;
   if (!drm) return;

   if (drm->context)
   {
      go2_context_destroy(drm->context);
      drm->context = NULL;
   }

   go2_presenter_destroy(drm->presenter);
   drm->presenter = NULL;

   go2_display_destroy(drm->display);
   drm->display = NULL;
}

static enum gfx_ctx_api gfx_ctx_drm_get_api(void *data)
{
   return drm_api;
}

static bool gfx_ctx_drm_bind_api(void *video_driver,
      enum gfx_ctx_api api, unsigned major, unsigned minor)
{
   (void)video_driver;

   drm_api     = api;
#ifdef HAVE_EGL
   g_egl_major = major;
   g_egl_minor = minor;
#endif

   switch (api)
   {
      case GFX_CTX_OPENGL_API:
#if defined(HAVE_EGL) && defined(HAVE_OPENGL)

#ifndef EGL_KHR_create_context
         if ((major * 1000 + minor) >= 3001)
            return false;
#endif
         return egl_bind_api(EGL_OPENGL_API);
#else
         break;
#endif
      case GFX_CTX_OPENGL_ES_API:
#if defined(HAVE_EGL) && defined(HAVE_OPENGLES)

#ifndef EGL_KHR_create_context
         if (major >= 3)
            return false;
#endif
         return egl_bind_api(EGL_OPENGL_ES_API);
#else
         break;
#endif
      case GFX_CTX_OPENVG_API:
#if defined(HAVE_EGL) && defined(HAVE_VG)
         return egl_bind_api(EGL_OPENVG_API);
#endif
      case GFX_CTX_NONE:
      default:
         break;
   }

   return false;
}

static void gfx_ctx_drm_swap_interval(void *data, int interval)
{
   gfx_ctx_drm_data_t *drm = (gfx_ctx_drm_data_t*)data;
   drm->interval           = interval;

   if (interval > 1)
      RARCH_WARN("[KMS]: Swap intervals > 1 currently not supported. Will use swap interval of 1.\n");
}

static bool gfx_ctx_drm_set_video_mode(void *data,
      video_frame_info_t *video_info,
      unsigned width, unsigned height,
      bool fullscreen)
{
   gfx_ctx_drm_data_t *drm     = (gfx_ctx_drm_data_t*)data;

   if (!drm)
      return false;

   frontend_driver_install_signal_handler();

   drm->fb_width    = 480;
   drm->fb_height   = 320;

   if (!drm->context)
   {
      go2_context_attributes_t attr;
      attr.major = 3;
      attr.minor = 2;
      attr.red_bits = 8;
      attr.green_bits = 8;
      attr.blue_bits = 8;
      attr.alpha_bits = 8;
      attr.depth_bits = 0;
      attr.stencil_bits = 0;

      drm->context = go2_context_create(drm->display, 480, 320, &attr);
   }

   go2_context_make_current(drm->context);

   glClear(GL_COLOR_BUFFER_BIT);

   return true;
}

static void gfx_ctx_drm_get_video_size(void *data,
      unsigned *width, unsigned *height)
{
   gfx_ctx_drm_data_t *drm = (gfx_ctx_drm_data_t*)data;

   if (!drm)
      return;

   *width  = drm->fb_width;
   *height = drm->fb_height;
}

static void gfx_ctx_drm_check_window(void *data, bool *quit,
      bool *resize, unsigned *width, unsigned *height, bool is_shutdown)
{
   (void)data;
   (void)width;
   (void)height;

   *resize = false;
   *quit   = (bool)frontend_driver_get_signal_handler_state();
}

static bool gfx_ctx_drm_has_focus(void *data)
{
   return true;
}

static bool gfx_ctx_drm_suppress_screensaver(void *data, bool enable)
{
   (void)data;
   (void)enable;
   return false;
}

static void gfx_ctx_drm_swap_buffers(void *data, void *data2)
{
   gfx_ctx_drm_data_t        *drm = (gfx_ctx_drm_data_t*)data;
   video_frame_info_t *video_info = (video_frame_info_t*)data2;

   switch (drm_api)
   {
      case GFX_CTX_OPENGL_API:
      case GFX_CTX_OPENGL_ES_API:
      case GFX_CTX_OPENVG_API:
#ifdef HAVE_EGL
         //egl_swap_buffers(&drm->egl);
         go2_context_swap_buffers(drm->context);

         go2_surface_t* surface = go2_context_surface_lock(drm->context);
         go2_presenter_post(drm->presenter,
                     surface,
                     0, 0, 480, 320,
                     0, 0, 320, 480,
                     GO2_ROTATION_DEGREES_270);
         go2_context_surface_unlock(drm->context, surface);
#endif
         break;
      default:
         printf("unhandled gfx_ctx_drm_swap_buffers\n");
         break;
   }
}

static uint32_t gfx_ctx_drm_get_flags(void *data)
{
   uint32_t             flags = 0;
   gfx_ctx_drm_data_t    *drm = (gfx_ctx_drm_data_t*)data;

   BIT32_SET(flags, GFX_CTX_FLAGS_CUSTOMIZABLE_SWAPCHAIN_IMAGES);

   if (drm->core_hw_context_enable)
      BIT32_SET(flags, GFX_CTX_FLAGS_GL_CORE_CONTEXT);

   if (string_is_equal(video_driver_get_ident(), "glcore"))
   {
#if defined(HAVE_SLANG) && defined(HAVE_SPIRV_CROSS)
      BIT32_SET(flags, GFX_CTX_FLAGS_SHADERS_SLANG);
#endif
   }
   else
      BIT32_SET(flags, GFX_CTX_FLAGS_SHADERS_GLSL);

   return flags;
}

static void gfx_ctx_drm_set_flags(void *data, uint32_t flags)
{
   gfx_ctx_drm_data_t *drm     = (gfx_ctx_drm_data_t*)data;
   if (BIT32_GET(flags, GFX_CTX_FLAGS_GL_CORE_CONTEXT))
      drm->core_hw_context_enable = true;
}

static void gfx_ctx_drm_bind_hw_render(void *data, bool enable)
{
   gfx_ctx_drm_data_t *drm     = (gfx_ctx_drm_data_t*)data;

   switch (drm_api)
   {
      case GFX_CTX_OPENGL_API:
      case GFX_CTX_OPENGL_ES_API:
      case GFX_CTX_OPENVG_API:
#ifdef HAVE_EGL
         egl_bind_hw_render(&drm->egl, enable);
#endif
         break;
      case GFX_CTX_NONE:
      default:
         break;
   }
}

const gfx_ctx_driver_t gfx_ctx_drm = {
   gfx_ctx_drm_init,
   gfx_ctx_drm_destroy,
   gfx_ctx_drm_get_api,
   gfx_ctx_drm_bind_api,
   gfx_ctx_drm_swap_interval,
   gfx_ctx_drm_set_video_mode,
   gfx_ctx_drm_get_video_size,
   drm_get_refresh_rate,
   NULL, /* get_video_output_size */
   NULL, /* get_video_output_prev */
   NULL, /* get_video_output_next */
   NULL, /* get_metrics */
   NULL,
   NULL, /* update_title */
   gfx_ctx_drm_check_window,
   NULL, /* set_resize */
   gfx_ctx_drm_has_focus,
   gfx_ctx_drm_suppress_screensaver,
   false, /* has_windowed */
   gfx_ctx_drm_swap_buffers,
   gfx_ctx_drm_input_driver,
   gfx_ctx_drm_get_proc_address,
   NULL,
   NULL,
   NULL,
   "kms",
   gfx_ctx_drm_get_flags,
   gfx_ctx_drm_set_flags,
   gfx_ctx_drm_bind_hw_render,
   NULL,
   NULL
};
