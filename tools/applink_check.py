#!/usr/bin/env python3
"""On-device check of the desktop app's link protocol (protocol/PROTOCOL.md), milestone A1.

A small independent client: opens the unit's USB serial port, says HELLO, reads the settings
table and the state, sets a value, checks that a change typed into the shell arrives as an
event, and tries bad input. Prints PASS or FAIL per step.

  ~/.platformio/penv/bin/python tools/applink_check.py [port]

It changes the reverb mix for a moment and puts it back. Needs pyserial (PlatformIO's Python
has it). Close any serial monitor first.
"""
import glob, os
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
    def f32(self): return self.take("f")

    def str(self):
        n = self.u16()
        v = self.d[self.i:self.i + n].decode()
        self.i += n
        return v

    def done(self): return self.i == len(self.d)


class Link:
    def __init__(self, port):
        self.s = serial.Serial(port, 115200, timeout=0.05)
        self.buf, self.text, self.seq, self.late = b"", b"", 0, []

    def poll(self, wait):
        """Frames (type, flags, sequence, payload) received within `wait` seconds; text is kept apart."""
        out, t0 = self.late, time.time()
        self.late = []
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
            batch = self.poll(0.05)
            for i, f in enumerate(batch):
                if f[1] & 1 and f[2] == self.seq:
                    self.late = batch[i + 1:]  # frames right behind the reply: for the next poll
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

    f, _ = link.request(0x01, struct.pack("<H", 2) + string("applink_check"))
    r = Reader(f[3])
    hello = dict(protocol=r.u16(), firmware=r.str(), block=r.u16(), rate=r.u32(), models=r.u16(), irs=r.u16(),
                 path=r.u16(), saved=r.u16(), settings=r.u16(), hash=r.u32())
    check(f[1] == 1 and r.done() and hello["protocol"] == 2 and hello["rate"] == 48000, "HELLO: %s" % hello)

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

    # Meter frames: 20 per second for a second, then off.
    f, _ = link.request(0x50, struct.pack("<H", 20))
    meters = [e for e in link.poll(1.0) if e[0] == 0x51 and e[1] == 4]
    ok = f[1] == 1 and Reader(f[3]).u16() == 20 and 15 <= len(meters) <= 22 and all(len(e[3]) == 81 for e in meters)
    m = struct.unpack("<fffIffffffIIB8f", meters[-1][3]) if ok else ()
    ok = ok and all(0 <= p < 4 for p in m[14:]) and 0 <= m[0] <= 1.5 and 0 <= m[2] <= 1 and 1 < m[4] < 100 and m[4] <= m[5] + 5
    check(ok, "SUBSCRIBE 20: %d METERS frames in 1 s; in %.4f out %.4f gate %.2f cpu %.1f%% (max %.1f%%) bits %d"
          % ((len(meters),) + ((m[0], m[1], m[2], m[4], m[5], m[12]) if m else (0, 0, 0, 0, 0, 0))))
    f, _ = link.request(0x50, struct.pack("<H", 0))
    link.poll(0.2)
    check(f[1] == 1 and not [e for e in link.poll(0.5) if e[0] == 0x51], "SUBSCRIBE 0: the frames stop")

    # Presets: list; save the sound as a new one; change it and recall; rename; delete.
    def presets():
        f, _ = link.request(0x20)
        r = Reader(f[3])
        return [(r.str(), r.u16()) for _ in range(r.u16())]

    if files["card"]:
        before, was = presets(), state()[1]["preset"]
        check(True, "PRESET_LIST: %d presets, recalled %r" % (len(before), was))
        f, ev = link.request(0x23, string("applink test"), wait=6)
        made = Reader(f[3]).str() if f and f[1] == 1 else ""
        ev += link.poll(0.3)
        listed = made in [p[0] for p in presets()]
        check(made.endswith("applink test") and listed and any(e[0] == 0x26 for e in ev)
              and state()[1]["preset"] == made, "PRESET_NEW saves it as %r, with a PRESETS_CHANGED event" % made)
        link.request(0x10, struct.pack("<Hi", mix, new))
        link.poll(0.2)
        edited = state()[1]["edited"]
        f, _ = link.request(0x21, string(made), wait=6)
        link.poll(0.3)
        vals2, files2 = state()
        check(edited and f[1] == 1 and vals2[mix] == old and not files2["edited"],
              "an edit sets the edited mark; PRESET_RECALL brings the saved value back")
        f, _ = link.request(0x24, string("applink renamed"), wait=6)
        renamed = Reader(f[3]).str() if f and f[1] == 1 else ""
        check(renamed.endswith("applink renamed") and renamed[:2] == made[:2], "PRESET_RENAME: %r, same number" % renamed)
        f, _ = link.request(0x21, string("no such preset"))
        check(f[1] == 3 and Reader(f[3]).u16() == 3, "PRESET_RECALL of an unknown name: error NOT_FOUND")
        f, _ = link.request(0x25, string(renamed), wait=6)
        link.poll(0.3)
        check(f[1] == 1 and presets() == before, "PRESET_DELETE: the list is as before")
        # Back to the sound from before, unsaved edits included: the preset, then every value.
        if was:
            link.request(0x21, string(was), wait=6)
            link.poll(0.3)
        for i, v in enumerate(state()[0][:hello["saved"]]):
            if v != vals[i]:
                link.request(0x10, struct.pack("<Hi", i, vals[i]))
        link.poll(0.3)
        vals3, files3 = state()
        # The edited mark may have gone: a preset can be marked edited with every value as
        # saved (changed and changed back), and the recall above clears that.
        check(vals3[:hello["saved"]] == vals[:hello["saved"]] and dict(files3, edited=files["edited"]) == files,
              "the sound from before is back (preset %r, edited mark %d)" % (was, files3["edited"]))
        for i in range(hello["saved"]):
            if vals3[i] != vals[i]:
                print("      setting %d: was %d, is %d" % (i, vals[i], vals3[i]))
        if dict(files3, edited=files["edited"]) != files:
            print("      was %r, is %r" % (files, files3))
        files = files3
    else:
        print("SKIP  presets: no SD card")

    # Models, IRs and files: lists; send an IR and a model, load the model, delete both.
    def file_list(t):
        f, _ = link.request(t, wait=5)
        r = Reader(f[3])
        return [(r.str(), r.u32(), r.u32()) for _ in range(r.u16())]

    def put(path, data, crc=None, wait=20):
        """Uploads data; returns the reply to FILE_PUT_END (or the first error reply) and the seconds it took."""
        t0 = time.time()
        f, _ = link.request(0x40, string(path) + struct.pack("<II", len(data), zlib.crc32(data) if crc is None else crc))
        if f[1] != 1:
            return f, 0
        chunk = Reader(f[3]).u16()
        for off in range(0, len(data), chunk):
            f, _ = link.request(0x41, struct.pack("<I", off) + data[off:off + chunk], wait=5)
            if f is None or f[1] != 1:
                return f, 0
        f, _ = link.request(0x42, wait=wait)
        return f, time.time() - t0

    if files["card"]:
        models, irs = file_list(0x30), file_list(0x38)
        check(files["model"] in [m[0] for m in models] and files["ir"] in [i[0] for i in irs] and all(m[1] > 0 for m in models),
              "MODEL_LIST: %d models, IR_LIST: %d IRs, the loaded ones among them" % (len(models), len(irs)))
        # A 0.1 s mono 16-bit WAV: a click and silence.
        pcm = struct.pack("<h", 20000) + bytes(2 * 4799)
        wav = b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, 48000, 96000, 2, 16) \
            + b"data" + struct.pack("<I", len(pcm)) + pcm
        f, _ = put("/IR/applink test.wav", wav, crc=1)
        check(f[1] == 3 and Reader(f[3]).u16() == 8 and "/IR/applink test.wav" not in [i[0] for i in file_list(0x38)],
              "an upload with a wrong CRC: error CHECKSUM, nothing saved")
        f, secs = put("/IR/applink test.wav", wav)
        check(f[1] == 1 and ("/IR/applink test.wav", len(wav), 0) in file_list(0x38) and state()[1] == files,
              "FILE_PUT: a %d-byte IR arrives and is listed; the loaded files stay marked" % len(wav))
        for bad in ("/SECRET/x.wav", "/AMP/../x.nam", "/AMP/.hidden.nam", "/x.txt", "/AMP/sub/x.nam"):
            f, _ = put(bad, b"1234")
            if not (f[1] == 3 and Reader(f[3]).u16() == 5):
                check(False, "FILE_PUT to %s must be refused" % bad)
        check(True, "FILE_PUT outside the allowed folders: error BAD_PATH")
        nam = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../test/fixtures/nam/A2.nam"), "rb").read()
        f, secs = put("/AMP/applink test.nam", nam)
        check(f[1] == 1 and ("/AMP/applink test.nam", len(nam), 0) in file_list(0x30),
              "FILE_PUT: a %d KB model in %.1f s (%.0f KB/s)" % (len(nam) // 1024, secs, len(nam) / 1024 / max(secs, 0.01)))
        f, _ = link.request(0x31, string("/AMP/applink test.nam"), wait=8)
        link.poll(0.3)
        check(f[1] == 1 and state()[1]["model"] == "/AMP/applink test.nam", "MODEL_LOAD loads the model just sent")
        f, _ = link.request(0x31, string(files["model"]), wait=8)
        f, _ = link.request(0x39, string(files["ir"]), wait=8)
        link.poll(0.3)
        check(f[1] == 1 and state()[1] == files, "MODEL_LOAD and IR_LOAD of the files from before: the state is as it was")
        for path in ("/AMP/applink test.nam", "/IR/applink test.wav"):
            f, _ = link.request(0x45, string(path), wait=8)
        check(f[1] == 1 and file_list(0x30) == models and file_list(0x38) == irs and state()[1] == files,
              "FILE_DELETE of both: the lists and the state are as before")
        f, _ = link.request(0x45, string("/AMP/applink test.nam"))
        check(f[1] == 3 and Reader(f[3]).u16() == 3, "FILE_DELETE of a missing file: error NOT_FOUND")
        # Reading back: the folder's list, and a file as FILE_DATA replies.
        f, _ = link.request(0x47, string("/AMP"), wait=5)
        r = Reader(f[3])
        listed = []
        for _ in range(r.u16()):
            name, _size, _time, is_folder = r.str(), r.u32(), r.u32(), r.u8()
            if not is_folder:
                listed.append("/AMP/" + name)
        listed.sort()
        check(f[1] == 1 and listed == sorted(m[0] for m in models), "DIR_LIST /AMP: the same %d files as MODEL_LIST" % len(listed))
        # Backing tracks may lie in folders below /TRACKS; the other folders have none.
        f, _ = link.request(0x47, string("/TRACKS"), wait=5)
        r = Reader(f[3])
        folders = []
        for _ in range(r.u16()):
            name, _size, _time, is_folder = r.str(), r.u32(), r.u32(), r.u8()
            if is_folder:
                folders.append(name)
        if folders:
            sub = "/TRACKS/" + sorted(folders)[0]
            f, _ = put(sub + "/applink test.wav", wav)
            ok = f[1] == 1
            f, _ = link.request(0x47, string(sub), wait=5)
            r = Reader(f[3])
            names = []
            for _ in range(r.u16()):
                names.append(r.str())
                r.u32(), r.u32(), r.u8()
            ok = ok and f[1] == 1 and "applink test.wav" in names
            f, _ = link.request(0x45, string(sub + "/applink test.wav"), wait=8)
            check(ok and f[1] == 1, "FILE_PUT, DIR_LIST and FILE_DELETE in a folder below /TRACKS (%s)" % sub)
        else:
            print("  (no folder below /TRACKS on the card: files in folders not checked)")
        bad = 0
        for path in ("/TRACKS/.hidden/x.wav", "/TRACKS//x.wav", "/TRACKS/a/../x.wav", "/IR/sub/x.wav"):
            f, _ = link.request(0x40, string(path) + struct.pack("<II", 4, 0), wait=5)
            bad += f[1] == 3 and Reader(f[3]).u16() == 5
        f, _ = link.request(0x47, string("/IR/sub"), wait=5)
        bad += f[1] == 3 and Reader(f[3]).u16() == 5
        check(bad == 5, "hidden, empty and '..' folders below /TRACKS and folders below /IR: error BAD_PATH")
        # The backing track's own messages (PROTOCOL.md 4.8), with renaming and folders below
        # /TRACKS (4.5), in a folder of their own that is removed again. The track is not played.
        def track(f):
            r = Reader(f[3])
            s = dict(path=r.str(), playing=r.u8(), place=r.f32(), length=r.f32(), tempo=r.u16(), bpm=r.f32(), sure=r.u8(),
                     scan=r.i16(), first=r.f32(), beats=r.u8(), snap=r.u8(), loop=r.u8(), a=r.f32(), b=r.f32(), gaps=r.u32(),
                     kind=r.u8(), rate=r.u32(), channels=r.u8(), kbps=r.u16(), tracks=r.u16(), reading=r.u8(), number=r.u32())
            s["whole"] = r.done()
            return s

        def command(n, a=0.0, b=0.0, c=0.0):
            f, _ = link.request(0x73, struct.pack("<Bfff", n, a, b, c), wait=3)
            return f

        def list_read():
            t0 = time.time()
            while time.time() - t0 < 30:
                f, _ = link.request(0x74)
                if f and f[1] == 1 and not track(f)["reading"]:
                    return track(f)
                time.sleep(0.2)
            return None

        def rows(folder):
            f, _ = link.request(0x70, string(folder) + struct.pack("<H", 0), wait=3)
            if f[1] != 1:
                return Reader(f[3]).u16()  # the error's code
            r = Reader(f[3])
            total, first = r.u16(), r.u16()
            got = [(r.str(), r.u8(), r.u16()) for _ in range(r.u16())]
            return got if r.done() and first == 0 and total == len(got) else None

        f, _ = link.request(0x74)
        was = track(f)
        check(f[1] == 1 and was["whole"], "TRACK_STATE: %s" % {k: was[k] for k in ("path", "playing", "tracks", "reading", "number")})
        if was["playing"] == 2:
            print("SKIP  backing track: one is playing, and these checks would stop it")
        else:
            home, song = "/TRACKS/applink check", "applink check/sub/tone.wav"
            quiet = b"RIFF" + struct.pack("<I", 36 + 192000) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, 48000, 96000, 2, 16) \
                + b"data" + struct.pack("<I", 192000) + bytes(192000)  # 2 s of silence
            f, _ = link.request(0x4A, string(home + "/sub"), wait=5)
            ok = f[1] == 1
            f, _ = put("/TRACKS/" + song, quiet)
            now = list_read()
            check(ok and f[1] == 1 and now and now["number"] > was["number"] and now["tracks"] == was["tracks"] + 1,
                  "FOLDER_NEW and a track sent into it: the list is read anew and has one track more")
            check(rows("applink check") == [("sub", 1, 1)] and rows("applink check/sub") == [("tone.wav", 0, 0)] and rows("no such") == 3,
                  "TRACK_LIST: the folder, the track in it, NOT_FOUND for a folder that is not there")
            f, _ = link.request(0x71, string("applink tone") + struct.pack("<H", 0), wait=3)
            r = Reader(f[3])
            check(f[1] == 1 and (r.u16(), r.u8(), r.u16(), r.u16()) == (1, 0, 0, 1) and r.str() == song and r.done(),
                  "TRACK_FIND 'applink tone': the one track, by its path")
            f, _ = link.request(0x72, string(song), wait=6)
            s = track(f)
            check(f[1] == 1 and s["whole"] and (s["path"], s["playing"], s["kind"], s["rate"], s["channels"], s["loop"]) == (song, 1, 1, 48000, 1, 0)
                  and abs(s["length"] - 2.0) < 0.01, "TRACK_OPEN: stopped, 2.0 s, WAV, 48000 Hz, mono")
            f, _ = link.request(0x72, string("applink check/none.wav"), wait=6)
            check(f[1] == 3 and Reader(f[3]).u16() == 3, "TRACK_OPEN of a track that is not there: error NOT_FOUND")
            command(10, 0.0)  # loop points stay where they are set
            s = track(command(4, 1.0))
            ok = abs(s["place"] - 1.0) < 0.01
            s = track(command(14, 0.5, 1.5))
            ok = ok and s["loop"] == 2 and abs(s["a"] - 0.5) < 0.01 and abs(s["b"] - 1.5) < 0.01
            ok = ok and track(command(15))["loop"] == 1 and track(command(16))["loop"] == 2 and track(command(17))["loop"] == 0
            s = track(command(12))
            ok = ok and s["loop"] == 3 and abs(s["a"] - 1.0) < 0.01
            command(17)
            ok = ok and track(command(5, 80.0))["tempo"] == 80 and track(command(5, 100.0))["tempo"] == 100
            check(ok, "TRACK_COMMAND: seek, loop a section, off, on, forgotten, a marked start, tempo 80 and 100")
            bad = 0
            for f in (command(99), command(5, 200.0), command(14, 1.0, 1.1), command(16)):
                bad += f[1] == 3 and Reader(f[3]).u16() == 2
            check(bad == 4, "TRACK_COMMAND: an unknown command, tempo 200, a loop of 0.1 s, loop on without points: error BAD_PAYLOAD")
            link.poll(0.6)
            link.s.write(b"track seek 0.5\n")
            events = [track(e) for e in link.poll(1.0) if e[0] == 0x75 and e[1] == 4]
            check(any(abs(e["place"] - 0.5) < 0.01 for e in events), "'track seek 0.5' typed into the shell arrives as a TRACK_CHANGED event")
            f, _ = link.request(0x46, string("/TRACKS/" + song) + string(home + "/moved/tone 2.wav"), wait=6)
            f2, _ = link.request(0x74)
            now = list_read()
            check(f[1] == 1 and track(f2)["path"] == "" and rows("applink check/moved") == [("tone 2.wav", 0, 0)] and rows("applink check/sub") == []
                  and now["tracks"] == was["tracks"] + 1, "FILE_RENAME moves the open track into a new folder: it is closed, the list follows")
            bad = 0
            for a, b in (("/IR/x.wav", "/IR/y.wav"), (home, home + "/sub/in itself"), ("/TRACKS/applink check/moved/tone 2.wav", "/IR/tone.wav")):
                f, _ = link.request(0x46, string(a) + string(b), wait=6)
                bad += f[1] == 3 and Reader(f[3]).u16() == 5
            f, _ = link.request(0x46, string(home + "/nothing.wav") + string(home + "/x.wav"), wait=6)
            bad += f[1] == 3 and Reader(f[3]).u16() == 3
            f, _ = link.request(0x4B, string("/TRACKS"), wait=15)
            bad += f[1] == 3 and Reader(f[3]).u16() == 5
            check(bad == 5, "FILE_RENAME outside /TRACKS, into itself, of nothing; FOLDER_DELETE of /TRACKS: refused")
            f, _ = command(1), None
            check(f[1] == 3 and Reader(f[3]).u16() == 3, "TRACK_COMMAND with no track open: error NOT_FOUND")
            f, _ = link.request(0x4B, string(home), wait=15)
            removed = Reader(f[3]).u16() if f[1] == 1 else -1
            now = list_read()
            check(removed == 1 and rows("applink check") == 3 and now["tracks"] == was["tracks"],
                  "FOLDER_DELETE: the folder is gone with the track in it, the list is as it was")
            if was["path"]:
                f, _ = link.request(0x72, string(was["path"]), wait=6)
                check(f[1] == 1 and track(f)["path"] == was["path"], "the track that was open before is open again")
        f, _ = put("/IR/applink test.wav", wav)
        link.seq += 1
        link.s.write(frame(0x43, 0, link.seq, string("/IR/applink test.wav")))
        got, t0 = b"", time.time()
        while len(got) < len(wav) and time.time() - t0 < 5:
            for t, fl, seq, payload in link.poll(0.05):
                if t == 0x44 and fl == 1 and seq == link.seq and struct.unpack_from("<II", payload) == (len(got), len(wav)):
                    got += payload[8:]
        check(got == wav, "FILE_GET returns the %d bytes that were sent" % len(wav))
        link.request(0x45, string("/IR/applink test.wav"), wait=8)
        f, _ = link.request(0x43, string("/AMP/../x"))
        check(f[1] == 3 and Reader(f[3]).u16() == 5, "FILE_GET outside the allowed folders: error BAD_PATH")
        f, _ = link.request(0x49, wait=8)
        r = Reader(f[3])
        check((r.u16(), r.u16(), r.u16()) == (len(irs), len(models), len(before)) and state()[1] == files, "RESCAN: the same counts")
    else:
        print("SKIP  files: no SD card")

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

    # The loaded IR's samples (for the app's pictures of it) and the card's space.
    f, _ = link.request(0x3A)
    if f and f[1] == 1:
        r = Reader(f[3])
        r.u32(), r.u16(), r.u16(), r.u32()
        length = r.u32()
        f, _ = link.request(0x3B, wait=2.0)
        r = Reader(f[3]) if f and f[1] == 1 else None
        n = r.u16() if r else -1
        peak = r.take("f") if r else 0.0
        samples = [r.i16() for _ in range(n)] if r else []
        check(r is not None and n == length and r.done() and peak > 0 and max(abs(v) for v in samples) == 32767,
              "IR_DATA: %d samples (IR_INFO says %d in use), peak %.3f" % (n, length, peak))
    else:
        f, _ = link.request(0x3B)
        check(f and f[1] == 3 and Reader(f[3]).u16() == 3, "IR_DATA with no IR loaded: error NOT_FOUND")
    t0 = time.time()
    f, _ = link.request(0x48, wait=8.0)
    took = time.time() - t0
    r = Reader(f[3]) if f and f[1] == 1 else None
    present, total, free = (r.u8(), r.u32(), r.u32()) if r else (0, 0, 0)
    check(r is not None and r.done() and (not present or 0 < free <= total),
          "CARD_INFO: card %s, %.1f GB, %.1f GB free, answered in %.2f s" % ("in" if present else "out", total / 1048576, free / 1048576, took))

    # The tuner: opening it shows in the METERS activity bits; a tuner the app opened is closed
    # when the session ends (or the guitar would stay muted).
    def tuner_bit():
        link.request(0x50, struct.pack("<H", 20))
        m = [e for e in link.poll(0.4) if e[0] == 0x51]
        link.request(0x50, struct.pack("<H", 0))
        link.poll(0.2)
        return bool(m and m[-1][3][48] & 32)  # the activity bits

    names = {e["key"]: e["choices"] for e in schema}
    check(len(names["drum style"]) == schema[[e["key"] for e in schema].index("drum style")]["hi"] + 1
          and names["drum song"][0] == "off" and all(names["drum kit"]),
          "the schema names the drum styles %s..., kits %s and songs" % (names["drum style"][:3], names["drum kit"]))
    was_open = tuner_bit()
    f, _ = link.request(0x52, b"\x01")
    check(f[1] == 1 and not was_open and tuner_bit(), "TUNER_OPEN: the tuner shows as open in METERS")

    time.sleep(5.5)
    link.poll(0.3)
    f, _ = link.request(0x03)
    check(f is not None and f[1] == 3, "after 5 s of silence the session has ended")
    link.request(0x01, struct.pack("<H", 2) + string("applink_check"))
    check(not tuner_bit(), "the tuner the app left open was closed when the session ended")

    print("%d failed" % failures if failures else "all passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
