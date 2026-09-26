# 🎬 OpenMotion

> **Free, open-source motion graphics editor — the After Effects alternative that actually runs on Linux.**

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux%20%7C%20macOS%20%7C%20Android-green)]()
[![Status](https://img.shields.io/badge/Status-Pre--Alpha-orange)]()
[![Built with Rust](https://img.shields.io/badge/Core-Rust%20🦀-orange)]()
[![Built with Flutter](https://img.shields.io/badge/UI-Flutter-blue)]()

---

## ✨ What is OpenMotion?

OpenMotion is a **cross-platform, GPU-accelerated motion graphics editor** built for creators who refuse proprietary software.

Whether you're on Arch Linux at 3am making anime edits, or on Windows trying to escape Adobe's subscription model — OpenMotion is your tool.

Inspired by Alight Motion, powered by Rust and Flutter, licensed under GPL v3.

---

## 🎯 Why OpenMotion?

| Problem | OpenMotion's Answer |
|---|---|
| After Effects is Windows/Mac only | Runs on Linux, Windows, macOS, Android |
| Alight Motion is mobile-only | Full desktop experience |
| Everything good costs money | Free forever. Source available. Always. |
| Electron apps eat your RAM | Rust core + Flutter UI = lightweight |
| Closed formats lock your work | Open `.omproj` format (JSON-based) |
| No preset sharing on Linux | Built-in preset system + AM XML import |

---

## 🚀 Features (Roadmap)

### ✅ M1 — Skeleton *(In Progress)*
- [ ] Flutter desktop shell (Windows / Linux / macOS)
- [ ] Rust core compiles and links via FFI
- [ ] Import video file and read metadata via FFmpeg
- [ ] Black preview canvas renders in UI

### 🔲 M2 — Basic Editing
- [ ] Timeline with clip arrangement
- [ ] Trim / Split / Move clips
- [ ] Basic text overlay
- [ ] Export via FFmpeg

### 🔲 M3 — Motion Graphics Core
- [ ] Keyframe system with interpolation
- [ ] Transform animations (position, scale, rotation, opacity)
- [ ] Easing curves editor
- [ ] Layer blending modes

### 🔲 M4 — Effects & Presets
- [ ] Built-in effects library
- [ ] JSON preset system
- [ ] **Alight Motion XML preset importer** 👀
- [ ] Lottie animation support

### 🔲 M5 — Community Launch
- [ ] AppImage + Windows installer + macOS DMG
- [ ] Android build on Google Play
- [ ] Preset sharing marketplace
- [ ] Full documentation

---

## 🏗️ Architecture

```
OpenMotion/
├── packages/
│   ├── openmotion_core/     # Rust — rendering, keyframes, effects, media I/O
│   └── openmotion_ui/       # Flutter — timeline, preview, inspector, panels
├── docs/
│   ├── REFERENCES.md        # Learning resources for contributors & Jules
│   └── ARCHITECTURE.md      # Deep dive into system design
├── tools/
│   └── preset_converter/    # Alight Motion XML → OpenMotion JSON
└── LICENSE                  # GPL v3
```

---

## 🛠️ Tech Stack

| Layer | Technology |
|---|---|
| UI | Flutter 3.x (cross-platform) |
| Core Engine | Rust 🦀 |
| GPU Rendering | wgpu (Vulkan / Metal / DX12 / OpenGL) |
| Media Processing | FFmpeg via rust-ffmpeg |
| Rust ↔ Flutter Bridge | flutter_rust_bridge |
| Project Format | JSON (`.omproj`) |
| License | GPL v3 |

---

## 📦 Distribution

| Platform | Price | Notes |
|---|---|---|
| GitHub Releases | **Free** | Executable + Source — always |
| Flathub | Pay what you want | Linux first-class citizen |
| F-Droid | **Free** | FOSS Android store |
| Microsoft Store | $4.99–$9.99 | Convenience pricing |
| Steam | $9.99 | Auto-updates + Steam Wallet |
| Google Play | $1.99–$2.99 | Android mainstream |
| Itch.io | Pay what you want | Indie community |

> The source code is always free. You pay for convenience — never for the software itself.

---

## 🐧 Who is this for?

- **Linux creators** who are tired of booting Windows just for Alight Motion
- **Anime editors** who want AMV tools that work natively on their system
- **FOSS enthusiasts** who have an irrational (but correct) fear of proprietary software
- **Indie creators** in countries where Adobe CC is not a realistic option
- **Developers** who want to contribute to a Rust + Flutter open source project

---

## 🤝 Contributing

OpenMotion is community-driven from day one.

```bash
git clone https://github.com/YOUR_USERNAME/OpenMotion
cd OpenMotion

# Build Rust core
cd packages/openmotion_core
cargo build

# Run Flutter UI
cd ../openmotion_ui
flutter run
```

See [CONTRIBUTING.md](./docs/CONTRIBUTING.md) for full setup guide.

---

## 📄 License

OpenMotion is licensed under the **GNU General Public License v3.0**.

You are free to use, study, modify, and distribute this software.
If you distribute a modified version, you must keep it under GPL v3.

See [LICENSE](./LICENSE) for full terms.

---

## 💙 Support the Project

The software is free. The developer still needs coffee. ☕

- ⭐ Star this repo
- 🐛 Report bugs and request features via Issues
- 💸 [GitHub Sponsors](#) | [Ko-fi](#)
- 📢 Share OpenMotion with your Linux and FOSS communities

---

<div align="center">
  <strong>Made with 🦀 Rust + 💙 Flutter + ☕ Too much coffee</strong><br/>
  <em>Built by the community, for the community.</em>
</div>
