//! The Tauri shell around `namforge-core`: starts the device thread, passes its events to the
//! window, and keeps a snapshot so a window that opens (or reloads) later can catch up.

use std::sync::{Arc, Mutex};

use namforge_core::{DeviceHandle, DeviceInfo, Event, Files, Preset, Setting};
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
            Event::Meters { .. } | Event::PresetsChanged | Event::Refused { .. } => {}
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

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
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
                        Event::Meters { .. } => {}
                        other => eprintln!("event: {other:?}"),
                    }
                }
                let _ = handle.emit("device", &event);
            });
            app.manage(App { device, snapshot });
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![snapshot, set_param, preset])
        .run(tauri::generate_context!())
        .expect("error while running NAM Forge");
}
