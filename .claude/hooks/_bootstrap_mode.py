#!/usr/bin/env python3
# block-via: warning-only (helper module -- never invoked as a hook directly;
# imported by phase1_evidence_gate / section_commit_gate to downgrade their
# own BLOCK to WARN on the bootstrap commit).
"""Bootstrap-mode helper for new gate hooks (TODO-08 §22).

When a hook's own implementation file is in the staged diff, the hook
should downgrade BLOCK to WARN for that single commit. Without this,
shipping a new gate triggers the gate against itself and the agent has
to use SKIP_*-env workarounds (which trains around the gate, defeating
its purpose).

Per Codex design pointer 1 (2026-04-28): paths must be normalized to
repo-relative form before comparison. The hook's `__file__` is absolute;
`git diff --cached --name-only` returns repo-relative paths. The helper
canonicalizes both sides.

Usage from a gate hook:
    from _bootstrap_mode import is_bootstrap_commit
    if is_bootstrap_commit(__file__):
        # downgrade BLOCK to WARN for this commit
        sys.stderr.write("[gate] WARN -- bootstrap commit ...\\n")
        return 0
"""
import os
import subprocess
import sys
from pathlib import Path
from typing import Optional


def _repo_root() -> Optional[str]:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            text=True, timeout=2, stderr=subprocess.DEVNULL,
        ).strip() or None
    except Exception:
        return None


def _staged_paths(root: str) -> list[str]:
    try:
        out = subprocess.check_output(
            ["git", "diff", "--cached", "--name-only"],
            cwd=root, text=True, timeout=5, stderr=subprocess.DEVNULL,
        )
    except Exception:
        return []
    return [p.strip() for p in out.splitlines() if p.strip()]


def is_bootstrap_commit(hook_self_path: str, repo_root: str | os.PathLike[str] | None = None) -> bool:
    """Return True when the hook's own implementation file is in the
    staged diff. Both sides normalized to repo-relative form.

    `hook_self_path` should be `__file__` from the calling hook.
    Returns False on any error -- bootstrap-mode is a downgrade
    convenience, not a load-bearing gate, so fail-closed is wrong here.
    """
    root = str(repo_root) if repo_root is not None else _repo_root()
    if not root:
        return False
    try:
        hook_abs = Path(hook_self_path).resolve()
        try:
            hook_rel = hook_abs.relative_to(Path(root).resolve())
        except ValueError:
            return False
    except Exception:
        return False
    hook_rel_str = str(hook_rel).replace("\\", "/")
    for staged in _staged_paths(root):
        if staged.replace("\\", "/") == hook_rel_str:
            return True
    return False
