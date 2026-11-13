// Teensy 4.1 guitar DSP platform: main firmware.
//
// Signal flow today (see HLD.md):
//   PCM1808 ADC right ─► input gain ─► NAM A2-Lite amp model ─► output level ─► PCM5102A DAC left + right
//
// Audio runs in the I2S DMA interrupt, one block of DSP_BLOCK frames (48 kHz, 24-bit) at a
// time. loop() only handles the serial shell and USB MIDI.
//
// The amp model is the NeuralAmpModelerCore example A2 model, embedded in flash from
// test/fixtures/nam/A2.nam and read at boot with the same reader that will load SD files.
//
// Serial commands (pio device monitor), one per line:
//   s          print audio statistics (CPU load, overruns, input/output peak levels)
//   a          toggle the amp model (off = clean guitar straight through)
//   g <dB>     input gain trim on top of the model's calibration (default 0)
//   v <dB>     output level trim on top of the loudness normalisation (default 0)
//   b          toggle bypass (bypass = stereo input copied to output bit-for-bit, so the
//              guitar is heard on the right output only)
//   m          toggle output mute

#include <Arduino.h>
#include <I2s32.h>
#include <dsp/ByteSource.h>
#include <dsp/Convert.h>
#include <dsp/Port.h>
#include <dsp/nam/NamA2.h>
#include <dsp/nam/NamFile.h>

// GPIO wired to the PCM5102A XSMT pad (HLD.md section 1, item 1b). -1 = not wired.
static constexpr int kDacMutePin = -1;

// Which ADC input the guitar is wired to. The other input is unused.
static constexpr bool kGuitarOnRightChannel = true;

// Level at the ADC pins that reads 0 dBFS: 1.06 Vrms (datasheet value, not yet measured).
// There is no input buffer or preamp yet, so this is also the level at the guitar jack.
static constexpr float kAdcFullScaleDbu = 2.7f;

// Loudness the output is normalised to when the model file states its loudness
// (the NAM plugin's convention).
static constexpr float kTargetLoudnessDb = -18.0f;

// ---- Embedded model file ---------------------------------------------------------
__asm__(".section .progmem.nammodel,\"a\"\n"
        ".balign 4\n"
        ".global g_modelFileStart\ng_modelFileStart:\n"
        ".incbin \"test/fixtures/nam/A2.nam\"\n"
        ".global g_modelFileEnd\ng_modelFileEnd:\n"
        ".previous\n");
extern "C" const uint8_t g_modelFileStart[], g_modelFileEnd[];

// ---- Amp model -------------------------------------------------------------------
using Nam = dsp::NamA2<i2s32::kBlock>;

static Nam g_nam;                                   // weights: RAM1 (DTCM)
static float g_namHistory[Nam::kHistoryFloats];     // plain global = DTCM; do not move to DMAMEM
static float g_weights[dsp::nama2::kWeightCount];
static dsp::NamInfo g_modelInfo;
static bool g_modelLoaded = false;

// The network needs its receptive field (6,332 samples) of input before its output is right.
static constexpr uint32_t kWarmBlocks = 6400 / i2s32::kBlock;
static constexpr uint32_t kFadeBlocks = 480 / i2s32::kBlock;   // 10 ms fade-in after the warm-up

// ---- Parameters shared with the audio interrupt (single 32-bit writes are atomic) ---
static volatile bool g_ampOn = true;
static volatile bool g_bypass = false;
static volatile bool g_muted = false;
static volatile float g_inputGain = 1.0f;    // linear, applied before the model
static volatile float g_outputGain = 1.0f;   // linear, applied after the model
static volatile float g_inPeak = 0.0f;       // peak of the guitar input since the last stats print
static volatile float g_outPeak = 0.0f;      // peak of the output since the last stats print

static float g_inputTrimDb = 0.0f;           // user trims (loop() only)
static float g_outputTrimDb = 0.0f;
static float g_calibrationDb = 0.0f;         // from the model's input_level_dbu
static float g_normaliseDb = 0.0f;           // from the model's loudness

static inline float dbToGain(float db) { return powf(10.0f, db / 20.0f); }
static inline float gainToDb(float g) { return 20.0f * log10f(g > 1.0e-9f ? g : 1.0e-9f); }

FASTRUN static void audioCallback(const int32_t* rx, int32_t* tx, size_t frames) {
    static uint32_t blocksSinceStart = 0;
    static float inGain = 1.0f, outGain = 1.0f;   // smoothed copies of the targets

    if (g_bypass) {
        memcpy(tx, rx, 2 * frames * sizeof(int32_t));
        return;
    }

    float left[i2s32::kBlock];
    float right[i2s32::kBlock];
    dsp::int24InToFloat(rx, left, right, frames);
    float* const guitar = kGuitarOnRightChannel ? right : left;
    float* const wet = kGuitarOnRightChannel ? left : right;   // the unused channel's buffer

    // Input: peak meter, then gain with a per-sample ramp towards the target (no zipper noise).
    const float inTarget = g_inputGain, outTarget = g_outputGain;
    const float inStep = (inTarget - inGain) / static_cast<float>(frames);
    const float outStep = (outTarget - outGain) / static_cast<float>(frames);
    float inPeak = g_inPeak;
    for (size_t i = 0; i < frames; ++i) {
        const float x = guitar[i];
        inPeak = fmaxf(inPeak, fabsf(x));
        inGain += inStep;
        wet[i] = x * inGain;
    }
    inGain = inTarget;
    g_inPeak = inPeak;

    // The model always runs, so its state stays warm and the amp can be switched without a gap.
    const bool model = g_modelLoaded;
    if (model) g_nam.process(wet, wet);

    float fade = 1.0f;
    if (blocksSinceStart < kWarmBlocks) fade = 0.0f;
    else if (blocksSinceStart < kWarmBlocks + kFadeBlocks)
        fade = static_cast<float>(blocksSinceStart - kWarmBlocks) / static_cast<float>(kFadeBlocks);
    if (blocksSinceStart < kWarmBlocks + kFadeBlocks) ++blocksSinceStart;

    const bool amp = model && g_ampOn;
    const float* const src = amp ? wet : guitar;
    const float level = g_muted ? 0.0f : fade;
    float outPeak = g_outPeak;
    for (size_t i = 0; i < frames; ++i) {
        outGain += outStep;
        const float y = src[i] * (amp ? outGain : 1.0f) * level;
        outPeak = fmaxf(outPeak, fabsf(y));
        wet[i] = y;
    }
    outGain = outTarget;
    g_outPeak = outPeak;

    dsp::floatToInt24Out(wet, wet, tx, frames);   // mono to both DAC channels
}

static void applyGains() {
    g_inputGain = dbToGain(g_calibrationDb + g_inputTrimDb);
    g_outputGain = dbToGain(g_normaliseDb + g_outputTrimDb);
}

DSP_COLD static bool loadEmbeddedModel() {
    const uint32_t size = static_cast<uint32_t>(g_modelFileEnd - g_modelFileStart);
    dsp::MemorySource src(g_modelFileStart, size);
    const char* err = "";
    if (!dsp::readNamA2Lite(src, g_weights, &g_modelInfo, &err)) {
        Serial.printf("model: could not read the embedded .nam file: %s\n", err);
        return false;
    }
    g_nam.begin(g_namHistory);
    g_nam.setWeights(g_weights);

    // Calibration: the model was captured with input_level_dbu reading 0 dBFS; our ADC reads
    // 0 dBFS at kAdcFullScaleDbu. The difference makes the model see the level its amp saw.
    g_calibrationDb = isnan(g_modelInfo.inputLevelDbu) ? 0.0f : kAdcFullScaleDbu - g_modelInfo.inputLevelDbu;
    g_normaliseDb = isnan(g_modelInfo.loudness) ? 0.0f : kTargetLoudnessDb - g_modelInfo.loudness;
    return true;
}

static void printModel() {
    if (!g_modelLoaded) {
        Serial.println("model: none (clean passthrough only)");
        return;
    }
    Serial.printf("model: NAM A2-Lite example, %d weights, %.0f Hz, input level %.2f dBu, loudness %.1f dB\n",
                  dsp::nama2::kWeightCount, static_cast<double>(g_modelInfo.sampleRate),
                  static_cast<double>(g_modelInfo.inputLevelDbu), static_cast<double>(g_modelInfo.loudness));
    Serial.printf("       input gain %+.1f dB (calibration %+.1f, trim %+.1f), output %+.1f dB (normalise %+.1f, trim %+.1f)\n",
                  static_cast<double>(g_calibrationDb + g_inputTrimDb), static_cast<double>(g_calibrationDb),
                  static_cast<double>(g_inputTrimDb), static_cast<double>(g_normaliseDb + g_outputTrimDb),
                  static_cast<double>(g_normaliseDb), static_cast<double>(g_outputTrimDb));
}

static void printStats() {
    const i2s32::Stats s = i2s32::stats();
    noInterrupts();
    const float inPeak = g_inPeak, outPeak = g_outPeak;
    g_inPeak = 0.0f;
    g_outPeak = 0.0f;
    interrupts();
    Serial.printf("load %.1f%% (max %.1f%%)  overruns %lu  fifo errors %lu  in peak %.1f dBFS  out peak %.1f dBFS  amp=%d bypass=%d mute=%d\n",
                  static_cast<double>(100.0f * i2s32::load(s.lastCycles)),
                  static_cast<double>(100.0f * i2s32::load(s.maxCycles)),
                  static_cast<unsigned long>(s.overruns), static_cast<unsigned long>(s.fifoErrors),
                  static_cast<double>(gainToDb(inPeak)), static_cast<double>(gainToDb(outPeak)),
                  g_ampOn && g_modelLoaded, g_bypass, g_muted);
}

static void handleCommand(char* line) {
    char* arg = line + 1;
    while (*arg == ' ') ++arg;

    switch (line[0]) {
    case 's':
        printStats();
        i2s32::resetStats();
        break;
    case 'a':
        g_ampOn = !g_ampOn;
        Serial.printf("amp=%d\n", g_ampOn && g_modelLoaded);
        break;
    case 'g': {
        const float db = static_cast<float>(atof(arg));
        if (db >= -40.0f && db <= 40.0f) { g_inputTrimDb = db; applyGains(); }
        printModel();
        break;
    }
    case 'v': {
        const float db = static_cast<float>(atof(arg));
        if (db >= -60.0f && db <= 24.0f) { g_outputTrimDb = db; applyGains(); }
        printModel();
        break;
    }
    case 'b':
        g_bypass = !g_bypass;
        Serial.printf("bypass=%d\n", g_bypass);
        break;
    case 'm':
        g_muted = !g_muted;
        i2s32::setMute(g_muted);
        Serial.printf("mute=%d\n", g_muted);
        break;
    default:
        Serial.println("commands: s (stats) | a (amp on/off) | g <dB> (input trim) | v <dB> (output trim) | b (bypass) | m (mute)");
        printModel();
        break;
    }
}

static void pollSerial() {
    static char buf[64];
    static size_t len = 0;
    while (Serial.available()) {
        const char ch = Serial.read();
        if (ch == '\r' || ch == '\n') {
            if (len) { buf[len] = '\0'; handleCommand(buf); len = 0; }
        } else if (len < sizeof(buf) - 1) {
            buf[len++] = ch;
        }
    }
}

void setup() {
    Serial.begin(115200);

    // Load the model before audio starts, so the interrupt never sees half-set weights.
    g_modelLoaded = loadEmbeddedModel();
    applyGains();

    i2s32::Config cfg;
    cfg.callback = audioCallback;
    cfg.mutePin = kDacMutePin;
    i2s32::begin(cfg);

    const uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 1500) {}
    Serial.printf("Guitar DSP ready: 48 kHz / 24-bit, block %u frames. Type ? for commands.\n",
                  static_cast<unsigned>(i2s32::kBlock));
    printModel();
}

void loop() {
    pollSerial();
    while (usbMIDI.read()) {}   // drain incoming MIDI; mapping comes with presets

    static elapsedMillis sinceStats;
    if (sinceStats >= 5000) {
        sinceStats = 0;
        printStats();
    }
}
