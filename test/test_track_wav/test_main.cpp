// Host unit tests for dsp/player/WavStream.h (a WAV file read a piece at a time as 16-bit
// stereo, for the backing-track player).
// Run with: pio test -e native
#include <unity.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include <dsp/ByteSource.h>
#include <dsp/player/WavStream.h>

using dsp::MemorySource;
using dsp::WavStream;

void setUp() {}
void tearDown() {}

static void put16(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)x);
    v.push_back((uint8_t)(x >> 8));
}
static void put32(std::vector<uint8_t>& v, uint32_t x) {
    put16(v, x);
    put16(v, x >> 16);
}

// A WAV file around `data` (format 1 = PCM, 3 = float), with a LIST chunk before the data
// as recording programs write it.
static std::vector<uint8_t> wav(int format, int channels, int bits, uint32_t rate, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> v;
    const int align = channels * bits / 8;
    v.insert(v.end(), {'R', 'I', 'F', 'F'});
    put32(v, 4 + 24 + 12 + 8 + (uint32_t)data.size());
    v.insert(v.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(v, 16);
    put16(v, (uint32_t)format);
    put16(v, (uint32_t)channels);
    put32(v, rate);
    put32(v, rate * (uint32_t)align);
    put16(v, (uint32_t)align);
    put16(v, (uint32_t)bits);
    v.insert(v.end(), {'L', 'I', 'S', 'T'});
    put32(v, 4);
    v.insert(v.end(), {'I', 'N', 'F', 'O'});
    v.insert(v.end(), {'d', 'a', 't', 'a'});
    put32(v, (uint32_t)data.size());
    v.insert(v.end(), data.begin(), data.end());
    return v;
}

static uint8_t g_scratch[600];  // not a multiple of every frame size, on purpose

static void test_16_bit_stereo_is_read_as_it_is() {
    std::vector<uint8_t> data;
    for (int i = 0; i < 5000; i++) {
        put16(data, (uint16_t)(int16_t)(i - 2500));
        put16(data, (uint16_t)(int16_t)(2500 - i));
    }
    const std::vector<uint8_t> file = wav(1, 2, 16, 44100, data);
    MemorySource src(file.data(), (uint32_t)file.size());
    WavStream w(g_scratch, sizeof(g_scratch));
    const char* err = "";
    TEST_ASSERT_TRUE_MESSAGE(w.open(src, &err), err);
    TEST_ASSERT_EQUAL_UINT32(44100, w.sampleRate());
    TEST_ASSERT_EQUAL_UINT32(5000, w.frames());
    std::vector<int16_t> out(2 * 6000);
    int total = 0;
    for (;;) {  // in pieces
        const int got = w.read(out.data() + 2 * total, 700);
        total += got;
        if (got < 700) break;
    }
    TEST_ASSERT_EQUAL_INT(5000, total);
    for (int i = 0; i < 5000; i++) {
        TEST_ASSERT_EQUAL_INT16((int16_t)(i - 2500), out[2 * i]);
        TEST_ASSERT_EQUAL_INT16((int16_t)(2500 - i), out[2 * i + 1]);
    }
    TEST_ASSERT_EQUAL_INT(0, w.read(out.data(), 10));  // at the end
}

static void test_seek_lands_on_the_frame() {
    std::vector<uint8_t> data;
    for (int i = 0; i < 3000; i++) {
        put16(data, (uint16_t)i);
        put16(data, (uint16_t)i);
    }
    const std::vector<uint8_t> file = wav(1, 2, 16, 48000, data);
    MemorySource src(file.data(), (uint32_t)file.size());
    WavStream w(g_scratch, sizeof(g_scratch));
    const char* err = "";
    TEST_ASSERT_TRUE(w.open(src, &err));
    int16_t out[2 * 8];
    TEST_ASSERT_TRUE(w.seek(1234));
    TEST_ASSERT_EQUAL_INT(8, w.read(out, 8));
    TEST_ASSERT_EQUAL_INT16(1234, out[0]);
    TEST_ASSERT_EQUAL_INT16(1241, out[14]);
    TEST_ASSERT_TRUE(w.seek(2996));
    TEST_ASSERT_EQUAL_INT(4, w.read(out, 8));  // only 4 left
    TEST_ASSERT_EQUAL_INT16(2999, out[6]);
    TEST_ASSERT_TRUE(w.seek(99999));  // past the end: the end
    TEST_ASSERT_EQUAL_INT(0, w.read(out, 8));
}

static void test_mono_goes_to_both_sides_and_24_bits_keep_the_top_16() {
    std::vector<uint8_t> data;
    for (int i = 0; i < 1000; i++) {
        const int32_t s = (i - 500) * 4096 + 77;  // 24-bit; the low byte is dropped
        data.push_back((uint8_t)s);
        data.push_back((uint8_t)(s >> 8));
        data.push_back((uint8_t)(s >> 16));
    }
    const std::vector<uint8_t> file = wav(1, 1, 24, 48000, data);
    MemorySource src(file.data(), (uint32_t)file.size());
    WavStream w(g_scratch, sizeof(g_scratch));
    const char* err = "";
    TEST_ASSERT_TRUE_MESSAGE(w.open(src, &err), err);
    std::vector<int16_t> out(2 * 1000);
    TEST_ASSERT_EQUAL_INT(1000, w.read(out.data(), 1000));
    for (int i = 0; i < 1000; i++) {
        const int16_t want = (int16_t)(((i - 500) * 4096 + 77) >> 8);
        TEST_ASSERT_EQUAL_INT16(want, out[2 * i]);
        TEST_ASSERT_EQUAL_INT16(want, out[2 * i + 1]);
    }
}

static void test_float_is_scaled_and_clipped_and_extra_channels_are_dropped() {
    const float frames[][3] = {{0.5f, -0.25f, 0.9f}, {1.5f, -3.0f, 0.9f}, {0.0f, 1.0f, 0.9f}};
    std::vector<uint8_t> data(sizeof(frames));
    memcpy(data.data(), frames, sizeof(frames));
    const std::vector<uint8_t> file = wav(3, 3, 32, 48000, data);
    MemorySource src(file.data(), (uint32_t)file.size());
    WavStream w(g_scratch, sizeof(g_scratch));
    const char* err = "";
    TEST_ASSERT_TRUE_MESSAGE(w.open(src, &err), err);
    int16_t out[2 * 3];
    TEST_ASSERT_EQUAL_INT(3, w.read(out, 3));
    TEST_ASSERT_EQUAL_INT16(16384, out[0]);
    TEST_ASSERT_EQUAL_INT16(-8192, out[1]);
    TEST_ASSERT_EQUAL_INT16(32767, out[2]);
    TEST_ASSERT_EQUAL_INT16(-32768, out[3]);
    TEST_ASSERT_EQUAL_INT16(0, out[4]);
    TEST_ASSERT_EQUAL_INT16(32767, out[5]);
}

static void test_8_and_32_bit() {
    {
        const std::vector<uint8_t> data = {0, 128, 255, 192};  // stereo: (0, 128), (255, 192)
        const std::vector<uint8_t> file = wav(1, 2, 8, 48000, data);
        MemorySource src(file.data(), (uint32_t)file.size());
        WavStream w(g_scratch, sizeof(g_scratch));
        const char* err = "";
        TEST_ASSERT_TRUE(w.open(src, &err));
        int16_t out[4];
        TEST_ASSERT_EQUAL_INT(2, w.read(out, 2));
        TEST_ASSERT_EQUAL_INT16(-32768, out[0]);
        TEST_ASSERT_EQUAL_INT16(0, out[1]);
        TEST_ASSERT_EQUAL_INT16(127 * 256, out[2]);
        TEST_ASSERT_EQUAL_INT16(64 * 256, out[3]);
    }
    {
        std::vector<uint8_t> data;
        put32(data, (uint32_t)(int32_t)0x12345678);
        put32(data, (uint32_t)(int32_t)-0x12345678);
        const std::vector<uint8_t> file = wav(1, 2, 32, 48000, data);
        MemorySource src(file.data(), (uint32_t)file.size());
        WavStream w(g_scratch, sizeof(g_scratch));
        const char* err = "";
        TEST_ASSERT_TRUE(w.open(src, &err));
        int16_t out[2];
        TEST_ASSERT_EQUAL_INT(1, w.read(out, 1));
        TEST_ASSERT_EQUAL_INT16(0x1234, out[0]);
        TEST_ASSERT_EQUAL_INT16((int16_t)((-0x12345678) >> 16), out[1]);
    }
}

static void test_not_a_wav_is_refused() {
    const uint8_t junk[64] = {'I', 'D', '3'};
    MemorySource src(junk, sizeof(junk));
    WavStream w(g_scratch, sizeof(g_scratch));
    const char* err = nullptr;
    TEST_ASSERT_FALSE(w.open(src, &err));
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_FALSE(w.isOpen());
    int16_t out[2];
    TEST_ASSERT_EQUAL_INT(0, w.read(out, 1));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_16_bit_stereo_is_read_as_it_is);
    RUN_TEST(test_seek_lands_on_the_frame);
    RUN_TEST(test_mono_goes_to_both_sides_and_24_bits_keep_the_top_16);
    RUN_TEST(test_float_is_scaled_and_clipped_and_extra_channels_are_dropped);
    RUN_TEST(test_8_and_32_bit);
    RUN_TEST(test_not_a_wav_is_refused);
    return UNITY_END();
}
