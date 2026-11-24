#!/usr/bin/env python3
"""
Exhaustive geometry matrix for fixed sample JT-A.

Runs every supported lossless geometric op (with and without -trim),
records decoded dimensions and four-corner labels, and checks whether any
single op yields 300x400 with corners (W,B,G,R) in row-major order
(TL,TR,BL,BR).

Source corners: TL=R TR=G / BL=B BR=W, source matrix [R G; B W].
Target sequence (W,B,G,R) is the source's 180-degree permutation:
    [W B; G R]  (even permutation, 4 cycles of length 2 -> parity even)
Every op that swaps width and height (rot90/rot270/transpose/transverse)
is an odd permutation of the four corners, so none can produce an even
permutation.  180-degree rotation does produce (W,B,G,R) but keeps the
400x300 dimensions.  Hence "300x400 + W,B,G,R" has no dihedral solution.
"""
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / "album" / "albumbatch"
DJPEG = ROOT / "build-local" / "djpeg-static"
SRC = ROOT / "artifacts" / "fixture" / "JT-A.jpg"
OUT = ROOT / "artifacts" / "runs" / "geometry-matrix.txt"

OPS = ["none", "hflip", "vflip", "transpose", "transverse",
       "rot90", "rot180", "rot270"]
LAB = {(254, 0, 0): "R", (0, 255, 1): "G", (0, 0, 254): "B",
       (255, 255, 255): "W"}


def measure(jpg: Path):
    out = subprocess.run([str(DJPEG), str(jpg)], check=True,
                         capture_output=True).stdout
    i = 2
    v = []
    while len(v) < 3:
        while out[i] in b" \t\n\r":
            i += 1
        s = i
        while out[i] not in b" \t\n\r":
            i += 1
        v.append(int(out[s:i]))
    w, h, d = v[0], v[1], out[i + 1:]

    def px(x, y):
        o = (y * w + x) * 3
        return tuple(d[o:o + 3])

    cs = [LAB[px(0, 0)], LAB[px(w - 1, 0)], LAB[px(0, h - 1)],
          LAB[px(w - 1, h - 1)]]
    return w, h, cs


def main():
    lines = [
        "JT-A geometry matrix (source 400x300, corners TL,TR,BL,BR = R,G,B,W)",
        f"{'op':11} {'mode':8} {'dims':9} corners(TL,TR,BL,BR)  "
        "300x400&W,B,G,R?",
        "-" * 72,
    ]
    hits = 0
    for op in OPS:
        for mode, extra in (("default", []), ("trim", ["-trim"])):
            with tempfile.NamedTemporaryFile(suffix=".jpg", delete=False) as f:
                dst = Path(f.name)
            r = subprocess.run([str(TOOL), "-op", op, *extra,
                                "-o", str(dst), str(SRC)],
                               capture_output=True, text=True)
            if r.returncode != 0:
                lines.append(f"{op:11} {mode:8} ERROR: "
                             f"{r.stderr.strip().splitlines()[-1]}")
                dst.unlink(missing_ok=True)
                continue
            w, h, cs = measure(dst)
            hit = (w, h) == (300, 400) and cs == ["W", "B", "G", "R"]
            hits += int(hit)
            lines.append(f"{op:11} {mode:8} {str(w)+'x'+str(h):9} "
                         f"{','.join(cs):17} {'YES' if hit else 'no'}")
            dst.unlink()
    lines.append("-" * 72)
    lines.append(f"ops matching 300x400 + W,B,G,R: {hits}")
    lines.append("rot180 produces W,B,G,R but at 400x300 (dims not swapped).")
    text = "\n".join(lines)
    OUT.write_text(text + "\n")
    print(text)


if __name__ == "__main__":
    main()
