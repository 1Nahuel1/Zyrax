import type { DetectedGame, Profile } from "./api";

export function normalizeExe(exe: string | null | undefined): string {
  if (!exe) return "";
  const trimmed = exe.trim().replace(/^["']+|["']+$/g, "");
  if (!trimmed) return "";
  const base = trimmed.split(/[\\/]/).pop() ?? "";
  return base.trim().toLowerCase();
}

export function profileMatchesGame(
  profile: Pick<Profile, "gameExe">,
  game: Pick<DetectedGame, "exe">
): boolean {
  const bound = normalizeExe(profile.gameExe);
  return bound.length > 0 && bound === normalizeExe(game.exe);
}

export function escapeHtml(value: string): string {
  return value
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#39;");
}

export function escapeAttr(value: string): string {
  return escapeHtml(value);
}
