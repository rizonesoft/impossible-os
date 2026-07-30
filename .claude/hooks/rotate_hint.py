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
# P1 STILL HOLDS, and it is the reason this flag stays False: `rollover-wip`
# requires `_unpushed_count() > 0`, and mid-section the runner is always either
# dirty (uncommitted) or fully pushed -- so re-enabling the hint alone would only
# produce hints that refuse, which is exactly what the retirement avoided.
#
# TO RE-ENABLE, all four must land together (the split predictor at
# `section-manifest.py:207` is NOT sufficient on its own: §34 had ALREADY been
# split and the remainder still ran 330 turns, because the driver was the review
# loop -- 23 findings -- which no item-count threshold can predict):
#   1. adopt periodic unpushed WIP commits mid-section (breaks P1)   -- NOT DONE
#   2. close arm-readiness A7 (range scan + fail-closed)             -- DONE 07-30
#   3. close arm-readiness A8 + A9 (review-resolved certification)   -- NOT DONE
#   4. flip this True and restore the SECTIONS-phase doctrine in the SKILL
ROTATE_HINT_ENABLED = False

# Turn-count proxy for the doctrine context band. Deliberately conservative: the
# hint is advisory, and P4.6 only acts at a safe boundary, so an early hint costs
# nothing. Tune via the canary (P4.4/P4.8).
ROTATE_HINT_TURNS = 140

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
    "rollover). At the NEXT WIP-clean boundary in THIS section -- work committed "
    "but NOT yet pushed (there must be local unshipped WIP; push is not required "
    "mid-section), any open Codex review fully resolved (findings triaged + fixed "
    "+ green, not merely received), no background job -- run `python3 "
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
    return 0


if __name__ == "__main__":
    sys.exit(main())
