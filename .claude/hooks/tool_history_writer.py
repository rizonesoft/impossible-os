#!/usr/bin/env python3
# block-via: warning-only (PostToolUse logger -- never blocks)
"""PostToolUse tool-history writer (TODO-08 §11).

Appends one JSON-line record per tool call to .claude/state/tool-history.jsonl
with shape:
    {"event": "PostToolUse", "tool_name": "<name>", "tool_use_id": "<id>",
     "ts_ns": <int>, "duration_ms": <int|null>, "success": <bool|null>}

This is the source-of-truth log that other §11 hooks read:
  - stop_audit.py walks the tail to detect acknowledged-but-skipped promises.
  - Future drift checks can use the log to count subagent budgets etc.

Size bounding (per design Q1):
  - Live rotate at LIVE_ROTATE_BYTES (default 10 MiB) -- rename to .1 and
    truncate the live file. Only one rotation slot is kept; older data is
    discarded on the second rotation.
  - SessionStart truncates the live file when older than 7 days (handled
    by session_start.py, not here).

Hook category: WARN-ONLY logger; failure to write is silent.
"""
import json
import os
import subprocess
import sys
import time
from typing import Optional


_STATE_REL = ".claude/state/tool-history.jsonl"
_ROTATE_REL = ".claude/state/tool-history.jsonl.1"
LIVE_ROTATE_BYTES = 10 * 1024 * 1024


def _repo_root() -> Optional[str]:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            text=True, timeout=2, stderr=subprocess.DEVNULL,
        ).strip() or None
    except Exception:
        return None


def _ts_ns() -> int:
    return time.time_ns() if hasattr(time, "time_ns") else int(time.time() * 1e9)


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict):
        return 0

    root = _repo_root()
    if not root:
        return 0

    state_path = os.path.join(root, _STATE_REL)
    rotate_path = os.path.join(root, _ROTATE_REL)
    state_dir = os.path.dirname(state_path)
    try:
        os.makedirs(state_dir, exist_ok=True)
    except Exception:
        return 0

    ti = d.get("tool_input", {}) or {}
    record = {
        "event": d.get("event_type") or "PostToolUse",
        "tool_name": d.get("tool_name", ""),
        "tool_use_id": d.get("tool_use_id", ""),
        "ts_ns": _ts_ns(),
        "duration_ms": d.get("duration_ms"),
        "success": d.get("success"),
    }
    # Per design Q1: live rotate at 10 MiB. Single .1 slot; older lost.
    try:
        if os.path.exists(state_path) and os.path.getsize(state_path) >= LIVE_ROTATE_BYTES:
            try:
                os.replace(state_path, rotate_path)
            except Exception:
                pass
    except Exception:
        pass

    try:
        with open(state_path, "a", encoding="utf-8") as f:
            f.write(json.dumps(record, separators=(",", ":")) + "\n")
    except Exception:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
