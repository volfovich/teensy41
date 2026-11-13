#!/usr/bin/env python3
"""On-device check of the desktop app's link protocol (protocol/PROTOCOL.md), milestone A1.

A small independent client: opens the unit's USB serial port, says HELLO, reads the settings
table and the state, sets a value, checks that a change typed into the shell arrives as an
event, and tries bad input. Prints PASS or FAIL per step.

  ~/.platformio/penv/bin/python tools/applink_check.py [port]

It changes the reverb mix for a moment and puts it back. Needs pyserial (PlatformIO's Python
has it). Close any serial monitor first.
"""
import glob
import struct
import sys
import time
import zlib

import serial

MAGIC = b"\xf1\xd5"
failures = 0


def frame(t, flags, seq, payload=b""):
    body = struct.pack("<BBHI", t, flags, seq, len(payload)) + payload
    return MAGIC + body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


def string(x):
    b = x.encode()
    return struct.pack("<H", len(b)) + b


class Reader:
    def __init__(self, d):
        self.d, self.i = d, 0

    def take(self, fmt):
        v = struct.unpack_from("<" + fmt, self.d, self.i)[0]
        self.i += struct.calcsize(fmt)
        return v

    def u8(self): return self.take("B")
    def u16(self): return self.take("H")
    def i16(self): return self.take("h")
    def u32(self): return self.take("I")
    def i32(self): return self.take("i")

    def str(self):
        n = self.u16()
        v = self.d[self.i:self.i + n].decode()
        self.i += n
        return v

    def done(self): return self.i == len(self.d)


class Link:
    def __init__(self, port):
        self.s = serial.Serial(port, 115200, timeout=0.05)
        self.buf, self.text, self.seq = b"", b"", 0

    def poll(self, wait):
        """Frames (type, flags, sequence, payload) received within `wait` seconds; text is kept apart."""
        out, t0 = [], time.time()
        while time.time() - t0 < wait:
            self.buf += self.s.read(65536)
            while True:
                i = self.buf.find(MAGIC)
                if i < 0:
                    self.text += self.buf
                    self.buf = b""
                    break
                self.text += self.buf[:i]
                self.buf = self.buf[i:]
                if len(self.buf) < 10:
                    break
                t, fl, seq, ln = struct.unpack("<BBHI", self.buf[2:10])
                if len(self.buf) < 14 + ln:
                    break
                body = self.buf[2:10 + ln]
                crc = struct.unpack("<I", self.buf[10 + ln:14 + ln])[0]
                assert crc == zlib.crc32(body) & 0xFFFFFFFF, "bad CRC from the device"
                out.append((t, fl, seq, self.buf[10:10 + ln]))
                self.buf = self.buf[14 + ln:]
        return out

    def request(self, t, payload=b"", wait=1.0):
        """Returns (reply or None, events seen while waiting)."""
        self.seq += 1
        self.s.write(frame(t, 0, self.seq, payload))
        t0, events = time.time(), []
        while time.time() - t0 < wait:
            for f in self.poll(0.05):
                if f[1] & 1 and f[2] == self.seq:
                    return f, events
                events.append(f)
        return None, events


def check(ok, what):
    global failures
    failures += 0 if ok else 1
    print(("PASS  " if ok else "FAIL  ") + what)


def main():
    ports = sys.argv[1:] or glob.glob("/dev/cu.usbmodem*")
    if not ports:
        sys.exit("no USB serial port found")
    link = Link(ports[0])
    time.sleep(1)
    link.s.reset_input_buffer()
    time.sleep(5.5)  # let an earlier session time out

    f, _ = link.request(0x03)
    check(f and f[1] == 3 and Reader(f[3]).u16() == 2, "before HELLO a request is refused")

    f, _ = link.request(0x01, struct.pack("<H", 1) + string("applink_check"))
    r = Reader(f[3])
    hello = dict(protocol=r.u16(), firmware=r.str(), block=r.u16(), rate=r.u32(), models=r.u16(), irs=r.u16(),
                 path=r.u16(), saved=r.u16(), settings=r.u16(), hash=r.u32())
    check(f[1] == 1 and r.done() and hello["protocol"] == 1 and hello["rate"] == 48000, "HELLO: %s" % hello)

    f, _ = link.request(0x02)
    r = Reader(f[3])
    schema = []
    for _ in range(r.u16()):
        e = dict(key=r.str(), name=r.str(), label=r.str(), lo=r.i16(), hi=r.i16(), default=r.i16(), step=r.u8(),
                 unit=r.u8(), flags=r.u8())
        e["choices"] = [r.str() for _ in range(r.u16())]
        schema.append(e)
    check(r.done() and len(schema) == hello["settings"] and zlib.crc32(f[3]) & 0xFFFFFFFF == hello["hash"],
          "GET_SCHEMA: %d settings in %d bytes, hash matches HELLO" % (len(schema), len(f[3])))

    def state():
        f, _ = link.request(0x03)
        r = Reader(f[3])
        vals = [r.i16() for _ in range(r.u16())]
        files = dict(model=r.str(), ir=r.str(), preset=r.str(), edited=r.u8(), card=r.u8())
        assert r.done()
        return vals, files

    vals, files = state()
    check(len(vals) == hello["settings"], "GET_STATE: %d values, %s" % (len(vals), files))

    mix = next(i for i, e in enumerate(schema) if e["key"] == "mix")
    old = vals[mix]
    new = old + 7 if old < 90 else old - 7
    f, ev = link.request(0x10, struct.pack("<Hi", mix, new))
    r = Reader(f[3])
    check(f[1] == 1 and (r.u16(), r.i32()) == (mix, new) and not ev,
          "SET_PARAM reverb mix %d -> %d: confirmed in the reply, no event for the app's own change" % (old, new))
    f, _ = link.request(0x10, struct.pack("<Hi", mix, 5000))
    r = Reader(f[3])
    r.u16()
    check(r.i32() == schema[mix]["hi"], "SET_PARAM out of range is clamped to %d" % schema[mix]["hi"])
    check(state()[0][mix] == schema[mix]["hi"], "GET_STATE shows the new value")

    link.s.write(("mix %d\n" % old).encode())
    events, t0 = [], time.time()
    while time.time() - t0 < 1.0:
        events += link.poll(0.05)
    changed = [(Reader(e[3]).u16(), struct.unpack_from("<i", e[3], 2)[0]) for e in events if e[0] == 0x11 and e[1] == 4]
    check((mix, old) in changed, "'mix %d' typed into the shell arrives as a PARAM_CHANGED event" % old)

    f, _ = link.request(0x7E)
    check(f[1] == 3 and Reader(f[3]).u16() == 1, "unknown message: error UNKNOWN_MESSAGE")
    f, _ = link.request(0x10, b"\x01")
    check(f[1] == 3 and Reader(f[3]).u16() == 2, "short SET_PARAM: error BAD_PAYLOAD")
    f, _ = link.request(0x10, struct.pack("<Hi", 999, 1))
    check(f[1] == 3 and Reader(f[3]).u16() == 3, "SET_PARAM for setting 999: error NOT_FOUND")

    drums = next(i for i, e in enumerate(schema) if e["name"] == "Drums")
    bad = bytearray(frame(0x03, 0, 77))
    bad[-1] ^= 0xFF
    link.s.write(bytes(bad) + b"drum on\n")
    time.sleep(0.4)
    link.poll(0.2)
    check(state()[0][drums] == 0, "a damaged frame with 'drum on' behind it is discarded whole; the link works after a pause")

    link.text = b""
    link.s.write(b"s\n")
    t0 = time.time()
    while time.time() - t0 < 2.5:
        link.poll(0.1)
    check(b"CPU:" in link.text and b"Driver:" in link.text, "the typed shell still answers in text while the app is connected")

    time.sleep(5.5)
    link.poll(0.3)
    f, _ = link.request(0x03)
    check(f is not None and f[1] == 3, "after 5 s of silence the session has ended")

    print("%d failed" % failures if failures else "all passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
