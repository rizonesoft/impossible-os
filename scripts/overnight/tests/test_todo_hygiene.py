#!/usr/bin/env python3
import subprocess, sys, tempfile, pathlib

HERE = pathlib.Path(__file__).resolve().parent
SCRIPT = HERE.parent.parent / "todo-hygiene.py"


def _run(text):
    with tempfile.TemporaryDirectory() as d:
        p = pathlib.Path(d) / "TODO-x.md"
        p.write_text(text, encoding="ascii")
        r = subprocess.run([sys.executable, str(SCRIPT), str(p)],
                           text=True, capture_output=True)
        return r.returncode, r.stdout


def test_placeholder_flagged():
    rc, out = _run("Shipped in <commit> after review.\n")
    assert rc == 1, out
    assert "placeholder" in out and "<commit>" in out


def test_tbd_and_date_flagged():
    rc, out = _run("Ship <DATE>; tracking <TBD>.\n")
    assert rc == 1, out
    assert out.count("placeholder") == 2


def test_hash_token_not_flagged():
    # <hash> appears in legitimate format descriptions (SID S-1-5-80-<hash>);
    # deliberately excluded to keep the enumerator zero-false-positive.
    rc, out = _run("generates S-1-5-80-<hash> for service accounts\n")
    assert rc == 0, out


def test_clean_file():
    rc, out = _run("- [x] Done and stamped.\n> **Verified:** 2026-01-01 -- ok\n")
    assert rc == 0, out


if __name__ == "__main__":
    test_placeholder_flagged()
    test_tbd_and_date_flagged()
    test_hash_token_not_flagged()
    test_clean_file()
    print("PASS: todo-hygiene")
