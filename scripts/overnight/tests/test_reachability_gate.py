#!/usr/bin/env python3
"""Fixpoint must refuse while filed work is unreachable.

WHY: "complete" has to mean nothing was left behind, not that the checklist
stopped growing. The stranded-deferral gate already covers `[/]` items whose
OWNER shipped; it cannot see a bare `- [ ]` sitting in a section the oracle
classifies DONE -- nothing revisits those. 333 such items were counted on
2026-08-02, the day this gate was added, and they had accumulated invisibly
because every existing check exempted them for an individually correct reason.

The two properties pinned here are the ones whose loss would be silent:

  * the detector EXITS 1 while unreachable work exists, so the gate can see it;
  * the detector FAILS OPEN on breakage, so a broken check can never strand a
    genuinely-complete run forever (the same discipline the stranded gate uses).
"""
from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[3]
REACH = REPO / "scripts" / "todo-reachability.py"
GUARD = REPO / ".claude" / "hooks" / "run_phase_guard.py"


def test_detector_signals_findings_with_exit_1():
    """The gate keys on the exit code; a detector that always exits 0 is inert."""
    r = subprocess.run([sys.executable, str(REACH)], cwd=REPO,
                       capture_output=True, text=True, timeout=300)
    assert r.returncode in (0, 1), f"unexpected exit {r.returncode}"
    has_findings = "unreachable finding" in r.stdout and not r.stdout.strip().startswith("0 ")
    if has_findings:
        assert r.returncode == 1, (
            "findings exist but the detector exited 0 -- the fixpoint gate "
            "would never fire")


def test_detector_is_clean_on_a_reachable_fixture():
    """No false positives on a well-formed file, or the gate blocks completion
    of a repo that is genuinely done."""
    with tempfile.TemporaryDirectory() as td:
        d = pathlib.Path(td) / "todo" / "00-infrastructure"
        d.mkdir(parents=True)
        f = d / "TODO-99-clean.md"
        f.write_text(
            "# Clean\n\n## Implementation Order\n\n"
            "| 💎 | 1 | Shipped thing | -- | [x] |\n"
            "| 💎 | 2 | Still open    | -- | [ ] |\n\n"
            "## 1. Shipped thing\n\n- [x] done\n"
            "> **Verified:** 2026-01-01 | commit `x`\n"
            "> **Quality reviewed:** 2026-01-01 | Codex\n\n"
            "## 2. Still open\n\n- [ ] genuinely open work, in an open section\n")
        r = subprocess.run([sys.executable, str(REACH), str(f)],
                           capture_output=True, text=True, timeout=120)
        assert r.returncode == 0, (
            f"false positive on a reachable fixture:\n{r.stdout}")


def test_gate_is_wired_into_fixpoint_and_fails_open():
    """The guard must CALL the detector, and must not block when it breaks."""
    src = GUARD.read_text(encoding="utf-8")
    assert "todo-reachability.py" in src, \
        "fixpoint does not consult the reachability detector"
    assert "failing OPEN, completion not blocked" in src, \
        "the reachability gate must fail open like the stranded gate"
    # the refusal must tell the operator what to DO, not just that it refused
    assert "naming its blocker" in src, "refusal text lacks the repair shape"


def test_standing_marker_exempts_a_recurring_task():
    """The doctrine and the completion gate must not contradict each other.

    `overnight-sequencer` instructs the run to LEAVE a recurring task as a bare
    `- [ ]` (converting it to `- [/]` with an invented blocker manufactures a
    false park). The reachability gate then refused `phase FIXPOINT` on exactly
    that shape, so a correctly-shaped corpus could never complete -- masked only
    while other items also blocked the gate, which means the drain SUCCEEDING is
    what would have exposed it. Found by the run itself, 2026-08-05.

    `standing:` is AUTHORED at park time. That is the distinction from the
    `parked-ownerless` detector removed on 2026-08-02: a human states the intent
    instead of a rule inferring it from shape. Unmarked items still flag.
    """
    import importlib.util, pathlib, tempfile, subprocess, os
    spec = importlib.util.spec_from_file_location("tr", str(REACH))
    tr = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tr)
    assert tr.STANDING_RE.search("- [ ] standing: re-check this annually")
    assert not tr.STANDING_RE.search("- [ ] wire the frobnicator")
    # and the filter drops ONLY the marked one
    body = ["- [x] done",
            "- [ ] standing: review triggers once per calendar year",
            "- [ ] real blocked work"]
    opens = [b for b in body if tr.OPEN_ITEM_RE.match(b)]
    real = [b for b in opens if not tr.STANDING_RE.search(b)]
    assert len(opens) == 2 and len(real) == 1 and "real blocked" in real[0]


if __name__ == "__main__":
    test_detector_signals_findings_with_exit_1()
    test_detector_is_clean_on_a_reachable_fixture()
    test_gate_is_wired_into_fixpoint_and_fails_open()
    test_standing_marker_exempts_a_recurring_task()
    print("PASS: reachability gate + standing-marker exemption")
