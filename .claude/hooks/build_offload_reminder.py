#!/usr/bin/env python3
# block-via: exit 2 (P3.4: overnight SECTIONS phase only -- BLOCK-with-reroute a
# BARE build/test/smoke/lint script into the deterministic run-artifact.sh wrapper;
# the outer-run-artifact.sh-wrapped form is exempt (prereq 1) so the sanctioned
# path never deadlocks. Interactive sessions are never gated.)
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

P3.4 (2026-07-14): promoted from WARN to BLOCK-with-reroute. The old
"warning-only" rationale (the checks-runner subagent's own Bash traversed
this hook) is obsolete -- checks-runner is deprecated for green mechanics and
the OUTER run-artifact.sh wrapper is now exempt (prereq 1), so blocking a BARE
build/test can never deadlock the sanctioned wrapped path. A bare script in the
SECTIONS phase now returns exit 2 with the reroute message; the model re-issues
via `run-artifact.sh`. Interactive sessions are exempt (single builds are normal
there; inline_churn_monitor covers sustained interactive fan-out). Fail-open on
any error -- a hook error must never wedge the run.
"""
from __future__ import annotations

import json
import re
import sys
import time
from pathlib import Path

FRESH_NS = 900 * 1_000_000_000  # 15 min: a dispatch this recent counts

# J2b: `lint.sh` is intentionally NOT here -- lint is cheap and does not flood
# the context like a full build/test/smoke run, so BLOCKing it to force the
# run-artifact.sh reroute was mild over-reach. Only the output-heavy scripts route.
_SCRIPT_RE = re.compile(
    r"\bbash\s+scripts/(?:build|test|test-smoke|test-tooling)\.sh\b")

_MSG = (
    "[build-offload BLOCK -- reroute] Overnight SECTIONS phase ran `{script}` "
    "BARE in the MAIN context. Re-issue it through the DETERMINISTIC "
    "wrapper (NO model): `bash scripts/overnight/run-artifact.sh <label> -- "
    "{script}` returns a compact JSON envelope (verdict + error lines + "
    "artifact path) and tees full output to disk; you quote the tail "
    "yourself. A green run spends ZERO Sonnet -- dispatch diagnostic-digester "
    "ONLY on a FAIL envelope. Measured cost of the bare path: 91 in-context "
    "build/test runs floods the 2026-07-02 overnight context."
)

# P3.4 prereq 3: suppress a repeat reminder for the SAME script within this
# window -- the reminder fired 9x on one properly-wrapped build/test sequence
# before the exemption, and duplicate nags add churn without new signal.
_DEDUP_WINDOW_S = 300


def _recently_fired(root: Path, script: str) -> bool:
    """True if build_offload_reminder already fired for `script` within
    _DEDUP_WINDOW_S. Reads the shared offload-events.jsonl (newest first)."""
    try:
        p = root / ".claude" / "state" / "offload-events.jsonl"
        if not p.exists():
            return False
        now = time.time()
        for line in reversed(p.read_text(encoding="utf-8").splitlines()):
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except Exception:
                continue
            if (rec.get("hook") == "build_offload_reminder"
                    and rec.get("kind") == "fire"
                    and rec.get("detail") == script):
                ts = rec.get("ts")
                return isinstance(ts, (int, float)) and (now - ts) <= _DEDUP_WINDOW_S
        return False
    except Exception:
        return False


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
    # Exempt the SANCTIONED wrapped route: `run-artifact.sh <label> -- bash
    # scripts/build.sh` legitimately contains the inner `bash scripts/build.sh`
    # that _SCRIPT_RE matches. Firing on it would nag (and, once promoted to a
    # BLOCK, would BLOCK) the very route this reminder recommends -- it fired 9x
    # on one properly-wrapped sequence (Codex audit 2026-07-13, verified). If an
    # outer run-artifact.sh wrapper is present, the command is already offloaded.
    if re.search(r"\brun-artifact\.sh\b", cmd):
        return 0
    root = _repo_root()
    if root is None or not _in_sections(root):
        return 0
    if _recent_checks_runner_dispatch(root):
        return 0
    # P3.4: BLOCK-with-reroute. Log once per window (dedup) but ALWAYS block --
    # a repeat bare attempt must keep being rerouted, so the dedup gates only the
    # offload-events log write, never the block decision.
    if not _recently_fired(root, m.group(0)):
        try:
            import _offload_log
            _offload_log.log_event(root, "fire", "build_offload_reminder", m.group(0))
        except Exception:
            pass
    sys.stderr.write(_MSG.format(script=m.group(0)) + "\n")
    return 2


if __name__ == "__main__":
    sys.exit(main())
