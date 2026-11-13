# The app: Volfovich NAM Forge

The macOS control app as it is built. Commands and a user's guide: [`app/README.md`](../app/README.md). The original plan and screen design: [design/APP_PLAN.md](design/APP_PLAN.md). The protocol: [`protocol/PROTOCOL.md`](../protocol/PROTOCOL.md). Last checked against the code: 13 Nov 2026.

## Parts

```
┌──────────────── NAM Forge.app (Tauri 2) ─────────────────┐
│  Window: Svelte 5 UI            app/src/                 │
│     ▲ events ("device")   │ commands (invoke)            │
│  Tauri shell              ▼        app/src-tauri/        │
│  namforge-core: device thread, session, backup, checks   │
│  namforge-link: frames, fields, message numbers          │
└──────────────────────────┬───────────────────────────────┘
                           │ USB serial (frames)
                     the unit's AppLink
```

| Part | Path | Owns |
| --- | --- | --- |
| `namforge-link` | `app/crates/namforge-link` | The protocol on the wire: frame encoding and parsing with CRC, payload reader and writer, message numbers, error codes. No I/O. Shares `protocol/test_frames.txt` with the firmware's tests. |
| `namforge-core` | `app/crates/namforge-core` | Everything about the unit, with no UI code: `device.rs` (the thread that finds the unit, connects, reconnects, and executes commands), `session.rs` (one request and its reply per call), `types.rs` (what the UI sees), `files.rs` (checks before a file is sent), `backup.rs` (backup and restore), `curve.rs` (an IR's waveform and frequency response). `examples/monitor.rs` drives it from a terminal. |
| Tauri shell | `app/src-tauri` | Starts the device thread, keeps a snapshot of the unit's state, forwards events to the window, offers the commands below, file and folder dialogs. |
| UI | `app/src` | The screen. `lib/device.svelte.ts` is the store that mirrors the unit; `routes/+page.svelte` is the main screen; `lib/controls/` has the knob, meter, level bar and the control chooser; `lib/Tuner.svelte` the tuner. |

## How it talks to the unit

- **Finding it:** the device thread looks for a USB serial port with the Teensy's ids (16C0:0489) once a second, opens it and sends `HELLO`.
- **On connect:** it reads the settings table (`GET_SCHEMA`), the state (`GET_STATE`), the preset, model and IR lists, the card's space and the loaded files' details, and subscribes to meters.
- **The IR's pictures:** when a different IR is loaded the core fetches its samples as the unit uses them (`IR_DATA`) and computes the waveform and the frequency response itself (`curve.rs`: FFT, averaged over a sixth of an octave, 120 points from 20 Hz to 20 kHz). The window only draws them (`lib/IrGraph.svelte`).
- **While connected:** the unit sends an event when a setting changes from elsewhere (shell, MIDI, a preset), when the preset list changes, and meter frames. The app asks for the full state every 2 s as a keep-alive; the unit ends a session after 5 s of silence.
- **Changing a setting:** the UI sends `set_param`; while a knob is dragged only the newest value per setting goes out, and the UI holds its own value briefly so the echo does not make the knob jump.
- **Timeouts:** 1 s for a normal reply, 6 s for a load, 15 s for the end of a file transfer. A lost connection takes the app back to "searching".

### Commands the window can call

| Command | Does |
| --- | --- |
| `snapshot` | The whole mirrored state (used at start and after a reconnect) |
| `set_param` | Sets one setting by id |
| `preset` | `recall`, `save`, `new`, `rename`, `delete` |
| `card_file` | `load-model`, `load-ir`, `delete` for a path on the card |
| `pick_files`, `send_files` | The file dialog; sending `.nam` and `.wav` files to the card |
| `tuner` | Opens or closes the tuner |
| `backup`, `pick_backup`, `restore` | Backup of the card to a folder, choosing one, sending it back |
| `ui_log` | Prints a line from the window to the terminal (with `NAMFORGE_LOG`) |

Events to the window (one channel, `device`): searching, connected, schema, state, param, files, meters, presets, tuner, models, irs, card, modelInfo, irInfo, transfer, job, refused, disconnected.

## The screen is built from the unit's table

The app has no list of settings of its own. It reads the unit's table (names, ranges, units, choices) and joins it with [`protocol/blocks.json`](../protocol/blocks.json), which says how the chain is shown: the blocks in order, each one's colour, switch, settings, which meter peak and activity bit belong to it. `lib/blocks.ts` does the join; `lib/format.ts` formats values the way the unit does. A rate in Hz gets a logarithmic knob (`controls/Knob.svelte`).

- A new setting in the firmware appears in the app by itself. Until `blocks.json` names it, it shows under "Other settings".
- The planned touch screen on the unit is meant to read the same `blocks.json`.

## The screen

- **Top bar:** connection, preset name with ‹ ›, Save, Save as…, Tuner, Levels, input and output meters (with CLIP), the master volume slider.
- **Left:** the library on the card in four tabs: Presets, Amps, IRs, Card (free space, backup and restore).
- **Middle:** the chain as tiles (Amp, Noise gate, Cab, Doubler, Mod, Delay, Reverb), each with its switch and a level bar; under it the Drums and bass lane.
- **Bottom:** the selected block's controls; for Amp and Cab also the loaded file's facts, and for Cab the IR's waveform and frequency response.
- **Footer:** firmware version, block size and sample rate, CPU (average and maximum), overruns, overload mutes, clipped samples.
- Dialogs ask before a recall discards unsaved edits, before a sent file replaces one, before a delete and before a restore.

## Files

- **Sending:** a `.nam` is checked first (NAM A2 with a Lite network of 1,871 weights, 48 kHz) and sent untouched to `/AMP`; a `.wav` goes to `/IR`. The unit writes to a temporary file and renames it at the end.
- **Backup:** `~/Music/NAM Forge/Backups/<date time>/`, laid out like the card (`AMP/`, `IR/`, `PRESETS/`, `DRUMS/`, root IRs at the top). The unit's cache folder is not copied.
- **Restore:** sends a chosen backup folder back. Files of the same name are replaced, presets included; other files on the card stay.

## Working without the unit

`npm run dev` shows the UI in a browser with saved data (`app/src/lib/demo-snapshot.json`) and moving meters. The address's hash opens things, as `#block/tab/tuner/ask`: `#reverb` opens the reverb, `#amp/models/tuner` the Amps tab and the tuner. Refresh the data after the settings table changes with `tools/applink_snapshot.py`.

## Debug switches

| Switch | Does |
| --- | --- |
| `NAMFORGE_LOG=1` | One line per device event, and the window's script errors and log lines, in the terminal |
| `NAMFORGE_TUNER=1` | Opens the tuner 3 s after the start |

## Not built
From the plan: renaming files (a renamed model or IR would break the presets that name it: to be decided), a local library on the Mac, preset packs, A/B compare, undo, a set list view, code signing and an installer. The protocol's schema hash and file-count limits are received but not used yet.
