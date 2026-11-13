# Volfovich NAM Forge (desktop app)

The macOS control app for the Teensy guitar DSP unit. Plan: [`../APP.md`](../APP.md). Link protocol: [`../protocol/PROTOCOL.md`](../protocol/PROTOCOL.md).

## Layout
- `crates/namforge-link/` – the protocol: frames, payload fields, message numbers. No I/O.
- `crates/namforge-core/` – the device side: finds the unit on USB, keeps a session open, mirrors its settings, reconnects. No UI code. `examples/monitor.rs` exercises it from a terminal.
- `src-tauri/` – the Tauri shell: starts the device thread, passes its events to the window.
- `src/` – the UI (SvelteKit, TypeScript, Svelte 5).

## Commands
Needs Rust (https://rustup.rs) and Node. Run from `app/`.

```
npm install                 # once
npm run tauri dev           # run the app with live reload
npm run tauri build         # build NAM Forge.app (target/release/bundle/macos)
npm run check               # type-check the UI
cargo test                  # Rust tests (protocol and session, no hardware needed)
cargo run -p namforge-core --example monitor 10            # print what the unit reports for 10 s
cargo run -p namforge-core --example monitor 5 mix 20      # ... and set a setting by its key
```

Only one program can hold the unit's serial port: close `pio device monitor` before starting the app, and the app before flashing or using the monitor.

`NAMFORGE_LOG=1` makes the app print one line per device event to the terminal.

## State (milestone A1 of APP.md)
The app connects by itself, shows the loaded preset, model and IR, and lists every setting from the unit's own table with a slider, switch or selector. Changes go both ways. It is a plain list for now; the chain view comes at A2. The icon is still Tauri's placeholder.
