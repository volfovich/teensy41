// Host tests for the IR loading pipeline (dsp/conv/IRLoader.h): WAV formats, channel handling,
// resampling to 48 kHz, trimming, truncation and normalisation. Same checks as
// teensy-ir-loader's test/host/test_loader.py, rewritten in C++ so they run under
// `pio test -e native` without numpy.
#include <unity.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include <dsp/ByteSource.h>
#include <dsp/conv/IRLoader.h>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kMaxLen = 8192;

void put(std::vector<uint8_t>& b, const void* p, size_t n) {
    const uint8_t* c = static_cast<const uint8_t*>(p);
    b.insert(b.end(), c, c + n);
}
void put32(std::vector<uint8_t>& b, uint32_t v) { put(b, &v, 4); }
void put16(std::vector<uint8_t>& b, uint16_t v) { put(b, &v, 2); }

// data: interleaved doubles in [-1, 1). fmt 1 = PCM, 3 = float.
std::vector<uint8_t> wav(const std::vector<double>& data, int channels, uint32_t rate, int fmt, int bytesPer,
                         bool extensible = false, bool junk = false) {
    std::vector<uint8_t> raw;
    for (double v : data) {
        if (fmt == 3) {
            if (bytesPer == 4) { const float f = static_cast<float>(v); put(raw, &f, 4); }
            else put(raw, &v, 8);
        } else if (bytesPer == 1) {
            raw.push_back(static_cast<uint8_t>(std::fmin(std::fmax(std::round(v * 128.0 + 128.0), 0.0), 255.0)));
        } else {
            const double scale = std::ldexp(1.0, 8 * bytesPer - 1);
            const int64_t q = static_cast<int64_t>(std::fmin(std::fmax(std::round(v * scale), -scale), scale - 1.0));
            put(raw, &q, static_cast<size_t>(bytesPer));   // little-endian host
        }
    }
    std::vector<uint8_t> fmtChunk;
    const uint16_t tag = extensible ? 0xFFFE : static_cast<uint16_t>(fmt);
    put16(fmtChunk, tag);
    put16(fmtChunk, static_cast<uint16_t>(channels));
    put32(fmtChunk, rate);
    put32(fmtChunk, rate * static_cast<uint32_t>(channels * bytesPer));
    put16(fmtChunk, static_cast<uint16_t>(channels * bytesPer));
    put16(fmtChunk, static_cast<uint16_t>(8 * bytesPer));
    if (extensible) {
        put16(fmtChunk, 22);
        put16(fmtChunk, static_cast<uint16_t>(8 * bytesPer));
        put32(fmtChunk, 0);
        const uint8_t guidTail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        put16(fmtChunk, static_cast<uint16_t>(fmt));
        put(fmtChunk, guidTail, sizeof(guidTail));
    }
    std::vector<uint8_t> body;
    put(body, "WAVE", 4);
    if (junk) {   // an odd-sized chunk (padded to even) before fmt, and a LIST after it
        put(body, "JUNK", 4); put32(body, 5); put(body, "abcde\0", 6);
    }
    put(body, "fmt ", 4); put32(body, static_cast<uint32_t>(fmtChunk.size())); put(body, fmtChunk.data(), fmtChunk.size());
    if (junk) { put(body, "LIST", 4); put32(body, 4); put(body, "INFO", 4); }
    put(body, "data", 4); put32(body, static_cast<uint32_t>(raw.size())); put(body, raw.data(), raw.size());
    std::vector<uint8_t> out;
    put(out, "RIFF", 4); put32(out, static_cast<uint32_t>(body.size())); put(out, body.data(), body.size());
    return out;
}

struct Loaded {
    bool ok = false;
    const char* err = "";
    dsp::IRPrepResult res;
    std::vector<float> ir;
};

Loaded load(const std::vector<uint8_t>& file, dsp::Normalize norm = dsp::Normalize::kNone, bool trim = false,
            bool mix = false, bool exactKernel = false, int maxLen = kMaxLen) {
    Loaded l;
    dsp::MemorySource src(file.data(), static_cast<uint32_t>(file.size()));
    dsp::IRPrepOptions opt;
    opt.normalize = norm;
    if (!trim) { opt.trimLeadDb = 0.0f; opt.trimTailDb = 0.0f; }
    if (mix) opt.channels = dsp::ChannelMode::kMix;
    std::vector<float> scratch(2048), table(14336), out(static_cast<size_t>(maxLen));   // the firmware's sizes
    if (!exactKernel) { opt.kernelTable = table.data(); opt.kernelTableCap = static_cast<int>(table.size()); }
    l.ok = dsp::prepareIR(src, opt, scratch.data(), static_cast<int>(scratch.size()), out.data(), maxLen, l.res, &l.err);
    if (l.ok) l.ir.assign(out.begin(), out.begin() + l.res.length);
    return l;
}

// A band-limited test "IR", evaluated at any rate: sines up to 3 kHz under a smooth 40 ms
// window. It starts and ends at zero with no step, so sampling it at different rates gives
// the same waveform (a signal with a sharp start would alias differently at each rate).
double shape(double t) {
    if (t < 0.0 || t > 0.04) return 0.0;
    const double w = std::sin(kPi * t / 0.04);
    return w * w * (0.5 * std::sin(2 * kPi * 440.0 * t) + 0.3 * std::sin(2 * kPi * 1500.0 * t + 0.5) +
                    0.15 * std::sin(2 * kPi * 3000.0 * t + 1.0));
}
std::vector<double> shapeAt(double rate, int frames) {
    std::vector<double> x(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) x[static_cast<size_t>(i)] = shape(i / rate);
    return x;
}

} // namespace

void setUp() {}
void tearDown() {}

static void test_decodes_every_sample_format() {
    const std::vector<double> x = shapeAt(48000.0, 3000);
    struct Fmt { int fmt, bytes; bool ext; double tol; const char* name; };
    const Fmt fmts[] = {{1, 1, false, 1.0 / 100, "PCM 8"},   {1, 2, false, 1.0 / 30000, "PCM 16"}, {1, 3, false, 1e-6, "PCM 24"},
                        {1, 4, false, 1e-6, "PCM 32"},      {3, 4, false, 1e-7, "float 32"},     {3, 8, false, 1e-7, "float 64"},
                        {1, 3, true, 1e-6, "PCM 24 extensible"}, {3, 4, true, 1e-7, "float 32 extensible"}};
    for (const Fmt& f : fmts) {
        const Loaded l = load(wav(x, 1, 48000, f.fmt, f.bytes, f.ext));
        TEST_ASSERT_TRUE_MESSAGE(l.ok, f.name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(3000, l.res.length, f.name);
        TEST_ASSERT_FALSE_MESSAGE(l.res.resampled, f.name);
        double worst = 0.0;
        for (size_t i = 0; i < x.size(); ++i) worst = std::fmax(worst, std::fabs(static_cast<double>(l.ir[i]) - x[i]));
        TEST_ASSERT_TRUE_MESSAGE(worst <= f.tol, f.name);
    }
}

static void test_stereo_first_channel_and_mix() {
    const std::vector<double> left = shapeAt(48000.0, 2000);
    std::vector<double> inter;
    for (double v : left) { inter.push_back(v); inter.push_back(v * 0.5); }
    const Loaded first = load(wav(inter, 2, 48000, 3, 4));
    TEST_ASSERT_TRUE(first.ok);
    const Loaded mixed = load(wav(inter, 2, 48000, 3, 4), dsp::Normalize::kNone, false, true);
    TEST_ASSERT_TRUE(mixed.ok);
    for (size_t i = 0; i < left.size(); ++i) {
        TEST_ASSERT_FLOAT_WITHIN(1e-6f, static_cast<float>(left[i]), first.ir[i]);
        TEST_ASSERT_FLOAT_WITHIN(1e-6f, static_cast<float>(left[i] * 0.75), mixed.ir[i]);
    }
}

static void test_skips_other_chunks_and_rejects_non_wav() {
    const Loaded l = load(wav(shapeAt(48000.0, 3000), 1, 48000, 1, 2, false, true));
    TEST_ASSERT_TRUE(l.ok);
    TEST_ASSERT_EQUAL_INT(3000, l.res.length);

    const std::vector<uint8_t> junk(500, 0x41);
    const Loaded bad = load(junk);
    TEST_ASSERT_FALSE(bad.ok);
    TEST_ASSERT_NOT_NULL(bad.err);
}

// Files at other rates come out as the same waveform sampled at 48 kHz, scaled by
// fsIn / 48000: the loader keeps the filter gain of an impulse response, not its sample values.
static void test_resamples_to_48k() {
    const std::vector<double> want = shapeAt(48000.0, 2400);
    for (int exact = 0; exact < 2; ++exact) {
        // The accuracy check uses the original test's rates. (8 kHz files load, see below, but
        // this signal's 3 kHz component sits in that rate's filter transition band.)
        for (double rate : {22050.0, 32000.0, 44100.0, 88200.0, 96000.0, 192000.0}) {
            const int frames = static_cast<int>(0.05 * rate);
            const Loaded l = load(wav(shapeAt(rate, frames), 1, static_cast<uint32_t>(rate), 3, 4), dsp::Normalize::kNone,
                                  false, false, exact != 0);
            TEST_ASSERT_TRUE(l.ok);
            TEST_ASSERT_TRUE(l.res.resampled);
            double err2 = 0.0, sig2 = 0.0;
            const int n = static_cast<int>(l.ir.size()) < 2000 ? static_cast<int>(l.ir.size()) : 2000;
            for (int i = 0; i < n; ++i) {
                const double w = want[static_cast<size_t>(i)] * rate / 48000.0;
                const double d = static_cast<double>(l.ir[static_cast<size_t>(i)]) - w;
                err2 += d * d;
                sig2 += w * w;
            }
            char msg[64];
            snprintf(msg, sizeof(msg), "%.0f Hz, %s kernel", rate, exact ? "exact" : "table");
            TEST_ASSERT_TRUE_MESSAGE(n >= 2000, msg);
            TEST_ASSERT_TRUE_MESSAGE(std::sqrt(err2 / sig2) < 5e-3, msg);
        }
    }
}

static void test_loads_the_lowest_supported_rate() {
    const Loaded l = load(wav(shapeAt(8000.0, 400), 1, 8000, 1, 2));
    TEST_ASSERT_TRUE(l.ok);
    TEST_ASSERT_TRUE(l.res.resampled);
    TEST_ASSERT_INT_WITHIN(60, 2400, l.res.length);   // 50 ms at 48 kHz
}

static void test_removes_content_above_the_new_nyquist() {
    std::vector<double> x(9600);
    for (size_t i = 0; i < x.size(); ++i) x[i] = 0.5 * std::sin(2 * kPi * 30000.0 * static_cast<double>(i) / 96000.0);
    const Loaded l = load(wav(x, 1, 96000, 3, 4));
    TEST_ASSERT_TRUE(l.ok);
    float peak = 0.0f;
    for (size_t i = 200; i + 200 < l.ir.size(); ++i) peak = std::fmax(peak, std::fabs(l.ir[i]));
    TEST_ASSERT_TRUE(peak < 1e-3f);
}

static void test_truncates_long_files_with_a_fade() {
    std::vector<double> x(96000);
    uint32_t rng = 1;
    for (double& v : x) { rng = rng * 1664525u + 1013904223u; v = (static_cast<double>(rng >> 8) / 8388608.0 - 1.0) * 0.3; }
    const Loaded l = load(wav(x, 1, 48000, 3, 4));
    TEST_ASSERT_TRUE(l.ok);
    TEST_ASSERT_TRUE(l.res.truncated);
    TEST_ASSERT_TRUE(l.res.length <= kMaxLen && l.res.length >= kMaxLen - 256);
    float peak = 0.0f;
    for (float v : l.ir) peak = std::fmax(peak, std::fabs(v));
    TEST_ASSERT_TRUE(std::fabs(l.ir.back()) < peak * 1e-2f);

    // High-rate files are streamed through the small scratch window.
    const Loaded hi = load(wav(std::vector<double>(x.begin(), x.end()), 1, 384000, 3, 4));
    TEST_ASSERT_TRUE(hi.ok);
}

static void test_trims_leading_silence_and_silent_tail() {
    // A body that starts at full level, so the trim point is unambiguous.
    std::vector<double> body(1500);
    for (size_t i = 0; i < body.size(); ++i)
        body[i] = 0.8 * std::exp(-static_cast<double>(i) / 150.0) * std::cos(2 * kPi * 1000.0 * static_cast<double>(i) / 48000.0);
    std::vector<double> x(100, 0.0);
    x.insert(x.end(), body.begin(), body.end());
    x.resize(6000, 0.0);
    const Loaded l = load(wav(x, 1, 48000, 3, 4), dsp::Normalize::kNone, true);
    TEST_ASSERT_TRUE(l.ok);
    TEST_ASSERT_EQUAL_INT(100, l.res.leadTrimmed);
    TEST_ASSERT_TRUE(l.res.length <= 2000);
    for (int i = 0; i < 50; ++i) TEST_ASSERT_FLOAT_WITHIN(1e-6f, static_cast<float>(body[static_cast<size_t>(i)]), l.ir[static_cast<size_t>(i)]);
}

static void test_normalises_and_rejects_silence() {
    const std::vector<uint8_t> file = wav(shapeAt(48000.0, 3000), 1, 48000, 3, 4);
    const Loaded energy = load(file, dsp::Normalize::kEnergy);
    double e = 0.0;
    for (float v : energy.ir) e += static_cast<double>(v) * static_cast<double>(v);
    TEST_ASSERT_TRUE(std::fabs(e - 1.0) < 1e-4);

    const Loaded peakN = load(file, dsp::Normalize::kPeak);
    float peak = 0.0f;
    for (float v : peakN.ir) peak = std::fmax(peak, std::fabs(v));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, peak);

    const Loaded silent = load(wav(std::vector<double>(1000, 0.0), 1, 48000, 1, 2), dsp::Normalize::kEnergy, true);
    TEST_ASSERT_FALSE(silent.ok);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_decodes_every_sample_format);
    RUN_TEST(test_stereo_first_channel_and_mix);
    RUN_TEST(test_skips_other_chunks_and_rejects_non_wav);
    RUN_TEST(test_resamples_to_48k);
    RUN_TEST(test_loads_the_lowest_supported_rate);
    RUN_TEST(test_removes_content_above_the_new_nyquist);
    RUN_TEST(test_truncates_long_files_with_a_fade);
    RUN_TEST(test_trims_leading_silence_and_silent_tail);
    RUN_TEST(test_normalises_and_rejects_silence);
    return UNITY_END();
}
