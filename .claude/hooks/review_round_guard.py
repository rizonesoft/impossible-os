#!/usr/bin/env python3
"""Stalled-progress backstop for Codex review rounds.

WHY count-of-rounds is the WRONG metric: a review that keeps surfacing NEW
findings round after round is doing its job -- a deep, escalating review that
finds genuine concurrency/integrity bugs late (round 14+) is more valuable
than one capped early on a fixed count. A hard round-count cap would ship
unfixed HIGH-severity bugs just because the review ran "too long".

Since 2026-11-20 (owner decision) the pin names no `no_progress_cap`: a repair chain runs until it is clean
or reaches `pass_cap`, and every reconciliation reviews the newest repair adversarially, so a stall rule is no
longer needed to stop a review that re-attacks the whole section. A pin that names a cap gets the rules below.
When it does, the backstop fires on **stalled progress**, not round count. What counts as progress depends on
the section (operator, 2026-11-19):
  - A section touching one of the pin's `deep_paths` (boot, kernel, file operations): a round that finds anything NEW is
    progress, and K consecutive rounds with nothing new STOP the loop (`no_progress_cap`).
  - Every other section: a round is progress only when its worst finding (`--highest`, the reply's
    HIGHEST line) is less severe than the last round's. A review that finds a fresh edge case of the
    same severity on every fix is not converging, and K such rounds in a row STOP the loop.
  - Plus a very high COUNT CEILING as a final infinite-loop guard: `pass_cap`
    in .conclave/reviewers.toml (33 since 2026-11-19, the one cap for all code; it counts every
    returned pass of a section).
A round that finds a NEW finding resets the stall streak to 0 -- the review
may run as long as it stays productive.

This complements, but does NOT replace, implement-todo-section step 13.5's
narrative re-adversarial-trigger rule (file-shape based re-review judgment
made in the moment). Step 13.5 is a point-in-time judgment call with no
memory of prior rounds; this hook gives the loop durable state that survives
context compaction and session boundaries, so a stalled review is caught even
if the narrative rule's context has been compacted away.

Per-slice state (resets on slice change): .claude/state/review-rounds holds
`<slice>\n<round_count>\n<no_progress_streak>\n<last_highest>`. SLICE id convention: use
"<todo-path>#<section>", e.g.
"todo/02-kernel-core/TODO-12-native-api-ssdt.md#8".

CLI:
  --bump SLICE --highest P1|P2|P3|none [--progress new|none] [--base REV | --paths-file FILE]
        count a round. --highest is the reply's HIGHEST line; without it (a legacy
        dispatch) the new-finding rule counts the round. --progress is read only under
        that rule: `new` = this round produced a NEW finding (resets the stall streak),
        `none` = nothing new; missing, it is `new`. The section's files are FILE (one
        repository path per line) or REV..HEAD plus the working tree, REV being --base
        or $REVIEW_BASE (the dispatch's base); with neither, or when git cannot say,
        the section is taken as deep (the earlier, longer rule). Exit 0 = keep going,
        2 = CAPPED (stop the loop: spin unresolved findings to a follow-up + escalate).
  --status [SLICE]
  --reset
Both numbers come from the reviewer pin (.conclave/reviewers.toml: pass_cap and
no_progress_cap). Env OVERNIGHT_REVIEW_NOPROGRESS_CAP and
OVERNIGHT_REVIEW_ROUND_CEILING override them for one call. A pin that cannot be
read leaves the built-in 5 and 33, takes every section as deep, and says so on stderr: the backstop must keep
working when the pin is broken. Optional --state PATH and --pin PATH (testability). Stdlib only.
"""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_STATE = ROOT / ".claude" / "state" / "review-rounds"


BUILTIN_NOPROGRESS_CAP = 5
BUILTIN_CEILING = 33
# Most to least severe; a round is progress when its worst finding sits later in this order than the last.
SEVERITY_ORDER = ("P1", "P2", "P3", "none")
ALL_DEEP = None  # deep_paths when the pin cannot be read: every section keeps the longer rule
DEEP = "deep"  # the state's rule marker for a round counted by the new-finding rule


def _pin_caps(pin_path: Path | None = None) -> tuple[int | None, int]:
    """(no_progress_cap, pass_cap) from the reviewer pin, or the built-in pair when it cannot be read.
    no_progress_cap is None when the pin names none: nothing stalls, pass_cap still ends the loop."""
    return _pin(pin_path)[:2]


def _pin(pin_path: Path | None = None):
    """(no_progress_cap, pass_cap, deep_paths) from the reviewer pin; the built-ins and ALL_DEEP without it."""
    scripts = str(ROOT / "scripts")
    if scripts not in sys.path:
        sys.path.insert(0, scripts)
    try:
        import review_pin
        pin = review_pin.load_pin(pin_path) if pin_path else review_pin.load_pin()
        return pin["no_progress_cap"], pin["pass_cap"], pin["deep_paths"]
    except Exception as exc:  # noqa: BLE001 -- any failure to read the pin keeps the backstop alive
        sys.stderr.write(f"[review-round-guard] the reviewer pin could not be read ({exc}); "
                         f"using {BUILTIN_NOPROGRESS_CAP} and {BUILTIN_CEILING}, every section deep\n")
        return BUILTIN_NOPROGRESS_CAP, BUILTIN_CEILING, ALL_DEEP


def _env_int(name: str, default: int) -> int:
    try:
        return max(1, int(os.environ.get(name, "")))
    except ValueError:
        return default


def _noprogress_cap(pin_path: Path | None = None) -> int | None:
    pinned = _pin_caps(pin_path)[0]
    if pinned is None and not os.environ.get("OVERNIGHT_REVIEW_NOPROGRESS_CAP", "").strip():
        return None
    return _env_int("OVERNIGHT_REVIEW_NOPROGRESS_CAP", pinned if pinned is not None else BUILTIN_NOPROGRESS_CAP)


def _ceiling(pin_path: Path | None = None) -> int:
    return _env_int("OVERNIGHT_REVIEW_ROUND_CEILING", _pin_caps(pin_path)[1])


def _read(state: Path) -> tuple[str, int, int, str]:
    """(slice, rounds, streak, last highest); a state from before 2026-11-19 has no highest, read as ""."""
    try:
        parts = state.read_text(encoding="utf-8").splitlines()
        last = parts[3].strip() if len(parts) > 3 else ""
        return parts[0].strip(), int(parts[1]), int(parts[2]), last if last in (*SEVERITY_ORDER, DEEP) else ""
    except (OSError, ValueError, IndexError):
        return "", 0, 0, ""


def _write(state: Path, slice_id: str, count: int, streak: int, highest: str = "") -> None:
    state.parent.mkdir(parents=True, exist_ok=True)
    state.write_text(f"{slice_id}\n{count}\n{streak}\n{highest}\n", encoding="utf-8")


def _git_paths(*args):
    """Paths git lists for args (NUL-delimited, so no path is quoted), or None when git fails, so the caller
    cannot mistake a failure for no paths."""
    try:
        r = subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return None
    if r.returncode != 0:
        return None
    try:
        return [p for p in r.stdout.decode("utf-8").split("\0") if p]
    except UnicodeDecodeError:
        return None


def _normal(path: str) -> str:
    """A repository-relative path as the pin spells its prefixes: forward slashes, no leading ./"""
    path = path.strip().replace("\\", "/")
    while path.startswith("./"):
        path = path[2:]
    return path


def section_paths(paths_file: Path | None = None, base: str | None = None):
    """The section's files, or None when they are unknown: paths_file's lines, else the section's range
    base..HEAD (base is the REVIEW_BASE the dispatch uses) plus the working tree, renames counted on both
    sides. Without either there is no way to tell the section's files from another's, so they are unknown."""
    if paths_file is not None:
        try:
            listed = paths_file.read_text(encoding="utf-8").splitlines()
        except (OSError, ValueError):
            return None
        return [_normal(p) for p in listed if p.strip()]
    if not base:
        return None
    parts = [_git_paths("diff", "--name-only", "-z", "--no-renames", f"{base}..HEAD"),
             _git_paths("diff", "--name-only", "-z", "--no-renames", "HEAD"),
             _git_paths("ls-files", "-z", "--others", "--exclude-standard")]
    return None if any(p is None for p in parts) else sorted({_normal(p) for part in parts for p in part})


def is_deep(paths, deep_paths) -> bool:
    """True when the section keeps the new-finding rule: it touches a deep prefix, or either list is
    unknown (None), which keeps the longer rule rather than cutting a kernel review short."""
    if paths is None or deep_paths is None:
        return True
    return any(p.startswith(prefix) for p in paths for prefix in deep_paths)


def bump(state: Path, slice_id: str, progress_new: bool,
         noprogress_cap: int | None, ceiling: int, highest: str | None = None) -> tuple[int, int, bool, str]:
    """Count a round. Returns (count, streak, capped, reason).

    highest None: the new-finding rule (a deep section), progress_new decides. Otherwise the severity rule:
    the slice's first round, or one whose worst finding is less severe than the last round's, is progress;
    a last round of unknown severity (a state written before the rule) counts as progress, never a false cap.
    """
    prev_slice, prev_count, prev_streak, prev_mark = _read(state)
    same = slice_id == prev_slice
    mark = DEEP if highest is None else highest
    # A streak counted under the other rule (or a state from before the rules) says nothing about this one.
    switched = same and (prev_mark == "" or (prev_mark == DEEP) != (mark == DEEP))
    if highest is None:
        moved = progress_new
    else:
        moved = not same or switched or SEVERITY_ORDER.index(highest) > SEVERITY_ORDER.index(prev_mark)
    count = prev_count + 1 if same else 1  # a new slice gets a fresh budget
    base = 0 if switched else prev_streak
    streak = 0 if moved else (base + 1 if same else 1)
    _write(state, slice_id, count, streak, mark)
    if noprogress_cap is not None and streak >= noprogress_cap:
        what = "no new finding" if highest is None else "a worst finding no less severe than the last"
        return count, streak, True, f"{streak} consecutive rounds with {what}"
    if count >= ceiling:
        return count, streak, True, f"hit the {ceiling}-round ceiling"
    return count, streak, False, ""


def main(argv: list) -> int:
    if "--selftest" in argv:
        return _selftest()
    state = DEFAULT_STATE
    if "--state" in argv:
        state = Path(argv[argv.index("--state") + 1])
    pin_path = Path(argv[argv.index("--pin") + 1]) if "--pin" in argv else None
    noprogress_cap, ceiling = _noprogress_cap(pin_path), _ceiling(pin_path)
    deep_paths = _pin(pin_path)[2]

    if "--reset" in argv:
        _write(state, "", 0, 0, "")
        print(f"review-round counter reset (no-progress cap {noprogress_cap or 'none'}, ceiling {ceiling})")
        return 0
    if "--bump" in argv:
        i = argv.index("--bump")
        slice_id = argv[i + 1] if i + 1 < len(argv) and not argv[i + 1].startswith("--") else ""
        values = {name: _arg(argv, name) for name in ("--highest", "--progress", "--paths-file", "--base")}
        bad = [name for name, value in values.items() if name in argv and not value]
        highest = values["--highest"]
        if not slice_id or bad or (highest is not None and highest not in SEVERITY_ORDER):
            print("usage: review_round_guard.py --bump SLICE --highest P1|P2|P3|none [--progress new|none] "
                  "[--base REVIEW_BASE | --paths-file FILE]  (--highest is the reply's HIGHEST line)"
                  + (f"; {', '.join(bad)} needs a value" if bad else ""), file=sys.stderr)
            return 1
        progress_new = (values["--progress"] or "new").lower() != "none"
        listed = values["--paths-file"]
        base = values["--base"] or os.environ.get("REVIEW_BASE", "").strip() or None
        # No HIGHEST (a legacy dispatch that asks for none) leaves only the new-finding rule to count it.
        deep = highest is None or is_deep(section_paths(Path(listed) if listed else None, base), deep_paths)
        count, streak, capped, reason = bump(state, slice_id, progress_new, noprogress_cap, ceiling,
                                             None if deep else highest)
        rule = ("no stall cap" if noprogress_cap is None
                else "new-finding rule, deep section" if deep else "severity rule")
        tag = (f"CAPPED: {reason}" if capped else "counted against pass_cap" if noprogress_cap is None
               else f"no-progress streak {streak}/{noprogress_cap}")
        print(f"review round {count} for {slice_id} ({tag}; {rule}; ceiling {ceiling})")
        return 2 if capped else 0
    if "--status" in argv:
        s, c, st, _ = _read(state)
        stall = "no stall cap" if noprogress_cap is None else f"no-progress streak {st}/{noprogress_cap}"
        print(f"slice {s or '(none)'}: round {c}, {stall}, ceiling {ceiling}")
        return 0
    print("usage: review_round_guard.py [--bump SLICE --highest P1|P2|P3|none [--progress new|none] "
          "[--base REVIEW_BASE | --paths-file FILE] | --status | --reset] [--state PATH] [--selftest]",
          file=sys.stderr)
    return 1


def _arg(argv: list, name: str):
    """The value after name; None when name is absent, last, or followed by another option or nothing."""
    if name not in argv:
        return None
    i = argv.index(name)
    value = argv[i + 1] if i + 1 < len(argv) else ""
    return None if not value.strip() or value.startswith("--") else value


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
        # severity rule: the same P2 on every fix stalls, however many findings are new
        st6 = Path(td) / "r6"
        results = [bump(st6, "S", True, 3, 30, "P2") for _ in range(4)]
        check("same-severity-stalls", [r[2] for r in results] == [False, False, False, True])
        # severity rule: a fall from P1 to P2 is progress
        st7 = Path(td) / "r7"
        bump(st7, "S", True, 3, 30, "P1"); bump(st7, "S", True, 3, 30, "P1")
        _, s7, c7, _ = bump(st7, "S", True, 3, 30, "P2")
        check("falling-severity-resets", s7 == 0 and c7 is False)
        # a worse severity (P2 -> P1) is no progress
        st8 = Path(td) / "r8"
        bump(st8, "S", True, 3, 30, "P2")
        _, s8, _, _ = bump(st8, "S", True, 3, 30, "P1")
        check("worsening-is-not-progress", s8 == 1)
        # a switch between the rules starts the streak again
        st9 = Path(td) / "r9"
        for _ in range(3):
            bump(st9, "S", True, 3, 30, "P2")              # severity streak 2
        _, s9, c9, _ = bump(st9, "S", False, 3, 30)        # first deep round, nothing new
        check("rule-switch-resets", s9 == 1 and c9 is False)
        # deep paths: a kernel file keeps the new-finding rule; unknown paths do too
        check("kernel-is-deep", is_deep(["src/kernel/sched.c", "docs/x.md"], ("src/kernel/",)))
        check("tooling-not-deep", not is_deep(["scripts/inventory/capture.py"], ("src/kernel/",)))
        check("unknown-paths-deep", is_deep(None, ("src/kernel/",)) and is_deep([], None))

    if failures:
        print("selftest FAIL: " + ", ".join(failures), file=sys.stderr)
        return 1
    print("selftest OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
