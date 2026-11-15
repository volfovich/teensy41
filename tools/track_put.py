#!/usr/bin/env python3
"""Sends a backing track to the unit's SD card, into /TRACKS or a folder below it, over the
app's link protocol (protocol/PROTOCOL.md, FILE_PUT). For until the app does this itself.

  ~/.platformio/penv/bin/python tools/track_put.py <file.wav|mp3> [name on the card]

The name on the card may have folders in front ("Rock/Song.mp3"); the unit makes them. A
name that ends in a slash is a folder, and the file keeps its own name in it ("Rock/").

The unit rescans the card afterwards, so the track is in the list at once (shell: track).
Needs pyserial (PlatformIO's Python has it). Close the app and any serial monitor first.
"""
import glob
import os
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from applink_check import Link, Reader, string  # noqa: E402


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    data = open(sys.argv[1], "rb").read()
    name = sys.argv[2] if len(sys.argv) > 2 else os.path.basename(sys.argv[1])
    if name.endswith("/"):
        name += os.path.basename(sys.argv[1])
    path = "/TRACKS/" + name.lstrip("/")
    ports = glob.glob("/dev/cu.usbmodem*")
    if not ports:
        sys.exit("no USB serial port found")
    link = Link(ports[0])
    time.sleep(1)
    link.s.reset_input_buffer()
    f, _ = link.request(0x01, struct.pack("<H", 2) + string("track_put"))
    if f is None or f[1] != 1:
        time.sleep(5.5)  # an earlier session: let it time out, then once more
        f, _ = link.request(0x01, struct.pack("<H", 2) + string("track_put"))
    if f is None or f[1] != 1:
        sys.exit("the unit does not answer HELLO")

    def fail(f, what):
        if f is None:
            sys.exit("%s: no reply" % what)
        r = Reader(f[3])
        sys.exit("%s: error %d, %s" % (what, r.u16(), r.str()))

    t0 = time.time()
    f, _ = link.request(0x40, string(path) + struct.pack("<II", len(data), zlib.crc32(data)), wait=5)
    if f is None or f[1] != 1:
        fail(f, "FILE_PUT_BEGIN")
    chunk = Reader(f[3]).u16()
    for off in range(0, len(data), chunk):
        f, _ = link.request(0x41, struct.pack("<I", off) + data[off:off + chunk], wait=5)
        if f is None or f[1] != 1:
            fail(f, "FILE_PUT_DATA at %d" % off)
        if off // chunk % 64 == 0:
            print("\r%3d %%" % (100 * off // max(len(data), 1)), end="", flush=True)
    f, _ = link.request(0x42, wait=30)
    if f is None or f[1] != 1:
        fail(f, "FILE_PUT_END")
    secs = time.time() - t0
    print("\r%s: %d KB in %.1f s (%.0f KB/s)" % (path, len(data) // 1024, secs, len(data) / 1024 / max(secs, 0.01)))


if __name__ == "__main__":
    main()
