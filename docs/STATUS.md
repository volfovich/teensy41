# Status

The tracking page: what works and how that is known, what has not been checked, and what is open. Update it when something is finished, tested or found. Last updated: 15 Nov 2026.

## Where things stand (end of 13 Nov 2026)

**Added on 14 Nov 2026 (not committed when written; the unit runs this firmware):** the memory recap (`mem`, background code to flash, load buffers to the PSRAM); and the touch screen's first version, ([SCREEN.md](SCREEN.md)): 140 host test cases pass, the on-unit protocol check passes. The author wired the display the same day; it is found at boot, calibrated and in use. Waiting for the author there: the verdict on look and touch, and noise by ear. Later the same day the home screen was laid out anew at the author's request (the preset in one bar, the chain in one lane, the drums in their own panel with tempo and beat, the limiter beside the master volume): 141 host test cases pass; flashed, the unit runs it with 0 overruns and its new controls answer injected taps. The author then had it made plainer (names only on the tiles) and found the drums' style name cut off after start and stop: fixed, with a host test that uses every screen at random and compares the glass with a whole repaint (142 host test cases pass).

**Also on 14 Nov 2026: the doubler was removed** at the author's word (DSP, settings, MIDI CC 82, the screen's tile, the app; link protocol version 2). Built, 141 host test cases and the app's tests pass. Flashed the same day: the shell's `s` shows every setting as before the flash (the EEPROM record was converted), 0 overruns; `tools/applink_check.py` passes on the unit (its first run failed six checks because the recalled preset changed twice while it ran, most likely at the unit's screen; run again undisturbed, all pass); the app's preview data was read from the unit again; the new app is in `/Applications`. Not done: the new app was not opened against the unit, and the CPU with everything on was not measured again.

**15 Nov 2026: the backing-track player.** Files in `/TRACKS` (WAV or MP3, any common sample rate) play beside the drums, with play, pause, stop, seek, a looped section, the tempo changed from 50 % to 150 % at the same pitch, the track's own tempo and beats found by a scan, bars, and loop points on beats and bar lines; from the shell (`track ...`, [CONTROL.md](CONTROL.md)). How it is built: [SIGNAL_CHAIN.md](SIGNAL_CHAIN.md), "Backing track". The plan's five steps and what is left: "Backing-track player" under "Open work" below.
- **The unit runs steps 1 to 4** (everything but the touch screen's part), flashed and checked by levels and the shell with made-up test tracks, the master turned down; its settings were compared with those from before and are the same, and the test tracks were removed from the card. The `/TRACKS` folder is new on the card and empty.
- **Step 5, the touch screen's part, was flashed at the author's word** and runs (0 overruns).
- **Folders and search (same day, asked for by the author, whose tracks are in folders below `/TRACKS`):** the unit lists the whole tree (59 tracks in 9 folders on the author's card now; built for thousands), the shell and the touch screen go through it folder by folder, and both search all folders by words ([CONTROL.md](CONTROL.md), [SCREEN.md](SCREEN.md)). Flashed and checked from the shell and by injected taps; not yet seen on the glass by the author.
- **Nobody has heard any of it.** How the tempo change sounds, and whether the tempo scan is right on real songs, are for the author to try.
- During this work the drums were started on the unit by mistake for a few seconds (a bare `drum` typed to read their state flips them) and stopped again; the unit was flashed seven times with the master volume where it was.

Work was stopped here at the author's word. Everything described on this page is committed on the branch `i2s-driver-and-hld`, and the unit runs the firmware built from it.

- **The unit:** the full chain is running: amp, gate, cab, mod (chorus, flanger, ce2, phase90), delay (digital, tape, analog), reverb (hall, gated, plate, spring), drums and bass, master volume, limiter, tuner. The sound of everything ported is as ported; the author confirmed that by ear.
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
| Delay (digital), reverb (hall, gated) | Working | Host tests; in use. Bit for bit unchanged behind the new banks (host test `test_fx_banks`) |
| Reverb: `plate`, `spring`; delay: `tape`, `analog` | Working, **not yet heard by the author** | 46 host tests (decay times, stability, tone, level, the spring's chirp, the delays' repeats, glide, compander, the change between effects). On the unit with a test tone: each costs no more CPU than the ported effect it replaces, no overruns; settings restored afterwards |
| Mod: chorus, flanger | Working; heard by the author on 13 Nov 2026 and found fine | 16 host tests for the block (clicks, stability, ranges, rate, loudness); their output is bit for bit what it was before the two new types were added (compared on the host); on the unit with test tones: 2.4 % CPU, levels and CPU unchanged with Mod off |
| Mod: `ce2` (CE-2 style chorus), `phase90` (Phase 90 style phaser) | Working, **not yet heard by the author** | Host tests: the ce2's triangle sweep, range and low-passed wet signal; the phase90's two notches, sweep rate and feedback hump; both in the click, stability, width, loudness and block-size tests. On the unit with a 220 Hz tone: ce2 2.7 % CPU, phase90 2.4 %, output within 1.3 dB of the Mod-off level, no overruns. Since then the two types have only their pedal's dials (the rest held by the firmware), which was checked in the shell's `s` only |
| Presets on the card | Working | Recall, save, new, rename, delete from the shell and the app |
| Drums and bass | Working | Host tests; running on the board |
| Backing track: WAV at 48 kHz, play, pause, stop, seek, loop (15 Nov 2026) | Working from the shell, **not yet heard by the author** | 22 host test cases (the queue, the WAV reader, the player: order, pause, seek, loop joins, a loop set while playing, `loop()` away for 0.4 s). On the unit with test tones sent to `/TRACKS`: a −12 dBFS tone at `track vol +6` and `master -40` reads −45.1 dBFS at the output (−46 expected, plus the guitar path's noise); 16-bit stereo and 24-bit mono files play; the track ends by itself; seek and loop land where asked; 0.9 % CPU in the interrupt while playing, 0.0 % otherwise; no gaps and no overruns through a card rescan and the shell's 5 s tuner; `overload` stops the track. Settings restored and the test files removed afterwards |
| Backing track: MP3, and files not at 48 kHz (15 Nov 2026) | Working from the shell, **not yet heard by the author** | 12 host test cases: MP3 files made with LAME (constant and variable bitrate, 44.1, 48 and 22.05 kHz, mono) decode to the tones they were made from, a seek gives the samples of a decode from the start within 1 step of 16 bits, a tag with a picture is stepped over; the resampler gives a 44.1 kHz tone at 48 kHz with the error 60 dB down, level within 0.2 dB to 15 kHz, images 60 dB down, a seek bit for bit. On the unit: a −12 dBFS tone as 44.1 kHz MP3 reads the same −45.1 dBFS as the WAV; a 4-minute VBR MP3 opens in 0.35 s, seeks to 3:00, loops 3:10 to 3:12, ends by itself; no gaps or overruns, also through a card rescan; deepest stack use 22.7 KB of 85 KB |
| Backing track: tempo 50 % to 150 % at the same pitch (15 Nov 2026) | Working from the shell, **not yet heard by the author** | 10 host test cases: length within 40 ms of the track's divided by the tempo, tones at their pitch and level without clicks, both sides moved together, 12 drum-like hits stay 12, none doubled, each within 1.5 dB of its level; hits every 25 ms do not stall it; the player's place follows the tempo, a change while playing does not jump, a loop holds at 75 %. On the unit with a 4-minute 44.1 kHz MP3 at 50, 70, 80 and 150 %: no gaps, no overruns, the queue full, a tone at its level (−45.4 dBFS against −45.1), `loop()` busy 25 to 46 % of the time |
| Backing track: tempo and beats found, bars, loop points on beats and bar lines, a file beside each track (15 Nov 2026) | Working from the shell on made-up music; **not tried on real songs** | 9 host test cases: bands of kick, snare, hats, bass and chords at 76 to 174 BPM, at 22, 44.1 and 48 kHz: tempo within 0.005 BPM (174 is found as 87), grid within 5 ms at the first and the last beat, also after 4 minutes; hats alone; a tempo swinging by 10 % comes out "unsure"; silence and a tone give no beat; the track file reads back what was written, skips unknown lines and survives damage; the grid's arithmetic. On the unit: two MP3 songs made at 97 and 120 BPM read 97.00 and 120.00 BPM; scanning takes 7.5 s for a 1-minute MP3 and 28 s for a 4-minute one, goes on through a pause; `track bar`, `track loop bars 3 4` (8.995 s to 13.94 s, as the grid says), `track tempo 80 bpm` (82 %), half and double; tempo and loop are back after reopening the track |
| Backing tracks in folders: list, paging, search (15 Nov 2026) | Working from the shell and by injected taps, **not yet tried by the author** | 7 host test cases for the list (`test_track_index`: order, paths, a half-read list, what does not fit, the search, a browser's moves, 6000 tracks in 40 folders) and 1 for the screen's list (folders, held page buttons, FIND with and without hits). On the unit with the author's card: 59 tracks in 9 folders read in 75 ms (1.4 s while an MP3 played); `track`, `track <n>`, `track ..`, `track find`, a track opened by path and by words; a card rescan while a track played left no gap; the `.trk` file is written beside a track in a folder and read back; by taps: into a folder, a page down, a track opened, FIND "iron", a hit opened. `tools/applink_check.py` passes, with a file put, listed and deleted in a folder below `/TRACKS`. RAM1 code unchanged at 58.0 KB, PSRAM 1.91 MB. Not tried: a card with thousands of tracks (only on the host) |
| Backing track: the touch screen's part (15 Nov 2026) | Flashed, run by injected taps, **not yet judged by the author** | 1 host test case with every control of the new screen, the list of tracks, a tap and a slide on the bar, the held tempo buttons, repainting only what changed; the random-use test (6000 steps against a whole repaint) now includes it; the firmware with it builds, RAM1 code unchanged at 58.0 KB |
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
- **Sending a backing track over the link is slow:** 130 KB/s measured (15 Nov 2026), so a 40 MB WAV takes about 5 minutes; a card reader is quicker for long files.
- **The link dropped one upload of a backing track** (15 Nov 2026): at 0.9 MB of 2.7 MB the unit answered "Say HELLO first", as if the session had timed out; the same file went through on the second try. Not looked into; the app's own uploads are much smaller.
- **The backing track at its dearest keeps `loop()` busy 46 % of the time** (a 44.1 kHz MP3 at 150 % tempo, typical preset, 15 Nov 2026). The screen and the shell answered as usual; not measured with every block on (the interrupt then takes 69 % instead of 57 %).
- **The first version of the beat scan took the ripple of sustained chords for hits** (two notes a few Hz apart make the level swing): on the unit it found no beat in one test song and was unsure of the other. Fixed the same day: a rise counts only above the band's highest level of the last 40 ms. Real music may hold more such surprises; it has not been tried.
- **"Bar 1 is here" moves the grid by half a beat when said nearer to the middle between two beats** (15 Nov 2026). That repairs a grid the scan put on the off-beats, and it goes wrong if it is said a quarter of a beat late (83 ms at 180 BPM); saying it again on a beat puts it right.
- **A slowed backing track's drum hits come early** against its steady sounds: about 35 ms at 50 %, 15 ms at 70 % (host tests). That is how the tempo change keeps a hit whole. Whether it is heard as loose timing is for the author's ear.
- **The author on the phase vocoder (15 Nov 2026): "drums sound fine, ticking is gone, bass sometimes smears, but that is good enough".** After that the phases below 150 Hz were kept running through hits that are not in the bass ([DECISIONS.md](DECISIONS.md)): flashed, 80 % plays without gaps at 57 % of `loop()`; not yet heard by the author. If the bass still smears, the remaining cause is the window's length.
- **RAM2's free heap is 18 KB** (15 Nov 2026, after the phase vocoder's buffers); 5 KB is the most ever taken.
- **The author's first try of the player (15 Nov 2026): "loading and reading takes very long" and "changing the BPM creates very nasty artifacts".** Done the same day: (1) a tempo change now reaches the player once, 0.25 s after the value stops moving (held buttons had restarted it ten times a second: 161 gaps counted); (2) the tempo change itself was rebuilt with 60 ms pieces and 10 ms joins, which the author then heard as "terrible ticking, even at 1 or 2 BPM", and after that replaced by a phase vocoder ([DECISIONS.md](DECISIONS.md)); with it on the unit at 99, 80, 50, 125 and 150 %: no gaps, also none at the changes, the queue full, `loop()` busy 58, 50, 68 and 73 % (typical preset; with every block on the interrupt leaves `loop()` a quarter less, so above 125 % it may not keep up: not measured); (3) the MP3 decoder runs from RAM1, which halves the tempo scan's time. **Not yet heard by the author.** Still slow: the scan takes 55 s for an 8.5-minute MP3 and stands still while the track plays (one decoder), and opening an MP3 takes 0.5 to 1 s (the file is read through once for its length and seek table), during which the screen does not answer.
- **RAM1 code crossed into its 96 KB step on purpose** (15 Nov 2026, for the MP3 decoder): 72.7 KB with the phase vocoder, 25.6 KB of the step free, 47.6 KB left for the stack against 23.9 KB of deepest use.
- **The shell's `cap` (3 s raw capture) stops `loop()` for longer than the backing track's queue lasts:** a playing track gets a gap (counted as `gaps` in `s`). The shell's `tuner` keeps the track fed.
- **`tools/applink_check.py` clears the "edited" mark of a preset whose values are all as saved** (seen 15 Nov 2026: the preset was marked edited before, plain after; every value the same). The check used to fail two checks over that mark and now accepts it.
- **Reading the list of backing tracks takes 0.6 ms per folder or track** (15 Nov 2026), so a card with 6000 tracks is read in about 4 s after power-on, in the background, and longer while a track plays (the MP3 decoder shares `loop()`). Until then folders not yet read show as empty and a search finds only what has been read. Not measured with a card that full.
- **`tools/applink_check.py` fails two checks when the unit's unsaved edit is a different amp model** (seen 15 Nov 2026): its restore step sets the values but not the model, and the model comes back only in a later step. The unit ends as it started (compared by the shell's `s` and `pre show`).
- **Cab bypass also skips the output gain and preset level.** As ported; see [SIGNAL_CHAIN.md](SIGNAL_CHAIN.md). Left alone unless the author wants it changed.
- **Levels after the cab can exceed 0 dBFS** with a high-gain model and a resonant IR at amp output 0 dB. Harmless inside the chain; left alone.
- **Presets 01 and 03** could not be put back to their contents of the morning of 13 Nov 2026 after that day's level experiments; preset 02 was rewritten once.
- **Every model in the "Headbangers Ball" pack reports the same gear and type**: that text is the same inside the files, the app shows what is there.
- **`ADC_FULL_SCALE_DBU` is a datasheet figure**, so the amp's drive may be off by a few dB.
- **The SD card disturbs the ADC** (a thump below 20 Hz about every 1.4 s). Handled by the 30 Hz input high-pass. 14 Nov 2026: after the display was wired the pulse grew to −61 dBFS raw and −66 dBFS after the high-pass, with a 4 ms edge, about every 1.6 s, and opened preset 01's gate (threshold −70 dB); no high-pass that spares the guitar removes such an edge (4th order at 60 Hz leaves −71 dBFS). The author then re-seated a loose wire (which one was not recorded): 30 s of idle input showed no pulse at all, −79 dBFS raw peak and −83 to −85 dBFS after the high-pass. So the thump was most likely the card's current pulse across a poor contact. If it comes back, check the supply and ground joints to the ADC board before touching the filter.
- **The DAC cannot be muted by firmware** (XSMT soldered high): volume down before flashing.
- **No input buffer:** the ADC's 60 kΩ input loads a passive pickup. Use a buffered pedal in front until one is built.

## Open work

### App (from [design/APP_PLAN.md](design/APP_PLAN.md))
- Renaming files on the card (`FILE_RENAME`). Open question first: a renamed model or IR is no longer found by the presets that name it, so either the unit rewrites those presets or the app warns.
- Later: a local library on the Mac, preset packs, A/B compare, undo, a set list view, a drum song editor.
- If it is ever shared: signing, notarisation, an installer, update check.

### Backing-track player (plan agreed with the author on 15 Nov 2026)
For practising against a track from the card: play, loop a section, see the BPM, change the tempo. The author chose shell and touch screen for the first version, and BPM detection with a beat grid (loop points that snap to beats and bars). Everything costly runs in `loop()` and fills a queue; the interrupt only copies ([DECISIONS.md](DECISIONS.md)).

1. **WAV playback: done (15 Nov 2026),** see the Firmware table above.
2. **MP3 and other sample rates: done (15 Nov 2026),** see the Firmware table above. Changed from the plan: no file is written beside the track yet, because finding an MP3's frames takes only 0.35 s for 4 minutes; that file comes with step 4.
3. **Tempo change with the pitch kept: done (15 Nov 2026),** see the Firmware table above. `track tempo <50-150>`.
4. **BPM and beat grid: done (15 Nov 2026),** see the Firmware table above. Changed from the plan: the grid is one steady tempo and a first beat, not a list of beats ([DECISIONS.md](DECISIONS.md)); the scan runs while the track is not playing instead of before its first play.
5. **Touch screen: flashed (15 Nov 2026), waiting for the author's verdict on the glass.** A screen for the track (file list, where it is on a bar that takes a finger, play / pause, stop, A, B, LOOP, tempo, the track's own tempo with /2, x2 and BAR 1, volume) and a TRACK button as the fifth in the home screen's bottom line, whose names are now in the small font ([SCREEN.md](SCREEN.md), with pictures).

Added after the plan (15 Nov 2026): tracks in folders below `/TRACKS`, the list by folder on the screen and in the shell, and a search over all folders (see the Firmware table above).

Not in this version: the app's panel for tracks and sending them from the app (the unit already accepts `/TRACKS` over the link, `tools/track_put.py` uses that), a meter for the track, MIDI controllers, the drums in time with the track, transposing.

Not judged yet: how the tempo change sounds (nobody has heard it; its numbers are first choices). Not tried yet: the tempo scan on real music (only on made-up bands of drums, bass and chords).

### Firmware
- Try USB MIDI with a controller or a DAW.
- The touch screen ([SCREEN.md](SCREEN.md)): wired and running since 14 Nov 2026; with a settings screen for each block since the same day; open: the author's verdict on look and touch, noise by ear. Choosing the model and the IR is in too, and a speed trainer screen one tap from home. Saving as a new preset (with a keyboard for the name) and the master volume are in as well, then dragging a value, renaming and deleting the recalled preset, and the drums' style, song, kit and volume on the trainer screen's second page, with a list to pick from for every named choice. The home screen's second layout (14 Nov 2026) is flashed and runs: open are the 42 px chain tiles under a finger and the author's verdict on it.
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
