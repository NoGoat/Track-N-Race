#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]
mod host;
use host::{Core, HostState, Settings};
use serde_json::json;
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc, Mutex,
};
use tauri::{
    menu::{Menu, MenuItem},
    tray::TrayIconBuilder,
    Emitter, Manager,
};

fn request_exit(app: &tauri::AppHandle) {
    let state = app.state::<HostState>();
    if state.exit_started.swap(true, Ordering::AcqRel) {
        return;
    }
    let core = state.core.clone();
    let app = app.clone();
    let log_dir = state.log_dir.clone();
    let settings = state.settings.clone();
    tauri::async_runtime::spawn(async move {
        match core.shutdown().await {
            Ok(private) if private.is_object() => {
                let result = (|| -> Result<(), String> {
                    let mut settings = settings.lock().map_err(|e| e.to_string())?;
                    settings.set("pairing.engineState", json!(private.to_string()))?;
                    settings.set("pairing.enabled", private["enabled"].clone())
                })();
                if let Err(error) = result {
                    host::append_log(&log_dir, &format!("Save pairing on exit: {error}"));
                }
            }
            Err(error) => host::append_log(&log_dir, &format!("Shutdown failed: {error}")),
            _ => {}
        }
        app.state::<HostState>()
            .exit_finished
            .store(true, Ordering::Release);
        app.exit(0);
    });
}
fn open_files(app: &tauri::AppHandle, files: Vec<String>) {
    let state = app.state::<HostState>();
    let mut pending = state
        .pending_files
        .lock()
        .unwrap_or_else(|e| e.into_inner());
    for file in files {
        if state.renderer_ready.load(Ordering::Acquire) {
            let _ = app.emit_to("main", "open-recording", &file);
        } else {
            pending.push(file);
        }
    }
    if let Some(window) = app.get_webview_window("main") {
        let _ = window.unminimize();
        let _ = window.show();
        let _ = window.set_focus();
    }
}
fn recording_args(args: impl Iterator<Item = String>) -> Vec<String> {
    args.filter(|s| {
        let lower = s.to_lowercase();
        (lower.ends_with(".tnrd") || lower.ends_with(".trnd")) && std::path::Path::new(s).is_file()
    })
    .collect()
}
fn main() {
    let app = tauri::Builder::default()
        .plugin(tauri_plugin_single_instance::init(|app, args, _cwd| {
            open_files(app, recording_args(args.into_iter()));
        }))
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_opener::init())
        .setup(|app| {
            let data = app.path().app_data_dir()?;
            let log_dir = data.join("launch-diagnostics");
            let settings =
                Settings::load(data.join("settings.json")).map_err(std::io::Error::other)?;
            let native_titlebar = settings.get("nativeTitlebar", json!(true)) == true;
            let core = Core::new().map_err(std::io::Error::other)?;
            app.manage(HostState {
                core,
                settings: Arc::new(Mutex::new(settings)),
                pending_files: Mutex::new(recording_args(std::env::args().skip(1))),
                renderer_ready: AtomicBool::new(false),
                log_dir,
                exit_started: AtomicBool::new(false),
                exit_finished: AtomicBool::new(false),
            });
            if let Some(window) = app.get_webview_window("main") {
                window.set_decorations(native_titlebar)?;
                // Keep a window visible even when initialization fails.
                window.show()?;
            }
            let show = MenuItem::with_id(app, "show", "Show Track N Race", true, None::<&str>)?;
            let quit = MenuItem::with_id(app, "quit", "Quit", true, None::<&str>)?;
            let menu = Menu::with_items(app, &[&show, &quit])?;
            let mut tray = TrayIconBuilder::new()
                .tooltip("Track N Race Tauri")
                .menu(&menu)
                .on_menu_event(|app, event| match event.id.as_ref() {
                    "show" => {
                        if let Some(window) = app.get_webview_window("main") {
                            let _ = window.unminimize();
                            let _ = window.show();
                            let _ = window.set_focus();
                        }
                    }
                    "quit" => request_exit(app),
                    _ => {}
                });
            if let Some(icon) = app.default_window_icon() {
                tray = tray.icon(icon.clone());
            }
            tray.build(app)?;
            Ok(())
        })
        .on_window_event(|window, event| {
            if let tauri::WindowEvent::CloseRequested { api, .. } = event {
                api.prevent_close();
                request_exit(window.app_handle());
            }
        })
        .invoke_handler(tauri::generate_handler![
            host::bootstrap,
            host::settings_set,
            host::engine_start,
            host::engine_command,
            host::stream_start,
            host::stream_ack,
            host::renderer_ready,
            host::diagnostics,
            host::update_check,
        ])
        .build(tauri::generate_context!())
        .expect("Unable to initialize Track N Race Tauri");
    app.run(|app, event| match event {
        tauri::RunEvent::ExitRequested { api, .. } => {
            if !app
                .state::<HostState>()
                .exit_finished
                .load(Ordering::Acquire)
            {
                api.prevent_exit();
                request_exit(app);
            }
        }
        #[cfg(target_os = "macos")]
        tauri::RunEvent::Opened { urls } => {
            open_files(
                app,
                urls.into_iter()
                    .filter_map(|url| url.to_file_path().ok())
                    .map(|path| path.to_string_lossy().into_owned())
                    .collect(),
            );
        }
        _ => {}
    });
}
