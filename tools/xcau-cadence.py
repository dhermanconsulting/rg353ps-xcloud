#!/usr/bin/env python3
"""Per-second arrival cadence of the video access units in an .xcau recording.

Runs on the device (Python 3.9 is present there) so the 50 MB file never has
to come back over the slow link:

    python3 xcau-cadence.py /userdata/rec-wreckfest.xcau

Prints one line per second: units, gaps over 25 ms, gaps over 50 ms, the
largest gap, and the largest unit in KB. A steady 60 fps source shows 60
units and no long gaps; 30 fps content shows 30 units with ~33 ms gaps.
"""
import struct
import sys


def main(path):
    f = open(path, "rb")
    if f.read(8)[:4] != b"XCAU":
        sys.exit("not a recording")
    last = None
    t0 = None
    rows = {}
    audio = {}
    while True:
        h = f.read(20)
        if len(h) < 20:
            break
        kind = h[0]
        ts, = struct.unpack("<Q", h[8:16])
        size, = struct.unpack("<I", h[16:20])
        f.seek(size, 1)
        if t0 is None:
            t0 = ts
        sec = (ts - t0) // 1000
        if kind == 1:
            audio[sec] = audio.get(sec, 0) + 1
            continue
        if kind != 0:
            continue
        r = rows.setdefault(sec, [0, 0, 0, 0, 0])
        r[0] += 1
        r[4] = max(r[4], size // 1024)
        if last is not None:
            g = ts - last
            r[1] += g > 25
            r[2] += g > 50
            r[3] = max(r[3], g)
        last = ts
    print("sec units >25ms >50ms maxgap maxKB audio/s")
    for s in sorted(rows):
        r = rows[s]
        print("%3d %5d %5d %5d %6d %5d %7d" % (s, r[0], r[1], r[2], r[3], r[4],
                                             audio.get(s, 0)))


if __name__ == "__main__":
    main(sys.argv[1])
