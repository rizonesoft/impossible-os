#!/usr/bin/env python3
"""Stalled-progress backstop for Codex review rounds.

WHY count-of-rounds is the WRONG metric: a review that keeps surfacing NEW
findings round after round is doing its job -- a deep, escalating review that
finds genuine concurrency/integrity bugs late (round 14+) is more valuable
than one capped early on a fixed count. A hard round-count cap would ship
unfixed HIGH-severity bugs just because the review ran "too long".

So the backstop fires on **stalled progress**, not round count:
  - STOP when K consecutive rounds find NOTHING NEW (default K=3) -- a genuine
    loop (same finding repeating / no forward motion), not a deep escalating
    review.
  - Plus a very high COUNT CEILING (default 30) as a final infinite-loop guard.
A round that finds a NEW finding resets the stall streak to 0 -- the review
may run as long as it stays productive.

This complements, but does NOT replace, implement-todo-section step 13.5's
narrative re-adversarial-trigger rule (file-shape based re-review judgment
made in the moment). Step 13.5 is a point-in-time judgment call with no
memory of prior rounds; this hook gives the loop durable state that survives
context compaction and session boundaries, so a stalled review is caught even
if the narrative rule's context has been compacted away.

Per-slice state (resets on slice change): .claude/state/review-rounds holds
`<slice>\n<round_count>\n<no_progress_streak>`. SLICE id convention: use
"<todo-path>#<section>", e.g.
"todo/02-kernel-core/TODO-12-native-api-ssdt.md#8".

CLI:
  --bump SLICE --progress new|none   count a round. `new` = this round produced a
        NEW finding (resets the stall streak); `none` = no new finding (approve /
        repeat / nothing actionable -- increments the streak). Missing --progress
        defaults to `new` (conservative: never a false cap). Exit 0 = keep going,
        2 = CAPPED (stop the loop: spin unresolved findings to a follow-up + escalate).
  --status [SLICE]
  --reset
Env: OVERNIGHT_REVIEW_NOPROGRESS_CAP (default 3), OVERNIGHT_REVIEW_ROUND_CEILING
(default 30). Optional --state PATH (testability). Stdlib only.
"""
from __future__ import annotations

import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_STATE = ROOT / ".claude" / "state" / "review-rounds"


def _noprogress_cap() -> int:
    try:
        return max(1, int(os.environ.get("OVERNIGHT_REVIEW_NOPROGRESS_CAP", "3")))
    except ValueError:
        return 3


def _ceiling() -> int:
    try:
        return max(1, int(os.environ.get("OVERNIGHT_REVIEW_ROUND_CEILING", "30")))
    except ValueError:
        return 30


def _read(state: Path) -> tuple[str, int, int]:
    try:
        parts = state.read_text(encoding="utf-8").splitlines()
        return parts[0].strip(), int(parts[1]), int(parts[2])
    except (OSError, ValueError, IndexError):
        return "", 0, 0


def _write(state: Path, slice_id: str, count: int, streak: int) -> None:
    state.parent.mkdir(parents=True, exist_ok=True)
    state.write_text(f"{slice_id}\n{count}\n{streak}\n", encoding="utf-8")


def bump(state: Path, slice_id: str, progress_new: bool,
         noprogress_cap: int, ceiling: int) -> tuple[int, int, bool, str]:
    """Count a round. Returns (count, streak, capped, reason)."""
    prev_slice, prev_count, prev_streak = _read(state)
    if slice_id == prev_slice:
        count = prev_count + 1
        streak = 0 if progress_new else prev_streak + 1
    else:  # new slice -- fresh budget
        count = 1
        streak = 0 if progress_new else 1
    _write(state, slice_id, count, streak)
    if streak >= noprogress_cap:
        return count, streak, True, f"{streak} consecutive rounds with no new finding"
    if count >= ceiling:
        return count, streak, True, f"hit the {ceiling}-round ceiling"
    return count, streak, False, ""


def main(argv: list) -> int:
    if "--selftest" in argv:
        return _selftest()
    state = DEFAULT_STATE
    if "--state" in argv:
        state = Path(argv[argv.index("--state") + 1])
    noprogress_cap, ceiling = _noprogress_cap(), _ceiling()

    if "--reset" in argv:
        _write(state, "", 0, 0)
        print(f"review-round counter reset (no-progress cap {noprogress_cap}, ceiling {ceiling})")
        return 0
    if "--bump" in argv:
        i = argv.index("--bump")
        slice_id = argv[i + 1] if i + 1 < len(argv) and not argv[i + 1].startswith("--") else ""
        if not slice_id:
            print("usage: review_round_guard.py --bump SLICE --progress new|none", file=sys.stderr)
            return 1
        progress = "new"
        if "--progress" in argv:
            j = argv.index("--progress")
            progress = argv[j + 1] if j + 1 < len(argv) else "new"
        progress_new = progress.lower() != "none"
        count, streak, capped, reason = bump(state, slice_id, progress_new, noprogress_cap, ceiling)
        tag = f"CAPPED: {reason}" if capped else f"no-progress streak {streak}/{noprogress_cap}"
        print(f"review round {count} for {slice_id} ({tag}; ceiling {ceiling})")
        return 2 if capped else 0
    if "--status" in argv:
        s, c, st = _read(state)
        print(f"slice {s or '(none)'}: round {c}, no-progress streak {st}/{noprogress_cap}, ceiling {ceiling}")
        return 0
    print("usage: review_round_guard.py [--bump SLICE --progress new|none | --status | --reset] [--state PATH] [--selftest]", file=sys.stderr)
    return 1


def _selftest() -> int:
    import tempfile

    failures = []

    def check(name, cond):
        if not cond:
            failures.append(name)

    with tempfile.TemporaryDirectory() as td:
        st = Path(td) / "review-rounds"
        # a long PRODUCTIVE review never caps -- every round finds something new
        capped_any = False
        for _ in range(25):
            *_, capped, _r = bump(st, "TODO-01", True, 3, 30)
            capped_any = capped_any or capped
        check("productive-25-rounds-not-capped", not capped_any)
        # 3 consecutive no-new rounds -> CAPPED (stalled)
        st2 = Path(td) / "r2"
        bump(st2, "S", True, 3, 30)            # round 1: new
        _, s1, c1, _ = bump(st2, "S", False, 3, 30)  # streak 1
        _, s2, c2, _ = bump(st2, "S", False, 3, 30)  # streak 2
        _, s3, c3, r3 = bump(st2, "S", False, 3, 30) # streak 3 -> capped
        check("stall-1-not-capped", s1 == 1 and c1 is False)
        check("stall-2-not-capped", s2 == 2 and c2 is False)
        check("stall-3-capped", s3 == 3 and c3 is True and "no new finding" in r3)
        # a NEW finding resets the streak (escaping the stall)
        st3 = Path(td) / "r3"
        bump(st3, "S", False, 3, 30); bump(st3, "S", False, 3, 30)  # streak 2
        _, s, capped, _ = bump(st3, "S", True, 3, 30)               # new -> reset
        check("new-finding-resets", s == 0 and capped is False)
        # ceiling catches a pathological case even if 'new' keeps being claimed
        st4 = Path(td) / "r4"
        capped_at = None
        for i in range(40):
            cnt, _, capped, _ = bump(st4, "S", True, 3, 4)  # ceiling 4
            if capped and capped_at is None:
                capped_at = cnt
        check("ceiling-catches", capped_at == 4)
        # new slice resets
        st5 = Path(td) / "r5"
        bump(st5, "A", False, 3, 30); bump(st5, "A", False, 3, 30)
        cnt, s, capped, _ = bump(st5, "B", False, 3, 30)
        check("new-slice-resets", cnt == 1 and s == 1 and capped is False)

    if failures:
        print("selftest FAIL: " + ", ".join(failures), file=sys.stderr)
        return 1
    print("selftest OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
