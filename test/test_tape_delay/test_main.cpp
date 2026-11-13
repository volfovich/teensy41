// Host unit tests for dsp/fx/TapeDelay.h (the delay's tape type).
// Run with: pio test -e native
#include <unity.h>
#include <dsp/fx/TapeDelay.h>

#include "../fx_test_util.h"

using dsp::TapeDelay;
using namespace fxtest;

static constexpr int kLen = 36008;  // the firmware's line: 750 ms

// A TapeDelay with its line, set clean: no modulation, bright.
struct Rig {
    std::vector<float> line = std::vector<float>(kLen, 9.0f);  // init() must clear it
    TapeDelay d{kFs};
    Rig() {
        d.init(line.data(), kLen);
        d.setDelaySamples(9600.0f);
        d.setFeedback(0.5f);
        d.setTone(1.0f);
        d.setModDepth(0.0f);
        d.setDrive(0.0f);
    }
    void run(const std::vector<float>& x, std::vector<float>& l, std::vector<float>& r, int block = kB) {
        l.assign(x.size(), 0.0f);
        r.assign(x.size(), 0.0f);
        for (size_t at = 0; at + block <= x.size(); at += block) d.process(&x[at], &l[at], &r[at], block);
    }
};

// 50 ms of a 1 kHz sine with soft edges, then silence.
static std::vector<float> burst(int n, float amp) {
    std::vector<float> x(n, 0.0f);
    for (int i = 0; i < 2400; i++)
        x[i] = amp * sinf(kPi2 * 1000.0f * (float)i / kFs) * fminf(1.0f, fminf((float)i, (float)(2400 - i)) / 240.0f);
    return x;
}

// The periods between upward zero crossings in v[from, to), in samples.
static std::vector<double> periods(const std::vector<float>& v, size_t from, size_t to) {
    std::vector<double> at;
    for (size_t i = from + 1; i < to; i++)
        if (v[i - 1] < 0.0f && v[i] >= 0.0f) at.push_back((double)i - (double)v[i] / ((double)v[i] - v[i - 1]));
    std::vector<double> p;
    for (size_t k = 1; k < at.size(); k++) p.push_back(at[k] - at[k - 1]);
    return p;
}

void setUp() {}
void tearDown() {}

static void test_first_repeat_comes_at_the_set_time() {
    Rig rig;
    const std::vector<float> x = burst(kS, 0.25f);
    std::vector<float> l, r;
    rig.run(x, l, r);
    TEST_ASSERT_LESS_THAN_FLOAT(1e-4f, peak(l, 0, 9500));  // wet only: nothing before it
    const double first = rms(l, 9600, 12000) / rms(x, 0, 2400);
    printf("first repeat against the input: %.2f dB\n", db(first));
    TEST_ASSERT_FLOAT_WITHIN(2.0f, 0.0f, (float)db(first));
}

static void test_repeats_follow_the_setting() {
    const float fb[3] = {0.0f, 0.5f, 0.9f};
    for (int k = 0; k < 3; k++) {
        Rig rig;
        rig.d.setFeedback(fb[k]);
        std::vector<float> l, r;
        rig.run(burst(kS, 0.25f), l, r);
        const double ratio = rms(l, 19200, 21600) / rms(l, 9600, 12000);
        printf("repeats %.1f: second repeat against the first %.2f\n", fb[k], ratio);
        // The loop gain is 0.96 x the setting, less what the filters take from a 1 kHz tone.
        TEST_ASSERT_LESS_OR_EQUAL_FLOAT(0.96f * fb[k] + 0.01f, (float)ratio);
        TEST_ASSERT_GREATER_OR_EQUAL_FLOAT(0.75f * 0.96f * fb[k], (float)ratio);
    }
}

static void test_stays_stable_with_everything_at_maximum() {
    Rig rig;
    rig.d.setDelaySamples((float)rig.d.maxDelay());
    rig.d.setFeedback(1.0f);
    rig.d.setModDepth(1.0f);
    rig.d.setDrive(1.0f);
    std::vector<float> l, r;
    rig.run(noise(60 * kS, 7, 0.9f), l, r);
    TEST_ASSERT_TRUE(finiteAndBelow(l, 4.0f));
    TEST_ASSERT_TRUE(finiteAndBelow(r, 4.0f));
    // And the repeats die away once the input stops.
    const float loud = peak(l, 59 * kS, 60 * kS);
    rig.run(std::vector<float>(60 * kS, 0.0f), l, r);
    TEST_ASSERT_LESS_THAN_FLOAT(0.1f * loud, peak(l, 59 * kS, 60 * kS));
}

static void test_tone_darkens_the_repeats() {
    double high[2];
    int k = 0;
    for (float tone : {0.0f, 1.0f}) {
        Rig rig;
        rig.d.setFeedback(0.0f);
        rig.d.setTone(tone);
        std::vector<float> l, r;
        rig.run(noise(3 * kS, 5, 0.2f), l, r);
        high[k++] = db(bandRms(l, 4000.0f, kS, 3 * kS) / bandRms(l, 500.0f, kS, 3 * kS));
    }
    printf("4 kHz against 500 Hz in one repeat: tone 0 %.1f dB, tone 1 %.1f dB\n", high[0], high[1]);
    TEST_ASSERT_LESS_THAN_FLOAT((float)high[1] - 10.0f, (float)high[0]);
}

// Without Mod a repeated tone keeps its pitch exactly; with it the pitch wavers a little.
static void test_mod_wavers_the_pitch() {
    double dev[2];
    int k = 0;
    for (float mod : {0.0f, 1.0f}) {
        Rig rig;
        rig.d.setFeedback(0.0f);
        rig.d.setModDepth(mod);
        std::vector<float> l, r;
        rig.run(sine(1000.0f, 0.2f, 5 * kS), l, r);
        double worst = 0.0;
        for (double p : periods(l, kS, 5 * kS)) worst = fmax(worst, fabs(p - 48.0) / 48.0);
        dev[k++] = worst;
    }
    printf("largest pitch deviation: Mod 0 %.4f %%, Mod 100 %.2f %%\n", 100.0 * dev[0], 100.0 * dev[1]);
    TEST_ASSERT_LESS_THAN_FLOAT(0.0005f, (float)dev[0]);
    TEST_ASSERT_GREATER_THAN_FLOAT(0.005f, (float)dev[1]);
    TEST_ASSERT_LESS_THAN_FLOAT(0.02f, (float)dev[1]);
}

// A time change bends the pitch of what is in the line (never by more than an octave) and
// makes no step; then the new time holds.
static void test_time_change_glides() {
    Rig rig;
    rig.d.setFeedback(0.0f);
    const std::vector<float> x = sine(500.0f, 0.2f, 6 * kS);
    std::vector<float> l(x.size(), 0.0f), r(x.size(), 0.0f);
    for (size_t at = 0; at + kB <= x.size(); at += kB) {
        if (at == (size_t)kS) rig.d.setDelaySamples(24000.0f);       // 200 -> 500 ms
        if (at == (size_t)(3 * kS)) rig.d.setDelaySamples(4800.0f);  // -> 100 ms
        rig.d.process(&x[at], &l[at], &r[at], kB);
    }
    const float steady = largestStep(l, 24000, 48000);
    TEST_ASSERT_LESS_THAN_FLOAT(2.5f * steady, largestStep(l, kS, 6 * kS));
    double longest = 0.0, shortest = 1e9;
    for (double p : periods(l, kS, 5 * kS)) {
        longest = fmax(longest, p);
        shortest = fmin(shortest, p);
    }
    printf("period of a 96-sample tone while the time moves: %.1f to %.1f samples\n", shortest, longest);
    TEST_ASSERT_GREATER_THAN_FLOAT(1.5f * 96.0f, (float)longest);   // lengthening: flat
    TEST_ASSERT_LESS_OR_EQUAL_FLOAT(2.02f * 96.0f, (float)longest);
    TEST_ASSERT_LESS_THAN_FLOAT(0.8f * 96.0f, (float)shortest);     // shortening: sharp
    TEST_ASSERT_GREATER_OR_EQUAL_FLOAT(0.66f * 96.0f, (float)shortest);
    for (double p : periods(l, 5 * kS + 24000, 6 * kS)) TEST_ASSERT_FLOAT_WITHIN(0.05f, 96.0f, (float)p);
}

// With a silent line there is nothing to bend: the time applies at once.
static void test_silent_line_takes_the_time_at_once() {
    Rig rig;
    std::vector<float> l, r;
    rig.run(noise(kS, 2, 0.3f), l, r);
    while (!rig.d.clearStep(4096)) {}
    rig.d.setDelaySamples(4800.0f);
    rig.d.setFeedback(0.0f);
    const std::vector<float> x = burst(kS, 0.25f);
    rig.run(x, l, r);
    TEST_ASSERT_LESS_THAN_FLOAT(1e-4f, peak(l, 0, 4700));
    TEST_ASSERT_FLOAT_WITHIN(2.0f, 0.0f, (float)db(rms(l, 4800, 7200) / rms(x, 0, 2400)));
}

static void test_clear_gives_exact_silence() {
    Rig rig;
    std::vector<float> l, r;
    rig.run(noise(kS, 3, 0.5f), l, r);
    int steps = 0;
    while (!rig.d.clearStep(4096)) steps++;
    TEST_ASSERT_EQUAL(kLen / 4096, steps);
    rig.run(std::vector<float>(kS, 0.0f), l, r);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, peak(l, 0, l.size()));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, peak(r, 0, r.size()));
}

static void test_block_size_does_not_matter() {
    const std::vector<float> x = noise(24000, 5, 0.5f);
    Rig a, b;
    a.d.setModDepth(0.5f);
    b.d.setModDepth(0.5f);
    std::vector<float> la, ra, lb, rb;
    a.run(x, la, ra, 32);
    b.run(x, lb, rb, 96);
    for (size_t i = 0; i < x.size(); i++) {
        TEST_ASSERT_EQUAL_FLOAT(la[i], lb[i]);
        TEST_ASSERT_EQUAL_FLOAT(ra[i], rb[i]);
    }
}

// Drive: quiet playing passes the tape unchanged, loud playing is squeezed.
static void test_drive_saturates_loud_signals_only() {
    double gain[2][2];
    for (int dr = 0; dr < 2; dr++)
        for (int lv = 0; lv < 2; lv++) {
            Rig rig;
            rig.d.setFeedback(0.0f);
            rig.d.setDrive(dr ? 1.0f : 0.0f);
            const std::vector<float> x = sine(440.0f, lv ? 0.5f : 0.005f, 2 * kS);
            std::vector<float> l, r;
            rig.run(x, l, r);
            gain[dr][lv] = db(rms(l, kS, 2 * kS) / rms(x, kS, 2 * kS));
        }
    printf("gain of one repeat: no drive %.2f (quiet) / %.2f dB (loud); full drive %.2f / %.2f dB\n", gain[0][0],
           gain[0][1], gain[1][0], gain[1][1]);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, (float)gain[0][0], (float)gain[1][0]);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, (float)gain[0][0], (float)gain[0][1]);
    TEST_ASSERT_LESS_THAN_FLOAT((float)gain[0][1] - 6.0f, (float)gain[1][1]);
}

// The right head reads the same tape a little apart in wow and flutter: the sides are alike
// but not the same with Mod, and the same without.
static void test_sides_differ_only_by_the_wow() {
    for (float mod : {0.0f, 1.0f}) {
        Rig rig;
        rig.d.setModDepth(mod);
        std::vector<float> l, r;
        rig.run(noise(3 * kS, 8, 0.3f), l, r);
        float diff = 0.0f;
        for (size_t i = kS; i < l.size(); i++) diff = fmaxf(diff, fabsf(l[i] - r[i]));
        if (mod == 0.0f) TEST_ASSERT_EQUAL_FLOAT(0.0f, diff);
        else TEST_ASSERT_GREATER_THAN_FLOAT(0.01f, diff);
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_first_repeat_comes_at_the_set_time);
    RUN_TEST(test_repeats_follow_the_setting);
    RUN_TEST(test_stays_stable_with_everything_at_maximum);
    RUN_TEST(test_tone_darkens_the_repeats);
    RUN_TEST(test_mod_wavers_the_pitch);
    RUN_TEST(test_time_change_glides);
    RUN_TEST(test_silent_line_takes_the_time_at_once);
    RUN_TEST(test_clear_gives_exact_silence);
    RUN_TEST(test_block_size_does_not_matter);
    RUN_TEST(test_drive_saturates_loud_signals_only);
    RUN_TEST(test_sides_differ_only_by_the_wow);
    return UNITY_END();
}
