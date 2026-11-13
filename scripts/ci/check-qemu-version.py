#!/usr/bin/env python3
"""Refuse a CI image whose QEMU is not the one scripts/ci-qemu-version.txt records.

The CI-parity gate compares the local QEMU with that file, so the file must name
the QEMU the image holds, exactly: a substring test accepted 10.2.10 for 10.2.1.

usage: qemu-system-x86_64 --version | check-qemu-version.py [recorded-file]
Exit: 0 equal, 1 different or unreadable.
"""
import re
import sys
from pathlib import Path

RECORD = Path(__file__).resolve().parents[1] / "ci-qemu-version.txt"
VERSION = re.compile(r"^QEMU emulator version (\d+\.\d+\.\d+)(?=[\s(]|$)")


def recorded(path: Path) -> str:
    for line in path.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            return line
    return ""


def held(version_output: str) -> str:
    m = VERSION.match(version_output.splitlines()[0] if version_output else "")
    return m.group(1) if m else ""


def main(argv: list[str]) -> int:
    want = recorded(Path(argv[1]) if len(argv) > 1 else RECORD)
    got = held(sys.stdin.read())
    print(f"image: {got or '(unreadable)'}; recorded: {want or '(empty)'}")
    if not want or not got or got != want:
        print(f"::error::the image holds QEMU {got or '(unreadable)'} but scripts/ci-qemu-version.txt records {want or '(nothing)'}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
