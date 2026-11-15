// TrackPlayer.cpp - see TrackPlayer.h. All of it runs in the background, from flash (DSP_COLD).
#include <dsp/player/TrackPlayer.h>

#include <string.h>

#include <dsp/Port.h>

namespace dsp {

DSP_COLD void TrackPlayer::setSource(TrackSource* src) {
  src_ = src;
  state_ = src ? kStopped : kEmpty;
  out_.setRun(false);
  loopA_ = loopB_ = 0;
  nextPos_ = 0;
  flushPending_ = true;
  eof_ = false;
  spliceLeft_ = 0;
  pieceCount_ = 0;
}

DSP_COLD void TrackPlayer::setTempo(float rate) {
  if (rate < TimeStretch::kMinRate) rate = TimeStretch::kMinRate;
  if (rate > TimeStretch::kMaxRate) rate = TimeStretch::kMaxRate;
  if (!stretch_ || (rate > 0.995f && rate < 1.005f)) rate = 1.0f;
  if (rate == rate_) return;
  if (src_ && state_ == kPlaying && out_.running() && !flushPending_) {
    // On from 100 ms after what is heard now (or from the queue's end, if that is sooner):
    // the tempo change needs four windows of the track before it gives any sound.
    const uint32_t queued = ring_.written() - ring_.taken();
    uint32_t pos = frameAt(ring_.taken() + (queued < kTempoKeepFrames ? queued : kTempoKeepFrames));
    if (pos > src_->frames()) pos = src_->frames();
    rate_ = rate;
    restartAt(pos, kTempoKeepFrames);
  } else {
    nextPos_ = position();
    rate_ = rate;
    if (src_) flushPending_ = true;
  }
}

DSP_COLD void TrackPlayer::play() {
  if (src_) state_ = kPlaying;  // service() starts the output once enough is queued
}

DSP_COLD void TrackPlayer::pause() {
  if (state_ != kPlaying) return;
  state_ = kPaused;
  out_.setRun(false);
}

DSP_COLD void TrackPlayer::stop() {
  if (!src_) return;
  state_ = kStopped;
  out_.setRun(false);
  nextPos_ = looping() ? loopA_ : 0;
  flushPending_ = true;
}

DSP_COLD void TrackPlayer::seek(uint32_t frame) {
  if (!src_) return;
  if (frame > src_->frames()) frame = src_->frames();
  restartAt(frame);
}

DSP_COLD bool TrackPlayer::setLoop(uint32_t a, uint32_t b) {
  if (!src_ || b > src_->frames() || a >= b || b - a < kMinLoopFrames) return false;
  loopA_ = a;
  loopB_ = b;
  resync();
  return true;
}

DSP_COLD void TrackPlayer::clearLoop() {
  loopA_ = loopB_ = 0;
  resync();
}

// The source continues at `frame`. While the track is heard, what is queued beyond the
// next kKeepFrames is replaced; otherwise the queue is emptied first.
DSP_COLD void TrackPlayer::restartAt(uint32_t frame, uint32_t keep) {
  if (state_ == kPlaying && out_.running() && !flushPending_) {
    ring_.cut(keep);
    const uint32_t at = ring_.written();
    while (pieceCount_ > 0 && (int32_t)(pieces_[pieceCount_ - 1].at - at) >= 0) pieceCount_--;
    src_->seek(frame);
    srcPos_ = frame;
    eof_ = false;
    srcEnded_ = false;
    out_.setEnding(false);
    spliceLeft_ = 0;
    if (stretch_) {
      stretch_->reset();
      stretch_->setRate(rate_);
    }
    notePiece(at, frame);
  } else {
    nextPos_ = frame;
    flushPending_ = true;
  }
}

// After the loop changed: what is queued was read under the old one, so it is read again
// from 50 ms after what is heard now.
DSP_COLD void TrackPlayer::resync() {
  if (!src_ || state_ != kPlaying || !out_.running() || flushPending_) return;
  const uint32_t heard = position();
  if ((int32_t)(ring_.written() - ring_.taken()) <= (int32_t)kKeepFrames) return;
  uint32_t pos = frameAt(ring_.taken() + kKeepFrames);
  if (pos > src_->frames()) pos = src_->frames();
  // Closer to B than those 50 ms: the jump is due within them.
  if (looping() && heard < loopB_ && pos >= loopB_) pos = loopA_;
  restartAt(pos);
}

DSP_COLD void TrackPlayer::notePiece(uint32_t at, uint32_t src) {
  // Pieces the output has left behind are no longer needed.
  while (pieceCount_ >= 2 && (int32_t)(ring_.taken() - pieces_[1].at) >= 0) {
    memmove(pieces_, pieces_ + 1, sizeof(Piece) * (size_t)(pieceCount_ - 1));
    pieceCount_--;
  }
  if (pieceCount_ == kMaxPieces) {
    memmove(pieces_, pieces_ + 1, sizeof(Piece) * (size_t)(pieceCount_ - 1));
    pieceCount_--;
  }
  pieces_[pieceCount_].at = at;
  pieces_[pieceCount_].src = src;
  pieces_[pieceCount_].rate = rate_;
  pieceCount_++;
}

// The source frame at a count of the queue.
DSP_COLD uint32_t TrackPlayer::frameAt(uint32_t count) const {
  if (pieceCount_ == 0) return nextPos_;
  for (int i = pieceCount_ - 1; i >= 0; i--) {
    if ((int32_t)(count - pieces_[i].at) < 0) continue;
    const uint32_t n = count - pieces_[i].at;
    return pieces_[i].src + (pieces_[i].rate == 1.0f ? n : (uint32_t)((double)n * (double)pieces_[i].rate + 0.5));
  }
  return pieces_[0].src;
}

DSP_COLD uint32_t TrackPlayer::position() const {
  if (!src_) return 0;
  if (flushPending_) return nextPos_;
  const uint32_t p = frameAt(ring_.taken());
  return p > src_->frames() ? src_->frames() : p;
}

// Back to A, which will be heard from the queue's count `at`. tail: the source stands at
// B, and what follows there is kept for the crossfade.
DSP_COLD void TrackPlayer::jumpToLoopStart(bool tail, uint32_t at) {
  int t = tail ? src_->read(tail_, kSpliceFrames) : 0;
  if (t < 0) t = 0;
  memset(tail_ + 2 * t, 0, (size_t)(kSpliceFrames - t) * 2 * sizeof(int16_t));
  src_->seek(loopA_);
  srcPos_ = loopA_;
  spliceLeft_ = kSpliceFrames;
  notePiece(at, loopA_);
}

// Up to maxFrames frames of the source into the work buffer, up to the loop's end if that
// comes first, crossfaded after a jump. *atLoopEnd: the source now stands at B. *ended:
// the source gave less than asked for (its end).
DSP_COLD int TrackPlayer::readSource(int maxFrames, bool* atLoopEnd, bool* ended) {
  uint32_t n = (uint32_t)maxFrames;
  const bool loop = looping();
  if (loop && srcPos_ < loopB_ && loopB_ - srcPos_ < n) n = loopB_ - srcPos_;
  int got = src_->read(chunk_, (int)n);
  if (got < 0) got = 0;
  if (spliceLeft_ && got) {
    const int m = got < spliceLeft_ ? got : spliceLeft_;
    int k = kSpliceFrames - spliceLeft_;
    for (int i = 0; i < m; i++, k++) {
      const int32_t wNew = k + 1, wOld = kSpliceFrames - k;
      chunk_[2 * i] = (int16_t)((tail_[2 * k] * wOld + chunk_[2 * i] * wNew) / (kSpliceFrames + 1));
      chunk_[2 * i + 1] = (int16_t)((tail_[2 * k + 1] * wOld + chunk_[2 * i + 1] * wNew) / (kSpliceFrames + 1));
    }
    spliceLeft_ -= m;
  }
  srcPos_ += (uint32_t)got;
  *atLoopEnd = loop && srcPos_ == loopB_;
  *ended = !*atLoopEnd && (uint32_t)got < n;
  return got;
}

// One step of filling the queue; false if there was nothing to do.
DSP_COLD bool TrackPlayer::fill() {
  if (eof_ || ring_.space() < (uint32_t)chunkFrames_) return false;
  bool atLoopEnd = false, ended = false;
  if (!stretching()) {
    const int got = readSource(chunkFrames_, &atLoopEnd, &ended);
    ring_.write(chunk_, (uint32_t)got);
    if (atLoopEnd) {
      jumpToLoopStart(true, ring_.written());
    } else if (ended) {
      if (looping()) {
        jumpToLoopStart(false, ring_.written());  // played on past B (a seek behind it): round from the end
      } else {
        eof_ = true;
        out_.setEnding(true);
      }
    }
    return true;
  }
  // With the stretcher: what it has ready goes to the queue; otherwise it gets more source.
  const int ready = stretch_->pull(chunk_, chunkFrames_);
  if (ready > 0) {
    ring_.write(chunk_, (uint32_t)ready);
    return true;
  }
  if (srcEnded_ || stretch_->room() <= 0) {
    if (stretch_->done() || !srcEnded_) {  // (no room and nothing ready cannot happen)
      eof_ = true;
      out_.setEnding(true);
    }
    return false;
  }
  const int room = stretch_->room();
  const int got = readSource(room < chunkFrames_ ? room : chunkFrames_, &atLoopEnd, &ended);
  stretch_->push(chunk_, got);
  // What was just pushed is heard once the stretcher's backlog has come out.
  const uint32_t heardAt = ring_.written() + (uint32_t)(stretch_->lag() / rate_ + 0.5f);
  if (atLoopEnd) {
    jumpToLoopStart(true, heardAt);
  } else if (ended) {
    if (looping()) {
      jumpToLoopStart(false, heardAt);
    } else {
      stretch_->finish();
      srcEnded_ = true;
    }
  }
  return true;
}

DSP_COLD void TrackPlayer::service(int maxChunks) {
  if (flushPending_) {
    if (!out_.idle()) return;  // still fading out
    ring_.reset();
    pieceCount_ = 0;
    flushPending_ = false;
    eof_ = false;
    srcEnded_ = false;
    spliceLeft_ = 0;
    out_.setEnding(false);
    if (stretch_) {
      stretch_->reset();
      stretch_->setRate(rate_);
    }
    if (src_) {
      src_->seek(nextPos_);
      srcPos_ = nextPos_;
      notePiece(ring_.written(), nextPos_);
    }
  }
  if (!src_) return;
  for (int i = 0; i < maxChunks; i++)
    if (!fill()) break;
  if (state_ != kPlaying) return;
  if (!out_.running()) {
    if (eof_ || ring_.queued() >= kStartFrames) out_.setRun(true);
  } else if (eof_ && ring_.queued() == 0) {
    // The end: stopped, back at the start.
    state_ = kStopped;
    out_.setRun(false);
    nextPos_ = 0;
    flushPending_ = true;
  }
}

}  // namespace dsp
