#!/usr/bin/env python3
"""Generate fixed sample JT-A: 400x300 baseline JPEG, quadrants R/G/B/W.

Corners (row-major TL,TR,BL,BR) = red, green, blue, white.
4:4:4 subsampling so the iMCU is 8x8; emitted as a single-scan sequential
baseline JPEG via libjpeg-turbo cjpeg (lossless-transform eligible).
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CJPEG = ROOT / "build-local" / "cjpeg-static"
OUT_DIR = ROOT / "artifacts" / "fixture"
W, H = 400, 300

RED, GREEN, BLUE, WHITE = (255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255)

# Quadrant fill -> corners: TL=R, TR=G, BL=B, BR=W
rows = []
for y in range(H):
    row = bytearray()
    for x in range(W):
        c = (RED if (x < W // 2 and y < H // 2) else
             GREEN if (x >= W // 2 and y < H // 2) else
             BLUE if (x < W // 2 and y >= H // 2) else WHITE)
        row += bytes(c)
    rows.append(bytes(row))
ppm = b"P6\n%d %d\n255\n" % (W, H) + b"".join(rows)

OUT_DIR.mkdir(parents=True, exist_ok=True)
ppm_path = OUT_DIR / "JT-A.ppm"
jpg_path = OUT_DIR / "JT-A.jpg"
ppm_path.write_bytes(ppm)
subprocess.run([str(CJPEG), "-sample", "1x1", "-baseline", "-quality", "100",
                "-outfile", str(jpg_path), str(ppm_path)], check=True)
print(jpg_path)
