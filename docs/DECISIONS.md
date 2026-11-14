# Decisions

The choices made so far, with their reasons, newest group last. Add a line when something is decided, so the reason is not lost. The fuller arguments and measurements are in [design/HLD.md](design/HLD.md) and [design/APP_PLAN.md](design/APP_PLAN.md).

## Platform and audio

| Decision | Why |
| --- | --- |
| 48 kHz only, 24-bit converters, float inside | NAM models are trained at 48 kHz, so nothing is resampled on the device |
| No PJRC Audio Library on the audio path; own I2S driver | The library is 16-bit, 44.1 kHz, 128-sample blocks (2.9 ms each) |
| The whole chain runs in the DMA interrupt, one block at a time | Lowest latency, deterministic timing; `loop()` is free for files and control |
| Block size 32 samples | 16 was tried: the amp is fine, but the convolver's cost doubles and the worst case overruns |
| No algorithm may add latency | The point of the platform; so no lookahead, oversampling, linear-phase filters or large FFT blocks. One exception, switched by the user: the limiter's 1 ms lookahead (13 Nov 2026). |
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
| A Mod block (chorus / flanger) after the cab (then: after the doubler) and before the delay (13 Nov 2026) | The author's choice: after the amp and cab the modulation stays clean and wide, and the delay and reverb carry it. Off, it passes its input bit for bit, so the ported sound is intact. |
| Two more Mod types, `ce2` and `phase90`, as types of the same block with the same six settings (13 Nov 2026) | Asked for by the author: a chorus in the style of the Boss CE-2 and a phaser in the style of the MXR Phase 90. Built from the pedals' published circuits, not from recordings; the settings the pedals do not have (mix, width, feedback on the ce2, depth on the phase90) stay usable, and the documents say where the pedal itself sits. One block, one type at a time, so the CPU budget is untouched. |
| A setting that does not apply to the chosen type or mode is hidden, and the two pedal types have only their pedal's dials: ce2 rate and depth, phase90 rate (13 Nov 2026) | Asked for by the author. What is hidden is held by the firmware at the pedal's own values (mono, half and half, no feedback; the phase90 is the early script-logo pedal), so a hidden value can never colour the sound. The rule lives in `protocol/blocks.json` (`only`) for both UIs. |
| Two more reverbs (plate, spring) as modes of the Reverb block and two more delays (tape, analog) as types of the Delay block (13 Nov 2026) | Asked for by the author, who chose these four from the options offered. One mode or type runs at a time, each costs no more CPU than the ported one it replaces (measured), and they use the blocks' existing memory, so neither budget moves. |
| The new reverbs and delays are separate classes behind a bank (`ReverbBank`, `DelayBank`); `Reverb80` and `StudioDelay` are not edited (13 Nov 2026) | The sound stays exactly as ported: with hall, gated and digital the bank hands the samples through, and a host test compares them bit for bit. Fixes can still go back to the old project. |
| The new effects are written from published papers (Dattorro 1997; Valimaki, Parker and Abel 2010; Raffel and Smith 2010; the tape-echo literature), with no code taken from elsewhere (13 Nov 2026) | The repository is public. HexeFX's library is MIT and was read for comparison; the ReverbSC ports (LGPL) and the GPL tape and BBD code were not used. |
| The plate's lengths are the paper's times 1.3, not 1.61 (13 Nov 2026) | So the plate and the 150 ms pre-delay fit the 148 KB the reverbs share; growing that memory would take OCRAM heap. The plate is a fifth smaller than the paper's. |
| The spring: one-multiply all-passes, 26 per spring, the two springs feeding each other, output at the far ends (13 Nov 2026) | Measured on the unit: the chip's multiply-add is the cost, and 36 two-multiply all-passes per spring took 11.6 % against the hall's 8.1 %. Two separate spring loops also rang up to 12 dB louder on single notes that fell on a resonance; one loop through both springs needs less gain for the same decay (5 dB spread measured). |
| Tape and analog delays glide to a new time; `dtype` is the last saved setting, not next to the delay's others (13 Nov 2026) | The glide (pitch bend) is part of those sounds; the digital delay keeps its crossfade. Setting ids are positions, so a new setting goes at the end. |
| The touch screen is drawn by our own small layer (`lib/ui`), not LVGL, and without a frame buffer (14 Nov 2026) | Chosen by the author. A full frame is 154 KB, which no fast memory has room for; strips of 10 KB and repainting only what changed need none, cost the audio nothing measurable, and the look is ours. The screens are portable code, so they are tested and drawn as pictures on the Mac. |
| The screen's pins: SPI0 with CS 10, DC 9, reset 5, touch CS 4 and IRQ 3; backlight at fixed brightness (14 Nov 2026) | Pin 8, the usual touch pin, is the ADC's data. No PWM dimming, so the backlight puts no switching noise on the 5 V the ADC shares. |
| The screen keeps its calibration and on/off choice in its own EEPROM record, and the meters are shared through `Meters.cpp` (14 Nov 2026) | The settings record and `AudioChain`'s interrupt code stay as ported; three readers of clear-on-read peaks would otherwise steal from each other. |
| The home screen shows the chain as one lane of seven tiles; the limiter stands beside the master volume and the drums have their own panel; the preset list opens from the preset's bar (14 Nov 2026) | Asked for by the author: the first layout's two rows did not read as a chain and the drums sat among its tiles. Eight tiles in one row would be 37 px wide; the limiter is the one to leave the row because it is not part of a preset and acts after the master volume, on the drums too. The PRESETS button gave its place to it. |
| The doubler is removed everywhere: DSP, settings, MIDI, screen, app (14 Nov 2026) | The author does not use it. With it off the chain already passed the cab's signal to the mod untouched, so the sound of everything else is the same. Older presets keep loading: a preset's unknown lines are ignored. The settings' ids after it moved down by three, so the EEPROM record got a new mark ("NFG2") and is converted once. |
| The link protocol is version 2; unit and app refuse the other's version 1 (14 Nov 2026) | The doubler's CPU value and peak left the middle of `METERS`; a mixed pair would show wrong meters without saying so. |
| The home screen's tiles show names only; no kit name, no bar number (14 Nov 2026) | The author found the first try, with each block's main value on its tile, crowded. |
| The limiter's lowest gain goes through `Meters.cpp` like the peaks (14 Nov 2026) | The screen now reads it as well as the app, and it clears on reading. |
| The core's start-up and background routines are linked into flash by name; IR load buffers and file lists are in the PSRAM (14 Nov 2026) | RAM1 code was 1.6 KB from its step and the RAM2 heap 58 KB. Nothing the audio interrupt runs or reads was moved, so the sound and the CPU figures are unchanged: 7.2 KB of code room and 109 KB of heap. `atan2f` (the bass's tuning, called in the interrupt at a note) and the delay and reverb lines (read per sample) stay where they are. |
| The chain's one-time constructors and libm's double-precision routines are linked into flash (13 Nov 2026) | The new audio code took RAM1 code over its 64 KB step (the stack lost 32 KB). Only the IR loader, itself in flash, uses doubles; moving these brought the code back under the step without touching the audio path. |
| The Mod block compensates its own level, for loudness rather than peaks (13 Nov 2026) | Asked for by the author. Matching peaks would make it sound about 3 dB quieter when switched on; matching loudness keeps the volume where it was. No setting for it. |
| The Mod rate is shown in Hz, on a logarithmic knob | The author's choice; the real speed is visible, and slow rates get fine steps |
| New saved settings are appended after the last saved one | Existing EEPROM records and setting ids stay valid |
| A brickwall limiter as the last stage, after the master volume; global, off by default (13 Nov 2026) | The author wants the output never to clip. After the master it protects what really goes to the DAC, drums included. Global because it is protection, like the master. Off by default so that nothing changes until it is switched on. |
| The limiter's lookahead is a switch, zero latency by default | The author's choice: lowest latency for playing, the cleaner 1 ms lookahead when wanted |
| The limiter limits sample peaks with a −1 dB default ceiling, no oversampling | True-peak limiting needs oversampling, which the CPU budget does not allow |
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
