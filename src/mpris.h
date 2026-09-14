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
