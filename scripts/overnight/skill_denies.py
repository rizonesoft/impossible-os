#!/usr/bin/env python3
"""Does a permission deny rule refuse a skill the sequencer's pipeline needs?

v22, 2026-11-07. `~/.claude/settings.json` denied `Skill(review *)`, aimed at the
built-in /review. The matcher reads `name *` as a PREFIX, so it also refused
`review-todo-section`, and no section could ship for a whole run while every
segment reported a clean checkpoint. Measured by a fresh-session probe: with the
rule, `Skill execution blocked by permission rules`; without it, the skill loads.

A deny is the operator's to change, so nothing here edits one. The check runs at
arm time (refuse) and at segment end (trip the breaker), because no amount of
relaunching gets past a permission rule.

Usage: skill_denies.py [PROJECT_DIR]   exit 0 clear, 1 a pipeline skill is denied.
"""
from __future__ import annotations

import json
import os
import re
import sys
from pathlib import Path

_RULE_RE = re.compile(r"^Skill\((.+)\)$")
# Named here as well as in PHASE_ALLOWED_SKILLS: the review step is reached through
# them, and CLAUDE.md's mandatory triggers name the reception skill.
_ALSO_REQUIRED = {"implement-todo-section", "implement-todo-item",
                  "review-todo-section", "complete-todo-file",
                  "codex-adversarial-review-section",
                  "superpowers:receiving-code-review"}


def rule_matches(rule: str, skill: str) -> bool:
    m = _RULE_RE.match(rule.strip())
    if not m:
        return rule.strip() == "Skill"
    pat = m.group(1).strip()
    for suffix in (" *", ":*", "*"):
        if pat.endswith(suffix):
            return skill.startswith(pat[: -len(suffix)])
    return skill == pat


def _deny_rules(project: Path) -> list:
    rules = []
    for p in (Path(os.path.expanduser("~/.claude/settings.json")),
              project / ".claude/settings.json",
              project / ".claude/settings.local.json"):
        try:
            data = json.loads(p.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue  # fail-direction: an absent layer denies nothing
        rules += [r for r in (data.get("permissions") or {}).get("deny") or []
                  if isinstance(r, str)]
    return rules


def _pipeline_skills(project: Path) -> set:
    skills = set(_ALSO_REQUIRED)
    guard = project / ".claude/hooks/run_phase_guard.py"
    try:
        text = guard.read_text(encoding="utf-8")
    except OSError:
        return skills  # fail-direction: the fixed list still covers the ship path
    body = re.search(r"^PHASE_ALLOWED_SKILLS = \{(.*?)^\}", text, re.M | re.S)
    if body:
        skills |= set(re.findall(r'"([a-z][a-z0-9:-]+)"', body.group(1)))
    return skills


def denied(project: Path) -> list:
    """[(skill, rule)] for every pipeline skill a deny rule refuses."""
    out = []
    rules = _deny_rules(project)
    for skill in sorted(_pipeline_skills(project)):
        for rule in rules:
            if rule_matches(rule, skill):
                out.append((skill, rule))
                break
    return out


def main(argv: list) -> int:
    project = Path(argv[0] if argv else ".").resolve()
    hits = denied(project)
    for skill, rule in hits:
        print(f"{skill} is refused by the deny rule {rule}")
    return 1 if hits else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
