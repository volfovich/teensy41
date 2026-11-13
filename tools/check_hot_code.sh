#!/bin/sh
# Fails if a function the audio interrupt runs was linked into flash instead of ITCM (RAM1).
# Run after `pio run` whenever tools/imxrt1062_t41_cold.ld or the audio path changes:
#   tools/check_hot_code.sh [path/to/firmware.elf]
# Code in ITCM has addresses below 0x20000000; code in flash starts at 0x60000000.
ELF="${1:-.pio/build/teensy41/firmware.elf}"
NM="${NM:-$HOME/.platformio/packages/toolchain-gccarmnoneeabi-teensy/bin/arm-none-eabi-nm}"
[ -f "$ELF" ] || { echo "check_hot_code: $ELF not found (run pio run first)"; exit 2; }

# Names of the per-block and per-sample functions of the audio path.
HOT='audioCallback|AudioChain::process|AudioChain::toOutput|i2s32::.*isr|NamA2<[0-9]+>::(process|taps|ringTaps|layer|head)|AmpStage<[0-9]+>::process|DcBlocker::process|ToneStack::process|NoiseGate<[0-9]+>::(process|processNam|level)|PartitionedConvolver<.*>::(process|convolveSet)|FFT<[0-9]+>::(forwardReal|inverseReal|butterflies)|FxSlot<[0-9]+>::(begin|feed|mix|end)|FxStage<.*>::process|Doubler::process|Modulation::process|Limiter::(process|lookGain)|AudioChain::(toMix|clipOutput)|StudioDelay::process|Reverb80::(process|front|hallBlock|gatedBlock|fadeOut)|ReverbBank::process|DelayBank::process|PlateReverb::process|SpringReverb::(process|spring)|TapeDelay::process|AnalogDelay::process|BankSwitch::(begin|end)|DrumMachine::process|PluckBass::process|Biquad::process|Shelf::process|readHermite|floatToInt24|int24InToFloat|memset$|memcpy$|memmove$'

BAD=$("$NM" -C "$ELF" | awk '$1 ~ /^6/ && $2 ~ /^[TtWw]$/' | grep -E "$HOT")
if [ -n "$BAD" ]; then
  echo "check_hot_code: FAIL, audio-path functions in flash:"
  echo "$BAD"
  exit 1
fi
N=$("$NM" -C "$ELF" | awk '$1 ~ /^0/ && $2 ~ /^[TtWw]$/' | grep -cE "$HOT")
echo "check_hot_code: OK, no audio-path function is in flash ($N matching functions found in ITCM)"
