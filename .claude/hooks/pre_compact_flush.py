#!/usr/bin/env python3
# block-via: warning-only (PreCompact snapshot; never blocks)
"""PreCompact hook -- snapshot state before compaction (TODO-08 §11).

When the harness is about to compact context, copy every
.claude/state/*.json file into .claude/state/.compaction-snapshots/<ts>/
so mid-pipeline state (especially the §10 step-state file) survives the
compaction. Per design Q4: keep at most SNAPSHOT_RETAIN_COUNT directories;
older snapshots are deleted on each new compact event.

JSONL files (tool-history.jsonl, subagent-log.jsonl, skip-log.jsonl) are
NOT snapshotted -- they are append-only logs and the live file persists
across compactions.
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
SNAPSHOT_RETAIN_COUNT = 3


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
