#ifndef FOGWALL_MPRIS_H
#define FOGWALL_MPRIS_H

#include <stdbool.h>

/* Optional album-art tint sync via MPRIS over the D-Bus session bus
 * (compiled out without libdbus-1). Watches the active MPRIS player's
 * Metadata property; on track change, decodes the cover art referenced by
 * mpris:artUrl and extracts a dominant color, smoothly lerped into
 * state->cfg.color. Local file:// art is read directly; remote https://
 * art (what Spotify itself serves today) is fetched via libcurl when
 * available (compiled out without libcurl — falls back to the static
 * --color value, same as no MPRIS at all). The fetch is TLS-verified,
 * https-only (including on redirect), time- and size-bounded, since the
 * URL comes from whatever MPRIS player happens to be running — untrusted
 * input. Falls back to --color with no MPRIS player, no D-Bus, no libcurl,
 * or a decode/fetch failure. The D-Bus connection fd joins the main
 * poll() — the art fetch itself is a bounded blocking call on track
 * change, not integrated into poll(); it does not run per-frame. */

struct fogwall_state;

/* Sets state->mpris_fd (-1 when unavailable). */
void mpris_init(struct fogwall_state *state);

/* Call when state->mpris_fd is readable. */
void mpris_dispatch(struct fogwall_state *state);

void mpris_finish(struct fogwall_state *state);

#endif
