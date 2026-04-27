#!/usr/bin/env python3
# PostToolUse Edit/Write/MultiEdit reminder: when the agent edits
# a markdown file under .claude/skills/, emit a systemMessage
# nudging to keep CLAUDE.md's Skills table in sync if the change
# was structural (new skill, renamed skill).
#
# Extracted from .claude/settings.json inline-Python (TODO-08
# section 7). Behavior is byte-equivalent to the original.
#
# Hook category: REMINDER. Wrap.sh-eligible (marker:
# `.claude/skills/`).
import json
import sys


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    path = (d.get("tool_input", {}) or {}).get("file_path", "") or ""
    if (not path
            or ".claude/skills/" not in path
            or not path.endswith(".md")):
        return 0
    msg = (
        "[CLAUDE.md sync REMINDER] Edited a skill file at " + path + ". "
        "If this was a NEW skill (new directory under .claude/skills/) "
        "or a renamed skill, update the Skills table in CLAUDE.md in "
        "the same commit. For content/behavior changes to an existing "
        "skill that kept its name, no CLAUDE.md update needed."
    )
    print(json.dumps({"systemMessage": msg}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
