#!/usr/bin/env python3
# PostToolUse Edit/Write/MultiEdit reminder: when the agent edits
# a markdown file under todo/, emit a systemMessage suggesting
# validate-todo-file for structural changes; checklist toggles
# and prose tweaks do not need it.
#
# Extracted from .claude/settings.json inline-Python (TODO-08
# section 7). Behavior is byte-equivalent to the original.
#
# Hook category: REMINDER. Wrap.sh-eligible (marker: `todo/`).
import json
import sys


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    path = (d.get("tool_input", {}) or {}).get("file_path", "") or ""
    if not path or not path.endswith(".md"):
        return 0
    parts = path.replace("\\", "/").split("/")
    if "todo" not in parts:
        return 0
    msg = (
        "[validate-todo-file REMINDER] Edited a TODO file at "
        + path + ". "
        "If this was a structural edit (new section added/removed, "
        "Implementation Order row changed, XREF added/removed, "
        "OS Comparison row changed, new checklist item with "
        "prerequisites), invoke validate-todo-file to check "
        "structural integrity before committing. "
        "For pure checklist [x] toggles, prose tweaks, or "
        "notes-only updates, no validation needed. "
        "Before ARMING an overnight run over an edited TODO, validation "
        "is mandatory regardless of edit size: run validate-todo-file "
        "(it dispatches todo-validation-mapper) and require a clean pass "
        "before handoff (runner-kit law: discipline alone missed two "
        "validation passes on 2026-07-03)."
    )
    print(json.dumps({"systemMessage": msg}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
