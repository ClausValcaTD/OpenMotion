# OpenMotion architecture

This document describes the engine in `packages/openmotion_core/` as it exists
today. It documents only what is in the source; anything not yet built is marked
"Planned".

## Overview

OpenMotion's engine is a static C++17 library plus one command-line tool. It
reads a JSON project file (`.omproj`), evaluates animated properties, rasterizes
the layers on the CPU, and pipes raw frames to the external `ffmpeg` program to
produce an MP4. Images, video and audio are all accessed by running the external
`ffmpeg` and `ffprobe` executables as child processes; the library does **not**
link against FFmpeg.

The build is CMake. The library target is `openmotion_core` (aliased
`OpenMotion::openmotion_core`), the CLI target is `om-render`, and the tests are
one Catch2 executable, `openmotion_tests`, registered with CTest.

## Module map

### `project` — the `.omproj` model

`include/openmotion/project.hpp` and `src/project.cpp`. Defines the in-memory
model (`Project`, `Layer`, `Transform`, `Property`, `Keyframe`, the `Easing` and
`LayerType` enums) and the JSON reader/writer. `load_from_string`,
`load_from_file`, `to_json_string` and `save_to_file` are the public entry
points. Parsing is defensive: bad input becomes a `ProjectError`, never an
exception that escapes. See [`OMPROJ_FORMAT.md`](OMPROJ_FORMAT.md) for the format
itself.

### `keyframe` — animation evaluation

`include/openmotion/keyframe.hpp` and `src/keyframe.cpp`. `apply_easing` maps a
`t` in `[0, 1]` through one of the five easing curves. `evaluate` returns a
property's value at a time in milliseconds: a constant when there are no
keyframes, the nearest keyframe's value outside the track, and otherwise a
linear interpolation eased by the **start** keyframe of the segment. It uses
`std::upper_bound` (binary search), not a linear scan. `evaluate_transform`
evaluates all six transform properties, and `is_layer_active` tests the
`start_ms <= t < end_ms` window.

### `renderer` — the software rasterizer and exporter

`include/openmotion/renderer.hpp` and `src/renderer.cpp`. `render_frame`
rasterizes one frame into an opaque black RGBA canvas, drawing layers bottom to
top. It supports `solid`, `rect`, `image` and `video` layers; `audio` layers are
skipped (they are mixed during export), and `text` layers are skipped. Image and
video layers share one geometry path: a rotated, scaled quad is rasterized by
testing each pixel centre against the transformed rectangle, with bilinear
sampling in premultiplied alpha for textures. `RenderCache` holds decoded images
and open video decoders so they are created once and reused across frames.

Every drawn pixel is composited with its layer's `blend` mode (see
`internal::blend` below): the effective alpha is `opacity * coverage * texture
alpha`, and the result is `(1 - a) * cb + a * blend_channel(mode, cb, cs)` per
channel. `Normal` keeps the original source-over formula, so a project without a
blend mode renders byte-identically to before the feature existed.

`render_to_mp4` validates the project, collects and validates audio layers,
builds the `ffmpeg` command, then writes each rendered frame to the `ffmpeg`
child's standard input. The video is encoded with libx264 (`-preset veryfast
-crf 18`, `+faststart`) when ffmpeg reports that encoder; otherwise the codec
arguments are omitted and ffmpeg's default encoder is used. `build_video_filter_args`
pins BT.709 limited-range yuv420p and re-asserts the colour tags with a trailing
`setparams` filter, which is what keeps ffprobe reporting them on ffmpeg 7.1.x.

### `internal::audio_mix` — the audio filter graph

`src/internal/audio_mix.hpp` and `src/audio_mix.cpp`. A small, pure module:
`build_audio_filter_graph` turns a list of `AudioInput` values into the
`-filter_complex` string that delays, gains and sums the layers with `adelay`,
`volume` and `amix`. `format_fixed` renders locale-independent fixed-decimal
numbers. There is no I/O here, so it is unit-tested without `ffmpeg`.

### `internal::image_loader` — still images and media probing

`src/internal/image_loader.hpp` and `src/image_loader.cpp`. `load_image` probes
an image's size with `ffprobe`, checks it against the supported limits, then
decodes it to straight-alpha RGBA by running `ffmpeg` with `-frames:v 1` and
reading the raw bytes. `probe_media` is a lighter probe returning native
dimensions and container duration, used by the video path.

### `internal::video_stream` — streaming video decode

`src/internal/video_stream.hpp` and `src/video_stream.cpp`. `VideoStream` owns a
long-lived `ffmpeg` child process that streams raw RGBA frames resampled to the
project frame rate. `frame_at` advances that process frame by frame, using the
pure helper `plan_advance` to decide whether to reuse, read the next frame, skip
a few, or restart the process with a seek. The timing helpers
(`video_frame_index`, `plan_advance`) contain no I/O and are unit-tested
directly.

### `internal::blend` — blend-mode math

`src/internal/blend.hpp`. A header-only, pure module: `blend_channel(mode, cb,
cs)` applies one separable blend function (`Normal`, `Multiply`, `Screen`,
`Overlay`, `Exclusion`, `ColorBurn`, `LinearDodge`) to a pair of channel values
in `[0, 1]`, clamps the result to `[0, 1]`, and returns the backdrop for a NaN
input. It does no I/O and allocates nothing, so it is unit-tested directly
without `ffmpeg` (see `tests/test_blend.cpp`). The renderer maps each 8-bit
canvas channel to `[0, 1]`, calls it, and scales back.

### `internal::shell_quote` — safe command arguments

`src/internal/shell_quote.hpp`. A single header-only `shell_quote` function that
wraps a string in single quotes for `/bin/sh`, escaping embedded quotes. Every
path that goes into an `ffmpeg`/`ffprobe` command is passed through it, so a
filename cannot inject shell commands.

### `media` — standalone metadata probe

`include/openmotion/media.hpp` and `src/media.cpp`. `parse_frame_rate` parses an
ffprobe frame-rate fraction such as `"30000/1001"`, and `open_video` returns a
`VideoMetadata` (duration, fps, width, height) for a file. This module is
self-contained and currently used mainly by its own tests; the renderer's own
probing lives in `image_loader`/`video_stream`.

### `om-render` — the CLI

`tools/om_render.cpp`. Takes `<input.omproj> <output.mp4>`, loads the project,
sets the render cache's base directory to the input file's directory (so
relative `source` paths resolve next to the project), calls `render_to_mp4`, and
prints the frame count or an error.

### `compositor` and `effects` — placeholders

`include/openmotion/compositor.hpp` and `include/openmotion/effects.hpp` declare
empty namespaces (`openmotion::compositor`, `openmotion::effects`) and nothing
else. They exist to reserve the module names. Effects are Planned.

## Data flow

```
.omproj (JSON)
   │  project::load_from_file
   ▼
Project model  ──►  keyframe::evaluate_transform / is_layer_active
   │
   ▼
renderer::render_frame  (software rasterizer, per frame)
   │   ├── solid / rect: colour fill and rotated quad
   │   ├── image: image_loader::load_image  (ffprobe + ffmpeg, cached)
   │   └── video: video_stream::VideoStream (long-lived ffmpeg, cached)
   ▼
raw RGBA frames ──► ffmpeg stdin ──► MP4 (BT.709)
                         ▲
   audio layers ──► audio_mix::build_audio_filter_graph ──► -filter_complex
                    (each audio source probed with ffprobe first)
```

`render_to_mp4` owns the frame loop and the `ffmpeg` process; the renderer never
touches the filesystem for output. Audio is added to the same `ffmpeg` command
as a filter graph, so the exported file carries both the video stream and the
mixed audio stream.

## Conventions

- **No exceptions across module APIs.** Every fallible public function returns a
  `std::variant<T, SomeError>` (`ProjectError`, `MediaError`, `RenderError`) or a
  `std::optional<Error>`. Failures carry a message; the caller decides what to
  do. Internal code may use exceptions only where a standard-library call can
  throw (for example an allocation), and catches them at the boundary.
- **No raw `new`/`delete`.** Ownership is expressed with `std::unique_ptr`,
  `std::shared_ptr` and standard containers.
- **Sanitizer-clean Debug builds.** `-fsanitize=address,undefined` (plus
  `-fno-omit-frame-pointer`) is enabled for Debug, so undefined behaviour such as
  an out-of-range `double`-to-integer conversion is caught in tests. Conversions
  are guarded explicitly (for example `fits_int64` in `project.cpp`).
- **Warnings are errors.** The library, CLI and tests are compiled with
  `-Wall -Wextra -Wpedantic -Werror`.
- **External processes, quoted.** All media access goes through `ffmpeg` and
  `ffprobe` child processes, and every path argument is passed through
  `shell_quote`. `SIGPIPE` is ignored around the export pipe so a broken child
  surfaces as a short write rather than killing the process.
- **Locale independence.** Numbers written into `ffmpeg` commands are formatted
  with integer math (`format_fixed`), not `snprintf("%f")`, so the decimal
  separator does not depend on the process locale.

## Tests

The tests live in `packages/openmotion_core/tests/`, one file per area:
`test_project.cpp`, `test_keyframe.cpp`, `test_media.cpp`, `test_renderer.cpp`,
`test_blend.cpp`, `test_video_layers.cpp` and `test_audio.cpp`. They are one
Catch2 executable discovered by CTest, so each `TEST_CASE` runs as its own CTest
test.

Tests that need the media tools call `command_available("ffmpeg")` /
`command_available("ffprobe")` first and `SKIP(...)` when the tools are absent,
so the suite passes on a machine without `ffmpeg`. The tests that skip are the
ones that generate or decode media: the generated-clip metadata test, the
`render_to_mp4` tests, the image-through-ffmpeg tests, the video-layer playback
tests and the audio-mixing tests. The pure logic — project parsing and
validation, keyframe evaluation, frame-rate parsing, the audio filter graph, and
frame-index/advance planning — is tested without any external tools.

## Known limitations and design decisions

- **Video layers hold their last frame.** When the requested frame index is past
  the end of the clip, `VideoStream` keeps returning the last decoded frame
  rather than erroring or showing black.
- **`-noautorotate`.** Both the image and video paths decode with
  `-noautorotate`, so the raw pixel buffer always matches the probed
  width/height. A file carrying an EXIF rotation tag is therefore *not*
  rotated — orientation support is Planned.
- **30-frame skip window.** `plan_advance` reads forward one frame at a time for
  small jumps (up to 30 frames) and otherwise restarts the decoder with a seek.
  The window trades a little decode work against the cost of seeking.
- **Seek truncation and clamping.** Seek times are truncated, never rounded up,
  so a seek lands at or just before the target frame's presentation time. Seeks
  at or past the container duration are clamped to the last frame that can still
  be produced.
- **Analytic edge coverage.** Quad edges use an analytic coverage estimate (no
  supersampling, no per-pixel allocation), so rotated and scaled edges are
  anti-aliased. Textures are bilinearly filtered.
- **Video layers are silent.** The video decode runs with `-an`; audio comes only
  from `audio` layers.
- **No UI or preview.** The only entry point is the `om-render` CLI; there is no
  real-time playback.
- **`text` layers are parsed but not drawn.** The type exists in the model and
  format, but the renderer skips it.
