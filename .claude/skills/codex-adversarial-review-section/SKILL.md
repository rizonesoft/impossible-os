---
name: codex-adversarial-review-section
description: Run adversarial code review for one implemented TODO section, fix findings, and re-review up to 3 rounds until no unresolved High/Critical issues remain.
---

# Codex Adversarial Review -- Section Loop

> **External-Reviewer Contract:** Codex is a subordinate reviewer, not authority. Every finding from this skill goes through `superpowers:receiving-code-review` (verify at file:line, Fix / Reject / Accept, never blind-implement). Canonical contract: [docs/infrastructure/ai-system.md#external-reviewer-contract-codex](../../../docs/infrastructure/ai-system.md#external-reviewer-contract-codex).

## Use This Skill When

- You just finished implementing a TODO section and need adversarial review before committing.
- The `/implement-todo-section` pipeline reaches Stage 6 (it invokes this skill).
- The user asks "review this section" or "find bugs in §N."
- You want to stress-test a bounded set of changed files for a single section's work.
- Do NOT use for full-TODO review -- use `/codex-review-todo` instead.

## Workflow

1. Scope the review to one implemented section (`§N`) and its changed files/symbols.
2. Extract ALL adversarial angles to challenge. The review MUST be full-spectrum, not limited to one category (e.g., security-only). Every dispatch must cover ALL of:
   - **Concurrency/SMP safety** -- new shared mutable state, lock ordering, atomics, per-CPU data races
   - **Race conditions** -- TOCTOU between check and use, concurrent access to shared data without locks, interrupt-context vs thread-context conflicts
   - **Error-path handling and rollback** -- partial state on failure, resource leaks, cleanup, what happens when allocation fails mid-operation
   - **Regressions** -- did the change break any existing functionality? Callers of modified functions, struct layout changes affecting other files, removed/renamed symbols
   - **Wiring/registration correctness** -- SSDT/dispatch/hooks, format registration, magic matching, init ordering
   - **Boundary and ABI assumptions** -- integer overflow, unaligned access, struct packing, type-pun UB, sign extension
   - **Memory safety** -- buffer overflows, use-after-free, double-free, null pointer dereference, uninitialized reads, kernel vs user pointer confusion
   - **Functional correctness** -- does the code do what the spec says? Entry points, return values, edge cases, off-by-one errors
   - **Security** -- can crafted input (malicious binary, bad syscall args) corrupt kernel state, escalate privilege, or cause denial of service?
   - **Code quality** -- dead code, unused variables, missing const, style, magic numbers without defines
   - **Performance** -- unnecessary allocations, O(n^2) where O(n) suffices, byte-by-byte where bulk ops exist, hot-path bloat
   - **Bare metal correctness** -- MMIO caching attributes, TLB flush scope (local vs IPI shootdown), CPUID-gated instructions, identity mapping assumptions
   This was learned the hard way: a security-only review of §5 missed entry-point range validation (F-06) and dead code (F-08) which required a supplemental full-spectrum review.
3. Dispatch adversarial review via the Codex plugin.
   - Command:
     ```bash
     node "$HOME/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<focus prompt>"
     ```
   - Include the exact file paths and line ranges in the focus prompt.
   - List ALL adversarial angles from step 2 in the prompt -- do not omit any.
   - Request findings by severity: `Critical`, `High`, `Medium`, `Low`.
   - Run in background for large reviews (> 3 files).
4. Triage findings -- the new `receiving_review_required.py` PreToolUse hook BLOCKS any subsequent `Edit` / `Write` / `MultiEdit` until you invoke `Skill(name="superpowers:receiving-code-review")` to triage the findings. The block is the hard gate; the existing PostToolUse `systemMessage` reminder is the secondary signal. State lives in `.claude/state/last-codex-review.json` (gitignored, schema documented in `.claude/state/README.md`); 1 hour TTL stale-pass; opt-out via `RECEIVING_REVIEW_OVERRIDE=1` env var on the same call (the agent must state what code-evidence quote justifies skipping). **Adversarial-review false-positive watch:** Codex reads code without runtime context and frequently flags "missing lock" when the caller already holds it, "race" on paths single-threaded by construction (BSP boot phase 0), or "buffer overflow" on buffers static-asserted larger than the access. Verify at file:line before classifying. Priority for valid findings: always fix `Critical` and `High`; fix `Medium` unless accepted with a concrete technical reason.
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
- Always use the Codex plugin for the review -- self-review has implementation bias and misses things that a fresh reader catches (proven: §1 Codex found 2 Critical issues self-review missed).
