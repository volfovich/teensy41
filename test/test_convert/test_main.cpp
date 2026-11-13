// Host unit tests for dsp/Convert.h. Run with: pio test -e native
#include <unity.h>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <dsp/Convert.h>

using dsp::floatToInt24Word;
using dsp::int24WordToFloat;

static int32_t wordOf(int32_t sample24) {
    return static_cast<int32_t>(static_cast<uint32_t>(sample24) << 8);
}

void setUp() {}
void tearDown() {}

// Every 24-bit value survives int -> float -> int unchanged.
static void test_round_trip_is_exact_for_all_24_bit_values() {
    int mismatches = 0;
    for (int32_t s = -8388608; s <= 8388607; ++s) {
        if (floatToInt24Word(int24WordToFloat(wordOf(s))) != wordOf(s)) ++mismatches;
    }
    TEST_ASSERT_EQUAL_INT(0, mismatches);
}

static void test_input_scaling() {
    TEST_ASSERT_EQUAL_FLOAT(0.0f, int24WordToFloat(0));
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, int24WordToFloat(INT32_MIN));
    TEST_ASSERT_EQUAL_FLOAT(0.5f, int24WordToFloat(wordOf(4194304)));
    TEST_ASSERT_TRUE(int24WordToFloat(wordOf(8388607)) < 1.0f);
}

// Bits 7..0 of an input word carry no audio and must not change the result.
static void test_input_ignores_low_byte() {
    for (int32_t s : {-8388608, -12345, -1, 0, 1, 54321, 8388607}) {
        const int32_t clean = wordOf(s);
        const int32_t dirty = clean | 0xFF;
        TEST_ASSERT_EQUAL_FLOAT(int24WordToFloat(clean), int24WordToFloat(dirty));
    }
}

static void test_output_saturates() {
    TEST_ASSERT_EQUAL_INT32(wordOf(8388607), floatToInt24Word(1.0f));
    TEST_ASSERT_EQUAL_INT32(wordOf(8388607), floatToInt24Word(1.5f));
    TEST_ASSERT_EQUAL_INT32(wordOf(8388607), floatToInt24Word(1.0e30f));
    TEST_ASSERT_EQUAL_INT32(wordOf(8388607), floatToInt24Word(std::numeric_limits<float>::infinity()));
    TEST_ASSERT_EQUAL_INT32(INT32_MIN, floatToInt24Word(-1.0f));
    TEST_ASSERT_EQUAL_INT32(INT32_MIN, floatToInt24Word(-7.0f));
    TEST_ASSERT_EQUAL_INT32(INT32_MIN, floatToInt24Word(-std::numeric_limits<float>::infinity()));
}

static void test_output_nan_is_silence() {
    TEST_ASSERT_EQUAL_INT32(0, floatToInt24Word(std::numeric_limits<float>::quiet_NaN()));
}

static void test_output_low_byte_is_zero_and_rounds_to_nearest() {
    for (float x = -1.2f; x <= 1.2f; x += 0.000317f) {
        TEST_ASSERT_EQUAL_INT32(0, floatToInt24Word(x) & 0xFF);
    }
    const float lsb = 1.0f / 8388608.0f;
    TEST_ASSERT_EQUAL_INT32(wordOf(0), floatToInt24Word(0.4f * lsb));
    TEST_ASSERT_EQUAL_INT32(wordOf(1), floatToInt24Word(0.6f * lsb));
    TEST_ASSERT_EQUAL_INT32(wordOf(0), floatToInt24Word(-0.4f * lsb));
    TEST_ASSERT_EQUAL_INT32(wordOf(-1), floatToInt24Word(-0.6f * lsb));
}

static void test_block_functions_interleave() {
    const int32_t in[6] = {wordOf(100), wordOf(-100), wordOf(200), wordOf(-200), wordOf(300), wordOf(-300)};
    float l[3], r[3];
    dsp::int24InToFloat(in, l, r, 3);
    TEST_ASSERT_EQUAL_FLOAT(100.0f / 8388608.0f, l[0]);
    TEST_ASSERT_EQUAL_FLOAT(-200.0f / 8388608.0f, r[1]);
    TEST_ASSERT_EQUAL_FLOAT(300.0f / 8388608.0f, l[2]);

    int32_t out[6];
    dsp::floatToInt24Out(l, r, out, 3);
    TEST_ASSERT_EQUAL_INT32_ARRAY(in, out, 6);

    // Same buffer for both channels = mono to both outputs.
    dsp::floatToInt24Out(l, l, out, 3);
    TEST_ASSERT_EQUAL_INT32(out[0], out[1]);
    TEST_ASSERT_EQUAL_INT32(wordOf(300), out[5]);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_round_trip_is_exact_for_all_24_bit_values);
    RUN_TEST(test_input_scaling);
    RUN_TEST(test_input_ignores_low_byte);
    RUN_TEST(test_output_saturates);
    RUN_TEST(test_output_nan_is_silence);
    RUN_TEST(test_output_low_byte_is_zero_and_rounds_to_nearest);
    RUN_TEST(test_block_functions_interleave);
    return UNITY_END();
}
