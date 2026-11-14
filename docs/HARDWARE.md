# Teensy 4.1 Audio DSP Platform: PCM1808 + PCM5102A Design Doc

Nov 12, 2026 · @Jacob

> **Note (13 Nov 2026):** this is the hardware document as first written, kept for the wiring, the boards' jumpers and the bring-up checklist. Since then: the firmware runs at **48 kHz only** with its own I2S driver (`lib/i2s32`), so the Teensy4i2s starter code in "Software: keeping 24 bits" is history; the guitar goes into the ADC's **right** channel. The review of this document is in [`design/HLD.md`](design/HLD.md) §1, the firmware in [`FIRMWARE.md`](FIRMWARE.md), open hardware work in [`STATUS.md`](STATUS.md).

## Overview

A Teensy 4.1 sits between a PCM1808 stereo ADC and a PCM5102A stereo DAC and processes audio in real time at 24-bit resolution.

Signal path: line in → PCM1808 → I2S → Teensy 4.1 (DSP) → I2S → PCM5102A → line out.

The Teensy is the I2S master and generates all three clocks. Both converter boards are slaves. The ADC takes its master clock from the Teensy; the DAC regenerates its own from the bit clock.

This doc records the hardware as modified and soldered on 12 Nov 2026. Jumper states were checked from photos; continuity has not yet been measured.

## Wiring

Ten wires leave the Teensy: three clocks, two data lines, two supplies and ground. MCLK goes to the ADC only.

| Signal | Teensy 4.1 pin | PCM1808 pin | PCM5102A pin |
| --- | --- | --- | --- |
| Master clock (MCLK) | 23 | SCK | **Do not connect** (SCK is grounded by jumper) |
| Bit clock (BCLK) | 21 | BCK | BCK |
| Word clock (LRCLK) | 20 | LRC | LCK |
| ADC data → Teensy | 8 (IN1) | OUT | – |
| Teensy → DAC data | 7 (OUT1A) | – | DIN |
| 3.3 V | 3.3V | 3.3V | – |
| 5 V | VIN (USB 5 V) | +5V | VIN |
| Ground | GND | GND | GND |

Extra connections on the ADC board itself:

| PCM1808 pin | Connect to | Result |
| --- | --- | --- |
| FMT (silkscreen reads "PMT") | GND | 24-bit I2S output format |
| MD0 | GND | Slave mode |
| MD1 | GND | Slave mode |

Analog connections:

- **Input:** left and right line-in plus ground go to the three pads at the top edge of the PCM1808 board. The guitar is wired to the **right** input; the left input is unused.
- **Output:** the 3.5 mm jack on the PCM5102A board, or its L / G / R / G pads.

Notes:

- Wiring Teensy pin 23 to the DAC's SCK pad would short the clock output to ground. Leave that pad empty.
- The PCM1808 board has a 3.3V pad and a GND pad on each side. One of each is enough if the pairs are joined on the board; confirm with a continuity check.
- Keep the MCLK wire short. It carries about 11.3 MHz.

## Touch screen

A 2.8-inch SPI display board (ILI9341, 240 x 320) with a resistive touch panel (XPT2046), on SPI0. **Wired by the author on 14 Nov 2026** and found by the firmware at boot; the firmware expects exactly these pins (`src/Screen.h`) and also runs without the display. Which way the backlight was wired (3.3 V direct, or VIN through 100 Ω) was not recorded. The software: [SCREEN.md](SCREEN.md).

| Display pin | Teensy 4.1 pin | Note |
| --- | --- | --- |
| VCC | VIN (5 V) | The board has its own 3.3 V regulator (jumper J1 open) |
| GND | GND | At the Teensy, not at a converter board's ground |
| CS | 10 | |
| RESET | 5 | |
| DC | 9 | |
| SDI (MOSI) and T_DIN | 11 | Shared by the display and the touch chip |
| SCK and T_CLK | 13 | Shared; the Teensy's LED is on this pin and will flicker |
| SDO (MISO) and T_DO | 12 | Shared |
| LED | 3.3 V, or VIN through 100 Ω | Never 5 V without the resistor. Fixed brightness: no PWM dimming, so no switching noise on the supply |
| T_CS | 4 | |
| T_IRQ | 3 | |

- Pins 7, 8, 20, 21 and 23 are the I2S and stay as they are. PJRC's usual touch pin (8) is the ADC's data line here, which is why touch is on 4 and 3.
- Keep the wires short (under 10 cm) and away from the ADC board and the MCLK wire: the bus runs at 30 MHz.
- The SD slot on the display board is not connected; the unit uses the Teensy's own.
- The display's logic is 3.3 V. Do not bridge J1 while VCC is at 5 V.

## PCM1808 ADC board

The board runs as a 24-bit I2S slave with its stock input filter removed.

**Input modification (done).** The stock board had a 10 kΩ series resistor and a 100 nF shunt capacitor on each input, a low-pass at about 160 Hz. On both channels the capacitor was removed and the resistor pads were bridged with solder. The input now reaches the chip only through the 10 µF coupling capacitor.

| Parameter | Value |
| --- | --- |
| Mode | Slave (MD0 = MD1 = low) |
| Output format | I2S, 24-bit, MSB first (FMT = low) |
| Analog supply | 5 V on the +5V pin |
| Digital supply | 3.3 V on the 3.3V pin |
| Logic level on OUT | 3.3 V (follows the digital supply) |
| Full-scale input | About 3 Vpp (1.06 Vrms) with a 5 V analog supply |
| Input impedance | About 60 kΩ |
| Dynamic range | About 99 dB |

The Teensy 4.1 is not 5 V tolerant. The OUT pin is safe only while the board's 3.3V pin is fed from 3.3 V, never from 5 V.

## PCM5102A DAC board

All five solder jumpers are set, and the DAC is configured for I2S input, un-muted, with its clock derived from BCK.

Back of board (each jumper bridges its middle pad to the H or L side):

| Jumper | Function | Soldered to | Meaning |
| --- | --- | --- | --- |
| H1L | FLT | L | Normal-latency FIR filter |
| H2L | DEMP | L | De-emphasis off |
| H3L | XSMT | H | Un-muted |
| H4L | FMT | L | I2S format |

Front of board:

| Jumper | Soldered | Meaning |
| --- | --- | --- |
| SCK (next to the SCK pad) | Bridged | SCK tied to ground; the chip's PLL generates the system clock from BCK |

| Parameter | Value |
| --- | --- |
| Supply | 5 V on VIN (on-board 3.3 V regulators) |
| Logic level | 3.3 V |
| Accepted word length | 16, 24 or 32 bits |
| Full-scale output | About 2.1 Vrms |
| Dynamic range | About 112 dB |

The header pads FLT, DEMP, XSMT, FMT and A3V3 along the long edge stay unconnected, since the jumpers set those pins.

**Supply voltage.** The PCM5102A chip itself is a 3.3 V part (absolute maximum about 3.9 V) and 5 V directly on it would destroy it. 5 V is safe only on the board's **VIN** pin, because this style of board (GY-PCM5102) has on-board 3.3 V regulators between VIN and the chip. The **A3V3** pad is the regulator's output: never feed it, and never put 5 V on it. Confirm the regulators are present with the checklist below before the first power-up. If the board turns out to have no regulator, power VIN from 3.3 V instead.

## Clocking and data format

Each channel gets a 32-bit slot on the wire, and both converters use the top 24 bits of it, so the link is 24-bit without any hardware setting.

| Clock | Ratio | At 44.1 kHz (not used) | At 48 kHz |
| --- | --- | --- | --- |
| MCLK (pin 23) | 256 × fs | 11.2896 MHz | 12.288 MHz |
| BCLK (pin 21) | 64 × fs | 2.8224 MHz | 3.072 MHz |
| LRCLK (pin 20) | fs | 44.1 kHz | 48 kHz |

- **ADC:** in slave mode the PCM1808 detects the 256 × fs master clock automatically and outputs 24 bits, MSB first, in each 32-bit slot.
- **DAC:** with SCK grounded the PCM5102A locks its PLL to BCLK. This needs BCLK at 64 × fs, which is what the Teensy produces.
- **Sample alignment:** data is left-aligned in the 32-bit word. The audio sits in bits 31–8 and bits 7–0 are zero.

The PCM1808 supports sample rates up to 96 kHz, which sets the ceiling for this pair.

## Software: keeping 24 bits

The stock Teensy Audio Library is 16-bit throughout and drops the low 8 bits, so the DSP code must use a library that passes the full 32-bit word.

| Option | Sample type | Fits when |
| --- | --- | --- |
| [Teensy4i2s](https://github.com/ValdemarOrn/Teensy4i2s) (recommended start) | `int32_t` buffers in one callback | Writing your own DSP from scratch |
| [OpenAudio\_ArduinoLibrary](https://github.com/chipaudette/OpenAudio_ArduinoLibrary) | `float` blocks, Audio-library style patching | You want ready-made F32 blocks |

Teensy4i2s uses the same pins as the wiring table and always moves 32 bits per sample. Convert to float on the way in and back on the way out:

```cpp
#include "Teensy4i2s.h"

void processAudio(int32_t** in, int32_t** out) {
  for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
    float l = in[0][i] * (1.0f / 2147483648.0f);   // -1..+1
    float r = in[1][i] * (1.0f / 2147483648.0f);

    // ... DSP here ...

    out[0][i] = (int32_t)(l * 2147483647.0f);
    out[1][i] = (int32_t)(r * 2147483647.0f);
  }
}

void setup() {
  i2sAudioCallback = processAudio;
  InitI2s();
}

void loop() {}
```

The callback and init names above are written from memory. Check them against the library's passthrough example before building on them.

**Proving the path is 24-bit.** In the callback, OR together bits 15–8 of every raw input word in a block and print the result. Non-zero and changing means data below the 16-bit level is arriving. Always zero means something is truncating.

**What 24-bit buys here.** The ADC's 99 dB dynamic range is about 16.5 bits, so its lowest bits are noise. The benefit is processing headroom and the DAC's 112 dB output floor.

## Bring-up checklist

Before power:

- [ ] DAC board: regulators present (one or two small 5-pin SOT-23-5 parts near VIN). If there are none, VIN must get 3.3 V, not 5 V
- [ ] DAC board: VIN is **not** shorted to the A3V3 pad (0 Ω would mean VIN goes straight to the chip)
- [ ] DAC board: no continuity between A3V3 and AGND (rules out a jumper blob touching its third pad)
- [ ] DAC board: SCK pad shows continuity to GND
- [ ] DAC board: XSMT pad shows continuity to A3V3; FLT, DEMP and FMT pads show continuity to GND
- [ ] ADC board: FMT, MD0 and MD1 wired to GND
- [ ] ADC board: 3.3V pin goes to Teensy 3.3V, +5V pin goes to Teensy VIN
- [ ] Nothing is wired to the DAC's SCK pad

After power:

- [ ] First power-up with only the DAC board's VIN and GND connected: the A3V3 pad reads about 3.3 V. If it reads about 5 V, disconnect immediately
- [ ] Teensy 3.3V rail still reads 3.3 V with both boards attached
- [ ] Output test: generate a sine in code and hear it on the DAC jack
- [ ] Passthrough test: line in to line out with the starter code
- [ ] 24-bit test: bits 15–8 of the input words are active
- [ ] Noise test: short the ADC inputs to ground and log the RMS level

For an analog loopback from DAC to ADC, keep the digital level at or below −6 dBFS. The DAC's 2.1 Vrms full scale is twice the ADC's 1.06 Vrms and will clip it.

## Open items and limits

- **Jumpers verified by photo only.** The multimeter checks in the checklist are still to do.
- ~~Teensy4i2s API names unconfirmed.~~ Closed: confirmed, and the firmware no longer uses that library.
- **USB 5 V feeds the ADC's analog supply.** It is fine for development; a cleaner 5 V source should lower the noise floor.
- **No external anti-alias filter or input protection on the ADC.** The chip's internal filter covers normal line sources. A small series resistor and capacitor per input could be added later if RF pickup or over-voltage becomes a problem.
- **Chip figures are datasheet values quoted from memory**, not measured on these boards.

## Sources

- [ValdemarOrn/Teensy4i2s](https://github.com/ValdemarOrn/Teensy4i2s): pinout, clock ratios, 32-bit transfers
- [chipaudette/OpenAudio\_ArduinoLibrary](https://github.com/chipaudette/OpenAudio_ArduinoLibrary): 32-bit float audio classes
- [PJRC forum: I2S input limitation](https://forum.pjrc.com/threads/67783-I2s-input-limitation-16-bits-44100?p=284258): stock Audio Library is 16-bit
