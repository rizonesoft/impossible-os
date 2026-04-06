---
name: codex-design-review
description: Pre-implementation design review via Codex. Before writing code for a TODO section, have Codex review the plan for feasibility issues, missing edge cases, SMP hazards, and architectural problems. Catches design flaws before implementation time is spent. Use before implementing a complex or high-risk section.
---

# Codex Design Review

## Use This Skill When

- About to implement a TODO section that touches SMP-sensitive, boot-path, or security-critical code.
- The section's plan looks complex and you want a second opinion before writing code.
- The user asks for a design review or feasibility check on a planned section.

## Workflow

1. **Read the TODO section** -- full text, checklist items, test checkpoint, and all `-> XREF:` dependencies.
2. **Read the integration surface** -- use Grep/Glob to find the existing code that the section will modify or depend on. Identify key structs, APIs, and call paths.
3. **Build a design review prompt** covering:
   - The section's goal and all planned checklist items
   - The existing code it integrates with (file paths, key functions)
   - Known constraints (SMP safety, freestanding kernel, identity mapping, bare metal)
   - Specific questions: "Is this approach correct?", "What edge cases are missing?", "What could this break?"
4. **Dispatch to Codex plugin:**
   ```bash
   node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<design review prompt>"
   ```
   Frame as design review, not code review -- Codex should analyze the PLAN, not existing code.
5. **Evaluate findings:**
   - **Blocker** -- the plan has a fundamental flaw that would require redesign after implementation. Fix the TODO section before implementing.
   - **Warning** -- the plan is viable but has edge cases or risks to handle during implementation. Note them for the implementer.
   - **Clear** -- no significant issues found. Proceed to implementation.
6. **Update the TODO section** if blockers or warnings were found:
   - Add missing checklist items for edge cases Codex identified
   - Add `> [!WARNING]` callouts for risks
   - Adjust the approach if Codex found a fundamental flaw

## Prompt Template

```
Design review for TODO-XX §N: <section title>

Plan:
<paste checklist items>

Integration surface:
<list key files, functions, structs the section will modify>

Constraints:
- Freestanding kernel (no stdlib)
- SMP-safe from day one (2+ CPUs)
- Identity-mapped address space (for now)
- Bare metal is the acceptance criteria

Questions:
1. Is this approach architecturally sound?
2. What edge cases or failure modes are missing from the plan?
3. What existing functionality could this break (regressions)?
4. Are there SMP/concurrency hazards in the proposed design?
5. Are there simpler alternatives that achieve the same goal?
```

## Guardrails

- This is a PLAN review, not a code review. Do not dispatch if the code is already written -- use `codex-adversarial-review-section` instead.
- Do not implement code in this skill. Only modify the TODO section text.
- If Codex returns "all clear," proceed with implementation. Do not re-review endlessly.
