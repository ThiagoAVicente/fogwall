#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "wayland.h"
#include "hypr.h"

#define HYPR_MAX_MONITORS 16

struct hypr_monitor {
    char name[64];
    long active_ws;
    bool fullscreen;
};

static int hypr_connect(const char *socket_name)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (runtime == NULL || sig == NULL) {
        return -1;
    }
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    int n = snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/hypr/%s/%s",
                     runtime, sig, socket_name);
    if (n < 0 || (size_t)n >= sizeof(addr.sun_path)) {
        return -1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* One-shot command on .socket.sock; returns bytes read into buf, -1 on
 * error. Response is NUL-terminated. */
static int hypr_query(const char *cmd, char *buf, size_t cap)
{
    int fd = hypr_connect(".socket.sock");
    if (fd < 0) {
        return -1;
    }
    size_t len = strlen(cmd);
    if (write(fd, cmd, len) != (ssize_t)len) {
        close(fd);
        return -1;
    }
    size_t got = 0;
    for (;;) {
        ssize_t r = read(fd, buf + got, cap - 1 - got);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            break;
        }
        got += (size_t)r;
        if (got >= cap - 1) {
            break;
        }
    }
    close(fd);
    buf[got] = '\0';
    return (int)got;
}

/* Minimal JSON field scanning over Hyprland's output. Object boundaries are
 * tracked by brace depth so nested objects (activeWorkspace etc.) don't
 * shadow top-level fields. */

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    return p;
}

/* At `p` pointing past `"key":`, extract a string value into out. */
static void scan_string(const char *p, char *out, size_t cap)
{
    p = skip_ws(p);
    if (*p != '"') {
        out[0] = '\0';
        return;
    }
    p++;
    size_t i = 0;
    while (*p != '\0' && *p != '"' && i + 1 < cap) {
        out[i++] = *p++;
    }
    out[i] = '\0';
}

/* Parse `j/monitors` output: name + activeWorkspace.id per monitor. */
static int parse_monitors(const char *json, struct hypr_monitor *mons,
                          int cap)
{
    /* Top-level monitor objects sit at brace depth 1 (the array adds no
     * depth); their nested objects (activeWorkspace, …) at depth 2. */
    int count = 0;
    int depth = 0;
    bool in_active_ws = false;
    const char *p = json;
    while (*p != '\0') {
        if (*p == '{') {
            depth++;
            if (depth == 1 && count < cap) {
                mons[count].name[0] = '\0';
                mons[count].active_ws = LONG_MIN;
                mons[count].fullscreen = false;
            }
        } else if (*p == '}') {
            if (depth == 1 && count < cap) {
                count++;
            }
            if (depth == 2) {
                in_active_ws = false;
            }
            depth--;
        } else if (*p == '"' && count < cap) {
            if (depth == 1 && strncmp(p, "\"name\":", 7) == 0) {
                scan_string(p + 7, mons[count].name,
                            sizeof(mons[count].name));
                p += 6;
            } else if (depth == 1 &&
                       strncmp(p, "\"activeWorkspace\":", 18) == 0) {
                in_active_ws = true;
                p += 17;
            } else if (depth == 2 && in_active_ws &&
                       strncmp(p, "\"id\":", 5) == 0) {
                mons[count].active_ws = strtol(skip_ws(p + 5), NULL, 10);
                p += 4;
            }
        }
        p++;
    }
    return count;
}

/* Parse `j/workspaces` output; mark monitors whose active workspace has a
 * fullscreen window. */
static void parse_workspaces(const char *json, struct hypr_monitor *mons,
                             int n_mons)
{
    int depth = 0;
    long ws_id = LONG_MIN;
    bool ws_fullscreen = false;
    const char *p = json;
    while (*p != '\0') {
        if (*p == '{') {
            depth++;
            if (depth == 1) {
                ws_id = LONG_MIN;
                ws_fullscreen = false;
            }
        } else if (*p == '}') {
            if (depth == 1 && ws_fullscreen) {
                for (int i = 0; i < n_mons; i++) {
                    if (mons[i].active_ws == ws_id) {
                        mons[i].fullscreen = true;
                    }
                }
            }
            depth--;
        } else if (*p == '"' && depth == 1) {
            if (strncmp(p, "\"id\":", 5) == 0) {
                ws_id = strtol(skip_ws(p + 5), NULL, 10);
                p += 4;
            } else if (strncmp(p, "\"hasfullscreen\":", 16) == 0) {
                ws_fullscreen = strncmp(skip_ws(p + 16), "true", 4) == 0;
                p += 15;
            }
        }
        p++;
    }
}

void hypr_refresh(struct fogwall_state *st)
{
    static char buf[65536];
    struct hypr_monitor mons[HYPR_MAX_MONITORS];

    if (hypr_query("j/monitors", buf, sizeof(buf)) <= 0) {
        return;
    }
    int n_mons = parse_monitors(buf, mons, HYPR_MAX_MONITORS);
    if (n_mons == 0) {
        return;
    }
    if (hypr_query("j/workspaces", buf, sizeof(buf)) <= 0) {
        return;
    }
    parse_workspaces(buf, mons, n_mons);

    struct fogwall_output *o;
    wl_list_for_each(o, &st->outputs, link) {
        if (o->name == NULL) {
            continue;
        }
        for (int i = 0; i < n_mons; i++) {
            if (strcmp(mons[i].name, o->name) == 0) {
                output_set_paused_hypr(o, mons[i].fullscreen);
                break;
            }
        }
    }
}

int hypr_init(struct fogwall_state *st)
{
    st->hypr_fd = hypr_connect(".socket2.sock");
    st->hypr_buf_len = 0;
    if (st->hypr_fd >= 0) {
        fcntl(st->hypr_fd, F_SETFL, O_NONBLOCK);
        hypr_refresh(st);
    }
    return st->hypr_fd;
}

static bool hypr_event_relevant(const char *line)
{
    static const char *prefixes[] = {
        "fullscreen>>", "workspace>>", "workspacev2>>", "focusedmon>>",
        "focusedmonv2>>", "openwindow>>", "closewindow>>", "movewindow>>",
        "movewindowv2>>", "monitoradded>>", "monitorremoved>>",
    };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        if (strncmp(line, prefixes[i], strlen(prefixes[i])) == 0) {
            return true;
        }
    }
    return false;
}

void hypr_handle_events(struct fogwall_state *st)
{
    bool refresh = false;
    for (;;) {
        ssize_t r = read(st->hypr_fd, st->hypr_buf + st->hypr_buf_len,
                         sizeof(st->hypr_buf) - 1 - st->hypr_buf_len);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        if (r <= 0) {
            /* Hyprland went away; the Wayland display is dying too. */
            close(st->hypr_fd);
            st->hypr_fd = -1;
            return;
        }
        st->hypr_buf_len += (size_t)r;
        st->hypr_buf[st->hypr_buf_len] = '\0';

        char *line = st->hypr_buf;
        char *nl;
        while ((nl = strchr(line, '\n')) != NULL) {
            *nl = '\0';
            if (hypr_event_relevant(line)) {
                refresh = true;
            }
            line = nl + 1;
        }
        size_t rest = st->hypr_buf_len - (size_t)(line - st->hypr_buf);
        memmove(st->hypr_buf, line, rest);
        st->hypr_buf_len = rest;
        if (st->hypr_buf_len >= sizeof(st->hypr_buf) - 1) {
            st->hypr_buf_len = 0; /* pathological line; drop it */
        }
    }
    if (refresh) {
        hypr_refresh(st);
    }
}

void hypr_finish(struct fogwall_state *st)
{
    if (st->hypr_fd >= 0) {
        close(st->hypr_fd);
        st->hypr_fd = -1;
    }
}
