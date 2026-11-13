# Technologies

What the project is made of: parts, tools, libraries and other people's work. Versions are the ones installed on 13 Nov 2026.

## Hardware

| Part | Role | Notes |
| --- | --- | --- |
| Teensy 4.1 | Processor board | NXP i.MX RT1062, Cortex-M7 at 600 MHz, single and double precision FPU, 1 MB RAM, 8 MB flash, SD slot |
| PCM1808 board | ADC | 24-bit, I2S slave, guitar on the right channel |
| PCM5102A board | DAC | 24-bit (32-bit slots), I2S, its PLL runs from the bit clock |
| APS6404L | 8 MB PSRAM | Hand-soldered under the Teensy; verified by `hw_tests/psram_deep.cpp` |
| SD card | Models, IRs, presets, drum songs | The Teensy's own slot (SDIO) |

Not there yet: a high-impedance input buffer for the guitar, firmware control of the DAC's mute pin, a display. Details in [HARDWARE.md](HARDWARE.md).

## Firmware

| Technology | Version | Used for |
| --- | --- | --- |
| PlatformIO Core | 6.2.0 | Build, upload, tests |
| PlatformIO `teensy` platform | 6.0.0 | Board support |
| Teensyduino core (`framework-arduinoteensy`) | 1.162.0 | Startup, USB serial and MIDI, SD/SdFat, EEPROM |
| GCC ARM (`toolchain-gccarmnoneeabi-teensy`) | 1.150201.0 (GCC 15.2) | Compiler, C++17, `-O2` |
| Unity | from PlatformIO | Host unit tests (`env:native`) |
| Python 3 | – | Build scripts (`scripts/`, `tools/`), on-unit checks |

Written for this project: the I2S driver (`lib/i2s32`: SAI1, eDMA), sample conversion, the link protocol, the cold-code linker script.

**Deliberately not used:**
- **PJRC Audio Library:** 16-bit, 44.1 kHz, 128-sample blocks. The project needs 24-bit, 48 kHz and small blocks.
- **USB audio:** the core's is 16-bit / 44.1 kHz and tied to the Audio Library.
- **NeuralAmpModelerCore, Eigen, a JSON library on the device:** 392 KB of code in RAM1 and twice the CPU. The project's own engine reads `.nam` files as a stream. The reference engine is kept only as a benchmark (`env:nam_bench`) and as the source of the test vectors.
- **ArduinoJson:** presets are plain text lines.

## DSP methods

| Block | Method |
| --- | --- |
| Amp | Neural Amp Modeler A2-Lite: a small causal WaveNet-style network (23 layers, 3 channels, 1,871 weights), computed a layer at a time over the block |
| Cab | Uniformly partitioned frequency-domain convolution (overlap-save), partition = block, radix FFT in `dsp/Fft.h` |
| IR loading | Windowed-sinc resampling to 48 kHz, trimming, energy normalisation |
| Gate | Hard gate on the input level, or the NAM plugin's expander |
| Delay, reverb, doubler | Delay lines with modulation; the hall is an 8-line feedback delay network |
| Limiter | Brickwall peak limiter: instant attack, 10 ms hold, exponential release, stereo-linked; optional 1 ms lookahead (sliding minimum, then a moving average) |
| Mod | Chorus and flanger: one LFO-swept, Hermite-interpolated delay line per side, with low-passed feedback |
| Tuner | YIN pitch detection at 24 kHz |
| Drums, bass | Sample playback with a bus compressor; a plucked-string bass; General MIDI file reader |

## App

| Technology | Version | Used for |
| --- | --- | --- |
| Rust | 1.100.0 | The core and the shell |
| Tauri | 2 | The app's window and the bridge between Rust and the UI |
| `tauri-plugin-dialog` | 2 | File and folder dialogs |
| `serialport` crate | 4 | The USB serial port |
| `serde`, `serde_json` | 1 | Data to the UI, reading `.nam` headers |
| Svelte | 5 | The UI |
| SvelteKit + `adapter-static` | 2 / 3 | The UI's project layout, built to static files |
| TypeScript | 6.0 | The UI's language |
| Vite | 8 | Dev server and bundler |
| Node / npm | – | Running the UI tools |

The app is macOS only for now, not signed, run as a local build.

## Third-party code and assets

| What | From | Licence | Where |
| --- | --- | --- | --- |
| Example amp model `A2.nam` | NeuralAmpModelerCore (Steven Atkinson) | MIT | Built into the firmware; test fixtures |
| Noise gate (NAM type) and DC blocker | AudioDSPTools (Steven Atkinson) | MIT | `lib/dsp/src/dsp/fx/NoiseGate.h`, `nam/AmpStage.h` |
| Effect building blocks | hexefx_audiolib_F32 (Piotr Zapart) | MIT | `lib/dsp/src/dsp/Blocks.h` (notice in the file header) |
| Drum samples | AVL Drumkits | CC BY-SA 3.0 | `kits/rock/` (see `kits/rock/CREDITS.md`) |
| NeuralAmpModelerCore, Eigen | their authors | MIT, MPL 2.0 | Fetched into `third_party/` only for `env:nam_bench`; not in the repo, not in the firmware |

The ported DSP and application code is the author's own, from [teensy-ir-loader](https://github.com/volfovich/teensy-ir-loader).
