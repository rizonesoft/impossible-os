#!/usr/bin/env python3
# PreToolUse Skill matcher reminder: when the agent invokes one
# of the implementation / review / create-todo flagship skills,
# emit a `systemMessage` reminding the agent of the full pipeline
# discipline (no corner cutting, no skipped Codex, completion-first).
#
# Extracted from .claude/settings.json inline-Python (TODO-08 section
# 7). Behavior is byte-equivalent to the original.
#
# Hook category: REMINDER (systemMessage; never blocks). Safe to
# wrap with .claude/hooks/wrap.sh marker-prefilter; markers are the
# six skill names below.
#
# Doctrine: CLAUDE.md "Mandatory Skill Triggers";
# memory feedback_no_corner_cutting / feedback_skill_invocation_drift.
import json
import sys


_GATED_SKILLS = (
    "implement-todo-section",
    "implement-ssdt-range",
    "review-todo-section",
    "quality-review-section",
    "create-todo",
    "complete-todo-file",
)


_PIPELINE_REMINDER = (
    "[NO CODE SHIPS WITHOUT BEING EXAMINED FROM EVERY ANGLE. "
    "Completion-first, not checklist-first.] "
    "IMPLEMENT: (1) Read+XREFs (2) Explore + run the completion radar: "
    "correctness, completeness, wiring, parity, superiority, ownership "
    "(3) Walk domain code quality gates -- READ the skill, check EVERY gate "
    "(4) Implement -- no stubs, no ownerless adjacent gaps (5) Build "
    "(6) Tests (7) Update tables and TODO ownership "
    "(8) MANDATORY Codex adversarial -- DISPATCH to plugin, apply "
    "receiving-code-review, FIX everything valid "
    "(9) Self-review BEFORE fixing, including adjacent completeness and "
    "competitive polish (10) Fix loop (11) Build+loose ends "
    "(12) Commit (13) Invoke /review-todo-section. "
    "REVIEW/QUALITY: run evidence, adversarial review, domain gates, "
    "Codex quality, parity+superiority analysis, "
    "feature+adjacent completeness, and self-review. "
    "Prefer the strongest available Opus-class model for implementation "
    "and TODO section review judgment. "
    "CREATE-TODO: plan for credible completeness, not paper completion; "
    "file owner sections/XREFs now, not later. "
    "Prefer the strongest available Opus-class model for "
    "roadmap-shaping work. "
    "EVERY finding gets FIXED or FILED in a concrete owner item. "
    "Domain quality skill gets READ and WALKED, not just the hook reminder."
)


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    ti = d.get("tool_input", {}) or {}
    skill = ti.get("skill") or ti.get("name") or ""
    if skill not in _GATED_SKILLS:
        return 0
    print(json.dumps({"systemMessage": _PIPELINE_REMINDER}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
