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
| The backing track is read, decoded (and later resampled and time-stretched) in `loop()`; the interrupt only copies a block from a queue (15 Nov 2026) | The interrupt is at its 80 % budget and may not touch the card. The price: 2.7 s of queue, and a seek or loop change on a playing track is heard after 50 ms |
| The interrupt reads the backing track's queue from the PSRAM (15 Nov 2026) | Until then nothing the interrupt reads was in the PSRAM. The queue is 512 KB (RAM2 has about 100 KB free), and it is read front to back, 128 bytes a block: the use the PSRAM measurements allow. It stays the only exception |
| MP3 decoder: minimp3 (15 Nov 2026) | One header file, public domain (CC0), no allocation, fixed memory (6.7 KB of state, 17 KB of stack per frame). Built for layer 3 only. Vendored in `lib/dsp/src/dsp/player/vendor/`, unchanged |
| An MP3 is read through once each time it is opened, for its length and seek table (15 Nov 2026) | They come from the frame headers alone: 0.35 s for a 4-minute file, so they are not kept in the file beside the track |
| The beat grid is one steady tempo and a first beat, not a list of beat times (15 Nov 2026) | Changed from the plan, which named beat tracking by dynamic programming. Backing tracks are mostly made to a click, and for those two numbers fitted over the whole track are more exact (5 ms after 4 minutes in the host tests) and can be corrected by hand (half, double, a value; "bar 1 is here" too until 15 Nov 2026, removed as of no use to the author). A track with a wandering tempo gets a grid marked unsure; free loop points remain |
| The scan runs while the track is not playing, a few ms per pass of `loop()` (15 Nov 2026) | It has to decode the whole file (7 s per minute of MP3) and uses the one decoder. So a new track can be played at once, without waiting for the scan; the tempo shows up after it has been open and still for long enough. A second decoder for scanning during play would cost 11 KB of RAM2, which has 28 KB left |
| What is known about a track is a text file beside it (`song.mp3.trk`) (15 Nov 2026) | Like the presets: readable, editable, survives firmware changes (unknown lines are skipped), travels with the card. The audio file's size in it tells a replaced file from the same one |
| Tempo change by WSOLA in the time domain, with hits kept whole (15 Nov 2026; replaced the same day, see the next row) | Cheap enough for `loop()` beside the decoder and no "phasy" sound on steady notes, unlike a phase vocoder. Plain WSOLA smears or doubles drum hits when slowing down, so pieces are placed around sudden rises in level. The first version (40 ms pieces overlapping by half) the author heard as "very nasty artifacts" on real songs; see the next row |
| The tempo change is a phase vocoder with locked phases, and hits passed through unstretched (15 Nov 2026, third version) | The second version (WSOLA with 60 ms pieces and 10 ms joins) the author heard as "terrible ticking", even at a change of 1 %: it made joins it did not need (16 % of the pieces moved at 99.4 %), and any join in a dense mix can tick. Looked into: WSOLA / SoundTouch (the same class), Rubber Band (GPL, far too heavy), Signalsmith Stretch (MIT, but allocates from the heap and is built for desktop CPUs), the phase-gradient vocoder of "Phase Vocoder Done Right" (a heap sort and trigonometry per bin). Chosen: Laroche and Dolson's identity phase locking, the lightest method without joins, with Rubber Band's idea for drums (windows of a hit are laid down unstretched with their own phases, the time is made up after). Written for this unit: sum and difference of the sides in one complex 2048-point FFT, no maths-library calls per peak. Host tests: tones at pitch and level without clicks; hits single, at their level, 5 ms early at 85 %, 21 ms at 50 %. Known limits: 23 Hz bins blur the lowest bass notes' harmonics (4096 points would not fit RAM2), and what the phase reset at a hit does to sustained sounds. Not yet judged by the author |
| At a drum hit the phases below 150 Hz are reset only if the level rose there too (15 Nov 2026) | The author heard the bass smear at times. A reset at every hit broke a bass note's phase each time (host test: a 60 Hz note under bursts of hiss measured 0.028 of its 0.3 as one tone; now 0.300). A kick drum, whose rise is in the bass, still gets the full reset, so it keeps its shape. The other cause of bass smear, the 43 ms window's 23 Hz bins, is not touched by this: that needs a second, longer window for the low band (about 15 % more of `loop()` and 25 KB of RAM2, which has 18 KB free) |
| The phase vocoder's code is in RAM1, its input in the PSRAM (15 Nov 2026) | A window is 1 to 2 ms of work, several audio interrupts long. First measure, from flash and with library trigonometry: `loop()` 95 % busy and gaps. With the code in RAM1 (4 KB), fast arc tangent and table sines, the CPU's bit-reverse instruction and two FFT passes per sweep: 58 % at 80 % tempo, 73 % at 150 %, no gaps (typical preset). Its 16 KB of input is read once per window, which is what the PSRAM is for; RAM2's heap is 18 KB |
| A tempo change reaches the player once the value has stood still for 0.25 s (15 Nov 2026) | Each change while playing makes the player start anew (a seek, the queue cut, the stretcher emptied). The screen's held button gives ten changes a second: the author heard that, and the unit counted 161 gaps |
| The MP3 decoder's code is in RAM1 (15 Nov 2026) | From flash the tempo scan decoded 9 times as fast as the music plays, from RAM1 18 times (an 8.5-minute song: 55 s instead of 110 s; the author found the wait very long). It costs the next 32 KB step of RAM1: 47.6 KB are left for the stack, whose deepest use is 23.9 KB. The scan's, the resampler's and the tempo change's own loops were tried there too and gained nothing, so they stay in flash |
| The tempo change's buffers are in RAM2, not the PSRAM (15 Nov 2026) | Measured on the unit with the WSOLA version, which searched its input in place (the phase vocoder's input, read once per window, is in the PSRAM again: three rows up): with its input in the PSRAM a 44.1 kHz MP3 at 150 % took over 90 % of `loop()`'s time; in RAM2 (and with a lighter search) 46 %. The PSRAM is right for data that passes through once (the queue, file buffers), not for data worked on in place |
| The whole tree below `/TRACKS` is kept as one list in the PSRAM (15 Nov 2026) | The author's tracks are in folders and will be thousands. With the list in memory a folder opens at once and a search over all folders takes no card access; 545 KB of the PSRAM's 6.6 MB free holds 8192 folders and tracks. Reading one folder from the card each time it is shown was the other way: less memory, but a search would walk the card |
| The list is read breadth-first, in 3 ms pieces from `loop()` (15 Nov 2026) | Breadth-first keeps one folder open on the card at a time and makes each folder's rows one sorted run, with no recursion. In pieces, power-on does not wait for a big card and the screen stays alive; what has been read can be used meanwhile |
| A track is known by its path, not by a number (15 Nov 2026) | Numbers in the list change when the card is read again. The open track is reopened by its path after a remount without waiting for the list. The shell's `track <n>` is a row of the list last shown, as on the screen |
| A search matches words in the track's name or its folders' names (15 Nov 2026) | Folders are mostly artists or styles, so "metallica one" should find the track; `_` counts as a space because many file names use it. Every word must be there; no ranking: hits come in the list's order, 256 at most |
| The track list keeps page buttons, which now repeat when held (15 Nov 2026) | The author's choice over dragging the list: a drag on the resistive panel is less sure than a tap. Folders and FIND keep any one list short |
| The backing track's queue holds 16-bit samples (15 Nov 2026) | Half the memory and half the copying of 32-bit; a backing track is finished, mastered music, where 16 bits are the CD's |
| The backing track joins beside the drums and has its own volume (15 Nov 2026) | It is not the guitar's sound: no output gain, preset level or effects on it, and the master volume and limiter act on it like on everything else |
| LOOP switches the backing track's loop off and on; its points stay and are saved with the track either way (15 Nov 2026) | Asked for by the author. Before, LOOP forgot the points, and a section had to be marked again to get it back. Forgetting them is left to the shell (`track loop clear`); marking A or B again moves a point |
| The loop can be set in bars: a start as bar and beat, a length of 1, 2, 4 ... 32 bars, every change applied at once (15 Nov 2026) | Asked for by the author. The start moves by beats as well as by bars because the unit's bars count from the first beat the scan found, which need not be the song's bar 1. Applied at once, the loop is heard while it is moved |
| "Bar 1 is here" removed (15 Nov 2026) | The author: "useless for me". Gone: the screen's BAR 1 button, the shell's `track bar`, the `.trk` file's `bar` line (old lines are skipped). The price: a grid the scan put on the off-beats can only be moved with `track bpm <value>` given at a beat |
| The app gets the tracks' lists from the unit's own list in memory (`TRACK_LIST`, `TRACK_FIND`), not from the card's folders (`DIR_LIST`) (16 Nov 2026) | The list is there already, sorted as the unit shows it, and a search over thousands of tracks takes no card access. A reply holds 16 KB, so a long folder is asked for in pieces |
| The backing track's state goes to the app as one message, sent at a change and four times a second while the place moves (16 Nov 2026) | One layout for the reply and the event; the app moves the place itself in between. The device finds a change by comparing a checksum of the state without the place |
| The player's commands are one message with a command number and three numbers (16 Nov 2026) | Eighteen small commands with at most three values each; a new one needs no new message. Volume and play / pause stay settings, as the touch screen and MIDI have them |
| Renaming, moving, new folders and removing folders exist below `/TRACKS` only (16 Nov 2026) | Presets name models and IRs by their path, so a renamed one would break them (still open, see STATUS); nothing names a backing track. A track's `.trk` file moves or goes with it |
| After a change below `/TRACKS` only the list of tracks is read again; the card is not mounted anew (16 Nov 2026) | A remount lets go of every open file and reads all five lists; for one track sent among many that is seconds each time. The open track stays open; one that is removed, replaced or moved is closed first |
| Removing a folder removes everything in it, and the app asks first (16 Nov 2026) | Emptying a folder file by file over the link is a request and a list read per file. The unit does not ask: the question belongs to whoever can show it |

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
