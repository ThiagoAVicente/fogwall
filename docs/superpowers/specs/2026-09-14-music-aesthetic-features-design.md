# Music-reactive aesthetic features

Three additions to fogwall's existing Spotify-reactive fog, built on the
current PipeWire loudness capture (`audio.c`) and poll()-driven main loop.

## 1. Beat-sync speed pulse

Onset detection layered on the existing smoothed loudness envelope in
`audio.c`: track a short moving average of `audio_level`, flag an onset when
the instantaneous level exceeds the average by a threshold with a refractory
window (~200ms) to avoid double-triggers. On onset, kick a new
`audio_beat` float to 1.0; it decays exponentially each dispatch (same decay
style as the existing envelopes). No new dependency — reuses the PipeWire
stream already open for loudness.

`audio_beat` is passed to the shader via a new `u_beat` uniform and
multiplies the Lissajous path speed (fog masses visibly lurch on the beat,
decaying back to base speed between hits). Zero cost when no Spotify
playing (`audio_beat` stays 0, same as `audio_level`/`audio_music` today).

## 2. Album-art tint sync

New `src/mpris.c` / `src/mpris.h`, gated behind `HAVE_DBUS` (meson optional
dependency on `dbus-1`, same pattern as `HAVE_PIPEWIRE`).

- Connects to the session bus, watches
  `org.mpris.MediaPlayer2.Spotify`'s `PropertiesChanged` signal for
  `Metadata` (filtered `dbus_bus_add_match`, fed into the main `poll()` via
  `dbus_watch_get_unix_fd`).
- On change, reads `mpris:artUrl` (Spotify caches art locally under
  `~/.cache/spotify/...` or XDG cache, exposed as a `file://` URI — no
  network fetch needed).
- Decodes the art (JPEG/PNG) with vendored `stb_image.h` (single header,
  dropped in `third_party/`, no new build dependency beyond compiling one
  more translation unit).
- Computes a dominant color: downsample to a small grid, average, boost
  saturation slightly (matches the "highlight tint" role `--color` already
  plays — see `wayland.h`'s `color[3]`).
- Smoothly lerps `cfg.color` toward the extracted color over ~2s to avoid
  jarring cuts on track change.
- No DBus / no Spotify / decode failure -> falls back to the static
  `--color` flag value, exactly as today. Zero overhead when unavailable
  (mirrors `audio_fd == -1` pattern).

## 3. Idle drift palette

Pure shader-side, no new state beyond what's already smoothed
(`audio_music`). When `audio_music` has been near 0 for a while (no Spotify
audio, i.e. either not playing or Spotify closed), slowly rotate hue of the
current tint over minutes via a time-based rotation in the fragment shader,
so the wallpaper isn't static during silence. Fades out the drift smoothly
as `audio_music` rises (music resuming snaps back toward the real/art tint).

## Build changes

`meson.build`: add optional `dependency('dbus-1', required: false)` ->
`-DHAVE_DBUS`. Add `src/mpris.c` to sources (compiles to a no-op stub
without `HAVE_DBUS`, same as `audio.c`'s existing `#else` branch). Vendor
`third_party/stb_image.h`.

## Out of scope

- No album-art fetch over network (local cache only) — keeps zero-network
  posture of the project.
- No tempo/BPM estimation beyond simple onset detection — full beat-tracking
  algorithms are overkill for a "pulse" effect.
