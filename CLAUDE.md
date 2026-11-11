# Teensy 4.1 Audio DSP

PlatformIO project for real-time audio DSP on a Teensy 4.1 (i.MX RT1062, Cortex-M7 @ 600 MHz, single+double FPU)
using the PJRC Teensy Audio Library (Teensyduino core 1.62). Assumed hardware: PJRC Audio Shield Rev D (SGTL5000 codec, I2S).

## Commands
- Build firmware: `pio run`
- Flash: `pio run -t upload` (Teensy Loader GUI; press the board's button if it doesn't auto-reboot)
- Serial monitor / command shell: `pio device monitor` (type `?` for commands)
- Host unit tests for portable DSP: `pio test -e native`
- PSRAM hardware test (replaces app firmware until you re-upload): `pio run -e psram_test -t upload && pio device monitor -e psram_test`
- Deep PSRAM inspection (~3 min + endless soak): `pio run -e psram_deep -t upload && pio device monitor -e psram_deep`
- Refresh editor IntelliSense after adding libs/flags: `pio run -t compiledb`

## Layout
- `src/main.cpp` – audio graph (AudioConnection wiring), codec setup, serial command shell
- `lib/dsp/` – **portable, header-only float DSP** (no Arduino/Teensy includes). Put algorithms here so they are unit-testable on the host.
- `lib/audio_fx/` – `AudioStream` subclasses that wrap `lib/dsp` algorithms for the Teensy Audio Library. Teensy-only.
- `test/` – Unity tests, run on the host via the `native` env.

## Audio library facts
- Block = `AUDIO_BLOCK_SAMPLES` (128) int16 samples @ `AUDIO_SAMPLE_RATE_EXACT` (44100). USB audio requires 128.
- `update()` runs in a low-priority software interrupt every ~2.9 ms. No `Serial`, `malloc`, `new`, or blocking calls inside it.
- `receiveWritable()` for in-place processing; `receiveReadOnly()` + `allocate()` when output differs from input. Always `release()` every block you receive/allocate, including on early-return paths.
- Change parameters from `loop()` inside `AudioNoInterrupts()` / `AudioInterrupts()` so `update()` never sees half-written state.
- `AudioMemory(n)` is a fixed pool; watch `AudioMemoryUsageMax()` and `AudioProcessorUsageMax()` (serial `s` command).
- USB type is `USB_MIDI_AUDIO_SERIAL` (set in `platformio.ini`): Teensy appears as a stereo USB audio device on the computer.

## DSP conventions
- Process in `float` internally; convert with CMSIS `arm_q15_to_float` / `arm_float_to_q15` (saturating). CMSIS-DSP (`<arm_math.h>`) is linked by default.
- Use `float` literals (`0.5f`) and `std::sin`/`sinf`, never implicit double; `-Wdouble-promotion` is on for `src/` and the native tests.
- Normalize samples to [-1, 1). Keep per-channel state in the effect object; no globals in DSP code.
- Every new algorithm in `lib/dsp` gets a host test in `test/` (frequency response, stability, edge cases) before hardware testing.

## PSRAM (8 MB APS6404L, hand-soldered, verified by hw_tests/psram_deep.cpp)
- Mapped at 0x70000000, 8 MB, D-cached, 105.6 MHz QSPI (core default). Use for big buffers: delay lines, reverb, loopers, samples.
- `EXTMEM` globals are **not zeroed or initialized at boot** (core TODO): clear delay buffers explicitly in setup().
- `extmem_malloc()` silently falls back to internal RAM (malloc) when PSRAM is full: check the returned address if it matters.
- Throughput ~31 MB/s write / ~33 MB/s read sequential; a random cache-missing 32-bit read costs ~0.9 us.
  At 44.1 kHz that's fine for a few block-wise delay taps, but don't do scattered per-sample PSRAM reads in many voices.
  Prefer copying a block (128 samples) between PSRAM and an internal-RAM scratch buffer with memcpy.
- `-Wextra`/`-Wdouble-promotion` apply to our code only; `scripts/system_headers.py` makes framework headers -isystem.
