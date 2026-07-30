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
    # TODO-08 §21: capture a primary-input identifier so heuristic
    # gates that read tool-history can answer questions like "was a
    # todo/**/*.md file Read before the first src/ Edit?" without
    # re-scanning the whole transcript. The field is best-effort and
    # bounded to keep storage marginal.
    target = ""
    try:
        if isinstance(ti, dict):
            tn_low = (d.get("tool_name") or "").lower()
            if "file_path" in ti and isinstance(ti["file_path"], str):
                target = ti["file_path"]
            elif "path" in ti and isinstance(ti["path"], str):
                target = ti["path"]
            elif "pattern" in ti and isinstance(ti["pattern"], str):
                target = ti["pattern"]
            elif "query" in ti and isinstance(ti["query"], str):
                target = ti["query"]
            elif tn_low == "skill" and isinstance(ti.get("skill"), str):
                target = ti["skill"]
            elif "command" in ti and isinstance(ti["command"], str):
                target = ti["command"]
            target = target[:240]
    except Exception:
        target = ""
    record = {
        "event": d.get("event_type") or "PostToolUse",
        "tool_name": d.get("tool_name", ""),
        "tool_use_id": d.get("tool_use_id", ""),
        "ts_ns": _ts_ns(),
        "duration_ms": d.get("duration_ms"),
        "success": d.get("success"),
        "target": target,
    }
    # Read EXTENT (token-saver v04 item 2, added 2026-07-30). Sizing re-read waste
    # needs the bytes a Read actually pulled, and nothing recorded it: a
    # `Read(offset, limit)` slice and a whole-file read were indistinguishable, so
    # the only available estimator had to price every read at full file size. That
    # over-counted so badly it reported re-read waste at 139% of the run's total
    # cache-read -- a number that is self-evidently invalid, and worse than none.
    # `offset`/`limit` are the request's own extent (no response access needed
    # here); `bytes` is the file's size at read time, which for a full read IS the
    # extent and for a slice bounds it. Absent/unreadable -> omitted, never
    # guessed, so a consumer can tell "unsized" from "zero".
    try:
        if (record["tool_name"] or "").lower() == "read" and target:
            ti2 = d.get("tool_input") or {}
            for k in ("offset", "limit"):
                if isinstance(ti2.get(k), int):
                    record[k] = ti2[k]
            fp = target if os.path.isabs(target) else os.path.join(root, target)
            record["bytes"] = os.path.getsize(fp)
    except Exception:
        pass
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
