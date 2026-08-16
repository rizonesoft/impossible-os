#!/usr/bin/env python3
# block-via: exit 2 (headless overnight run only -- OVERNIGHT_SEQUENCER_RUN=1;
# interactive sessions are never gated)
"""PreToolUse (Bash): R3 -- one LONG verdict wait beats N short polls.

wait-for-codex-verdict.sh self-bounds at 100s so a BARE call survives the Bash
tool's ~120s default kill (B1), and asks the caller to re-invoke on exit 3.
Headless compliance with that contract is the single largest turn-count waste
measured in the 2026-07-19 digest: run-20260719-022200 issued 51 separate
poll calls (27% of its Bash calls) while a review converged -- every poll is a
full model turn re-reading a ~350K-token cached prefix (~18M cache-read tokens
of pure polling in one session).

R3 enforces the script's own "single LONG wait" escape hatch in the headless
run: a wait-for-codex-verdict.sh call must carry `--max >= {MIN_MAX_S}` AND a
Bash tool `timeout` that outlives it (max*1000 + {TIMEOUT_SLACK_MS}ms slack).
A compliant call sleeps up to 9 minutes inside ONE turn and still returns the
moment every verdict lands (the script exits early on completion), so a long
bound costs nothing when the review is already done.

Interactive sessions are exempt (the env discriminator is absent) -- a human
poking at a log with a quick bare call is fine. Fail-open on any error: a
broken guard must never wedge the run.
"""
from __future__ import annotations

import json
import os
import re
import sys

MIN_MAX_S = 300           # required --max floor (seconds)
TIMEOUT_SLACK_MS = 30_000  # tool timeout must exceed --max by this much
SUGGEST_MAX_S = 540        # suggested bound: fits the 600000ms tool ceiling

# Match an INVOCATION of the waiter, not a substring anywhere (v14 close-out,
# 2026-08-16). The old `\bwait-for-codex-verdict\.sh\b` fired on the script
# name appearing inside a `cat >> <capture-file>` heredoc body or a commit
# message, blocking a filing that merely DISCUSSED the waiter. An invocation
# has the script in command position: at the start of a pipeline segment,
# after a control operator, or as the argument of a runner (`bash`/`sh`/
# `timeout N`/`env`), optionally path-qualified. A name preceded by ordinary
# text (prose, an echo/cat argument) is a mention and no longer matches.
_WAIT_RE = re.compile(
    r"(?:^|[;&|(]|&&|\|\||\bbash\s+|\bsh\s+|\btimeout\s+\S+\s+|\benv\s+)"
    r"\s*(?:\S*/)?wait-for-codex-verdict\.sh\b")
_MAX_RE = re.compile(r"--max(?:=|\s+)(\d+)")

_MSG = (
    "[codex-wait-discipline BLOCK -- R3] Headless verdict waits must be ONE "
    "long call, not a poll loop (measured: 51 polls x ~350K cached tokens in "
    "run-20260719-022200). Re-issue with `--max {suggest}` AND the Bash tool "
    "parameter `timeout: {tool_ms}` (the tool's default ~120s kill would "
    "otherwise end the wait early -- B1). The script still returns the moment "
    "all verdicts land, so the long bound is free when the review is done. "
    "Got: --max={got_max}, timeout={got_timeout}."
)


def main() -> int:
    if os.environ.get("OVERNIGHT_SEQUENCER_RUN") != "1":
        return 0
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict) or d.get("tool_name") != "Bash":
        return 0
    ti = d.get("tool_input")
    if not isinstance(ti, dict):
        return 0
    cmd = str(ti.get("command") or "")
    if not _WAIT_RE.search(cmd):
        return 0
    m = _MAX_RE.search(cmd)
    max_s = int(m.group(1)) if m else 0
    timeout_ms = ti.get("timeout")
    timeout_ok = (isinstance(timeout_ms, (int, float))
                  and timeout_ms >= max_s * 1000 + TIMEOUT_SLACK_MS)
    if max_s >= MIN_MAX_S and timeout_ok:
        return 0
    sys.stderr.write(_MSG.format(
        suggest=SUGGEST_MAX_S,
        tool_ms=SUGGEST_MAX_S * 1000 + 60_000,
        got_max=max_s or "(none)",
        got_timeout=int(timeout_ms) if isinstance(timeout_ms, (int, float))
        else "(none)") + "\n")
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import _offload_log
        from pathlib import Path
        _offload_log.log_event(Path.cwd(), "fire", "codex_wait_discipline",
                               f"max={max_s} timeout={timeout_ms}")
    except Exception:
        pass
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:
        sys.exit(0)  # fail-open: a broken guard must never wedge the run
