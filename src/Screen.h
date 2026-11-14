// Screen.h - the unit's touch screen: a 2.8-inch ILI9341 (240x320, SPI) with an XPT2046
// resistive touch panel, used in landscape. Wiring: docs/HARDWARE.md; the screens
// themselves are portable code in lib/ui (as built: docs/SCREEN.md). This file is the unit's
// side: the display and touch drivers, the UiModel that lets the screens see and change the
// unit, touch calibration, and debug commands.
//
// Everything here runs in loop(); the audio interrupt never touches the display.
// A unit without a display attached is detected at boot and nothing is drawn or read.
#pragma once

#include <stdint.h>

// Pins (SPI0: MOSI 11, MISO 12, SCK 13, shared by the display and the touch chip).
static const uint8_t kTftCs = 10, kTftDc = 9, kTftReset = 5, kTouchCs = 4, kTouchIrq = 3;

// Call once in setup(), after the audio has started (it waits about 0.3 s for the display).
void screenBegin();
// Call from loop(): reads the touch panel and paints at most one strip of the screen.
void serviceScreen();
// Shell 'tft ...': see the help.
void screenCommand(const char* arg);
