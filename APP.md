# Volfovich NAM Forge: Desktop Control App Plan

Nov 13, 2026 · Status: A0 to A5 done and part of A6 (app icon, waiting screen, number keys, confirmations before a recall discards edits or an upload replaces a file). The app connects to the unit by itself and shows the main screen: the library (presets, amp models, IRs), the chain as blocks, the drums lane with the styles', kits' and songs' names, the selected block's knobs, input and output meters and CPU, and a tuner. Left out for now: the model and IR detail views (`MODEL_INFO`, `IR_INFO`), downloading files from the card, renaming files, folder listing, card space, and a separate device view (its numbers are in the footer). Checked: every message against the real unit; the screens as rendered in a browser with saved data; the real window's tuner with the unit's test tone (`tuner test 110`). First use by a person (13 Nov) found the output meter's CLIP label stuck on (fixed) and the tuner "not working" (not reproduced: with the test tone the real window shows the right note; open). Not yet checked: the tuner with a guitar, and a preset saved in the app recalled by a MIDI program change.

**Volfovich NAM Forge** (short name in the window title and the Dock: **NAM Forge**) is a macOS app that controls the guitar DSP unit over USB: edit the sound on a Quad Cortex style screen, manage presets, and send NAM models and IRs straight to the device.

Companion to [`HLD.md`](HLD.md) (the firmware). Items marked **(verify)** are assumptions to check before relying on them.

---

## 1. Decisions

| Question | Decision | Why |
| --- | --- | --- |
| Platform | **macOS only** | Chosen by the author. The stack below would also build for Windows and Linux later with little change. |
| App stack | **Tauri 2: web UI (TypeScript) + Rust core** | A web UI is the quickest route to a polished, animated interface. The Rust core owns the USB connection and files. The app is about 10 MB. |
| Link to the device | **USB serial for the app, USB MIDI for controllers** | The device already exposes both. Serial is fast and carries any bytes, so file transfer, full state sync and live meters are simple. MIDI SysEx would be slow and awkward for a 670 KB model file. |
| Where the code lives | **`app/` in this repository** | The protocol is defined once and used by both sides, and a firmware change and its app change land together. |
| Signal chain layout | **Fixed order, as in the firmware** | The chain is amp → gate → cab → doubler → delay → reverb. The app shows it as blocks but does not let them be reordered. |
| Name | **Volfovich NAM Forge** | Bundle identifier `com.volfovich.namforge`. The name lives in the Tauri config and one constants file, so it is easy to change. |
| Audience | **Personal use now, possibly wider later** | Version 1 runs as a local build: no code signing, notarisation, installer or auto-update. Things that are cheap now and costly to add later stay in: the version check when connecting, clear error messages, no hard-coded personal paths, a settings file for the library location. |
| Model files | **Sent untouched** | The device already picks the Lite network out of an A2 file. The app does not rewrite model files. |
| On-device TFT UI | **Similar, not identical** | The two UIs share one design language (§3.5); the desktop app uses its larger screen for more detail. |

MIDI stays exactly as it is (program change, controllers), for footswitches, controllers and DAWs. The app does not use it.

---

## 2. What the app does

### 2.1 Must have (version 1)
- **Connect automatically** when the unit is plugged in, and show a clear state: connected, disconnected, firmware too old.
- **Edit every setting** the firmware has, live, with the device as the source of truth. A change made by MIDI or the serial shell shows up in the app within a moment.
- **Presets:** list, recall, save, save as, rename, delete, reorder. Show the "edited" mark and what differs from the saved preset.
- **Amp models:** list the models on the device, load one, see its details (gear, tone type, loudness, calibration). Add `.nam` files by drag and drop or a file picker; delete and rename them.
- **IRs:** the same for `.wav` files, with a picture of the IR before it is sent.
- **Meters:** input and output level, gate state, clip count, CPU per stage, overruns.
- **Tuner:** a large tuner view; the guitar is muted while it is open.
- **Drums and bass:** style, song, kit, tempo, count-in, volumes, start and stop.

### 2.2 Should have (version 1.x)
- **Local library:** a folder on the Mac holding models, IRs and presets, with search and tags. The device is then a subset of the library.
- **Preset packs:** export a preset with its model and IR as one file; import it on another unit.
- **Backup and restore** of everything on the SD card.
- **A/B compare** between the edited sound and the saved preset.
- **Undo and redo** of edits.
- **Set list view:** a few large preset tiles for playing, with keyboard shortcuts.

### 2.3 Ideas for later
- **Firmware update from the app** (it would drive the Teensy loader).
- **MIDI mapping editor.** The mapping is a fixed table in the firmware today; it would move to a file on the SD card.
- **Level calibration wizard:** play hard, the app reads the input peak and suggests the pad and amp input settings.
- **Noise check:** the `cap` capture shown as a waveform and spectrum, for chasing hum and interference.
- **Model browser:** search and download A2 models from TONE3000 inside the app, if their service offers a way to do that **(verify)**.
- **Drum song editor.**

---

## 3. User interface

The look is in the spirit of the Quad Cortex and similar units: dark, a signal chain of blocks across the screen, large touch-friendly controls. It is our own design, not a copy of any product's screens or artwork.

### 3.1 Main screen

```
┌──────────────────────────────────────────────────────────────────────────────────┐
│ ● Connected   ◄  02 JCM800 •edited  ►   [Save] [Save as]      IN ▮▮▮▯  OUT ▮▮▯▯  CPU 56% │
├───────────┬──────────────────────────────────────────────────────────────────────┤
│ LIBRARY   │                                                                      │
│ Presets   │   ┌──────┐   ┌──────┐   ┌──────┐   ┌──────┐   ┌──────┐   ┌──────┐   │
│ Amps      │ ─►│ GATE │──►│ AMP  │──►│ CAB  │──►│ DBL  │──►│ DELAY│──►│REVERB│─► │
│ IRs       │   └──────┘   └──────┘   └──────┘   └──────┘   └──────┘   └──────┘   │
│ Drums     │                                                                      │
│           │   ┌───────────────────────────── DRUMS + BASS ─────────────────────┐ │
│ search…   │   │ ▶  rock  100 BPM   kit: rock   count-in 2   bass: E            │ │
│           │   └────────────────────────────────────────────────────────────────┘ │
│ ▸ 01 Recto├──────────────────────────────────────────────────────────────────────┤
│ ▸ 02 JCM  │  AMP · JCM 800 2203, OD808 boosted, gain 7              [on]         │
│ ▸ 03 Vint │   (Input)   (Bass)   (Middle)   (Treble)   (Presence)   (Output)     │
│           │    +0.0      +2.0      0.0       +0.5       +1.5         +1.0  dB     │
└───────────┴──────────────────────────────────────────────────────────────────────┘
```

- **Top bar:** connection state, preset name with previous and next, the edited mark, save buttons, meters and total CPU.
- **Chain:** one block per stage. Click a block to edit it below; click its power icon (or press space) to switch it on or off. A block that is off is dimmed; a block whose tail is still ringing pulses.
- **Editor panel:** the selected block's controls as knobs and switches. The amp and cab blocks also show the loaded file and a button to change it.
- **Library drawer:** presets, amp models, IRs and drum songs, with search. Drag a model onto the amp block or an IR onto the cab block to load it. Drag files from Finder onto the drawer to add them.
- **Drums lane:** transport and the main drum and bass settings; the rest opens in the editor panel.

### 3.2 Controls
- **Knobs:** drag up and down; hold Shift for fine steps; double-click to type a value; right-click to reset to the default. Each shows its value in the setting's own unit (dB, ms, %).
- **Sending while dragging:** values go to the device at most about 60 times a second, and the device's answer confirms the final value.
- **Keyboard:** arrow keys change preset, space toggles the selected block, `T` opens the tuner, `⌘S` saves, `⌘Z` undoes.

### 3.3 Other views
- **Tuner:** full-screen note name, cents needle and frequency.
- **Model details:** gear, tone type, loudness and calibration from the file; the gain the device applies.
- **IR details:** waveform and frequency response, length, sample rate, what the device will do to it (resample, trim, cut at 4096 samples).
- **Device:** firmware version, block size, SD card space, CPU per stage, overruns, overload mutes, a button to rescan the card.
- **Set list:** large preset tiles.

### 3.4 The UI is built from the device's own table
The firmware keeps every setting in one table: key, name, short label, range, default, step, unit and choices (`lib/params` `ParamDefs`). The app asks the device for that table when it connects and builds its knobs from it. A setting added to the firmware then appears in the app without an app change. The app only adds what the table cannot know: which block a setting belongs to, and how to draw it.

### 3.5 Shared design language with the TFT touch UI
The unit will later get a 2.8-inch TFT with touch. Its UI should feel like the same product, without being limited to what fits on a small screen.

| | What |
| --- | --- |
| **Shared by both** | Block names, order, colour and icon per block. Setting names, units and value formatting (both are built from the device's settings table). Preset numbering and the "edited" mark. Meter colours and ranges. |
| **Desktop only**, because it has the room | Full file names and model descriptions. The library drawer with search. Model and IR detail views. Preset lists beside the chain. Drag and drop, keyboard shortcuts, modern menus. |
| **TFT only** | Large touch targets. One block's controls per screen. No file management beyond choosing from lists. |

The shared values (block colours, icon names, the rules for formatting each unit) go in one small file under `protocol/`, so both UIs read the same definitions. The TFT UI gets its own plan once the display is chosen.

---

## 4. Architecture

```
┌──────────────────────── macOS app (Tauri 2) ────────────────────────┐        ┌──── device ────┐
│  UI: TypeScript + Svelte                                            │        │                │
│   screens, knobs, meters, drag and drop                             │        │  SerialUI      │
│        ▲ events (state, meters)        │ commands                   │        │   text shell   │
│        │                               ▼                            │  USB   │      +         │
│  Core: Rust                                                         │ serial │  AppLink       │
│   device link (find port, frames, retries) ◄──────────────────────────────►  │   binary frames│
│   device model (settings table, state, files)                       │        │                │
│   library (local folder, packs, backups)                            │        │  Params, IR /  │
│   file checks (.nam is A2-Lite, .wav readable)                      │        │  Amp / Preset  │
└─────────────────────────────────────────────────────────────────────┘        │  libraries     │
                                                                               └────────────────┘
```

- **Rust core.** Finds the device by its USB identity, opens the serial port, speaks the protocol, and keeps a copy of the device's state. It exposes commands to the UI and pushes events to it. Files never pass through the UI layer.
- **UI.** Svelte with TypeScript. Knobs and meters are drawn with SVG or canvas. It holds no device logic.
- **Firmware side: `AppLink`.** A new module next to `SerialUI`. It parses frames, calls the same functions the shell calls (`setParam`, `recallPreset`, `loadIRIndex`, `loadAmpIndex`), and reports changes. It runs in `loop()` only; the audio interrupt is untouched.

Proposed libraries, all **(verify)** at the start of the work: the `serialport` Rust crate for the port, Tauri's file dialog and drag-and-drop support, `hound` for reading WAV files, `serde_json` for `.nam` files.

---

## 5. The link protocol

### 5.1 One port, two languages
The serial port keeps its typed shell. The app's frames share the port and are told apart by their first bytes.

- A frame starts with two bytes that never occur in typed text: `0xF1 0xD5`.
- The shell's line reader hands the port to `AppLink` when it sees them, and takes it back when the frame ends.
- When the app connects it sends `HELLO`. From then on the firmware also sends events. It keeps printing its usual text (load messages, the shell's answers); the app skips text. Typed commands still work.
- The session ends after 5 s without a frame from the app.
- The firmware does not hook every place a setting can change. About 30 times a second it compares every setting and the loaded files with what it last told the app, and sends an event for each difference. So changes from MIDI, the shell and preset recalls all reach the app the same way.

### 5.2 Frame

| Field | Size | Notes |
| --- | --- | --- |
| Magic | 2 | `F1 D5` |
| Type | 1 | Message type |
| Flags | 1 | Bit 0: this is a reply. Bit 1: error reply. |
| Sequence | 2 | Set by the sender of a request, echoed in the reply |
| Length | 4 | Payload bytes, little-endian. At most 64 KB per frame. |
| Payload | n | Binary, fields little-endian, strings as length + UTF-8 |
| CRC32 | 4 | Over type through payload |

USB serial already delivers bytes reliably and in order. The CRC is there to catch a mis-framed stream after a reconnect, not line noise.

### 5.3 Messages

| Group | Message | What it does |
| --- | --- | --- |
| Session | `HELLO` | Protocol version, firmware version, block size, sample rate, limits (max models, IRs, path length), a hash of the settings table |
| | `GET_SCHEMA` | The whole settings table, so the app can build its controls |
| | `GET_STATE` | Every setting's value, the loaded model, IR and preset, the edited flag |
| Settings | `SET_PARAM` | Set one setting by number. The reply carries the value actually applied (after clamping). |
| | `PARAM_CHANGED` (event) | Sent for every change, whoever made it: the app, MIDI, the shell or a preset recall |
| Presets | `PRESET_LIST`, `PRESET_RECALL`, `PRESET_SAVE`, `PRESET_NEW`, `PRESET_RENAME`, `PRESET_DELETE` | The same operations as the shell's `pre` commands |
| Models, IRs | `MODEL_LIST`, `MODEL_LOAD`, `MODEL_INFO`, `IR_LIST`, `IR_LOAD`, `IR_INFO` | Lists with name, size and date; load by path |
| Files | `FILE_PUT_BEGIN`, `FILE_PUT_DATA`, `FILE_PUT_END` | Upload in chunks to a temporary file, checked by size and CRC32, then renamed into place. A failed upload leaves nothing behind. |
| | `FILE_GET`, `FILE_DELETE`, `FILE_RENAME`, `DIR_LIST`, `CARD_INFO`, `RESCAN` | Download (for backup), manage, free space, rebuild the lists |
| Live data | `SUBSCRIBE` | Ask for meter frames at a given rate |
| | `METERS` (event) | Input and output peak, gate gain, clip count, CPU per stage and total, overruns, overload mutes |
| | `TUNER_OPEN`, `TUNER` (event) | Open or close the tuner; note, cents and frequency about 10 times a second |
| Other | `LOG` (event) | Messages the firmware would have printed (load results, errors) |
| | `ERROR` (reply) | A code and a message for the user |

### 5.4 Rules
- **The device owns the state.** The app shows what the device reports. After any doubt (reconnect, error) it asks for the full state again.
- **Versioning.** `HELLO` carries a protocol version. A newer app talks to an older firmware where it can and says clearly when it cannot. The settings table hash tells the app when to fetch the table again.
- **Paths are restricted** to the folders the firmware uses: `/IR` (and the root, for IRs already there), `/AMP`, `/PRESETS`, `/DRUMS`. The app cannot write elsewhere on the card.
- **File names** must fit the firmware's limit (95 characters with the folder). The app shortens or asks before sending.

### 5.5 Speed
- The Teensy 4.1's USB serial moves several MB a second **(verify on this board)**, so the SD card's write speed is the limit. A 670 KB model should upload in about a second.
- Uploads run in `loop()`. The audio interrupt has a higher priority than USB and the SD card, so playing should continue during a transfer. This is **to be measured**: an upload while playing, watching for overruns and for supply noise at the ADC like the SD card's idle thump.

---

## 6. Files

### 6.1 Amp models (`.nam`)
- **Check before sending:** the file must be NAM A2 with a Lite network, at 48 kHz. An A1 file or a Full-only file is refused in the app with a plain message, before any upload.
- **Sent untouched.** An A2 file holds two networks, Full and Lite, and is typically about 670 KB. The app sends the file exactly as it is, and the device picks out the Lite network, as it does today.
- **Limits:** 64 models on the device. The app shows the count and refuses the 65th.
- **After upload:** the device rescans, and the model can be loaded at once. Its weights cache is written on first load, as today.

### 6.2 IRs (`.wav`)
- **Sent as they are.** The device already reads any common WAV format and rate, resamples to 48 kHz, trims, cuts at 4096 samples and normalises.
- **Preview in the app:** waveform and frequency response, and a note of what the device will change (for example "96 kHz, will be resampled; 1.2 s, will be cut to 85 ms").
- **Limits:** 128 IRs on the device.

### 6.3 Presets
- **On the device:** the existing plain-text files in `/PRESETS`, one setting per line plus the model's and the IR's paths. The format does not change, so presets stay compatible with teensy-ir-loader.
- **In the app:** the device's presets are listed and edited in place. Saving goes through the device, so the file on the card is always what the device wrote.
- **A preset points at files.** When a preset's model or IR is missing on the device, the app says so and offers to send it from the library.
- **Preset packs (later):** one file holding the preset text, its `.nam` and its `.wav`, for sharing and for moving between units.

### 6.4 Local library (later)
A folder, by default `~/Music/NAM Forge/` (the location is a setting), with `Models/`, `IRs/`, `Presets/` and `Backups/`. The app indexes it, shows which items are on the device, and can sync either way.

---

## 7. Firmware work

All of it is in `loop()` code; nothing in the audio path changes.

| Item | Where | Notes |
| --- | --- | --- |
| Frame parser and writer | `lib/applink` (portable, host-tested) | No Arduino includes; works on a byte stream, so tests feed it byte arrays |
| Message handlers | `src/AppLink.cpp` | Thin: decode, call the existing functions, encode the answer |
| Port sharing with the shell | `src/SerialUI.cpp` | Hand over on the magic bytes; quiet mode while the app is connected |
| Change reports | `src/Params.cpp` and the libraries | One hook where a setting, the model, the IR or the preset changes, so MIDI and shell changes reach the app too |
| File upload and download | `src/AppLink.cpp` | Temporary file, CRC, rename; path whitelist |
| Lists with size and date | `IRLibrary`, `AmpLibrary`, `PresetLibrary`, `DrumSongs` | Today they keep paths only |
| Settings table export | `lib/params` | Serialise `ParamDefs` rows |
| Meter and tuner frames | `src/AppLink.cpp` | From the values the shell's `s` and `m` already read |
| Version constant | `include/config.h` | Firmware and protocol versions |

Memory: the new code is background code, so it goes on the cold list of the linker script. One 64 KB frame buffer can live in PSRAM.

---

## 8. App work

```
app/
  src-tauri/            Rust core
    src/link/           port discovery, frames, requests with timeouts, reconnect
    src/device/         settings table, state mirror, presets, files
    src/library/        local folder index, packs, backups        (later)
    src/checks/         .nam and .wav inspection
    src/commands.rs     what the UI can call
  src/                  UI (Svelte + TypeScript)
    lib/chain/          blocks, chain view
    lib/controls/       knob, switch, selector, meter
    lib/library/        drawer, lists, drag and drop
    lib/views/          tuner, device, model and IR details
  mock-device/          a stand-in device for development and tests
protocol/
  PROTOCOL.md           the message definitions, the single reference for both sides
```

- **Mock device.** A small program that speaks the protocol and behaves like the unit (settings table, presets, file lists, moving meters). It lets the UI be built and tested without the hardware, and it is what the automated tests run against.
- **Finding the device.** By the Teensy's USB vendor and product numbers for the "MIDI + Serial" type (`16C0:0489`, read from the unit), then confirmed by `HELLO`.

---

## 9. Milestones

| M | Deliverable | Done when |
| --- | --- | --- |
| A0 ✔ | `protocol/PROTOCOL.md` and the frame code on both sides, with tests | Done: 15 firmware host tests and 12 Rust tests pass against the same example frames (`protocol/test_frames.txt`) |
| A1 ✔ | Firmware: `HELLO`, table, state, set a setting, change reports. App skeleton that connects and shows raw values. | Done. Firmware verified with `tools/applink_check.py`; the Rust core verified against the unit with its `monitor` example; the app launches, connects and receives the state. The "MIDI change shows in the app" check was done with the typed shell in place of MIDI. |
| A2 | Main screen: chain, editor panel with knobs, block on and off, meters | Every setting can be edited from the app with no audible stepping |
| A3 | Presets: list, recall, save, save as, rename, delete, edited mark | A preset saved in the app is recalled correctly by MIDI program change |
| A4 | File transfer: add, delete, rename models and IRs; load them; checks and previews | A model dragged from Finder is playing within about 3 s; a bad file is refused with a clear message |
| A5 | Tuner, drums lane, device view | – |
| A6 | Polish: animation, keyboard shortcuts, error states, app icon | Used for a full practice session without touching the serial shell |
| A7 | Later items: library, packs, backup, A/B, undo, set list | – |
| A8 | Sharing, if wanted: code signing, notarisation, an installer, an update check, Windows and Linux builds | Someone else can install and run it without help |

The mock device makes A2 to A6 largely independent of the firmware, so UI work and firmware work can overlap after A1.

---

## 10. Testing

- **Protocol:** the same set of example frames is checked by the firmware's host tests and by the Rust tests, so the two sides cannot drift apart.
- **Firmware:** host tests for the frame parser (split frames, bad CRC, oversized length, text mixed in between frames).
- **Rust core:** tests against the mock device: connect, reconnect mid-transfer, timeouts, a full state sync.
- **UI:** component tests for the knob (drag, fine, typed value, reset) and the value formatting per unit.
- **On the device:**
  - upload while playing, with overruns and the input noise watched;
  - pull the cable mid-upload, check nothing half-written remains;
  - app, MIDI controller and typed shell all changing settings at once;
  - a card with the maximum number of files.

---

## 11. Risks

| Risk / question | Impact | Mitigation |
| --- | --- | --- |
| Sharing one serial port between the shell and the app | Text and frames could be confused | Magic bytes, quiet mode, host tests with mixed input |
| SD writes during play may cause noise or overruns | Clicks while uploading | Measure at A4; if needed, mute or warn during uploads |
| The settings table is shared with teensy-ir-loader | The app's table export must not change it | Export reads the table; it does not alter it |
| The app is unsigned (personal use) | macOS warns on first run of a build made elsewhere | Built and run locally for now; signing is milestone A8 |
| Device and app versions drift | Odd failures | Version check in `HELLO`, with a clear message |
| Two things edit at once (app and MIDI) | The app shows a stale value | The device reports every change; the app never assumes its own write won |
| Scope | The later list is long | Version 1 is section 2.1 only |
