use std::str::FromStr;

use tauri::AppHandle;
use tauri_plugin_global_shortcut::{GlobalShortcutExt, Shortcut, ShortcutEvent, ShortcutState};

use crate::{profiles, runtime};

pub const DEFAULT_HOTKEY: &str = "F11";

pub fn handle_event(app: &AppHandle, _shortcut: &Shortcut, event: ShortcutEvent) {
    if event.state == ShortcutState::Pressed {
        runtime::toggle_overlay(app);
    }
}

fn register(app: &AppHandle, accelerator: &str) -> Result<(), String> {
    let accelerator = accelerator.trim();
    if accelerator.is_empty() {
        return Err("Hotkey cannot be empty".to_string());
    }
    let shortcut = Shortcut::from_str(accelerator)
        .map_err(|e| format!("Invalid hotkey \"{accelerator}\": {e}"))?;
    let shortcuts = app.global_shortcut();
    shortcuts
        .unregister_all()
        .map_err(|e| format!("Could not release the previous hotkey: {e}"))?;
    shortcuts
        .register(shortcut)
        .map_err(|e| format!("Could not register \"{accelerator}\": {e}"))
}

pub fn register_from_profiles(app: &AppHandle) {
    let requested = profiles::active_hotkey(app).unwrap_or_else(|| DEFAULT_HOTKEY.to_string());
    if let Err(error) = register(app, &requested) {
        tracing::warn!("{error}; falling back to {DEFAULT_HOTKEY}");
        if let Err(fallback) = register(app, DEFAULT_HOTKEY) {
            tracing::warn!("could not register fallback hotkey: {fallback}");
        }
    }
}

#[tauri::command]
pub fn set_global_hotkey(app: AppHandle, accelerator: String) -> Result<(), String> {
    register(&app, &accelerator)
}
