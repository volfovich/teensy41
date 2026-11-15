// WavStream.cpp - see WavStream.h. All of it runs in the background, from flash (DSP_COLD).
#include <dsp/player/WavStream.h>

#include <math.h>
#include <string.h>

#include <dsp/Port.h>

namespace dsp {

namespace {

DSP_COLD int16_t fromFloat(double x) {
  const double v = x * 32768.0;
  if (v >= 32767.0) return 32767;
  if (v <= -32768.0) return -32768;
  return (int16_t)(v + (v >= 0.0 ? 0.5 : -0.5));  // rounded by hand: no libm routine in RAM1
}

// One sample (little-endian, `bytes` wide) as 16 bits.
DSP_COLD int16_t sample16(const uint8_t* p, int bytes, bool isFloat) {
  if (isFloat) {
    if (bytes == 4) {
      float f;
      memcpy(&f, p, 4);
      return fromFloat(f);
    }
    double d;
    memcpy(&d, p, 8);
    return fromFloat(d);
  }
  if (bytes == 1) return (int16_t)(((int)p[0] - 128) << 8);  // 8-bit WAV is unsigned
  // The two highest bytes.
  return (int16_t)(uint16_t)(p[bytes - 2] | (p[bytes - 1] << 8));
}

}  // namespace

DSP_COLD bool WavStream::open(ByteSource& src, const char** err) {
  src_ = nullptr;
  if (!parseWav(src, info_, err)) return false;
  if (info_.blockAlign > scratchBytes_) {
    *err = "too many channels";
    return false;
  }
  src_ = &src;
  if (!seek(0)) {
    src_ = nullptr;
    *err = "cannot read the data";
    return false;
  }
  return true;
}

DSP_COLD bool WavStream::seek(uint32_t frame) {
  if (!src_) return false;
  if (frame > info_.frames) frame = info_.frames;
  if (!src_->seek(info_.dataOffset + frame * info_.blockAlign)) return false;
  pos_ = frame;
  return true;
}

DSP_COLD int WavStream::read(int16_t* dst, int maxFrames) {
  if (!src_ || maxFrames <= 0) return 0;
  const uint32_t left = info_.frames - pos_;
  int want = (uint32_t)maxFrames < left ? maxFrames : (int)left;
  if (want <= 0) return 0;

  // 16-bit stereo is already what the caller wants (the file and both targets are
  // little-endian).
  if (info_.format == 1 && info_.bitsPerSample == 16 && info_.channels == 2 && info_.blockAlign == 4) {
    const int got = src_->read(dst, want * 4) / 4;
    pos_ += (uint32_t)(got > 0 ? got : 0);
    return got > 0 ? got : 0;
  }

  const int align = info_.blockAlign;
  const int bytes = align / info_.channels;
  const bool isFloat = info_.format == 3;
  const int second = info_.channels >= 2 ? bytes : 0;  // where the right side is in a frame
  int done = 0;
  while (done < want) {
    int n = scratchBytes_ / align;
    if (n > want - done) n = want - done;
    const int got = src_->read(scratch_, n * align) / align;
    for (int i = 0; i < got; i++) {
      const uint8_t* f = scratch_ + i * align;
      dst[2 * (done + i)] = sample16(f, bytes, isFloat);
      dst[2 * (done + i) + 1] = sample16(f + second, bytes, isFloat);
    }
    done += got > 0 ? got : 0;
    if (got < n) break;
  }
  pos_ += (uint32_t)done;
  return done;
}

}  // namespace dsp
