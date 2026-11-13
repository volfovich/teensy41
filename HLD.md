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
| NAM A2-Lite | ≤ 50 % | TONE3000 claims about 50 % on a 600 MHz Cortex-M7 **(verify at M1; the budget's main risk)** |
| IR cab, 2048 taps | ~4 % (B=32) / ~7 % (B=16) | Partitioned FFT convolution (§6.3) |
| Gate, comp, EQ, mod, delay, reverb | ≤ 20 % | |
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
| [NAM A2](https://www.tone3000.com/guides/nam-a2-the-complete-guide) (MIT) | **Use the model format and architecture** | A2-Lite variant (3 channels). Converted on the host from `.nam` to `.namb` with `nam2namb` from `nam-binary-loader`. |
| NeuralAmpModelerCore + `nam-binary-loader` (as used in [tone-3000/nam-pedal](https://github.com/tone-3000/nam-pedal), MIT) | **Use as reference, then tune or replace** | Phase A runs it on the Teensy with the A2 fast path (`NAM_ENABLE_A2_FAST`) to benchmark, and on the host to produce golden vectors. Phase B is an optimised engine: TONE3000's C engine if it is published, otherwise our own (§6.2). |
| SdFat (bundled with Teensyduino, built-in SDIO slot) | **Use** | Reads `.namb` models, IR WAVs and presets from `loop()` only. |
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

- **Status: implemented in `lib/i2s32`, builds, not yet run on hardware.** `hw_tests/i2s_loopback.cpp` verifies it on the bare board with one jumper (pin 7 → pin 8); see §10.
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
  - The guitar input is mono (ADC left channel). The ADC right channel is reserved: second input, re-amp, or expression input. **(Decide.)**
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
ADC L ─► [1] input cal + DC block ─► [2] noise gate ─► [3] comp ─► [4] boost/drive
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
- The size of an A2 `.namb` file, which packs Full and Lite, is unknown **(verify)**. Our loader keeps only the Lite weights.

**Phase A: benchmark and reference (M1)**
- First step: inspect the current NeuralAmpModelerCore, `nam-binary-loader` and `nam-pedal` repos to see what is published for A2 (fast path, C engine).
- Build the core + loader for the Teensy with the fast path enabled, float samples, and flush-to-zero.
- Benchmark without audio I/O, with A2-Lite models only: run the engine over a buffer in `loop()` and count cycles at B = 16, 32 and 48. This is the **go/no-go** for the CPU budget, and it decides B and the effects budget.
- Risks: Eigen with dynamic allocation in `prepare` is acceptable. Teensyduino builds with `-fno-exceptions`, and if the core library throws it must be patched or guarded. Both are to be checked.
- Same code on the host (native env): generates **golden vectors** (input sweep, noise, DI guitar clip → reference output) for tests.

**Phase B: optimised engine (M4)**, in `lib/dsp/nam/`:
- If TONE3000's C engine is published under MIT, adopt it and tune it for the Teensy instead of writing from scratch. Otherwise write our own as below.
- Fixed-topology A2 implementation: static buffers, no Eigen.
- Inner loops written for the M7: 3-channel dot products unrolled, kernel-major loops over the block, ring buffers sized to powers of two (masking instead of modulo), weights in DTCM, code `FASTRUN` (ITCM).
- Leaky ReLU is branch-free (`fmaxf(x, 0.01f*x)`). That is cheap (no tanh).
- Accepted when outputs match the golden vectors within about −100 dB RMS error, at no more CPU than Phase A.
- Only Phase B ships if it is faster. Otherwise Phase A stays.

**Model loading and switching:**
- Host: `tools/nam_convert` (wraps `nam2namb`) produces `.namb`, which is copied to SD under `/models/`.
- `loop()`:
  1. Read `.namb` into a staging buffer.
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
| SD card | – | `/models/*.namb`, `/irs/*.wav`, `/presets/*.json` |

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
- New `env:nam_bench` (M1): `hw_tests/nam_bench.cpp`, the NAM engine run over a buffer with cycle counts, no audio I/O.
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
    nam/           A2 engine (Phase B), .namb parser
    conv/          UPOLS convolver, FFT abstraction (CMSIS on target, simple FFT on host)
    fx/            gate, comp, eq, chorus, delay, reverb, tuner
src/
  main.cpp         setup, interrupt hook → Chain, loop: shell / MIDI / loaders
  control/         serial shell, MIDI map, presets (ArduinoJson), asset loaders (SdFat)
hw_tests/
  psram_*.cpp      (existing)  i2s_bringup.cpp  nam_bench.cpp  latency_test.cpp
tools/
  nam_convert/     .nam → .namb (wraps nam2namb)
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
| M1 | **NAM A2-Lite benchmark (go/no-go).** A2-Lite models only. Reference core with the A2 fast path on the Teensy, no audio I/O. Host build produces golden vectors. Choose B (16 or 32). | A2-Lite cycles per sample measured at B = 16/32/48; CPU budget in §2.1 confirmed or revised; golden vectors generated |
| M2 | Own driver `lib/i2s32` plus float passthrough chain, serial stats, `USB_MIDI_SERIAL`, software fault path (silence on overrun streak). Host renderer `tools/render`. | Round-trip latency measured ≈ 2·B + converter delays; zero overruns; Audio Library removed |
| M3 | IR loader (UPOLS) plus SD loading and crossfade | Matches FIR reference; ~4 % CPU; glitch-free IR swap |
| M4 | NAM A2-Lite in the chain (model loading, prewarm, calibration), then the optimised engine | Real time with IR inside the 80 % rule; matches golden vectors; CPU ≤ Phase A |
| M5 | Gate, comp, EQ, chorus, delay (PSRAM), reverb, tuner; presets plus MIDI | All host tests pass; worst-case CPU ≤ 80 % |
| M6 | Switchable-gain preamp (hardware) plus physical controls | Calibrated input; footswitch and encoder UI |

**Progress (Nov 2026):** the converter boards were not wired yet, so the driver part of M2 was built first: `lib/i2s32`, `dsp/Convert.h` with host tests, the loopback self-test, and a passthrough `src/main.cpp` on `USB_MIDI_SERIAL`. The Audio Library and `lib/audio_fx` are gone. Still open in M2: running the loopback test on the board, the latency measurement with converters, the fault path, and `tools/render`. M1 is next.

The order puts the largest risk first: M1 needs only the bare Teensy, so it can run in parallel with M0. A unity-gain Hi-Z input buffer (§1 item 1) should be built alongside M2, since everything judged by ear after that depends on it.

---

## 13. Risks and open items

| Risk / question | Impact | Mitigation |
| --- | --- | --- |
| A2-Lite CPU is a vendor claim (about 50 % at 600 MHz), and `nam-pedal` publishes no A2 numbers | Could leave too little room for effects, or not run in real time at all | Benchmark at M1, before any driver work. Fall back to lighter effects, or B = 32 instead of 16. The Phase B engine is a further lever. |
| A2 support status in NeuralAmpModelerCore / `nam-binary-loader` (fast path, exceptions, Eigen on Teensyduino, A2 `.namb` size), and whether TONE3000's C engine is published | Delays M1; changes the size of Phase B | Check the repos at M1 start. Phase B removes the dependency. |
| Datasheet figures quoted from memory (ADC/DAC group delay, full-scale levels, PCM1808 LSB padding) | Latency and calibration numbers off | Measure in M0/M2 and update this doc. |
| DAC cannot be muted by firmware (XSMT rework deferred) | Buzz on a firmware crash; possible pop on boot or flash | Volume down before flashing or resetting. Revisit the rework before the unit is used with an amp at volume. |
| No Hi-Z input yet | Tone cannot be judged with a guitar plugged straight in | Build the unity-gain buffer alongside M2. Use a buffered pedal or DI until then, and DI recordings through `tools/render`. |
| Audio interrupt priority above USB | Possible USB jitter | Keep the interrupt under 80 % of the block period. Test the serial shell under load. |
| PSRAM access from the interrupt | Cache-miss stalls | Block-wise memcpy only; delay is the only PSRAM user. |
| USB 5 V feeds the ADC analog supply | Higher noise floor | Clean 5 V or LDO from an external supply later (already in the hardware doc). |
| ADC right channel use | – | Decide: second input, re-amp, or expression pedal. |

---

## Sources

- [TONE3000: Introducing NAM Architecture 2 (A2)](https://www.tone3000.com/blog/introducing-neural-amp-modeler-nam-architecture-2-a2)
- [TONE3000: NAM A2, the complete guide](https://www.tone3000.com/guides/nam-a2-the-complete-guide) (architecture, A2-Lite, `.namb`, Cortex-M7 claim)
- [tone-3000/nam-pedal](https://github.com/tone-3000/nam-pedal): Daisy Seed reference (48 kHz, 48-sample blocks, NeuralAmpModelerCore, `nam2namb`)
- [ValdemarOrn/Teensy4i2s](https://github.com/ValdemarOrn/Teensy4i2s): 32-bit I2S for Teensy 4 (`i2sAudioCallback`, `InitI2s`, `AudioConfig.h`)
- Hardware doc in this repo, and its sources
