#ifndef FOGWALL_MPRIS_H
#define FOGWALL_MPRIS_H

#include <stdbool.h>

/* Optional album-art tint sync via MPRIS over the D-Bus session bus
 * (compiled out without libdbus-1). Watches the active MPRIS player's
 * Metadata property; on track change, decodes the cover art referenced by
 * mpris:artUrl and extracts a dominant color, smoothly lerped into
 * state->cfg.color. Local file:// art (as typically exposed by Spotify and
 * Spotify-compatible clients) is decoded; remote/http art URLs from other
 * MPRIS players are gracefully skipped, leaving the previous tint in place.
 * Falls back to the static --color value with no MPRIS player, no D-Bus, or
 * a decode failure. The D-Bus connection fd joins the main poll(). */

struct fogwall_state;

/* Sets state->mpris_fd (-1 when unavailable). */
void mpris_init(struct fogwall_state *state);

/* Call when state->mpris_fd is readable. */
void mpris_dispatch(struct fogwall_state *state);

void mpris_finish(struct fogwall_state *state);

#endif
