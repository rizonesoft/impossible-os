#!/usr/bin/env python3
# block-via: warning-only (PreToolUse reminder; never blocks -- checks-runner's
# own Bash calls pass through the same hook, so a BLOCK would deadlock it)
"""PreToolUse (Bash): checks-runner offload reminder for overnight SECTIONS.

When the overnight sequencer is in SECTIONS phase and the main loop runs a
bare verification script (build.sh / test.sh / test-smoke.sh / lint.sh /
test-tooling.sh) via Bash with no fresh checks-runner dispatch, inject a
systemMessage reminding it of the checks-runner route. Measured
(run-20260702-141810.log): 91 in-context build/test invocations, 0
checks-runner dispatches across 29 pipeline passes -- the skill text alone
does not hold overnight.

Freshness MUST be checks-runner-specific (fixed 2026-07-05): the original
check treated ANY recent Agent dispatch as cover, so a kernel-explorer or
parity-research-analyst call minutes earlier silenced the reminder while the
actual build/test still ran inline. Measured regression from that bug
(run-20260704-213648.log): 99 in-context build.sh/test.sh calls -- WORSE than
the 91-call baseline that motivated this hook in the first place.

Warning-only by design: the checks-runner subagent executes these same
scripts via Bash and its calls traverse this hook too; a BLOCK would break
the sanctioned path. Interactive sessions are exempt (single builds are
normal there; inline_churn_monitor covers sustained interactive fan-out).
Fail-open on any error.
"""
from __future__ import annotations

import json
import re
import sys
import time
from pathlib import Path

FRESH_NS = 900 * 1_000_000_000  # 15 min: a dispatch this recent counts

_SCRIPT_RE = re.compile(
    r"\bbash\s+scripts/(?:build|test|test-smoke|lint|test-tooling)\.sh\b")

_MSG = (
    "[checks-runner offload -- not a block] Overnight SECTIONS phase is "
    "running `{script}` in the MAIN context with no recent Agent dispatch. "
    "Doctrine (implement-todo-section step 15/16, review-todo-section "
    "build steps): route loop rebuilds and suite runs through "
    "Agent(subagent_type=\"checks-runner\") and quote the on-disk artifact "
    "tail yourself. Measured cost of ignoring this: 91 in-context "
    "build/test runs in the 2026-07-02 overnight run."
)


def _repo_root() -> Path | None:
    here = Path(__file__).resolve()
    for p in here.parents:
        if (p / ".claude").is_dir():
            return p
    return None


def _in_sections(root: Path) -> bool:
    try:
        st = json.loads(
            (root / ".claude" / "state" / "sequencer-run.json").read_text())
    except Exception:
        return False
    return bool(st.get("active")) and st.get("phase") == "SECTIONS"


def _recent_checks_runner_dispatch(root: Path) -> bool:
    try:
        st = json.loads(
            (root / ".claude" / "state" /
             "last-agent-dispatch.json").read_text())
    except Exception:
        return False
    by_type = st.get("by_type")
    entry = by_type.get("checks-runner") if isinstance(by_type, dict) else None
    ts = entry.get("timestamp_ns") if isinstance(entry, dict) else None
    return isinstance(ts, int) and (time.time_ns() - ts) <= FRESH_NS


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict) or d.get("tool_name") != "Bash":
        return 0
    cmd = (d.get("tool_input") or {}).get("command") or ""
    m = _SCRIPT_RE.search(cmd)
    if not m:
        return 0
    root = _repo_root()
    if root is None or not _in_sections(root):
        return 0
    if _recent_checks_runner_dispatch(root):
        return 0
    try:
        import _offload_log
        _offload_log.log_event(root, "fire", "build_offload_reminder", m.group(0))
    except Exception:
        pass
    print(json.dumps({"systemMessage": _MSG.format(script=m.group(0))}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
