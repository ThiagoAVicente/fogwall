#ifndef FOGWALL_AUDIO_H
#define FOGWALL_AUDIO_H

#include <stdbool.h>

/* Optional Spotify loudness capture via PipeWire (compiled out without
 * libpipewire-0.3). A capture stream attaches to Spotify's output node
 * whenever one exists and feeds a smoothed 0..1 envelope into
 * state->audio_level; the fog shader pulses with it. No Spotify node, no
 * stream — zero overhead. The PipeWire loop fd joins the main poll(). */

struct fogwall_state;

/* Sets state->audio_fd (-1 when unavailable). */
void audio_init(struct fogwall_state *state);

/* Call when state->audio_fd is readable. */
void audio_dispatch(struct fogwall_state *state);

/* Pause/resume capture (e.g. while every output is paused). */
void audio_set_active(struct fogwall_state *state, bool active);

void audio_finish(struct fogwall_state *state);

#endif
