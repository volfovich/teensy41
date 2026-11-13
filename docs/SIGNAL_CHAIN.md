# Signal chain

The audio path as it is in the code. Source: `src/main.cpp` (the block callback), `src/AudioChain.h` (`process()`), `src/Params.cpp` (what each setting does). Last checked against the code: 13 Nov 2026.

**The path is kept as ported from teensy-ir-loader: the author wants the sound unchanged.** These were added, all at the author's request and none changing the ported sound: the master volume at the end, the Mod block, the Limiter block, and two more reverb modes (plate, spring) and two more delay types (tape, analog) beside the ported ones, which run unchanged. Mod and Limiter pass their input bit for bit while they are off. A change to any gain, level or order here needs the author's say first.

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
MOD        chorus, flanger, ce2 or phase90, stereo
  ▼
DELAY      stereo
  ▼
REVERB     stereo
  ▼
+ DRUMS and BASS                        added here, never through the guitar's effects
  ▼
× master volume                         the last gain
  ▼
LIMITER    brickwall, optional          on everything; nothing above its ceiling
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

### Mod (`mod`, in presets)
`dsp/fx/Modulation.h`. New in this project (13 Nov 2026), not ported. A chorus, a flanger, a chorus in the style of the Boss CE-2 or a phaser in the style of the MXR Phase 90 (`mod type`), stereo in and out; with the doubler off its input is mono and the two sides get the sweep at different points of its cycle, which makes it wide.

| Setting | Does |
| --- | --- |
| `mod type` | `chorus`: the delay moves around 12 ms, by up to ±5 ms. `flanger`: it moves between 0.3 ms and up to 7 ms. `ce2` and `phase90`: see below. |
| `mod rate` | The sweep's speed, 0.05 to 10 Hz. Above 1 Hz (chorus) or 2 Hz (flanger) the sweep gets narrower as the rate goes up, so the pitch wobble does not grow. |
| `mod depth` | How far the delay moves (the phase90: how far its notches move) |
| `mod fb` | Feedback, up to 90 %: the flanger's resonance; on the choruses a thicker, ringing sound. The phase90: up to 0.75 around three of its stages, a hump between the notches. |
| `mod mix` | Dry against wet, equal power. 50 % is equal parts (the flanger's deepest notches); 100 % on the chorus is vibrato. |
| `mod width` | How far apart the two sides' sweeps are: 0 keeps mono, 100 % is half a cycle |

**`ce2`, in the style of the Boss CE-2** (added 13 Nov 2026). Built from the pedal's published circuit, not from a recording of one:
- A triangle LFO moves the delay between 4.4 and 6.8 ms at full depth. That is the pedal's 1024-stage delay chip at the clock rates owners measured (75 to 115 kHz at full depth). The sweep does not get narrower at high rates, as the pedal's does not.
- The wet signal passes a 6.6 kHz low-pass (third order), for the filters around the pedal's delay chip. That is what makes it darker and rounder than `chorus`.
- **Two dials, as on the pedal: `mod rate` and `mod depth`.** The firmware holds the rest as the pedal has it (`Params.cpp`): half input and half effect, no feedback, mono. `mod fb`, `mod mix` and `mod width` keep their stored values for the other types but are not used and not shown. The pedal's own rate goes up to about 3.5 Hz; the dial goes on to 10 Hz.
- Left out: the chip's noise and distortion, and the treble boost before and cut after it (they cancel).

**`phase90`, in the style of the MXR Phase 90** (added 13 Nov 2026). Also from the published circuit:
- Four first-order all-pass stages per side. A triangle LFO moves their corner between 141 Hz (the pedal's 24 kΩ and 47 nF with the JFET off) and 1.6 kHz at full depth, linearly in frequency. Mixed with the input that gives two notches, at 0.41 and 2.41 times the corner: 58 to 660 Hz and 340 Hz to 3.9 kHz.
- **One dial, as on the pedal: `mod rate`.** The firmware holds the rest (`Params.cpp`): the full sweep, half input and half effect, mono, no feedback, which is the early ("script" logo) pedal. The other four settings are not used and not shown.
- The effect itself (`Modulation.h`) can do more, should it be wanted later: a narrower sweep, stereo, and the later ("block" logo) pedal's feedback from the last stage into the second, which adds a midrange hump (about 0.42).
- **An estimate:** the 1.6 kHz top of the sweep. No published figure was found; the bottom follows from the parts. To be tuned by ear.
- The delay lines are not used; the type costs the same CPU as the others.

- The dry signal is not delayed, so the block adds no latency.
- **Level:** compensated automatically, so the block is as loud on as off. Without feedback, dry and wet at equal power already come out as loud as the input (the comb's peaks and notches even out over the sweep). Feedback makes the wet signal louder, and the block turns itself down by the matching amount. Measured in the host test with guitar-band noise (80 Hz to 2.5 kHz) over all four types and all tested settings: within 0.32 dB of the input, where it would be up to +6.2 dB without the compensation. The phase90's wet signal is not delayed, so it does not even out against the dry one; its loudness is worked out from its response instead (the average over that band and over the sweep).
- The compensation matches **loudness, not peaks**. A steady sine that sits on a comb peak still reads higher on the meters: on the unit (13 Nov 2026) the chorus at its defaults +2 to +3 dB, the flanger with feedback 70 % and full depth +5 to +7 dB. Real playing sits between the two; the Mod tile's level bar shows it.
- Switching on fills the delay lines for 20 ms, then fades in over 20 ms; a change of type fades out and back in. No clicks.

**Settings that do not apply are hidden.** The app shows a setting only with the types or modes it belongs to (`only` in `protocol/blocks.json`), and the shell's `s` leaves it out: the Mod dials above; the gate's release with the `nam` gate type (that gate has its own fixed timing); the reverb's and the delay's settings as listed with their modes and types below. A hidden setting keeps its stored value and can still be set from the shell; it does nothing until its type or mode is chosen.

### Delay (`dl`, in presets)
`dsp/fx/DelayBank.h` holds three delays; `dtype` picks one. All take `dmix`, `dtime` (20 to 750 ms), `dfb` repeats, `dtone` and `dmod`; the mono signal (the left side) goes in, as ported.

| `dtype` | What it is | Its other settings |
| --- | --- | --- |
| `digital` (the default) | `dsp/fx/StudioDelay.h`, **ported and unchanged**: an 80s studio dual delay, the right side at 0.75 of the time. Fixed voicing in `include/config.h` (`DELAY80_*`). | `dduck` (the repeats duck while playing), `ddrive`, `dera` |
| `tape` | `dsp/fx/TapeDelay.h`: a tape echo. The tape is written through a soft clip, played back through a 110 Hz low cut and a low-pass (`dtone`, 1.2 to 7 kHz), so each repeat loses lows and highs. `dmod` is wow (0.6 Hz, up to 1 % in pitch) and flutter (7.3 Hz, up to 0.3 %). The right side reads the same tape a little apart in wow and flutter. | `ddrive` (how hard the tape is hit) |
| `analog` | `dsp/fx/AnalogDelay.h`: a bucket-brigade pedal, mono. A low-pass before the line and a third-order one after it (`dtone`, 1.5 to 4.5 kHz, up to 30 % lower for times above 300 ms, as the chip's clock slows), a 2:1 compressor before and a 1:2 expander after the line with a 12 ms lag (the repeats swell in a little), the chips' soft clip. `dmod` is a chorus on the repeats (0.5 Hz, up to 2.5 ms). | none |

- **Time changes:** the digital delay crossfades to the new time (20 ms, no pitch change). Tape and analog glide: the repeats bend in pitch, by an octave at most, and settle within about a second and a half for a large change. With nothing in the line the time applies at once.
- **Level:** tape and analog use the digital delay's gains (repeat level x 1.2, the input lowered as the repeats go up). They do not duck, so with a steady tone and many repeats they read higher on the meters than the digital delay at the same settings (measured: 5 to 6 dB at mix 50 %, repeats 50 %, duck 40 %).
- **Sources:** tape after the usual parts of the published tape-echo models (Arnardottir, Abel and Smith, AES 2008); analog after Raffel and Smith, "Practical Modeling of Bucket-Brigade Device Circuits" (DAFx-10). Written for this project; no code taken from elsewhere. The voicing numbers (filter corners, wow and flutter depth, compander timing) are first choices, to be tuned by ear.

### Reverb (`rv`, in presets)
`dsp/fx/ReverbBank.h` holds three reverbs; `rmode` picks one of four modes. All take `mix`, `size` and `tone`; the two sides are summed to mono going in.

| `rmode` | What it is | Its other settings |
| --- | --- | --- |
| `hall` (the default), `gated` | `dsp/fx/Reverb80.h`, **ported and unchanged**: an 8-line FDN hall, and a gated burst. | `rpre` pre-delay, `rera`; `rmod` (hall), `rgate` (gated: the gate time) |
| `plate` | `dsp/fx/PlateReverb.h`: Dattorro's plate (J. Audio Eng. Soc., 1997): four input all-passes, then a tank of two halves feeding each other (a modulated all-pass, a delay, a damping low-pass, an all-pass, a delay each), seven output taps per side. `size` is the decay, 0.5 to 8 s; `tone` the damping, 2.5 to 12 kHz. Dense and bright from the first moment. | `rpre`, `rmod` (how far the tank's two all-passes move) |
| `spring` | `dsp/fx/SpringReverb.h`: a two-spring tank after Valimaki, Parker and Abel (J. Audio Eng. Soc., 2010), cut down to fit the CPU. Each spring is 26 stretched all-passes (which smear every echo into a rising chirp, the "drip") and a delay (33 and 42 ms); each returns into the other. `size` is the decay, 0.8 to 3.2 s; `tone` the spring's top, 2 to 3.8 kHz. The sound is taken at the springs' far ends, so it starts 33 ms after the input. | none |

- **The plate is a fifth smaller than the paper's.** Its lengths are the paper's (given for 29.8 kHz) times 1.3, not the 1.61 that 48 kHz asks for, so the tank and the 150 ms pre-delay fit the memory the reverbs share.
- **The spring has far fewer all-passes than the paper** (26 per spring against 100) and only its low-frequency chirp loop: the chirps are shorter than a real tank's. That is what the CPU allows (see below).
- **Level:** plate and spring at 100 % wet are as loud as the input on pink noise, within 0.6 dB (host tests), as the hall is. On steady single notes the spring varies by about 5 dB from note to note (measured on the unit at five notes), as any reverb built on a loop does.
- **Sources:** written for this project from the two papers; no code taken from elsewhere. HexeFX's library (MIT) was read for comparison.

**The effects of one block share its memory**, so only one of them holds sound at a time. A change between hall and gated is Reverb80's own (as ported). Any other change of `rmode`, and every change of `dtype`, does the same thing: the tail fades out over 20 ms, the memory is cleared in silence (up to 12 ms), and the other effect starts; with the block off and rung out the change is immediate. The settings go to every effect that has them, so they are in place after a change. With `hall`, `gated` and `digital` the samples are exactly those of the ported effects (host test `test_fx_banks`).

An effect that is off and has rung out is idle: it costs no CPU and passes its input through bit for bit. With doubler, mod, delay, reverb and drums all idle, the mono signal goes to both outputs.

### Drums and bass (`drum`, not in presets)
`dsp/drums/`. Sampled kit, styles, songs (built in and MIDI files from `/DRUMS`), a plucked bass, count-in, speed trainer. Added to the outputs after the reverb.

### Master volume (`master`, global)
The last gain, on everything: guitar, effects, drums and bass. From silent (its lowest step) to 0 dB; at 0 dB the samples are unchanged. Not in presets. Added on 13 Nov 2026 at the author's request; it is the app's slider beside the meters.

### Limiter (`lim`, global)
`dsp/fx/Limiter.h`. New in this project (13 Nov 2026). The last stage, after the master volume, on guitar, effects, drums and bass together: a brickwall peak limiter, so that nothing goes to the DAC above its ceiling. Not in presets; off by default.

| Setting | Does |
| --- | --- |
| `lim ceil` | The ceiling, −12 to −0.5 dB (default −1 dB). No sample comes out above it. |
| `lim rel` | Release, 10 to 1000 ms (default 100): how fast the gain comes back after a peak |
| `lim look` | Lookahead: the gain glides down over the millisecond before a peak instead of dropping at it. **Adds 1 ms of latency while the limiter is on** (the one exception to the no-latency rule, chosen by this switch). |

How it stays transparent:
- **Under the ceiling it does nothing.** Without lookahead the samples pass bit for bit; with lookahead they pass bit for bit, 1 ms later.
- **Both sides get the same gain**, so the stereo image does not move.
- **A hold of 10 ms**, renewed by every further peak of about the same height, keeps the gain still between the half-waves of a note. A steady tone 6 dB over the ceiling comes out as the same tone, quieter: distortion below −60 dB in the host test, at 82 Hz and at 1 kHz, with and without lookahead.
- **Without lookahead** the gain drops at the very sample that would go over: exact, but sudden. Clean on occasional peaks; when driven hard, the lookahead is cleaner.
- Switching the lookahead changes the delay, so the output dips to silence and back within 5 ms instead of jumping.

Limits to know:
- It limits sample peaks, not the peaks between samples (that needs oversampling, which the CPU budget does not allow). That is why the default ceiling is −1 dB and not 0.
- It is protection, not a loudness tool: there is no make-up gain.
- Measured on the unit (13 Nov 2026) with a tone 5 dB over the ceiling: the output read exactly the ceiling (−12.0 and −9.0 dBFS), 0 clipped samples, in both modes.

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
| Master volume | `master` | global | the last gain |
| Limiter | `lim ceil` | global | after the master: only ever turns down, and only above its ceiling |

- A high-gain model with a resonant IR can peak near or above 0 dBFS after the cab at `amp out` 0 dB. Between blocks that is harmless (float); the output gain's default of −6 dB is the headroom before the DAC.
- **Block meters:** the chain keeps the peak at the output of amp, gate, cab (after the output gain and preset level), doubler, mod, delay, reverb and limiter (`takeStagePeak`); the limiter also reports how far it is turning down. The app shows them on the blocks' tiles: green, yellow from −6 dBFS, red from −1 dBFS. An idle effect reports the level before it.
- **Checking levels without a guitar:** `sig 220 -20` in the shell replaces the guitar with a sine (`sig 0` ends it). Turn the master down first: a high-gain model makes any input loud.

## Tuner
The input, low-passed at 4 kHz and decimated to 24 kHz, goes into a ring buffer; `loop()` runs YIN pitch detection on it (`dsp/fx/PitchDetect.h`, `src/Tuner.cpp`). `tuner test <Hz>` feeds the tuner a sine instead.

## CPU, memory, latency

Measured on the board, 32-sample blocks (budget: worst case at or below 80 % of a block).

| What | CPU | Date |
| --- | --- | --- |
| Amp (network, DC blocker, tone) | 32.9 % | 13 Nov 2026 |
| Doubler | 1.1 % | 13 Nov 2026 |
| Mod: chorus, flanger, phase90 / ce2 | 2.4 % / 2.7 % | 13 Nov 2026 |
| Limiter: on and idle / limiting / limiting with lookahead | 0.6 % / 1.1 % / 1.9 % | 13 Nov 2026 |
| Delay: digital / tape / analog | 4.8 % / 3.5 % / 3.0 % | 13 Nov 2026 |
| Reverb: hall / gated / plate / spring | 8.1 % / 6.7 % / 4.8 % / 8.0 % | 13 Nov 2026 |
| Drums and bass, rock at 100 BPM | 4.0 % avg | 13 Nov 2026 |
| Driver, conversion, gate, idle convolver | about 4.5 % | 13 Nov 2026 |
| A typical preset: amp + 4 k IR + reverb | 56.3 % avg, 57.1 % max | 13 Nov 2026 |
| The same + mod | 58.7 % avg, 59.5 % max | 13 Nov 2026 |
| Everything on: + doubler, mod, delay, drums and bass (rock, 100 BPM) | 69 % avg, **80.4 % max** | 13 Nov 2026 |
| Everything on, in one later session of 14 s each (limiter on): hall + digital / plate + tape / spring + analog / gated + digital | 75.6 % / 69.7 % / 73.0 % / 73.3 % max | 13 Nov 2026 |

| Stage | Memory |
| --- | --- |
| Amp history | 87 KB, DTCM (must stay there: OCRAM costs about 4 % CPU) |
| Cab spectra and history | DTCM |
| Doubler | 8 KB, OCRAM |
| Mod | 16 KB, OCRAM |
| Limiter | 1.3 KB, DTCM |
| Delay | 144 KB DTCM + 144 KB OCRAM (shared by the three types; tape and analog use the first line) |
| Reverb | 148 KB, OCRAM (shared by the three reverbs: the plate uses 146 KB of it, the spring 15 KB) |
| Drum kits | flash |

The everything-on figure above was measured before the limiter existed; with the limiter working as well, expect 1 to 2 % more (not measured together). With everything on, the worst block now touches the 80 % budget (by subtraction it was about 78 % before the Mod block; that was not measured); no overruns were seen. The drums' busiest moments set that maximum.

Latency, analog in to analog out: about **2.2 ms**, or 3.2 ms while the limiter runs with lookahead (ADC filter 0.36, two blocks 1.33, FIFOs 0.04, DAC filter 0.42; datasheet figures, not measured). Nothing else in the chain adds latency: no lookahead, no oversampling, no FFT larger than the block. Details and the 16-sample experiment: [design/HLD.md](design/HLD.md) §2.
