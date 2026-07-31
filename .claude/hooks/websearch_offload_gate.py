#!/usr/bin/env python3
# block-via: exit 2 (headless overnight run only -- OVERNIGHT_SEQUENCER_RUN=1;
# researcher SUBAGENTS and interactive sessions are never gated)
"""PreToolUse (WebSearch/WebFetch): R4 -- main-session web research reroute.

The three researcher agents (parity-research-analyst, spec-research-analyst,
web-research-analyst) exist so multi-page web reads land in a throwaway
context. The 2026-07-19 digest caught the headless MAIN session firing 10
WebSearch calls in a 36-second burst (run-20260719-100809 log:75-84) IN
PARALLEL with a parity-research-analyst dispatch doing the same research --
paying for 10 raw search-result payloads on top of the agent's bounded report,
with every subsequent turn re-reading them in the cached prefix.

R4 blocks main-session WebSearch/WebFetch in the headless run and reroutes to
the owning researcher. Subagent calls pass untouched (the researchers DO the
searching -- keyed on the agent transcript path, same discriminator as
runner_bash_guard), so the reroute can never deadlock. Interactive sessions
are exempt (env discriminator absent). Fail-open on any error.
"""
from __future__ import annotations

import json
import os
import sys

_MSG = (
    "[websearch-offload BLOCK -- R4] Headless main-session {tool} is a "
    "researcher-agent task: dispatch parity-research-analyst (Win11/Linux "
    "feature parity), spec-research-analyst (normative hardware/format specs), "
    "or web-research-analyst (toolchain/emulator/host/CI -- everything else) "
    "and consume its bounded report instead of raw search payloads (measured: "
    "10 main-session WebSearch calls ran in parallel with a researcher "
    "dispatch on the same question, run-20260719-100809). Subagent calls pass "
    "untouched."
)


def _is_subagent_caller(d: dict) -> bool:
    """Positive subagent identity from the payload (2026-07-31 fix).

    Was keyed on `transcript_path` alone, which a subagent payload fills with
    the PARENT session's transcript -- so every researcher dispatch read as
    main-session and was blocked by the rule that exists to route work TO it
    (measured 3 consecutive sections; run-20260731-095007 log:218-240).
    """
    path = str(d.get("transcript_path") or "")
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import runner_bash_guard
        return runner_bash_guard.is_subagent_payload(d)
    except Exception:
        return _is_subagent_transcript(path)


def _is_subagent_transcript(path: str) -> bool:
    # Single source of truth: runner_bash_guard's battle-tested detector
    # (review 2026-07-19 reuse finding -- a copy here would silently diverge
    # the next time the transcript-naming heuristic is hardened). Fallback
    # only if the import itself breaks: treat as MAIN session, which fails
    # toward the reroute message, never toward silently skipping the gate.
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import runner_bash_guard
        return runner_bash_guard._is_subagent_transcript(path)
    except Exception:
        if not path:
            return False
        base = os.path.basename(path)
        return base.startswith("agent-") or "/subagents/" in path


def main() -> int:
    if os.environ.get("OVERNIGHT_SEQUENCER_RUN") != "1":
        return 0
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict):
        return 0
    tool = d.get("tool_name")
    if tool not in ("WebSearch", "WebFetch"):
        return 0
    if _is_subagent_caller(d):
        return 0
    sys.stderr.write(_MSG.format(tool=tool) + "\n")
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import _offload_log
        from pathlib import Path
        _offload_log.log_event(Path.cwd(), "fire", "websearch_offload_gate",
                               str(tool))
    except Exception:
        pass
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:
        sys.exit(0)  # fail-open: a broken guard must never wedge the run
