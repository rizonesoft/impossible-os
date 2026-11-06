#!/usr/bin/env python3
"""Count findings a previous run left in a capture file with no verdict.

arm-sequencer.sh refuses a CLOSED newest capture file, which catches a close-out
that forgot to open the next version. It cannot catch a close-out that never
happened: the file stays open, the arm passes, and the next run's findings mix
with the old run's (v21 sat that way for five weeks). A freshly opened version has
an empty `## Found live this cycle`, so any item there predates this arm.

A re-arm inside one cycle is legitimate, so the caller warns rather than refuses.

Usage: capture-open-findings.py FILE  -> prints the count. Exit 2 if unreadable.
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import todo_fence  # noqa: E402  (the one heading rule; see its section 43 note)

HEADING = "## Found live this cycle"
ITEM = re.compile(r"^- \[ \]")
VERDICT = re.compile(r"\*\*VERDICT")


def count(text: str) -> int:
    open_items, verdict_seen, in_section = 0, True, False
    for line in text.splitlines():
        if todo_fence.is_h2(line):
            in_section = line.strip() == HEADING
            continue
        if not in_section:
            continue
        if ITEM.match(line):
            open_items += 1
            verdict_seen = False
        elif VERDICT.search(line) and not verdict_seen:
            open_items -= 1
            verdict_seen = True
    return open_items


def main(argv) -> int:
    if len(argv) != 2:
        sys.stderr.write(__doc__)
        return 2
    try:
        with open(argv[1], encoding="utf-8", errors="replace") as fh:
            print(count(fh.read()))
    except OSError as exc:
        sys.stderr.write(f"capture-open-findings: {exc}\n")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
