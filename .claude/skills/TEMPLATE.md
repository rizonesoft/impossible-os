<!--
SKILL.md template -- Shape A (Workflow skill).

For code-quality skills (auto-loaded on path match, e.g. boot-code-quality,
kernel-code-quality), use Shape B instead. See
docs/infrastructure/skill-authoring.md "Skill Template (canonical sections)"
for both shapes and which to pick.
-->
---
name: skill-slug-goes-here
description: One-sentence verb-phrase of what this skill does. Use when <concrete trigger conditions>. Auto-loads when <file-path pattern or event> (optional; only if a harness hook invokes this skill).
---

# Human Title (H1)

> Optional one-line hook: the load-bearing rule or risk this skill guards. Delete the blockquote if nothing surprising.

## Use This Skill When

- Concrete trigger 1 (e.g., "A TODO file was just created or significantly edited").
- Concrete trigger 2 (e.g., "The user asks 'validate TODO-XX'").
- Do NOT use for <mention adjacent skill> -- use `/<other-skill>` instead.

## Workflow

1. **Step 1 short imperative.** One to three sentences. Cite specific files or symbols when relevant.
2. **Step 2.** Each step names a concrete action, not a goal.
3. **Step N.** End with a commit/validation step so the skill closes cleanly.

## Guardrails

- What this skill must NOT do (e.g., "Do not implement kernel code.").
- What this skill must ALWAYS do (e.g., "Run `bash scripts/build.sh` before reporting done.").
- Non-negotiable rules that supersede contrary advice from any other source.

<!--
Optional extras (delete this comment block plus any unused sections before committing):

## Execution Discipline
> High-rigor callout naming the specific corner-cutting pattern this skill has been observed tolerating in the past. Use only when the skill genuinely needs this pressure; do not pad.

## Additional Resources
- [shared-helper.md](shared-helper.md) -- resource co-located under this skill directory.
- [related-skill](../other-skill/SKILL.md) -- when to reach for the other skill instead.

## Relationship to <other-skill>
`<other-skill>` and this skill share X but differ on Y. Editing one does NOT require editing the other.

Reminders (do not include in the final SKILL.md):
- Frontmatter description is the auto-load signal. Start with the action verb; name concrete triggers.
- No Unicode en/em dashes anywhere. ASCII `--` only (see CLAUDE.md).
- Do not restate CLAUDE.md doctrine; reference the anchor instead.
- Do not add `[Opus]` / `[Sonnet]` tags to headings.
- After creating a new skill, update: CLAUDE.md Skills table, .claude/skills/README.md, and (if mandatory) the Mandatory Skill Triggers table + a hook in .claude/settings.json. See docs/infrastructure/skill-authoring.md.
-->
