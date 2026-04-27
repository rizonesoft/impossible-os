#!/usr/bin/env python3
# PostToolUse Bash reminder: when a Codex adversarial-review
# dispatch just completed, emit a `systemMessage` reminding the
# agent to apply the receiving-code-review discipline (verify each
# finding at file:line, classify Fix / Reject / Accept, no blind
# implement, no performative agreement).
#
# Extracted from .claude/settings.json inline-Python (TODO-08
# section 7). Behavior is byte-equivalent to the original.
#
# Hook category: REMINDER. Wrap.sh-eligible (markers:
# `codex-companion.mjs adversarial-review`). The
# receiving_review_required.py PreToolUse hook is the BLOCKING
# counterpart that gates subsequent edits until the receive
# happens; this hook just nudges with the right reminder shape.
import json
import sys


_REMINDER = (
    "[external review complete -- receiving-code-review REQUIRED] "
    "Apply superpowers:receiving-code-review discipline NOW. "
    "For each finding: (1) read the cited code at the file:line first, "
    "do NOT trust the summary; "
    "(2) classify as Fix / Reject / Accept with concrete evidence -- "
    "The external reviewer reads code without runtime context and "
    "CAN be wrong; "
    "(3) reject false positives with code evidence (caller already "
    "holds lock X at file:line, path is single-threaded by construction "
    "because Y, buffer is static-asserted larger than the access, etc.); "
    "(4) YAGNI check -- if Codex demands implementing an unused "
    "feature, grep for callers and remove the dead code instead; "
    "(5) NO performative agreement -- never write great catch or "
    "you are absolutely right; just state the fix or the rejection."
)


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    cmd = (d.get("tool_input", {}) or {}).get("command", "") or ""
    if "codex-companion.mjs" not in cmd or "adversarial-review" not in cmd:
        return 0
    print(json.dumps({"systemMessage": _REMINDER}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
