#!/usr/bin/env python3
"""Format `claude -p --output-format stream-json --verbose` output for run logs.

Reads stream-json lines on stdin and writes one timestamped line per event:
assistant text blocks (when the message completes), tool-use events as
"tool: <name>  <key argument>" (the command/file/skill/etc. that makes the line
self-explanatory), failed tool results as "tool error: <message>", and the
final result marked "=== final ===". Lines that are not parseable JSON pass
through unchanged. Stdlib only.
"""
from __future__ import annotations

import json
import sys
from datetime import datetime


def stamp() -> str:
    return datetime.now().strftime("%H:%M:%S")


def emit(text: str) -> None:
    for line in text.splitlines() or [""]:
        sys.stdout.write(f"{stamp()} {line}\n")
    sys.stdout.flush()


def _clip(value, limit: int = 200) -> str:
    """Collapse whitespace/newlines to one line and cap the length (ASCII only)."""
    s = " ".join(str(value).split())
    return s if len(s) <= limit else s[: limit - 3] + "..."


def _bash(inp: dict) -> str:
    desc = (inp.get("description") or "").strip()
    cmd = (inp.get("command") or "").strip()
    lines = cmd.splitlines()
    first = lines[0] if lines else ""
    more = " ..." if len(lines) > 1 else ""
    parts = []
    if desc:
        parts.append(desc)
    if first:
        parts.append(f"$ {first}{more}")
    return "  ".join(parts)


def _grep(inp: dict) -> str:
    pattern = inp.get("pattern") or ""
    path = inp.get("path")
    return f"{pattern}  in {path}" if path else str(pattern)


def _skill(inp: dict) -> str:
    name = inp.get("skill") or inp.get("command") or ""
    args = inp.get("args") or ""
    return f"{name} {args}".strip()


def _task(inp: dict) -> str:
    subtype = inp.get("subagent_type") or ""
    desc = inp.get("description") or inp.get("prompt") or ""
    label = f"{subtype}: {desc}" if subtype else str(desc)
    return label.strip(": ").strip()


def _todos(inp: dict) -> str:
    todos = inp.get("todos")
    if not isinstance(todos, list):
        return ""
    done = sum(1 for t in todos if isinstance(t, dict) and t.get("status") == "completed")
    active = next(
        (t.get("content") for t in todos
         if isinstance(t, dict) and t.get("status") == "in_progress"),
        None,
    )
    head = f"{done}/{len(todos)} done"
    return f"{head}; now: {active}" if active else head


# name -> function extracting the most informative argument for the log line.
TOOL_ARG = {
    "Bash": _bash,
    "Edit": lambda i: i.get("file_path"),
    "MultiEdit": lambda i: i.get("file_path"),
    "Write": lambda i: i.get("file_path"),
    "Read": lambda i: i.get("file_path"),
    "NotebookEdit": lambda i: i.get("notebook_path"),
    "Glob": lambda i: i.get("pattern"),
    "Grep": _grep,
    "Skill": _skill,
    "Task": _task,
    "Agent": _task,
    "WebFetch": lambda i: i.get("url"),
    "WebSearch": lambda i: i.get("query"),
    "ToolSearch": lambda i: i.get("query"),
    "TodoWrite": _todos,
}

# generic fallback: first informative scalar for tools without a dedicated rule.
_GENERIC_KEYS = ("file_path", "path", "url", "query", "pattern", "command", "name")


def _generic(inp: dict) -> str:
    for key in _GENERIC_KEYS:
        value = inp.get(key)
        if isinstance(value, str) and value.strip():
            return value
    return ""


def summarize_tool(name: str, inp) -> str:
    if not isinstance(inp, dict):
        return ""
    fn = TOOL_ARG.get(name)
    summary = ""
    if fn is not None:
        try:
            summary = fn(inp) or ""
        except Exception:
            summary = ""
    if not summary:
        summary = _generic(inp)
    return _clip(summary) if summary else ""


def _result_text(content) -> str:
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        parts = [
            b.get("text", "")
            for b in content
            if isinstance(b, dict) and b.get("type") == "text"
        ]
        return " ".join(p for p in parts if p)
    return ""


def handle(event: dict) -> None:
    kind = event.get("type")
    if kind == "assistant":
        for block in (event.get("message") or {}).get("content") or []:
            if block.get("type") == "text" and block.get("text"):
                emit(block["text"])
            elif block.get("type") == "tool_use":
                name = block.get("name", "unknown")
                summary = summarize_tool(name, block.get("input"))
                emit(f"tool: {name}  {summary}".rstrip())
    elif kind == "user":
        # surface failed tool results so overnight logs flag errors inline.
        for block in (event.get("message") or {}).get("content") or []:
            if (
                isinstance(block, dict)
                and block.get("type") == "tool_result"
                and block.get("is_error")
            ):
                msg = _result_text(block.get("content"))
                emit(f"tool error: {_clip(msg)}" if msg else "tool error")
    elif kind == "result":
        emit("=== final ===")
        result = event.get("result")
        if isinstance(result, str) and result:
            emit(result)


def main() -> int:
    for raw in sys.stdin:
        line = raw.rstrip("\n")
        if not line.strip():
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            sys.stdout.write(raw if raw.endswith("\n") else raw + "\n")
            sys.stdout.flush()
            continue
        if isinstance(event, dict):
            handle(event)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
