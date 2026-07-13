#!/usr/bin/env python3
# block-via: warning-only (advisory systemMessage; never blocks a tool call)
"""D1: warn when the main session RACES an in-flight exploratory Agent dispatch.

An exploratory agent (kernel-explorer / section-context-mapper / *-mapper /
Explore) runs in the BACKGROUND. If the main session then greps/reads the SAME
files the agent was told to explore, it duplicates the agent's work (measured:
20-30 net-zero grep/reads while an explorer ran). This hook records the in-flight
dispatch's target FILE set and, on the FIRST overlapping Grep/Read, injects a
one-line "you are racing the in-flight <agent>" reminder -- so the main session
BLOCKS on the dispatch (reads its report) or does NON-overlapping work (the
correct parallel pattern: a couple of targeted spot-checks, not a re-exploration).

Overlap is by source-file BASENAME (low false-positive: reading task.c while an
agent maps task.c is racing; unrelated files are fine). Warns ONCE per window.
Fail-open, warning-only -- a miss or a false positive costs nothing.

Wiring (settings.json):
  PreToolUse Task|Agent   -> record the in-flight target set (recording on PRE,
        not POST, so a SYNCHRONOUS agent's SubagentStop clears the window before
        any grep can false-match).
  PostToolUse Grep|Read   -> warn on first overlap.
  SubagentStop            -> clear (the agent returned).
State: .claude/state/inflight-dispatch.json
"""
from __future__ import annotations

import json
import re
import sys
import time
from pathlib import Path

STATE = Path(__file__).resolve().parent.parent / "state" / "inflight-dispatch.json"
TTL_NS = 15 * 60 * 1_000_000_000  # stale-in-flight backstop if SubagentStop is missed
_FILE_RE = re.compile(r"\b([\w-]+\.(?:c|h|asm|S|cc|cpp|hpp|py))\b")

# Only READ-ONLY exploratory agents race on a file surface. A judgment/executor
# dispatch does not, so recording it would only produce false racing warnings.
_EXPLORER_AGENTS = {
    "kernel-explorer", "section-context-mapper", "concurrency-evidence-mapper",
    "review-evidence-mapper", "xref-dependency-mapper", "test-coverage-mapper",
    "todo-validation-mapper", "Explore", "code-explorer", "general-purpose",
}


def _load():
    try:
        d = json.loads(STATE.read_text())
        return d if isinstance(d, dict) else {}
    except Exception:
        return {}


def _save(d):
    try:
        STATE.parent.mkdir(parents=True, exist_ok=True)
        STATE.write_text(json.dumps(d))
    except Exception:
        pass


def _clear():
    try:
        STATE.unlink()
    except FileNotFoundError:
        pass
    except Exception:
        pass


def main():
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict):
        return 0
    tool = d.get("tool_name") or ""
    ti = d.get("tool_input") or {}

    # SubagentStop (no tool_name) -> the agent returned; close the window.
    if not tool:
        _clear()
        return 0

    if tool in ("Task", "Agent"):
        atype = str(ti.get("subagent_type") or "")
        if atype and atype not in _EXPLORER_AGENTS:
            return 0  # non-exploratory dispatch: nothing to race on a file set
        prompt = str(ti.get("prompt") or ti.get("description") or "")
        toks = sorted({m.group(1) for m in _FILE_RE.finditer(prompt)})[:40]
        if toks:
            _save({"agent": atype or "agent", "tokens": toks,
                   "ts_ns": time.time_ns(), "warned": False})
        return 0

    if tool in ("Grep", "Read"):
        st = _load()
        toks = st.get("tokens")
        if not toks or st.get("warned"):
            return 0
        if time.time_ns() - (st.get("ts_ns") or 0) > TTL_NS:
            _clear()
            return 0
        if tool == "Read":
            hay = str(ti.get("file_path") or "")
        else:  # Grep
            hay = str(ti.get("path") or "") + " " + str(ti.get("pattern") or "")
        hits = [t for t in toks if t in hay]
        if hits:
            st["warned"] = True
            _save(st)
            print(json.dumps({"systemMessage":
                "[racing in-flight agent -- not a block] the " + str(st.get("agent"))
                + " dispatch you just launched is mapping " + ", ".join(hits[:3])
                + " in the background; this " + tool + " re-covers that surface. "
                "BLOCK on the dispatch (read its report) or do NON-overlapping work "
                "instead -- do not re-explore the same files inline (D1 waste: 20-30 "
                "net-zero grep/reads). Warns once per dispatch."}))
        return 0

    return 0


if __name__ == "__main__":
    sys.exit(main())
