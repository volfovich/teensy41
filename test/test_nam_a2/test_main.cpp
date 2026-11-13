// Host tests for the NAM A2-Lite engine (dsp/nam/NamA2.h) and the .nam reader (dsp/nam/NamFile.h).
// Run with: pio test -e native   (from the project root; the fixtures are in test/fixtures/nam)
#include <unity.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include <dsp/ByteSource.h>
#include <dsp/nam/GoldenSignal.h>
#include <dsp/nam/NamA2.h>
#include <dsp/nam/NamFile.h>

using dsp::NamA2;
namespace nama2 = dsp::nama2;

static const char* const kModelPath = "test/fixtures/nam/A2.nam";
static const char* const kRefPath = "test/fixtures/nam/A2_lite_ref.f32";

static std::vector<uint8_t> g_file;        // A2.nam
static std::vector<float> g_weights;       // its Lite network
static std::vector<float> g_ref;           // NeuralAmpModelerCore's output on the golden signal
static std::vector<float> g_signal;
static dsp::NamInfo g_info;

static std::vector<uint8_t> readFile(const char* path) {
    std::vector<uint8_t> data;
    if (FILE* f = std::fopen(path, "rb")) {
        std::fseek(f, 0, SEEK_END);
        data.resize(static_cast<size_t>(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
        std::fclose(f);
    }
    return data;
}

template <int B>
static std::vector<float> render(const std::vector<float>& weights, const std::vector<float>& in) {
    std::vector<float> history(NamA2<B>::kHistoryFloats);
    auto nam = std::make_unique<NamA2<B>>();
    nam->begin(history.data());
    nam->setWeights(weights.data());
    const size_t n = in.size() / B * B;
    std::vector<float> out(n);
    for (size_t i = 0; i < n; i += B) nam->process(&in[i], &out[i]);
    return out;
}

void setUp() {}
void tearDown() {}

static void test_weight_count_matches_a2_lite() {
    TEST_ASSERT_EQUAL_INT(1871, nama2::kWeightCount);
}

static void test_reads_the_example_a2_file() {
    TEST_ASSERT_TRUE_MESSAGE(!g_file.empty(), "cannot open test/fixtures/nam/A2.nam (run from the project root)");
    dsp::MemorySource src(g_file.data(), static_cast<uint32_t>(g_file.size()));
    g_weights.assign(nama2::kWeightCount, 0.0f);
    const char* err = "";
    const bool ok = dsp::readNamA2Lite(src, g_weights.data(), &g_info, &err);
    TEST_ASSERT_TRUE_MESSAGE(ok, err);
    TEST_ASSERT_TRUE(g_info.container);
    TEST_ASSERT_EQUAL_FLOAT(48000.0f, g_info.sampleRate);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 14.76f, g_info.inputLevelDbu);
    bool anyNonZero = false;
    for (float w : g_weights) {
        TEST_ASSERT_TRUE(std::isfinite(w));
        anyNonZero = anyNonZero || w != 0.0f;
    }
    TEST_ASSERT_TRUE(anyNonZero);
}

static void test_rejects_files_that_are_not_a2() {
    float weights[nama2::kWeightCount];
    dsp::NamInfo info;
    const char* err = nullptr;

    const char notJson[] = "this is not a model file";
    dsp::MemorySource a(notJson, sizeof(notJson) - 1);
    TEST_ASSERT_FALSE(dsp::readNamA2Lite(a, weights, &info, &err));
    TEST_ASSERT_NOT_NULL(err);

    const char lstm[] = R"({"version":"0.5.4","architecture":"LSTM","config":{"hidden_size":16},"weights":[0.1,0.2],"sample_rate":48000})";
    dsp::MemorySource b(lstm, sizeof(lstm) - 1);
    TEST_ASSERT_FALSE(dsp::readNamA2Lite(b, weights, &info, &err));

    // The real file cut short at several points must fail cleanly, never crash.
    for (size_t cut : {size_t{0}, size_t{1}, size_t{100}, g_file.size() / 3, g_file.size() / 2, g_file.size() - 1}) {
        dsp::MemorySource c(g_file.data(), static_cast<uint32_t>(cut));
        TEST_ASSERT_FALSE(dsp::readNamA2Lite(c, weights, &info, &err));
    }
}

// The on-board check feeds golden_input.f32; it must be the signal the reference was made from.
static void test_golden_input_file_matches_the_generator() {
    const std::vector<uint8_t> bytes = readFile("test/fixtures/nam/golden_input.f32");
    TEST_ASSERT_EQUAL_size_t(g_signal.size() * sizeof(float), bytes.size());
    TEST_ASSERT_EQUAL_INT(0, std::memcmp(bytes.data(), g_signal.data(), bytes.size()));
}

// The engine against the reference implementation (NeuralAmpModelerCore) on the same model.
static void test_matches_the_reference_engine() {
    TEST_ASSERT_EQUAL_size_t_MESSAGE(nama2::kGoldenFrames, g_ref.size(), "cannot read A2_lite_ref.f32");
    const std::vector<float> out = render<32>(g_weights, g_signal);
    double err2 = 0.0, sig2 = 0.0, maxErr = 0.0, peak = 0.0;
    for (size_t i = nama2::kGoldenCompareFrom; i < out.size(); ++i) {
        const double d = static_cast<double>(out[i]) - static_cast<double>(g_ref[i]);
        err2 += d * d;
        sig2 += static_cast<double>(g_ref[i]) * static_cast<double>(g_ref[i]);
        maxErr = std::fmax(maxErr, std::fabs(d));
        peak = std::fmax(peak, std::fabs(static_cast<double>(g_ref[i])));
    }
    const double db = 10.0 * std::log10(sig2 / (err2 + 1e-30));
    std::printf("    max error %.2e, output peak %.3f, error %.0f dB below the signal\n", maxErr, peak, db);
    TEST_ASSERT_TRUE_MESSAGE(peak > 0.05, "the reference is silent");
    TEST_ASSERT_TRUE_MESSAGE(maxErr < 1e-5, "max error against NeuralAmpModelerCore too large");
    TEST_ASSERT_TRUE_MESSAGE(db > 100.0, "error is less than 100 dB below the signal");
}

// Every block size must give the same audio (the block size only changes how work is grouped).
template <int B>
static void checkBlockSize(const std::vector<float>& ref32) {
    const std::vector<float> out = render<B>(g_weights, g_signal);
    double maxErr = 0.0;
    const size_t n = out.size() < ref32.size() ? out.size() : ref32.size();
    for (size_t i = 0; i < n; ++i) maxErr = std::fmax(maxErr, std::fabs(static_cast<double>(out[i]) - static_cast<double>(ref32[i])));
    std::printf("    B=%3d: max difference from B=32 %.2e, history %d KB\n", B, maxErr,
                static_cast<int>(NamA2<B>::kHistoryFloats * sizeof(float) / 1024));
    TEST_ASSERT_TRUE(maxErr < 1e-5);
}

static void test_block_sizes_agree() {
    const std::vector<float> ref32 = render<32>(g_weights, g_signal);
    checkBlockSize<8>(ref32);
    checkBlockSize<16>(ref32);
    checkBlockSize<48>(ref32);
    checkBlockSize<64>(ref32);
    checkBlockSize<128>(ref32);
}

static void test_reset_restores_the_initial_state() {
    constexpr int B = 32;
    std::vector<float> history(NamA2<B>::kHistoryFloats);
    auto nam = std::make_unique<NamA2<B>>();
    nam->begin(history.data());
    nam->setWeights(g_weights.data());
    const size_t n = 8192;
    std::vector<float> first(n), again(n);
    for (size_t i = 0; i < n; i += B) nam->process(&g_signal[i], &first[i]);
    nam->reset();
    for (size_t i = 0; i < n; i += B) nam->process(&g_signal[i], &again[i]);
    TEST_ASSERT_EQUAL_INT(0, std::memcmp(first.data(), again.data(), n * sizeof(float)));
}

static void test_in_place_processing() {
    constexpr int B = 32;
    const std::vector<float> separate = render<B>(g_weights, g_signal);
    std::vector<float> history(NamA2<B>::kHistoryFloats);
    auto nam = std::make_unique<NamA2<B>>();
    nam->begin(history.data());
    nam->setWeights(g_weights.data());
    std::vector<float> buf(g_signal.begin(), g_signal.begin() + 4096);
    for (size_t i = 0; i < buf.size(); i += B) nam->process(&buf[i], &buf[i]);
    TEST_ASSERT_EQUAL_INT(0, std::memcmp(buf.data(), separate.data(), buf.size() * sizeof(float)));
}

int main() {
    g_file = readFile(kModelPath);
    const std::vector<uint8_t> refBytes = readFile(kRefPath);
    g_ref.resize(refBytes.size() / sizeof(float));
    std::memcpy(g_ref.data(), refBytes.data(), g_ref.size() * sizeof(float));
    g_signal.resize(nama2::kGoldenFrames);
    nama2::goldenSignal(g_signal.data());

    UNITY_BEGIN();
    RUN_TEST(test_weight_count_matches_a2_lite);
    RUN_TEST(test_reads_the_example_a2_file);
    RUN_TEST(test_rejects_files_that_are_not_a2);
    RUN_TEST(test_golden_input_file_matches_the_generator);
    RUN_TEST(test_matches_the_reference_engine);
    RUN_TEST(test_block_sizes_agree);
    RUN_TEST(test_reset_restores_the_initial_state);
    RUN_TEST(test_in_place_processing);
    return UNITY_END();
}
