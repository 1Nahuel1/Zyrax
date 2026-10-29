use serde::{Deserialize, Serialize};
use std::fs;
use std::path::PathBuf;
use tauri::Manager;

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Profile {
    pub id: String,
    pub name: String,
    pub game_exe: Option<String>,
    pub upscaler: String,
    pub scale_factor: f32,
    pub sharpness: f32,
    pub frame_generation: bool,
    pub frame_gen_multiplier: u8,
    pub overlay_enabled: bool,
    #[serde(default = "default_true")]
    pub overlay_show_fps: bool,
    #[serde(default = "default_true")]
    pub overlay_show_scale: bool,
    #[serde(default = "default_overlay_position")]
    pub overlay_position: String,
    pub hotkey_toggle: String,
}

fn default_true() -> bool {
    true
}

fn default_overlay_position() -> String {
    "bottom-left".into()
}

impl Default for Profile {
    fn default() -> Self {
        Self {
            id: "default".into(),
            name: "Default".into(),
            game_exe: None,
            upscaler: "fsr".into(),
            scale_factor: 1.5,
            sharpness: 0.5,
            frame_generation: false,
            frame_gen_multiplier: 2,
            overlay_enabled: true,
            overlay_show_fps: true,
            overlay_show_scale: true,
            overlay_position: "bottom-left".into(),
            hotkey_toggle: "F11".into(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct AppData {
    pub profiles: Vec<Profile>,
    pub active_profile_id: Option<String>,
}

fn data_path(app: &tauri::AppHandle) -> Result<PathBuf, String> {
    let dir = app
        .path()
        .app_config_dir()
        .map_err(|e| format!("no config dir: {e}"))?;
    fs::create_dir_all(&dir).map_err(|e| format!("mkdir failed: {e}"))?;
    Ok(dir.join("zyrax.json"))
}

pub(crate) fn load(app: &tauri::AppHandle) -> AppData {
    let Ok(path) = data_path(app) else {
        return AppData::default();
    };
    match fs::read_to_string(&path) {
        Ok(raw) => serde_json::from_str(&raw).unwrap_or_default(),
        Err(_) => AppData::default(),
    }
}

fn save(app: &tauri::AppHandle, data: &AppData) -> Result<(), String> {
    let path = data_path(app)?;
    let raw = serde_json::to_string_pretty(data).map_err(|e| e.to_string())?;
    fs::write(path, raw).map_err(|e| e.to_string())
}

pub(crate) fn active_hotkey(app: &tauri::AppHandle) -> Option<String> {
    let data = load(app);
    let active = data
        .active_profile_id
        .as_deref()
        .and_then(|id| data.profiles.iter().find(|p| p.id == id))
        .or_else(|| data.profiles.first())?;
    Some(active.hotkey_toggle.clone())
}

#[tauri::command]
pub fn get_profiles(app: tauri::AppHandle) -> AppData {
    let mut data = load(&app);
    if data.profiles.is_empty() {
        data.profiles.push(Profile::default());
        let _ = save(&app, &data);
    }
    data
}

#[tauri::command]
pub fn save_profile(app: tauri::AppHandle, profile: Profile) -> Result<AppData, String> {
    let mut data = load(&app);
    match data.profiles.iter_mut().find(|p| p.id == profile.id) {
        Some(existing) => *existing = profile,
        None => data.profiles.push(profile),
    }
    save(&app, &data)?;
    Ok(data)
}

#[tauri::command]
pub fn delete_profile(app: tauri::AppHandle, id: String) -> Result<AppData, String> {
    let mut data = load(&app);
    data.profiles.retain(|p| p.id != id);
    if data.active_profile_id.as_deref() == Some(&id) {
        data.active_profile_id = None;
    }
    save(&app, &data)?;
    Ok(data)
}

#[tauri::command]
pub fn set_active_profile(app: tauri::AppHandle, id: Option<String>) -> Result<AppData, String> {
    let mut data = load(&app);
    data.active_profile_id = id;
    save(&app, &data)?;
    Ok(data)
}
