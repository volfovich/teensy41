// Own NAM A2-Lite engine (dsp/nam/NamA2.h) on the bare Teensy 4.1: correctness and CPU cost.
// No audio I/O.
//
//   pio run -e nam_a2_bench -t upload && pio device monitor -e nam_a2_bench
//
// 1. Reads the example A2 model (the .nam JSON file, embedded in flash) with the streaming
//    reader the firmware will use for SD files, and times it.
// 2. Runs the golden test signal (embedded, bit-identical to the PC's) through the engine on the board and compares the output with
//    NeuralAmpModelerCore's reference output (also embedded): same check as the host test.
// 3. Measures CPU cycles per block at block sizes 16, 32 and 64, with the engine's history in
//    DTCM, and at 32 with the history in OCRAM for comparison.
//
// Compare with env:nam_bench, which measures the reference engine on the same model.

#include <Arduino.h>

#include <dsp/ByteSource.h>
#include <dsp/nam/GoldenSignal.h>
#include <dsp/nam/NamA2.h>
#include <dsp/nam/NamFile.h>

namespace nama2 = dsp::nama2;

// The model file and the reference output, straight from test/fixtures into flash.
__asm__(".section .progmem.namfixtures,\"a\"\n"
        ".balign 4\n"
        ".global g_a2NamStart\ng_a2NamStart:\n"
        ".incbin \"test/fixtures/nam/A2.nam\"\n"
        ".global g_a2NamEnd\ng_a2NamEnd:\n"
        ".balign 4\n"
        ".global g_refStart\ng_refStart:\n"
        ".incbin \"test/fixtures/nam/A2_lite_ref.f32\"\n"
        ".global g_refEnd\ng_refEnd:\n"
        ".balign 4\n"
        ".global g_inputStart\ng_inputStart:\n"
        ".incbin \"test/fixtures/nam/golden_input.f32\"\n"
        ".previous\n");
extern "C" const uint8_t g_a2NamStart[], g_a2NamEnd[];
extern "C" const float g_refStart[], g_refEnd[], g_inputStart[];

static constexpr float kBudgetCyclesPerSample = 600000000.0f / 48000.0f;

static float g_weights[nama2::kWeightCount];
static dsp::NamInfo g_info;

// One engine per block size. Objects and DTCM histories are ordinary globals (RAM1).
static dsp::NamA2<16> g_nam16;
static dsp::NamA2<32> g_nam32;
static dsp::NamA2<64> g_nam64;
static dsp::NamA2<32> g_nam32Ocram;
static float g_hist16[dsp::NamA2<16>::kHistoryFloats];
static float g_hist32[dsp::NamA2<32>::kHistoryFloats];
static float g_hist64[dsp::NamA2<64>::kHistoryFloats];
DMAMEM static float g_hist32Ocram[dsp::NamA2<32>::kHistoryFloats];

// The board's output for the golden signal (OCRAM; not part of the timed path).
DMAMEM static float g_output[nama2::kGoldenFrames];

static uint32_t g_rng = 12345;
static float noise() {   // -0.25 .. 0.25
    g_rng = g_rng * 1664525u + 1013904223u;
    return (static_cast<float>(g_rng >> 8) / 8388608.0f - 1.0f) * 0.25f;
}

static bool loadModel() {
    const uint32_t size = static_cast<uint32_t>(g_a2NamEnd - g_a2NamStart);
    dsp::MemorySource src(g_a2NamStart, size);
    const char* err = "";
    const uint32_t t0 = micros();
    const bool ok = dsp::readNamA2Lite(src, g_weights, &g_info, &err);
    const uint32_t ms = (micros() - t0) / 1000;
    if (!ok) {
        Serial.printf("  [FAIL] reading the .nam file: %s\n", err);
        return false;
    }
    Serial.printf("  [PASS] read A2-Lite from the %lu-byte .nam file in %lu ms: %d weights, %.0f Hz, input level %.2f dBu\n",
                  static_cast<unsigned long>(size), static_cast<unsigned long>(ms), nama2::kWeightCount,
                  static_cast<double>(g_info.sampleRate), static_cast<double>(g_info.inputLevelDbu));
    return true;
}

static void golden() {
    // The input comes from the fixture file, not from goldenSignal(): the board's sinf/expf
    // differ from the PC's in the last bit, and the comparison needs the exact same input.
    g_nam32.reset();
    for (int i = 0; i < nama2::kGoldenFrames; i += 32) g_nam32.process(&g_inputStart[i], &g_output[i]);
    double err2 = 0.0, sig2 = 0.0;
    float maxErr = 0.0f, peak = 0.0f;
    for (int i = nama2::kGoldenCompareFrom; i < nama2::kGoldenFrames; ++i) {
        const float d = g_output[i] - g_refStart[i];
        err2 += static_cast<double>(d) * static_cast<double>(d);
        sig2 += static_cast<double>(g_refStart[i]) * static_cast<double>(g_refStart[i]);
        maxErr = max(maxErr, fabsf(d));
        peak = max(peak, fabsf(g_refStart[i]));
    }
    const double db = 10.0 * log10(sig2 / (err2 + 1e-30));
    const bool pass = peak > 0.05f && maxErr < 1e-5f && db > 100.0;
    Serial.printf("  [%s] output matches NeuralAmpModelerCore: max error %.2e, peak %.3f, error %.0f dB below the signal\n",
                  pass ? "PASS" : "FAIL", static_cast<double>(maxErr), static_cast<double>(peak), db);
}

template <int B>
static void bench(const char* where, dsp::NamA2<B>& nam) {
    const int blocks = 3 * 48000 / B;   // 3 s of audio
    float in[B], out[B];
    uint64_t total = 0;
    uint32_t lo = UINT32_MAX, hi = 0;
    nam.reset();
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < B; ++i) in[i] = noise();
        const uint32_t t0 = ARM_DWT_CYCCNT;
        nam.process(in, out);
        const uint32_t c = ARM_DWT_CYCCNT - t0;
        total += c;
        if (c < lo) lo = c;
        if (c > hi) hi = c;
    }
    const float avg = static_cast<float>(total) / static_cast<float>(blocks) / static_cast<float>(B);
    Serial.printf("  block %3d  history %-5s (%3d KB)  cycles/sample avg %5.0f  min %5.0f  max %5.0f   CPU avg %5.1f%%  max %5.1f%%   %.2f cycles/MAC\n",
                  B, where, static_cast<int>(dsp::NamA2<B>::kHistoryFloats * sizeof(float) / 1024),
                  static_cast<double>(avg), static_cast<double>(static_cast<float>(lo) / B),
                  static_cast<double>(static_cast<float>(hi) / B),
                  static_cast<double>(100.0f * avg / kBudgetCyclesPerSample),
                  static_cast<double>(100.0f * static_cast<float>(hi) / B / kBudgetCyclesPerSample),
                  static_cast<double>(avg / nama2::kMacsPerSample));
}

static void runAll() {
    Serial.printf("\n=== Own NAM A2-Lite engine: CPU %lu MHz, budget %.0f cycles/sample at 48 kHz, %d MAC/sample ===\n",
                  static_cast<unsigned long>(F_CPU_ACTUAL / 1000000), static_cast<double>(kBudgetCyclesPerSample),
                  nama2::kMacsPerSample);
    if (!loadModel()) return;
    g_nam16.setWeights(g_weights);
    g_nam32.setWeights(g_weights);
    g_nam64.setWeights(g_weights);
    g_nam32Ocram.setWeights(g_weights);
    golden();
    bench<16>("DTCM", g_nam16);
    bench<32>("DTCM", g_nam32);
    bench<64>("DTCM", g_nam64);
    bench<32>("OCRAM", g_nam32Ocram);
    Serial.println("=== done. Press any key to run again. ===");
}

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 4000) {}

    // Flush-to-zero + default-NaN, as the audio driver sets them.
    uint32_t fpscr;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1u << 24) | (1u << 25);
    __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr));

    g_nam16.begin(g_hist16);
    g_nam32.begin(g_hist32);
    g_nam64.begin(g_hist64);
    g_nam32Ocram.begin(g_hist32Ocram);
    runAll();
}

void loop() {
    if (Serial.available()) {
        while (Serial.available()) Serial.read();
        runAll();
    }
}
