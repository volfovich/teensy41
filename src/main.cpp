// Teensy 4.1 guitar DSP platform: main firmware.
//
// Signal flow today (driver bring-up stage, see HLD.md):
//   PCM1808 ADC left ─► float ─► [DSP chain goes here] ─► float ─► PCM5102A DAC left + right
//
// Audio runs in the I2S DMA interrupt, one block of DSP_BLOCK frames (48 kHz, 24-bit) at a
// time. loop() only handles the serial shell and USB MIDI.
//
// Serial commands (pio device monitor), one per line:
//   s    print audio statistics (CPU load, overruns)
//   b    toggle bypass (bypass = input copied to output bit-for-bit)
//   m    toggle output mute

#include <Arduino.h>
#include <I2s32.h>
#include <dsp/Convert.h>

// GPIO wired to the PCM5102A XSMT pad (HLD.md section 1, item 1b). -1 = not wired.
static constexpr int kDacMutePin = -1;

static volatile bool g_bypass = false;
static volatile bool g_muted = false;

FASTRUN static void audioCallback(const int32_t* rx, int32_t* tx, size_t frames) {
    if (g_bypass) {
        memcpy(tx, rx, 2 * frames * sizeof(int32_t));
        return;
    }

    float left[i2s32::kBlock];
    float right[i2s32::kBlock];
    dsp::int24InToFloat(rx, left, right, frames);

    // Guitar is mono on the ADC's left channel. The DSP chain will process `left` here.

    if (g_muted) memset(left, 0, sizeof(left));
    dsp::floatToInt24Out(left, left, tx, frames);
}

static void printStats() {
    const i2s32::Stats s = i2s32::stats();
    Serial.printf("blocks %lu  load %.1f%% (max %.1f%%)  overruns %lu  fifo errors %lu  bypass=%d  mute=%d\n",
                  static_cast<unsigned long>(s.blocks),
                  static_cast<double>(100.0f * i2s32::load(s.lastCycles)),
                  static_cast<double>(100.0f * i2s32::load(s.maxCycles)),
                  static_cast<unsigned long>(s.overruns), static_cast<unsigned long>(s.fifoErrors),
                  g_bypass, g_muted);
}

static void handleCommand(char* line) {
    switch (line[0]) {
    case 's':
        printStats();
        i2s32::resetStats();
        break;
    case 'b':
        g_bypass = !g_bypass;
        Serial.printf("bypass=%d\n", g_bypass);
        break;
    case 'm':
        g_muted = !g_muted;
        i2s32::setMute(g_muted);
        Serial.printf("mute=%d\n", g_muted);
        break;
    default:
        Serial.println("commands: s (stats) | b (bypass) | m (mute)");
        break;
    }
}

static void pollSerial() {
    static char buf[64];
    static size_t len = 0;
    while (Serial.available()) {
        const char ch = Serial.read();
        if (ch == '\r' || ch == '\n') {
            if (len) { buf[len] = '\0'; handleCommand(buf); len = 0; }
        } else if (len < sizeof(buf) - 1) {
            buf[len++] = ch;
        }
    }
}

void setup() {
    Serial.begin(115200);

    i2s32::Config cfg;
    cfg.callback = audioCallback;
    cfg.mutePin = kDacMutePin;
    i2s32::begin(cfg);

    const uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 1500) {}
    Serial.printf("Guitar DSP ready: 48 kHz / 24-bit, block %u frames. Type ? for commands.\n",
                  static_cast<unsigned>(i2s32::kBlock));
}

void loop() {
    pollSerial();
    while (usbMIDI.read()) {}   // drain incoming MIDI; mapping comes with presets

    static elapsedMillis sinceStats;
    if (sinceStats >= 5000) {
        sinceStats = 0;
        printStats();
    }
}
