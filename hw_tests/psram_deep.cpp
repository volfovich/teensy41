// Teensy 4.1 PSRAM deep inspection.
//   pio run -e psram_deep -t upload && pio device monitor -e psram_deep
//
// One-time deep suite (~3-4 min), then an endless soak loop with temperature readout:
//   A. chip identity      raw JEDEC ID read over SPI: manufacturer, Known-Good-Die byte, density
//   B. allocator          near-full extmem_malloc, many small blocks, overlap check
//   C. data/address bus   walking 1/0 on 3 locations; power-of-two address aliasing
//   D. March C-           6 data backgrounds, ascending+descending read/modify/write
//                         (stuck-at, transition, coupling faults between neighbouring cells)
//   E. random access      full-permutation scattered writes/reads (address decoder, cache-miss path)
//   F. memcpy             random misaligned lengths/offsets crossing the chip's 1 KB pages
//   G. ISR concurrency    48 kHz timer ISR running a PSRAM ring buffer (like an audio delay line)
//                         while the main loop hammers the rest of the chip
//   H. retention          fill, sit idle 60 s (chip self-refresh only), verify
//   I. clock margin       repeat key tests at 88 / 105.6 (default) / 120 / 132 MHz,
//                         and 144 MHz (beyond the 133 MHz chip spec: informational only)
//   J. performance        sequential and random-access throughput / latency
// The D-cache is flushed+invalidated before every verify so reads come from the chip.

#include <Arduino.h>
#include <IntervalTimer.h>

extern "C" uint8_t external_psram_size;

static volatile uint32_t* const kBase = reinterpret_cast<volatile uint32_t*>(0x70000000);
static uint32_t gWords = 0;
static uint32_t gBytes = 0;

// ---------------------------------------------------------------------------------------------
// Error bookkeeping
// ---------------------------------------------------------------------------------------------
static constexpr uint32_t kMaxPrinted = 6;
static uint32_t gTestErrs = 0;     // errors in the current test
static uint32_t gBadBits = 0;      // OR of (expected ^ actual), all tests

struct Result { char name[40]; uint32_t errs; bool informational; };
static Result gResults[48];
static uint32_t gNumResults = 0;

static void beginTest() { gTestErrs = 0; }

static void err(const char* what, uint32_t addr, uint32_t expect, uint32_t got) {
    gBadBits |= expect ^ got;
    if (gTestErrs++ < kMaxPrinted)
        Serial.printf("      %s: addr 0x%08lX expected 0x%08lX got 0x%08lX (xor 0x%08lX)\n", what, addr,
                      expect, got, expect ^ got);
}

static void endTest(const char* name, const char* info = nullptr, bool informational = false) {
    if (gNumResults < 48) {
        Result& r = gResults[gNumResults++];
        snprintf(r.name, sizeof(r.name), "%s", name);
        r.errs = gTestErrs;
        r.informational = informational;
    }
    Serial.printf("  %-34s %s", name, gTestErrs ? "FAIL" : "ok  ");
    if (gTestErrs) Serial.printf(" (%lu errors)", gTestErrs);
    if (info) Serial.printf("  %s", info);
    Serial.println();
}

static inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dUL;
    x ^= x >> 15; x *= 0x846ca68bUL;
    x ^= x >> 16;
    return x;
}

static void flushRange(volatile void* p, uint32_t bytes) { arm_dcache_flush_delete(const_cast<void*>(p), bytes); }
static void flushAll() { flushRange(kBase, gBytes); }

static float mbps(uint32_t bytes, uint32_t us) { return us ? bytes / (float)us : 0.0f; }  // bytes/us == MB/s

// ---------------------------------------------------------------------------------------------
// Low-level FlexSPI2 access (runs from ITCM; PSRAM must not be touched while in SPI mode)
// ---------------------------------------------------------------------------------------------
FASTRUN static void flexspiIdle() {
    while ((FLEXSPI2_STS0 & (FLEXSPI_STS0_ARBIDLE | FLEXSPI_STS0_SEQIDLE)) !=
           (FLEXSPI_STS0_ARBIDLE | FLEXSPI_STS0_SEQIDLE)) {}
}

FASTRUN static void flexspiCommand(uint32_t index, uint32_t addr) {
    FLEXSPI2_IPCR0 = addr;
    FLEXSPI2_IPCR1 = FLEXSPI_IPCR1_ISEQID(index);
    FLEXSPI2_IPCMD = FLEXSPI_IPCMD_TRG;
    while (!(FLEXSPI2_INTR & FLEXSPI_INTR_IPCMDDONE)) {}
    FLEXSPI2_INTR = FLEXSPI_INTR_IPCMDDONE;
}

// Uses the LUT sequences the Teensy core installed at boot:
//   0 = exit QPI, 3 = read ID (SPI, 0x9F), 4 = enter QPI.
FASTRUN static void readChipId(uint8_t id[8]) {
    flushAll();                          // no dirty PSRAM lines may be written back while in SPI mode
    __disable_irq();
    flexspiIdle();
    flexspiCommand(0, 0);                // exit QPI
    FLEXSPI2_IPRXFCR |= FLEXSPI_IPRXFCR_CLRIPRXF;
    FLEXSPI2_IPCR0 = 0;
    FLEXSPI2_IPCR1 = FLEXSPI_IPCR1_ISEQID(3) | FLEXSPI_IPCR1_IDATSZ(8);
    FLEXSPI2_IPCMD = FLEXSPI_IPCMD_TRG;
    while (!(FLEXSPI2_INTR & FLEXSPI_INTR_IPCMDDONE)) {}
    const uint32_t w0 = FLEXSPI2_RFDR0, w1 = FLEXSPI2_RFDR1;
    FLEXSPI2_INTR = FLEXSPI_INTR_IPCMDDONE | FLEXSPI_INTR_IPRXWA;
    FLEXSPI2_IPRXFCR |= FLEXSPI_IPRXFCR_CLRIPRXF;
    flexspiCommand(4, 0);                // back to QPI
    __enable_irq();
    for (int i = 0; i < 4; ++i) { id[i] = w0 >> (8 * i); id[4 + i] = w1 >> (8 * i); }
}

struct ClockSetting { const char* name; uint8_t podf, sel; float mhz; bool beyondSpec; };
static const ClockSetting kClocks[] = {
    {"88.0 MHz",  5, 3,  88.0f, false},
    {"105.6 MHz", 4, 3, 105.6f, false},   // Teensy core default
    {"120.0 MHz", 5, 1, 120.0f, false},
    {"132.0 MHz", 3, 3, 132.0f, false},
    {"144.0 MHz", 4, 1, 144.0f, true},    // > 133 MHz APS6404L max: informational
};
static constexpr int kDefaultClock = 1;

FASTRUN static void setPsramClock(const ClockSetting& c) {
    flushAll();
    __disable_irq();
    flexspiIdle();
    FLEXSPI2_MCR0 |= FLEXSPI_MCR0_MDIS;
    CCM_CCGR7 &= ~CCM_CCGR7_FLEXSPI2(CCM_CCGR_ON);
    CCM_CBCMR = (CCM_CBCMR & ~(CCM_CBCMR_FLEXSPI2_PODF_MASK | CCM_CBCMR_FLEXSPI2_CLK_SEL_MASK)) |
                CCM_CBCMR_FLEXSPI2_PODF(c.podf) | CCM_CBCMR_FLEXSPI2_CLK_SEL(c.sel);
    CCM_CCGR7 |= CCM_CCGR7_FLEXSPI2(CCM_CCGR_ON);
    FLEXSPI2_MCR0 &= ~FLEXSPI_MCR0_MDIS;
    __enable_irq();
    delayMicroseconds(50);
}

// ---------------------------------------------------------------------------------------------
// Test building blocks
// ---------------------------------------------------------------------------------------------
template <typename Gen>
static void fill(uint32_t first, uint32_t count, Gen gen) {
    for (uint32_t i = first; i < first + count; ++i) kBase[i] = gen(i);
}

template <typename Gen>
static void verify(const char* what, uint32_t first, uint32_t count, Gen gen) {
    for (uint32_t i = first; i < first + count; ++i) {
        const uint32_t got = kBase[i], e = gen(i);
        if (got != e) err(what, 0x70000000UL + 4 * i, e, got);
    }
}

// Returns write and read throughput via out-params.
template <typename Gen>
static void fillVerifyAll(const char* what, Gen gen, float* wr = nullptr, float* rd = nullptr) {
    const uint32_t t0 = micros();
    fill(0, gWords, gen);
    flushAll();
    const uint32_t t1 = micros();
    verify(what, 0, gWords, gen);
    const uint32_t t2 = micros();
    if (wr) *wr = mbps(gBytes, t1 - t0);
    if (rd) *rd = mbps(gBytes, t2 - t1);
}

static void testDataBus() {
    beginTest();
    const uint32_t idx[3] = {0, gWords / 2 + 7, gWords - 1};
    for (uint32_t k : idx) {
        volatile uint32_t* p = kBase + k;
        for (int bit = 0; bit < 32; ++bit)
            for (int inv = 0; inv < 2; ++inv) {
                const uint32_t v = inv ? ~(1UL << bit) : (1UL << bit);
                *p = v;
                flushRange(p, 4);
                const uint32_t got = *p;
                if (got != v) err("data bus", (uint32_t)p, v, got);
            }
    }
    endTest("data bus walking 1/0");
}

static void testAddressBus() {
    beginTest();
    const uint32_t pat = 0xAAAAAAAAUL, anti = 0x55555555UL;
    kBase[0] = pat;
    for (uint32_t off = 1; off < gWords; off <<= 1) kBase[off] = pat;
    for (uint32_t t = 0; t < gWords; t = t ? (t << 1) : 1) {
        kBase[t] = anti;
        flushAll();
        for (uint32_t off = 0; off < gWords; off = off ? (off << 1) : 1) {
            const uint32_t e = (off == t) ? anti : pat, got = kBase[off];
            if (got != e) err("addr bus", 0x70000000UL + 4 * off, e, got);
        }
        kBase[t] = pat;
    }
    endTest("address bus / aliasing");
}

// March C-: {up w0} {up r0 w1} {up r1 w0} {down r0 w1} {down r1 w0} {any r0}
static void testMarch(uint32_t bg) {
    beginTest();
    const uint32_t d = bg, nd = ~bg;
    const uint32_t t0 = millis();
    for (uint32_t i = 0; i < gWords; ++i) kBase[i] = d;
    flushAll();
    auto up = [&](uint32_t r, uint32_t w) {
        for (uint32_t i = 0; i < gWords; ++i) {
            const uint32_t got = kBase[i];
            if (got != r) err("march", 0x70000000UL + 4 * i, r, got);
            kBase[i] = w;
        }
        flushAll();
    };
    auto down = [&](uint32_t r, uint32_t w) {
        for (uint32_t i = gWords; i-- > 0;) {
            const uint32_t got = kBase[i];
            if (got != r) err("march", 0x70000000UL + 4 * i, r, got);
            kBase[i] = w;
        }
        flushAll();
    };
    up(d, nd);
    up(nd, d);
    down(d, nd);
    down(nd, d);
    verify("march", 0, gWords, [d](uint32_t) { return d; });
    char name[40], info[32];
    snprintf(name, sizeof(name), "March C- bg 0x%08lX", bg);
    snprintf(info, sizeof(info), "%.1f s", (double)((millis() - t0) / 1000.0f));
    endTest(name, info);
}

// Full-period permutation of word indices: (i * A + B) mod 2^k, A odd.
static void testRandomAccess(uint32_t seed, float* latencyNs = nullptr) {
    beginTest();
    const uint32_t mask = gWords - 1;  // gWords is a power of two (8 MB / 4)
    const uint32_t a1 = (hash32(seed) | 1), b1 = hash32(seed + 1);
    const uint32_t a2 = (hash32(seed + 2) | 1), b2 = hash32(seed + 3);
    for (uint32_t i = 0; i < gWords; ++i) {
        const uint32_t idx = (i * a1 + b1) & mask;
        kBase[idx] = hash32(idx ^ seed);
    }
    flushAll();
    const uint32_t t0 = micros();
    for (uint32_t i = 0; i < gWords; ++i) {
        const uint32_t idx = (i * a2 + b2) & mask;
        const uint32_t got = kBase[idx], e = hash32(idx ^ seed);
        if (got != e) err("random", 0x70000000UL + 4 * idx, e, got);
    }
    const uint32_t us = micros() - t0;
    if (latencyNs) *latencyNs = us * 1000.0f / gWords;
    char name[40];
    snprintf(name, sizeof(name), "random-order access #%lu", seed);
    endTest(name);
}

// Misaligned memcpy across 1 KB page boundaries, RAM->PSRAM, PSRAM->PSRAM, PSRAM->RAM.
DMAMEM static uint8_t gSrc[16384] __attribute__((aligned(32)));
DMAMEM static uint8_t gDst[16384] __attribute__((aligned(32)));

static void testMemcpy() {
    beginTest();
    uint8_t* const psram = reinterpret_cast<uint8_t*>(0x70000000);
    for (uint32_t i = 0; i < sizeof(gSrc); ++i) gSrc[i] = (uint8_t)hash32(i * 31 + 7);
    uint32_t r = 0x12345;
    for (int trial = 0; trial < 3000; ++trial) {
        r = hash32(r + trial);
        const uint32_t len = 1 + (r % 8192);
        const uint32_t srcOff = hash32(r ^ 1) % (sizeof(gSrc) - len);
        // Bias destinations to straddle 1 KB page boundaries with odd misalignment.
        const uint32_t page = hash32(r ^ 2) % (gBytes / 1024 - 20);
        const uint32_t dstA = page * 1024 + 1024 - (hash32(r ^ 3) % 64);
        const uint32_t dstB = (dstA + gBytes / 2 + (hash32(r ^ 4) % 977)) % (gBytes - len);

        memcpy(psram + dstA, gSrc + srcOff, len);          // RAM -> PSRAM
        flushRange(psram + dstA, len);
        memcpy(psram + dstB, psram + dstA, len);           // PSRAM -> PSRAM
        flushRange(psram + dstB, len);
        memcpy(gDst, psram + dstB, len);                   // PSRAM -> RAM
        if (memcmp(gDst, gSrc + srcOff, len) != 0) {
            for (uint32_t k = 0; k < len; ++k)
                if (gDst[k] != gSrc[srcOff + k]) {
                    err("memcpy", 0x70000000UL + dstB + k, gSrc[srcOff + k], gDst[k]);
                    break;
                }
        }
    }
    endTest("memcpy misaligned/page-crossing x3000");
}

// ---- G. ISR concurrency ----------------------------------------------------------------
static constexpr uint32_t kRingWords = 65536;           // 256 KB ring at the end of PSRAM
static constexpr uint32_t kRingLag = 4096;               // read back 4096 samples later
static volatile uint32_t* gRing = nullptr;
static volatile uint32_t gIsrCount = 0, gIsrErrs = 0, gIsrFirstBad = 0;

FASTRUN static void ringIsr() {
    const uint32_t k = gIsrCount;
    gRing[k & (kRingWords - 1)] = hash32(k ^ 0xA5A5A5A5UL);
    if (k >= kRingLag) {
        const uint32_t j = k - kRingLag;
        const uint32_t got = gRing[j & (kRingWords - 1)];
        if (got != hash32(j ^ 0xA5A5A5A5UL)) {
            if (!gIsrErrs) gIsrFirstBad = j;
            ++gIsrErrs;
        }
    }
    gIsrCount = k + 1;
}

static void testIsrConcurrency() {
    beginTest();
    const uint32_t mainWords = gWords - kRingWords;
    gRing = kBase + mainWords;
    gIsrCount = gIsrErrs = 0;
    IntervalTimer t;
    t.begin(ringIsr, 1000000.0f / 48000.0f);
    for (uint32_t round = 0; round < 6; ++round) {
        const uint32_t seed = 0xC0FFEE00UL + round;
        fill(0, mainWords, [seed](uint32_t i) { return hash32(i ^ seed); });
        flushRange(kBase, mainWords * 4);
        verify("main vs ISR", 0, mainWords, [seed](uint32_t i) { return hash32(i ^ seed); });
    }
    t.end();
    if (gIsrErrs) {
        Serial.printf("      ISR ring: %lu bad samples (first at sample %lu)\n", gIsrErrs, gIsrFirstBad);
        gTestErrs += gIsrErrs;
    }
    char info[48];
    snprintf(info, sizeof(info), "%lu ISR samples @48 kHz", gIsrCount);
    endTest("ISR ring buffer + main traffic", info);
}

static void testRetention(uint32_t seconds) {
    beginTest();
    const uint32_t seed = 0x5EED0000UL ^ millis();
    fill(0, gWords, [seed](uint32_t i) { return hash32(i ^ seed); });
    flushAll();
    Serial.printf("      idle %lu s ", seconds);
    for (uint32_t s = 0; s < seconds; ++s) {
        delay(1000);
        if (s % 10 == 9) Serial.print('.');
    }
    Serial.println();
    verify("retention", 0, gWords, [seed](uint32_t i) { return hash32(i ^ seed); });
    char name[40];
    snprintf(name, sizeof(name), "retention after %lu s idle", seconds);
    endTest(name);
}

// Key tests used for clock margin and soak.
static void quickSuite(const char* tag, uint32_t seed, bool informational) {
    char name[40];
    float wr = 0, rd = 0;
    beginTest();
    fillVerifyAll("fill", [](uint32_t) { return 0x55555555UL; });
    fillVerifyAll("fill", [](uint32_t) { return 0xAAAAAAAAUL; });
    fillVerifyAll("fill", [](uint32_t i) { return ~(0x70000000UL + 4 * i); });
    fillVerifyAll("fill", [seed](uint32_t i) { return hash32(i ^ seed); }, &wr, &rd);
    fillVerifyAll("fill", [seed](uint32_t i) { return ~hash32(i ^ seed); });
    char info[48];
    snprintf(info, sizeof(info), "wr %.1f / rd %.1f MB/s", (double)wr, (double)rd);
    snprintf(name, sizeof(name), "%s fills", tag);
    endTest(name, info, informational);
    testRandomAccess(seed);
    if (informational && gNumResults) gResults[gNumResults - 1].informational = true;
}

// ---- B. allocator ---------------------------------------------------------------------
static void testAllocator() {
    beginTest();
    // Largest single block: binary search down from the full size.
    uint32_t lo = 0, hi = gBytes;
    while (hi - lo > 4096) {
        const uint32_t mid = (lo + hi) / 2;
        void* p = extmem_malloc(mid);
        if (p) { lo = mid; extmem_free(p); } else hi = mid;
    }
    uint32_t* big = static_cast<uint32_t*>(extmem_malloc(lo));
    if (!big) { ++gTestErrs; }
    else {
        volatile uint32_t* v = big;
        for (uint32_t i = 0; i < lo / 4; ++i) v[i] = hash32(i + 99);
        flushRange(v, lo);
        for (uint32_t i = 0; i < lo / 4; ++i)
            if (v[i] != hash32(i + 99)) err("big block", (uint32_t)(v + i), hash32(i + 99), v[i]);
        extmem_free(big);
    }
    char info[48];
    snprintf(info, sizeof(info), "largest block %.2f MB", (double)(lo / 1048576.0f));
    endTest("extmem_malloc largest block", info);

    // Many small blocks with unique contents: catches overlapping allocations.
    beginTest();
    static void* blocks[4096];
    static uint32_t sizes[4096];
    uint32_t n = 0, total = 0;
    while (n < 4096) {
        const uint32_t sz = 4 * (1 + hash32(n) % 1024);   // 4 B .. 4 KB
        void* p = extmem_malloc(sz);
        if (!p) break;
        const uint32_t a = (uint32_t)p;
        // When PSRAM is full, extmem_malloc silently falls back to malloc() (internal RAM2):
        // that marks "PSRAM exhausted", not an error. Anything else outside PSRAM is a bug.
        if (a >= 0x20200000UL && a < 0x20280000UL) { free(p); break; }
        if (a < 0x70000000UL || a + sz > 0x70000000UL + gBytes) err("alloc range", a, 0, 0);
        blocks[n] = p; sizes[n] = sz; total += sz;
        volatile uint32_t* w = static_cast<uint32_t*>(p);
        for (uint32_t k = 0; k < sz / 4; ++k) w[k] = (n << 16) ^ k;
        ++n;
    }
    flushAll();
    for (uint32_t b = 0; b < n; ++b) {
        volatile uint32_t* w = static_cast<uint32_t*>(blocks[b]);
        for (uint32_t k = 0; k < sizes[b] / 4; ++k)
            if (w[k] != ((b << 16) ^ k)) err("block overlap", (uint32_t)(w + k), (b << 16) ^ k, w[k]);
    }
    for (uint32_t b = 0; b < n; ++b) extmem_free(blocks[b]);
    void* after = extmem_malloc(lo);   // everything freed -> big block available again
    if (!after) { Serial.println("      fragmentation: largest block unavailable after free-all"); ++gTestErrs; }
    else extmem_free(after);
    snprintf(info, sizeof(info), "%lu blocks, %.2f MB", n, (double)(total / 1048576.0f));
    endTest("extmem_malloc small blocks", info);
}

static void printBitDiagnosis() {
    if (!gBadBits) return;
    uint32_t lines = 0;
    for (int b = 0; b < 32; ++b)
        if (gBadBits & (1UL << b)) lines |= 1UL << (b % 4);
    Serial.printf("  bad bit mask 0x%08lX -> suspect PSRAM data line(s):", gBadBits);
    for (int l = 0; l < 4; ++l)
        if (lines & (1UL << l)) Serial.printf(" SIO%d", l);
    Serial.println();
}

static bool gSuiteFailed = false;

void setup() {
    Serial.begin(115200);
    const uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 4000) {}
    delay(200);

    Serial.println("\n=== Teensy 4.1 PSRAM DEEP INSPECTION ===");
    Serial.printf("CPU %lu MHz, die temp %.1f C, external_psram_size = %u MB\n", F_CPU_ACTUAL / 1000000,
                  (double)tempmonGetTemp(), external_psram_size);
    if (external_psram_size == 0) {
        Serial.println("RESULT: NO PSRAM DETECTED");
        while (true) delay(1000);
    }
    gBytes = (uint32_t)external_psram_size * 1048576UL;
    gWords = gBytes / 4;
    const uint32_t tStart = millis();

    // ---- A. identity (before allocator use; contents preserved check) ----
    Serial.println("\n[A] chip identity");
    beginTest();
    fill(0, gWords, [](uint32_t i) { return hash32(i ^ 0x1D1D1D1DUL); });
    uint8_t id[8];
    readChipId(id);
    verify("data across ID read", 0, gWords, [](uint32_t i) { return hash32(i ^ 0x1D1D1D1DUL); });
    Serial.printf("      raw ID: %02X %02X | %02X %02X %02X %02X %02X %02X\n", id[0], id[1], id[2], id[3],
                  id[4], id[5], id[6], id[7]);
    const char* mfr = id[0] == 0x0D ? "AP Memory (APS6404L family)" : id[0] == 0x9D ? "ISSI" : "UNKNOWN";
    Serial.printf("      manufacturer 0x%02X = %s\n", id[0], mfr);
    Serial.printf("      KGD 0x%02X = %s\n", id[1],
                  id[1] == 0x5D ? "PASS (known good die)" : id[1] == 0x55 ? "FAIL (factory-marked bad die!)" : "unexpected");
    if (id[0] == 0x0D) {
        const uint32_t dens = id[2] >> 5;
        Serial.printf("      density code %lu = %s\n", dens, dens == 2 ? "64 Mbit (8 MB)" : dens == 1 ? "32 Mbit" : dens == 3 ? "128 Mbit" : "?");
    }
    if (id[1] != 0x5D || (id[0] != 0x0D && id[0] != 0x9D)) ++gTestErrs;
    endTest("JEDEC ID / KGD + data preserved");

    // ---- B. allocator (must run before raw tests overwrite the heap) ----
    Serial.println("\n[B] extmem_malloc allocator");
    testAllocator();

    Serial.println("\n[C] buses");
    testDataBus();
    testAddressBus();

    Serial.println("\n[D] March C- (coupling / transition faults)");
    const uint32_t bgs[] = {0x00000000UL, 0x55555555UL, 0x33333333UL, 0x0F0F0F0FUL, 0x00FF00FFUL, 0x0000FFFFUL};
    for (uint32_t bg : bgs) testMarch(bg);

    Serial.println("\n[E] random-order access");
    float lat = 0;
    testRandomAccess(1, &lat);
    testRandomAccess(2);

    Serial.println("\n[F] memcpy");
    testMemcpy();

    Serial.println("\n[G] ISR concurrency");
    testIsrConcurrency();

    Serial.println("\n[H] data retention");
    testRetention(60);

    Serial.println("\n[I] clock margin");
    for (const ClockSetting& c : kClocks) {
        setPsramClock(c);
        Serial.printf("    -- PSRAM clock %s%s\n", c.name, c.beyondSpec ? " (beyond spec, informational)" : "");
        quickSuite(c.name, (uint32_t)(c.mhz * 10), c.beyondSpec);
    }
    setPsramClock(kClocks[kDefaultClock]);
    Serial.println("    -- back to default 105.6 MHz");
    quickSuite("105.6 MHz (restored)", 777, false);

    Serial.println("\n[J] performance @ 105.6 MHz");
    {
        const uint32_t n = sizeof(gSrc);
        uint8_t* const psram = reinterpret_cast<uint8_t*>(0x70000000);
        uint32_t t = micros();
        for (uint32_t off = 0; off < 4 * 1048576; off += n) memcpy(psram + off, gSrc, n);
        flushRange(psram, 4 * 1048576);
        const float w = mbps(4 * 1048576, micros() - t);
        t = micros();
        for (uint32_t off = 0; off < 4 * 1048576; off += n) memcpy(gDst, psram + off, n);
        const float r = mbps(4 * 1048576, micros() - t);
        Serial.printf("      memcpy RAM->PSRAM %.1f MB/s, PSRAM->RAM %.1f MB/s\n", (double)w, (double)r);
        Serial.printf("      random 32-bit read (cache miss) ~%.0f ns each\n", (double)lat);
    }

    // ---- Summary ----
    uint32_t fails = 0, infoFails = 0;
    for (uint32_t i = 0; i < gNumResults; ++i) {
        if (!gResults[i].errs) continue;
        if (gResults[i].informational) ++infoFails; else ++fails;
    }
    Serial.printf("\n=== SUMMARY: %lu tests, %lu failed", gNumResults, fails);
    if (infoFails) Serial.printf(", %lu beyond-spec informational failures", infoFails);
    Serial.printf(" (%.0f s, die temp %.1f C) ===\n", (double)((millis() - tStart) / 1000.0f), (double)tempmonGetTemp());
    for (uint32_t i = 0; i < gNumResults; ++i)
        if (gResults[i].errs)
            Serial.printf("  %s %-34s %lu errors\n", gResults[i].informational ? "info" : "FAIL", gResults[i].name,
                          gResults[i].errs);
    printBitDiagnosis();
    gSuiteFailed = fails != 0;
    Serial.printf("DEEP RESULT: %s\n\nStarting endless soak at 105.6 MHz...\n", gSuiteFailed ? "FAIL" : "PASS");
}

void loop() {
    static uint32_t pass = 0, totalErrs = 0;
    ++pass;
    const uint32_t before = gNumResults;
    quickSuite("soak", 0xABC00000UL + pass, false);
    for (uint32_t i = before; i < gNumResults; ++i) totalErrs += gResults[i].errs;
    gNumResults = before;  // don't grow the results table
    Serial.printf("SOAK pass %lu: total errors %lu, die temp %.1f C\n", pass, totalErrs, (double)tempmonGetTemp());
}
