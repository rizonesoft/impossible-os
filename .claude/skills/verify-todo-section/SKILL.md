---
name: verify-todo-section
description: Verify an already-implemented TODO section through the full quality pipeline without implementing anything new. Runs Codex adversarial review, test coverage analysis, self-review checklist walk, section validation, and loose-end scan. Use to audit completed work, re-verify after upstream changes, or confirm a section is truly done before marking a TODO complete.
---

# Verify TODO Section

## Use This Skill When

- A section is marked `[x]` and you want to confirm it's truly complete.
- Upstream code changed (refactor, dependency update) and you need to re-verify a section.
- The user asks "is this section really done?" or "verify §N for me."
- Before running `/complete-todo` to graduate a TODO to documentation.
- Auditing a section implemented in a previous session.

## Workflow

### Phase 1 -- Understand What Was Implemented

1. **Read the section** -- full text, checklist items, test checkpoint, warning boxes, `-> XREF:` lines.
2. **Resolve dependencies** -- verify every `-> XREF:` target is still valid and still `[x]`. If a dependency regressed, flag it.
3. **Explore the implementation** -- Grep/Glob for every symbol, function, and type referenced in the checklist items. Read the source files. Understand what was actually built vs what the checklist claims.

### Phase 2 -- Codex Reviews (same as implement steps 9, 13-14)

4. **Codex test coverage analysis** -- dispatch to Codex plugin. List every public function from the section and every existing test assertion. Ask Codex to find untested error paths, boundaries, and negative cases. If gaps found, add missing tests (this is the ONE implementation action allowed).
    ```bash
    node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<test coverage prompt>"
    ```
5. **Codex adversarial review** (MANDATORY) -- dispatch to Codex plugin. Scope to the section's changed files/symbols. Full-spectrum: SMP, race conditions, error paths, regressions, boundaries, memory safety, security, functional correctness, performance, bare metal. Request findings by severity.
    ```bash
    node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<focus prompt>"
    ```
6. **Fix loop** (max 3 rounds) -- apply `superpowers:receiving-code-review` discipline: do NOT blindly fix every Codex finding. For each finding, verify it technically first -- Codex can be wrong. If valid, fix root cause. If wrong, reject with code evidence. If valid but out of scope, accept with justification. Rebuild after each fix. Re-review via Codex focusing on previous findings.

### Phase 3 -- Self-Review (same as implement step 15)

7. **Final self-review** (MANDATORY):
    - Regressions: did any existing functionality break since this was implemented?
    - Race conditions: any shared mutable state without synchronization?
    - Bugs: edge cases, off-by-one, null pointer paths?
    - Performance: unnecessary allocations, O(n^2) where O(n) suffices?
    - Industry standards: no hacks, no patches, no workarounds, no TODO/FIXME.
    - **Checklist item-by-item:** walk every `- [x]` item. For each: is it STILL implemented, wired, and functional? Could upstream changes have broken it?
    - **Test checkpoint verification:** does the test checkpoint's expected serial output / POST codes / behavior still hold?

### Phase 4 -- Codex Deep Analysis

8. **Codex consistency audit** -- dispatch to Codex plugin. Verify that struct offsets, constants, API contracts, and registration tables match between the section's header, implementation, test, and assembly files. Follow the `codex-consistency-audit` skill. Fix any mismatches found.
    ```bash
    node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<consistency prompt>"
    ```
9. **Codex dead code scan** -- dispatch to Codex plugin. Find unreachable functions, unused `#define`s, orphaned types, and stale declarations in the section's source files. Follow the `codex-dead-code` skill. Remove safe-to-remove items, rebuild.
    ```bash
    node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<dead code prompt>"
    ```
10. **Codex performance review** (for hot-path sections) -- if the section touches ISR paths, scheduler, compositor, spinlock-protected regions, or per-tick code, dispatch a performance review. Follow the `codex-perf-review` skill. Fix critical/high findings. Skip for non-hot-path sections (struct definitions, metadata parsers, test-only code).
    ```bash
    node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<perf review prompt>"
    ```

11. **SSDT audit** (auto-triggered for SSDT sections) -- if the section's checklist items reference `ssdt_register()`, SSDT service numbers, `NtXxx` functions, or the TODO is TODO-05/TODO-12, invoke the `audit-ssdt` skill. This verifies service number <-> function <-> registration <-> master table row consistency. Fix any mismatches found. Skip for sections with no SSDT involvement.

### Phase 5 -- Validate and Reconcile

11. **Build** -- `bash scripts/build.sh`, confirm `=== BUILD OK ===`.
12. **Validate section** (MANDATORY) -- invoke `validate-todo-section` skill. Evidence-based checklist classification. Catches stale/optimistic status claims.
13. **Tie up loose ends** -- scan for:
    - Deferred items pointing to this section that weren't resolved.
    - Stale warning boxes that should be updated.
    - Cross-TODO dependency notes that are now satisfied or broken.
    - Implementation Order / OS Comparison rows that need status updates.
14. **Report findings** to the user:
    - **PASS** -- section is verified, all items confirmed, no issues found.
    - **PASS with fixes** -- section verified after fixing N issues found by Codex reviews.
    - **FAIL** -- section has unresolved Critical/High issues. List what needs work.

### Phase 6 -- Commit (only if fixes were made)

15. **Commit and push** -- only if any phase produced code fixes, test additions, or dead code removal. Use message: `"fix: verify §N -- <summary of fixes>"`. If no changes were made, skip this step.

## What This Skill Does NOT Do

- Does NOT implement new checklist items. If a checklist item is `[ ]`, it stays `[ ]`.
- Does NOT create new source files (except test additions from step 4).
- Does NOT modify the section's checklist text (except fixing stale `[x]` -> `[ ]` if evidence shows regression).
- Does NOT run the design review (step 4 of implement) -- the code already exists.
- Does NOT run kernel-code-quality gates -- the code already exists.

## Guardrails

- Do not implement missing features. If something is unimplemented, flag it and keep `[ ]`.
- Do not weaken tests to make findings go away.
- The Codex adversarial review (step 5) is MANDATORY. No exceptions for "simple" sections.
- If the section was never implemented (all `[ ]`), this skill is wrong -- use `/implement-todo-section` instead.
- Commit only if actual code/test changes were made during verification.
