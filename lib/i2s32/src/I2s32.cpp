#include "I2s32.h"

#include <Arduino.h>
#include <DMAChannel.h>

namespace i2s32 {
namespace {

constexpr size_t kHalfWords = 2 * kBlock;                    // one block, interleaved L/R
constexpr size_t kHalfBytes = kHalfWords * sizeof(int32_t);
constexpr uint32_t kUnmuteAfterBlocks = 16;

// Circular DMA buffers: two halves of one block each. 32-byte aligned and a multiple of
// 32 bytes long, so cache maintenance never touches neighbouring data.
DMAMEM __attribute__((aligned(32))) int32_t rxBuf[2 * kHalfWords];
DMAMEM __attribute__((aligned(32))) int32_t txBuf[2 * kHalfWords];

DMAChannel rxDma(false);
DMAChannel txDma(false);

volatile Callback g_callback = nullptr;
int g_mutePin = -1;
volatile bool g_userMuted = false;
uint32_t g_periodCycles = 0;

volatile uint32_t g_blocks = 0;
volatile uint32_t g_lastCycles = 0;
volatile uint32_t g_maxCycles = 0;
volatile uint32_t g_overruns = 0;
volatile uint32_t g_fifoErrors = 0;
volatile int32_t g_txLeadFrames = 0;

// PLL4 (audio PLL) = 24 MHz * (div + num/denom). Same sequence as the Audio Library's
// set_audioClock() (utility/imxrt_hw.cpp), copied so we do not link that library.
void setAudioPll(uint32_t div, uint32_t num, uint32_t denom) {
    CCM_ANALOG_PLL_AUDIO = CCM_ANALOG_PLL_AUDIO_BYPASS | CCM_ANALOG_PLL_AUDIO_ENABLE
                         | CCM_ANALOG_PLL_AUDIO_POST_DIV_SELECT(2)   // post divider /1
                         | CCM_ANALOG_PLL_AUDIO_DIV_SELECT(div);
    CCM_ANALOG_PLL_AUDIO_NUM = num & CCM_ANALOG_PLL_AUDIO_NUM_MASK;
    CCM_ANALOG_PLL_AUDIO_DENOM = denom & CCM_ANALOG_PLL_AUDIO_DENOM_MASK;
    CCM_ANALOG_PLL_AUDIO &= ~CCM_ANALOG_PLL_AUDIO_POWERDOWN;
    while (!(CCM_ANALOG_PLL_AUDIO & CCM_ANALOG_PLL_AUDIO_LOCK)) {}
    CCM_ANALOG_MISC2 &= ~(CCM_ANALOG_MISC2_DIV_MSB | CCM_ANALOG_MISC2_DIV_LSB);
    CCM_ANALOG_PLL_AUDIO &= ~CCM_ANALOG_PLL_AUDIO_BYPASS;
}

void configClocksAndSai() {
    CCM_CCGR5 |= CCM_CCGR5_SAI1(CCM_CCGR_ON);

    // Stop and reset SAI1 in case something ran before us.
    I2S1_TCSR = 0;
    I2S1_RCSR = 0;
    I2S1_TCSR = I2S_TCSR_SR | I2S_TCSR_FR;
    I2S1_RCSR = I2S_RCSR_SR | I2S_RCSR_FR;
    I2S1_TCSR = 0;
    I2S1_RCSR = 0;

    // 48 kHz exactly: 24 MHz * (28 + 84/125) = 688.128 MHz; /4 /14 = 12.288 MHz = 256 fs.
    setAudioPll(28, 84, 125);
    CCM_CSCMR1 = (CCM_CSCMR1 & ~CCM_CSCMR1_SAI1_CLK_SEL_MASK) | CCM_CSCMR1_SAI1_CLK_SEL(2); // PLL4
    CCM_CS1CDR = (CCM_CS1CDR & ~(CCM_CS1CDR_SAI1_CLK_PRED_MASK | CCM_CS1CDR_SAI1_CLK_PODF_MASK))
               | CCM_CS1CDR_SAI1_CLK_PRED(4 - 1) | CCM_CS1CDR_SAI1_CLK_PODF(14 - 1);

    // MCLK pin is an output driven from the SAI1 clock root.
    IOMUXC_GPR_GPR1 = (IOMUXC_GPR_GPR1 & ~IOMUXC_GPR_GPR1_SAI1_MCLK1_SEL_MASK)
                    | IOMUXC_GPR_GPR1_SAI1_MCLK_DIR | IOMUXC_GPR_GPR1_SAI1_MCLK1_SEL(0);

    CORE_PIN23_CONFIG = 3;   // MCLK
    CORE_PIN21_CONFIG = 3;   // RX_BCLK
    CORE_PIN20_CONFIG = 3;   // RX_SYNC (LRCLK)
    CORE_PIN7_CONFIG = 3;    // TX_DATA0
    CORE_PIN8_CONFIG = 3;    // RX_DATA0
    IOMUXC_SAI1_RX_DATA0_SELECT_INPUT = 2;

    // Frame format as in the core's AudioOutputI2S::config_i2s(): I2S, two 32-bit words,
    // BCLK = MCLK / 4 = 64 fs. The receiver generates the clocks; the transmitter is
    // synchronous to it.
    //
    // FIFO watermarks are where we differ from the core. The core keeps ~28 words queued in
    // the TX FIFO and ~16 in the RX FIFO (about 22 frames of hidden latency). The DMA
    // answers a request within a few cycles, so we keep the FIFOs nearly empty instead:
    // TX requests when <= 1 word is queued, RX requests as soon as 1 word has arrived.
    I2S1_TMR = 0;
    I2S1_TCR1 = I2S_TCR1_TFW(1);
    I2S1_TCR2 = I2S_TCR2_SYNC(1) | I2S_TCR2_BCP | I2S_TCR2_BCD | I2S_TCR2_DIV(1) | I2S_TCR2_MSEL(1);
    I2S1_TCR3 = I2S_TCR3_TCE;
    I2S1_TCR4 = I2S_TCR4_FRSZ(2 - 1) | I2S_TCR4_SYWD(32 - 1) | I2S_TCR4_MF
              | I2S_TCR4_FSD | I2S_TCR4_FSE | I2S_TCR4_FSP | I2S_TCR4_FCONT;
    I2S1_TCR5 = I2S_TCR5_WNW(32 - 1) | I2S_TCR5_W0W(32 - 1) | I2S_TCR5_FBT(32 - 1);

    I2S1_RMR = 0;
    I2S1_RCR1 = I2S_RCR1_RFW(0);
    I2S1_RCR2 = I2S_RCR2_SYNC(0) | I2S_RCR2_BCP | I2S_RCR2_BCD | I2S_RCR2_DIV(1) | I2S_RCR2_MSEL(1);
    I2S1_RCR3 = I2S_RCR3_RCE;
    I2S1_RCR4 = I2S_RCR4_FRSZ(2 - 1) | I2S_RCR4_SYWD(32 - 1) | I2S_RCR4_MF
              | I2S_RCR4_FSE | I2S_RCR4_FSP | I2S_RCR4_FSD;
    I2S1_RCR5 = I2S_RCR5_WNW(32 - 1) | I2S_RCR5_W0W(32 - 1) | I2S_RCR5_FBT(32 - 1);
}

void configDma() {
    rxDma.begin(true);
    txDma.begin(true);

    // RX: SAI1 receive data register -> rxBuf, 32-bit words, circular.
    rxDma.TCD->SADDR = (void*)&I2S1_RDR0;
    rxDma.TCD->SOFF = 0;
    rxDma.TCD->ATTR = DMA_TCD_ATTR_SSIZE(2) | DMA_TCD_ATTR_DSIZE(2);
    rxDma.TCD->NBYTES_MLNO = 4;
    rxDma.TCD->SLAST = 0;
    rxDma.TCD->DADDR = rxBuf;
    rxDma.TCD->DOFF = 4;
    rxDma.TCD->CITER_ELINKNO = 2 * kHalfWords;
    rxDma.TCD->DLASTSGA = -static_cast<int32_t>(sizeof(rxBuf));
    rxDma.TCD->BITER_ELINKNO = 2 * kHalfWords;
    rxDma.TCD->CSR = DMA_TCD_CSR_INTHALF | DMA_TCD_CSR_INTMAJOR;
    rxDma.triggerAtHardwareEvent(DMAMUX_SOURCE_SAI1_RX);

    // TX: txBuf -> SAI1 transmit data register. No interrupt; RX drives the processing.
    txDma.TCD->SADDR = txBuf;
    txDma.TCD->SOFF = 4;
    txDma.TCD->ATTR = DMA_TCD_ATTR_SSIZE(2) | DMA_TCD_ATTR_DSIZE(2);
    txDma.TCD->NBYTES_MLNO = 4;
    txDma.TCD->SLAST = -static_cast<int32_t>(sizeof(txBuf));
    txDma.TCD->DADDR = (void*)&I2S1_TDR0;
    txDma.TCD->DOFF = 0;
    txDma.TCD->CITER_ELINKNO = 2 * kHalfWords;
    txDma.TCD->DLASTSGA = 0;
    txDma.TCD->BITER_ELINKNO = 2 * kHalfWords;
    txDma.TCD->CSR = 0;
    txDma.triggerAtHardwareEvent(DMAMUX_SOURCE_SAI1_TX);
}

// Flush-to-zero + default-NaN, for this context and for every interrupt context created
// later (FPDSCR is the template the core loads into FPSCR on exception entry). Denormals
// in filter and reverb tails would otherwise cost hundreds of cycles per operation.
void configFloatMode() {
    constexpr uint32_t kFzDn = (1u << 24) | (1u << 25);
    uint32_t fpscr;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= kFzDn;
    __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr));
    *reinterpret_cast<volatile uint32_t*>(0xE000EF3C) |= kFzDn;   // FPDSCR
}

FASTRUN void isr() {
    const uint32_t t0 = ARM_DWT_CYCCNT;

    const uint32_t rxPos = reinterpret_cast<uint32_t>(rxDma.TCD->DADDR);
    const uint32_t txPos = reinterpret_cast<uint32_t>(txDma.TCD->SADDR);
    rxDma.clearInterrupt();

    // The DMA is now filling one half; the other half is complete and ours to read.
    // The matching TX half has just finished playing (TX runs a FIFO's worth ahead of RX),
    // so what we write there starts playing one block period from now.
    const size_t half = (rxPos < reinterpret_cast<uint32_t>(rxBuf) + kHalfBytes) ? 1 : 0;
    int32_t* const rx = rxBuf + half * kHalfWords;
    int32_t* const tx = txBuf + half * kHalfWords;

    // Diagnostic: TX position relative to RX, in frames, wrapped to (-kBlock, kBlock].
    {
        const int32_t rxWord = static_cast<int32_t>((rxPos - reinterpret_cast<uint32_t>(rxBuf)) / 4);
        const int32_t txWord = static_cast<int32_t>((txPos - reinterpret_cast<uint32_t>(txBuf)) / 4);
        int32_t lead = (txWord - rxWord) / 2;
        const int32_t ring = static_cast<int32_t>(2 * kBlock);
        if (lead > ring / 2) lead -= ring;
        if (lead <= -ring / 2) lead += ring;
        g_txLeadFrames = lead;
    }

    arm_dcache_delete(rx, kHalfBytes);            // drop stale cached copy before reading
    const Callback cb = g_callback;
    if (cb) cb(rx, tx, kBlock);
    else memset(tx, 0, kHalfBytes);
    arm_dcache_flush_delete(tx, kHalfBytes);      // push to RAM where the DMA reads it

    // FIFO under/overflow means samples were lost or repeated. Count it and clear the flag.
    if (I2S1_TCSR & I2S_TCSR_FEF) { I2S1_TCSR |= I2S_TCSR_FEF; g_fifoErrors = g_fifoErrors + 1; }
    if (I2S1_RCSR & I2S_RCSR_FEF) { I2S1_RCSR |= I2S_RCSR_FEF; g_fifoErrors = g_fifoErrors + 1; }

    const uint32_t blocks = g_blocks + 1;
    g_blocks = blocks;
    if (blocks == kUnmuteAfterBlocks && g_mutePin >= 0 && !g_userMuted) digitalWriteFast(g_mutePin, HIGH);

    const uint32_t cycles = ARM_DWT_CYCCNT - t0;
    g_lastCycles = cycles;
    if (cycles > g_maxCycles) g_maxCycles = cycles;
    // An interrupt cannot pre-empt itself, so an overrun shows up as either too many
    // cycles, or the next half-buffer interrupt already waiting when we leave.
    if (cycles > g_periodCycles || (DMA_INT & (1u << rxDma.channel))) g_overruns = g_overruns + 1;

    asm("dsb");
}

} // namespace

void begin(const Config& config) {
    g_callback = config.callback;
    g_mutePin = config.mutePin;
    g_userMuted = false;
    if (g_mutePin >= 0) {
        pinMode(g_mutePin, OUTPUT);
        digitalWriteFast(g_mutePin, LOW);
    }

    g_periodCycles = static_cast<uint32_t>(static_cast<uint64_t>(F_CPU_ACTUAL) * kBlock / kSampleRate);
    g_blocks = 0;
    g_lastCycles = g_maxCycles = g_overruns = g_fifoErrors = 0;

    configFloatMode();

    memset(rxBuf, 0, sizeof(rxBuf));
    memset(txBuf, 0, sizeof(txBuf));
    arm_dcache_flush_delete(rxBuf, sizeof(rxBuf));
    arm_dcache_flush_delete(txBuf, sizeof(txBuf));

    configClocksAndSai();
    configDma();

    rxDma.attachInterrupt(isr, config.irqPriority);
    rxDma.enable();
    txDma.enable();

    // Start both directions together. TX is synchronous to RX, so both begin on the same
    // frame and the RX-half -> TX-half mapping in isr() stays fixed.
    __disable_irq();
    I2S1_TCSR = I2S_TCSR_TE | I2S_TCSR_BCE | I2S_TCSR_FRDE | I2S_TCSR_FR;
    I2S1_RCSR = I2S_RCSR_RE | I2S_RCSR_BCE | I2S_RCSR_FRDE | I2S_RCSR_FR;
    __enable_irq();
}

void setCallback(Callback cb) { g_callback = cb; }

void setMute(bool muted) {
    g_userMuted = muted;
    if (g_mutePin >= 0) digitalWriteFast(g_mutePin, muted ? LOW : HIGH);
}

Stats stats() {
    Stats s;
    __disable_irq();
    s.blocks = g_blocks;
    s.lastCycles = g_lastCycles;
    s.maxCycles = g_maxCycles;
    s.overruns = g_overruns;
    s.fifoErrors = g_fifoErrors;
    s.txLeadFrames = g_txLeadFrames;
    __enable_irq();
    return s;
}

void resetStats() {
    __disable_irq();
    g_maxCycles = 0;
    g_overruns = 0;
    g_fifoErrors = 0;
    __enable_irq();
}

uint32_t blockPeriodCycles() { return g_periodCycles; }

float load(uint32_t cycles) {
    return g_periodCycles ? static_cast<float>(cycles) / static_cast<float>(g_periodCycles) : 0.0f;
}

} // namespace i2s32
