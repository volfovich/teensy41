// Host unit tests for dsp/fx/Modulation.h (the Mod block: chorus and flanger).
// Run with: pio test -e native
#include <unity.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <dsp/fx/Modulation.h>

using dsp::Modulation;

static constexpr float kFs = 48000.0f;
static constexpr int kB = 32;
static constexpr float kPi2 = 6.2831853f;

// A Modulation with its two lines.
struct Rig {
    float lineL[Modulation::kLen], lineR[Modulation::kLen];
    Modulation mod{kFs};
    Rig() { mod.init(lineL, lineR); }
};

// Runs `in` (mono, on both sides) through the effect in blocks of `block`; an idle block is the input.
static void run(Modulation& m, const std::vector<float>& in, std::vector<float>& l, std::vector<float>& r, int block = kB) {
    l.assign(in.size(), 0.0f);
    r.assign(in.size(), 0.0f);
    for (size_t at = 0; at + block <= in.size(); at += block) {
        if (!m.process(&in[at], &in[at], &l[at], &r[at], block)) {
            memcpy(&l[at], &in[at], sizeof(float) * block);
            memcpy(&r[at], &in[at], sizeof(float) * block);
        }
    }
}

static std::vector<float> sine(float hz, float amp, int n) {
    std::vector<float> v(n);
    for (int i = 0; i < n; i++) v[i] = amp * sinf(kPi2 * hz * (float)i / kFs);
    return v;
}

static float peak(const std::vector<float>& v, int from, int to) {
    float p = 0.0f;
    for (int i = from; i < to; i++) p = fmaxf(p, fabsf(v[i]));
    return p;
}

void setUp() {}
void tearDown() {}

// Off, or without its memory, the effect is idle: nothing is written.
static void test_idle_writes_nothing() {
    Modulation bare(kFs);
    float in[kB] = {}, l[kB], r[kB];
    for (float& v : l) v = 7.0f;
    bare.setOn(true);
    TEST_ASSERT_FALSE(bare.ready());
    TEST_ASSERT_FALSE(bare.process(in, in, l, r, kB));
    TEST_ASSERT_EQUAL_FLOAT(7.0f, l[0]);

    Rig rig;
    TEST_ASSERT_TRUE(rig.mod.ready());
    TEST_ASSERT_FALSE(rig.mod.process(in, in, l, r, kB));
    TEST_ASSERT_EQUAL_FLOAT(7.0f, l[0]);
}

// Switching on starts from the input and switching off ends on it, without a jump, and
// then the effect is idle again.
static void test_on_and_off_ramp_from_and_to_the_input() {
    Rig rig;
    rig.mod.set(Modulation::kChorus, 0.8f, 1.0f, 0.0f, 0.5f, 0.5f);
    const std::vector<float> in = sine(220.0f, 0.5f, 48000);
    float l[kB], r[kB];
    rig.mod.setOn(true);
    TEST_ASSERT_TRUE(rig.mod.process(&in[0], &in[0], l, r, kB));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, in[0], l[0]);  // the first sample is all but the input
    int at = kB;
    for (; at < 4800; at += kB) rig.mod.process(&in[at], &in[at], l, r, kB);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, rig.mod.fade());

    rig.mod.setOn(false);
    int blocks = 0;
    float lastL = 0.0f, lastIn = 0.0f;
    while (rig.mod.process(&in[at], &in[at], l, r, kB)) {
        lastL = l[kB - 1];
        lastIn = in[at + kB - 1];
        at += kB;
        blocks++;
        TEST_ASSERT_TRUE_MESSAGE(blocks < 100, "the effect never went idle");
    }
    TEST_ASSERT_INT_WITHIN(2, 30, blocks);  // 20 ms = 960 samples = 30 blocks
    TEST_ASSERT_EQUAL_FLOAT(lastIn, lastL);  // faded out: exactly the input
    TEST_ASSERT_EQUAL_FLOAT(0.0f, rig.mod.fade());
}

// Mix 0 is the input, bit for bit, whatever the other settings.
static void test_mix_zero_is_the_input() {
    Rig rig;
    rig.mod.set(Modulation::kFlanger, 2.0f, 1.0f, 1.0f, 0.0f, 1.0f);
    rig.mod.setOn(true);
    const std::vector<float> in = sine(330.0f, 0.7f, 9600);
    std::vector<float> l, r;
    run(rig.mod, in, l, r);
    for (size_t i = 0; i < in.size(); i++) {
        TEST_ASSERT_EQUAL_FLOAT(in[i], l[i]);
        TEST_ASSERT_EQUAL_FLOAT(in[i], r[i]);
    }
}

// The flanger with no depth is a fixed comb at 0.3 ms (14.4 samples): at mix 50 % a sine
// whose half period is that delay cancels, one whose period is that delay adds up.
static void test_flanger_without_depth_is_a_comb() {
    const float d = Modulation::kFlangerMinMs * kFs / 1000.0f;
    {
        Rig rig;
        rig.mod.set(Modulation::kFlanger, 0.5f, 0.0f, 0.0f, 0.5f, 0.0f);
        rig.mod.setOn(true);
        std::vector<float> l, r;
        run(rig.mod, sine(kFs / (2.0f * d), 1.0f, 9600), l, r);
        TEST_ASSERT_TRUE_MESSAGE(peak(l, 4800, 9600) < 0.03f, "the notch is not deep");
    }
    {
        Rig rig;
        rig.mod.set(Modulation::kFlanger, 0.5f, 0.0f, 0.0f, 0.5f, 0.0f);
        rig.mod.setOn(true);
        std::vector<float> l, r;
        run(rig.mod, sine(kFs / d, 1.0f, 9600), l, r);
        TEST_ASSERT_FLOAT_WITHIN(0.04f, 1.4142f, peak(l, 4800, 9600));
    }
}

// The delay stays inside the line and above the interpolator's minimum for every setting.
static void test_delay_range_fits_the_line() {
    Rig rig;
    for (int type : {Modulation::kChorus, Modulation::kFlanger})
        for (float rate : {0.01f, 0.05f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 50.0f})
            for (float depth : {-1.0f, 0.0f, 0.3f, 1.0f, 2.0f}) {
                rig.mod.set(type, rate, depth, 0.0f, 0.5f, 0.5f);
                TEST_ASSERT_TRUE(rig.mod.delayMin() >= 4.0f);
                TEST_ASSERT_TRUE(rig.mod.delayMax() <= (float)(Modulation::kLen - 1));
                TEST_ASSERT_TRUE(rig.mod.delayMin() <= rig.mod.delayMax());
            }
    rig.mod.set(Modulation::kFlanger, 0.5f, 1.0f, 0.0f, 0.5f, 0.5f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.3f * 48.0f, rig.mod.delayMin());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 7.0f * 48.0f, rig.mod.delayMax());
    rig.mod.set(Modulation::kChorus, 0.5f, 1.0f, 0.0f, 0.5f, 0.5f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 7.0f * 48.0f, rig.mod.delayMin());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 17.0f * 48.0f, rig.mod.delayMax());
    // Above the knee the sweep shrinks with the rate: the same pitch wobble.
    rig.mod.set(Modulation::kChorus, 4.0f, 1.0f, 0.0f, 0.5f, 0.5f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, (12.0f + 1.25f) * 48.0f, rig.mod.delayMax());
}

// The delay really moves over its range, at the rate set. A ramp in (wet only) gives the
// delay back: wet[n] = slope * (n - delay[n]).
static void test_delay_sweeps_its_range_at_the_rate() {
    Rig rig;
    const float rate = 2.0f;
    rig.mod.set(Modulation::kFlanger, rate, 1.0f, 0.0f, 1.0f, 0.0f);
    rig.mod.setOn(true);
    const int n = 10 * 48000;
    const float slope = 1.0f / 1024.0f;
    std::vector<float> in(n), l, r;
    for (int i = 0; i < n; i++) in[i] = slope * (float)i;
    run(rig.mod, in, l, r);
    const float mid = 0.5f * (rig.mod.delayMin() + rig.mod.delayMax());
    float lo = 1e9f, hi = -1e9f, prev = 0.0f;
    int crossings = 0;
    for (int i = 4800; i < n; i++) {  // after the fade-in and the first pass of the line
        const float delay = (float)i - l[i] / slope;
        lo = fminf(lo, delay);
        hi = fmaxf(hi, delay);
        if (i > 4800 && prev < mid && delay >= mid) crossings++;
        prev = delay;
    }
    TEST_ASSERT_FLOAT_WITHIN(0.5f, rig.mod.delayMin(), lo);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, rig.mod.delayMax(), hi);
    TEST_ASSERT_INT_WITHIN(1, (int)(rate * 9.9f), crossings);  // 9.9 s at 2 Hz
}

// Full feedback, full depth and a full-scale input stay bounded, and ring out afterwards.
static void test_feedback_is_stable() {
    for (int type : {Modulation::kChorus, Modulation::kFlanger}) {
        Rig rig;
        rig.mod.set(type, 0.3f, 1.0f, 1.0f, 0.5f, 1.0f);
        rig.mod.setOn(true);
        const int n = 14 * 48000;  // 10 s of signal, then silence
        std::vector<float> in(n, 0.0f), l, r;
        uint32_t seed = 12345;
        for (int i = 0; i < 10 * 48000; i++) {
            seed = seed * 1664525u + 1013904223u;
            in[i] = i % 4800 < 2400 ? (float)(int32_t)seed / 2147483648.0f : (i & 64 ? 1.0f : -1.0f);  // noise, then a square
        }
        run(rig.mod, in, l, r);
        float p = 0.0f;
        for (int i = 0; i < n; i++) {
            TEST_ASSERT_TRUE(std::isfinite(l[i]) && std::isfinite(r[i]));
            p = fmaxf(p, fmaxf(fabsf(l[i]), fabsf(r[i])));
        }
        TEST_ASSERT_TRUE_MESSAGE(p < 15.0f, "the feedback builds up");
        // 0.9 per pass of at most 17 ms: after 3 s it is 160 passes down.
        TEST_ASSERT_TRUE_MESSAGE(peak(l, 13 * 48000, n) < 1e-4f, "the feedback does not ring out");
    }
}

// Changing any setting, the type, or on/off while a note plays makes no click: no step
// between two samples far beyond the signal's own.
static void test_changes_do_not_click() {
    Rig rig;
    rig.mod.set(Modulation::kChorus, 0.8f, 0.5f, 0.0f, 0.5f, 0.5f);
    const std::vector<float> in = sine(220.0f, 0.5f, 6 * 48000);
    std::vector<float> l(in.size()), r(in.size());
    int change = 0;
    for (size_t at = 0; at + kB <= in.size(); at += kB) {
        if (at % 9600 == 0) {
            switch (change++ % 10) {
                case 0: rig.mod.setOn(true); break;
                case 1: rig.mod.set(Modulation::kChorus, 5.0f, 1.0f, 0.0f, 0.5f, 0.5f); break;
                case 2: rig.mod.set(Modulation::kChorus, 0.1f, 0.0f, 0.8f, 1.0f, 1.0f); break;
                case 3: rig.mod.set(Modulation::kFlanger, 0.5f, 1.0f, 1.0f, 0.5f, 0.0f); break;
                case 4: rig.mod.set(Modulation::kFlanger, 10.0f, 0.2f, 0.0f, 0.1f, 1.0f); break;
                case 5: rig.mod.set(Modulation::kChorus, 0.8f, 0.5f, 0.3f, 0.5f, 0.5f); break;
                case 6: rig.mod.setOn(false); break;
                case 7: rig.mod.setOn(true); break;
                case 8: rig.mod.set(Modulation::kFlanger, 0.3f, 1.0f, 0.5f, 0.5f, 0.5f); break;
                case 9: rig.mod.setOn(false); break;
            }
        }
        if (!rig.mod.process(&in[at], &in[at], &l[at], &r[at], kB)) {
            memcpy(&l[at], &in[at], sizeof(float) * kB);
            memcpy(&r[at], &in[at], sizeof(float) * kB);
        }
    }
    // The input's largest step is 0.5 * 2 pi * 220 / 48000 = 0.0144.
    float worst = 0.0f;
    size_t worstAt = 0;
    for (size_t i = 1; i < in.size(); i++) {
        const float step = fmaxf(fabsf(l[i] - l[i - 1]), fabsf(r[i] - r[i - 1]));
        if (step > worst) worst = step, worstAt = i;
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "largest step %.4f at sample %u (change %u + %u)", worst, (unsigned)worstAt,
             (unsigned)(worstAt / 9600), (unsigned)(worstAt % 9600));
    TEST_ASSERT_TRUE_MESSAGE(worst < 0.06f, msg);
}

// Width 0 keeps a mono input mono; with width the two sides differ.
static void test_width() {
    const std::vector<float> in = sine(440.0f, 0.5f, 48000);
    std::vector<float> l, r;
    {
        Rig rig;
        rig.mod.set(Modulation::kChorus, 1.0f, 1.0f, 0.4f, 0.5f, 0.0f);
        rig.mod.setOn(true);
        run(rig.mod, in, l, r);
        for (size_t i = 0; i < in.size(); i++) TEST_ASSERT_EQUAL_FLOAT(l[i], r[i]);
    }
    {
        Rig rig;
        rig.mod.set(Modulation::kChorus, 1.0f, 1.0f, 0.4f, 0.5f, 1.0f);
        rig.mod.setOn(true);
        run(rig.mod, in, l, r);
        float diff = 0.0f;
        for (size_t i = 0; i < in.size(); i++) diff = fmaxf(diff, fabsf(l[i] - r[i]));
        TEST_ASSERT_TRUE(diff > 0.1f);
    }
}

// The effect is as loud as its input, whatever the settings: the RMS of a guitar-like
// signal (noise between 80 Hz and 2.5 kHz) stays within 0.5 dB.
static void test_level_is_compensated() {
    const int n = 21 * 48000;
    std::vector<float> in(n);
    dsp::Biquad lp, hp;
    lp.setLowpass(2500.0f, kFs);
    hp.setHighpass(80.0f, kFs);
    uint32_t seed = 777;
    for (int i = 0; i < n; i++) {
        seed = seed * 1664525u + 1013904223u;
        in[i] = 0.2f * hp.process(lp.process((float)(int32_t)seed / 2147483648.0f));
    }
    auto rms = [&](const std::vector<float>& v) {
        double sum = 0.0;
        for (int i = 48000; i < n; i++) sum += (double)v[i] * v[i];
        return sqrt(sum / (n - 48000));
    };
    const double ref = rms(in);
    float worst = 0.0f, worstRaw = 0.0f;
    char where[96] = "";
    std::vector<float> l, r;
    for (int type : {Modulation::kChorus, Modulation::kFlanger})
        for (float rate : {0.3f, 2.0f})
            for (float depth : {0.3f, 1.0f})
                for (float fb : {0.0f, 0.5f, 1.0f})
                    for (float mix : {0.25f, 0.5f, 0.75f, 1.0f}) {
                        Rig rig;
                        rig.mod.set(type, rate, depth, fb, mix, 0.5f);
                        rig.mod.setOn(true);
                        run(rig.mod, in, l, r);
                        const float db = 20.0f * log10f((float)(rms(l) / ref));
                        const float raw = db - 20.0f * log10f(rig.mod.makeup());  // without the compensation
                        if (fabsf(raw) > fabsf(worstRaw)) worstRaw = raw;
                        if (getenv("MOD_LEVELS")) printf("LV type %d rate %.1f depth %.1f fb %.1f mix %.2f: %+.2f dB (raw %+.2f)\n", type, rate, depth, fb, mix, db, raw);
                        if (fabsf(db) > fabsf(worst)) {
                            worst = db;
                            snprintf(where, sizeof(where), "%+.2f dB at type %d rate %.1f depth %.1f fb %.1f mix %.2f", db, type,
                                     rate, depth, fb, mix);
                        }
                    }
    printf("level: worst %s; without the compensation the worst would be %+.2f dB\n", where, worstRaw);
    TEST_ASSERT_TRUE_MESSAGE(fabsf(worst) < 0.5f, where);
}

// The block size does not change the result.
static void test_block_size_does_not_matter() {
    const std::vector<float> in = sine(196.0f, 0.6f, 19200);
    std::vector<float> ref, refR;
    for (int block : {32, 16, 64}) {
        Rig rig;
        rig.mod.set(Modulation::kFlanger, 0.7f, 0.8f, 0.6f, 0.5f, 0.5f);
        rig.mod.setOn(true);
        std::vector<float> l, r;
        run(rig.mod, in, l, r, block);
        if (ref.empty()) {
            ref = l;
            refR = r;
            continue;
        }
        for (size_t i = 0; i < in.size(); i++) {
            TEST_ASSERT_EQUAL_FLOAT(ref[i], l[i]);
            TEST_ASSERT_EQUAL_FLOAT(refR[i], r[i]);
        }
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_idle_writes_nothing);
    RUN_TEST(test_on_and_off_ramp_from_and_to_the_input);
    RUN_TEST(test_mix_zero_is_the_input);
    RUN_TEST(test_flanger_without_depth_is_a_comb);
    RUN_TEST(test_delay_range_fits_the_line);
    RUN_TEST(test_delay_sweeps_its_range_at_the_rate);
    RUN_TEST(test_feedback_is_stable);
    RUN_TEST(test_changes_do_not_click);
    RUN_TEST(test_width);
    RUN_TEST(test_level_is_compensated);
    RUN_TEST(test_block_size_does_not_matter);
    return UNITY_END();
}
