// Host unit tests for dsp/fx/Modulation.h (the Mod block: chorus, flanger, ce2, phase90).
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
    for (int type : {Modulation::kFlanger, Modulation::kCe2, Modulation::kPhaser}) {
        Rig rig;
        rig.mod.set(type, 2.0f, 1.0f, 1.0f, 0.0f, 1.0f);
        rig.mod.setOn(true);
        const std::vector<float> in = sine(330.0f, 0.7f, 9600);
        std::vector<float> l, r;
        run(rig.mod, in, l, r);
        for (size_t i = 0; i < in.size(); i++) {
            TEST_ASSERT_EQUAL_FLOAT(in[i], l[i]);
            TEST_ASSERT_EQUAL_FLOAT(in[i], r[i]);
        }
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
    for (int type : {Modulation::kChorus, Modulation::kFlanger, Modulation::kCe2})
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
    // The ce2: 4.4 to 6.8 ms at full depth, at any rate (the pedal does not narrow it).
    for (float rate : {0.5f, 4.0f}) {
        rig.mod.set(Modulation::kCe2, rate, 1.0f, 0.0f, 0.5f, 0.5f);
        TEST_ASSERT_FLOAT_WITHIN(0.01f, 4.4f * 48.0f, rig.mod.delayMin());
        TEST_ASSERT_FLOAT_WITHIN(0.01f, 6.8f * 48.0f, rig.mod.delayMax());
    }
    // An unknown type is the chorus.
    rig.mod.set(99, 0.5f, 1.0f, 0.0f, 0.5f, 0.5f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 17.0f * 48.0f, rig.mod.delayMax());
}

// The ce2's delay moves as a triangle over its range: the same speed up and down, and a
// ramp in (wet only) gives the delay back, later by the low-pass's own (constant) delay.
static void test_ce2_sweeps_as_a_triangle() {
    Rig rig;
    const float rate = 2.0f;
    rig.mod.set(Modulation::kCe2, rate, 1.0f, 0.0f, 1.0f, 0.0f);
    rig.mod.setOn(true);
    const int n = 5 * 48000;
    const float slope = 1.0f / 1024.0f;
    std::vector<float> in(n), l, r;
    for (int i = 0; i < n; i++) in[i] = slope * (float)i;
    run(rig.mod, in, l, r);
    for (float& v : l) v /= rig.mod.makeup();  // the wet side alone is turned up a little for its low-pass
    const float speed = 4.0f * rate * 0.5f * (rig.mod.delayMax() - rig.mod.delayMin()) / kFs;  // samples per sample
    // The speed is taken over 10 ms: one sample's step is below what a float ramp resolves.
    const int span = 480;
    float lo = 1e9f, hi = -1e9f;
    int steady = 0, all = 0;
    for (int i = 4800; i < n; i++) {
        const float delay = (float)i - l[i] / slope;
        lo = fminf(lo, delay);
        hi = fmaxf(hi, delay);
        if (i >= 4800 + span) {
            const float before = (float)(i - span) - l[i - span] / slope;
            all++;
            if (fabsf(fabsf(delay - before) - speed * (float)span) < 0.1f * speed * (float)span) steady++;
        }
    }
    TEST_ASSERT_FLOAT_WITHIN(1.0f, rig.mod.delayMax() - rig.mod.delayMin(), hi - lo);
    TEST_ASSERT_TRUE_MESSAGE(lo - rig.mod.delayMin() > 1.0f && lo - rig.mod.delayMin() < 8.0f, "the low-pass's delay is not there");
    TEST_ASSERT_TRUE_MESSAGE(steady > all * 9 / 10, "the delay does not move at one speed");
}

// The ce2's wet signal is dark (the filters around the pedal's delay chip); the plain
// chorus's is not.
static void test_ce2_wet_is_low_passed() {
    const std::vector<float> in = sine(12000.0f, 0.5f, 19200);
    std::vector<float> l, r;
    Rig a, b;
    a.mod.set(Modulation::kCe2, 0.5f, 0.2f, 0.0f, 1.0f, 0.0f);
    a.mod.setOn(true);
    run(a.mod, in, l, r);
    TEST_ASSERT_TRUE(peak(l, 9600, 19200) < 0.5f * 0.2f);  // 6.6 kHz, third order: about -16 dB at 12 kHz
    b.mod.set(Modulation::kChorus, 0.5f, 0.2f, 0.0f, 1.0f, 0.0f);
    b.mod.setOn(true);
    run(b.mod, in, l, r);
    TEST_ASSERT_TRUE(peak(l, 9600, 19200) > 0.5f * 0.8f);
    // A low note passes at full level.
    Rig c;
    c.mod.set(Modulation::kCe2, 0.5f, 0.2f, 0.0f, 1.0f, 0.0f);
    c.mod.setOn(true);
    run(c.mod, sine(220.0f, 0.5f, 19200), l, r);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.5f * c.mod.makeup(), peak(l, 9600, 19200));
}

// The frequency at which one phaser stage with its corner at cornerHz turns the phase by
// 2 atan(tangent): the stage is exact in tan(pi f / fs), and the corner's own tangent is
// the effect's short series.
static float phaserHz(float cornerHz, float tangent) {
    const float w = 3.14159265f * cornerHz / kFs, w2 = w * w;
    return atanf(w * (1.0f + w2 * (0.33333333f + w2 * 0.13333333f)) * tangent) * kFs / 3.14159265f;
}

// The phaser without depth and feedback is four all-pass stages at the middle of the
// sweep: mixed half and half, a sine that each stage turns by 45 or by 135 degrees
// cancels (the two notches), a very low one adds up, and the wet signal alone is as
// loud as the input.
static void test_phaser_without_depth_has_two_notches() {
    const float corner = 0.5f * (Modulation::kPhaserLowHz + Modulation::kPhaserHighHz);
    std::vector<float> l, r;
    for (float tangent : {0.41421356f, 2.41421356f}) {
        Rig rig;
        rig.mod.set(Modulation::kPhaser, 0.5f, 0.0f, 0.0f, 0.5f, 0.0f);
        rig.mod.setOn(true);
        run(rig.mod, sine(phaserHz(corner, tangent), 1.0f, 19200), l, r);
        TEST_ASSERT_TRUE_MESSAGE(peak(l, 9600, 19200) < 0.02f, "a notch is not deep");
    }
    {
        Rig rig;
        rig.mod.set(Modulation::kPhaser, 0.5f, 0.0f, 0.0f, 0.5f, 0.0f);
        rig.mod.setOn(true);
        run(rig.mod, sine(20.0f, 1.0f, 48000), l, r);
        TEST_ASSERT_FLOAT_WITHIN(0.03f, 1.4142f * rig.mod.makeup(), peak(l, 24000, 48000));
    }
    for (float hz : {100.0f, 870.0f, 5000.0f}) {
        Rig rig;
        rig.mod.set(Modulation::kPhaser, 0.5f, 0.0f, 0.0f, 1.0f, 0.0f);
        rig.mod.setOn(true);
        run(rig.mod, sine(hz, 0.5f, 19200), l, r);
        TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.5f * rig.mod.makeup(), peak(l, 9600, 19200));
    }
}

// The phaser's notches move at the rate set: a 2 kHz sine is crossed by the upper notch
// (0.8 to 3.9 kHz at full depth) twice in every cycle of the LFO, and not by the lower.
static void test_phaser_sweeps_at_the_rate() {
    Rig rig;
    const float rate = 2.0f;
    rig.mod.set(Modulation::kPhaser, rate, 1.0f, 0.0f, 0.5f, 0.0f);
    rig.mod.setOn(true);
    const int n = 6 * 48000;
    std::vector<float> l, r;
    run(rig.mod, sine(2000.0f, 1.0f, n), l, r);
    int dips = 0;
    bool high = true;
    for (int at = 48000; at + 48 <= n; at += 48) {  // two periods of the sine at a time
        const float p = peak(l, at, at + 48);
        if (high && p < 0.15f) dips++, high = false;
        else if (!high && p > 0.6f) high = true;
    }
    TEST_ASSERT_INT_WITHIN(1, (int)(2.0f * rate * 5.0f), dips);
}

// The phaser's feedback puts a hump between the notches, as the later pedal has: with
// no depth, a sine that the three stages inside the loop turn by 180 degrees in all
// (60 each) comes out of the wet side 1 / (1 - g) times as large.
static void test_phaser_feedback_makes_a_hump() {
    const float corner = 0.5f * (Modulation::kPhaserLowHz + Modulation::kPhaserHighHz);
    const float g = 0.56f * Modulation::kPhaserMaxFeedback;
    Rig rig;
    rig.mod.set(Modulation::kPhaser, 0.5f, 0.0f, 0.56f, 1.0f, 0.0f);
    rig.mod.setOn(true);
    std::vector<float> l, r;
    run(rig.mod, sine(phaserHz(corner, 0.57735027f), 0.2f, 48000), l, r);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.2f * rig.mod.makeup() / (1.0f - g), peak(l, 24000, 48000));
    // And far below the corner the loop takes away: 1 / (1 + g).
    Rig low;
    low.mod.set(Modulation::kPhaser, 0.5f, 0.0f, 0.56f, 1.0f, 0.0f);
    low.mod.setOn(true);
    run(low.mod, sine(10.0f, 0.2f, 96000), l, r);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.2f * low.mod.makeup() / (1.0f + g), peak(l, 48000, 96000));
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
    for (int type : {Modulation::kChorus, Modulation::kFlanger, Modulation::kCe2, Modulation::kPhaser}) {
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
    const std::vector<float> in = sine(220.0f, 0.5f, 8 * 48000);
    std::vector<float> l(in.size()), r(in.size());
    int change = 0;
    for (size_t at = 0; at + kB <= in.size(); at += kB) {
        if (at % 9600 == 0) {
            switch (change++ % 20) {
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
                case 10: rig.mod.set(Modulation::kCe2, 3.0f, 1.0f, 0.0f, 0.5f, 0.0f); rig.mod.setOn(true); break;
                case 11: rig.mod.set(Modulation::kCe2, 10.0f, 1.0f, 0.8f, 1.0f, 1.0f); break;
                case 12: rig.mod.set(Modulation::kPhaser, 0.5f, 1.0f, 0.0f, 0.5f, 0.0f); break;
                case 13: rig.mod.set(Modulation::kPhaser, 10.0f, 1.0f, 1.0f, 0.5f, 1.0f); break;
                case 14: rig.mod.set(Modulation::kPhaser, 0.2f, 0.0f, 0.5f, 1.0f, 0.5f); break;
                case 15: rig.mod.setOn(false); break;
                case 16: rig.mod.setOn(true); break;
                case 17: rig.mod.set(Modulation::kChorus, 0.8f, 0.5f, 0.0f, 0.5f, 0.5f); break;
                case 18: rig.mod.set(Modulation::kPhaser, 4.0f, 1.0f, 0.56f, 0.5f, 0.5f); break;
                case 19: rig.mod.set(Modulation::kCe2, 0.5f, 0.5f, 0.0f, 0.5f, 0.5f); break;
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
    for (int type : {Modulation::kChorus, Modulation::kCe2, Modulation::kPhaser}) {
        {
            Rig rig;
            rig.mod.set(type, 1.0f, 1.0f, 0.4f, 0.5f, 0.0f);
            rig.mod.setOn(true);
            run(rig.mod, in, l, r);
            for (size_t i = 0; i < in.size(); i++) TEST_ASSERT_EQUAL_FLOAT(l[i], r[i]);
        }
        {
            Rig rig;
            rig.mod.set(type, 1.0f, 1.0f, 0.4f, 0.5f, 1.0f);
            rig.mod.setOn(true);
            run(rig.mod, in, l, r);
            float diff = 0.0f;
            for (size_t i = 0; i < in.size(); i++) diff = fmaxf(diff, fabsf(l[i] - r[i]));
            TEST_ASSERT_TRUE(diff > 0.1f);
        }
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
    for (int type : {Modulation::kChorus, Modulation::kFlanger, Modulation::kCe2, Modulation::kPhaser})
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
    for (int type : {Modulation::kFlanger, Modulation::kCe2, Modulation::kPhaser}) {
        ref.clear();
        for (int block : {32, 16, 64}) {
            Rig rig;
            rig.mod.set(type, 0.7f, 0.8f, 0.6f, 0.5f, 0.5f);
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
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_idle_writes_nothing);
    RUN_TEST(test_on_and_off_ramp_from_and_to_the_input);
    RUN_TEST(test_mix_zero_is_the_input);
    RUN_TEST(test_flanger_without_depth_is_a_comb);
    RUN_TEST(test_delay_range_fits_the_line);
    RUN_TEST(test_delay_sweeps_its_range_at_the_rate);
    RUN_TEST(test_ce2_sweeps_as_a_triangle);
    RUN_TEST(test_ce2_wet_is_low_passed);
    RUN_TEST(test_phaser_without_depth_has_two_notches);
    RUN_TEST(test_phaser_sweeps_at_the_rate);
    RUN_TEST(test_phaser_feedback_makes_a_hump);
    RUN_TEST(test_feedback_is_stable);
    RUN_TEST(test_changes_do_not_click);
    RUN_TEST(test_width);
    RUN_TEST(test_level_is_compensated);
    RUN_TEST(test_block_size_does_not_matter);
    return UNITY_END();
}
