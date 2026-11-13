# Teensy 4.1 Guitar DSP Platform: High-Level Design

Nov 12, 2026 · Status: draft for review

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

At 600 MHz and 48 kHz there are **12,500 CPU cycles per sample**: 400 k cycles per 32-sample block, or 200 k per 16-sample block.

| Stage | Target share | Notes |
| --- | --- | --- |
| NAM A2-Lite | **32.4 % measured at B = 32** (35.0 % at B = 16) | Own engine `dsp::NamA2`, history in DTCM (§6.2). The reference engine measured 62.5 % / 75.5 %. |
| IR cab, 2048 taps | ~4 % (B=32) / ~7 % (B=16) | Partitioned FFT convolution (§6.3) |
| Gate, comp, EQ, mod, delay, reverb | ≤ 20 % target; **about 40 % available at B = 32, about 36 % at B = 16** | 80 % rule minus NAM, IR and driver |
| Driver, conversion, parameter smoothing | ~2 % | |
| **Headroom** | **≥ 20 %** | Covers interrupt jitter, PSRAM cache misses, and USB/SD activity |

Rule: the sum of worst-case stage costs per block, measured with the DWT cycle counter, must stay at or below **80 %** of the block period. The shares above are targets within that 80 %. If NAM measures above 50 % at M1, the effects budget shrinks first.

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

## 3. What we use, how, and what we build

| Component | Decision | How it is used |
| --- | --- | --- |
| Teensyduino core 1.62 (PlatformIO `teensy`) | **Use** | Boot, clocks, `DMAChannel`, USB serial and MIDI, `EXTMEM`/`DMAMEM`, `elapsedMillis` |
| PJRC Teensy Audio Library | **Drop** | 16-bit / 44.1 kHz / 128-sample blocks. Kept only until M2 replaces `src/main.cpp`. |
| [Teensy4i2s](https://github.com/ValdemarOrn/Teensy4i2s) | **Use for bring-up (M0)** | 32-bit I2S at 48 kHz, same pins. Proves the hardware before we write a driver. Not used in production: its `AUDIO_BLOCK_SAMPLES` name clashes with the core's, and its buffer queue adds latency. |
| OpenAudio_ArduinoLibrary (F32) | **Don't use** | An AudioStream-style graph scheduled from a software interrupt adds buffering. Its I2S 24/32-bit support is uncertain **(verify)**. We need none of its blocks. |
| **Own I2S driver** (`lib/i2s32`) | **Build** | SAI1 + eDMA ping-pong, 32-bit words, PLL4 at exactly 48 kHz, compile-time B, processing in the DMA interrupt. Modelled on the core's `output_i2s.cpp`/`input_i2s.cpp` configuration and on Teensy4i2s. |
| CMSIS-DSP (`arm_math.h`, already linked) | **Use** | `arm_rfft_fast_f32` for the IR, `arm_biquad_cascade_df2T_f32` for EQ, vector multiply/add/scale, and complex multiply-accumulate. |
| [NAM A2](https://www.tone3000.com/guides/nam-a2-the-complete-guide) (MIT) | **Use the model format and architecture** | A2-Lite variant (3 channels). `.nam` files are read directly on the device (§6.2); no converter. |
| NeuralAmpModelerCore + `nam-binary-loader` (as used in [tone-3000/nam-pedal](https://github.com/tone-3000/nam-pedal), MIT) | **Use as reference, then tune or replace** | Phase A runs it on the Teensy with the A2 fast path (`NAM_ENABLE_A2_FAST`) to benchmark, and on the host to produce golden vectors. Phase B is an optimised engine: TONE3000's C engine if it is published, otherwise our own (§6.2). |
| SdFat (bundled with Teensyduino, built-in SDIO slot) | **Use** | Reads `.nam` models, IR WAVs and presets from `loop()` only. |
| ArduinoJson | **Use** | Preset files. |
| `usbMIDI` (core) | **Use** | Program change selects presets; control changes set parameters. |
| `lib/dsp` (ours, portable, header-only) | **Build and extend** | All algorithms, unit-tested on the host. The existing `dsp::Biquad` is reused. |
| `lib/audio_fx` (AudioStream wrappers) | **Delete at M2** | No AudioStream on the new path. |

---

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
  - The guitar input is mono, wired to the ADC **right** channel (`kGuitarOnRightChannel` in `src/main.cpp`). The ADC left channel is spare: second input, re-amp, or expression input. **(Decide.)**
- Instrumentation: DWT `CYCCNT` per stage, plus max/avg per block.
- Overrun detection: an interrupt cannot pre-empt itself, so the handler checks at exit. It counts an overrun if its elapsed cycles exceed the block period, or if the next DMA half-interrupt is already pending.
- Mute line: the driver supports an optional GPIO wired to the DAC's XSMT (`Config::mutePin`), held low during boot until the chain runs. It is currently not wired (`-1`), because the XSMT rework is deferred (§1 item 1b).
- Fault path: on a streak of overruns the chain output is replaced with silence in software. A hard crash cannot be muted without the XSMT line; the DAC may replay the last buffer until reset.

### 4.2 DSP engine: `lib/dsp` (portable, host-tested)

- `dsp::Processor` interface:
  - `prepare(fs, maxBlock)`, `process(float* buf, size_t n)` (mono), or a stereo variant.
  - `reset()`, `setBypass(bool)` with a short crossfade, and `latencySamples()`, which must return 0 on the audio path.
- `dsp::Chain` has fixed stage slots in a fixed order (§5). There is no dynamic graph and no allocation in `process`. All memory is reserved at `prepare()` from static arenas, and each arena's placement (DTCM, OCRAM or PSRAM) is chosen by the Teensy-side wrapper.
- `dsp` code has no Arduino includes. Memory placement is passed in as pointers and sizes, so the same code runs in host tests.

### 4.3 Control plane: `loop()`, `src/`

- **Parameter flow:** `loop()` writes a full `ChainParams` struct into the back buffer, then publishes it by setting an index (`std::atomic<uint32_t>`). The interrupt picks it up at the next block start. This needs no locks and guarantees the interrupt never sees a half-written struct.
- **Smoothing:** continuous parameters (gains, mix, cutoff) are ramped per sample (one-pole or linear over 10–20 ms) to avoid zipper noise. Biquad coefficients are recomputed in `loop()` and swapped with the parameter set.
- **Asset loading** (model, IR) happens entirely in `loop()`, into a staging buffer, followed by a hand-off (§6.2, §6.3). The interrupt never touches SD or calls `malloc`.
- **Interfaces:**
  - Serial shell: extends the existing `pollSerial`/`handleCommand` pattern in `src/main.cpp`.
  - USB MIDI: program change selects presets; control changes are mapped to parameters.
  - Later: footswitches, encoders, display.

---

## 5. Signal chain

```
ADC R ─► [1] input cal + DC block ─► [2] noise gate ─► [3] comp ─► [4] boost/drive
      ─► [5] NAM A2-Lite ─► [6] IR cab ─► [7] EQ ─► [8] mod (chorus)
      ─► [9] delay ─► [10] reverb ─► [11] stereo out: level + safety clip ─► DAC L/R
                      └── mono → stereo split at [9] (ping-pong delay, stereo reverb)

tuner tap after [1]  →  ring buffer  →  pitch detection in loop()
```

| # | Stage | Algorithm | Memory | Approx. CPU |
| --- | --- | --- | --- | --- |
| 1 | Input cal + DC block | Gain from calibration (§6.1) plus a 1-pole HPF at about 10 Hz | DTCM | < 0.5 % |
| 2 | Noise gate | Envelope follower with hysteresis and attack/hold/release. Always detects on the clean input. Where the gain reduction is applied is a parameter: before NAM, after the IR, or both. Gating only in front does not silence a high-gain model that amplifies residual input noise, so "after" is the default for high-gain presets. | DTCM | < 0.5 % |
| 3 | Compressor (opt.) | Feed-forward, peak/RMS detector, no lookahead | DTCM | ~1 % |
| 4 | Boost / drive (opt.) | Clean boost plus tone (biquad) at first. A drive stage would be a second small NAM, or static waveshaping with an anti-aliasing compromise. **(Decide later; oversampling is banned, §2.2.)** | DTCM | ~1 % |
| 5 | **NAM A2-Lite** | §6.2 | DTCM | ≤ 50 % |
| 6 | **IR cab** | §6.3 | OCRAM | ~4 % |
| 7 | EQ | 5-band parametric plus low/high cut, built on `dsp::Biquad` (CMSIS DF2T cascade on target) | DTCM | ~1 % |
| 8 | Modulation | Chorus/flanger: modulated short delay, linear or Hermite interpolation, max 30 ms | DTCM | ~1 % |
| 9 | Delay | Up to 2.5 s, feedback with filtering, tap tempo, ping-pong. Block-wise PSRAM access (§7). | PSRAM, plus a scratch buffer in DTCM | ~2 % |
| 10 | Reverb | 8-line FDN (Householder matrix) or Dattorro plate, with damping and pre-delay | OCRAM | ~5 % |
| 11 | Output | Master level, soft safety clip just below 0 dBFS, 24-bit conversion. No dither is needed, since float→24-bit truncation noise is far below the DAC's 112 dB. | – | < 0.5 % |

Every stage has bypass (with a 5–10 ms crossfade) and its own cycle meter. Spillover of delay and reverb tails across preset changes is a nice-to-have.

---

## 6. Core components in detail

### 6.1 Input calibration

- At the ADC pins, 0 dBFS = 1.06 Vrms ≈ +2.7 dBu **(verify by measurement)**. With a preamp of gain G dB, 0 dBFS at the guitar jack = (2.7 − G) dBu.
- NAM model metadata carries `input_level_dbu`: the dBu level that maps to 0 dBFS during capture. The digital trim is `trim_dB = (2.7 − G) − input_level_dbu`, so the model sees the same drive level as the real amp did.
- Use `output_level_dbu` and loudness metadata the same way, to level-match between models.
- Fall back to a manual input trim when a model has no metadata.

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
- **About half the CPU of the reference engine**, with a steady worst case, and almost no penalty for small blocks. **B = 16 is now possible**: NAM 35 % + IR about 7 % + driver 2 % leaves about 36 % for effects, and would cut latency by 0.67 ms. B stays 32 until the rest of the chain exists; revisit at M5.
- **DTCM matters:** the same engine with its history in OCRAM costs 36.1 %.
- **Model files are read directly:** `dsp::readNamA2Lite` streams a `.nam` JSON file (A2 container or plain A2-Lite WaveNet) and extracts the Lite weights and metadata. The 666 KB example file parses in 43 ms from flash on the board; from SD it will be slower **(measure)**. So **no host converter and no `.namb` format are needed**: copy `.nam` files to the SD card. Files that are not A2 are refused with a message.
- No Eigen, JSON library, exceptions or heap: the reference engine is now only a benchmark and the source of golden vectors.

**How the engine works (design notes for further tuning):**
- A layer at a time over the whole block; two taps per pass with their 18 weights held in registers; one more pass for LeakyReLU, the head sum and the 1x1 residual, written straight into the next layer's ring buffer.
- One ring buffer per layer, sized to the taps' reach plus a block, so every block costs the same.
- Remaining levers, from the earlier project's measurements: hand-written assembly for the tap passes (about −3 % CPU) and 720 MHz overclock (not planned).

**Model loading and switching:**
- `.nam` files are copied to the SD card under `/models/` as downloaded.
- `loop()`:
  1. Stream the `.nam` file through `dsp::readNamA2Lite` into a staging weight buffer (1,871 floats). A small cache file next to the model can skip the JSON parse on later loads, as in teensy-ir-loader (`NamCache.h`), if SD parsing proves slow.
  2. Validate (architecture = A2, Lite variant, 48 kHz).
  3. Ask the interrupt to fade out (10 ms). The interrupt then skips the NAM stage.
  4. `loop()` swaps the weights and clears the history. Clearing about 150 KB is too slow to do inside one block, which is why it happens here and not in the interrupt.
  5. **Prewarm**: the interrupt runs the new model on silence for the receptive field (132 ms) with output muted, because biases make "zeroed state" differ from "steady state".
  6. Fade in.
- A model change therefore causes about 150 ms of silence, which is acceptable for preset changes. Crossfading two NAMs is not possible within the CPU budget.

**Scope: NAM A2 only (A2-Lite).** A1 models are not supported, loaded, or benchmarked; the loader rejects them.

### 6.3 IR loader (cab simulation)

- **Format:** mono WAV, 48 kHz, up to **2048 taps (43 ms)**. A host tool `tools/ir_prep` resamples, trims, and normalises to 32-bit float WAV. The device accepts only 48 kHz and rejects anything else, so it never resamples.
- **Algorithm:** uniformly partitioned overlap-save convolution (UPOLS) with partition size = B and FFT size = 2B. It **adds no latency** beyond the I/O block.
  - B = 32: 64 partitions, FFT size 64, 33 bins.
  - Per block: 1 real FFT, 64 × 33 complex multiply-accumulates, 1 inverse FFT. About 15 k cycles, or about 4 % CPU.
  - A direct 2048-tap FIR would cost about 20 %, so it is used only as the test reference.
  - At B = 16 the per-sample cost doubles (128 partitions). If B = 16 is chosen and the IR share matters, fall back to two-stage partitioning: a head at B and a tail at 4·B or 8·B, with the tail's work spread evenly across blocks. This still adds no latency.
- **Memory:** IR spectra plus the input frequency-domain delay line ≈ 2 × 17 KB, in OCRAM (DMAMEM, cached).
- **Switching:** a new IR is transformed in `loop()` into a second convolver instance, and the interrupt **crossfades** the two over about 20 ms with no gap. Two convolvers run only during the crossfade.
- **Options:** IR level normalisation; low/high cut implemented with `dsp::Biquad`. Optional dual-IR blend (two convolvers, about 8 %) if budget allows.

### 6.4 Tuner

- The interrupt copies the post-input signal, decimated 4× (12 kHz), into a ring buffer.
- `loop()` runs YIN/McLeod pitch detection on it, using no audio-thread CPU.
- Tuner mode can mute the output.

---

## 7. Memory map

| Region | Size | Use |
| --- | --- | --- |
| ITCM (RAM1) | shared 512 KB | `FASTRUN` hot code: interrupt handler, NAM kernels, convolution MAC loop |
| DTCM (RAM1) | (rest of 512 KB) | NAM weights and history (130–150 KB), small DSP state (gate, comp, EQ, chorus ≤ 30 ms ≈ 6 KB), per-block scratch, parameter sets |
| OCRAM (RAM2, `DMAMEM`) | 512 KB | DMA buffers (1 KB), IR spectra and delay line (~34 KB ×2 during crossfade), reverb delay lines (~128 KB), asset staging buffers, heap |
| PSRAM (`EXTMEM`) | 8 MB | Delay line (2.5 s × 2 ch × 4 B ≈ 1 MB), future looper. **Zeroed in `setup()`** (not initialised at boot). Accessed block-wise with memcpy to and from DTCM scratch, never with scattered per-sample reads (≈ 0.9 µs per cache miss). |
| SD card | – | `/models/*.nam`, `/irs/*.wav`, `/presets/*.json` |

RAM1 is shared between code (ITCM) and data (DTCM), and Teensyduino places all code in ITCM by default. Cold code (SdFat, ArduinoJson, the shell, the loaders) is marked `FLASHMEM` so that it stays in flash and leaves RAM1 for the NAM state.

---

## 8. Presets and control

- **Preset** = JSON on SD:
  - model file and IR file names
  - per-stage bypass and parameters
  - input trim, output level
  - MIDI mappings
- 128 presets are addressable through MIDI program change.
- **Serial shell** (extends the current command parser):
  - `s` stats: CPU per stage, max cycles, missed deadlines, latency setting
  - `p <n>` preset
  - `m <file>` model
  - `ir <file>` IR
  - `set <stage>.<param> <value>`
  - `bypass <stage>`
  - `save <n>`
- **USB MIDI:**
  - program change → preset
  - control change → mapped parameters (expression on CC 11, etc.)
  - tap tempo on a CC
- **Physical controls** (M6, hardware TBD): 2–3 footswitches (bypass, preset up/down, tap), 4 encoders, and a small OLED.

---

## 9. Build configuration changes

- `env:teensy41`:
  - `-D USB_MIDI_SERIAL`
  - remove `AUDIO_BLOCK_SAMPLES` and `AUDIO_SAMPLE_RATE_EXACT`
  - add `-D DSP_FS=48000 -D DSP_BLOCK=32`
  - benchmark `TEENSY_OPT_FASTEST` (-O3) against -O2 for the DSP path
  - add `-fno-math-errno`; evaluate `-ffast-math` for `lib/dsp` only. The host tests must use the same math flags as the target, or golden-vector comparisons will drift.
- `env:nam_bench` (exists): `hw_tests/nam_bench.cpp`, the NAM reference engine run over a buffer with cycle counts, no audio I/O. `scripts/nam_core.py` fetches pinned NeuralAmpModelerCore and Eigen into `third_party/` (git-ignored).
- New native program `tools/render` (§10): WAV in → `dsp::Chain` → WAV out.
- `env:i2s_loopback` (exists): `hw_tests/i2s_loopback.cpp`, the driver self-test on the bare board with a pin 7 → pin 8 jumper.
- New `env:i2s_bringup` (M0): `hw_tests/i2s_bringup.cpp` with Teensy4i2s as a `lib_deps` git dependency, `USB_SERIAL`. Contents: sine out, passthrough, 24-bit bit-activity check, input noise RMS.
- New `env:latency_test`: impulse out → analog loopback → measured round trip in samples.
- `env:native` (host tests): unchanged, plus NeuralAmpModelerCore for golden-vector generation, or golden vectors checked in as binary fixtures. **(Decide at M1.)**

---

## 10. Testing strategy

**Host (`pio test -e native`, Unity)**, one suite per algorithm, written before any hardware testing (per `CLAUDE.md`):

| Test | Checks |
| --- | --- |
| `test_biquad` (exists) | Response, stability |
| `test_convolver` | UPOLS against direct FIR on random IRs and inputs (error < −120 dB); latency = 0; IR swap crossfade has no clicks |
| `test_nam_a2` | Golden vectors from the reference engine; receptive field; prewarm reaches steady state; no denormal slowdown on silence |
| `test_dynamics` | Gate and compressor thresholds and time constants |
| `test_delay`, `test_reverb` | Feedback stability at maximum settings; decay time; no DC build-up |
| `test_chain` | Stage order, bypass crossfades, parameter swap, `latencySamples() == 0` for all stages |
| `test_convert` | int24↔float round trip, clamp at ±1, no overflow at full scale |

**Host offline renderer (`tools/render`):** a native program that runs a WAV file through the same `dsp::Chain` code as the firmware and writes a WAV. It serves two purposes:
- Development and listening tests for NAM, IR and effects using DI recordings, before the hardware and preamp are ready.
- Regression checks: render a fixed input with a fixed preset and compare with a stored reference file.

**On target:**
- Driver self-test (`env:i2s_loopback`, bare board plus one jumper): sample rate is 48 kHz, every bit of every word survives TX → RX, round-trip delay in frames, no errors at 50 % and 80 % callback load, overruns are detected and the driver recovers, then a soak.
- Bring-up checklist from the hardware doc.
- 24-bit check: bits 15–8 of the raw words are active.
- CPU per stage with `CYCCNT`, at worst-case settings, for 10 minutes with no missed deadlines.
- Latency measured by analog loopback and compared with §2.2.
- Noise floor with inputs shorted.
- Model and IR switching while playing.
- SD and USB traffic during audio, with no glitches.

---

## 11. Repository layout (target)

```
lib/
  i2s32/           SAI1 + eDMA 48 kHz / 32-bit driver (Teensy only)
  dsp/src/dsp/
    Biquad.h       (existing)
    Processor.h  Chain.h  Params.h  Smoother.h  Convert.h
    nam/           NamA2.h (A2-Lite engine), NamFile.* (.nam reader), GoldenSignal.h
    conv/          UPOLS convolver, FFT abstraction (CMSIS on target, simple FFT on host)
    fx/            gate, comp, eq, chorus, delay, reverb, tuner
src/
  main.cpp         setup, interrupt hook → Chain, loop: shell / MIDI / loaders
  control/         serial shell, MIDI map, presets (ArduinoJson), asset loaders (SdFat)
hw_tests/
  psram_*.cpp      (existing)  i2s_bringup.cpp  nam_bench.cpp  latency_test.cpp
tools/
  ir_prep/         resample / trim / normalise IRs to 48 kHz
  render/          host WAV → Chain → WAV renderer
test/              Unity suites (§10)
```

`lib/audio_fx/` is removed at M2. `CLAUDE.md` is updated at M2 to cover the new hardware, 48 kHz / 24-bit, no Audio Library, and the new commands.

The FFT used in the convolver must also run on the host, where CMSIS is unavailable. A thin `dsp::RealFft` wrapper selects CMSIS on target and a small radix-2 FFT on the host.

---

## 12. Milestones

| M | Deliverable | Exit criteria |
| --- | --- | --- |
| M0 | Hardware bring-up with Teensy4i2s | Hardware doc checklist passes; sine out; passthrough; 24-bit bits active; noise RMS logged. Boards as built (no re-soldering). |
| M1 ✔ (benchmark done: go, B = 32) | **NAM A2-Lite benchmark (go/no-go).** A2-Lite models only. Reference core with the A2 fast path on the Teensy, no audio I/O. Host build produces golden vectors. Choose B (16 or 32). | A2-Lite cycles per sample measured at B = 16/32/48; CPU budget in §2.1 confirmed or revised; golden vectors generated |
| M2 | Own driver `lib/i2s32` plus float passthrough chain, serial stats, `USB_MIDI_SERIAL`, software fault path (silence on overrun streak). Host renderer `tools/render`. | Round-trip latency measured ≈ 2·B + converter delays; zero overruns; Audio Library removed |
| M3 | IR loader (UPOLS) plus SD loading and crossfade | Matches FIR reference; ~4 % CPU; glitch-free IR swap |
| M4 (engine ✔) | NAM A2-Lite in the chain: model loading from SD, prewarm, calibration. The engine itself is done (32.4 % at B = 32, matches the reference). | Guitar through a real model on the board, inside the 80 % rule |
| M5 | Gate, comp, EQ, chorus, delay (PSRAM), reverb, tuner; presets plus MIDI | All host tests pass; worst-case CPU ≤ 80 % |
| M6 | Switchable-gain preamp (hardware) plus physical controls | Calibrated input; footswitch and encoder UI |

**Progress (Nov 2026):** the converter boards were not wired yet, so the driver part of M2 was built first: `lib/i2s32`, `dsp/Convert.h` with host tests, the loopback self-test, and a passthrough `src/main.cpp` on `USB_MIDI_SERIAL`. The Audio Library and `lib/audio_fx` are gone. Passthrough was then confirmed on the wired boards. Still open in M2: the latency measurement with converters, the fault path, and `tools/render`. M1 is next.

The order puts the largest risk first: M1 needs only the bare Teensy, so it can run in parallel with M0. A unity-gain Hi-Z input buffer (§1 item 1) should be built alongside M2, since everything judged by ear after that depends on it.

---

## 13. Risks and open items

| Risk / question | Impact | Mitigation |
| --- | --- | --- |
| `.nam` parse time from SD is unmeasured (43 ms from flash) | Slow preset changes | Measure at M4; add the weights cache file if needed. |
| Datasheet figures quoted from memory (ADC/DAC group delay, full-scale levels, PCM1808 LSB padding) | Latency and calibration numbers off | Measure in M0/M2 and update this doc. |
| DAC cannot be muted by firmware (XSMT rework deferred) | Buzz on a firmware crash; possible pop on boot or flash | Volume down before flashing or resetting. Revisit the rework before the unit is used with an amp at volume. |
| No Hi-Z input yet | Tone cannot be judged with a guitar plugged straight in | Build the unity-gain buffer alongside M2. Use a buffered pedal or DI until then, and DI recordings through `tools/render`. |
| Audio interrupt priority above USB | Possible USB jitter | Keep the interrupt under 80 % of the block period. Test the serial shell under load. |
| PSRAM access from the interrupt | Cache-miss stalls | Block-wise memcpy only; delay is the only PSRAM user. |
| USB 5 V feeds the ADC analog supply | Higher noise floor | Clean 5 V or LDO from an external supply later (already in the hardware doc). |
| ADC left channel use (guitar is on the right) | – | Decide: second input, re-amp, or expression pedal. |

---

## Sources

- [TONE3000: Introducing NAM Architecture 2 (A2)](https://www.tone3000.com/blog/introducing-neural-amp-modeler-nam-architecture-2-a2)
- [TONE3000: NAM A2, the complete guide](https://www.tone3000.com/guides/nam-a2-the-complete-guide) (architecture, A2-Lite, `.namb`, Cortex-M7 claim)
- [tone-3000/nam-pedal](https://github.com/tone-3000/nam-pedal): Daisy Seed reference (48 kHz, 48-sample blocks, NeuralAmpModelerCore, `nam2namb`)
- [ValdemarOrn/Teensy4i2s](https://github.com/ValdemarOrn/Teensy4i2s): 32-bit I2S for Teensy 4 (`i2sAudioCallback`, `InitI2s`, `AudioConfig.h`)
- Hardware doc in this repo, and its sources
