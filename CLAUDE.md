# Teensy 4.1 Guitar DSP Platform

PlatformIO project for real-time guitar processing on a Teensy 4.1 (i.MX RT1062, Cortex-M7 @ 600 MHz, single+double FPU):
NAM A2-Lite amp models, IR cab loader, effects. 48 kHz / 24-bit end to end, lowest possible latency.
Hardware: PCM1808 ADC + PCM5102A DAC on I2S (Teensy is clock master), 8 MB PSRAM. No PJRC Audio Library on the audio path.

- Start at `README.md`: it lists every document and which one owns which fact.
- As built: `docs/SCREEN.md` (the unit's touch screen), `docs/SIGNAL_CHAIN.md` (the audio path and every gain), `docs/FIRMWARE.md` (structure, memory), `docs/CONTROL.md` (serial commands, settings table, MIDI map), `docs/APP.md` (the macOS app), `protocol/PROTOCOL.md`.
- Tracking: `docs/STATUS.md` (what works, what is unchecked, open items), `docs/DECISIONS.md` (choices and reasons). Working rules and recipes: `docs/DEVELOPMENT.md`.
- Design record: `docs/design/HLD.md` (budgets, benchmarks), `docs/design/APP_PLAN.md` (the app's plan), `docs/HARDWARE.md`.
- State on 13 Nov 2026: work stopped at the author's word after the Mod block and the app's IR graphs, then went on the same day with the limiter, two more Mod types (ce2, phase90), two more reverb modes (plate, spring) and two more delay types (tape, analog); `docs/STATUS.md` ("Where things stand") lists what waits for the author. 14 Nov 2026: memory recap (`mem`), and the touch screen's first version, wired and running; the same day its home screen was laid out anew (one chain lane, drums panel, limiter beside the master volume): flashed and running, not yet judged by the author.
- 15 Nov 2026: the backing-track player (`/TRACKS`, WAV and MP3, loop, tempo change, tempo scan and beat grid): steps 1 to 4 of 5 are flashed and checked from the shell; step 5 (its touch screen and a fifth button on home) is built and host-tested but not flashed, waiting for the author's look at the pictures in `docs/SCREEN.md`. Nothing of it has been heard by the author. `docs/STATUS.md` ("Backing-track player") has the plan and what is open. The same day step 5 was flashed at the author's word, and tracks in folders below `/TRACKS` were added (the author's are in folders, thousands to come): one list of the whole tree in the PSRAM (`dsp::TrackIndex`), gone through by folder and searched by words from the shell and the touch screen; flashed and checked, not yet judged by the author.
- **Keep the documents true:** a change to the chain, a setting, a command, a message or the app's structure updates the document that owns it in the same commit; finished or newly found things go into `docs/STATUS.md`, new choices into `docs/DECISIONS.md`.

## Commands
- Build firmware: `pio run`
- Flash: `pio run -t upload` (Teensy Loader GUI; press the board's button if it doesn't auto-reboot)
- Serial monitor / command shell: `pio device monitor` (type `?` for commands, `s` for status, `mem` for memory)
- Check the desktop app's link protocol on the device (close the serial monitor first): `~/.platformio/penv/bin/python tools/applink_check.py`
- Send a backing track to the card over the app's link (close the app and the monitor first; 130 KB/s): `~/.platformio/penv/bin/python tools/track_put.py <file.wav|mp3> [folder/ or folder/name on the card]`
- The MP3 test files of the host tests (`test/fixtures/player/`) are checked in; `tools/make_track_fixtures.py` makes them again (needs the `lameenc` Python package).
- After changing the settings table: refresh the app's browser-preview data with `tools/applink_snapshot.py` (see `app/README.md`) and add new settings to `protocol/blocks.json`
- Check that no audio code was linked into flash: `tools/check_hot_code.sh` (after `pio run`)
- Host unit tests for portable DSP: `pio test -e native`
- The touch screen's screens as pictures, without the unit: `pio test -e native -f test_ui && python3 tools/ui_render.py` (PNG files in `.pio/ui/`); after changing `protocol/blocks.json` also run `python3 tools/make_blocks_header.py`
- I2S driver self-test (bare board, jumper pin 7 -> pin 8; replaces app firmware until you re-upload):
  `pio run -e i2s_loopback -t upload && pio device monitor -e i2s_loopback`
- Reference NAM engine (NeuralAmpModelerCore) benchmark, for comparison only (fetches it into `third_party/` on first build):
  `pio run -e nam_bench -t upload && pio device monitor -e nam_bench`
- Own NAM A2-Lite engine: on-board correctness check + CPU benchmark (bare board, no audio):
  `pio run -e nam_a2_bench -t upload && pio device monitor -e nam_a2_bench`
- PSRAM hardware test: `pio run -e psram_test -t upload && pio device monitor -e psram_test`
- Deep PSRAM inspection (~4 min + endless soak): `pio run -e psram_deep -t upload && pio device monitor -e psram_deep`
- Refresh editor IntelliSense after adding libs/flags: `pio run -t compiledb`

## Layout
- `src/main.cpp` – setup and the audio block callback (ADC words -> float -> `AudioChain` -> DAC words)
- `src/AudioChain.*` – the whole signal chain: amp (NAM A2-Lite) -> gate -> IR convolution -> output gain + preset level -> mod (chorus / flanger / ce2 / phase90) -> delay (digital / tape / analog) -> reverb (hall / gated / plate / spring), + drums and bass, + backing track -> master volume -> limiter (optional) -> clip
- `src/` (rest) – IR / amp / preset libraries on the SD card, `Params`, `Settings` (EEPROM), `SerialUI`, `AppLink` (the desktop app's binary protocol, on the same serial port as the shell), `MidiControl` (USB MIDI: program change = preset, controllers), `Tuner`, `DrumSongs`, `TrackLibrary` (backing tracks in `/TRACKS` and the folders below it)
- `include/config.h` – compile-time settings and hardware constants (guitar channel, ADC full scale, mute pin)
- `lib/i2s32/` – I2S driver (SAI1 + eDMA, 48 kHz, 32-bit slots, block callback in the DMA interrupt). Teensy-only.
- `lib/dsp/` – **portable float DSP** (namespace `dsp`, no Arduino/Teensy includes), unit-tested on the host: `nam/`, `conv/`, `fx/`, `drums/`, `player/` (the backing-track player)
- `lib/ui/` – the touch screen's screens, touch handling and drawing (namespace `ui_tft`). Portable, host-tested (`test/test_ui`); `src/Screen.cpp` is the unit's side (ILI9341 + XPT2046 on SPI0: CS 10, DC 9, reset 5, touch CS 4, IRQ 3). All of it must stay on the cold list (flash). The display is wired (14 Nov 2026); without one the screen stays off.
- `lib/params/` – the settings table (`ParamDefs`) and preset file format. Portable.
- `lib/applink/` – the desktop app's link protocol on the firmware side (frames, payload fields, message numbers). Portable. Reference: `protocol/PROTOCOL.md`; the chain's blocks as the UIs show them (names, colours, which settings belong where): `protocol/blocks.json`; shared examples `protocol/test_frames.txt` (regenerate with `python3 protocol/make_test_frames.py`).
- `app/` – the macOS control app, "Volfovich NAM Forge" (as built: `docs/APP.md`, plan: `docs/design/APP_PLAN.md`, commands: `app/README.md`): Tauri 2, Rust core in `app/crates`, Svelte UI in `app/src`. Only one program can hold the unit's serial port: close the app before flashing or opening the serial monitor.
- `hw_tests/` – standalone firmware images that test hardware (PSRAM, I2S loopback, NAM benchmarks); each has its own env.
- `tools/` – cold-code linker script, `check_hot_code.sh`, drum kit and song compilers. `kits/`, `songs/` – drum assets.
- `test/` – Unity suites, run on the host via the `native` env; fixtures in `test/fixtures/`.

## Ported code (from github.com/volfovich/teensy-ir-loader)
- Almost everything in `lib/dsp`, `lib/params` and `src/` except `main.cpp`, `AudioChain.*` (adapted), `lib/i2s32` and `dsp/Convert.h` is ported from that project. Its `NAM.md`, `DELAY.md`, `REVERB.md`, `PRESETS.md`, `DRUMS.md` describe the same code.
- Ported files keep the original formatting (2-space indent, `printf` with floats). Only the namespace (`irdsp` -> `dsp`), include paths (`<dsp/...>`, `<params/...>`) and placement macros (`DSP_COLD`, `DSP_PROGMEM`) changed. Keep edits small so fixes can go back to the old project.
- The ported test suites run the original checks inside one Unity test each (`#define main ported_main`).
- The settings table is the old project's minus its three codec levels (`in`, `out`, `hp`: line in, line out, headphones), which this hardware does not have. Preset files are still interchangeable (those three were never in presets); the EEPROM record is not (`Settings.cpp` converts the old record once).
- `Reverb80` and `StudioDelay` are ported and are not edited: the plate and spring reverbs and the tape and analog delays are separate classes written for this project (`PlateReverb`, `SpringReverb`, `TapeDelay`, `AnalogDelay`), chosen through `ReverbBank` / `DelayBank`, which hand hall, gated and digital through bit for bit (`test_fx_banks`). A new mode or type must cost no more CPU than the ported one and fit the block's existing memory.
- The ported doubler was removed on 14 Nov 2026 (the author does not use it): `Doubler.h`, its three settings (`dbl`, `dbl time`, `dbl width`), its tile and its MIDI controller (CC 82). Older presets still load (their `dbl` lines are ignored); `Settings.cpp` converts the EEPROM record once; the link protocol went to version 2.
- Not ported and not wanted: the OLED panel, encoder and keys. The unit's own UI is a 2.8-inch TFT with touch, designed from scratch (`docs/SCREEN.md`).

## Audio facts
- 48 kHz only. Block = `DSP_BLOCK` frames (32, set in `platformio.ini`; 16 was tried and overruns because the convolver's cost doubles) = `i2s32::kBlock` = `kBlock` in `config.h`. In-to-out latency is about 2 blocks plus converter filters.
- I2S words are 32-bit, interleaved L,R, with the 24-bit sample in bits 31..8. Convert with `dsp/Convert.h` (`int24InToFloat` / `floatToInt24Out`: saturating, exact round trip).
- The block callback runs in the RX DMA interrupt at priority 48 (above USB). No `Serial`, `malloc`, `new`, SD access, or blocking calls inside it, and it must finish within one block period (0.67 ms at 32 frames = 400k CPU cycles).
- Each effect's header states its threading model: setters from `loop()`, `process()` in the interrupt.
- In the shell a toggle's key alone flips it: `drum` starts or stops the drums, it does not show them. To read state use `s`; to set, give the value (`drum off`).
- Watch the serial `s` command: per-stage CPU, driver overruns, FIFO errors, levels. Budget: worst case at or below 80% of the block period. Measured on 13 Nov 2026: a typical preset (amp, 4k IR, reverb) 57% max; everything on (doubler, mod, delay, reverb, drums) 80% max, so there is no room for another effect without making some. The doubler (1.1%) was removed on 14 Nov 2026 at the author's word (not used); not measured again since.
- Levels: the output gain (`g`, global, -60 to +24 dB, default -6 dB) and the preset's level (`pvol`) are one gain applied after the cab and before the mod, delay and reverb, exactly as in teensy-ir-loader; the drums and bass are not affected by it. With the cab bypassed (`b on`) this gain is skipped too (as ported). The master volume (`master`, silent to 0 dB, global, default 0 dB = no change) is a separate last gain before the DAC, added on 13 Nov 2026 at the author's request. The rest of the audio path must stay as ported: the author wants the sound unchanged. `AudioChain` keeps the peak at each block's output (`takeStagePeak`) for the app's block meters.
- Backing track (15 Nov 2026, in progress: plan in `docs/STATUS.md`, "Backing-track player"): a file from `/TRACKS` is read and decoded in `loop()` (`serviceTrack()`) into a 2.7 s queue in the PSRAM; the interrupt only copies a block from it (`dsp::TrackOut`) and adds it beside the drums, with its own level (`track vol`). Anything costly for the track (MP3 decoding, resampling, the tempo change) runs in `loop()`, never in the interrupt; what `loop()` works on in place goes in RAM2, not PSRAM (the interrupt empties the data cache every block). With no track playing the chain runs as before. Each track is scanned once for tempo and beats while it is open and not playing; the result and the track's last tempo and loop (its points, also while it is switched off) are in `<name>.trk` beside it.
- Checking levels without a guitar: serial `sig 220 -20` replaces the guitar with a sine at a known level (`sig 0` ends it); read the levels with `s` or the app's block meters. Turn the master down first (`master -40`): a high-gain model makes any input loud. Put it back afterwards.
- Overload protection: 16 overruns in a row make the driver go silent and stop calling the callback (`i2s32::faulted()`); `serviceFault()` in `main.cpp` sheds load and resumes after 1 s. Serial `overload` tests it.
- The guitar input passes a 30 Hz high-pass first (`INPUT_HIGHPASS_HZ`): the SD card puts a sub-20 Hz thump on the ADC about every 1.4 s.
- Flush-to-zero and default-NaN float modes are enabled by `i2s32::begin()` (FPSCR and FPDSCR).
- USB type is `USB_MIDI_SERIAL`. USB audio is not available: the core's implementation is 16-bit / 44.1 kHz.
- Guitar input is mono on the ADC **right** channel (`GUITAR_ON_RIGHT_CHANNEL` in `config.h`).

## Memory placement (RAM1 is tight)
- All code goes to ITCM (RAM1) by default. `tools/imxrt1062_t41_cold.ld` keeps background code (libc, SdFat, SerialUI, libraries, loaders) in flash. Never add a file with audio-interrupt code to its cold lists.
- After changing the linker script or the audio path, run `tools/check_hot_code.sh` (after `pio run`): it fails if an audio-path function was linked into flash.
- The NAM history must be a plain global (DTCM), not `DMAMEM`: OCRAM costs about 4% CPU more.
- RAM1 code grows in 32 KB steps taken from the stack's space (ITCM): 72.7 KB of the 96 KB step on 15 Nov 2026, 25.6 KB left, "free for local variables" 47.6 KB in `pio run`. The step from 64 KB was taken on purpose that day: the backing track's MP3 decoder and its tempo change (a phase vocoder, `dsp/player/TimeStretch.h`) run from RAM1 (from flash the tempo scan took twice as long, and the tempo change could not keep up). If the figure drops by another 32 KB, a step was crossed: move background code to the cold list (the stack's deepest use measured with serial `mem` is 23.9 KB, with an MP3 playing).
- Data only `loop()` touches goes in PSRAM (IR load buffers, the file lists); nothing the audio interrupt reads does (the weights of a model being swapped in stay in RAM2), with one exception: the backing track's queue, which the interrupt copies from block by block, front to back.
- Load-time-only buffers can go in PSRAM (`EXTMEM`), as the IR resampler table does.

## DSP conventions
- Process in `float` internally, samples normalized to [-1, 1). `lib/dsp` must also build on the host.
- Keep state in the effect object, memory handed in by the caller; no globals in DSP code.
- Every new algorithm in `lib/dsp` gets a host test in `test/` before hardware testing.
- No algorithm on the audio path may add latency: no lookahead, no oversampling, no linear-phase filters, no FFT blocks larger than the audio block. The one agreed exception is the limiter's lookahead switch (1 ms, off by default).
- Warnings: `-Wall` for `src/`; `-Wall -Wextra` for the native tests.

## PSRAM (8 MB APS6404L, hand-soldered, verified by hw_tests/psram_deep.cpp)
- Mapped at 0x70000000, 8 MB, D-cached, 105.6 MHz QSPI (core default). Use for big buffers: delay lines, reverb, loopers, samples.
- `EXTMEM` globals are **not zeroed or initialized at boot** (core TODO): clear delay buffers explicitly in setup().
- `extmem_malloc()` silently falls back to internal RAM (malloc) when PSRAM is full: check the returned address if it matters.
- Throughput ~31 MB/s write / ~33 MB/s read sequential; a random cache-missing 32-bit read costs ~0.9 us.
  At 48 kHz that's fine for a few block-wise delay taps, but don't do scattered per-sample PSRAM reads in many voices.
  Prefer copying a block between PSRAM and an internal-RAM scratch buffer with memcpy.
- Warnings apply to our code only; `scripts/system_headers.py` makes framework headers -isystem.
