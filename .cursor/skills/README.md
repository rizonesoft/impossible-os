# Cursor skills (TODO workflows only)

This repo keeps **only** TODO roadmap workflows in Cursor:

| Skill | Path | Role |
| --- | --- | --- |
| Validate full TODO file | `validate-todo-file/SKILL.md` | Structure, Implementation Order, XREFs, parity |
| Validate one section | `validate-todo-section/SKILL.md` | Evidence vs code for a single `## N.` section |
| Gap analysis | `gap-analysis-todo/SKILL.md` | Win11/Linux research, overlaps, ownership |

Implementation, SSDT wiring, unit-test authoring, Codex/Copilot review pipelines, and related skills live under **`.claude/skills/`** for Claude Code. Do **not** rsync the full `.claude/skills/` tree into `.cursor/skills/` unless you intend to restore the wide mirror.

Rules: `.cursor/rules/todo-validate-gap-workflows.mdc`, `.cursor/rules/todo-workflows-always-pointer.mdc`. Hook: `beforeReadFile` on `todo/**/TODO-*.md` via `.cursor/hooks/todo-read-reminder.py`.
