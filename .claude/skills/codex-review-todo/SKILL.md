---
name: codex-review-todo
description: Run a Codex adversarial review against all implemented sections of a TODO file. Identifies design flaws, race conditions, missing error handling, and assumption violations in the actual code behind completed checklist items.
---

# Codex Adversarial Review -- TODO Implementation Audit

> **External-Reviewer Contract:** Codex is a subordinate reviewer, not authority. Every finding from this skill goes through `superpowers:receiving-code-review` (verify at file:line, Fix / Reject / Accept, never blind-implement). Canonical contract: [docs/infrastructure/ai-system.md#external-reviewer-contract-codex-copilot](../../../docs/infrastructure/ai-system.md#external-reviewer-contract-codex-copilot).

## Use This Skill When

- A TODO file has completed sections (`[x]`) that need adversarial review against the actual code.
- The user wants to verify that implemented sections are production-quality, not just "builds and boots."
- After a batch of implementations, before marking a TODO as fully complete.
- For single-section scope, prefer `implement-todo-section` (embedded adversarial review + fix + section verification loop).

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
   node "$HOME/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<focus prompt>"
   ```
   Run in background for large reviews (> 3 sections).
5. **Collect and present findings** verbatim from Codex output.
6. **Triage findings** -- the PostToolUse hook fires `receiving-code-review` reminder; follow it. Verify each cited line against the actual code; YAGNI-check missing-functionality flags by grepping for callers first.
7. **Classify each verified finding:**
   - **Fix now** -- verified correctness bug, race condition, use-after-free, data loss risk
   - **Reject** -- finding is wrong; document the technical reason with code evidence (caller already holds lock X, path is single-threaded, etc.)
   - **Accepted risk** -- finding is valid but documented limitation with explicit justification (e.g., "ATA is void/legacy")
8. **Present findings in the conversation only -- do NOT write them into the TODO file.**
   - Display each finding with severity, one-line summary, file reference, classification, and (for rejections) the code evidence.
   - No performative agreement -- never frame findings as "great catches"; just present them factually.
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
