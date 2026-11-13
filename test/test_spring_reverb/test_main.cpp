// Host unit tests for dsp/fx/SpringReverb.h (the reverb's spring mode).
// Run with: pio test -e native
#include <unity.h>
#include <dsp/fx/SpringReverb.h>

#include "../fx_test_util.h"

using dsp::SpringReverb;
using namespace fxtest;

// A SpringReverb with its memory, at the settings table's defaults.
struct Rig {
    std::vector<float> mem = std::vector<float>(SpringReverb::kMemFloats, 9.0f);  // init() must clear it
    SpringReverb rv{kFs};
    Rig() {
        rv.init(mem.data());
        rv.setSize(0.5f);
        rv.setTone(0.5f);
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
    TEST_ASSERT_LESS_OR_EQUAL(36993, SpringReverb::kMemFloats);
}

static void test_decay_time_follows_size() {
    const float want[3] = {0.8f, 1.6f, 3.2f};
    int k = 0;
    for (float size : {0.0f, 0.5f, 1.0f}) {
        Rig rig;
        rig.rv.setSize(size);
        std::vector<float> l, r;
        rig.run(impulse(20 * kS), l, r);
        const double t = rt60(l, r, 0);
        printf("size %.1f: decay %.2f s (wanted %.2f)\n", size, t, want[k]);
        TEST_ASSERT_FLOAT_WITHIN(0.25f * want[k], want[k], (float)t);
        k++;
    }
}

static void test_stays_stable_at_the_longest_decay() {
    Rig rig;
    rig.rv.setSize(1.0f);
    rig.rv.setTone(1.0f);
    std::vector<float> l, r;
    rig.run(noise(60 * kS, 7, 0.9f), l, r);
    TEST_ASSERT_TRUE(finiteAndBelow(l, 8.0f));
    TEST_ASSERT_TRUE(finiteAndBelow(r, 8.0f));
    rig.run(std::vector<float>(20 * kS, 0.0f), l, r);
    TEST_ASSERT_LESS_THAN_FLOAT(1e-3f, peak(l, 19 * kS, 20 * kS));
}

// The drip: within one pass through a spring the highs arrive later than the lows, the
// later the nearer they are to the spring's top frequency (4.8 kHz).
static void test_each_echo_is_a_rising_chirp() {
    Rig rig;
    rig.rv.setTone(1.0f);
    const std::vector<float> x = impulse(kS);
    std::vector<float> l, r;
    rig.run(x, l, r);
    // The left side is the first spring's far end: the first pass arrives after its delay
    // (1601 samples) and the next one 1999 later. The measuring filter's own delay is taken off.
    double t[3];
    int k = 0;
    for (float hz : {500.0f, 3000.0f, 4300.0f}) t[k++] = centreTime(l, hz, 1601, 3400) - centreTime(x, hz, 0, 1500);
    printf("first pass, delay of 500 Hz %.0f, 3 kHz %.0f, 4.3 kHz %.0f samples\n", t[0], t[1], t[2]);
    TEST_ASSERT_GREATER_THAN_FLOAT((float)t[0] + 40.0f, (float)t[1]);
    TEST_ASSERT_GREATER_THAN_FLOAT((float)t[1] + 100.0f, (float)t[2]);
}

// Nothing of the all-passes' repeated pattern above the spring's top comes out, and Tone
// lowers the top.
static void test_top_is_limited_and_tone_lowers_it() {
    double at3k[2], at8k[2];
    int k = 0;
    for (float tone : {0.0f, 1.0f}) {
        Rig rig;
        rig.rv.setTone(tone);
        const std::vector<float> x = noise(4 * kS, 9, 0.3f);
        std::vector<float> l, r;
        rig.run(x, l, r);
        at3k[k] = db(bandRms(l, 3000.0f, 2 * kS, 4 * kS) / bandRms(l, 800.0f, 2 * kS, 4 * kS));
        at8k[k] = db(bandRms(l, 8000.0f, 2 * kS, 4 * kS) / bandRms(l, 800.0f, 2 * kS, 4 * kS));
        k++;
    }
    printf("against 800 Hz: 3 kHz %.1f dB (tone 0) / %.1f dB (tone 1); 8 kHz %.1f / %.1f dB\n", at3k[0], at3k[1],
           at8k[0], at8k[1]);
    TEST_ASSERT_LESS_THAN_FLOAT(-20.0f, (float)at8k[1]);
    TEST_ASSERT_LESS_THAN_FLOAT((float)at3k[1] - 6.0f, (float)at3k[0]);
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

static void test_clear_gives_exact_silence() {
    Rig rig;
    std::vector<float> l, r;
    rig.run(noise(kS, 3, 0.5f), l, r);
    while (!rig.rv.clearStep(4096)) {}
    rig.run(std::vector<float>(kS, 0.0f), l, r);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, peak(l, 0, l.size()));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, peak(r, 0, r.size()));
}

static void test_block_size_does_not_matter() {
    const std::vector<float> x = noise(9600, 5, 0.5f);
    Rig a, b;
    std::vector<float> la, ra, lb, rb;
    a.run(x, la, ra, 32);
    b.run(x, lb, rb, 80);  // not a multiple of the effect's own chunk
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
    RUN_TEST(test_each_echo_is_a_rising_chirp);
    RUN_TEST(test_top_is_limited_and_tone_lowers_it);
    RUN_TEST(test_sides_differ_and_level_matches_the_input);
    RUN_TEST(test_clear_gives_exact_silence);
    RUN_TEST(test_block_size_does_not_matter);
    return UNITY_END();
}
