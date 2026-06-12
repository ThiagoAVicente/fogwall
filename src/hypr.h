#ifndef FOGWALL_HYPR_H
#define FOGWALL_HYPR_H

/* Optional Hyprland IPC integration.
 *
 * Hyprland implements zwlr_foreign_toplevel_manager_v1 but never sends
 * output_enter, so fullscreen windows cannot be attributed to an output via
 * Wayland — and its renderer keeps sending frame callbacks to background
 * layer surfaces underneath fullscreen windows. Its IPC fills the gap:
 * .socket2.sock pushes events (no polling; it is just one more fd in the
 * main poll()), and on relevant events we query monitors/workspaces for the
 * per-monitor "hasfullscreen" state. */

struct fogwall_state;

/* Returns the event socket fd to poll, or -1 if not running under Hyprland. */
int hypr_init(struct fogwall_state *state);

/* Drain pending event lines; refreshes pause state when relevant. */
void hypr_handle_events(struct fogwall_state *state);

/* Query monitors/workspaces and apply per-output pause. */
void hypr_refresh(struct fogwall_state *state);

void hypr_finish(struct fogwall_state *state);

#endif
