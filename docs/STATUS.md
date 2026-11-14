# Status

The tracking page: what works and how that is known, what has not been checked, and what is open. Update it when something is finished, tested or found. Last updated: 14 Nov 2026.

## Where things stand (end of 13 Nov 2026)

**Added on 14 Nov 2026 (not committed when written; the unit runs this firmware):** the memory recap (`mem`, background code to flash, load buffers to the PSRAM); and the touch screen's first version, ([SCREEN.md](SCREEN.md)): 135 host test cases pass, the on-unit protocol check passes. The author wired the display the same day; it is found at boot, calibrated and in use. Waiting for the author there: the verdict on look and touch, and noise by ear.

Work was stopped here at the author's word. Everything described on this page is committed on the branch `i2s-driver-and-hld`, and the unit runs the firmware built from it.

- **The unit:** the full chain is running: amp, gate, cab, doubler, mod (chorus, flanger, ce2, phase90), delay (digital, tape, analog), reverb (hall, gated, plate, spring), drums and bass, master volume, limiter, tuner. The sound of everything ported is as ported; the author confirmed that by ear.
- **The app:** connects by itself and covers editing, presets, files, meters, tuner, backup and restore, the IR's graphs and the card's free space.
- **Checks at this point:** 121 host test cases, 23 Rust tests, the on-unit protocol check and the audio code placement check all pass. A typical preset runs at 56 % CPU; everything on at once reaches the 80 % budget.
- **Waiting for the author:**
  1. Listen to the two new reverb modes (`plate`, `spring`) and the two new delay types (`tape`, `analog`), added on 13 Nov 2026 after the Mod types, and say what to tune: their voicing numbers are first choices.
  1. Listen to the two new Mod types, `ce2` and `phase90` (added after the stop, the same day), and say what to tune; the chorus and the flanger were heard by the author on 13 Nov 2026 and are fine. The limiter was tried by the author on 13 Nov 2026 and works well; still to say: whether the lookahead was compared.
  2. Try the things only a person can: the Cab graphs with a real IR, the Card tab's buttons and dialogs, drag and drop, the Rate knob's feel.
  3. Decide how renaming a file should treat presets that name it (see "Open work").
- **Not started:** anything under "Open work" below.

## Firmware

| Area | State | How it was checked |
| --- | --- | --- |
| I2S driver, 48 kHz / 24-bit | Working | Clean passthrough by ear; 1,500 blocks/s; no overruns or FIFO errors |
| Overload protection | Working | Serial `overload` on the board |
| NAM A2-Lite amp | Working | Matches the reference engine to 4.8e-7 (host and board); models loaded from the card |
| Noise gate, tone, DC blocker | Working | Host tests against references; in use |
| Cab IR loading and convolution | Working | Host tests; IRs loaded from the card and played |
| Doubler, delay (digital), reverb (hall, gated) | Working | Host tests; in use. Bit for bit unchanged behind the new banks (host test `test_fx_banks`) |
| Reverb: `plate`, `spring`; delay: `tape`, `analog` | Working, **not yet heard by the author** | 46 host tests (decay times, stability, tone, level, the spring's chirp, the delays' repeats, glide, compander, the change between effects). On the unit with a test tone: each costs no more CPU than the ported effect it replaces, no overruns; settings restored afterwards |
| Mod: chorus, flanger | Working; heard by the author on 13 Nov 2026 and found fine | 16 host tests for the block (clicks, stability, ranges, rate, loudness); their output is bit for bit what it was before the two new types were added (compared on the host); on the unit with test tones: 2.4 % CPU, levels and CPU unchanged with Mod off |
| Mod: `ce2` (CE-2 style chorus), `phase90` (Phase 90 style phaser) | Working, **not yet heard by the author** | Host tests: the ce2's triangle sweep, range and low-passed wet signal; the phase90's two notches, sweep rate and feedback hump; both in the click, stability, width, loudness and block-size tests. On the unit with a 220 Hz tone: ce2 2.7 % CPU, phase90 2.4 %, output within 1.3 dB of the Mod-off level, no overruns. Since then the two types have only their pedal's dials (the rest held by the firmware), which was checked in the shell's `s` only |
| Presets on the card | Working | Recall, save, new, rename, delete from the shell and the app |
| Drums and bass | Working | Host tests; running on the board |
| Tuner | Working | Test tone (`tuner test 110`) shows the right note; the author reported it fine with a guitar |
| Settings in EEPROM | Working | Survive power cycles; old record converted once |
| Limiter (brickwall, optional lookahead) | Working, off by default; the author tried it on 13 Nov 2026 and found it works well (whether with lookahead too was not said) | 8 host tests (never over the ceiling in 72 combinations, untouched below it, distortion under −60 dB on steady tones, switching without clicks); on the unit a tone 5 dB over read exactly the ceiling with 0 clipped samples, in both modes |
| Master volume, block level meters | Working | Test tones through the chain; app meters |
| App link (protocol) | Working | `tools/applink_check.py` passes on the unit: every implemented message, file transfer both ways, the IR's samples, the card's space |
| USB MIDI | Built, **not tried** | Map prints; no real controller or DAW has sent messages |
| Sound | **As ported** | Confirmed by the author on 13 Nov 2026 after two unrequested level changes were reverted; test tones matched the morning's firmware within 0.3 dB over 36 points |

Measured (13 Nov 2026, 32-sample blocks): a typical preset (amp, 4 k IR, reverb) 56.3 % CPU average, 57.1 % maximum, 0 overruns. With everything on (doubler, mod, delay, reverb, drums and bass) 69 % average, 80.4 % maximum, 0 overruns. Per effect (13 Nov 2026): reverb hall 8.1 %, gated 6.7 %, plate 4.8 %, spring 8.0 %; delay digital 4.8 %, tape 3.5 %, analog 3.0 %. RAM1 code 58.4 KB, 88.4 KB free for the stack, RAM2 heap 99 KB (14 Nov 2026, with the touch screen's code in; after moving background code to flash and load buffers to the PSRAM; the CPU figures were measured again and are the same, an IR load takes 8 ms instead of 6).

## App

| Area | State | How it was checked |
| --- | --- | --- |
| Connect, reconnect, mirror of all settings | Working | Against the unit |
| Chain blocks, editors, knobs, drums lane | Working | Against the unit; screens in the browser preview |
| Only the dials that apply (Mod type, gate type, reverb mode) | Built, **not seen in the real window** | Browser preview: the reverb in hall mode shows no Gate dial; the shell's `s` shows the Mod line per type |
| Presets: recall, save, save as, rename, delete, edited mark | Working | Against the unit |
| Amp and IR lists, load, send, delete, file facts | Working | Against the unit |
| Meters, CLIP, CPU, block level bars | Working | Against the unit with test tones |
| Master volume | Working | Against the unit |
| CPU meter in the top bar | Built, **not seen in the real window** | Browser preview at 1040, 1180, 1280 and 1400 px; it shows the same values as the footer, which were checked against the unit |
| Tuner | Working | Real window with the unit's test tone |
| Confirmations (recall with edits, replace a file, delete, restore) | Working | Browser preview and the unit |
| IR waveform and frequency response in the Cab editor | Working | The unit's IR fetched and computed through the app's core (4,049 samples, 120 points); the drawing seen in the browser preview with made-up data only |
| Card free space in the Card tab | Working | Against the unit: 14.8 GB free of 14.9 GB, answered in 0.8 s |
| Backup of the card | Working | Full backup through the app's core: 24 files, 4.4 MB, a model byte-identical to the original |
| Restore | Working for one file | A one-file backup restored and removed again; **a full restore over the card has not been run** |

## Not checked by a person
- The Card tab's buttons and the folder dialog in the real window (seen only in the browser preview).
- The Cab editor's two graphs with a real IR in the real window (the numbers behind them were checked, the drawing only with preview data).
- Drag and drop of files onto the real window; the file dialog.
- A preset saved in the app, then recalled by a MIDI program change.
- USB MIDI with a real controller.
- Latency by measurement (the 2.2 ms is from datasheets).
- 24-bit bit activity and the noise floor of the converters.
- A long soak with everything on.
- The worst-case CPU (long IR, busy drums at 240 BPM, IR switching) at 32-sample blocks.

## Known issues and observations
- **RAM1 code is 7.2 KB from its next 32 KB step** (58.4 KB of 64 KB, 14 Nov 2026). The reverbs and delays of 13 Nov took it to 63.9 KB; moving the core's start-up and background routines to flash gave 5.6 KB back. Crossing the step takes 32 KB from the stack's 88.4 KB; the deepest stack use measured is 17.5 KB (`mem`), so that step can be taken when needed. CPU, not memory, is the limit for new effects. Not yet in the stack measurement: `cap`, `amp selftest`, and loading a model that has no cache file.
- **The limiter works on sample peaks**, not on peaks between samples; its default ceiling of −1 dB is the margin for that.
- The limiter's cost with everything else on was not measured together: expect the worst block at 81 to 82 %, over the 80 % budget, when all blocks, the drums and the lookahead limiter run at once.
- Asking the card for its free space stops the unit's `loop()` for about 0.8 s (16 GB card): the audio keeps running, the app's meters pause for that moment. It happens when the app connects and after each file sent or deleted.
- **With every block and the drums on, the worst block reaches the 80 % CPU budget** (80.4 % measured; by subtraction about 78 % before the Mod block). No overruns, and the overload protection covers a spike, but there is no margin left for another effect without making room.
- **The Mod block's level is compensated for loudness** (within 0.3 dB on guitar-band noise in the host test), not for peaks: a steady sine on a comb peak still reads up to +3 dB (chorus) or +7 dB (flanger with strong feedback) higher.
- The Mod block's voicing is a first version, to be tuned by ear. The `ce2` and `phase90` types follow the pedals' published circuits, not recordings or measurements of real pedals; the top of the phase90's sweep (1.6 kHz) is an estimate.
- The worst block with everything on was not measured again with the new Mod types: the ce2 costs 0.3 % more than the chorus the 80.4 % was measured with.
- With the new reverbs and delays the worst block was compared in one session (everything on, limiter with lookahead, 14 s each): hall + digital 75.6 %, plate + tape 69.7 %, spring + analog 73.0 %, gated + digital 73.3 %. The new ones are below the ported pair. These windows are shorter than the one that gave 80.4 %, so they compare the combinations; they are not a new worst case.
- **The new reverbs' and delays' voicing is a first version**, built from papers, not from measurements of real units: the plate is a fifth smaller than Dattorro's (to fit the memory), the spring has 26 all-passes per spring where the paper has 100 (to fit the CPU), so its chirps are short; the tape's wow and flutter, the analog's filters and compander timing are first choices.
- A change of reverb mode to or from plate or spring, or of delay type, while the effect sounds, cuts its tail: 20 ms fade, a few ms of silence, then the new effect from nothing. The same happens when a preset change changes the mode or type.
- **Cab bypass also skips the output gain and preset level.** As ported; see [SIGNAL_CHAIN.md](SIGNAL_CHAIN.md). Left alone unless the author wants it changed.
- **Levels after the cab can exceed 0 dBFS** with a high-gain model and a resonant IR at amp output 0 dB. Harmless inside the chain; left alone.
- **Presets 01 and 03** could not be put back to their contents of the morning of 13 Nov 2026 after that day's level experiments; preset 02 was rewritten once.
- **Every model in the "Headbangers Ball" pack reports the same gear and type**: that text is the same inside the files, the app shows what is there.
- **`ADC_FULL_SCALE_DBU` is a datasheet figure**, so the amp's drive may be off by a few dB.
- **The SD card disturbs the ADC** (a thump below 20 Hz about every 1.4 s). Handled by the 30 Hz input high-pass; the cause is still there.
- **The DAC cannot be muted by firmware** (XSMT soldered high): volume down before flashing.
- **No input buffer:** the ADC's 60 kΩ input loads a passive pickup. Use a buffered pedal in front until one is built.

## Open work

### App (from [design/APP_PLAN.md](design/APP_PLAN.md))
- Renaming files on the card (`FILE_RENAME`). Open question first: a renamed model or IR is no longer found by the presets that name it, so either the unit rewrites those presets or the app warns.
- Later: a local library on the Mac, preset packs, A/B compare, undo, a set list view, a drum song editor.
- If it is ever shared: signing, notarisation, an installer, update check.

### Firmware
- Try USB MIDI with a controller or a DAW.
- The touch screen ([SCREEN.md](SCREEN.md)): wired and running since 14 Nov 2026; with a settings screen for each block since the same day; open: the author's verdict on look and touch, noise by ear. Choosing the model and the IR is in too, and a speed trainer screen one tap from home. Not there yet: saving as new, the master volume.
- Lower latency, if wanted: a non-uniformly partitioned convolver, then 16-sample blocks again.
- Later effects: compressor, parametric EQ, more Mod types (tremolo), more reverbs or delays (shimmer, ping-pong, reverse were offered and not chosen); a longer delay or a looper in PSRAM.

### Hardware (the author's side)
- Input buffer (1 MΩ), then set `INPUT_PAD_DB`.
- A cleaner supply for the ADC.
- Optional: XSMT to a GPIO for a firmware mute; the DAC's low-latency filter jumper (saves about 0.35 ms).

### Protocol
- Reserved and not implemented: `FILE_RENAME`, the `LOG` event, the `BUSY` error.

## History in short
| Date | What |
| --- | --- |
| 11 Nov 2026 | Project set up; PSRAM soldered and verified |
| 13 Nov 2026 | Hardware doc and design (HLD); I2S driver; NAM benchmarks; the chain, effects, presets and drums ported from teensy-ir-loader; codec settings removed; link protocol; the app through milestones A0 to A5 and part of A6; master volume; block meters; backup and restore; documentation set; the Mod block (chorus and flanger) with automatic level compensation; the IR's waveform and frequency response and the card's free space in the app; a CPU meter in the app's top bar; the Limiter block; two more Mod types (`ce2`, `phase90`); settings that do not apply to the chosen type or mode are hidden in the app. |
| 13 Nov 2026 | Two more reverb modes (plate, spring) and two more delay types (tape, analog) behind `ReverbBank` / `DelayBank`, the ported effects untouched; the `dtype` setting; editors and the shell show each mode's and type's own settings; measured on the unit |
