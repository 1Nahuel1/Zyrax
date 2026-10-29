import "./style.css";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { openUrl } from "@tauri-apps/plugin-opener";
import {
  api,
  type DetectedGame,
  type GpuInfo,
  type LayerStatus,
  type Profile,
  type RuntimeStatus,
} from "./api";

type View = "dashboard" | "upscaling" | "framegen" | "overlay" | "profiles" | "settings";

interface State {
  view: View;
  games: DetectedGame[];
  gpus: GpuInfo[];
  profiles: Profile[];
  activeProfileId: string | null;
  runtime: RuntimeStatus;
  layer: LayerStatus | null;
  selectedPid: number | null;
  scanning: boolean;
  toast: string | null;
}

const state: State = {
  view: "dashboard",
  games: [],
  gpus: [],
  profiles: [],
  activeProfileId: null,
  runtime: {
    state: "idle",
    pid: null,
    gameName: null,
    upscaler: null,
    scaleFactor: 1.5,
    sharpness: 0.5,
    frameGeneration: false,
    overlayEnabled: false,
    injectedAt: null,
    message: "Ready. Select a game to attach Zyrax.",
  },
  layer: null,
  selectedPid: null,
  scanning: false,
  toast: null,
};

const appWindow = getCurrentWindow();

interface NavItem {
  id: View;
  label: string;
  active: string;
  idle: string;
}

const NAV: NavItem[] = [
  {
    id: "dashboard",
    label: "DASHBOARD",
    active: "bg-cyber-neon/10 border-cyber-neon/60 text-cyber-neon shadow-neon-cyan",
    idle: "border-cyber-neon/25 text-cyber-neon/70 hover:bg-cyber-neon/10 hover:border-cyber-neon/50",
  },
  {
    id: "upscaling",
    label: "UPSCALING",
    active: "bg-cyber-cyan/10 border-cyber-cyan/60 text-cyber-cyan shadow-neon-cyan",
    idle: "border-cyber-cyan/25 text-cyber-cyan/70 hover:bg-cyber-cyan/10 hover:border-cyber-cyan/50",
  },
  {
    id: "framegen",
    label: "FRAME GEN",
    active: "bg-cyber-magenta/10 border-cyber-magenta/60 text-cyber-magenta shadow-neon-magenta",
    idle: "border-cyber-magenta/25 text-cyber-magenta/70 hover:bg-cyber-magenta/10 hover:border-cyber-magenta/50",
  },
  {
    id: "overlay",
    label: "OVERLAY",
    active: "bg-cyber-green/10 border-cyber-green/60 text-cyber-green shadow-neon-green",
    idle: "border-cyber-green/25 text-cyber-green/70 hover:bg-cyber-green/10 hover:border-cyber-green/50",
  },
  {
    id: "profiles",
    label: "PROFILES",
    active: "bg-cyber-orange/10 border-cyber-orange/60 text-cyber-orange shadow-neon-orange",
    idle: "border-cyber-orange/25 text-cyber-orange/70 hover:bg-cyber-orange/10 hover:border-cyber-orange/50",
  },
  {
    id: "settings",
    label: "SETTINGS",
    active: "bg-white/5 border-white/40 text-gray-200 shadow-neon-cyan",
    idle: "border-white/15 text-gray-400 hover:bg-white/5 hover:border-white/30",
  },
];

function activeProfile(): Profile | null {
  return state.profiles.find((p) => p.id === state.activeProfileId) ?? state.profiles[0] ?? null;
}

function uid(): string {
  return `p_${Date.now()}_${Math.floor(Math.random() * 1000)}`;
}

function toast(msg: string) {
  state.toast = msg;
  render();
  window.setTimeout(() => {
    if (state.toast === msg) {
      state.toast = null;
      render();
    }
  }, 2600);
}

function hookBadge(): string {
  const map: Record<string, [string, string]> = {
    idle: ["IDLE", "text-gray-400 border-gray-500/40"],
    attaching: ["ATTACHING", "text-cyber-orange border-cyber-orange/50 pulse-glow"],
    active: ["ACTIVE", "text-cyber-green border-cyber-green/60"],
    failed: ["FAILED", "text-cyber-pink border-cyber-pink/60"],
  };
  const [label, cls] = map[state.runtime.state] ?? map.idle;
  return `<span class="px-2 py-0.5 text-[10px] border ${cls}">${label}</span>`;
}

function statusDot(on: boolean): string {
  return `<span class="inline-block w-2 h-2 rounded-full ${on ? "bg-cyber-green shadow-[0_0_8px_#00ff88]" : "bg-gray-600"}"></span>`;
}

function renderDashboard(): string {
  const gpu = state.gpus[0];
  const active = activeProfile();
  const attached = state.runtime.state === "active";

  const gpuCard = gpu
    ? `
      <div class="flex items-start justify-between">
        <div>
          <div class="text-xs text-cyber-neon/70 mb-1">GPU DETECTED</div>
          <div class="text-lg text-white">${gpu.name}</div>
          <div class="text-xs text-gray-400 mt-1">${gpu.vendor} · driver: ${gpu.driver}${
        gpu.vramMb ? ` · ${gpu.vramMb} MB VRAM` : ""
      }</div>
        </div>
        <div class="flex flex-wrap gap-1 justify-end max-w-[45%]">
          ${gpu.apiSupport
            .map(
              (a) =>
                `<span class="px-1.5 py-0.5 text-[10px] border border-cyber-cyan/40 text-cyber-cyan">${a}</span>`
            )
            .join("")}
        </div>
      </div>`
    : `<div class="text-sm text-gray-500">No GPU detected.</div>`;

  return `
    <h1 class="text-xl font-bold text-cyber-neon cyber-glow mb-1">[ SYSTEM STATUS ]</h1>
    <div class="h-px bg-gradient-to-r from-cyber-neon via-cyber-magenta to-transparent mb-6"></div>

    <div class="grid grid-cols-1 xl:grid-cols-2 gap-4 mb-4">
      <div class="bg-cyber-panel/60 border border-cyber-neon/40 p-4 hover:shadow-neon-cyan transition-all">${gpuCard}</div>

      <div class="bg-cyber-panel/60 border ${
        attached ? "border-cyber-green/50" : "border-cyber-pink/40"
      } p-4 transition-all">
        <div class="flex items-center justify-between mb-3">
          <div class="text-xs text-gray-400">RUNTIME</div>
          ${hookBadge()}
        </div>
        <div class="text-lg text-white">${
          state.runtime.gameName ?? "NO GAME ATTACHED"
        }</div>
        <div class="text-xs text-gray-400 mt-1">${state.runtime.message}</div>
        ${
          attached
            ? `<div class="mt-3 text-xs text-cyber-cyan/80">
                UPSCALER: <span class="text-cyber-cyan">${(state.runtime.upscaler ?? "off").toUpperCase()}</span>
                · FRAMEGEN: <span class="${state.runtime.frameGeneration ? "text-cyber-green" : "text-gray-500"}">${
                state.runtime.frameGeneration ? "ON" : "OFF"
              }</span>
                · OVERLAY: <span class="${state.runtime.overlayEnabled ? "text-cyber-green" : "text-gray-500"}">${
                state.runtime.overlayEnabled ? "ON" : "OFF"
              }</span>
              </div>
              <button data-action="detach" class="mt-3 w-full px-3 py-1.5 border border-cyber-pink/50 text-cyber-pink text-xs hover:bg-cyber-pink/10 transition-all">DETACH</button>`
            : ""
        }
      </div>
    </div>

    <div class="bg-cyber-panel/60 border border-cyber-cyan/40 p-4 mb-4">
      <div class="flex items-center justify-between mb-3">
        <div class="text-sm text-cyber-cyan">[ RUNNING GAMES ]</div>
        <button data-action="scan" class="px-3 py-1 border border-cyber-green/50 text-cyber-green hover:bg-cyber-green/10 text-xs transition-all ${
          state.scanning ? "opacity-50 pointer-events-none" : ""
        }">${state.scanning ? "SCANNING..." : "SCAN GAMES"}</button>
      </div>
      ${
        state.games.length === 0
          ? `<div class="text-xs text-gray-500 py-2">${
              state.scanning ? "Scanning processes..." : "No games detected. Launch a game and scan."
            }</div>`
          : `<div class="space-y-1 max-h-56 overflow-auto">
              ${state.games
                .map(
                  (g) => `
                <div class="flex items-center justify-between px-2 py-1.5 border ${
                  state.selectedPid === g.pid
                    ? "border-cyber-neon/60 bg-cyber-neon/10"
                    : "border-white/5 hover:border-cyber-cyan/30"
                } transition-all">
                  <div class="min-w-0">
                    <div class="text-sm text-white truncate">${g.name}</div>
                    <div class="text-[10px] text-gray-500 truncate">pid ${g.pid} · ${
                      g.source
                    }${g.launcher ? ` · ${g.launcher}` : ""}</div>
                  </div>
                  <button data-action="select-game" data-pid="${g.pid}" class="ml-3 px-3 py-1 text-xs border ${
                    state.selectedPid === g.pid
                      ? "border-cyber-neon text-cyber-neon"
                      : "border-cyber-cyan/40 text-cyber-cyan hover:bg-cyber-cyan/10"
                  } transition-all">${state.selectedPid === g.pid ? "SELECTED" : "SELECT"}</button>
                </div>`
                )
                .join("")}
            </div>`
      }
    </div>

    <div class="bg-cyber-panel/60 border border-cyber-orange/40 p-4">
      <div class="text-sm text-cyber-orange mb-3">[ ACTIVE PROFILE ]</div>
      <div class="flex items-center justify-between">
        <div>
          <div class="text-lg text-white">${active ? active.name : "NONE"}</div>
          <div class="text-xs text-gray-400">${
            active
              ? `Upscaler: ${active.upscaler.toUpperCase()} · x${active.scaleFactor} · FrameGen: ${
                  active.frameGeneration ? "ON" : "OFF"
                }`
              : "Create a profile to get started"
          }</div>
        </div>
        <button data-action="apply" class="px-4 py-2 border border-cyber-green/50 text-cyber-green hover:bg-cyber-green/10 text-sm transition-all ${
          state.selectedPid === null ? "opacity-40 pointer-events-none" : ""
        }">${attached ? "RE-APPLY" : "APPLY TO GAME"}</button>
      </div>
    </div>
  `;
}

function renderUpscaling(): string {
  const p = activeProfile();
  const upscalers = [
    {
      id: "off",
      label: "OFF",
      desc: "No spatial upscaling",
      active: "border-white/50 bg-white/5",
      idle: "border-white/10 hover:border-white/30",
      title: "text-gray-300",
    },
    {
      id: "fsr",
      label: "FSR",
      desc: "AMD FidelityFX Super Resolution",
      active: "border-cyber-neon/70 bg-cyber-neon/10 shadow-neon-cyan",
      idle: "border-white/10 hover:border-cyber-neon/40",
      title: "text-cyber-neon",
    },
    {
      id: "dlss",
      label: "DLSS",
      desc: "NVIDIA Deep Learning Super Sampling",
      active: "border-cyber-magenta/70 bg-cyber-magenta/10 shadow-neon-magenta",
      idle: "border-white/10 hover:border-cyber-magenta/40",
      title: "text-cyber-magenta",
    },
  ];
  return `
    <h1 class="text-xl font-bold text-cyber-cyan cyber-glow mb-1">[ UPSCALING ]</h1>
    <div class="h-px bg-gradient-to-r from-cyber-cyan via-cyber-neon to-transparent mb-6"></div>
    ${
      !p
        ? `<div class="text-sm text-gray-500">Create a profile first.</div>`
        : `
      <div class="grid grid-cols-3 gap-3 mb-6">
        ${upscalers
          .map(
            (u) => `
          <button data-action="set-upscaler" data-value="${u.id}" class="text-left p-4 border transition-all ${
            p.upscaler === u.id ? u.active : u.idle
          }">
            <div class="text-lg font-bold ${u.title}">${u.label}</div>
            <div class="text-[11px] text-gray-400 mt-1">${u.desc}</div>
          </button>`
          )
          .join("")}
      </div>

      ${
        p.upscaler !== "fsr"
          ? `<div class="text-[11px] text-cyber-orange border border-cyber-orange/30 bg-cyber-orange/5 px-3 py-2 mb-3">
               Only FSR is implemented by the Linux Vulkan layer. Select FSR to attach.
             </div>`
          : ""
      }

      <div class="bg-cyber-panel/60 border border-cyber-cyan/30 p-5 space-y-6">
        <div>
          <div class="flex justify-between text-sm mb-2">
            <span class="text-cyber-cyan">SCALE FACTOR</span>
            <span class="text-white">x${p.scaleFactor.toFixed(2)}</span>
          </div>
          <input data-action="scale-factor" type="range" min="1" max="4" step="0.25" value="${p.scaleFactor}"
            class="w-full accent-cyber-cyan" />
          <div class="text-[10px] text-gray-500 mt-1">Internal resolution multiplier applied before presentation.</div>
        </div>
        <div>
          <div class="flex justify-between text-sm mb-2">
            <span class="text-cyber-cyan">SHARPNESS</span>
            <span class="text-white">${Math.round(p.sharpness * 100)}%</span>
          </div>
          <input data-action="sharpness" type="range" min="0" max="1" step="0.05" value="${p.sharpness}"
            class="w-full accent-cyber-cyan" />
          <div class="text-[10px] text-gray-500 mt-1">Contrast-adaptive sharpening strength.</div>
        </div>
      </div>
    `
    }
  `;
}

function renderFramegen(): string {
  const p = activeProfile();
  return `
    <h1 class="text-xl font-bold text-cyber-magenta cyber-glow mb-1">[ FRAME GENERATION ]</h1>
    <div class="h-px bg-gradient-to-r from-cyber-magenta via-cyber-pink to-transparent mb-6"></div>
    ${
      !p
        ? `<div class="text-sm text-gray-500">Create a profile first.</div>`
        : `
      <div class="text-[11px] text-cyber-orange border border-cyber-orange/30 bg-cyber-orange/5 px-3 py-2 mb-3">
        The Linux Vulkan layer does not synthesize frames yet; profiles only store this preference.
      </div>
      <div class="bg-cyber-panel/60 border border-cyber-magenta/40 p-5 mb-4">
        <div class="flex items-center justify-between">
          <div>
            <div class="text-sm text-white">ENABLE FRAME GENERATION</div>
            <div class="text-xs text-gray-400 mt-1">Interpolates synthetic frames between rendered frames.</div>
          </div>
          <button data-action="toggle-framegen" class="w-14 h-7 border ${
            p.frameGeneration
              ? "border-cyber-green/60 bg-cyber-green/20"
              : "border-gray-600 bg-transparent"
          } relative transition-all">
            <span class="absolute top-0.5 ${
              p.frameGeneration ? "left-7 bg-cyber-green" : "left-0.5 bg-gray-500"
            } w-6 h-6 transition-all"></span>
          </button>
        </div>
      </div>

      <div class="bg-cyber-panel/60 border border-cyber-pink/30 p-5 ${
        p.frameGeneration ? "" : "opacity-40 pointer-events-none"
      }">
        <div class="text-sm text-cyber-pink mb-3">MULTIPLIER</div>
        <div class="grid grid-cols-3 gap-3">
          ${[2, 3, 4]
            .map(
              (m) => `
            <button data-action="set-multiplier" data-value="${m}" class="py-3 border transition-all ${
              p.frameGenMultiplier === m
                ? "border-cyber-magenta text-cyber-magenta bg-cyber-magenta/10 shadow-neon-magenta"
                : "border-white/10 text-gray-400 hover:border-cyber-magenta/40"
            }">
              <div class="text-xl font-bold">${m}x</div>
              <div class="text-[10px] mt-1">${m === 2 ? "Balanced" : m === 3 ? "Aggressive" : "Extreme"}</div>
            </button>`
            )
            .join("")}
        </div>
        <div class="text-[10px] text-gray-500 mt-3">
          Higher multipliers increase perceived smoothness but add latency and artifacts.
        </div>
      </div>
    `
    }
  `;
}

function renderOverlay(): string {
  const p = activeProfile();
  return `
    <h1 class="text-xl font-bold text-cyber-green cyber-glow mb-1">[ OVERLAY ]</h1>
    <div class="h-px bg-gradient-to-r from-cyber-green via-cyber-cyan to-transparent mb-6"></div>
    ${
      !p
        ? `<div class="text-sm text-gray-500">Create a profile first.</div>`
        : `
      <div class="bg-cyber-panel/60 border border-cyber-green/40 p-5 mb-4">
        <div class="flex items-center justify-between">
          <div>
            <div class="text-sm text-white">IN-GAME OVERLAY</div>
            <div class="text-xs text-gray-400 mt-1">Draws an FPS and scale HUD with the FSR shader.</div>
          </div>
          <button data-action="toggle-overlay" class="w-14 h-7 border ${
            p.overlayEnabled
              ? "border-cyber-green/60 bg-cyber-green/20"
              : "border-gray-600 bg-transparent"
          } relative transition-all">
            <span class="absolute top-0.5 ${
              p.overlayEnabled ? "left-7 bg-cyber-green" : "left-0.5 bg-gray-500"
            } w-6 h-6 transition-all"></span>
          </button>
        </div>
      </div>

      <div class="bg-cyber-panel/60 border border-cyber-cyan/30 p-5">
        <div class="text-sm text-cyber-cyan mb-3">TOGGLE HOTKEY</div>
        <div class="flex items-center gap-3">
          <input data-action="hotkey" value="${p.hotkeyToggle}" maxlength="12"
            class="bg-black/40 border border-cyber-cyan/40 text-cyber-cyan px-3 py-2 text-sm w-40 outline-none focus:border-cyber-cyan" />
          <span class="text-[11px] text-gray-500">Global hotkey registration is not implemented yet; use this toggle.</span>
        </div>
      </div>
    `
    }
  `;
}

function renderProfiles(): string {
  return `
    <h1 class="text-xl font-bold text-cyber-orange cyber-glow mb-1">[ PROFILES ]</h1>
    <div class="h-px bg-gradient-to-r from-cyber-orange via-cyber-pink to-transparent mb-6"></div>

    <div class="flex justify-end mb-3">
      <button data-action="new-profile" class="px-3 py-1.5 border border-cyber-green/50 text-cyber-green text-xs hover:bg-cyber-green/10 transition-all">+ NEW PROFILE</button>
    </div>

    ${
      state.profiles.length === 0
        ? `<div class="text-sm text-gray-500">No profiles yet.</div>`
        : `<div class="space-y-2">
            ${state.profiles
              .map(
                (p) => `
              <div class="flex items-center justify-between p-3 border ${
                p.id === state.activeProfileId
                  ? "border-cyber-neon/60 bg-cyber-neon/5"
                  : "border-white/10"
              }">
                <div class="min-w-0">
                  <div class="flex items-center gap-2">
                    <span class="text-sm text-white">${p.name}</span>
                    ${
                      p.id === state.activeProfileId
                        ? `<span class="text-[9px] px-1.5 py-0.5 border border-cyber-neon/50 text-cyber-neon">ACTIVE</span>`
                        : ""
                    }
                  </div>
                  <div class="text-[10px] text-gray-500 mt-0.5">
                    ${p.upscaler.toUpperCase()} x${p.scaleFactor} · FrameGen ${p.frameGeneration ? "ON" : "OFF"} · Overlay ${
                  p.overlayEnabled ? "ON" : "OFF"
                }
                  </div>
                </div>
                <div class="flex gap-2 shrink-0">
                  <button data-action="activate-profile" data-id="${p.id}" class="px-2 py-1 text-[11px] border border-cyber-cyan/40 text-cyber-cyan hover:bg-cyber-cyan/10">ACTIVATE</button>
                  <button data-action="delete-profile" data-id="${p.id}" class="px-2 py-1 text-[11px] border border-cyber-pink/40 text-cyber-pink hover:bg-cyber-pink/10">DEL</button>
                </div>
              </div>`
              )
              .join("")}
          </div>`
    }
  `;
}

function formatLabel(format: number): string {
  const map: Record<number, string> = {
    44: "B8G8R8A8_UNORM",
    50: "B8G8R8A8_SRGB",
    97: "R16G16B16A16_SFLOAT",
  };
  return map[format] ?? `VkFormat ${format}`;
}

function renderLayerCard(): string {
  const layer = state.layer;
  const shell = (body: string) => `
      <div class="bg-cyber-panel/60 border border-white/10 p-4">
        <div class="text-sm text-cyber-cyan mb-2">VULKAN FSR LAYER</div>
        ${body}
      </div>`;

  if (!layer) return shell('<div class="text-xs text-gray-400">Checking layer status...</div>');

  const badge = layer.active
    ? '<span class="text-cyber-green">ACTIVE</span>'
    : layer.installedForGames
      ? '<span class="text-cyber-orange">READY</span>'
      : layer.installed
        ? '<span class="text-cyber-orange">AVAILABLE</span>'
        : '<span class="text-cyber-pink">NOT FOUND</span>';

  const stats = layer.stats;
  const statsHtml = stats && stats.enabled
    ? `<div class="mt-3 grid grid-cols-2 gap-x-4 gap-y-1 text-xs text-gray-400">
         <div>Frames: <span class="text-cyber-cyan">${stats.presents}</span></div>
         <div>Output: <span class="text-cyber-cyan">${stats.width}x${stats.height}</span></div>
         <div>Render: <span class="text-cyber-cyan">${stats.renderWidth}x${stats.renderHeight}</span></div>
         <div>Mode: <span class="text-cyber-cyan">${stats.upscale ? `FSR x${stats.scale.toFixed(2)}` : "RCAS only"}</span></div>
         <div>Sharpness: <span class="text-cyber-cyan">${stats.sharpness.toFixed(2)}</span></div>
         <div>Format: <span class="text-cyber-cyan">${formatLabel(stats.format)}</span></div>
         <div>PID: <span class="text-cyber-cyan">${stats.pid}</span></div>
         <div>Updated: <span class="text-cyber-cyan">${new Date(stats.timestamp * 1000).toLocaleTimeString()}</span></div>
       </div>`
    : '<div class="mt-3 text-xs text-gray-500">No active attachment. Launch a Vulkan game, then attach it from the dashboard.</div>';

  const paths = layer.sourceDir
    ? `<div class="mt-2 text-[11px] text-gray-500 break-all">${layer.sourceDir}</div>`
    : "";

  const installBtn = layer.installed && !layer.installedForGames
    ? '<button data-action="install-layer" class="px-3 py-1 border border-cyber-magenta/50 text-cyber-magenta text-xs hover:bg-cyber-magenta/10 hover:shadow-neon-magenta transition-all">INSTALL SYSTEM-WIDE</button>'
    : "";

  return shell(`
      <div class="flex items-center justify-between mb-2">
        <div class="text-xs text-gray-400">${layer.message}</div>
        <div class="text-xs font-bold shrink-0 ml-3">${badge}</div>
      </div>
      ${paths}
      ${statsHtml}
      <div class="mt-3 flex flex-wrap gap-2">
        <button data-action="refresh-layer" class="px-3 py-1 border border-cyber-cyan/50 text-cyber-cyan text-xs hover:bg-cyber-cyan/10 hover:shadow-neon-cyan transition-all">REFRESH</button>
        ${installBtn}
      </div>`);
}

function renderSettings(): string {
  return `
    <h1 class="text-xl font-bold text-gray-300 cyber-glow mb-1">[ SETTINGS ]</h1>
    <div class="h-px bg-gradient-to-r from-gray-500 via-cyber-cyan to-transparent mb-6"></div>

    <div class="space-y-3">
      <div class="bg-cyber-panel/60 border border-white/10 p-4">
        <div class="text-sm text-cyber-cyan mb-1">ABOUT ZYRAX</div>
        <div class="text-xs text-gray-400 leading-relaxed">
          Zyrax is an open-source upscaling and frame generation overlay for Windows and Linux,
          inspired by Lossless Scaling. Built with Tauri, Rust and a cyberpunk UI.
        </div>
      </div>
      <div class="bg-cyber-panel/60 border border-white/10 p-4">
        <div class="text-sm text-cyber-cyan mb-2">SYSTEM INFO</div>
        <div class="text-xs text-gray-400 space-y-1">
          <div>Platform: ${navigator.platform || "unknown"}</div>
          <div>GPUs detected: ${state.gpus.length}</div>
          <div>Profiles: ${state.profiles.length}</div>
          <div>Hook state: ${state.runtime.state.toUpperCase()}</div>
        </div>
      </div>
      ${renderLayerCard()}
      <div class="bg-cyber-panel/60 border border-white/10 p-4">
        <div class="text-sm text-cyber-cyan mb-2">LINKS</div>
        <button data-action="open-repo" class="text-xs text-cyber-magenta underline hover:text-cyber-pink">github.com/1Nahuel1/Zyrax</button>
      </div>
    </div>
  `;
}

const VIEWS: Record<View, () => string> = {
  dashboard: renderDashboard,
  upscaling: renderUpscaling,
  framegen: renderFramegen,
  overlay: renderOverlay,
  profiles: renderProfiles,
  settings: renderSettings,
};

function render(): void {
  const app = document.querySelector<HTMLDivElement>("#app");
  if (!app) return;

  const navHtml = NAV.map(
    (n) => `
      <button data-nav="${n.id}" class="w-full text-left px-4 py-2 border text-sm transition-all ${
        state.view === n.id ? n.active : n.idle
      }">[ ${n.label} ]</button>`
  ).join("");

  app.innerHTML = `
  <div class="h-screen flex flex-col scanline">
    <header data-tauri-drag-region class="bg-cyber-panel/80 backdrop-blur border-b border-cyber-neon/30 px-6 py-3 flex items-center justify-between select-none">
      <div class="flex items-center gap-3 pointer-events-none">
        <div class="text-2xl font-bold tracking-widest cyber-glow text-cyber-neon">ZYRAX</div>
        <div class="text-xs text-cyber-magenta/70">v0.1.0</div>
      </div>
      <div class="flex gap-2 text-xs">
        <button data-win="min" class="px-3 py-1 border border-cyber-neon/50 hover:bg-cyber-neon/10 hover:shadow-neon-cyan transition-all">_</button>
        <button data-win="max" class="px-3 py-1 border border-cyber-magenta/50 hover:bg-cyber-magenta/10 hover:shadow-neon-magenta transition-all">□</button>
        <button data-win="close" class="px-3 py-1 border border-cyber-pink/50 hover:bg-cyber-pink/20 transition-all">×</button>
      </div>
    </header>

    <main class="flex-1 flex overflow-hidden">
      <aside class="w-56 bg-cyber-panel/60 border-r border-cyber-cyan/30 flex flex-col shrink-0">
        <nav class="flex-1 p-4 space-y-2">${navHtml}</nav>
        <div class="p-4 border-t border-cyber-cyan/30 text-xs space-y-1">
          <div class="flex items-center gap-2 text-gray-400">${statusDot(state.runtime.state === "active")} HOOK: <span class="text-cyber-cyan">${state.runtime.state.toUpperCase()}</span></div>
          <div class="flex items-center gap-2 text-gray-400">${statusDot(state.gpus.length > 0)} GPU: <span class="text-cyber-cyan">${state.gpus.length > 0 ? "DETECTED" : "NONE"}</span></div>
          <div class="flex items-center gap-2 text-gray-400">${statusDot(state.layer?.installed ?? false)} LAYER: <span class="text-cyber-cyan">${state.layer?.active ? "ACTIVE" : state.layer?.installed ? "READY" : "NONE"}</span></div>
        </div>
      </aside>

      <section class="flex-1 p-6 overflow-auto">${VIEWS[state.view]()}</section>
    </main>

    ${
      state.toast
        ? `<div class="absolute bottom-4 right-4 max-w-sm bg-cyber-panel border border-cyber-green/50 text-cyber-green text-xs px-4 py-2 shadow-[0_0_20px_#00ff8855]">${state.toast}</div>`
        : ""
    }
  </div>
  `;

  bindEvents();
}

function bindEvents(): void {
  document.querySelectorAll<HTMLElement>("[data-nav]").forEach((el) => {
    el.addEventListener("click", () => {
      state.view = el.dataset.nav as View;
      render();
    });
  });

  document.querySelectorAll<HTMLElement>("[data-win]").forEach((el) => {
    el.addEventListener("click", async () => {
      const action = el.dataset.win;
      if (action === "min") await appWindow.minimize();
      else if (action === "max") await appWindow.toggleMaximize();
      else if (action === "close") await appWindow.close();
    });
  });

  document.querySelectorAll<HTMLElement>("[data-action]").forEach((el) => {
    el.addEventListener("click", () => handleAction(el));
    if (el.dataset.action === "scale-factor" || el.dataset.action === "sharpness") {
      el.addEventListener("input", () => handleAction(el));
    }
  });

  const hotkey = document.querySelector<HTMLInputElement>('[data-action="hotkey"]');
  hotkey?.addEventListener("change", () => handleAction(hotkey));
}

let syncTimer: number | null = null;

async function syncRuntimeProfile(): Promise<void> {
  const p = activeProfile();
  if (!p || state.runtime.state !== "active" || state.runtime.pid == null) return;
  try {
    state.runtime = await api.attachToGame({
      pid: state.runtime.pid,
      gameName: state.runtime.gameName ?? "Running game",
      upscaler: p.upscaler,
      scaleFactor: p.scaleFactor,
      sharpness: p.sharpness,
      frameGeneration: p.frameGeneration,
      overlayEnabled: p.overlayEnabled,
    });
    if (state.runtime.state === "failed") toast(state.runtime.message);
    render();
  } catch (e) {
    toast(`Layer update failed: ${e}`);
  }
}

function scheduleRuntimeSync(): void {
  if (state.runtime.state !== "active" || state.runtime.pid == null) return;
  if (syncTimer != null) window.clearTimeout(syncTimer);
  syncTimer = window.setTimeout(() => {
    syncTimer = null;
    void syncRuntimeProfile();
  }, 350);
}

async function persistActive(mutate: (p: Profile) => void) {
  const p = activeProfile();
  if (!p) return;
  mutate(p);
  const data = await api.saveProfile(p);
  state.profiles = data.profiles;
  state.activeProfileId = data.activeProfileId;
  render();
  scheduleRuntimeSync();
}

async function handleAction(el: HTMLElement): Promise<void> {
  const action = el.dataset.action;

  switch (action) {
    case "scan": {
      state.scanning = true;
      render();
      try {
        state.games = await api.scanGames();
        toast(
          state.games.length > 0
            ? `Found ${state.games.length} game process(es)`
            : "No games detected"
        );
      } catch (e) {
        toast(`Scan failed: ${e}`);
      } finally {
        state.scanning = false;
        render();
      }
      break;
    }
    case "select-game": {
      state.selectedPid = Number(el.dataset.pid);
      render();
      break;
    }
    case "detach": {
      state.runtime = await api.detachFromGame();
      toast("Detached from game");
      break;
    }
    case "apply": {
      const p = activeProfile();
      const game = state.games.find((g) => g.pid === state.selectedPid);
      if (!p || !game) {
        toast("Select a running game first");
        return;
      }
      try {
        state.runtime = await api.attachToGame({
          pid: game.pid,
          gameName: game.name,
          upscaler: p.upscaler,
          scaleFactor: p.scaleFactor,
          sharpness: p.sharpness,
          frameGeneration: p.frameGeneration,
          overlayEnabled: p.overlayEnabled,
        });
        if (state.runtime.state === "failed") toast(state.runtime.message);
        else toast(`Applied to ${game.name}`);
      } catch (e) {
        toast(`Attach failed: ${e}`);
      }
      break;
    }
    case "set-upscaler": {
      const value = el.dataset.value!;
      await persistActive((p) => (p.upscaler = value));
      toast(`Upscaler set to ${value.toUpperCase()}`);
      break;
    }
    case "scale-factor": {
      const value = Number((el as HTMLInputElement).value);
      await persistActive((p) => (p.scaleFactor = value));
      break;
    }
    case "sharpness": {
      const value = Number((el as HTMLInputElement).value);
      await persistActive((p) => (p.sharpness = value));
      break;
    }
    case "toggle-framegen": {
      await persistActive((p) => (p.frameGeneration = !p.frameGeneration));
      break;
    }
    case "set-multiplier": {
      const value = Number(el.dataset.value);
      await persistActive((p) => (p.frameGenMultiplier = value));
      break;
    }
    case "toggle-overlay": {
      await persistActive((p) => (p.overlayEnabled = !p.overlayEnabled));
      break;
    }
    case "hotkey": {
      const value = (el as HTMLInputElement).value;
      await persistActive((p) => (p.hotkeyToggle = value));
      toast(`Hotkey set to ${value}`);
      break;
    }
    case "new-profile": {
      const profile: Profile = {
        id: uid(),
        name: `Profile ${state.profiles.length + 1}`,
        gameExe: null,
        upscaler: "fsr",
        scaleFactor: 1.5,
        sharpness: 0.5,
        frameGeneration: false,
        frameGenMultiplier: 2,
        overlayEnabled: true,
        hotkeyToggle: "F11",
      };
      const data = await api.saveProfile(profile);
      state.profiles = data.profiles;
      state.activeProfileId = data.activeProfileId;
      toast("Profile created");
      break;
    }
    case "activate-profile": {
      const data = await api.setActiveProfile(el.dataset.id!);
      state.profiles = data.profiles;
      state.activeProfileId = data.activeProfileId;
      scheduleRuntimeSync();
      toast("Profile activated");
      break;
    }
    case "delete-profile": {
      const data = await api.deleteProfile(el.dataset.id!);
      state.profiles = data.profiles;
      state.activeProfileId = data.activeProfileId;
      render();
      toast("Profile deleted");
      break;
    }
    case "refresh-layer": {
      state.layer = await api.getLayerStatus();
      toast(state.layer.installed ? "Layer status refreshed" : "Layer not found");
      break;
    }
    case "install-layer": {
      try {
        state.layer = await api.installLayer();
        toast("Layer installed system-wide");
      } catch (e) {
        toast(`Install failed: ${e}`);
      }
      break;
    }
    case "open-repo": {
      try {
        await openUrl("https://github.com/1Nahuel1/Zyrax");
      } catch {
        toast("Could not open browser");
      }
      break;
    }
  }
}

async function init(): Promise<void> {
  render();
  try {
    const [data, gpus, runtime, layer] = await Promise.all([
      api.getProfiles(),
      api.detectGpu(),
      api.getRuntimeStatus(),
      api.getLayerStatus(),
    ]);
    state.profiles = data.profiles;
    state.activeProfileId = data.activeProfileId ?? data.profiles[0]?.id ?? null;
    state.gpus = gpus;
    state.runtime = runtime;
    state.layer = layer;
    render();
  } catch (e) {
    toast(`Init error: ${e}`);
  }
}

init();
