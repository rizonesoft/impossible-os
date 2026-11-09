#!/usr/bin/env python3
"""Every file a generator writes is a target of the rule that runs it, and is ASCII.

WHY (2026-11-09): tools/convert_icon.py writes include/kernel/os_logo.h AND
src/kernel/os_logo.c, but only the header was the rule's target. make treated the
.c as plain source, compiled os_logo.o from the stale copy under -j, and a later make
call in scripts/build.sh relinked kernel.exe after the UKI had packed it
("[uki] FATAL: ... embeds a kernel that differs from build/kernel.exe").
The generators also emitted U+2014, which the code-style policy forbids, so every
regeneration dirtied the committed copies that had been corrected by hand.
"""
from __future__ import annotations

import pathlib
import subprocess
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[3]
DASHES = (chr(0x2013), chr(0x2014))

# generator -> every file one run of it writes
GENERATED = {
    "tools/convert_icon.py": ("include/kernel/os_logo.h", "src/kernel/os_logo.c"),
    "tools/convert_bsod_icon.py": ("src/kernel/bsod_icon.h",),
    "tools/convert_boot_font.py": ("src/kernel/boot_splash_font_data.h",),
}


def test_every_generated_file_is_rebuilt_when_its_generator_changes():
    for tool, outputs in GENERATED.items():
        for out in outputs:
            r = subprocess.run(["make", "-n", "-W", tool, out], cwd=REPO,
                               capture_output=True, text=True, timeout=120)
            assert tool in r.stdout, f"{out} has no rule running {tool}:\n{r.stdout}{r.stderr}"


def test_generators_emit_ascii():
    # Each generator finds its root from its own path, so a copy under a temp root
    # reads the real resources and writes only into the temp tree.
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / "resources").symlink_to(REPO / "resources")
        for tool, outputs in GENERATED.items():
            for rel in (tool, *outputs):
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
            (root / tool).write_bytes((REPO / tool).read_bytes())
            r = subprocess.run(["python3", str(root / tool)], cwd=root, capture_output=True,
                               text=True, timeout=300)
            assert r.returncode == 0, (tool, r.stderr)
            for out in outputs:
                text = (root / out).read_text(encoding="utf-8")
                assert not any(c in text for c in DASHES), f"{out} carries a Unicode dash"


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
            print("ok", name)
