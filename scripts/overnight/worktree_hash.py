#!/usr/bin/env python3
"""Working-tree content hashing -- the ONE correct content-binding primitive.

Why this exists (2026-07-11): section-manifest.py, evidence-bundle.py, and
impact-cone.py all bound their evidence/cache keys to the GIT INDEX
(`git ls-files -s` blob hashes / `git diff --cached`). That silently reuses
stale evidence whenever the working tree has unstaged or untracked changes --
exactly the state a mid-implementation session is in. The runner executes the
LIVE WORKING TREE, so evidence must bind to working-tree bytes, including
untracked files.

Import this; do not re-roll index-based hashing:

    from worktree_hash import content_hashes, worktree_key, changed_paths

* content_hashes(root, rels) -> {rel: "sha256:<hex>" | "absent"}
    sha256 of the CURRENT on-disk bytes of each path (untracked included).
* worktree_key(root, rels) -> 12-char digest over the sorted content hashes,
    a stable cache/bundle key that changes the instant any input's bytes change.
* changed_paths(root, rng=None) -> sorted list of repo-relative paths that
    differ from HEAD (staged AND unstaged) PLUS untracked files; or, when rng
    is given, the paths changed across that git range.

Stdlib only; safe to import from hooks and scripts.
"""
from __future__ import annotations

import hashlib
import json
import subprocess
import sys
from pathlib import Path


def _git(root, *args, timeout=30):
    try:
        r = subprocess.run(["git", "-C", str(root), *args],
                           capture_output=True, text=True, timeout=timeout)
        return r.stdout if r.returncode == 0 else ""
    except Exception:
        return ""


def content_hashes(root, rels) -> dict:
    """rel -> 'sha256:<hex>' of current working-tree bytes, or 'absent'."""
    root = Path(root)
    out = {}
    for rel in rels:
        p = root / rel
        try:
            h = hashlib.sha256(p.read_bytes()).hexdigest()
            out[rel] = f"sha256:{h}"
        except OSError:
            out[rel] = "absent"
    return out


def worktree_key(root, rels) -> str:
    """Stable 12-char key over the sorted (rel, content-hash) pairs. Two runs
    with byte-identical inputs get the same key; ANY edit (staged, unstaged, or
    untracked) changes it."""
    items = sorted(content_hashes(root, rels).items())
    return hashlib.sha256(json.dumps(items).encode()).hexdigest()[:12]


def changed_paths(root, rng=None) -> list:
    """Repo-relative paths that differ from HEAD (staged + unstaged) plus
    untracked files; or the paths changed across `rng` when given."""
    root = Path(root)
    if rng:
        names = _git(root, "diff", "--name-only", rng).splitlines()
        return sorted({n for n in names if n.strip()})
    # tracked changes vs HEAD (covers both staged and unstaged)
    tracked = _git(root, "diff", "HEAD", "--name-only").splitlines()
    # untracked, honoring .gitignore
    untracked = _git(root, "ls-files", "--others", "--exclude-standard").splitlines()
    return sorted({n for n in (*tracked, *untracked) if n.strip()})


def main(argv) -> int:
    # CLI for tests/debugging: print the key + hashes for the given paths.
    root = Path(".").resolve()
    if "--project" in argv:
        i = argv.index("--project")
        root = Path(argv[i + 1]).resolve()
        argv = argv[:i] + argv[i + 2:]
    if argv and argv[0] == "--changed":
        print(json.dumps(changed_paths(root, argv[1] if len(argv) > 1 else None)))
        return 0
    rels = argv
    print(json.dumps({"key": worktree_key(root, rels),
                      "hashes": content_hashes(root, rels)}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
