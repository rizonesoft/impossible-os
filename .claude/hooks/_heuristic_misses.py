#!/usr/bin/env python3
# block-via: warning-only (HELPER module -- never blocks; emit_warn writes stderr only)
"""Shared helper for TODO-08 partial-enforcement WARN-first heuristic gates.

Each of the seven heuristic gates in §21 needs to do two things atomically:
  (1) emit a `systemMessage`-equivalent WARN to stderr, and
  (2) append one structured JSON line to `.claude/state/heuristic-misses.jsonl`.

Doing those side-by-side at every WARN site keeps the dataset that drives
the WARN -> ERROR promotion pipeline consistent: every line in the miss
log corresponds 1:1 with a WARN that was actually shown to the agent /
user. Decoupling the two (e.g. logging from `skill_step_observer.py`
while WARNing from `skill_step_block.py`) breaks the dataset because a
PostToolUse observer cannot see the staged diff or section-commit-gate
local decision. (Codex design review M1, 2026-04-29.)

Schema for `heuristic-misses.jsonl`:
    {
      "ts_ns": <int>,
      "step": <int>,           # implement-todo-section step id (1/2/3/9/15/17/18)
      "todo_path": <str|"">,
      "section": <int|null>,
      "signal": <str>,         # short tag identifying the heuristic miss
      "detail": <str>,         # human-readable one-line context
      "false_positive_user_flagged": false  # set retroactively via the
                                            # `scripts/heuristic-mark-false-positive.sh`
                                            # helper; defaults to false
    }

WARN format:
    [heuristic-step-<N>] WARN -- <detail>
                          (signal: <signal>; logged to .claude/state/heuristic-misses.jsonl)

The WARN is written to stderr unconditionally; PreToolUse hooks calling
this MUST exit 0 (heuristic WARNs are non-blocking by construction).
"""

import json
import os
import sys
import time
from typing import Optional


_MISS_LOG_REL = ".claude/state/heuristic-misses.jsonl"
_MISS_LOG_MAX_BYTES = 5 * 1024 * 1024   # 5 MiB -> rotate to .1


def _ts_ns() -> int:
    return time.time_ns() if hasattr(time, "time_ns") else int(time.time() * 1e9)


def emit_warn(
    repo_root: Optional[str],
    step: int,
    signal: str,
    detail: str,
    todo_path: str = "",
    section: Optional[int] = None,
) -> None:
    """Emit a WARN to stderr AND append one miss-log line.

    Best-effort -- never raises. If the repo root or state dir is
    unavailable, the WARN still gets written to stderr but the
    miss-log append is silently skipped.
    """
    text = (
        "[heuristic-step-" + str(step) + "] WARN -- " + (detail or "(no detail)") + "\n"
        "                          (signal: " + signal + "; logged to "
        + _MISS_LOG_REL + ")"
    )
    try:
        sys.stderr.write(text + "\n")
    except Exception:
        pass

    if not repo_root:
        return

    log_path = os.path.join(repo_root, _MISS_LOG_REL)
    rec = {
        "ts_ns": _ts_ns(),
        "step": step,
        "todo_path": todo_path or "",
        "section": section,
        "signal": signal,
        "detail": (detail or "")[:240],
        "false_positive_user_flagged": False,
    }
    try:
        os.makedirs(os.path.dirname(log_path), exist_ok=True)
        # Codex M2 fix 2026-04-29: serialize rotate+append under an
        # advisory flock so concurrent hooks racing through emit_warn
        # cannot interleave a rotation with a peer's append (which
        # silently dropped the peer's record from the live summary).
        # The FP marker takes the same lock; see
        # scripts/heuristic-mark-false-positive.sh.
        lock_path = log_path + ".lock"
        try:
            import fcntl as _fcntl
            have_flock = True
        except ImportError:
            _fcntl = None
            have_flock = False
        lock_fd = None
        try:
            if have_flock:
                lock_fd = os.open(lock_path,
                                   os.O_RDWR | os.O_CREAT, 0o600)
                _fcntl.flock(lock_fd, _fcntl.LOCK_EX)
            try:
                if os.path.exists(log_path) and \
                   os.path.getsize(log_path) >= _MISS_LOG_MAX_BYTES:
                    os.replace(log_path, log_path + ".1")
            except Exception:
                pass
            with open(log_path, "a", encoding="utf-8") as f:
                f.write(json.dumps(rec, separators=(",", ":")) + "\n")
        finally:
            if lock_fd is not None:
                try:
                    if have_flock:
                        _fcntl.flock(lock_fd, _fcntl.LOCK_UN)
                except Exception:
                    pass
                try:
                    os.close(lock_fd)
                except Exception:
                    pass
    except Exception:
        pass


def parse_active_todo_section(state_entry: dict):
    """Best-effort extraction of (todo_path, section_id) from a
    skill-progress.json entry. Returns ("", None) when unknown.
    Used so heuristic misses can be attributed to the right
    section for ratio reporting."""
    if not isinstance(state_entry, dict):
        return ("", None)
    todo_path = state_entry.get("todo_path") or state_entry.get("path") or ""
    section = state_entry.get("section")
    if not isinstance(todo_path, str):
        todo_path = ""
    if not isinstance(section, int):
        section = None
    return (todo_path, section)
