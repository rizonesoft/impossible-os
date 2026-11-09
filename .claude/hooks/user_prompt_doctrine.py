#!/usr/bin/env python3
# block-via: warning-only (UserPromptSubmit reminder; never blocks the prompt)
"""UserPromptSubmit hook (TODO-08 §11).

Scans the user's prompt for bypass-shaped phrases ("skip review", "just commit",
"ignore the hook", "bypass the gate", "disable the hook") and injects a
systemMessage reminding the agent of the relevant doctrine + the legitimate
opt-out env var. Does NOT block the prompt -- the user's intent is
authoritative; the reminder ensures the agent reads the doctrine before
acting.

Per design Q3: pattern is intentionally narrow to avoid false-positives on
ordinary requests that happen to mention "skip" or "ignore".
"""
import json
import re
import sys


_BYPASS_PATTERNS = [
    (r"\bskip\s+(?:the\s+)?review\b", "section_commit_gate (review evidence)"),
    (r"\bbypass\s+(?:the\s+)?(?:gate|hook|review)\b", "section_commit_gate"),
    (r"\bignore\s+(?:the\s+)?hook\b", "all enforcement hooks"),
    (r"\bdisable\s+(?:the\s+)?hook\b", "all enforcement hooks"),
    (r"\bjust\s+commit\b", "section_commit_gate"),
    (r"\bjust\s+ship\b", "section_commit_gate"),
    (r"\bskip\s+(?:the\s+)?codex\b", "section_commit_gate (Codex review stamps)"),
    (r"\bskip\s+(?:the\s+)?test", "section_commit_gate (test evidence)"),
]


_REMINDER = (
    "[doctrine reminder -- not a block] User prompt contains a bypass-shape "
    "phrase ({phrase}). The relevant gate is `{gate}`. Documented opt-outs "
    "(use only when legitimate revert / stamp-only / known-false-positive "
    "applies):\n"
    "  - SKIP_REVIEW_HOOK=1 + SKIP_REVIEW_HOOK_REASON='<text >= 12 chars>' "
    "(section_commit_gate)\n"
    "  - CODEX_FLAG_OVERRIDE=1 (codex_model_flag_block; same-call only)\n"
    "Doctrine: CLAUDE.md \"Mandatory Skill Triggers\" + memory "
    "feedback_skill_invocation_drift / feedback_never_skip_review / "
    "feedback_no_corner_cutting. The user's intent is authoritative; this is "
    "a reminder, not a block."
)


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict):
        return 0

    prompt = d.get("prompt") or d.get("user_prompt") or ""
    if not isinstance(prompt, str) or not prompt:
        return 0

    for pat, gate in _BYPASS_PATTERNS:
        m = re.search(pat, prompt, re.I)
        if m:
            msg = _REMINDER.format(phrase=repr(m.group(0)), gate=gate)
            print(json.dumps({"systemMessage": msg}))
            return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
