// Host unit tests for dsp/player/StereoRing.h and TrackOut.h (the backing track's queue and
// its side of the audio interrupt).
// Run with: pio test -e native
#include <unity.h>
#include <cmath>
#include <cstdint>
#include <vector>
#include <dsp/player/StereoRing.h>
#include <dsp/player/TrackOut.h>

using dsp::StereoRing;
using dsp::TrackOut;

void setUp() {}
void tearDown() {}

// Frames whose left sample is `from`, from + 1, ... and whose right is its negative.
static std::vector<int16_t> ramp(int from, int n) {
    std::vector<int16_t> v(2 * n);
    for (int i = 0; i < n; i++) {
        v[2 * i] = (int16_t)(from + i);
        v[2 * i + 1] = (int16_t)-(from + i);
    }
    return v;
}

static void test_what_goes_in_comes_out_across_the_wrap() {
    std::vector<int16_t> mem(2 * 1024);
    StereoRing ring;
    ring.begin(mem.data(), 1024);
    TEST_ASSERT_EQUAL_UINT32(1024, ring.space());
    int next = 0, expect = 0;
    for (int round = 0; round < 50; round++) {
        const std::vector<int16_t> in = ramp(next, 300);
        TEST_ASSERT_EQUAL_UINT32(300, ring.write(in.data(), 300));
        next += 300;
        int16_t out[2 * 300];
        TEST_ASSERT_EQUAL_UINT32(300, ring.read(out, 300));
        for (int i = 0; i < 300; i++, expect++) {
            TEST_ASSERT_EQUAL_INT16((int16_t)expect, out[2 * i]);
            TEST_ASSERT_EQUAL_INT16((int16_t)-expect, out[2 * i + 1]);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(15000, ring.written());
    TEST_ASSERT_EQUAL_UINT32(15000, ring.taken());
}

static void test_a_full_queue_takes_no_more_and_an_empty_one_gives_none() {
    std::vector<int16_t> mem(2 * 256);
    StereoRing ring;
    ring.begin(mem.data(), 256);
    const std::vector<int16_t> in = ramp(0, 400);
    TEST_ASSERT_EQUAL_UINT32(256, ring.write(in.data(), 400));
    TEST_ASSERT_EQUAL_UINT32(0, ring.space());
    TEST_ASSERT_EQUAL_UINT32(0, ring.write(in.data(), 10));
    int16_t out[2 * 400];
    TEST_ASSERT_EQUAL_UINT32(256, ring.read(out, 400));
    TEST_ASSERT_EQUAL_UINT32(0, ring.read(out, 32));
    TEST_ASSERT_EQUAL_UINT32(0, ring.queued());
}

static void test_cut_keeps_the_next_frames_and_fades_into_the_new() {
    std::vector<int16_t> mem(2 * 8192);
    StereoRing ring;
    ring.begin(mem.data(), 8192);
    // 6000 frames of a constant 1000 queued; 100 already played.
    std::vector<int16_t> old(2 * 6000, 1000);
    ring.write(old.data(), 6000);
    int16_t skip[2 * 100];
    ring.read(skip, 100);
    TEST_ASSERT_EQUAL_UINT32(6000 - 100 - 2000, ring.cut(2000));
    TEST_ASSERT_EQUAL_UINT32(2000, ring.queued());
    // The new audio: a constant 5000.
    std::vector<int16_t> fresh(2 * 1000, 5000);
    ring.write(fresh.data(), 1000);
    std::vector<int16_t> out(2 * 3000);
    TEST_ASSERT_EQUAL_UINT32(3000, ring.read(out.data(), 3000));
    for (int i = 0; i < 2000; i++) TEST_ASSERT_EQUAL_INT16(1000, out[2 * i]);  // kept as it was
    // Then kFade frames that rise from the old level to the new, without a step.
    const int F = (int)StereoRing::kFade;
    int prev = 1000;
    for (int i = 0; i < F; i++) {
        const int v = out[2 * (2000 + i)];
        TEST_ASSERT_TRUE(v >= prev && v <= 5000);
        TEST_ASSERT_TRUE(v - prev <= (5000 - 1000) / F + 2);
        prev = v;
    }
    TEST_ASSERT_INT_WITHIN(20, 5000, prev);
    for (int i = 2000 + F; i < 3000; i++) TEST_ASSERT_EQUAL_INT16(5000, out[2 * i]);
}

static void test_cut_never_leaves_less_than_the_reader_may_be_taking() {
    std::vector<int16_t> mem(2 * 4096);
    StereoRing ring;
    ring.begin(mem.data(), 4096);
    std::vector<int16_t> in(2 * 4000, 1);
    ring.write(in.data(), 4000);
    ring.cut(0);
    TEST_ASSERT_EQUAL_UINT32(StereoRing::kMinKeep, ring.queued());
    TEST_ASSERT_EQUAL_UINT32(0, ring.cut(2000));  // less queued than asked to keep: nothing goes
    TEST_ASSERT_EQUAL_UINT32(StereoRing::kMinKeep, ring.queued());
}

static void test_idle_output_writes_nothing() {
    std::vector<int16_t> mem(2 * 1024);
    StereoRing ring;
    ring.begin(mem.data(), 1024);
    TrackOut out(48000.0f);
    out.begin(&ring);
    const std::vector<int16_t> in = ramp(100, 500);
    ring.write(in.data(), 500);
    float l[32], r[32];
    for (int i = 0; i < 32; i++) l[i] = r[i] = 7.0f;
    TEST_ASSERT_FALSE(out.process(l, r, 32));
    TEST_ASSERT_TRUE(out.idle());
    for (int i = 0; i < 32; i++) TEST_ASSERT_EQUAL_FLOAT(7.0f, l[i]);
    TEST_ASSERT_EQUAL_UINT32(500, ring.queued());  // not read
}

static void test_output_fades_in_plays_exactly_and_fades_out() {
    std::vector<int16_t> mem(2 * 4096);
    StereoRing ring;
    ring.begin(mem.data(), 4096);
    TrackOut out(48000.0f);
    out.begin(&ring);
    std::vector<int16_t> in(2 * 4000);
    for (int i = 0; i < 4000; i++) {
        in[2 * i] = 16384;
        in[2 * i + 1] = -8192;
    }
    ring.write(in.data(), 4000);
    out.setRun(true);
    TEST_ASSERT_FALSE(out.idle());
    float l[32], r[32];
    float prev = 0.0f;
    for (int b = 0; b < 15; b++) {  // 480 frames = 10 ms
        TEST_ASSERT_TRUE(out.process(l, r, 32));
        for (int i = 0; i < 32; i++) {
            TEST_ASSERT_TRUE(l[i] >= prev && l[i] - prev < 0.002f);  // rises without a step
            prev = l[i];
        }
    }
    TEST_ASSERT_TRUE(out.process(l, r, 32));
    for (int i = 0; i < 32; i++) {
        TEST_ASSERT_EQUAL_FLOAT(0.5f, l[i]);    // bit exact at full volume
        TEST_ASSERT_EQUAL_FLOAT(-0.25f, r[i]);
    }
    out.setGain(0.5f);
    for (int b = 0; b < 15; b++) out.process(l, r, 32);
    TEST_ASSERT_EQUAL_FLOAT(0.25f, l[31]);
    out.setRun(false);
    TEST_ASSERT_FALSE(out.idle());  // still fading
    int blocks = 0;
    while (out.process(l, r, 32)) blocks++;
    TEST_ASSERT_INT_WITHIN(1, 8, blocks);  // 5 ms from half volume
    TEST_ASSERT_EQUAL_FLOAT(0.0f, l[31]);
    TEST_ASSERT_TRUE(out.idle());
    TEST_ASSERT_EQUAL_UINT32(0, out.underruns());
}

static void test_a_gap_is_silence_and_counted_unless_the_track_is_ending() {
    std::vector<int16_t> mem(2 * 1024);
    StereoRing ring;
    ring.begin(mem.data(), 1024);
    TrackOut out(48000.0f);
    out.begin(&ring);
    std::vector<int16_t> in(2 * 16, 20000);
    ring.write(in.data(), 16);
    out.setRun(true);
    float l[32], r[32];
    TEST_ASSERT_TRUE(out.process(l, r, 32));
    for (int i = 16; i < 32; i++) TEST_ASSERT_EQUAL_FLOAT(0.0f, l[i]);
    TEST_ASSERT_EQUAL_UINT32(1, out.underruns());
    out.setEnding(true);
    TEST_ASSERT_TRUE(out.process(l, r, 32));
    TEST_ASSERT_EQUAL_UINT32(1, out.underruns());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_what_goes_in_comes_out_across_the_wrap);
    RUN_TEST(test_a_full_queue_takes_no_more_and_an_empty_one_gives_none);
    RUN_TEST(test_cut_keeps_the_next_frames_and_fades_into_the_new);
    RUN_TEST(test_cut_never_leaves_less_than_the_reader_may_be_taking);
    RUN_TEST(test_idle_output_writes_nothing);
    RUN_TEST(test_output_fades_in_plays_exactly_and_fades_out);
    RUN_TEST(test_a_gap_is_silence_and_counted_unless_the_track_is_ending);
    return UNITY_END();
}
