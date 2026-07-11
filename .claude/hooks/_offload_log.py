#!/usr/bin/env python3
"""Shared offload-event logger (reminder->gate promotion evidence).

The offload reminder hooks (read_offload_reminder, build_offload_reminder,
interactive_offload_router, agent_dispatch_required WARN path) are candidates
for promotion to hard gates -- but only after their false-positive rate is
MEASURED, not guessed. Every reminder fire and every Agent dispatch appends a
one-line JSON record here; scripts/overnight/offload-report.py pairs them
(was a fire followed by a dispatch within the follow window?) and prints the
per-hook follow rate. A high follow rate = the reminder was right and a gate
is safe; a low one = promoting it would mostly block legitimate inline work.

Append-only JSONL at .claude/state/offload-events.jsonl, self-rotating at
1 MB (keeps the newest half). Single os.write under PIPE_BUF so parallel
hook processes never interleave. Fail-open: logging must never break a hook.
"""
from __future__ import annotations

import json
import os
import time
from pathlib import Path

_REL = ".claude/state/offload-events.jsonl"
_MAX_BYTES = 1 << 20


def log_event(root, kind: str, hook: str, detail: str = "") -> None:
    """kind: 'fire' (a reminder/warn fired) or 'dispatch' (an Agent ran)."""
    try:
        path = Path(root) / _REL
        path.parent.mkdir(parents=True, exist_ok=True)
        rec = json.dumps({"ts": int(time.time()), "kind": kind,
                          "hook": hook, "detail": detail[:200]}) + "\n"
        fd = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o644)
        try:
            os.write(fd, rec.encode())
        finally:
            os.close(fd)
        if path.stat().st_size > _MAX_BYTES:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
            path.write_text("\n".join(lines[len(lines) // 2:]) + "\n",
                            encoding="utf-8")
    except Exception:
        pass
