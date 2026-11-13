// NAM A2-Lite CPU benchmark on the bare Teensy 4.1 (HLD milestone M1). No audio I/O.
//
//   pio run -e nam_bench -t upload && pio device monitor -e nam_bench
//
// Runs the reference engine (NeuralAmpModelerCore, A2 fast path) on the example A2-Lite model
// at several block sizes and reports CPU cycles per sample against the 48 kHz budget
// (600 MHz / 48000 = 12500 cycles per sample).
//
// The engine allocates its state with new/malloc, which on the Teensy 4.1 is RAM2 (OCRAM,
// cached). The code runs from ITCM. Interrupts stay enabled, so "max" includes USB/systick.

// The NAM / Eigen headers must come before Arduino.h: Arduino defines macros such as F()
// that collide with template parameter names inside Eigen.
#include <malloc.h>
#include <memory>
#include <vector>

#include "NAM/dsp.h"
#include "NAM/wavenet/a2_fast.h"
#include "a2_lite_model.h"

#include <Arduino.h>

static constexpr int kMaxBlock = 128;
static constexpr float kCyclesPerSampleBudget = 600000000.0f / 48000.0f;

static std::unique_ptr<nam::DSP> g_model;
static float g_in[kMaxBlock];
static float g_out[kMaxBlock];

static uint32_t g_rng = 0x12345678u;
static float noise() {                       // white noise, about -12 dBFS peak
    g_rng = g_rng * 1664525u + 1013904223u;
    return (static_cast<float>(static_cast<int32_t>(g_rng)) * (1.0f / 2147483648.0f)) * 0.25f;
}

static size_t heapUsed() { return mallinfo().uordblks; }

static bool buildModel() {
    const size_t heap0 = heapUsed();
    const uint32_t t0 = micros();
    try {
        const nlohmann::json config = nlohmann::json::parse(kA2LiteConfigJson);
        int channels = 0;
        if (!nam::wavenet::a2_fast::is_a2_shape(config, &channels)) {
            Serial.println("FAIL: example model is not recognised as A2 by the fast path");
            return false;
        }
        Serial.printf("model: A2 fast path, %d channels (A2-Lite), %u weights (%u bytes)\n", channels,
                      static_cast<unsigned>(kA2LiteWeightCount), static_cast<unsigned>(sizeof(kA2LiteWeights)));
        auto cfg = nam::wavenet::a2_fast::create_a2_fast_config(config, kA2LiteSampleRate);
        std::vector<float> weights(kA2LiteWeights, kA2LiteWeights + kA2LiteWeightCount);
        g_model = cfg->create(std::move(weights), kA2LiteSampleRate);
        g_model->Reset(kA2LiteSampleRate, kMaxBlock);
    } catch (const std::exception& e) {
        Serial.printf("FAIL: exception while building the model: %s\n", e.what());
        return false;
    }
    const uint32_t t1 = micros();
    Serial.printf("build + reset: %lu ms, heap used by model: %u bytes\n",
                  static_cast<unsigned long>((t1 - t0) / 1000), static_cast<unsigned>(heapUsed() - heap0));

    const uint32_t t2 = micros();
    g_model->prewarm();
    Serial.printf("prewarm (%d samples): %lu ms\n", g_model->GetPrewarmSamples(),
                  static_cast<unsigned long>((micros() - t2) / 1000));
    return true;
}

static void bench(int block, const char* label, bool silence) {
    float* in[1] = {g_in};
    float* out[1] = {g_out};
    const int blocks = 3 * 48000 / block;          // 3 seconds of audio
    uint32_t minC = UINT32_MAX, maxC = 0;
    uint64_t sum = 0;
    float peak = 0.0f;
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < block; ++i) g_in[i] = silence ? 0.0f : noise();
        const uint32_t t0 = ARM_DWT_CYCCNT;
        g_model->process(in, out, block);
        const uint32_t c = ARM_DWT_CYCCNT - t0;
        sum += c;
        if (c < minC) minC = c;
        if (c > maxC) maxC = c;
        for (int i = 0; i < block; ++i) peak = max(peak, fabsf(g_out[i]));
    }
    const float avgPerSample = static_cast<float>(sum) / static_cast<float>(blocks) / static_cast<float>(block);
    const float minPerSample = static_cast<float>(minC) / static_cast<float>(block);
    const float maxPerSample = static_cast<float>(maxC) / static_cast<float>(block);
    Serial.printf("  block %3d %-8s cycles/sample avg %6.0f  min %6.0f  max %6.0f   CPU avg %5.1f%%  max %5.1f%%   out peak %.4f\n",
                  block, label, static_cast<double>(avgPerSample), static_cast<double>(minPerSample),
                  static_cast<double>(maxPerSample),
                  static_cast<double>(100.0f * avgPerSample / kCyclesPerSampleBudget),
                  static_cast<double>(100.0f * maxPerSample / kCyclesPerSampleBudget),
                  static_cast<double>(peak));
}

static void runAll() {
    Serial.printf("\n=== NAM A2-Lite benchmark: CPU %lu MHz, budget %.0f cycles/sample at 48 kHz ===\n",
                  static_cast<unsigned long>(F_CPU_ACTUAL / 1000000), static_cast<double>(kCyclesPerSampleBudget));
    if (!g_model && !buildModel()) return;
    for (int block : {16, 32, 48, 64, 128}) bench(block, "noise", false);
    bench(32, "silence", true);
    Serial.println("=== done. Press any key to run again. ===");
}

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 4000) {}

    // Flush-to-zero + default-NaN, as the audio driver sets them (denormals are very slow).
    uint32_t fpscr;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1u << 24) | (1u << 25);
    __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr));

    runAll();
}

void loop() {
    if (Serial.available()) {
        while (Serial.available()) Serial.read();
        runAll();
    }
}
