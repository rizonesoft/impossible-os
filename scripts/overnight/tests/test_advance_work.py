#!/usr/bin/env python3
"""Smoke test for advance-work.py -- the pre-Opus packet assembler.

advance-work orchestrates the real oracle/classify over the whole repo, so (like
test-launch.sh) it runs against the real tree with --no-pack for speed. It
asserts the packet is well-formed, not any specific cursor (that moves as the
repo progresses). The composed tools each have their own hermetic tests."""
import json
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
AW = HERE.parent / "advance-work.py"
REPO = HERE.parent.parent.parent


def test_packet_is_well_formed():
    r = subprocess.run([sys.executable, str(AW), "--no-pack"],
                       capture_output=True, text=True, cwd=str(REPO), timeout=90)
    assert r.returncode == 0, r.stderr
    d = json.loads(r.stdout)
    assert d.get("action") in ("work", "fixpoint", "blocked", "unknown"), d
    if d["action"] == "work":
        assert d.get("cursor", {}).get("file"), d
        assert "sections_summary" in d, d
        # preflight verdict is always surfaced (cached stamp or 'not run')
        assert "preflight" in d, d


if __name__ == "__main__":
    test_packet_is_well_formed()
    print("PASS: advance-work")
