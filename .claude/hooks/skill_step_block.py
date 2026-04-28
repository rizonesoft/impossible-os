#!/usr/bin/env python3
"""PreToolUse skill-step blocker (TODO-08 §10).

On `Bash(git commit:*)`, `Skill(skill="review-todo-section")`, and the
section-commit signature (caught by section_commit_gate), looks up the
most-recent multi-step skill state from .claude/state/skill-progress.json
and refuses the call (exit 2) if the required terminal steps are missing
or the observed steps are non-contiguous.

Block message names the missing step list so the agent can run them
before retrying. Per Codex design review 2026-04-28 Q4, the documented
opt-out mirrors the existing SKIP_REVIEW_HOOK pattern:

    SKIP_SKILL_STEP_BLOCK=1 SKIP_SKILL_STEP_BLOCK_REASON="<text>"

Both required; reason is logged to .claude/state/skip-log.jsonl so the
opt-out is auditable.
"""

import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

_HOOK_DIR = Path(__file__).resolve().parent
if str(_HOOK_DIR) not in sys.path:
    sys.path.insert(0, str(_HOOK_DIR))

# pylint: disable=wrong-import-position
from skill_step_map import (  # noqa: E402
    MULTI_STEP_SKILLS,
    REQUIRED_TERMINAL_STEPS,
)
# Reuse the alias-aware git-commit classifier from section_commit_gate
# (same one post_commit_smoketest.py imports). Closes the false-positive
# observed when a python -c heredoc body contained the literal text
# "git commit" (incident 2026-04-28).
import section_commit_gate as scg  # noqa: E402


_STATE_REL = ".claude/state/skill-progress.json"
_SKIP_LOG_REL = ".claude/state/skip-log.jsonl"
_ORPHAN_SKIP_LOG_REL = ".claude/state/skill-progress-skip.log"
_ORPHAN_SKIP_LOG_MAX_LINES = 200


def _log_orphan_skip(root: str, name: str, entry: dict) -> None:
    """TODO-08 §16: append a JSONL record when _select_active_skill
    skips a `compaction_orphaned` entry. 200-line ring buffer
    (truncate from the front when over). Audit trail in case the
    skip behavior fires on what should have been a live entry.
    """
    p = os.path.join(root, _ORPHAN_SKIP_LOG_REL)
    rec = {
        "ts_ns": time.time_ns() if hasattr(time, "time_ns")
                 else int(time.time() * 1e9),
        "kind": "ORPHAN_SKIP",
        "skill": name,
        "started_head_sha": entry.get("started_head_sha", ""),
        "orphan_ts_ns": entry.get("orphan_ts_ns", 0),
        "orphan_reason": entry.get("orphan_reason", ""),
    }
    try:
        os.makedirs(os.path.dirname(p), exist_ok=True)
        # TODO-08 §16 Codex H1-adjacent fix (2026-04-28): ring-buffer
        # rewrite must not race against a concurrent appender. Use
        # advisory flock on a sibling .lock file; per-process tmp
        # name is a uniqueness backstop so even if the lock is a
        # no-op (Windows host) two writers do not clobber each
        # others tmp.
        lock_path = p + ".lock"
        try:
            import fcntl as _fcntl  # local import for portability
            have_flock = True
        except ImportError:
            _fcntl = None
            have_flock = False
        lock_fd = None
        try:
            if have_flock:
                lock_fd = os.open(lock_path, os.O_RDWR | os.O_CREAT, 0o600)
                _fcntl.flock(lock_fd, _fcntl.LOCK_EX)
            with open(p, "a", encoding="utf-8") as f:
                f.write(json.dumps(rec) + "\n")
            # Ring-buffer truncation: if file > MAX lines, keep the tail.
            with open(p, "r", encoding="utf-8") as f:
                lines = f.readlines()
            if len(lines) > _ORPHAN_SKIP_LOG_MAX_LINES:
                lines = lines[-_ORPHAN_SKIP_LOG_MAX_LINES:]
                tmp = p + ".tmp." + str(os.getpid()) + "." + str(
                    time.time_ns() if hasattr(time, "time_ns")
                    else int(time.time() * 1e9)
                )
                with open(tmp, "w", encoding="utf-8") as f:
                    f.writelines(lines)
                os.replace(tmp, p)
        except Exception:
            pass
        finally:
            if lock_fd is not None:
                try:
                    _fcntl.flock(lock_fd, _fcntl.LOCK_UN)
                except Exception:
                    pass
                try:
                    os.close(lock_fd)
                except Exception:
                    pass
    except Exception:
        pass


def _repo_root():
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            text=True, timeout=2, stderr=subprocess.DEVNULL,
        ).strip() or None
    except Exception:
        return None


def _is_blocking_signature(d: dict) -> bool:
    r"""Return True if the current tool call should be gated by the
    step-state check. Triggers:
      - Bash that is genuinely invoking `git commit` (alias-aware)
      - Skill(skill="review-todo-section")

    Uses section_commit_gate._bash_is_git_commit for the Bash classifier
    so wrapped (`gci`, `git -c alias.X='...' X`, `env VAR=val git commit`)
    and aliased commit invocations are detected, AND so the literal
    string "git commit" appearing inside a python -c heredoc body or
    similar non-command-invocation context does NOT false-positive.
    The naive `re.search(r"\bgit\s+commit\b", cmd)` we used to ship
    matched any literal occurrence; documented incident 2026-04-28
    (user tripped it when synthesizing skill-progress.json with a
    'commit ...' evidence_token string).
    """
    tn = d.get("tool_name", "")
    ti = d.get("tool_input", {}) or {}
    if tn == "Bash":
        cmd = ti.get("command") or ""
        try:
            scg._ensure_crc()
            is_commit, _ = scg._bash_is_git_commit(cmd)
        except Exception:
            return False
        return bool(is_commit)
    if tn == "Skill":
        skill = ti.get("skill") or ti.get("name") or ""
        if skill == "review-todo-section":
            return True
        return False
    return False


def _select_active_skill(state: dict, root: str = ""):
    """Pick the most-recently-started multi-step skill entry. Returns
    (skill_name, entry_dict) or (None, None) if state is empty.

    TODO-08 §16: skip entries with `compaction_orphaned: true`. The
    PreCompact hook marks every active entry orphaned on compaction
    because the PostToolUse step-observer cannot run during summary
    generation, so the entry's steps_observed list freezes; without
    this skip the gate would BLOCK every post-compaction commit
    (see file header for the structural catch-22 rationale)."""
    if not isinstance(state, dict):
        return (None, None)
    best = None
    best_ts = -1
    for name, entry in state.items():
        if name not in MULTI_STEP_SKILLS:
            continue
        if not isinstance(entry, dict):
            continue
        if entry.get("compaction_orphaned") is True:
            # §16: log the skip for audit; never gate against an
            # orphaned entry whose steps_observed froze pre-compact.
            if root:
                _log_orphan_skip(root, name, entry)
            continue
        ts = entry.get("started_ts", 0)
        if not isinstance(ts, int):
            continue
        if ts > best_ts:
            best_ts = ts
            best = (name, entry)
    return best if best else (None, None)


def _missing_terminal_steps(skill: str, observed) -> list:
    required = REQUIRED_TERMINAL_STEPS.get(skill, [])
    seen = set()
    if isinstance(observed, list):
        for o in observed:
            if isinstance(o, dict):
                n = o.get("n")
                if isinstance(n, int):
                    seen.add(n)
    return [n for n in required if n not in seen]


def _log_skip(root: str, reason: str, skill: str, missing: list) -> None:
    p = os.path.join(root, _SKIP_LOG_REL)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    rec = {
        "ts_ns": time.time_ns() if hasattr(time, "time_ns") else int(time.time() * 1e9),
        "kind": "SKIP_SKILL_STEP_BLOCK",
        "reason": reason[:240],
        "skill": skill,
        "missing": missing,
    }
    try:
        with open(p, "a", encoding="utf-8") as f:
            f.write(json.dumps(rec) + "\n")
    except Exception:
        pass


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0

    if not _is_blocking_signature(d):
        return 0

    root = _repo_root()
    if not root:
        return 0

    # Opt-out (Q4): both env vars required.
    if os.environ.get("SKIP_SKILL_STEP_BLOCK", "") == "1":
        reason = os.environ.get("SKIP_SKILL_STEP_BLOCK_REASON", "")
        if len(reason) < 12:
            sys.stderr.write(
                "[skill-step-block] SKIP_SKILL_STEP_BLOCK=1 set but "
                "SKIP_SKILL_STEP_BLOCK_REASON missing or under 12 chars; "
                "the opt-out requires a plain-language reason >= 12 chars."
            )
            return 2
        # Will log on the way out below.

    state_path = os.path.join(root, _STATE_REL)
    try:
        with open(state_path, "r", encoding="utf-8") as f:
            state = json.load(f)
    except Exception:
        # No state -> nothing to enforce. The §4 section-commit gate is
        # the canonical guard for sections without an active skill flow.
        return 0

    skill, entry = _select_active_skill(state, root)
    if not skill or not entry:
        return 0

    observed = entry.get("steps_observed", [])
    missing = _missing_terminal_steps(skill, observed)
    if not missing:
        return 0

    if os.environ.get("SKIP_SKILL_STEP_BLOCK", "") == "1":
        _log_skip(root, os.environ.get("SKIP_SKILL_STEP_BLOCK_REASON", ""),
                  skill, missing)
        return 0

    sys.stderr.write(
        "[skill-step-block] BLOCK -- skill `" + skill + "` is at commit/"
        "review-todo-section gate but step(s) " + str(missing) + " were "
        "never observed by the §10 step observer. Run them or set "
        "SKIP_SKILL_STEP_BLOCK=1 SKIP_SKILL_STEP_BLOCK_REASON=\"<text>\" "
        "(both required, reason >= 12 chars). The required terminal "
        "step list for `" + skill + "` is "
        + str(REQUIRED_TERMINAL_STEPS.get(skill, [])) + ". Steps "
        "observed so far: " + str(sorted(o.get("n") for o in observed
                                          if isinstance(o, dict))) + ". "
        "Doctrine: feedback_skill_invocation_drift memory + TODO-08 §10."
    )
    return 2


if __name__ == "__main__":
    sys.exit(main())
