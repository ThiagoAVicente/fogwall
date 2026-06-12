# fogwall

Ultra-lightweight procedural fog live wallpaper for Hyprland (and any
wlroots-style Wayland compositor with layer-shell). Plain C99, one binary,
no runtime, no scripting.

- Animated FBM fog (4 octaves, slow circular drift) rendered in a GLSL
  fragment shader via EGL + OpenGL ES 2.0
- Runs on the `background` layer via `wlr-layer-shell-unstable-v1`
- **Pauses completely** (zero CPU, zero GPU submissions, no frame callbacks)
  while any fullscreen or maximized window covers the same output
- Sleeps in `poll()` on the Wayland fd — no timers, no busy loops, no polling
- Frame cap via `--fps` (default 24); drift loops seamlessly every 40 min so
  shader time precision never degrades

## Build

Dependencies (Arch: all in `wayland`, `mesa`, `meson`, `ninja`, `pkgconf`):
`libwayland-client`, `wayland-egl`, `libEGL`, `libGLESv2`, `wayland-scanner`.
Protocol XMLs are vendored in `protocols/` — no wlr-protocols package needed.

```sh
meson setup build --buildtype=release -Db_lto=true -Db_pie=true
ninja -C build
strip build/fogwall
```

## Usage

```sh
fogwall [--color <hex>] [--output <name>] [--fps <n>]
```

| Flag | Meaning | Default |
|------|---------|---------|
| `--color <hex>` | highlight tint, e.g. `#a0c8ff` | `#ffffff` |
| `--output <name>` | render only on this output, e.g. `eDP-1` | all outputs |
| `--fps <n>` | frame cap (1–240) | `24` |

Hyprland autostart:

```
exec-once = fogwall --color "#88aaff"
```

## Measuring the budget

RSS after 30 s of runtime:

```sh
cat /proc/$(pidof fogwall)/status | grep VmRSS
```

`VmRSS` includes shared read-only pages of libwayland/libEGL/libGLESv2 and
the Mesa driver. The anonymous footprint of fogwall itself is what matters:

```sh
grep -A1 anon /proc/$(pidof fogwall)/smaps_rollup
```

Target: `Pss_Anon` < 3 MB. CPU steady-state should read 0.0 % in `top` — the
process makes ~2 wakeups per frame (frame callback + fps deadline) and none
at all while paused.

To verify the fullscreen pause: start a fullscreen window (or a game) on the
same output and watch `top` — fogwall must drop to 0 wakeups; no GL calls and
no frame callbacks are scheduled until the window unfullscreens or closes.

## Pause/resume: protocol note

The spec for this tool originally called for tracking fullscreen state via
`ext-foreign-toplevel-list-v1`. That protocol only carries `title`, `app_id`
and `identifier` events — it has **no `state` and no `output_enter` event**,
so fullscreen/maximized detection is impossible with it (its XML is still
vendored in `protocols/` for reference). The companion ext protocol that
will carry state had not landed in wayland-protocols on this system.

fogwall therefore uses `wlr-foreign-toplevel-management-unstable-v1`
(version ≥ 2 for the `fullscreen` state), which Hyprland implements. The
pause logic lives in `recompute_pause()` in `src/wayland.c`: any
non-minimized toplevel that is fullscreen or maximized on an output pauses
rendering on that output; rendering resumes when it closes, minimizes or
unfullscreens.

## License

MIT — see `LICENSE`.
