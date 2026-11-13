#!/usr/bin/env python3
"""Writes the unit's settings table and current state as the JSON the desktop app's window
receives (a Snapshot, app/src/lib/types.ts). The app uses it as its preview data when it runs
in a plain browser without the unit (app/src/lib/demo-snapshot.json).

    ~/.platformio/penv/bin/python tools/applink_snapshot.py > app/src/lib/demo-snapshot.json

Close the app and the serial monitor first.
"""
import glob, json, struct, sys, time

from applink_check import Link, Reader, string


def main():
    ports = sys.argv[1:] or glob.glob("/dev/cu.usbmodem*")
    if not ports:
        sys.exit("no USB serial port found")
    link = Link(ports[0])
    time.sleep(1)
    link.s.reset_input_buffer()

    f, _ = link.request(0x01, struct.pack("<H", 1) + string("applink_snapshot"))
    r = Reader(f[3])
    info = dict(protocol=r.u16(), firmware=r.str(), blockSize=r.u16(), sampleRate=r.u32(), maxModels=r.u16(),
                maxIrs=r.u16(), maxPath=r.u16(), savedSettings=r.u16(), settings=r.u16(), schemaHash=r.u32(),
                port="preview")
    f, _ = link.request(0x02)
    r = Reader(f[3])
    settings = []
    for i in range(r.u16()):
        e = dict(id=i, key=r.str(), name=r.str(), label=r.str(), min=r.i16(), max=r.i16(), default=r.i16(),
                 step=r.u8(), unit=r.u8(), inPreset=bool(r.u8() & 1))
        e["choices"] = [r.str() for _ in range(r.u16())]
        settings.append(e)
    f, _ = link.request(0x03)
    r = Reader(f[3])
    values = [r.i16() for _ in range(r.u16())]
    files = dict(model=r.str(), ir=r.str(), preset=r.str(), edited=bool(r.u8()), card=bool(r.u8()))
    json.dump(dict(connected=True, detail="", info=info, settings=settings, values=values, files=files), sys.stdout,
              indent=1)
    print()


if __name__ == "__main__":
    main()
