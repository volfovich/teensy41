# The app: Volfovich NAM Forge

The macOS control app as it is built. Commands and a user's guide: [`app/README.md`](../app/README.md). The original plan and screen design: [design/APP_PLAN.md](design/APP_PLAN.md). The protocol: [`protocol/PROTOCOL.md`](../protocol/PROTOCOL.md). Last checked against the code: 16 Nov 2026.

**16 Nov 2026: the backing-track player and the tracks on the card were added** (the Tracks tab, the lane, the panel; "Backing tracks" below). They were built against the protocol document (§4.8 and §4.5) and the made-up unit of the Rust tests and the browser preview, and **have not yet been run against the unit**.

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
| `namforge-core` | `app/crates/namforge-core` | Everything about the unit, with no UI code: `device.rs` (the thread that finds the unit, connects, reconnects, and executes commands), `session.rs` (one request and its reply per call), `types.rs` (what the UI sees), `files.rs` (checks before a file is sent, a backing track's path and name among them), `backup.rs` (backup and restore), `curve.rs` (an IR's waveform and frequency response). `examples/monitor.rs` drives it from a terminal. |
| Tauri shell | `app/src-tauri` | Starts the device thread, keeps a snapshot of the unit's state, forwards events to the window, offers the commands below, file and folder dialogs. |
| UI | `app/src` | The screen. `lib/device.svelte.ts` is the store that mirrors the unit; `routes/+page.svelte` is the main screen; `lib/controls/` has the knob, meter, level bar and the control chooser; `lib/Tuner.svelte` the tuner; `lib/track.svelte.ts` what the window asks of the backing-track player (and the preview's made-up one), `lib/TracksTab.svelte`, `lib/TrackLane.svelte` and `lib/TrackPanel.svelte` its tab, lane and panel. |

## How it talks to the unit

- **Finding it:** the device thread looks for a USB serial port with the Teensy's ids (16C0:0489) once a second, opens it and sends `HELLO`.
- **On connect:** it reads the settings table (`GET_SCHEMA`), the state (`GET_STATE`), the preset, model and IR lists, the card's space and the loaded files' details, and subscribes to meters. Then it asks for the backing track's state (`TRACK_STATE`): firmware that answers `UNKNOWN_MESSAGE` has no player, and the window hides everything about tracks; otherwise the top folder of `/TRACKS` is fetched.
- **The IR's pictures:** when a different IR is loaded the core fetches its samples as the unit uses them (`IR_DATA`) and computes the waveform and the frequency response itself (`curve.rs`: FFT, averaged over a sixth of an octave, 120 points from 20 Hz to 20 kHz). The window only draws them (`lib/IrGraph.svelte`).
- **While connected:** the unit sends an event when a setting changes from elsewhere (shell, MIDI, a preset), when the preset list changes, and meter frames. The app asks for the full state every 2 s as a keep-alive (and with it for the backing track's state); the unit ends a session after 5 s of silence.
- **Changing a setting:** the UI sends `set_param`; while a knob is dragged only the newest value per setting goes out, and the UI holds its own value briefly so the echo does not make the knob jump.
- **Timeouts:** 1 s for a normal reply, 6 s for a load, for opening a track and for a rename, 15 s for the end of a file transfer and for deleting a folder. A lost connection takes the app back to "searching".

### Commands the window can call

| Command | Does |
| --- | --- |
| `snapshot` | The whole mirrored state (used at start and after a reconnect) |
| `set_param` | Sets one setting by id |
| `preset` | `recall`, `save`, `new`, `rename`, `delete` |
| `card_file` | `load-model`, `load-ir`, `delete` for a path on the card |
| `pick_files`, `send_files` | The file dialog; sending `.nam` and `.wav` files to the card |
| `tuner` | Opens or closes the tuner |
| `track_list`, `track_find` | Show a folder below `/TRACKS`; find tracks in all folders by words ("" ends the search). The core remembers both and fetches them again when the unit reads its list anew |
| `track_open` | Opens a track by its path below `/TRACKS`; "" closes the open one |
| `track_command` | One of the player's commands by its number (PROTOCOL.md §4.8) with up to three values: play, pause, stop, go to a time, tempo, the track's own tempo, beats in a bar, snap, scan, the loop |
| `track_file` | `rename`, `new-folder`, `delete-folder`, `delete` for a path below `/TRACKS` |
| `pick_tracks`, `send_tracks` | The file dialog for `.wav` and `.mp3`; sending them into a folder below `/TRACKS` |
| `backup`, `pick_backup`, `restore` | Backup of the card to a folder, choosing one, sending it back |
| `ui_log` | Prints a line from the window to the terminal (with `NAMFORGE_LOG`) |

Events to the window (one channel, `device`): searching, connected, schema, state, param, files, meters, presets, tuner, models, irs, card, modelInfo, irInfo, transfer, job, track (the player's state; with no state: this firmware has no player), trackList (the rows of the folder shown), trackFound (a search's hits), refused, disconnected.

## The screen is built from the unit's table

The app has no list of settings of its own. It reads the unit's table (names, ranges, units, choices) and joins it with [`protocol/blocks.json`](../protocol/blocks.json), which says how the chain is shown: the blocks in order, each one's colour, switch, settings, which meter peak and activity bit belong to it. `lib/blocks.ts` does the join; `lib/format.ts` formats values the way the unit does. A rate in Hz gets a logarithmic knob (`controls/Knob.svelte`). A setting that decides which others are shown (a type or a mode) is drawn on a line of its own above the dials, so the dials never change lines when it is switched. A block may carry `only`, which names the settings that are shown only with certain types or modes (the Mod type's dials, the gate's release, the settings each reverb mode and delay type has; `settingShown` in `lib/blocks.ts`). The library column can be dragged wider or narrower by its edge (double-click for the usual width; remembered; the chain always keeps the room its tiles need), and names in its lists wrap instead of being cut, so long model names can be read in full. A block may carry a `note` (a line of explanation in its editor) and a `reduction` (the limiter's gain reduction, shown on its tile while it works).

- A new setting in the firmware appears in the app by itself. Until `blocks.json` names it, it shows under "Other settings".
- The unit's touch screen reads the same `blocks.json` (through a generated header, see [SCREEN.md](SCREEN.md)).

## The screen

- **Top bar:** connection, preset name with ‹ ›, Save, Save as…, Tuner, Levels, the CPU meter, input and output meters (with CLIP), the master volume slider. In a window narrower than about 1250 px the meters move to a second line.
- **CPU meter** (`controls/CpuMeter.svelte`): the bar is the average share of each audio block's time the chain takes, the mark the highest block of the last 2 s, the line the 80 % budget. Green below 70 %, yellow to 80 %, red from 80 %.
- **Left:** the library on the card in five tabs: Presets, Amps, IRs, Tracks (only with firmware that has the player), Card (free space, backup and restore).
- **Middle:** the chain as tiles (Amp, Noise gate, Cab, Mod, Delay, Reverb, Limiter), each with its switch and a level bar; under it the Drums and bass lane and the Backing track lane (the open track, play / pause, stop, place and length, tempo and loop if set).
- **Bottom:** the selected block's controls, or the backing track's panel after a click on its lane; for Amp and Cab also the loaded file's facts, and for Cab the IR's waveform and frequency response.
- **Footer:** firmware version, block size and sample rate, CPU (average and maximum), overruns, overload mutes, clipped samples.
- Dialogs ask before a recall discards unsaved edits, before a sent file replaces one, before a delete and before a restore.

## Files

- **Sending:** a `.nam` is checked first (NAM A2 with a Lite network of 1,871 weights, 48 kHz) and sent untouched to `/AMP`; a `.wav` goes to `/IR`. The unit writes to a temporary file and renames it at the end.
- **Backup:** `~/Music/NAM Forge/Backups/<date time>/`, laid out like the card (`AMP/`, `IR/`, `PRESETS/`, `DRUMS/`, root IRs at the top). The unit's cache folder is not copied.
- **Restore:** sends a chosen backup folder back. Files of the same name are replaced, presets included; other files on the card stay.

## Backing tracks

Added 16 Nov 2026; not yet run against the unit. What the player is: [SIGNAL_CHAIN.md](SIGNAL_CHAIN.md), "Backing track". The messages: PROTOCOL.md §4.8, and `FILE_RENAME`, `FOLDER_NEW`, `FOLDER_DELETE` in §4.5.

- **The state** (`TRACK_STATE`, and the event `TRACK_CHANGED` four times a second while a track plays) is mirrored as `track` in the snapshot. Between two reports the window moves the place on by itself, at the tempo's share of real time and round a loop, so the bar runs smoothly (`trackPlace` in `lib/track.svelte.ts`).
- **The lists** come from the unit's own list of the whole tree (`TRACK_LIST`, `TRACK_FIND`), fetched in pieces until all rows are in. The core keeps the folder and the search the window shows and fetches them again when the state's list number changes, when "being read" ends, and every 2 s while the unit is still reading. A folder that is gone gives way to the top one.
- **The Tracks tab** (`lib/TracksTab.svelte`): the folder as a breadcrumb (a click goes up), folders first (with their row counts), then tracks, the open one marked; a click on a track opens it on the unit. The search field asks a quarter second after the typing stops; a hit shows its name and its folder; clearing it (or Esc) returns to the folder. Each row has rename and delete; a delete asks first, and for a folder says that everything in it goes, with its row count. "Send tracks…" sends into the folder shown and asks before a track of the same name is replaced; "New folder" makes one there. While the unit reads its list the tab says so. A rename keeps the track in its folder (the message could also move it; the app does not offer that).
- **The panel** (`lib/TrackPanel.svelte`): a bar for the whole track (click or drag to go there; the loop's section on it, fainter while the loop is off; bar lines once the tempo is known, every 2nd, 4th … when they would be closer than 7 pixels), time, length, bar and beat, Play / Pause, Stop, A, B, Loop on / off, Forget loop, the loop in bars (start by beat or bar, "Here", length 1 to 32), the tempo from 50 to 150 % with what that is in BPM and a "100 %" button, the track's own tempo with ÷2, ×2 and a typed value, beats in a bar (1 to 16), snap, "Scan again", the scan's progress ("finding the tempo 40 %"; it runs only while the track does not play, and the panel says so), the volume (`track vol`), what the file is, and the count of gaps if there were any.
- **One request at a time** holds here too: while the bar or the tempo slider is dragged the window sends the newest value once per display frame, and the device thread sends only the newest that is still waiting. The window shows its own value for 0.4 s after a change, as for a knob.
- **Play and pause** use the player's commands, not the unsaved setting "Track". With the player there, `track vol` and "Track" belong to the panel and are no longer under "Other settings".
- **Sending tracks** uses `FILE_PUT_*` like every upload. Checked first: `.wav` or `.mp3`, a name that does not start with a dot, a name of at most 127 bytes, at most 255 bytes below `/TRACKS` and 263 for the whole path, a size that fits in 32 bits. The link carries about 130 KB a second (a 5 MB track takes 40 s), so each `transfer` event has the file's place in the batch (`index` of `count`) and the batch's bytes (`allSent` of `allTotal`), and the tab shows both bars. The upload's own frames keep the session open; meters, settings and the player's state that arrive meanwhile are passed to the window after every piece. If the unit refuses with "Say HELLO first" (it ended the session once in testing), the app says `HELLO` again and sends that file once more. A batch cannot be stopped once it runs.
- **Dropped files** go into the folder shown while the Tracks tab is open, else to the amp models and IRs as before.

## Working without the unit

`npm run dev` shows the UI in a browser with saved data (`app/src/lib/demo-snapshot.json`) and moving meters. The address's hash opens things, as `#block/tab/tuner/ask`: `#reverb` opens the reverb, `#amp/models/tuner` the Amps tab and the tuner, `#track/tracks` the backing track's panel and the Tracks tab. The preview has a made-up player (a small tree of tracks, one open and playing a loop at 80 %) that answers every control; it is in `lib/track.svelte.ts`, not in the saved data. Refresh the data after the settings table changes with `tools/applink_snapshot.py`.

## Debug switches

| Switch | Does |
| --- | --- |
| `NAMFORGE_LOG=1` | One line per device event, and the window's script errors and log lines, in the terminal |
| `NAMFORGE_TUNER=1` | Opens the tuner 3 s after the start |

## Not built
From the plan: renaming models and IRs (a renamed model or IR would break the presets that name it: to be decided; backing tracks and their folders can be renamed), moving a track to another folder, stopping a batch of tracks on its way, tracks in the backup, a local library on the Mac, preset packs, A/B compare, undo, a set list view, code signing and an installer. The protocol's schema hash and file-count limits are received but not used yet.
