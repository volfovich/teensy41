# Teensy 4.1 Guitar DSP Platform and NAM Forge

A guitar processor on a Teensy 4.1, and the macOS app that controls it.

- **The unit:** guitar in, a Neural Amp Modeler (NAM A2-Lite) amp model, noise gate, cabinet impulse response, doubler, a Mod block (chorus, flanger, a CE-2 style chorus, a Phase 90 style phaser), a delay (digital, tape echo or analog) and a reverb (hall, gated, plate or spring), an output limiter, plus a drum machine with bass for practice. 48 kHz / 24-bit from converter to converter, 32-bit float inside, about 2.2 ms from input to output.
- **The app, "Volfovich NAM Forge":** shows the chain as blocks, edits every setting, manages presets, sends amp models and IRs to the unit's SD card, has a tuner and level meters, draws the loaded IR's waveform and frequency response, and backs the card up to the Mac.

Most of the DSP and the firmware's application code is ported from the author's earlier project, [teensy-ir-loader](https://github.com/volfovich/teensy-ir-loader). New here: the hardware (PCM1808 ADC and PCM5102A DAC instead of a codec), the I2S driver, the link protocol and the app.

```
                         ┌──────────────── Teensy 4.1 ─────────────────┐
 guitar ─► PCM1808 ADC ─►│ I2S driver ─► signal chain ─► I2S driver    │─► PCM5102A DAC ─► amp / phones
                         │   (audio interrupt, one 32-sample block)    │
                         │                                             │
                         │ loop(): serial shell, USB MIDI, app link,   │
                         │         SD card (models, IRs, presets)      │
                         └───────────────▲─────────────────────────────┘
                                         │ USB (serial + MIDI)
                              ┌──────────┴───────────┐
                              │ NAM Forge (macOS)    │   or a serial terminal, or a MIDI controller
                              └──────────────────────┘
```

Current state, open items and what has not been checked: [docs/STATUS.md](docs/STATUS.md).

## Quick start

Firmware (needs [PlatformIO](https://platformio.org)):

```
pio run                  # build
pio run -t upload        # flash (Teensy Loader; press the board's button if it does not reboot)
pio device monitor       # the serial shell: ? lists the commands, s prints the status
pio test -e native       # the host tests
```

App (needs Rust and Node; run from `app/`):

```
npm install              # once
npm run tauri dev        # run the app
```

Only one program can hold the unit's serial port: quit the app before flashing or opening the serial monitor, and the other way round.

## The SD card

| Folder | Holds |
| --- | --- |
| `/AMP` | NAM A2 amp models (`.nam`), as downloaded. `/AMP/.cache` is the unit's own (faster loading). |
| `/IR` or the root | Cabinet impulse responses (`.wav`, any common format and rate) |
| `/PRESETS` | Presets, one text file each (`01 Name.txt`) |
| `/DRUMS` | General MIDI drum files (`.mid`) as extra drum songs |

Without a card the unit still plays: the amp uses a built-in example model and there is no cab.

## Documents

| Document | What it covers |
| --- | --- |
| [docs/SIGNAL_CHAIN.md](docs/SIGNAL_CHAIN.md) | The audio path, block by block: order, gains and levels, settings, CPU, memory, latency |
| [docs/FIRMWARE.md](docs/FIRMWARE.md) | How the firmware is built up: modules, the two contexts, settings and presets, memory placement, build environments |
| [docs/CONTROL.md](docs/CONTROL.md) | Reference: serial commands, the settings table, the USB MIDI map, test aids |
| [docs/APP.md](docs/APP.md) | The macOS app as built: its parts, how it talks to the unit, the screen |
| [docs/SCREEN.md](docs/SCREEN.md) | The unit's touch screen as built: screens, structure, what it costs, what is not checked |
| [protocol/PROTOCOL.md](protocol/PROTOCOL.md) | The link protocol between the app and the unit |
| [docs/HARDWARE.md](docs/HARDWARE.md) | Wiring, the two converter boards, clocking, bring-up |
| [docs/TECHNOLOGIES.md](docs/TECHNOLOGIES.md) | Parts, toolchains, libraries, third-party code and licences |
| [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) | Working on the project: checks to run, how to add things, pitfalls |
| [docs/STATUS.md](docs/STATUS.md) | What works, what is not checked, open items, next steps |
| [docs/DECISIONS.md](docs/DECISIONS.md) | The decisions taken so far and why |
| [docs/design/HLD.md](docs/design/HLD.md) | Firmware design record: review of the hardware, budgets, benchmarks |
| [docs/design/APP_PLAN.md](docs/design/APP_PLAN.md) | The app's plan: features, screen design, milestones |
| [app/README.md](app/README.md) | The app's commands and how to use it |
| [songs/README.md](songs/README.md) | The built-in drum songs and their text format |
| [CLAUDE.md](CLAUDE.md) | Instructions for the coding assistant |

When something changes, the place to update is the document that owns the fact: a level or a block in `SIGNAL_CHAIN.md`, a command or setting in `CONTROL.md`, a message in `PROTOCOL.md`, progress in `STATUS.md`, a choice in `DECISIONS.md`.
