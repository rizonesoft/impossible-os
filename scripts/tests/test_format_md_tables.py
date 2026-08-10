#!/usr/bin/env python3
import pathlib, shutil, subprocess, sys, tempfile

HERE = pathlib.Path(__file__).resolve().parent
SCRIPT = HERE.parent / "format-md-tables.py"


def _run(*args):
    return subprocess.run([sys.executable, str(SCRIPT), *args],
                          text=True, capture_output=True)


def _write(tmp, name, content):
    p = pathlib.Path(tmp) / name
    p.write_text(content)
    return p


def test_ragged_table_gets_aligned():
    with tempfile.TemporaryDirectory() as d:
        p = _write(d, "a.md", (
            "| A | Bee | C |\n"
            "|-|:-:|--:|\n"
            "| 1 | two | 3 |\n"
            "| longcell | x | 99 |\n"
        ))
        r = _run(str(p))
        assert r.returncode == 0, r.stderr
        out = p.read_text()
        lines = out.splitlines()
        assert lines[0] == "| A        | Bee |   C |"
        assert lines[1] == "| -------- | :-: | --: |"
        assert lines[2] == "| 1        | two |   3 |"
        assert lines[3] == "| longcell |  x  |  99 |"


def test_idempotent_second_run_no_change():
    with tempfile.TemporaryDirectory() as d:
        p = _write(d, "a.md", (
            "| A | Bee | C |\n"
            "|-|:-:|--:|\n"
            "| 1 | two | 3 |\n"
            "| longcell | x | 99 |\n"
        ))
        _run(str(p))
        first = p.read_text()
        _run(str(p))
        assert p.read_text() == first


def test_prose_table_left_untouched():
    """A table whose Summary COLUMN is prose is exempt from alignment.

    THE RULE IS PER-COLUMN, NOT PER-CELL, and this test used to encode the old
    per-cell rule: one 90-char cell in an otherwise tabular table. That stopped
    exempting anything when the rule deliberately changed (a single long entry
    must not drop a genuinely tabular table out of Check 17), and the assertion
    was never updated -- nothing noticed, because this file was wired into no
    suite and therefore never ran. It is wired into `scripts/test-tooling.sh`
    now.
    """
    with tempfile.TemporaryDirectory() as d:
        rows = "".join(
            f"| 2026-01-{n:02d} | {'sentence ' * 12}|\n" for n in range(1, 5)
        )
        content = "| Date | Summary |\n|------|---------|\n" + rows
        p = _write(d, "a.md", content)
        r = _run(str(p))
        assert r.returncode == 0, r.stderr
        assert p.read_text() == content, (
            "a prose COLUMN should be exempt:\n" + p.read_text())


def test_check_mode_reports_without_writing():
    with tempfile.TemporaryDirectory() as d:
        content = "| A | B |\n|-|-|\n| 1 | 22 |\n"
        p = _write(d, "a.md", content)
        r = _run("--check", str(p))
        assert r.returncode == 1
        assert "would align" in r.stdout
        assert p.read_text() == content  # not written


def test_already_aligned_check_mode_clean():
    with tempfile.TemporaryDirectory() as d:
        # every cell already at the 3-char minimum column width -- true no-op
        content = "| Abc | Bee |\n| --- | --- |\n| 123 | 456 |\n"
        p = _write(d, "a.md", content)
        r = _run("--check", str(p))
        assert r.returncode == 0
        assert r.stdout == ""


def test_directory_recurses_and_skips_non_tables():
    with tempfile.TemporaryDirectory() as d:
        sub = pathlib.Path(d) / "sub"
        sub.mkdir()
        _write(sub, "b.md", "just prose, no tables here.\n")
        misaligned = _write(sub, "c.md", "| A | B |\n|-|-|\n| 1 | 22 |\n")
        r = _run(str(d))
        assert r.returncode == 0, r.stderr
        assert misaligned.read_text() == "| A   | B   |\n| --- | --- |\n| 1   | 22  |\n"


def test_center_and_right_alignment_markers_preserved():
    with tempfile.TemporaryDirectory() as d:
        p = _write(d, "a.md", "| Left | Mid | Right |\n|:--|:-:|--:|\n| a | bb | ccc |\n")
        _run(str(p))
        sep = p.read_text().splitlines()[1]
        cells = [c.strip() for c in sep.strip("|").split("|")]
        assert cells[0].startswith(":") and not cells[0].endswith(":")
        assert cells[1].startswith(":") and cells[1].endswith(":")
        assert cells[2].endswith(":") and not cells[2].startswith(":")


def test_non_table_content_untouched():
    with tempfile.TemporaryDirectory() as d:
        content = "# Title\n\nSome text with a | pipe | in it, not a table.\n"
        p = _write(d, "a.md", content)
        r = _run(str(p))
        assert r.returncode == 0, r.stderr
        assert p.read_text() == content


def test_indented_table_keeps_its_indentation():
    """A table indented to sit INSIDE a list item must not be moved to column 0.

    This is the root of the destructive case: re-emitting an indented table at
    column 0 exits its list container, which ends an enclosing fence early and
    silently rewrites literal content. Alignment is this tool's job; moving a
    block between Markdown containers never was.
    """
    with tempfile.TemporaryDirectory() as d:
        content = (
            "# T\n\n"
            "- item:\n"
            "  | a | bb |\n"
            "  | - | -- |\n"
            "  | 1 | 2 |\n"
        )
        p = _write(d, "a.md", content)
        r = _run(str(p))
        assert r.returncode == 0, r.stderr
        out = p.read_text()
        body = [l for l in out.split("\n") if l.strip().startswith("|")]
        assert body, out
        for line in body:
            assert line.startswith("  |"), f"indentation lost: {line!r}\n{out}"


def test_fenced_table_is_never_touched():
    """A `|` block inside a fenced example is literal text, not a table."""
    with tempfile.TemporaryDirectory() as d:
        content = (
            "# T\n\n"
            "```markdown\n"
            "| a | bb |\n"
            "|-|-|\n"
            "| 1 | 2 |\n"
            "```\n"
        )
        p = _write(d, "a.md", content)
        r = _run(str(p))
        assert r.returncode == 0, r.stderr
        assert p.read_text() == content, "fenced example was rewritten"
        c = _run("--check", str(p))
        assert c.returncode == 0, f"--check flagged a fenced example: {c.stdout}{c.stderr}"


def test_table_after_a_fence_is_still_aligned():
    """Skipping fenced lines must not blind the tool to a REAL table after one."""
    with tempfile.TemporaryDirectory() as d:
        content = (
            "# T\n\n"
            "```\n"
            "not a table\n"
            "```\n\n"
            "| a | bb |\n"
            "|-|-|\n"
            "| 1 | 2 |\n"
        )
        p = _write(d, "a.md", content)
        r = _run(str(p))
        assert r.returncode == 0, r.stderr
        out = p.read_text()
        assert "| --- |" in out or "| -- |" in out, f"real table not aligned:\n{out}"


def test_missing_shared_mask_fails_closed():
    """With the shared fence mask unavailable the tool must REFUSE, not proceed.

    A mutating tool whose safety check silently turns itself off is worse than
    one that stops: lint Check 17 shells out to `--check`, so a swallowed
    ImportError would both corrupt fenced content and tell the operator to run
    the corrupting repair.
    """
    import importlib.util
    with tempfile.TemporaryDirectory() as d:
        shutil.copy(str(SCRIPT), str(pathlib.Path(d) / "format-md-tables.py"))
        spec = importlib.util.spec_from_file_location(
            "_fmt_isolated", str(pathlib.Path(d) / "format-md-tables.py"))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        try:
            mod._fence_mask(["a"])
        except mod.MaskUnavailable:
            return
        raise AssertionError("mask unavailable did not fail closed")


if __name__ == "__main__":
    test_ragged_table_gets_aligned()
    test_idempotent_second_run_no_change()
    test_prose_table_left_untouched()
    test_check_mode_reports_without_writing()
    test_already_aligned_check_mode_clean()
    test_directory_recurses_and_skips_non_tables()
    test_center_and_right_alignment_markers_preserved()
    test_non_table_content_untouched()
    test_indented_table_keeps_its_indentation()
    test_fenced_table_is_never_touched()
    test_table_after_a_fence_is_still_aligned()
    test_missing_shared_mask_fails_closed()
    print("PASS: format-md-tables")
