#!/usr/bin/env python3
# token-saver v02 (2026-07-28): every metrics record ever written carried
# `"section": 0`. Root cause: the documented cursor shape is
# `cursor <domain> <file>` with NO index, so `section_idx` never left the value
# `start` initialises it to, and stream-report.py stamped that constant onto all
# 56 rows across every run. With no section-to-turn-count correlation there is
# no dataset for the split-predictor calibration -- and since cache-read cost is
# quadratic in segment length, knowing WHICH sections run long is the whole
# input. The guard now derives the index from the triage oracle when the caller
# omits it.
#
# The load-bearing property is FAIL-OPEN: this is a metrics field, so no
# derivation failure may ever fail a `cursor` move and wedge a run.
#
# STATE_PATH resolves to the REAL repo at import, so this test monkeypatches it
# (and repo_root / _emit_anchor) to a temp dir -- it must never touch live state.
import importlib.util
import json
import pathlib
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
HOOK = HERE.parents[2] / ".claude/hooks/run_phase_guard.py"
FAILS = []


def check(name, cond):
    if not cond:
        FAILS.append(name)


def _load(state_file, repo_dir):
    spec = importlib.util.spec_from_file_location("rpg_cursor_test", HOOK)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    mod.STATE_PATH = state_file
    mod.repo_root = lambda: repo_dir
    mod._emit_anchor = lambda state: None
    return mod


def _stub_oracle(repo_dir, payload):
    """Write a fake sequencer_triage.py that prints `payload` for --classify."""
    hooks = pathlib.Path(repo_dir) / ".claude/hooks"
    hooks.mkdir(parents=True, exist_ok=True)
    (hooks / "sequencer_triage.py").write_text(
        "import sys, json\n"
        f"print(json.dumps({payload!r}))\n")


def test_explicit_index_still_wins():
    """An explicitly passed index must never be second-guessed by derivation."""
    with tempfile.TemporaryDirectory() as d:
        mod = _load(pathlib.Path(d) / "state.json", pathlib.Path(d))
        _stub_oracle(d, {"sections": [{"n": 9, "class": "NEEDS_WORK"}]})
        mod.save_state({"active": True, "file": None, "section_idx": 0})
        check("explicit accepted", mod.cli(["cursor", "dom", "todo/T.md", "4"]) == 0)
        check("explicit recorded", mod.load_state().get("section_idx") == 4)


def test_omitted_index_is_derived_from_the_oracle():
    """The measured bug: without this, section_idx stays 0 forever."""
    with tempfile.TemporaryDirectory() as d:
        mod = _load(pathlib.Path(d) / "state.json", pathlib.Path(d))
        _stub_oracle(d, {"sections": [{"n": 1, "class": "DONE"},
                                      {"n": 2, "class": "DONE"},
                                      {"n": 19, "class": "NEEDS_WORK"},
                                      {"n": 20, "class": "NEEDS_WORK"}]})
        mod.save_state({"active": True, "file": None, "section_idx": 0})
        check("cursor accepted", mod.cli(["cursor", "dom", "todo/T.md"]) == 0)
        check("first NEEDS_WORK derived", mod.load_state().get("section_idx") == 19)


def test_derivation_failure_is_fail_open():
    """No oracle, unparseable output, or an all-DONE file must leave the cursor
    move succeeding and the previous value standing. A metrics field must never
    be able to wedge a run."""
    with tempfile.TemporaryDirectory() as d:
        mod = _load(pathlib.Path(d) / "state.json", pathlib.Path(d))
        # (a) no oracle on disk at all
        mod.save_state({"active": True, "file": None, "section_idx": 5})
        check("no-oracle succeeds", mod.cli(["cursor", "dom", "todo/T.md"]) == 0)
        check("no-oracle keeps previous", mod.load_state().get("section_idx") == 5)
        # (b) oracle returns every section DONE -> nothing to derive
        _stub_oracle(d, {"sections": [{"n": 1, "class": "DONE"}]})
        mod.save_state({"active": True, "file": None, "section_idx": 5})
        check("all-done succeeds", mod.cli(["cursor", "dom", "todo/T.md"]) == 0)
        check("all-done keeps previous", mod.load_state().get("section_idx") == 5)
        # (c) oracle emits a shape the parser does not expect
        _stub_oracle(d, {"sections": "not-a-list"})
        mod.save_state({"active": True, "file": None, "section_idx": 5})
        check("bad-shape succeeds", mod.cli(["cursor", "dom", "todo/T.md"]) == 0)
        check("bad-shape keeps previous", mod.load_state().get("section_idx") == 5)


def test_derive_helper_returns_none_rather_than_raising():
    """_derive_section_idx is called on a run-critical path; it must swallow
    everything and report None."""
    with tempfile.TemporaryDirectory() as d:
        mod = _load(pathlib.Path(d) / "state.json", pathlib.Path(d))
        check("missing oracle -> None", mod._derive_section_idx("todo/T.md") is None)
        _stub_oracle(d, {"sections": [{"n": "not-an-int", "class": "NEEDS_WORK"}]})
        check("non-int n -> None", mod._derive_section_idx("todo/T.md") is None)
        _stub_oracle(d, {"no_sections_key": True})
        check("absent key -> None", mod._derive_section_idx("todo/T.md") is None)


if __name__ == "__main__":
    test_explicit_index_still_wins()
    test_omitted_index_is_derived_from_the_oracle()
    test_derivation_failure_is_fail_open()
    test_derive_helper_returns_none_rather_than_raising()
    if FAILS:
        for f in FAILS:
            print("FAIL:", f)
        raise SystemExit(1)
    print("test_cursor_section_idx OK (derivation + explicit-wins + fail-open)")
