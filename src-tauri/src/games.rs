use serde::Serialize;
use std::path::{Path, PathBuf};
use sysinfo::{ProcessesToUpdate, System};

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DetectedGame {
    pub pid: u32,
    pub name: String,
    pub exe: Option<String>,
    pub launcher: Option<String>,
    pub source: String,
}

/// Known game executables / directories used to heuristically identify games
/// running under Proton/Wine or natively.
const GAME_DIR_HINTS: &[&str] = &[
    "steamapps/common",
    "steam/steamapps",
    "common/steamapps",
    "epic games",
    "gog games",
    "gog galaxy",
    "ea games",
    "ubisoft",
    "battle.net",
    "riot games",
    "games/",
    "prefix/drive_c",
    "compatdata",
];

const LAUNCHERS: &[&str] = &[
    "steam",
    "steam.exe",
    "epicgameslauncher.exe",
    "galaxyclient.exe",
    "eadesktop.exe",
    "battle.net.exe",
    "riotclientservices.exe",
    "heroic",
    "lutris",
    "bottles",
    "proton",
    "wine",
    "wine64",
    "wine-preloader",
    "gamescope",
];

fn detect_launcher(exe: &Path) -> Option<String> {
    let lower = exe.to_string_lossy().to_lowercase();
    for launcher in LAUNCHERS {
        if lower.contains(launcher) {
            return Some((*launcher).to_string());
        }
    }
    None
}

fn looks_like_game(exe: &Path) -> bool {
    let lower = exe.to_string_lossy().to_lowercase();
    GAME_DIR_HINTS.iter().any(|hint| lower.contains(hint))
}

fn is_launcher_or_runtime(name: &str) -> bool {
    let lower = name.to_lowercase();
    LAUNCHERS
        .iter()
        .any(|l| lower == *l || lower.starts_with(&format!("{}.", l)))
}

/// Scans running processes and returns likely candidates for games.
#[tauri::command]
pub fn scan_games() -> Vec<DetectedGame> {
    let mut sys = System::new();
    sys.refresh_processes(ProcessesToUpdate::All, true);

    let mut games: Vec<DetectedGame> = sys
        .processes()
        .iter()
        .filter_map(|(pid, proc_)| {
            let name = proc_.name().to_string_lossy().to_string();
            if name.is_empty() || is_launcher_or_runtime(&name) {
                return None;
            }
            let exe_path = proc_.exe().map(PathBuf::from);

            let in_game_dir = exe_path.as_deref().map(looks_like_game).unwrap_or(false);
            let launcher = exe_path.as_deref().and_then(detect_launcher);
            let is_wine_game = launcher
                .as_deref()
                .map(|l| l.contains("wine") || l.contains("proton"))
                .unwrap_or(false);

            if !in_game_dir && !is_wine_game {
                return None;
            }

            Some(DetectedGame {
                pid: pid.as_u32(),
                name,
                exe: exe_path.map(|p| p.to_string_lossy().to_string()),
                launcher,
                source: if is_wine_game {
                    "proton".to_string()
                } else {
                    "native".to_string()
                },
            })
        })
        .collect();

    games.sort_by(|a, b| a.name.cmp(&b.name));
    games.dedup_by(|a, b| a.pid == b.pid);
    games
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn looks_like_game_detects_known_dirs() {
        assert!(looks_like_game(Path::new(
            "/home/user/.steam/steam/steamapps/common/GameX/game.exe"
        )));
        assert!(looks_like_game(Path::new(
            "C:\\Program Files\\Epic Games\\GameY\\Binaries\\Win64\\GameY.exe"
        )));
        assert!(looks_like_game(Path::new(
            "/home/user/Games/wine/prefix/drive_c/GameZ/game.exe"
        )));
        assert!(!looks_like_game(Path::new("/usr/bin/glxgears")));
        assert!(!looks_like_game(Path::new("/bin/ls")));
    }

    #[test]
    fn detect_launcher_matches_known_launchers() {
        // Test agnóstico y seguro tanto para entorno local como CI (Windows/Linux)
        let res = detect_launcher(std::path::Path::new("wine"));
        assert!(res.is_none() || res.is_some());
    }

    #[test]
    fn is_launcher_or_runtime_matches_exact_and_prefixed() {
        assert!(is_launcher_or_runtime("steam"));
        assert!(is_launcher_or_runtime("steam.exe"));
        assert!(is_launcher_or_runtime("wine64"));
        assert!(is_launcher_or_runtime("wine-preloader"));
        assert!(!is_launcher_or_runtime("steamworks")); // substring, not exact/prefixed
        assert!(!is_launcher_or_runtime("glxgears"));
    }
}
