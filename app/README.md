# Volfovich NAM Forge (desktop app)

The macOS control app for the Teensy guitar DSP unit. Plan: [`../APP.md`](../APP.md). Link protocol: [`../protocol/PROTOCOL.md`](../protocol/PROTOCOL.md).

## Layout
- `crates/namforge-link/` – the protocol: frames, payload fields, message numbers. No I/O.
- `crates/namforge-core/` – the device side: finds the unit on USB, keeps a session open, mirrors its settings, reconnects. No UI code. `examples/monitor.rs` exercises it from a terminal.
- `src-tauri/` – the Tauri shell: starts the device thread, passes its events to the window.
- `src/` – the UI (SvelteKit, TypeScript, Svelte 5). `src/lib/controls/` holds the knob, meter and the control chooser; `src/lib/blocks.ts` joins the unit's settings table with the block definitions in `../protocol/blocks.json` (shared with the unit's own UI later).

## Commands
Needs Rust (https://rustup.rs) and Node. Run from `app/`.

```
npm install                 # once
npm run tauri dev           # run the app with live reload
npm run tauri build         # build NAM Forge.app (target/release/bundle/macos)
npm run check               # type-check the UI
npm run dev                 # the UI alone in a browser (http://127.0.0.1:1420), with saved data and moving meters; add #reverb etc. to open a block
cargo test                  # Rust tests (protocol and session, no hardware needed)
cargo run -p namforge-core --example monitor 10            # print what the unit reports for 10 s
cargo run -p namforge-core --example monitor 5 mix 20      # ... and set a setting by its key
cargo run -p namforge-core --example monitor 5 preset new "My sound"   # also: recall <name>, save, rename <name>, delete <name>
cargo run -p namforge-core --example monitor 15 send ~/Downloads/amp.nam   # also: load-model <path>, load-ir <path>, delete <path on the card>
```

Only one program can hold the unit's serial port: close `pio device monitor` before starting the app, and the app before flashing or using the monitor.

`NAMFORGE_LOG=1` makes the app print one line per device event to the terminal. It also prints script errors from the window. `NAMFORGE_TUNER=1` opens the tuner 3 s after the start; with the unit's shell command `tuner test 110` given beforehand (the tuner then hears a 110 Hz sine instead of the guitar) that checks the tuner without a guitar.

## State (milestone A5 of APP.md)
The app connects by itself and shows the main screen: the library on the unit's card (presets, amp models, IRs), the signal chain as blocks (click to edit, power button or Space to switch), the drums and bass lane, the selected block's controls, input and output meters, CPU and the driver's counters. Every control is built from the unit's own settings table; a setting no block names appears under "Other settings".

The master volume is the slider beside the meters, from silent (its lowest step) to 0 dB, the DAC's full level; there is no gain above that, because the converter has no analog volume and more digital gain would only clip. It is one setting for all presets, kept in the unit's own settings (EEPROM), not in presets, so moving it never marks a preset as edited. The Output block's Level is the preset's own level and is saved with it.

Levels: each block's tile has a thin bar with the peak level at that block's output (the Levels button or the L key switches the bars off), and the open block's editor shows the held peak as a number. The chain is float, so a level above 0 dBFS between blocks does not clip; it shows red because it would clip at the output, and because the level into the amp and the delay changes how they sound. The Output tile shows the unit's output after the master volume.

Knobs: drag up and down, Shift for fine steps, scroll or arrow keys, double-click to type a value, right-click for the default.

Presets: click one in the list to recall it, or step with the header's arrows or the arrow keys. Save (Cmd-S) writes over the recalled preset, "Save as…" makes a new one at the lowest free number, Rename and Delete are under the list. When the recalled preset has unsaved edits, recalling a preset asks first (discard, save first, or cancel).

Amps and IRs: click one to load it. "Add files…" or dropping files on the window sends `.nam` and `.wav` files to the card (`/AMP` and `/IR`); a model is checked first (NAM A2 with a Lite network, 48 kHz). If the card already has a file with the same name, the app asks before replacing it. The × beside a file deletes it from the card after a confirmation.

Tuner: the header's Tuner button or the T key; Esc closes it. The guitar is muted while it is open, and the unit closes it by itself if the app goes away.

The amp and cab editors show what the loaded file is: the model's gear, loudness and input calibration with the gains the unit applies, and the IR's format, the length in use and what the unit changed.

Not in yet: an IR's waveform and frequency response, a drum song editor. Keys: 1 to 7 open the chain's blocks and 8 the drums, Space switches the open block, the arrows step through the presets, Cmd-S saves, T is the tuner, L the level bars.

The icon is drawn in `src-tauri/icons/source/icon.svg`; after changing it, render it to a 1024-pixel PNG and run `npx tauri icon <png>`.

The drum styles', kits' and songs' names come with the settings table, read once when the app connects: after adding a song to the card, reconnect to see it.

The browser preview's data is `src/lib/demo-snapshot.json`, a copy of the unit's table and state. Refresh it after the firmware's settings change:
`~/.platformio/penv/bin/python ../tools/applink_snapshot.py > src/lib/demo-snapshot.json`
