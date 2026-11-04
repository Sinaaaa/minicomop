/*
 * minicomp - a minimal X11 compositing manager derived from xfwm4's compositor.c
 *
 *   xcompmgr - (c) 2003 Keith Packard
 *   xfwm4    - (c) 2005-2011 Olivier Fourdan
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * What it does
 * ------------
 *   - Composites top-level windows with XRender (overlay window as output).
 *   - Window translucency: _NET_WM_WINDOW_OPACITY and ARGB visuals.
 *   - Anti-tearing, using the same strategies, in the same order, as xfwm4:
 *       auto    : GLX (swap interval + XSync fence + GL fence), unless the GL
 *                 renderer is on xfwm4's "prefer XPresent" list (Intel) or its
 *                 blacklist (software renderers); then XPresent (2 buffers);
 *                 then nothing.
 *       glx     : GLX only
 *       xpresent: XPresent only
 *       off     : plain XRenderComposite to the screen
 *
 * What it does not do (on purpose)
 * --------------------------------
 *   No shadows, no zoom, no frame opacity, no cursor painting, no
 *   unredirection of fullscreen windows, no window manager integration.
 *   It is a compositing manager only; run it next to any window manager
 *   that does not composite itself.
 *
 * Dependencies on Arch BTW
 * -----
 * sudo pacman -S libx11 libxext libxcomposite libxdamage libxfixes libxrender libxpresent libepoxy
 * 
 * 
 * 
 * Build
 * -----
 *   gcc -O2 -Wall -o minicomp minicomp.c \
 *       $(pkg-config --cflags --libs x11 xext xcomposite xdamage xfixes \
 *                    xrender xpresent epoxy)
 *
 *   Add -DNO_GLX to drop GLX/epoxy, -DNO_PRESENT to drop XPresent.
 *
 * Usage
 * -----
 *   minicomp [-d display] [--vblank=auto|glx|xpresent|off]
 */

#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/shape.h>

#ifndef NO_GLX
#define HAVE_EPOXY 1
#include <epoxy/gl.h>
#include <epoxy/glx.h>
#include <X11/extensions/sync.h>
#endif

#ifndef NO_PRESENT
#define HAVE_PRESENT_EXTENSION 1
#include <X11/extensions/Xpresent.h>
#endif

/* ------------------------------------------------------------------------ */
/* Constants (same values/semantics as xfwm4)                                */
/* ------------------------------------------------------------------------ */

#define NET_WM_OPAQUE                   0xffffffffU

#define N_BUFFERS                       2

#define TIMEOUT_REPAINT_MS              1
#define TIMEOUT_THROTTLED_REPAINT_MS    500
#define TIMEOUT_MAX_RETRIES             100

#define WIN_IS_ARGB(cw)                 ((cw)->argb)
#define WIN_IS_OPAQUE(cw)               (((cw)->opacity == NET_WM_OPAQUE) && !WIN_IS_ARGB(cw))
#define WIN_IS_VIEWABLE(cw)             ((cw)->viewable)
#define WIN_HAS_DAMAGE(cw)              ((cw)->damage)
#define WIN_IS_VISIBLE(cw)              (WIN_IS_VIEWABLE(cw) && WIN_HAS_DAMAGE(cw))
#define WIN_IS_DAMAGED(cw)              ((cw)->damaged)

typedef enum
{
    VBLANK_OFF = 0,
    VBLANK_XPRESENT,
    VBLANK_GLX,
    VBLANK_AUTO
} vblankMode;

/* ------------------------------------------------------------------------ */
/* Data structures                                                           */
/* ------------------------------------------------------------------------ */

typedef struct _CWindow CWindow;

struct _CWindow
{
    CWindow *next;                  /* towards the bottom of the stack */
    CWindow *prev;                  /* towards the top of the stack    */

    Window id;
    XWindowAttributes attr;

    bool viewable;
    bool damaged;
    bool argb;
    bool skipped;

    Damage damage;
    Pixmap name_window_pixmap;
    Picture picture;
    Picture alphaPict;

    XserverRegion extents;
    XserverRegion borderSize;
    XserverRegion borderClip;

    uint32_t opacity;
};

typedef struct
{
    Display *dpy;
    int screen;
    Window xroot;
    Visual *visual;
    int depth;
    int width;
    int height;

    /* extensions */
    int composite_major, composite_minor;
    int damage_event_base, damage_error_base;
    bool have_overlays;
    bool have_present;
    int present_opcode, present_event_base, present_error_base;
    bool have_xsync;

    Atom atom_opacity;
    Atom atom_xrootpmap;
    Atom atom_xsetroot;
    Atom atom_pixmap;

    /* output */
    Window cm_win;
    Window overlay;
    Window root_overlay;
    Window output;
    bool compositor_active;

    Picture rootPicture;
    Picture rootTile;

    vblankMode vblank_mode;

    /* windows, top of stack first */
    CWindow *cwindows;
    CWindow *cwindows_tail;

    /* damage tracking */
    XserverRegion allDamage;
    XserverRegion prevDamage;
    XserverRegion screenRegion;

    /* back buffers (1 with GLX/none, N_BUFFERS with XPresent) */
    Pixmap rootPixmap[N_BUFFERS];
    Picture rootBuffer[N_BUFFERS];
    int use_n_buffers;
    int current_buffer;

    /* repaint timer (replaces g_timeout_add_full) */
    bool timeout_active;
    int64_t timeout_due;
    int timeout_period;
    unsigned number_of_retries;

    /* vsync method */
    bool use_glx;
    bool use_present;
    bool present_pending;

#ifdef HAVE_EPOXY
    GLXContext glx_context;
    GLXWindow glx_window;
    GLXFBConfig glx_fbconfig;
    GLXPixmap glx_drawable[N_BUFFERS];
    GLuint rootTexture;
    GLint texture_type;
    GLint texture_target;
    GLint texture_format;
    bool texture_inverted;
    bool has_mesa_swap_control;
    bool has_ext_swap_control;
    bool has_ext_swap_control_tear;
    bool has_ext_arb_sync;
    GLsync gl_sync;
    XSyncFence fence[N_BUFFERS];
#endif
} ScreenInfo;

static ScreenInfo g_screen;
static volatile sig_atomic_t quit_flag = 0;

/* ------------------------------------------------------------------------ */
/* Small utilities: X error trapping, time                                   */
/* ------------------------------------------------------------------------ */

#define TRAP_MAX 64
static int trap_sp = 0;
static int trap_err[TRAP_MAX];

static int
x_error_handler (Display *dpy, XErrorEvent *ev)
{
    if (trap_sp > 0)
    {
        trap_err[trap_sp - 1] = ev->error_code;
        return 0;
    }
    else
    {
        char buf[128];

        XGetErrorText (dpy, ev->error_code, buf, sizeof (buf) - 1);
        fprintf (stderr, "minicomp: X error: %s (request %d.%d, resource 0x%lx)\n",
                 buf, ev->request_code, ev->minor_code, ev->resourceid);
    }
    return 0;
}

static void
trap_push (Display *dpy)
{
    (void) dpy;
    if (trap_sp < TRAP_MAX)
    {
        trap_err[trap_sp++] = Success;
    }
}

static int
trap_pop (Display *dpy)
{
    XSync (dpy, False);
    if (trap_sp <= 0)
    {
        return Success;
    }
    return trap_err[--trap_sp];
}

static void
trap_pop_ignored (Display *dpy)
{
    (void) trap_pop (dpy);
}

static int64_t
now_ms (void)
{
    struct timespec ts;

    clock_gettime (CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------------------ */
/* Forward declarations                                                      */
/* ------------------------------------------------------------------------ */

static void add_damage (ScreenInfo *S, XserverRegion damage);
static void damage_win (CWindow *cw);
static void damage_screen (ScreenInfo *S);

/* ------------------------------------------------------------------------ */
/* Regions, pixmaps, pictures                                                */
/* ------------------------------------------------------------------------ */

static XserverRegion
get_screen_region (ScreenInfo *S)
{
    XRectangle r;

    r.x = 0;
    r.y = 0;
    r.width = S->width;
    r.height = S->height;

    return XFixesCreateRegion (S->dpy, &r, 1);
}

static int
get_region_bounds (Display *dpy, XserverRegion region, XRectangle *bounds)
{
    XRectangle *rects;
    int nrects;

    memset (bounds, 0, sizeof (*bounds));
    rects = XFixesFetchRegionAndBounds (dpy, region, &nrects, bounds);
    if (rects)
    {
        XFree (rects);
    }
    return nrects;
}

static bool
is_region_empty (Display *dpy, XserverRegion region)
{
    XRectangle bounds;
    int nrects;

    nrects = get_region_bounds (dpy, region, &bounds);
    return (nrects == 0 || bounds.width == 0 || bounds.height == 0);
}

static Pixmap
safe_create_pixmap (ScreenInfo *S, Drawable d, int width, int height, int depth)
{
    Pixmap pixmap;

    trap_push (S->dpy);
    pixmap = XCreatePixmap (S->dpy, d, width, height, depth);
    if (trap_pop (S->dpy) != Success)
    {
        return None;
    }
    return pixmap;
}

static Pixmap
create_root_pixmap (ScreenInfo *S)
{
    return safe_create_pixmap (S, S->xroot, S->width, S->height, S->depth);
}

static Picture
create_root_buffer (ScreenInfo *S, Pixmap pixmap)
{
    XRenderPictFormat *format;

    if (pixmap == None)
    {
        return None;
    }
    format = XRenderFindVisualFormat (S->dpy, S->visual);
    if (format == NULL)
    {
        return None;
    }
    return XRenderCreatePicture (S->dpy, pixmap, format, 0, NULL);
}

static Picture
solid_picture (ScreenInfo *S, bool argb, double a, double r, double g, double b)
{
    Pixmap pixmap;
    Picture picture;
    XRenderPictureAttributes pa;
    XRenderPictFormat *render_format;
    XRenderColor c;

    render_format = XRenderFindStandardFormat (S->dpy,
                                               argb ? PictStandardARGB32 : PictStandardA8);
    if (render_format == NULL)
    {
        return None;
    }

    pixmap = XCreatePixmap (S->dpy, S->output, 1, 1, argb ? 32 : 8);
    if (pixmap == None)
    {
        return None;
    }

    pa.repeat = True;
    picture = XRenderCreatePicture (S->dpy, pixmap, render_format, CPRepeat, &pa);
    if (picture == None)
    {
        XFreePixmap (S->dpy, pixmap);
        return None;
    }

    c.alpha = a * 0xffff;
    c.red   = r * 0xffff;
    c.green = g * 0xffff;
    c.blue  = b * 0xffff;

    XRenderFillRectangle (S->dpy, PictOpSrc, picture, &c, 0, 0, 1, 1);
    XFreePixmap (S->dpy, pixmap);

    return picture;
}

static Picture
root_tile (ScreenInfo *S)
{
    Display *dpy = S->dpy;
    Picture picture = None;
    Atom backgroundProps[2];
    int p;

    backgroundProps[0] = S->atom_xrootpmap;
    backgroundProps[1] = S->atom_xsetroot;

    for (p = 0; p < 2; p++)
    {
        Atom actual_type;
        int actual_format;
        unsigned long nitems;
        unsigned long bytes_after;
        unsigned char *prop = NULL;
        int result;

        result = XGetWindowProperty (dpy, S->xroot, backgroundProps[p],
                                     0, 4, False, AnyPropertyType,
                                     &actual_type, &actual_format,
                                     &nitems, &bytes_after, &prop);

        if ((result == Success) &&
            (actual_type == S->atom_pixmap) &&
            (actual_format == 32) &&
            (nitems == 1) && prop)
        {
            Pixmap pixmap = *(Pixmap *) prop;
            XRenderPictureAttributes pa;
            XRenderPictFormat *format;

            XFree (prop);
            pa.repeat = True;
            format = XRenderFindVisualFormat (dpy, S->visual);
            if (format)
            {
                trap_push (dpy);
                picture = XRenderCreatePicture (dpy, pixmap, format, CPRepeat, &pa);
                if (trap_pop (dpy) != Success)
                {
                    picture = None;
                }
            }
            if (picture != None)
            {
                break;
            }
        }
        else if (prop)
        {
            XFree (prop);
        }
    }

    if (picture == None)
    {
        XRenderColor c;

        /* Background defaults to just plain black */
        c.red   = 0x0000;
        c.green = 0x0000;
        c.blue  = 0x0000;
        c.alpha = 0xffff;
        picture = XRenderCreateSolidFill (dpy, &c);
    }
    return picture;
}

/* ------------------------------------------------------------------------ */
/* GLX                                                                       */
/* ------------------------------------------------------------------------ */

#ifdef HAVE_EPOXY

static bool
check_glx_renderer (ScreenInfo *S)
{
    const char *glRenderer;
    const char *blacklisted[] = {
        "Software Rasterizer",
        "Mesa X11",
        "llvmpipe",
        "softpipe",
        "SVGA3D",
        "virgl",
        NULL
    };
#ifdef HAVE_PRESENT_EXTENSION
    const char *prefer_xpresent[] = {
        "Intel",
        /* Cannot add AMD and Radeon until the fix for
         * https://gitlab.freedesktop.org/xorg/driver/xf86-video-amdgpu/-/issues/10
         * is included in a release.
         */
        NULL
    };
#endif
    int i;

    glRenderer = (const char *) glGetString (GL_RENDERER);
    if (glRenderer == NULL)
    {
        fprintf (stderr, "minicomp: Cannot identify GLX renderer.\n");
        return false;
    }

#ifdef HAVE_PRESENT_EXTENSION
    if (S->vblank_mode == VBLANK_AUTO)
    {
        i = 0;
        while (prefer_xpresent[i] && !strcasestr (glRenderer, prefer_xpresent[i]))
        {
            i++;
        }
        if (prefer_xpresent[i])
        {
            fprintf (stderr, "minicomp: Prefer XPresent with %s\n", glRenderer);
            return false;
        }
    }
#else
    (void) S;
#endif

    i = 0;
    while (blacklisted[i] && !strcasestr (glRenderer, blacklisted[i]))
    {
        i++;
    }
    if (blacklisted[i])
    {
        fprintf (stderr, "minicomp: Unsupported GL renderer (%s).\n", glRenderer);
        return false;
    }

    return true;
}

static bool
check_gl_extensions (ScreenInfo *S)
{
    /* Note: the GL context must be current for this to work */
    if (S->texture_type == GL_TEXTURE_2D)
    {
        /*
         * If all we have is GL_TEXTURE_2D then we ought to have
         * GL_ARB_texture_non_power_of_two, otherwise we'll fail.
         */
        return epoxy_has_gl_extension ("GL_ARB_texture_non_power_of_two");
    }
    return true;
}

static bool
choose_glx_settings (ScreenInfo *S)
{
    static GLint visual_attribs[] = {
        GLX_DRAWABLE_TYPE, GLX_PIXMAP_BIT | GLX_WINDOW_BIT,
        GLX_X_RENDERABLE,  True,
        GLX_DOUBLEBUFFER,  True,
        GLX_CONFIG_CAVEAT, GLX_NONE,
        GLX_DEPTH_SIZE,    1,
        GLX_RED_SIZE,      1,
        GLX_GREEN_SIZE,    1,
        GLX_BLUE_SIZE,     1,
        GLX_RENDER_TYPE,   GLX_RGBA_BIT,
        None
    };
    GLint texture_type = 0;
    GLint texture_target = 0;
    GLint texture_format = 0;
    bool texture_inverted = false;
    int n_configs, i;
    int value, status;
    GLXFBConfig *configs, fb_config = NULL;
    XVisualInfo *visual_info;
    bool fb_match;
    VisualID xvisual_id;

    configs = glXChooseFBConfig (S->dpy, S->screen, visual_attribs, &n_configs);
    if (configs == NULL)
    {
        fprintf (stderr, "minicomp: Cannot retrieve GLX frame buffer config.\n");
        return false;
    }

    fb_match = false;
    xvisual_id = XVisualIDFromVisual (S->visual);
    for (i = 0; i < n_configs; i++)
    {
        visual_info = glXGetVisualFromFBConfig (S->dpy, configs[i]);
        if (!visual_info)
        {
            continue;
        }

        if (visual_info->visualid != xvisual_id)
        {
            XFree (visual_info);
            continue;
        }
        XFree (visual_info);

        status = glXGetFBConfigAttrib (S->dpy, configs[i], GLX_DRAWABLE_TYPE, &value);
        if (status != Success || !(value & GLX_PIXMAP_BIT))
        {
            continue;
        }

        status = glXGetFBConfigAttrib (S->dpy, configs[i],
                                       GLX_BIND_TO_TEXTURE_TARGETS_EXT, &value);
        if (status != Success)
        {
            continue;
        }

        if (value & GLX_TEXTURE_RECTANGLE_BIT_EXT)
        {
            texture_type = GL_TEXTURE_RECTANGLE_ARB;
            texture_target = GLX_TEXTURE_RECTANGLE_EXT;
        }
        else if (value & GLX_TEXTURE_2D_BIT_EXT)
        {
            texture_type = GL_TEXTURE_2D;
            texture_target = GLX_TEXTURE_2D_EXT;
        }
        else
        {
            continue;
        }

        status = glXGetFBConfigAttrib (S->dpy, configs[i], GLX_RED_SIZE, &value);
        if (status == Success && value > 8)
        {
            /* RGB10 config, skipped */
            continue;
        }

        status = glXGetFBConfigAttrib (S->dpy, configs[i],
                                       GLX_BIND_TO_TEXTURE_RGBA_EXT, &value);
        if (status == Success && value == True)
        {
            texture_format = GLX_TEXTURE_FORMAT_RGBA_EXT;
        }
        else
        {
            status = glXGetFBConfigAttrib (S->dpy, configs[i],
                                           GLX_BIND_TO_TEXTURE_RGB_EXT, &value);
            if (status == Success && value == True)
            {
                texture_format = GLX_TEXTURE_FORMAT_RGB_EXT;
            }
            else
            {
                continue;
            }
        }

        fb_config = configs[i];
        fb_match = true;
        break;
    }
    XFree (configs);

    if (!fb_match)
    {
        fprintf (stderr, "minicomp: Cannot find a matching visual for the frame buffer config.\n");
        return false;
    }

    S->texture_type = texture_type;
    S->texture_target = texture_target;
    S->texture_format = texture_format;
    S->texture_inverted = texture_inverted;
    S->glx_fbconfig = fb_config;

    return true;
}

static void
free_glx_data (ScreenInfo *S)
{
    trap_push (S->dpy);

    if (S->has_ext_arb_sync && S->gl_sync)
    {
#if defined (glDeleteSync)
        glDeleteSync (S->gl_sync);
#endif
        S->gl_sync = 0;
    }

    glXMakeCurrent (S->dpy, None, NULL);

    if (S->glx_context)
    {
        glXDestroyContext (S->dpy, S->glx_context);
        S->glx_context = None;
    }

    if (S->glx_window)
    {
        glXDestroyWindow (S->dpy, S->glx_window);
        S->glx_window = None;
    }
    trap_pop_ignored (S->dpy);
}

static bool
init_glx (ScreenInfo *S)
{
    int error_base, event_base;
    int version;

    if (!glXQueryExtension (S->dpy, &error_base, &event_base))
    {
        fprintf (stderr, "minicomp: GLX extension missing, GLX support disabled.\n");
        return false;
    }

    version = epoxy_glx_version (S->dpy, S->screen);
    if (version < 13)
    {
        fprintf (stderr, "minicomp: GLX version %d is too old, GLX support disabled.\n", version);
        return false;
    }

    if (!epoxy_has_glx_extension (S->dpy, S->screen, "GLX_EXT_texture_from_pixmap"))
    {
        fprintf (stderr, "minicomp: GLX_EXT_texture_from_pixmap missing, GLX support disabled.\n");
        return false;
    }

    if (!choose_glx_settings (S))
    {
        fprintf (stderr, "minicomp: Cannot find a matching GLX config, vsync disabled.\n");
        return false;
    }

    S->glx_context = glXCreateNewContext (S->dpy, S->glx_fbconfig, GLX_RGBA_TYPE, 0, True);
    if (!S->glx_context)
    {
        fprintf (stderr, "minicomp: Could not create GLX context.\n");
        return false;
    }

    S->glx_window = glXCreateWindow (S->dpy, S->glx_fbconfig, S->output, NULL);
    if (!S->glx_window)
    {
        fprintf (stderr, "minicomp: Could not create GLX window.\n");
        free_glx_data (S);
        return false;
    }

    if (!glXMakeCurrent (S->dpy, S->glx_window, S->glx_context))
    {
        fprintf (stderr, "minicomp: Could not make GL context current.\n");
        free_glx_data (S);
        return false;
    }

    if (!check_glx_renderer (S))
    {
        free_glx_data (S);
        return false;
    }

    if (!check_gl_extensions (S))
    {
        fprintf (stderr, "minicomp: Screen is missing required GL extension, GL support disabled.\n");
        free_glx_data (S);
        return false;
    }

    glDisable (GL_DEPTH_TEST);
    glDepthMask (GL_FALSE);
    glTexEnvi (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glDisable (GL_BLEND);

    glLoadIdentity ();

    glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* Swap control methods available */
    S->has_mesa_swap_control =
        epoxy_has_glx_extension (S->dpy, S->screen, "GLX_MESA_swap_control");
    S->has_ext_swap_control =
        epoxy_has_glx_extension (S->dpy, S->screen, "GLX_EXT_swap_control");
    S->has_ext_swap_control_tear =
        epoxy_has_glx_extension (S->dpy, S->screen, "GLX_EXT_swap_control_tear");
    /* Sync */
    S->has_ext_arb_sync = epoxy_has_gl_extension ("GL_ARB_sync");

    return true;
}

static void
enable_glx_texture (ScreenInfo *S)
{
    glBindTexture (S->texture_type, S->rootTexture);
    glEnable (S->texture_type);
}

static void
disable_glx_texture (ScreenInfo *S)
{
    glBindTexture (S->texture_type, None);
    glDisable (S->texture_type);
}

/*
 * XSync fences: make sure the X server has finished rendering to the root
 * pixmap before GL samples from it.
 */
static void
fence_create (ScreenInfo *S, unsigned short buffer)
{
    if (S->fence[buffer] == None)
    {
        S->fence[buffer] = XSyncCreateFence (S->dpy, S->rootPixmap[buffer], False);
    }
}

static void
fence_reset (ScreenInfo *S, unsigned short buffer)
{
    if (S->fence[buffer] == None)
    {
        return;
    }
    XSyncResetFence (S->dpy, S->fence[buffer]);
}

static bool
is_fence_triggered (ScreenInfo *S, unsigned short buffer)
{
    Bool triggered = False;

    if (S->fence[buffer] == None)
    {
        return true;
    }

    if (!XSyncQueryFence (S->dpy, S->fence[buffer], &triggered))
    {
        return true;
    }

    return (bool) triggered;
}

static void
fence_sync (ScreenInfo *S, unsigned short buffer)
{
    if (is_fence_triggered (S, buffer))
    {
        return;
    }

    XSyncTriggerFence (S->dpy, S->fence[buffer]);
    XSyncAwaitFence (S->dpy, &S->fence[buffer], 1);

    fence_reset (S, buffer);
}

static void
fence_destroy (ScreenInfo *S, unsigned short buffer)
{
    if (S->fence[buffer])
    {
        XSyncDestroyFence (S->dpy, S->fence[buffer]);
        S->fence[buffer] = None;
    }
}

static void
set_swap_interval (ScreenInfo *S, unsigned short buffer)
{
#if defined (glXSwapIntervalEXT)
    if (S->has_ext_swap_control)
    {
        if (S->has_ext_swap_control_tear)
        {
            /* Adaptive vsync */
            glXSwapIntervalEXT (S->dpy, S->glx_drawable[buffer], -1);
        }
        else
        {
            glXSwapIntervalEXT (S->dpy, S->glx_drawable[buffer], 1);
        }
        return;
    }
#endif

#if defined (glXSwapIntervalMESA)
    if (S->has_mesa_swap_control)
    {
        glXSwapIntervalMESA (1);
        return;
    }
#endif

    fprintf (stderr, "minicomp: No swap control available\n");
}

static void
create_glx_drawable (ScreenInfo *S, unsigned short buffer)
{
    int pixmap_attribs[] = {
        GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
        GLX_TEXTURE_FORMAT_EXT, GLX_TEXTURE_FORMAT_RGB_EXT,
        None
    };

    pixmap_attribs[1] = S->texture_target;
    pixmap_attribs[3] = S->texture_format;

    S->glx_drawable[buffer] = glXCreatePixmap (S->dpy, S->glx_fbconfig,
                                               S->rootPixmap[buffer],
                                               pixmap_attribs);
}

static void
bind_glx_texture (ScreenInfo *S, unsigned short buffer)
{
    if (S->rootTexture == None)
    {
        glGenTextures (1, &S->rootTexture);
    }
    if (S->glx_drawable[buffer] == None)
    {
        create_glx_drawable (S, buffer);
        set_swap_interval (S, buffer);
    }
    enable_glx_texture (S);
    glXBindTexImageEXT (S->dpy, S->glx_drawable[buffer], GLX_FRONT_EXT, NULL);
}

static void
unbind_glx_texture (ScreenInfo *S, unsigned short buffer)
{
    if (S->glx_drawable[buffer])
    {
        glXReleaseTexImageEXT (S->dpy, S->glx_drawable[buffer], GLX_FRONT_EXT);
    }
}

static void
destroy_glx_drawable (ScreenInfo *S, unsigned short buffer)
{
    if (S->glx_drawable[buffer])
    {
        unbind_glx_texture (S, buffer);
        glXDestroyPixmap (S->dpy, S->glx_drawable[buffer]);
        S->glx_drawable[buffer] = None;
    }

    if (S->rootTexture)
    {
        disable_glx_texture (S);
        glDeleteTextures (1, &S->rootTexture);
        S->rootTexture = None;
    }
}

static void
set_glx_scale (ScreenInfo *S, int width, int height, double scale)
{
    if (S->texture_type == GL_TEXTURE_RECTANGLE_ARB)
    {
        glScaled ((double) width * scale, (double) height * scale, 1.0);
    }
    else
    {
        glScaled (1.0 * scale, 1.0 * scale, 1.0);
    }
}

static void
redraw_glx_rects (ScreenInfo *S, XRectangle *rects, int nrects)
{
    int i;

    glBegin (GL_QUADS);
    for (i = 0; i < nrects; i++)
    {
        double texture_x1 = (double) (rects[i].x) / S->width;
        double texture_y1 = (double) (rects[i].y) / S->height;
        double texture_x2 = (double) (rects[i].x + rects[i].width) / S->width;
        double texture_y2 = (double) (rects[i].y + rects[i].height) / S->height;
        double vertice_x1 = 2 * texture_x1 - 1.0;
        double vertice_y1 = -2 * texture_y1 + 1.0;
        double vertice_x2 = 2 * texture_x2 - 1.0;
        double vertice_y2 = -2 * texture_y2 + 1.0;

        if (S->texture_inverted)
        {
            texture_y1 = 1.0 - texture_y1;
            texture_y2 = texture_y2 - 1.0;
        }

        glTexCoord2f (texture_x1, texture_y1);
        glVertex2f (vertice_x1, vertice_y1);
        glTexCoord2f (texture_x2, texture_y1);
        glVertex2f (vertice_x2, vertice_y1);
        glTexCoord2f (texture_x2, texture_y2);
        glVertex2f (vertice_x2, vertice_y2);
        glTexCoord2f (texture_x1, texture_y2);
        glVertex2f (vertice_x1, vertice_y2);
    }
    glEnd ();
}

static void
redraw_glx_screen (ScreenInfo *S)
{
    XRectangle root_rect = { 0, 0, S->width, S->height };

    redraw_glx_rects (S, &root_rect, 1);
}

static void
redraw_glx_texture (ScreenInfo *S, unsigned short buffer)
{
    fence_sync (S, buffer);

    if (S->has_ext_arb_sync)
    {
#if defined (glDeleteSync)
        glDeleteSync (S->gl_sync);
#endif
        S->gl_sync = 0;
    }

    bind_glx_texture (S, buffer);

    glDrawBuffer (GL_BACK);
    glViewport (0, 0, S->width, S->height);

    glMatrixMode (GL_TEXTURE);
    glPushMatrix ();

    set_glx_scale (S, S->width, S->height, 1.0);
    glTranslated (0.0, 0.0, 0.0);

    redraw_glx_screen (S);

    glXSwapBuffers (S->dpy, S->glx_window);

    glPopMatrix ();

    unbind_glx_texture (S, buffer);

    if (S->has_ext_arb_sync)
    {
#if defined (glFenceSync)
        S->gl_sync = glFenceSync (GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
#endif
    }
}

#endif /* HAVE_EPOXY */

/* ------------------------------------------------------------------------ */
/* XPresent                                                                  */
/* ------------------------------------------------------------------------ */

#ifdef HAVE_PRESENT_EXTENSION

static void
present_error (ScreenInfo *S, int error_code)
{
    char buf[64];

    XGetErrorText (S->dpy, error_code, buf, 63);
    fprintf (stderr, "minicomp: Dismissing XPresent as unusable, error %i (%s)\n",
             error_code, buf);

    S->present_pending = false;
    S->use_present = false;
}

static void
present_flip (ScreenInfo *S, XserverRegion region, unsigned short buffer)
{
    static uint32_t present_serial;
    int result;

    if (region == None)
    {
        return;
    }

    trap_push (S->dpy);
    XPresentPixmap (S->dpy, S->output,
                    S->rootPixmap[buffer],
                    present_serial++, None, region, 0, 0, None, None, None,
                    PresentOptionNone, 0, 1, 0, NULL, 0);
    result = trap_pop (S->dpy);

    if (result != 0)
    {
        present_error (S, result);
        return;
    }

    S->present_pending = true;
}

#endif /* HAVE_PRESENT_EXTENSION */

/* ------------------------------------------------------------------------ */
/* Window helpers                                                            */
/* ------------------------------------------------------------------------ */

static CWindow *
find_win (ScreenInfo *S, Window id)
{
    CWindow *cw;

    for (cw = S->cwindows; cw; cw = cw->next)
    {
        if (cw->id == id)
        {
            return cw;
        }
    }
    return NULL;
}

static bool
is_output (ScreenInfo *S, Window id)
{
    return (id == S->output || id == S->overlay || id == S->root_overlay ||
            id == S->cm_win);
}

static void
list_unlink (ScreenInfo *S, CWindow *cw)
{
    if (cw->prev)
    {
        cw->prev->next = cw->next;
    }
    else if (S->cwindows == cw)
    {
        S->cwindows = cw->next;
    }

    if (cw->next)
    {
        cw->next->prev = cw->prev;
    }
    else if (S->cwindows_tail == cw)
    {
        S->cwindows_tail = cw->prev;
    }
    cw->next = NULL;
    cw->prev = NULL;
}

static void
list_push_top (ScreenInfo *S, CWindow *cw)
{
    cw->prev = NULL;
    cw->next = S->cwindows;
    if (S->cwindows)
    {
        S->cwindows->prev = cw;
    }
    else
    {
        S->cwindows_tail = cw;
    }
    S->cwindows = cw;
}

static void
list_push_bottom (ScreenInfo *S, CWindow *cw)
{
    cw->next = NULL;
    cw->prev = S->cwindows_tail;
    if (S->cwindows_tail)
    {
        S->cwindows_tail->next = cw;
    }
    else
    {
        S->cwindows = cw;
    }
    S->cwindows_tail = cw;
}

static void
list_insert_before (ScreenInfo *S, CWindow *cw, CWindow *sibling)
{
    cw->next = sibling;
    cw->prev = sibling->prev;
    if (sibling->prev)
    {
        sibling->prev->next = cw;
    }
    else
    {
        S->cwindows = cw;
    }
    sibling->prev = cw;
}

static XserverRegion
win_extents (CWindow *cw)
{
    XRectangle r;

    r.x = cw->attr.x;
    r.y = cw->attr.y;
    r.width = cw->attr.width + cw->attr.border_width * 2;
    r.height = cw->attr.height + cw->attr.border_width * 2;

    return XFixesCreateRegion (g_screen.dpy, &r, 1);
}

static Picture
get_window_picture (CWindow *cw)
{
    ScreenInfo *S = &g_screen;
    XRenderPictureAttributes pa;
    XRenderPictFormat *format;
    Drawable draw;

    draw = cw->id;

    trap_push (S->dpy);
    if (cw->name_window_pixmap == None)
    {
        cw->name_window_pixmap = XCompositeNameWindowPixmap (S->dpy, cw->id);
    }
    if ((trap_pop (S->dpy) == Success) && (cw->name_window_pixmap != None))
    {
        draw = cw->name_window_pixmap;
    }

    format = XRenderFindVisualFormat (S->dpy, cw->attr.visual);
    if (!format)
    {
        format = XRenderFindVisualFormat (S->dpy, S->visual);
    }
    if (format)
    {
        Picture pict;

        trap_push (S->dpy);
        pa.subwindow_mode = IncludeInferiors;
        pict = XRenderCreatePicture (S->dpy, draw, format, CPSubwindowMode, &pa);
        if (trap_pop (S->dpy) == Success)
        {
            return pict;
        }
    }

    return None;
}

static XserverRegion
border_size (CWindow *cw)
{
    ScreenInfo *S = &g_screen;
    XserverRegion border;

    trap_push (S->dpy);
    border = XFixesCreateRegionFromWindow (S->dpy, cw->id, ShapeBounding);
    if (cw->picture != None)
    {
        /*
         * The bounding region is relative to the inner window origin, but the
         * name window pixmap starts at the outer corner of the border, so the
         * clip must be shifted by the border width (otherwise windows with a
         * border lose border_width pixels on their right and bottom edges).
         */
        int off = (cw->name_window_pixmap != None) ? cw->attr.border_width : 0;

        XFixesSetPictureClipRegion (S->dpy, cw->picture, off, off, border);
    }
    XFixesTranslateRegion (S->dpy, border,
                           cw->attr.x + cw->attr.border_width,
                           cw->attr.y + cw->attr.border_width);

    if (trap_pop (S->dpy) != Success)
    {
        return None;
    }

    return border;
}

static void
free_win_data (CWindow *cw, bool delete)
{
    ScreenInfo *S = &g_screen;
    Display *dpy = S->dpy;

    trap_push (dpy);
    if (cw->name_window_pixmap)
    {
        XFreePixmap (dpy, cw->name_window_pixmap);
        cw->name_window_pixmap = None;
    }

    if (cw->picture)
    {
        XRenderFreePicture (dpy, cw->picture);
        cw->picture = None;
    }

    if (cw->alphaPict && delete)
    {
        XRenderFreePicture (dpy, cw->alphaPict);
        cw->alphaPict = None;
    }

    if (cw->borderSize)
    {
        XFixesDestroyRegion (dpy, cw->borderSize);
        cw->borderSize = None;
    }

    if (cw->extents)
    {
        XFixesDestroyRegion (dpy, cw->extents);
        cw->extents = None;
    }

    if (cw->borderClip)
    {
        XFixesDestroyRegion (dpy, cw->borderClip);
        cw->borderClip = None;
    }

    if (delete && cw->damage)
    {
        XDamageDestroy (dpy, cw->damage);
        cw->damage = None;
    }
    trap_pop_ignored (dpy);
}

static uint32_t
get_opacity_prop (ScreenInfo *S, Window w, uint32_t def)
{
    Atom actual;
    int format;
    unsigned long n, left;
    unsigned char *data = NULL;
    int result;

    trap_push (S->dpy);
    result = XGetWindowProperty (S->dpy, w, S->atom_opacity, 0L, 1L, False,
                                 XA_CARDINAL, &actual, &format, &n, &left, &data);
    trap_pop_ignored (S->dpy);

    if (result == Success && data != NULL)
    {
        if (actual == XA_CARDINAL && format == 32 && n == 1)
        {
            uint32_t i = (uint32_t) (*(unsigned long *) data);

            XFree (data);
            return i;
        }
        XFree (data);
    }
    return def;
}

/* ------------------------------------------------------------------------ */
/* Damage handling                                                           */
/* ------------------------------------------------------------------------ */

static void
add_repair (ScreenInfo *S)
{
    if (!S->timeout_active)
    {
        S->timeout_active = true;
        S->timeout_period = TIMEOUT_REPAINT_MS;
        S->timeout_due = now_ms () + TIMEOUT_REPAINT_MS;
    }
}

static void
remove_timeouts (ScreenInfo *S)
{
    S->timeout_active = false;
}

static void
add_damage (ScreenInfo *S, XserverRegion damage)
{
    if (damage == None)
    {
        return;
    }

    if (S->screenRegion == None)
    {
        S->screenRegion = get_screen_region (S);
    }

    XFixesIntersectRegion (S->dpy, damage, damage, S->screenRegion);
    if (S->allDamage != None)
    {
        XFixesUnionRegion (S->dpy, S->allDamage, S->allDamage, damage);
        XFixesDestroyRegion (S->dpy, damage);
    }
    else
    {
        S->allDamage = damage;
    }

    add_repair (S);
}

/* Exclude opaque windows in front of the given area */
static void
fix_region (CWindow *cw, XserverRegion region)
{
    ScreenInfo *S = &g_screen;
    CWindow *cw2;

    for (cw2 = S->cwindows; cw2; cw2 = cw2->next)
    {
        if (cw2 == cw)
        {
            break;
        }
        else if (WIN_IS_OPAQUE (cw2) && WIN_IS_VISIBLE (cw2))
        {
            /* Make sure the window's areas are up-to-date... */
            if (cw2->picture == None)
            {
                cw2->picture = get_window_picture (cw2);
            }
            if (cw2->borderSize == None)
            {
                cw2->borderSize = border_size (cw2);
            }
            /* ...before subtracting them from the damaged zone. */
            if (cw2->borderSize)
            {
                XFixesSubtractRegion (S->dpy, region, region, cw2->borderSize);
            }
        }
    }
}

static void
damage_screen (ScreenInfo *S)
{
    XserverRegion region;

    region = get_screen_region (S);
    /* region will be freed by add_damage () */
    add_damage (S, region);
}

static void
damage_win (CWindow *cw)
{
    XserverRegion extents;

    extents = win_extents (cw);
    fix_region (cw, extents);
    /* extents region will be freed by add_damage () */
    add_damage (&g_screen, extents);
}

static void
update_extents (CWindow *cw)
{
    ScreenInfo *S = &g_screen;

    if (WIN_IS_VISIBLE (cw))
    {
        damage_win (cw);
    }

    if (cw->extents)
    {
        XFixesDestroyRegion (S->dpy, cw->extents);
        cw->extents = None;
    }
}

static void
repair_win (CWindow *cw)
{
    ScreenInfo *S = &g_screen;
    XserverRegion parts;

    if (!(cw->damage))
    {
        /* Input Only have no damage */
        return;
    }

    trap_push (S->dpy);
    if (cw->damaged)
    {
        parts = XFixesCreateRegion (S->dpy, NULL, 0);
        /* Copy the damage region to parts, subtracting it from the window's damage */
        XDamageSubtract (S->dpy, cw->damage, None, parts);
        XFixesTranslateRegion (S->dpy, parts,
                               cw->attr.x + cw->attr.border_width,
                               cw->attr.y + cw->attr.border_width);
    }
    else
    {
        parts = win_extents (cw);
        /* Subtract all damage from the window's damage */
        XDamageSubtract (S->dpy, cw->damage, None, None);
    }
    trap_pop_ignored (S->dpy);

    if (parts)
    {
        fix_region (cw, parts);
        /* parts region will be destroyed by add_damage () */
        add_damage (S, parts);
        cw->damaged = true;
    }
}

static void
determine_mode (CWindow *cw)
{
    ScreenInfo *S = &g_screen;
    XRenderPictFormat *format;

    if (cw->alphaPict)
    {
        XRenderFreePicture (S->dpy, cw->alphaPict);
        cw->alphaPict = None;
    }

    if (cw->attr.class == InputOutput)
    {
        format = XRenderFindVisualFormat (S->dpy, cw->attr.visual);
        cw->argb = ((format) && (format->type == PictTypeDirect) && (format->direct.alphaMask));
    }
    else
    {
        cw->argb = false;
    }

    if (cw->extents)
    {
        XserverRegion damage;

        damage = XFixesCreateRegion (S->dpy, NULL, 0);
        XFixesCopyRegion (S->dpy, damage, cw->extents);
        fix_region (cw, damage);
        /* damage region will be destroyed by add_damage () */
        add_damage (S, damage);
    }
}

static void
set_win_opacity (CWindow *cw, uint32_t opacity)
{
    cw->opacity = opacity;
    determine_mode (cw);
    if (WIN_IS_VISIBLE (cw))
    {
        damage_win (cw);
    }
}

/* ------------------------------------------------------------------------ */
/* Window life cycle                                                         */
/* ------------------------------------------------------------------------ */

static void
add_win (ScreenInfo *S, Window id)
{
    CWindow *cw;

    if (is_output (S, id) || id == S->xroot || find_win (S, id))
    {
        return;
    }

    cw = calloc (1, sizeof (CWindow));
    if (cw == NULL)
    {
        return;
    }

    cw->id = id;

    trap_push (S->dpy);
    if (!XGetWindowAttributes (S->dpy, id, &cw->attr))
    {
        trap_pop_ignored (S->dpy);
        free (cw);
        return;
    }
    if (trap_pop (S->dpy) != Success)
    {
        free (cw);
        return;
    }

    cw->viewable = (cw->attr.map_state == IsViewable);
    cw->opacity = NET_WM_OPAQUE;

    if (cw->attr.class == InputOutput)
    {
        trap_push (S->dpy);
        cw->damage = XDamageCreate (S->dpy, id, XDamageReportNonEmpty);
        XSelectInput (S->dpy, id, PropertyChangeMask);
        if (trap_pop (S->dpy) != Success)
        {
            cw->damage = None;
        }
        cw->opacity = get_opacity_prop (S, id, NET_WM_OPAQUE);
    }

    determine_mode (cw);
    list_push_top (S, cw);

    if (cw->viewable && cw->damage)
    {
        /* Window already mapped (startup / reparent): its content is valid */
        cw->damaged = true;
        damage_win (cw);
    }
}

static void
map_win (CWindow *cw)
{
    cw->attr.map_state = IsViewable;
    cw->viewable = true;
    cw->damaged = false;
    /* Drop anything stale, a new name pixmap is needed after each map */
    free_win_data (cw, false);
}

static void
unmap_win (CWindow *cw)
{
    if (WIN_IS_VISIBLE (cw))
    {
        damage_win (cw);
    }
    cw->attr.map_state = IsUnmapped;
    cw->viewable = false;
    cw->damaged = false;
    free_win_data (cw, false);
}

static void
destroy_win (ScreenInfo *S, Window id)
{
    CWindow *cw = find_win (S, id);

    if (cw == NULL)
    {
        return;
    }

    if (WIN_IS_VISIBLE (cw))
    {
        damage_win (cw);
    }
    list_unlink (S, cw);
    free_win_data (cw, true);
    free (cw);
}

static void
resize_win (CWindow *cw, int x, int y, int width, int height, int bw)
{
    bool size_changed;

    size_changed = (width != cw->attr.width ||
                    height != cw->attr.height ||
                    bw != cw->attr.border_width);

    /* Damage the old position, then drop the cached extents */
    update_extents (cw);

    cw->attr.x = x;
    cw->attr.y = y;
    cw->attr.width = width;
    cw->attr.height = height;
    cw->attr.border_width = bw;

    if (cw->borderSize)
    {
        XFixesDestroyRegion (g_screen.dpy, cw->borderSize);
        cw->borderSize = None;
    }

    if (size_changed)
    {
        trap_push (g_screen.dpy);
        if (cw->name_window_pixmap)
        {
            XFreePixmap (g_screen.dpy, cw->name_window_pixmap);
            cw->name_window_pixmap = None;
        }
        if (cw->picture)
        {
            XRenderFreePicture (g_screen.dpy, cw->picture);
            cw->picture = None;
        }
        trap_pop_ignored (g_screen.dpy);
    }

    /* Damage the new position */
    if (WIN_IS_VISIBLE (cw))
    {
        damage_win (cw);
    }
}

/* Put cw right above the window `above', or at the bottom if `above' is None */
static void
restack_win (ScreenInfo *S, CWindow *cw, Window above)
{
    CWindow *sibling = NULL;
    XserverRegion extents;

    if (above != None)
    {
        sibling = find_win (S, above);
        if (sibling == NULL || sibling == cw)
        {
            /* Unknown sibling (overlay, untracked...), keep current position */
            return;
        }
    }

    if ((sibling == NULL && cw == S->cwindows_tail) ||
        (sibling != NULL && cw->next == sibling))
    {
        return;
    }

    list_unlink (S, cw);
    if (sibling)
    {
        list_insert_before (S, cw, sibling);
    }
    else
    {
        list_push_bottom (S, cw);
    }

    if (WIN_IS_VISIBLE (cw))
    {
        /* Stacking order changed: repaint the whole window area */
        extents = win_extents (cw);
        add_damage (S, extents);
    }
}

static void
circulate_win (ScreenInfo *S, CWindow *cw, int place)
{
    XserverRegion extents;

    list_unlink (S, cw);
    if (place == PlaceOnTop)
    {
        list_push_top (S, cw);
    }
    else
    {
        list_push_bottom (S, cw);
    }

    if (WIN_IS_VISIBLE (cw))
    {
        extents = win_extents (cw);
        add_damage (S, extents);
    }
}

/* ------------------------------------------------------------------------ */
/* Painting                                                                  */
/* ------------------------------------------------------------------------ */

static void
paint_root (ScreenInfo *S, Picture paint_buffer)
{
    if (S->rootTile == None)
    {
        S->rootTile = root_tile (S);
        if (S->rootTile == None)
        {
            return;
        }
    }

    XRenderComposite (S->dpy, PictOpSrc, S->rootTile, None, paint_buffer,
                      0, 0, 0, 0, 0, 0, S->width, S->height);
}

static void
paint_win (CWindow *cw, XserverRegion region, Picture paint_buffer, bool solid_part)
{
    ScreenInfo *S = &g_screen;
    bool paint_solid;
    int x, y;
    unsigned int w, h;

    if (cw->picture == None)
    {
        return;
    }

    paint_solid = (solid_part && WIN_IS_OPAQUE (cw));

    x = cw->attr.x;
    y = cw->attr.y;
    w = cw->attr.width + 2 * cw->attr.border_width;
    h = cw->attr.height + 2 * cw->attr.border_width;

    if (paint_solid)
    {
        XFixesSetPictureClipRegion (S->dpy, paint_buffer, 0, 0, region);
        XRenderComposite (S->dpy, PictOpSrc, cw->picture, None, paint_buffer,
                          0, 0, 0, 0, x, y, w, h);
        if (cw->borderSize)
        {
            XFixesSubtractRegion (S->dpy, region, region, cw->borderSize);
        }
    }
    else if (!solid_part)
    {
        XRenderComposite (S->dpy, PictOpOver, cw->picture, cw->alphaPict,
                          paint_buffer, 0, 0, 0, 0, x, y, w, h);
    }
}

static void
paint_all (ScreenInfo *S, XserverRegion region, unsigned short buffer)
{
    Display *dpy = S->dpy;
    XserverRegion paint_region;
    XRectangle region_bounds;
    Picture paint_buffer;
    CWindow *cw;

    trap_push (dpy);

    /* Create root buffer if not done yet */
    if (S->rootPixmap[buffer] == None)
    {
        S->rootPixmap[buffer] = create_root_pixmap (S);
#ifdef HAVE_EPOXY
        if (S->use_glx && S->rootPixmap[buffer] != None)
        {
            fence_create (S, buffer);
        }
#endif
    }

    if (S->rootBuffer[buffer] == None)
    {
        S->rootBuffer[buffer] = create_root_buffer (S, S->rootPixmap[buffer]);
    }

    if (S->rootBuffer[buffer] == None)
    {
        trap_pop_ignored (dpy);
        return;
    }
    paint_buffer = S->rootBuffer[buffer];

    /* Copy the original given region */
    paint_region = XFixesCreateRegion (dpy, NULL, 0);
    XFixesCopyRegion (dpy, paint_region, region);

    /*
     * Painting from top to bottom, reducing the clipping area at each iteration.
     * Only the opaque windows are painted 1st.
     */
    for (cw = S->cwindows; cw; cw = cw->next)
    {
        if (!WIN_IS_VISIBLE (cw) || !WIN_IS_DAMAGED (cw))
        {
            cw->skipped = true;
            continue;
        }

        if ((cw->attr.x + cw->attr.width < 1) || (cw->attr.y + cw->attr.height < 1) ||
            (cw->attr.x >= S->width) || (cw->attr.y >= S->height))
        {
            cw->skipped = true;
            continue;
        }

        if (cw->extents == None)
        {
            cw->extents = win_extents (cw);
        }
        if (cw->picture == None)
        {
            cw->picture = get_window_picture (cw);
        }
        if (cw->borderSize == None)
        {
            cw->borderSize = border_size (cw);
        }
        if (WIN_IS_OPAQUE (cw))
        {
            paint_win (cw, paint_region, paint_buffer, true);
        }

        if (cw->borderClip == None)
        {
            cw->borderClip = XFixesCreateRegion (dpy, NULL, 0);
            XFixesCopyRegion (dpy, cw->borderClip, paint_region);
        }

        cw->skipped = false;
    }

    /*
     * region has changed because of the XFixesSubtractRegion (),
     * reapply clipping for the last iteration.
     */
    XFixesSetPictureClipRegion (dpy, paint_buffer, 0, 0, paint_region);
    if (!is_region_empty (dpy, paint_region))
    {
        paint_root (S, paint_buffer);
    }

    /*
     * Painting from bottom to top, translucent windows are painted now...
     */
    for (cw = S->cwindows_tail; cw; cw = cw->prev)
    {
        if (cw->skipped)
        {
            continue;
        }

        if (cw->picture)
        {
            if ((cw->opacity != NET_WM_OPAQUE) && !(cw->alphaPict))
            {
                cw->alphaPict = solid_picture (S, false,
                                               (double) cw->opacity / NET_WM_OPAQUE,
                                               0.0, 0.0, 0.0);
            }
            if (cw->borderClip && cw->borderSize)
            {
                XFixesIntersectRegion (dpy, cw->borderClip, cw->borderClip, cw->borderSize);
                XFixesSetPictureClipRegion (dpy, paint_buffer, 0, 0, cw->borderClip);
                paint_win (cw, paint_region, paint_buffer, false);
            }
        }

        if (cw->borderClip)
        {
            XFixesDestroyRegion (dpy, cw->borderClip);
            cw->borderClip = None;
        }
    }

    /* Copying data back to screen */
#ifdef HAVE_EPOXY
    if (S->use_glx)
    {
        /*
         * The X server must be done rendering into the root pixmap
         * before GL reads from it: re-arm the fence, it gets triggered
         * (and awaited) right before the texture is bound.
         */
        fence_reset (S, buffer);
    }
    else
#endif
    {
        /* Set clipping back to the given region */
        XFixesSetPictureClipRegion (dpy, S->rootBuffer[buffer], 0, 0, region);
    }

#ifdef HAVE_PRESENT_EXTENSION
    if (S->use_present)
    {
        present_flip (S, region, buffer);
    }
    else
#endif
#ifdef HAVE_EPOXY
    if (S->use_glx)
    {
        redraw_glx_texture (S, buffer);
    }
    else
#endif
    {
        get_region_bounds (dpy, region, &region_bounds);
        XRenderComposite (dpy, PictOpSrc, paint_buffer,
                          None, S->rootPicture,
                          region_bounds.x, region_bounds.y,
                          region_bounds.x, region_bounds.y,
                          region_bounds.x, region_bounds.y,
                          region_bounds.width, region_bounds.height);
        XFlush (dpy);
    }

    XFixesDestroyRegion (dpy, paint_region);

    trap_pop_ignored (dpy);
}

/* ------------------------------------------------------------------------ */
/* Repaint scheduling (same logic as xfwm4's repair_screen / timeout cb)     */
/* ------------------------------------------------------------------------ */

static bool
repair_screen (ScreenInfo *S)
{
    XserverRegion damage;

    if (!S->compositor_active)
    {
        return false;
    }

    if (S->width == 0 || S->height == 0)
    {
        return false;
    }

#ifdef HAVE_PRESENT_EXTENSION
    /*
     * We do not paint the screen because we are waiting for
     * a pending present notification, do not cancel the callback yet...
     */
    if (S->use_present && S->present_pending)
    {
        return (S->allDamage != None);
    }
#endif /* HAVE_PRESENT_EXTENSION */

#ifdef HAVE_EPOXY
    /*
     * We do not paint the screen because we are waiting for
     * the GL pipeline to complete, do not cancel the callback yet...
     */
    if (S->use_glx && S->gl_sync)
    {
#if defined (GL_SIGNALED)
        GLint status = GL_SIGNALED;
#if defined (glGetSynciv)
        glGetSynciv (S->gl_sync, GL_SYNC_STATUS, sizeof (GLint), NULL, &status);
#endif
        if (status != GL_SIGNALED)
        {
            return (S->allDamage != None);
        }
#endif
    }
#endif /* HAVE_EPOXY */

    damage = S->allDamage;
    if (damage)
    {
        if (S->use_n_buffers > 1)
        {
            if (S->prevDamage)
            {
                XFixesUnionRegion (S->dpy, S->prevDamage, S->prevDamage, damage);
                damage = S->prevDamage;
            }
        }

        remove_timeouts (S);
        paint_all (S, damage, S->current_buffer);

        if (S->use_n_buffers > 1)
        {
            S->current_buffer = (S->current_buffer + 1) % S->use_n_buffers;

            if (S->prevDamage)
            {
                XFixesDestroyRegion (S->dpy, S->prevDamage);
            }

            S->prevDamage = S->allDamage;
        }
        else
        {
            XFixesDestroyRegion (S->dpy, S->allDamage);
        }
        S->allDamage = None;
    }

    return false;
}

static void
compositor_timeout_cb (ScreenInfo *S)
{
    bool retry;

    retry = repair_screen (S);

    if (retry)
    {
        if (S->number_of_retries <= TIMEOUT_MAX_RETRIES)
        {
            S->number_of_retries++;
        }
        if (S->number_of_retries == TIMEOUT_MAX_RETRIES)
        {
            /* Throttling repaint after 100 unsuccessful retries */
            S->timeout_period = TIMEOUT_THROTTLED_REPAINT_MS;
        }
        S->timeout_active = true;
        S->timeout_due = now_ms () + S->timeout_period;
    }
    else
    {
        S->timeout_active = false;
        S->number_of_retries = 0;
    }
}

/* ------------------------------------------------------------------------ */
/* Screen management                                                         */
/* ------------------------------------------------------------------------ */

static void
free_buffers (ScreenInfo *S)
{
    unsigned short buffer;

    for (buffer = 0; buffer < N_BUFFERS; buffer++)
    {
#ifdef HAVE_EPOXY
        if (S->use_glx)
        {
            destroy_glx_drawable (S, buffer);
            fence_destroy (S, buffer);
        }
#endif
        if (S->rootBuffer[buffer])
        {
            XRenderFreePicture (S->dpy, S->rootBuffer[buffer]);
            S->rootBuffer[buffer] = None;
        }
        if (S->rootPixmap[buffer])
        {
            XFreePixmap (S->dpy, S->rootPixmap[buffer]);
            S->rootPixmap[buffer] = None;
        }
    }
}

static void
update_screen_size (ScreenInfo *S, int width, int height)
{
    trap_push (S->dpy);

    S->width = width;
    S->height = height;

    /* Buffers are recreated lazily by paint_all () */
    free_buffers (S);

    if (S->root_overlay != None)
    {
        XResizeWindow (S->dpy, S->root_overlay, width, height);
    }

    if (S->screenRegion)
    {
        XFixesDestroyRegion (S->dpy, S->screenRegion);
        S->screenRegion = None;
    }
    if (S->allDamage)
    {
        XFixesDestroyRegion (S->dpy, S->allDamage);
        S->allDamage = None;
    }
    if (S->prevDamage)
    {
        XFixesDestroyRegion (S->dpy, S->prevDamage);
        S->prevDamage = None;
    }
    S->current_buffer = 0;

    trap_pop_ignored (S->dpy);

    damage_screen (S);
}

static vblankMode
parse_vblank_mode (const char *s)
{
    if (strcasecmp (s, "xpresent") == 0)
    {
        return VBLANK_XPRESENT;
    }
    if (strcasecmp (s, "glx") == 0)
    {
        return VBLANK_GLX;
    }
    if (strcasecmp (s, "off") == 0)
    {
        return VBLANK_OFF;
    }
    return VBLANK_AUTO;
}

static bool
check_cm_selection (ScreenInfo *S, Atom sel)
{
    return (XGetSelectionOwner (S->dpy, sel) != None);
}

static bool
manage_screen (ScreenInfo *S)
{
    Display *dpy = S->dpy;
    XRenderPictureAttributes pa;
    XRenderPictFormat *visual_format;
    unsigned short buffer;
    char name[32];
    Atom cm_atom;
    Window root_ret, parent_ret, *children = NULL;
    unsigned int nchildren, i;

    snprintf (name, sizeof (name), "_NET_WM_CM_S%d", S->screen);
    cm_atom = XInternAtom (dpy, name, False);
    if (check_cm_selection (S, cm_atom))
    {
        fprintf (stderr, "minicomp: Another compositing manager is running on screen %i\n",
                 S->screen);
        return false;
    }

    S->cm_win = XCreateSimpleWindow (dpy, S->xroot, -10, -10, 1, 1, 0, 0, 0);
    XSetSelectionOwner (dpy, cm_atom, S->cm_win, CurrentTime);
    if (XGetSelectionOwner (dpy, cm_atom) != S->cm_win)
    {
        fprintf (stderr, "minicomp: Cannot acquire %s\n", name);
        XDestroyWindow (dpy, S->cm_win);
        S->cm_win = None;
        return false;
    }

    S->output = S->xroot;
    if (S->have_overlays)
    {
        S->overlay = XCompositeGetOverlayWindow (dpy, S->xroot);
        if (S->overlay != None)
        {
            XserverRegion region;
            XSetWindowAttributes attributes;

            memset (&attributes, 0, sizeof (attributes));
            XMapRaised (dpy, S->overlay);

            region = XFixesCreateRegion (dpy, NULL, 0);
            XFixesSetWindowShapeRegion (dpy, S->overlay, ShapeBounding, 0, 0, 0);
            XFixesSetWindowShapeRegion (dpy, S->overlay, ShapeInput, 0, 0, region);
            XFixesDestroyRegion (dpy, region);

            S->root_overlay = XCreateWindow (dpy, S->overlay,
                                             0, 0, S->width, S->height, 0, S->depth,
                                             InputOutput, S->visual, 0, &attributes);
            XSelectInput (dpy, S->root_overlay, ExposureMask);
            XMapRaised (dpy, S->root_overlay);

            S->output = S->root_overlay;
        }
        else
        {
            /* Something is wrong with overlay support */
            S->have_overlays = false;
        }
    }

    XSelectInput (dpy, S->xroot,
                  SubstructureNotifyMask | StructureNotifyMask |
                  ExposureMask | PropertyChangeMask);

    XCompositeRedirectSubwindows (dpy, S->xroot, CompositeRedirectManual);
    S->compositor_active = true;

    visual_format = XRenderFindVisualFormat (dpy, S->visual);
    if (!visual_format)
    {
        fprintf (stderr, "minicomp: Cannot find visual format on screen %i\n", S->screen);
        return false;
    }

    pa.subwindow_mode = IncludeInferiors;
    S->rootPicture = XRenderCreatePicture (dpy, S->output, visual_format,
                                           CPSubwindowMode, &pa);
    if (S->rootPicture == None)
    {
        fprintf (stderr, "minicomp: Cannot create root picture on screen %i\n", S->screen);
        return false;
    }

    S->rootTile = None;
    S->allDamage = None;
    S->prevDamage = None;
    S->screenRegion = get_screen_region (S);
    S->cwindows = NULL;
    S->cwindows_tail = NULL;
    S->timeout_active = false;
    S->number_of_retries = 0;
    S->current_buffer = 0;
    S->use_n_buffers = 1;
    for (buffer = 0; buffer < N_BUFFERS; buffer++)
    {
        S->rootPixmap[buffer] = None;
        S->rootBuffer[buffer] = None;
#ifdef HAVE_EPOXY
        S->glx_drawable[buffer] = None;
        S->fence[buffer] = None;
#endif
    }
    XClearArea (dpy, S->output, 0, 0, 0, 0, True);

#ifdef HAVE_EPOXY
    S->use_glx = (S->vblank_mode == VBLANK_AUTO ||
                  S->vblank_mode == VBLANK_GLX);
    S->use_glx &= S->have_xsync;

    if (S->use_glx)
    {
        S->use_n_buffers = 1;
        S->glx_context = None;
        S->glx_window = None;
        S->rootTexture = None;
        S->gl_sync = 0;
        S->use_glx = init_glx (S);
    }
#else
    S->use_glx = false;
#endif

#ifdef HAVE_PRESENT_EXTENSION
    S->use_present = S->have_present &&
                     !S->use_glx &&
                     (S->vblank_mode == VBLANK_AUTO ||
                      S->vblank_mode == VBLANK_XPRESENT);
    if (S->use_present)
    {
        S->use_n_buffers = N_BUFFERS;
        S->present_pending = false;
        XPresentSelectInput (dpy, S->output, PresentCompleteNotifyMask);
    }
#else
    S->use_present = false;
#endif

    if (S->use_present)
    {
        fprintf (stderr, "minicomp: using XPresent for vsync\n");
    }
    else if (S->use_glx)
    {
        fprintf (stderr, "minicomp: using GLX for vsync\n");
    }
    else
    {
        fprintf (stderr, "minicomp: no vsync support\n");
    }

    /* Add all existing windows (bottom to top, each one is pushed on top) */
    XGrabServer (dpy);
    if (XQueryTree (dpy, S->xroot, &root_ret, &parent_ret, &children, &nchildren))
    {
        for (i = 0; i < nchildren; i++)
        {
            add_win (S, children[i]);
        }
        if (children)
        {
            XFree (children);
        }
    }
    XUngrabServer (dpy);

    damage_screen (S);

    return true;
}

static void
unmanage_screen (ScreenInfo *S)
{
    Display *dpy = S->dpy;
    CWindow *cw, *next;

    if (!S->compositor_active)
    {
        return;
    }
    S->compositor_active = false;
    remove_timeouts (S);

    trap_push (dpy);

    for (cw = S->cwindows; cw; cw = next)
    {
        next = cw->next;
        free_win_data (cw, true);
        free (cw);
    }
    S->cwindows = NULL;
    S->cwindows_tail = NULL;

    free_buffers (S);

#ifdef HAVE_EPOXY
    if (S->use_glx)
    {
        free_glx_data (S);
    }
#endif

    if (S->allDamage)
    {
        XFixesDestroyRegion (dpy, S->allDamage);
        S->allDamage = None;
    }
    if (S->prevDamage)
    {
        XFixesDestroyRegion (dpy, S->prevDamage);
        S->prevDamage = None;
    }
    if (S->screenRegion)
    {
        XFixesDestroyRegion (dpy, S->screenRegion);
        S->screenRegion = None;
    }
    if (S->rootTile)
    {
        XRenderFreePicture (dpy, S->rootTile);
        S->rootTile = None;
    }
    if (S->rootPicture)
    {
        XRenderFreePicture (dpy, S->rootPicture);
        S->rootPicture = None;
    }

    XCompositeUnredirectSubwindows (dpy, S->xroot, CompositeRedirectManual);

    if (S->root_overlay)
    {
        XDestroyWindow (dpy, S->root_overlay);
        S->root_overlay = None;
    }
    if (S->overlay)
    {
        XCompositeReleaseOverlayWindow (dpy, S->xroot);
        S->overlay = None;
    }
    if (S->cm_win)
    {
        XDestroyWindow (dpy, S->cm_win);
        S->cm_win = None;
    }

    XClearArea (dpy, S->xroot, 0, 0, 0, 0, True);
    trap_pop_ignored (dpy);
    XSync (dpy, False);
}

/* ------------------------------------------------------------------------ */
/* Events                                                                    */
/* ------------------------------------------------------------------------ */

#ifdef HAVE_PRESENT_EXTENSION
static void
handle_generic_event (ScreenInfo *S, XEvent *ev)
{
    XGenericEventCookie *cookie = &ev->xcookie;

    if (cookie->extension != S->present_opcode)
    {
        return;
    }

    if (XGetEventData (S->dpy, cookie))
    {
        if (cookie->evtype == PresentCompleteNotify)
        {
            XPresentCompleteNotifyEvent *pev = (XPresentCompleteNotifyEvent *) cookie->data;

            if (pev->window == S->output)
            {
                /* present completed, present pending cleared */
                S->present_pending = false;
            }
        }
        XFreeEventData (S->dpy, cookie);
    }
}
#endif /* HAVE_PRESENT_EXTENSION */

static void
handle_event (ScreenInfo *S, XEvent *ev)
{
    CWindow *cw;

    if (ev->type == S->damage_event_base + XDamageNotify)
    {
        XDamageNotifyEvent *dev = (XDamageNotifyEvent *) ev;

        cw = find_win (S, dev->drawable);
        if (cw)
        {
            repair_win (cw);
        }
        return;
    }

    switch (ev->type)
    {
        case CreateNotify:
            if (ev->xcreatewindow.parent == S->xroot)
            {
                add_win (S, ev->xcreatewindow.window);
            }
            break;

        case DestroyNotify:
            destroy_win (S, ev->xdestroywindow.window);
            break;

        case MapNotify:
            cw = find_win (S, ev->xmap.window);
            if (cw)
            {
                map_win (cw);
            }
            break;

        case UnmapNotify:
            cw = find_win (S, ev->xunmap.window);
            if (cw)
            {
                unmap_win (cw);
            }
            break;

        case ReparentNotify:
            if (ev->xreparent.parent == S->xroot)
            {
                add_win (S, ev->xreparent.window);
            }
            else
            {
                destroy_win (S, ev->xreparent.window);
            }
            break;

        case ConfigureNotify:
        {
            XConfigureEvent *ce = &ev->xconfigure;

            if (ce->window == S->xroot)
            {
                if (ce->width != S->width || ce->height != S->height)
                {
                    update_screen_size (S, ce->width, ce->height);
                }
                break;
            }

            cw = find_win (S, ce->window);
            if (cw)
            {
                resize_win (cw, ce->x, ce->y, ce->width, ce->height, ce->border_width);
                restack_win (S, cw, ce->above);
            }
            break;
        }

        case CirculateNotify:
            cw = find_win (S, ev->xcirculate.window);
            if (cw)
            {
                circulate_win (S, cw, ev->xcirculate.place);
            }
            break;

        case Expose:
            if (ev->xexpose.window == S->xroot || ev->xexpose.window == S->output)
            {
                XRectangle r;
                XserverRegion region;

                r.x = ev->xexpose.x;
                r.y = ev->xexpose.y;
                r.width = ev->xexpose.width;
                r.height = ev->xexpose.height;
                region = XFixesCreateRegion (S->dpy, &r, 1);
                add_damage (S, region);
            }
            break;

        case PropertyNotify:
        {
            XPropertyEvent *pe = &ev->xproperty;

            if (pe->window == S->xroot)
            {
                if (pe->atom == S->atom_xrootpmap || pe->atom == S->atom_xsetroot)
                {
                    if (S->rootTile)
                    {
                        XRenderFreePicture (S->dpy, S->rootTile);
                        S->rootTile = None;
                    }
                    damage_screen (S);
                }
            }
            else if (pe->atom == S->atom_opacity)
            {
                cw = find_win (S, pe->window);
                if (cw)
                {
                    uint32_t opacity = (pe->state == PropertyDelete) ?
                                        NET_WM_OPAQUE :
                                        get_opacity_prop (S, cw->id, NET_WM_OPAQUE);

                    set_win_opacity (cw, opacity);
                }
            }
            break;
        }

        case GenericEvent:
#ifdef HAVE_PRESENT_EXTENSION
            handle_generic_event (S, ev);
#endif
            break;

        default:
            break;
    }
}

/* ------------------------------------------------------------------------ */
/* Display initialisation, main loop                                         */
/* ------------------------------------------------------------------------ */

static bool
init_display (ScreenInfo *S)
{
    Display *dpy = S->dpy;
    int event_base, error_base;
    int fixes_event_base, fixes_error_base;
    int fixes_major = 0, fixes_minor = 0;

    if (!XRenderQueryExtension (dpy, &event_base, &error_base))
    {
        fprintf (stderr, "minicomp: The display does not support the RENDER extension.\n");
        return false;
    }

    if (!XCompositeQueryExtension (dpy, &event_base, &error_base))
    {
        fprintf (stderr, "minicomp: The display does not support the COMPOSITE extension.\n");
        return false;
    }
    XCompositeQueryVersion (dpy, &S->composite_major, &S->composite_minor);
    if (S->composite_major == 0 && S->composite_minor < 2)
    {
        fprintf (stderr, "minicomp: COMPOSITE %d.%d is too old (need 0.2).\n",
                 S->composite_major, S->composite_minor);
        return false;
    }
    S->have_overlays = (S->composite_major > 0) || (S->composite_minor >= 3);

    if (!XDamageQueryExtension (dpy, &S->damage_event_base, &S->damage_error_base))
    {
        fprintf (stderr, "minicomp: The display does not support the DAMAGE extension.\n");
        return false;
    }

    if (!XFixesQueryExtension (dpy, &fixes_event_base, &fixes_error_base))
    {
        fprintf (stderr, "minicomp: The display does not support the XFIXES extension.\n");
        return false;
    }
    XFixesQueryVersion (dpy, &fixes_major, &fixes_minor);
    if (fixes_major < 2)
    {
        fprintf (stderr, "minicomp: XFIXES %d.%d is too old (need 2.0).\n",
                 fixes_major, fixes_minor);
        return false;
    }

#ifdef HAVE_PRESENT_EXTENSION
    if (!XPresentQueryExtension (dpy, &S->present_opcode,
                                 &S->present_event_base, &S->present_error_base))
    {
        fprintf (stderr, "minicomp: The display does not support the XPresent extension.\n");
        S->have_present = false;
    }
    else
    {
        S->have_present = true;
    }
#endif

#ifdef HAVE_EPOXY
    {
        int sync_event, sync_error, sync_major = 0, sync_minor = 0;

        S->have_xsync = false;
        if (XSyncQueryExtension (dpy, &sync_event, &sync_error) &&
            XSyncInitialize (dpy, &sync_major, &sync_minor))
        {
            /* Fences were introduced in SYNC 3.1 */
            S->have_xsync = (sync_major > 3) || (sync_major == 3 && sync_minor >= 1);
        }
        if (!S->have_xsync)
        {
            fprintf (stderr, "minicomp: SYNC >= 3.1 (fences) not available, GLX vsync disabled.\n");
        }
    }
#endif

    S->atom_opacity = XInternAtom (dpy, "_NET_WM_WINDOW_OPACITY", False);
    S->atom_xrootpmap = XInternAtom (dpy, "_XROOTPMAP_ID", False);
    S->atom_xsetroot = XInternAtom (dpy, "_XSETROOT_ID", False);
    S->atom_pixmap = XInternAtom (dpy, "PIXMAP", False);

    return true;
}

static void
signal_handler (int sig)
{
    (void) sig;
    quit_flag = 1;
}

static void
usage (const char *prog)
{
    fprintf (stderr,
             "Usage: %s [-d display] [--vblank=auto|glx|xpresent|off]\n", prog);
}

int
main (int argc, char **argv)
{
    ScreenInfo *S = &g_screen;
    const char *display_name = NULL;
    vblankMode mode = VBLANK_AUTO;
    struct sigaction sa;
    int i;

    for (i = 1; i < argc; i++)
    {
        if (strcmp (argv[i], "-d") == 0 && i + 1 < argc)
        {
            display_name = argv[++i];
        }
        else if (strncmp (argv[i], "--vblank=", 9) == 0)
        {
            mode = parse_vblank_mode (argv[i] + 9);
        }
        else
        {
            usage (argv[0]);
            return (strcmp (argv[i], "-h") == 0 || strcmp (argv[i], "--help") == 0) ? 0 : 1;
        }
    }

    S->dpy = XOpenDisplay (display_name);
    if (S->dpy == NULL)
    {
        fprintf (stderr, "minicomp: cannot open display %s\n",
                 display_name ? display_name : "(default)");
        return 1;
    }

    XSetErrorHandler (x_error_handler);

    S->screen = DefaultScreen (S->dpy);
    S->xroot = RootWindow (S->dpy, S->screen);
    S->visual = DefaultVisual (S->dpy, S->screen);
    S->depth = DefaultDepth (S->dpy, S->screen);
    S->width = DisplayWidth (S->dpy, S->screen);
    S->height = DisplayHeight (S->dpy, S->screen);
    S->vblank_mode = mode;

    if (!init_display (S))
    {
        XCloseDisplay (S->dpy);
        return 1;
    }

    memset (&sa, 0, sizeof (sa));
    sa.sa_handler = signal_handler;
    sigemptyset (&sa.sa_mask);
    sa.sa_flags = 0;                /* no SA_RESTART: let poll () return */
    sigaction (SIGINT, &sa, NULL);
    sigaction (SIGTERM, &sa, NULL);

    if (!manage_screen (S))
    {
        unmanage_screen (S);
        XCloseDisplay (S->dpy);
        return 1;
    }

    while (!quit_flag)
    {
        struct pollfd pfd;
        int timeout = -1;

        XFlush (S->dpy);

        /* Process everything already queued by Xlib */
        while (XPending (S->dpy))
        {
            XEvent ev;

            XNextEvent (S->dpy, &ev);
            handle_event (S, &ev);
        }

        if (S->timeout_active)
        {
            int64_t now = now_ms ();

            if (now >= S->timeout_due)
            {
                compositor_timeout_cb (S);
                continue;
            }
            timeout = (int) (S->timeout_due - now);
        }

        pfd.fd = ConnectionNumber (S->dpy);
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll (&pfd, 1, timeout) < 0 && errno != EINTR)
        {
            break;
        }
    }

    unmanage_screen (S);
    XCloseDisplay (S->dpy);

    return 0;
}
