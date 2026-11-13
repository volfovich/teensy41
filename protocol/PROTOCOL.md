# NAM Forge Link Protocol

Version 1 · The single reference for both sides: the firmware (`lib/applink`, `src/AppLink.cpp`) and the app (`app/crates/namforge-link`).

The plan this belongs to is [`docs/design/APP_PLAN.md`](../docs/design/APP_PLAN.md); the app as built is in [`docs/APP.md`](../docs/APP.md). Sections 1 to 3 are implemented and tested on the firmware side (app milestone A0); the session and settings messages (A1), `SUBSCRIBE` / `METERS` (A2), presets (A3), the tuner (A5), and from A4 the lists and loading of models and IRs, `FILE_PUT_*`, `FILE_DELETE` and `RESCAN` are implemented on both sides. `MODEL_INFO`, `IR_INFO`, `IR_DATA`, `FILE_GET`, `DIR_LIST` and `CARD_INFO` too. Not implemented yet, on either side: `FILE_RENAME`, the `LOG` event, and the `BUSY` error (the numbers are reserved). Section 4 defines the messages; each group's payloads are final only once its milestone is built, and the "Since" column says which milestone that is.

---

## 1. Transport

- The device's USB serial port (the Teensy's "MIDI + Serial" USB type). Baud rate settings are ignored by USB serial.
- The port is shared with the typed command shell. Frames and text are told apart by the frame's first two bytes (§2).
- USB serial delivers bytes reliably and in order. The CRC guards against a mis-framed stream (for example after a reconnect), not against line noise.

## 2. Frame

| Field | Bytes | Notes |
| --- | --- | --- |
| Magic | 2 | `F1 D5`. Neither byte occurs in typed ASCII commands. |
| Type | 1 | Message number (§4) |
| Flags | 1 | Bit 0 reply, bit 1 error, bit 2 event (§2.1) |
| Sequence | 2 | Little-endian. Chosen by the requester, echoed in the reply. 0 in events. |
| Length | 4 | Little-endian payload byte count, 0 to 65536 |
| Payload | Length | §3 |
| CRC-32 | 4 | Little-endian. The common CRC-32 (zlib, IEEE 802.3: polynomial `EDB88320` reflected, initial value and final XOR `FFFFFFFF`) over the bytes from Type to the end of Payload. |

A frame is 14 bytes plus its payload.

### 2.1 Flags

| Bit | Name | Meaning |
| --- | --- | --- |
| 0 (`01`) | Reply | Answers the request with the same type and sequence |
| 1 (`02`) | Error | Set together with Reply: the request failed. Payload: error code (u16) and a message for the user (string). |
| 2 (`04`) | Event | Sent by the device unprompted |

A request has no flag set. Requests go from the app to the device only.

### 2.2 Telling frames from text

The receiver reads one byte at a time:

- Outside a frame, a byte other than `F1` is **text** and goes to the command shell (on the device) or is ignored (in the app).
- `F1` followed by `D5` starts a frame. `F1` followed by anything else is dropped, and that other byte is looked at afresh.
- Inside a frame nothing is text, including line ends and the magic bytes themselves.

### 2.3 Errors and getting back in step

After a bad frame the receiver cannot know where the next one starts, and on the device the bytes of a damaged frame must never reach the shell, where they could run as a command.

- A frame with a wrong CRC, or with a length over 65536 or over the receiver's buffer, is dropped.
- The receiver then **discards every byte until the line has been idle** for a while.
- Idle time: **100 ms** on the device. A frame that stops half way is abandoned after the same time.
- The sender's part: when a request gets no reply within its timeout (§2.4), wait at least **250 ms** before sending anything else.

The device's receive buffer holds a payload of at least 16384 bytes. The app never sends a larger one; for uploads, `FILE_PUT_BEGIN`'s reply gives the largest data chunk.

### 2.4 Requests, replies, timeouts

- Every request gets exactly one reply, with the same type and sequence and the Reply flag. The exception is `FILE_GET`, which is answered by a series of `FILE_DATA` replies (§4.5).
- The app sends one request at a time and waits for its reply. Events may arrive in between.
- Default timeout 1 s. Loads from the SD card (`MODEL_LOAD`, `IR_LOAD`, `PRESET_RECALL`, `RESCAN`) take longer: the app waits 6 s for them, and 15 s for `FILE_PUT_END`.
- Sequence numbers start at 1 and wrap from 65535 to 1.

### 2.5 Shared examples

`protocol/test_frames.txt` holds example frames, written by `protocol/make_test_frames.py` (a third implementation, using zlib's CRC). Both sides' tests must encode every example byte for byte and decode it back.

## 3. Payload fields

| Kind | Encoding |
| --- | --- |
| `u8`, `u16`, `u32` | Unsigned, little-endian |
| `i16`, `i32` | Two's complement, little-endian |
| `f32` | IEEE 754 single, little-endian |
| `bool` | `u8`, 0 or 1 |
| `string` | `u16` byte count, then that many bytes of UTF-8. No terminator, no zero bytes inside. |
| `bytes` | Raw bytes to the end of the payload, or with a count given by the message |
| list | `u16` count, then that many items |

A receiver rejects a payload that is too short or has bytes left over (`BAD_PAYLOAD`), except where a message says later versions may append fields.

## 4. Messages

Type numbers are in hex. "→" is a request from the app, "←" a reply or event from the device.

### 4.1 Session (since A1)

**`01 HELLO`** opens a session: from then on the device sends events. The session ends after 5 s without any frame from the app, so an idle app sends a `GET_STATE` or a `SUBSCRIBE` every second or two. Before a `HELLO` every other request is refused. The device keeps printing its usual text on the port throughout (load messages, the shell's answers); the app skips text.

| Dir | Fields |
| --- | --- |
| → | `u16` protocol version the app speaks · `string` app name and version |
| ← | `u16` protocol version the device speaks · `string` firmware version · `u16` block size in frames · `u32` sample rate · `u16` most amp models · `u16` most IRs · `u16` longest path in bytes · `u16` number of saved settings · `u16` number of all settings · `u32` hash of the settings table |

Later protocol versions may append fields to the reply; a receiver ignores what it does not know. If the device cannot serve the app's version it answers with error `VERSION`.

**`02 GET_SCHEMA`** returns the settings table, in setting-number order, so the app can build its controls.

| Dir | Fields |
| --- | --- |
| → | (empty) |
| ← | list of: `string` key ("dmix") · `string` name ("Delay mix") · `string` label ("Mix") · `i16` minimum · `i16` maximum · `i16` default · `u8` step · `u8` unit (§6) · `u8` flags (bit 0: stored in presets) · list of `string` choices (empty if none) |

The minimum and maximum are the device's current limits (the table's, narrowed to what this firmware has: the longest delay, the number of drum styles, kits and songs). The drum style, kit and song settings carry the firmware's names as their choices (the song's first choice is "off").

**`03 GET_STATE`** returns everything the app shows.

| Dir | Fields |
| --- | --- |
| → | (empty) |
| ← | list of `i16`: the value of every setting, in setting-number order · `string` loaded model path ("" if none) · `string` loaded IR path · `string` recalled preset name · `bool` preset edited · `bool` SD card present |

### 4.2 Settings (since A1)

**`10 SET_PARAM`**

| Dir | Fields |
| --- | --- |
| → | `u16` setting number · `i32` value |
| ← | `u16` setting number · `i32` the value now in force (after clamping to the setting's range) |

**`11 PARAM_CHANGED`** (event) is sent for every change of a setting's value, whoever made it: MIDI, the typed shell or a preset recall. A `SET_PARAM` from the app gets its reply and no event of its own. The device finds changes by comparing every setting with what it last reported, about 30 times a second, so a value that changes and changes back within that time is not reported.

| Dir | Fields |
| --- | --- |
| ← | `u16` setting number · `i32` value |

**`12 STATE_CHANGED`** (event) is sent when the loaded model, the loaded IR, the recalled preset, the edited mark or the card's presence changes. Same fields as the `GET_STATE` reply from the model path on.

### 4.3 Presets (since A3)

| Type | Request | Reply |
|---|---|---|
| `20 PRESET_LIST` | (empty) | list of: `string` name ("03 Lead") · `u16` number (0 if the name has none) |
| `21 PRESET_RECALL` | `string` name | (empty); `STATE_CHANGED` and `PARAM_CHANGED` events follow |
| `22 PRESET_SAVE` | (empty): saves over the recalled preset | (empty) |
| `23 PRESET_NEW` | `string` name | `string` the name it was saved under (with its number) |
| `24 PRESET_RENAME` | `string` new name, for the recalled preset | `string` the new name |
| `25 PRESET_DELETE` | `string` name | (empty) |

Names are exact ("03 Lead"). A recall loads the model and the IR before it replies, which can take a second or two. Errors: `NO_CARD`, `NOT_FOUND` (no such preset, or none recalled for save and rename), `FULL` (99 presets), `BAD_PAYLOAD` (a new name that is empty or taken), `IO`.

**`26 PRESETS_CHANGED`** (event, empty) is sent when a preset was added, deleted or renamed, whoever did it. The app then asks for the list again.

### 4.4 Amp models and IRs (since A4)

| Type | Request | Reply |
|---|---|---|
| `30 MODEL_LIST` | (empty) | list of: `string` path · `u32` size in bytes · `u32` modified time (seconds since 1970, 0 if unknown) |
| `31 MODEL_LOAD` | `string` path | (empty); `STATE_CHANGED` follows |
| `32 MODEL_INFO` | (empty): the loaded model | `string` name · `string` gear make · `string` gear model · `string` gear type · `string` tone type · `f32` sample rate · `f32` loudness · `f32` input level in dBu · `f32` output level in dBu (NaN where the file has none) · `f32` calibration gain applied in dB (NaN: calibration is off) · `f32` levelling gain applied in dB. Error `NOT_FOUND` when none is loaded. |
| `38 IR_LIST` | (empty) | as `MODEL_LIST` |
| `39 IR_LOAD` | `string` path | (empty); `STATE_CHANGED` follows |
| `3A IR_INFO` | (empty): the loaded IR | `u32` source sample rate · `u16` channels · `u16` bits per sample · `u32` frames in the file · `u32` length in use, in samples · `bool` resampled · `bool` cut at the maximum length · `f32` normalisation gain |
| `3B IR_DATA` | (empty): the loaded IR | `u16` number of samples (the length in use) · `f32` scale · that many `i16`: the IR as the unit convolves with it, at the unit's sample rate; a sample is value ÷ 32767 × scale. `NOT_FOUND` when no IR is loaded. For pictures of the IR, not for copying it. |

### 4.5 Files on the SD card (since A4)

Paths are absolute and must name a file directly inside `/IR`, `/AMP`, `/PRESETS` or `/DRUMS` (no further folders), or a `.wav` file in the root. Names that start with a dot and paths with `..` are refused. Anything else is refused with `BAD_PATH`.

After an upload or a delete the device rescans the card before it replies; the loaded model and IR keep playing and stay marked. The lists report a modified time of 0 for now, and leave out the built-in model.

**Upload.** One upload at a time.

| Type | Request | Reply |
|---|---|---|
| `40 FILE_PUT_BEGIN` | `string` path · `u32` size · `u32` CRC-32 of the whole file | `u16` largest data chunk the device accepts |
| `41 FILE_PUT_DATA` | `u32` offset · `bytes` | `u32` bytes received so far |
| `42 FILE_PUT_END` | (empty) | (empty). The device checks size and CRC, then moves the temporary file into place. On a mismatch: error `CHECKSUM`, and nothing is left on the card. |

A `FILE_PUT_BEGIN` while an upload is open abandons the earlier one. An upload with no data for 10 s is abandoned.

**Download.**

| Type | Request | Reply |
|---|---|---|
| `43 FILE_GET` | `string` path | a series of `44 FILE_DATA` replies with the request's sequence: `u32` offset · `u32` total size · `bytes`. The last one has offset + length = total size. |

**Other.**

| Type | Request | Reply |
|---|---|---|
| `45 FILE_DELETE` | `string` path | (empty) |
| `46 FILE_RENAME` | `string` from · `string` to | (empty) |
| `47 DIR_LIST` | `string` folder | list of: `string` name · `u32` size · `u32` modified time · `bool` is a folder |
| `48 CARD_INFO` | (empty) | `bool` card present · `u32` total space in KB · `u32` free space in KB (both 0 without a card). The device counts the free space on each request, which takes a moment (0.8 s measured on a 16 GB card, longer on larger ones): allow 6 s. |
| `49 RESCAN` | (empty) | `u16` IRs found · `u16` models found · `u16` presets found · `u16` drum songs found |

### 4.6 Live data (meters since A2, tuner since A5)

**`50 SUBSCRIBE`**

| Dir | Fields |
| --- | --- |
| → | `u16` meter frames per second, 0 to 60 (0 = off) |
| ← | `u16` the rate in force |

**`51 METERS`** (event)

| Dir | Fields |
| --- | --- |
| ← | `f32` input peak, linear, since the last frame · `f32` output peak · `f32` gate gain, 0 to 1 · `u32` clipped samples since start · `f32` CPU percent, whole chain, average · `f32` the same, maximum since the last frame · `f32` amp CPU percent · `f32` doubler · `f32` delay · `f32` reverb · `f32` drums · `u32` overruns since start · `u32` overload mutes since start · `u8` activity bits (0 amp running, 1 delay ringing, 2 reverb ringing, 3 drums playing, 4 output muted by overload, 5 tuner open) · 7 × `f32` the peak at each block's output since the last frame: amp, noise gate, cab (after the output gain and the preset's level), doubler, delay, reverb, mod (the mod sits between the doubler and the delay; its peak is last because it was added last). Linear; above 1.0 is over full scale, which only clips at the output. A block that is off shows the level that passes through it. (The first six were added after the first A2 firmware and the seventh with the Mod block: an app must accept a frame with none, six or seven, and ignore more.) |

**`52 TUNER_OPEN`**: → `bool` open. ← (empty). The guitar is muted while the tuner is open. A tuner the app opened is closed when its session ends.

**`53 TUNER`** (event, when the reading changes, at most 10 per second while open): `bool` a pitch was found · `f32` frequency in Hz · `u8` note (0 = C … 11 = B) · `i16` octave · `f32` cents off, −50 to +50.

### 4.7 Other (since A1)

**`60 LOG`** (event, **not implemented**): `u8` level (0 info, 1 warning, 2 error) · `string` text. What the firmware would have printed: load results, errors, overload messages.

## 5. Error codes

| Code | Name | Meaning |
| --- | --- | --- |
| 1 | `UNKNOWN_MESSAGE` | A message type this firmware does not know |
| 2 | `BAD_PAYLOAD` | The payload is too short, too long or has a bad value |
| 3 | `NOT_FOUND` | No such preset, model, IR or file |
| 4 | `NO_CARD` | The SD card is missing |
| 5 | `BAD_PATH` | A path outside the allowed folders, or too long |
| 6 | `BUSY` | A load or a transfer is already running (reserved: the firmware never sends it) |
| 7 | `IO` | The SD card reported an error |
| 8 | `CHECKSUM` | An uploaded file's size or CRC did not match |
| 9 | `FULL` | The list is at its limit, or the card is full |
| 10 | `VERSION` | The app's protocol version is not supported |

## 6. Setting units

The `unit` byte of `GET_SCHEMA`, as in the firmware's settings table (`lib/params`, `ui::Unit`).

| Value | Unit | Shown as |
| --- | --- | --- |
| 0 | None | The number, or the choice's name |
| 1 | On/off | A switch |
| 2 | Percent | `30 %` |
| 3 | Milliseconds | `380 ms` |
| 4 | Half-dB steps | value ÷ 2 with a sign: `+3.5 dB` |
| 5 | BPM | `100 BPM` |
| 6 | Bars | `2 bars` |
| 7 | dB | `-64 dB` |
| 8 | Name | A choice's name, or a name the device supplies (a drum style, kit or song) |
| 9 | Hundredths of a Hz | value ÷ 100: `0.80 Hz` |

## 7. Changing the protocol

- Adding a message type, an error code, or fields at the end of a reply that says it may grow does **not** change the protocol version.
- Anything else does: changing a field, removing a message, changing the frame.
- Update this document, `lib/applink/src/applink/Messages.h`, and `app/crates/namforge-link/src/lib.rs` together, and add an example to `make_test_frames.py` for each new message layout.
