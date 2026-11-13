// SerialUI.h - USB serial command line and live level meter.
// Ported from teensy-ir-loader (src/SerialUI.h) for the PCM1808 / PCM5102A hardware and the i2s32 driver.
#pragma once

void printHelp();
// Call from loop(): reads serial input and runs complete command lines.
void serviceSerial();
// Call from loop(): prints the level meter every 200 ms while it is enabled.
void serviceMeter();
