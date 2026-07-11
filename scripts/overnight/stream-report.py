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
import os
import re
import sys
from datetime import datetime

# A Bash command that is really a code SEARCH (grep/rg/find/...). Split out so
# the report can prove whether an optimization actually cut main-loop search
# volume rather than just moving it into a subagent.
_SEARCH_RE = re.compile(
    r"(?:^|[|;&`]|\$\()\s*(?:rg|grep|egrep|fgrep|find|ag|ack|locate)\b")


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
    # Flatten the FULL command before clipping (runner-kit 2026-07-03): the
    # old first-line-only render dropped heredoc commit messages and Codex
    # prompt bodies from the run log entirely.
    desc = (inp.get("description") or "").strip()
    cmd = " ".join((inp.get("command") or "").split())
    parts = []
    if desc:
        parts.append(desc)
    if cmd:
        parts.append(_clip(f"$ {cmd}", 500))
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


def _content_text(content) -> str:
    """Extract assistant text from Claude or Codex-ish content shapes."""
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        parts = []
        for block in content:
            if not isinstance(block, dict):
                continue
            text = (
                block.get("text")
                or block.get("output_text")
                or block.get("content")
            )
            if isinstance(text, str) and text:
                parts.append(text)
        return "\n".join(parts)
    return ""


def _codex_command(event: dict) -> str:
    for key in ("cmd", "command", "shell_command"):
        value = event.get(key)
        if isinstance(value, str) and value.strip():
            return value
    item = event.get("item")
    if isinstance(item, dict):
        for key in ("cmd", "command", "shell_command"):
            value = item.get(key)
            if isinstance(value, str) and value.strip():
                return value
    return ""


def _codex_message(event: dict) -> str:
    text = _content_text(event.get("content"))
    if text:
        return text
    message = event.get("message")
    if isinstance(message, str):
        return message
    if isinstance(message, dict):
        text = _content_text(message.get("content"))
        if text:
            return text
    item = event.get("item")
    if isinstance(item, dict):
        if item.get("type") in ("message", "assistant_message"):
            text = _content_text(item.get("content"))
            if text:
                return text
        if isinstance(item.get("message"), str):
            return item["message"]
    return ""


class SectionMetrics:
    """Accumulate per-section token + tool counts; flush JSONL at boundaries."""

    TOKEN_FIELDS = (
        "input_tokens", "output_tokens",
        "cache_read_input_tokens", "cache_creation_input_tokens",
    )

    def __init__(self, path: str | None) -> None:
        self.path = path
        self.index = 0
        self.model_seen: str | None = None  # run-level, NOT reset per section
        # Run-level dedupe: the CLI emits one stream event PER CONTENT BLOCK,
        # each repeating the same message envelope (id + usage). Counting every
        # event inflated turns ~1.9x (7101 reported vs 3688 real requests in
        # run-20260710) and double-added usage. One message id = one API request.
        self._seen_msg_ids: set = set()
        self._reset()

    def _reset(self) -> None:
        self.turns = 0
        self.tokens = {k: 0 for k in self.TOKEN_FIELDS}
        self.sidechain_turns = 0
        self.sidechain_tokens = {k: 0 for k in self.TOKEN_FIELDS}
        self.agent_dispatches = 0
        self.grep_calls = 0
        self.lsp_calls = 0
        # Per-origin tool attribution (main loop vs subagent sidechain), so a
        # report can prove WHICH loop an optimization moved work out of. Keys:
        # read, grep, glob, bash, bash_search, lsp, agent, edit, other.
        self.main_tools: dict = {}
        self.side_tools: dict = {}

    def note_model(self, model) -> None:
        """Log the ACTUAL model id from each MAIN-LOOP assistant turn's API
        response -- not just the --model flag the launcher was invoked with.
        Closes a real gap: overnight-launch.sh echoes its own CLI args at
        startup, but nothing confirmed which model actually produced the work,
        or caught a mid-run fallback-to-sonnet (or a wrong-default regression)
        silently. Caller must NOT feed sidechain (subagent) events here: the
        analyst fleet legitimately runs Sonnet, and its messages interleave
        with the main loop in the same stream.
        """
        if not isinstance(model, str) or not model:
            return
        if self.model_seen is None:
            self.model_seen = model
            expected = (os.environ.get("OVERNIGHT_MODEL") or "opus").lower()
            if expected != "inherit" and expected not in model.lower():
                emit(f"model confirmed: {model}  ** WARNING: expected '{expected}' "
                     f"per OVERNIGHT_MODEL/arm default -- check overnight-arm.sh model drop-in **")
            else:
                emit(f"model confirmed: {model}")
        elif model != self.model_seen:
            emit(f"model changed: {self.model_seen} -> {model} "
                 f"(fallback engaged mid-run, or an override took effect)")
            self.model_seen = model

    def add_usage(self, usage, msg_id=None, sidechain: bool = False) -> None:
        """Count one API request's usage exactly once, into the main-loop or
        sidechain (subagent) bucket. Duplicate stream events for the same
        message id (one per content block) are ignored; events without an id
        (synthetic/test) count individually as before."""
        if not isinstance(usage, dict):
            return
        if msg_id:
            if msg_id in self._seen_msg_ids:
                return
            self._seen_msg_ids.add(msg_id)
        bucket = self.sidechain_tokens if sidechain else self.tokens
        if sidechain:
            self.sidechain_turns += 1
        else:
            self.turns += 1
        for k in self.TOKEN_FIELDS:
            v = usage.get(k)
            if isinstance(v, int):
                bucket[k] += v

    def add_tool(self, name: str, inp=None, sidechain: bool = False) -> None:
        # Legacy aggregate counters (main + sidechain combined) kept for
        # back-compat with metrics-report / existing dashboards.
        if name in ("Task", "Agent"):
            self.agent_dispatches += 1
        elif name == "Grep":
            self.grep_calls += 1
        elif name.startswith("mcp__lsp-bridge__"):
            self.lsp_calls += 1
        # Per-origin split.
        if name in ("Task", "Agent"):
            key = "agent"
        elif name == "Read":
            key = "read"
        elif name == "Grep":
            key = "grep"
        elif name == "Glob":
            key = "glob"
        elif name in ("Edit", "Write", "MultiEdit"):
            key = "edit"
        elif name.startswith(("mcp__lsp-bridge__", "mcp__lsp")):
            key = "lsp"
        elif name == "Bash":
            cmd = (inp or {}).get("command", "") if isinstance(inp, dict) else ""
            key = "bash_search" if _SEARCH_RE.search(cmd or "") else "bash"
        else:
            key = "other"
        bucket = self.side_tools if sidechain else self.main_tools
        bucket[key] = bucket.get(key, 0) + 1

    def flush(self, marker: str) -> None:
        if not self.path:
            return
        rec = {
            "section_index": self.index,
            "marker": marker,
            "timestamp": stamp(),
            "turns": self.turns,
            **self.tokens,
            "sidechain_turns": self.sidechain_turns,
            **{f"sidechain_{k}": v for k, v in self.sidechain_tokens.items()},
            "agent_dispatches": self.agent_dispatches,
            "grep_calls": self.grep_calls,
            "lsp_calls": self.lsp_calls,
            "main_tools": self.main_tools,
            "sidechain_tools": self.side_tools,
            "model": self.model_seen,
            "effort": os.environ.get("OVERNIGHT_EFFORT") or None,
        }
        with open(self.path, "a", encoding="ascii") as fh:
            fh.write(json.dumps(rec) + "\n")
        self.index += 1
        self._reset()


def handle(event: dict, metrics: "SectionMetrics") -> None:
    kind = event.get("type")
    if kind == "assistant":
        message = event.get("message") or {}
        # parent_tool_use_id marks a subagent sidechain event; those carry the
        # SUBAGENT's model (Sonnet analyst fleet), so only main-loop events
        # (parent_tool_use_id absent/null) witness a genuine fallback flip.
        sidechain = bool(event.get("parent_tool_use_id"))
        if not sidechain:
            metrics.note_model(message.get("model"))
        metrics.add_usage(message.get("usage"), msg_id=message.get("id"),
                          sidechain=sidechain)
        for block in message.get("content") or []:
            if block.get("type") == "text" and block.get("text"):
                emit(block["text"])
            elif block.get("type") == "tool_use":
                name = block.get("name", "unknown")
                metrics.add_tool(name, block.get("input"), sidechain=sidechain)
                summary = summarize_tool(name, block.get("input"))
                emit(f"tool: {name}  {summary}".rstrip())
                if name == "Bash":
                    cmd = (block.get("input") or {}).get("command") or ""
                    if "run_phase_guard.py progress" in cmd:
                        metrics.flush("progress")
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
        metrics.flush("final")
        emit("=== final ===")
        result = event.get("result")
        if isinstance(result, str) and result:
            emit(result)
    elif isinstance(kind, str) and kind in (
        "message", "agent_message", "assistant_message", "item.completed",
    ):
        text = _codex_message(event)
        if text:
            emit(text)
    elif isinstance(kind, str) and (
        kind.endswith(".started")
        or kind.endswith(".completed")
        or "exec" in kind
        or "command" in kind
    ):
        cmd = _codex_command(event)
        if cmd:
            label = "Bash"
            if "completed" in kind:
                code = event.get("exit_code")
                suffix = f"  exit={code}" if code is not None else ""
                emit(f"tool: {label}  {_clip(cmd)}{suffix}")
            else:
                emit(f"tool: {label}  {_clip(cmd)}")
    elif isinstance(kind, str) and "error" in kind:
        emit(f"tool error: {_clip(_codex_message(event) or event)}")
    elif os.environ.get("OVERNIGHT_STREAM_KEEP_UNKNOWN") == "1":
        emit("event: " + _clip(json.dumps(event, sort_keys=True), 500))


def main() -> int:
    metrics = SectionMetrics(os.environ.get("OVERNIGHT_METRICS_FILE") or None)
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
            handle(event, metrics)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
