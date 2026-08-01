#!/usr/bin/env python3
# block-via: none (PostToolUse; ADVISORY only -- never blocks, never changes flow)
"""PostToolUse: context-rotation hint (P4.1).

A turn-count PROXY for the ~200-250K-token doctrine band (a hook cannot read the
session's live context size, so tool-call count since the last rollover is the
practical proxy). In the overnight SECTIONS phase this counts tool events in
`.claude/state/rotate-hint.json` and, once the count crosses ROTATE_HINT_TURNS,
sets an ADVISORY `hint: true` flag. It NEVER blocks and NEVER changes flow -- it
only sets a flag. P4.6 (safe-boundary firing) decides IF/WHEN to act on the hint
at a WIP-clean boundary; run_phase_guard clears the file on a verified rollover so
each fresh worker counts from zero. Fail-open on any error (a hint that never
fires just means the size-trigger is unavailable, never a wedge).
"""
from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path

# B1 (Canary #2, 2026-07-14): the mid-section rotation is RETIRED as an active
# mechanism, on TWO stated premises. The A1-A9 verb + gate code stays in place,
# dormant and fail-safe; this flag keeps the hook inert so the runner is never
# nudged into a rotation that would only refuse.
#
#   (P1) the runner commits AND pushes atomically at ship, so there is no
#        committed-but-unpushed WIP window for `rollover-wip` to fire on;
#   (P2) a full ~2h/7-review section reached only ~132 tool-events, so 140 ~= one
#        section and the per-section full rollover already handles context hygiene.
#
# P2 IS FALSIFIED (measured 2026-07-30). Three consecutive segments of that day's
# canary ran 334, 420 and 768 tool-events -- 2.4x, 3x and 5.5x the threshold:
#
#     run-20260730-151719   334 tool-events   335 turns   $179.27   end-ctx 492K
#     run-20260730-190528   420 tool-events
#     run-20260730-111032   768 tool-events
#
# The 151719 segment shipped exactly ONE section (TODO-04 §34) and cost $179.27
# with cache-read at 85.3% of spend; `cost-summary.py` puts the same work split
# across 2-3 segments at 59-69% of that cache-read. So "140 ~= one section" was a
# property of Canary #2's section sizes, not of the runner, and the per-section
# rollover demonstrably does NOT bound context any more.
#
# P1 was NOT a property of the runner -- it was a property of the RETIRED
# doctrine, which never told the worker to make a mid-section WIP commit. The
# restored SECTIONS-phase procedure does exactly that (a LOCAL commit, deliberately
# unpushed), so the `_unpushed_count() > 0` precondition is now created rather than
# waited for.
#
# All four re-enable prerequisites are closed (2026-07-30). Note the split
# predictor at `section-manifest.py:207` is NOT a substitute for any of them: §34
# had ALREADY been split and the remainder still ran 330 turns, because the driver
# was the review loop (23 findings) -- knowable only AFTER the review runs.
#   1. periodic unpushed WIP commits mid-section  -- DONE (restored doctrine)
#   2. arm-readiness A7 (range scan + fail-closed) -- DONE
#   3. arm-readiness A8 + A9 (receipt certification + run-id binding) -- DONE
#   4. this flag + the SECTIONS-phase doctrine    -- DONE
ROTATE_HINT_ENABLED = True

# Countable SECTIONS-phase tool-events before the hint fires. RE-DERIVED
# 2026-07-31 from the FIRST live segment after the re-enable, which exposed an
# arithmetic error in the previous derivation.
#
# WHAT WENT WRONG AT 200. The old number came from "the ~300K context band lands
# at turn ~215", then used 215 as an EVENT threshold. But this hook counts only
# the matcher set (Bash|Read|Grep|Glob|Agent|Task) and only while phase ==
# SECTIONS -- measured on segment run-20260731-000502 that is 201 events across
# 308 turns, i.e. **0.65 countable events per turn**. So 200 events corresponds
# to turn ~307 of a 308-turn segment: the hint fired at event 201 of 201, one
# event before the section shipped. It was not dead -- it was correct and
# useless, with no runway left to reach a WIP boundary and rotate.
#
# THE DERIVATION, done in the right units this time. Same segment: start context
# ~117K, end 520,582, accumulation 1,311 tok/turn over 308 turns.
#     250K -> turn 102 (33% in) -> ~66 events
#     300K -> turn 140 (45% in) -> ~91 events
#     350K -> turn 178 (58% in) -> ~116 events
# 90 targets the ~300K band while leaving over half the segment as runway, which
# is the whole point: a hint with nowhere left to act is not a trigger.
#
# WHY AN EARLY HINT IS SAFE. It is advisory and P4.6 acts only at a WIP-clean
# boundary; a section that ships first simply rolls over normally and the ship
# rollover clears the counter. The cost of firing early is zero, the cost of
# firing late is the entire feature -- so the asymmetry says bias low.
ROTATE_HINT_TURNS = 90

# Once the hint is set, re-surface it every N further tool-events until a verified
# rollover clears the file -- the crossing-turn message can scroll far out of view
# before a WIP-clean boundary arrives, and the runner acts on what it can see.
ROTATE_RENUDGE_TURNS = 30

HINT_REL = ".claude/state/rotate-hint.json"

# Advisory surface for the P4.6 consumer (SKILL SECTIONS-phase doctrine). The flag
# file is invisible to the runner unless we say so; this bridges the two. It is a
# systemMessage only -- never blocks, never changes flow (matches
# inline_churn_monitor). The full mechanics live in the SKILL; this is the pointer.
_MSG = (
    "[sequencer] context-rotation hint set ({n} tool-events since the last "
    "rollover). CREATE the boundary -- do NOT wait for one. Finish the unit of "
    "work in hand, then at the FIRST green build/test point commit what you have "
    "LOCALLY and do NOT push (`git commit -m \"wip: ...\" -- <paths>`): that "
    "commit IS the precondition, and nothing in the normal flow produces one "
    "(measured 2026-07-31: this hint fired every segment and rollover-wip was "
    "attempted ZERO times, because every run commit is pushed). Then, with any "
    "open Codex review fully resolved (findings triaged + fixed "
    "+ green, not merely received) and no background job -- run `python3 "
    ".claude/hooks/run_phase_guard.py review-resolved` (records a green+bound "
    "receipt), then `... rollover-wip`. VERIFIED -> final-answer + "
    "END the turn (mid-section context rotation: the watchdog relaunches a fresh "
    "worker that resumes this SAME section from the enriched checkpoint). REFUSED "
    "-> READ the reason: a transient boundary (uncommitted WIP, a running job) is "
    "repaired at the next clean boundary, but a STRUCTURAL refusal (already pushed "
    "/ stale review binding / rollover_refused / phase != SECTIONS) will NOT "
    "change by retrying -- continue in-session and let the next ship -> full "
    "rollover handle rotation. Never force it, never abandon in-flight work."
)


def _repo_root() -> Path | None:
    """Root for this hook's state.

    An explicit `CLAUDE_HOOK_STATE_ROOT` wins so a TEST can be authoritative
    about its own fixture: the `__file__` walk below always lands on the real
    repo (this file lives there), which is how a "fixture" test drove the LIVE
    counter to 95 on 2026-07-31 while never touching its temp dir.
    """
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import _state_root
        forced = _state_root.override()
        if forced:
            return Path(forced)
    except Exception:
        pass
    here = Path(__file__).resolve()
    for p in here.parents:
        if (p / ".claude").is_dir():
            return p
    return None


def _in_sections(root: Path) -> bool:
    try:
        st = json.loads((root / ".claude/state/sequencer-run.json").read_text())
    except Exception:
        return False
    return bool(st.get("active")) and st.get("phase") == "SECTIONS"


def main() -> int:
    try:
        json.load(sys.stdin)  # consume payload; any tool event is one "turn"
    except Exception:
        return 0
    # B1: retired -> fully inert (no counting, no reminder). Dormant fail-safe.
    if not ROTATE_HINT_ENABLED:
        return 0
    # A2 (review 2026-07-14): count + emit ONLY inside the headless overnight run.
    # arm-sequencer.sh sets OVERNIGHT_SEQUENCER_RUN=1 in the unit environment; an
    # interactive operator session in the same checkout (e.g. the attended-canary
    # watcher) has it unset. Without this gate, that session's tool events inflate
    # the runner's shared counter AND it receives the rollover-wip systemMessage --
    # mirrors run_phase_guard.py `_in_overnight_run()` and agent_dispatch_required.
    if os.environ.get("OVERNIGHT_SEQUENCER_RUN") != "1":
        return 0
    root = _repo_root()
    if root is None or not _in_sections(root):
        return 0
    p = root / HINT_REL
    try:
        data = json.loads(p.read_text())
        if not isinstance(data, dict):
            data = {}
    except Exception:
        data = {}
    data["count"] = int(data.get("count", 0)) + 1
    newly_set = False
    if data["count"] >= ROTATE_HINT_TURNS and not data.get("hint"):
        data["hint"] = True
        data["hint_since_ts"] = time.time()
        data["hint_at_turns"] = data["count"]
        newly_set = True
    # A4 (review 2026-07-14): persist ATOMICALLY, then emit ONLY if it persisted.
    # (1) tmp + os.replace (matching run_phase_guard.save_state) so a crash
    # mid-write cannot leave a truncated file. (2) If the write fails, the
    # incremented count is in-memory only; emitting anyway would re-cross/re-emit
    # every event forever while rollover-wip (which reads the FILE) sees no hint
    # and refuses -- an emit/refuse loop. So fail-silent: no persisted transition,
    # no reminder. The lost-increment race across concurrent hooks is unreachable
    # (the A2 gate leaves only the single headless session incrementing, and its
    # PostToolUse hooks run serially -- a file lock would be dead weight).
    persisted = False
    try:
        p.parent.mkdir(parents=True, exist_ok=True)
        tmp = p.with_suffix(p.suffix + ".tmp")
        tmp.write_text(json.dumps(data))
        os.replace(tmp, p)
        persisted = True
    except Exception:
        persisted = False
    # Surface the flag on the crossing turn, then re-nudge every RENUDGE turns
    # until a verified rollover clears the file. A legacy file with hint:true but
    # no hint_at_turns yields since==0 -> no re-nudge (conservative, never spams).
    if persisted and data.get("hint"):
        since = data["count"] - int(data.get("hint_at_turns", data["count"]))
        if newly_set or (since > 0 and since % ROTATE_RENUDGE_TURNS == 0):
            try:
                print(json.dumps({"systemMessage": _MSG.format(n=data["count"])}))
            except Exception:
                pass
            # DURABLE RECORD (2026-08-01). A PostToolUse systemMessage reaches
            # the run's TRANSCRIPT but NOT the stream-json stdout that
            # stream-report.py parses, so the run log shows nothing: the
            # canary's first segment carried 8 hint occurrences in its
            # transcript and 0 in its log. Teaching stream-report to decode
            # `attachment` events was verified by REPLAYING transcript lines --
            # which proves it handles them if they arrive, not that they do.
            # Writing our own line removes the dependency on what the stream
            # happens to carry, and it survives the rollover that unlinks the
            # counter, so "did the rotation fire?" is a grep instead of a
            # forensic reconstruction.
            try:
                rec = root / ".claude/overnight/advisories.jsonl"
                rec.parent.mkdir(parents=True, exist_ok=True)
                with rec.open("a", encoding="utf-8") as fh:
                    fh.write(json.dumps({
                        "ts": time.time(), "hook": "rotate_hint",
                        "event": "context-rotation hint",
                        "count": data["count"],
                        "threshold": ROTATE_HINT_TURNS,
                    }) + "\n")
            except Exception:
                pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
