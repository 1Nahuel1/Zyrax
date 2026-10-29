mod games;
mod gpu;
mod hotkeys;
mod layer;
mod processes;
mod profiles;
mod runtime;

use runtime::RuntimeState;

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tracing_subscriber::fmt()
        .with_max_level(tracing::Level::INFO)
        .init();

    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .plugin(
            tauri_plugin_global_shortcut::Builder::new()
                .with_handler(hotkeys::handle_event)
                .build(),
        )
        .manage(RuntimeState::default())
        .setup(|app| {
            hotkeys::register_from_profiles(app.handle());
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            processes::list_processes,
            processes::get_process_stats,
            games::scan_games,
            gpu::detect_gpu,
            layer::get_layer_status,
            layer::install_layer,
            layer::uninstall_layer,
            hotkeys::set_global_hotkey,
            profiles::get_profiles,
            profiles::save_profile,
            profiles::delete_profile,
            profiles::set_active_profile,
            runtime::get_runtime_status,
            runtime::attach_to_game,
            runtime::detach_from_game,
            runtime::set_overlay_enabled,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
