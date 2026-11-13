# Teensy 4.1 Guitar DSP Platform: High-Level Design

Nov 13, 2026 · Status: driver, amp, cab, effects, presets and drums are running on the board; the signal chain and app layer are ported from [teensy-ir-loader](https://github.com/volfovich/teensy-ir-loader)

Companion to [`Teensy 4.1 Audio DSP Platform PCM1808 + PCM5102A Design Doc.md`](<Teensy 4.1 Audio DSP Platform PCM1808 + PCM5102A Design Doc.md>) (the "hardware doc").
Numbers marked **(verify)** are taken from datasheets or vendor claims quoted from memory, and need to be checked or measured.

---

## 0. Goals

| # | Requirement | What it means in practice |
| --- | --- | --- |
| R1 | Guitar processor: NAM A2 amp model, IR cab loader, further DSP effects | A fixed, configurable chain of stages, with models and IRs loaded from SD |
| R2 | 24-bit, 48 kHz all the way | I2S at 48 kHz with 32-bit slots and 24-bit data. Float32 inside. No resampling anywhere on the device. NAM models are trained at 48 kHz, so they run natively. |
| R3 | Minimum latency | Small blocks, processing in the DMA interrupt, zero-latency algorithms only (no lookahead, no oversampling, no added FFT latency). The DAC's low-latency filter is a deferred option (§1 item 2). |

The PJRC Teensy Audio Library is **not** used on the audio path. It is 16-bit, runs at 44.1 kHz, and uses 128-sample blocks (2.9 ms per block), which breaks R2 and R3.

---

## 1. Review of the hardware doc

The pinout (MCLK 23, BCLK 21, LRCLK 20, IN1 8, OUT1A 7) is correct. So are the PCM1808 slave-mode and I2S jumper settings (MD0 = MD1 = FMT = low), the PCM5102A jumpers, its BCK-derived PLL at 64·fs, and the "24-bit data left-aligned in a 32-bit slot" description. The doc reads clearly. The items below are gaps or corrections. Items 1, 1b and 2 affect the hardware. **Decision (Nov 2026): the boards are not re-soldered for now, so items 1b and 2 are deferred.** The design and budgets below assume the boards as built.

| # | Severity | Finding | Recommendation |
| --- | --- | --- | --- |
| 1 | **High** | **There is no guitar input stage.** The PCM1808 input impedance is about 60 kΩ. A passive pickup wants about 1 MΩ, or it loses treble and sounds dull. A single-coil at about 100 mVrms would also sit at about −20 dBFS, wasting about 20 dB of ADC dynamic range. | Add a Hi-Z buffer/preamp before the ADC: 1 MΩ input, a JFET or low-noise op-amp (TL072 / OPA1642 class), switchable gain of about 0 / +10 / +20 dB, and input protection (series R plus clamp diodes). Power it from a clean 9–12 V supply or from 5 V with a rail splitter. **Build a simple unity-gain buffer early** (one op-amp, 1 MΩ): it is needed before any tone can be judged. The switchable-gain version can wait (M6). Until the buffer exists, put a buffered pedal or DI box in front. |
| 1b | Medium | **The DAC cannot be muted by firmware.** XSMT is soldered high (H3L = H), so the output is live during boot and flashing, and after a crash the DMA keeps replaying the last buffer into the amp. | **Deferred.** Later option: remove the H3L bridge and wire XSMT to a Teensy GPIO with a pull-down resistor, so firmware can mute on boot and on a fault (§4.1). Until then: no performance cost, but turn the amp or headphone volume down before flashing or resetting. The chip mutes itself when the clocks stop, which should cover most boot and flash noise **(verify)**. |
| 2 | Medium (latency) | **The DAC filter jumper is set to normal latency.** H1L (FLT) = L selects the 8× FIR filter, with a group delay of about 20/fs ≈ 0.42 ms **(verify)**. FLT = H selects the low-latency IIR filter, about 3.5/fs ≈ 0.07 ms **(verify)**. | **Deferred.** Later option: re-solder H1L to **H**, which saves about 0.35 ms. Staying on the FIR filter costs that latency and nothing else; it is the linear-phase filter, so sound quality is the same or marginally better. |
| 3 | Low | The clock table lists 44.1 kHz, and the note "MCLK … about 11.3 MHz" assumes 44.1 kHz. | This platform runs at **48 kHz only**. MCLK = 12.288 MHz, BCLK = 3.072 MHz. Remove the 44.1 kHz column. |
| 4 | Medium (bug) | **The starter code's float→int32 conversion overflows.** `2147483647.0f` rounds to 2³¹, so `l = 1.0f` produces 2³¹, which is outside `int32_t` (undefined behaviour). DSP gain above 0 dBFS has the same problem. | Scale by 2²³, saturate to the 24-bit range, round, then shift left 8 (`dsp::floatToInt24Word` in `lib/dsp/src/dsp/Convert.h`). This gives exact 24-bit output with zeroed low bits, and int→float→int is exact for every 24-bit value. |
| 5 | Low | On input, the bits after the PCM1808's 24-bit LSB (bits 7–0 of the slot) are not guaranteed to be zero **(verify)**. | Mask with `& 0xFFFFFF00` before converting. |
| 6 | Medium | **USB audio cannot coexist** with this I/O. The Teensy core's USB audio is 16-bit / 44.1 kHz and is clocked by the Audio Library. `platformio.ini` currently builds `USB_MIDI_AUDIO_SERIAL`. | Switch to `USB_MIDI_SERIAL`, which keeps the serial shell and adds USB MIDI control. For recording, use the analog output into an audio interface. |
| 7 | Low | The level relationship between ADC full scale and the NAM model input is not defined. ADC full scale = 1.06 Vrms ≈ **+2.7 dBu** at the ADC pins **(verify)**. | Define an input-calibration stage (§6.1). Once a preamp exists, the doc should state its gain. |
| 7b | Low | The DAC board is fed 5 V while the PCM5102A chip is a 3.3 V part. This is correct only because the board has on-board 3.3 V regulators behind VIN; it has not been confirmed on the actual board. | Note and three checks added to the hardware doc (regulators present, VIN not shorted to A3V3, A3V3 reads 3.3 V at first power-up). |
| 8 | Info | Open item "Teensy4i2s API names unconfirmed" is now confirmed. The library exposes `i2sAudioCallback(int32_t** inputs, int32_t** outputs)` and `InitI2s()`. Sample rate and block size are `#define`s in its `AudioConfig.h` (defaults 48000 and 64). | Use it for M0 bring-up only (§3). |
| 9 | Info | `CLAUDE.md` still describes the SGTL5000 Audio Shield, the 16-bit Audio Library, 44.1 kHz, and 128-sample blocks. | Update it when M2 lands (§11). |

---

## 2. Budgets

### 2.1 CPU

At 600 MHz and 48 kHz there are **12,500 CPU cycles per sample**: 400 k cycles per 32-sample block.

Measured on the board (13 Nov 2026, 32-sample blocks, built-in example model, no IR loaded, serial `s`):

| Stage | CPU |
| --- | --- |
| NAM A2-Lite amp (with DC blocker and tone stack) | 32.9 % |
| Doubler | 1.1 % |
| Studio delay | 4.7 % |
| 80s reverb (hall) | 8.4 % |
| Drums and bass, rock style at 100 BPM | 4.0 % avg |
| Driver, conversion, gate, idle convolver | about 4.5 % |
| **Amp + doubler + delay + reverb** | **51.6 % avg, 52.4 % max** |
| **The same plus drums and bass** | **55.3 % avg, 67.9 % max** |

- Not yet measured here: an IR loaded. In teensy-ir-loader a 4096-sample IR cost about 12 % at this block size and a 2048-sample one about 6 %.
- So the expected worst case with a long IR and busy drums is around 80 %, the limit below. teensy-ir-loader measured 86 % for its worst case (240 BPM with crashes, IR switching) on the same processor.
- Rule: the worst case per block must stay at or below **80 %** of the block period. Levers if it does not: cap the IR at 2048 samples while the amp is on (about −6 %), 64-sample blocks, or hand-written assembly for the NAM tap passes (about −3 %).
- No overruns or FIFO errors in any of these runs.

### 2.2 Latency (analog in → analog out)

| Contributor | B = 32 | B = 16 | Notes |
| --- | --- | --- | --- |
| PCM1808 decimation filter | 0.36 ms | 0.36 ms | ≈ 17.4/fs **(verify)**. Fixed by the chip. |
| Block buffering (ping-pong DMA) | 1.33 ms | 0.67 ms | 2·B/fs: one block to fill, one block to play |
| SAI FIFOs | 0.04 ms | 0.04 ms | About 2 frames with our watermarks **(verify with the loopback test)**. The core's Audio Library settings keep about 22 frames queued (≈ 0.47 ms). |
| DSP algorithms | 0 | 0 | NAM is causal, and the IR partition size equals B |
| PCM5102A filter, FLT = L (as built) | 0.42 ms | 0.42 ms | ≈ 20/fs **(verify)**. 0.07 ms if the jumper is moved to H later. |
| **Total** | **≈ 2.2 ms** | **≈ 1.5 ms** | With FLT = H: ≈ 1.8 ms / ≈ 1.1 ms. Compare a typical floor modeller at 2–4 ms. |

- **Default block size B = 32. Stretch target B = 16.** B is a compile-time constant (`kBlockSize`). The choice is made at M1, after measuring how NAM efficiency changes at B = 16, where per-block overhead doubles.
- **Optional experiment (M2): TX phase offset.** Block buffering is 2·B only because TX is aligned to RX. Starting TX about 0.8·B after RX, which matches the 80 % CPU rule, cuts it to about 1.8·B. The gain is small (0.07–0.13 ms), so this is not part of the baseline.
- Banned on the audio path, because each adds latency: lookahead limiters, linear-phase EQ, oversampling, and FFT block sizes larger than B.

---

## 3. What we use, and where it comes from

Most of the firmware is ported from **teensy-ir-loader**, the author's earlier project for a Teensy 4.0 with an SGTL5000 Audio Shield. Its DSP was already portable, host-tested and designed for 48 kHz and 32-sample blocks. What is new here is the hardware layer.

| Component | Source | Notes |
| --- | --- | --- |
| I2S driver `lib/i2s32` | **New** | SAI1 + eDMA, 48 kHz, 32-bit slots, block callback in the DMA interrupt (§4.1) |
| Sample conversion `dsp/Convert.h` | **New** | 24-bit words ↔ float, saturating |
| NAM A2-Lite engine, `.nam` reader, weights cache | Ported | `dsp/nam/` (§6.2) |
| Amp stage, DC blocker, tone stack, noise gate | Ported | `dsp/nam/AmpStage.h`, `dsp/fx/` |
| IR loader and partitioned convolver | Ported | `dsp/conv/` (§6.3) |
| Doubler, studio delay, 80s reverb, effect slots | Ported | `dsp/fx/` |
| Tuner (YIN pitch detection) | Ported | `dsp/fx/PitchDetect.h`, `src/Tuner.*` |
| Drum machine, bass, MIDI drum files, kits, songs | Ported | `dsp/drums/`, `kits/`, `songs/`, `tools/` |
| The chain | Ported and adapted | `src/AudioChain.*`: was a Teensy Audio Library object with 16-bit I/O, now a plain class with float I/O |
| SD libraries, presets, settings, serial UI | Ported and adapted | `src/`, `lib/params`: built-in SDIO slot; codec and display code removed |
| OLED panel, encoder, keys | **Not ported** | Decision: no panel on this build for now |
| Teensyduino core, SD/SdFat, EEPROM, USB serial + MIDI | Framework | The PJRC Audio Library is not used |
| NeuralAmpModelerCore | Reference only | Benchmark (`env:nam_bench`) and the golden test vectors |

Third-party code that came along, with its notices kept in the file headers: hexefx_audiolib_F32 building blocks (MIT, Piotr Zapart), AudioDSPTools noise gate and DC blocker (MIT, Steven Atkinson), the NeuralAmpModelerCore example model (MIT), and AVL Drumkits samples (CC BY-SA 3.0, `kits/rock/CREDITS.md`).

Porting rules: ported files keep the old project's formatting; only the namespace (`irdsp` → `dsp`), include paths and placement macros change. Fixes should be made so they can go back to teensy-ir-loader.

## 4. Architecture

```
               ┌────────────────────────── Teensy 4.1 ──────────────────────────────┐
 guitar ─► Hi-Z│ SAI1 RX ─► eDMA ─► rx[2][B] ─┐                                       │
 preamp ─► ADC │                              │  DMA ISR (priority above USB/SD):     │
  (PCM1808)    │                              ├─► int24→float ─► Chain::process() ──┐ │
               │ SAI1 TX ◄─ eDMA ◄─ tx[2][B] ◄┘◄── float→int24 (clamp) ◄───────────┘ │
   DAC ◄───────│                                                                     │
 (PCM5102A)    │   ▲ params (double-buffered, atomic swap at block start)            │
               │   │                                                                 │
               │ loop(): serial shell · USB MIDI · SD (models / IRs / presets) ·      │
               │         loaders · CPU / latency stats · tuner display               │
               └─────────────────────────────────────────────────────────────────────┘
```

### 4.1 Audio driver: `lib/i2s32` (Teensy only)

- **Status: implemented in `lib/i2s32` and confirmed on the wired boards (13 Nov 2026):** clean guitar passthrough, exactly 1,500 blocks/s (48 kHz), 0.7 % load, no overruns or FIFO errors. Not yet measured: round-trip latency and 24-bit bit activity. `hw_tests/i2s_loopback.cpp` is a bare-board self-test (jumper pin 7 → pin 8); it was not run, and must not be run with the converter boards attached.
- Same SAI1 pins as the hardware doc. The Teensy is I2S master. MCLK = 256·fs, BCLK = 64·fs, 2 × 32-bit slots per frame. PLL4 is programmed for exactly 48 kHz.
- One eDMA channel each for RX and TX. Each uses a circular buffer of 2 halves × B frames × 2 channels × 32 bits. The buffers sit in `DMAMEM` (OCRAM), are 32-byte aligned, and get `arm_dcache_delete`/`arm_dcache_flush` around each access. At B=32 this is 1 KB in total.
- The RX half/complete interrupt runs the whole chain. It writes the TX half with the same index as the RX half it just read; that TX half has just finished playing, so the in→out delay is 2·B plus the FIFO depth. RX and TX are started together and TX is synchronous to RX, so this relationship stays deterministic.
- SAI FIFO watermarks are set low (TX requests at ≤ 1 word queued, RX at 1 word received). The core's defaults (28 and 16 words) would add about 22 frames of latency.
- The DMA interrupt priority is set above USB and SDIO, so audio pre-empts control traffic. USB and SD tolerate the delay of at most one block.
- Flush-to-zero and default-NaN are set in **FPSCR and FPDSCR** (`0xE000EF3C`). Setting FPDSCR makes the interrupt context inherit them. Denormals in NAM and reverb tails would otherwise cost large amounts of CPU.
- Conversion:
  - Implemented in the portable `dsp/Convert.h` and called from the block callback, so it is host-tested.
  - In: `(raw >> 8) * 2⁻²³` (the low byte is dropped).
  - Out: `x * 2²³`, saturated to [−2²³, 2²³−1], rounded, `<< 8`. NaN becomes 0.
  - The guitar input is mono, wired to the ADC **right** channel (`GUITAR_ON_RIGHT_CHANNEL` in `include/config.h`). The ADC left channel is spare: second input, re-amp, or expression input. **(Decide.)**
- Instrumentation: DWT `CYCCNT` per stage, plus max/avg per block.
- Overrun detection: an interrupt cannot pre-empt itself, so the handler checks at exit. It counts an overrun if its elapsed cycles exceed the block period, or if the next DMA half-interrupt is already pending.
- Mute line: the driver supports an optional GPIO wired to the DAC's XSMT (`Config::mutePin`), held low during boot until the chain runs. It is currently not wired (`-1`), because the XSMT rework is deferred (§1 item 1b).
- Fault path: on a streak of overruns the chain output is replaced with silence in software. A hard crash cannot be muted without the XSMT line; the DAC may replay the last buffer until reset.

### 4.2 DSP library: `lib/dsp` (portable, host-tested)

- Namespace `dsp`, no Arduino includes. Every block takes its memory from the caller, so the firmware decides what lives in DTCM, OCRAM or PSRAM and the same code runs in host tests.
- Each effect documents its own threading model: setters are called from `loop()`, `process()` runs in the audio interrupt, and a setter either writes single values or fills a spare set and switches to it.
- An effect that is off and has rung out goes idle: it costs no CPU and passes its input through bit-identically (`FxSlot`, `FxStage`).

### 4.3 The chain and the control plane: `src/`

- `AudioChain::process(in, outL, outR)` runs the whole chain for one block. `src/main.cpp`'s callback converts the ADC words to float, calls it, and converts back.
- `loop()` services the serial UI, the level meter and the delayed EEPROM save. File loading (IRs, models, presets, MIDI drum files) runs here; the audio never stops for it.
- Settings live in one table (`lib/params` `ParamDefs.cpp`): serial key, name, range, default and unit. The serial commands, the EEPROM record and the preset files all read it. `Params.cpp` says what each setting does.
- The table is identical to teensy-ir-loader's, so presets and the EEPROM layout are interchangeable. Three of its settings (`in`, `out`, `hp`) were codec levels and do nothing on this hardware.
- Cold code is kept out of RAM1 by `tools/imxrt1062_t41_cold.ld`; `tools/check_hot_code.sh` verifies that no audio-path function was linked into flash. This matters: before the rule was tightened, the NAM inner loops had landed in flash.

## 5. Signal chain

```
ADC R ─► 30 Hz high-pass (src/main.cpp, removes supply thumps)
      ─► amp (NAM A2-Lite, optional: DC blocker, tone stack)
      ─► noise gate (listens to the input, gates after the amp)
      ─► IR convolution (cab) ─► bypass blend × output gain          mono
      ─► doubler ─► studio 80s delay ─► 80s reverb (hall / gated)     stereo
      ─► + drums and bass (never through the guitar effects) ─► DAC L / R

tuner tap on the input (decimated to 24 kHz) → pitch detection in loop(); mutes the guitar
```

| Stage | What it does | Memory |
| --- | --- | --- |
| Amp | A2-Lite network, input and output gains, 5 Hz DC blocker, bass / middle / treble / presence. Click-free on/off and model swap; 132 ms warm-up after a load. | History 87 KB in DTCM |
| Noise gate | Hard gate with adjustable release (default), or the NAM plugin's expander | – |
| Cab IR | Up to 4096 samples, 25 ms crossfade between IRs, passthrough with none loaded | Spectra in DTCM |
| Doubler | A second "take" 1–35 ms later, panned opposite | 8 KB, OCRAM |
| Delay | Dual delay, 20–750 ms, ducking, drive, "era" | 144 KB DTCM + 144 KB OCRAM |
| Reverb | Hall (8-line FDN) or gated, pre-delay, "era" | 148 KB, OCRAM |
| Drums, bass | Sampled or synthesized kit, styles, songs, MIDI files, speed trainer | Kits in flash |

Planned in the first draft and not in the ported chain: compressor, chorus/flanger and a multi-band parametric EQ. They move to a "later" list (§13).

## 6. Core components in detail

### 6.1 Input calibration

- At the ADC pins, 0 dBFS = 1.06 Vrms ≈ +2.7 dBu **(datasheet value, not yet measured)**: `ADC_FULL_SCALE_DBU` in `include/config.h`. A buffer in front adds its loss as `INPUT_PAD_DB` (0 now; teensy-ir-loader's TL072 buffer is −6 dB).
- With `amp cal` on, the gain into the model is that full scale minus the model's `input_level_dbu` (or minus +12 dBu, the NAM plugin's default, for an uncalibrated capture). For the example model: 2.7 − 14.76 = −12.1 dB.
- The model's output is levelled to −12 dB loudness (`AMP_LEVEL_DB`) from the file's `loudness`, plus the `amp out` setting.
- There is no input gain control on this hardware, so a guitar's level at the ADC is what it is until a preamp exists (§1 item 1).

### 6.2 NAM A2-Lite

**Model facts** (from TONE3000's A2 guide **(verify against the reference code)**):
- WaveNet-like, feedforward and causal.
- 3 dilation stacks with kernel size 6 and dilations {1, 3, 7, 17, 41, 101, 239}, plus a 2-layer degridding pair (k = 15, d = {1, 13}). 23 layers in total.
- Leaky-ReLU activation (α = 0.01) and a convolutional head.
- Receptive field = 3·5·409 + 14·(1+13) + 1 = **6,332 samples (132 ms)**. A2-Full has 8 channels and A2-Lite has 3, both in the same file. **We target A2-Lite.** A2-Full is about 7× the compute and cannot run here.

**Memory:**
- Per-layer causal history is (k−1)·d samples × 3 channels, about 6.3 k × 3 × 4 B ≈ 76 KB as a minimum. With power-of-two ring buffers and block margin, plan for **130–150 KB**.
- Weights are a few KB **(verify)**.
- Both go in **DTCM** (zero wait state, not cached). TONE3000's Daisy implementation also pins weights in TCM.

**What the reference actually provides (checked Nov 2026):**
- The `nam-pedal` README publishes no A2 numbers at all, so the A2-Lite CPU claim is unverified until our own M1 benchmark.
- TONE3000's A2 guide says A2-Lite is real-time on a Cortex-M7 through two routes: the **A2 fast path** in NeuralAmpModelerCore (`NAM_ENABLE_A2_FAST`, on by default; unrolled kernels sized to A2's channel counts, about 6× faster than the Eigen path on desktop), and a "purpose-built", dependency-free C engine. Whether that C engine is published in `nam-pedal` is unclear **(verify)**.

**Phase A: benchmark and reference (M1)**
- First step: inspect the current NeuralAmpModelerCore, `nam-binary-loader` and `nam-pedal` repos to see what is published for A2 (fast path, C engine).
- Build the core + loader for the Teensy with the fast path enabled, float samples, and flush-to-zero.
- Benchmark without audio I/O, with A2-Lite models only: run the engine over a buffer in `loop()` and count cycles at B = 16, 32 and 48. This is the **go/no-go** for the CPU budget, and it decides B and the effects budget.
- Risks: Eigen with dynamic allocation in `prepare` is acceptable. Teensyduino builds with `-fno-exceptions`, and if the core library throws it must be patched or guarded. Both are to be checked.
- Same code on the host (native env): generates **golden vectors** (input sweep, noise, DI guitar clip → reference output) for tests.

**M1 result (13 Nov 2026): A2-Lite runs in real time on the Teensy 4.1.**

Measured on the board with `env:nam_bench` (`hw_tests/nam_bench.cpp`): NeuralAmpModelerCore at commit `d3189ed`, A2 fast path, the A2-Lite sub-model of the core's `example_models/A2.nam` (1,871 weights, 7.5 KB), white-noise input, 3 s per row, 600 MHz, -O2. Budget is 12,500 cycles per sample.

| Block (frames) | Cycles/sample avg | CPU avg | CPU worst block |
| --- | --- | --- | --- |
| 16 | 9,438 | 75.5 % | 80.0 % |
| 32 | 7,818 | 62.5 % | 64.4 % |
| 48 | 7,291 | 58.3 % | 59.6 % |
| 64 | 7,012 | 56.1 % | 57.2 % |
| 128 | 6,591 | 52.7 % | 53.6 % |

- Silence costs the same as noise (62.1 % at B = 32), so there is no denormal slowdown with flush-to-zero on.
- The vendor's "about 50 %" matches only at large blocks. Small blocks pay a per-block overhead: about 30 k cycles per block, from the slope between rows.
- **B = 16 is not usable with this engine**: 75.5 % for NAM alone leaves nothing inside the 80 % rule. **B = 32 stays the block size.**
- This is the unoptimised starting point: the model state is on the heap (OCRAM, cached) instead of DTCM, and the build is -O2. An -O3 build did not fit, see below.
- **RAM1 problem:** the reference engine with nlohmann/json, Eigen and the C++ exception runtime puts 392 KB of code in ITCM and leaves 22 KB for the stack. It cannot be combined with the rest of the firmware as it is, and -O3 overflows RAM1 by 6 KB. This makes Phase B necessary, not optional.
- What is published: the fast path exists in the core (`NAM/wavenet/a2_fast.cpp`, about 1,000 lines). `nam-pedal` and `nam-binary-loader` contain no separate C engine, and the `.namb` loader predates A2 (last change April 2026) **(not tested with an A2 file)**.
- Build notes: the core needs C++20; exceptions are enabled per file only (enabling them globally does not link on the Teensy); its one `thread_local` becomes a plain global; NAM/Eigen headers must be included before `Arduino.h`. All of this is in `scripts/nam_core.py`.
- Not yet done from M1: host build and golden vectors.

**Own engine result (13 Nov 2026): 32.4 % CPU at B = 32, bit-compatible with the reference.**

`lib/dsp/src/dsp/nam/NamA2.h` is ported from the author's earlier project, [teensy-ir-loader](https://github.com/volfovich/teensy-ir-loader) (`NamA2.h`, `NamFile.*`), where it ran on a Teensy 4.0 behind a 16-bit / 44.1 kHz codec with resampling to 48 kHz. Here it runs natively at 48 kHz with no resampling. Measured on this board with `env:nam_a2_bench` (`hw_tests/nam_a2_bench.cpp`), same model and conditions as the reference benchmark above:

| Block (frames) | History | Cycles/sample avg | CPU avg | CPU worst block | Reference engine |
| --- | --- | --- | --- | --- | --- |
| 16 | DTCM, 80 KB | 4,378 | 35.0 % | 36.0 % | 75.5 % |
| 32 | DTCM, 87 KB | 4,045 | 32.4 % | 32.9 % | 62.5 % |
| 64 | DTCM, 101 KB | 3,936 | 31.5 % | 31.8 % | 56.1 % |
| 32 | OCRAM, 87 KB | 4,511 | 36.1 % | 38.0 % | – |

- **Correct:** on the board and on the host, the output matches NeuralAmpModelerCore on the golden test signal to a maximum error of 4.8e-7, 130 dB below the signal (`test/test_nam_a2`, fixtures in `test/fixtures/nam`). All block sizes give identical output.
- **About half the CPU of the reference engine**, with a steady worst case, and almost no penalty for small blocks. **B = 16 is now possible**: NAM 35 % + IR about 7 % + driver 2 % leaves about 36 % for effects, and would cut latency by 0.67 ms. B stays 32 for now; the rest of the chain's costs at 16 are not measured (§12, next steps).
- **DTCM matters:** the same engine with its history in OCRAM costs 36.1 %.
- **Model files are read directly:** `dsp::readNamA2Lite` streams a `.nam` JSON file (A2 container or plain A2-Lite WaveNet) and extracts the Lite weights and metadata. The 666 KB example file parses in 43 ms from flash on the board; from SD it will be slower **(measure)**. So **no host converter and no `.namb` format are needed**: copy `.nam` files to the SD card. Files that are not A2 are refused with a message.
- No Eigen, JSON library, exceptions or heap: the reference engine is now only a benchmark and the source of golden vectors.

**In the live audio path (13 Nov 2026).** `src/main.cpp` now runs guitar → input gain → `NamA2` → output level → DAC, with the example A2 model embedded in flash and read at boot by `readNamA2Lite`. Measured on the board: **33.8 % total load (max 34.0 %), no overruns or FIFO errors.**
- Input gain defaults to the model's calibration (`kAdcFullScaleDbu` 2.7 − `input_level_dbu` 14.76 = −12.1 dB); output defaults to loudness normalisation to −18 dB (+6.0 dB for this model). Both have serial trims (`g`, `v`).
- The model always runs, so switching the amp on and off (`a`) has no gap; the output is muted for the 132 ms warm-up after boot, then faded in.
- With nothing playing, the input peaks at about −84 dBFS and the amp output at about −53 dBFS: the model adds about 30 dB of gain to the input noise, so a noise gate is needed (§5 stage 2).
- This simple path was then replaced by the ported `AmpStage` inside `AudioChain` (§5): model switching, the DC blocker, tone controls and the gate come with it.

**How the engine works (design notes for further tuning):**
- A layer at a time over the whole block; two taps per pass with their 18 weights held in registers; one more pass for LeakyReLU, the head sum and the 1x1 residual, written straight into the next layer's ring buffer.
- One ring buffer per layer, sized to the taps' reach plus a block, so every block costs the same.
- Remaining levers, from the earlier project's measurements: hand-written assembly for the tap passes (about −3 % CPU) and 720 MHz overclock (not planned).

**Model loading and switching (as ported, `AmpLibrary` + `AmpStage`):**
- `.nam` files are copied to `/AMP` on the SD card as downloaded. `loop()` streams the file through `dsp::readNamA2Lite` into a staging buffer, or reads the model's 7.7 KB cache file if it has one.
- With the amp on, a new model is swapped in within a 20 ms fade to silence, the network warms up for its receptive field (132 ms) in silence, then fades back in: a gap of about 170 ms. With the amp off the weights are just swapped.
- Switching the amp on keeps the dry signal during the warm-up, then crossfades to the amp in 20 ms. Switching it off stops the network, so it costs no CPU.

**Scope: NAM A2 only (A2-Lite).** A1 models are not supported, loaded, or benchmarked; the loader rejects them.

### 6.3 IR loader (cab simulation), as ported

- **Files:** WAV in the card's root or `/IR`: mono or stereo, 8/16/24/32-bit PCM or 32/64-bit float, including `WAVE_FORMAT_EXTENSIBLE`, at any rate from 8 to 384 kHz.
- **On load** (in `loop()`): streamed from the card, resampled to 48 kHz with a windowed-sinc kernel, leading silence and inaudible tail trimmed, cut at 4096 samples with a fade, and loudness-normalised.
- Resampling an IR file once at load is file preparation, not sample-rate conversion in the audio path. So the first draft's rule "reject non-48 kHz files, prepare them with a host tool" is dropped.
- **Convolution:** uniformly partitioned overlap-save with partition = block size, so it adds no latency. Two spectrum sets share one input history, so a new IR is crossfaded in over 25 ms without clicks.
- The 56 KB resampler table is only used while loading, so on this board it lives in PSRAM.

### 6.4 Tuner

- The chain copies the input, low-passed and decimated to 24 kHz, into a ring; `loop()` runs YIN on it. The guitar is muted while the tuner is open, before the delay and reverb so their tails ring out.

## 7. Memory map (as built, from `teensy_size`)

| Region | Used | Free | What is there |
| --- | --- | --- | --- |
| RAM1 code (ITCM) | 64 KB | – | The audio path only; background code stays in flash |
| RAM1 data (DTCM) | 362 KB | 96 KB for the stack | NAM history (87 KB), one delay line (144 KB), the convolver's spectra and history, effect state |
| RAM2 (OCRAM) | 449 KB | 75 KB heap | The other delay line (144 KB), reverb (148 KB), file lists, load buffers, DMA buffers |
| PSRAM | 57 KB | about 7.9 MB | IR resampler table. Free for a longer delay or a looper later; measure before using it per sample. |
| Flash | about 1.6 MB | 6.5 MB | Code, the built-in example model (666 KB as JSON), drum kits |

- The NAM history must stay in DTCM: in OCRAM the engine costs 36.1 % instead of 32.4 %.
- RAM1 is the tight one. The two things that keep it workable are the cold-code linker script and reading model files as a stream.

## 8. Presets, settings and control (as ported)

- **Presets:** whole sounds as plain text files in `/PRESETS` on the card (amp and model, gate, IR, doubler, delay, reverb, level). Not JSON, so no ArduinoJson.
- **Settings:** the last IR and model, levels and every effect and drum setting, in EEPROM, saved a moment after the last change.
- **Models:** `.nam` files in `/AMP`. A 7.7 KB cache per model in `/AMP/.cache` makes later loads fast. With no model on the card, a built-in example model is listed.
- **Serial UI:** `?` lists the commands; `s` prints the status with per-stage CPU, driver overruns and levels; `m` shows a live meter. Every setting has a key from the table (`amp on`, `dl on`, `dtime 380`, `rv on`, `pre ...`, `drum ...`).
- **USB MIDI:** enumerated, incoming messages are drained, nothing is mapped yet.
- **Not here:** the OLED panel and its controls (teensy-ir-loader's `DISPLAY.md`). Pins 2, 3, 4, 5, 9, 16, 17 are free if it is added later.
- For details of each block see teensy-ir-loader's `NAM.md`, `DELAY.md`, `REVERB.md`, `PRESETS.md` and `DRUMS.md`; they describe the same code.

## 9. Build environments

| Env | What it is |
| --- | --- |
| `teensy41` (default) | The firmware. `USB_MIDI_SERIAL`, `DSP_BLOCK=32`, cold-code linker script, drum kits and songs compiled by `tools/pio_kits.py`. |
| `native` | Host unit tests (`pio test -e native`) |
| `i2s_loopback` | Driver self-test on a bare board (jumper pin 7 → 8). Not with the converters attached. |
| `nam_a2_bench` | Own NAM engine: on-board check against the reference output, and CPU cost |
| `nam_bench` | Reference NAM engine CPU cost (fetches NeuralAmpModelerCore and Eigen into `third_party/`) |
| `psram_test`, `psram_deep` | PSRAM hardware tests |

## 10. Tests

**Host (`pio test -e native`): 15 suites, all passing.** The suites ported from teensy-ir-loader run its original checks unchanged inside one Unity test each.

| Suite | Covers |
| --- | --- |
| `test_convert` | 24-bit ↔ float: exact round trip, saturation, NaN |
| `test_nam_a2` | Engine against NeuralAmpModelerCore (max error 4.8e-7, 130 dB below the signal), block sizes, reset |
| `test_nam` | Amp stage switching and model swap, both gates against a reference, DC blocker, tone stack |
| `test_namfile` | `.nam` reader: key orders, wrong architectures, damaged files, the weights cache |
| `test_convolver`, `test_irloader` | Convolution against direct FIR, IR crossfade; WAV formats, resampling, trimming, normalisation |
| `test_fxstage`, `test_studio_delay`, `test_reverb80`, `test_doubler`, `test_pitch` | The effects and the tuner |
| `test_drums`, `test_bass`, `test_midi` | Drum machine, bass, MIDI drum files |
| `test_presets` | Settings table and preset files |

**On the board:**
- Done: clean passthrough by ear; own NAM engine matches the reference; amp, doubler, delay and reverb running with no overruns; drums running.
- After every change to the linker script or the audio path: `tools/check_hot_code.sh`.
- Not done: IR loading, model loading and presets from a real SD card; latency by analog loopback; 24-bit bit activity; a long soak with everything on; a listening check of the delay, reverb and drums on this hardware.

## 11. Repository layout

```
lib/
  i2s32/                 I2S driver (Teensy only)
  dsp/src/dsp/           portable DSP, namespace dsp
    Convert.h  ByteSource.h  Port.h  Blocks.h  Fft.h
    nam/                 NamA2.h  NamFile.*  NamCache.h  AmpStage.h  GoldenSignal.h
    conv/                PartitionedConvolver.h  IRLoader.*
    fx/                  NoiseGate  ToneStack  Doubler  StudioDelay  Reverb80  FxSlot  FxStage  PitchDetect
    drums/               DrumMachine  DrumPatterns  MidiDrums  PluckBass  (+ generated kit and song data)
  params/                ParamDefs.*  PresetFile.*
src/                     main.cpp  AudioChain.*  IRLibrary  AmpLibrary  PresetLibrary  Params  Settings
                         SerialUI  Tuner  DrumSongs  SdSource.h
include/config.h         compile-time settings and hardware constants
hw_tests/                psram_*.cpp  i2s_loopback.cpp  nam_bench.cpp  nam_a2_bench.cpp
tools/                   imxrt1062_t41_cold.ld  check_hot_code.sh  make_kits.py  make_songs.py  import_kit.py  pio_kits.py
scripts/                 system_headers.py  nam_core.py
kits/  songs/            drum kit samples and built-in songs
test/                    Unity suites and test/fixtures/nam
```

## 12. Milestones

| M | Deliverable | State |
| --- | --- | --- |
| M0 | Hardware bring-up | Done by ear (clean passthrough). Measurements (24-bit activity, noise floor) not done. |
| M1 | NAM A2-Lite benchmark | Done: reference engine 62.5 %, own engine 32.4 % at 32-sample blocks |
| M2 | Own I2S driver | Done and running. Latency measurement and the software fault path are open. |
| M3 | IR loader | Ported and host-tested. Not yet exercised with a real SD card. |
| M4 | NAM in the chain | Done with the built-in model. Loading from SD not yet exercised. |
| M5 | Effects, presets, drums | Ported and running. Listening check and SD presets open. |
| M6 | Input preamp, physical controls | Not started. The OLED panel code exists in teensy-ir-loader. |

Next steps, in order:
1. Put an SD card in the Teensy 4.1 slot with IRs in `/IR` and A2 models in `/AMP`; check loading, presets and the worst-case CPU with a long IR.
2. Measure latency and the input level of a real guitar; decide on the input buffer.
3. Revisit 16-sample blocks: the amp is 35 % there, but the convolver and effects costs rise too.

## 13. Risks and open items

| Risk / question | Impact | Mitigation |
| --- | --- | --- |
| Worst-case CPU with a long IR, busy drums and everything on is unmeasured here (expected near 80 %) | Overruns | Measure with an SD card; cap the IR at 2048 while the amp is on; 64-sample blocks as a fallback |
| SD paths are ported but untested on this board (built-in SDIO instead of SPI) | IR, model and preset loading may need fixes | First thing to test with a card |
| No Hi-Z input and no input gain | Dull, quiet guitar; calibration off by the unknown loss | Build the buffer; set `INPUT_PAD_DB` |
| ADC full-scale level is a datasheet figure | Amp drive level off by a few dB | Measure with a known signal; adjust `ADC_FULL_SCALE_DBU` |
| The SD card disturbs the ADC: a thump below 20 Hz at −66 dBFS about every 1.4 s, in every card state (selected, stand-by, idle, controller clock off). Likely the card's current pulses on the supply. | Opened the gate into high-gain models: audible bursts | **Fixed in firmware** by a 2nd-order high-pass on the input (`INPUT_HIGHPASS_HZ`, now 30 Hz; first tried at 40 Hz, where 15 s idle with the card in peaked at −84 dBFS). The cause is still there; a cleaner ADC supply would remove it at the source. Serial `cap` records 3 s of raw input for this kind of check. |
| DAC cannot be muted by firmware (XSMT rework deferred) | Buzz on a firmware crash; possible pop on boot or flash | Volume down before flashing or resetting |
| A hot function linked into flash | Large, silent CPU increase | `tools/check_hot_code.sh` after linker-script or audio-path changes |
| Engine speed depends on the compiler's register allocation | CPU could change with a toolchain update | Re-run `env:nam_a2_bench` after toolchain changes |
| Two copies of the ported code (here and teensy-ir-loader) | Fixes drift apart | Keep ported files close to the original; port fixes both ways |
| RAM2 heap is 75 KB | Tight if more load buffers are added | PSRAM for load-time buffers |
| Later features | – | Compressor, chorus, parametric EQ, USB MIDI mapping, OLED panel, 16-sample blocks, longer delay or looper in PSRAM |

## Sources

- [TONE3000: Introducing NAM Architecture 2 (A2)](https://www.tone3000.com/blog/introducing-neural-amp-modeler-nam-architecture-2-a2)
- [TONE3000: NAM A2, the complete guide](https://www.tone3000.com/guides/nam-a2-the-complete-guide) (architecture, A2-Lite, `.namb`, Cortex-M7 claim)
- [tone-3000/nam-pedal](https://github.com/tone-3000/nam-pedal): Daisy Seed reference (48 kHz, 48-sample blocks, NeuralAmpModelerCore, `nam2namb`)
- [ValdemarOrn/Teensy4i2s](https://github.com/ValdemarOrn/Teensy4i2s): 32-bit I2S for Teensy 4 (`i2sAudioCallback`, `InitI2s`, `AudioConfig.h`)
- Hardware doc in this repo, and its sources
