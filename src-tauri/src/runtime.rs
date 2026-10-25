use crate::layer;
use serde::{Deserialize, Serialize};
use std::sync::Mutex;
use sysinfo::{Pid, ProcessesToUpdate, System};

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "lowercase")]
pub enum HookState {
    Idle,
    Attaching,
    Active,
    Failed,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RuntimeStatus {
    pub state: HookState,
    pub pid: Option<u32>,
    pub game_name: Option<String>,
    pub upscaler: Option<String>,
    pub scale_factor: f32,
    pub sharpness: f32,
    pub frame_generation: bool,
    pub overlay_enabled: bool,
    pub injected_at: Option<u64>,
    pub message: String,
}

impl Default for RuntimeStatus {
    fn default() -> Self {
        Self {
            state: HookState::Idle,
            pid: None,
            game_name: None,
            upscaler: None,
            scale_factor: 1.5,
            sharpness: 0.5,
            frame_generation: false,
            overlay_enabled: false,
            injected_at: None,
            message: "Ready. Select a game to attach Zyrax.".into(),
        }
    }
}

pub struct RuntimeState {
    pub status: Mutex<RuntimeStatus>,
}

impl Default for RuntimeState {
    fn default() -> Self {
        Self {
            status: Mutex::new(RuntimeStatus::default()),
        }
    }
}

#[tauri::command]
pub fn get_runtime_status(state: tauri::State<RuntimeState>) -> RuntimeStatus {
    state.status.lock().unwrap().clone()
}

#[tauri::command]
pub fn attach_to_game(
    state: tauri::State<RuntimeState>,
    pid: u32,
    game_name: String,
    upscaler: String,
    scale_factor: f32,
    sharpness: f32,
    frame_generation: bool,
    overlay_enabled: bool,
) -> RuntimeStatus {
    let mut guard = state.status.lock().unwrap();

    let upscaler = upscaler.trim().to_ascii_lowercase();
    if upscaler != "fsr" {
        *guard = RuntimeStatus {
            state: HookState::Failed,
            pid: Some(pid),
            game_name: Some(game_name),
            message: "Only FSR is implemented by the Linux Vulkan layer. Select FSR.".to_string(),
            ..Default::default()
        };
        return guard.clone();
    }

    let scale_factor = if scale_factor.is_finite() {
        scale_factor.clamp(1.0, 4.0)
    } else {
        1.5
    };
    let sharpness = if sharpness.is_finite() {
        sharpness.clamp(0.0, 1.0)
    } else {
        0.5
    };

    // Verify the target process is still alive before attaching.
    let mut sys = System::new();
    sys.refresh_processes(ProcessesToUpdate::All, true);
    if sys.process(Pid::from_u32(pid)).is_none() {
        *guard = RuntimeStatus {
            state: HookState::Failed,
            pid: None,
            game_name: None,
            message: format!("Process {pid} is no longer running."),
            ..Default::default()
        };
        return guard.clone();
    }

    if !layer::layer_installed_for_games() {
        *guard = RuntimeStatus {
            state: HookState::Failed,
            pid: Some(pid),
            game_name: Some(game_name),
            message: "Vulkan layer is not installed. Install it in Settings first.".to_string(),
            ..Default::default()
        };
        return guard.clone();
    }

    if let Err(e) = layer::write_layer_control(
        pid,
        true,
        scale_factor,
        sharpness,
        overlay_enabled,
        frame_generation,
    ) {
        *guard = RuntimeStatus {
            state: HookState::Failed,
            pid: Some(pid),
            game_name: Some(game_name),
            message: format!("Could not activate the layer: {e}"),
            ..Default::default()
        };
        return guard.clone();
    }

    *guard = RuntimeStatus {
        state: HookState::Active,
        pid: Some(pid),
        game_name: Some(game_name.clone()),
        upscaler: Some(upscaler.clone()),
        scale_factor,
        sharpness,
        frame_generation,
        overlay_enabled,
        injected_at: Some(
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|d| d.as_secs())
                .unwrap_or(0),
        ),
        message: format!(
            "Attached to {game_name}. Upscaler: {}, x{scale_factor:.2}, FrameGen: {}",
            upscaler.to_uppercase(),
            if frame_generation { "ON" } else { "OFF" }
        ),
    };
    guard.clone()
}

#[tauri::command]
pub fn detach_from_game(state: tauri::State<RuntimeState>) -> RuntimeStatus {
    let mut guard = state.status.lock().unwrap();
    if let Some(pid) = guard.pid {
        let _ = layer::write_layer_control(
            pid,
            false,
            guard.scale_factor,
            guard.sharpness,
            guard.overlay_enabled,
            guard.frame_generation,
        );
    }
    *guard = RuntimeStatus {
        message: "Detached. Hooks released.".into(),
        ..Default::default()
    };
    guard.clone()
}

#[tauri::command]
pub fn set_overlay_enabled(state: tauri::State<RuntimeState>, enabled: bool) -> RuntimeStatus {
    let mut guard = state.status.lock().unwrap();
    guard.overlay_enabled = enabled;
    if guard.state == HookState::Active {
        if let Some(pid) = guard.pid {
            if let Err(e) = layer::write_layer_control(
                pid,
                true,
                guard.scale_factor,
                guard.sharpness,
                enabled,
                guard.frame_generation,
            ) {
                guard.message = format!("Overlay toggled, but layer update failed: {e}");
                return guard.clone();
            }
        }
    }
    guard.clone()
}
