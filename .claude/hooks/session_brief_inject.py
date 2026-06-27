#!/usr/bin/env python3
# block-via: warning-only (SessionStart context injection; never blocks)
"""SessionStart hook -- re-inject the situational brief after a compaction/resume.

On source in {compact, resume} for an ACTIVE overnight run, recompute the
runner_status brief and emit it as a systemMessage so the post-compaction context
starts oriented. Interactive sessions (no active run) get nothing. Fail-open.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path


def _repo_root() -> Path | None:
    here = Path(__file__).resolve()
    for p in here.parents:
        if (p / ".claude").is_dir():
            return p
    return None


def brief_for_event(event: dict, root: Path) -> str | None:
    if not isinstance(event, dict):
        return None
    if event.get("source") not in ("compact", "resume"):
        return None
    try:
        st = json.loads(
            (root / ".claude" / "state" / "sequencer-run.json").read_text(encoding="utf-8"))
    except Exception:
        return None
    if not st.get("active"):
        return None
    try:
        sys.path.insert(0, str(root / ".claude" / "hooks"))
        import runner_status
        return runner_status.full_brief(root)
    except Exception:
        return None


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    root = _repo_root()
    if root is None:
        return 0
    brief = brief_for_event(d, root)
    if brief:
        print(json.dumps({"systemMessage":
                          "[post-compaction re-orient -- situational brief]\n" + brief}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
