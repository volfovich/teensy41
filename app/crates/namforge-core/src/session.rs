//! One session with the unit over a byte stream: requests with replies, and events in between.

use std::io::{self, Read, Write};
use std::time::{Duration, Instant};

use namforge_link::{encode_frame, msg, ErrorCode, Fed, Frame, FrameParser, Reader, WireError, Writer};

use crate::types::{CardSpace, DeviceInfo, Event, FileEntry, Files, IrInfo, Meters, ModelInfo, Preset, Setting, TrackRow, TrackState};

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
/// For `FILE_PUT_END` and `FOLDER_DELETE`: the unit answers when it is done.
const LONG_REPLY_TIMEOUT: Duration = Duration::from_secs(15);
/// What the unit answers to any request but `HELLO` when it has no session open (it ended ours).
pub const NO_SESSION: &str = "Say HELLO first";
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

    /// The meter, tuner, setting and backing-track events that arrived during earlier requests,
    /// for showing them while a long job (a file on its way to the unit) holds the line. Other
    /// events stay for [`Session::events`].
    pub fn live_events(&mut self) -> Vec<Event> {
        let live = |f: &Frame| matches!(f.msg_type, msg::METERS | msg::TUNER | msg::PARAM_CHANGED | msg::TRACK_CHANGED);
        let (now, later): (Vec<Frame>, Vec<Frame>) = std::mem::take(&mut self.pending).into_iter().partition(live);
        self.pending = later;
        now.iter().filter_map(decode_event).collect()
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

    /// The loaded model's details; `None` when no model is loaded or the firmware has no such request.
    pub fn model_info(&mut self) -> Result<Option<ModelInfo>, SessionError> {
        let f = match self.request(msg::MODEL_INFO, &[]) {
            Ok(f) => f,
            Err(SessionError::Device { .. }) => return Ok(None),
            Err(e) => return Err(e),
        };
        let known = |v: f32| (!v.is_nan()).then_some(v);
        (|| -> Result<ModelInfo, WireError> {
            let mut r = Reader::new(&f.payload);
            Ok(ModelInfo {
                name: r.str()?,
                gear_make: r.str()?,
                gear_model: r.str()?,
                gear_type: r.str()?,
                tone_type: r.str()?,
                sample_rate: r.f32()?,
                loudness: known(r.f32()?),
                input_level_dbu: known(r.f32()?),
                output_level_dbu: known(r.f32()?),
                calibration_db: known(r.f32()?),
                level_db: r.f32()?,
            })
        })()
        .map(Some)
        .map_err(|problem| SessionError::Malformed { request: msg::MODEL_INFO, problem })
    }

    /// The loaded IR's details; `None` when no IR is loaded or the firmware has no such request.
    pub fn ir_info(&mut self) -> Result<Option<IrInfo>, SessionError> {
        let f = match self.request(msg::IR_INFO, &[]) {
            Ok(f) => f,
            Err(SessionError::Device { .. }) => return Ok(None),
            Err(e) => return Err(e),
        };
        (|| -> Result<IrInfo, WireError> {
            let mut r = Reader::new(&f.payload);
            Ok(IrInfo {
                sample_rate: r.u32()?,
                channels: r.u16()?,
                bits: r.u16()?,
                frames: r.u32()?,
                length: r.u32()?,
                resampled: r.bool()?,
                cut: r.bool()?,
                gain: r.f32()?,
                wave: Vec::new(),
                response: Vec::new(),
            })
        })()
        .map(Some)
        .map_err(|problem| SessionError::Malformed { request: msg::IR_INFO, problem })
    }

    /// The loaded IR's samples as the unit uses them (at its sample rate); `None` when no IR
    /// is loaded or the firmware has no such request.
    pub fn ir_data(&mut self) -> Result<Option<Vec<f32>>, SessionError> {
        let f = match self.request(msg::IR_DATA, &[]) {
            Ok(f) => f,
            Err(SessionError::Device { .. }) => return Ok(None),
            Err(e) => return Err(e),
        };
        (|| -> Result<Vec<f32>, WireError> {
            let mut r = Reader::new(&f.payload);
            let (n, peak) = (r.u16()?, r.f32()?);
            let mut samples = Vec::with_capacity(n as usize);
            for _ in 0..n {
                samples.push(r.u16()? as i16 as f32 * peak / 32767.0);
            }
            r.done()?;
            Ok(samples)
        })()
        .map(Some)
        .map_err(|problem| SessionError::Malformed { request: msg::IR_DATA, problem })
    }

    /// The card's size and free space; `None` without a card or when the firmware has no such
    /// request. The unit may take a few seconds to count the free space of a large card.
    pub fn card_info(&mut self) -> Result<Option<CardSpace>, SessionError> {
        let f = match self.request_within(msg::CARD_INFO, &[], SLOW_REPLY_TIMEOUT) {
            Ok(f) => f,
            Err(SessionError::Device { .. }) => return Ok(None),
            Err(e) => return Err(e),
        };
        (|| -> Result<Option<CardSpace>, WireError> {
            let mut r = Reader::new(&f.payload);
            let (present, total_kb, free_kb) = (r.bool()?, r.u32()?, r.u32()?);
            Ok(present.then_some(CardSpace { total_kb, free_kb }))
        })()
        .map_err(|problem| SessionError::Malformed { request: msg::CARD_INFO, problem })
    }

    /// The files and folders in one of the card's folders ("/AMP", "/"): name, size, is a folder.
    pub fn dir_list(&mut self, folder: &str) -> Result<Vec<(String, u32, bool)>, SessionError> {
        let malformed = |problem| SessionError::Malformed { request: msg::DIR_LIST, problem };
        let mut w = Writer::new();
        w.str(folder).map_err(malformed)?;
        let f = self.request_within(msg::DIR_LIST, &w.finish(), SLOW_REPLY_TIMEOUT)?;
        (|| -> Result<Vec<(String, u32, bool)>, WireError> {
            let mut r = Reader::new(&f.payload);
            let n = r.u16()?;
            let mut list = Vec::with_capacity(n as usize);
            for _ in 0..n {
                let (name, size, _modified, is_folder) = (r.str()?, r.u32()?, r.u32()?, r.bool()?);
                list.push((name, size, is_folder));
            }
            r.done()?;
            Ok(list)
        })()
        .map_err(malformed)
    }

    /// Reads a file from the card. It arrives as a series of `FILE_DATA` replies; `progress`
    /// is called with the bytes received so far.
    pub fn get_file(&mut self, path: &str, mut progress: impl FnMut(u32, u32)) -> Result<Vec<u8>, SessionError> {
        let malformed = |problem| SessionError::Malformed { request: msg::FILE_GET, problem };
        let mut w = Writer::new();
        w.str(path).map_err(malformed)?;
        self.sequence = if self.sequence == u16::MAX { 1 } else { self.sequence + 1 };
        let frame = encode_frame(msg::FILE_GET, 0, self.sequence, &w.finish()).expect("a small request");
        self.io.write_all(&frame)?;
        self.io.flush()?;
        let mut data: Vec<u8> = Vec::new();
        let mut deadline = Instant::now() + SLOW_REPLY_TIMEOUT;
        loop {
            for f in self.read_frames()? {
                if f.is_event() {
                    self.pending.push(f);
                } else if f.is_reply() && f.sequence == self.sequence && f.is_error() {
                    let mut r = Reader::new(&f.payload);
                    let code = r.u16().map(ErrorCode::from_u16).unwrap_or(ErrorCode::Other(0));
                    return Err(SessionError::Device { request: msg::FILE_GET, code, message: r.str().unwrap_or_default() });
                } else if f.is_reply() && f.sequence == self.sequence && f.msg_type == msg::FILE_DATA {
                    let mut r = Reader::new(&f.payload);
                    let (offset, total) = (r.u32().map_err(malformed)?, r.u32().map_err(malformed)?);
                    let chunk = &f.payload[8..];
                    if offset as usize != data.len() {
                        return Err(malformed(WireError::TooShort));
                    }
                    data.extend_from_slice(chunk);
                    progress(data.len() as u32, total);
                    if data.len() as u32 >= total {
                        return Ok(data);
                    }
                    deadline = Instant::now() + SLOW_REPLY_TIMEOUT;
                }
            }
            if Instant::now() >= deadline {
                return Err(SessionError::Timeout { request: msg::FILE_GET });
            }
        }
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
        self.put_file_each(path, data, |_, sent| progress(sent))
    }

    /// As [`Session::put_file`], but `each` also gets the session after every piece, so that
    /// it can take the events that came in meanwhile ([`Session::live_events`]).
    pub fn put_file_each(&mut self, path: &str, data: &[u8], mut each: impl FnMut(&mut Self, u32)) -> Result<(), SessionError> {
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
            each(self, sent as u32);
        }
        self.request_within(msg::FILE_PUT_END, &[], LONG_REPLY_TIMEOUT).map(|_| ())
    }

    /// As [`Session::put_file_each`], for long uploads: if the unit refuses with "Say HELLO
    /// first" (it ended the session half way; seen once in testing), the session is opened
    /// again and the file is sent once more from its start.
    pub fn put_file_again(&mut self, path: &str, data: &[u8], mut each: impl FnMut(&mut Self, u32)) -> Result<(), SessionError> {
        match self.put_file_each(path, data, &mut each) {
            Err(SessionError::Device { message, .. }) if message == NO_SESSION => {
                self.hello("")?;
                self.put_file_each(path, data, &mut each)
            }
            other => other,
        }
    }

    /// The backing-track player's state (`TRACK_STATE`); `None` when the firmware does not know
    /// the request: it is older and has no player.
    pub fn track_state(&mut self) -> Result<Option<TrackState>, SessionError> {
        match self.request(msg::TRACK_STATE, &[]) {
            Ok(f) => track_reply(&f).map(Some),
            Err(SessionError::Device { code: ErrorCode::UnknownMessage, .. }) => Ok(None),
            Err(e) => Err(e),
        }
    }

    /// The rows of a folder below `/TRACKS` ("" is `/TRACKS` itself, "Rock/Live" a folder in
    /// it): folders first, then tracks. A reply holds what fits in 16 KB, so this asks again
    /// from the next row until it has them all.
    pub fn track_list(&mut self, folder: &str) -> Result<Vec<TrackRow>, SessionError> {
        let malformed = |problem| SessionError::Malformed { request: msg::TRACK_LIST, problem };
        let mut rows: Vec<TrackRow> = Vec::new();
        loop {
            let mut w = Writer::new();
            w.str(folder).map_err(malformed)?;
            w.u16(rows.len() as u16);
            let f = self.request(msg::TRACK_LIST, &w.finish())?;
            let (total, first, page) = (|| -> Result<(u16, u16, Vec<TrackRow>), WireError> {
                let mut r = Reader::new(&f.payload);
                let (total, first) = (r.u16()?, r.u16()?);
                let page = (0..r.u16()?).map(|_| Ok(TrackRow { name: r.str()?, folder: r.bool()?, rows: r.u16()? })).collect::<Result<_, WireError>>()?;
                r.done()?;
                Ok((total, first, page))
            })()
            .map_err(malformed)?;
            if !take_page(&mut rows, total, first, page).map_err(malformed)? {
                return Ok(rows);
            }
        }
    }

    /// The tracks from all folders that have every word of `text` in their name or in a
    /// folder's name (`TRACK_FIND`), as paths below `/TRACKS`, and whether the unit found more
    /// than the 256 it lists.
    pub fn track_find(&mut self, text: &str) -> Result<(Vec<String>, bool), SessionError> {
        let malformed = |problem| SessionError::Malformed { request: msg::TRACK_FIND, problem };
        let mut hits: Vec<String> = Vec::new();
        loop {
            let mut w = Writer::new();
            w.str(text).map_err(malformed)?;
            w.u16(hits.len() as u16);
            let f = self.request(msg::TRACK_FIND, &w.finish())?;
            let (total, more, first, page) = (|| -> Result<(u16, bool, u16, Vec<String>), WireError> {
                let mut r = Reader::new(&f.payload);
                let (total, more, first) = (r.u16()?, r.bool()?, r.u16()?);
                let page = (0..r.u16()?).map(|_| r.str()).collect::<Result<_, WireError>>()?;
                r.done()?;
                Ok((total, more, first, page))
            })()
            .map_err(malformed)?;
            if !take_page(&mut hits, total, first, page).map_err(malformed)? {
                return Ok((hits, more));
            }
        }
    }

    /// Opens a track by its path below `/TRACKS`; "" closes the open one. What played stops.
    pub fn track_open(&mut self, path: &str) -> Result<TrackState, SessionError> {
        let mut w = Writer::new();
        w.str(path).map_err(|problem| SessionError::Malformed { request: msg::TRACK_OPEN, problem })?;
        let f = self.request_within(msg::TRACK_OPEN, &w.finish(), SLOW_REPLY_TIMEOUT)?;
        track_reply(&f)
    }

    /// One of the player's commands (`namforge_link::track_cmd`); what a command does not use
    /// is 0. Returns the state after it.
    pub fn track_command(&mut self, command: u8, a: f32, b: f32, c: f32) -> Result<TrackState, SessionError> {
        let mut w = Writer::new();
        w.u8(command).f32(a).f32(b).f32(c);
        let f = self.request(msg::TRACK_COMMAND, &w.finish())?;
        track_reply(&f)
    }

    /// Gives a track or a folder below `/TRACKS` a new name or place (whole paths:
    /// "/TRACKS/Rock/Song.mp3").
    pub fn rename(&mut self, from: &str, to: &str) -> Result<(), SessionError> {
        let malformed = |problem| SessionError::Malformed { request: msg::FILE_RENAME, problem };
        let mut w = Writer::new();
        w.str(from).map_err(malformed)?;
        w.str(to).map_err(malformed)?;
        self.request_within(msg::FILE_RENAME, &w.finish(), SLOW_REPLY_TIMEOUT).map(|_| ())
    }

    /// Makes a folder below `/TRACKS` ("/TRACKS/Rock/Live"); one that is there already is no error.
    pub fn folder_new(&mut self, path: &str) -> Result<(), SessionError> {
        let mut w = Writer::new();
        w.str(path).map_err(|problem| SessionError::Malformed { request: msg::FOLDER_NEW, problem })?;
        self.request(msg::FOLDER_NEW, &w.finish()).map(|_| ())
    }

    /// Removes a folder below `/TRACKS` with everything in it; returns the number of files
    /// removed. The unit answers when it is done, which can take seconds.
    pub fn folder_delete(&mut self, path: &str) -> Result<u16, SessionError> {
        let malformed = |problem| SessionError::Malformed { request: msg::FOLDER_DELETE, problem };
        let mut w = Writer::new();
        w.str(path).map_err(malformed)?;
        let f = self.request_within(msg::FOLDER_DELETE, &w.finish(), LONG_REPLY_TIMEOUT)?;
        Reader::new(&f.payload).u16().map_err(malformed)
    }

    /// Opens or closes the tuner (the guitar is muted while it is open).
    pub fn tuner_open(&mut self, open: bool) -> Result<(), SessionError> {
        let mut w = Writer::new();
        w.u8(open as u8);
        self.request(msg::TUNER_OPEN, &w.finish()).map(|_| ())
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
    m.tuner_open = bits & 32 != 0;
    // Added later: older firmware's frame ends here.
    while let Ok(peak) = r.f32() {
        m.stage_peaks.push(peak);
    }
    Ok(m)
}

/// Adds one reply's rows to a list that is fetched in pieces (`TRACK_LIST`, `TRACK_FIND`).
/// Returns whether more must be asked for.
fn take_page<R>(rows: &mut Vec<R>, total: u16, first: u16, page: Vec<R>) -> Result<bool, WireError> {
    // The unit's list may have changed between two requests: what it sends counts.
    if first as usize > rows.len() {
        return Err(WireError::TooShort);
    }
    rows.truncate(first as usize);
    let got = !page.is_empty();
    rows.extend(page);
    rows.truncate(total as usize);
    // A reply without rows ends it too, so a wrong count cannot keep the app asking for ever.
    Ok(got && rows.len() < total as usize)
}

/// The backing-track player's state (PROTOCOL.md 4.8). Later firmware may append fields.
fn read_track_state(r: &mut Reader<'_>) -> Result<TrackState, WireError> {
    Ok(TrackState {
        path: r.str()?,
        playing: r.u8()?,
        place: r.f32()?,
        length: r.f32()?,
        tempo: r.u16()?,
        own_tempo: r.f32()?,
        sure: r.bool()?,
        scan: r.i16()?,
        first_beat: r.f32()?,
        beats_per_bar: r.u8()?,
        snap: r.bool()?,
        loop_state: r.u8()?,
        loop_from: r.f32()?,
        loop_to: r.f32()?,
        gaps: r.u32()?,
        file_kind: r.u8()?,
        file_rate: r.u32()?,
        channels: r.u8()?,
        kbps: r.u16()?,
        tracks: r.u16()?,
        reading: r.bool()?,
        list_number: r.u32()?,
    })
}

fn track_reply(f: &Frame) -> Result<TrackState, SessionError> {
    read_track_state(&mut Reader::new(&f.payload)).map_err(|problem| SessionError::Malformed { request: f.msg_type, problem })
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
        msg::TUNER => Some(Event::Tuner { valid: r.bool().ok()?, hz: r.f32().ok()?, note: r.u8().ok()?, octave: r.i16().ok()?, cents: r.f32().ok()? }),
        msg::PRESETS_CHANGED => Some(Event::PresetsChanged),
        msg::METERS => Some(Event::Meters { meters: read_meters(&mut r).ok()? }),
        msg::TRACK_CHANGED => Some(Event::Track { state: Some(read_track_state(&mut r).ok()?) }),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use namforge_link::flags;
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
        /// Every request that arrived: its type and payload.
        log: Vec<(u8, Vec<u8>)>,
        /// Firmware with the backing-track player; without, its requests are unknown.
        tracks: bool,
        /// The rows of the folder "Rock", and how many of them fit in one reply.
        rows: Vec<(&'static str, bool, u16)>,
        page: usize,
        /// The open track and what the commands set.
        open: String,
        place: f32,
        tempo: u16,
        /// The session is dropped once, when an upload has got this far.
        forget_at: Option<u32>,
        putting: (String, Vec<u8>),
        files: Vec<(String, Vec<u8>)>,
    }

    impl FakeUnit {
        fn new() -> Self {
            Self { values: vec![0, 30, 380], protocol: namforge_link::PROTOCOL_VERSION, tempo: 100, page: 3, ..Default::default() }
        }
        fn with_tracks() -> Self {
            let rows = vec![("Live", true, 12), ("Old", true, 0), ("Intro.wav", false, 0), ("Iron man.mp3", false, 0), ("Song_2.mp3", false, 0), ("Song 3.mp3", false, 0), ("Song 4.mp3", false, 0), ("Zebra.wav", false, 0)];
            Self { tracks: true, rows, ..Self::new() }
        }
        fn refuse(&mut self, f: &Frame, code: u16, text: &str) {
            let mut w = Writer::new();
            w.u16(code);
            w.str(text).unwrap();
            self.send(f.msg_type, flags::REPLY | flags::ERROR, f.sequence, w.finish());
        }
        /// The player's state as firmware a little newer than this app sends it: with a field
        /// the app does not know at its end.
        fn track_state(&self, w: &mut Writer) {
            w.str(&self.open).unwrap();
            w.u8(if self.open.is_empty() { 0 } else { 1 }).f32(self.place).f32(if self.open.is_empty() { 0.0 } else { 200.0 }).u16(self.tempo);
            w.f32(120.0).bool(true).i16(if self.open.is_empty() { -1 } else { 100 }).f32(0.5).u8(4).bool(true);
            w.u8(0).f32(0.0).f32(0.0).u32(0).u8(2).u32(44100).u8(2).u16(192);
            w.u16(self.rows.len() as u16).bool(false).u32(3);
            w.u16(0xBEEF);
        }
        fn requests(&self, msg_type: u8) -> Vec<Vec<u8>> {
            self.log.iter().filter(|(t, _)| *t == msg_type).map(|(_, p)| p.clone()).collect()
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
            self.log.push((f.msg_type, f.payload.clone()));
            let mut w = Writer::new();
            let mut r = Reader::new(&f.payload);
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
                    let (id, v) = (r.u16().unwrap(), r.i32().unwrap().clamp(0, 100));
                    self.values[id as usize] = v as i16;
                    w.u16(id).i32(v);
                }
                msg::TRACK_STATE if self.tracks => self.track_state(&mut w),
                msg::TRACK_OPEN if self.tracks => {
                    let path = r.str().unwrap();
                    if path == "Rock/Gone.mp3" {
                        return self.refuse(&f, 3, "No such track");
                    }
                    (self.open, self.place, self.tempo) = (path, 0.0, 100);
                    self.track_state(&mut w);
                }
                msg::TRACK_COMMAND if self.tracks => {
                    let (command, a) = (r.u8().unwrap(), r.f32().unwrap());
                    if self.open.is_empty() {
                        return self.refuse(&f, 3, "No track is open");
                    }
                    match command {
                        4 => self.place = a,
                        5 => self.tempo = a as u16,
                        1..=18 => {}
                        _ => return self.refuse(&f, 2, "Unknown command"),
                    }
                    self.track_state(&mut w);
                }
                msg::TRACK_LIST | msg::TRACK_FIND if self.tracks => {
                    let (text, first) = (r.str().unwrap(), r.u16().unwrap() as usize);
                    let find = f.msg_type == msg::TRACK_FIND;
                    if !find && text != "Rock" {
                        return self.refuse(&f, 3, "No such folder");
                    }
                    let rows: Vec<_> = self.rows.iter().filter(|(name, folder, _)| !find || (!folder && name.to_lowercase().contains(&text))).collect();
                    let first = first.min(rows.len());
                    let sent = &rows[first..rows.len().min(first + self.page)];
                    w.u16(rows.len() as u16);
                    if find {
                        w.bool(true);
                    }
                    w.u16(first as u16).u16(sent.len() as u16);
                    for (name, folder, inside) in sent {
                        if find {
                            w.str(&format!("Rock/{name}")).unwrap();
                        } else {
                            w.str(name).unwrap();
                            w.bool(*folder).u16(*inside);
                        }
                    }
                }
                msg::FILE_RENAME | msg::FOLDER_NEW if self.tracks => {}
                msg::FOLDER_DELETE if self.tracks => {
                    w.u16(12);
                }
                msg::FILE_PUT_BEGIN => {
                    self.putting = (r.str().unwrap(), Vec::new());
                    w.u16(8);
                }
                msg::FILE_PUT_DATA => {
                    let offset = r.u32().unwrap();
                    if self.forget_at.is_some_and(|at| offset >= at) {
                        // The session ended half way through the upload.
                        (self.forget_at, self.greeted) = (None, false);
                        return self.refuse(&f, 2, "Say HELLO first");
                    }
                    self.putting.1.extend_from_slice(&f.payload[4..]);
                    w.u32(self.putting.1.len() as u32);
                }
                msg::FILE_PUT_END => self.files.push(std::mem::take(&mut self.putting)),
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
        assert_eq!((info.protocol, info.block_size, info.sample_rate, info.settings), (2, 32, 48000, 3));
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
        for cpu in [56.0f32, 57.5, 32.0, 6.0, 9.0, 0.0] {
            w.f32(cpu);
        }
        w.u32(2).u32(1).u8(0b0_1101);
        w.f32(0.75).f32(0.5).f32(1.5);
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
        assert_eq!(m.stage_peaks, [0.75, 0.5, 1.5]);
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

    fn hex(h: &str) -> Vec<u8> {
        (0..h.len() / 2).map(|i| u8::from_str_radix(&h[2 * i..2 * i + 2], 16).unwrap()).collect()
    }

    fn greeted(dev: FakeUnit) -> Session<FakeUnit> {
        let mut s = Session::new(dev);
        s.hello("fake").unwrap();
        s
    }

    // The payloads in hex below are the examples of protocol/test_frames.txt.

    #[test]
    fn track_state_is_decoded() {
        let payload = hex("0d00526f636b2f536f6e672e6d7033020000a7420080a2435000b81ef942016400d34d423f040102cdcc4a42cdcc7c42000000000244ac00000280003b000007000000");
        let mut dev = FakeUnit::new();
        dev.send(msg::TRACK_CHANGED, flags::EVENT, 0, payload);
        let mut s = Session::new(dev);
        let mut events = Vec::new();
        for _ in 0..40 {
            events.extend(s.events().unwrap());
        }
        let [Event::Track { state: Some(t) }] = &events[..] else { panic!("{events:?}") };
        assert_eq!((t.path.as_str(), t.playing, t.place, t.length, t.tempo), ("Rock/Song.mp3", 2, 83.5, 325.0, 80));
        assert_eq!((t.own_tempo, t.sure, t.scan, t.beats_per_bar, t.snap), (124.56, true, 100, 4, true));
        assert!((t.first_beat - 0.759).abs() < 1e-6);
        assert_eq!((t.loop_state, t.loop_from, t.loop_to, t.gaps), (2, 50.7, 63.2, 0));
        assert_eq!((t.file_kind, t.file_rate, t.channels, t.kbps), (2, 44100, 2, 128));
        assert_eq!((t.tracks, t.reading, t.list_number), (59, false, 7));
        // What the window gets: "loop" is the protocol's name for the loop's state.
        let json = serde_json::to_value(t).unwrap();
        assert_eq!((json["loop"].as_u64(), json["ownTempo"].as_f64().map(|v| v as f32), json["listNumber"].as_u64()), (Some(2), Some(124.56), Some(7)));
    }

    #[test]
    fn track_state_with_fields_added_later_and_too_short() {
        let mut s = greeted(FakeUnit::with_tracks());
        // The fake appends a field this app does not know.
        let t = s.track_state().unwrap().unwrap();
        assert_eq!((t.path.as_str(), t.playing, t.scan, t.tracks, t.list_number), ("", 0, -1, 8, 3));
        // A state cut short is not taken for one.
        let short = Frame { msg_type: msg::TRACK_STATE, flags: flags::REPLY, sequence: 1, payload: hex("0d00526f636b2f536f6e672e6d70330200") };
        assert!(matches!(track_reply(&short), Err(SessionError::Malformed { request: msg::TRACK_STATE, problem: WireError::TooShort })));
    }

    #[test]
    fn older_firmware_has_no_player() {
        let mut s = greeted(FakeUnit::new());
        assert_eq!(s.track_state().unwrap(), None);
        // Any other refusal is not "older firmware".
        let mut s = Session::new(FakeUnit::with_tracks());
        assert!(matches!(s.track_state(), Err(SessionError::Device { code: ErrorCode::BadPayload, .. })));
    }

    #[test]
    fn track_list_is_fetched_in_pieces() {
        let mut s = greeted(FakeUnit::with_tracks());
        let rows = s.track_list("Rock").unwrap();
        assert_eq!(rows.len(), 8);
        assert_eq!(rows[0], TrackRow { name: "Live".into(), folder: true, rows: 12 });
        assert_eq!(rows[7], TrackRow { name: "Zebra.wav".into(), folder: false, rows: 0 });
        // Three rows fit in one of the fake's replies: asked from row 0, 3 and 6.
        let asked = s.io.requests(msg::TRACK_LIST);
        assert_eq!(asked, [hex("0400526f636b0000"), hex("0400526f636b0300"), hex("0400526f636b0600")]);
        // One reply is enough when all fit, and an empty folder ends at once.
        s.io.log.clear();
        s.io.page = 100;
        assert_eq!(s.track_list("Rock").unwrap().len(), 8);
        s.io.rows.clear();
        assert!(s.track_list("Rock").unwrap().is_empty());
        assert_eq!(s.io.requests(msg::TRACK_LIST).len(), 2);
        assert!(matches!(s.track_list("Nowhere"), Err(SessionError::Device { code: ErrorCode::NotFound, .. })));
    }

    #[test]
    fn track_list_and_find_replies_as_in_the_shared_examples() {
        let mut rows: Vec<TrackRow> = Vec::new();
        let payload = hex("03000000030004004c697665010c000900496e74726f2e7761760000000a00536f6e675f322e6d7033000000");
        let mut r = Reader::new(&payload);
        let (total, first) = (r.u16().unwrap(), r.u16().unwrap());
        let page = (0..r.u16().unwrap()).map(|_| TrackRow { name: r.str().unwrap(), folder: r.bool().unwrap(), rows: r.u16().unwrap() }).collect();
        r.done().unwrap();
        assert!(!take_page(&mut rows, total, first, page).unwrap());
        assert_eq!(rows.iter().map(|r| (r.name.as_str(), r.folder, r.rows)).collect::<Vec<_>>(), [("Live", true, 12), ("Intro.wav", false, 0), ("Song_2.mp3", false, 0)]);
        // A reply that starts after the rows the app has is refused; one that starts earlier replaces them.
        assert!(take_page(&mut rows, 9, 5, Vec::new()).is_err());
        assert!(take_page(&mut rows, 9, 1, vec![TrackRow { name: "x".into(), folder: false, rows: 0 }]).unwrap());
        assert_eq!(rows.len(), 2);
    }

    #[test]
    fn track_find_is_fetched_in_pieces() {
        let mut s = greeted(FakeUnit::with_tracks());
        let (hits, more) = s.track_find("song").unwrap();
        assert_eq!(hits, ["Rock/Song_2.mp3", "Rock/Song 3.mp3", "Rock/Song 4.mp3"]);
        assert!(more);
        s.io.page = 2;
        assert_eq!(s.track_find("song").unwrap().0.len(), 3);
        assert_eq!(s.io.requests(msg::TRACK_FIND), [hex("0400736f6e670000"), hex("0400736f6e670000"), hex("0400736f6e670200")]);
        assert_eq!(s.track_find("nothing").unwrap(), (Vec::new(), true));
    }

    #[test]
    fn track_open_and_commands() {
        use namforge_link::track_cmd;
        let mut s = greeted(FakeUnit::with_tracks());
        assert!(matches!(s.track_command(track_cmd::PLAY, 0.0, 0.0, 0.0), Err(SessionError::Device { code: ErrorCode::NotFound, .. })));
        assert!(matches!(s.track_open("Rock/Gone.mp3"), Err(SessionError::Device { code: ErrorCode::NotFound, .. })));
        let t = s.track_open("Rock/Song.mp3").unwrap();
        assert_eq!((t.path.as_str(), t.playing, t.length), ("Rock/Song.mp3", 1, 200.0));
        assert_eq!(s.track_command(track_cmd::SEEK, 83.5, 0.0, 0.0).unwrap().place, 83.5);
        assert_eq!(s.track_command(track_cmd::TEMPO, 80.0, 0.0, 0.0).unwrap().tempo, 80);
        s.track_command(track_cmd::LOOP_BARS, 33.0, 1.0, 8.0).unwrap();
        s.track_command(track_cmd::LOOP_SECTION, 50.7, 63.2, 0.0).unwrap();
        assert!(matches!(s.track_command(99, 0.0, 0.0, 0.0), Err(SessionError::Device { code: ErrorCode::BadPayload, .. })));
        assert_eq!(s.track_open("").unwrap().playing, 0);
        assert_eq!(s.io.requests(msg::TRACK_OPEN), [hex("0d00526f636b2f476f6e652e6d7033"), hex("0d00526f636b2f536f6e672e6d7033"), hex("0000")]);
        let sent = s.io.requests(msg::TRACK_COMMAND);
        assert_eq!(sent[0], hex("01000000000000000000000000"));
        assert_eq!(sent[1], hex("040000a7420000000000000000"));
        assert_eq!(sent[2], hex("050000a0420000000000000000"));
        assert_eq!(sent[3], hex("12000004420000803f00000041"), "the shared example");
        assert_eq!(sent[4], hex("0ecdcc4a42cdcc7c4200000000"));
    }

    #[test]
    fn rename_new_folder_and_delete_folder() {
        let mut s = greeted(FakeUnit::with_tracks());
        s.rename("/TRACKS/Rock/Song.mp3", "/TRACKS/Slow/Song.mp3").unwrap();
        s.folder_new("/TRACKS/Rock/Live").unwrap();
        assert_eq!(s.folder_delete("/TRACKS/Rock").unwrap(), 12);
        assert_eq!(s.io.requests(msg::FILE_RENAME), [hex("15002f545241434b532f526f636b2f536f6e672e6d703315002f545241434b532f536c6f772f536f6e672e6d7033")], "the shared example");
        assert_eq!(s.io.requests(msg::FOLDER_NEW), [hex("11002f545241434b532f526f636b2f4c697665")]);
        assert_eq!(s.io.requests(msg::FOLDER_DELETE), [hex("0c002f545241434b532f526f636b")]);
        // Older firmware refuses them; that is an answer, not a lost connection.
        let mut s = greeted(FakeUnit::new());
        assert!(matches!(s.folder_new("/TRACKS/Rock"), Err(SessionError::Device { code: ErrorCode::UnknownMessage, .. })));
    }

    #[test]
    fn an_upload_says_hello_again_when_the_unit_dropped_the_session() {
        let data: Vec<u8> = (0..40u8).collect();
        let mut dev = FakeUnit::with_tracks();
        dev.forget_at = Some(16);
        let mut s = greeted(dev);
        // Without the retry the refusal comes back as it is.
        match s.put_file_each("/TRACKS/Rock/a.mp3", &data, |_, _| {}) {
            Err(SessionError::Device { message, .. }) => assert_eq!(message, NO_SESSION),
            other => panic!("{other:?}"),
        }
        assert!(s.io.files.is_empty());

        s.hello("fake").unwrap();
        s.io.forget_at = Some(16);
        s.io.log.clear();
        s.io.event_param(1, 44);
        let (mut progress, mut seen) = (Vec::new(), Vec::new());
        s.put_file_again("/TRACKS/Rock/a.mp3", &data, |s, sent| {
            progress.push(sent);
            seen.extend(s.live_events());
        })
        .unwrap();
        assert_eq!(s.io.files, [("/TRACKS/Rock/a.mp3".to_string(), data.clone())]);
        assert_eq!((s.io.requests(msg::HELLO).len(), s.io.requests(msg::FILE_PUT_BEGIN).len(), s.io.requests(msg::FILE_PUT_END).len()), (1, 2, 1));
        // Two pieces went before the unit forgot the session; then the whole file again.
        assert_eq!(progress, [8, 16, 8, 16, 24, 32, 40]);
        // Events that came in meanwhile can be shown during the upload, once.
        assert_eq!(seen, [Event::Param { id: 1, value: 44 }]);
        assert!(s.events().unwrap().is_empty());

        // Only once: a unit that refuses again is reported.
        s.io.forget_at = Some(0);
        s.io.greeted = false;
        assert!(matches!(s.put_file_again("/TRACKS/Rock/b.mp3", &data, |_, _| {}), Err(SessionError::Device { .. })));
        assert_eq!(s.io.files.len(), 1);
    }
}
