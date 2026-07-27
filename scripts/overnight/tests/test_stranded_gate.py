#!/usr/bin/env python3
"""P6.1/P6.3 -- stranded-deferral owner sweep + fixpoint gate.

Pins the completeness guarantee the sequencer's section oracle cannot provide
on its own: `sequencer_triage.classify_section` reads the Implementation Order
row plus SECTION stamps, so a `[/]` checklist item INSIDE a shipped section is
invisible to it and fixpoint would never revisit it. These tests pin that the
gate refuses that state, that a disposition clears it, and -- most importantly
-- that the gate can never wedge an unattended run.
"""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
SCRIPT = REPO / "scripts/overnight/stranded_deferrals.py"
FAILS = []


def check(name, cond):
    if not cond:
        FAILS.append(name)


def run(*args, cwd=REPO):
    return subprocess.run([sys.executable, str(SCRIPT), *args],
                          cwd=str(cwd), capture_output=True, text=True,
                          timeout=180)


def test_item_key_is_content_bound_but_line_independent():
    """A moved item keeps its disposition; an EDITED item loses it."""
    sys.path.insert(0, str(REPO / "scripts/overnight"))
    import stranded_deferrals as sd
    base = {"source": "todo/a/TODO-1.md", "line": 10,
            "target": "todo/b/TODO-2.md", "section": 3, "item": "do the thing"}
    moved = dict(base, line=999)
    edited = dict(base, item="do a different thing")
    other_owner = dict(base, section=4)
    check("key-line-independent", sd.item_key(base) == sd.item_key(moved))
    check("key-text-bound", sd.item_key(base) != sd.item_key(edited))
    check("key-owner-bound", sd.item_key(base) != sd.item_key(other_owner))
    # whitespace-only reflow must not invalidate a disposition
    reflowed = dict(base, item="do   the\n  thing")
    check("key-whitespace-stable", sd.item_key(base) == sd.item_key(reflowed))


def test_gate_refuses_while_undispositioned_and_passes_once_cleared():
    """The live tree currently has stranded items -> gate must refuse. Then
    disposition every one of them and it must pass. Runs against a COPY of the
    disposition store so the developer's real state is untouched."""
    store = REPO / "scripts/overnight/../../.claude/state/stranded-dispositions.json"
    store = store.resolve()
    backup = store.read_text() if store.exists() else None
    try:
        if store.exists():
            store.unlink()
        r = run("--gate")
        check("gate-refuses-undispositioned", r.returncode == 1)
        check("gate-names-the-cause",
              "unblocked-but-parked" in (r.stderr or ""))
        check("gate-offers-a-way-forward",
              "--dispose" in (r.stderr or "") and "park" in (r.stderr or ""))

        items = json.loads(run("--json").stdout)
        stranded = [e for e in items if e.get("stranded")]
        check("live-tree-has-stranded-items", len(stranded) > 0)

        sys.path.insert(0, str(REPO / "scripts/overnight"))
        import stranded_deferrals as sd
        d = {sd.item_key(e): {"action": "park", "reason": "pinned by selftest"}
             for e in stranded}
        store.parent.mkdir(parents=True, exist_ok=True)
        store.write_text(json.dumps(d, indent=1))
        r2 = run("--gate")
        check("gate-passes-once-dispositioned", r2.returncode == 0)
        check("gate-pass-is-explicit", "PASS" in (r2.stdout or ""))
    finally:
        if backup is None:
            store.unlink(missing_ok=True)
        else:
            store.write_text(backup)


def test_gate_can_never_wedge_a_run():
    """`park` is always legal, so every item has a way forward. This is the
    property that made it safe to promote P6.3 from advisory to blocking."""
    sys.path.insert(0, str(REPO / "scripts/overnight"))
    import stranded_deferrals as sd
    check("park-is-a-valid-action", "park" in sd.VALID_ACTIONS)
    check("reopen-is-a-valid-action", "reopen" in sd.VALID_ACTIONS)
    check("done-is-a-valid-action", "done" in sd.VALID_ACTIONS)


def test_disposition_requires_a_recorded_reason():
    with tempfile.TemporaryDirectory():
        r = run("--dispose", "abc", "--action", "park", "--reason", "sh")
        check("short-reason-rejected", r.returncode == 2)
        r = run("--dispose", "abc", "--action", "bogus",
                "--reason", "long enough reason")
        check("bad-action-rejected", r.returncode == 2)


def test_owner_sweep_filters_to_the_shipping_section():
    """P6.1: the sweep a shipping section runs must return only items that
    name IT as owner -- otherwise the shipping pass gets the whole backlog."""
    items = json.loads(run("--json").stdout)
    if not items:
        return
    target = items[0]["target"]
    stem = Path(target).stem
    sec = str(items[0]["section"])
    r = run("--owner", stem, "--section", sec)
    check("owner-sweep-runs", r.returncode == 0)
    out = r.stdout or ""
    if "no inbound parked items" not in out:
        # every reported line must belong to the requested owner+section
        expected = sum(1 for e in items
                       if stem in e.get("target", "")
                       and str(e.get("section")) == sec)
        check("owner-sweep-count-matches",
              f"{expected} inbound parked item" in out)


def test_reopen_guidance_forbids_the_invisible_in_place_flip():
    """The oracle classifies on the Implementation Order ROW + section stamps,
    never on checklist items. So flipping `- [/]` -> `- [ ]` inside a shipped
    section leaves the item invisible to the runner AND drops it out of this
    audit (which matches `- [/]` only) -- invisible to BOTH nets, i.e. silently
    lost work. Both the gate's help text and the owner-sweep step must say so."""
    src = (REPO / "scripts/overnight/stranded_deferrals.py").read_text()
    check("gate-help-forbids-in-place-flip", "in place" in src
          and "invisible to both nets" in src.lower())
    skill = (REPO / ".claude/skills/implement-todo-section/SKILL.md").read_text()
    check("skill-forbids-in-place-flip",
          "do NOT just flip" in skill or "Do NOT just flip" in skill)
    check("skill-names-the-correct-shape",
          "concrete `- [ ]` item in a section the oracle can still see" in skill)


def test_fixpoint_wires_the_gate():
    """The gate is only a completeness guarantee if fixpoint actually calls it."""
    guard = (REPO / ".claude/hooks/run_phase_guard.py").read_text()
    check("fixpoint-invokes-gate",
          "stranded_deferrals.py" in guard and '"--gate"' in guard)
    check("fixpoint-refuses-on-gate-rc1", "gate_rc == 1" in guard)
    check("gate-fails-open-on-infra-error", "failing OPEN" in guard)


if __name__ == "__main__":
    test_item_key_is_content_bound_but_line_independent()
    test_gate_refuses_while_undispositioned_and_passes_once_cleared()
    test_gate_can_never_wedge_a_run()
    test_disposition_requires_a_recorded_reason()
    test_owner_sweep_filters_to_the_shipping_section()
    test_reopen_guidance_forbids_the_invisible_in_place_flip()
    test_fixpoint_wires_the_gate()
    if FAILS:
        for f in FAILS:
            print("FAIL:", f, file=sys.stderr)
        raise SystemExit(1)
    print("test_stranded_gate OK (owner sweep + fixpoint gate + anti-wedge)")
