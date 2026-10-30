use serde::Serialize;
use sysinfo::{ProcessesToUpdate, System};

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RunningProcess {
    pub pid: u32,
    pub name: String,
    pub exe: Option<String>,
    pub memory_mb: u64,
    pub cpu_usage: f32,
}

const SYSTEM_PROCESS_BLOCKLIST: &[&str] = &[
    "system",
    "system idle process",
    "idle",
    "kernel_task",
    "kthreadd",
    "init",
    "systemd",
    "zyrax",
    "explorer.exe",
    "svchost.exe",
    "csrss.exe",
    "wininit.exe",
    "winlogon.exe",
    "lsass.exe",
    "services.exe",
    "smss.exe",
    "dwm.exe",
    "conhost.exe",
    "runtimebroker.exe",
    "registry",
    "memory compression",
];

fn is_system_process(name: &str) -> bool {
    let lower = name.to_lowercase();
    SYSTEM_PROCESS_BLOCKLIST
        .iter()
        .any(|b| lower == *b || lower.starts_with("systemd-"))
}

/// Returns processes that are plausible game/application candidates,
/// sorted by memory usage (descending).
#[tauri::command]
pub fn list_processes() -> Vec<RunningProcess> {
    let mut sys = System::new();
    sys.refresh_processes(ProcessesToUpdate::All, true);

    let mut processes: Vec<RunningProcess> = sys
        .processes()
        .iter()
        .filter_map(|(pid, proc_)| {
            let name = proc_.name().to_string_lossy().to_string();
            if name.is_empty() || is_system_process(&name) {
                return None;
            }
            Some(RunningProcess {
                pid: pid.as_u32(),
                name,
                exe: proc_.exe().map(|p| p.to_string_lossy().to_string()),
                memory_mb: proc_.memory() / (1024 * 1024),
                cpu_usage: proc_.cpu_usage(),
            })
        })
        .collect();

    processes.sort_by_key(|p| std::cmp::Reverse(p.memory_mb));
    processes.truncate(200);
    processes
}

/// Returns a live refresh of a single process' CPU and memory usage.
#[tauri::command]
pub fn get_process_stats(pid: u32) -> Option<RunningProcess> {
    let mut sys = System::new();
    sys.refresh_processes(ProcessesToUpdate::All, true);
    let pid = sysinfo::Pid::from_u32(pid);
    sys.process(pid).map(|proc_| RunningProcess {
        pid: pid.as_u32(),
        name: proc_.name().to_string_lossy().to_string(),
        exe: proc_.exe().map(|p| p.to_string_lossy().to_string()),
        memory_mb: proc_.memory() / (1024 * 1024),
        cpu_usage: proc_.cpu_usage(),
    })
}
