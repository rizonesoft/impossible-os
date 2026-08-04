#!/usr/bin/env python3
# Per-skill step-evidence map for the skill-step-state telemetry system
# (doctrine: TODO-08 §10; the inline "§10" label was retired 2026-04-30
# because section numbers drift across gap audits + renumbering).
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
#   step 9  -- Codex test-coverage dispatch (OPTIONAL on trivial sections)
#   step 13 -- MANDATORY Codex adversarial dispatch (canonical marker:
#              adversarial; adversarial-impl is an accepted alias/variant
#              retained for older prompt-shape references)
#   step 16 -- second build (Bash: bash scripts/build.sh, fires after step 13)
#   step 19 -- commit + push (Bash: git commit / git push)
#   step 20 -- Skill(review-todo-section) invocation OR the inline review
#              pipeline equivalent (consistency / perf / re-adversarial
#              dispatches that prove the post-impl review pipeline ran
#              without explicit Skill(review-todo-section) invocation;
#              the OS-level Codex hooks + the section-commit four-dispatch
#              gate independently enforce the dispatch evidence, so the
#              observer just records that step 20 happened).
# Coverage closes TODO-08 §25: all 7 canonical review-kind markers
# (design / adversarial / adversarial-impl / test-coverage / consistency
# / perf / re-adversarial) now bind to step numbers; the prior shape
# silently dropped the 5 non-{design, adversarial} kinds during implement
# flow.
# ----------------------------------------------------------------------------
_IMPLEMENT_TODO_SECTION: List[StepRule] = [
    (4,  "Bash",  "__REVIEW_KIND__:design"),
    (7,  "Bash",  r"\bbash\s+(?:[^\"\']*?/)?scripts/build\.sh\b"),
    (9,  "Bash",  "__REVIEW_KIND__:test-coverage"),
    # Step 13 uses adversarial as the canonical implementation-time marker;
    # adversarial-impl remains an accepted alias/variant for older prompts.
    (13, "Bash",  "__REVIEW_KIND__:adversarial"),
    (13, "Bash",  "__REVIEW_KIND__:adversarial-impl"),
    (13, "Skill", r"^codex-adversarial-review-section$"),
    (16, "Bash",  r"\bbash\s+(?:[^\"\']*?/)?scripts/build\.sh\b"),
    (19, "Bash",  r"\bgit\s+commit\b"),
    (20, "Bash",  "__REVIEW_KIND__:consistency"),
    (20, "Bash",  "__REVIEW_KIND__:perf"),
    (20, "Bash",  "__REVIEW_KIND__:re-adversarial"),
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
    (5,    "Bash", "__REVIEW_KIND__:adversarial"),
    # adversarial-impl is accepted as an implementation-time alias/variant;
    # if it fires during the review pipeline (rare but legitimate when the
    # reviewer asks Codex to re-evaluate the diff under implementation eyes)
    # it also satisfies step 5 evidence. TODO-08 §25 coverage extension.
    (5,    "Bash", "__REVIEW_KIND__:adversarial-impl"),
    (8,    "Bash", "__REVIEW_KIND__:consistency"),
    (8,    "Bash", "__REVIEW_KIND__:perf"),
    # test-coverage dispatches during the review pipeline are observed
    # at step 8 (the broader quality-audit phase); the codex-test-coverage
    # skill is also valid evidence for step 8 via the Skill matcher below.
    (8,    "Bash", "__REVIEW_KIND__:test-coverage"),
    (8,    "Skill", r"^codex-test-coverage$"),
    (13,   "Bash", "__REVIEW_KIND__:re-adversarial"),
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
# gap-audit-todo: TODO-level gap audit. Phase 3.5 mandates a Codex
# red-team dispatch (`[review-kind: gap-audit]`) between Phase 3 (gap
# detection) and Phase 4 (add missing sections). The dispatch is the
# only mechanical-evidence step in the whole skill -- everything else
# is research, classification, and TODO editing without a stable tool
# signature. We bind the dispatch to step 14 (the section-size audit
# step that immediately precedes Phase 3.5) since the SKILL.md numbers
# the dispatch as 14.5 and the StepRule integer key has to be a whole
# number.
# ----------------------------------------------------------------------------
_GAP_AUDIT_TODO: List[StepRule] = [
    (14, "Bash", "__REVIEW_KIND__:gap-audit"),
]


# ----------------------------------------------------------------------------
# validate-todo-file: structural audit. The skill produces no Codex
# dispatch / build / commit -- the only mechanical signal is Edit/Write
# on the target TODO file. Telemetry-only (no terminal-step requirement
# in REQUIRED_TERMINAL_STEPS) -- the observer records that the skill
# ran without blocking any commit on it. todo-pipeline orchestrates
# stronger gating around it.
# ----------------------------------------------------------------------------
_VALIDATE_TODO_FILE: List[StepRule] = [
    (1, "Edit",  r"todo/[^/]+/TODO-[^/]+\.md$"),
    (1, "Write", r"todo/[^/]+/TODO-[^/]+\.md$"),
]


# ----------------------------------------------------------------------------
# todo-pipeline: 3-stage TODO preparation orchestrator.
#   Stage 1: Skill(validate-todo-file)   -- structural cleanup
#   Stage 2: Skill(gap-audit-todo)       -- Win11/Linux research +
#            mandatory Codex red-team via [review-kind: gap-audit]
#   Stage 3: Skill(validate-todo-file)   -- post-edit sanity check
# Stages 1 and 3 fire the same Skill call -- the observer cannot
# distinguish them. We gate only on Stage 2 (the load-bearing
# competitive-completeness pass), and the Codex gap-audit dispatch
# inside Stage 2 produces a separate Bash signal that doubles as
# the strong-form gate.
# ----------------------------------------------------------------------------
_TODO_PIPELINE: List[StepRule] = [
    (1, "Skill", r"^validate-todo-file$"),
    (2, "Skill", r"^gap-audit-todo$"),
    (2, "Bash",  "__REVIEW_KIND__:gap-audit"),
]


# ----------------------------------------------------------------------------
# quality-review-section: industry standards / parity / superiority pass.
# Builds on review pipeline; minimal additional evidence.
# ----------------------------------------------------------------------------
# ----------------------------------------------------------------------------
# implement-todo-item: single-checklist-item scope.
#   step 6  -- first build (Bash: bash scripts/build.sh)
#   step 9  -- MANDATORY Codex adversarial dispatch (trivial-fix opt-out via
#              SKIP_CODEX_REVIEW=1 + reason; the gate hook honors the env)
#   step 11 -- second build after fixes (also Bash: bash scripts/build.sh;
#              the observer records both step 6 and step 11 from the same
#              regex -- the second occurrence in the conversation transcript
#              flips step 11 satisfied)
#   step 14 -- commit (Bash: git commit). Push is a separate Bash signal but
#              not gated; commit is the load-bearing action.
# Note: step 16 (auto-promotion to /review-todo-section when the item closes
# the section) is a CONDITIONAL terminal step. The section_review_required
# hook independently enforces it post-commit when the IO row flips, so we do
# not double-gate here.
# ----------------------------------------------------------------------------
_IMPLEMENT_TODO_ITEM: List[StepRule] = [
    (6,  "Bash", r"\bbash\s+(?:[^\"\']*?/)?scripts/build\.sh\b"),
    (9,  "Bash", "__REVIEW_KIND__:adversarial"),
    (9,  "Bash", "__REVIEW_KIND__:adversarial-impl"),
    (9,  "Skill", r"^codex-adversarial-review-section$"),
    (11, "Bash", r"\bbash\s+(?:[^\"\']*?/)?scripts/build\.sh\b"),
    (14, "Bash", r"\bgit\s+commit\b"),
]


_QUALITY_REVIEW_SECTION: List[StepRule] = [
    (1, "Bash", r"\bbash\s+(?:[^\"\']*?/)?scripts/build\.sh\b"),
    # Quality-review Codex dispatches are adversarial-class; bind via the
    # canonical sentinel so typo'd or marker-less dispatches route to the
    # observer's typo-WARN path instead of getting silent step credit.
    # Closes the post-impl Codex re-adversarial M3 finding (broad regex
    # bypassed the marker validation contract).
    (2, "Bash", "__REVIEW_KIND__:adversarial"),
    (3, "Bash", r"\bgit\s+commit\b"),
]


SKILL_STEP_MAP: Dict[str, List[StepRule]] = {
    "implement-todo-section": _IMPLEMENT_TODO_SECTION,
    "implement-todo-item":    _IMPLEMENT_TODO_ITEM,
    "review-todo-section":    _REVIEW_TODO_SECTION,
    "verify-todo-section":    _VERIFY_TODO_SECTION,
    "complete-todo-file":     _COMPLETE_TODO_FILE,
    "quality-review-section": _QUALITY_REVIEW_SECTION,
    "gap-audit-todo":         _GAP_AUDIT_TODO,
    "validate-todo-file":     _VALIDATE_TODO_FILE,
    "todo-pipeline":          _TODO_PIPELINE,
}


# Required terminal steps -- the gate refuses commits that lack any of these.
# Optional steps (e.g. design review on a docs-only section) are not listed
# here; they are observed when they happen but never demanded.
REQUIRED_TERMINAL_STEPS: Dict[str, List[int]] = {
    # implement: design optional (conditional skip), build mandatory, adversarial
    # mandatory, rebuild mandatory, commit mandatory.
    "implement-todo-section": [7, 13, 16, 19],
    # implement-item: build, adversarial, rebuild, commit. Step 16
    # (post-commit /review-todo-section auto-invoke) is gated separately
    # by section_review_required.py when the item closes a section.
    "implement-todo-item": [6, 9, 11, 14],
    # review: build, adversarial, consistency, perf, commit.
    "review-todo-section": [4, 5, 8, 17],
    "verify-todo-section": [4, 5, 8, 17],
    # complete: build, tests, commit. (Smoke test is conditional on boot-path).
    "complete-todo-file": [1, 2, 4],
    # quality: build, codex, commit.
    "quality-review-section": [1, 2, 3],
    # gap-audit: only the codex-gap-audit dispatch is mechanically
    # checkable; the rest of the skill is research + TODO editing.
    "gap-audit-todo": [14],
    # validate-todo-file: telemetry only (no terminal-step gate). The
    # skill is structural cleanup; gating it would block harmless
    # standalone validations. Stronger gating happens via todo-pipeline
    # when validate-todo-file runs as part of the orchestrated flow.
    "validate-todo-file": [],
    # todo-pipeline: Stage 2 (gap-audit-todo + its Codex dispatch) is
    # the load-bearing step. Stages 1 and 3 fire the same Skill call
    # and are recorded as step 1; gating step 1 would force every
    # pipeline run to invoke validate-todo-file even when it already
    # ran in a prior session, so we only require step 2.
    "todo-pipeline": [2],
}


# Multi-step skill names -- the observer / blocker only act on these.
MULTI_STEP_SKILLS = frozenset(SKILL_STEP_MAP.keys())


# Sentinel patterns used in SKILL_STEP_MAP for [review-kind: X] dispatches.
# Replaces unanchored regex with a first-non-blank-line classifier
# delegated to _review_kind.detect_review_kind_from_cmd. Closes the
# drift named in TODO-08 §17 deferred-XREF M2 (step-5 telemetry vs
# phase1_evidence_gate marker-rule mismatch).
_SENTINEL_REVIEW_KIND_PREFIX = "__REVIEW_KIND__:"


def bound_review_kinds(skill: str) -> List[str]:
    """Return the canonical review-kind markers that bind to at least one
    step number under `skill`. Used by skill_step_observer.py to
    distinguish 'kind is canonical but the active skill doesn't bind it'
    (silent fallthrough) from 'kind is non-canonical / typo' (loud WARN).
    Closes the section-25 M1 finding from the post-impl Codex pipeline.
    """
    kinds: List[str] = []
    for _step_n, _evidence_tool, pattern in SKILL_STEP_MAP.get(skill, []):
        if pattern.startswith(_SENTINEL_REVIEW_KIND_PREFIX):
            kind = pattern[len(_SENTINEL_REVIEW_KIND_PREFIX):]
            if kind not in kinds:
                kinds.append(kind)
    return kinds


def match_step(skill: str, tool_name: str, signature: str) -> List[int]:
    """Return the list of step numbers whose evidence rule matches the
    given (tool_name, signature). signature is the relevant tool_input
    field as documented at the StepRule type alias.

    For sentinel-prefixed patterns of shape `__REVIEW_KIND__:<kind>`,
    delegate to the shared first-non-blank-line classifier in
    _review_kind. Plain regex patterns continue to use re.search.
    """
    rules = SKILL_STEP_MAP.get(skill, [])
    matched = []
    detect_kind = None
    for step_n, evidence_tool, pattern in rules:
        if evidence_tool != tool_name:
            continue
        if pattern.startswith(_SENTINEL_REVIEW_KIND_PREFIX):
            if detect_kind is None:
                # ALL kinds in the command, not just the leading one: several
                # dispatches routinely share a single Bash call, and matching
                # only the first meant a performed step read as never observed.
                from _review_kind import detect_review_kinds_from_cmd
                detect_kind = detect_review_kinds_from_cmd(signature)
            want = pattern[len(_SENTINEL_REVIEW_KIND_PREFIX):]
            if want in detect_kind:
                matched.append(step_n)
            continue
        if re.search(pattern, signature, re.S):
            matched.append(step_n)
    return matched
