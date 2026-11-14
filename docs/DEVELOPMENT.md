# Development guide

How to work on the project without breaking it. Commands for the app are in [`app/README.md`](../app/README.md). Last checked: 13 Nov 2026.

## Setup

- Firmware: PlatformIO (`pio`). On this Mac its tools are in `~/.platformio/penv/bin`.
- App: Rust (`~/.cargo/bin`) and Node (`/opt/homebrew/bin`); run `npm install` in `app/` once.
- In a VS Code terminal, clear `NODE_OPTIONS` and `VSCODE_INSPECTOR_OPTIONS` before running the app's tools (`unset NODE_OPTIONS VSCODE_INSPECTOR_OPTIONS`); the debugger's settings make them hang.

## Everyday commands

| Task | Command |
| --- | --- |
| Build the firmware | `pio run` |
| Flash | `pio run -t upload` |
| Shell | `pio device monitor` |
| Host tests | `pio test -e native` |
| Audio code placement check | `tools/check_hot_code.sh` (after `pio run`) |
| Protocol check on the unit | `~/.platformio/penv/bin/python tools/applink_check.py` |
| Rust tests | `cargo test` in `app/` |
| UI type check | `npm run check` in `app/` |
| Run the app | `npm run tauri dev` in `app/` |
| Editor index | `pio run -t compiledb` |

## What to run after a change

| Changed | Run |
| --- | --- |
| Anything in `lib/dsp`, `lib/params`, `lib/applink` | `pio test -e native` |
| The audio path or the linker script | `pio run`, `tools/check_hot_code.sh`, then on the unit `s`: CPU as before, 0 overruns |
| Any background firmware code | `pio run`: "free for local variables" must stay at 86.8 KB (a drop of 32 KB means RAM1 code crossed a step); `mem` on the unit shows the room left and the stack's deepest use |
| `src/AppLink.cpp` or the protocol | Host tests, `cargo test`, flash, `tools/applink_check.py` |
| The settings table | See "Adding a setting" |
| The app's Rust | `cargo test` |
| The app's UI | `npm run check`, then look at it (`npm run dev` for the browser preview) |

## Rules that are easy to break

1. **The sound stays as ported.** No change to gains, levels or the order of the chain without the author asking for it. Measuring is fine.
2. **Tests on the unit put back what they touch:** every setting, the master volume, the loaded model and IR, and unsaved preset edits. `tools/applink_check.py` does this itself. The unit is the author's working instrument. Save the state first (`tools/applink_snapshot.py`), restore to the **saved values** (not to defaults or to what they are assumed to be), and compare a second snapshot with the first.
3. **One program per serial port.** Quit the app before flashing, the monitor or the Python tools. After an upload wait about 15 s for the port to return.
4. **Nothing slow in the audio interrupt:** no `Serial`, `malloc`, `new`, SD access or waiting inside `process()` or anything it calls.
5. **No added latency:** no lookahead, oversampling, linear-phase filters or FFT blocks larger than the audio block. The limiter's lookahead switch is the one agreed exception.
6. **Never list a file with interrupt code as cold** in `tools/imxrt1062_t41_cold.ld`.
7. **Ported files stay close to the original** (formatting included), so fixes can move between this project and teensy-ir-loader.
8. **New DSP gets a host test first**, then hardware.

## Recipes

### Adding a setting
1. Add a row to `kParams` and the enum in `lib/params/src/params/ParamDefs.{h,cpp}`: saved settings before the "Not saved" rows; mark `kPreset` if a preset should store it. Adding at the end of the saved ones keeps old EEPROM records valid.
2. Say what it does in `applyParam()` in `src/Params.cpp`.
3. Name it in a block of `protocol/blocks.json` so the app shows it in the right place.
4. Update the counts in `test/test_presets` if it is in presets, and the table in [CONTROL.md](CONTROL.md).
5. Flash, then refresh the app's preview data: `~/.platformio/penv/bin/python tools/applink_snapshot.py > app/src/lib/demo-snapshot.json`.

The shell command, the help line, the EEPROM slot and the app's control come by themselves.

### Adding a protocol message
1. Describe it in `protocol/PROTOCOL.md` first.
2. Number it in `lib/applink/src/applink/Messages.h` and in `app/crates/namforge-link/src/lib.rs`.
3. Handle it in `src/AppLink.cpp`; add the call in `app/crates/namforge-core/src/session.rs`.
4. Add a case to `tools/applink_check.py` and, if it has a new frame shape, to `protocol/make_test_frames.py` (then `python3 protocol/make_test_frames.py`).

### Adding an effect block
1. The algorithm goes in `lib/dsp/src/dsp/fx/` with a host test in `test/`.
2. Place it in `AudioChain::process()`, give it memory in `AudioChain.cpp`, add its settings, a stage peak for its meter, and its functions to `HOT` in `tools/check_hot_code.sh`.
3. Add the block to `protocol/blocks.json`.
4. This changes the sound's path: agree it with the author first, and measure the CPU.

### Drum kits and songs
`tools/import_kit.py`, `tools/make_kits.py`, `tools/make_songs.py`; the build runs them through `tools/pio_kits.py`. The songs' text format is in [`songs/README.md`](../songs/README.md).

## Testing on the unit without a guitar

| Aid | Use |
| --- | --- |
| `sig <Hz> <dBFS>` | A known sine instead of the guitar; read levels with `s` or the app's block meters. Master down first. |
| `tuner test <Hz>` | The tuner hears a sine |
| `tools/applink_check.py` | Every protocol message against the real unit, including file transfer; restores the sound |
| `tools/applink_snapshot.py` | The unit's table and state as JSON |
| `cargo run -p namforge-core --example monitor …` | The app's core from a terminal |
| `NAMFORGE_LOG=1`, `NAMFORGE_TUNER=1` | The app's debug switches |

## Hardware tests
Each replaces the firmware until it is flashed again: `psram_test`, `psram_deep`, `nam_a2_bench`, `nam_bench`, and `i2s_loopback` (bare board only, jumper pin 7 to pin 8).

## Commits
Work is on the branch `i2s-driver-and-hld`. Commit when the author says so; `.vscode/` stays untracked.
