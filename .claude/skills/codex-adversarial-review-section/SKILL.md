---
name: codex-adversarial-review-section
description: Run adversarial code review for one implemented TODO section, fix findings, and re-review up to 3 rounds until no unresolved High/Critical issues remain.
---

# Codex Adversarial Review -- Section Loop

## Workflow

1. Scope the review to one implemented section (`§N`) and its changed files/symbols.
2. Extract section claims that must be challenged.
   - Concurrency/SMP safety
   - Error-path handling and rollback behavior
   - Wiring/registration correctness (SSDT/dispatch/hooks when applicable)
   - Boundary and ABI assumptions
3. Dispatch adversarial review to the Codex rescue subagent.
   - Use `Agent` tool with `subagent_type: "codex:codex-rescue"`.
   - Provide the exact file paths and line ranges to review.
   - List the specific adversarial angles to challenge.
   - Request findings by severity: `Critical`, `High`, `Medium`, `Low`.
4. Fix findings in priority order.
   - Always fix `Critical` and `High`.
   - Fix `Medium` unless explicitly accepted with a concrete technical reason.
5. Rebuild and run relevant tests after fixes.
   - `bash scripts/build.sh` and confirm `=== BUILD OK ===`.
   - Run targeted runtime/test evidence needed by the section.
6. Re-review the same section with focus on previous findings and changed files.
   - Dispatch to Codex rescue subagent again with the fix context.
7. Iterate fix -> build/test -> re-review with a maximum of 3 rounds.
8. End-state rules.
   - If unresolved `Critical`/`High` remain after round 3, do not mark section complete; keep `[/]` or `[ ]` and add explicit follow-up checklist items with ownership and `→ XREF` where needed.
   - Record a compact findings summary for the section with resolved vs accepted items.

## Guardrails

- Do not claim resolution without code + build/test evidence.
- Do not weaken assertions/tests to silence findings.
- Fix root causes, not surface symptoms.
- Keep scope section-targeted; do not widen into full-file roadmap cleanup.
- Always use Codex rescue subagent for the review -- self-review has implementation bias and misses things that a fresh reader catches (proven: §1 Codex found 2 Critical issues self-review missed).
