# Zyrax

> Cyberpunk **upscaling + frame generation** overlay for Windows and Linux, inspired by Lossless Scaling.

![status](https://img.shields.io/badge/status-alpha-orange)
![license](https://img.shields.io/badge/license-MIT-00fff9)
![platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-ff00ff)

Zyrax is an open-source desktop app that detects your running games, lets you
configure spatial upscaling (AMD **FSR**) and AI upscaling (**DLSS**), enable
**frame generation**, and shows a neon **in-game overlay** — all wrapped in a
cyberpunk UI built with Tauri + Rust and a Vite/TypeScript frontend.

---

## Features

- **Game detection** — scans running processes and identifies native and
  Proton/Wine titles by executable path heuristics.
- **GPU detection** — enumerates GPUs, driver and supported graphics APIs
  (Vulkan, OpenGL, DirectX 11/12, FSR/DLSS).
- **Upscaling profiles** — choose FSR / DLSS / Off, scale factor and sharpness.
- **Frame generation** — toggle synthetic frame interpolation and pick a
  multiplier (2x / 3x / 4x).
- **Cyberpunk overlay** — configurable overlay with a toggle hotkey.
- **Profile manager** — create, activate and delete per-game configuration
  profiles (persisted to disk).
- **Cross-platform shell** — Windows and Linux via Tauri 2.

## Tech stack

| Layer     | Technology                                   |
| --------- | -------------------------------------------- |
| Shell     | [Tauri 2](https://tauri.app/) (Rust)         |
| Backend   | Rust (`sysinfo`, `serde`)                    |
| Frontend  | [Vite](https://vitejs.dev/) + TypeScript     |
| Styling   | [Tailwind CSS 4](https://tailwindcss.com/)   |

## Project status

This is an **alpha**. The application shell, game/GPU detection, profile
management, persistence and the full cyberpunk UI are implemented and working.

The **graphics interception layer** that actually injects FSR/DLSS and
frame generation into a game's render pipeline is the next milestone. The
`attach_to_game` command already validates the target process and manages the
hook lifecycle (`idle → attaching → active`); the rendering hooks plug into
that lifecycle. See the roadmap below.

## Getting started

```bash
# install dependencies
bun install

# run in development (opens the desktop window)
bun run tauri dev

# production build
bun run tauri build
```

> Requires Rust, Bun and the [Tauri system dependencies](https://tauri.app/start/prerequisites/).

## Architecture

```
src/                     # Vite/TypeScript frontend
  api.ts                 # typed wrappers around Tauri commands
  main.ts                # app shell, routing and views
  style.css              # Tailwind + cyberpunk theme
src-tauri/src/
  main.rs                # binary entry
  lib.rs                 # Tauri builder, command registration
  processes.rs           # process enumeration (sysinfo)
  games.rs               # game detection heuristics
  gpu.rs                 # GPU / API detection (Linux + Windows)
  profiles.rs            # profile persistence (JSON in app config dir)
  runtime.rs             # hook lifecycle & runtime status
```

### How upscaling is applied (design)

1. **Detect** the target game process and its graphics API.
2. **Attach** an interception layer to the swapchain / present path:
   - Windows: DXGI / Vulkan layer hooks.
   - Linux: a Vulkan implicit layer (works with Proton/DXVK) or gamescope.
3. **Upscale** the backbuffer with FSR (or DLSS where supported).
4. **Generate** interpolated frames when enabled.
5. **Overlay** a render pass on top with stats and toggles.

## Roadmap

- [x] Cyberpunk UI shell and navigation
- [x] Game detection (native + Proton/Wine)
- [x] GPU & graphics API detection
- [x] Profile management with persistence
- [x] Runtime hook lifecycle & status
- [ ] Vulkan implicit layer (Linux) with FSR
- [ ] DXGI swapchain hook (Windows) with FSR
- [ ] DLSS integration
- [ ] Frame generation pipeline
- [ ] In-game overlay renderer
- [ ] Global hotkey registration

## Contributing

Issues and PRs are welcome. This project is early — the graphics hook layer is
the area that needs the most help.

## License

MIT © 1Nahuel1
