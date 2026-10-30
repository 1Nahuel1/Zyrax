import { describe, it, expect } from "bun:test";
import { escapeHtml, escapeAttr, normalizeExe, profileMatchesGame } from "./lib";

describe("escapeHtml", () => {
  it("escapes ampersand", () => {
    expect(escapeHtml("a & b")).toBe("a &amp; b");
  });

  it("escapes less-than", () => {
    expect(escapeHtml("<script>")).toBe("&lt;script&gt;");
  });

  it("escapes greater-than", () => {
    expect(escapeHtml("a > b")).toBe("a &gt; b");
  });

  it("escapes double quotes", () => {
    expect(escapeHtml('he said "hi"')).toBe('he said &quot;hi&quot;');
  });

  it("escapes single quotes", () => {
    expect(escapeHtml("it's")).toBe("it&#39;s");
  });

  it("escapes all characters combined", () => {
    expect(escapeHtml('<img src=x onerror=alert(1)>')).toBe(
      "&lt;img src=x onerror=alert(1)&gt;"
    );
  });

  it("escapes attribute injection attempt", () => {
    expect(escapeHtml('" onmouseover="alert(1)')).toBe(
      "&quot; onmouseover=&quot;alert(1)"
    );
  });

  it("leaves safe text unchanged", () => {
    expect(escapeHtml("Hello World 123")).toBe("Hello World 123");
  });

  it("handles empty string", () => {
    expect(escapeHtml("")).toBe("");
  });
});

describe("escapeAttr", () => {
  it("escapes same as escapeHtml", () => {
    const value = 'test"&<>';
    expect(escapeAttr(value)).toBe(escapeHtml(value));
  });

  it("escapes double quotes for attribute context", () => {
    expect(escapeAttr('value="x"')).toBe('value=&quot;x&quot;');
  });

  it("handles empty string", () => {
    expect(escapeAttr("")).toBe("");
  });
});

describe("normalizeExe", () => {
  it("normalizes a full path to basename lowercase", () => {
    expect(normalizeExe("C:\\Games\\MyGame.exe")).toBe("mygame.exe");
    expect(normalizeExe("/usr/games/my game.x86")).toBe("my game.x86");
  });

  it("strips surrounding quotes", () => {
    expect(normalizeExe('"MyGame.exe"')).toBe("mygame.exe");
    expect(normalizeExe("'MyGame.exe'")).toBe("mygame.exe");
  });

  it("returns empty for null/undefined/empty", () => {
    expect(normalizeExe(null)).toBe("");
    expect(normalizeExe(undefined)).toBe("");
    expect(normalizeExe("")).toBe("");
    expect(normalizeExe("   ")).toBe("");
  });
});

describe("profileMatchesGame", () => {
  it("returns true when gameExe matches game exe", () => {
    const profile = { gameExe: "game.exe" } as Parameters<
      typeof profileMatchesGame
    >[0];
    const game = { exe: "/path/to/GAME.EXE" } as Parameters<
      typeof profileMatchesGame
    >[1];
    expect(profileMatchesGame(profile, game)).toBe(true);
  });

  it("returns false when gameExe is empty", () => {
    const profile = { gameExe: null } as Parameters<
      typeof profileMatchesGame
    >[0];
    const game = { exe: "/path/to/GAME.EXE" } as Parameters<
      typeof profileMatchesGame
    >[1];
    expect(profileMatchesGame(profile, game)).toBe(false);
  });

  it("returns false when exe does not match", () => {
    const profile = { gameExe: "other.exe" } as Parameters<
      typeof profileMatchesGame
    >[0];
    const game = { exe: "/path/to/game.exe" } as Parameters<
      typeof profileMatchesGame
    >[1];
    expect(profileMatchesGame(profile, game)).toBe(false);
  });
});
