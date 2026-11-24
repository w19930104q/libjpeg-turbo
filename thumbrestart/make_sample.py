#!/usr/bin/env python3
"""Generate the fixed JT-B sample: three deterministic 257x129 RGB PPM images.

Images 2 and 3 are pixel-identical (so correct 0-interval encodings must be
byte-for-byte equal); image 1 differs.  All generation is deterministic and
uses the Python standard library only (run with python3 -I).

Usage: python3 -I thumbrestart/make_sample.py [output-dir]
"""

import os
import sys

WIDTH, HEIGHT = 257, 129


def make_image(seed):
    buf = bytearray(WIDTH * HEIGHT * 3)
    for y in range(HEIGHT):
        for x in range(WIDTH):
            i = (y * WIDTH + x) * 3
            buf[i] = (x * 7 + seed) & 255
            buf[i + 1] = (y * 11 + seed * 3) & 255
            buf[i + 2] = (x + y + seed * 5) & 255
    return bytes(buf)


def write_ppm(path, pixels):
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (WIDTH, HEIGHT))
        f.write(pixels)


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "sample"
    )
    os.makedirs(outdir, exist_ok=True)
    seeds = (1, 42, 42)  # image 1 unique; images 2 and 3 identical
    for idx, seed in enumerate(seeds, start=1):
        path = os.path.join(outdir, "JT-B-%d.ppm" % idx)
        write_ppm(path, make_image(seed))
        print("wrote %s (%dx%d RGB)" % (path, WIDTH, HEIGHT))


if __name__ == "__main__":
    main()
