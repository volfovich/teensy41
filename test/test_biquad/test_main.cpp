// Host unit tests for lib/dsp. Run with: pio test -e native
#include <unity.h>
#include <cmath>
#include <vector>
#include <dsp/Biquad.h>

using dsp::Biquad;
using dsp::BiquadCoeffs;

static constexpr float kFs = 44100.0f;

static float toDb(float mag) { return 20.0f * std::log10(mag); }

// Run a sine through the filter and measure steady-state RMS gain.
static float measuredGain(const BiquadCoeffs& c, float f) {
    Biquad bq;
    bq.setCoeffs(c);
    const int n = static_cast<int>(kFs);  // 1 s
    const int settle = n / 2;
    double inSq = 0.0, outSq = 0.0;
    for (int i = 0; i < n; ++i) {
        const float x = std::sin(2.0f * dsp::kPi * f * static_cast<float>(i) / kFs);
        const float y = bq.process(x);
        if (i >= settle) {
            inSq += static_cast<double>(x) * static_cast<double>(x);
            outSq += static_cast<double>(y) * static_cast<double>(y);
        }
    }
    return static_cast<float>(std::sqrt(outSq / inSq));
}

void setUp() {}
void tearDown() {}

void test_identity_passes_through() {
    Biquad bq;
    const float in[4] = {1.0f, -0.5f, 0.25f, 0.0f};
    float out[4];
    bq.process(in, out, 4);
    for (int i = 0; i < 4; ++i) TEST_ASSERT_EQUAL_FLOAT(in[i], out[i]);
}

void test_lowpass_dc_gain_is_unity() {
    const auto c = BiquadCoeffs::lowpass(kFs, 1000.0f, 0.7071f);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, c.magnitudeAt(kFs, 0.0f));
}

void test_lowpass_minus3db_at_cutoff_butterworth() {
    const auto c = BiquadCoeffs::lowpass(kFs, 1000.0f, 0.7071f);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, -3.01f, toDb(c.magnitudeAt(kFs, 1000.0f)));
    TEST_ASSERT_FLOAT_WITHIN(0.1f, -3.01f, toDb(measuredGain(c, 1000.0f)));
}

void test_lowpass_attenuates_high_freqs() {
    const auto c = BiquadCoeffs::lowpass(kFs, 1000.0f, 0.7071f);
    // 2nd order: ~-12 dB/octave well above cutoff. At 10 kHz expect < -35 dB.
    TEST_ASSERT_LESS_THAN_FLOAT(-35.0f, toDb(measuredGain(c, 10000.0f)));
}

void test_highpass_blocks_dc_passes_highs() {
    const auto c = BiquadCoeffs::highpass(kFs, 200.0f, 0.7071f);
    TEST_ASSERT_LESS_THAN_FLOAT(1e-4f, c.magnitudeAt(kFs, 0.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.0f, toDb(c.magnitudeAt(kFs, 10000.0f)));
}

void test_peaking_gain_at_center() {
    const auto c = BiquadCoeffs::peaking(kFs, 3000.0f, 1.0f, 6.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 6.0f, toDb(c.magnitudeAt(kFs, 3000.0f)));
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.0f, toDb(c.magnitudeAt(kFs, 0.0f)));
}

void test_notch_kills_center() {
    const auto c = BiquadCoeffs::notch(kFs, 60.0f, 10.0f);
    TEST_ASSERT_LESS_THAN_FLOAT(-60.0f, toDb(c.magnitudeAt(kFs, 60.0f) + 1e-9f));
}

void test_impulse_response_decays() {
    Biquad bq;
    bq.setCoeffs(BiquadCoeffs::lowpass(kFs, 100.0f, 5.0f));  // resonant, slow decay
    float y = bq.process(1.0f);
    for (int i = 0; i < 44100; ++i) y = bq.process(0.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, y);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_identity_passes_through);
    RUN_TEST(test_lowpass_dc_gain_is_unity);
    RUN_TEST(test_lowpass_minus3db_at_cutoff_butterworth);
    RUN_TEST(test_lowpass_attenuates_high_freqs);
    RUN_TEST(test_highpass_blocks_dc_passes_highs);
    RUN_TEST(test_peaking_gain_at_center);
    RUN_TEST(test_notch_kills_center);
    RUN_TEST(test_impulse_response_decays);
    return UNITY_END();
}
