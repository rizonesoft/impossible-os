#!/usr/bin/env python3
"""Block Edit / Write / MultiEdit until the latest Codex review has
been processed through `Skill(superpowers:receiving-code-review)`.

PreToolUse hook on `Edit` / `Write` / `MultiEdit`. Reads
`.claude/state/last-codex-review.json` (written by
`codex_review_completed.py`). Block decision:

  * No state file -> allow (no recent Codex trigger).
  * `received: true` -> allow (agent already triaged).
  * `received: false` AND `now - timestamp_ns > 3600s` -> allow
    (stale trigger from > 1h ago; agent has presumably moved on).
  * `received: false` AND TTL still valid -> BLOCK (exit 2) with
    the documented envelope naming the trigger and the opt-out.

Stale-pass policy:

  * TIME-based (1 hour TTL): YES. A stale `received: false` from
    yesterday should not block today.
  * NEW REVIEW: YES (implicit). Each new trigger overwrites the
    state with `received: false` + new timestamp.
  * HEAD / tree mismatch: NO. Codex design review High caught
    "stale-tree allow" semantics as a bypass: the agent could
    edit a non-code file (changing the tree) between trigger and
    receive, then the gate would see HEAD/tree mismatch and let
    the next code edit through without the receive ever firing.
    The block stays until the receive happens, period.

Opt-out:

  `RECEIVING_REVIEW_OVERRIDE=1` env var on the same call. Agent
  must set it explicitly per call AND state in their next message
  what code-evidence quote justifies skipping (e.g. "review found
  zero findings, output was 'no issues'"). Documented self-audit
  surface for the user.

Fail-open on malformed state / missing git / hook bug -- a hook
crash should never block tool calls.

Owner: TODO-08-automation-hardening section 3.
"""

import json
import os
import sys
import time
from pathlib import Path

TTL_SECONDS = 3600  # 1 hour stale-pass window


def _opt_out() -> bool:
    return os.environ.get("RECEIVING_REVIEW_OVERRIDE", "") == "1"


def _load_state(path: Path) -> dict | None:
    if not path.exists():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return None


def _ns_to_iso(ns: int) -> str:
    """Format a nanosecond timestamp as a UTC ISO-8601 string for
    the BLOCK envelope. Defensive against bad input."""
    try:
        from datetime import datetime, timezone
        return datetime.fromtimestamp(ns / 1e9, tz=timezone.utc).isoformat(timespec="seconds")
    except Exception:
        return f"ts_ns={ns}"


def _emit_block(state: dict, target_path: str) -> None:
    trigger = state.get("trigger") or "(unknown)"
    ts_ns = int(state.get("timestamp_ns") or 0)
    ts_iso = _ns_to_iso(ts_ns) if ts_ns else "(unknown time)"
    age_s = max(0, int((time.time_ns() - ts_ns) / 1e9)) if ts_ns else 0
    sys.stderr.write(
        f"[receiving-review-required] BLOCK -- Codex review at "
        f"{ts_iso} (trigger: {trigger}) has not been processed "
        f"through `Skill(superpowers:receiving-code-review)`.\n"
        f"[receiving-review-required] age: {age_s}s; TTL: "
        f"{TTL_SECONDS}s; target file: {target_path or '(unknown)'}\n"
        f"[receiving-review-required] policy: invoke "
        f"`Skill(name=\"superpowers:receiving-code-review\")` to "
        f"verify each finding at file:line and classify "
        f"Fix / Reject / Accept BEFORE editing. The receive must "
        f"happen even when the review reports zero findings -- the "
        f"act of receiving is the verification.\n"
        f"[receiving-review-required] opt-out: set "
        f"`RECEIVING_REVIEW_OVERRIDE=1` on the same call AND "
        f"state in your next message what code-evidence quote "
        f"justifies skipping (e.g. \"review found zero findings, "
        f"output was 'no issues'\").\n"
        f"[receiving-review-required] doctrine: CLAUDE.md "
        f"\"Mandatory Skill Triggers\" + memory "
        f"feedback_skill_invocation_drift / "
        f"feedback_never_skip_review.\n"
    )


def main() -> int:
    if _opt_out():
        return 0
    try:
        payload = json.load(sys.stdin)
    except Exception:
        return 0  # malformed -- fail open
    if payload.get("tool_name") not in ("Edit", "Write", "MultiEdit"):
        return 0
    # Hot-path optimization (review-pipeline perf H5): compute the
    # state path from this file's location instead of forking
    # `git rev-parse --show-toplevel`. The hook lives at
    # `<repo>/.claude/hooks/receiving_review_required.py`; the state
    # file is always at `<repo>/.claude/state/last-codex-review.json`.
    # Pre-fix Codex measurement: ~34ms median per Edit on the
    # no-state fast path (git subprocess dominated). Post-fix: skip
    # the fork entirely when the state file is absent.
    state_path = Path(__file__).resolve().parent.parent / "state" / "last-codex-review.json"
    if not state_path.exists():
        return 0  # no prior Codex trigger -- fast-path early return
    state = _load_state(state_path)
    if state is None:
        return 0  # malformed state file -- fail open
    if state.get("received") is True:
        return 0  # already triaged
    ts_ns = state.get("timestamp_ns")
    if not isinstance(ts_ns, int):
        return 0  # malformed state -- fail open
    age_s = (time.time_ns() - ts_ns) / 1e9
    if age_s > TTL_SECONDS:
        return 0  # stale trigger; allow
    target = (payload.get("tool_input") or {}).get("file_path", "")
    _emit_block(state, target)
    return 2


if __name__ == "__main__":
    sys.exit(main())
