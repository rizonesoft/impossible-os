#!/usr/bin/env python3
"""Smoke test for advance-work.py -- the pre-Opus packet assembler.

advance-work orchestrates the real oracle/classify over the whole repo, so (like
test-launch.sh) it runs against the real tree with --no-pack for speed. It
asserts the packet is well-formed, not any specific cursor (that moves as the
repo progresses). The composed tools each have their own hermetic tests."""
import json
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
AW = HERE.parent / "advance-work.py"
REPO = HERE.parent.parent.parent


def test_packet_is_well_formed():
    r = subprocess.run([sys.executable, str(AW), "--no-pack"],
                       capture_output=True, text=True, cwd=str(REPO), timeout=90)
    assert r.returncode == 0, r.stderr
    d = json.loads(r.stdout)
    assert d.get("action") in ("work", "fixpoint", "blocked", "unknown"), d
    if d["action"] == "work":
        assert d.get("cursor", {}).get("file"), d
        assert "sections_summary" in d, d
        # preflight verdict is always surfaced (cached stamp or 'not run')
        assert "preflight" in d, d


def _mod():
    import importlib.util
    spec = importlib.util.spec_from_file_location("advance_work", AW)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_cursor_disagreement_is_a_field_not_a_silent_choice():
    """v17 close-out (2026-08-29): the guard's explicit cursor said 60 while
    the oracle said 59 and nothing reconciled them. The packet now carries the
    disagreement; the oracle stays the answer."""
    m = _mod()
    todo = "todo/00-infrastructure/TODO-06-x.md"
    st = {"file": todo, "section_idx": 60, "section_source": "explicit"}
    d = m._cursor_disagreement(st, todo, {"n": 59, "class": "NEEDS_WORK"})
    assert d and d["cursor_section_idx"] == 60 and d["oracle_section"] == 59, d
    # Agreement, a derived cursor, another file, or no next section -> nothing.
    assert m._cursor_disagreement(st, todo, {"n": 60}) is None
    assert m._cursor_disagreement({**st, "section_source": "derived"}, todo, {"n": 59}) is None
    assert m._cursor_disagreement(st, "todo/other.md", {"n": 59}) is None
    assert m._cursor_disagreement(st, todo, None) is None
    assert m._cursor_disagreement({}, todo, {"n": 59}) is None


if __name__ == "__main__":
    test_packet_is_well_formed()
    test_cursor_disagreement_is_a_field_not_a_silent_choice()
    print("PASS: advance-work")
