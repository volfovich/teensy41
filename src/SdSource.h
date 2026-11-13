// SdSource.h - an SD card file as an dsp::ByteSource, for the loaders
// (IRLoader.h, NamFile.h).
// Ported from teensy-ir-loader (src/SdSource.h) for the PCM1808 / PCM5102A hardware and the i2s32 driver.
#pragma once

#include <SD.h>

#include <dsp/conv/IRLoader.h>

class SdSource : public dsp::ByteSource {
 public:
  explicit SdSource(File& f) : f_(f) {}
  int read(void* dst, int n) override { return f_.read(dst, n); }
  bool seek(uint32_t pos) override { return f_.seek(pos); }

 private:
  File& f_;
};
