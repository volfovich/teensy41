#pragma once
// Template for a custom Teensy Audio Library block that does its DSP in float.
//
// update() runs in the audio software interrupt every AUDIO_BLOCK_SAMPLES samples
// (128 @ 44.1 kHz = ~2.9 ms). Keep it bounded: no allocation, no Serial, no blocking.
// Parameter changes from loop() are made atomic with AudioNoInterrupts()/AudioInterrupts().

#include <Arduino.h>
#include <Audio.h>        // AudioStream, AudioNoInterrupts()/AudioInterrupts()
#include <arm_math.h>
#include <dsp/Biquad.h>

class AudioEffectBiquadFloat : public AudioStream {
public:
    AudioEffectBiquadFloat() : AudioStream(1, inputQueueArray_) {}

    void setCoeffs(const dsp::BiquadCoeffs& c) {
        AudioNoInterrupts();
        filter_.setCoeffs(c);
        AudioInterrupts();
    }

    void setBypass(bool b) { bypass_ = b; }
    bool bypass() const { return bypass_; }

    void update() override {
        audio_block_t* block = receiveWritable(0);
        if (!block) return;

        if (!bypass_) {
            float buf[AUDIO_BLOCK_SAMPLES];
            arm_q15_to_float(block->data, buf, AUDIO_BLOCK_SAMPLES);   // int16 -> [-1, 1)
            filter_.process(buf, buf, AUDIO_BLOCK_SAMPLES);
            arm_float_to_q15(buf, block->data, AUDIO_BLOCK_SAMPLES);   // saturating
        }

        transmit(block);
        release(block);
    }

private:
    audio_block_t* inputQueueArray_[1];
    dsp::Biquad filter_;
    volatile bool bypass_ = false;
};
