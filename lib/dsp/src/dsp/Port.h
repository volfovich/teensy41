#pragma once
// Placement hints that only matter on the Teensy 4.x. On the host they expand to nothing.
//
// DSP_COLD puts a function in flash instead of the fast RAM1 (ITCM) that the Teensy copies
// all code to by default. Use it for code that never runs in the audio interrupt (file
// loading, setup), which leaves RAM1 for the audio path, its state and the stack.
// DSP_PROGMEM does the same for constant tables, which would otherwise be copied to DTCM.

#if defined(__IMXRT1062__)
#define DSP_COLD __attribute__((section(".flashmem")))
#define DSP_PROGMEM __attribute__((section(".progmem")))
#else
#define DSP_COLD
#define DSP_PROGMEM
#endif
