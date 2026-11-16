//! The Tauri shell around `namforge-core`: starts the device thread, passes its events to the
//! window, and keeps a snapshot so a window that opens (or reloads) later can catch up.

use std::sync::{Arc, Mutex};

use namforge_core::{CardSpace, DeviceHandle, DeviceInfo, Event, FileEntry, Files, IrInfo, ModelInfo, Preset, Setting, TrackRow, TrackState};
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
    model_info: Option<ModelInfo>,
    ir_info: Option<IrInfo>,
    card: Option<CardSpace>,
    /// The backing-track player's state; `None`: the firmware has no player (or nothing is
    /// known yet), and the window hides everything about tracks.
    track: Option<TrackState>,
    /// The folder below `/TRACKS` that the Tracks tab shows.
    track_list: TrackList,
}

#[derive(Debug, Clone, Default, Serialize)]
#[serde(rename_all = "camelCase")]
struct TrackList {
    folder: String,
    rows: Vec<TrackRow>,
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
                // Another unit, or other firmware: what it has of tracks is told anew.
                self.track = None;
                self.track_list = TrackList::default();
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
            Event::ModelInfo { info } => self.model_info = info.clone(),
            Event::IrInfo { info } => self.ir_info = info.clone(),
            Event::Card { space } => self.card = *space,
            Event::Track { state } => self.track = state.clone(),
            Event::TrackList { folder, rows } => self.track_list = TrackList { folder: folder.clone(), rows: rows.clone() },
            Event::TrackFound { .. } => {}
            Event::Meters { .. } | Event::Tuner { .. } | Event::Job { .. } | Event::PresetsChanged | Event::Refused { .. } | Event::Transfer { .. } => {}
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

/// Copies the unit's card into `~/Music/NAM Forge/Backups/<name>` and returns that folder.
/// Progress and the result arrive as `job` events.
#[tauri::command]
fn backup(handle: tauri::AppHandle, app: State<'_, App>, name: String) -> Result<String, String> {
    if name.is_empty() || name.starts_with('.') || name.contains(['/', ':']) {
        return Err("That name cannot be a folder's name.".into());
    }
    let dest = handle.path().audio_dir().map_err(|e| e.to_string())?.join("NAM Forge").join("Backups").join(name);
    std::fs::create_dir_all(&dest).map_err(|e| format!("Cannot create {}: {e}", dest.display()))?;
    app.device.backup(&dest);
    Ok(dest.to_string_lossy().into_owned())
}

/// Lets the user choose a backup folder; returns it with the card paths its files would go to
/// (empty if it is not a backup). Nothing is sent yet.
#[tauri::command]
async fn pick_backup(handle: tauri::AppHandle) -> Option<(String, Vec<String>)> {
    let start = handle.path().audio_dir().ok().map(|d| d.join("NAM Forge").join("Backups"));
    let mut dialog = handle.dialog().file().set_title("Choose a backup folder");
    if let Some(dir) = start.filter(|d| d.is_dir()) {
        dialog = dialog.set_directory(dir);
    }
    let folder = dialog.blocking_pick_folder()?.into_path().ok()?;
    let files = namforge_core::backup::restorable(&folder).into_iter().map(|(_, card)| card).collect();
    Some((folder.to_string_lossy().into_owned(), files))
}

/// Sends a backup folder's files back to the card (the answer to `pick_backup`).
#[tauri::command]
fn restore(app: State<'_, App>, folder: String) {
    app.device.restore(folder);
}

/// A line from the window for the terminal (with NAMFORGE_LOG=1): script errors, and what the
/// window shows at moments worth checking.
#[tauri::command]
fn ui_log(text: String) {
    if std::env::var_os("NAMFORGE_LOG").is_some() {
        eprintln!("ui: {text}");
    }
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

/// Opens the file dialog and returns what the user picked. The window then sends the files
/// with `send_files`, after asking about any that would replace a file on the card.
#[tauri::command]
async fn pick_files(handle: tauri::AppHandle) -> Vec<String> {
    let picked = handle.dialog().file().add_filter("Amp models and IRs", &["nam", "wav"]).blocking_pick_files();
    picked
        .unwrap_or_default()
        .into_iter()
        .filter_map(|file| file.into_path().ok())
        .map(|path| path.to_string_lossy().into_owned())
        .collect()
}

/// Asks for the rows of a folder below `/TRACKS` ("" is `/TRACKS` itself). They arrive as a
/// `trackList` event, and again whenever the unit has read its list anew.
#[tauri::command]
fn track_list(app: State<'_, App>, folder: String) {
    app.device.track_list(&folder);
}

/// Finds tracks in all folders by words; the hits arrive as a `trackFound` event. "" ends the search.
#[tauri::command]
fn track_find(app: State<'_, App>, text: String) {
    app.device.track_find(&text);
}

/// Opens a track by its path below `/TRACKS` ("Rock/Song.mp3"); "" closes the open one. The
/// state arrives as a `track` event.
#[tauri::command]
fn track_open(app: State<'_, App>, path: String) {
    app.device.track_open(&path);
}

/// One of the player's commands by its number (PROTOCOL.md 4.8), with up to three values.
/// The state after it arrives as a `track` event, a refusal as `refused`.
#[tauri::command]
fn track_command(app: State<'_, App>, command: u8, a: Option<f32>, b: Option<f32>, c: Option<f32>) {
    app.device.track_command(command, a.unwrap_or(0.0), b.unwrap_or(0.0), c.unwrap_or(0.0));
}

/// A change to the tracks on the card: "rename" (`path` to `to`), "new-folder",
/// "delete-folder" (with everything in it) or "delete" (one track). Paths are below `/TRACKS`.
#[tauri::command]
fn track_file(app: State<'_, App>, action: String, path: String, to: Option<String>) -> Result<(), String> {
    match action.as_str() {
        "rename" => app.device.track_rename(&path, &to.unwrap_or_default()),
        "new-folder" => app.device.track_folder_new(&path),
        "delete-folder" => app.device.track_folder_delete(&path),
        "delete" => app.device.track_delete(&path),
        other => return Err(format!("unknown track file action {other}")),
    }
    Ok(())
}

/// Sends .wav and .mp3 files from the Mac into a folder below `/TRACKS`, one after the
/// other. Progress arrives as `transfer` events, with the file's number in the batch.
#[tauri::command]
fn send_tracks(app: State<'_, App>, paths: Vec<String>, folder: String) {
    app.device.send_tracks(paths.into_iter().map(Into::into).collect(), &folder);
}

/// Opens the file dialog for backing tracks and returns what the user picked.
#[tauri::command]
async fn pick_tracks(handle: tauri::AppHandle) -> Vec<String> {
    let picked = handle.dialog().file().set_title("Send backing tracks to the unit").add_filter("Backing tracks", &["wav", "mp3"]).blocking_pick_files();
    picked
        .unwrap_or_default()
        .into_iter()
        .filter_map(|file| file.into_path().ok())
        .map(|path| path.to_string_lossy().into_owned())
        .collect()
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
                        // The player's state comes four times a second while a track plays.
                        Event::Meters { .. } | Event::Tuner { .. } | Event::Track { state: Some(_) } => {}
                        Event::TrackList { folder, rows } => eprintln!("event: track list /TRACKS/{folder} ({} rows)", rows.len()),
                        Event::TrackFound { text, hits, .. } => eprintln!("event: tracks found for {text:?} ({})", hits.len()),
                        Event::Models { files } => eprintln!("event: models ({})", files.len()),
                        Event::Irs { files } => eprintln!("event: irs ({})", files.len()),
                        other => eprintln!("event: {other:?}"),
                    }
                }
                let _ = handle.emit("device", &event);
            });
            app.manage(App { device, snapshot });
            // For checking the tuner from a terminal: NAMFORGE_TUNER=1 opens it after 3 s.
            if std::env::var_os("NAMFORGE_TUNER").is_some() {
                let h = app.handle().clone();
                std::thread::spawn(move || {
                    std::thread::sleep(std::time::Duration::from_secs(3));
                    h.state::<App>().device.tuner(true);
                });
            }
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![snapshot, set_param, preset, card_file, send_files, pick_files, tuner, ui_log, backup, pick_backup, restore, track_list, track_find, track_open, track_command, track_file, send_tracks, pick_tracks])
        .run(tauri::generate_context!())
        .expect("error while running NAM Forge");
}
