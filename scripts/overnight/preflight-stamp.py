#!/usr/bin/env python3
"""Tree-hash-keyed PREFLIGHT stamp: skip the build+test baseline when nothing changed.

PREFLIGHT's `scripts/build.sh` + full `scripts/test.sh QUIET=1` baseline
(~19k tests) re-ran on every fresh run start even when HEAD and the dirty
diff were byte-identical to the last green baseline (lesson from the retired
Codex overnight runner, 2026-07-04: nine relaunches x full gates in ~100
minutes). The stamp key is HEAD + sha256(git diff HEAD) + sha256(git status
--porcelain), so ANY commit, tracked edit, or new/removed untracked path
invalidates it; a TTL (default 12h) bounds staleness from non-tree inputs
(toolchain, host state).

Usage:
  preflight-stamp.py PROJECT_DIR check [--ttl-hours N]
      exit 0 + cached summary when the stamp matches and is fresh
          -> SKIP the build/test baseline
      exit 1 with the reason otherwise -> run the gates
  preflight-stamp.py PROJECT_DIR record --summary "BUILD OK; 19750+16 PASS"
      write the stamp for the current tree (call ONLY after green gates)

Stamp: .claude/overnight/state/preflight-stamp.json. Stdlib only.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path


def _git(project_dir: Path, *args: str) -> str | None:
    try:
        r = subprocess.run(["git", "-C", str(project_dir), *args],
                           capture_output=True, timeout=60)
        return r.stdout.decode("utf-8", "replace") if r.returncode == 0 else None
    except Exception:
        return None


def tree_key(project_dir: Path) -> str | None:
    head = _git(project_dir, "rev-parse", "HEAD")
    diff = _git(project_dir, "diff", "HEAD")
    status = _git(project_dir, "status", "--porcelain", "-uall")
    if head is None or diff is None or status is None:
        return None
    # The runner's own runtime dir (this stamp, streaks, reports) must not
    # invalidate the key it feeds -- normally gitignored, filtered anyway.
    # -uall expands untracked dirs to full paths so the filter can match.
    status = "\n".join(line for line in status.splitlines()
                       if ".claude/overnight/" not in line)
    return "{}:{}:{}".format(
        head.strip(),
        hashlib.sha256(diff.encode()).hexdigest()[:16],
        hashlib.sha256(status.encode()).hexdigest()[:16],
    )


def stamp_path(project_dir: Path) -> Path:
    return project_dir / ".claude" / "overnight" / "state" / "preflight-stamp.json"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("project_dir")
    ap.add_argument("verb", choices=["check", "record"])
    ap.add_argument("--summary", default="")
    ap.add_argument("--ttl-hours", type=float, default=12.0)
    ap.add_argument("--now", type=int, default=None, help="epoch override (tests)")
    args = ap.parse_args()

    project = Path(args.project_dir)
    path = stamp_path(project)
    now = args.now if args.now is not None else int(time.time())
    key = tree_key(project)

    if args.verb == "record":
        if key is None:
            print("preflight-stamp: git unavailable, stamp not recorded", file=sys.stderr)
            return 1
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({"key": key, "epoch": now,
                                    "summary": args.summary}))
        print(f"preflight stamp recorded for tree {key.split(':')[0][:12]}")
        return 0

    # check
    if key is None:
        print("PREFLIGHT STAMP MISS: git unavailable")
        return 1
    try:
        stamp = json.loads(path.read_text())
    except (OSError, ValueError):
        print("PREFLIGHT STAMP MISS: no stamp recorded")
        return 1
    if stamp.get("key") != key:
        print("PREFLIGHT STAMP MISS: tree changed since last green baseline")
        return 1
    age = now - int(stamp.get("epoch", 0))
    if age > args.ttl_hours * 3600:
        print(f"PREFLIGHT STAMP MISS: stamp expired ({age // 3600}h old)")
        return 1
    print("PREFLIGHT STAMP VALID (age {}m): {}".format(
        age // 60, stamp.get("summary", "") or "green baseline"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
