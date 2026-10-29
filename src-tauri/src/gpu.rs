use serde::Serialize;

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct GpuInfo {
    pub name: String,
    pub vendor: String,
    pub driver: String,
    pub vram_mb: u64,
    pub api_support: Vec<String>,
}

#[allow(dead_code)]
fn vendor_from_name(name: &str) -> &'static str {
    let lower = name.to_lowercase();
    if lower.contains("nvidia") || lower.contains("geforce") || lower.contains("quadro") {
        "NVIDIA"
    } else if lower.contains("amd") || lower.contains("radeon") || lower.contains("ati") {
        "AMD"
    } else if lower.contains("intel") {
        "Intel"
    } else {
        "Unknown"
    }
}

fn apis_for_vendor(vendor: &str) -> Vec<String> {
    let mut apis = vec!["Vulkan".to_string(), "OpenGL".to_string()];
    match vendor {
        "NVIDIA" => {
            apis.push("DirectX 11".into());
            apis.push("DirectX 12".into());
            apis.push("DLSS".into());
        }
        "AMD" => {
            apis.push("DirectX 11".into());
            apis.push("DirectX 12".into());
            apis.push("FSR".into());
        }
        _ => {
            apis.push("DirectX 11".into());
            apis.push("DirectX 12".into());
        }
    }
    apis
}

#[cfg(target_os = "linux")]
fn detect_gpus_linux() -> Vec<GpuInfo> {
    use std::fs;
    let mut gpus = Vec::new();
    let drm_path = std::path::Path::new("/sys/class/drm");
    let Ok(entries) = fs::read_dir(drm_path) else {
        return gpus;
    };
    for entry in entries.flatten() {
        let name = entry.file_name().to_string_lossy().to_string();
        if !name.starts_with("card") || name.contains('-') {
            continue;
        }
        let device = entry.path().join("device");
        let read = |file: &str| {
            fs::read_to_string(device.join(file))
                .ok()
                .map(|s| s.trim().to_string())
        };
        let vendor_id = read("vendor").unwrap_or_default();
        let device_id = read("device").unwrap_or_default();
        let driver = fs::read_link(device.join("driver"))
            .ok()
            .and_then(|p| p.file_name().map(|n| n.to_string_lossy().to_string()))
            .unwrap_or_else(|| "unknown".into());

        let vendor = match vendor_id.as_str() {
            "0x10de" => "NVIDIA",
            "0x1002" | "0x1022" => "AMD",
            "0x8086" => "Intel",
            _ => "Unknown",
        }
        .to_string();

        let vram_mb = read("mem_info_vram_total")
            .and_then(|s| s.parse::<u64>().ok())
            .map(|b| b / (1024 * 1024))
            .unwrap_or(0);

        let display = format!("{} GPU [{}:{}]", vendor, vendor_id, device_id);
        gpus.push(GpuInfo {
            name: display,
            vendor: vendor.clone(),
            driver,
            vram_mb,
            api_support: apis_for_vendor(&vendor),
        });
    }
    gpus
}

#[cfg(target_os = "windows")]
fn detect_gpus_windows() -> Vec<GpuInfo> {
    use std::process::Command;
    let mut gpus = Vec::new();
    let output = Command::new("wmic")
        .args([
            "path",
            "win32_VideoController",
            "get",
            "Name,AdapterRAM,DriverVersion",
            "/format:csv",
        ])
        .output();
    if let Ok(out) = output {
        let text = String::from_utf8_lossy(&out.stdout);
        for line in text.lines().skip(1) {
            let cols: Vec<&str> = line.split(',').collect();
            if cols.len() < 4 {
                continue;
            }
            let vram = cols[2].trim().parse::<u64>().unwrap_or(0) / (1024 * 1024);
            let driver = cols[3].trim().to_string();
            let name = cols[1].trim().to_string();
            if name.is_empty() {
                continue;
            }
            let vendor = vendor_from_name(&name).to_string();
            gpus.push(GpuInfo {
                name,
                vendor: vendor.clone(),
                driver,
                vram_mb: vram,
                api_support: apis_for_vendor(&vendor),
            });
        }
    }
    gpus
}

#[tauri::command]
pub fn detect_gpu() -> Vec<GpuInfo> {
    #[cfg(target_os = "linux")]
    {
        detect_gpus_linux()
    }
    #[cfg(target_os = "windows")]
    {
        detect_gpus_windows()
    }
    #[cfg(not(any(target_os = "linux", target_os = "windows")))]
    {
        Vec::new()
    }
}
