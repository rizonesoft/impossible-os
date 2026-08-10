#!/usr/bin/env python3
# Protects P3.1 (2026-07-12 plan): the split predictor missed exactly-8-item
# ABI/SSDT sections (`abi_impact and open_items > 8`), and a SPLIT-RECOMMENDED
# verdict could be overridden by a free-form "cohesive". Fix: ABI threshold
# `>= 6`, and a STRUCTURED waiver (est_files/subsystems/est_tests/context_budget/
# rationale) is required to override.
import importlib.util
import json
import subprocess
import sys
import tempfile
import pathlib

HERE = pathlib.Path(__file__).resolve().parent
MANIFEST = HERE.parent / "section-manifest.py"


def _load():
    sys.path.insert(0, str(MANIFEST.parent))
    spec = importlib.util.spec_from_file_location("section_manifest", MANIFEST)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _repo_with_todo(d, items):
    root = pathlib.Path(d)
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    (root / "src/kernel/nt").mkdir(parents=True)
    (root / "src/kernel/nt/env.c").write_text("int e(void){return 0;}\n")
    (root / "todo").mkdir()
    body = ["# Test TODO", "", "## 1. SSDT env syscalls (ABI impact)",
            "Touches `src/kernel/nt/env.c` and the SSDT shadow table.", ""]
    body += [f"- [ ] item {i}" for i in range(items)]
    (root / "todo/TODO-99.md").write_text("\n".join(body) + "\n")
    subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
    return root


def _manifest(root):
    r = subprocess.run(
        [sys.executable, str(MANIFEST), "todo/TODO-99.md", "1",
         "--project", str(root)], cwd=str(root), capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return json.loads(r.stdout)


def test_abi_8_item_section_now_splits():
    with tempfile.TemporaryDirectory() as d:
        root = _repo_with_todo(d, items=8)      # exactly 8 -> old `> 8` missed it
        cx = _manifest(root)["complexity"]
        assert "SPLIT-RECOMMENDED" in cx["verdict"], cx
        assert ">=6 ABI gate" in cx["verdict"], cx
        assert cx["waiver_required"] is True


def test_small_abi_section_fits():
    with tempfile.TemporaryDirectory() as d:
        root = _repo_with_todo(d, items=3)      # below the >=6 ABI gate
        cx = _manifest(root)["complexity"]
        assert cx["verdict"] == "fits-one-context", cx
        assert cx["waiver_required"] is False


def test_calibrated_item_threshold_boundary():
    """The item threshold was CALIBRATED (2026-07-28) against 15 shipped
    sections reconstructed from history -- pre-ship features paired with the
    turns each actually cost. `open_items` is the only feature with predictive
    power (r = +0.50); the old `> 12` gate caught 2 of 8 sections that ran past
    250 turns. `>= 5` was chosen to MINIMISE TOTAL COST, not recall, because a
    section carries ~60 turns of fixed review overhead and splitting below
    T ~= 220 costs more than it saves.

    Pinned so the boundary cannot drift back without re-doing that work.

    WHAT IS COUNTED changed 2026-08-09: WORK items, not every open box. Every
    section carries a `Commit: "..."` line (1,464 corpus-wide) that costs no
    implementation turns, and counting it meant a section with FOUR real items
    tripped a threshold calibrated on effort -- every split waiver written
    against this said so by hand. The NUMBER is untouched; the calibration
    measured totals including that line, so this raises the effective bar by one
    and gives back a modelled half-point (+7.0% -> +6.5%), which one avoided
    split repays several times over at ~60 turns of fixed overhead."""
    with tempfile.TemporaryDirectory() as d:
        cx = _manifest(_repo_with_todo(d, items=5))["complexity"]
        assert "SPLIT-RECOMMENDED" in cx["verdict"], cx
        assert "5 work items" in cx["verdict"], cx
    with tempfile.TemporaryDirectory() as d:
        cx = _manifest(_repo_with_todo(d, items=4))["complexity"]
        # 4 items may still SPLIT on files/subsystems, but never on item count.
        assert "4 work items" not in cx["verdict"], cx


def test_commit_line_is_not_a_work_item():
    """The whole point of the 2026-08-09 change, and it fails against the old
    counter: five boxes of which one is the commit line is FOUR items of work,
    and must not trip the threshold."""
    with tempfile.TemporaryDirectory() as d:
        root = _repo_with_todo(d, items=4)
        # append the bookkeeping line every shipped section carries
        for p in pathlib.Path(root).rglob("TODO-*.md"):
            p.write_text(p.read_text(encoding="utf-8")
                         + '\n- [ ] Commit: `"subsystem: the change"`\n',
                         encoding="utf-8")
        cx = _manifest(root)["complexity"]
        assert cx["open_items"] == 5, cx
        assert "SPLIT-RECOMMENDED" not in cx["verdict"], cx


def test_the_bar_rises_as_a_file_approaches_its_cap():
    """The cap is a fixed BUDGET and splitting spends it: measured 2026-08-10,
    13 of one file's 39 sections were split CHILDREN, so a third of its cap
    consumption was re-partitioning work that already existed -- each slot
    unavailable to a review finding that needs a home. The predictor was blind
    to this: it optimised turns PER SECTION and never modelled that a section
    also costs a slot and brings its own review wave.

    Below the window nothing changes; a file with room should decompose freely."""
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "sm", str(pathlib.Path(__file__).resolve().parent.parent / "section-manifest.py"))
    sm = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sm)
    cap = sm._soft_cap()
    assert cap, "the soft cap must be readable from its owner, not restated here"
    trigger = cap - sm.HEADROOM_WINDOW
    loose_t, _, loose_tight = sm.item_threshold(trigger - 1)
    tight_t, _, tight_tight = sm.item_threshold(trigger)
    assert (loose_t, loose_tight) == (sm.ITEM_THRESHOLD, False), (loose_t, loose_tight)
    assert (tight_t, tight_tight) == (sm.ITEM_THRESHOLD_TIGHT, True), (tight_t, tight_tight)
    assert sm.ITEM_THRESHOLD_TIGHT > sm.ITEM_THRESHOLD, "the tight bar must be HIGHER"


def test_a_waiver_near_the_cap_must_answer_the_budget_question():
    """Overriding a split near the cap is not just "this is cohesive" -- it is
    "this decomposition is worth a slot a pending finding will not get". A
    different claim, so a different field. Below the window the ordinary five
    stand, or the extra field would be ceremony on every small file."""
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "sm", str(pathlib.Path(__file__).resolve().parent.parent / "section-manifest.py"))
    sm = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sm)
    five = {"est_files": 4, "subsystems": ["a"], "est_tests": 8,
            "context_budget": "180K", "rationale": "cohesive because ..."}
    assert sm.validate_split_waiver(five) == (True, [])
    ok, missing = sm.validate_split_waiver(five, tight=True)
    assert not ok and missing == ["cap_tradeoff"], (ok, missing)
    six = dict(five, cap_tradeoff="earns a slot ahead of a pending finding because ...")
    assert sm.validate_split_waiver(six, tight=True) == (True, [])


def test_validate_split_waiver_structured_only():
    mod = _load()
    good = {"est_files": 3, "subsystems": ["src/kernel"], "est_tests": 4,
            "context_budget": "120K", "rationale": "one cohesive syscall family"}
    ok, missing = mod.validate_split_waiver(good)
    assert ok, missing
    # a free-form string is NOT a structured waiver
    ok, _ = mod.validate_split_waiver("cohesive")
    assert not ok
    # missing a field
    bad = dict(good); del bad["est_tests"]
    ok, missing = mod.validate_split_waiver(bad)
    assert not ok and "est_tests" in missing, missing
    # a bool is not an int estimate
    b2 = dict(good); b2["est_files"] = True
    ok, missing = mod.validate_split_waiver(b2)
    assert not ok and "est_files" in missing, missing
    # empty list / blank string rejected
    b3 = dict(good); b3["subsystems"] = []
    assert not mod.validate_split_waiver(b3)[0]


def test_waiver_check_cli():
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        good = root / "w.json"
        good.write_text(json.dumps(
            {"est_files": 2, "subsystems": ["src/kernel"], "est_tests": 3,
             "context_budget": "100K", "rationale": "cohesive"}))
        r = subprocess.run([sys.executable, str(MANIFEST), "waiver-check", str(good)],
                           capture_output=True, text=True)
        assert r.returncode == 0 and json.loads(r.stdout)["ok"] is True, r.stdout
        bad = root / "b.json"
        bad.write_text(json.dumps({"rationale": "cohesive"}))
        r = subprocess.run([sys.executable, str(MANIFEST), "waiver-check", str(bad)],
                           capture_output=True, text=True)
        assert r.returncode == 1 and json.loads(r.stdout)["ok"] is False, r.stdout


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print(f"ok {name}")
    print("PASS: split-predictor (P3.1)")
