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
    path = root / ".claude" / "state" / "last-agent-dispatch.json"
    # Per-type dispatch map (review_dispatch_gate / xref_dispatch_reminder,
    # 2026-07-03): preserve by_type across writes so gates can ask "was a
    # review-evidence-mapper dispatched in this pass window", not just "was
    # ANYTHING dispatched recently". Top-level fields keep their old shape for
    # the existing consumers (agent_dispatch_required, build_offload_reminder).
    by_type: dict = {}
    try:
        prev = json.loads(path.read_text())
        if isinstance(prev.get("by_type"), dict):
            by_type = prev["by_type"]
    except Exception:
        pass
    stype = ti.get("subagent_type") or ""
    now = time.time_ns()
    if stype:
        by_type[stype] = {
            "timestamp_ns": now,
            "session_id": d.get("session_id") or "",
        }
        if len(by_type) > 48:  # bounded; drop oldest entries
            for k, _ in sorted(by_type.items(),
                               key=lambda kv: kv[1].get("timestamp_ns", 0))[: len(by_type) - 48]:
                by_type.pop(k, None)
    state = {
        "timestamp_ns": now,
        "subagent_type": stype,
        "head_sha": _head_sha(root),
        "by_type": by_type,
    }
    _write_atomic(path, state)
    try:
        import _offload_log
        _offload_log.log_event(root, "dispatch", "agent_dispatch_recorder", stype)
    except Exception:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
