#!/usr/bin/env python3
"""P2.1/P2.2 convergence gate: never redispatch an unchanged kind on unchanged
relevant inputs, scoped per kind. Runs the hook's own selftest plus CLI-level
exit-code checks (0 = REDISPATCH, 1 = CONVERGED)."""
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
HOOK = REPO / ".claude/hooks/review_convergence.py"


def test_selftest_passes():
    r = subprocess.run([sys.executable, str(HOOK), "--selftest"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr


def _repo(d):
    root = pathlib.Path(d)
    (root / "src/kernel").mkdir(parents=True)
    (root / "todo").mkdir()
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    (root / "src/kernel/a.c").write_text("int a(void){return 1;}\n")
    (root / "todo/TODO-01.md").write_text("# t\n")
    subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
    subprocess.run(["git", "-C", str(root), "-c", "user.email=t@t",
                    "-c", "user.name=t", "commit", "-qm", "i"],
                   check=True, capture_output=True)
    return root


def _cli(root, *args):
    return subprocess.run([sys.executable, str(HOOK), *args],
                          cwd=str(root), capture_output=True, text=True)


def test_cli_record_then_converged_then_redispatch():
    with tempfile.TemporaryDirectory() as d:
        root = _repo(d)
        sl = "todo/TODO-01.md#1"
        # First: no prior verdict -> REDISPATCH (exit 0).
        assert _cli(root, "should-redispatch", sl, "adversarial").returncode == 0
        # Record the verdict, then unchanged inputs -> CONVERGED (exit 1).
        assert _cli(root, "record", sl, "adversarial").returncode == 0
        assert _cli(root, "should-redispatch", sl, "adversarial").returncode == 1
        # Edit source -> REDISPATCH (exit 0).
        (root / "src/kernel/a.c").write_text("int a(void){return 2;}\n")
        assert _cli(root, "should-redispatch", sl, "adversarial").returncode == 0


if __name__ == "__main__":
    test_selftest_passes()
    test_cli_record_then_converged_then_redispatch()
    print("PASS: review-convergence P2.1/P2.2")
