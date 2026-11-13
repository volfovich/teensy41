//! The unit on a background thread: find it on USB, open a session, keep it alive, apply the
//! UI's commands, and start over when it is unplugged.

use std::collections::HashMap;
use std::sync::mpsc::{self, Receiver, Sender, TryRecvError};
use std::thread;
use std::time::{Duration, Instant};

use serialport::{SerialPort, SerialPortType};

use crate::session::{Session, SessionError};
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

    let mut last_state = Instant::now() - KEEPALIVE;
    loop {
        if last_state.elapsed() >= KEEPALIVE {
            match s.state() {
                Ok((values, files)) => emit(Event::State { values, files }),
                Err(e) => return lost(e),
            }
            last_state = Instant::now();
        }

        // The UI's commands, newest value per setting, in the order the settings were first
        // touched.
        let mut order: Vec<u16> = Vec::new();
        let mut newest: HashMap<u16, i32> = HashMap::new();
        loop {
            match commands.try_recv() {
                Ok(Command::SetParam { id, value }) => {
                    if newest.insert(id, value).is_none() {
                        order.push(id);
                    }
                }
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

        // Waits up to the port's read timeout, so this loop does not spin.
        match s.events() {
            Ok(events) => events.into_iter().for_each(emit),
            Err(e) => return lost(e),
        }
    }
}
