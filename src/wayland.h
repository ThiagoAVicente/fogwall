#ifndef FOGWALL_WAYLAND_H
#define FOGWALL_WAYLAND_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>
#include <GLES2/gl2.h>

#include "egl.h"

struct fogwall_config {
    float color[3];          /* highlight tint, linear-ish 0..1 */
    const char *output_name; /* NULL = all outputs */
    int fps;
};

struct fogwall_state {
    struct fogwall_config cfg;

    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct zwlr_foreign_toplevel_manager_v1 *toplevel_manager;

    struct wl_list outputs;   /* fogwall_output.link */
    struct wl_list toplevels; /* fogwall_toplevel.link */

    struct fogwall_egl egl;
    GLuint program;
    GLint u_resolution;
    GLint u_time;
    GLint u_highlight;
    GLint a_pos;

    int64_t start_ms;
    bool ready;   /* initial roundtrips done */
    bool running;

    /* Hyprland IPC (see hypr.h); -1 when not under Hyprland */
    int hypr_fd;
    char hypr_buf[4096];
    size_t hypr_buf_len;
};

struct fogwall_output {
    struct wl_list link;
    struct fogwall_state *state;
    struct wl_output *wl_output;
    uint32_t global_name;
    char *name;

    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_egl_window *egl_window;
    EGLSurface egl_surface;
    struct wl_callback *frame_cb;

    int32_t width, height;
    bool configured;
    bool paused;        /* effective: paused_proto || paused_hypr */
    bool paused_proto;  /* via wlr-foreign-toplevel state */
    bool paused_hypr;   /* via Hyprland IPC hasfullscreen */
    bool frame_ready;   /* frame callback fired; render is gated by --fps */
    int64_t last_frame_ms;
};

#define FOGWALL_TOPLEVEL_MAX_OUTPUTS 8

struct fogwall_toplevel {
    struct wl_list link;
    struct fogwall_state *state;
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    struct wl_output *outputs[FOGWALL_TOPLEVEL_MAX_OUTPUTS];
    int n_outputs;
    bool fullscreen, maximized, minimized, activated;
    bool pend_fullscreen, pend_maximized, pend_minimized, pend_activated;
};

bool wayland_init(struct fogwall_state *state);
void wayland_finish(struct fogwall_state *state);
void output_render(struct fogwall_output *output);
void output_set_paused_hypr(struct fogwall_output *output, bool paused);
int64_t now_ms(void);

#endif
