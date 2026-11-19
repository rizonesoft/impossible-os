#!/usr/bin/env python3
"""Each desktop shortcut of `scripts/run-desktop.sh` carries its own badge on the logo.

Until 2026-11-20 all three (Start, Watch, Stop) carried the same plain logo, so they could not be told apart
on the desktop, where the other projects' run shortcuts carry a runner, eye or stop badge.
"""
from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("campaign_icon", ROOT / "scripts" / "campaign-icon.py")
assert _spec is not None and _spec.loader is not None
icon = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(icon)

# The badge colour each shortcut must carry: Start runs Claude, Watch is the eye, Stop the stop square.
EXPECTED = {"start": "claude", "watch": "watch", "stop": "stop"}


def badge_fill(path: Path) -> tuple[int, int, int]:
    """The colour inside the badge ring of the 256-pixel frame, below the white mark."""
    width, _, rgba = icon.read_ico(path.read_bytes())[256]
    radius = 256 * 0.24
    centre = 256 - radius - 256 * 0.01
    x, y = round(centre), round(centre + radius * 0.75)
    o = (y * width + x) * 4
    return tuple(rgba[o:o + 3])


# rsvg-convert is optional (scripts/setup.sh does not install it; without it the shortcuts keep the WSL icon).
# The shortcuts' icons are made on the WSL side of the Windows host (bash and GNU timeout).
@unittest.skipUnless(shutil.which("rsvg-convert") and shutil.which("timeout"), "needs rsvg-convert and timeout")
class RunDesktopIconsTest(unittest.TestCase):
    def test_each_shortcut_has_its_own_badge(self):
        with tempfile.TemporaryDirectory() as tmp:
            # GNU timeout runs bash in a process group of its own and, on expiry or on a TERM from the
            # runner (it stays in the test's group, so the runner's group kill reaches it), signals that
            # whole group: rsvg-convert and python3 stop with bash.
            done = subprocess.run(["timeout", "--kill-after=5", "120",
                                   "bash", "-c", 'source scripts/run-desktop.sh && make_icons "$1"', "_", tmp],
                                  cwd=ROOT, check=False,
                                  creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            self.assertEqual(done.returncode, 0, "make_icons failed (124: timed out)")
            fills = {}
            for verb, badge in EXPECTED.items():
                path = Path(tmp) / f"impossible-os-{verb}.ico"
                self.assertTrue(path.is_file(), f"no icon for {verb}")
                want = icon.hex_colour(icon.BADGES[badge][0])
                got = badge_fill(path)
                self.assertTrue(all(abs(a - b) <= 6 for a, b in zip(got, want)),
                                f"{verb}: badge colour {got}, expected {want} ({badge})")
                fills[verb] = got
            self.assertEqual(len(set(fills.values())), 3, "the three shortcuts must look different")


if __name__ == "__main__":
    unittest.main()
