// Host unit tests for dsp/fx/ReverbBank.h and DelayBank.h: the ported effects come out
// unchanged, and a change of mode or type between effects that share memory is clean.
// Run with: pio test -e native
#include <unity.h>
#include <dsp/fx/DelayBank.h>
#include <dsp/fx/ReverbBank.h>

#include "../fx_test_util.h"

using dsp::DelayBank;
using dsp::ReverbBank;
using namespace fxtest;

static constexpr int kLen = 36008;  // the firmware's delay lines: 750 ms

static void settings(dsp::Reverb80& r) {
    r.setSize(0.6f);
    r.setTone(0.4f);
    r.setPreDelayMs(30.0f);
    r.setMod(0.5f);
    r.setEra(0.3f);
    r.setGateMs(250.0f);
}
static void settings(ReverbBank& r) {
    r.setSize(0.6f);
    r.setTone(0.4f);
    r.setPreDelayMs(30.0f);
    r.setMod(0.5f);
    r.setEra(0.3f);
    r.setGateMs(250.0f);
}
template <class D>
static void delaySettings(D& d) {
    d.setFeedback(0.6f);
    d.setTone(0.5f);
    d.setModDepth(0.3f);
    d.setDrive(0.4f);
    d.setDuck(0.5f);
    d.setEra(0.5f);
}

struct ReverbRig {
    std::vector<float> mem = std::vector<float>(ReverbBank::kMemFloats, 9.0f);
    ReverbBank rv{kFs};
    ReverbRig() {
        rv.init(mem.data());
        settings(rv);
    }
    void block(const float* x, float* l, float* r) { rv.process(x, x, l, r, kB); }
};

struct DelayRig {
    std::vector<float> a = std::vector<float>(kLen, 9.0f), b = std::vector<float>(kLen, 9.0f);
    DelayBank d{kFs};
    DelayRig() {
        d.init(a.data(), b.data(), kLen);
        d.setDelaySamples(9600.0f);
        delaySettings(d);
    }
};

void setUp() {}
void tearDown() {}

// Hall and gated through the bank, with a change between them while sounding, are
// Reverb80's own samples.
static void test_hall_and_gated_are_exactly_reverb80() {
    const std::vector<float> x = noise(4 * kS, 11, 0.4f);
    ReverbRig rig;
    std::vector<float> mem(dsp::Reverb80::kMemFloats);
    dsp::Reverb80 ref(kFs);
    ref.init(mem.data());
    settings(ref);
    float l[kB], r[kB], rl[kB], rr[kB];
    for (size_t at = 0; at + kB <= x.size(); at += kB) {
        if (at == (size_t)kS) {
            rig.rv.setMode(ReverbBank::kGated);
            ref.setMode(dsp::Reverb80::kGated);
        }
        if (at == (size_t)(3 * kS)) {
            rig.rv.setMode(ReverbBank::kHall);
            ref.setMode(dsp::Reverb80::kHall);
        }
        rig.block(&x[at], l, r);
        ref.process(&x[at], &x[at], rl, rr, kB);
        TEST_ASSERT_EQUAL_MEMORY(rl, l, sizeof(l));
        TEST_ASSERT_EQUAL_MEMORY(rr, r, sizeof(r));
    }
}

// The digital type through the bank is StudioDelay's own samples, time changes included.
static void test_digital_is_exactly_studio_delay() {
    const std::vector<float> x = noise(4 * kS, 12, 0.4f);
    DelayRig rig;
    std::vector<float> a(kLen), b(kLen);
    dsp::StudioDelay ref(kFs);
    ref.init(a.data(), b.data(), kLen);
    ref.setDelaySamples(9600.0f);
    delaySettings(ref);
    float l[kB], r[kB], rl[kB], rr[kB];
    for (size_t at = 0; at + kB <= x.size(); at += kB) {
        if (at == (size_t)(2 * kS)) {
            rig.d.setDelaySamples(20000.0f);
            ref.setDelaySamples(20000.0f);
        }
        rig.d.process(&x[at], l, r, kB);
        ref.process(&x[at], rl, rr, kB);
        TEST_ASSERT_EQUAL_MEMORY(rl, l, sizeof(l));
        TEST_ASSERT_EQUAL_MEMORY(rr, r, sizeof(r));
    }
}

// What one change does to the output: how long the old sound fades, how long the silence
// lasts, the largest step, and whether the new effect then sounds.
struct Change {
    int fade = 0, silent = 0;
    float step = 0.0f, after = 0.0f;
};

// Runs `process` (one block into l, r) on noise, calls `change` after one second, and
// measures; `quiet` after the change feeds silence, so anything heard later is new sound.
template <class P, class C>
static Change measure(P process, C change) {
    const std::vector<float> x = noise(3 * kS, 13, 0.4f);
    std::vector<float> l(x.size(), 0.0f), r(x.size(), 0.0f);
    for (size_t at = 0; at + kB <= x.size(); at += kB) {
        if (at == (size_t)kS) change();
        process(&x[at], &l[at], &r[at]);
    }
    Change c;
    size_t i = kS;
    while (i < l.size() && (l[i] != 0.0f || r[i] != 0.0f)) i++;
    c.fade = (int)(i - kS);
    const size_t zero = i;
    while (i < l.size() && l[i] == 0.0f && r[i] == 0.0f) i++;
    c.silent = (int)(i - zero);
    // Steps against what the sound itself does before the change.
    c.step = largestStep(l, kS - 64, zero + 64) / largestStep(l, kS / 2, kS - 64);
    c.after = (float)rms2(l, r, 2 * kS, 3 * kS);
    return c;
}

static void test_reverb_changes_fade_clear_and_restart() {
    const int from[] = {ReverbBank::kHall, ReverbBank::kPlate, ReverbBank::kSpring, ReverbBank::kPlate, ReverbBank::kGated};
    const int to[] = {ReverbBank::kPlate, ReverbBank::kSpring, ReverbBank::kHall, ReverbBank::kGated, ReverbBank::kSpring};
    for (int k = 0; k < 5; k++) {
        ReverbRig rig;
        rig.rv.setMode(from[k]);
        const Change c = measure([&](const float* x, float* l, float* r) { rig.block(x, l, r); },
                                 [&] { rig.rv.setMode(to[k]); });
        printf("reverb mode %d -> %d: fade %d samples, silence %d, step x %.2f, level after %.3f\n", from[k], to[k],
               c.fade, c.silent, c.step, c.after);
        TEST_ASSERT_INT_WITHIN(96, 960, c.fade);  // 20 ms
        TEST_ASSERT_GREATER_OR_EQUAL(32, c.silent);
        // The clearing (at most 10 blocks), the 30 ms pre-delay, the way to the first tap.
        TEST_ASSERT_LESS_THAN(320 + 1440 + 960, c.silent);
        TEST_ASSERT_LESS_THAN_FLOAT(1.5f, c.step);
        TEST_ASSERT_GREATER_THAN_FLOAT(0.05f, c.after);
    }
}

static void test_delay_changes_fade_clear_and_restart() {
    const int from[] = {DelayBank::kDigital, DelayBank::kTape, DelayBank::kAnalog, DelayBank::kTape};
    const int to[] = {DelayBank::kTape, DelayBank::kAnalog, DelayBank::kDigital, DelayBank::kDigital};
    for (int k = 0; k < 4; k++) {
        DelayRig rig;
        rig.d.setDelaySamples(2400.0f);  // 50 ms: the new type is heard soon after the silence
        rig.d.setType(from[k]);
        const Change c = measure([&](const float* x, float* l, float* r) { rig.d.process(x, l, r, kB); },
                                 [&] { rig.d.setType(to[k]); });
        printf("delay type %d -> %d: fade %d samples, silence %d, step x %.2f, level after %.3f\n", from[k], to[k],
               c.fade, c.silent, c.step, c.after);
        TEST_ASSERT_INT_WITHIN(96, 960, c.fade);
        // The clearing (9 or 18 blocks), then the first repeat: 50 ms, or 37.5 ms on the
        // digital delay's right side.
        TEST_ASSERT_GREATER_OR_EQUAL(288 + 1700, c.silent);
        TEST_ASSERT_LESS_THAN(576 + 2400 + 96, c.silent);
        TEST_ASSERT_LESS_THAN_FLOAT(1.5f, c.step);
        TEST_ASSERT_GREATER_THAN_FLOAT(0.02f, c.after);  // the digital delay ducks under the noise
    }
}

// Nothing of the old effect's sound is left in the shared memory: with silence fed after
// the change, the new effect stays silent.
static void test_nothing_is_left_over_from_the_old_effect() {
    for (int to : {ReverbBank::kPlate, ReverbBank::kSpring, ReverbBank::kHall}) {
        ReverbRig rig;
        rig.rv.setMode(to == ReverbBank::kHall ? ReverbBank::kPlate : ReverbBank::kHall);
        const std::vector<float> x = noise(kS, 14, 0.5f);
        const std::vector<float> zero(kB, 0.0f);
        float l[kB], r[kB];
        for (size_t at = 0; at + kB <= x.size(); at += kB) rig.block(&x[at], l, r);
        rig.rv.setMode(to);
        float late = 0.0f;
        for (int b = 0; b < 3000; b++) {
            rig.block(zero.data(), l, r);
            if (b > 200)
                for (int i = 0; i < kB; i++) late = fmaxf(late, fmaxf(fabsf(l[i]), fabsf(r[i])));
        }
        TEST_ASSERT_EQUAL_FLOAT(0.0f, late);
    }
    for (int to : {DelayBank::kTape, DelayBank::kAnalog, DelayBank::kDigital}) {
        DelayRig rig;
        rig.d.setType(to == DelayBank::kDigital ? DelayBank::kTape : DelayBank::kDigital);
        const std::vector<float> x = noise(kS, 15, 0.5f);
        const std::vector<float> zero(kB, 0.0f);
        float l[kB], r[kB];
        for (size_t at = 0; at + kB <= x.size(); at += kB) rig.d.process(&x[at], l, r, kB);
        rig.d.setType(to);
        float late = 0.0f;
        for (int b = 0; b < 3000; b++) {
            rig.d.process(zero.data(), l, r, kB);
            if (b > 200)
                for (int i = 0; i < kB; i++) late = fmaxf(late, fmaxf(fabsf(l[i]), fabsf(r[i])));
        }
        TEST_ASSERT_EQUAL_FLOAT(0.0f, late);
    }
}

// With the lines clear (the block was off) a change applies at once: the first block is
// already the new effect, the same as that effect on its own.
static void test_change_from_clear_lines_is_immediate() {
    const std::vector<float> x = noise(kS, 16, 0.4f);
    {
        ReverbRig rig;
        rig.rv.setPreDelayMs(0.0f);
        rig.rv.setMode(ReverbBank::kSpring);
        std::vector<float> mem(dsp::SpringReverb::kMemFloats);
        dsp::SpringReverb ref(kFs);
        ref.init(mem.data());
        ref.setSize(0.6f);
        ref.setTone(0.4f);
        float l[kB], r[kB], rl[kB], rr[kB];
        for (size_t at = 0; at + kB <= x.size(); at += kB) {
            rig.block(&x[at], l, r);
            ref.process(&x[at], &x[at], rl, rr, kB);
            TEST_ASSERT_EQUAL_MEMORY(rl, l, sizeof(l));
        }
        // After the stage's idle clearing the next change is immediate too.
        while (!rig.rv.clearStep(4096)) {}
        rig.rv.setMode(ReverbBank::kPlate);
        // The plate's first tap is 346 samples in: sound within 16 blocks, so no clearing
        // of the shared memory (10 blocks of silence) came first.
        float any = 0.0f;
        for (int b = 0; b < 16; b++) {
            rig.block(&x[b * kB], l, r);
            for (int i = 0; i < kB; i++) any = fmaxf(any, fabsf(l[i]) + fabsf(r[i]));
        }
        TEST_ASSERT_GREATER_THAN_FLOAT(0.0f, any);
        std::vector<float> pmem(dsp::PlateReverb::kMemFloats);
        dsp::PlateReverb plate(kFs);
        plate.init(pmem.data());
        plate.setSize(0.6f);
        plate.setTone(0.4f);
        plate.setMod(0.5f);
        plate.setPreDelayMs(0.0f);
        for (int b = 0; b < 16; b++) plate.process(&x[b * kB], &x[b * kB], rl, rr, kB);
        TEST_ASSERT_EQUAL_MEMORY(rl, l, sizeof(l));
    }
    {
        DelayRig rig;
        rig.d.setDelaySamples(480.0f);
        rig.d.setType(DelayBank::kAnalog);
        std::vector<float> line(kLen);
        dsp::AnalogDelay ref(kFs);
        ref.init(line.data(), kLen);
        ref.setDelaySamples(480.0f);
        ref.setFeedback(0.6f);
        ref.setTone(0.5f);
        ref.setModDepth(0.3f);
        float l[kB], r[kB], rl[kB], rr[kB];
        for (size_t at = 0; at + kB <= x.size(); at += kB) {
            rig.d.process(&x[at], l, r, kB);
            ref.process(&x[at], rl, rr, kB);
            TEST_ASSERT_EQUAL_MEMORY(rl, l, sizeof(l));
        }
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_hall_and_gated_are_exactly_reverb80);
    RUN_TEST(test_digital_is_exactly_studio_delay);
    RUN_TEST(test_reverb_changes_fade_clear_and_restart);
    RUN_TEST(test_delay_changes_fade_clear_and_restart);
    RUN_TEST(test_nothing_is_left_over_from_the_old_effect);
    RUN_TEST(test_change_from_clear_lines_is_immediate);
    return UNITY_END();
}
