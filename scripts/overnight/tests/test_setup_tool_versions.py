#!/usr/bin/env python3
"""The nasm version probe must read both spellings. NASM 3.x prints `3.01`, and the
old three-part pattern read nothing, so setup.sh --verify failed a current tool as
"version unparseable". Control: a version-less line must still read as empty, so the
fail-closed path stays reachable.
"""
import pathlib
import re
import stat
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[3]
LINE = re.search(r'"nasm:nasm:[^:]+:(.*)"$', (REPO / "scripts/setup.sh").read_text(), re.M)
assert LINE, "nasm probe line not found"


def probe(output: str) -> str:
    with tempfile.TemporaryDirectory() as d:
        fake = pathlib.Path(d, "nasm")
        fake.write_text(f"#!/bin/sh\necho '{output}'\n")
        fake.chmod(fake.stat().st_mode | stat.S_IEXEC)
        r = subprocess.run(["bash", "-c", f'PATH="{d}:$PATH"; {LINE.group(1)}'],
                           capture_output=True, text=True, timeout=20)
    return r.stdout.strip()


def test_two_part_version():
    assert probe("NASM version 3.01") == "3.01"


def test_three_part_version_with_build_date():
    assert probe("NASM version 2.16.01 compiled on Dec 21 2023") == "2.16.01"


def test_no_version_stays_empty():
    assert probe("NASM version unknown") == ""


if __name__ == "__main__":
    test_two_part_version()
    test_three_part_version_with_build_date()
    test_no_version_stays_empty()
    print("PASS test_setup_tool_versions")
    sys.exit(0)
