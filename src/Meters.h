// Meters.h - the chain's peak readings for more than one reader. AudioChain's take...Peak()
// calls return the peak since the last call and clear it, so two readers (the app's meters
// and the touch screen's, or the shell's 's') would steal each other's peaks. Here each
// reader gets every peak once: what one reader takes is kept for the others.
// Background only (loop()); the audio interrupt is not involved.
#pragma once

enum MeterReader { kMeterShell, kMeterApp, kMeterScreen, kMeterReaders };

float meterInputPeak(MeterReader who);
float meterOutputPeak(MeterReader who);
float meterStagePeak(MeterReader who, int stage);
// The limiter's lowest gain since this reader's last call (1 = it did nothing).
float meterLimiterGain(MeterReader who);
