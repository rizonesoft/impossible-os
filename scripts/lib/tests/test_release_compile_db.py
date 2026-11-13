#!/usr/bin/env python3
"""The release gate reads compile_commands.json as bear writes it.

Two shapes in a real database broke the release-flavor proof on 2026-11-13, the
first time release.yml ran on this tree since March: bear records the build's
own -fsyntax-only AST query over sched/syscall.c as a second entry with no -o,
and the coverage check globbed src/kernel and demanded rsdump.c and utprint.c,
which the Makefile deliberately leaves out of ACPICA.
"""
import importlib.util
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = str(HERE.parents[2])
LIB = Path(os.environ.get("RELEASE_LIB_DIR", HERE.parent))


def _load(name, file):
    spec = importlib.util.spec_from_file_location(name, LIB / file)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


seam = _load("seam", "release-seam-inventory.py")
audit = _load("audit", "test-only-ref-audit.py")

CLANG = "/usr/bin/clang-19"
SYSCALL = os.path.join(REPO, "src/kernel/sched/syscall.c")
FLAGS = ["--target=x86_64-elf", "-O2"]


def compile_entry(path, on=False):
    obj = "build/" + os.path.relpath(path, REPO)[4:-2] + ".o"
    args = [CLANG] + FLAGS + (["-DKERNEL_TESTS"] if on else []) + ["-c", "-o", obj, path]
    return {"directory": REPO, "file": path, "arguments": args}


def analysis_entry(path):
    """The shape scripts/gen-user-abi.py's AST query leaves in the database."""
    return {"directory": REPO, "file": path, "arguments": [
        CLANG, "-c"] + FLAGS + ["-fsyntax-only", "-Xclang", "-ast-dump=json", path]}


def db_for(flavor):
    on = flavor == "on"
    return [compile_entry(p, on) for p in sorted(seam.expected_sources(REPO, flavor))]


def test_an_analysis_entry_is_not_a_compile():
    a, c = analysis_entry(SYSCALL), compile_entry(SYSCALL)
    assert not seam.is_kernel_tu(a, REPO) and seam.is_kernel_tu(c, REPO)
    assert not audit.is_production_tu(a, REPO, set()) and audit.is_production_tu(c, REPO, set())


def test_release_db_with_the_analysis_entry_is_complete():
    db = db_for("off")
    db.insert(0, analysis_entry(SYSCALL))
    assert seam.db_flavor(db, REPO) == "off"
    assert seam.check_db_coverage(db, REPO) == []


def test_the_makefile_excludes_are_not_demanded():
    off = seam.expected_sources(REPO, "off")
    names = {os.path.basename(p) for p in off}
    assert "rsdump.c" not in names and "utprint.c" not in names
    assert "provenance_release.c" in names
    assert any("/acpica/components/" in p for p in off)
    on = seam.expected_sources(REPO, "on")
    assert not any("/acpica/components/" in p for p in on)
    assert any("/src/kernel/test/" in p for p in on)


def test_refusal_a_missing_production_tu_is_still_reported():
    for flavor in ("off", "on"):
        db = db_for(flavor)
        gone = db.pop(len(db) // 2)["file"]
        assert seam.db_flavor(db, REPO) == flavor
        assert seam.check_db_coverage(db, REPO) == [os.path.realpath(gone)], flavor
    db = db_for("off")
    db = [analysis_entry(e["file"]) if e["file"] == SYSCALL else e for e in db]
    assert seam.check_db_coverage(db, REPO) == [os.path.realpath(SYSCALL)], \
        "an analysis entry must not stand in for the TU's compile"


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
    print("PASS test_release_compile_db")
