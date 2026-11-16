//! The unit on a background thread: find it on USB, open a session, keep it alive, apply the
//! UI's commands, and start over when it is unplugged.

use std::collections::HashMap;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::mpsc::{self, Receiver, Sender, TryRecvError};
use std::thread;
use std::time::{Duration, Instant};

use serialport::{SerialPort, SerialPortType};

use namforge_link::{msg, track_cmd, ErrorCode};

use crate::session::{Session, SessionError};
use crate::files;
use crate::types::{Event, TrackState};

/// The Teensy's USB identity with the firmware's "MIDI + Serial" USB type.
const USB_VID: u16 = 0x16C0;
const USB_PID: u16 = 0x0489;

/// The serial port's read timeout: how long one wait for events lasts.
const READ_TIMEOUT: Duration = Duration::from_millis(10);
/// The device ends a session after 5 s without a frame; a full state every 2 s keeps it open
/// and corrects anything an event missed.
const KEEPALIVE: Duration = Duration::from_millis(2000);
/// Meter frames per second asked of the device.
const METER_RATE: u16 = 30;
const RETRY: Duration = Duration::from_millis(1000);

enum Command {
    SetParam { id: u16, value: i32 },
    /// A preset request (its message type) with its name, if it takes one.
    Preset { msg_type: u8, name: Option<String> },
    /// `MODEL_LOAD`, `IR_LOAD` or `FILE_DELETE` with a path on the card.
    Path { msg_type: u8, path: String },
    /// Send this file from the Mac to the card.
    Send { local: PathBuf },
    Tuner { open: bool },
    /// Copy the card into this folder on the Mac.
    Backup { dest: PathBuf },
    /// Send this backup folder's files back to the card.
    Restore { src: PathBuf },
    /// Show this folder below `/TRACKS` ("" is `/TRACKS` itself).
    TrackList { folder: String },
    /// Find tracks by words; "" ends the search.
    TrackFind { text: String },
    /// Open a track by its path below `/TRACKS`; "" closes the open one.
    TrackOpen { path: String },
    /// One of the player's commands (`track_cmd`).
    Track { command: u8, a: f32, b: f32, c: f32 },
    /// The four below take paths below `/TRACKS` ("Rock/Song.mp3").
    TrackRename { from: String, to: String },
    TrackFolderNew { path: String },
    TrackFolderDelete { path: String },
    TrackDelete { path: String },
    /// Send these files from the Mac into a folder below `/TRACKS`.
    SendTracks { files: Vec<PathBuf>, folder: String },
}

/// The UI's handle on the device thread. Dropping it stops the thread.
pub struct DeviceHandle {
    commands: Sender<Command>,
}

impl DeviceHandle {
    /// Sets a value. The device's answer comes back as an [`Event::Param`], so the UI always
    /// shows what the device really applied. While a control is dragged only the newest value
    /// per setting is sent.
    pub fn set_param(&self, id: u16, value: i32) {
        let _ = self.commands.send(Command::SetParam { id, value });
    }

    /// Recalls the preset with this name ("03 Lead").
    pub fn recall_preset(&self, name: &str) {
        self.preset(msg::PRESET_RECALL, Some(name));
    }
    /// Writes the current sound over the recalled preset.
    pub fn save_preset(&self) {
        self.preset(msg::PRESET_SAVE, None);
    }
    /// Saves the current sound as a new preset; the unit gives it the lowest free number.
    pub fn new_preset(&self, name: &str) {
        self.preset(msg::PRESET_NEW, Some(name));
    }
    /// Renames the recalled preset (its number stays).
    pub fn rename_preset(&self, name: &str) {
        self.preset(msg::PRESET_RENAME, Some(name));
    }
    pub fn delete_preset(&self, name: &str) {
        self.preset(msg::PRESET_DELETE, Some(name));
    }
    /// Loads an amp model from the card ("/AMP/x.nam").
    pub fn load_model(&self, path: &str) {
        let _ = self.commands.send(Command::Path { msg_type: msg::MODEL_LOAD, path: path.into() });
    }
    /// Loads an IR from the card.
    pub fn load_ir(&self, path: &str) {
        let _ = self.commands.send(Command::Path { msg_type: msg::IR_LOAD, path: path.into() });
    }
    /// Deletes a model or IR file from the card.
    pub fn delete_file(&self, path: &str) {
        let _ = self.commands.send(Command::Path { msg_type: msg::FILE_DELETE, path: path.into() });
    }
    /// Sends a .nam or .wav file to the card (to /AMP or /IR). Progress and the result arrive
    /// as [`Event::Transfer`].
    pub fn send_file(&self, local: impl Into<PathBuf>) {
        let _ = self.commands.send(Command::Send { local: local.into() });
    }
    /// Opens or closes the tuner. Readings arrive as [`Event::Tuner`].
    pub fn tuner(&self, open: bool) {
        let _ = self.commands.send(Command::Tuner { open });
    }
    /// Copies the card's models, IRs, presets and drum songs into a folder on the Mac. Progress
    /// and the result arrive as [`Event::Job`].
    pub fn backup(&self, dest: impl Into<PathBuf>) {
        let _ = self.commands.send(Command::Backup { dest: dest.into() });
    }
    /// Sends a backup folder's files back to the card, replacing files of the same name.
    pub fn restore(&self, src: impl Into<PathBuf>) {
        let _ = self.commands.send(Command::Restore { src: src.into() });
    }
    /// Asks for the rows of a folder below `/TRACKS` ("" is `/TRACKS` itself, "Rock/Live" a
    /// folder in it). They arrive as [`Event::TrackList`], and again whenever the unit has
    /// read its list anew: the folder asked for last is the one the window shows.
    pub fn track_list(&self, folder: &str) {
        let _ = self.commands.send(Command::TrackList { folder: folder.into() });
    }
    /// Finds tracks in all folders by words; the hits arrive as [`Event::TrackFound`], and
    /// again when the unit's list changes. "" ends the search.
    pub fn track_find(&self, text: &str) {
        let _ = self.commands.send(Command::TrackFind { text: text.into() });
    }
    /// Opens a track by its path below `/TRACKS`; "" closes the open one. The state arrives
    /// as [`Event::Track`].
    pub fn track_open(&self, path: &str) {
        let _ = self.commands.send(Command::TrackOpen { path: path.into() });
    }
    /// One of the player's commands (`namforge_link::track_cmd`, PROTOCOL.md 4.8); what a
    /// command does not use is 0. The state after it arrives as [`Event::Track`]. While a
    /// seek bar or the tempo is dragged only the newest value is sent.
    pub fn track_command(&self, command: u8, a: f32, b: f32, c: f32) {
        let _ = self.commands.send(Command::Track { command, a, b, c });
    }
    /// Gives a track or a folder a new name or place; both are paths below `/TRACKS`.
    pub fn track_rename(&self, from: &str, to: &str) {
        let _ = self.commands.send(Command::TrackRename { from: from.into(), to: to.into() });
    }
    /// Makes a folder below `/TRACKS` ("Rock/Live").
    pub fn track_folder_new(&self, path: &str) {
        let _ = self.commands.send(Command::TrackFolderNew { path: path.into() });
    }
    /// Removes a folder below `/TRACKS` with everything in it.
    pub fn track_folder_delete(&self, path: &str) {
        let _ = self.commands.send(Command::TrackFolderDelete { path: path.into() });
    }
    /// Deletes a track (its path below `/TRACKS`) from the card.
    pub fn track_delete(&self, path: &str) {
        let _ = self.commands.send(Command::TrackDelete { path: path.into() });
    }
    /// Sends .wav and .mp3 files into a folder below `/TRACKS` ("" is `/TRACKS` itself), one
    /// after the other. Progress and each file's result arrive as [`Event::Transfer`].
    pub fn send_tracks(&self, files: Vec<PathBuf>, folder: &str) {
        let _ = self.commands.send(Command::SendTracks { files, folder: folder.into() });
    }
    fn preset(&self, msg_type: u8, name: Option<&str>) {
        let _ = self.commands.send(Command::Preset { msg_type, name: name.map(str::to_string) });
    }
}

/// Starts the device thread. `emit` is called from that thread for every [`Event`].
pub fn spawn(emit: impl Fn(Event) + Send + 'static) -> DeviceHandle {
    let (tx, rx) = mpsc::channel();
    thread::Builder::new()
        .name("namforge-device".into())
        .spawn(move || run(rx, emit))
        .expect("start the device thread");
    DeviceHandle { commands: tx }
}

/// The unit's serial port, if it is plugged in. On macOS each port appears twice
/// (/dev/tty.* and /dev/cu.*); the cu one is the one to open.
pub fn find_port() -> Option<String> {
    let ports = serialport::available_ports().ok()?;
    let mut matches: Vec<String> = ports
        .into_iter()
        .filter(|p| matches!(&p.port_type, SerialPortType::UsbPort(u) if u.vid == USB_VID && u.pid == USB_PID))
        .map(|p| p.port_name)
        .collect();
    matches.sort_by_key(|name| !name.contains("/cu."));
    matches.into_iter().next()
}

fn open(port: &str) -> Result<Box<dyn SerialPort>, String> {
    serialport::new(port, 115_200).timeout(READ_TIMEOUT).open().map_err(|e| e.to_string())
}

enum End {
    /// The UI is gone: stop.
    Quit,
    Lost(String),
}

fn run(commands: Receiver<Command>, emit: impl Fn(Event)) {
    let mut last_detail = String::new();
    loop {
        // Commands sent while nothing is connected are dropped.
        loop {
            match commands.try_recv() {
                Ok(_) => {}
                Err(TryRecvError::Empty) => break,
                Err(TryRecvError::Disconnected) => return,
            }
        }
        let attempt = match find_port() {
            None => Err("No unit found. Plug it in with a USB cable.".to_string()),
            Some(port) => open(&port)
                .map_err(|e| format!("Found the unit on {port} but could not open it ({e}). Close any serial monitor."))
                .map(|io| (port, io)),
        };
        match attempt {
            Err(detail) => {
                if detail != last_detail {
                    emit(Event::Searching { detail: detail.clone() });
                    last_detail = detail;
                }
            }
            Ok((port, io)) => {
                last_detail.clear();
                match serve(Session::new(io), &port, &commands, &emit) {
                    End::Quit => return,
                    End::Lost(reason) => emit(Event::Disconnected { reason }),
                }
            }
        }
        thread::sleep(RETRY);
    }
}

fn serve(mut s: Session<Box<dyn SerialPort>>, port: &str, commands: &Receiver<Command>, emit: &impl Fn(Event)) -> End {
    let lost = |e: SessionError| End::Lost(e.to_string());

    let info = match s.hello(port) {
        Ok(info) => info,
        Err(e) => return lost(e),
    };
    let max_path = info.max_path as usize;
    emit(Event::Connected { info });
    match s.schema() {
        Ok(settings) => emit(Event::Schema { settings }),
        Err(e) => return lost(e),
    }

    // Firmware before meters refuses this; the app then simply shows none.
    match s.subscribe(METER_RATE) {
        Ok(_) | Err(SessionError::Device { .. }) => {}
        Err(e) => return lost(e),
    }

    // Firmware before presets refuses this; the list then stays empty.
    match s.presets() {
        Ok(presets) => emit(Event::Presets { presets }),
        Err(SessionError::Device { .. }) => {}
        Err(e) => return lost(e),
    }

    if let Err(e) = file_lists(&mut s, emit) {
        return lost(e);
    }

    // Firmware before the backing-track player does not know this request: the UI then hides
    // everything about tracks. With a player, the first state makes the top folder be fetched.
    let mut tracks = TrackView::default();
    match s.track_state() {
        Ok(state) => {
            if let Some(state) = &state {
                tracks.supported = true;
                tracks.seen(state);
            }
            emit(Event::Track { state });
        }
        Err(SessionError::Device { .. }) => emit(Event::Track { state: None }),
        Err(e) => return lost(e),
    }

    let mut last_state = Instant::now() - KEEPALIVE;
    let mut card: Option<bool> = None;
    // The model and IR whose details the UI has, and the amp's calibration switch, which changes them.
    let mut detailed = Detailed::default();
    loop {
        if last_state.elapsed() >= KEEPALIVE {
            match s.state() {
                Ok((values, files)) => {
                    // Every time: the amp's calibration switch changes the model's gains.
                    detailed.model = None;
                    if let Err(e) = details(&mut s, &files, &mut detailed, emit) {
                        return lost(e);
                    }
                    emit(Event::State { values, files })
                }
                Err(e) => return lost(e),
            }
            if tracks.supported {
                match s.track_state() {
                    Ok(Some(state)) => {
                        tracks.seen(&state);
                        // While the unit reads its list the folder shown fills up.
                        tracks.refresh |= state.reading;
                        emit(Event::Track { state: Some(state) });
                    }
                    Ok(None) | Err(SessionError::Device { .. }) => {}
                    Err(e) => return lost(e),
                }
            }
            last_state = Instant::now();
        }

        // The UI's commands, newest value per setting, in the order the settings were first
        // touched.
        let mut order: Vec<u16> = Vec::new();
        let mut newest: HashMap<u16, i32> = HashMap::new();
        let mut presets: Vec<(u8, Option<String>)> = Vec::new();
        let mut others: Vec<Command> = Vec::new();
        loop {
            match commands.try_recv() {
                Ok(Command::SetParam { id, value }) => {
                    if newest.insert(id, value).is_none() {
                        order.push(id);
                    }
                }
                Ok(Command::Preset { msg_type, name }) => presets.push((msg_type, name)),
                // A seek bar or the tempo being dragged: the newest value takes the place of
                // one that is still waiting.
                Ok(Command::Track { command, a, b, c }) if matches!(command, track_cmd::SEEK | track_cmd::TEMPO) => {
                    let waiting = others.iter_mut().find_map(|o| match o {
                        Command::Track { command: k, a, .. } if *k == command => Some(a),
                        _ => None,
                    });
                    match waiting {
                        Some(old) => *old = a,
                        None => others.push(Command::Track { command, a, b, c }),
                    }
                }
                Ok(other) => others.push(other),
                Err(TryRecvError::Empty) => break,
                Err(TryRecvError::Disconnected) => return End::Quit,
            }
        }
        for id in order {
            match s.set_param(id, newest[&id]) {
                Ok(value) => emit(Event::Param { id, value }),
                // A refusal (a setting this firmware does not have) is not a lost connection.
                Err(SessionError::Device { .. }) => {}
                Err(e) => return lost(e),
            }
        }

        for (msg_type, name) in presets {
            match s.preset(msg_type, name.as_deref()) {
                Ok(()) => last_state = Instant::now() - KEEPALIVE, // the full state, right away
                Err(SessionError::Device { message, .. }) => emit(Event::Refused { message }),
                Err(e) => return lost(e),
            }
        }

        for command in others {
            let command = match track_job(&mut s, command, &mut tracks, emit) {
                Ok(Some(other)) => other,
                Ok(None) => continue,
                Err(e) => return lost(e),
            };
            let (result, lists) = match command {
                Command::Path { msg_type, path } => (s.with_path(msg_type, &path), msg_type == msg::FILE_DELETE),
                Command::Send { local } => (send_file(&mut s, &local, max_path, emit), true),
                Command::Tuner { open } => (s.tuner_open(open), false),
                Command::Backup { dest } => (crate::backup::backup(&mut s, &dest, emit), false),
                Command::Restore { src } => (crate::backup::restore(&mut s, &src, emit), true),
                _ => continue,
            };
            match result {
                Ok(()) => last_state = Instant::now() - KEEPALIVE,
                Err(SessionError::Device { message, .. }) => emit(Event::Refused { message }),
                Err(e) => return lost(e),
            }
            if lists {
                if let Err(e) = file_lists(&mut s, emit) {
                    return lost(e);
                }
            }
        }

        // Waits up to the port's read timeout, so this loop does not spin.
        match s.events() {
            Ok(events) => {
                for event in events {
                    // A card put in or taken out: its files are different ones.
                    if let Event::Files { files } = &event {
                        if let Err(e) = details(&mut s, files, &mut detailed, emit) {
                            return lost(e);
                        }
                        if card.replace(files.card) == Some(!files.card) {
                            if let Err(e) = file_lists(&mut s, emit) {
                                return lost(e);
                            }
                        }
                    }
                    if let Event::Track { state: Some(state) } = &event {
                        tracks.seen(state);
                    }
                    if event == Event::PresetsChanged {
                        match s.presets() {
                            Ok(presets) => emit(Event::Presets { presets }),
                            Err(SessionError::Device { .. }) => {}
                            Err(e) => return lost(e),
                        }
                    } else {
                        emit(event);
                    }
                }
            }
            Err(e) => return lost(e),
        }

        // The unit read its list of tracks anew (or has just finished): what the window shows
        // of it is fetched again.
        if std::mem::take(&mut tracks.refresh) {
            if let Err(e) = track_lists(&mut s, &mut tracks, emit) {
                return lost(e);
            }
        }
    }
}

/// What the window shows of the unit's backing tracks, and what is known of the unit's list.
#[derive(Default)]
struct TrackView {
    /// The firmware has the player.
    supported: bool,
    /// The folder below `/TRACKS` the window shows, and the search it shows ("" for none).
    folder: String,
    find: String,
    /// The state's "list number" and "being read" as last seen.
    list_number: Option<u32>,
    reading: bool,
    /// The folder and the search are to be fetched again.
    refresh: bool,
}

impl TrackView {
    /// Looks at a state from the unit: a new list number means its list is being read anew,
    /// and the end of "being read" that it is complete. Both times the app asks again.
    fn seen(&mut self, state: &TrackState) {
        if self.list_number != Some(state.list_number) || (self.reading && !state.reading) {
            self.refresh = true;
        }
        self.list_number = Some(state.list_number);
        self.reading = state.reading;
    }
}

/// Fetches the folder the window shows, and its search if it has one.
fn track_lists<T: Read + Write>(s: &mut Session<T>, view: &mut TrackView, emit: &impl Fn(Event)) -> Result<(), SessionError> {
    let mut rows = s.track_list(&view.folder);
    // The folder is gone (deleted or renamed): the top one is shown. While the unit is still
    // reading, "not found" may only mean "not read yet", and the folder is shown empty.
    if matches!(rows, Err(SessionError::Device { code: ErrorCode::NotFound, .. })) && !view.reading && !view.folder.is_empty() {
        view.folder.clear();
        rows = s.track_list("");
    }
    match rows {
        Ok(rows) => emit(Event::TrackList { folder: view.folder.clone(), rows }),
        Err(SessionError::Device { .. }) => emit(Event::TrackList { folder: view.folder.clone(), rows: Vec::new() }),
        Err(e) => return Err(e),
    }
    track_found(s, view, emit)
}

fn track_found<T: Read + Write>(s: &mut Session<T>, view: &TrackView, emit: &impl Fn(Event)) -> Result<(), SessionError> {
    if view.find.is_empty() {
        return Ok(());
    }
    match s.track_find(&view.find) {
        Ok((hits, more)) => emit(Event::TrackFound { text: view.find.clone(), hits, more }),
        Err(SessionError::Device { message, .. }) => emit(Event::Refused { message }),
        Err(e) => return Err(e),
    }
    Ok(())
}

/// Carries out a command about the backing tracks; any other command is handed back. What
/// the unit refuses is reported to the window; only a lost connection is an error.
fn track_job<T: Read + Write>(s: &mut Session<T>, command: Command, view: &mut TrackView, emit: &impl Fn(Event)) -> Result<Option<Command>, SessionError> {
    let refused = |result: Result<(), SessionError>| match result {
        Err(SessionError::Device { message, .. }) => {
            emit(Event::Refused { message });
            Ok(())
        }
        other => other,
    };
    // A change on the card: the unit reads its list again and says so (`TrackView::seen`);
    // here only the card's free space is asked for.
    let card = |s: &mut Session<T>| -> Result<(), SessionError> {
        emit(Event::Card { space: s.card_info()? });
        Ok(())
    };
    let state = |view: &mut TrackView, result: Result<TrackState, SessionError>| {
        refused(result.map(|state| {
            view.seen(&state);
            emit(Event::Track { state: Some(state) });
        }))
    };
    match command {
        Command::TrackList { folder } => {
            view.folder = folder;
            match s.track_list(&view.folder) {
                Ok(rows) => emit(Event::TrackList { folder: view.folder.clone(), rows }),
                // Not read yet, or gone: the next reading of the list settles it.
                Err(SessionError::Device { code: ErrorCode::NotFound, .. }) if view.reading => emit(Event::TrackList { folder: view.folder.clone(), rows: Vec::new() }),
                Err(SessionError::Device { message, .. }) => {
                    emit(Event::Refused { message });
                    view.refresh = true;
                }
                Err(e) => return Err(e),
            }
        }
        Command::TrackFind { text } => {
            view.find = text.trim().to_string();
            track_found(s, view, emit)?;
        }
        Command::TrackOpen { path } => state(view, s.track_open(&path))?,
        Command::Track { command, a, b, c } => state(view, s.track_command(command, a, b, c))?,
        Command::TrackRename { from, to } => match (files::track_card_path(&from), files::track_card_path(&to)) {
            (Ok(from), Ok(to)) => refused(s.rename(&from, &to))?,
            (Err(why), _) | (_, Err(why)) => emit(Event::Refused { message: why }),
        },
        Command::TrackFolderNew { path } => match files::track_card_path(&path) {
            Ok(path) => refused(s.folder_new(&path))?,
            Err(why) => emit(Event::Refused { message: why }),
        },
        Command::TrackFolderDelete { path } => match files::track_card_path(&path) {
            Ok(path) => {
                refused(s.folder_delete(&path).map(|_| ()))?;
                card(s)?;
            }
            Err(why) => emit(Event::Refused { message: why }),
        },
        Command::TrackDelete { path } => match files::track_card_path(&path) {
            Ok(path) => {
                refused(s.with_path(msg::FILE_DELETE, &path))?;
                card(s)?;
            }
            Err(why) => emit(Event::Refused { message: why }),
        },
        Command::SendTracks { files, folder } => {
            send_tracks(s, &files, &folder, view, emit)?;
            card(s)?;
        }
        other => return Ok(Some(other)),
    }
    Ok(None)
}

/// Sends backing tracks into a folder below `/TRACKS`, one after the other. Each file's
/// progress and the batch's are reported as transfers; a file that cannot be sent is reported
/// and the next one follows. The link carries about 130 KB a second, so a batch takes minutes:
/// the frames of the upload keep the session open, and meters and the player's state that
/// arrive meanwhile are passed on after every piece.
fn send_tracks<T: Read + Write>(s: &mut Session<T>, locals: &[PathBuf], folder: &str, view: &mut TrackView, emit: &impl Fn(Event)) -> Result<(), SessionError> {
    let sizes: Vec<u64> = locals.iter().map(|p| std::fs::metadata(p).map(|m| m.len()).unwrap_or(0)).collect();
    let all_total: u64 = sizes.iter().sum();
    let count = locals.len() as u32;
    let mut before = 0u64;
    for (i, (local, &size)) in locals.iter().zip(&sizes).enumerate() {
        let name = local.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
        let total = size.min(u32::MAX as u64) as u32;
        let transfer = |sent: u32, done: bool, error: &str| {
            // A file that failed counts as passed, so the batch's bar goes on.
            let all_sent = before + if error.is_empty() { sent as u64 } else { size };
            emit(Event::Transfer { name: name.clone(), sent, total, done, error: error.to_string(), index: i as u32 + 1, count, all_sent, all_total })
        };
        let data = match files::check_track(local, folder, size) {
            Ok(checked) => std::fs::read(local).map(|data| (checked, data)).map_err(|e| format!("The file could not be read ({e}).")),
            Err(why) => Err(why),
        };
        match data {
            Err(why) => transfer(0, true, &why),
            Ok((checked, data)) => {
                transfer(0, false, "");
                let sent = s.put_file_again(&checked.device_path, &data, |s, sent| {
                    for event in s.live_events() {
                        if let Event::Track { state: Some(state) } = &event {
                            view.seen(state);
                        }
                        emit(event);
                    }
                    transfer(sent, false, "");
                });
                match sent {
                    Ok(()) => transfer(total, true, ""),
                    Err(SessionError::Device { message, .. }) => transfer(0, true, &message),
                    Err(e) => return Err(e),
                }
            }
        }
        before += size;
    }
    Ok(())
}

/// Fetches the details of the loaded model and IR when a different one is loaded.
fn details(
    s: &mut Session<Box<dyn SerialPort>>,
    files: &crate::types::Files,
    known: &mut Detailed,
    emit: &impl Fn(Event),
) -> Result<(), SessionError> {
    if known.model.as_deref() != Some(files.model.as_str()) {
        known.model = Some(files.model.clone());
        let info = s.model_info()?;
        if known.model_info.as_ref() != Some(&info) {
            known.model_info = Some(info.clone());
            emit(Event::ModelInfo { info });
        }
    }
    if known.ir.as_deref() != Some(files.ir.as_str()) {
        known.ir = Some(files.ir.clone());
        let mut info = s.ir_info()?;
        if let Some(info) = info.as_mut() {
            if let Some(samples) = s.ir_data()? {
                info.wave = crate::curve::wave(&samples);
                info.response = crate::curve::response(&samples, 48000.0); // the unit runs at 48 kHz only
            }
        }
        emit(Event::IrInfo { info });
    }
    Ok(())
}

/// The model and IR whose details the UI has been sent.
#[derive(Default)]
struct Detailed {
    model: Option<String>,
    ir: Option<String>,
    model_info: Option<Option<crate::types::ModelInfo>>,
}

/// Fetches the model and IR lists. Firmware before file support refuses; the lists stay empty.
fn file_lists(s: &mut Session<Box<dyn SerialPort>>, emit: &impl Fn(Event)) -> Result<(), SessionError> {
    for (msg_type, models) in [(msg::MODEL_LIST, true), (msg::IR_LIST, false)] {
        match s.file_list(msg_type) {
            Ok(files) => emit(if models { Event::Models { files } } else { Event::Irs { files } }),
            Err(SessionError::Device { .. }) => {}
            Err(e) => return Err(e),
        }
    }
    emit(Event::Card { space: s.card_info()? });
    Ok(())
}

/// Reads, checks and sends one file. Problems with the file itself are reported as a finished
/// transfer with an error, not as a failure of the connection.
fn send_file(s: &mut Session<Box<dyn SerialPort>>, local: &Path, max_path: usize, emit: &impl Fn(Event)) -> Result<(), SessionError> {
    let name = local.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    let transfer = |sent: u32, total: u32, done: bool, error: &str| {
        emit(Event::Transfer { name: name.clone(), sent, total, done, error: error.to_string(), index: 1, count: 1, all_sent: sent as u64, all_total: total as u64 })
    };
    let data = match std::fs::read(local) {
        Ok(data) => data,
        Err(e) => {
            transfer(0, 0, true, &format!("The file could not be read ({e})."));
            return Ok(());
        }
    };
    let total = data.len() as u32;
    let checked = match files::check(local, &data, max_path) {
        Ok(checked) => checked,
        Err(why) => {
            transfer(0, total, true, &why);
            return Ok(());
        }
    };
    transfer(0, total, false, "");
    match s.put_file(&checked.device_path, &data, |sent| transfer(sent, total, false, "")) {
        Ok(()) => {
            transfer(total, total, true, "");
            Ok(())
        }
        Err(SessionError::Device { message, .. }) => {
            transfer(0, total, true, &message);
            Ok(())
        }
        Err(e) => Err(e),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_track_lists_are_asked_for_again_when_the_unit_reads_anew() {
        let mut view = TrackView::default();
        let state = |list_number: u32, reading: bool| TrackState { list_number, reading, ..Default::default() };
        // The first state after connecting.
        view.seen(&state(3, false));
        assert!(std::mem::take(&mut view.refresh));
        // The place moving changes nothing.
        view.seen(&state(3, false));
        assert!(!view.refresh);
        // A change on the card: a new list number, then the end of the reading.
        view.seen(&state(4, true));
        assert!(std::mem::take(&mut view.refresh));
        view.seen(&state(4, true));
        assert!(!view.refresh);
        view.seen(&state(4, false));
        assert!(std::mem::take(&mut view.refresh));
    }
}
