#include <stdio.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "egl.h"

bool egl_init(struct fogwall_egl *egl, struct wl_display *wl_display)
{
    egl->display = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR,
                                         wl_display, NULL);
    if (egl->display == EGL_NO_DISPLAY) {
        fprintf(stderr, "fogwall: eglGetPlatformDisplay failed\n");
        return false;
    }
    if (!eglInitialize(egl->display, NULL, NULL)) {
        fprintf(stderr, "fogwall: eglInitialize failed\n");
        return false;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        fprintf(stderr, "fogwall: eglBindAPI failed\n");
        return false;
    }

    static const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 0,
        EGL_DEPTH_SIZE, 0,
        EGL_STENCIL_SIZE, 0,
        EGL_NONE,
    };
    EGLint count = 0;
    if (!eglChooseConfig(egl->display, config_attribs, &egl->config, 1,
                         &count) || count < 1) {
        fprintf(stderr, "fogwall: no suitable EGL config\n");
        return false;
    }

    static const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE,
    };
    egl->context = eglCreateContext(egl->display, egl->config,
                                    EGL_NO_CONTEXT, context_attribs);
    if (egl->context == EGL_NO_CONTEXT) {
        fprintf(stderr, "fogwall: eglCreateContext failed\n");
        return false;
    }
    return true;
}

void egl_finish(struct fogwall_egl *egl)
{
    if (egl->display == EGL_NO_DISPLAY) {
        return;
    }
    eglMakeCurrent(egl->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
    if (egl->context != EGL_NO_CONTEXT) {
        eglDestroyContext(egl->display, egl->context);
    }
    eglTerminate(egl->display);
    egl->display = EGL_NO_DISPLAY;
}
