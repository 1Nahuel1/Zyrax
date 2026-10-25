use std::fs;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};

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
    pub dev_build: bool,
    pub manifest_path: Option<String>,
    pub library_path: Option<String>,
    pub source_dir: Option<String>,
    pub stats_path: String,
    pub stats: Option<LayerStats>,
    pub active: bool,
    pub message: String,
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

fn find_layer() -> Option<(PathBuf, PathBuf)> {
    for dir in candidate_dirs() {
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
pub fn get_layer_status() -> LayerStatus {
    let stats_path = stats_path();
    let stats = read_stats(&stats_path);
    let active = stats
        .as_ref()
        .map(|s| s.enabled && now_unix() - s.timestamp <= 3)
        .unwrap_or(false);

    let (installed, dev_build, manifest_path, library_path, source_dir, message) =
        match find_layer() {
            Some((manifest, library)) => {
                let dir = manifest.parent().map(Path::to_path_buf);
                let dev = dir
                    .as_ref()
                    .map(|d| d.ends_with("vk-layer/build"))
                    .unwrap_or(false);
                let message = if dev {
                    "Layer compiled in the repository. Install it to enable it system-wide."
                } else {
                    "Layer installed. Launch a Vulkan game with ZYRAX_FSR_ENABLE=1."
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
                "Layer not found. Build it with vk-layer/build.sh.",
            ),
        };

    LayerStatus {
        installed,
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
pub fn install_layer() -> Result<LayerStatus, String> {
    let (manifest, library) = find_layer().ok_or_else(|| {
        "No built layer found. Run vk-layer/build.sh inside the repository first.".to_string()
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

    Ok(get_layer_status())
}
