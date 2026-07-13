#!/usr/bin/env python3
# block-via: warning-only (PostToolUse reminder; never blocks -- a verification
# run must never be gated, this only nudges cadence)
"""PostToolUse (Bash): verify-cadence reminder for the overnight SECTIONS phase.

P3.3 -- the fix loop should run TARGETED suites (`SUITE=xx`) while iterating and
reserve ONE full suite + smoke for the section boundary. A 2026-07-13 run instead
ran ~40 builds / ~17 smoke mid-loop. This hook counts FULL-suite runs
(`scripts/test.sh` with no `SUITE=`) and SMOKE runs (`test-smoke`) within the
current cursor section and, past a small threshold, reminds the model to switch to
targeted suites mid-loop. It is a frequency nudge, never a gate (it CANNOT hang
the run -- smoke stays unconditional AT the boundary). Interactive sessions and
the wrapped/bare form are both counted; the reset is keyed on the section cursor.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

# Warn once the SAME section has run this many FULL suites or SMOKE runs mid-loop.
_FULL_SUITE_THRESHOLD = 3   # targeted suites are free; >=3 full runs is churn
_SMOKE_THRESHOLD = 2        # each smoke boots the OS (~expensive); >=2 mid-loop is churn

_FULL_SUITE_RE = re.compile(r"scripts/test\.sh\b")
_TARGETED_RE = re.compile(r"\bSUITE=")
_SMOKE_RE = re.compile(r"\btest-smoke(?:\.sh)?\b")

_MSG = (
    "[verify-cadence -- not a block] This section has run {kind} x{n} mid-loop. "
    "During the fix loop use TARGETED suites (`bash scripts/overnight/run-artifact.sh "
    "lbl -- bash scripts/test.sh SUITE=<cat>`) -- they re-verify only the owning "
    "category over unchanged content, near-free. Reserve ONE full suite + smoke for "
    "the SECTION BOUNDARY (the final build/self-review), not every fix round "
    "(measured: ~40 builds / ~17 smoke in one section)."
)


def _repo_root() -> Path | None:
    here = Path(__file__).resolve()
    for p in here.parents:
        if (p / ".claude").is_dir():
            return p
    return None


def _section_state(root: Path):
    """(in_sections, section_key). section_key = file#idx of the cursor."""
    try:
        st = json.loads((root / ".claude/state/sequencer-run.json").read_text())
    except Exception:
        return (False, "")
    if not (isinstance(st, dict) and st.get("active") and st.get("phase") == "SECTIONS"):
        return (False, "")
    return (True, f"{st.get('file')}#{st.get('section_idx')}")


def _state_path(root: Path) -> Path:
    return root / ".claude/state/verify-cadence.json"


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict) or d.get("tool_name") != "Bash":
        return 0
    cmd = (d.get("tool_input") or {}).get("command") or ""
    root = _repo_root()
    if root is None:
        return 0
    in_sections, section_key = _section_state(root)
    if not in_sections:
        return 0

    is_full = bool(_FULL_SUITE_RE.search(cmd)) and not _TARGETED_RE.search(cmd)
    is_smoke = bool(_SMOKE_RE.search(cmd))
    if not (is_full or is_smoke):
        return 0

    sp = _state_path(root)
    try:
        st = json.loads(sp.read_text())
        if not isinstance(st, dict):
            st = {}
    except Exception:
        st = {}
    # Reset the counters when the cursor moves to a new section.
    if st.get("section_key") != section_key:
        st = {"section_key": section_key, "full": 0, "smoke": 0}

    kind = None
    if is_smoke:
        st["smoke"] = int(st.get("smoke", 0)) + 1
        if st["smoke"] >= _SMOKE_THRESHOLD:
            kind, n = "smoke", st["smoke"]
    if is_full:
        st["full"] = int(st.get("full", 0)) + 1
        if kind is None and st["full"] >= _FULL_SUITE_THRESHOLD:
            kind, n = "the full suite", st["full"]

    try:
        sp.parent.mkdir(parents=True, exist_ok=True)
        sp.write_text(json.dumps(st))
    except Exception:
        pass

    if kind is not None:
        print(json.dumps({"systemMessage": _MSG.format(kind=kind, n=n)}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
