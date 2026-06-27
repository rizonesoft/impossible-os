#!/usr/bin/env python3
# block-via: warning-only (WS1b agent-discipline WARN; never exits 2)
"""PreToolUse: WARN when an overnight SECTIONS-phase source edit happens with no
recent agent dispatch. WARN-only (exit 0 + stderr); invisible in interactive
sessions. Fail-open. The BLOCK promotion + triviality classifier is a later plan.
"""
from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path

FRESH_NS = 1800 * 1_000_000_000  # 30 min


def _is_source_target(path: str) -> bool:
    if not path:
        return False
    if path.endswith(".md"):
        return False
    for seg in (".claude/", "build/", "docs/"):
        if seg in path:
            return False
    return path.endswith((".c", ".h", ".asm", ".S", ".py", ".sh"))


def _repo_root() -> Path | None:
    here = Path(__file__).resolve()
    for p in here.parents:
        if (p / ".claude").is_dir():
            return p
    return None


def _guard_in_sections(root: Path) -> bool:
    try:
        st = json.loads((root / ".claude" / "state" / "sequencer-run.json").read_text())
    except Exception:
        return False
    return bool(st.get("active")) and st.get("phase") == "SECTIONS"


def _recent_dispatch(root: Path) -> bool:
    try:
        st = json.loads((root / ".claude" / "state" / "last-agent-dispatch.json").read_text())
    except Exception:
        return False
    ts = st.get("timestamp_ns")
    return isinstance(ts, int) and (time.time_ns() - ts) <= FRESH_NS


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    # Invisible outside a headless overnight run.
    if not os.environ.get("OVERNIGHT_SEQUENCER_RUN"):
        return 0
    if os.environ.get("SKIP_AGENT_DISPATCH_HOOK") == "1":
        return 0
    if d.get("tool_name") not in ("Edit", "Write", "MultiEdit"):
        return 0
    ti = d.get("tool_input") or {}
    if not _is_source_target(ti.get("file_path") or ti.get("path") or ""):
        return 0
    root = _repo_root()
    if root is None or not _guard_in_sections(root):
        return 0
    if _recent_dispatch(root):
        return 0
    sys.stderr.write(
        "[agent-dispatch reminder] WS1b: a source edit in the SECTIONS phase with "
        "no read-only explorer/auditor dispatch in the last 30 min. Default-on "
        "agents keep the main context lean. Dispatch one, or set "
        "SKIP_AGENT_DISPATCH_HOOK=1 with a logged reason if this section is tiny.\n"
    )
    return 0  # WARN-only this plan


if __name__ == "__main__":
    sys.exit(main())
