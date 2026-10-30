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
    pub managed_install: bool,
    pub update_available: bool,
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

fn managed_install_dir() -> Result<PathBuf, String> {
    dirs::data_dir()
        .map(|dir| dir.join("vulkan/implicit_layer.d"))
        .ok_or_else(|| "Could not resolve the user data directory".to_string())
}

fn installed_at(dir: &Path) -> bool {
    dir.join(MANIFEST_NAME).is_file() && dir.join(LIBRARY_NAME).is_file()
}

fn same_bytes(left: &Path, right: &Path) -> bool {
    match (fs::read(left), fs::read(right)) {
        (Ok(left), Ok(right)) => left == right,
        _ => false,
    }
}

fn update_available(manifest: &Path, library: &Path) -> bool {
    let Ok(dir) = managed_install_dir() else {
        return false;
    };
    if !installed_at(&dir) {
        return false;
    }
    !same_bytes(manifest, &dir.join(MANIFEST_NAME)) || !same_bytes(library, &dir.join(LIBRARY_NAME))
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

/// Control file payload consumed by the Vulkan layer.
#[derive(Debug, Clone)]
pub struct LayerControl {
    pub pid: u32,
    pub enabled: bool,
    pub scale_factor: f32,
    pub sharpness: f32,
    pub overlay: bool,
    pub overlay_show_fps: bool,
    pub overlay_show_scale: bool,
    pub overlay_position: String,
    pub frame_generation: bool,
}

/// Write the control file consumed by the Vulkan layer. The layer polls this
/// file and activates only for the matching PID, so attaching works for a game
/// that is already running.
pub fn write_layer_control(control: &LayerControl) -> Result<(), String> {
    #[cfg(not(target_os = "linux"))]
    {
        let _ = control;
        return Err(
            "The DXGI hook is not implemented; the Zyrax Vulkan layer is Linux-only.".to_string(),
        );
    }

    #[cfg(target_os = "linux")]
    {
        let path = control_path();
        let tmp = path.with_extension(format!("tmp.{}", std::process::id()));
        let scale = control.scale_factor.clamp(1.0, 4.0);
        let sharp = control.sharpness.clamp(0.0, 1.0);
        let position = match control.overlay_position.to_ascii_lowercase().as_str() {
            "top-left" => 1,
            "bottom-right" => 2,
            "top-right" => 3,
            _ => 0,
        };
        let body = format!(
            "version=1\nenabled={}\npid={}\nscale={:.4}\nsharpness={:.4}\noverlay={}\noverlay_fps={}\noverlay_scale={}\noverlay_pos={}\nframe_gen={}\n",
            control.enabled as u8,
            control.pid,
            scale,
            sharp,
            control.overlay as u8,
            control.overlay_show_fps as u8,
            control.overlay_show_scale as u8,
            position,
            control.frame_generation as u8
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
    let managed_install = managed_install_dir()
        .map(|dir| installed_at(&dir))
        .unwrap_or(false);
    let found = find_layer(&app);
    let update_available = found
        .as_ref()
        .map(|(manifest, library)| update_available(manifest, library))
        .unwrap_or(false);

    let (installed, dev_build, manifest_path, library_path, source_dir, message) = match found {
        Some((manifest, library)) => {
            let dir = manifest.parent().map(Path::to_path_buf);
            let dev = dir
                .as_ref()
                .map(|d| d.ends_with("vk-layer/build"))
                .unwrap_or(false);
            let message = if update_available {
                "A newer bundled layer is available. Update it system-wide."
            } else if installed_for_games {
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
        managed_install,
        update_available,
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

    let target = managed_install_dir()?;

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

#[tauri::command]
pub fn uninstall_layer(app: tauri::AppHandle) -> Result<LayerStatus, String> {
    let target = managed_install_dir()?;
    let mut removed = false;
    for name in [MANIFEST_NAME, LIBRARY_NAME] {
        let path = target.join(name);
        if path.is_file() {
            fs::remove_file(&path).map_err(|e| format!("remove {}: {e}", path.display()))?;
            removed = true;
        }
    }
    if !removed {
        return Err(format!("Layer is not installed in {}", target.display()));
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

        write_layer_control(&LayerControl {
            pid: 4242,
            enabled: true,
            scale_factor: 1.75,
            sharpness: 0.65,
            overlay: true,
            overlay_show_fps: true,
            overlay_show_scale: false,
            overlay_position: "top-right".into(),
            frame_generation: false,
        })
        .unwrap();
        let body = fs::read_to_string(&path).unwrap();
        assert!(body.contains("enabled=1"));
        assert!(body.contains("pid=4242"));
        assert!(body.contains("scale=1.7500"));
        assert!(body.contains("sharpness=0.6500"));
        assert!(body.contains("overlay=1"));
        assert!(body.contains("overlay_fps=1"));
        assert!(body.contains("overlay_scale=0"));
        assert!(body.contains("overlay_pos=3"));
        assert!(body.contains("frame_gen=0"));

        write_layer_control(&LayerControl {
            pid: 4242,
            enabled: false,
            scale_factor: 1.0,
            sharpness: 0.0,
            overlay: false,
            overlay_show_fps: true,
            overlay_show_scale: true,
            overlay_position: "bottom-left".into(),
            frame_generation: false,
        })
        .unwrap();
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
