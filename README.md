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

The **graphics interception layer** that injects FSR and frame generation into
a game's render pipeline is under active development. The `attach_to_game`
command already validates the target process and manages the hook lifecycle
(`idle → attaching → active`); the rendering hooks plug into that lifecycle.

On Linux there is a working **Vulkan implicit layer** (see below) that performs
a real, resolution-changing **FSR 1.0** upscale: the game is told to render at
`1 / scale` of the surface, then the layer runs **EASU** (edge-adaptive spatial
upscaling) followed by **RCAS** (robust contrast adaptive sharpening) up to the
full output size. The app detects the layer and reads live frame stats from it
in **Settings**.

## Vulkan layer (Linux, alpha)

Requirements: `g++`, `glslangValidator`, `spirv-val`, `xxd` and Vulkan headers.

```bash
cd vk-layer
./build.sh          # compiles shaders + libzyrax_fsr.so into vk-layer/build
./install.sh        # copies it to ~/.local/share/vulkan/implicit_layer.d
```

Run any Vulkan application with the layer enabled:

```bash
ZYRAX_FSR_ENABLE=1 ZYRAX_FSR_DEBUG=1 vkcube
```

For Steam games, set the launch options to `ZYRAX_FSR_ENABLE=1 %command%`.

| Variable | Meaning |
| --- | --- |
| `ZYRAX_FSR_ENABLE=1` | Enable the layer (implicit layers are opt-in here). |
| `ZYRAX_FSR_DISABLE=1` | Force-disable it. |
| `ZYRAX_FSR_SCALE` | FSR scale factor `1.0`–`4.0` (default `1.5`). `1.0` = sharpening only. |
| `ZYRAX_FSR_SHARPNESS` | RCAS sharpening strength, `0.0`–`1.0` (default `0.5`). |
| `ZYRAX_FSR_STATS` | JSON stats path (default `/tmp/zyrax_fsr_stats.json`). |
| `ZYRAX_FSR_DEBUG=1` | Print layer diagnostics to stderr. |

How it works:

1. The layer reports a reduced surface size through
   `vkGetPhysicalDeviceSurfaceCapabilitiesKHR`, so the application renders at
   `output / scale`.
2. `vkCreateSwapchainKHR` is intercepted to add transfer usage and to create
   the real swapchain at the full output size.
3. At present time the layer copies the low-res region into a scratch image,
   runs **EASU** into an intermediate full-size image and **RCAS** into the
   swapchain image. `sc.upscale` / `sc.renderExtent` keep both sizes separate.
4. On Wayland, `currentExtent` is `UINT32_MAX`; the layer learns the window size
   from the first swapchain and requests a single
   `VK_ERROR_OUT_OF_DATE_KHR` recreate so the app switches to the reduced
   extent.
5. If the swapchain cannot be sampled as a transfer source, lacks the required
   format features, or resource creation fails, the layer disables FSR for that
   surface, presents in passthrough and requests one recreate at the native
   size.

Limitations: no frame generation; only Vulkan swapchains presented on the
hooked queue; swapchain alpha is not preserved (output is opaque); on Wayland,
resizing the window after startup may keep the original internal output
resolution until the app recreates with a new size.
Validated with `vkcube` on RADV under Wayland and X11, with the broken
`VK_LAYER_LSFGVK_frame_generation` implicit layer and other overlays disabled.
DXVK games should work but have not been exhaustively tested.

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
  layer.rs               # Vulkan layer discovery, install & live stats
vk-layer/                # Linux Vulkan implicit layer (C++)
  src/layer.cpp          # loader interface, swapchain/present hooks, FSR EASU + RCAS passes
  shaders/               # full-screen triangle + FSR EASU/RCAS shaders
  build.sh               # shader compile + shared library build
  install.sh             # per-user install into ~/.local/share/vulkan
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
- [x] Vulkan implicit layer (Linux) with EASU + RCAS resolution-changing FSR upscale
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
