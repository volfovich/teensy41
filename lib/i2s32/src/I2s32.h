#pragma once
// Low-latency I2S driver for Teensy 4.x: SAI1 + eDMA, 48 kHz, two 32-bit slots per frame.
// No dependency on the PJRC Audio Library.
//
// Pins (same as the Audio Library / hardware doc):
//   23 MCLK (256 fs = 12.288 MHz)   21 BCLK (64 fs)   20 LRCLK   7 data out   8 data in
//
// The callback runs in the RX DMA interrupt once per block of DSP_BLOCK frames.
// It must be bounded: no Serial, no malloc, no blocking, and it must finish within one
// block period (DSP_BLOCK / 48000 s). Samples are interleaved L,R,L,R... as raw 32-bit
// I2S words; 24-bit converters use the top 24 bits (see dsp/Convert.h).

#include <stddef.h>
#include <stdint.h>

#ifndef DSP_BLOCK
#define DSP_BLOCK 32
#endif

namespace i2s32 {

constexpr size_t kBlock = DSP_BLOCK;        // frames per callback
constexpr uint32_t kSampleRate = 48000;

static_assert(kBlock >= 4 && kBlock <= 512 && (kBlock & (kBlock - 1)) == 0,
              "DSP_BLOCK must be a power of two between 4 and 512");

// rx: kBlock frames just received (read-only). tx: kBlock frames to fill; they start
// playing one block period after the callback was entered.
using Callback = void (*)(const int32_t* rx, int32_t* tx, size_t frames);

struct Config {
    Callback callback = nullptr;   // nullptr = transmit silence
    int mutePin = -1;              // GPIO wired to the DAC's XSMT (high = un-muted); -1 = none
    uint8_t irqPriority = 48;      // lower number = more urgent; USB is 112, default is 128
    // Overload protection: after this many overruns in a row the driver stops calling the
    // callback, sends silence and mutes the DAC (if it has the pin) until clearFault().
    // Without it an overloaded callback starves loop() completely. 0 = off.
    uint16_t faultAfterOverruns = 16;
};

struct Stats {
    uint32_t blocks;          // callbacks run since begin()
    uint32_t lastCycles;      // CPU cycles spent in the last interrupt
    uint32_t maxCycles;       // worst case since resetStats()
    uint32_t overruns;        // interrupts that took longer than a block period
    uint32_t fifoErrors;      // SAI FIFO underrun/overflow events
    int32_t txLeadFrames;     // how far the TX DMA is ahead of the RX DMA (diagnostic)
    uint32_t faults;          // times the overload protection has tripped since begin()
};

// Starts the clocks, both DMA channels and the interrupt. Call once from setup().
void begin(const Config& config);

// Swap the callback atomically (safe from loop()).
void setCallback(Callback cb);

// Drive the mute pin (no-op when Config::mutePin is -1). begin() leaves the DAC muted
// and the driver un-mutes it automatically after the first few blocks.
void setMute(bool muted);

Stats stats();
void resetStats();                 // clears maxCycles, overruns, fifoErrors

// True while the overload protection holds the output silent (Config::faultAfterOverruns).
bool faulted();
// Ends the fault: the callback runs again from the next block. Call from loop(), after
// reducing the load (or it trips again).
void clearFault();

uint32_t blockPeriodCycles();      // CPU cycles in one block period
float load(uint32_t cycles);       // cycles as a fraction of the block period (0..1)

} // namespace i2s32
