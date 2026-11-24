#!/usr/bin/env python3
"""JT-B regression test: restart-marker state must not leak across images
when one compressor object is reused.

Runs thumbrestart in both TurboJPEG and raw libjpeg C API modes and checks:
  1. exit code 0 (the C harness itself verifies DRI/RSTn/byte-identity and
     decodes all three outputs back to the source pixels);
  2. stdout reports exactly one DRI=8 image (run 1), two images without DRI
     or RSTn markers (runs 2-3), and a final PASS line.

Run:  python3 -I thumbrestart/test_jtb.py [build-dir]
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build-local")
TOOL = os.path.join(ROOT, "thumbrestart", "thumbrestart")


def run_mode(mode):
    proc = subprocess.run(
        [TOOL, "-m", mode], capture_output=True, text=True, timeout=120
    )
    print("---- mode=%s (exit %d) ----" % (mode, proc.returncode))
    print(proc.stdout, end="")
    if proc.stderr:
        print(proc.stderr, end="", file=sys.stderr)
    assert proc.returncode == 0, "thumbrestart -m %s failed" % mode

    rows = re.findall(
        r"run (\d) \(interval=(\d+)\): (\d+) bytes, DRI=(\d+)\(value=(\d+)\), "
        r"RSTn=(\d+) \.\.\. (OK|FAIL)",
        proc.stdout,
    )
    assert len(rows) == 3, "expected 3 result rows, got %d" % len(rows)

    for idx, row in enumerate(rows):
        run, interval, size, dri, drival, rst, verdict = row
        assert int(run) == idx + 1
        assert verdict == "OK", "run %d reported FAIL" % run
        if idx == 0:
            assert int(interval) == 8 and int(dri) == 1
            assert int(drival) == 8 and int(rst) > 0, row
        else:
            assert int(interval) == 0 and int(dri) == 0
            assert int(rst) == 0, row

    # Runs 2 and 3 have identical pixels and parameters -> identical bytes.
    sizes = [int(r[2]) for r in rows]
    assert sizes[1] == sizes[2], "run 2/3 size mismatch"

    assert "DECODER ERROR" not in proc.stdout
    assert "old-decoder compatibility: FAIL" not in proc.stdout
    assert proc.stdout.rstrip().endswith(
        "PASS: all JT-B restart-marker invariants hold"
    )


def main():
    assert os.path.isfile(TOOL), (
        "thumbrestart binary missing; run thumbrestart/build.sh first"
    )
    for mode in ("tj", "capi"):
        run_mode(mode)
    print("\nJT-B: all checks passed for TurboJPEG and libjpeg C API")


if __name__ == "__main__":
    main()
