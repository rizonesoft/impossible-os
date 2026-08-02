#!/usr/bin/env python3
"""The post-commit COUNT refresh must not widen a path-limited commit.

WHY (measured twice, both times misdiagnosed). `git commit -- <paths>` is the
documented protection for committing while an unattended run holds the same
working tree: it commits only the named paths and ignores whatever else is in
the shared index. The post-commit hook then ran a BARE `git commit --amend` to
fold in COUNT.md/README, and a bare amend commits the WHOLE INDEX -- silently
converting every path-limited commit in the repo into a full-index one.

  2026-07-31  an attended commit that `git add`-ed only its own four files
              still shipped the run's staged section deferral (8 lines).
  2026-08-02  a path-limited 3-path commit shipped 13 files, including 527
              lines of the run's in-flight test_usermode_launcher.c under an
              unrelated commit message.

Both were attributed to "the index is shared, nothing can be done"; the real
mechanism was this hook, and `--only` fixes it. This test pins that, because
the failure is invisible at commit time -- the commit succeeds and looks right
until someone reads what it actually contains.
"""
from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[3]
HOOK = REPO / ".githooks" / "post-commit"


def _git(*a, cwd, check=True):
    r = subprocess.run(["git", *a], cwd=cwd, capture_output=True, text=True,
                       timeout=120)
    if check and r.returncode != 0:
        raise AssertionError(f"git {' '.join(a)}: {r.stderr}")
    return r.stdout.strip()


def test_amend_does_not_absorb_unrelated_staged_files():
    with tempfile.TemporaryDirectory() as td:
        root = pathlib.Path(td)
        _git("init", "-b", "main", ".", cwd=root)
        _git("config", "user.email", "t@t", cwd=root)
        _git("config", "user.name", "t", cwd=root)
        (root / "COUNT.md").write_text("count: 0\n")
        (root / "README.md").write_text("readme\n")
        (root / "mine.txt").write_text("v1\n")
        (root / "theirs.c").write_text("int a(void){return 1;}\n")
        _git("add", "-A", cwd=root)
        _git("commit", "-m", "seed", cwd=root)

        # Another session stages its in-flight work in the SHARED index.
        (root / "theirs.c").write_text("int a(void){return 2;}\n/* 500 more lines */\n")
        _git("add", "theirs.c", cwd=root)
        # We commit ONLY our own file, path-limited.
        (root / "mine.txt").write_text("v2\n")
        _git("commit", "-m", "mine only", "--", "mine.txt", cwd=root)

        # Now run the repo's post-commit amend logic the same way git would.
        hooks = root / ".git" / "hooks"
        hooks.mkdir(parents=True, exist_ok=True)
        shutil.copy(HOOK, hooks / "post-commit")
        os.chmod(hooks / "post-commit", 0o755)
        subprocess.run(["bash", str(hooks / "post-commit")], cwd=root,
                       capture_output=True, text=True, timeout=180)

        files = _git("show", "--name-only", "--format=", "HEAD", cwd=root).split()
        assert "theirs.c" not in files, (
            "the post-commit amend absorbed another session's staged work: "
            f"{files}")
        assert "mine.txt" in files, files
        # and their work must still be staged, not silently consumed
        staged = _git("diff", "--cached", "--name-only", cwd=root).split()
        assert "theirs.c" in staged, (
            f"another session's staged file vanished from the index: {staged}")


if __name__ == "__main__":
    test_amend_does_not_absorb_unrelated_staged_files()
    print("PASS: post-commit amend stays within its own paths")
