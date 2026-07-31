#!/usr/bin/env python3
"""Shared: where does a hook read/write `.claude/state`?

WHY THIS EXISTS (2026-07-31, proven by accident). Hooks resolved their root
either by walking up from `__file__` or by running `git rev-parse
--show-toplevel` in the CURRENT directory. Both make the answer a property of
the ENVIRONMENT rather than of the caller, so a test cannot isolate the hook it
is testing:

  - `rotate_hint.py` walks from `__file__`, which lives in the real repo. An
    attended attempt to test it against a `tempfile` fixture -- passed via BOTH
    `CLAUDE_PROJECT_DIR` and `cwd` -- drove the LIVE counter to `{"count": 95,
    "hint": true}` instead. The fixture was never touched. Impact was nil only
    because the run happened to be between segments.
  - `phase1_evidence_gate.py` asks git in the cwd, so whether a `mktemp`
    fixture resolves to the fixture, the real repo, or nothing depends on how
    the test set itself up.

The measured cost is not the contamination itself but the FLAKINESS it causes:
five control-plane assertions failed across two suite runs on 2026-07-31 and
passed on the next with zero intervening edits, because the verdicts depended
on what the live run was concurrently writing. These are the gates protecting
the section-commit and evidence contracts, so a flapping verdict is the worst
place to have one -- a real regression reads as "the usual flake", and a green
run proves less than it appears to.

CONTRACT: an explicit override always wins, so a test can be authoritative
about its own fixture. Absent the override, behaviour is exactly as before --
this is additive, and no production path changes.
"""
from __future__ import annotations

import os
from typing import Callable, Optional

#: Tests set this to a fixture directory. Nothing in production sets it.
ENV_VAR = "CLAUDE_HOOK_STATE_ROOT"


def override() -> Optional[str]:
    """The explicit test-supplied root, or None."""
    root = os.environ.get(ENV_VAR)
    if root and os.path.isdir(root):
        return root
    return None


def resolve(fallback: Callable[[], Optional[str]]) -> Optional[str]:
    """`override()` if set, else whatever the hook resolved before.

    `fallback` is the hook's ORIGINAL resolver, passed in rather than
    reimplemented here: each hook has its own correct notion of "my root"
    (primary worktree, git toplevel, `__file__` walk) and collapsing them into
    one shared guess would trade a test-isolation bug for a resolution bug.
    """
    forced = override()
    if forced:
        return forced
    try:
        return fallback()
    except Exception:
        return None
