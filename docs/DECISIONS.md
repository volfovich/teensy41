# Decisions

The choices made so far, with their reasons, newest group last. Add a line when something is decided, so the reason is not lost. The fuller arguments and measurements are in [design/HLD.md](design/HLD.md) and [design/APP_PLAN.md](design/APP_PLAN.md).

## Platform and audio

| Decision | Why |
| --- | --- |
| 48 kHz only, 24-bit converters, float inside | NAM models are trained at 48 kHz, so nothing is resampled on the device |
| No PJRC Audio Library on the audio path; own I2S driver | The library is 16-bit, 44.1 kHz, 128-sample blocks (2.9 ms each) |
| The whole chain runs in the DMA interrupt, one block at a time | Lowest latency, deterministic timing; `loop()` is free for files and control |
| Block size 32 samples | 16 was tried: the amp is fine, but the convolver's cost doubles and the worst case overruns |
| No algorithm may add latency | The point of the platform; so no lookahead, oversampling, linear-phase filters or large FFT blocks |
| Worst-case CPU at or below 80 % of a block | Margin for interrupts and for cases not yet measured |
| USB is serial + MIDI, no USB audio | The core's USB audio is 16-bit / 44.1 kHz |
| Converter boards are not re-soldered for now | So no firmware mute of the DAC and the DAC stays on its normal-latency filter (costs 0.35 ms) |
| 30 Hz high-pass on the input | Removes the SD card's sub-audio thump in firmware; 40 Hz and 20 Hz were also tried |
| Overload protection mutes and sheds load | An overloaded interrupt otherwise starves `loop()` completely |

## Amp modelling

| Decision | Why |
| --- | --- |
| NAM A2-Lite only; A1 and A2-Full are refused | A2-Full is about 7× the compute; A1 is not wanted |
| Own engine instead of NeuralAmpModelerCore | 32 % CPU against 62 %, and the reference with its libraries needs 392 KB of RAM1 code. Output matches the reference to 4.8e-7. |
| `.nam` files are read directly on the device | No converter tool and no second file format; a small cache makes later loads fast |
| The amp's history lives in DTCM | In OCRAM the same code costs about 4 % more CPU |
| Models are levelled to −12 dB loudness | As in teensy-ir-loader: the amp stands in for a tube amp's line out. −18 was tried on 13 Nov 2026 and reverted. |

## Porting from teensy-ir-loader

| Decision | Why |
| --- | --- |
| Port the DSP, settings, presets, shell and drums rather than rewrite | Already portable, host-tested and designed for 48 kHz and 32-sample blocks |
| Ported files keep their formatting; only namespace, include paths and placement macros change | Fixes can move between the two projects |
| **The sound stays exactly as ported** (13 Nov 2026) | The author's reference; presets are tuned by ear against it. Two "improvements" to the levels were heard at once and reverted. Any change to the audio path is agreed first. |
| The three codec levels (line in, line out, headphones) are removed | This hardware has no such gains. Presets stay interchangeable; the EEPROM record is converted once. |
| The OLED panel, encoder and keys are not ported | The unit will get a 2.8-inch touch screen with its own design |
| Preset files stay plain text, one setting per line | Readable, tolerant of unknown lines, no JSON library on the device |

## Levels

| Decision | Why |
| --- | --- |
| Output gain (`g`, global) and preset level (`pvol`) stay one gain right after the cab | Exactly as ported |
| A separate master volume as the last gain, silent to 0 dB, global, never in presets (13 Nov 2026) | The author wanted a master outside the blocks, near the meters, that does not mark a preset as edited. At 0 dB it changes nothing, so the ported sound is intact. |
| A Mod block (chorus / flanger) after the doubler and before the delay (13 Nov 2026) | The author's choice: after the amp and cab the modulation stays clean and wide, and the delay and reverb carry it. Off, it passes its input bit for bit, so the ported sound is intact. |
| The Mod block compensates its own level, for loudness rather than peaks (13 Nov 2026) | Asked for by the author. Matching peaks would make it sound about 3 dB quieter when switched on; matching loudness keeps the volume where it was. No setting for it. |
| The Mod rate is shown in Hz, on a logarithmic knob | The author's choice; the real speed is visible, and slow rates get fine steps |
| New saved settings are appended after the last saved one | Existing EEPROM records and setting ids stay valid |
| Block level meters show peaks at each block's output | To see where a level gets too hot before it reaches the next block or the output |

## The app

| Decision | Why |
| --- | --- |
| Name "Volfovich NAM Forge", short "NAM Forge" | The author's choice |
| Tauri 2 with a Rust core and a Svelte 5 UI | A small native app; the device logic is plain Rust with no UI code and its own tests |
| One USB serial port for the shell and the app, told apart by a two-byte frame start | No second USB interface; the shell keeps working |
| The screen is built from the unit's settings table plus `protocol/blocks.json` | A new setting needs no app change; the future touch screen can share the block definitions |
| Models are sent untouched | The device extracts what it needs; the file on the card is the file that was downloaded |
| The app asks before discarding edits, replacing or deleting a file, or restoring | Asked for after the first real use |
| Personal use for now: no signing, installer or auto-update | Cheap things that help later are kept: version check in `HELLO`, clear errors, no personal paths |
| Backups go to `~/Music/NAM Forge/Backups/<date time>/`, laid out like the card | A backup is a plain folder that can be read or copied by hand |
| Restore replaces files of the same name and leaves the rest | A restore never deletes |
| The IR's waveform and frequency response are computed from the IR as the unit uses it (13 Nov 2026) | It shows what is really heard: after resampling, trimming, cutting and normalising. The app's core does the maths; the window only draws. |
| File renaming is held back (13 Nov 2026) | A renamed model or IR is no longer found by the presets that name it. Waiting for the author's choice: the unit rewrites those presets, or the app warns. |

## Working method

| Decision | Why |
| --- | --- |
| Every new algorithm gets a host test before hardware | Bugs are cheaper to find on the Mac |
| `tools/check_hot_code.sh` after linker or audio-path changes | A hot function in flash is slow without any error |
| Tests on the unit restore every setting they touch | The unit is in daily use; a test once discarded unsaved edits |
| Documents own facts: one place per fact (13 Nov 2026) | So they can be kept true; see the table in the [README](../README.md) |
