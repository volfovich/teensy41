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
```

Only one program can hold the unit's serial port: close `pio device monitor` before starting the app, and the app before flashing or using the monitor.

`NAMFORGE_LOG=1` makes the app print one line per device event to the terminal.

## State (milestone A2 of APP.md)
The app connects by itself and shows the main screen: the signal chain as blocks (click to edit, power button or Space to switch), the drums and bass lane, the selected block's controls, input and output meters, CPU and the driver's counters. Every control is built from the unit's own settings table; a setting no block names appears under "Other settings".

Knobs: drag up and down, Shift for fine steps, scroll or arrow keys, double-click to type a value, right-click for the default.

Not in yet: the library drawer, preset previous / next / save, choosing a model or IR (milestones A3 and A4), names for drum styles, kits and songs (A5, shown as numbers for now). The icon is still Tauri's placeholder.

The browser preview's data is `src/lib/demo-snapshot.json`, a copy of the unit's table and state. Refresh it after the firmware's settings change:
`~/.platformio/penv/bin/python ../tools/applink_snapshot.py > src/lib/demo-snapshot.json`
