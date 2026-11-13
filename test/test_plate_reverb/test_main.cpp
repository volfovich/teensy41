// Host unit tests for dsp/fx/PlateReverb.h (the reverb's plate mode).
// Run with: pio test -e native
#include <unity.h>
#include <dsp/fx/PlateReverb.h>

#include "../fx_test_util.h"

using dsp::PlateReverb;
using namespace fxtest;

// A PlateReverb with its memory, at the settings table's defaults.
struct Rig {
    std::vector<float> mem = std::vector<float>(PlateReverb::kMemFloats, 9.0f);  // init() must clear it
    PlateReverb rv{kFs};
    Rig() {
        rv.init(mem.data());
        rv.setSize(0.5f);
        rv.setTone(0.5f);
        rv.setMod(0.4f);
        rv.setPreDelayMs(40.0f);
    }
    void run(const std::vector<float>& x, std::vector<float>& l, std::vector<float>& r, int block = kB) {
        l.assign(x.size(), 0.0f);
        r.assign(x.size(), 0.0f);
        for (size_t at = 0; at + block <= x.size(); at += block) rv.process(&x[at], &x[at], &l[at], &r[at], block);
    }
};

void setUp() {}
void tearDown() {}

static void test_fits_the_shared_memory() {
    // The reverbs share Reverb80's block: 36,993 floats (ReverbBank asserts the same).
    TEST_ASSERT_LESS_OR_EQUAL(36993, PlateReverb::kMemFloats);
}

static void test_decay_time_follows_size() {
    const float want[3] = {0.5f, 2.0f, 8.0f};
    int k = 0;
    for (float size : {0.0f, 0.5f, 1.0f}) {
        Rig rig;
        rig.rv.setSize(size);
        std::vector<float> l, r;
        rig.run(impulse(30 * kS), l, r);
        const double t = rt60(l, r, 0);
        printf("size %.1f: decay %.2f s (wanted %.1f)\n", size, t, want[k]);
        TEST_ASSERT_FLOAT_WITHIN(0.25f * want[k], want[k], (float)t);
        k++;
    }
}

static void test_stays_stable_at_the_longest_decay() {
    Rig rig;
    rig.rv.setSize(1.0f);
    rig.rv.setTone(1.0f);
    rig.rv.setMod(1.0f);
    std::vector<float> l, r;
    rig.run(noise(60 * kS, 7, 0.9f), l, r);
    TEST_ASSERT_TRUE(finiteAndBelow(l, 8.0f));
    TEST_ASSERT_TRUE(finiteAndBelow(r, 8.0f));
    // And it decays once the input stops.
    rig.run(std::vector<float>(30 * kS, 0.0f), l, r);
    TEST_ASSERT_LESS_THAN_FLOAT(1e-3f, peak(l, 29 * kS, 30 * kS));
}

static void test_pre_delay_sets_the_first_sound() {
    Rig rig;
    rig.rv.setPreDelayMs(80.0f);
    std::vector<float> l, r;
    rig.run(impulse(kS), l, r);
    size_t first = 0;
    while (first < l.size() && l[first] == 0.0f && r[first] == 0.0f) first++;
    // The pre-delay (3840 samples) plus the shortest way through the diffusers and to a tap.
    TEST_ASSERT_GREATER_OR_EQUAL(3840, (int)first);
    TEST_ASSERT_LESS_THAN(3840 + 480, (int)first);
}

static void test_tone_darkens_the_tail() {
    double high[2];
    int k = 0;
    for (float tone : {0.0f, 1.0f}) {
        Rig rig;
        rig.rv.setTone(tone);
        std::vector<float> l, r;
        rig.run(impulse(3 * kS), l, r);
        // One second in: the highs against the mids.
        high[k++] = db(bandRms(l, 6000.0f, kS, 2 * kS) / bandRms(l, 600.0f, kS, 2 * kS));
    }
    printf("6 kHz against 600 Hz one second in: tone 0 %.1f dB, tone 1 %.1f dB\n", high[0], high[1]);
    TEST_ASSERT_LESS_THAN_FLOAT((float)high[1] - 10.0f, (float)high[0]);
}

static void test_sides_differ_and_level_matches_the_input() {
    Rig rig;
    const std::vector<float> x = pink(6 * kS, 4, 0.3f);
    std::vector<float> l, r;
    rig.run(x, l, r);
    const double c = correlation(l, r, 3 * kS, 6 * kS);
    const double d = db(rms2(l, r, 3 * kS, 6 * kS) / rms(x, 3 * kS, 6 * kS));
    printf("left/right correlation %.2f, wet level against the input %.2f dB\n", c, d);
    TEST_ASSERT_LESS_THAN_FLOAT(0.5f, (float)fabs(c));
    TEST_ASSERT_FLOAT_WITHIN(3.0f, 0.0f, (float)d);
}

static void test_mod_moves_the_tail() {
    std::vector<float> l[2], r[2];
    int k = 0;
    for (float mod : {0.0f, 1.0f}) {
        Rig rig;
        rig.rv.setSize(0.8f);
        rig.rv.setMod(mod);
        rig.run(impulse(3 * kS), l[k], r[k]);
        k++;
    }
    double diff = 0, ref = 0;
    for (int i = kS; i < 3 * kS; i++) {
        diff += (double)(l[0][i] - l[1][i]) * (l[0][i] - l[1][i]);
        ref += (double)l[0][i] * l[0][i];
    }
    TEST_ASSERT_GREATER_THAN_FLOAT(0.1f, (float)sqrt(diff / ref));
}

static void test_clear_gives_exact_silence() {
    Rig rig;
    std::vector<float> l, r;
    rig.run(noise(kS, 3, 0.5f), l, r);
    int steps = 0;
    while (!rig.rv.clearStep(4096)) steps++;
    TEST_ASSERT_EQUAL(PlateReverb::kMemFloats / 4096, steps);
    rig.run(std::vector<float>(kS, 0.0f), l, r);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, peak(l, 0, l.size()));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, peak(r, 0, r.size()));
}

static void test_block_size_does_not_matter() {
    const std::vector<float> x = noise(9600, 5, 0.5f);
    Rig a, b;
    std::vector<float> la, ra, lb, rb;
    a.run(x, la, ra, 32);
    b.run(x, lb, rb, 96);
    for (size_t i = 0; i < x.size(); i++) {
        TEST_ASSERT_EQUAL_FLOAT(la[i], lb[i]);
        TEST_ASSERT_EQUAL_FLOAT(ra[i], rb[i]);
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_fits_the_shared_memory);
    RUN_TEST(test_decay_time_follows_size);
    RUN_TEST(test_stays_stable_at_the_longest_decay);
    RUN_TEST(test_pre_delay_sets_the_first_sound);
    RUN_TEST(test_tone_darkens_the_tail);
    RUN_TEST(test_sides_differ_and_level_matches_the_input);
    RUN_TEST(test_mod_moves_the_tail);
    RUN_TEST(test_clear_gives_exact_silence);
    RUN_TEST(test_block_size_does_not_matter);
    return UNITY_END();
}
