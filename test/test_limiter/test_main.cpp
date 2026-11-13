// Host unit tests for dsp/fx/Limiter.h (the Limiter block: a brickwall output limiter).
// Run with: pio test -e native
#include <unity.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <dsp/fx/Limiter.h>

using dsp::Limiter;

static constexpr float kFs = 48000.0f;
static constexpr float kPi2 = 6.2831853f;
static constexpr int kLook = Limiter::kLook;

static float dbToLin(float db) { return powf(10.0f, db / 20.0f); }

// Runs l / r through the limiter in place, in blocks; returns how many blocks it touched.
static int run(Limiter& lim, std::vector<float>& l, std::vector<float>& r, int block = 32) {
    int touched = 0;
    for (size_t at = 0; at + block <= l.size(); at += block) {
        float peak = 0.0f;
        for (int i = 0; i < block; i++) peak = fmaxf(peak, fmaxf(fabsf(l[at + i]), fabsf(r[at + i])));
        touched += lim.process(&l[at], &r[at], block, peak);
    }
    return touched;
}

static std::vector<float> sine(float hz, float amp, int n) {
    std::vector<float> v(n);
    for (int i = 0; i < n; i++) v[i] = amp * sinf(kPi2 * hz * (float)i / kFs);
    return v;
}

static float peakOf(const std::vector<float>& v, size_t from = 0) {
    float p = 0.0f;
    for (size_t i = from; i < v.size(); i++) p = fmaxf(p, fabsf(v[i]));
    return p;
}

// A nasty signal: loud noise, bursts, single spikes, a square, silence.
static std::vector<float> nasty(int n, uint32_t seed) {
    std::vector<float> v(n);
    for (int i = 0; i < n; i++) {
        seed = seed * 1664525u + 1013904223u;
        const float noise = (float)(int32_t)seed / 2147483648.0f;
        const int part = (i / 4800) % 5;
        v[i] = part == 0 ? 4.0f * noise : part == 1 ? (i % 997 == 0 ? 8.0f : 0.05f * noise) : part == 2 ? (i & 32 ? 2.5f : -2.5f)
               : part == 3 ? 0.0f : 1.5f * sinf(kPi2 * 82.4f * (float)i / kFs) + noise;
    }
    return v;
}

void setUp() {}
void tearDown() {}

// The point of it: nothing ever comes out above the ceiling, in either mode, for any
// ceiling, release and block size.
static void test_output_never_exceeds_the_ceiling() {
    for (bool look : {false, true})
        for (float ceilDb : {-0.5f, -1.0f, -6.0f, -12.0f})
            for (float rel : {10.0f, 100.0f, 1000.0f})
                for (int block : {16, 32, 64}) {
                    Limiter lim(kFs);
                    lim.set(true, ceilDb, rel, look);
                    std::vector<float> l = nasty(96000, 1), r = nasty(96000, 99);
                    run(lim, l, r, block);
                    const float c = dbToLin(ceilDb), p = fmaxf(peakOf(l), peakOf(r));
                    char msg[96];
                    snprintf(msg, sizeof(msg), "peak %.7f over ceiling %.7f (look %d, rel %.0f, block %d)", p, c, look, rel, block);
                    TEST_ASSERT_TRUE_MESSAGE(p <= c * 1.000001f, msg);
                }
}

// Under the ceiling nothing is changed: without lookahead the limiter does not even run,
// with lookahead the output is the input 1 ms later, bit for bit.
static void test_below_the_ceiling_is_untouched() {
    const std::vector<float> in = sine(330.0f, 0.85f, 48000);  // ceiling -1 dB = 0.891
    {
        Limiter lim(kFs);
        lim.set(true, -1.0f, 100.0f, false);
        std::vector<float> l = in, r = in;
        TEST_ASSERT_EQUAL_INT(0, run(lim, l, r));
        TEST_ASSERT_EQUAL_INT(0, lim.latencySamples());
        TEST_ASSERT_EQUAL_FLOAT(1.0f, lim.takeMinGain());
    }
    {
        Limiter lim(kFs);
        lim.set(true, -1.0f, 100.0f, true);
        std::vector<float> l = in, r = in;
        run(lim, l, r);
        TEST_ASSERT_EQUAL_INT(kLook, lim.latencySamples());
        for (size_t i = 960; i < in.size(); i++) TEST_ASSERT_EQUAL_FLOAT(in[i - kLook], l[i]);  // after the switch-on dip
        TEST_ASSERT_EQUAL_FLOAT(1.0f, lim.takeMinGain());
    }
    {
        Limiter lim(kFs);  // off: nothing at all, however loud
        std::vector<float> l = sine(330.0f, 3.0f, 4800), r = l;
        TEST_ASSERT_EQUAL_INT(0, run(lim, l, r));
        TEST_ASSERT_EQUAL_FLOAT(3.0f * sinf(kPi2 * 330.0f * 100.0f / kFs), l[100]);
    }
}

// A steady tone over the ceiling comes out as the same tone, just quieter: the gain
// settles and stays still (the hold bridges the gaps between half-waves), so there is
// next to no distortion. Checked at a guitar's low E and at 1 kHz, 6 dB over.
static void test_a_steady_tone_is_not_distorted() {
    for (bool look : {false, true})
        for (float hz : {82.41f, 1000.0f}) {
            Limiter lim(kFs);
            lim.set(true, -6.0f, 100.0f, look);
            const int n = 96000;
            std::vector<float> l = sine(hz, 1.0f, n), r = l;
            run(lim, l, r);
            // The part of the last second that is not the tone itself (least squares fit).
            double ss = 0, cc = 0, sc = 0, ys = 0, yc = 0, yy = 0;
            for (int i = 48000; i < n; i++) {
                const double a = kPi2 * hz * (double)i / kFs, s = sin(a), c = cos(a), y = l[i];
                ss += s * s, cc += c * c, sc += s * c, ys += y * s, yc += y * c, yy += y * y;
            }
            const double det = ss * cc - sc * sc, a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det;
            const double rest = yy - (a * ys + b * yc);
            const float thdDb = 10.0f * log10f((float)(fmax(rest, 1e-30) / yy));
            const float level = (float)sqrt(a * a + b * b);
            char msg[96];
            snprintf(msg, sizeof(msg), "%.0f Hz, look %d: distortion %.1f dB, level %.4f", hz, look, thdDb, level);
            TEST_ASSERT_TRUE_MESSAGE(thdDb < -60.0f, msg);
            TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, dbToLin(-6.0f), level, msg);
        }
}

// What the lookahead is for: a sudden peak. Without it the gain drops in one step at the
// peak; with it the gain glides down over the millisecond before and is there in time.
static void test_lookahead_glides_into_a_peak() {
    const int n = 9600, at = 4800;
    for (bool look : {false, true}) {
        Limiter lim(kFs);
        lim.set(true, -6.0f, 100.0f, look);
        std::vector<float> l(n, 0.25f), r(n, 0.25f);  // a level to read the gain from
        l[at] = r[at] = 2.0f;                         // needs gain 0.25
        const std::vector<float> in = l;
        run(lim, l, r);
        const int d = look ? kLook : 0;
        float step = 0.0f, prev = 1.0f;
        for (int i = 2000; i <= at + d; i++) {
            const float g = l[i] / in[i - d];
            step = fmaxf(step, prev - g);
            prev = g;
        }
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, dbToLin(-6.0f), l[at + d]);  // the peak itself: on the ceiling
        if (look) TEST_ASSERT_TRUE_MESSAGE(step < 0.03f, "the lookahead gain does not glide");  // 0.75 over 49 samples
        else TEST_ASSERT_TRUE(step > 0.7f);
    }
}

// After a peak the gain holds, then comes back with the release time, and ends at exactly
// 1: the limiter goes back to doing nothing.
static void test_release_returns_to_unity() {
    Limiter lim(kFs);
    lim.set(true, -6.0f, 50.0f, false);
    const int n = 48000;
    std::vector<float> l(n, 0.25f), r(n, 0.25f);
    l[100] = r[100] = 1.0f;
    const std::vector<float> in = l;
    for (int at = 0; at < 4800; at += 32) lim.process(&l[at], &r[at], 32, at == 96 ? 1.0f : 0.25f);
    const float low = dbToLin(-6.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, low, l[100] / in[100]);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, low, l[100 + 470] / in[100 + 470]);  // still held after 9.8 ms
    // One release time after the hold: 63 % of the way back.
    const float g = l[100 + 480 + 2400] / in[100 + 480 + 2400];
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f - (1.0f - low) * expf(-1.0f), g);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, low, lim.takeMinGain());
    int touched = 0;
    for (int at = 4800; at + 32 <= n; at += 32) touched += lim.process(&l[at], &r[at], 32, 0.25f);
    TEST_ASSERT_TRUE(touched > 0 && touched < 900);  // back at exactly 1 after 11 release times (0.55 s)
    TEST_ASSERT_FALSE(lim.busy());
    TEST_ASSERT_EQUAL_FLOAT(0.25f, l[n - 1]);
}

// Both sides get the same gain, so the balance between them stays.
static void test_both_sides_get_the_same_gain() {
    for (bool look : {false, true}) {
        Limiter lim(kFs);
        lim.set(true, -3.0f, 100.0f, look);
        std::vector<float> l = sine(220.0f, 2.0f, 24000), r = sine(220.0f, 0.5f, 24000);
        run(lim, l, r);
        for (size_t i = 12000; i < l.size(); i++)
            if (fabsf(r[i]) > 0.05f) TEST_ASSERT_FLOAT_WITHIN(1e-3f, 4.0f, l[i] / r[i]);
    }
}

// Switching the lookahead, or the limiter, while a loud note plays: never over the
// ceiling, and no jump in the output (it dips to silence and back).
static void test_switching_does_not_click() {
    Limiter lim(kFs);
    const int n = 10 * 9600;
    std::vector<float> l = sine(220.0f, 1.5f, n), r = l;
    int change = 0;
    float worst = 0.0f, prev = 0.0f, peak = 0.0f;
    size_t worstAt = 0;
    size_t firstOn = 0;
    for (size_t at = 0; at + 32 <= l.size(); at += 32) {
        if (at % 9600 == 0) {
            switch (change++) {
                case 0: break;                                    // off: the tone as it is
                case 1: lim.set(true, -1.0f, 100.0f, false); firstOn = at; break;
                case 2: lim.set(true, -1.0f, 100.0f, true); break;
                case 3: lim.set(true, -6.0f, 20.0f, true); break;
                case 4: lim.set(true, -6.0f, 20.0f, false); break;
                case 5: lim.set(true, -1.0f, 100.0f, true); break;
                case 6: lim.set(false, -1.0f, 100.0f, true); break;  // off from lookahead
                case 7: lim.set(true, -3.0f, 100.0f, true); break;
                default: break;
            }
        }
        float bp = 0.0f;
        for (int i = 0; i < 32; i++) bp = fmaxf(bp, fabsf(l[at + i]));
        if (lim.on() || lim.busy()) lim.process(&l[at], &r[at], 32, bp);
        for (int i = 0; i < 32; i++) {
            // Switching on without lookahead while over the ceiling is the limiter's own
            // sudden attack; everything after that must be smooth.
            if (at + i > firstOn + 64 && firstOn && change != 7 && fabsf(l[at + i] - prev) > worst) worst = fabsf(l[at + i] - prev), worstAt = at + i;
            if (change >= 2 && change != 7 && change != 8) peak = fmaxf(peak, fabsf(l[at + i]));
            prev = l[at + i];
        }
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "largest step %.4f at change %u + %u, peak %.4f", worst, (unsigned)(worstAt / 9600), (unsigned)(worstAt % 9600), peak);
    // The tone's own largest step is 1.5 * 2 pi * 220 / 48000 = 0.043.
    TEST_ASSERT_TRUE_MESSAGE(worst < 0.06f, msg);
    TEST_ASSERT_TRUE_MESSAGE(peak <= dbToLin(-1.0f) * 1.000001f, msg);
}

// The block size does not change the result.
static void test_block_size_does_not_matter() {
    for (bool look : {false, true}) {
        std::vector<float> ref;
        for (int block : {32, 16, 64}) {
            Limiter lim(kFs);
            lim.set(true, -3.0f, 80.0f, look);
            std::vector<float> l = nasty(19200, 5), r = nasty(19200, 6);
            run(lim, l, r, block);
            if (ref.empty()) ref = l;
            else
                for (size_t i = 0; i < ref.size(); i++) TEST_ASSERT_EQUAL_FLOAT(ref[i], l[i]);
        }
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_output_never_exceeds_the_ceiling);
    RUN_TEST(test_below_the_ceiling_is_untouched);
    RUN_TEST(test_a_steady_tone_is_not_distorted);
    RUN_TEST(test_lookahead_glides_into_a_peak);
    RUN_TEST(test_release_returns_to_unity);
    RUN_TEST(test_both_sides_get_the_same_gain);
    RUN_TEST(test_switching_does_not_click);
    RUN_TEST(test_block_size_does_not_matter);
    return UNITY_END();
}
