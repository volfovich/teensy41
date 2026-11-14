# Control reference

Every way to control the unit, taken from the code: `src/SerialUI.cpp`, `lib/params/src/params/ParamDefs.cpp`, `src/MidiControl.cpp`. Last checked: 13 Nov 2026. The app's binary protocol is in [`protocol/PROTOCOL.md`](../protocol/PROTOCOL.md).

## Serial shell

`pio device monitor` (or any terminal on the USB serial port). `?` or `help` prints the list with the current values; the unit's own list is always the up-to-date one.

### Settings
Every setting with a key in the table below is a command: the key, then the value.

```
amp on            dtime 380         amp in +3.5        gate type hard        master -10
```

- A switch alone flips (`amp`), a choice alone steps to the next (`rmode`).
- Decibels may have a half: `g -6.5`. A rate is typed in Hz: `mod rate 0.8`.
- `drum style`, `drum kit`, `drum song` take a name or a number; alone they list what there is.

### Other commands

| Command | Does |
| --- | --- |
| `l` | Lists the IR files |
| `<number>` | Loads that IR |
| `n` / `p` | Next / previous IR |
| `amp list` | Lists the amp models |
| `amp <n or name>` | Loads model n, or the first whose name contains the text |
| `amp info` | What the loaded model says about itself |
| `pre` | Lists the presets; `>` marks the recalled one, `*` edited |
| `pre <n or name>` | Recalls a preset |
| `pre save` | Saves the sound over the recalled preset |
| `pre new <name>` | Saves the sound as a new preset |
| `pre name <text>` | Renames the recalled preset |
| `pre del <n>` | Deletes preset n |
| `pre show` | What differs from the recalled preset |
| `tuner` | The tuner for 5 s (the guitar is muted meanwhile) |
| `midi` | Prints the USB MIDI map |
| `drum loop <a> <b>` / `drum loop off` | Loops bars a to b of the song |
| `drum train <step> <every> <target>` | Sets the speed trainer and switches it on (`drum train on` / `off` switches it) |
| `bass key <note>` | The bass's key as a note name (`E`, `F#`, `Bb`) |
| `m` | Live level meter on / off |
| `s` | Status: CPU per stage, driver overruns and FIFO errors, levels |
| `r` | Rescans the SD card |

### Test and debug aids

| Command | Does |
| --- | --- |
| `sig <Hz> <dBFS>` | A sine at a known level replaces the guitar (`sig 0` ends it). Turn the master down first. |
| `tuner test <Hz>` | The tuner hears a sine instead of the guitar (`tuner test 0` ends it) |
| `cap` | Sends 3 s of the raw input (before the high-pass) as floats over USB |
| `overload` | Wastes CPU for 400 blocks to test the overload protection (mutes for 1 s) |
| `tft` | The touch screen: whether a display was found, on or off, the screen shown, and its commands |
| `tft on`, `tft off`, `tft auto` | Draw and read touch always, never, or when a display is found at boot (the default); kept |
| `tft cal` | Calibrate the touch panel: touch two crosses |
| `tft drag <x0> <y0> <x1> <y1> [ms]` | Debug: a finger put down at the first point, slid to the second and lifted (600 ms, or as given, 300 ms at least) |
| `tft tap <x> <y> [ms]` | Debug: a touch at a screen point (320 x 240), as a finger would: 80 ms, or as long as given (a hold) |
| `tft touch` | Debug: touch readings for 10 s |
| `tft bench meters`, `full`, `off` | Debug: draw continuously to measure what drawing costs the audio (read it with `s`) |
| `mem` | Memory: RAM1 code and what is left of its 32 KB step, RAM1 data, deepest stack use since boot, RAM2 and its heap, PSRAM, flash |
| `amp selftest <n>` | Runs the loaded model on n floats sent over USB and returns the output |

## Settings table

65 values in the firmware's order (the number is the setting's id in the link protocol). The first 62 are saved in the unit; "In presets" marks the 40 a preset stores. New settings are added after the last saved one, so ids of saved settings never change; the three unsaved ones at the end move up. Defaults are what a blank unit starts with.

| # | Key | Name | Range | Default | In presets |
| --- | --- | --- | --- | --- | --- |
| 0 | `dl` | Delay | on, off | off | yes |
| 1 | `dmix` | Delay mix | 0 % to 100 % | 30 % | yes |
| 2 | `dtime` | Delay time | 20 ms to 750 ms | 380 ms | yes |
| 3 | `dfb` | Delay repeats | 0 % to 100 % | 35 % | yes |
| 4 | `dtone` | Delay tone | 0 % to 100 % | 60 % | yes |
| 5 | `dmod` | Delay modulation | 0 % to 100 % | 15 % | yes |
| 6 | `dduck` | Delay ducking | 0 % to 100 % | 40 % | yes |
| 7 | `ddrive` | Delay drive | 0 % to 100 % | 20 % | yes |
| 8 | `dera` | Delay era | 0 % to 100 % | 40 % | yes |
| 9 | `rv` | Reverb | on, off | off | yes |
| 10 | `mix` | Reverb mix | 0 % to 100 % | 25 % | yes |
| 11 | `size` | Reverb size | 0 % to 100 % | 50 % | yes |
| 12 | `tone` | Reverb tone | 0 % to 100 % | 50 % | yes |
| 13 | `rmode` | Reverb mode | hall, gated, plate, spring | hall | yes |
| 14 | `rpre` | Reverb pre-delay | 0 ms to 150 ms | 40 ms | yes |
| 15 | `rmod` | Reverb modulation | 0 % to 100 % | 40 % | yes |
| 16 | `rera` | Reverb era | 0 % to 100 % | 40 % | yes |
| 17 | `rgate` | Reverb gate | 100 ms to 600 ms | 300 ms | yes |
| 18 | `g` | Output gain | -60.0 dB to +24.0 dB | -6.0 dB | no |
| 19 | `b` | Cab bypass | on, off | off | yes |
| 20 | `drum style` | Drum style | the built-in styles | the second | no |
| 21 | `drum kit` | Drum kit | the built-in kits | the second | no |
| 22 | `drum song` | Drum song | off, or a song | off | no |
| 23 | `drum crash` | Crash | off, 2 bars, 4 bars, 8 bars, 16 bars | 4 bars | no |
| 24 | `bpm` | Tempo | 40 BPM to 240 BPM | 100 BPM | no |
| 25 | `drum vol` | Drum volume | 0 % to 100 % | 60 % | no |
| 26 | `drum comp` | Drum compression | 0 % to 100 % | 40 % | no |
| 27 | `drum count` | Count-in | off, 1 bar, 2 bars, 3 bars, 4 bars | 2 bars | no |
| 28 | `drum train` | Speed trainer | on, off | off | no |
| 29 | `train step` | Trainer step | 1 BPM to 20 BPM | 5 BPM | no |
| 30 | `train every` | Trainer every | 1 to 16 bars | 4 bars | no |
| 31 | `train target` | Trainer target | 40 BPM to 240 BPM | 140 BPM | no |
| 32 | `bass` | Bass | on, off | on | no |
| 33 | `bass vol` | Bass volume | 0 % to 100 % | 70 % | no |
| 34 | `bass key` | Bass key | C, C#, D, Eb, E, F, F#, G, Ab, A, Bb, B | E | no |
| 35 | `bass tone` | Bass tone | 0 % to 100 % | 50 % | no |
| 36 | `amp` | Amp | on, off | off | yes |
| 37 | `amp in` | Amp input | -24.0 dB to +24.0 dB | 0 dB | yes |
| 38 | `amp out` | Amp output | -24.0 dB to +24.0 dB | 0 dB | yes |
| 39 | `amp cal` | Amp calibration | on, off | on | yes |
| 40 | `gate thr` | Gate threshold | -90 dB to -20 dB | -64 dB | yes |
| 41 | `amp bass` | Amp bass | -12.0 dB to +12.0 dB | 0 dB | yes |
| 42 | `amp mid` | Amp middle | -10.0 dB to +10.0 dB | 0 dB | yes |
| 43 | `amp treble` | Amp treble | -12.0 dB to +12.0 dB | 0 dB | yes |
| 44 | `amp presence` | Amp presence | -10.0 dB to +10.0 dB | 0 dB | yes |
| 45 | `gate rel` | Gate release | 10 ms to 500 ms | 50 ms | yes |
| 46 | `gate` | Noise gate | on, off | on | yes |
| 47 | `gate type` | Gate type | nam, hard | hard | yes |
| 48 | `pvol` | Preset level | -24.0 dB to +12.0 dB | 0 dB | yes |
| 49 | `master` | Master volume | silent (the lowest step), -59.5 dB to 0 dB | 0 dB | no |
| 50 | `mod` | Mod | on, off | off | yes |
| 51 | `mod type` | Mod type | chorus, flanger, ce2, phase90 (ce2 uses only rate and depth, phase90 only rate; see [SIGNAL_CHAIN.md](SIGNAL_CHAIN.md)) | chorus | yes |
| 52 | `mod rate` | Mod rate | 0.05 Hz to 10.00 Hz | 0.80 Hz | yes |
| 53 | `mod depth` | Mod depth | 0 % to 100 % | 50 % | yes |
| 54 | `mod fb` | Mod feedback | 0 % to 100 % | 0 % | yes |
| 55 | `mod mix` | Mod mix | 0 % to 100 % | 50 % | yes |
| 56 | `mod width` | Mod width | 0 % to 100 % | 50 % | yes |
| 57 | `lim` | Limiter | on, off | off | no |
| 58 | `lim ceil` | Limiter ceiling | -12.0 dB to -0.5 dB | -1.0 dB | no |
| 59 | `lim rel` | Limiter release | 10 ms to 1000 ms | 100 ms | no |
| 60 | `lim look` | Limiter lookahead | on, off | off | no |
| 61 | `dtype` | Delay type | digital, tape, analog | digital | yes |
| 62 | `drum` | Drums | on, off | off | no |
| 66 | – | Loop from | off, or a bar of the song | off | no |
| 67 | – | Loop to | a bar of the song | the last bar | no |

Notes:
- `g` and `pvol` are one gain after the cab; `master` is the last gain before the DAC, and the limiter (`lim`) comes after it ([SIGNAL_CHAIN.md](SIGNAL_CHAIN.md)).
- `dtime`'s upper end is `DELAY_MAX_MS` in `include/config.h`.
- `dtype` sits at the end and not with the delay's other settings because it was added later (ids are positions). Which settings each delay type and reverb mode has: [SIGNAL_CHAIN.md](SIGNAL_CHAIN.md). A preset written before `dtype` existed loads as `digital`; a preset with `rmode plate`, `rmode spring` or `dtype` loads on older firmware with those lines ignored (hall, digital).
- The amp model and the IR are not settings: they are files, chosen with `amp <name>` and `<number>`, and stored in presets as paths.

## USB MIDI

Any channel. `midi` in the shell prints this map from the code.

| Message | Does |
| --- | --- |
| Program Change n | Recalls preset n + 1 (by the number in its name) |
| CC 7 | Output gain, −40 to 0 dB |
| CC 16 | Amp input, −12 to +12 dB |
| CC 17 | Amp output, −12 to +12 dB |
| CC 70, 71, 72, 73 | Amp bass, middle, treble, presence (whole range) |
| CC 12, 13, 14 | Delay mix, time, repeats |
| CC 91, 92 | Reverb mix, size |
| CC 80, 81, 83, 84 | Amp, gate, delay, reverb on / off (on from 64). CC 82 was the doubler, removed on 14 Nov 2026, and does nothing. |
| CC 85 | Cab bypass |
| CC 86 | Drums play / stop |
| CC 87 | Tuner |
| CC 88 | Mod on / off |

Not yet tried with a real controller ([STATUS.md](STATUS.md)).
