// Host unit tests for dsp/player/ResampledSource.h (a backing track brought to the output's
// sample rate a piece at a time).
// Run with: pio test -e native
#include <unity.h>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <dsp/player/ResampledSource.h>

using dsp::ResampledSource;

void setUp() {}
void tearDown() {}

static const double kPi2 = 6.283185307179586;

// A track held in memory.
struct MemTrack : dsp::TrackSource {
    uint32_t rate;
    std::vector<int16_t> v;  // interleaved
    uint32_t pos = 0;
    int seeks = 0;
    explicit MemTrack(uint32_t r) : rate(r) {}
    uint32_t sampleRate() const override { return rate; }
    uint32_t frames() const override { return (uint32_t)(v.size() / 2); }
    int read(int16_t* dst, int maxFrames) override {
        int got = 0;
        for (; got < maxFrames && pos < frames(); got++, pos++) {
            dst[2 * got] = v[2 * pos];
            dst[2 * got + 1] = v[2 * pos + 1];
        }
        return got;
    }
    bool seek(uint32_t frame) override {
        pos = frame > frames() ? frames() : frame;
        seeks++;
        return true;
    }
};

// Left: a sine of hzL at ampL (of full scale); right: hzR at ampR.
static MemTrack tones(uint32_t rate, double seconds, double hzL, double ampL, double hzR = 0, double ampR = 0) {
    MemTrack t(rate);
    const int n = (int)(rate * seconds);
    t.v.resize(2 * n);
    for (int i = 0; i < n; i++) {
        t.v[2 * i] = (int16_t)lrint(ampL * 32767.0 * sin(kPi2 * hzL * i / rate));
        t.v[2 * i + 1] = (int16_t)lrint(ampR * 32767.0 * sin(kPi2 * hzR * i / rate));
    }
    return t;
}

struct Mem {
    std::vector<float> table = std::vector<float>(ResampledSource::kTableFloats);
    std::vector<float> window = std::vector<float>(2 * ResampledSource::kWindowFrames);
    std::vector<int16_t> raw = std::vector<int16_t>(2 * ResampledSource::kReadFrames);
    ResampledSource::Memory get() { return {table.data(), window.data(), raw.data()}; }
};

static std::vector<int16_t> readAll(ResampledSource& s, int piece = 500) {
    std::vector<int16_t> out, buf(2 * piece);
    for (;;) {
        const int got = s.read(buf.data(), piece);
        out.insert(out.end(), buf.begin(), buf.begin() + 2 * got);
        if (got < piece) break;
    }
    return out;
}

// The amplitude (of full scale) of `hz` in one channel of frames [from, to).
static double toneAmp(const std::vector<int16_t>& v, int ch, size_t from, size_t to, double hz, double rate) {
    double re = 0, im = 0;
    for (size_t i = from; i < to; i++) {
        const double x = v[2 * i + ch] / 32767.0, ph = kPi2 * hz * (double)i / rate;
        re += x * cos(ph);
        im += x * sin(ph);
    }
    return 2.0 * sqrt(re * re + im * im) / (double)(to - from);
}

static void test_44k_to_48k_is_the_same_tone_at_the_new_rate() {
    MemTrack in = tones(44100, 1.0, 1000.0, 0.5, 3000.0, 0.25);
    Mem mem;
    ResampledSource rs(mem.get(), 48000);
    TEST_ASSERT_TRUE(rs.setSource(&in));
    TEST_ASSERT_EQUAL_UINT32(48000, rs.sampleRate());
    TEST_ASSERT_EQUAL_UINT32(48000, rs.frames());
    const std::vector<int16_t> out = readAll(rs);
    TEST_ASSERT_EQUAL_UINT32(48000, (uint32_t)(out.size() / 2));
    // Sample for sample what the tone is at 48 kHz (away from the two ends).
    double err = 0, sig = 0;
    for (int k = 2000; k < 46000; k++) {
        const double wantL = 0.5 * 32767.0 * sin(kPi2 * 1000.0 * k / 48000.0);
        const double wantR = 0.25 * 32767.0 * sin(kPi2 * 3000.0 * k / 48000.0);
        err += (out[2 * k] - wantL) * (out[2 * k] - wantL) + (out[2 * k + 1] - wantR) * (out[2 * k + 1] - wantR);
        sig += wantL * wantL + wantR * wantR;
    }
    const double snr = 10.0 * log10(sig / err);
    char msg[64];
    snprintf(msg, sizeof(msg), "signal to error %.1f dB", snr);
    TEST_ASSERT_TRUE_MESSAGE(snr > 60.0, msg);
}

static void test_the_level_holds_up_to_15_khz_and_nothing_folds_back() {
    Mem mem;
    for (double hz : {100.0, 5000.0, 10000.0, 15000.0}) {
        MemTrack in = tones(44100, 0.5, hz, 0.5);
        ResampledSource rs(mem.get(), 48000);
        TEST_ASSERT_TRUE(rs.setSource(&in));
        const std::vector<int16_t> out = readAll(rs);
        TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.5f, (float)toneAmp(out, 0, 4000, 20000, hz, 48000.0));
        TEST_ASSERT_TRUE(toneAmp(out, 1, 4000, 20000, hz, 48000.0) < 1e-4);  // the silent side stays silent
    }
    // An 18 kHz tone at 44.1 kHz has an image at 26.1 kHz, which at 48 kHz would come out
    // at 21.9 kHz: it must be gone (60 dB down). The tone itself is where the filter
    // starts to close: within 3 dB.
    MemTrack in = tones(44100, 0.5, 18000.0, 0.5);
    ResampledSource rs(mem.get(), 48000);
    TEST_ASSERT_TRUE(rs.setSource(&in));
    const std::vector<int16_t> out = readAll(rs);
    char msg[96];
    snprintf(msg, sizeof(msg), "18 kHz %.4f, image %.5f", toneAmp(out, 0, 4000, 20000, 18000.0, 48000.0),
             toneAmp(out, 0, 4000, 20000, 21900.0, 48000.0));
    TEST_MESSAGE(msg);
    TEST_ASSERT_TRUE_MESSAGE(toneAmp(out, 0, 4000, 20000, 18000.0, 48000.0) > 0.35, msg);
    TEST_ASSERT_TRUE_MESSAGE(toneAmp(out, 0, 4000, 20000, 21900.0, 48000.0) < 0.0005, msg);
}

static void test_a_steady_level_stays_exactly() {
    MemTrack in(44100);
    in.v.assign(2 * 5000, 12345);
    Mem mem;
    ResampledSource rs(mem.get(), 48000);
    TEST_ASSERT_TRUE(rs.setSource(&in));
    const std::vector<int16_t> out = readAll(rs);
    for (size_t k = 100; k < out.size() / 2 - 100; k++) TEST_ASSERT_INT16_WITHIN(1, 12345, out[2 * k]);
}

static void test_a_seek_gives_the_samples_a_read_from_the_start_gives() {
    MemTrack in = tones(44100, 1.0, 440.0, 0.4, 1234.0, 0.3);
    Mem mem;
    ResampledSource rs(mem.get(), 48000);
    TEST_ASSERT_TRUE(rs.setSource(&in));
    const std::vector<int16_t> all = readAll(rs, 777);
    for (uint32_t k : {0u, 1u, 15u, 16u, 17u, 1000u, 23456u, 47000u, 47999u}) {
        TEST_ASSERT_TRUE(rs.seek(k));
        int16_t got[2 * 900];
        const int n = rs.read(got, 900);
        const int expect = 48000 - (int)k < 900 ? 48000 - (int)k : 900;
        TEST_ASSERT_EQUAL_INT(expect, n);
        TEST_ASSERT_EQUAL_INT16_ARRAY(&all[2 * k], got, 2 * n);
    }
    TEST_ASSERT_TRUE(rs.seek(99999));
    int16_t one[2];
    TEST_ASSERT_EQUAL_INT(0, rs.read(one, 1));
}

static void test_other_rates() {
    Mem mem;
    struct Case {
        uint32_t rate;
        double hz;
    };
    for (const Case c : {Case{22050, 1000.0}, Case{32000, 2000.0}, Case{96000, 1000.0}, Case{88200, 5000.0}, Case{8000, 500.0}}) {
        MemTrack in = tones(c.rate, 0.5, c.hz, 0.5);
        ResampledSource rs(mem.get(), 48000);
        TEST_ASSERT_TRUE(rs.setSource(&in));
        TEST_ASSERT_EQUAL_UINT32(24000, rs.frames());
        const std::vector<int16_t> out = readAll(rs);
        TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.5f, (float)toneAmp(out, 0, 4000, 20000, c.hz, 48000.0));
    }
    // Going down from 96 kHz: a 30 kHz tone, which 48 kHz cannot hold, must not fold to 18 kHz.
    MemTrack high = tones(96000, 0.5, 30000.0, 0.5);
    ResampledSource rs(mem.get(), 48000);
    TEST_ASSERT_TRUE(rs.setSource(&high));
    const std::vector<int16_t> out = readAll(rs);
    TEST_ASSERT_TRUE(toneAmp(out, 0, 4000, 20000, 18000.0, 48000.0) < 0.001);
    // Rates too far from the output's are refused.
    MemTrack slow(4000), fast(192000);
    slow.v.assign(200, 0);
    fast.v.assign(200, 0);
    TEST_ASSERT_FALSE(rs.setSource(&slow));
    TEST_ASSERT_FALSE(rs.setSource(&fast));
    TEST_ASSERT_EQUAL_UINT32(0, rs.frames());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_44k_to_48k_is_the_same_tone_at_the_new_rate);
    RUN_TEST(test_the_level_holds_up_to_15_khz_and_nothing_folds_back);
    RUN_TEST(test_a_steady_level_stays_exactly);
    RUN_TEST(test_a_seek_gives_the_samples_a_read_from_the_start_gives);
    RUN_TEST(test_other_rates);
    return UNITY_END();
}
