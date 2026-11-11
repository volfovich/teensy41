// Teensy 4.1 PSRAM verification firmware.
//   Build + flash:  pio run -e psram_test -t upload
//   Watch:          pio device monitor -e psram_test
//
// Tests (whole detected PSRAM, starting at 0x70000000):
//   1. detection        external_psram_size reported by the core's startup ID probe
//   2. extmem_malloc    allocator works and returns PSRAM addresses (first pass only)
//   3. data bus         walking 1s / 0s at start, middle, end
//   4. address bus      power-of-two offsets, catches stuck/shorted address bits and aliasing
//                       (e.g. a chip that is really smaller than reported)
//   5. byte/halfword    sub-word writes hit the right byte lanes
//   6. full fills       fixed patterns, address-as-data, and pseudo-random fills, with
//                       read/write throughput
// The D-cache is flushed+invalidated before every verify so reads come from the chip.
// Passes repeat forever (soak test): marginal solder joints often fail only intermittently.

#include <Arduino.h>

extern "C" uint8_t external_psram_size;

static volatile uint32_t* const kBase = reinterpret_cast<volatile uint32_t*>(0x70000000);
static uint32_t gWords = 0;
static uint32_t gPassErrors = 0;
static uint32_t gBadBits = 0;     // OR of (expected ^ actual) over all errors
static uint32_t gTotalErrors = 0;

static constexpr uint32_t kMaxPrinted = 8;

static void flushRange(volatile void* p, uint32_t bytes) {
    arm_dcache_flush_delete(const_cast<void*>(p), bytes);
}

static void flushAll() { flushRange(kBase, gWords * 4); }

static void recordError(const char* test, volatile uint32_t* addr, uint32_t expect, uint32_t got,
                        uint32_t& count) {
    gBadBits |= expect ^ got;
    ++gPassErrors;
    if (count++ < kMaxPrinted) {
        Serial.printf("    %s: addr 0x%08lX expected 0x%08lX got 0x%08lX (xor 0x%08lX)\n", test,
                      (uint32_t)addr, expect, got, expect ^ got);
    }
}

static inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dUL;
    x ^= x >> 15; x *= 0x846ca68bUL;
    x ^= x >> 16;
    return x;
}

static void result(const char* name, uint32_t errs) {
    if (errs) Serial.printf("  %-28s FAIL (%lu errors)\n", name, errs);
    else      Serial.printf("  %-28s ok\n", name);
}

// ---- 3. data bus --------------------------------------------------------------
static void testDataBus() {
    uint32_t errs = 0;
    const uint32_t idx[3] = {0, gWords / 2, gWords - 1};
    for (uint32_t k = 0; k < 3; ++k) {
        volatile uint32_t* p = kBase + idx[k];
        for (int bit = 0; bit < 32; ++bit) {
            for (int inv = 0; inv < 2; ++inv) {
                const uint32_t v = inv ? ~(1UL << bit) : (1UL << bit);
                *p = v;
                flushRange(p, 4);
                const uint32_t got = *p;
                if (got != v) recordError("data bus", p, v, got, errs);
            }
        }
    }
    result("data bus (walking 1/0)", errs);
}

// ---- 4. address bus -----------------------------------------------------------
static void testAddressBus() {
    uint32_t errs = 0;
    const uint32_t pat = 0xAAAAAAAAUL, anti = 0x55555555UL;

    // Seed every power-of-two word offset (+ offset 0) with the pattern.
    kBase[0] = pat;
    for (uint32_t off = 1; off < gWords; off <<= 1) kBase[off] = pat;

    // Write anti to each offset in turn; nothing else may change (aliasing / shorted lines).
    for (uint32_t test = 0; test < gWords; test = test ? (test << 1) : 1) {
        kBase[test] = anti;
        flushAll();
        if (kBase[0] != (test == 0 ? anti : pat))
            recordError("addr bus", kBase, test == 0 ? anti : pat, kBase[0], errs);
        for (uint32_t off = 1; off < gWords; off <<= 1) {
            const uint32_t expect = (off == test) ? anti : pat;
            const uint32_t got = kBase[off];
            if (got != expect) recordError("addr bus", kBase + off, expect, got, errs);
        }
        kBase[test] = pat;
    }
    result("address bus / aliasing", errs);
}

// ---- 5. byte & halfword lanes ---------------------------------------------------
static void testSubWord() {
    uint32_t errs = 0;
    const uint32_t n = 65536;  // bytes, at start and end of the chip(s)
    const uint32_t starts[2] = {0, gWords * 4 - n};
    for (uint32_t s : starts) {
        volatile uint8_t* b = reinterpret_cast<volatile uint8_t*>(kBase) + s;
        for (uint32_t i = 0; i < n; ++i) b[i] = (uint8_t)(i * 7 + 3);
        flushRange(b, n);
        volatile uint32_t* w = reinterpret_cast<volatile uint32_t*>(b);
        for (uint32_t i = 0; i < n / 4; ++i) {
            const uint32_t j = i * 4;
            const uint32_t e = (uint32_t)(uint8_t)(j * 7 + 3) | (uint32_t)(uint8_t)((j + 1) * 7 + 3) << 8 |
                               (uint32_t)(uint8_t)((j + 2) * 7 + 3) << 16 | (uint32_t)(uint8_t)((j + 3) * 7 + 3) << 24;
            if (w[i] != e) recordError("byte write", w + i, e, w[i], errs);
        }

        volatile uint16_t* h = reinterpret_cast<volatile uint16_t*>(b);
        for (uint32_t i = 0; i < n / 2; ++i) h[i] = (uint16_t)(i * 0x9E37 + 1);
        flushRange(h, n);
        for (uint32_t i = 0; i < n / 4; ++i) {
            const uint32_t e = (uint32_t)(uint16_t)(2 * i * 0x9E37 + 1) |
                               (uint32_t)(uint16_t)((2 * i + 1) * 0x9E37 + 1) << 16;
            if (w[i] != e) recordError("halfword write", w + i, e, w[i], errs);
        }
    }
    result("byte / halfword lanes", errs);
}

// ---- 6. full-region fills -----------------------------------------------------
template <typename Gen>
static void fillVerify(const char* name, Gen gen) {
    uint32_t errs = 0;
    const uint32_t t0 = micros();
    for (uint32_t i = 0; i < gWords; ++i) kBase[i] = gen(i);
    flushAll();
    const uint32_t t1 = micros();
    for (uint32_t i = 0; i < gWords; ++i) {
        const uint32_t got = kBase[i];
        const uint32_t e = gen(i);
        if (got != e) recordError(name, kBase + i, e, got, errs);
    }
    const uint32_t t2 = micros();
    const float mb = gWords * 4.0f / 1048576.0f;
    if (errs) {
        Serial.printf("  %-28s FAIL (%lu errors)\n", name, errs);
    } else {
        Serial.printf("  %-28s ok   write %5.1f MB/s  read %5.1f MB/s\n", name,
                      (double)(mb / ((t1 - t0) * 1e-6f)), (double)(mb / ((t2 - t1) * 1e-6f)));
    }
}

static void testFills(uint32_t pass) {
    const uint32_t fixed[] = {0x00000000UL, 0xFFFFFFFFUL, 0x55555555UL, 0xAAAAAAAAUL,
                              0x33333333UL, 0xCCCCCCCCUL, 0x0F0F0F0FUL, 0xF0F0F0F0UL};
    char name[32];
    for (uint32_t v : fixed) {
        snprintf(name, sizeof(name), "fill 0x%08lX", v);
        fillVerify(name, [v](uint32_t) { return v; });
    }
    fillVerify("fill address", [](uint32_t i) { return 0x70000000UL + i * 4; });
    fillVerify("fill ~address", [](uint32_t i) { return ~(0x70000000UL + i * 4); });
    for (uint32_t s = 0; s < 3; ++s) {
        const uint32_t seed = pass * 0x1000193UL + s * 0x9E3779B9UL + 1;
        snprintf(name, sizeof(name), "random seed %08lX", seed);
        fillVerify(name, [seed](uint32_t i) { return hash32(i ^ seed); });
    }
}

// ---- 2. extmem_malloc -------------------------------------------------------------
static void testMalloc() {
    uint32_t errs = 0;
    const uint32_t bytes = 1024 * 1024;
    uint32_t* p = static_cast<uint32_t*>(extmem_malloc(bytes));
    const uint32_t a = (uint32_t)p;
    if (!p || a < 0x70000000UL || a + bytes > 0x70000000UL + gWords * 4) {
        Serial.printf("  %-28s FAIL (returned %p)\n", "extmem_malloc 1 MB", p);
        ++gPassErrors;
        if (p) extmem_free(p);
        return;
    }
    volatile uint32_t* v = p;
    for (uint32_t i = 0; i < bytes / 4; ++i) v[i] = hash32(i);
    flushRange(v, bytes);
    for (uint32_t i = 0; i < bytes / 4; ++i)
        if (v[i] != hash32(i)) recordError("extmem_malloc", v + i, hash32(i), v[i], errs);
    extmem_free(p);
    Serial.printf("  %-28s %s (at 0x%08lX)\n", "extmem_malloc 1 MB", errs ? "FAIL" : "ok", a);
}

static void diagnoseBits() {
    if (!gBadBits) return;
    // In QPI mode each byte goes out as two nibbles over SIO0..3, so bit b of every byte
    // travels on SIO (b % 4). A bad line shows up as a repeating bit pattern.
    uint32_t lines = 0;
    for (int b = 0; b < 32; ++b)
        if (gBadBits & (1UL << b)) lines |= 1UL << (b % 4);
    Serial.printf("  bad bit mask 0x%08lX -> suspect PSRAM data line(s):", gBadBits);
    for (int l = 0; l < 4; ++l)
        if (lines & (1UL << l)) Serial.printf(" SIO%d", l);
    Serial.println();
}

void setup() {
    Serial.begin(115200);
    const uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 4000) {}
    delay(200);

    Serial.println();
    Serial.println("=== Teensy 4.1 PSRAM test ===");
    Serial.printf("CPU %lu MHz, external_psram_size = %u MB\n", F_CPU_ACTUAL / 1000000, external_psram_size);

    if (external_psram_size == 0) {
        Serial.println("RESULT: NO PSRAM DETECTED. The startup ID read failed. Check chip orientation (pin 1 dot),");
        Serial.println("        CS (pin 1), SCLK (pin 6), SIO0-3 (pins 5,2,3,7), VCC (8), GND (4) solder joints.");
        while (true) delay(1000);
    }
    gWords = (uint32_t)external_psram_size * 1048576UL / 4;
    Serial.printf("Testing 0x70000000 - 0x%08lX (%lu bytes)\n", 0x70000000UL + gWords * 4 - 1, gWords * 4);
}

void loop() {
    static uint32_t pass = 0;
    ++pass;
    gPassErrors = 0;
    const uint32_t t0 = millis();
    Serial.printf("\n--- pass %lu ---\n", pass);

    if (pass == 1) testMalloc();  // before raw tests overwrite the allocator's heap
    testDataBus();
    testAddressBus();
    testSubWord();
    testFills(pass);

    gTotalErrors += gPassErrors;
    diagnoseBits();
    Serial.printf("RESULT pass %lu: %s  [%u MB PSRAM] (%lu errors this pass, %lu total, %.1f s)\n", pass,
                  gPassErrors ? "FAIL" : "OK", external_psram_size, gPassErrors, gTotalErrors, (double)((millis() - t0) / 1000.0f));
}
