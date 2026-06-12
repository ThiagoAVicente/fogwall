#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

#include "wayland.h"

static volatile sig_atomic_t quit = 0;

static void handle_signal(int sig)
{
    (void)sig;
    quit = 1;
}

static bool parse_color(const char *s, float out[3])
{
    if (*s == '#') {
        s++;
    }
    if (strlen(s) != 6) {
        return false;
    }
    unsigned int r, g, b;
    if (sscanf(s, "%2x%2x%2x", &r, &g, &b) != 3) {
        return false;
    }
    out[0] = (float)r / 255.0f;
    out[1] = (float)g / 255.0f;
    out[2] = (float)b / 255.0f;
    return true;
}

static void usage(FILE *to)
{
    fprintf(to,
        "usage: fogwall [--color <hex>] [--output <name>] [--fps <n>]\n"
        "\n"
        "  --color <hex>    highlight tint, e.g. \"#a0c8ff\" (default #ffffff)\n"
        "  --output <name>  render only on this output, e.g. eDP-1\n"
        "                   (default: all outputs)\n"
        "  --fps <n>        frame cap, 1-240 (default 24)\n"
        "  -h, --help       show this help\n"
        "  -V, --version    show version\n");
}

int main(int argc, char *argv[])
{
    struct fogwall_state state = {
        .cfg = {
            .color = { 1.0f, 1.0f, 1.0f },
            .output_name = NULL,
            .fps = 24,
        },
        .running = true,
    };

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--color") == 0 && i + 1 < argc) {
            if (!parse_color(argv[++i], state.cfg.color)) {
                fprintf(stderr, "fogwall: invalid color \"%s\"\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            state.cfg.output_name = argv[++i];
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            state.cfg.fps = atoi(argv[++i]);
            if (state.cfg.fps < 1 || state.cfg.fps > 240) {
                fprintf(stderr, "fogwall: --fps must be 1-240\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(argv[i], "-V") == 0 ||
                   strcmp(argv[i], "--version") == 0) {
            puts("fogwall 0.1.0");
            return 0;
        } else {
            fprintf(stderr, "fogwall: unknown argument \"%s\"\n", argv[i]);
            usage(stderr);
            return 1;
        }
    }

    struct sigaction sa = { .sa_handler = handle_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (!wayland_init(&state)) {
        wayland_finish(&state);
        return 1;
    }

    const int64_t interval = 1000 / state.cfg.fps;
    struct pollfd pfd = {
        .fd = wl_display_get_fd(state.display),
        .events = POLLIN,
    };

    /* One iteration per wakeup; wakeups happen only on Wayland events
     * (frame callbacks, toplevel state changes) or on the --fps deadline of
     * an output whose frame callback already fired. Zero work while every
     * output is paused or occluded. */
    while (!quit && state.running) {
        while (wl_display_prepare_read(state.display) != 0) {
            if (wl_display_dispatch_pending(state.display) < 0) {
                goto disconnected;
            }
        }
        if (wl_display_flush(state.display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(state.display);
            goto disconnected;
        }

        int timeout = -1;
        int64_t now = now_ms();
        struct fogwall_output *o;
        wl_list_for_each(o, &state.outputs, link) {
            if (!o->frame_ready || o->paused) {
                continue;
            }
            int64_t due = o->last_frame_ms + interval - now;
            if (due < 0) {
                due = 0;
            }
            if (timeout < 0 || due < timeout) {
                timeout = (int)due;
            }
        }

        int ret = poll(&pfd, 1, timeout);
        if (ret < 0) {
            wl_display_cancel_read(state.display);
            if (errno == EINTR) {
                continue;
            }
            perror("fogwall: poll");
            break;
        }
        if (ret > 0 && (pfd.revents & POLLIN)) {
            if (wl_display_read_events(state.display) < 0) {
                goto disconnected;
            }
        } else {
            wl_display_cancel_read(state.display);
        }
        if (wl_display_dispatch_pending(state.display) < 0) {
            goto disconnected;
        }

        now = now_ms();
        wl_list_for_each(o, &state.outputs, link) {
            if (o->frame_ready && !o->paused &&
                    now - o->last_frame_ms >= interval) {
                output_render(o);
            }
        }
    }

    wayland_finish(&state);
    return 0;

disconnected:
    fprintf(stderr, "fogwall: lost connection to Wayland display\n");
    wayland_finish(&state);
    return 1;
}
