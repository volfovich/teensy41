// Mp3Stream.cpp - see Mp3Stream.h. All of it runs in the background, from flash: the
// firmware's linker script keeps this file's code and tables there (the decoder is
// compiled into this file).
#include <dsp/player/Mp3Stream.h>

#include <string.h>

#include <dsp/Port.h>

#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "vendor/minimp3.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace dsp {

static_assert(sizeof(mp3dec_t) <= Mp3Stream::kDecoderBytes, "Mp3Stream::kDecoderBytes is too small");
static_assert(MINIMP3_MAX_SAMPLES_PER_FRAME <= Mp3Stream::kPcmSamples, "Mp3Stream::kPcmSamples is too small");

bool Mp3Stream::seekBytes(uint32_t offset) {
  if (!src_->seek(offset)) return false;
  fileAt_ = offset;
  inLen_ = inPos_ = 0;
  inEof_ = false;
  return true;
}

// Moves what is unread to the front and tops the buffer up from the file.
void Mp3Stream::refill() {
  if (inPos_) {
    memmove(m_.in, m_.in + inPos_, (size_t)(inLen_ - inPos_));
    fileAt_ += (uint32_t)inPos_;
    inLen_ -= inPos_;
    inPos_ = 0;
  }
  while (!inEof_ && inLen_ < m_.inBytes) {
    const int got = src_->read(m_.in + inLen_, m_.inBytes - inLen_);
    if (got <= 0) inEof_ = true;
    else inLen_ += got;
  }
}

// The next frame: decoded into m_.pcm (decode) or only stepped over. Returns its samples
// per channel (a frame that cannot be decoded: its length, with m_.pcm silent), or 0 at
// the end of the file.
int Mp3Stream::nextFrame(bool decode) {
  mp3dec_t* dec = (mp3dec_t*)m_.decoder;
  for (;;) {
    // The decoder wants to see the start of the following frame too.
    if (!inEof_ && inLen_ - inPos_ < 4096) refill();
    if (inLen_ - inPos_ <= 4) return 0;
    mp3dec_frame_info_t info;
    info.frame_bytes = 0;
    info.frame_offset = -1;
    const int n = mp3dec_decode_frame(dec, m_.in + inPos_, inLen_ - inPos_, decode ? m_.pcm : nullptr, &info);
    const bool found = info.frame_offset >= 0;
    if (found) lastOffset_ = info.frame_offset;
    const int before = inPos_;
    inPos_ += info.frame_bytes;
    if (found) {
      lastOffset_ += before - inPos_;  // frameOffset() adds inPos_ as it is now
      pcmChannels_ = info.channels;
      if (n > 0) return n;
      // A frame that would not decode (its data starts in frames that were not read).
      if (decode) memset(m_.pcm, 0, sizeof(int16_t) * (size_t)kPcmSamples);
      return perFrame_ ? perFrame_ : 1152;
    }
    if (info.frame_bytes == 0) {
      // Nothing found in what is there: more of the file, or the end.
      if (inEof_) return 0;
      if (inPos_ == 0 && inLen_ == m_.inBytes) inPos_ = inLen_ - 4;  // a buffer of junk: drop it
      refill();
    }
  }
}

bool Mp3Stream::open(ByteSource& src, const char** err) {
  src_ = &src;
  total_ = pos_ = 0;
  frameCount_ = 0;
  pcmLen_ = pcmPos_ = 0;
  perFrame_ = 0;
  // ID3v2 tags: "ID3", version, flags, then the size in four 7-bit bytes.
  dataStart_ = 0;
  for (int tags = 0; tags < 4; tags++) {
    uint8_t h[10];
    if (!src.seek(dataStart_) || src.read(h, 10) != 10 || memcmp(h, "ID3", 3)) break;
    const uint32_t size = ((uint32_t)(h[6] & 0x7F) << 21) | ((uint32_t)(h[7] & 0x7F) << 14) |
                          ((uint32_t)(h[8] & 0x7F) << 7) | (uint32_t)(h[9] & 0x7F);
    dataStart_ += 10 + size + ((h[5] & 0x10) ? 10 : 0);
  }
  mp3dec_init((mp3dec_t*)m_.decoder);
  if (!seekBytes(dataStart_)) {
    src_ = nullptr;
    *err = "cannot read the file";
    return false;
  }
  // Every frame's header, for the length and the seek table.
  mp3dec_t* dec = (mp3dec_t*)m_.decoder;
  for (;;) {
    const int n = nextFrame(false);
    if (n <= 0) break;
    const uint32_t hz = (uint32_t)hdr_sample_rate_hz(dec->header);
    if (frameCount_ == 0) {
      rate_ = hz;
      perFrame_ = n;
      channels_ = pcmChannels_;
      kbps_ = (int)hdr_bitrate_kbps(dec->header);
    } else if (hz != rate_ || n != perFrame_) {
      break;  // another format from here on: the track ends here
    }
    if (frameCount_ % kIndexEvery == 0) {
      const uint32_t slot = frameCount_ / kIndexEvery;
      if ((int)slot >= m_.indexCap) {
        src_ = nullptr;
        *err = "too long for the seek table";
        return false;
      }
      m_.index[slot] = frameOffset();
    }
    frameCount_++;
  }
  if (frameCount_ == 0) {
    src_ = nullptr;
    *err = "no MP3 (layer 3) audio found";
    return false;
  }
  total_ = frameCount_ * (uint32_t)perFrame_;
  if (!seek(0)) {
    src_ = nullptr;
    *err = "cannot read the file";
    return false;
  }
  return true;
}

bool Mp3Stream::seek(uint32_t frame) {
  if (!src_) return false;
  if (frame > total_) frame = total_;
  const uint32_t target = frame / (uint32_t)perFrame_;  // the MP3 frame that holds it
  const uint32_t first = target > (uint32_t)kRunIn ? target - (uint32_t)kRunIn : 0;
  const uint32_t slot = first / kIndexEvery;
  mp3dec_init((mp3dec_t*)m_.decoder);
  if (!seekBytes(m_.index[slot])) return false;
  pcmLen_ = pcmPos_ = 0;
  pos_ = frame;
  if (target >= frameCount_) return true;  // the end
  // Step over the frames up to the run-in, decode the run-in for nothing, then the target.
  for (uint32_t f = slot * kIndexEvery; f <= target; f++) {
    const int n = nextFrame(f >= first);
    if (n <= 0) return true;  // the file ended early: read() gives nothing
    if (f == target) {
      pcmLen_ = n;
      pcmPos_ = (int)(frame - target * (uint32_t)perFrame_);
    }
  }
  return true;
}

int Mp3Stream::read(int16_t* dst, int maxFrames) {
  if (!src_ || maxFrames <= 0) return 0;
  const uint32_t left = total_ - pos_;
  const int want = (uint32_t)maxFrames < left ? maxFrames : (int)left;
  int done = 0;
  while (done < want) {
    if (pcmPos_ >= pcmLen_) {
      pcmLen_ = nextFrame(true);
      pcmPos_ = 0;
      if (pcmLen_ <= 0) {
        pcmLen_ = 0;
        break;
      }
    }
    int n = pcmLen_ - pcmPos_;
    if (n > want - done) n = want - done;
    if (pcmChannels_ == 2) {
      memcpy(dst + 2 * done, m_.pcm + 2 * pcmPos_, sizeof(int16_t) * 2 * (size_t)n);
    } else {
      for (int i = 0; i < n; i++) dst[2 * (done + i)] = dst[2 * (done + i) + 1] = m_.pcm[pcmPos_ + i];
    }
    pcmPos_ += n;
    done += n;
  }
  pos_ += (uint32_t)done;
  return done;
}

}  // namespace dsp
