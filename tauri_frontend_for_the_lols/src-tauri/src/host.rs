use serde_json::{json, Value};
use std::{
    ffi::{c_char, c_void, CStr, CString},
    fs,
    io::Write,
    path::PathBuf,
    sync::{
        atomic::{AtomicBool, AtomicU64, Ordering},
        Arc, Condvar, Mutex,
    },
    time::Duration,
};
use tauri::{
    ipc::{Channel, Response},
    Manager,
};

extern "C" {
    fn tnr_create() -> *mut c_void;
    fn tnr_call(host: *mut c_void, json: *const c_char) -> *mut c_char;
    fn tnr_next(host: *mut c_void, length: *mut usize) -> *mut u8;
    fn tnr_free(ptr: *mut c_void);
    fn tnr_destroy(host: *mut c_void);
}

// SAFETY: commands are serialized by calls, next() has its own C++ queue lock.
// The Arc keeps the opaque host alive until the stream and commands finish.
pub struct Core {
    ptr: *mut c_void,
    calls: Arc<tokio::sync::Mutex<()>>,
    poll: Mutex<()>,
    stream_worker: Mutex<Option<std::thread::JoinHandle<()>>>,
    pub stopping: AtomicBool,
    generation: AtomicU64,
    acknowledged: Mutex<u64>,
    ack_ready: Condvar,
}
unsafe impl Send for Core {}
unsafe impl Sync for Core {}
impl Core {
    pub fn new() -> Result<Arc<Self>, String> {
        let ptr = unsafe { tnr_create() };
        if ptr.is_null() {
            return Err("Unable to allocate native telemetry host".into());
        }
        Ok(Arc::new(Self {
            ptr,
            calls: Arc::new(tokio::sync::Mutex::new(())),
            poll: Mutex::new(()),
            stream_worker: Mutex::new(None),
            stopping: AtomicBool::new(false),
            generation: AtomicU64::new(0),
            acknowledged: Mutex::new(0),
            ack_ready: Condvar::new(),
        }))
    }
    fn call(&self, command: Value) -> Result<Value, String> {
        let input = CString::new(command.to_string()).map_err(|e| e.to_string())?;
        let ptr = unsafe { tnr_call(self.ptr, input.as_ptr()) };
        if ptr.is_null() {
            return Err("Native command allocation failed".into());
        }
        let text = unsafe { CStr::from_ptr(ptr).to_string_lossy().into_owned() };
        unsafe {
            tnr_free(ptr.cast());
        }
        let result: Value = serde_json::from_str(&text).map_err(|e| e.to_string())?;
        if result["ok"] != true {
            return Err(result["error"]
                .as_str()
                .unwrap_or("Native command failed")
                .into());
        }
        Ok(result["data"].clone())
    }
    pub async fn command(self: &Arc<Self>, command: Value) -> Result<Value, String> {
        if self.stopping.load(Ordering::Acquire) {
            return Err("Application is shutting down".into());
        }
        let guard = self.calls.clone().lock_owned().await;
        let core = self.clone();
        tauri::async_runtime::spawn_blocking(move || {
            let _guard = guard;
            core.call(command)
        })
        .await
        .map_err(|e| e.to_string())?
    }
    fn next(&self) -> Option<Vec<u8>> {
        let _poll = self.poll.lock().unwrap_or_else(|e| e.into_inner());
        let mut len = 0;
        let ptr = unsafe { tnr_next(self.ptr, &mut len) };
        if ptr.is_null() {
            return None;
        }
        let data = unsafe { std::slice::from_raw_parts(ptr, len).to_vec() };
        unsafe {
            tnr_free(ptr.cast());
        }
        Some(data)
    }
    pub async fn shutdown(self: &Arc<Self>) -> Result<Value, String> {
        self.stopping.store(true, Ordering::Release);
        self.ack_ready.notify_all();
        let guard = self.calls.clone().lock_owned().await;
        let core = self.clone();
        tauri::async_runtime::spawn_blocking(move || {
            let _guard = guard;
            core.call(json!({"op":"shutdown"}))
        })
        .await
        .map_err(|e| e.to_string())?
    }
}
impl Drop for Core {
    fn drop(&mut self) {
        unsafe {
            tnr_destroy(self.ptr);
        }
    }
}

pub struct Settings {
    path: PathBuf,
    pub value: Value,
}
impl Settings {
    pub fn load(path: PathBuf) -> Result<Self, String> {
        let value: Value = match fs::read(&path) {
            Ok(bytes) => serde_json::from_slice(&bytes)
                .map_err(|e| format!("Invalid settings at {}: {e}", path.display()))?,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => json!({}),
            Err(e) => return Err(e.to_string()),
        };
        if !value.is_object() {
            return Err("Settings must be a JSON object".into());
        }
        Ok(Self { path, value })
    }
    pub fn get(&self, key: &str, fallback: Value) -> Value {
        let mut value = &self.value;
        for part in key.split('.') {
            match value.get(part) {
                Some(next) => value = next,
                None => return fallback,
            }
        }
        value.clone()
    }
    pub fn set(&mut self, key: &str, value: Value) -> Result<(), String> {
        if key
            .split('.')
            .any(|s| s.is_empty() || matches!(s, "__proto__" | "constructor" | "prototype"))
        {
            return Err("Invalid settings key".into());
        }
        let mut next = self.value.clone();
        let mut cursor = &mut next;
        let mut parts = key.split('.').peekable();
        while let Some(part) = parts.next() {
            if !cursor.is_object() {
                *cursor = json!({});
            }
            if parts.peek().is_none() {
                cursor[part] = value;
                break;
            }
            cursor = cursor
                .as_object_mut()
                .unwrap()
                .entry(part)
                .or_insert(json!({}));
        }
        let parent = self.path.parent().ok_or("Invalid settings path")?;
        fs::create_dir_all(parent).map_err(|e| e.to_string())?;
        let temporary = self.path.with_extension("json.tmp");
        {
            let mut file = fs::File::create(&temporary).map_err(|e| e.to_string())?;
            file.write_all(&serde_json::to_vec_pretty(&next).map_err(|e| e.to_string())?)
                .map_err(|e| e.to_string())?;
            file.sync_all().map_err(|e| e.to_string())?;
        }
        fs::rename(&temporary, &self.path).map_err(|e| e.to_string())?;
        self.value = next;
        Ok(())
    }
    fn public(&self) -> Value {
        let mut value = self.value.clone();
        if let Some(pairing) = value.get_mut("pairing").and_then(Value::as_object_mut) {
            pairing.remove("engineState");
        }
        value
    }
}
pub struct HostState {
    pub core: Arc<Core>,
    pub settings: Arc<Mutex<Settings>>,
    pub pending_files: Mutex<Vec<String>>,
    pub renderer_ready: AtomicBool,
    pub log_dir: PathBuf,
    pub exit_started: AtomicBool,
    pub exit_finished: AtomicBool,
}

#[tauri::command]
pub fn bootstrap(state: tauri::State<'_, HostState>) -> Result<Value, String> {
    let settings = state.settings.lock().map_err(|e| e.to_string())?;
    let platform = if cfg!(target_os = "windows") {
        "win32"
    } else if cfg!(target_os = "macos") {
        "darwin"
    } else {
        "linux"
    };
    Ok(json!({"settings":settings.public(),"platform":platform,"logDir":state.log_dir}))
}
#[tauri::command]
pub fn settings_set(
    state: tauri::State<'_, HostState>,
    key: String,
    value: Value,
) -> Result<(), String> {
    if key == "pairing.engineState" {
        return Err("Pairing credentials are managed by the native host".into());
    }
    state
        .settings
        .lock()
        .map_err(|e| e.to_string())?
        .set(&key, value)
}
#[tauri::command]
pub async fn engine_start(state: tauri::State<'_, HostState>) -> Result<Value, String> {
    let config = {
        let s = state.settings.lock().map_err(|e| e.to_string())?;
        json!({
            "op":"initialize",
            "port":s.get("udp.port",json!(20777)),
            "bindAddress":s.get("udp.bindAddress",json!("0.0.0.0")),
            "format":s.get("udp.protocol",json!("auto")),
            "forwardTargets":if s.get("udp.forwardingEnabled",json!(false)) == true { s.get("udp.forwardTargets",json!([])) } else { json!([]) },
            "teamColorOverrides":s.get("teamColorOverrides",json!({})),
            "loggingEnabled":s.get("logging.enabled",json!(false)),
            "outputDirectory":s.get("logging.directory",json!("")),
            "pairEnabled":s.get("pairing.enabled",json!(false)),
            "pairStateJson":s.get("pairing.engineState",json!("")),
            "enabled":s.get("debug.additionalLogging",json!(false))
        })
    };
    state.core.command(config).await
}
#[tauri::command]
pub async fn engine_command(
    state: tauri::State<'_, HostState>,
    command: Value,
) -> Result<Value, String> {
    // Engine construction/shutdown are lifecycle operations, not renderer commands.
    if matches!(command["op"].as_str(), Some("initialize" | "shutdown")) {
        return Err("Use the application lifecycle commands".into());
    }
    let operation = command["op"].as_str().unwrap_or("unknown").to_owned();
    let result = state.core.command(command).await;
    if let Err(error) = &result {
        append_log(&state.log_dir, &format!("Native {operation}: {error}"));
    }
    result
}

fn encode_event(header: Value, binary: &[u8]) -> Vec<u8> {
    let header = header.to_string().into_bytes();
    let mut result = Vec::with_capacity(4 + header.len() + binary.len());
    result.extend_from_slice(&(header.len() as u32).to_le_bytes());
    result.extend_from_slice(&header);
    result.extend_from_slice(binary);
    result
}
#[tauri::command]
pub fn stream_start(state: tauri::State<'_, HostState>, channel: Channel<Response>) -> u64 {
    let core = state.core.clone();
    let settings = state.settings.clone();
    let log_dir = state.log_dir.clone();
    let mut worker = state
        .core
        .stream_worker
        .lock()
        .unwrap_or_else(|e| e.into_inner());
    let generation = core.generation.fetch_add(1, Ordering::AcqRel) + 1;
    core.ack_ready.notify_all();
    if let Some(previous) = worker.take() {
        let _ = previous.join();
    }
    *core.acknowledged.lock().unwrap_or_else(|e| e.into_inner()) = 0;
    core.ack_ready.notify_all();
    *worker = Some(std::thread::spawn(move || {
        let mut sequence = 0u64;
        while !core.stopping.load(Ordering::Acquire)
            && core.generation.load(Ordering::Acquire) == generation
        {
            let Some(mut packet) = core.next() else {
                continue;
            };
            if packet.len() < 4 {
                continue;
            }
            let length = u32::from_le_bytes(packet[..4].try_into().unwrap()) as usize;
            if length > packet.len() - 4 {
                continue;
            }
            let Ok(mut header) = serde_json::from_slice::<Value>(&packet[4..4 + length]) else {
                continue;
            };
            // Private pairing state never crosses into the webview.
            if header["kind"] == "pair" {
                let payload = &header["payload"];
                let persisted = payload["persisted"].as_str().unwrap_or("").to_owned();
                let public = payload["state"].clone();
                let result = (|| -> Result<(), String> {
                    let mut s = settings.lock().map_err(|e| e.to_string())?;
                    s.set("pairing.engineState", json!(persisted))?;
                    let private: Value =
                        serde_json::from_str(&persisted).map_err(|e| e.to_string())?;
                    s.set("pairing.enabled", private["enabled"].clone())
                })();
                if let Err(error) = result {
                    append_log(&log_dir, &format!("Pairing persistence: {error}"));
                }
                header["payload"] = public;
            } else if header["kind"] == "diagnostic" {
                append_log(
                    &log_dir,
                    header["payload"].as_str().unwrap_or("Native diagnostic"),
                );
            }
            sequence += 1;
            header["sequence"] = json!(sequence);
            header["generation"] = json!(generation);
            packet = encode_event(header, &packet[4 + length..]);
            if core.generation.load(Ordering::Acquire) != generation {
                break;
            }
            if channel.send(Response::new(packet)).is_err() {
                break;
            }
            // One binary envelope in flight. ACK does not wait for history decode.
            let mut ack = core.acknowledged.lock().unwrap_or_else(|e| e.into_inner());
            while *ack < sequence
                && !core.stopping.load(Ordering::Acquire)
                && core.generation.load(Ordering::Acquire) == generation
            {
                ack = core
                    .ack_ready
                    .wait_timeout(ack, Duration::from_millis(100))
                    .unwrap_or_else(|e| e.into_inner())
                    .0;
            }
        }
    }));
    generation
}
#[tauri::command]
pub fn stream_ack(state: tauri::State<'_, HostState>, generation: u64, sequence: u64) {
    if state.core.generation.load(Ordering::Acquire) == generation {
        let mut ack = state
            .core
            .acknowledged
            .lock()
            .unwrap_or_else(|e| e.into_inner());
        *ack = (*ack).max(sequence);
        state.core.ack_ready.notify_all();
    }
}
#[tauri::command]
pub fn renderer_ready(
    app: tauri::AppHandle,
    state: tauri::State<'_, HostState>,
) -> Result<Vec<String>, String> {
    // Take the queue lock before publishing readiness so a second instance
    // cannot race a drain and lose its open-file request.
    let mut files = state.pending_files.lock().map_err(|e| e.to_string())?;
    state.renderer_ready.store(true, Ordering::Release);
    if let Some(window) = app.get_webview_window("main") {
        window.show().map_err(|e| e.to_string())?;
    }
    Ok(std::mem::take(&mut *files))
}
pub fn append_log(dir: &std::path::Path, text: &str) {
    let _ = fs::create_dir_all(dir);
    if let Ok(mut file) = fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(dir.join("main.log"))
    {
        let _ = writeln!(file, "{:?} {text}", std::time::SystemTime::now());
    }
}
#[tauri::command]
pub fn diagnostics(state: tauri::State<'_, HostState>, snapshot: Value) {
    let enabled = state
        .settings
        .lock()
        .map(|s| s.get("debug.memoryLog", json!(false)) == true)
        .unwrap_or(false);
    if enabled {
        append_log(&state.log_dir, &snapshot.to_string());
    }
}
#[tauri::command]
pub async fn update_check(
    state: tauri::State<'_, HostState>,
    version: String,
) -> Result<Value, String> {
    // This experimental frontend has no published update artifacts. Return no
    // update rather than offering the Electron installer as a Tauri upgrade.
    let _ = (&state, version);
    Ok(Value::Null)
}
