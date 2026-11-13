//! One session with the unit over a byte stream: requests with replies, and events in between.

use std::io::{self, Read, Write};
use std::time::{Duration, Instant};

use namforge_link::{encode_frame, flags, msg, ErrorCode, Fed, Frame, FrameParser, Reader, WireError, Writer};

use crate::types::{DeviceInfo, Event, FileEntry, Files, Meters, Preset, Setting};

/// The app's name as sent in `HELLO`.
pub const APP_ID: &str = concat!("NAM Forge ", env!("CARGO_PKG_VERSION"));

#[derive(Debug)]
pub enum SessionError {
    Io(io::Error),
    /// No reply within the timeout.
    Timeout { request: u8 },
    /// The device answered with an error reply.
    Device { request: u8, code: ErrorCode, message: String },
    /// A reply that does not have the fields PROTOCOL.md says it has.
    Malformed { request: u8, problem: WireError },
    /// The device speaks a protocol version this app does not.
    Version { device: u16 },
}

impl std::fmt::Display for SessionError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            SessionError::Io(e) => write!(f, "USB connection lost ({e})"),
            SessionError::Timeout { request } => write!(f, "the unit did not answer (request {request:#04x})"),
            SessionError::Device { message, code, .. } => write!(f, "the unit refused: {message} ({code:?})"),
            SessionError::Malformed { request, problem } => {
                write!(f, "unexpected answer from the unit to request {request:#04x}: {problem}")
            }
            SessionError::Version { device } => write!(
                f,
                "the unit's firmware speaks link protocol version {device}, this app speaks {}; update the older one",
                namforge_link::PROTOCOL_VERSION
            ),
        }
    }
}

impl std::error::Error for SessionError {}

impl From<io::Error> for SessionError {
    fn from(e: io::Error) -> Self {
        SessionError::Io(e)
    }
}

const REPLY_TIMEOUT: Duration = Duration::from_millis(1000);
/// For requests that load or write files on the SD card (a preset recall loads a model and an IR).
const SLOW_REPLY_TIMEOUT: Duration = Duration::from_millis(6000);
/// A frame that stops arriving for this long is abandoned (PROTOCOL.md 2.3). Longer than the
/// stream's read timeout, so a frame split across several reads is put together.
const LINE_IDLE: Duration = Duration::from_millis(100);

/// The protocol over a stream whose `read` returns within a short timeout (the serial port's
/// read timeout): `read` returning `TimedOut` or 0 bytes means "nothing yet".
pub struct Session<T: Read + Write> {
    io: T,
    parser: FrameParser,
    sequence: u16,
    /// Events that arrived while waiting for a reply, oldest first.
    pending: Vec<Frame>,
    last_byte: Instant,
}

impl<T: Read + Write> Session<T> {
    pub fn new(io: T) -> Self {
        Self { io, parser: FrameParser::default(), sequence: 0, pending: Vec::new(), last_byte: Instant::now() }
    }

    /// Reads what is there and returns the frames in it. Text from the unit's shell is skipped.
    fn read_frames(&mut self) -> Result<Vec<Frame>, SessionError> {
        let mut buf = [0u8; 4096];
        let n = match self.io.read(&mut buf) {
            Ok(n) => n,
            Err(e) if matches!(e.kind(), io::ErrorKind::TimedOut | io::ErrorKind::WouldBlock) => 0,
            Err(e) => return Err(e.into()),
        };
        if n > 0 {
            self.last_byte = Instant::now();
        } else if self.parser.busy() && self.last_byte.elapsed() >= LINE_IDLE {
            // A quiet line: a half-received or damaged frame is abandoned (PROTOCOL.md 2.3).
            self.parser.idle();
        }
        let mut frames = Vec::new();
        for &b in &buf[..n] {
            if let Fed::Frame(f) = self.parser.feed(b) {
                frames.push(f);
            }
        }
        Ok(frames)
    }

    /// Sends a request and waits for its reply. Events that arrive meanwhile are kept for
    /// [`Session::events`].
    pub fn request(&mut self, msg_type: u8, payload: &[u8]) -> Result<Frame, SessionError> {
        self.request_within(msg_type, payload, REPLY_TIMEOUT)
    }

    fn request_within(&mut self, msg_type: u8, payload: &[u8], timeout: Duration) -> Result<Frame, SessionError> {
        self.sequence = if self.sequence == u16::MAX { 1 } else { self.sequence + 1 };
        let frame = encode_frame(msg_type, 0, self.sequence, payload).expect("request payloads are small");
        self.io.write_all(&frame)?;
        self.io.flush()?;
        let deadline = Instant::now() + timeout;
        loop {
            for f in self.read_frames()? {
                if f.is_reply() && f.sequence == self.sequence && f.msg_type == msg_type {
                    if f.is_error() {
                        let mut r = Reader::new(&f.payload);
                        let code = r.u16().map(ErrorCode::from_u16).unwrap_or(ErrorCode::Other(0));
                        let message = r.str().unwrap_or_default();
                        return Err(SessionError::Device { request: msg_type, code, message });
                    }
                    return Ok(f);
                } else if f.is_event() {
                    self.pending.push(f);
                }
                // Anything else is a late reply to a request that timed out: dropped.
            }
            if Instant::now() >= deadline {
                return Err(SessionError::Timeout { request: msg_type });
            }
        }
    }

    /// Waits up to the stream's read timeout for events, and returns them with any that
    /// arrived during earlier requests.
    pub fn events(&mut self) -> Result<Vec<Event>, SessionError> {
        let mut frames = std::mem::take(&mut self.pending);
        frames.extend(self.read_frames()?.into_iter().filter(Frame::is_event));
        Ok(frames.iter().filter_map(decode_event).collect())
    }

    pub fn hello(&mut self, port: &str) -> Result<DeviceInfo, SessionError> {
        let mut w = Writer::new();
        w.u16(namforge_link::PROTOCOL_VERSION);
        w.str(APP_ID).expect("a short string");
        let f = self.request(msg::HELLO, &w.finish())?;
        let info = (|| -> Result<DeviceInfo, WireError> {
            let mut r = Reader::new(&f.payload);
            Ok(DeviceInfo {
                protocol: r.u16()?,
                firmware: r.str()?,
                block_size: r.u16()?,
                sample_rate: r.u32()?,
                max_models: r.u16()?,
                max_irs: r.u16()?,
                max_path: r.u16()?,
                saved_settings: r.u16()?,
                settings: r.u16()?,
                schema_hash: r.u32()?,
                port: port.to_string(),
                // Later protocol versions may append fields: no done() check here.
            })
        })()
        .map_err(|problem| SessionError::Malformed { request: msg::HELLO, problem })?;
        if info.protocol != namforge_link::PROTOCOL_VERSION {
            return Err(SessionError::Version { device: info.protocol });
        }
        Ok(info)
    }

    pub fn schema(&mut self) -> Result<Vec<Setting>, SessionError> {
        let f = self.request(msg::GET_SCHEMA, &[])?;
        (|| -> Result<Vec<Setting>, WireError> {
            let mut r = Reader::new(&f.payload);
            let count = r.u16()?;
            let mut out = Vec::with_capacity(count as usize);
            for id in 0..count {
                let (key, name, label) = (r.str()?, r.str()?, r.str()?);
                let (min, max, default) = (r.i16()?, r.i16()?, r.i16()?);
                let (step, unit, flag_bits) = (r.u8()?, r.u8()?, r.u8()?);
                let choices = (0..r.u16()?).map(|_| r.str()).collect::<Result<Vec<_>, _>>()?;
                out.push(Setting { id, key, name, label, min, max, default, step, unit, in_preset: flag_bits & 1 != 0, choices });
            }
            r.done()?;
            Ok(out)
        })()
        .map_err(|problem| SessionError::Malformed { request: msg::GET_SCHEMA, problem })
    }

    pub fn state(&mut self) -> Result<(Vec<i16>, Files), SessionError> {
        let f = self.request(msg::GET_STATE, &[])?;
        (|| -> Result<(Vec<i16>, Files), WireError> {
            let mut r = Reader::new(&f.payload);
            let values = (0..r.u16()?).map(|_| r.i16()).collect::<Result<Vec<_>, _>>()?;
            let files = read_files(&mut r)?;
            r.done()?;
            Ok((values, files))
        })()
        .map_err(|problem| SessionError::Malformed { request: msg::GET_STATE, problem })
    }

    pub fn presets(&mut self) -> Result<Vec<Preset>, SessionError> {
        let f = self.request(msg::PRESET_LIST, &[])?;
        (|| -> Result<Vec<Preset>, WireError> {
            let mut r = Reader::new(&f.payload);
            let n = r.u16()?;
            let list = (0..n).map(|_| Ok(Preset { name: r.str()?, number: r.u16()? })).collect::<Result<_, WireError>>()?;
            r.done()?;
            Ok(list)
        })()
        .map_err(|problem| SessionError::Malformed { request: msg::PRESET_LIST, problem })
    }

    /// One of the preset requests that take a name (`PRESET_RECALL`, `_NEW`, `_RENAME`,
    /// `_DELETE`) or none (`PRESET_SAVE`). The changes arrive as events.
    pub fn preset(&mut self, msg_type: u8, name: Option<&str>) -> Result<(), SessionError> {
        let mut w = Writer::new();
        if let Some(name) = name {
            if w.str(name).is_err() {
                let (code, message) = (ErrorCode::from_u16(2), "The name is too long".to_string());
                return Err(SessionError::Device { request: msg_type, code, message });
            }
        }
        self.request_within(msg_type, &w.finish(), SLOW_REPLY_TIMEOUT).map(|_| ())
    }

    /// The amp models (`MODEL_LIST`) or the IRs (`IR_LIST`) on the card.
    pub fn file_list(&mut self, msg_type: u8) -> Result<Vec<FileEntry>, SessionError> {
        let f = self.request_within(msg_type, &[], SLOW_REPLY_TIMEOUT)?;
        (|| -> Result<Vec<FileEntry>, WireError> {
            let mut r = Reader::new(&f.payload);
            let n = r.u16()?;
            let mut list = Vec::with_capacity(n as usize);
            for _ in 0..n {
                let (path, size, _modified) = (r.str()?, r.u32()?, r.u32()?);
                list.push(FileEntry { path, size });
            }
            r.done()?;
            Ok(list)
        })()
        .map_err(|problem| SessionError::Malformed { request: msg_type, problem })
    }

    /// A request whose payload is one path and whose reply is empty: `MODEL_LOAD`, `IR_LOAD`,
    /// `FILE_DELETE`.
    pub fn with_path(&mut self, msg_type: u8, path: &str) -> Result<(), SessionError> {
        let mut w = Writer::new();
        w.str(path).map_err(|problem| SessionError::Malformed { request: msg_type, problem })?;
        self.request_within(msg_type, &w.finish(), SLOW_REPLY_TIMEOUT).map(|_| ())
    }

    /// Sends a file to the card. `progress` is called with the bytes sent so far. The unit
    /// checks size and CRC, puts the file in place and rescans the card before it replies.
    pub fn put_file(&mut self, path: &str, data: &[u8], mut progress: impl FnMut(u32)) -> Result<(), SessionError> {
        let malformed = |problem| SessionError::Malformed { request: msg::FILE_PUT_BEGIN, problem };
        let mut w = Writer::new();
        w.str(path).map_err(malformed)?;
        w.u32(data.len() as u32).u32(namforge_link::crc32(data, 0));
        let f = self.request_within(msg::FILE_PUT_BEGIN, &w.finish(), SLOW_REPLY_TIMEOUT)?;
        let chunk = Reader::new(&f.payload).u16().map_err(malformed)?.max(1) as usize;
        let mut sent = 0usize;
        for part in data.chunks(chunk) {
            let mut w = Writer::new();
            w.u32(sent as u32).bytes(part);
            self.request_within(msg::FILE_PUT_DATA, &w.finish(), SLOW_REPLY_TIMEOUT)?;
            sent += part.len();
            progress(sent as u32);
        }
        self.request_within(msg::FILE_PUT_END, &[], Duration::from_secs(15)).map(|_| ())
    }

    /// Asks for meter frames at `rate` per second (0 = none); returns the rate in force.
    pub fn subscribe(&mut self, rate: u16) -> Result<u16, SessionError> {
        let mut w = Writer::new();
        w.u16(rate);
        let f = self.request(msg::SUBSCRIBE, &w.finish())?;
        Reader::new(&f.payload).u16().map_err(|problem| SessionError::Malformed { request: msg::SUBSCRIBE, problem })
    }

    /// Sets a value and returns the one now in force (the device clamps to the setting's range).
    pub fn set_param(&mut self, id: u16, value: i32) -> Result<i32, SessionError> {
        let mut w = Writer::new();
        w.u16(id).i32(value);
        let f = self.request(msg::SET_PARAM, &w.finish())?;
        (|| -> Result<i32, WireError> {
            let mut r = Reader::new(&f.payload);
            r.u16()?;
            let now = r.i32()?;
            r.done()?;
            Ok(now)
        })()
        .map_err(|problem| SessionError::Malformed { request: msg::SET_PARAM, problem })
    }
}

fn read_meters(r: &mut Reader<'_>) -> Result<Meters, WireError> {
    let mut m = Meters {
        input_peak: r.f32()?,
        output_peak: r.f32()?,
        gate_gain: r.f32()?,
        clipped: r.u32()?,
        cpu: r.f32()?,
        cpu_max: r.f32()?,
        cpu_amp: r.f32()?,
        cpu_doubler: r.f32()?,
        cpu_delay: r.f32()?,
        cpu_reverb: r.f32()?,
        cpu_drums: r.f32()?,
        overruns: r.u32()?,
        overload_mutes: r.u32()?,
        ..Meters::default()
    };
    let bits = r.u8()?;
    m.amp_running = bits & 1 != 0;
    m.delay_ringing = bits & 2 != 0;
    m.reverb_ringing = bits & 4 != 0;
    m.drums_playing = bits & 8 != 0;
    m.muted = bits & 16 != 0;
    Ok(m)
}

fn read_files(r: &mut Reader<'_>) -> Result<Files, WireError> {
    Ok(Files { model: r.str()?, ir: r.str()?, preset: r.str()?, edited: r.bool()?, card: r.bool()? })
}

/// An event frame as the UI's [`Event`]; `None` for kinds this version does not know.
fn decode_event(f: &Frame) -> Option<Event> {
    let mut r = Reader::new(&f.payload);
    match f.msg_type {
        msg::PARAM_CHANGED => Some(Event::Param { id: r.u16().ok()?, value: r.i32().ok()? }),
        msg::STATE_CHANGED => Some(Event::Files { files: read_files(&mut r).ok()? }),
        msg::PRESETS_CHANGED => Some(Event::PresetsChanged),
        msg::METERS => Some(Event::Meters { meters: read_meters(&mut r).ok()? }),
        _ => None,
    }
}

// The flags constants are part of the protocol crate's public face; keep the import used.
const _: u8 = flags::EVENT;

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::VecDeque;

    /// A stand-in for the unit: answers the A1 requests like the firmware does, and can have
    /// text and events queued as if they came from the shell or MIDI.
    #[derive(Default)]
    struct FakeUnit {
        parser: FrameParser,
        out: VecDeque<u8>,
        values: Vec<i16>,
        greeted: bool,
        protocol: u16,
        mute: bool,
    }

    impl FakeUnit {
        fn new() -> Self {
            Self { values: vec![0, 30, 380], protocol: 1, ..Default::default() }
        }
        fn send(&mut self, t: u8, fl: u8, seq: u16, payload: Vec<u8>) {
            self.out.extend(encode_frame(t, fl, seq, &payload).unwrap());
        }
        fn event_param(&mut self, id: u16, value: i32) {
            let mut w = Writer::new();
            w.u16(id).i32(value);
            self.send(msg::PARAM_CHANGED, flags::EVENT, 0, w.finish());
        }
        fn text(&mut self, s: &str) {
            self.out.extend(s.as_bytes());
        }
        fn handle(&mut self, f: Frame) {
            if self.mute {
                return;
            }
            let mut w = Writer::new();
            match f.msg_type {
                msg::HELLO => {
                    self.greeted = true;
                    w.u16(self.protocol);
                    w.str("0.1.0").unwrap();
                    w.u16(32).u32(48000).u16(64).u16(128).u16(95).u16(2).u16(3).u32(7);
                }
                _ if !self.greeted => {
                    w.u16(2);
                    w.str("Say HELLO first").unwrap();
                    return self.send(f.msg_type, flags::REPLY | flags::ERROR, f.sequence, w.finish());
                }
                msg::GET_SCHEMA => {
                    w.u16(3);
                    for (key, name, lo, hi, def, unit, choices) in [
                        ("dl", "Delay", 0i16, 1i16, 0i16, 1u8, vec![]),
                        ("dmix", "Delay mix", 0, 100, 30, 2, vec![]),
                        ("rmode", "Reverb mode", 0, 1, 0, 0, vec!["hall", "gated"]),
                    ] {
                        w.str(key).unwrap();
                        w.str(name).unwrap();
                        w.str("L").unwrap();
                        w.i16(lo).i16(hi).i16(def).u8(1).u8(unit).u8(1);
                        w.u16(choices.len() as u16);
                        for c in choices {
                            w.str(c).unwrap();
                        }
                    }
                }
                msg::GET_STATE => {
                    w.u16(self.values.len() as u16);
                    for v in &self.values {
                        w.i16(*v);
                    }
                    w.str("/AMP/a.nam").unwrap();
                    w.str("/IR/b.wav").unwrap();
                    w.str("01 Clean").unwrap();
                    w.bool(false).bool(true);
                }
                msg::SET_PARAM => {
                    let mut r = Reader::new(&f.payload);
                    let (id, v) = (r.u16().unwrap(), r.i32().unwrap().clamp(0, 100));
                    self.values[id as usize] = v as i16;
                    w.u16(id).i32(v);
                }
                _ => {
                    w.u16(1);
                    w.str("Not supported").unwrap();
                    return self.send(f.msg_type, flags::REPLY | flags::ERROR, f.sequence, w.finish());
                }
            }
            self.send(f.msg_type, flags::REPLY, f.sequence, w.finish());
        }
    }

    impl Read for FakeUnit {
        fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
            // A few bytes at a time, so frames arrive split across reads.
            let n = buf.len().min(7).min(self.out.len());
            for b in buf.iter_mut().take(n) {
                *b = self.out.pop_front().unwrap();
            }
            if n == 0 {
                return Err(io::ErrorKind::TimedOut.into());
            }
            Ok(n)
        }
    }

    impl Write for FakeUnit {
        fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
            for &b in buf {
                if let Fed::Frame(f) = self.parser.feed(b) {
                    self.handle(f);
                }
            }
            Ok(buf.len())
        }
        fn flush(&mut self) -> io::Result<()> {
            Ok(())
        }
    }

    #[test]
    fn handshake_schema_state_and_set() {
        let mut s = Session::new(FakeUnit::new());
        let info = s.hello("fake").unwrap();
        assert_eq!((info.protocol, info.block_size, info.sample_rate, info.settings), (1, 32, 48000, 3));
        let schema = s.schema().unwrap();
        assert_eq!(schema.len(), 3);
        assert_eq!((schema[1].key.as_str(), schema[1].min, schema[1].max, schema[1].unit), ("dmix", 0, 100, 2));
        assert_eq!(schema[2].choices, ["hall", "gated"]);
        assert!(schema[0].in_preset);
        let (values, files) = s.state().unwrap();
        assert_eq!(values, [0, 30, 380]);
        assert_eq!((files.model.as_str(), files.preset.as_str(), files.edited, files.card), ("/AMP/a.nam", "01 Clean", false, true));
        assert_eq!(s.set_param(1, 45).unwrap(), 45);
        assert_eq!(s.set_param(1, 5000).unwrap(), 100, "the device clamps");
    }

    #[test]
    fn events_and_shell_text_between_replies() {
        let mut s = Session::new(FakeUnit::new());
        s.hello("fake").unwrap();
        // Text and an event are already on the line when the next request goes out.
        s.io.text("Delay mix 44%\r\n");
        s.io.event_param(1, 44);
        s.io.text("more text\n");
        assert_eq!(s.set_param(1, 50).unwrap(), 50);
        assert_eq!(s.events().unwrap(), [Event::Param { id: 1, value: 44 }]);
        // The fake hands over a few bytes per read, so a frame takes several calls, as a
        // frame split across USB packets would.
        let drain = |s: &mut Session<FakeUnit>| -> Vec<Event> {
            let mut all = Vec::new();
            for _ in 0..20 {
                all.extend(s.events().unwrap());
            }
            all
        };
        assert!(drain(&mut s).is_empty());
        s.io.event_param(2, 1);
        assert_eq!(drain(&mut s), [Event::Param { id: 2, value: 1 }]);
    }

    #[test]
    fn meter_frames_become_events() {
        let mut dev = FakeUnit::new();
        let mut w = Writer::new();
        w.f32(0.5).f32(0.25).f32(1.0).u32(3);
        for cpu in [56.0f32, 57.5, 32.0, 1.0, 6.0, 9.0, 0.0] {
            w.f32(cpu);
        }
        w.u32(2).u32(1).u8(0b0_1101);
        dev.send(msg::METERS, flags::EVENT, 0, w.finish());
        let mut s = Session::new(dev);
        let mut events = Vec::new();
        for _ in 0..40 {
            events.extend(s.events().unwrap());
        }
        let [Event::Meters { meters: m }] = &events[..] else { panic!("{events:?}") };
        assert_eq!((m.input_peak, m.output_peak, m.gate_gain, m.clipped), (0.5, 0.25, 1.0, 3));
        assert_eq!((m.cpu, m.cpu_max, m.cpu_amp, m.cpu_drums), (56.0, 57.5, 32.0, 0.0));
        assert_eq!((m.overruns, m.overload_mutes), (2, 1));
        assert!(m.amp_running && !m.delay_ringing && m.reverb_ringing && m.drums_playing && !m.muted);
    }

    #[test]
    fn errors_are_reported() {
        let mut s = Session::new(FakeUnit::new());
        match s.state() {
            Err(SessionError::Device { code: ErrorCode::BadPayload, message, .. }) => assert_eq!(message, "Say HELLO first"),
            other => panic!("{other:?}"),
        }
        s.hello("fake").unwrap();
        assert!(matches!(s.request(0x7E, &[]), Err(SessionError::Device { code: ErrorCode::UnknownMessage, .. })));

        let mut newer = FakeUnit::new();
        newer.protocol = 9;
        assert!(matches!(Session::new(newer).hello("fake"), Err(SessionError::Version { device: 9 })));

        let mut silent = FakeUnit::new();
        silent.mute = true;
        let t0 = Instant::now();
        assert!(matches!(Session::new(silent).hello("fake"), Err(SessionError::Timeout { .. })));
        assert!(t0.elapsed() >= REPLY_TIMEOUT);
    }
}
