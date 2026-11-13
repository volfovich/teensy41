# Teensy 4.1 Guitar DSP Platform

PlatformIO project for real-time guitar processing on a Teensy 4.1 (i.MX RT1062, Cortex-M7 @ 600 MHz, single+double FPU):
NAM A2-Lite amp models, IR cab loader, effects. 48 kHz / 24-bit end to end, lowest possible latency.
Hardware: PCM1808 ADC + PCM5102A DAC on I2S (Teensy is clock master), 8 MB PSRAM. No PJRC Audio Library on the audio path.

- Design: `HLD.md` (architecture, budgets, milestones). Hardware: `Teensy 4.1 Audio DSP Platform PCM1808 + PCM5102A Design Doc.md`.

## Commands
- Build firmware: `pio run`
- Flash: `pio run -t upload` (Teensy Loader GUI; press the board's button if it doesn't auto-reboot)
- Serial monitor / command shell: `pio device monitor` (type `?` for commands, `s` for status)
- Check the desktop app's link protocol on the device (close the serial monitor first): `~/.platformio/penv/bin/python tools/applink_check.py`
- After changing the settings table: refresh the app's browser-preview data with `tools/applink_snapshot.py` (see `app/README.md`) and add new settings to `protocol/blocks.json`
- Check that no audio code was linked into flash: `tools/check_hot_code.sh` (after `pio run`)
- Host unit tests for portable DSP: `pio test -e native`
- I2S driver self-test (bare board, jumper pin 7 -> pin 8; replaces app firmware until you re-upload):
  `pio run -e i2s_loopback -t upload && pio device monitor -e i2s_loopback`
- Reference NAM engine (NeuralAmpModelerCore) benchmark, for comparison only (fetches it into `third_party/` on first build):
  `pio run -e nam_bench -t upload && pio device monitor -e nam_bench`
- Own NAM A2-Lite engine: on-board correctness check + CPU benchmark (bare board, no audio):
  `pio run -e nam_a2_bench -t upload && pio device monitor -e nam_a2_bench`
- PSRAM hardware test: `pio run -e psram_test -t upload && pio device monitor -e psram_test`
- Deep PSRAM inspection (~3 min + endless soak): `pio run -e psram_deep -t upload && pio device monitor -e psram_deep`
- Refresh editor IntelliSense after adding libs/flags: `pio run -t compiledb`

## Layout
- `src/main.cpp` – setup and the audio block callback (ADC words -> float -> `AudioChain` -> DAC words)
- `src/AudioChain.*` – the whole signal chain: amp (NAM A2-Lite) -> gate -> IR convolution -> doubler -> delay -> reverb, + drums and bass
- `src/` (rest) – IR / amp / preset libraries on the SD card, `Params`, `Settings` (EEPROM), `SerialUI`, `AppLink` (the desktop app's binary protocol, on the same serial port as the shell), `MidiControl` (USB MIDI: program change = preset, controllers), `Tuner`, `DrumSongs`
- `include/config.h` – compile-time settings and hardware constants (guitar channel, ADC full scale, mute pin)
- `lib/i2s32/` – I2S driver (SAI1 + eDMA, 48 kHz, 32-bit slots, block callback in the DMA interrupt). Teensy-only.
- `lib/dsp/` – **portable float DSP** (namespace `dsp`, no Arduino/Teensy includes), unit-tested on the host: `nam/`, `conv/`, `fx/`, `drums/`
- `lib/params/` – the settings table (`ParamDefs`) and preset file format. Portable.
- `lib/applink/` – the desktop app's link protocol on the firmware side (frames, payload fields, message numbers). Portable. Reference: `protocol/PROTOCOL.md`; the chain's blocks as the UIs show them (names, colours, which settings belong where): `protocol/blocks.json`; shared examples `protocol/test_frames.txt` (regenerate with `python3 protocol/make_test_frames.py`).
- `app/` – the macOS control app, "Volfovich NAM Forge" (plan: `APP.md`, commands: `app/README.md`): Tauri 2, Rust core in `app/crates`, Svelte UI in `app/src`. Only one program can hold the unit's serial port: close the app before flashing or opening the serial monitor.
- `hw_tests/` – standalone firmware images that test hardware (PSRAM, I2S loopback, NAM benchmarks); each has its own env.
- `tools/` – cold-code linker script, `check_hot_code.sh`, drum kit and song compilers. `kits/`, `songs/` – drum assets.
- `test/` – Unity suites, run on the host via the `native` env; fixtures in `test/fixtures/`.

## Ported code (from github.com/volfovich/teensy-ir-loader)
- Almost everything in `lib/dsp`, `lib/params` and `src/` except `main.cpp`, `AudioChain.*` (adapted), `lib/i2s32` and `dsp/Convert.h` is ported from that project. Its `NAM.md`, `DELAY.md`, `REVERB.md`, `PRESETS.md`, `DRUMS.md` describe the same code.
- Ported files keep the original formatting (2-space indent, `printf` with floats). Only the namespace (`irdsp` -> `dsp`), include paths (`<dsp/...>`, `<params/...>`) and placement macros (`DSP_COLD`, `DSP_PROGMEM`) changed. Keep edits small so fixes can go back to the old project.
- The ported test suites run the original checks inside one Unity test each (`#define main ported_main`).
- The settings table is the old project's minus its three codec levels (`in`, `out`, `hp`: line in, line out, headphones), which this hardware does not have. Preset files are still interchangeable (those three were never in presets); the EEPROM record is not (`Settings.cpp` converts the old record once).
- Not ported and not wanted: the OLED panel, encoder and keys. The planned UI is a 2.8-inch TFT with touch, designed from scratch.

## Audio facts
- 48 kHz only. Block = `DSP_BLOCK` frames (32, set in `platformio.ini`; 16 was tried and overruns because the convolver's cost doubles) = `i2s32::kBlock` = `kBlock` in `config.h`. In-to-out latency is about 2 blocks plus converter filters.
- I2S words are 32-bit, interleaved L,R, with the 24-bit sample in bits 31..8. Convert with `dsp/Convert.h` (`int24InToFloat` / `floatToInt24Out`: saturating, exact round trip).
- The block callback runs in the RX DMA interrupt at priority 48 (above USB). No `Serial`, `malloc`, `new`, SD access, or blocking calls inside it, and it must finish within one block period (0.67 ms at 32 frames = 400k CPU cycles).
- Each effect's header states its threading model: setters from `loop()`, `process()` in the interrupt.
- Watch the serial `s` command: per-stage CPU, driver overruns, FIFO errors, levels. Budget: worst case at or below 80% of the block period. Measured: amp + doubler + delay + reverb 52%, with drums 68% max.
- Overload protection: 16 overruns in a row make the driver go silent and stop calling the callback (`i2s32::faulted()`); `serviceFault()` in `main.cpp` sheds load and resumes after 1 s. Serial `overload` tests it.
- The guitar input passes a 30 Hz high-pass first (`INPUT_HIGHPASS_HZ`): the SD card puts a sub-20 Hz thump on the ADC about every 1.4 s.
- Flush-to-zero and default-NaN float modes are enabled by `i2s32::begin()` (FPSCR and FPDSCR).
- USB type is `USB_MIDI_SERIAL`. USB audio is not available: the core's implementation is 16-bit / 44.1 kHz.
- Guitar input is mono on the ADC **right** channel (`GUITAR_ON_RIGHT_CHANNEL` in `config.h`).

## Memory placement (RAM1 is tight)
- All code goes to ITCM (RAM1) by default. `tools/imxrt1062_t41_cold.ld` keeps background code (libc, SdFat, SerialUI, libraries, loaders) in flash. Never add a file with audio-interrupt code to its cold lists.
- After changing the linker script or the audio path, run `tools/check_hot_code.sh` (after `pio run`): it fails if an audio-path function was linked into flash.
- The NAM history must be a plain global (DTCM), not `DMAMEM`: OCRAM costs about 4% CPU more.
- Load-time-only buffers can go in PSRAM (`EXTMEM`), as the IR resampler table does.

## DSP conventions
- Process in `float` internally, samples normalized to [-1, 1). `lib/dsp` must also build on the host.
- Keep state in the effect object, memory handed in by the caller; no globals in DSP code.
- Every new algorithm in `lib/dsp` gets a host test in `test/` before hardware testing.
- No algorithm on the audio path may add latency: no lookahead, no oversampling, no linear-phase filters, no FFT blocks larger than the audio block.
- Warnings: `-Wall` for `src/`; `-Wall -Wextra` for the native tests.

## PSRAM (8 MB APS6404L, hand-soldered, verified by hw_tests/psram_deep.cpp)
- Mapped at 0x70000000, 8 MB, D-cached, 105.6 MHz QSPI (core default). Use for big buffers: delay lines, reverb, loopers, samples.
- `EXTMEM` globals are **not zeroed or initialized at boot** (core TODO): clear delay buffers explicitly in setup().
- `extmem_malloc()` silently falls back to internal RAM (malloc) when PSRAM is full: check the returned address if it matters.
- Throughput ~31 MB/s write / ~33 MB/s read sequential; a random cache-missing 32-bit read costs ~0.9 us.
  At 48 kHz that's fine for a few block-wise delay taps, but don't do scattered per-sample PSRAM reads in many voices.
  Prefer copying a block between PSRAM and an internal-RAM scratch buffer with memcpy.
- `-Wextra`/`-Wdouble-promotion` apply to our code only; `scripts/system_headers.py` makes framework headers -isystem.
