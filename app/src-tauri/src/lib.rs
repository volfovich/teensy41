//! The Tauri shell around `namforge-core`: starts the device thread, passes its events to the
//! window, and keeps a snapshot so a window that opens (or reloads) later can catch up.

use std::sync::{Arc, Mutex};

use namforge_core::{DeviceHandle, DeviceInfo, Event, FileEntry, Files, Preset, Setting};
use tauri_plugin_dialog::DialogExt;
use serde::Serialize;
use tauri::{Emitter, Manager, State};

/// Everything the window needs to draw itself from scratch.
#[derive(Debug, Clone, Default, Serialize)]
#[serde(rename_all = "camelCase")]
struct Snapshot {
    connected: bool,
    /// What to tell the user while not connected.
    detail: String,
    info: Option<DeviceInfo>,
    settings: Vec<Setting>,
    values: Vec<i16>,
    files: Files,
    presets: Vec<Preset>,
    models: Vec<FileEntry>,
    irs: Vec<FileEntry>,
}

impl Snapshot {
    fn apply(&mut self, event: &Event) {
        match event {
            Event::Searching { detail } => {
                self.connected = false;
                self.detail = detail.clone();
            }
            Event::Connected { info } => {
                self.connected = true;
                self.detail.clear();
                self.info = Some(info.clone());
            }
            Event::Schema { settings } => self.settings = settings.clone(),
            Event::State { values, files } => {
                self.values = values.clone();
                self.files = files.clone();
            }
            Event::Param { id, value } => {
                if let Some(v) = self.values.get_mut(*id as usize) {
                    *v = *value as i16;
                }
            }
            Event::Files { files } => self.files = files.clone(),
            Event::Presets { presets } => self.presets = presets.clone(),
            Event::Models { files } => self.models = files.clone(),
            Event::Irs { files } => self.irs = files.clone(),
            Event::Meters { .. } | Event::Tuner { .. } | Event::PresetsChanged | Event::Refused { .. } | Event::Transfer { .. } => {}
            Event::Disconnected { reason } => {
                self.connected = false;
                self.detail = reason.clone();
            }
        }
    }
}

struct App {
    device: DeviceHandle,
    snapshot: Arc<Mutex<Snapshot>>,
}

#[tauri::command]
fn snapshot(app: State<'_, App>) -> Snapshot {
    app.snapshot.lock().unwrap().clone()
}

/// The device's answer comes back as a `param` event with the value it applied.
#[tauri::command]
fn set_param(app: State<'_, App>, id: u16, value: i32) {
    app.device.set_param(id, value);
}

/// One of "recall", "save", "new", "rename", "delete"; `name` for all but "save". The result
/// arrives as events (`files`, `presets`, or `refused` with the unit's reason).
#[tauri::command]
fn preset(app: State<'_, App>, action: String, name: Option<String>) -> Result<(), String> {
    let name = name.unwrap_or_default();
    match action.as_str() {
        "recall" => app.device.recall_preset(&name),
        "save" => app.device.save_preset(),
        "new" => app.device.new_preset(&name),
        "rename" => app.device.rename_preset(&name),
        "delete" => app.device.delete_preset(&name),
        other => return Err(format!("unknown preset action {other}")),
    }
    Ok(())
}

/// Loads a model ("model") or an IR ("ir") that is on the card, or deletes a file ("delete").
#[tauri::command]
fn card_file(app: State<'_, App>, action: String, path: String) -> Result<(), String> {
    match action.as_str() {
        "model" => app.device.load_model(&path),
        "ir" => app.device.load_ir(&path),
        "delete" => app.device.delete_file(&path),
        other => return Err(format!("unknown file action {other}")),
    }
    Ok(())
}

/// Opens or closes the tuner; the guitar is muted while it is open.
#[tauri::command]
fn tuner(app: State<'_, App>, open: bool) {
    app.device.tuner(open);
}

/// Sends .nam and .wav files from the Mac to the card, one after the other. Progress arrives
/// as `transfer` events.
#[tauri::command]
fn send_files(app: State<'_, App>, paths: Vec<String>) {
    for path in paths {
        app.device.send_file(path);
    }
}

/// Opens the file dialog and sends what the user picks.
#[tauri::command]
async fn pick_files(handle: tauri::AppHandle) -> Result<(), String> {
    let picked = handle.dialog().file().add_filter("Amp models and IRs", &["nam", "wav"]).blocking_pick_files();
    let app = handle.state::<App>();
    for file in picked.unwrap_or_default() {
        if let Ok(path) = file.into_path() {
            app.device.send_file(path);
        }
    }
    Ok(())
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .setup(|app| {
            let snapshot = Arc::new(Mutex::new(Snapshot { detail: "Looking for the unit…".into(), ..Default::default() }));
            let handle = app.handle().clone();
            let shared = snapshot.clone();
            let device = namforge_core::spawn(move |event| {
                shared.lock().unwrap().apply(&event);
                if std::env::var_os("NAMFORGE_LOG").is_some() {
                    // For checking the link from a terminal: one line per event.
                    match &event {
                        Event::State { values, files } => {
                            eprintln!("event: state ({} values, preset {:?})", values.len(), files.preset)
                        }
                        Event::Schema { settings } => eprintln!("event: schema ({} settings)", settings.len()),
                        Event::Meters { .. } | Event::Tuner { .. } => {}
                        Event::Models { files } => eprintln!("event: models ({})", files.len()),
                        Event::Irs { files } => eprintln!("event: irs ({})", files.len()),
                        other => eprintln!("event: {other:?}"),
                    }
                }
                let _ = handle.emit("device", &event);
            });
            app.manage(App { device, snapshot });
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![snapshot, set_param, preset, card_file, send_files, pick_files, tuner])
        .run(tauri::generate_context!())
        .expect("error while running NAM Forge");
}
