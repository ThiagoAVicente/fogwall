#ifndef FOGWALL_EGL_H
#define FOGWALL_EGL_H

#include <stdbool.h>
#include <EGL/egl.h>

struct wl_display;

struct fogwall_egl {
    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
};

bool egl_init(struct fogwall_egl *egl, struct wl_display *wl_display);
void egl_finish(struct fogwall_egl *egl);

#endif
