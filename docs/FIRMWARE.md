# Firmware structure

How the firmware is put together. For the audio path itself see [SIGNAL_CHAIN.md](SIGNAL_CHAIN.md); for commands and settings see [CONTROL.md](CONTROL.md). Last checked against the code: 13 Nov 2026.

## Two contexts

Everything in the firmware runs in one of two places.

| | Audio interrupt | `loop()` |
| --- | --- | --- |
| What runs | `audioCallback()` in `src/main.cpp`: converts the ADC's words to float, calls `AudioChain::process()`, converts back | Serial shell, app link, USB MIDI, meters, tuner, SD card, loading of models, IRs and presets, the delayed EEPROM save, overload handling |
| When | Every block: 1,500 times a second (32 samples at 48 kHz), from the I2S driver's receive DMA interrupt, priority above USB and SD | Whenever the interrupt is not running |
| Rules | Must finish within one block (0.67 ms = 400 k CPU cycles). No `Serial`, no `malloc`/`new`, no SD access, nothing that waits. | May take as long as it needs; the audio never stops for it |

How they talk: each effect's setters are written to be called from `loop()` while `process()` runs in the interrupt (single values, or a spare set that is switched in). Each header states its own rule. Meters and peaks go the other way through `volatile` values.

## Modules

```
src/main.cpp        setup(), loop(), the block callback, overload handling, test signal, raw capture
src/AudioChain.*    the signal chain and its memory
src/Params.*        get and set any setting; what each setting does to the chain
src/Settings.*      the settings record in EEPROM
src/IRLibrary.*     the card's IR list, loading an IR
src/AmpLibrary.*    the card's amp model list, loading a model, the weights cache
src/PresetLibrary.* presets on the card: list, recall, save, rename, delete, "edited" mark
src/DrumSongs.*     drum songs: built in and MIDI files from the card
src/TrackLibrary.*  backing tracks: the files in /TRACKS, playing one (the file is read in loop())
src/Tuner.*         the tuner's pitch detection in loop()
src/SerialUI.*      the text shell on USB serial
src/AppLink.*       the app's binary protocol on the same USB serial port
src/MidiControl.*   USB MIDI: program change and controllers
src/SdSource.h      an SD file as a byte stream for the portable loaders
include/config.h    compile-time settings and hardware constants

lib/i2s32/          the I2S driver (Teensy only)
lib/dsp/            portable DSP, namespace dsp: nam/, conv/, fx/, drums/, player/
lib/params/         the settings table and the preset file format (portable)
lib/applink/        the link protocol's frames and fields (portable)
```

`lib/dsp`, `lib/params` and `lib/applink` have no Arduino or Teensy includes, so they build and are tested on the Mac (`pio test -e native`).

### Who calls whom

```
serial text ──► SerialUI ─┐
app frames ───► AppLink  ─┼─► Params (setParam) ─► AudioChain's setters ─► heard in the next block
USB MIDI ─────► MidiControl┘         │
                                     └─► Settings (EEPROM, saved a moment later)

PresetLibrary ─► Params (every preset setting) + AmpLibrary (model) + IRLibrary (IR)
AmpLibrary ────► dsp::readNamA2Lite ─► AudioChain.amp()
IRLibrary ─────► dsp IR loader ──────► AudioChain.engine()
TrackLibrary ──► dsp::TrackPlayer (loop(): file ─► queue) ─► AudioChain.track() (interrupt: queue ─► output)
```

One serial port carries two languages: text lines for the shell and binary frames for the app. A frame starts with two bytes that cannot begin a command (`0xF1 0xD5`); `serviceSerial()` hands each byte to `appLinkFeed()` first.

## The I2S driver (`lib/i2s32`)

- SAI1 with one eDMA channel each for receive and transmit; the Teensy is the clock master (MCLK 12.288 MHz, BCLK 3.072 MHz, 48 kHz exactly from PLL4).
- 32-bit slots with the 24-bit sample in the top bits. Conversion to and from float is in the portable `dsp/Convert.h` (saturating, exact round trip).
- Low FIFO watermarks, so the path adds about 2 frames beyond the two blocks of buffering.
- Sets flush-to-zero and default-NaN for the interrupt, so denormals cost nothing.
- Counts overruns (a block that took too long) and FIFO errors; `s` in the shell shows them.
- **Overload protection:** after 16 overruns in a row it sends silence and stops calling the callback, which gives `loop()` the CPU back. `serviceFault()` in `main.cpp` then stops the drums, resumes after 1 s, and on a second trip within 10 s switches the mod, delay and reverb off.

## Settings

Everything adjustable is a row in one table: `lib/params/src/params/ParamDefs.cpp` (key, name, range, default, step, unit, choices, "in presets"). The shell's commands and help, the EEPROM record, the preset files, the MIDI map and the app's whole screen are generated from it.

- 67 values: 63 saved, 4 not (drums playing, loop from, loop to, track playing). 40 are in presets.
- Values are whole numbers; decibel settings are in half-dB steps, the Mod rate in hundredths of a Hz.
- A few ranges are narrowed at run time by `paramLimits()` (the number of drum styles, kits and songs, the song's bars).
- `Params.cpp`'s `applyParam()` is the one place that says what a setting does.

**EEPROM record** (`Settings.cpp`): a magic number ("NFG2"), the count of values, the last IR, model and preset paths, then the 63 values. Saved a moment after the last change. A record from newer or older firmware keeps the values it has and takes defaults for the rest. teensy-ir-loader's record ("IRLR", which also held three codec levels) is converted once at boot.

**Preset files** (`lib/params` `PresetFile.*`, `PresetLibrary.cpp`): plain text in `/PRESETS`, named `NN Name.txt`. One line per setting as you would type it in the shell, plus the model's and the IR's paths:

```
amp model /AMP/JCM 800.nam
ir /IR/OH Hair-Metal.wav
amp on
amp in +3.5
dtime 380
```

Unknown lines are ignored, missing ones get the default, so files move between firmware versions and to and from teensy-ir-loader. A preset is "edited" when the live values differ from its file.

## Files on the card

| What | Where | Loading |
| --- | --- | --- |
| Amp models | `/AMP/*.nam` (up to 64) | The JSON is streamed through `dsp::readNamA2Lite`, which pulls out the A2-Lite weights and the file's facts. Files that are not A2 are refused. A 7.7 KB cache per model in `/AMP/.cache` makes later loads fast. |
| IRs | `/*.wav`, `/IR/*.wav` (up to 128) | Any common WAV format and rate; resampled, trimmed and normalised at load |
| Presets | `/PRESETS/*.txt` | Read line by line |
| Drum songs | `/DRUMS/*.mid` | General MIDI drum files |
| Backing tracks | `*.wav`, `*.mp3` in `/TRACKS` and the folders below it, up to 8 deep (up to 8192 folders and tracks together) | The list (`dsp::TrackIndex`, in the PSRAM) is read from the card after it is mounted, a folder at a time and 3 ms per pass of `loop()`, so the unit does not wait for it: 0.6 ms per entry measured (59 tracks in 9 folders: 41 ms of reading, 75 ms in all; several seconds for thousands, longer while a track plays). Left out, and said so in the shell: names of 128 characters or more, paths of 256 or more, hidden names. A track is known by its path. Not loaded: read a piece at a time while playing (`dsp::WavStream`, `dsp::Mp3Stream`), resampled to 48 kHz if need be (`dsp::ResampledSource`). An MP3 is read through once when it is opened, for its length and seek table. Beside each track the unit keeps `<name>.trk` (text): the tempo and beats its scan found, and the tempo, loop and bar 1 the track was left with. The file stays open while it is the open track; a card rescan closes and reopens it without a gap in the sound. |

The app writes files through `AppLink` (to a temporary file first, then renamed) and only inside these five folders, and in folders below `/TRACKS`. The app itself does not offer `/TRACKS` yet: `tools/track_put.py` sends a backing track over the same link (into a folder if one is named), or the card goes into a card reader. `AppLink` also hands the app the loaded IR's samples (`IRLibrary` keeps them after a load) and the card's size and free space.

## Memory placement

The Teensy 4.1 has several kinds of memory with very different speeds. RAM1 is the tight one.

| Region | Used (14 Nov 2026) | Holds |
| --- | --- | --- |
| RAM1 code (ITCM) | 72.7 KB of a 96 KB step (25.6 KB left before the stack loses another 32 KB; 15 Nov 2026) | The audio path (39 KB), the USB interrupt, the maths and memory routines it uses, and the backing track's MP3 decoder (10.5 KB) and tempo change (4 KB), there for speed |
| RAM1 data (DTCM) | 378 KB, 47.6 KB left for the stack (15 Nov 2026; deepest use 23.9 KB, measured that day with an MP3 playing: the decoder takes about 17 KB of stack for each frame) | One delay line (144 KB), the chain with the convolver's spectra (118 KB), amp history (90 KB) |
| RAM2 (OCRAM, `DMAMEM`) | 506 KB, 18 KB heap (5 KB of it ever taken; 15 Nov 2026) | Reverb (148 KB), the other delay line (144 KB), drum songs and bass (54 KB), the backing track's tempo change (65 KB), MP3 decoder and resampler (29 KB), tuner and mod lines, the model being swapped in, the screen's strip (10 KB), USB and I2S buffers |
| PSRAM (`EXTMEM`, 8 MB) | 1.91 MB (15 Nov 2026) | The raw capture buffer (575 KB), the backing track's queue (512 KB), the list of backing tracks (545 KB: 448 KB of names, 96 KB of entries, two browsers' search hits), the MP3 seek table and file buffer (48 KB), the beat scan's onsets (32 KB), IR resampler table, IR load buffers, the five file lists, app link buffers |
| Flash | 1.72 MB of 7.9 MB | Background code, the touch screen with its fonts, the built-in model, drum kits |

- By default the Teensy core puts **all** code in RAM1. `tools/imxrt1062_t41_cold.ld` keeps background code (libc, SD, shell, app link, loaders) in flash instead. A file with interrupt code must never be added to its lists.
- `tools/check_hot_code.sh` (after `pio run`) fails if an audio-path function ended up in flash, where it would run several times slower without any error.
- The chain's constructors (they run once) and libm's double-precision sine, cosine and square root (only the IR loader uses doubles) are sent to flash by the same script; the single-precision maths stays in RAM1, because the drums' compressor and the effects' coefficient updates call it from the interrupt.
- The delay and the reverb are each a bank of effects on one block of memory (`dsp/fx/DelayBank.h`, `ReverbBank.h`): the ported effect and the two written here. The bank passes the settings to all of them and runs one; `BankSwitch` in `dsp/fx/FxParts.h` does the fade, clear and restart between them. Their setup, clearing and coefficient code is in `.cpp` files marked `DSP_COLD` (flash); only `process()` is in RAM1.
- The Teensy core's own start-up and background routines (clock, PWM and USB set-up, the EEPROM's byte access, the calendar, the fault handler, `SPI.begin`), `setup()` and `i2s32::begin()` are sent to flash by name in the same script (14 Nov 2026: 5.6 KB). What stays in RAM1 besides the audio path: the USB interrupt with its read and write paths, `memcpy`/`memset`/`str*`, single-precision maths, the 64-bit divide the drums' clock uses, and the core's flash-programming routines, which cannot run from the flash they write.
- Data that only `loop()` touches is in the PSRAM: the IR load buffers and the lists of IR, model, preset, song and track files. Not there: anything the audio interrupt reads, which includes the weights of a model being swapped in (the interrupt copies them) and the drum song's events. The one exception (15 Nov 2026) is the backing track's queue: the interrupt copies one block from it per block, front to back (128 bytes), which is the block-wise use the PSRAM is good for, and 512 KB fits nowhere else.
- The shell's `mem` prints these sizes from the running unit, the deepest stack use since boot (`paintStack()` marks the free stack first thing in `setup()`) and how much heap was ever taken. Measured on 14 Nov 2026 after IR and model loads, preset changes, the tuner, a card rescan and the app link check: 17.5 KB of stack, so the next 32 KB step of RAM1 code would still leave 59.5 KB for a stack that needs under 20.
- The backing track's tempo change (`dsp::TimeStretch`) (a phase vocoder since 15 Nov 2026) goes through its 59 KB of work buffers several times for every 11 ms of output, so they are in RAM2; its 16 KB of input, read once per window, is in the PSRAM. Work buffers in the PSRAM took about three times as long (measured with the first version): the audio interrupt goes through more than the data cache holds in every block, so nearly every PSRAM read from `loop()` is a miss. That leaves 18 KB of heap; 5 KB is the most ever taken.
- The double-precision arc tangent (the beat scan uses it once per track), `strtoul` and the track library's start-up code are sent to flash by name in the linker script, like the other background routines.
- The backing track's MP3 decoder (minimp3, compiled into `lib/dsp`'s `Mp3Stream.cpp`) has its tables in flash and, since 15 Nov 2026, its code in RAM1: it runs in `loop()` only, but from flash it decoded half as fast, which made the tempo scan of a song take twice as long ([DECISIONS.md](DECISIONS.md)). Its rounding and the resampler's are done by hand so that no further libm routine comes into RAM1.
- The touch screen (`src/Screen.cpp`, `src/Meters.cpp`, `lib/ui`, and the display, touch and SPI libraries) is all in flash and adds nothing to RAM1 code: [SCREEN.md](SCREEN.md).
- RAM1 code grows in 32 KB steps taken from the stack's space. If "free for local variables" in the build output drops by 32 KB, background code has leaked into RAM1.
- The amp's history must be a plain global (DTCM); in OCRAM the amp costs about 4 % more CPU.
- `EXTMEM` memory is not cleared at boot, and a random read from it costs about 0.9 µs: use it block-wise.

## Build environments (`platformio.ini`)

| Env | What it is |
| --- | --- |
| `teensy41` (default) | The firmware: `USB_MIDI_SERIAL`, `DSP_BLOCK=32`, the cold-code linker script; drum kits and songs are compiled in by `tools/pio_kits.py` |
| `native` | Host tests: 31 suites, 194 cases (15 Nov 2026) |
| `i2s_loopback` | Driver self-test on a bare board (jumper pin 7 to pin 8); never with the converter boards attached |
| `nam_a2_bench` | The NAM engine against the reference output, and its CPU cost |
| `nam_bench` | The reference engine (NeuralAmpModelerCore) for comparison; fetched into `third_party/` |
| `psram_test`, `psram_deep` | PSRAM hardware tests |

The hardware-test environments extend `teensy41` and replace the firmware on the board until it is flashed again.

## Kept for later
- Message numbers `FILE_RENAME` and `LOG` and the error `BUSY` in the protocol: reserved, not implemented.
