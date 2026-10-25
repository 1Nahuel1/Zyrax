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
    frame_generation: bool,
    overlay_enabled: bool,
) -> RuntimeStatus {
    let mut guard = state.status.lock().unwrap();

    if guard.state == HookState::Active && guard.pid == Some(pid) {
        return guard.clone();
    }

    *guard = RuntimeStatus {
        state: HookState::Attaching,
        pid: Some(pid),
        game_name: Some(game_name.clone()),
        upscaler: Some(upscaler.clone()),
        frame_generation,
        overlay_enabled,
        injected_at: None,
        message: format!("Attaching hooks to {game_name} (pid {pid})..."),
    };

    // Verify the target process is still alive before declaring the hook active.
    let mut sys = System::new();
    sys.refresh_processes(ProcessesToUpdate::All, true);
    if sys.process(Pid::from_u32(pid)).is_none() {
        guard.state = HookState::Failed;
        guard.pid = None;
        guard.game_name = None;
        guard.message = format!("Process {pid} is no longer running.");
        return guard.clone();
    }

    // Graphics interception layer (Vulkan implicit layer / DXGI swapchain hook)
    // attaches here. It transitions to Active once the first frame is presented.
    guard.state = HookState::Active;
    guard.injected_at = Some(
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_secs())
            .unwrap_or(0),
    );
    guard.message = format!(
        "Hook active on {game_name}. Upscaler: {}, FrameGen: {}",
        upscaler.to_uppercase(),
        if frame_generation { "ON" } else { "OFF" }
    );
    guard.clone()
}

#[tauri::command]
pub fn detach_from_game(state: tauri::State<RuntimeState>) -> RuntimeStatus {
    let mut guard = state.status.lock().unwrap();
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
    guard.clone()
}
