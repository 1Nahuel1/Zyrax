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
    pub overlay_show_fps: bool,
    pub overlay_show_scale: bool,
    pub overlay_position: String,
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
            overlay_show_fps: true,
            overlay_show_scale: true,
            overlay_position: "bottom-left".into(),
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

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct AttachRequest {
    pub pid: u32,
    pub game_name: String,
    pub upscaler: String,
    pub scale_factor: f32,
    pub sharpness: f32,
    pub frame_generation: bool,
    pub overlay_enabled: bool,
    pub overlay_show_fps: bool,
    pub overlay_show_scale: bool,
    pub overlay_position: String,
}

#[tauri::command]
pub fn get_runtime_status(state: tauri::State<RuntimeState>) -> RuntimeStatus {
    state.status.lock().unwrap().clone()
}

#[tauri::command]
pub fn attach_to_game(
    state: tauri::State<RuntimeState>,
    request: AttachRequest,
) -> RuntimeStatus {
    let mut guard = state.status.lock().unwrap();

    #[cfg(not(target_os = "linux"))]
    {
        *guard = RuntimeStatus {
            state: HookState::Failed,
            pid: Some(request.pid),
            game_name: Some(request.game_name.clone()),
            message: "The DXGI hook is not implemented; the Zyrax Vulkan layer is Linux-only."
                .to_string(),
            ..Default::default()
        };
        return guard.clone();
    }

    let upscaler = request.upscaler.trim().to_ascii_lowercase();
    if upscaler != "fsr" {
        *guard = RuntimeStatus {
            state: HookState::Failed,
            pid: Some(request.pid),
            game_name: Some(request.game_name),
            message: "Only FSR is implemented by the Linux Vulkan layer. Select FSR.".to_string(),
            ..Default::default()
        };
        return guard.clone();
    }

    // Frame generation is not implemented by the layer. Never attach with it
    // enabled; fall back to FSR only and warn the user.
    let frame_generation_requested = request.frame_generation;
    let frame_generation = false;

    let scale_factor = if request.scale_factor.is_finite() {
        request.scale_factor.clamp(1.0, 4.0)
    } else {
        1.5
    };
    let sharpness = if request.sharpness.is_finite() {
        request.sharpness.clamp(0.0, 1.0)
    } else {
        0.5
    };
    let overlay_position = match request.overlay_position.to_ascii_lowercase().as_str() {
        "top-left" => "top-left",
        "bottom-right" => "bottom-right",
        "top-right" => "top-right",
        _ => "bottom-left",
    }
    .to_string();

    // Verify the target process is still alive before attaching.
    let mut sys = System::new();
    sys.refresh_processes(ProcessesToUpdate::All, true);
    if sys.process(Pid::from_u32(request.pid)).is_none() {
        *guard = RuntimeStatus {
            state: HookState::Failed,
            pid: None,
            game_name: None,
            message: format!("Process {} is no longer running.", request.pid),
            ..Default::default()
        };
        return guard.clone();
    }

    if !layer::layer_installed_for_games() {
        *guard = RuntimeStatus {
            state: HookState::Failed,
            pid: Some(request.pid),
            game_name: Some(request.game_name),
            message: "Vulkan layer is not installed. Install it in Settings first.".to_string(),
            ..Default::default()
        };
        return guard.clone();
    }

    if let Err(e) = layer::write_layer_control(&layer::LayerControl {
        pid: request.pid,
        enabled: true,
        scale_factor,
        sharpness,
        overlay: request.overlay_enabled,
        overlay_show_fps: request.overlay_show_fps,
        overlay_show_scale: request.overlay_show_scale,
        overlay_position: overlay_position.clone(),
        frame_generation,
    }) {
        *guard = RuntimeStatus {
            state: HookState::Failed,
            pid: Some(request.pid),
            game_name: Some(request.game_name),
            message: format!("Could not activate the layer: {e}"),
            ..Default::default()
        };
        return guard.clone();
    }

    *guard = RuntimeStatus {
        state: HookState::Active,
        pid: Some(request.pid),
        game_name: Some(request.game_name.clone()),
        upscaler: Some(upscaler.clone()),
        scale_factor,
        sharpness,
        frame_generation,
        overlay_enabled: request.overlay_enabled,
        overlay_show_fps: request.overlay_show_fps,
        overlay_show_scale: request.overlay_show_scale,
        overlay_position,
        injected_at: Some(
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|d| d.as_secs())
                .unwrap_or(0),
        ),
        message: if frame_generation_requested {
            format!(
                "Attached to {}. Upscaler: {}, x{scale_factor:.2}. Frame generation is not implemented; attached with FSR only.",
                request.game_name,
                upscaler.to_uppercase()
            )
        } else {
            format!(
                "Attached to {}. Upscaler: {}, x{scale_factor:.2}, FrameGen: OFF",
                request.game_name,
                upscaler.to_uppercase()
            )
        },
    };
    guard.clone()
}

#[tauri::command]
pub fn detach_from_game(state: tauri::State<RuntimeState>) -> RuntimeStatus {
    let mut guard = state.status.lock().unwrap();
    if let Some(pid) = guard.pid {
        let _ = layer::write_layer_control(&layer::LayerControl {
            pid,
            enabled: false,
            scale_factor: guard.scale_factor,
            sharpness: guard.sharpness,
            overlay: guard.overlay_enabled,
            overlay_show_fps: guard.overlay_show_fps,
            overlay_show_scale: guard.overlay_show_scale,
            overlay_position: guard.overlay_position.clone(),
            frame_generation: guard.frame_generation,
        });
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
            if let Err(e) = layer::write_layer_control(&layer::LayerControl {
                pid,
                enabled: true,
                scale_factor: guard.scale_factor,
                sharpness: guard.sharpness,
                overlay: enabled,
                overlay_show_fps: guard.overlay_show_fps,
                overlay_show_scale: guard.overlay_show_scale,
                overlay_position: guard.overlay_position.clone(),
                frame_generation: guard.frame_generation,
            }) {
                guard.message = format!("Overlay toggled, but layer update failed: {e}");
                return guard.clone();
            }
        }
    }
    guard.clone()
}

pub(crate) fn toggle_overlay(app: &tauri::AppHandle) {
    use tauri::{Emitter, Manager};

    let state = app.state::<RuntimeState>();
    let mut guard = state.status.lock().unwrap();
    if guard.state != HookState::Active {
        guard.message = "Hotkey pressed, but no game is attached.".into();
        let status = guard.clone();
        drop(guard);
        let _ = app.emit("runtime-status", status);
        return;
    }

    let enabled = !guard.overlay_enabled;
    guard.overlay_enabled = enabled;
    if let Some(pid) = guard.pid {
        match layer::write_layer_control(&layer::LayerControl {
            pid,
            enabled: true,
            scale_factor: guard.scale_factor,
            sharpness: guard.sharpness,
            overlay: enabled,
            overlay_show_fps: guard.overlay_show_fps,
            overlay_show_scale: guard.overlay_show_scale,
            overlay_position: guard.overlay_position.clone(),
            frame_generation: guard.frame_generation,
        }) {
            Ok(()) => {
                guard.message = format!("Overlay toggled {}.", if enabled { "on" } else { "off" })
            }
            Err(e) => guard.message = format!("Overlay toggled, but layer update failed: {e}"),
        }
    }
    let status = guard.clone();
    drop(guard);
    let _ = app.emit("runtime-status", status);
}
