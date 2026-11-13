// I2S driver self-test for the bare Teensy 4.1 (no ADC/DAC boards needed).
//
//   pio run -e i2s_loopback -t upload && pio device monitor -e i2s_loopback
//
// Wiring: ONE jumper wire from pin 7 (I2S data out) to pin 8 (I2S data in).
// Without the jumper only the clock/interrupt steps run; the data steps are skipped.
// Do not run this with the DAC connected to an amplifier: the test pattern is full-scale noise.
//
// Steps:
//   1. Sample rate      block interrupts arrive at exactly 48 kHz / DSP_BLOCK
//   2. Bit-exact loop   every one of the 32 bits per word survives TX -> RX; reports the
//                       round-trip delay in frames (= the driver's digital latency)
//   3. Load             callback burning 50 % and 80 % of the block period: no errors
//   4. Overrun          a callback that is too slow is detected, and the driver recovers
//   5. Soak             bit-exact check until a key is pressed

#include <Arduino.h>
#include <I2s32.h>

using i2s32::kBlock;

static constexpr size_t kWordsPerBlock = 2 * kBlock;
static constexpr uint32_t kMaxDelayWords = 16 * kWordsPerBlock;

// Pseudo-random 32-bit word for stream position i. Stateless, so the receiver can
// compute what it expects for any position and delay.
static inline uint32_t pattern(uint32_t i) {
    i *= 0x9E3779B1u;
    i ^= i >> 15;
    i *= 0x85EBCA77u;
    i ^= i >> 13;
    return i;
}

// ---- Shared with the audio interrupt ------------------------------------------
static volatile uint32_t g_pos = 0;            // stream position (words) of the next block
static volatile bool g_snapRequest = false;
static int32_t g_snap[kWordsPerBlock];
static volatile uint32_t g_snapPos = 0;
static volatile bool g_verify = false;
static volatile uint32_t g_delayWords = 0;
static volatile uint32_t g_errors = 0;         // mismatching words
static volatile uint32_t g_checked = 0;        // words compared
static volatile uint32_t g_burnCycles = 0;     // busy-wait this long in every callback
static volatile uint32_t g_slowBlocks = 0;     // for this many blocks, burn g_slowCycles instead
static volatile uint32_t g_slowCycles = 0;

FASTRUN static void audioCallback(const int32_t* rx, int32_t* tx, size_t frames) {
    const uint32_t t0 = ARM_DWT_CYCCNT;
    const uint32_t pos = g_pos;
    const size_t words = 2 * frames;

    for (size_t i = 0; i < words; ++i) tx[i] = static_cast<int32_t>(pattern(pos + i));

    if (g_snapRequest) {
        memcpy(g_snap, rx, sizeof(g_snap));
        g_snapPos = pos;
        g_snapRequest = false;
    }
    if (g_verify) {
        const uint32_t expectPos = pos - g_delayWords;
        uint32_t errors = 0;
        for (size_t i = 0; i < words; ++i) {
            if (static_cast<uint32_t>(rx[i]) != pattern(expectPos + i)) ++errors;
        }
        g_errors = g_errors + errors;
        g_checked = g_checked + words;
    }
    g_pos = pos + words;

    uint32_t burn = g_burnCycles;
    if (g_slowBlocks) {
        g_slowBlocks = g_slowBlocks - 1;
        burn = g_slowCycles;
    }
    while (ARM_DWT_CYCCNT - t0 < burn) {}
}

// ---- Helpers ---------------------------------------------------------------------
static int g_failures = 0;

static void report(bool pass, const char* what) {
    Serial.printf("  [%s] %s\n", pass ? "PASS" : "FAIL", what);
    if (!pass) ++g_failures;
}

static bool waitForBlocks(uint32_t count, uint32_t timeoutMs) {
    const uint32_t target = i2s32::stats().blocks + count;
    const uint32_t t0 = millis();
    while (static_cast<int32_t>(i2s32::stats().blocks - target) < 0) {
        if (millis() - t0 > timeoutMs) return false;
    }
    return true;
}

// Returns the TX -> RX delay in words, or -1 when the pattern is not coming back.
static int32_t findDelayWords() {
    g_snapRequest = true;
    const uint32_t t0 = millis();
    while (g_snapRequest) {
        if (millis() - t0 > 500) return -1;
    }
    const uint32_t pos = g_snapPos;
    for (uint32_t d = 0; d <= kMaxDelayWords; ++d) {
        bool match = true;
        for (size_t i = 0; i < kWordsPerBlock && match; ++i) {
            match = static_cast<uint32_t>(g_snap[i]) == pattern(pos - d + i);
        }
        if (match) return static_cast<int32_t>(d);
    }
    return -1;
}

static void startVerify(uint32_t delayWords) {
    g_verify = false;
    g_delayWords = delayWords;
    g_errors = 0;
    g_checked = 0;
    g_verify = true;
}

static void printStats(const char* label) {
    const i2s32::Stats s = i2s32::stats();
    Serial.printf("    %s: load last %.1f%% max %.1f%%, overruns %lu, fifo errors %lu, "
                  "words checked %lu, bit errors in %lu words\n",
                  label, static_cast<double>(100.0f * i2s32::load(s.lastCycles)),
                  static_cast<double>(100.0f * i2s32::load(s.maxCycles)),
                  static_cast<unsigned long>(s.overruns), static_cast<unsigned long>(s.fifoErrors),
                  static_cast<unsigned long>(g_checked), static_cast<unsigned long>(g_errors));
}

// ---- Steps -------------------------------------------------------------------------
static bool stepSampleRate() {
    Serial.println("1. Sample rate");
    if (!waitForBlocks(2, 1000)) {
        report(false, "no audio interrupts at all (driver did not start)");
        return false;
    }
    // Align to a block edge, then time a known number of blocks.
    uint32_t b0 = i2s32::stats().blocks;
    while (i2s32::stats().blocks == b0) {}
    const uint32_t tStart = micros();
    b0 += 1;
    const uint32_t nBlocks = 5 * i2s32::kSampleRate / kBlock;
    while (static_cast<int32_t>(i2s32::stats().blocks - (b0 + nBlocks)) < 0) {}
    const uint32_t tEnd = micros();

    const double rate = static_cast<double>(nBlocks) * kBlock * 1.0e6 / static_cast<double>(tEnd - tStart);
    const double ppm = (rate / i2s32::kSampleRate - 1.0) * 1.0e6;
    Serial.printf("    measured %.3f Hz (%+.1f ppm vs 48000), block = %u frames = %.3f ms\n",
                  rate, ppm, static_cast<unsigned>(kBlock), 1000.0 * kBlock / i2s32::kSampleRate);
    const bool ok = fabs(ppm) < 50.0;
    report(ok, "sample rate is 48 kHz within 50 ppm");
    return ok;
}

// Returns the delay in words, or -1 when there is no loopback.
static int32_t stepLoopback() {
    Serial.println("2. Bit-exact loopback (jumper pin 7 -> pin 8)");
    const int32_t d = findDelayWords();
    if (d < 0) {
        Serial.println("    no loopback detected: the transmitted pattern is not arriving on pin 8.");
        Serial.println("    Fit a jumper from pin 7 to pin 8 and press any key to run again.");
        Serial.println("  [SKIP] data steps 2-5");
        return -1;
    }
    startVerify(static_cast<uint32_t>(d));
    delay(5000);
    const i2s32::Stats s = i2s32::stats();
    const uint32_t frames = static_cast<uint32_t>(d) / 2;
    Serial.printf("    round-trip delay: %ld words = %lu frames = %.3f ms  (2 x block = %u frames, extra = %ld)\n",
                  static_cast<long>(d), static_cast<unsigned long>(frames),
                  1000.0 * frames / i2s32::kSampleRate, static_cast<unsigned>(2 * kBlock),
                  static_cast<long>(frames) - static_cast<long>(2 * kBlock));
    Serial.printf("    TX DMA leads RX DMA by %ld frames\n", static_cast<long>(s.txLeadFrames));
    printStats("5 s");
    report((d & 1) == 0, "left/right channels stay aligned (even word delay)");
    report(g_checked > 0 && g_errors == 0, "all 32 bits of every word arrive unchanged");
    report(frames >= kBlock && frames <= 2 * kBlock + 8, "delay is at most 2 blocks plus FIFO");
    report(s.fifoErrors == 0, "no SAI FIFO errors");
    return d;
}

static void stepLoad(float fraction) {
    Serial.printf("3. Load: callback uses %.0f%% of the block period for 10 s\n",
                  static_cast<double>(100.0f * fraction));
    g_burnCycles = static_cast<uint32_t>(fraction * static_cast<float>(i2s32::blockPeriodCycles()));
    delay(50);
    i2s32::resetStats();
    g_errors = 0;
    g_checked = 0;
    delay(10000);
    const i2s32::Stats s = i2s32::stats();
    printStats("10 s");
    g_burnCycles = 0;
    report(s.overruns == 0, "no overruns");
    report(s.fifoErrors == 0, "no SAI FIFO errors");
    report(g_checked > 0 && g_errors == 0, "data still bit-exact");
}

static void stepOverrun(int32_t delayWords) {
    Serial.println("4. Overrun detection: 50 callbacks that take 1.5 block periods");
    i2s32::resetStats();
    g_slowCycles = i2s32::blockPeriodCycles() + i2s32::blockPeriodCycles() / 2;
    g_slowBlocks = 50;
    delay(500);
    const i2s32::Stats during = i2s32::stats();
    Serial.printf("    overruns counted: %lu, max load %.0f%%\n", static_cast<unsigned long>(during.overruns),
                  static_cast<double>(100.0f * i2s32::load(during.maxCycles)));
    report(during.overruns >= 40, "slow callbacks are counted as overruns");

    // After the overload the stream must be clean again without restarting anything.
    i2s32::resetStats();
    const int32_t after = findDelayWords();
    if (after >= 0) startVerify(static_cast<uint32_t>(after));
    delay(2000);
    const i2s32::Stats s = i2s32::stats();
    printStats("2 s after");
    report(after == delayWords, "delay is unchanged after the overload");
    report(after >= 0 && g_checked > 0 && g_errors == 0, "data is bit-exact again");
    report(s.overruns == 0 && s.fifoErrors == 0, "no overruns or FIFO errors after recovery");
}

static void stepSoak() {
    Serial.println("5. Soak: bit-exact check until a key is pressed (stats every 5 s)");
    i2s32::resetStats();
    g_errors = 0;
    g_checked = 0;
    uint32_t seconds = 0;
    elapsedMillis tick;
    while (!Serial.available()) {
        if (tick >= 5000) {
            tick = 0;
            seconds += 5;
            char label[24];
            snprintf(label, sizeof(label), "%lu s", static_cast<unsigned long>(seconds));
            printStats(label);
        }
    }
    while (Serial.available()) Serial.read();
    const i2s32::Stats s = i2s32::stats();
    report(g_errors == 0 && s.overruns == 0 && s.fifoErrors == 0, "soak finished with no errors");
}

static void runAll() {
    g_failures = 0;
    g_verify = false;
    g_burnCycles = 0;
    Serial.printf("\n=== I2S loopback test: block %u frames, CPU %lu MHz, block period %lu cycles ===\n",
                  static_cast<unsigned>(kBlock), static_cast<unsigned long>(F_CPU_ACTUAL / 1000000),
                  static_cast<unsigned long>(i2s32::blockPeriodCycles()));
    if (stepSampleRate()) {
        const int32_t d = stepLoopback();
        if (d >= 0) {
            stepLoad(0.5f);
            stepLoad(0.8f);
            stepOverrun(d);
            stepSoak();
        }
    }
    Serial.printf("=== %s (%d failed). Press any key to run again. ===\n",
                  g_failures ? "FAILED" : "DONE", g_failures);
}

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 4000) {}

    i2s32::Config cfg;
    cfg.callback = audioCallback;
    i2s32::begin(cfg);
    delay(100);
    runAll();
}

void loop() {
    if (Serial.available()) {
        while (Serial.available()) Serial.read();
        runAll();
    }
}
