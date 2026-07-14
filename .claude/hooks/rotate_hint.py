#!/usr/bin/env python3
# block-via: none (PostToolUse; ADVISORY only -- never blocks, never changes flow)
"""PostToolUse: context-rotation hint (P4.1).

A turn-count PROXY for the ~200-250K-token doctrine band (a hook cannot read the
session's live context size, so tool-call count since the last rollover is the
practical proxy). In the overnight SECTIONS phase this counts tool events in
`.claude/state/rotate-hint.json` and, once the count crosses ROTATE_HINT_TURNS,
sets an ADVISORY `hint: true` flag. It NEVER blocks and NEVER changes flow -- it
only sets a flag. P4.6 (safe-boundary firing) decides IF/WHEN to act on the hint
at a WIP-clean boundary; run_phase_guard clears the file on a verified rollover so
each fresh worker counts from zero. Fail-open on any error (a hint that never
fires just means the size-trigger is unavailable, never a wedge).
"""
from __future__ import annotations

import json
import sys
import time
from pathlib import Path

# Turn-count proxy for the doctrine context band. Deliberately conservative: the
# hint is advisory, and P4.6 only acts at a safe boundary, so an early hint costs
# nothing. Tune via the canary (P4.4/P4.8).
ROTATE_HINT_TURNS = 140

HINT_REL = ".claude/state/rotate-hint.json"


def _repo_root() -> Path | None:
    here = Path(__file__).resolve()
    for p in here.parents:
        if (p / ".claude").is_dir():
            return p
    return None


def _in_sections(root: Path) -> bool:
    try:
        st = json.loads((root / ".claude/state/sequencer-run.json").read_text())
    except Exception:
        return False
    return bool(st.get("active")) and st.get("phase") == "SECTIONS"


def main() -> int:
    try:
        json.load(sys.stdin)  # consume payload; any tool event is one "turn"
    except Exception:
        return 0
    root = _repo_root()
    if root is None or not _in_sections(root):
        return 0
    p = root / HINT_REL
    try:
        data = json.loads(p.read_text())
        if not isinstance(data, dict):
            data = {}
    except Exception:
        data = {}
    data["count"] = int(data.get("count", 0)) + 1
    if data["count"] >= ROTATE_HINT_TURNS and not data.get("hint"):
        data["hint"] = True
        data["hint_since_ts"] = time.time()
        data["hint_at_turns"] = data["count"]
    try:
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(json.dumps(data))
    except Exception:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
