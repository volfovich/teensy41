// Host unit tests for dsp/player/BeatScan.h (a backing track's tempo and beats).
// Run with: pio test -e native
#include <unity.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <dsp/player/BeatScan.h>

using dsp::BeatScan;

void setUp() {}
void tearDown() {}

static const double kPi2 = 6.283185307179586;

// A band: kick and snare on the beats, closed hats on the eighths between, a bass note per
// beat and a pad, from `start` seconds on. wander: the tempo drifts by that much (0.1 = 10 %)
// back and forth over the track.
static std::vector<int16_t> song(double rate, double seconds, double bpm, double start, double wander = 0.0, bool hatsOnly = false) {
    const int n = (int)(rate * seconds);
    std::vector<float> x(n, 0.0f);
    uint32_t s = 99;
    auto noise = [&]() {
        s = s * 1664525u + 1013904223u;
        return (float)(s >> 8) / 8388608.0f - 1.0f;
    };
    double t = start;
    for (int beat = 0; t < seconds - 0.3; beat++) {
        const double period = 60.0 / (bpm * (1.0 + wander * sin(kPi2 * t / 20.0)));
        const int at = (int)(t * rate);
        const int len = (int)(0.12 * rate);
        for (int i = 0; i < len && at + i < n; i++) {
            const double e = exp(-(double)i / (rate * 0.03));
            if (!hatsOnly) {
                if (beat % 2 == 0) x[at + i] += (float)(0.6 * e * sin(kPi2 * 60.0 * i / rate * (1.0 + 2.0 * e)));  // kick
                else x[at + i] += (float)(0.35 * e * noise() + 0.2 * e * sin(kPi2 * 190.0 * i / rate));              // snare
                x[at + i] += (float)(0.15 * exp(-(double)i / (rate * 0.2)) * sin(kPi2 * (55.0 + 10.0 * (beat / 4 % 3)) * i / rate));
            }
        }
        const int hat = (int)((t + period / 2) * rate), hlen = (int)(0.03 * rate);
        for (int i = 0; i < hlen && hat + i < n; i++) x[hat + i] += (float)(0.12 * exp(-(double)i / (rate * 0.006)) * noise());
        if (hatsOnly)
            for (int i = 0; i < hlen && at + i < n; i++) x[at + i] += (float)(0.2 * exp(-(double)i / (rate * 0.006)) * noise());
        t += period;
    }
    std::vector<int16_t> v(2 * n);
    for (int i = 0; i < n; i++) {
        const float pad = 0.04f * (float)sin(kPi2 * 220.0 * i / rate) + 0.03f * (float)sin(kPi2 * 277.2 * i / rate);
        const float y = x[i] + pad;
        v[2 * i] = (int16_t)lrintf(32767.0f * (y > 1.0f ? 1.0f : (y < -1.0f ? -1.0f : y)));
        v[2 * i + 1] = (int16_t)lrintf(32767.0f * 0.8f * (y > 1.0f ? 1.0f : (y < -1.0f ? -1.0f : y)));
    }
    return v;
}

static BeatScan::Result scan(const std::vector<int16_t>& v, uint32_t rate, int piece = 1024) {
    std::vector<BeatScan::Peak> peaks(BeatScan::kMaxPeaks);
    BeatScan bs({peaks.data()});
    bs.begin(rate);
    const int frames = (int)(v.size() / 2);
    for (int at = 0; at < frames; at += piece) bs.feed(&v[2 * at], frames - at < piece ? frames - at : piece);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)frames, bs.fed());
    return bs.finish();
}

// The beat nearest to t (seconds) in the grid, and how far t is from it (seconds).
static double offGrid(const BeatScan::Result& r, double t) {
    const double period = 60.0 / r.bpm;
    const double k = floor((t - r.firstBeat) / period + 0.5);
    return t - (r.firstBeat + k * period);
}

// halfToo: half the tempo is an answer as well (a fast track counted on every second beat).
static void checkTempo(double rate, double seconds, double bpm, double start, bool halfToo = false) {
    const BeatScan::Result r = scan(song(rate, seconds, bpm, start), (uint32_t)rate);
    char msg[160];
    snprintf(msg, sizeof(msg), "%.1f BPM at %.0f Hz from %.3f s: found %d, %.3f BPM, first beat %.3f s, confidence %.2f",
             bpm, rate, start, r.found, r.bpm, r.firstBeat, r.confidence);
    TEST_MESSAGE(msg);
    TEST_ASSERT_TRUE_MESSAGE(r.found, msg);
    TEST_ASSERT_TRUE_MESSAGE(r.confidence > 0.6f, msg);
    const bool half = halfToo && fabs(r.bpm - bpm / 2) < 0.01;
    TEST_ASSERT_TRUE_MESSAGE(fabs(r.bpm - bpm) < 0.02 || half, msg);
    // The grid lies on the beats, at the start and at the end (so it has not drifted).
    const double period = 60.0 / (half ? bpm / 2 : bpm);
    const double lastBeat = start + floor((seconds - 1.0 - start) / period) * period;
    snprintf(msg, sizeof(msg), "%.1f BPM: grid off by %.1f ms at the first beat, %.1f ms at the last", bpm,
             1000.0 * offGrid(r, start), 1000.0 * offGrid(r, lastBeat));
    TEST_MESSAGE(msg);
    TEST_ASSERT_TRUE_MESSAGE(fabs(offGrid(r, start)) < 0.012, msg);
    TEST_ASSERT_TRUE_MESSAGE(fabs(offGrid(r, lastBeat)) < 0.012, msg);
    // The first beat it names is the first one played (or within one beat before it).
    TEST_ASSERT_TRUE_MESSAGE(r.firstBeat <= start + 0.012 && r.firstBeat > start - period - 0.012, msg);
}

static void test_tempos_across_the_range() {
    checkTempo(48000, 60, 120.0, 0.5);
    checkTempo(48000, 60, 97.0, 0.21);
    checkTempo(48000, 60, 143.5, 1.337);
    checkTempo(48000, 60, 76.0, 0.05);
    checkTempo(48000, 60, 174.0, 0.4, true);  // (found as 87: kick and snare alternate)
}

static void test_other_sample_rates_and_a_long_track() {
    checkTempo(44100, 45, 104.0, 0.3);
    checkTempo(22050, 45, 128.0, 0.7);
    checkTempo(48000, 240, 91.3, 2.0);  // four minutes: the grid must still fit at the end
}

static void test_how_the_audio_arrives_does_not_matter() {
    const std::vector<int16_t> v = song(48000, 30, 110.0, 0.4);
    const BeatScan::Result a = scan(v, 48000, 1024), b = scan(v, 48000, 577), c = scan(v, 48000, 48000);
    TEST_ASSERT_EQUAL_FLOAT(a.bpm, b.bpm);
    TEST_ASSERT_EQUAL_FLOAT(a.bpm, c.bpm);
    TEST_ASSERT_EQUAL_FLOAT(a.firstBeat, b.firstBeat);
    TEST_ASSERT_EQUAL_FLOAT(a.firstBeat, c.firstBeat);
}

static void test_hats_alone_give_the_tempo_or_its_double() {
    // Only hats, on beats and eighths alike: the beat is not told from the eighth by level
    // alone, so either is an answer; the grid must fit the hats.
    const BeatScan::Result r = scan(song(48000, 40, 100.0, 0.5, 0.0, true), 48000);
    char msg[96];
    snprintf(msg, sizeof(msg), "%.2f BPM, confidence %.2f", r.bpm, r.confidence);
    TEST_ASSERT_TRUE_MESSAGE(r.found, msg);
    TEST_ASSERT_TRUE_MESSAGE(fabs(r.bpm - 100.0) < 0.05 || fabs(r.bpm - 200.0) < 0.1, msg);
    TEST_ASSERT_TRUE(fabs(offGrid(r, 0.5 + 30 * 0.6)) < 0.012);
}

static void test_no_steady_beat_is_said_so() {
    // Silence, a steady tone, and noise: nothing to find.
    std::vector<int16_t> quiet(2 * 48000 * 20, 0);
    TEST_ASSERT_FALSE(scan(quiet, 48000).found);
    std::vector<int16_t> tone(2 * 48000 * 20);
    for (size_t i = 0; i < tone.size() / 2; i++) tone[2 * i] = tone[2 * i + 1] = (int16_t)(8000.0 * sin(kPi2 * 330.0 * i / 48000.0));
    TEST_ASSERT_FALSE(scan(tone, 48000).found);
    // A band whose tempo swings by 10 % either way: no single tempo fits, and the
    // confidence says so (a steady one is above 0.6, see checkTempo()).
    const BeatScan::Result r = scan(song(48000, 60, 120.0, 0.5, 0.10), 48000);
    char msg[96];
    snprintf(msg, sizeof(msg), "wandering tempo: found %d, %.2f BPM, confidence %.2f", r.found, r.bpm, r.confidence);
    TEST_MESSAGE(msg);
    TEST_ASSERT_TRUE_MESSAGE(r.confidence < 0.5f, msg);
    // Too short to tell.
    TEST_ASSERT_FALSE(scan(song(48000, 3, 120.0, 0.5), 48000).found);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_tempos_across_the_range);
    RUN_TEST(test_other_sample_rates_and_a_long_track);
    RUN_TEST(test_how_the_audio_arrives_does_not_matter);
    RUN_TEST(test_hats_alone_give_the_tempo_or_its_double);
    RUN_TEST(test_no_steady_beat_is_said_so);
    return UNITY_END();
}
