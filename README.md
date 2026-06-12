# fogwall

Ultra-lightweight procedural fog live wallpaper for Hyprland (and any
wlroots-style Wayland compositor with layer-shell). Plain C99, one binary,
no runtime, no scripting.

- Black base with 4 big fog masses in the tint color, wandering on
  Lissajous paths (constantly changing direction), textured by 4-octave FBM
  noise — GLSL fragment shader via EGL + OpenGL ES 2.0; the whole animation
  loops seamlessly every 8 min
- Runs on the `background` layer via `wlr-layer-shell-unstable-v1`
- **Pauses completely** (zero CPU, zero GPU submissions, no frame callbacks)
  while any fullscreen or maximized window covers the same output
- Sleeps in `poll()` on the Wayland fd — no timers, no busy loops, no polling
- Frame cap via `--fps` (default 24)
- **Spotify-reactive** (optional, PipeWire): fog pulses with loudness and the
  tint sways in tone/strength — never hue — while Spotify plays; returns to
  the exact `--color` when it stops. The capture stream targets Spotify's
  node by `object.serial` (a plain id target silently falls back to the
  microphone), uses 50 ms buffers (~20 wakeups/s only while audio flows),
  joins the same `poll()`, and deactivates whenever every output is paused.
  Without libpipewire or without Spotify, the feature costs nothing.

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

Stop any other wallpaper daemon (awww, hyprpaper, swww, …) first — whichever
background-layer surface is created last stacks on top, so a running daemon
can cover fogwall entirely (fogwall then receives no frame callbacks and
idles, but you won't see the fog).

On hybrid Intel/NVIDIA laptops, force the Mesa EGL vendor so the NVIDIA
userspace (~80 MB of mappings, ~14 MB extra Pss_Anon) never loads:

```
exec-once = env __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json fogwall --color "#88aaff"
```

## Measured (Hyprland, 2×1920 outputs, 24 fps, Intel iGPU via Mesa)

```sh
grep VmRSS /proc/$(pidof fogwall)/status
grep -E 'Pss_Anon|Shared_Clean' /proc/$(pidof fogwall)/smaps_rollup
```

| Metric | Active (both outputs) | Fullscreen window focused |
|--------|----------------------|---------------------------|
| CPU (ticks/10 s) | 13 (≈1.3 % of one core) | **0** |
| Wakeups/s | ~147 (2–3 per frame per output) | **~1** |
| `Pss_Anon` | 8.2 MB | same |
| `VmRSS` | 136 MB | same |

`VmRSS` is dominated by 128 MB of `Shared_Clean` read-only pages of
libLLVM + libgallium — Mesa's shader compiler, already resident for the
compositor itself, so fogwall's marginal cost is the 8.2 MB of `Pss_Anon`
(GL driver heap, command buffers). The original <5 MB VmRSS target is not
achievable with any GL driver stack; `Pss_Anon` is the honest number. If the
NVIDIA EGL vendor also loads (hybrid laptops, see above), `Pss_Anon` grows
to ~22 MB.

While paused, no GL calls are made and no frame callbacks are scheduled —
CPU and GPU cost is literally zero until the fullscreen window goes away.

## Pause/resume: protocol note

The spec for this tool originally called for tracking fullscreen state via
`ext-foreign-toplevel-list-v1`. That protocol only carries `title`, `app_id`
and `identifier` events — it has **no `state` and no `output_enter` event**,
so fullscreen/maximized detection is impossible with it (its XML is still
vendored in `protocols/` for reference). The companion ext protocol that
will carry state had not landed in wayland-protocols on this system.

Worse: Hyprland implements `wlr-foreign-toplevel-management-unstable-v1`
but never sends `output_enter` (verified with `WAYLAND_DEBUG=1`), so a
fullscreen toplevel cannot be attributed to a monitor — and Hyprland keeps
sending frame callbacks to background-layer surfaces under fullscreen
windows, so occlusion does not pause anything either.

fogwall therefore uses two mechanisms (`src/wayland.c`, `src/hypr.c`):

1. **Hyprland IPC** (preferred, automatic when `$HYPRLAND_INSTANCE_SIGNATURE`
   is set): `.socket2.sock` is a second fd in the main `poll()` — no
   polling. On fullscreen/workspace/window events it queries `j/monitors` +
   `j/workspaces` and pauses every output whose active workspace
   `hasfullscreen`. Exact per-monitor attribution.
2. **wlr-foreign-toplevel state** (fallback for other wlroots compositors):
   an *activated*, non-minimized, fullscreen-or-maximized toplevel pauses
   the outputs it `output_enter`-ed. The `activated` check avoids freezing
   for fullscreen windows parked on hidden workspaces.

While paused, fogwall renders nothing and requests no frame callbacks; its
fd has no traffic, so it sits in `poll()` indefinitely. Measured: ~150
wakeups/s active (2 outputs) → ~1 wakeup/s, 0 CPU ticks paused.

## License

MIT — see `LICENSE`.
