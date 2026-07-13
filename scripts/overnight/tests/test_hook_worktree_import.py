#!/usr/bin/env python3
"""A1 regression: both review hooks must resolve `scripts/overnight` from their
own location and import `worktree_key`.

The bug: `Path(__file__).resolve().parent.parent / "scripts/overnight"` from a
hook at `.claude/hooks/<name>.py` resolves to `.claude/scripts/overnight` (does
NOT exist), so `from worktree_hash import worktree_key` raised, was swallowed by
`except Exception: return False`, and the deterministic diff-facts fast-path
ALWAYS reported "not fresh" -- forcing a full review-evidence-mapper dispatch on
every gated edit.

This test EXTRACTS each hook's actual `.parent` chain from source (rather than
hardcoding the correct depth), so a regression back to `.parent.parent` is caught
here, not silently on a live run.
"""
import importlib
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
HOOKS = ["review_dispatch_gate.py", "agent_dispatch_required.py"]

# `Path(__file__).resolve()` followed by one-or-more `.parent`, then (possibly
# across a line break) `/ "scripts/overnight"`.
_PATH_RE = re.compile(
    r'Path\(__file__\)\.resolve\(\)((?:\.parent)+)\s*/\s*"scripts/overnight"')


def _resolved_target(hook_path: pathlib.Path) -> pathlib.Path:
    src = hook_path.read_text(encoding="utf-8")
    m = _PATH_RE.search(src)
    assert m, f"{hook_path.name}: worktree_hash path construction not found"
    resolved = hook_path.resolve()
    for _ in range(m.group(1).count(".parent")):
        resolved = resolved.parent
    return resolved / "scripts/overnight"


def test_each_hook_path_resolves_to_real_dir():
    for h in HOOKS:
        target = _resolved_target(REPO / ".claude/hooks" / h)
        assert target.is_dir(), f"{h}: path resolves to {target} (does not exist)"
        assert (target / "worktree_hash.py").is_file(), \
            f"{h}: worktree_hash.py not at {target}"


def test_worktree_key_importable_from_hook_path():
    # Exercise the real import: worktree_key must load from the path each hook
    # constructs, and be callable.
    target = _resolved_target(REPO / ".claude/hooks" / HOOKS[0])
    sys.path.insert(0, str(target))
    try:
        wh = importlib.import_module("worktree_hash")
        assert callable(wh.worktree_key), "worktree_key is not callable"
    finally:
        sys.path.remove(str(target))


if __name__ == "__main__":
    test_each_hook_path_resolves_to_real_dir()
    test_worktree_key_importable_from_hook_path()
    print("PASS: review-hook worktree_hash import (A1)")
