// Host unit tests for dsp/player/Mp3Stream.h (an MP3 file decoded a frame at a time as 16-bit
// stereo, for the backing-track player). The files are made by tools/make_track_fixtures.py:
// two tones, 440 Hz left and 1 kHz right, growing louder over the file.
// Run with: pio test -e native
#include <unity.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <dsp/ByteSource.h>
#include <dsp/player/Mp3Stream.h>

using dsp::MemorySource;
using dsp::Mp3Stream;

void setUp() {}
void tearDown() {}

static std::vector<uint8_t> readFile(const char* name) {
    const std::string path = std::string("test/fixtures/player/") + name;
    FILE* f = fopen(path.c_str(), "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path.c_str());
    std::vector<uint8_t> v;
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) v.insert(v.end(), buf, buf + n);
    fclose(f);
    return v;
}

// The memory an Mp3Stream needs.
struct Mem {
    std::vector<float> decoder = std::vector<float>(Mp3Stream::kDecoderBytes / 4 + 1);
    std::vector<uint8_t> in = std::vector<uint8_t>(16384);
    std::vector<int16_t> pcm = std::vector<int16_t>(Mp3Stream::kPcmSamples);
    std::vector<uint32_t> index = std::vector<uint32_t>(8192);
    Mp3Stream::Memory get() {
        return {decoder.data(), in.data(), (int)in.size(), pcm.data(), index.data(), (int)index.size()};
    }
};

// Everything from where the stream stands, read in pieces of `piece` frames.
static std::vector<int16_t> readAll(Mp3Stream& s, int piece = 700) {
    std::vector<int16_t> out;
    std::vector<int16_t> buf(2 * piece);
    for (;;) {
        const int got = s.read(buf.data(), piece);
        out.insert(out.end(), buf.begin(), buf.begin() + 2 * got);
        if (got < piece) break;
    }
    return out;
}

// How strong `hz` is in one channel of frames [from, to): the amplitude of that sine.
static double toneAmp(const std::vector<int16_t>& v, int ch, size_t from, size_t to, double hz, double rate) {
    double re = 0, im = 0;
    for (size_t i = from; i < to; i++) {
        const double x = v[2 * i + ch] / 32768.0, ph = 6.283185307179586 * hz * (double)i / rate;
        re += x * cos(ph);
        im += x * sin(ph);
    }
    return 2.0 * sqrt(re * re + im * im) / (double)(to - from);
}

static void checkFile(const char* name, uint32_t rate, int channels, double seconds) {
    const std::vector<uint8_t> file = readFile(name);
    MemorySource src(file.data(), (uint32_t)file.size());
    Mem mem;
    Mp3Stream s(mem.get());
    const char* err = "";
    TEST_ASSERT_TRUE_MESSAGE(s.open(src, &err), err);
    TEST_ASSERT_EQUAL_UINT32(rate, s.sampleRate());
    TEST_ASSERT_EQUAL_INT(channels, s.channels());
    // The length: the sound plus the encoder's padding (under 0.15 s).
    const double len = (double)s.frames() / rate;
    TEST_ASSERT_TRUE_MESSAGE(len >= seconds && len < seconds + 0.15, name);

    const std::vector<int16_t> all = readAll(s);
    TEST_ASSERT_EQUAL_UINT32(s.frames(), (uint32_t)(all.size() / 2));
    TEST_ASSERT_EQUAL_UINT32(s.frames(), s.position());
    // The tones are there, at the level they were made with: around the middle of the file
    // the left one's amplitude is 0.15 + 0.5 x (where in the file).
    const size_t mid = (size_t)(rate * seconds / 2), half = rate / 10;
    const double want = 0.15 + 0.5 * 0.5;
    TEST_ASSERT_FLOAT_WITHIN(0.03f, (float)want, (float)toneAmp(all, 0, mid - half, mid + half, 440.0, rate));
    if (channels == 2) {
        TEST_ASSERT_FLOAT_WITHIN(0.03f, (float)(want * 0.75), (float)toneAmp(all, 1, mid - half, mid + half, 1000.0, rate));
        TEST_ASSERT_TRUE(toneAmp(all, 0, mid - half, mid + half, 1000.0, rate) < 0.01);  // the sides are apart
    } else {
        for (size_t i = 0; i < all.size() / 2; i++) TEST_ASSERT_EQUAL_INT16(all[2 * i], all[2 * i + 1]);
    }
    // Louder at the end than at the start.
    TEST_ASSERT_TRUE(toneAmp(all, 0, (size_t)(rate * (seconds - 0.4)), (size_t)(rate * (seconds - 0.2)), 440.0, rate) >
                     2.0 * toneAmp(all, 0, rate / 5, 2 * rate / 5, 440.0, rate));

    // A seek gives the very samples a decode from the start gives, wherever it lands.
    const uint32_t places[] = {0, 1, 575, 576, 1151, 1152, 5000, 12345, s.frames() / 2 + 7, s.frames() - 3000, s.frames() - 1};
    for (uint32_t p : places) {
        TEST_ASSERT_TRUE(s.seek(p));
        TEST_ASSERT_EQUAL_UINT32(p, s.position());
        int16_t got[2 * 2500];
        const int n = s.read(got, 2500);
        const uint32_t expect = s.frames() - p < 2500 ? s.frames() - p : 2500;
        TEST_ASSERT_EQUAL_INT((int)expect, n);
        for (int i = 0; i < 2 * n; i++) TEST_ASSERT_INT16_WITHIN_MESSAGE(1, all[2 * p + i], got[i], name);
    }
    // Past the end is the end.
    TEST_ASSERT_TRUE(s.seek(s.frames() + 1000));
    int16_t one[2];
    TEST_ASSERT_EQUAL_INT(0, s.read(one, 1));
}

static void test_cbr_44k_stereo() { checkFile("tone_44k_cbr128.mp3", 44100, 2, 3.0); }
static void test_vbr_44k_stereo() { checkFile("tone_44k_vbr.mp3", 44100, 2, 3.0); }
static void test_low_rate_mono() { checkFile("tone_22k_mono_32.mp3", 22050, 1, 3.0); }
static void test_48k() { checkFile("tone_48k_cbr96.mp3", 48000, 2, 2.0); }

static void test_a_tag_with_a_picture_in_front_is_stepped_over() {
    const std::vector<uint8_t> file = readFile("tone_44k_cbr128.mp3");
    // An ID3v2 tag of 70000 bytes whose content looks like MP3 frame starts.
    std::vector<uint8_t> tagged = {'I', 'D', '3', 3, 0, 0};
    const uint32_t size = 70000;
    for (int shift = 21; shift >= 0; shift -= 7) tagged.push_back((uint8_t)((size >> shift) & 0x7F));
    for (uint32_t i = 0; i < size; i++) tagged.push_back((uint8_t)(i % 418 == 0 ? 0xFF : (i % 418 == 1 ? 0xFB : (i % 418 == 2 ? 0x90 : 0x44))));
    tagged.insert(tagged.end(), file.begin(), file.end());

    Mem a, b;
    MemorySource plainSrc(file.data(), (uint32_t)file.size()), taggedSrc(tagged.data(), (uint32_t)tagged.size());
    Mp3Stream plain(a.get()), withTag(b.get());
    const char* err = "";
    TEST_ASSERT_TRUE_MESSAGE(plain.open(plainSrc, &err), err);
    TEST_ASSERT_TRUE_MESSAGE(withTag.open(taggedSrc, &err), err);
    TEST_ASSERT_EQUAL_UINT32(plain.frames(), withTag.frames());
    const std::vector<int16_t> x = readAll(plain), y = readAll(withTag, 333);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)x.size(), (uint32_t)y.size());
    TEST_ASSERT_EQUAL_INT16_ARRAY(x.data(), y.data(), x.size());
}

static void test_what_is_no_mp3_is_refused() {
    std::vector<uint8_t> junk(50000);
    for (size_t i = 0; i < junk.size(); i++) junk[i] = (uint8_t)(i * 7 + (i >> 5));
    memcpy(junk.data(), "RIFF", 4);
    MemorySource src(junk.data(), (uint32_t)junk.size());
    Mem mem;
    Mp3Stream s(mem.get());
    const char* err = nullptr;
    TEST_ASSERT_FALSE(s.open(src, &err));
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_FALSE(s.isOpen());
    const uint8_t tiny[3] = {0xFF, 0xFB, 0x90};
    MemorySource small(tiny, sizeof(tiny));
    TEST_ASSERT_FALSE(s.open(small, &err));
}

static void test_a_file_too_long_for_the_table_is_refused() {
    const std::vector<uint8_t> file = readFile("tone_44k_cbr128.mp3");
    MemorySource src(file.data(), (uint32_t)file.size());
    Mem mem;
    Mp3Stream::Memory m = mem.get();
    m.indexCap = 3;  // 48 frames; the file has about 116
    Mp3Stream s(m);
    const char* err = nullptr;
    TEST_ASSERT_FALSE(s.open(src, &err));
    TEST_ASSERT_NOT_NULL(err);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_cbr_44k_stereo);
    RUN_TEST(test_vbr_44k_stereo);
    RUN_TEST(test_low_rate_mono);
    RUN_TEST(test_48k);
    RUN_TEST(test_a_tag_with_a_picture_in_front_is_stepped_over);
    RUN_TEST(test_what_is_no_mp3_is_refused);
    RUN_TEST(test_a_file_too_long_for_the_table_is_refused);
    return UNITY_END();
}
