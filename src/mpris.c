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

#ifdef HAVE_CURL
#include <curl/curl.h>

/* Real album art is a few hundred KB at most; this is a generous cap
 * against a malicious/misbehaving server trying to exhaust memory. */
#define MPRIS_ART_MAX_BYTES (8 * 1024 * 1024)
#endif

static struct {
    DBusConnection *conn;
    struct fogwall_state *st;
    char last_art_path[1024];
} mpris;

/* file:// URI -> filesystem path, in place. Only handles the plain,
 * unescaped form some MPRIS players use for a local art cache. Most
 * players (including current Spotify) instead give a remote https:// URL,
 * handled separately via libcurl when available (see extract_dominant_color_url). */
static bool uri_to_path(const char *uri, char *out, size_t out_sz)
{
    if (strncmp(uri, "file://", 7) != 0) {
        return false;
    }
    snprintf(out, out_sz, "%s", uri + 7);
    return true;
}

/* Shared by the file:// and https:// decode paths. */
static bool dominant_color_from_pixels(const unsigned char *pix, long n,
        float rgb[3])
{
    if (n == 0) {
        return false;
    }
    double sum[3] = { 0, 0, 0 };
    for (long i = 0; i < n; i++) {
        sum[0] += pix[i * 3 + 0];
        sum[1] += pix[i * 3 + 1];
        sum[2] += pix[i * 3 + 2];
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
    return true;
}

static bool extract_dominant_color_file(const char *path, float rgb[3])
{
    int w, h, channels;
    unsigned char *pix = stbi_load(path, &w, &h, &channels, 3);
    if (pix == NULL) {
        return false;
    }
    bool ok = dominant_color_from_pixels(pix, (long)w * h, rgb);
    stbi_image_free(pix);
    return ok;
}

#ifdef HAVE_CURL

struct fetch_buf {
    unsigned char *data;
    size_t len;
};

static size_t curl_write_cb(void *contents, size_t size, size_t nmemb,
        void *userp)
{
    struct fetch_buf *buf = userp;
    size_t add = size * nmemb;
    if (add > 0 && buf->len + add > MPRIS_ART_MAX_BYTES) {
        return 0; /* abort: response too large */
    }
    unsigned char *grown = realloc(buf->data, buf->len + add);
    if (grown == NULL) {
        return 0;
    }
    memcpy(grown + buf->len, contents, add);
    buf->data = grown;
    buf->len += add;
    return add;
}

/* https:// only, TLS verification on, bounded size and time — this fetches
 * a URL that arrived over D-Bus from whatever MPRIS player is running, so
 * it's treated as untrusted input: no redirect to a non-https scheme, no
 * unbounded read, no indefinite hang. */
static bool fetch_url(const char *url, unsigned char **out, size_t *out_len)
{
    if (strncmp(url, "https://", 8) != 0) {
        return false;
    }
    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        return false;
    }
    struct fetch_buf buf = { NULL, 0 };
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "fogwall/0.1");

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200 || buf.data == NULL) {
        free(buf.data);
        return false;
    }
    *out = buf.data;
    *out_len = buf.len;
    return true;
}

static bool extract_dominant_color_url(const char *url, float rgb[3])
{
    unsigned char *data = NULL;
    size_t len = 0;
    if (!fetch_url(url, &data, &len)) {
        return false;
    }
    int w, h, channels;
    unsigned char *pix = stbi_load_from_memory(data, (int)len, &w, &h,
                                               &channels, 3);
    free(data);
    if (pix == NULL) {
        return false;
    }
    bool ok = dominant_color_from_pixels(pix, (long)w * h, rgb);
    stbi_image_free(pix);
    return ok;
}

#endif /* HAVE_CURL */

static void handle_art_url(const char *url)
{
    if (strcmp(url, mpris.last_art_path) == 0) {
        return;
    }
    float rgb[3];
    bool ok;
    char path[1024];
    if (uri_to_path(url, path, sizeof(path))) {
        ok = extract_dominant_color_file(path, rgb);
#ifdef HAVE_CURL
    } else if (strncmp(url, "https://", 8) == 0) {
        ok = extract_dominant_color_url(url, rgb);
#endif
    } else {
        return; /* unsupported scheme (or no libcurl) — skip, keep old tint */
    }
    if (!ok) {
        return; /* decode/fetch failed */
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

#ifdef HAVE_CURL
    curl_global_init(CURL_GLOBAL_DEFAULT);
#endif

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
        "path='/org/mpris/MediaPlayer2'",
        &err);
    if (dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        dbus_connection_close(mpris.conn);
        dbus_connection_unref(mpris.conn);
        mpris.conn = NULL;
        return;
    }
    if (!dbus_connection_add_filter(mpris.conn, on_message, NULL, NULL)) {
        dbus_connection_close(mpris.conn);
        dbus_connection_unref(mpris.conn);
        mpris.conn = NULL;
        return;
    }

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
#ifdef HAVE_CURL
    curl_global_cleanup();
#endif
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
