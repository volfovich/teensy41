# Teensy 4.1 Guitar DSP Platform

PlatformIO project for real-time guitar processing on a Teensy 4.1 (i.MX RT1062, Cortex-M7 @ 600 MHz, single+double FPU):
NAM A2-Lite amp models, IR cab loader, effects. 48 kHz / 24-bit end to end, lowest possible latency.
Hardware: PCM1808 ADC + PCM5102A DAC on I2S (Teensy is clock master), 8 MB PSRAM. No PJRC Audio Library on the audio path.

- Design: `HLD.md` (architecture, budgets, milestones). Hardware: `Teensy 4.1 Audio DSP Platform PCM1808 + PCM5102A Design Doc.md`.

## Commands
- Build firmware: `pio run`
- Flash: `pio run -t upload` (Teensy Loader GUI; press the board's button if it doesn't auto-reboot)
- Serial monitor / command shell: `pio device monitor` (type `?` for commands)
- Host unit tests for portable DSP: `pio test -e native`
- I2S driver self-test (bare board, jumper pin 7 -> pin 8; replaces app firmware until you re-upload):
  `pio run -e i2s_loopback -t upload && pio device monitor -e i2s_loopback`
- PSRAM hardware test: `pio run -e psram_test -t upload && pio device monitor -e psram_test`
- Deep PSRAM inspection (~3 min + endless soak): `pio run -e psram_deep -t upload && pio device monitor -e psram_deep`
- Refresh editor IntelliSense after adding libs/flags: `pio run -t compiledb`

## Layout
- `src/main.cpp` – setup, audio block callback, serial command shell, USB MIDI
- `lib/i2s32/` – I2S driver (SAI1 + eDMA, 48 kHz, 32-bit slots, block callback in the DMA interrupt). Teensy-only.
- `lib/dsp/` – **portable, header-only float DSP** (no Arduino/Teensy includes). Put algorithms here so they are unit-testable on the host.
- `hw_tests/` – standalone firmware images that test hardware (PSRAM, I2S loopback); each has its own env.
- `test/` – Unity tests, run on the host via the `native` env.

## Audio facts
- 48 kHz only. Block = `DSP_BLOCK` frames (32, set in `platformio.ini`) = `i2s32::kBlock`. In-to-out latency is about 2 blocks plus converter filters.
- I2S words are 32-bit, interleaved L,R, with the 24-bit sample in bits 31..8. Convert with `dsp/Convert.h` (`int24InToFloat` / `floatToInt24Out`: saturating, exact round trip).
- The block callback runs in the RX DMA interrupt at priority 48 (above USB). No `Serial`, `malloc`, `new`, SD access, or blocking calls inside it, and it must finish within one block period (0.67 ms at 32 frames = 400k CPU cycles).
- Pass parameters from `loop()` to the callback through `volatile`/atomic values or a double-buffered struct; the callback pre-empts `loop()` at any point.
- Watch `i2s32::stats()` (serial `s` command): load, max load, overruns, FIFO errors. Budget: worst case at or below 80% of the block period.
- Flush-to-zero and default-NaN float modes are enabled by `i2s32::begin()` (FPSCR and FPDSCR).
- USB type is `USB_MIDI_SERIAL` (set in `platformio.ini`). USB audio is not available: the core's implementation is 16-bit / 44.1 kHz.
- Guitar input is mono on the ADC **right** channel (`kGuitarOnRightChannel` in `src/main.cpp`); the left input is unused.

## DSP conventions
- Process in `float` internally, samples normalized to [-1, 1). CMSIS-DSP (`<arm_math.h>`) is available on the target only; `lib/dsp` must also build on the host.
- Use `float` literals (`0.5f`) and `std::sin`/`sinf`, never implicit double; `-Wdouble-promotion` is on for `src/` and the native tests.
- Keep per-channel state in the effect object; no globals in DSP code.
- Every new algorithm in `lib/dsp` gets a host test in `test/` (frequency response, stability, edge cases) before hardware testing.
- No algorithm on the audio path may add latency: no lookahead, no oversampling, no linear-phase filters, no FFT blocks larger than the audio block.

## PSRAM (8 MB APS6404L, hand-soldered, verified by hw_tests/psram_deep.cpp)
- Mapped at 0x70000000, 8 MB, D-cached, 105.6 MHz QSPI (core default). Use for big buffers: delay lines, reverb, loopers, samples.
- `EXTMEM` globals are **not zeroed or initialized at boot** (core TODO): clear delay buffers explicitly in setup().
- `extmem_malloc()` silently falls back to internal RAM (malloc) when PSRAM is full: check the returned address if it matters.
- Throughput ~31 MB/s write / ~33 MB/s read sequential; a random cache-missing 32-bit read costs ~0.9 us.
  At 48 kHz that's fine for a few block-wise delay taps, but don't do scattered per-sample PSRAM reads in many voices.
  Prefer copying a block between PSRAM and an internal-RAM scratch buffer with memcpy.
- `-Wextra`/`-Wdouble-promotion` apply to our code only; `scripts/system_headers.py` makes framework headers -isystem.
