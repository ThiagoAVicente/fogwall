#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include <GLES2/gl2.h>

#include "wayland.h"
#include "shader.h"

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "fog_frag.h"
#include "quad_vert.h"

/* All shader motion loops with this period (seconds); iTime is wrapped to it
 * so float precision never degrades. Must match TAU_OVER_PERIOD in fog.frag
 * (2*pi / FOG_TIME_PERIOD). */
#define FOG_TIME_PERIOD 480.0

int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------------- rendering ---------------- */

static const struct wl_callback_listener frame_listener;

void output_render(struct fogwall_output *o)
{
    struct fogwall_state *st = o->state;
    if (!o->configured || o->egl_surface == EGL_NO_SURFACE) {
        return;
    }
    if (!eglMakeCurrent(st->egl.display, o->egl_surface, o->egl_surface,
                        st->egl.context)) {
        fprintf(stderr, "fogwall: eglMakeCurrent failed\n");
        st->running = false;
        return;
    }
    if (st->program == 0) {
        /* Never block in eglSwapBuffers; frame callbacks pace us instead. */
        eglSwapInterval(st->egl.display, 0);
        st->program = shader_program_create(quad_vert_src, fog_frag_src);
        if (st->program == 0) {
            st->running = false;
            return;
        }
        st->u_resolution = glGetUniformLocation(st->program, "iResolution");
        st->u_time = glGetUniformLocation(st->program, "iTime");
        st->u_highlight = glGetUniformLocation(st->program, "uHighlight");
        st->a_pos = glGetAttribLocation(st->program, "pos");
    }

    glViewport(0, 0, o->width, o->height);
    glUseProgram(st->program);
    glUniform2f(st->u_resolution, (float)o->width, (float)o->height);
    double t = (double)(now_ms() - st->start_ms) / 1000.0;
    glUniform1f(st->u_time, (float)fmod(t, FOG_TIME_PERIOD));
    glUniform3f(st->u_highlight, st->cfg.color[0], st->cfg.color[1],
                st->cfg.color[2]);

    static const GLfloat verts[] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
    glEnableVertexAttribArray((GLuint)st->a_pos);
    glVertexAttribPointer((GLuint)st->a_pos, 2, GL_FLOAT, GL_FALSE, 0, verts);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    o->frame_cb = wl_surface_frame(o->surface);
    wl_callback_add_listener(o->frame_cb, &frame_listener, o);
    eglSwapBuffers(st->egl.display, o->egl_surface);

    o->last_frame_ms = now_ms();
    o->frame_ready = false;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    (void)time;
    struct fogwall_output *o = data;
    wl_callback_destroy(cb);
    o->frame_cb = NULL;
    /* The main loop renders once the --fps interval has elapsed. While
     * paused nothing is rendered and no callback is re-requested, so the
     * loop stays fully asleep in poll(). */
    o->frame_ready = true;
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

/* ---------------- pause/resume on fullscreen toplevels ---------------- */

static bool toplevel_on_output(struct fogwall_toplevel *t,
                               struct fogwall_output *o)
{
    /* No output_enter received (yet): be conservative and treat the
     * toplevel as covering every output — better a paused wallpaper than a
     * GPU burning behind a game. */
    if (t->n_outputs == 0) {
        return true;
    }
    for (int i = 0; i < t->n_outputs; i++) {
        if (t->outputs[i] == o->wl_output) {
            return true;
        }
    }
    return false;
}

static void recompute_pause(struct fogwall_state *st)
{
    struct fogwall_output *o;
    wl_list_for_each(o, &st->outputs, link) {
        bool covered = false;
        struct fogwall_toplevel *t;
        wl_list_for_each(t, &st->toplevels, link) {
            /* Require `activated`: compositors keep reporting fullscreen for
             * windows parked on hidden workspaces, which would wrongly pause
             * a perfectly visible wallpaper. A covering-but-unfocused window
             * is still handled: the compositor stops sending frame callbacks
             * to occluded surfaces, which stops this loop on its own. */
            if (t->minimized || !t->activated ||
                    !(t->fullscreen || t->maximized)) {
                continue;
            }
            if (toplevel_on_output(t, o)) {
                covered = true;
                break;
            }
        }
        if (covered == o->paused) {
            continue;
        }
        o->paused = covered;
        if (!covered && o->frame_cb == NULL && !o->frame_ready) {
            /* The render loop fully stopped while paused; kick it back on. */
            output_render(o);
        }
    }
}

static void toplevel_handle_title(void *data,
        struct zwlr_foreign_toplevel_handle_v1 *handle, const char *title)
{
    (void)data; (void)handle; (void)title;
}

static void toplevel_handle_app_id(void *data,
        struct zwlr_foreign_toplevel_handle_v1 *handle, const char *app_id)
{
    (void)data; (void)handle; (void)app_id;
}

static void toplevel_handle_output_enter(void *data,
        struct zwlr_foreign_toplevel_handle_v1 *handle,
        struct wl_output *output)
{
    (void)handle;
    struct fogwall_toplevel *t = data;
    if (t->n_outputs < FOGWALL_TOPLEVEL_MAX_OUTPUTS) {
        t->outputs[t->n_outputs++] = output;
    }
}

static void toplevel_handle_output_leave(void *data,
        struct zwlr_foreign_toplevel_handle_v1 *handle,
        struct wl_output *output)
{
    (void)handle;
    struct fogwall_toplevel *t = data;
    for (int i = 0; i < t->n_outputs; i++) {
        if (t->outputs[i] == output) {
            t->outputs[i] = t->outputs[--t->n_outputs];
            break;
        }
    }
}

static void toplevel_handle_state(void *data,
        struct zwlr_foreign_toplevel_handle_v1 *handle,
        struct wl_array *states)
{
    (void)handle;
    struct fogwall_toplevel *t = data;
    t->pend_fullscreen = false;
    t->pend_maximized = false;
    t->pend_minimized = false;
    t->pend_activated = false;
    uint32_t *s;
    wl_array_for_each(s, states) {
        switch (*s) {
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN:
            t->pend_fullscreen = true;
            break;
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED:
            t->pend_maximized = true;
            break;
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED:
            t->pend_minimized = true;
            break;
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED:
            t->pend_activated = true;
            break;
        default:
            break;
        }
    }
}

static void toplevel_handle_done(void *data,
        struct zwlr_foreign_toplevel_handle_v1 *handle)
{
    (void)handle;
    struct fogwall_toplevel *t = data;
    t->fullscreen = t->pend_fullscreen;
    t->maximized = t->pend_maximized;
    t->minimized = t->pend_minimized;
    t->activated = t->pend_activated;
    recompute_pause(t->state);
}

static void toplevel_handle_closed(void *data,
        struct zwlr_foreign_toplevel_handle_v1 *handle)
{
    (void)handle;
    struct fogwall_toplevel *t = data;
    struct fogwall_state *st = t->state;
    zwlr_foreign_toplevel_handle_v1_destroy(t->handle);
    wl_list_remove(&t->link);
    free(t);
    recompute_pause(st);
}

static void toplevel_handle_parent(void *data,
        struct zwlr_foreign_toplevel_handle_v1 *handle,
        struct zwlr_foreign_toplevel_handle_v1 *parent)
{
    (void)data; (void)handle; (void)parent;
}

static const struct zwlr_foreign_toplevel_handle_v1_listener
toplevel_handle_listener = {
    .title = toplevel_handle_title,
    .app_id = toplevel_handle_app_id,
    .output_enter = toplevel_handle_output_enter,
    .output_leave = toplevel_handle_output_leave,
    .state = toplevel_handle_state,
    .done = toplevel_handle_done,
    .closed = toplevel_handle_closed,
    .parent = toplevel_handle_parent,
};

static void toplevel_manager_toplevel(void *data,
        struct zwlr_foreign_toplevel_manager_v1 *manager,
        struct zwlr_foreign_toplevel_handle_v1 *handle)
{
    (void)manager;
    struct fogwall_state *st = data;
    struct fogwall_toplevel *t = calloc(1, sizeof(*t));
    if (t == NULL) {
        zwlr_foreign_toplevel_handle_v1_destroy(handle);
        return;
    }
    t->state = st;
    t->handle = handle;
    wl_list_insert(&st->toplevels, &t->link);
    zwlr_foreign_toplevel_handle_v1_add_listener(handle,
            &toplevel_handle_listener, t);
}

static void toplevel_manager_finished(void *data,
        struct zwlr_foreign_toplevel_manager_v1 *manager)
{
    struct fogwall_state *st = data;
    zwlr_foreign_toplevel_manager_v1_destroy(manager);
    st->toplevel_manager = NULL;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener
toplevel_manager_listener = {
    .toplevel = toplevel_manager_toplevel,
    .finished = toplevel_manager_finished,
};

/* ---------------- layer surface ---------------- */

static void layer_surface_configure(void *data,
        struct zwlr_layer_surface_v1 *layer_surface, uint32_t serial,
        uint32_t width, uint32_t height)
{
    struct fogwall_output *o = data;
    struct fogwall_state *st = o->state;

    zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
    if (width == 0 || height == 0) {
        width = 1920;
        height = 1080;
    }
    o->width = (int32_t)width;
    o->height = (int32_t)height;

    if (o->egl_window == NULL) {
        o->egl_window = wl_egl_window_create(o->surface, o->width, o->height);
        o->egl_surface = eglCreatePlatformWindowSurface(st->egl.display,
                st->egl.config, o->egl_window, NULL);
        if (o->egl_surface == EGL_NO_SURFACE) {
            fprintf(stderr, "fogwall: eglCreatePlatformWindowSurface failed\n");
            st->running = false;
            return;
        }
    } else {
        wl_egl_window_resize(o->egl_window, o->width, o->height, 0, 0);
    }

    /* Fully opaque: lets the compositor skip blending us. */
    struct wl_region *region = wl_compositor_create_region(st->compositor);
    wl_region_add(region, 0, 0, o->width, o->height);
    wl_surface_set_opaque_region(o->surface, region);
    wl_region_destroy(region);

    o->configured = true;
    if (!o->paused) {
        output_render(o);
    }
}

static void destroy_output_surface(struct fogwall_output *o)
{
    struct fogwall_state *st = o->state;
    if (o->frame_cb != NULL) {
        wl_callback_destroy(o->frame_cb);
        o->frame_cb = NULL;
    }
    if (o->egl_surface != EGL_NO_SURFACE) {
        eglMakeCurrent(st->egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        eglDestroySurface(st->egl.display, o->egl_surface);
        o->egl_surface = EGL_NO_SURFACE;
    }
    if (o->egl_window != NULL) {
        wl_egl_window_destroy(o->egl_window);
        o->egl_window = NULL;
    }
    if (o->layer_surface != NULL) {
        zwlr_layer_surface_v1_destroy(o->layer_surface);
        o->layer_surface = NULL;
    }
    if (o->surface != NULL) {
        wl_surface_destroy(o->surface);
        o->surface = NULL;
    }
    o->configured = false;
    o->frame_ready = false;
}

static void layer_surface_closed(void *data,
        struct zwlr_layer_surface_v1 *layer_surface)
{
    (void)layer_surface;
    destroy_output_surface(data);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

static void maybe_create_surface(struct fogwall_output *o)
{
    struct fogwall_state *st = o->state;
    if (o->surface != NULL) {
        return;
    }
    if (st->cfg.output_name != NULL &&
            (o->name == NULL || strcmp(o->name, st->cfg.output_name) != 0)) {
        return;
    }
    o->surface = wl_compositor_create_surface(st->compositor);
    o->layer_surface = zwlr_layer_shell_v1_get_layer_surface(st->layer_shell,
            o->surface, o->wl_output, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
            "fogwall");
    zwlr_layer_surface_v1_add_listener(o->layer_surface,
            &layer_surface_listener, o);
    zwlr_layer_surface_v1_set_size(o->layer_surface, 0, 0);
    zwlr_layer_surface_v1_set_anchor(o->layer_surface,
            ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(o->layer_surface, -1);
    wl_surface_commit(o->surface);
}

/* ---------------- wl_output ---------------- */

static void output_geometry(void *data, struct wl_output *output, int32_t x,
        int32_t y, int32_t phys_w, int32_t phys_h, int32_t subpixel,
        const char *make, const char *model, int32_t transform)
{
    (void)data; (void)output; (void)x; (void)y; (void)phys_w; (void)phys_h;
    (void)subpixel; (void)make; (void)model; (void)transform;
}

static void output_mode(void *data, struct wl_output *output, uint32_t flags,
        int32_t width, int32_t height, int32_t refresh)
{
    (void)data; (void)output; (void)flags; (void)width; (void)height;
    (void)refresh;
}

static void output_done(void *data, struct wl_output *output)
{
    (void)output;
    struct fogwall_output *o = data;
    if (o->state->ready) {
        maybe_create_surface(o);
    }
}

static void output_scale(void *data, struct wl_output *output, int32_t scale)
{
    (void)data; (void)output; (void)scale;
}

static void output_name(void *data, struct wl_output *output,
        const char *name)
{
    (void)output;
    struct fogwall_output *o = data;
    free(o->name);
    o->name = strdup(name);
}

static void output_description(void *data, struct wl_output *output,
        const char *description)
{
    (void)data; (void)output; (void)description;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

static void destroy_output(struct fogwall_output *o)
{
    destroy_output_surface(o);
    wl_output_destroy(o->wl_output);
    wl_list_remove(&o->link);
    free(o->name);
    free(o);
}

/* ---------------- registry ---------------- */

static void registry_global(void *data, struct wl_registry *registry,
        uint32_t name, const char *interface, uint32_t version)
{
    struct fogwall_state *st = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        st->compositor = wl_registry_bind(registry, name,
                &wl_compositor_interface, version < 4 ? version : 4);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        st->layer_shell = wl_registry_bind(registry, name,
                &zwlr_layer_shell_v1_interface, 1);
    } else if (strcmp(interface,
            zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
        st->toplevel_manager = wl_registry_bind(registry, name,
                &zwlr_foreign_toplevel_manager_v1_interface,
                version < 3 ? version : 3);
        zwlr_foreign_toplevel_manager_v1_add_listener(st->toplevel_manager,
                &toplevel_manager_listener, st);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        struct fogwall_output *o = calloc(1, sizeof(*o));
        if (o == NULL) {
            return;
        }
        o->state = st;
        o->global_name = name;
        o->egl_surface = EGL_NO_SURFACE;
        o->wl_output = wl_registry_bind(registry, name, &wl_output_interface,
                version < 4 ? version : 4);
        wl_output_add_listener(o->wl_output, &output_listener, o);
        wl_list_insert(&st->outputs, &o->link);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
        uint32_t name)
{
    (void)registry;
    struct fogwall_state *st = data;
    struct fogwall_output *o, *tmp;
    wl_list_for_each_safe(o, tmp, &st->outputs, link) {
        if (o->global_name == name) {
            destroy_output(o);
            return;
        }
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ---------------- init / teardown ---------------- */

bool wayland_init(struct fogwall_state *st)
{
    wl_list_init(&st->outputs);
    wl_list_init(&st->toplevels);
    st->start_ms = now_ms();

    st->display = wl_display_connect(NULL);
    if (st->display == NULL) {
        fprintf(stderr, "fogwall: cannot connect to Wayland display\n");
        return false;
    }
    st->registry = wl_display_get_registry(st->display);
    wl_registry_add_listener(st->registry, &registry_listener, st);
    if (wl_display_roundtrip(st->display) < 0) {
        return false;
    }
    if (st->compositor == NULL || st->layer_shell == NULL) {
        fprintf(stderr, "fogwall: compositor lacks %s\n",
                st->compositor == NULL ? "wl_compositor"
                                       : "zwlr_layer_shell_v1");
        return false;
    }
    if (st->toplevel_manager == NULL) {
        fprintf(stderr, "fogwall: warning: zwlr_foreign_toplevel_manager_v1 "
                "not available; cannot pause for fullscreen windows\n");
    }
    if (!egl_init(&st->egl, st->display)) {
        return false;
    }
    /* Second roundtrip: output names/modes and initial toplevel states. */
    if (wl_display_roundtrip(st->display) < 0) {
        return false;
    }
    st->ready = true;

    struct fogwall_output *o;
    bool any = false;
    wl_list_for_each(o, &st->outputs, link) {
        maybe_create_surface(o);
        any = any || o->surface != NULL;
    }
    if (!any && st->cfg.output_name != NULL) {
        fprintf(stderr, "fogwall: warning: output \"%s\" not found yet; "
                "waiting for it to appear\n", st->cfg.output_name);
    }
    return true;
}

void wayland_finish(struct fogwall_state *st)
{
    struct fogwall_output *o, *otmp;
    wl_list_for_each_safe(o, otmp, &st->outputs, link) {
        destroy_output(o);
    }
    struct fogwall_toplevel *t, *ttmp;
    wl_list_for_each_safe(t, ttmp, &st->toplevels, link) {
        zwlr_foreign_toplevel_handle_v1_destroy(t->handle);
        wl_list_remove(&t->link);
        free(t);
    }
    if (st->program != 0) {
        glDeleteProgram(st->program);
    }
    egl_finish(&st->egl);
    if (st->toplevel_manager != NULL) {
        zwlr_foreign_toplevel_manager_v1_destroy(st->toplevel_manager);
    }
    if (st->layer_shell != NULL) {
        zwlr_layer_shell_v1_destroy(st->layer_shell);
    }
    if (st->compositor != NULL) {
        wl_compositor_destroy(st->compositor);
    }
    if (st->registry != NULL) {
        wl_registry_destroy(st->registry);
    }
    if (st->display != NULL) {
        wl_display_disconnect(st->display);
    }
}
