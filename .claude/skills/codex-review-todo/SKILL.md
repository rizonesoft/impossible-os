---
name: codex-review-todo
description: Run a Codex adversarial review against all implemented sections of a TODO file. Identifies design flaws, race conditions, missing error handling, and assumption violations in the actual code behind completed checklist items.
---

# Codex Adversarial Review -- TODO Implementation Audit

## Use This Skill When

- A TODO file has completed sections (`[x]`) that need adversarial review against the actual code.
- The user wants to verify that implemented sections are production-quality, not just "builds and boots."
- After a batch of implementations, before marking a TODO as fully complete.

## Workflow

1. **Read the full TODO file.** Identify all sections marked `[x]` or `[/]` (implemented).
2. **For each implemented section**, extract:
   - The source files it modified (from the section's `**Files:**` line or checklist items)
   - The key functions, types, and APIs it introduced
   - The design claims it makes (e.g., "SMP-safe", "lock-free", "no allocation in ISR")
3. **Build a focused review prompt** listing:
   - Every implemented source file
   - Every design claim that needs adversarial challenge
   - Every cross-section dependency (e.g., "§5 depends on §3's lock being held")
4. **Run the Codex adversarial review** via:
   ```bash
   node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<focus prompt>"
   ```
   Run in background for large reviews (> 3 sections).
5. **Collect and present findings** verbatim from Codex output.
6. **Classify each finding:**
   - **Fix now** -- correctness bug, race condition, use-after-free, data loss risk
   - **Accepted risk** -- documented limitation with explicit justification (e.g., "ATA is void/legacy")
7. **Present findings in the conversation only -- do NOT write them into the TODO file.**
   - Display each finding with severity, one-line summary, file reference, and classification.
   - The fix skill (`/codex-fix-review`) is responsible for writing the final compact summary table into the TODO after resolution. The review skill never modifies the TODO.

## Focus Prompt Template

```
Review the implemented sections of [TODO file]. Focus on:
- [list source files]
- Design claims: [list claims from section prose]
- Cross-section dependencies: [list]
- SMP safety: any shared mutable state without synchronization?
- Error paths: can init fail silently? Are return values checked?
- Boot ordering: does this code run before/after sti? Before/after scheduler?
```

## Guardrails

- Do NOT fix code in this skill -- only identify issues.
- Do NOT mark findings as resolved without evidence (that's the fix skill's job).
- Present Codex output verbatim -- do not soften or reinterpret findings.
- If Codex returns "all-clear", note it but remain skeptical -- re-check one high-risk area manually.
- Skip sections marked `[ ]` (not implemented) -- nothing to review.
