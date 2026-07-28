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
import os
import re
import sys
import time
from pathlib import Path

FRESH_NS = 900 * 1_000_000_000  # 15 min: a dispatch this recent counts

# J2b: `lint.sh` is intentionally NOT here -- lint is cheap and does not flood
# the context like a full build/test/smoke run, so BLOCKing it to force the
# run-artifact.sh reroute was mild over-reach. Only the output-heavy scripts route.
#
# R2 (2026-07-19): bypass-shape hardening. The original pattern matched ONLY
# the literal `bash scripts/X.sh` form; absolute paths, `./`-prefix, direct
# execution at a command position, `make test-*`, interpreter flags
# (`bash -x`), quoted paths, env-var prefixes (`FOO=1 scripts/test.sh`),
# subshells, and newline-separated statements all sailed through raw
# (review 2026-07-19, finders A/B/altitude -- each shape verified live).
# Three separate patterns so each detector stays auditable:
#   - interpreter form: bash/sh (+ optional flags/quote) directly before the
#     script path, anywhere in the command (so `cat scripts/test.sh` stays
#     un-gated -- no interpreter);
#   - direct-exec form: the script path at a COMMAND position (start of
#     string/line or after ; & | ( ), optionally behind env-var assignments,
#     so a quoted mention mid-string does not trip the block;
#   - make form: a `test`/`test-<suite>` target at a command position
#     ((?!=) keeps `make test=1` variable assignments out).
_SUITE_SCRIPTS = r"scripts/(?:build|test|test-smoke|test-tooling)\.sh\b"
_CMD_POS = r"(?:^|[;&|(]\s*)"
_ENV_PREFIX = r"(?:[A-Za-z_][A-Za-z0-9_]*=\S*\s+)*"
_SCRIPT_RES = (
    # LOOKBEHIND, not \b (2026-07-28). `\b` is satisfied by the DOT inside any
    # `*.sh` filename -- `.` is a non-word char, `s` is a word char -- so
    # `scripts/build.sh scripts/test.sh` contained the substring
    # `sh scripts/test.sh` and a READ-ONLY `grep`/`ls` naming two suite scripts
    # was BLOCKED as if it were invoking one. Hit live by
    # `grep -n "..." scripts/build.sh scripts/test.sh`, where the reroute
    # advice ("re-issue through run-artifact.sh") is meaningless for a grep, so
    # the only exits were rephrasing or giving up.
    # `(?<![.\w])` keeps every real interpreter form matching -- `bash x`,
    # `sh x`, `/bin/bash x`, `time bash x` -- while refusing to treat a
    # filename's tail as the shell. A plain _CMD_POS anchor would be WRONG
    # here: it would stop matching `/bin/bash ...` and `time bash ...`.
    re.compile(r"(?<![.\w])(?:bash|sh)\s+(?:-\S+\s+)*[\"']?(?:\S*/)?" + _SUITE_SCRIPTS),
    re.compile(_CMD_POS + _ENV_PREFIX + r"[\"']?(?:\./|\S*/)?" + _SUITE_SCRIPTS,
               re.MULTILINE),
    re.compile(_CMD_POS + _ENV_PREFIX
               + r"make\s+(?:\S+\s+)*?test(?:-[A-Za-z0-9_-]+)?\b(?!=)",
               re.MULTILINE),
)


def _match_suite_invocation(cmd: str):
    """First match of any suite-script/make-test invocation shape, else None."""
    for rx in _SCRIPT_RES:
        m = rx.search(cmd)
        if m:
            return m
    return None

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


def _is_headless_run() -> bool:
    """True only inside the UNATTENDED run this gate governs.

    SESSION SCOPING (2026-07-28). `_in_sections()` reads the GLOBAL cursor and
    says nothing about who is issuing the command, so while a run sat in
    SECTIONS every interactive operator session in the repo inherited its phase
    gates -- including the BLOCK above, whose entire rationale ("91 in-context
    build/test runs floods the overnight context") is about the RUN's context
    budget, not an operator's. Observed live: an operator's read-only grep was
    blocked by a gate meant for the runner.

    The discriminator already exists and is used elsewhere for exactly this
    reason -- `run_phase_guard.handle_stop()` returns early on it so "an
    interactive operator session is never trapped". `OVERNIGHT_SEQUENCER_RUN=1`
    is set by the arm drop-in on the systemd unit and is never present in an
    operator shell.

    Deliberately the SAME single test as `run_phase_guard.is_headless()` and
    nothing more. A second, cleverer heuristic here would be a second answer to
    "is this the run?", and the two would drift.
    """
    return os.environ.get("OVERNIGHT_SEQUENCER_RUN") == "1"


def _in_sections(root: Path) -> bool:
    # Both conditions are required: the RUN must be in SECTIONS *and* this
    # session must BE the run. Either alone is not the gate's subject.
    if not _is_headless_run():
        return False
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
    m = _match_suite_invocation(cmd)
    if not m:
        return 0
    root = _repo_root()
    if root is None or not _in_sections(root):
        return 0
    # Exempt the SANCTIONED wrapped route: `run-artifact.sh <label> -- bash
    # scripts/build.sh` legitimately contains the inner `bash scripts/build.sh`
    # that _SCRIPT_RE matches. Firing on it would nag (and, once promoted to a
    # BLOCK, would BLOCK) the very route this reminder recommends -- it fired 9x
    # on one properly-wrapped sequence (Codex audit 2026-07-13, verified). If an
    # outer run-artifact.sh wrapper is present, the command is already offloaded.
    # R2: record the reroute as a `follow` event -- offload-report.py counted
    # only Agent dispatches as compliance, so the P3.4 reroute (a Bash call, not
    # an Agent) read as a 9% follow rate when the block was in fact working
    # (2026-07-19 measurement: all bare attempts blocked pre-execution).
    if re.search(r"\brun-artifact\.sh\b", cmd):
        try:
            import _offload_log
            _offload_log.log_event(root, "follow", "build_offload_reminder",
                                   m.group(0))
        except Exception:
            pass
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
