#!/usr/bin/env python3
"""run-all.sh runs every test a file defines, and a file it cannot run fails.

Until 2026-11-12 a pytest-style file (tests taking tmp_path, no __main__ runner) was
run with plain python3, which defines its tests and exits 0: test_review_pin.py and
test_three_pass_gate.py passed the suite with no assertion run.
"""
import os
import pathlib
import shutil
import subprocess
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
FAILING_PYTEST_STYLE = "def test_x(tmp_path):\n    assert False, 'ran'\n"
PASSING_PYTEST_STYLE = "def test_x(tmp_path):\n    assert tmp_path.exists()\n"
PLAIN = 'def test_x():\n    assert True\n\n\nif __name__ == "__main__":\n    test_x()\n'


def _suite(files, path=None):
    with tempfile.TemporaryDirectory() as d:
        tests = pathlib.Path(d) / "a/b/c"
        tests.mkdir(parents=True)
        (pathlib.Path(d) / "build").mkdir()
        (pathlib.Path(d) / "build/todo-cache.json").write_text("{}")  # skip the cache precondition
        shutil.copy(HERE / "run-all.sh", tests / "run-all.sh")
        for name, body in files.items():
            (tests / name).write_text(body)
        env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
        if path is not None:
            env["PATH"] = path
        return subprocess.run(["bash", str(tests / "run-all.sh")], capture_output=True, text=True,
                              timeout=120, env=env)


def test_refusal_a_failing_pytest_style_test_fails_the_suite():
    done = _suite({"test_a.py": FAILING_PYTEST_STYLE, "test_b.py": PLAIN})
    assert done.returncode != 0 and "FAIL test_a.py" in done.stdout, done.stdout
    assert "ok   test_b.py" in done.stdout, done.stdout


def test_refusal_a_fixture_test_after_the_runner_fails_the_suite():
    done = _suite({"test_a.py": PLAIN + "\n\n" + FAILING_PYTEST_STYLE})
    assert done.returncode != 0 and "FAIL test_a.py" in done.stdout, done.stdout


def test_a_passing_pytest_style_test_passes():
    done = _suite({"test_a.py": PASSING_PYTEST_STYLE})
    assert done.returncode == 0 and "== 1 passed, 0 failed ==" in done.stdout, done.stdout


def test_refusal_without_pytest_such_a_file_fails():
    with tempfile.TemporaryDirectory() as bindir:
        shim = pathlib.Path(bindir) / "python3"  # a python3 that cannot import pytest
        shim.write_text('#!/bin/sh\n[ "$1" = -c ] && case "$2" in *pytest*) exit 1;; esac\nexec /usr/bin/python3 "$@"\n')
        shim.chmod(0o755)
        done = _suite({"test_a.py": PASSING_PYTEST_STYLE}, path=f"{bindir}:/usr/bin:/bin")
        assert done.returncode != 0 and "pytest is not installed" in done.stdout, done.stdout
        done = _suite({"test_a.py": PLAIN + "\n\n" + PASSING_PYTEST_STYLE}, path=f"{bindir}:/usr/bin:/bin")
        assert done.returncode != 0 and "pytest is not installed" in done.stdout, done.stdout
        done = _suite({"test_b.py": PLAIN, "test_c.py": "assert True\n"}, path=f"{bindir}:/usr/bin:/bin")
        assert done.returncode == 0 and "== 2 passed, 0 failed ==" in done.stdout, done.stdout


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
    print("OK")
