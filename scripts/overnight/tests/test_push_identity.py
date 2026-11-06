#!/usr/bin/env python3
"""check-push-identity.py: a fixture identity cannot reach main again, and the
674 commits already there (mapped by .mailmap) never block a push."""
import os
import pathlib
import re
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[3]
TOOL = REPO / "scripts/check-push-identity.py"
GOOD = ("Derick Payne", "8640728+rizonesoft@users.noreply.github.com")
FIXTURE = ("s24 magic fixture", "t@example.invalid")


def _env(who):
    e = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    e.update(GIT_AUTHOR_NAME=who[0], GIT_AUTHOR_EMAIL=who[1],
             GIT_COMMITTER_NAME=who[0], GIT_COMMITTER_EMAIL=who[1])
    return e


def _commit(d, who):
    subprocess.run(["git", "commit", "-q", "--no-verify", "--allow-empty", "-m", "x"],
                   cwd=d, env=_env(who), check=True, timeout=30)
    return subprocess.run(["git", "rev-parse", "HEAD"], cwd=d, env=_env(who), check=True,
                          capture_output=True, text=True, timeout=30).stdout.strip()


def _check(d, local, remote):
    return subprocess.run([sys.executable, str(TOOL), local, remote], cwd=d, env=_env(GOOD),
                          capture_output=True, text=True, timeout=30)


def test_range():
    with tempfile.TemporaryDirectory() as d:
        subprocess.run(["git", "init", "-q", d], check=True, timeout=30, env=_env(GOOD))
        old_bad = _commit(d, FIXTURE)
        good = _commit(d, GOOD)
        assert _check(d, good, old_bad).returncode == 0, "a good commit on top of old history must pass"
        new_bad = _commit(d, FIXTURE)
        r = _check(d, new_bad, good)
        assert r.returncode == 1 and "s24 magic fixture" in r.stdout, r.stdout
        assert _check(d, "0" * 40, new_bad).returncode == 0, "a delete pushes no commits"
        assert _check(d, new_bad, "0" * 40).returncode == 1, "a new ref with bad commits refuses"
        assert _check(d, "deadbeef", good).returncode == 0, "an unreadable range must not stop a push"


def test_committer_alone_is_refused():
    with tempfile.TemporaryDirectory() as d:
        subprocess.run(["git", "init", "-q", d], check=True, timeout=30, env=_env(GOOD))
        base = _commit(d, GOOD)
        e = _env(GOOD)
        e.update(GIT_COMMITTER_NAME=FIXTURE[0], GIT_COMMITTER_EMAIL=FIXTURE[1])
        subprocess.run(["git", "commit", "-q", "--no-verify", "--allow-empty", "-m", "y"], cwd=d, env=e, check=True, timeout=30)
        head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=d, env=e, check=True, capture_output=True, text=True, timeout=30).stdout.strip()
        assert _check(d, head, base).returncode == 1


def test_real_identities_pass():
    sys.path.insert(0, str(TOOL.parent))
    src = TOOL.read_text()
    ns = {}
    exec(compile(src.split("def rev_args")[0], "x", "exec"), ns)
    for name, email in (GOOD, ("rizonesoft", "derick@rizonetech.com")):
        assert not ns["bad"](name, email), (name, email)


def test_mapping_and_hook_are_wired():
    assert re.search(r"Derick Payne <[^>]+> s24 magic fixture <t@example\.invalid>", (REPO / ".mailmap").read_text())
    hook = (REPO / ".githooks/pre-push").read_text()
    assert "check-push-identity.py" in hook and "SKIP_PUSH_IDENTITY_CHECK" in hook


if __name__ == "__main__":
    for fn in (test_range, test_committer_alone_is_refused, test_real_identities_pass, test_mapping_and_hook_are_wired):
        fn()
    print("PASS test_push_identity")
