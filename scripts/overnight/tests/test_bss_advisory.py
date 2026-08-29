#!/usr/bin/env python3
"""section-manifest.py carries the BSS-to-USER_BASE headroom as an ADVISORY
(v16 carry, wired at the v17 close-out 2026-08-29).

`scripts/overnight/bss-headroom.py` shipped at the v16 close-out and nothing
called it, so a section about to add kernel static data still discovered the
ceiling late. The manifest now attaches the sensor's dict for kernel-touching
sections and appends ONE gate line only when the tree is tight. Both
directions are pinned: a roomy tree adds no gate line (the whole value is lost
if it cries wolf), a tight tree does, and a missing map is never a finding.
"""
import importlib.util
import pathlib
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
MANIFEST = HERE.parent / "section-manifest.py"


def _mod():
    spec = importlib.util.spec_from_file_location("section_manifest", MANIFEST)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _root(d, bss_top_hex):
    r = pathlib.Path(d)
    (r / "user").mkdir()
    (r / "build").mkdir()
    (r / "user/user.ld").write_text("SECTIONS {\n  . = 0x800000;\n}\n")
    (r / "build/kernel.map").write_text(f"{bss_top_hex} b some_bss\n")
    return r


def test_roomy_tree_is_known_and_not_tight():
    m = _mod()
    with tempfile.TemporaryDirectory() as d:
        h = m._bss_headroom_advisory(_root(d, "00000000007a0000"))
        assert h and h.get("known") and not h.get("tight"), h


def test_tight_tree_is_flagged():
    m = _mod()
    with tempfile.TemporaryDirectory() as d:
        h = m._bss_headroom_advisory(_root(d, "00000000007ffe00"))
        assert h and h.get("known") and h.get("tight"), h


def test_missing_map_is_not_a_finding():
    m = _mod()
    with tempfile.TemporaryDirectory() as d:
        h = m._bss_headroom_advisory(pathlib.Path(d))
        assert h is None or (h.get("ok") and not h.get("known")), h


if __name__ == "__main__":
    test_roomy_tree_is_known_and_not_tight()
    test_tight_tree_is_flagged()
    test_missing_map_is_not_a_finding()
    print("PASS: bss headroom advisory")
