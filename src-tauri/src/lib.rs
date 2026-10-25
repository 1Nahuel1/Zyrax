mod games;
mod gpu;
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
        .manage(RuntimeState::default())
        .invoke_handler(tauri::generate_handler![
            processes::list_processes,
            processes::get_process_stats,
            games::scan_games,
            gpu::detect_gpu,
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
