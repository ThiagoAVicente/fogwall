#define _GNU_SOURCE /* strcasestr */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wayland.h"
#include "audio.h"

#ifdef HAVE_PIPEWIRE

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

/* 2400 samples @ 48 kHz = 50 ms buffers: ~20 wakeups/s while Spotify is
 * actually playing, none otherwise. */
#define AUDIO_LATENCY "2400/48000"

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
} audio;

static bool is_spotify_client(uint32_t id)
{
    for (int i = 0; i < audio.n_clients; i++) {
        if (audio.clients[i] == id) {
            return true;
        }
    }
    return false;
}

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
    }
    pw_stream_queue_buffer(audio.stream, b);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_process,
};

static void destroy_stream(void)
{
    if (audio.stream != NULL) {
        pw_stream_destroy(audio.stream);
        audio.stream = NULL;
    }
    audio.spotify_id = 0;
    audio.st->audio_level = 0.0f;
    audio.st->audio_music = 0.0f;
}

/* target must be the node's object.serial (or unique name) — WirePlumber
 * does not match plain node ids, and an unmatched target silently falls
 * back to the default source, i.e. the microphone. */
static void create_stream(const char *target)
{
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_TARGET_OBJECT, target,
        PW_KEY_NODE_LATENCY, AUDIO_LATENCY,
        NULL);
    audio.stream = pw_stream_new(audio.core, "fogwall-level", props);
    if (audio.stream == NULL) {
        return;
    }
    pw_stream_add_listener(audio.stream, &audio.stream_listener,
                           &stream_events, NULL);

    uint8_t buf[256];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(
            .format = SPA_AUDIO_FORMAT_F32,
            .rate = 48000,
            .channels = 1));
    if (pw_stream_connect(audio.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
            PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
            PW_STREAM_FLAG_DONT_RECONNECT, params, 1) < 0) {
        destroy_stream();
        return;
    }
    pw_stream_set_active(audio.stream, audio.active);
}

static void registry_global(void *data, uint32_t id, uint32_t permissions,
        const char *type, uint32_t version, const struct spa_dict *props)
{
    (void)data; (void)permissions; (void)version;
    if (props == NULL) {
        return;
    }
    if (strcmp(type, PW_TYPE_INTERFACE_Client) == 0) {
        const char *app = spa_dict_lookup(props, PW_KEY_APP_NAME);
        const char *bin = spa_dict_lookup(props,
                PW_KEY_APP_PROCESS_BINARY);
        if (((app != NULL && strcasestr(app, "spotify") != NULL) ||
                (bin != NULL && strcasestr(bin, "spotify") != NULL)) &&
                audio.n_clients < (int)(sizeof(audio.clients) /
                                        sizeof(audio.clients[0]))) {
            audio.clients[audio.n_clients++] = id;
        }
        return;
    }
    if (audio.spotify_id != 0 ||
            strcmp(type, PW_TYPE_INTERFACE_Node) != 0) {
        return;
    }
    const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (media_class == NULL ||
            strcmp(media_class, "Stream/Output/Audio") != 0) {
        return;
    }
    /* Spotify's stream node is anonymous ("audio-src"); identity lives on
     * the owning client. Fall back to name matching for other packagings. */
    const char *cid = spa_dict_lookup(props, PW_KEY_CLIENT_ID);
    const char *app = spa_dict_lookup(props, PW_KEY_APP_NAME);
    const char *node = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    bool match = (cid != NULL &&
                  is_spotify_client((uint32_t)strtoul(cid, NULL, 10))) ||
                 (app != NULL && strcasestr(app, "spotify") != NULL) ||
                 (node != NULL && strcasestr(node, "spotify") != NULL);
    if (!match) {
        return;
    }
    const char *serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
    if (serial == NULL) {
        return;
    }
    audio.spotify_id = id;
    create_stream(serial);
}

static void registry_global_remove(void *data, uint32_t id)
{
    (void)data;
    for (int i = 0; i < audio.n_clients; i++) {
        if (audio.clients[i] == id) {
            audio.clients[i] = audio.clients[--audio.n_clients];
            break;
        }
    }
    if (id != 0 && id == audio.spotify_id) {
        destroy_stream();
    }
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void core_error(void *data, uint32_t id, int seq, int res,
        const char *message)
{
    (void)data; (void)seq;
    if (id == PW_ID_CORE) {
        fprintf(stderr, "fogwall: pipewire error: %s (%d)\n", message, res);
        audio.dead = true;
    }
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .error = core_error,
};

void audio_init(struct fogwall_state *st)
{
    st->audio_fd = -1;
    st->audio_level = 0.0f;
    st->audio_music = 0.0f;
    audio.st = st;
    audio.active = true;

    pw_init(NULL, NULL);
    audio.loop = pw_loop_new(NULL);
    if (audio.loop == NULL) {
        return;
    }
    audio.context = pw_context_new(audio.loop, NULL, 0);
    if (audio.context != NULL) {
        audio.core = pw_context_connect(audio.context, NULL, 0);
    }
    if (audio.core == NULL) {
        audio_finish(st);
        return;
    }
    pw_core_add_listener(audio.core, &audio.core_listener, &core_events,
                         NULL);
    audio.registry = pw_core_get_registry(audio.core, PW_VERSION_REGISTRY,
                                          0);
    pw_registry_add_listener(audio.registry, &audio.registry_listener,
                             &registry_events, NULL);
    pw_loop_enter(audio.loop);
    st->audio_fd = pw_loop_get_fd(audio.loop);
}

void audio_dispatch(struct fogwall_state *st)
{
    if (st->audio_fd < 0) {
        return;
    }
    if (audio.dead || pw_loop_iterate(audio.loop, 0) < 0) {
        audio_finish(st);
    }
}

void audio_set_active(struct fogwall_state *st, bool active)
{
    if (st->audio_fd < 0 || audio.active == active) {
        return;
    }
    audio.active = active;
    if (audio.stream != NULL) {
        pw_stream_set_active(audio.stream, active);
    }
    if (!active) {
        st->audio_level = 0.0f;
        st->audio_music = 0.0f;
    }
}

void audio_finish(struct fogwall_state *st)
{
    if (audio.loop == NULL) {
        return;
    }
    destroy_stream();
    if (audio.registry != NULL) {
        pw_proxy_destroy((struct pw_proxy *)audio.registry);
        audio.registry = NULL;
    }
    if (audio.core != NULL) {
        pw_core_disconnect(audio.core);
        audio.core = NULL;
    }
    if (audio.context != NULL) {
        pw_context_destroy(audio.context);
        audio.context = NULL;
    }
    pw_loop_leave(audio.loop);
    pw_loop_destroy(audio.loop);
    audio.loop = NULL;
    pw_deinit();
    st->audio_fd = -1;
    st->audio_level = 0.0f;
    st->audio_music = 0.0f;
}

#else /* !HAVE_PIPEWIRE */

void audio_init(struct fogwall_state *st)
{
    st->audio_fd = -1;
    st->audio_level = 0.0f;
    st->audio_music = 0.0f;
}

void audio_dispatch(struct fogwall_state *st) { (void)st; }

void audio_set_active(struct fogwall_state *st, bool active)
{
    (void)st; (void)active;
}

void audio_finish(struct fogwall_state *st) { (void)st; }

#endif
