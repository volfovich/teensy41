# Volfovich NAM Forge (desktop app)

The macOS control app for the Teensy guitar DSP unit. How it is built: [`../docs/APP.md`](../docs/APP.md). Plan: [`../docs/design/APP_PLAN.md`](../docs/design/APP_PLAN.md). Link protocol: [`../protocol/PROTOCOL.md`](../protocol/PROTOCOL.md).

## Layout
- `crates/namforge-link/` – the protocol: frames, payload fields, message numbers. No I/O.
- `crates/namforge-core/` – the device side: finds the unit on USB, keeps a session open, mirrors its settings, reconnects. No UI code. `examples/monitor.rs` exercises it from a terminal.
- `src-tauri/` – the Tauri shell: starts the device thread, passes its events to the window.
- `src/` – the UI (SvelteKit, TypeScript, Svelte 5). `src/lib/controls/` holds the knob, meter and the control chooser; `src/lib/track.svelte.ts` and `src/lib/Track*.svelte` are the backing-track player's tab, lane and panel; `src/lib/blocks.ts` joins the unit's settings table with the block definitions in `../protocol/blocks.json` (shared with the unit's own UI later).

## Commands
Needs Rust (https://rustup.rs) and Node. Run from `app/`.

```
npm install                 # once
npm run tauri dev           # run the app with live reload
npm run tauri build         # build NAM Forge.app (target/release/bundle/macos)
npm run check               # type-check the UI
npm run dev                 # the UI alone in a browser (http://127.0.0.1:1420), with saved data and moving meters; add #reverb etc. to open a block, #track/tracks for the backing track
cargo test                  # Rust tests (protocol and session, no hardware needed)
cargo run -p namforge-core --example monitor 10            # print what the unit reports for 10 s
cargo run -p namforge-core --example monitor 5 mix 20      # ... and set a setting by its key
cargo run -p namforge-core --example monitor 5 preset new "My sound"   # also: recall <name>, save, rename <name>, delete <name>
cargo run -p namforge-core --example monitor 15 send ~/Downloads/amp.nam   # also: load-model <path>, load-ir <path>, delete <path on the card>
cargo run -p namforge-core --example monitor 120 backup /tmp/card           # also: restore <backup folder>
cargo run -p namforge-core --example monitor 5 track list Rock              # backing tracks: list [folder], find <words>, open <path below /TRACKS>,
                                                                            #   cmd <number> [a] [b] [c] (PROTOCOL.md 4.8), send <file> [folder],
                                                                            #   rename <from> <to>, mkdir <path>, rmdir <path>, delete <path>
```

Only one program can hold the unit's serial port: close `pio device monitor` before starting the app, and the app before flashing or using the monitor.

`NAMFORGE_LOG=1` makes the app print one line per device event to the terminal. It also prints script errors from the window. `NAMFORGE_TUNER=1` opens the tuner 3 s after the start; with the unit's shell command `tuner test 110` given beforehand (the tuner then hears a 110 Hz sine instead of the guitar) that checks the tuner without a guitar.

## Using it (milestone A5 of the plan)
The app connects by itself and shows the main screen: the library on the unit's card (presets, amp models, IRs), the signal chain as blocks (click to edit, power button or Space to switch), the drums and bass lane, the selected block's controls, input and output meters, CPU and the driver's counters. Every control is built from the unit's own settings table; a setting no block names appears under "Other settings".

Levels after the cab, as in the original firmware: the Cab block has Out gain (global, −60 to +24 dB, kept in the unit's settings) and Level (the preset's own, saved with it). They are one gain right after the cab, before the mod, delay and reverb; the drums and bass are not affected by it.

The master volume is the slider beside the meters: the last gain before the DAC, from silent (its lowest step) to 0 dB, where it changes nothing. It turns everything down, drums and bass included. It is global, kept in the unit's settings, never in presets, so moving it does not mark a preset as edited.

Limiter: the last tile. Switched on, nothing goes out above its ceiling; the tile shows how far it is turning down while it works. Lookahead makes hard limiting cleaner and adds 1 ms of latency. It is one setting for all presets.

CPU: the meter at the top shows how much of each audio block's time the unit needs: the bar is the average, the mark the highest block lately, the line the 80 % budget. It turns yellow from 70 % and red from 80 %; the footer has the same numbers with the overrun count.

Levels: each block's tile has a thin bar with the peak level at that block's output (the Levels button or the L key switches the bars off), and the open block's editor shows the held peak as a number. The chain is float, so a level above 0 dBFS between blocks does not clip; it shows red because it would clip at the output, and because the level into the amp and the delay changes how they sound. The Cab tile shows the level after the output gain and the preset's level; the OUT meter in the top bar shows what goes to the DAC, after the master volume.

Knobs: drag up and down, Shift for fine steps, scroll or arrow keys, double-click to type a value, right-click for the default.

Presets: click one in the list to recall it, or step with the header's arrows or the arrow keys. Save (Cmd-S) writes over the recalled preset, "Save as…" makes a new one at the lowest free number, Rename and Delete are under the list. When the recalled preset has unsaved edits, recalling a preset asks first (discard, save first, or cancel).

Amps and IRs: click one to load it. "Add files…" or dropping files on the window sends `.nam` and `.wav` files to the card (`/AMP` and `/IR`); a model is checked first (NAM A2 with a Lite network, 48 kHz). If the card already has a file with the same name, the app asks before replacing it. The × beside a file deletes it from the card after a confirmation.

Backing tracks (added 16 Nov 2026, built against the protocol document and not yet run against the unit; shown only with firmware that has the player): the Tracks tab lists the `.wav` and `.mp3` files in `/TRACKS` on the card, one folder at a time. Click a folder to go in and the path above the list to go up; click a track to open it on the unit. The field at the top finds tracks in all folders by words in their names or their folders' names; clear it to return to the folder. "Send tracks…", or files dropped on the window while the tab is open, go into the folder shown; the link carries about 130 KB a second, so a 5 MB track takes 40 s, and the tab shows the file's progress and the batch's. "New folder" makes one in the folder shown; the pencil and the × on a row rename and delete (a folder goes with everything in it, after a confirmation).

The Backing track lane under the drums shows the open track with play / pause and stop; a click on it opens the player's panel: a bar for the whole track (click or drag to go there, arrow keys for 5 s), A and B to mark a loop where the track is, Loop to switch it off and on (its points are kept with the track), "Forget loop", the loop set in bars, the tempo from 50 to 150 % at the same pitch, the track's own tempo as the unit found it (÷2, ×2 or a typed value correct it; "Scan again" finds it anew, which only runs while the track is not playing), beats in a bar, snapping of loop points to beats, and the track's volume. The 9 key opens the panel and Space then plays and pauses.

Tuner: the header's Tuner button or the T key; Esc closes it. The guitar is muted while it is open, and the unit closes it by itself if the app goes away.

The amp and cab editors show what the loaded file is: the model's gear, loudness and input calibration with the gains the unit applies, and the IR's format, the length in use and what the unit changed. The Cab editor also draws the IR as the unit uses it: its waveform and its frequency response from 20 Hz to 20 kHz.

Backup and restore (the Card tab): "Back up now" copies every amp model, IR, preset and drum song from the card into a new folder under `~/Music/NAM Forge/Backups/`, named by date and time and laid out like the card (`AMP/`, `IR/`, `PRESETS/`, `DRUMS/`, IRs from the card's root at the top). "Restore…" lets you pick such a folder, says what it holds, and after a yes sends the files back: files of the same name are replaced, others on the card are left alone.

The Card tab shows the card's free space. Not in yet: renaming models and IRs, a drum song editor. Keys: 1 to 7 open the chain's blocks, 8 the drums and 9 the backing track, Space switches the open block (or plays and pauses the track), the arrows step through the presets, Cmd-S saves, T is the tuner, L the level bars.

The icon is drawn in `src-tauri/icons/source/icon.svg`; after changing it, render it to a 1024-pixel PNG and run `npx tauri icon <png>`.

The drum styles', kits' and songs' names come with the settings table, read once when the app connects: after adding a song to the card, reconnect to see it.

The browser preview's data is `src/lib/demo-snapshot.json`, a copy of the unit's table and state. Refresh it after the firmware's settings change:
`~/.platformio/penv/bin/python ../tools/applink_snapshot.py > src/lib/demo-snapshot.json`
