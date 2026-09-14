# Music-reactive aesthetic features Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add beat-sync fog speed pulses, Spotify album-art tint sync, and idle hue drift to fogwall.

**Architecture:** Three additive, independently-gated subsystems layered on the existing PipeWire loudness capture and poll() main loop. Beat detection extends `audio.c`'s existing envelope math. Album-art sync is a new `mpris.c` module (DBus, gated by `HAVE_DBUS`) feeding a color target that `wayland.c` lerps into `cfg.color` each frame. Idle drift is a pure shader change gated on the existing `uMusic` uniform.

**Tech Stack:** C99, PipeWire (existing), libdbus-1 (new, optional), vendored `stb_image.h` (new, header-only), GLSL ES 2.0.

**No test framework exists in this project** (systems-level Wayland/EGL/DBus C — no unit-testable surface without real compositor/bus/hardware). Verification throughout is: build succeeds, `fogwall` runs, behavior checked by log output / visual inspection, matching how the existing Hyprland-IPC and PipeWire features in this codebase were verified (see `README.md`'s measurement notes).

---

## File Structure

- Modify `src/audio.c` / `src/audio.h` — add onset detection, `audio_beat` output (Task 1)
- Modify `src/wayland.h` — add `audio_beat`, `u_beat`, `mpris_fd`, DBus-driven color target fields (Tasks 1, 2)
- Modify `src/wayland.c` — wire `u_beat` uniform, lerp `cfg.color` toward `mpris_color` (Tasks 1, 2)
- Modify `src/shaders/fog.frag` — `uBeat` drives path speed; idle hue drift (Tasks 1, 3)
- Modify `src/main.c` — poll() gets a 4th fd (mpris), decay/lerp step (Task 2)
- Create `src/mpris.c` / `src/mpris.h` — MPRIS DBus watch + art decode (Task 2)
- Create `third_party/stb_image.h` — vendored single-header JPEG/PNG decoder (Task 2)
- Modify `meson.build` — optional `dbus-1` dependency, new sources (Task 2)
- Modify `README.md` — document all three features (Task 4)

---

### Task 1: Beat-sync speed pulse

**Files:**
- Modify: `src/audio.h`
- Modify: `src/audio.c:20-37,49-80,217-246,258-271,273-298,300-318`
- Modify: `src/wayland.h:47-51`
- Modify: `src/wayland.c:57-73`
- Modify: `src/shaders/fog.frag`
- Modify: `src/main.c:107-113,168-196`

- [ ] **Step 1: Add `audio_beat` to shared state**

In `src/wayland.h`, extend the audio block (currently lines 47-51):

```c
    /* Spotify loudness (see audio.h); -1 when unavailable */
    int audio_fd;
    float audio_level; /* loudness envelope 0..1, fast */
    float audio_music; /* "music is playing" envelope 0..1, slow release */
    float audio_beat;  /* onset kick, 0..1, fast decay (see audio.c) */
```

- [ ] **Step 2: Document the new output in `audio.h`**

In `src/audio.h`, update the top comment (currently lines 6-10):

```c
/* Optional Spotify loudness capture via PipeWire (compiled out without
 * libpipewire-0.3). A capture stream attaches to Spotify's output node
 * whenever one exists and feeds a smoothed 0..1 envelope into
 * state->audio_level (the fog shader pulses with it) and a fast-decaying
 * onset kick into state->audio_beat (the fog shader lurches on it). No
 * Spotify node, no stream — zero overhead. The PipeWire loop fd joins the
 * main poll(). */
```

- [ ] **Step 3: Track a running loudness average and detect onsets in `on_process`**

In `src/audio.c`, add a field to the file-local `audio` struct (currently lines 20-37):

```c
static struct {
    struct pw_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_registry *registry;
    struct spa_hook registry_listener;
    struct spa_hook core_listener;
    struct pw_stream *stream;
    struct spa_hook stream_listener;
    struct fogwall_state *st;
    uint32_t spotify_id;
    /* PipeWire clients identified as Spotify; their output nodes carry no
     * application.name themselves, only client.id. */
    uint32_t clients[8];
    int n_clients;
    bool active;
    bool dead;
    /* Onset detection: running average of target loudness, refractory
     * window so one transient doesn't fire multiple onsets. */
    float level_avg;
    int64_t last_onset_ms;
} audio;
```

Add `#include "wayland.h"` already present (it is, line 8) — `now_ms()` is declared there.

Replace the body of `on_process` (currently lines 49-80) with:

```c
static void on_process(void *data)
{
    (void)data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(audio.stream);
    if (b == NULL) {
        return;
    }
    struct spa_data *d = &b->buffer->datas[0];
    if (d->data != NULL && d->chunk->size >= sizeof(float)) {
        const float *samples = d->data;
        uint32_t n = d->chunk->size / sizeof(float);
        double sum = 0.0;
        for (uint32_t i = 0; i < n; i++) {
            sum += (double)samples[i] * (double)samples[i];
        }
        float rms = (float)sqrt(sum / (double)n);
        float target = rms * 5.0f;
        if (target > 1.0f) {
            target = 1.0f;
        }
        /* Attack only — release is wall-clock-driven in the main loop, so
         * the pulse dies out even when Spotify pauses and buffers stop. */
        float *lvl = &audio.st->audio_level;
        if (target > *lvl) {
            *lvl += (target - *lvl) * 0.7f;
        }
        if (target > 0.02f) {
            audio.st->audio_music = 1.0f;
        }

        /* Onset: this buffer's target loudness clears the running average
         * by a margin, and we're past the refractory window. 50 ms buffers
         * -> avg tracks slowly (0.15) so real beats still stand out over
         * sustained loud passages. */
        int64_t now = now_ms();
        if (target > audio.level_avg * 1.35f + 0.05f &&
                now - audio.last_onset_ms > 200) {
            audio.st->audio_beat = 1.0f;
            audio.last_onset_ms = now;
        }
        audio.level_avg += (target - audio.level_avg) * 0.15f;
    }
    pw_stream_queue_buffer(audio.stream, b);
}
```

- [ ] **Step 4: Reset beat state alongside the existing envelopes**

In `src/audio.c`, `destroy_stream` (currently lines 87-96) — add `audio.st->audio_beat = 0.0f;` next to the existing resets:

```c
static void destroy_stream(void)
{
    if (audio.stream != NULL) {
        pw_stream_destroy(audio.stream);
        audio.stream = NULL;
    }
    audio.spotify_id = 0;
    audio.st->audio_level = 0.0f;
    audio.st->audio_music = 0.0f;
    audio.st->audio_beat = 0.0f;
}
```

Do the same in `audio_init` (currently lines 217-246, right after `st->audio_music = 0.0f;`), `audio_set_active`'s `if (!active)` branch (currently lines 258-271), and `audio_finish` (currently lines 273-298) — each currently sets `audio_level`/`audio_music` to 0; add the matching `audio_beat = 0.0f;` line beside each. Also reset `audio.level_avg = 0.0f; audio.last_onset_ms = 0;` in `audio_init`.

And in the `#else /* !HAVE_PIPEWIRE */` stub block (currently lines 300-318), add `st->audio_beat = 0.0f;` to the stub `audio_init`.

- [ ] **Step 5: Decay `audio_beat` in the main loop**

In `src/main.c`, the wall-clock decay block (currently lines 179-195) — add a fast decay for the beat kick (faster than `audio_level`'s 5.0 so it reads as a sharp lurch, not a sustained push):

```c
        if (state.audio_level > 0.0f) {
            state.audio_level *= expf(-dt * 5.0f);
            if (state.audio_level < 0.004f) {
                state.audio_level = 0.0f;
            }
        }
        if (state.audio_beat > 0.0f) {
            state.audio_beat *= expf(-dt * 8.0f);
            if (state.audio_beat < 0.004f) {
                state.audio_beat = 0.0f;
            }
        }
        if (state.audio_music > 0.0f) {
            state.audio_music *= expf(-dt * 0.7f);
            if (state.audio_music < 0.01f) {
                state.audio_music = 0.0f;
            }
        }
```

- [ ] **Step 6: Wire `uBeat` uniform**

In `src/wayland.h`, add to the uniform locations block (currently lines 30-36, alongside `u_level`/`u_music`):

```c
    GLint u_level;
    GLint u_music;
    GLint u_beat;
```

In `src/wayland.c`, `output_render` (currently lines 57-73):

```c
        st->u_level = glGetUniformLocation(st->program, "uLevel");
        st->u_music = glGetUniformLocation(st->program, "uMusic");
        st->u_beat = glGetUniformLocation(st->program, "uBeat");
```

and after `glUniform1f(st->u_music, st->audio_music);` (line 73):

```c
    glUniform1f(st->u_beat, st->audio_beat);
```

- [ ] **Step 7: Make the beat drive Lissajous path speed in the shader**

In `src/shaders/fog.frag`, add the uniform declaration next to `uMusic`:

```glsl
uniform float uMusic;     /* music presence 0..1; gates the hue cycling */
uniform float uBeat;      /* onset kick 0..1, fast decay; nudges path speed */
```

Change the path-speed math in `main()`. Currently `t` alone drives every `sin(k * t)`. Introduce a beat-modulated time for the path terms only (leave `fdrift` and `field` on plain `t` so the noise texture itself doesn't visibly jump):

```glsl
    /* Beat kick nudges path speed only (not the noise field) — a sharp,
     * short-lived lurch in blob motion without disturbing the fog texture. */
    float tp = t * (1.0 + 0.5 * uBeat);

    /* 4 big fog masses on Lissajous paths — mismatched harmonics make each
     * one keep changing direction instead of orbiting. Music swells them. */
    float pulse = 1.0 + 0.25 * uLevel;
    float glow = 0.0;
    glow += blob(uv, 0.45 * vec2(sin(2.0 * tp + 0.5), sin(3.0 * tp + 1.7)),
                 0.38 * pulse, field);
    glow += blob(uv, 0.50 * vec2(sin(3.0 * tp + 2.9), sin(5.0 * tp + 0.4)),
                 0.33 * pulse, field);
    glow += blob(uv, 0.55 * vec2(sin(5.0 * tp + 4.2), sin(2.0 * tp + 3.1)),
                 0.30 * pulse, field);
    glow += blob(uv, 0.42 * vec2(sin(7.0 * tp + 1.1), sin(4.0 * tp + 5.0)),
                 0.36 * pulse, field);
```

- [ ] **Step 8: Build and smoke-test**

```bash
meson compile -C build
```
Expected: builds clean, no new warnings (project uses `warning_level=2`).

```bash
./build/fogwall --fps 30 &
sleep 2 && kill %1
```
Expected: runs and exits cleanly without crashing (no Spotify needed — `audio_beat` just stays 0, matching existing `audio_level` behavior when idle).

- [ ] **Step 9: Commit**

```bash
git add src/audio.c src/audio.h src/wayland.h src/wayland.c src/main.c src/shaders/fog.frag
git commit -m "Add beat-sync: onset detection kicks fog path speed"
```

---

### Task 2: Album-art tint sync (MPRIS + DBus)

**Files:**
- Create: `third_party/stb_image.h` (vendored, already staged at
  `/tmp/claude-1000/-home-vcnt-Projects-fogwall/bea85bd6-e2e0-47ae-aca8-bf63c3a546ff/scratchpad/stb_image.h`
  — copy it in as part of Step 1)
- Create: `src/mpris.c`
- Create: `src/mpris.h`
- Modify: `meson.build`
- Modify: `src/wayland.h`
- Modify: `src/wayland.c:57-73`
- Modify: `src/main.c:12-14,105-113,148-149,168-196`

- [ ] **Step 1: Vendor stb_image.h**

```bash
mkdir -p third_party
cp /tmp/claude-1000/-home-vcnt-Projects-fogwall/bea85bd6-e2e0-47ae-aca8-bf63c3a546ff/scratchpad/stb_image.h third_party/stb_image.h
```

Add a `third_party/README.md` one-liner:

```bash
cat > third_party/README.md <<'EOF'
Vendored single-header libraries.

- `stb_image.h` — https://github.com/nothings/stb (public domain / MIT), used
  by `src/mpris.c` to decode cached Spotify album art (JPEG/PNG) for tint
  extraction.
EOF
```

- [ ] **Step 2: Write `src/mpris.h`**

```c
#ifndef FOGWALL_MPRIS_H
#define FOGWALL_MPRIS_H

#include <stdbool.h>

/* Optional Spotify album-art tint sync via MPRIS over the D-Bus session bus
 * (compiled out without libdbus-1). Watches Spotify's MPRIS Metadata
 * property; on track change, decodes the cached cover art (mpris:artUrl is
 * always a local file:// URI for Spotify) and extracts a dominant color,
 * smoothly lerped into state->cfg.color. Falls back to the static --color
 * value with no Spotify, no D-Bus, or a decode failure. The D-Bus
 * connection fd joins the main poll(). */

struct fogwall_state;

/* Sets state->mpris_fd (-1 when unavailable). */
void mpris_init(struct fogwall_state *state);

/* Call when state->mpris_fd is readable. */
void mpris_dispatch(struct fogwall_state *state);

void mpris_finish(struct fogwall_state *state);

#endif
```

- [ ] **Step 3: Add MPRIS/tint fields to shared state**

In `src/wayland.h`, extend `struct fogwall_config` and `struct fogwall_state`. `cfg.color` stays the user's `--color` fallback; add a separate live target + current tint so `wayland.c` can lerp without mutating the user's original flag value:

```c
struct fogwall_config {
    float color[3];          /* highlight tint, linear-ish 0..1 */
    const char *output_name; /* NULL = all outputs */
    int fps;
};
```
(unchanged — keep `cfg.color` as the fallback/default)

Add after the `audio_beat` field added in Task 1:

```c
    /* MPRIS album-art tint (see mpris.h); -1 when unavailable */
    int mpris_fd;
    float art_color[3];   /* dominant color of current album art */
    bool art_color_valid; /* false until first successful decode */
    float tint_color[3];  /* live, lerped toward cfg.color or art_color */
```

- [ ] **Step 4: Write `src/mpris.c`**

```c
#define _GNU_SOURCE /* strcasestr */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wayland.h"
#include "mpris.h"

#ifdef HAVE_DBUS

#include <dbus/dbus.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO_FAILURE_STRINGS
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "stb_image.h"

static struct {
    DBusConnection *conn;
    struct fogwall_state *st;
    char last_art_path[1024];
} mpris;

/* file:// URI -> filesystem path, in place. Only handles the plain,
 * unescaped form Spotify emits for its local art cache. */
static bool uri_to_path(const char *uri, char *out, size_t out_sz)
{
    if (strncmp(uri, "file://", 7) != 0) {
        return false;
    }
    snprintf(out, out_sz, "%s", uri + 7);
    return true;
}

static void extract_dominant_color(const char *path, float rgb[3])
{
    int w, h, channels;
    unsigned char *pix = stbi_load(path, &w, &h, &channels, 3);
    if (pix == NULL) {
        return;
    }
    double sum[3] = { 0, 0, 0 };
    long n = (long)w * h;
    for (long i = 0; i < n; i++) {
        sum[0] += pix[i * 3 + 0];
        sum[1] += pix[i * 3 + 1];
        sum[2] += pix[i * 3 + 2];
    }
    stbi_image_free(pix);
    if (n == 0) {
        return;
    }
    float r = (float)(sum[0] / n / 255.0);
    float g = (float)(sum[1] / n / 255.0);
    float b = (float)(sum[2] / n / 255.0);

    /* Mild saturation boost so the average doesn't read as muddy grey —
     * push each channel away from the luma midpoint. */
    float luma = 0.3f * r + 0.59f * g + 0.11f * b;
    const float boost = 1.35f;
    rgb[0] = luma + (r - luma) * boost;
    rgb[1] = luma + (g - luma) * boost;
    rgb[2] = luma + (b - luma) * boost;
    for (int i = 0; i < 3; i++) {
        if (rgb[i] < 0.0f) rgb[i] = 0.0f;
        if (rgb[i] > 1.0f) rgb[i] = 1.0f;
    }
}

static void handle_art_url(const char *url)
{
    if (strcmp(url, mpris.last_art_path) == 0) {
        return;
    }
    char path[1024];
    if (!uri_to_path(url, path, sizeof(path))) {
        return; /* remote art URL (non-Spotify player) — skip, keep old tint */
    }
    float rgb[3];
    extract_dominant_color(path, rgb);
    if (rgb[0] == 0.0f && rgb[1] == 0.0f && rgb[2] == 0.0f) {
        return; /* decode failed */
    }
    snprintf(mpris.last_art_path, sizeof(mpris.last_art_path), "%s", url);
    mpris.st->art_color[0] = rgb[0];
    mpris.st->art_color[1] = rgb[1];
    mpris.st->art_color[2] = rgb[2];
    mpris.st->art_color_valid = true;
}

/* Metadata arrives as a{sv}; we only care about the "mpris:artUrl" entry,
 * which is a string. */
static void handle_metadata_variant(DBusMessageIter *variant_iter)
{
    if (dbus_message_iter_get_arg_type(variant_iter) != DBUS_TYPE_ARRAY) {
        return;
    }
    DBusMessageIter arr;
    dbus_message_iter_recurse(variant_iter, &arr);
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&arr, &entry);
        const char *key = NULL;
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&entry, &key);
        }
        dbus_message_iter_next(&entry);
        if (key != NULL && strcmp(key, "mpris:artUrl") == 0 &&
                dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
            DBusMessageIter val;
            dbus_message_iter_recurse(&entry, &val);
            if (dbus_message_iter_get_arg_type(&val) == DBUS_TYPE_STRING) {
                const char *url = NULL;
                dbus_message_iter_get_basic(&val, &url);
                if (url != NULL) {
                    handle_art_url(url);
                }
            }
        }
        dbus_message_iter_next(&arr);
    }
}

/* org.freedesktop.DBus.Properties.PropertiesChanged(interface, changed,
 * invalidated) — we only look at "changed"'s "Metadata" entry. */
static DBusHandlerResult on_message(DBusConnection *conn, DBusMessage *msg,
        void *data)
{
    (void)conn; (void)data;
    if (!dbus_message_is_signal(msg, "org.freedesktop.DBus.Properties",
            "PropertiesChanged")) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    DBusMessageIter args;
    if (!dbus_message_iter_init(msg, &args) ||
            dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    dbus_message_iter_next(&args); /* skip interface name */
    if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_ARRAY) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    DBusMessageIter changed;
    dbus_message_iter_recurse(&args, &changed);
    while (dbus_message_iter_get_arg_type(&changed) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&changed, &entry);
        const char *key = NULL;
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&entry, &key);
        }
        dbus_message_iter_next(&entry);
        if (key != NULL && strcmp(key, "Metadata") == 0 &&
                dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
            DBusMessageIter variant;
            dbus_message_iter_recurse(&entry, &variant);
            handle_metadata_variant(&variant);
        }
        dbus_message_iter_next(&changed);
    }
    return DBUS_HANDLER_RESULT_HANDLED;
}

void mpris_init(struct fogwall_state *st)
{
    st->mpris_fd = -1;
    st->art_color_valid = false;
    mpris.st = st;
    mpris.last_art_path[0] = '\0';

    DBusError err;
    dbus_error_init(&err);
    mpris.conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (mpris.conn == NULL) {
        dbus_error_free(&err);
        return;
    }
    dbus_connection_set_exit_on_disconnect(mpris.conn, FALSE);
    dbus_bus_add_match(mpris.conn,
        "type='signal',interface='org.freedesktop.DBus.Properties',"
        "member='PropertiesChanged',"
        "path='/org/mpris/MediaPlayer2',"
        "sender='org.mpris.MediaPlayer2.spotify'",
        &err);
    if (dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        dbus_connection_close(mpris.conn);
        dbus_connection_unref(mpris.conn);
        mpris.conn = NULL;
        return;
    }
    dbus_connection_add_filter(mpris.conn, on_message, NULL, NULL);

    int fd = -1;
    if (!dbus_connection_get_unix_fd(mpris.conn, &fd) || fd < 0) {
        dbus_connection_close(mpris.conn);
        dbus_connection_unref(mpris.conn);
        mpris.conn = NULL;
        return;
    }
    st->mpris_fd = fd;
}

void mpris_dispatch(struct fogwall_state *st)
{
    if (st->mpris_fd < 0 || mpris.conn == NULL) {
        return;
    }
    dbus_connection_read_write(mpris.conn, 0);
    while (dbus_connection_dispatch(mpris.conn) == DBUS_DISPATCH_DATA_REMAINS) {
        /* drain */
    }
    (void)st;
}

void mpris_finish(struct fogwall_state *st)
{
    if (mpris.conn != NULL) {
        dbus_connection_close(mpris.conn);
        dbus_connection_unref(mpris.conn);
        mpris.conn = NULL;
    }
    st->mpris_fd = -1;
}

#else /* !HAVE_DBUS */

void mpris_init(struct fogwall_state *st)
{
    st->mpris_fd = -1;
    st->art_color_valid = false;
}

void mpris_dispatch(struct fogwall_state *st) { (void)st; }

void mpris_finish(struct fogwall_state *st) { (void)st; }

#endif
```

- [ ] **Step 5: Add optional dbus-1 dependency to the build**

In `meson.build`, next to the existing `pipewire` block:

```meson
pipewire = dependency('libpipewire-0.3', required: false)
if pipewire.found()
  add_project_arguments('-DHAVE_PIPEWIRE', language: 'c')
endif

dbus = dependency('dbus-1', required: false)
if dbus.found()
  add_project_arguments('-DHAVE_DBUS', language: 'c')
endif
```

Add `src/mpris.c` to the executable's source list and `dbus` to `dependencies`:

```meson
executable(
  'fogwall',
  [
    'src/main.c',
    'src/wayland.c',
    'src/egl.c',
    'src/shader.c',
    'src/hypr.c',
    'src/audio.c',
    'src/mpris.c',
  ] + protocol_srcs + shader_hdrs,
  dependencies: [wayland_client, wayland_egl, egl, glesv2, libm, pipewire, dbus],
  install: true,
)
```

Add an include path for the vendored header (stb_image.h is only `#include`d from `mpris.c`, which lives in `src/`, so add `third_party` to the include dirs):

```meson
executable(
  'fogwall',
  [
    'src/main.c',
    'src/wayland.c',
    'src/egl.c',
    'src/shader.c',
    'src/hypr.c',
    'src/audio.c',
    'src/mpris.c',
  ] + protocol_srcs + shader_hdrs,
  include_directories: include_directories('third_party'),
  dependencies: [wayland_client, wayland_egl, egl, glesv2, libm, pipewire, dbus],
  install: true,
)
```

- [ ] **Step 6: Wire mpris into the main loop**

In `src/main.c`, add the include (currently lines 12-14):

```c
#include "wayland.h"
#include "hypr.h"
#include "audio.h"
#include "mpris.h"
```

After `audio_init(&state);` (currently line 105):

```c
    hypr_init(&state);
    audio_init(&state);
    mpris_init(&state);
    state.tint_color[0] = state.cfg.color[0];
    state.tint_color[1] = state.cfg.color[1];
    state.tint_color[2] = state.cfg.color[2];
```

Expand the pollfd array (currently lines 109-113) from 3 to 4:

```c
    struct pollfd pfds[4] = {
        { .fd = wl_display_get_fd(state.display), .events = POLLIN },
        { .fd = state.hypr_fd, .events = POLLIN },
        { .fd = state.audio_fd, .events = POLLIN },
        { .fd = state.mpris_fd, .events = POLLIN },
    };
```

Update `poll()`'s fd count and the per-iteration fd refresh (currently lines 147-149):

```c
        pfds[1].fd = state.hypr_fd;
        pfds[2].fd = state.audio_fd;
        pfds[3].fd = state.mpris_fd;
        int ret = poll(pfds, 4, timeout);
```

After the existing audio dispatch block (currently lines 171-175):

```c
        if (pfds[2].revents & (POLLERR | POLLHUP)) {
            audio_finish(&state);
        } else if (pfds[2].revents & POLLIN) {
            audio_dispatch(&state);
        }
        if (pfds[3].revents & (POLLERR | POLLHUP)) {
            mpris_finish(&state);
        } else if (pfds[3].revents & POLLIN) {
            mpris_dispatch(&state);
        }
```

In the wall-clock decay block (currently lines 179-195, after the `audio_beat`/`audio_music` decay added in Task 1), lerp `tint_color` toward whichever source is active — album art when valid, else the static `--color`:

```c
        const float *target = state.art_color_valid ? state.art_color
                                                      : state.cfg.color;
        for (int i = 0; i < 3; i++) {
            /* ~2s time constant: 1 - exp(-dt/2) */
            state.tint_color[i] +=
                (target[i] - state.tint_color[i]) * (1.0f - expf(-dt * 0.5f));
        }
```

Add `mpris_finish(&state);` next to the existing `audio_finish(&state);` calls at both the normal exit path (currently line 205) and the `disconnected:` path (currently line 212).

- [ ] **Step 7: Use `tint_color` instead of `cfg.color` in rendering**

In `src/wayland.c`, `output_render` (currently line 70-71) — replace `st->cfg.color` with `st->tint_color`:

```c
    glUniform3f(st->u_highlight, st->tint_color[0], st->tint_color[1],
                st->tint_color[2]);
```

- [ ] **Step 8: Build and smoke-test**

```bash
meson setup build --reconfigure --buildtype=release
meson compile -C build
```
Expected: `dbus = YES` in the configure summary (confirm with `meson configure build | grep -i dbus` if unsure), clean build.

```bash
./build/fogwall --fps 30 --color '#a0c8ff' &
sleep 2 && kill %1
```
Expected: runs cleanly with no Spotify/DBus session issues (falls back to `--color`, `art_color_valid` stays false).

If a DBus session bus and Spotify are available, run interactively and switch tracks — `tint_color` should ease toward the new album art's dominant color over ~2s. This step is manual/environment-dependent; note in the commit message if it couldn't be exercised live.

- [ ] **Step 9: Commit**

```bash
git add third_party/ src/mpris.c src/mpris.h meson.build src/wayland.h src/wayland.c src/main.c
git commit -m "Add MPRIS album-art tint sync via D-Bus"
```

---

### Task 3: Idle hue drift

**Files:**
- Modify: `src/shaders/fog.frag`

- [ ] **Step 1: Add a slow, idle-only hue rotation**

In `src/shaders/fog.frag`, after the existing tone/tint block (the one using `uMusic`), add a hue-rotation term that only appears as `uMusic` falls to 0, so it never fights the album-art/music sway from Task 2 (existing feature) while music plays:

```glsl
    /* Music sways only the tone and strength of the configured color —
     * the hue itself never changes. A slow sweep (12 s, integer factor 40
     * stays seamless across the iTime wrap) breathes between the pure
     * --color and a softer, dimmer version of it; uMusic eases everything
     * back to the exact --color when the music stops. */
    float tone = uMusic * (0.5 + 0.5 * sin(40.0 * t));
    vec3 grey = vec3(dot(uHighlight, vec3(0.3333)));
    vec3 tint = mix(uHighlight, grey, 0.45 * tone);
    tint *= 1.0 - 0.20 * tone;

    /* Idle drift: when nothing is playing (uMusic -> 0), slowly rotate hue
     * so a silent screen isn't perfectly static. Integer factor 4 keeps the
     * ~2 min rotation seamless across the iTime wrap; fades out smoothly as
     * uMusic rises so a resumed track snaps back to the real tint. */
    float drift = (1.0 - uMusic) * (4.0 * t);
    float ca = cos(drift), sa = sin(drift);
    mat3 hueRot = mat3(
        0.299 + 0.701 * ca + 0.168 * sa, 0.299 - 0.299 * ca - 0.328 * sa, 0.299 - 0.300 * ca + 1.250 * sa,
        0.587 - 0.587 * ca + 0.330 * sa, 0.587 + 0.413 * ca + 0.035 * sa, 0.587 - 0.588 * ca - 1.050 * sa,
        0.114 - 0.114 * ca - 0.497 * sa, 0.114 - 0.114 * ca + 0.292 * sa, 0.114 + 0.886 * ca - 0.203 * sa
    );
    tint = clamp(hueRot * tint, 0.0, 1.0);
```

(This is the standard hue-rotation-matrix-around-the-grey-axis formula, parameterized by angle `drift`.)

- [ ] **Step 2: Build and visually check**

```bash
meson compile -C build
./build/fogwall --fps 30 --color '#a0c8ff' &
```
Expected: with no Spotify running (`uMusic` stays 0), the fog's tint slowly cycles hue over ~90s (`2*pi / 4` in the `t` period scaled by `TAU_OVER_PERIOD`... verify empirically by watching for a couple minutes or lowering the `4.0` constant temporarily to `40.0` for a fast visual check, then reverting). Kill with `kill %1`.

- [ ] **Step 3: Commit**

```bash
git add src/shaders/fog.frag
git commit -m "Add idle hue drift when no music is playing"
```

---

### Task 4: Update documentation

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Document all three features and the new `dbus-1` optional dependency**

Read the current `README.md` in full first (`cat README.md`), then:
- Add bullet points in the feature list (alongside the existing Spotify-reactive bullet) describing: beat-sync path-speed pulses, MPRIS album-art tint sync (what it needs — DBus session bus, Spotify's local art cache — and its fallback), and idle hue drift.
- Add `dbus-1` (and note `libdbus-1`, matching the existing `libpipewire-0.3` phrasing) to the Dependencies section, marked optional like PipeWire.
- Note the vendored `third_party/stb_image.h` and why (art decode, header-only, no extra runtime dependency).
- If the README has a "Usage" flags table, no new flags were added by this plan (album-art sync activates automatically whenever Spotify + DBus are present, matching how audio-reactivity activates automatically) — call that out explicitly so users don't go looking for a missing `--art` flag.

- [ ] **Step 2: Commit**

```bash
git add README.md
git commit -m "Document beat-sync, album-art tint, and idle drift"
```

---

## Self-Review Notes

- **Spec coverage:** Task 1 covers spec §1 (beat-sync), Task 2 covers §2 (album-art tint) including the `HAVE_DBUS` gate and `stb_image.h` vendoring from the spec's "Build changes" section, Task 3 covers §3 (idle drift). Task 4 covers the user's "afterwards update docs (detail everything)" follow-up request, which arrived after the spec was written and isn't in the spec doc itself — added here as its own task.
- **Type consistency:** `audio_beat`, `u_beat`, `mpris_fd`, `art_color`, `art_color_valid`, `tint_color` are each introduced once (Tasks 1-2) and used with the same names in every later step/task.
- **Ordering:** Task 2 (`output_render` reading `tint_color`) depends on Task 1 having already added `u_beat` nearby in the same function — both tasks touch `src/wayland.c:57-73`; execute Task 1 before Task 2 to avoid a merge conflict in that hunk.
