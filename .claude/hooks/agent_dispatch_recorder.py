#!/usr/bin/env python3
"""PostToolUse: record the last Task/Agent dispatch for the WS1b gate.

State hook (warning-only; never exits 2). Writes
.claude/state/last-agent-dispatch.json atomically. Fail-open.
"""
from __future__ import annotations

import json
import os
import secrets
import subprocess
import sys
import time
from pathlib import Path


def _repo_root() -> Path | None:
    try:
        out = subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            text=True, timeout=2, stderr=subprocess.DEVNULL,
        ).strip()
    except Exception:
        return None
    return Path(out) if out else None


def _head_sha(root: Path) -> str:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=str(root),
            text=True, timeout=2, stderr=subprocess.DEVNULL,
        ).strip()
    except Exception:
        return ""


def _write_atomic(path: Path, data: dict) -> None:
    tmp = path.with_suffix(f"{path.suffix}.{os.getpid()}.{secrets.token_hex(4)}.tmp")
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        os.replace(str(tmp), str(path))
    except Exception:
        try:
            tmp.unlink()
        except Exception:
            pass


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if d.get("tool_name") not in ("Task", "Agent"):
        return 0
    root = _repo_root()
    if root is None:
        return 0
    ti = d.get("tool_input") or {}
    state = {
        "timestamp_ns": time.time_ns(),
        "subagent_type": ti.get("subagent_type") or "",
        "head_sha": _head_sha(root),
    }
    _write_atomic(root / ".claude" / "state" / "last-agent-dispatch.json", state)
    return 0


if __name__ == "__main__":
    sys.exit(main())
