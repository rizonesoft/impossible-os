#!/usr/bin/env python3
"""C3: bare_section_refs + citation_block share the `_content_lint` detectors
and CROSS-REPORT each other's findings, so one Write surfaces both classes at
once (no block-then-fix gauntlet). Also pins that the refactor preserved each
gate's block/exempt behavior."""
import importlib.util
import json
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
HOOKS = REPO / ".claude/hooks"
SIGN = "§"  # section-sign glyph, built here so this test file has no bare glyph+digit


def _load_cl():
    spec = importlib.util.spec_from_file_location("_content_lint", HOOKS / "_content_lint.py")
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def _run(hook, tool_input, env=None):
    payload = {"tool_name": "Write", "tool_input": tool_input}
    e = {**__import__("os").environ, **(env or {})}
    return subprocess.run([sys.executable, str(HOOKS / hook)],
                          input=json.dumps(payload), text=True,
                          capture_output=True, env=e)


# ---- shared detectors ----
def test_detectors_applicability_and_hits():
    cl = _load_cl()
    assert cl.bare_section_hits([f"// see {SIGN}5"], "src/kernel/x.c")
    assert not cl.bare_section_hits([f"// UEFI {SIGN}4.6"], "src/kernel/x.c")   # spec-qualified
    assert not cl.bare_section_hits([f"{SIGN}5"], "todo/x.md")                  # n/a
    assert not cl.bare_section_hits([f"{SIGN}5"], "src/kernel/fs/ntfs/m.c")     # spec-code
    assert cl.citation_hits(["// Codex H1 fix"], "src/kernel/x.c")
    assert not cl.citation_hits(["// Codex H1 fix CITATION-OK: ok"], "src/kernel/x.c")
    assert not cl.citation_hits(["// Codex H1 fix"], "src/libs/vendor.c")       # vendored
    assert not cl.citation_hits(["int Codex_review_x = 1;"], "src/kernel/x.c")  # not a comment


# ---- bare_section_refs.py gate ----
def test_bare_gate_blocks_and_exempts():
    r = _run("bare_section_refs.py", {"file_path": "src/kernel/x.c", "content": f"// {SIGN}5"})
    assert r.returncode == 2 and "bare section ref BLOCK" in r.stderr
    assert _run("bare_section_refs.py",
                {"file_path": "src/kernel/x.c", "content": "int x;"}).returncode == 0
    assert _run("bare_section_refs.py",
                {"file_path": "todo/x.md", "content": f"{SIGN}5"}).returncode == 0


def test_bare_gate_cross_reports_citation():
    r = _run("bare_section_refs.py",
             {"file_path": "src/kernel/x.c", "content": f"// {SIGN}5 (Codex H1 fix)"})
    assert r.returncode == 2
    assert "ALSO (citation gate" in r.stderr, r.stderr
    assert "file is UNCHANGED" in r.stderr


# ---- citation_block.py gate ----
def test_citation_gate_blocks_and_opts_out():
    r = _run("citation_block.py", {"file_path": "src/kernel/x.c", "content": "// Codex H1 fix"})
    assert r.returncode == 2 and "citation BLOCK" in r.stderr
    assert _run("citation_block.py",
                {"file_path": "src/kernel/x.c", "content": "// Codex H1 fix"},
                env={"SKIP_CITATION_BLOCK": "1"}).returncode == 0
    assert _run("citation_block.py",
                {"file_path": "src/libs/vendor.c", "content": "// Codex H1 fix"}).returncode == 0


def test_citation_gate_cross_reports_bare():
    r = _run("citation_block.py",
             {"file_path": "src/kernel/x.c", "content": f"// Codex H1 fix at {SIGN}5"})
    assert r.returncode == 2
    assert "ALSO (bare section-ref gate" in r.stderr, r.stderr


if __name__ == "__main__":
    test_detectors_applicability_and_hits()
    test_bare_gate_blocks_and_exempts()
    test_bare_gate_cross_reports_citation()
    test_citation_gate_blocks_and_opts_out()
    test_citation_gate_cross_reports_bare()
    print("PASS: content-lint gates + C3 cross-report")
