#!/usr/bin/env python3
import pathlib, subprocess, sys, tempfile

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
    with tempfile.TemporaryDirectory() as d:
        long_cell = "x" * 90
        content = (
            "| Date | Summary |\n"
            "|------|---------|\n"
            f"| 2026-01-01 | {long_cell} |\n"
        )
        p = _write(d, "a.md", content)
        r = _run(str(p))
        assert r.returncode == 0, r.stderr
        assert p.read_text() == content  # unchanged: a cell exceeds --max-cell


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


if __name__ == "__main__":
    test_ragged_table_gets_aligned()
    test_idempotent_second_run_no_change()
    test_prose_table_left_untouched()
    test_check_mode_reports_without_writing()
    test_already_aligned_check_mode_clean()
    test_directory_recurses_and_skips_non_tables()
    test_center_and_right_alignment_markers_preserved()
    test_non_table_content_untouched()
    print("PASS: format-md-tables")
