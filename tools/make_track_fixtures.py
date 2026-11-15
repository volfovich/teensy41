#!/usr/bin/env python3
"""Makes the MP3 files the host tests of the backing-track player read
(test/fixtures/player/). They are checked in; run this only to change them.

    python3 -m venv /tmp/venv && /tmp/venv/bin/pip install lameenc
    /tmp/venv/bin/python tools/make_track_fixtures.py

Each file is a few seconds of two tones, the left at 440 Hz and the right at 1 kHz, that
grow louder over the file (so every place in it sounds different).
"""
import math
import os
import struct

import lameenc

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "test", "fixtures", "player")


def tones(rate, channels, seconds):
    n = int(rate * seconds)
    out = bytearray()
    for i in range(n):
        amp = 0.15 + 0.5 * i / n
        left = amp * math.sin(2 * math.pi * 440 * i / rate)
        right = amp * 0.75 * math.sin(2 * math.pi * 1000 * i / rate)
        out += struct.pack("<h", int(round(left * 32767)))
        if channels == 2:
            out += struct.pack("<h", int(round(right * 32767)))
    return bytes(out)


def mp3(name, rate, channels, seconds, kbps=None, vbr=None):
    e = lameenc.Encoder()
    e.set_in_sample_rate(rate)
    e.set_out_sample_rate(rate)
    e.set_channels(channels)
    e.set_quality(2)
    if vbr is not None:
        e.set_vbr(4)  # vbr_mtrh, LAME's -V
        e.set_vbr_quality(vbr)
    else:
        e.set_bit_rate(kbps)
    e.silence()
    data = bytes(e.encode(tones(rate, channels, seconds))) + bytes(e.flush())
    open(os.path.join(OUT, name), "wb").write(data)
    print("%-24s %6d bytes" % (name, len(data)))


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    mp3("tone_44k_cbr128.mp3", 44100, 2, 3.0, kbps=128)
    mp3("tone_44k_vbr.mp3", 44100, 2, 3.0, vbr=4)
    mp3("tone_22k_mono_32.mp3", 22050, 1, 3.0, kbps=32)
    mp3("tone_48k_cbr96.mp3", 48000, 2, 2.0, kbps=96)
