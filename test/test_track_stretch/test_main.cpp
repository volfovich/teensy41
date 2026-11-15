// Host unit tests for dsp/player/TimeStretch.h (a backing track slower or faster at the same
// pitch).
// Run with: pio test -e native
#include <unity.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <dsp/player/TimeStretch.h>

using dsp::TimeStretch;

void setUp() {}
void tearDown() {}

static const double kPi2 = 6.283185307179586;
static const double kFs = 48000.0;

struct Mem {
    std::vector<int16_t> in = std::vector<int16_t>(2 * TimeStretch::kInFrames);
    std::vector<float> work = std::vector<float>(TimeStretch::kWorkFloats);
    std::vector<uint16_t> index = std::vector<uint16_t>(TimeStretch::kIndexWords);
    std::vector<int16_t> out = std::vector<int16_t>(2 * TimeStretch::kHop);
    TimeStretch::Memory get() { return {in.data(), work.data(), index.data(), out.data()}; }
};

// Runs a whole stereo signal through at `rate`, pushing `piece` frames at a time.
static std::vector<int16_t> stretch(const std::vector<int16_t>& in, float rate, int piece = 1024) {
    Mem mem;
    TimeStretch ts(mem.get());
    ts.setRate(rate);
    std::vector<int16_t> out, buf(2 * TimeStretch::kHop);
    size_t at = 0;
    const size_t frames = in.size() / 2;
    for (int guard = 0; guard < 1000000; guard++) {
        const int n = ts.pull(buf.data(), TimeStretch::kHop);
        if (n) {
            out.insert(out.end(), buf.begin(), buf.begin() + 2 * n);
            continue;
        }
        if (ts.done()) break;
        if (at >= frames) {
            ts.finish();
            continue;
        }
        size_t take = frames - at < (size_t)piece ? frames - at : (size_t)piece;
        if (take > (size_t)ts.room()) take = (size_t)ts.room();
        TEST_ASSERT_TRUE(take > 0);
        ts.push(&in[2 * at], (int)take);
        at += take;
    }
    return out;
}

static std::vector<int16_t> tone(double seconds, double hzL, double ampL, double hzR, double ampR) {
    const int n = (int)(kFs * seconds);
    std::vector<int16_t> v(2 * n);
    for (int i = 0; i < n; i++) {
        v[2 * i] = (int16_t)lrint(ampL * 32767.0 * sin(kPi2 * hzL * i / kFs));
        v[2 * i + 1] = (int16_t)lrint(ampR * 32767.0 * sin(kPi2 * hzR * i / kFs));
    }
    return v;
}

static double toneAmp(const std::vector<int16_t>& v, int ch, size_t from, size_t to, double hz) {
    double re = 0, im = 0;
    for (size_t i = from; i < to; i++) {
        const double x = v[2 * i + ch] / 32767.0, ph = kPi2 * hz * (double)i / kFs;
        re += x * cos(ph);
        im += x * sin(ph);
    }
    return 2.0 * sqrt(re * re + im * im) / (double)(to - from);
}

static double rms(const std::vector<int16_t>& v, int ch, size_t from, size_t to) {
    double s = 0;
    for (size_t i = from; i < to; i++) s += (v[2 * i + ch] / 32767.0) * (v[2 * i + ch] / 32767.0);
    return sqrt(s / (double)(to - from));
}

static void test_the_length_changes_and_the_pitch_does_not() {
    const std::vector<int16_t> in = tone(2.0, 440.0, 0.5, 1000.0, 0.3);
    for (float rate : {0.5f, 0.75f, 0.9f, 1.1f, 1.25f, 1.5f}) {
        const std::vector<int16_t> out = stretch(in, rate);
        const double frames = (double)out.size() / 2;
        char msg[96];
        snprintf(msg, sizeof(msg), "rate %.2f: %.0f frames", rate, frames);
        // As long as the input divided by the rate, to within two output frames.
        TEST_ASSERT_TRUE_MESSAGE(fabs(frames - 96000.0 / rate) <= 2.0 * TimeStretch::kHop, msg);
        // The same two tones at the same level; nothing an octave away.
        const size_t a = 4800, b = (size_t)frames - 4800;
        const double l = toneAmp(out, 0, a, b, 440.0), r = toneAmp(out, 1, a, b, 1000.0);
        snprintf(msg, sizeof(msg), "rate %.2f: 440 Hz %.3f, 1 kHz %.3f, rms %.3f", rate, l, r, rms(out, 0, a, b));
        TEST_ASSERT_TRUE_MESSAGE(l > 0.45 && l < 0.52, msg);
        TEST_ASSERT_TRUE_MESSAGE(r > 0.26 && r < 0.32, msg);
        TEST_ASSERT_TRUE_MESSAGE(fabs(rms(out, 0, a, b) - 0.5 / sqrt(2.0)) < 0.02, msg);
        TEST_ASSERT_TRUE(toneAmp(out, 0, a, b, 440.0 * rate) < 0.02);
        TEST_ASSERT_TRUE(toneAmp(out, 0, a, b, 440.0 / rate) < 0.02);
        // No clicks at the joins: no step larger than the steepest part of the two tones.
        int worst = 0;
        for (size_t i = a; i < b; i++) {
            const int d = abs(out[2 * i] - out[2 * (i - 1)]);
            worst = d > worst ? d : worst;
        }
        const double steepest = 0.5 * 32767.0 * kPi2 * 440.0 / kFs;
        snprintf(msg, sizeof(msg), "rate %.2f: largest step %d, a clean tone's %.0f", rate, worst, steepest);
        TEST_ASSERT_TRUE_MESSAGE(worst < 1.3 * steepest + 4, msg);
    }
}

static void test_both_sides_move_together() {
    // The same sound on both sides stays the same on both sides.
    std::vector<int16_t> in = tone(1.0, 330.0, 0.4, 330.0, 0.4);
    for (size_t i = 0; i < in.size() / 2; i++) {
        in[2 * i] = (int16_t)(in[2 * i] + (int16_t)(3000.0 * sin(kPi2 * 2100.0 * i / kFs)));
        in[2 * i + 1] = in[2 * i];
    }
    const std::vector<int16_t> out = stretch(in, 0.7f);
    // (Within one step of 16 bits: the two sides are rounded each by itself.)
    for (size_t i = 0; i < out.size() / 2; i++) TEST_ASSERT_INT16_WITHIN(1, out[2 * i], out[2 * i + 1]);
    // And a silent side stays silent.
    std::vector<int16_t> one = tone(1.0, 330.0, 0.4, 0.0, 0.0);
    const std::vector<int16_t> out2 = stretch(one, 1.3f);
    for (size_t i = 0; i < out2.size() / 2; i++) TEST_ASSERT_INT16_WITHIN(1, 0, out2[2 * i + 1]);
}

// Noise bursts of 30 ms every `every` seconds over a quiet tone, like a drum over a pad.
static std::vector<int16_t> hits(double seconds, double every) {
    std::vector<int16_t> v = tone(seconds, 220.0, 0.05, 220.0, 0.05);
    uint32_t s = 12345;
    const int n = (int)(kFs * seconds), len = (int)(kFs * 0.03);
    for (double t = 0.25; t + 0.05 < seconds; t += every) {
        const int at = (int)(t * kFs);
        for (int i = 0; i < len && at + i < n; i++) {
            s = s * 1664525u + 1013904223u;
            const double e = exp(-(double)i / (kFs * 0.008));
            const int16_t x = (int16_t)(0.6 * 32767.0 * e * ((double)(s >> 8) / 8388608.0 - 1.0));
            v[2 * (at + i)] = (int16_t)(v[2 * (at + i)] + x);
            v[2 * (at + i) + 1] = v[2 * (at + i)];
        }
    }
    return v;
}

// The times (seconds) where the level jumps up: the start of each hit. Hits closer than
// `apart` seconds count as one. *loudest (if given) gets each hit's largest sample.
static std::vector<double> onsets(const std::vector<int16_t>& v, double apart, std::vector<double>* loudest = nullptr) {
    std::vector<double> at;
    const int win = 240;  // 5 ms
    double prev = 0;
    for (size_t i = 0; i + win <= v.size() / 2; i += win) {
        const double e = rms(v, 0, i, i + win);
        if (e > 0.05 && prev < 0.04 && (at.empty() || (double)i / kFs - at.back() > apart)) {
            at.push_back((double)i / kFs);
            if (loudest) loudest->push_back(0.0);
        }
        if (loudest && !at.empty() && (double)i / kFs - at.back() < 0.03)
            for (size_t k = i; k < i + win; k++)
                if (abs(v[2 * k]) > loudest->back()) loudest->back() = abs(v[2 * k]);
        prev = e;
    }
    return at;
}

static void test_hits_stay_single_and_in_time() {
    const std::vector<int16_t> in = hits(6.0, 0.5);
    std::vector<double> wasLoud;
    const std::vector<double> was = onsets(in, 0.06, &wasLoud);
    TEST_ASSERT_EQUAL_UINT32(12, (uint32_t)was.size());
    for (float rate : {0.5f, 0.6f, 0.7f, 0.85f, 0.95f, 1.05f, 1.2f, 1.5f}) {
        const std::vector<int16_t> out = stretch(in, rate);
        std::vector<double> nowLoud;
        const std::vector<double> now = onsets(out, 0.06, &nowLoud);
        char msg[96];
        snprintf(msg, sizeof(msg), "rate %.2f: %u hits", rate, (unsigned)now.size());
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(12, (uint32_t)now.size(), msg);
        // None doubled: counted with hits 12 ms apart told apart, there are no more.
        snprintf(msg, sizeof(msg), "rate %.2f: %u hits when 12 ms apart count as two", rate, (unsigned)onsets(out, 0.012).size());
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(12, (uint32_t)onsets(out, 0.012).size(), msg);
        // Each as loud as it was (its largest sample within 1.5 dB).
        for (size_t i = 0; i < now.size(); i++) {
            snprintf(msg, sizeof(msg), "rate %.2f: hit %u at %.1f dB of its level", rate, (unsigned)i, 20.0 * log10(nowLoud[i] / wasLoud[i]));
            TEST_ASSERT_TRUE_MESSAGE(fabs(20.0 * log10(nowLoud[i] / wasLoud[i])) < 1.5, msg);
        }
        double worst = 0, sum = 0;
        for (size_t i = 0; i < now.size(); i++) {
            const double e = now[i] - was[i] / rate;
            worst = fabs(e) > fabs(worst) ? e : worst;
            sum += e;
        }
        snprintf(msg, sizeof(msg), "rate %.2f: hits off by %.0f ms on average, %.0f ms at worst", rate,
                 1000.0 * sum / now.size(), 1000.0 * worst);
        TEST_MESSAGE(msg);
        // A hit comes early when slowed: what lies before it in its 60 ms piece cannot be
        // repeated without the hit. All hits come early alike, so the beat stays even.
        TEST_ASSERT_TRUE_MESSAGE(fabs(worst) < 0.075, msg);
        TEST_ASSERT_TRUE_MESSAGE(fabs(worst - sum / now.size()) < 0.030, msg);
    }
}

static void test_hits_in_quick_succession_do_not_stall_it() {
    // A hit every 70 ms, and one every 25 ms: the stretcher is ahead or behind its pace
    // most of the time. It must go on taking input and give the right length.
    for (double every : {0.07, 0.025}) {
        const std::vector<int16_t> in = hits(4.0, every);
        for (float rate : {0.5f, 0.8f, 1.3f, 1.5f}) {
            const std::vector<int16_t> out = stretch(in, rate, 257);  // (stretch() fails if no input can be taken)
            char msg[96];
            snprintf(msg, sizeof(msg), "a hit every %.0f ms at rate %.2f: %u frames", 1000.0 * every, rate, (unsigned)(out.size() / 2));
            // (Within 0.1 s: what the last hits took from the tempo may still be owed at the end.)
            TEST_ASSERT_TRUE_MESSAGE(fabs((double)out.size() / 2 - 192000.0 / rate) <= 4800.0, msg);
        }
    }
}

static void test_how_the_input_arrives_does_not_matter() {
    const std::vector<int16_t> in = hits(2.0, 0.37);
    const std::vector<int16_t> a = stretch(in, 0.8f, 1024), b = stretch(in, 0.8f, 333), c = stretch(in, 0.8f, 4000);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)a.size(), (uint32_t)b.size());
    TEST_ASSERT_EQUAL_UINT32((uint32_t)a.size(), (uint32_t)c.size());
    TEST_ASSERT_EQUAL_INT16_ARRAY(a.data(), b.data(), a.size());
    TEST_ASSERT_EQUAL_INT16_ARRAY(a.data(), c.data(), a.size());
}

static void test_reset_starts_a_new_stream_and_the_rate_is_held_in_range() {
    Mem mem;
    TimeStretch ts(mem.get());
    ts.setRate(0.1f);
    TEST_ASSERT_EQUAL_FLOAT(TimeStretch::kMinRate, ts.rate());
    ts.setRate(9.0f);
    TEST_ASSERT_EQUAL_FLOAT(TimeStretch::kMaxRate, ts.rate());
    ts.setRate(0.8f);
    const std::vector<int16_t> in = tone(0.5, 440.0, 0.5, 440.0, 0.5);
    std::vector<int16_t> first(2 * TimeStretch::kHop), again(2 * TimeStretch::kHop);
    ts.push(in.data(), 6000);
    TEST_ASSERT_EQUAL_INT(TimeStretch::kHop, ts.pull(first.data(), TimeStretch::kHop));
    TEST_ASSERT_TRUE(ts.lag() > 0.0f);
    ts.pull(again.data(), TimeStretch::kHop);
    ts.reset();
    TEST_ASSERT_EQUAL_INT(TimeStretch::kInFrames, ts.room());
    TEST_ASSERT_EQUAL_INT(0, ts.pull(again.data(), TimeStretch::kHop));  // nothing in
    ts.push(in.data(), 6000);
    TEST_ASSERT_EQUAL_INT(TimeStretch::kHop, ts.pull(again.data(), TimeStretch::kHop));
    TEST_ASSERT_EQUAL_INT16_ARRAY(first.data(), again.data(), first.size());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_the_length_changes_and_the_pitch_does_not);
    RUN_TEST(test_both_sides_move_together);
    RUN_TEST(test_hits_stay_single_and_in_time);
    RUN_TEST(test_hits_in_quick_succession_do_not_stall_it);
    RUN_TEST(test_how_the_input_arrives_does_not_matter);
    RUN_TEST(test_reset_starts_a_new_stream_and_the_rate_is_held_in_range);
    return UNITY_END();
}
