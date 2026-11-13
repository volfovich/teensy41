# Signal chain

The audio path as it is in the code. Source: `src/main.cpp` (the block callback), `src/AudioChain.h` (`process()`), `src/Params.cpp` (what each setting does). Last checked against the code: 13 Nov 2026.

**The path is kept as ported from teensy-ir-loader: the author wants the sound unchanged.** The only addition is the master volume at the end. A change to any gain, level or order here needs the author's say first.

## Overview

One mono input (the guitar), two outputs. 48 kHz, blocks of 32 samples, 32-bit float inside.

```
ADC right channel (24-bit)
  │  to float
  ├──────────────► raw capture (debug, "cap")
  ▼
30 Hz high-pass                         main.cpp
  │  (test sine replaces the guitar here when "sig" is on)
  ▼
──── AudioChain::process() ──────────────────────────────────────────────
  ├──────────────► input meter
  ├──────────────► tuner tap (low-pass, 24 kHz, to loop())
  ▼
AMP        NAM A2-Lite model: input gain → network → DC blocker → tone → output gain
  ▼
GATE       listens to the input, gates the amp's output
  ▼
CAB        IR convolution
  ▼
× (output gain + preset level)          one gain; skipped when the cab is bypassed
  ▼
tuner mute                              mono up to here
  ▼
DOUBLER    mono in, stereo out
  ▼
DELAY      stereo
  ▼
REVERB     stereo
  ▼
+ DRUMS and BASS                        added here, never through the guitar's effects
  ▼
× master volume                         the last gain
  ▼
clip to full scale (counted)
──────────────────────────────────────────────────────────────────────────
  ▼
to 24-bit, DAC left and right
```

## The stages

Setting keys are the serial commands' (full table in [CONTROL.md](CONTROL.md)). "In presets" says whether a preset stores the setting.

### Input
- The guitar is on the ADC's **right** channel (`GUITAR_ON_RIGHT_CHANNEL`). The left one is unused.
- A 2nd-order **30 Hz high-pass** (`INPUT_HIGHPASS_HZ`) comes first. The SD card puts a thump below 20 Hz on the ADC about every 1.4 s; without the filter it opens the gate into a high-gain model.
- There is no input gain control: the level at the ADC is what the guitar and its buffer give. 0 dBFS at the ADC is taken as +2.7 dBu (`ADC_FULL_SCALE_DBU`, a datasheet value, not measured).

### Amp (`amp`, in presets)
`dsp/nam/AmpStage.h`, `NamA2.h`; loading in `src/AmpLibrary.cpp`.

| Step | What sets it |
| --- | --- |
| Input gain | `amp in` (±24 dB) plus, with `amp cal` on, the calibration: ADC full scale (+2.7 dBu) + `INPUT_PAD_DB` (0) − the model's `input_level_dbu` (or +12 dBu when the file has none) |
| Network | The A2-Lite model: 1,871 weights, causal, receptive field 132 ms |
| DC blocker | 5 Hz |
| Tone | `amp bass`, `amp mid`, `amp treble`, `amp presence`; all at 0 leaves the signal alone |
| Output gain | Levelling to −12 dB loudness (`AMP_LEVEL_DB` − the file's `loudness`) plus `amp out` (±24 dB) |

- Off: the input passes as it is and the network costs no CPU.
- Loading a model with the amp on: 20 ms fade out, 132 ms warm-up in silence, fade in (a gap of about 170 ms).

### Noise gate (`gate`, in presets)
`dsp/fx/NoiseGate.h`. It measures the **input** (before the amp) and gates the amp's output, so the threshold does not move with the amp's gain. `gate thr` (−90 to −20 dB, default −64), `gate rel` (10 to 500 ms), `gate type`: `hard` (default) or `nam` (the NAM plugin's expander).

### Cab (`b` = bypass, in presets)
`dsp/conv/PartitionedConvolver.h`, loading in `dsp/conv/IRLoader.cpp` and `src/IRLibrary.cpp`.
- Convolution with the loaded IR: up to 4,096 samples (85 ms), partition = block size, so no added latency.
- On load the file is resampled to 48 kHz, trimmed (leading silence, inaudible tail), cut at 4,096 samples and loudness-normalised (`IR_NORMALIZE`). A new IR is crossfaded in over 25 ms.
- With no IR loaded the signal passes through.

### Output gain and preset level
One gain, right after the cab: `g` (output gain, global, −60 to +24 dB, default −6 dB) **plus** `pvol` (the preset's level, −24 to +12 dB, in presets). It ramps over one block.

**With the cab bypassed, this gain is not applied either.** The code blends between "IR output × gain" and "IR input" (`y = w·g·ir + (1−w)·input`), so `b on` passes the gate's output as it is: no IR, and none of the default −6 dB either. That is how the ported chain works.

The drums and the bass are not affected by this gain.

### Tuner mute
While the tuner is open the guitar is faded out here (20 ms), before the delay and the reverb, so their tails ring out.

### Doubler (`dbl`, in presets)
`dsp/fx/Doubler.h`. A second "take" 1 to 35 ms later (`dbl time`), panned opposite (`dbl width`). Makes the signal stereo.

### Delay (`dl`, in presets)
`dsp/fx/StudioDelay.h`. An 80s studio dual delay: `dtime` 20 to 750 ms (the right side at 0.75 of it), `dfb` repeats, `dmix`, `dtone`, `dmod`, `dduck` (the repeats duck while playing), `ddrive`, `dera`. Fixed voicing in `include/config.h` (`DELAY80_*`).

### Reverb (`rv`, in presets)
`dsp/fx/Reverb80.h`. `rmode` hall (8-line FDN) or gated, `mix`, `size`, `tone`, `rpre` pre-delay, `rmod`, `rera`, `rgate` (gate time of the gated mode).

An effect that is off and has rung out is idle: it costs no CPU and passes its input through bit for bit. With doubler, delay, reverb and drums all idle, the mono signal goes to both outputs.

### Drums and bass (`drum`, not in presets)
`dsp/drums/`. Sampled kit, styles, songs (built in and MIDI files from `/DRUMS`), a plucked bass, count-in, speed trainer. Added to the outputs after the reverb.

### Master volume (`master`, global)
The last gain, on everything: guitar, effects, drums and bass. From silent (its lowest step) to 0 dB; at 0 dB the samples are unchanged. Not in presets. Added on 13 Nov 2026 at the author's request; it is the app's slider beside the meters.

### Output
Samples beyond full scale are clipped and counted (the app's CLIP label, `s` in the shell), then converted to 24 bits.

## Levels: where each gain sits

| Gain | Key | Scope | Position |
| --- | --- | --- | --- |
| Amp input (drive) | `amp in`, `amp cal` | preset | before the network |
| Amp output | `amp out` (+ levelling) | preset | after the network |
| Output gain | `g` | global | after the cab |
| Preset level | `pvol` | preset | after the cab, same gain as `g` |
| Drum, bass volume | `drum vol`, `bass vol` | global | inside the drum machine |
| Master volume | `master` | global | last, before the DAC |

- A high-gain model with a resonant IR can peak near or above 0 dBFS after the cab at `amp out` 0 dB. Between blocks that is harmless (float); the output gain's default of −6 dB is the headroom before the DAC.
- **Block meters:** the chain keeps the peak at the output of amp, gate, cab (after the output gain and preset level), doubler, delay and reverb (`takeStagePeak`). The app shows them on the blocks' tiles: green, yellow from −6 dBFS, red from −1 dBFS. An idle effect reports the level before it.
- **Checking levels without a guitar:** `sig 220 -20` in the shell replaces the guitar with a sine (`sig 0` ends it). Turn the master down first: a high-gain model makes any input loud.

## Tuner
The input, low-passed at 4 kHz and decimated to 24 kHz, goes into a ring buffer; `loop()` runs YIN pitch detection on it (`dsp/fx/PitchDetect.h`, `src/Tuner.cpp`). `tuner test <Hz>` feeds the tuner a sine instead.

## CPU, memory, latency

Measured on the board, 32-sample blocks (budget: worst case at or below 80 % of a block).

| What | CPU | Date |
| --- | --- | --- |
| Amp (network, DC blocker, tone) | 32.9 % | 13 Nov 2026 |
| Doubler | 1.1 % | 13 Nov 2026 |
| Delay | 4.7 % | 13 Nov 2026 |
| Reverb, hall | 8.4 % | 13 Nov 2026 |
| Drums and bass, rock at 100 BPM | 4.0 % avg | 13 Nov 2026 |
| Driver, conversion, gate, idle convolver | about 4.5 % | 13 Nov 2026 |
| A typical preset: amp + 4 k IR + reverb | 56.8 % avg, 57.6 % max | 13 Nov 2026 |

| Stage | Memory |
| --- | --- |
| Amp history | 87 KB, DTCM (must stay there: OCRAM costs about 4 % CPU) |
| Cab spectra and history | DTCM |
| Doubler | 8 KB, OCRAM |
| Delay | 144 KB DTCM + 144 KB OCRAM |
| Reverb | 148 KB, OCRAM |
| Drum kits | flash |

Latency, analog in to analog out: about **2.2 ms** (ADC filter 0.36, two blocks 1.33, FIFOs 0.04, DAC filter 0.42; datasheet figures, not measured). Nothing in the chain adds latency: no lookahead, no oversampling, no FFT larger than the block. Details and the 16-sample experiment: [design/HLD.md](design/HLD.md) §2.
