#!/usr/bin/env python3
# block-via: warning-only (SessionStart bootstrap; never blocks)
"""SessionStart hook (TODO-08 §11).

Runs at every Claude Code session-start event. Writes session metadata to
.claude/state/session.json, truncates tool-history.jsonl when stale, and
surfaces the most recent acknowledged-but-skipped warning as
`additionalContext` so the agent sees yesterday's unresolved promise.

State files written:
  .claude/state/session.json -- {session_id, started_ts_ns, cwd, head_sha}
  .claude/state/tool-history.jsonl -- truncated if older than 7 days

State files read:
  .claude/state/acknowledged-but-skipped.log -- last line surfaced as warning

Per design Q1: this is the 7-day truncate companion to the live rotation
in tool_history_writer.py.
"""
import json
import os
import subprocess
import sys
import time
from typing import Optional


_TOOL_HISTORY_REL = ".claude/state/tool-history.jsonl"
_SESSION_REL = ".claude/state/session.json"
_SKIP_LOG_REL = ".claude/state/acknowledged-but-skipped.log"

TOOL_HISTORY_TTL_SEC = 7 * 24 * 60 * 60


def _repo_root() -> Optional[str]:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            text=True, timeout=2, stderr=subprocess.DEVNULL,
        ).strip() or None
    except Exception:
        return None


def _head_sha(root: str) -> str:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"],
            cwd=root, text=True, timeout=2, stderr=subprocess.DEVNULL,
        ).strip()
    except Exception:
        return ""


def _ts_ns() -> int:
    return time.time_ns() if hasattr(time, "time_ns") else int(time.time() * 1e9)


def _truncate_if_stale(path: str) -> None:
    try:
        if not os.path.exists(path):
            return
        age_sec = time.time() - os.path.getmtime(path)
        if age_sec > TOOL_HISTORY_TTL_SEC:
            with open(path, "w", encoding="utf-8") as f:
                f.write("")
    except Exception:
        pass


def _last_skip_warning(path: str) -> str:
    try:
        with open(path, "r", encoding="utf-8") as f:
            lines = [ln.rstrip() for ln in f if ln.strip()]
        if not lines:
            return ""
        return lines[-1][:200]
    except Exception:
        return ""


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0

    root = _repo_root()
    if not root:
        return 0

    state_dir = os.path.join(root, ".claude", "state")
    try:
        os.makedirs(state_dir, exist_ok=True)
    except Exception:
        return 0

    sid = d.get("session_id") or d.get("sessionId") or ""
    cwd = d.get("cwd") or os.getcwd()
    record = {
        "session_id": sid,
        "started_ts_ns": _ts_ns(),
        "cwd": cwd,
        "head_sha": _head_sha(root),
    }
    session_path = os.path.join(root, _SESSION_REL)
    try:
        with open(session_path, "w", encoding="utf-8") as f:
            json.dump(record, f, indent=2, sort_keys=True)
    except Exception:
        pass

    _truncate_if_stale(os.path.join(root, _TOOL_HISTORY_REL))

    last_warn = _last_skip_warning(os.path.join(root, _SKIP_LOG_REL))
    if last_warn:
        msg = (
            "[SessionStart] Most recent acknowledged-but-skipped entry: "
            + last_warn
            + " (full log at .claude/state/acknowledged-but-skipped.log)"
        )
        print(json.dumps({"systemMessage": msg}))

    return 0


if __name__ == "__main__":
    sys.exit(main())
