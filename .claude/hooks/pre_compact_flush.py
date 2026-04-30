#!/usr/bin/env python3
# block-via: warning-only (PreCompact snapshot; never blocks)
"""PreCompact hook -- snapshot state + orphan-mark (TODO-08 §11+§16).

When the harness is about to compact context:

1. Mark every active entry in .claude/state/skill-progress.json as
   `compaction_orphaned: true` (TODO-08 §16). The PostToolUse step
   observer cannot run during summary generation, so the entry's
   steps_observed list freezes at whatever count was reached
   pre-compaction. Without this flag the skill-step-block hook would
   BLOCK every post-compaction `git commit` or
   `Skill(review-todo-section)` call indefinitely (PreToolUse gate
   vs PostToolUse observer is a structural catch-22 once an entry
   is orphaned). Both `skill_step_block.py` AND `skill_step_observer
   .py` skip orphaned entries; a resumed flow gets a fresh entry
   without inheriting stale state.

2. Copy every .claude/state/*.json into
   .claude/state/.compaction-snapshots/<ts>/ so mid-pipeline state
   survives the compaction (snapshot is read-only audit trail).
   Per design Q4: keep at most SNAPSHOT_RETAIN_COUNT directories;
   older snapshots are deleted on each new compact event.

JSONL files (tool-history.jsonl, subagent-log.jsonl, skip-log.jsonl)
are NOT snapshotted -- they are append-only logs and the live file
persists across compactions.
"""
import json
import os
import shutil
import subprocess
import sys
import time
from typing import Optional


_STATE_DIR_REL = ".claude/state"
_SNAPSHOT_DIR_REL = ".claude/state/.compaction-snapshots"
_SKILL_PROGRESS_REL = ".claude/state/skill-progress.json"
_TRANSCRIPT_CACHE_REL = ".claude/state/transcript-scan-cache.json"
SNAPSHOT_RETAIN_COUNT = 3


def _orphan_mark_skill_progress(state_path: str) -> None:
    """TODO-08 §16: mark every active skill-progress entry as
    `compaction_orphaned: true` so the post-compaction step-block
    + step-observer hooks skip them. Idempotent (existing flag is
    preserved with its original orphan_ts_ns). Atomic (tmp+rename).
    Best-effort: any I/O error is silently ignored -- the snapshot
    copy below is the audit trail that lets the user recover.
    """
    if not os.path.isfile(state_path):
        return
    try:
        with open(state_path, "r", encoding="utf-8") as f:
            state = json.load(f)
    except Exception:
        return
    if not isinstance(state, dict) or not state:
        return
    now = _ts_ns()
    changed = False
    for name, entry in list(state.items()):
        if not isinstance(entry, dict):
            continue
        if entry.get("compaction_orphaned") is True:
            # Idempotent -- preserve existing orphan_ts_ns.
            continue
        steps = entry.get("steps_observed") or []
        n = len(steps) if isinstance(steps, list) else 0
        entry["compaction_orphaned"] = True
        entry["orphan_ts_ns"] = now
        entry["orphan_reason"] = (
            "PreCompact fired with " + str(n) + " steps observed"
        )
        changed = True
    if not changed:
        return
    tmp = state_path + ".tmp"
    try:
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(state, f, indent=2)
            f.write("\n")
        os.replace(tmp, state_path)
    except Exception:
        try:
            os.unlink(tmp)
        except Exception:
            pass


def _repo_root() -> Optional[str]:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            text=True, timeout=2, stderr=subprocess.DEVNULL,
        ).strip() or None
    except Exception:
        return None


def _ts_ns() -> int:
    return time.time_ns() if hasattr(time, "time_ns") else int(time.time() * 1e9)


def _prune_old(snap_root: str, keep: int) -> None:
    """Keep the `keep` newest subdirectories under snap_root; delete the rest."""
    try:
        entries = []
        for name in os.listdir(snap_root):
            full = os.path.join(snap_root, name)
            if os.path.isdir(full):
                try:
                    entries.append((os.path.getmtime(full), full))
                except Exception:
                    continue
        entries.sort(reverse=True)
        for _, path in entries[keep:]:
            try:
                shutil.rmtree(path)
            except Exception:
                pass
    except Exception:
        pass


def main() -> int:
    # Hook is best-effort: snapshot regardless of payload shape. We do
    # consume stdin (so the harness pipe doesn't block) but we don't
    # use the parsed value. Codex review 2026-04-28 M1: even non-dict
    # JSON like `null` or `[]` is handled here because we never call
    # `.get(...)` on the result.
    try:
        json.load(sys.stdin)
    except Exception:
        pass

    root = _repo_root()
    if not root:
        return 0

    # §16: mark every active skill-progress entry as compaction_orphaned
    # BEFORE the snapshot copy. The snapshot then captures the marked
    # state so the audit trail shows orphan_ts_ns alongside the live
    # file's value.
    _orphan_mark_skill_progress(os.path.join(root, _SKILL_PROGRESS_REL))

    # §16: invalidate the transcript-scan offset cache. Compaction
    # changes the semantic window (the design-review gate state pre-
    # compaction does not bind on post-compaction edits), so the
    # cache's impl_seen/design_seen/pending_clear_ids must be reset.
    # Removing the file is sufficient; the next scan rebuilds it
    # from the post-compaction transcript suffix.
    cache_path = os.path.join(root, _TRANSCRIPT_CACHE_REL)
    try:
        os.unlink(cache_path)
    except (FileNotFoundError, IsADirectoryError):
        pass
    except Exception:
        pass

    state_dir = os.path.join(root, _STATE_DIR_REL)
    snap_root = os.path.join(root, _SNAPSHOT_DIR_REL)
    if not os.path.isdir(state_dir):
        return 0
    try:
        os.makedirs(snap_root, exist_ok=True)
    except Exception:
        return 0

    snap_dir = os.path.join(snap_root, str(_ts_ns()))
    try:
        os.makedirs(snap_dir, exist_ok=True)
    except Exception:
        return 0

    # Copy every *.json file at the top level of state_dir.
    try:
        for name in os.listdir(state_dir):
            if not name.endswith(".json"):
                continue
            src = os.path.join(state_dir, name)
            if not os.path.isfile(src):
                continue
            dst = os.path.join(snap_dir, name)
            try:
                shutil.copy2(src, dst)
            except Exception:
                pass
    except Exception:
        pass

    _prune_old(snap_root, SNAPSHOT_RETAIN_COUNT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
