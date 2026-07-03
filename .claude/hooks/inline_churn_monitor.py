#!/usr/bin/env python3
# block-via: warning-only (PostToolUse reminder; never blocks)
"""PostToolUse: inline-churn monitor (interactive + overnight).

Counts consecutive main-session legwork ops (Bash / Read / Grep / Glob)
since the last Agent dispatch and injects a systemMessage reminder at a
threshold, naming the offload routes. Complements interactive_offload_router
(prompt-time) by catching MID-TASK drift into in-context fan-out -- the
measured leak: the 2026-07-02 overnight run absorbed ~290 context-heavy ops
per hard section with ~2% offload; the 2026-07-03 interactive session that
BUILT the offload infrastructure still ran 250 Bash (170 grep-shaped) vs 8
dispatches. Warning-only: trust-contract verification reads and genuinely
small tasks are legitimate inline work; the reminder recurs every WINDOW
ops, it never blocks.

State: .claude/state/inline-churn.json {count, last_warn_count}. Reset by
any Agent/Task dispatch (agent_dispatch_recorder-adjacent but independent:
this hook resets itself on Agent PostToolUse since it is registered for
that tool too). Fail-open on any error.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

THRESHOLD = 25   # first reminder after this many inline ops
WINDOW = 25      # re-remind every WINDOW ops after that

_COUNTED = {"Bash", "Read", "Grep", "Glob"}
_RESET = {"Agent", "Task"}

_MSG = (
    "[inline-churn -- not a block] {n} inline Bash/Read/Grep/Glob ops since "
    "the last Agent dispatch. If this is fan-out exploration or verification, "
    "offload it: repo/code search -> Explore or kernel-explorer; "
    "build/test/lint runs -> checks-runner; TODO structure -> "
    "todo-validation-mapper; git archaeology -> git-historian; gh state -> "
    "gh-query-runner; log analysis -> serial-log-auditor / "
    "overnight-log-explorer. Trust-contract spot-checks and genuinely small "
    "tasks are fine inline; sustained fan-out is not. Doctrine: CLAUDE.md "
    "\"Interactive offload\" + memory feedback_interactive_agent_offload."
)


def _state_path() -> Path:
    return Path(__file__).resolve().parent.parent / "state" / "inline-churn.json"


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict):
        return 0
    tool = d.get("tool_name") or ""

    sp = _state_path()
    try:
        st = json.loads(sp.read_text())
        if not isinstance(st, dict):
            st = {}
    except Exception:
        st = {}
    count = st.get("count") if isinstance(st.get("count"), int) else 0
    last_warn = (st.get("last_warn_count")
                 if isinstance(st.get("last_warn_count"), int) else 0)

    if tool in _RESET:
        count, last_warn = 0, 0
    elif tool in _COUNTED:
        count += 1
    else:
        return 0

    warn = count >= THRESHOLD and (count - last_warn) >= WINDOW
    if warn:
        last_warn = count

    try:
        sp.parent.mkdir(parents=True, exist_ok=True)
        sp.write_text(json.dumps(
            {"count": count, "last_warn_count": last_warn}))
    except Exception:
        pass

    if warn:
        print(json.dumps({"systemMessage": _MSG.format(n=count)}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
