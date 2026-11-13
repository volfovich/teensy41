//! The unit on a background thread: find it on USB, open a session, keep it alive, apply the
//! UI's commands, and start over when it is unplugged.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::mpsc::{self, Receiver, Sender, TryRecvError};
use std::thread;
use std::time::{Duration, Instant};

use serialport::{SerialPort, SerialPortType};

use namforge_link::msg;

use crate::session::{Session, SessionError};
use crate::files;
use crate::types::Event;

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
    }
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
        emit(Event::IrInfo { info: s.ir_info()? });
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
    Ok(())
}

/// Reads, checks and sends one file. Problems with the file itself are reported as a finished
/// transfer with an error, not as a failure of the connection.
fn send_file(s: &mut Session<Box<dyn SerialPort>>, local: &Path, max_path: usize, emit: &impl Fn(Event)) -> Result<(), SessionError> {
    let name = local.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    let transfer = |sent: u32, total: u32, done: bool, error: &str| {
        emit(Event::Transfer { name: name.clone(), sent, total, done, error: error.to_string() })
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
