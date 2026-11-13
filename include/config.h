// config.h - compile-time settings: IR handling, memory sizes, the delay's
// fixed voicing and the hardware. The settings changeable over USB serial
// (levels, amp, delay, reverb, drums, bass), with their ranges and the
// defaults a blank EEPROM starts with, are in lib/params/src/params/ParamDefs.cpp.
#pragma once

// Reported to the desktop app (HELLO, protocol/PROTOCOL.md).
#define FIRMWARE_VERSION "0.1.0"

#include <I2s32.h>

// The audio format, fixed by the I2S driver (lib/i2s32): 48 kHz, DSP_BLOCK
// frames per block (platformio.ini). The block size is also the convolver's
// partition size, so it is the latency knob: about 2 blocks in to out.
constexpr int kBlock = (int)i2s32::kBlock;
constexpr float kSampleRate = (float)i2s32::kSampleRate;

// ---------------------------------------------------------------------------
// Latency / block size
// ---------------------------------------------------------------------------
// The audio block size is set in platformio.ini (-D DSP_BLOCK=32).
// The convolver's partition size equals the block size, so the convolution
// itself adds zero latency; total latency is ~2 blocks of I/O buffering plus
// the converters' filters (about 0.8 ms as the boards are built):
//   64 -> ~2.7 ms   32 -> ~1.3 ms   16 -> ~0.7 ms  (+ converters)
// Smaller blocks cost more CPU (see HLD.md for numbers).

// ---------------------------------------------------------------------------
// Impulse responses
// ---------------------------------------------------------------------------
// Longest IR in samples at 48 kHz. 4096 samples = 85 ms, longer than any
// cabinet has audible response; longer files are cut there with a short
// fade. Memory (DTCM) and convolution CPU scale with it: 8192 doubles both.
// Must be a multiple of the block size.
#define IR_MAX_LENGTH 4096

// Folders scanned for .wav files (FAT is case-insensitive: "/IR" == "/ir").
#define IR_SCAN_DIRS {"/", "/IR"}
#define IR_MAX_FILES 128
// The most amp models listed from /AMP.
#define AMP_MAX_FILES 64
#define IR_PATH_MAX 96

// Crossfade time when switching IRs (no clicks, no dropouts).
#define IR_FADE_MS 25

// Loudness matching between IRs: kEnergy (recommended), kPeak or kNone.
#define IR_NORMALIZE dsp::Normalize::kEnergy
// Stereo IR files: kFirst = left channel only, kMix = average of channels.
#define IR_CHANNEL_MODE dsp::ChannelMode::kFirst
// Trimming relative to the IR peak (set to 0 to disable).
//  - leading silence is pure latency, so it is removed
//  - the inaudible tail is removed to save CPU
#define IR_TRIM_LEAD_DB -70.0f
#define IR_TRIM_TAIL_DB -80.0f

// ---------------------------------------------------------------------------
// Delay (between the IR and the reverb): the studio 80s wet/dry/wet dual
// delay, after the digital racks of the era (see teensy-ir-loader's DELAY.md).
// ---------------------------------------------------------------------------
// Longest delay time. The two float delay lines need 2 x 4 bytes per sample
// of delay (750 ms = 288 KB at 48 kHz), one in RAM1 and one in RAM2: every
// 100 ms takes about 19 KB from each.
#define DELAY_MAX_MS 750
// Fixed voicing.
#define DELAY80_RIGHT_RATIO 0.75f        // right time = left time x this (0.1 .. 1)
#define DELAY80_CROSS 0.2f               // share of each side's repeats fed to the other (0 .. 0.5)
#define DELAY80_BASS_CUT 0.15f           // bass loss per repeat (0 .. 1)
#define DELAY80_MOD_RATE_HZ 0.8f         // chorus rate of the tail
#define DELAY80_DUCK_THRESHOLD_DB -36.0f // input level where ducking starts (full 12 dB above)
#define DELAY80_DUCK_RELEASE_MS 200.0f   // how fast the repeats swell back after you stop

// ---------------------------------------------------------------------------
// Amp (before the IR): NAM A2-Lite models from /AMP (see HLD.md and teensy-ir-loader's NAM.md).
// ---------------------------------------------------------------------------
// The loudness every model is levelled to, in dB (the model's gain is this
// minus its "loudness"; the amp's output setting comes on top). -18 is what NAM
// players use. It was -12 while the master volume sat right after the cab with a
// default of -6 dB; the master is now the last gain and tops out at 0 dB, so the
// 6 dB of headroom are taken here instead. Measured with a high-gain model, amp
// output 0 dB and a cab IR that peaks +10 dB at its resonance: about -5 dBFS
// after the cab (with -12 it was +1 dBFS, which clipped at master 0 dB).
#define AMP_LEVEL_DB -18.0f
// The loss of the buffer in front of the ADC, in dB (0 without one, as now).
// teensy-ir-loader's JFET TL072 buffer (1 MOhm in, 100 Ohm out) has -6 dB;
// with it a guitar's 0 dBFS is 6 dB hotter than the ADC's, and the
// calibration adds it.
#define INPUT_PAD_DB 0.0f
// The interface level, in dBu at 0 dBFS, that an uncalibrated model is
// played as if through (with amp cal on): the NAM plugin's default, +12 dBu.
#define AMP_UNCAL_REF_DBU 12.0f

// ---------------------------------------------------------------------------
// Hardware (see the hardware design doc)
// ---------------------------------------------------------------------------
// Which PCM1808 input the guitar is wired to: 1 = right, 0 = left. The other
// input is unused.
#define GUITAR_ON_RIGHT_CHANNEL 1
// The level at the ADC's pins that reads 0 dBFS, in dBu: 1.06 Vrms with a 5 V
// analog supply (datasheet value, not yet measured on this board). The amp's
// input calibration starts from it (AmpLibrary.h).
#define ADC_FULL_SCALE_DBU 2.7f
// High-pass on the guitar input, before everything else (2nd order, Hz).
// The SD card in the Teensy's slot draws a pulse of current about every 1.4 s
// whatever state it is in, and that reaches the ADC as a thump below 20 Hz at
// -66 dBFS, enough to open the gate into a high-gain model. On a captured
// pulse, 30 Hz leaves -81 dBFS (the noise floor is -84) and costs 0.1 dB at a
// guitar's low E, 0.2 dB at a 7-string's low B. (40 Hz: -82 dBFS, 0.2 / 0.7 dB;
// 20 Hz: -79 dBFS, almost no loss.)
#define INPUT_HIGHPASS_HZ 30.0f
// GPIO wired to the PCM5102A's XSMT pad (high = un-muted), or -1: the pad is
// soldered high for now (HLD.md section 1, item 1b).
#define DAC_MUTE_PIN -1
// The SD card: the Teensy 4.1's own slot.
#define SDCARD_CS_PIN BUILTIN_SDCARD
