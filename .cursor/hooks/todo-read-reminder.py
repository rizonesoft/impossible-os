#!/usr/bin/env python3
# Cursor beforeReadFile hook. If your build ignores agentMessage, check Output -> Hooks
# and try agent_message in stdout JSON (some Cursor builds differ).
"""Remind the agent about TODO validate / section / gap workflows when reading TODO-*.md."""
import json
import re
import sys


def _normalize_path(path: str) -> str:
    return path.replace("\\\\", "/").replace("\\", "/")


def is_todo_roadmap_todo_file(path: str) -> bool:
    """True for todo/**/TODO-*.md (case-insensitive basename)."""
    if not path:
        return False
    p = _normalize_path(path)
    pl = p.lower()
    if not pl.endswith(".md"):
        return False
    if not (pl.startswith("todo/") or "/todo/" in p):
        return False
    basename = p.split("/")[-1]
    return bool(re.match(r"^TODO-.+\.md$", basename, re.IGNORECASE))


def build_agent_message() -> str:
    return (
        "TODO roadmap file (TODO-*.md under todo/): Open the skill that matches the user task. "
        "Copy that skill's Progress checklist into the chat first; end with a checklist matrix "
        "(done / skipped or N/A / reason per line). "
        "Skills: .cursor/skills/validate-todo-file/SKILL.md (full-file structure); "
        ".cursor/skills/validate-todo-section/SKILL.md (one section vs code); "
        ".cursor/skills/gap-analysis-todo/SKILL.md (research plus gaps). "
        "History table row: required only for validate-todo-file (Action=validate) and "
        "gap-analysis-todo (Action=gap-analysis). Not required for validate-todo-section. "
        "Do not skip validate-todo-file step 7 (Win11/Linux parity) or gap-analysis Phase 2 "
        "(minimum 6 web searches, 2 WebFetch links). "
        "Rule: .cursor/rules/todo-validate-gap-workflows.mdc."
    )


def allow_response(extra=None):
    out = {"continue": True, "permission": "allow"}
    if extra:
        out.update(extra)
    return out


def run() -> None:
    data = json.load(sys.stdin)
    fp = data.get("file_path") or ""
    out = allow_response()
    if is_todo_roadmap_todo_file(fp):
        out["agentMessage"] = build_agent_message()
    print(json.dumps(out))


def main() -> None:
    try:
        run()
    except BaseException as exc:
        print(f"todo-read-reminder: error={exc!r}", file=sys.stderr, flush=True)
        print(json.dumps(allow_response()))


if __name__ == "__main__":
    main()
