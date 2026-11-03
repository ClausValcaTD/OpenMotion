# 🎬 OpenMotion

> **Free, open-source motion graphics editor — built to run anywhere, starting with the engine.**

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Status](https://img.shields.io/badge/Status-Pre--Alpha-orange)]()
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)]()
[![Build](https://img.shields.io/badge/Build-CMake-informational)]()

---

## ✨ What is OpenMotion?

OpenMotion is a cross-platform motion graphics editor in progress, in the spirit
of Alight Motion. It is GPL v3 and built around an **open, documented project
format** (`.omproj`, JSON-based) so your work is never locked inside one app.

**Status: Pre-Alpha.** Only the engine exists so far. There is no UI yet — the
current way to use it is the `om-render` command-line tool, which turns a
`.omproj` file into an `.mp4`. Everything below describes what the code in
`packages/openmotion_core/` does **today**, and nothing more.

---

## 🎯 Why OpenMotion?

| Problem | OpenMotion's answer |
|---|---|
| Good motion tools are often closed and platform-locked | Open source, and the core engine is plain C++17 + CMake |
| Closed formats lock your work away | Open, documented `.omproj` format (JSON) |
| You can't see how it works | The full source is here, under GPL v3 |

The software is free. Selling convenience builds — installers, store listings,
support — is fine. Hiding how it works is not.

---

## ✅ What works today

The engine (`packages/openmotion_core/`) currently does:

- **Project format** — load, validate and save `.omproj` (JSON, format version 1),
  with `ProjectError` reporting instead of exceptions.
- **Keyframe evaluation** — constant and animated properties with the easing
  curves `linear`, `ease_in`, `ease_out`, `ease_in_out` and `hold`, looked up by
  binary search.
- **Layers** — `solid`, `rect`, `image` and `video` layers are rendered by a
  software rasterizer; `audio` layers are mixed into the exported audio.
- **Images** — decoded through the external `ffmpeg`/`ffprobe` executables, with
  bilinear filtering in premultiplied alpha and a size cap.
- **Video** — decoded by streaming raw frames from an `ffmpeg` child process,
  resampled to the project frame rate; the last frame is held past the clip end.
- **Audio** — `audio` layers are delayed, gained and mixed with an ffmpeg filter
  graph, then encoded as AAC (192 kbit/s).
- **Output** — an MP4 encoded by ffmpeg (the codec is ffmpeg's default for the mp4 muxer), tagged BT.709.
- **`om-render` CLI** — renders a project to an MP4.

---

## 🔲 Not yet

These are **not implemented** — listed so the roadmap is honest:

- A graphical UI (timeline, preview, inspector).
- Text rendering, and Arabic shaping in particular. `text` layers are parsed but
  not drawn.
- Effects (the `effects` module is an empty placeholder).
- Anti-aliasing — the rasterizer uses hard pixel-centre coverage.
- Rotation-tag (EXIF orientation) support — images are decoded with
  `-noautorotate`, so a rotation tag is ignored rather than applied.
- An Alight Motion XML importer (see [`docs/AM_XML_NOTES.md`](docs/AM_XML_NOTES.md)).
- Audio from video layers — video layers stay muted.
- Real-time preview / playback.

---

## 🛠️ Tech stack

| Layer | Technology |
|---|---|
| Language | C++17 |
| Build | CMake |
| JSON | [nlohmann/json](https://github.com/nlohmann/json) (fetched by CMake) |
| Tests | [Catch2 v3](https://github.com/catchorg/Catch2) (fetched by CMake) |
| Media | External `ffmpeg` and `ffprobe` executables |
| License | GPL v3 |

There is **no** Rust, Flutter, wgpu or FFmpeg library linkage. FFmpeg is used
only by running the `ffmpeg` and `ffprobe` programs as child processes.

---

## 📦 Build and run

Requirements:

- A C++17 compiler and CMake (3.16 or newer).
- `ffmpeg` and `ffprobe` on your `PATH` — needed to load images/video and to
  export. Builds and the offline tests work without them; tests that need them
  are skipped.
- Network access on the first configure, to fetch nlohmann/json and Catch2.

```bash
cd packages/openmotion_core

# Configure (Debug enables AddressSanitizer + UndefinedBehaviorSanitizer)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug

# Build the engine, the om-render CLI and the tests
cmake --build build

# Run the tests
ctest --test-dir build
```

Render the bundled example:

```bash
./build/om-render examples/hello.omproj hello.mp4
```

`om-render` takes exactly two arguments, `<input.omproj> <output.mp4>`, and
resolves relative `source` paths against the directory of the input file.

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the module map and
[`docs/OMPROJ_FORMAT.md`](docs/OMPROJ_FORMAT.md) for the project format.

---

## 🤝 Contributing

OpenMotion is community-driven from day one.

```bash
git clone https://github.com/YOUR_USERNAME/OpenMotion
cd OpenMotion/packages/openmotion_core
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build
```

The Debug build is sanitizer-clean by design (`-fsanitize=address,undefined`),
and the code is compiled with `-Wall -Wextra -Wpedantic -Werror`, so a warning
fails the build. Please keep it that way.

---

## 📄 License

OpenMotion is licensed under the **GNU General Public License v3.0**.

You are free to use, study, modify, and distribute this software. If you
distribute a modified version, you must keep it under GPL v3.

See [LICENSE](./LICENSE) for the full terms.

---

## 💙 Support the project

The software is free. The developer still needs coffee. ☕

- ⭐ Star this repo
- 🐛 Report bugs and request features via Issues
- 📢 Share OpenMotion with your Linux and FOSS communities

---

## 🗺️ Long-term ideas (not started)

Nothing here is implemented or scheduled. It is a direction, not a promise.

**Possible distribution channels** (all still just ideas):

| Platform | Price idea | Notes |
|---|---|---|
| GitHub Releases | Free | Executable + source — always |
| Flathub | Pay what you want | Linux first-class citizen |
| F-Droid | Free | FOSS Android store |
| Microsoft Store | Convenience pricing | Paid convenience build |
| Steam | Convenience pricing | Auto-updates |
| Google Play | Convenience pricing | Android mainstream |
| Itch.io | Pay what you want | Indie community |

> The source code is always free. You would pay for convenience — never for the
> software itself.

**Rough milestone sketch** (no dates, nothing started):

- **M1 — Engine skeleton.** Software renderer, project format, keyframe
  evaluation, `om-render` CLI. *(Largely what exists today.)*
- **M2 — Editing.** Timeline, trim/split/move, basic text overlay.
- **M3 — Motion graphics core.** Easing-curve editor, layer blending modes.
- **M4 — Effects and presets.** Built-in effects library, JSON presets, an
  Alight Motion XML preset importer.
- **M5 — Community launch.** Packaging for desktop platforms, documentation.

<div align="center">
  <strong>Built by the community, for the community.</strong>
</div>
