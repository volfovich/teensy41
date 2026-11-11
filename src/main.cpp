// Teensy 4.1 audio DSP starter
//
// Signal flow (stereo):
//   Audio Shield line-in (I2S) ─┐
//                               ├─ mixer ─ AudioEffectBiquadFloat ─┬─ Audio Shield headphone/line-out (I2S)
//   USB audio from computer ────┘                                  └─ USB audio to computer
//
// Serial commands (pio device monitor), one per line:
//   s              print CPU / audio memory usage
//   b              toggle DSP bypass
//   f <hz>         set lowpass cutoff
//   q <q>          set lowpass Q
//   v <0..1>       headphone volume
//   i <line|mic>   select codec input

#include <Arduino.h>
#include <Audio.h>
#include <AudioEffectBiquadFloat.h>

// ---- Audio graph ------------------------------------------------------------
AudioInputI2S          i2sIn;
AudioInputUSB          usbIn;
AudioMixer4            mixL, mixR;
AudioEffectBiquadFloat fxL, fxR;
AudioOutputI2S         i2sOut;
AudioOutputUSB         usbOut;
AudioControlSGTL5000   codec;

AudioConnection c0(i2sIn, 0, mixL, 0);
AudioConnection c1(i2sIn, 1, mixR, 0);
AudioConnection c2(usbIn, 0, mixL, 1);
AudioConnection c3(usbIn, 1, mixR, 1);
AudioConnection c4(mixL, 0, fxL, 0);
AudioConnection c5(mixR, 0, fxR, 0);
AudioConnection c6(fxL, 0, i2sOut, 0);
AudioConnection c7(fxR, 0, i2sOut, 1);
AudioConnection c8(fxL, 0, usbOut, 0);
AudioConnection c9(fxR, 0, usbOut, 1);

// ---- Parameters -------------------------------------------------------------
constexpr float kFs = AUDIO_SAMPLE_RATE_EXACT;
float g_cutoffHz = 1000.0f;
float g_q = 0.7071f;

static void applyFilter() {
    const auto c = dsp::BiquadCoeffs::lowpass(kFs, g_cutoffHz, g_q);
    fxL.setCoeffs(c);
    fxR.setCoeffs(c);
}

static void printStats() {
    Serial.printf("CPU %.1f%% (max %.1f%%)  blocks %u (max %u)  bypass=%d  fc=%.0f Hz  Q=%.3f\n",
                  AudioProcessorUsage(), AudioProcessorUsageMax(),
                  AudioMemoryUsage(), AudioMemoryUsageMax(),
                  fxL.bypass(), (double)g_cutoffHz, (double)g_q);
}

static void handleCommand(char* line) {
    char* arg = line + 1;
    while (*arg == ' ') ++arg;

    switch (line[0]) {
    case 's':
        printStats();
        AudioProcessorUsageMaxReset();
        AudioMemoryUsageMaxReset();
        break;
    case 'b': {
        const bool b = !fxL.bypass();
        fxL.setBypass(b);
        fxR.setBypass(b);
        Serial.printf("bypass=%d\n", b);
        break;
    }
    case 'f': {
        const float hz = atof(arg);
        if (hz > 10.0f && hz < 0.49f * kFs) { g_cutoffHz = hz; applyFilter(); }
        printStats();
        break;
    }
    case 'q': {
        const float q = atof(arg);
        if (q > 0.05f && q < 50.0f) { g_q = q; applyFilter(); }
        printStats();
        break;
    }
    case 'v': {
        const float v = atof(arg);
        codec.volume(constrain(v, 0.0f, 1.0f));
        Serial.printf("volume=%.2f\n", (double)v);
        break;
    }
    case 'i':
        if (strncmp(arg, "mic", 3) == 0) codec.inputSelect(AUDIO_INPUT_MIC);
        else codec.inputSelect(AUDIO_INPUT_LINEIN);
        Serial.printf("input=%s\n", arg);
        break;
    default:
        Serial.println("commands: s | b | f <hz> | q <q> | v <0..1> | i <line|mic>");
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

    // Audio blocks are statically allocated; raise this if AudioMemoryUsageMax() hits it.
    AudioMemory(40);

    codec.enable();
    codec.inputSelect(AUDIO_INPUT_LINEIN);
    codec.volume(0.5f);

    for (int ch = 0; ch < 2; ++ch) {
        mixL.gain(ch, 0.5f);
        mixR.gain(ch, 0.5f);
    }

    applyFilter();

    const uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 1500) {}
    Serial.println("Teensy 4.1 audio DSP ready. Type ? for commands.");
}

void loop() {
    pollSerial();

    static elapsedMillis sinceStats;
    if (sinceStats >= 5000) {
        sinceStats = 0;
        printStats();
    }
}
