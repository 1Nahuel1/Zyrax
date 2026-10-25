import { invoke } from "@tauri-apps/api/core";

export interface RunningProcess {
  pid: number;
  name: string;
  exe: string | null;
  memoryMb: number;
  cpuUsage: number;
}

export interface DetectedGame {
  pid: number;
  name: string;
  exe: string | null;
  launcher: string | null;
  source: string;
}

export interface GpuInfo {
  name: string;
  vendor: string;
  driver: string;
  vramMb: number;
  apiSupport: string[];
}

export interface Profile {
  id: string;
  name: string;
  gameExe: string | null;
  upscaler: string;
  scaleFactor: number;
  sharpness: number;
  frameGeneration: boolean;
  frameGenMultiplier: number;
  overlayEnabled: boolean;
  hotkeyToggle: string;
}

export interface AppData {
  profiles: Profile[];
  activeProfileId: string | null;
}

export type HookState = "idle" | "attaching" | "active" | "failed";

export interface RuntimeStatus {
  state: HookState;
  pid: number | null;
  gameName: string | null;
  upscaler: string | null;
  frameGeneration: boolean;
  overlayEnabled: boolean;
  injectedAt: number | null;
  message: string;
}

export const api = {
  listProcesses: () => invoke<RunningProcess[]>("list_processes"),
  scanGames: () => invoke<DetectedGame[]>("scan_games"),
  detectGpu: () => invoke<GpuInfo[]>("detect_gpu"),
  getProfiles: () => invoke<AppData>("get_profiles"),
  saveProfile: (profile: Profile) => invoke<AppData>("save_profile", { profile }),
  deleteProfile: (id: string) => invoke<AppData>("delete_profile", { id }),
  setActiveProfile: (id: string | null) =>
    invoke<AppData>("set_active_profile", { id }),
  getRuntimeStatus: () => invoke<RuntimeStatus>("get_runtime_status"),
  attachToGame: (args: {
    pid: number;
    gameName: string;
    upscaler: string;
    frameGeneration: boolean;
    overlayEnabled: boolean;
  }) => invoke<RuntimeStatus>("attach_to_game", args),
  detachFromGame: () => invoke<RuntimeStatus>("detach_from_game"),
  setOverlayEnabled: (enabled: boolean) =>
    invoke<RuntimeStatus>("set_overlay_enabled", { enabled }),
};
