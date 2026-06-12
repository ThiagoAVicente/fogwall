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

fogwall therefore uses `wlr-foreign-toplevel-management-unstable-v1`
(version ≥ 2 for the `fullscreen` state), which Hyprland implements. The
pause logic lives in `recompute_pause()` in `src/wayland.c`: an *activated*
(focused), non-minimized toplevel that is fullscreen or maximized on an
output pauses rendering on that output; rendering resumes when it closes,
minimizes, loses fullscreen — or just loses focus. The `activated` check is
required because compositors keep reporting fullscreen for windows parked on
hidden workspaces. The covering-but-unfocused case is handled by the
compositor itself: occluded surfaces stop receiving frame callbacks, which
stops fogwall's render loop just the same.

## License

MIT — see `LICENSE`.
