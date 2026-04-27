#!/usr/bin/env python3
# Per-skill step-evidence map for the §10 step-state telemetry system.
#
# Each multi-step skill has a list of (step_number, tool_name, regex_pattern)
# tuples that pair a numbered step in the skill prose with the tool-call
# signature that proves the step ran. The observer (skill_step_observer.py)
# matches PostToolUse events against this map to record observed steps; the
# blocker (skill_step_block.py) reads the recorded set and refuses commits
# that lack the required terminal steps.
#
# Design discipline (per §10 design review 2026-04-28 Codex):
#   - Q3: design and adversarial reviews are DIFFERENT steps. Adversarial-
#     review evidence requires the literal `adversarial-review` token in the
#     codex-companion.mjs invocation; design dispatches do NOT count.
#   - Required terminal steps are the strict subset that, if missing, the
#     blocker refuses to allow a section-ship commit. Steps in `OPTIONAL`
#     positions (e.g. design review, smoke test on non-boot changes) are
#     observed if they fire but never required to pass the gate.
#
# Importable by:
#   - .claude/hooks/skill_step_observer.py (PostToolUse recording)
#   - .claude/hooks/skill_step_block.py    (PreToolUse gating)
#
# When adding a new multi-step skill, add an entry here and update the
# blocker's terminal-step list. Tests in scripts/test-tooling.sh exercise
# the observer + blocker round-trip.

import re
from typing import Dict, List, Tuple


# Step pattern record: (step_number, evidence_tool, evidence_regex_pattern)
# evidence_tool is "Bash" / "Edit" / "Skill" matching the harness tool name.
# evidence_regex_pattern is matched against:
#   - tool_input.command for Bash
#   - tool_input.file_path for Edit/Write/MultiEdit
#   - tool_input.skill for Skill (also accepts tool_input.name)
StepRule = Tuple[int, str, str]


# ----------------------------------------------------------------------------
# implement-todo-section: 20 steps documented in SKILL.md.
# Steps that produce mechanical evidence:
#   step 4  -- Codex design dispatch (OPTIONAL, conditional skip-rules apply)
#   step 7  -- first build (Bash: bash scripts/build.sh)
#   step 13 -- MANDATORY Codex adversarial dispatch
#   step 16 -- second build (Bash: bash scripts/build.sh, fires after step 13)
#   step 19 -- commit + push (Bash: git commit / git push)
#   step 20 -- Skill(review-todo-section) invocation
# Other steps are prose / TodoWrite / Edit-based and not enforced here.
# ----------------------------------------------------------------------------
_IMPLEMENT_TODO_SECTION: List[StepRule] = [
    (4,  "Bash",  r"codex-companion\.mjs.*?adversarial-review.*?\[review-kind:\s*design\b"),
    (7,  "Bash",  r"\bbash\s+(?:[^\"\']*?/)?scripts/build\.sh\b"),
    (13, "Bash",  r"codex-companion\.mjs.*?adversarial-review(?!.*?\[review-kind:\s*design)"),
    (13, "Skill", r"^codex-adversarial-review-section$"),
    (16, "Bash",  r"\bbash\s+(?:[^\"\']*?/)?scripts/build\.sh\b"),
    (19, "Bash",  r"\bgit\s+commit\b"),
    (20, "Skill", r"^review-todo-section$"),
]


# ----------------------------------------------------------------------------
# review-todo-section: pipeline phases.
#   step 4  -- first build
#   step 5  -- MANDATORY adversarial Codex dispatch ([review-kind: adversarial])
#   step 8a -- consistency Codex ([review-kind: consistency])
#   step 8b -- perf Codex ([review-kind: perf])
#   step 13.5 -- conditional re-adversarial ([review-kind: re-adversarial])
#   step 17 -- commit + push (review: prefix)
# ----------------------------------------------------------------------------
_REVIEW_TODO_SECTION: List[StepRule] = [
    (4,    "Bash", r"\bbash\s+(?:[^\"\']*?/)?scripts/build\.sh\b"),
    (5,    "Bash", r"codex-companion\.mjs.*?adversarial-review.*?\[review-kind:\s*adversarial\b"),
    (8,    "Bash", r"codex-companion\.mjs.*?adversarial-review.*?\[review-kind:\s*consistency\b"),
    (8,    "Bash", r"codex-companion\.mjs.*?adversarial-review.*?\[review-kind:\s*perf\b"),
    (13,   "Bash", r"codex-companion\.mjs.*?adversarial-review.*?\[review-kind:\s*re-adversarial\b"),
    (17,   "Bash", r"\bgit\s+commit\b"),
]


# ----------------------------------------------------------------------------
# verify-todo-section: same pipeline as review, downgrade-only stance.
# ----------------------------------------------------------------------------
_VERIFY_TODO_SECTION: List[StepRule] = list(_REVIEW_TODO_SECTION)


# ----------------------------------------------------------------------------
# complete-todo-file: full-file finalization.
#   build, test runner, smoke test, commit.
# ----------------------------------------------------------------------------
_COMPLETE_TODO_FILE: List[StepRule] = [
    (1, "Bash", r"\bbash\s+(?:[^\"\']*?/)?scripts/build\.sh\b"),
    (2, "Bash", r"\bbash\s+(?:[^\"\']*?/)?scripts/test\.sh\b"),
    (3, "Bash", r"\bbash\s+(?:[^\"\']*?/)?scripts/test-smoke\.sh\b"),
    (4, "Bash", r"\bgit\s+commit\b"),
]


# ----------------------------------------------------------------------------
# quality-review-section: industry standards / parity / superiority pass.
# Builds on review pipeline; minimal additional evidence.
# ----------------------------------------------------------------------------
_QUALITY_REVIEW_SECTION: List[StepRule] = [
    (1, "Bash", r"\bbash\s+(?:[^\"\']*?/)?scripts/build\.sh\b"),
    (2, "Bash", r"codex-companion\.mjs.*?adversarial-review"),
    (3, "Bash", r"\bgit\s+commit\b"),
]


SKILL_STEP_MAP: Dict[str, List[StepRule]] = {
    "implement-todo-section": _IMPLEMENT_TODO_SECTION,
    "review-todo-section":    _REVIEW_TODO_SECTION,
    "verify-todo-section":    _VERIFY_TODO_SECTION,
    "complete-todo-file":     _COMPLETE_TODO_FILE,
    "quality-review-section": _QUALITY_REVIEW_SECTION,
}


# Required terminal steps -- the gate refuses commits that lack any of these.
# Optional steps (e.g. design review on a docs-only section) are not listed
# here; they are observed when they happen but never demanded.
REQUIRED_TERMINAL_STEPS: Dict[str, List[int]] = {
    # implement: design optional (conditional skip), build mandatory, adversarial
    # mandatory, rebuild mandatory, commit mandatory.
    "implement-todo-section": [7, 13, 16, 19],
    # review: build, adversarial, consistency, perf, commit.
    "review-todo-section": [4, 5, 8, 17],
    "verify-todo-section": [4, 5, 8, 17],
    # complete: build, tests, commit. (Smoke test is conditional on boot-path).
    "complete-todo-file": [1, 2, 4],
    # quality: build, codex, commit.
    "quality-review-section": [1, 2, 3],
}


# Multi-step skill names -- the observer / blocker only act on these.
MULTI_STEP_SKILLS = frozenset(SKILL_STEP_MAP.keys())


def match_step(skill: str, tool_name: str, signature: str) -> List[int]:
    """Return the list of step numbers whose evidence rule matches the
    given (tool_name, signature). signature is the relevant tool_input
    field as documented at the StepRule type alias.
    """
    rules = SKILL_STEP_MAP.get(skill, [])
    matched = []
    for step_n, evidence_tool, pattern in rules:
        if evidence_tool != tool_name:
            continue
        if re.search(pattern, signature, re.S):
            matched.append(step_n)
    return matched
