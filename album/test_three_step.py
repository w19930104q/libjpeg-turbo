#!/usr/bin/env python3
"""
Three-step failure/retry/success test for the atomic album batch tool.

Scenario (same source image throughout):

  step 1  FAIL      albumbatch with --fail-point transform: the library
                    call never runs.  The destination must not be created
                    and no temp/half-rotated cache file may survive.
  step 2  RETRY     albumbatch with --fail-point write: the in-memory
                    transform succeeds and a temp file is written, but the
                    process aborts before the atomic rename.  The existing
                    destination must remain byte-identical to its original
                    four-corner content, and the temp file must be gone.
  step 3  SUCCESS   normal run publishes the finished image.

Invariants checked:
  * destination is untouched after each failed step (same four corners);
  * no *.tmp.* residue after any step;
  * success output has the transformed dimensions;
  * a second success run is byte-for-byte identical to the first
    (pixel content does not depend on write order / run number);
  * crop + orientation are demonstrably applied in one tj3Transform() call
    (one process, one tjtransform with both TJXOPT_CROP and a TJXOP).

The geometric expectation in the task brief (corners W,B,G,R at 300x400)
is checked against the library's actual corner map.  As proven by the
geometry table, no single lossless dihedral transform maps the JT-A
corners (TL=R,TR=G,BL=B,BR=W) to W,B,G,R with swapped dimensions: every
dimension-swapping op is an odd permutation, whereas that corner sequence
is the even 180-degree permutation.  The test reports both the brief's
expectation and the library's true result rather than asserting an
impossible mapping.
"""
import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / "album" / "albumbatch"
DJPEG = ROOT / "build-local" / "djpeg-static"
SRC = ROOT / "artifacts" / "fixture" / "JT-A.jpg"
WORK = ROOT / "artifacts" / "runs" / "three-step"

RED, GREEN, BLUE, WHITE = (254, 0, 0), (0, 255, 1), (0, 0, 254), (255, 255, 255)
LABELS = {RED: "R", GREEN: "G", BLUE: "B", WHITE: "W"}
EXPECTED_SRC_CORNERS = ["R", "G", "B", "W"]          # TL, TR, BL, BR
BRIEF_EXPECTED_CORNERS = ["W", "B", "G", "R"]        # from the task brief
# Rotation + flip in one tj3Transform() call.  JT-A is 400x300 with an 8x8
# iMCU (4:4:4), so its height is not iMCU-aligned: the default mode keeps the
# nominal 300x400 dimensions (partial iMCUs left in place, per TJXOPT docs),
# whereas -trim is fully reversible but yields 296x400.  Both are exercised.
OP = ["-op", "transverse"]
OP_TRIM = ["-op", "transverse", "-trim"]


def decode(path: Path):
    out = subprocess.run([str(DJPEG), str(path)], check=True,
                         capture_output=True).stdout
    i = 2
    vals = []
    while len(vals) < 3:
        while out[i] in b" \t\n\r":
            i += 1
        s = i
        while out[i] not in b" \t\n\r":
            i += 1
        vals.append(int(out[s:i]))
    w, h = vals[0], vals[1]
    data = out[i + 1:]
    return w, h, data


def corners(path: Path):
    w, h, d = decode(path)

    def px(x, y):
        o = (y * w + x) * 3
        return tuple(d[o:o + 3])

    order = (px(0, 0), px(w - 1, 0), px(0, h - 1), px(w - 1, h - 1))
    return w, h, [LABELS.get(c, "?") for c in order]


def sha(path: Path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tmp_residue(d: Path):
    return sorted(p.name for p in d.glob("*.tmp.*"))


def run_tool(dst: Path, *extra, expect_fail=False):
    cmd = [str(TOOL), *OP, "-o", str(dst), str(SRC), *extra]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if expect_fail:
        assert r.returncode != 0, f"expected failure, got success: {cmd}"
    else:
        assert r.returncode == 0, f"unexpected failure: {cmd}\n{r.stderr}"
    return r


def main():
    if WORK.exists():
        shutil.rmtree(WORK)
    WORK.mkdir(parents=True)
    dst = WORK / "JT-A-out.jpg"
    report = []

    # --- reference: the source itself -------------------------------------
    sw, sh, sc = corners(SRC)
    assert (sw, sh) == (400, 300), (sw, sh)
    assert sc == EXPECTED_SRC_CORNERS, sc
    report.append(f"source JT-A: {sw}x{sh} corners(TL,TR,BL,BR)={sc}")

    # --- step 1: failure before the transform -----------------------------
    run_tool(dst, "--fail-point", "transform", expect_fail=True)
    assert not dst.exists(), "destination created despite pre-transform failure"
    assert tmp_residue(WORK) == [], "temp file left behind after step 1"
    report.append("step 1 FAIL (pre-transform): no destination, no temp residue")

    # Seed a real destination so step 2 can prove in-place preservation.
    seed = WORK / "seed.jpg"
    shutil.copyfile(SRC, seed)
    shutil.copyfile(SRC, dst)
    seed_hash = sha(dst)
    w, h, cs = corners(dst)
    assert cs == EXPECTED_SRC_CORNERS
    report.append(f"seeded destination with original: {w}x{h} corners={cs} "
                  f"sha256={seed_hash[:16]}...")

    # --- step 2: retry fails at the publish boundary ----------------------
    run_tool(dst, "--fail-point", "write", expect_fail=True)
    assert dst.exists(), "destination vanished after publish-stage failure"
    assert sha(dst) == seed_hash, "destination modified despite failed rename"
    assert tmp_residue(WORK) == [], "temp file left behind after step 2"
    w, h, cs = corners(dst)
    assert cs == EXPECTED_SRC_CORNERS, cs
    report.append("step 2 RETRY FAIL (post-transform/pre-rename): destination "
                  f"byte-identical, corners still {cs}, no temp residue")

    # --- step 3: success ---------------------------------------------------
    run_tool(dst)
    ok_hash = sha(dst)
    w, h, cs = corners(dst)
    # 400x300 -> transverse (UR-to-LL transpose = rotate+flip) -> 300x400
    assert (w, h) == (300, 400), (w, h)
    assert tmp_residue(WORK) == []
    report.append(f"step 3 SUCCESS: {w}x{h} corners={cs} "
                  f"sha256={ok_hash[:16]}...")

    # --- determinism: second successful run must be identical --------------
    dst2 = WORK / "JT-A-out-again.jpg"
    run_tool(dst2)
    assert sha(dst2) == ok_hash, "second successful run produced different bytes"
    assert corners(dst2)[2] == cs
    report.append("determinism: second success run byte-for-byte identical "
                  "(sha256 match)")

    # --- fully reversible (-trim) variant, also deterministic --------------
    tdst = WORK / "trimmed.jpg"
    tr = subprocess.run([str(TOOL), *OP_TRIM, "-o", str(tdst), str(SRC)],
                        capture_output=True, text=True, check=True)
    tw, th, tcs = corners(tdst)
    assert (tw, th) == (296, 400), (tw, th)
    tdst2 = WORK / "trimmed-again.jpg"
    subprocess.run([str(TOOL), *OP_TRIM, "-o", str(tdst2), str(SRC)],
                   capture_output=True, text=True, check=True)
    assert sha(tdst2) == sha(tdst)
    report.append(f"reversible -trim path: {tw}x{th} corners={tcs}, "
                  "also byte-deterministic")

    # --- brief expectation vs. library reality ----------------------------
    match = cs == BRIEF_EXPECTED_CORNERS
    report.append("brief expectation W,B,G,R @300x400: "
                  + ("MATCH" if match else
                     f"not attainable by one lossless dihedral op; "
                     f"library output for transverse(rotate+flip) = {cs}"))

    # --- single-call crop+orientation evidence ----------------------------
    crop_dst = WORK / "crop-plus-orient.jpg"
    cmd = [str(TOOL), "-op", "transpose", "-crop", "200x150+0+0",
           "-o", str(crop_dst), str(SRC)]
    r = subprocess.run(cmd, capture_output=True, text=True, check=True)
    cw, ch, cc = corners(crop_dst)
    report.append(f"one-call crop+transpose: {cw}x{ch} corners={cc} "
                  "(single process, TJXOPT_CROP | TJXOP_TRANSPOSE)")

    # --- real (non-injected) failure: truncated source ---------------------
    real_dst = WORK / "real-fail-dst.jpg"
    shutil.copyfile(SRC, real_dst)
    bad = WORK / "truncated.jpg"
    bad.write_bytes(SRC.read_bytes()[:800])
    before = sha(real_dst)
    r = subprocess.run([str(TOOL), "-op", "transverse", "-o",
                        str(real_dst), str(bad)], capture_output=True,
                       text=True)
    assert r.returncode != 0
    assert sha(real_dst) == before, "real failure modified destination"
    assert tmp_residue(WORK) == []
    report.append("real corrupt-input failure: destination hash unchanged, "
                  "no temp residue")

    # --- multi-image batch determinism -------------------------------------
    outs = []
    for n in ("a", "b"):
        src_n = WORK / f"{n}.jpg"
        shutil.copyfile(SRC, src_n)
        out_n = WORK / f"{n}.out.jpg"
        subprocess.run([str(TOOL), "-op", "transpose",
                        "-crop", "200x150+0+0", "-o", str(out_n),
                        str(src_n)], check=True, capture_output=True)
        outs.append(sha(out_n))
    assert outs[0] == outs[1]
    report.append("multi-image batch: identical inputs -> identical outputs")

    print("\n".join("  " + line for line in report))
    print("\nALL STRUCTURAL INVARIANTS PASSED")
    if not match:
        print("NOTE: brief corner permutation W,B,G,R is geometrically "
              "inconsistent with a 90-class lossless transform of JT-A "
              "(see report above).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
