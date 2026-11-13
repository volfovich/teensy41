// Host tests for the desktop app's link protocol (lib/applink): frames against the shared
// examples in protocol/test_frames.txt, the stream parser's handling of text, errors and
// resynchronisation, and the payload reader and writer.
// Run with: pio test -e native   (from the project root)
#include <unity.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <applink/Frame.h>
#include <applink/Messages.h>
#include <applink/Wire.h>

using applink::FrameParser;
using Result = applink::FrameParser::Result;

namespace {

struct Example {
    std::string name;
    uint8_t type, flags;
    uint16_t sequence;
    std::vector<uint8_t> payload, frame;
};

std::vector<uint8_t> fromHex(const std::string& h) {
    std::vector<uint8_t> out;
    if (h == "-") return out;
    for (size_t i = 0; i + 1 < h.size(); i += 2) out.push_back(static_cast<uint8_t>(std::stoul(h.substr(i, 2), nullptr, 16)));
    return out;
}

std::vector<Example> loadExamples() {
    std::vector<Example> out;
    FILE* f = std::fopen("protocol/test_frames.txt", "r");
    if (!f) return out;
    char line[20000];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.empty() || s[0] == '#') continue;
        std::vector<std::string> parts;
        size_t at = 0;
        for (;;) {
            const size_t sep = s.find(" | ", at);
            parts.push_back(s.substr(at, sep == std::string::npos ? std::string::npos : sep - at));
            if (sep == std::string::npos) break;
            at = sep + 3;
        }
        if (parts.size() != 6) continue;
        Example e;
        e.name = parts[0];
        e.type = static_cast<uint8_t>(std::stoul(parts[1], nullptr, 16));
        e.flags = static_cast<uint8_t>(std::stoul(parts[2], nullptr, 16));
        e.sequence = static_cast<uint16_t>(std::stoul(parts[3]));
        e.payload = fromHex(parts[4]);
        e.frame = fromHex(parts[5]);
        out.push_back(e);
    }
    std::fclose(f);
    return out;
}

std::vector<Example> g_examples;
std::vector<uint8_t> g_buf(applink::kMaxPayload);

// Feeds bytes and records what came out.
struct Sink {
    FrameParser parser{g_buf.data(), static_cast<uint32_t>(g_buf.size())};
    std::string text;
    int frames = 0, badCrc = 0, tooLong = 0;
    applink::FrameHeader last;
    std::vector<uint8_t> lastPayload;

    void feed(const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            switch (parser.feed(p[i])) {
            case Result::kText: text.push_back(static_cast<char>(p[i])); break;
            case Result::kFrame:
                ++frames;
                last = parser.header();
                lastPayload.assign(parser.payload(), parser.payload() + last.length);
                break;
            case Result::kBadCrc: ++badCrc; break;
            case Result::kTooLong: ++tooLong; break;
            case Result::kConsumed: break;
            }
        }
    }
    void feed(const std::vector<uint8_t>& v) { feed(v.data(), v.size()); }
    void feed(const char* s) { feed(reinterpret_cast<const uint8_t*>(s), std::strlen(s)); }
};

} // namespace

void setUp() {}
void tearDown() {}

static void test_crc32_matches_the_standard_check_value() {
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, applink::crc32("123456789", 9));
    TEST_ASSERT_EQUAL_HEX32(0u, applink::crc32("", 0));
    // Continuing over two pieces gives the same as one pass.
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, applink::crc32("6789", 4, applink::crc32("12345", 5)));
}

static void test_examples_are_there() {
    TEST_ASSERT_TRUE_MESSAGE(g_examples.size() >= 10, "cannot read protocol/test_frames.txt (run from the project root)");
}

static void test_encodes_every_example_exactly() {
    std::vector<uint8_t> out(applink::kMaxPayload + applink::kOverhead);
    for (const Example& e : g_examples) {
        const size_t n = applink::encodeFrame(e.type, e.flags, e.sequence, e.payload.data(),
                                              static_cast<uint32_t>(e.payload.size()), out.data(), out.size());
        TEST_ASSERT_EQUAL_size_t_MESSAGE(e.frame.size(), n, e.name.c_str());
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, std::memcmp(out.data(), e.frame.data(), n), e.name.c_str());
    }
}

static void test_decodes_every_example() {
    for (const Example& e : g_examples) {
        Sink s;
        s.feed(e.frame);
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, s.frames, e.name.c_str());
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(e.type, s.last.type, e.name.c_str());
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(e.flags, s.last.flags, e.name.c_str());
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(e.sequence, s.last.sequence, e.name.c_str());
        TEST_ASSERT_EQUAL_size_t_MESSAGE(e.payload.size(), s.lastPayload.size(), e.name.c_str());
        TEST_ASSERT_TRUE_MESSAGE(s.lastPayload == e.payload, e.name.c_str());
        TEST_ASSERT_TRUE_MESSAGE(s.text.empty(), e.name.c_str());
        TEST_ASSERT_FALSE(s.parser.busy());
    }
}

// Typed commands and frames share the port: text passes through, frames do not leak into it.
static void test_text_and_frames_mixed() {
    Sink s;
    s.feed("s\n");
    for (const Example& e : g_examples) {
        s.feed(e.frame);
        s.feed("amp on\r\n");
    }
    std::string want = "s\n";
    for (size_t i = 0; i < g_examples.size(); ++i) want += "amp on\r\n";
    TEST_ASSERT_EQUAL_INT(static_cast<int>(g_examples.size()), s.frames);
    TEST_ASSERT_EQUAL_STRING(want.c_str(), s.text.c_str());
    TEST_ASSERT_EQUAL_INT(0, s.badCrc);
}

static void test_a_lone_first_magic_byte_is_dropped() {
    Sink s;
    const uint8_t bytes[] = {'a', applink::kMagic0, 'b', applink::kMagic0, applink::kMagic0, 'c'};
    s.feed(bytes, sizeof(bytes));
    TEST_ASSERT_EQUAL_STRING("abc", s.text.c_str());
    TEST_ASSERT_EQUAL_INT(0, s.frames);
    // ... and a real frame straight after a lone one is still found.
    const uint8_t lone[] = {applink::kMagic0};
    s.feed(lone, 1);
    s.feed(g_examples[0].frame);
    TEST_ASSERT_EQUAL_INT(1, s.frames);
}

// Every single-bit error in a frame is caught: no frame is delivered, and none of the damaged
// frame's bytes are passed on as text (they could run as a shell command).
static void test_any_flipped_bit_is_rejected() {
    const Example& e = g_examples[1];   // the hello request
    for (size_t byte = 2; byte < e.frame.size(); ++byte) {
        for (int bit = 0; bit < 8; ++bit) {
            std::vector<uint8_t> bad = e.frame;
            bad[byte] = static_cast<uint8_t>(bad[byte] ^ (1u << bit));
            Sink s;
            s.feed(bad);
            TEST_ASSERT_EQUAL_INT(0, s.frames);
            TEST_ASSERT_TRUE(s.text.empty());
            // After a pause the stream works again.
            s.parser.idle();
            s.feed(e.frame);
            s.feed("s\n");
            TEST_ASSERT_EQUAL_INT(1, s.frames);
            TEST_ASSERT_EQUAL_STRING("s\n", s.text.c_str());
        }
    }
}

static void test_bad_crc_swallows_until_idle() {
    std::vector<uint8_t> bad = g_examples[3].frame;
    bad[bad.size() - 1] ^= 0xFF;
    Sink s;
    s.feed(bad);
    s.feed(g_examples[4].frame);   // sent without a pause: swallowed
    s.feed("drum on\n");
    TEST_ASSERT_EQUAL_INT(1, s.badCrc);
    TEST_ASSERT_EQUAL_INT(0, s.frames);
    TEST_ASSERT_TRUE(s.text.empty());
    TEST_ASSERT_TRUE(s.parser.busy());

    s.parser.idle();
    s.feed(g_examples[4].frame);
    s.feed("ok\n");
    TEST_ASSERT_EQUAL_INT(1, s.frames);
    TEST_ASSERT_EQUAL_UINT8(g_examples[4].flags, s.last.flags);
    TEST_ASSERT_EQUAL_STRING("ok\n", s.text.c_str());
}

static void test_too_long_frame_is_swallowed_until_idle() {
    uint8_t small[16];
    FrameParser p(small, sizeof(small));
    const Example* big = nullptr;
    for (const Example& e : g_examples)
        if (e.payload.size() == 300) big = &e;
    TEST_ASSERT_NOT_NULL(big);
    int tooLong = 0, text = 0, frames = 0;
    for (uint8_t b : big->frame) {
        const Result r = p.feed(b);
        tooLong += r == Result::kTooLong;
        text += r == Result::kText;
        frames += r == Result::kFrame;
    }
    // Text sent before the stream pauses is swallowed too: it cannot be told from payload.
    for (char c : std::string("drum on\n")) text += p.feed(static_cast<uint8_t>(c)) == Result::kText;
    TEST_ASSERT_EQUAL_INT(1, tooLong);
    TEST_ASSERT_EQUAL_INT(0, text);
    TEST_ASSERT_EQUAL_INT(0, frames);
    TEST_ASSERT_TRUE(p.busy());

    p.idle();
    TEST_ASSERT_FALSE(p.busy());
    TEST_ASSERT_TRUE(p.feed('s') == Result::kText);
    // A frame that fits is read again afterwards.
    Result last = Result::kConsumed;
    for (uint8_t b : g_examples[0].frame) last = p.feed(b);
    TEST_ASSERT_TRUE(last == Result::kFrame);
}

static void test_a_length_over_the_protocol_limit_is_refused() {
    std::vector<uint8_t> f = {applink::kMagic0, applink::kMagic1, 0x41, 0, 1, 0, 0x01, 0x00, 0x01, 0x00};   // 65537
    Sink s;
    s.feed(f);
    TEST_ASSERT_EQUAL_INT(1, s.tooLong);
    uint8_t out[32];
    TEST_ASSERT_EQUAL_size_t(0, applink::encodeFrame(1, 0, 0, out, applink::kMaxPayload + 1, out, sizeof(out)));
}

static void test_half_a_frame_is_dropped_on_idle() {
    Sink s;
    const std::vector<uint8_t>& f = g_examples[2].frame;
    s.feed(f.data(), f.size() / 2);
    TEST_ASSERT_TRUE(s.parser.busy());
    s.parser.idle();
    s.feed("s\n");
    s.feed(f);
    TEST_ASSERT_EQUAL_STRING("s\n", s.text.c_str());
    TEST_ASSERT_EQUAL_INT(1, s.frames);
}

static void test_encode_refuses_a_buffer_that_is_too_small() {
    uint8_t out[22];   // exactly a frame with 8 payload bytes
    const uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    TEST_ASSERT_EQUAL_size_t(0, applink::encodeFrame(1, 0, 0, payload, 8, out, sizeof(out) - 1));
    TEST_ASSERT_EQUAL_size_t(22, applink::encodeFrame(1, 0, 0, payload, 8, out, sizeof(out)));
}

static void test_wire_round_trip() {
    uint8_t buf[128];
    applink::Writer w(buf, sizeof(buf));
    w.u8(0xAB);
    w.u16(0xBEEF);
    w.u32(0xDEADBEEFu);
    w.i16(-2);
    w.i32(-380);
    w.f32(-12.5f);
    w.str("Écho");
    w.str(nullptr);
    w.bytes("xyz", 3);
    TEST_ASSERT_TRUE(w.ok());

    applink::Reader r(w.data(), w.size());
    char s[16], empty[4];
    TEST_ASSERT_EQUAL_HEX8(0xAB, r.u8());
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, r.u16());
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFu, r.u32());
    TEST_ASSERT_EQUAL_INT16(-2, r.i16());
    TEST_ASSERT_EQUAL_INT32(-380, r.i32());
    TEST_ASSERT_EQUAL_FLOAT(-12.5f, r.f32());
    r.str(s, sizeof(s));
    r.str(empty, sizeof(empty));
    TEST_ASSERT_EQUAL_STRING("Écho", s);
    TEST_ASSERT_EQUAL_STRING("", empty);
    const uint8_t* raw = r.bytes(3);
    TEST_ASSERT_NOT_NULL(raw);
    TEST_ASSERT_EQUAL_INT(0, std::memcmp(raw, "xyz", 3));
    TEST_ASSERT_TRUE(r.done());
}

// The hello reply example, read field by field as PROTOCOL.md lays it out.
static void test_wire_reads_the_hello_reply_example() {
    const Example* e = nullptr;
    for (const Example& x : g_examples)
        if (x.name == "hello reply") e = &x;
    TEST_ASSERT_NOT_NULL(e);
    applink::Reader r(e->payload.data(), e->payload.size());
    char version[16];
    TEST_ASSERT_EQUAL_UINT16(applink::kProtocolVersion, r.u16());
    r.str(version, sizeof(version));
    TEST_ASSERT_EQUAL_STRING("0.1.0", version);
    TEST_ASSERT_EQUAL_UINT16(32, r.u16());        // block size
    TEST_ASSERT_EQUAL_UINT32(48000, r.u32());     // sample rate
    TEST_ASSERT_EQUAL_UINT16(64, r.u16());        // max models
    TEST_ASSERT_EQUAL_UINT16(128, r.u16());       // max IRs
    TEST_ASSERT_EQUAL_UINT16(95, r.u16());        // max path length
    TEST_ASSERT_EQUAL_UINT16(64, r.u16());        // saved settings
    TEST_ASSERT_EQUAL_UINT16(67, r.u16());        // all settings
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFu, r.u32()); // settings table hash
    TEST_ASSERT_TRUE(r.done());
}

static void test_wire_errors_are_sticky_and_safe() {
    uint8_t buf[4];
    applink::Writer w(buf, sizeof(buf));
    w.u32(1);
    w.u8(2);   // does not fit
    w.u8(3);
    TEST_ASSERT_FALSE(w.ok());
    TEST_ASSERT_EQUAL_size_t(4, w.size());

    const uint8_t data[] = {0x05, 0x00, 'a', 'b'};   // a string that claims 5 bytes but has 2
    applink::Reader r(data, sizeof(data));
    char s[8] = "junk";
    r.str(s, sizeof(s));
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_EQUAL_STRING("", s);
    TEST_ASSERT_EQUAL_UINT32(0, r.u32());   // after an error everything reads as zero
    TEST_ASSERT_FALSE(r.done());

    const uint8_t longer[] = {0x04, 0x00, 'a', 'b', 'c', 'd'};
    applink::Reader r2(longer, sizeof(longer));
    char tiny[4];
    r2.str(tiny, sizeof(tiny));   // 4 bytes + terminator do not fit in 4
    TEST_ASSERT_FALSE(r2.ok());

    const uint8_t zero[] = {0x03, 0x00, 'a', 0x00, 'b'};   // a zero byte inside a string
    applink::Reader r3(zero, sizeof(zero));
    r3.str(s, sizeof(s));
    TEST_ASSERT_FALSE(r3.ok());
}

int main() {
    g_examples = loadExamples();
    UNITY_BEGIN();
    RUN_TEST(test_crc32_matches_the_standard_check_value);
    RUN_TEST(test_examples_are_there);
    if (g_examples.size() >= 10) {
        RUN_TEST(test_encodes_every_example_exactly);
        RUN_TEST(test_decodes_every_example);
        RUN_TEST(test_text_and_frames_mixed);
        RUN_TEST(test_a_lone_first_magic_byte_is_dropped);
        RUN_TEST(test_any_flipped_bit_is_rejected);
        RUN_TEST(test_bad_crc_swallows_until_idle);
        RUN_TEST(test_too_long_frame_is_swallowed_until_idle);
        RUN_TEST(test_a_length_over_the_protocol_limit_is_refused);
        RUN_TEST(test_half_a_frame_is_dropped_on_idle);
        RUN_TEST(test_wire_reads_the_hello_reply_example);
    }
    RUN_TEST(test_encode_refuses_a_buffer_that_is_too_small);
    RUN_TEST(test_wire_round_trip);
    RUN_TEST(test_wire_errors_are_sticky_and_safe);
    return UNITY_END();
}
