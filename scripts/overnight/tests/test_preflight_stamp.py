#!/usr/bin/env python3
import pathlib, subprocess, sys, tempfile

HERE = pathlib.Path(__file__).resolve().parent
SCRIPT = HERE.parent / "preflight-stamp.py"

NOW = 1_800_000_000


def _run(root, verb, *extra, now=NOW):
    args = [sys.executable, str(SCRIPT), str(root), verb, "--now", str(now), *extra]
    return subprocess.run(args, text=True, capture_output=True)


def _mkrepo(d):
    root = pathlib.Path(d)
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    (root / "f.txt").write_text("v1\n")
    subprocess.run(["git", "-C", str(root), "add", "."], check=True)
    subprocess.run(["git", "-C", str(root), "-c", "user.email=t@t", "-c",
                    "user.name=t", "commit", "-q", "-m", "seed"], check=True)
    return root


def test_check_misses_without_stamp():
    with tempfile.TemporaryDirectory() as d:
        root = _mkrepo(d)
        r = _run(root, "check")
        assert r.returncode == 1 and "no stamp" in r.stdout


def test_record_then_check_hits_with_summary():
    with tempfile.TemporaryDirectory() as d:
        root = _mkrepo(d)
        r = _run(root, "record", "--summary", "BUILD OK; 19750+16 PASS")
        assert r.returncode == 0, r.stderr
        r = _run(root, "check", now=NOW + 300)
        assert r.returncode == 0, r.stdout
        assert "BUILD OK; 19750+16 PASS" in r.stdout and "age 5m" in r.stdout


def test_tracked_edit_invalidates():
    with tempfile.TemporaryDirectory() as d:
        root = _mkrepo(d)
        _run(root, "record", "--summary", "green")
        (root / "f.txt").write_text("v2\n")
        r = _run(root, "check")
        assert r.returncode == 1 and "tree changed" in r.stdout


def test_new_untracked_file_invalidates():
    with tempfile.TemporaryDirectory() as d:
        root = _mkrepo(d)
        _run(root, "record", "--summary", "green")
        (root / "new.txt").write_text("x\n")
        r = _run(root, "check")
        assert r.returncode == 1 and "tree changed" in r.stdout


def test_commit_invalidates():
    with tempfile.TemporaryDirectory() as d:
        root = _mkrepo(d)
        _run(root, "record", "--summary", "green")
        subprocess.run(["git", "-C", str(root), "-c", "user.email=t@t", "-c",
                        "user.name=t", "commit", "-q", "--allow-empty",
                        "-m", "more"], check=True)
        r = _run(root, "check")
        assert r.returncode == 1 and "tree changed" in r.stdout


def test_ttl_expiry():
    with tempfile.TemporaryDirectory() as d:
        root = _mkrepo(d)
        _run(root, "record", "--summary", "green")
        r = _run(root, "check", now=NOW + 13 * 3600)
        assert r.returncode == 1 and "expired" in r.stdout
        r = _run(root, "check", "--ttl-hours", "24", now=NOW + 13 * 3600)
        assert r.returncode == 0


def test_not_a_repo_fails_closed():
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        r = _run(root, "check")
        assert r.returncode == 1 and "git unavailable" in r.stdout


if __name__ == "__main__":
    test_check_misses_without_stamp()
    test_record_then_check_hits_with_summary()
    test_tracked_edit_invalidates()
    test_new_untracked_file_invalidates()
    test_commit_invalidates()
    test_ttl_expiry()
    test_not_a_repo_fails_closed()
    print("PASS: preflight_stamp")
