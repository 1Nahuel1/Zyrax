use std::fs;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};
use tauri::Manager;

const MANIFEST_NAME: &str = "VkLayer_zyrax_fsr.json";
const LIBRARY_NAME: &str = "libzyrax_fsr.so";

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct LayerStats {
    pub enabled: bool,
    #[serde(default)]
    pub presents: u64,
    #[serde(default)]
    pub sharpness: f32,
    #[serde(default)]
    pub width: u32,
    #[serde(default)]
    pub height: u32,
    #[serde(default)]
    pub render_width: u32,
    #[serde(default)]
    pub render_height: u32,
    #[serde(default)]
    pub upscale: bool,
    #[serde(default = "default_scale")]
    pub scale: f32,
    #[serde(default)]
    pub format: i32,
    #[serde(default)]
    pub pid: i32,
    #[serde(default)]
    pub timestamp: i64,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct LayerStatus {
    pub installed: bool,
    pub installed_for_games: bool,
    pub dev_build: bool,
    pub manifest_path: Option<String>,
    pub library_path: Option<String>,
    pub source_dir: Option<String>,
    pub stats_path: String,
    pub stats: Option<LayerStats>,
    pub active: bool,
    pub message: String,
}

fn default_scale() -> f32 {
    1.0
}

fn stats_path() -> PathBuf {
    std::env::var_os("ZYRAX_FSR_STATS")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/tmp/zyrax_fsr_stats.json"))
}

fn candidate_dirs() -> Vec<PathBuf> {
    let mut dirs: Vec<PathBuf> = Vec::new();

    if let Some(explicit) = std::env::var_os("ZYRAX_FSR_LAYER_DIR") {
        dirs.push(PathBuf::from(explicit));
    }

    if let Ok(cwd) = std::env::current_dir() {
        dirs.push(cwd.join("vk-layer/build"));
    }

    if let Ok(exe) = std::env::current_exe() {
        for ancestor in exe.ancestors().take(5) {
            dirs.push(ancestor.join("vk-layer/build"));
        }
    }

    #[cfg(debug_assertions)]
    if let Some(root) = Path::new(env!("CARGO_MANIFEST_DIR")).parent() {
        dirs.push(root.join("vk-layer/build"));
    }

    if let Some(data) = dirs::data_dir() {
        dirs.push(data.join("vulkan/implicit_layer.d"));
    }

    if let Some(data) = dirs::data_local_dir() {
        dirs.push(data.join("vulkan/implicit_layer.d"));
    }

    dirs.push(PathBuf::from("/usr/share/vulkan/implicit_layer.d"));
    dirs.push(PathBuf::from("/etc/vulkan/implicit_layer.d"));

    if let Ok(xdg) = std::env::var("XDG_DATA_DIRS") {
        for entry in xdg.split(':').filter(|s| !s.is_empty()) {
            dirs.push(Path::new(entry).join("vulkan/implicit_layer.d"));
        }
    }

    dirs
}

fn bundled_layer_dirs(app: &tauri::AppHandle) -> Vec<PathBuf> {
    let mut dirs = Vec::new();
    if let Ok(resource_dir) = app.path().resource_dir() {
        dirs.push(resource_dir.join("vk-layer"));
        dirs.push(resource_dir);
    }
    dirs
}

fn find_layer(app: &tauri::AppHandle) -> Option<(PathBuf, PathBuf)> {
    let mut dirs = Vec::new();
    if let Some(explicit) = std::env::var_os("ZYRAX_FSR_LAYER_DIR") {
        dirs.push(PathBuf::from(explicit));
    }
    dirs.extend(bundled_layer_dirs(app));
    dirs.extend(candidate_dirs());

    for dir in dirs {
        let manifest = dir.join(MANIFEST_NAME);
        if manifest.is_file() {
            let library = dir.join(LIBRARY_NAME);
            if library.is_file() {
                return Some((manifest, library));
            }
        }
    }
    None
}

#[cfg(target_os = "linux")]
fn control_path() -> PathBuf {
    std::env::var_os("ZYRAX_FSR_CONTROL")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/tmp/zyrax_fsr_control"))
}

/// Write the control file consumed by the Vulkan layer. The layer polls this
/// file and activates only for the matching PID, so attaching works for a game
/// that is already running.
pub fn write_layer_control(
    pid: u32,
    enabled: bool,
    scale_factor: f32,
    sharpness: f32,
    overlay: bool,
    frame_generation: bool,
) -> Result<(), String> {
    #[cfg(not(target_os = "linux"))]
    {
        let _ = (
            pid,
            enabled,
            scale_factor,
            sharpness,
            overlay,
            frame_generation,
        );
        return Err("The Vulkan layer is only available on Linux".to_string());
    }

    #[cfg(target_os = "linux")]
    {
        let path = control_path();
        let tmp = path.with_extension(format!("tmp.{}", std::process::id()));
        let scale = scale_factor.clamp(1.0, 4.0);
        let sharp = sharpness.clamp(0.0, 1.0);
        let body = format!(
            "version=1\nenabled={}\npid={}\nscale={:.4}\nsharpness={:.4}\noverlay={}\nframe_gen={}\n",
            enabled as u8,
            pid,
            scale,
            sharp,
            overlay as u8,
            frame_generation as u8
        );
        fs::write(&tmp, body).map_err(|e| format!("write {}: {e}", tmp.display()))?;
        fs::rename(&tmp, &path).map_err(|e| format!("rename {}: {e}", path.display()))?;
        Ok(())
    }
}

/// True when the layer has been installed in a system/user implicit-layer
/// directory (not merely built in the repository).
pub fn layer_installed_for_games() -> bool {
    candidate_dirs().iter().any(|dir| {
        !dir.ends_with("vk-layer/build")
            && dir.join(MANIFEST_NAME).is_file()
            && dir.join(LIBRARY_NAME).is_file()
    })
}

fn read_stats(path: &Path) -> Option<LayerStats> {
    let raw = fs::read_to_string(path).ok()?;
    serde_json::from_str(&raw).ok()
}

fn now_unix() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0)
}

#[tauri::command]
pub fn get_layer_status(app: tauri::AppHandle) -> LayerStatus {
    let stats_path = stats_path();
    let stats = read_stats(&stats_path);
    let active = stats
        .as_ref()
        .map(|s| s.enabled && now_unix() - s.timestamp <= 3)
        .unwrap_or(false);
    let installed_for_games = layer_installed_for_games();

    let (installed, dev_build, manifest_path, library_path, source_dir, message) =
        match find_layer(&app) {
            Some((manifest, library)) => {
                let dir = manifest.parent().map(Path::to_path_buf);
                let dev = dir
                    .as_ref()
                    .map(|d| d.ends_with("vk-layer/build"))
                    .unwrap_or(false);
                let message = if installed_for_games {
                    "Layer installed. Attach a running Vulkan game from the dashboard."
                } else if dev {
                    "Layer compiled in the repository. Install it to enable it system-wide."
                } else {
                    "Layer bundled with Zyrax. Install it to enable it system-wide."
                };
                (
                    true,
                    dev,
                    Some(manifest.to_string_lossy().to_string()),
                    Some(library.to_string_lossy().to_string()),
                    dir.map(|d| d.to_string_lossy().to_string()),
                    message,
                )
            }
            None => (
                false,
                false,
                None,
                None,
                None,
                "Layer not found. Reinstall Zyrax or build it with vk-layer/build.sh.",
            ),
        };

    LayerStatus {
        installed,
        installed_for_games,
        dev_build,
        manifest_path,
        library_path,
        source_dir,
        stats_path: stats_path.to_string_lossy().to_string(),
        stats,
        active,
        message: message.to_string(),
    }
}

#[tauri::command]
pub fn install_layer(app: tauri::AppHandle) -> Result<LayerStatus, String> {
    let (manifest, library) = find_layer(&app).ok_or_else(|| {
        "No layer found. Reinstall Zyrax or build it with vk-layer/build.sh.".to_string()
    })?;

    let target = dirs::data_dir()
        .ok_or_else(|| "Could not resolve the user data directory".to_string())?
        .join("vulkan/implicit_layer.d");

    fs::create_dir_all(&target).map_err(|e| format!("create {}: {e}", target.display()))?;

    let target_manifest = target.join(MANIFEST_NAME);
    let target_library = target.join(LIBRARY_NAME);

    if manifest != target_manifest {
        fs::copy(&manifest, &target_manifest).map_err(|e| format!("copy manifest: {e}"))?;
    }
    if library != target_library {
        fs::copy(&library, &target_library).map_err(|e| format!("copy library: {e}"))?;
    }

    Ok(get_layer_status(app))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[cfg(target_os = "linux")]
    #[test]
    fn writes_control_file_for_the_layer() {
        let dir = std::env::temp_dir().join(format!("zyrax-control-test-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        let path = dir.join("control");
        std::env::set_var("ZYRAX_FSR_CONTROL", &path);

        write_layer_control(4242, true, 1.75, 0.65, true, false).unwrap();
        let body = fs::read_to_string(&path).unwrap();
        assert!(body.contains("enabled=1"));
        assert!(body.contains("pid=4242"));
        assert!(body.contains("scale=1.7500"));
        assert!(body.contains("sharpness=0.6500"));
        assert!(body.contains("overlay=1"));
        assert!(body.contains("frame_gen=0"));

        write_layer_control(4242, false, 1.0, 0.0, false, false).unwrap();
        let body = fs::read_to_string(&path).unwrap();
        assert!(body.contains("enabled=0"));

        std::env::remove_var("ZYRAX_FSR_CONTROL");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn parses_stats_written_by_the_layer() {
        let dir = std::env::temp_dir().join(format!("zyrax-layer-test-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        let path = dir.join("stats.json");
        fs::write(
            &path,
            r#"{"enabled":true,"presents":787,"sharpness":0.800,"width":939,"height":1016,"renderWidth":626,"renderHeight":677,"upscale":true,"scale":1.500,"format":97,"pid":123,"timestamp":1700000000}"#,
        )
        .unwrap();

        let stats = read_stats(&path).expect("stats should parse");
        assert!(stats.enabled);
        assert_eq!(stats.presents, 787);
        assert!((stats.sharpness - 0.8).abs() < 1e-6);
        assert_eq!((stats.width, stats.height), (939, 1016));
        assert_eq!((stats.render_width, stats.render_height), (626, 677));
        assert!(stats.upscale);
        assert!((stats.scale - 1.5).abs() < 1e-6);
        assert_eq!(stats.format, 97);
        assert_eq!(stats.pid, 123);

        fs::remove_dir_all(&dir).ok();
    }
}
