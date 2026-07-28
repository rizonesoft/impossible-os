---
name: codex-fix-review
description: Fix all findings from a Codex adversarial review, then re-run the review until Codex confirms resolution. Iterates fix-review-fix until all high/medium findings are resolved or tracked.
---

# Codex Fix and Re-Review Loop

> **External-Reviewer Contract:** Codex is a subordinate reviewer, not authority. Every finding from this skill goes through `superpowers:receiving-code-review` (verify at file:line, Fix / Reject / Accept, never blind-implement). Canonical contract: [docs/infrastructure/ai-system.md#external-reviewer-contract-codex](../../../docs/infrastructure/ai-system.md#external-reviewer-contract-codex).

## Prompt Shape

Every dispatch from this skill MUST open its prompt with the marker `[review-kind: re-adversarial] <todo-path>` on the first non-blank line. The marker is what `.claude/hooks/skill_step_observer.py` and the section-commit four-dispatch gate use to attribute the dispatch. Un-marked dispatches waste a Codex round and block the next section-commit. Canonical reference for all 8 markers: [.claude/skills/codex-prompt-shape.md](../codex-prompt-shape.md).

## Use This Skill When

- A Codex adversarial review has returned findings that need fixing.
- The user says "fix the Codex findings" or "resolve the review issues."
- After `/codex-review-todo` produces a needs-attention verdict.
- For section-only scope, prefer `implement-todo-section` (includes embedded adversarial fix + re-review + section verification loop).

## Workflow

### Phase 1 -- Triage Findings

> The PostToolUse hook fires `receiving-code-review` reminder when Codex returns; follow it. **Common false positives in adversarial findings:** Codex reads code without runtime context and will sometimes flag "missing lock" when the caller already holds it, "race" on a path that runs only on the BSP during boot phase 0, or "buffer overflow" on a buffer that's static-asserted larger than the access. Verify at file:line and YAGNI-check (grep for callers; remove dead code rather than expand it) before classifying.

1. **Read the Codex review output.** List every finding with severity.
2. **For each finding, verify technically against the actual code first** before classifying.
3. **Classify each verified finding:**
   - **Fix now** -- correctness bug, race condition, missing error handling, architecture issue solvable with a small refactor. The finding is *verified valid*.
   - **Reject** -- the finding is wrong or misleading. Document the technical reason with code evidence (caller already holds lock X at file:line, path is single-threaded by construction because Y, etc.). Not "I disagree" -- explain WHY.
   - **Accepted** -- the finding is valid but out of scope or an inherent design trade-off. Document why and add a follow-up TODO checklist item with `→ XREF` if applicable.
4. **Create a task list** with one item per "fix now" finding.

### Phase 2 -- Fix

5. **For each "fix now" finding:**
   - Read the relevant source file at the cited line numbers.
   - Understand the root cause (not just the symptom).
   - **G1 -- read a swapped primitive's OWN contract before applying it repo-wide.**
     When a finding suggests swapping a primitive (lock type, atomic ordering,
     allocator, memory-order, e.g. `spin_lock_irqsave` -> `spin_lock`), read that
     primitive's header/contract FIRST -- not just the finding text -- before
     changing any call site. A 2026-07-12 run applied a perf reviewer's lock swap
     to 5 functions + header + callers, then the next round flagged a CRITICAL:
     plain `spin_unlock` does an UNCONDITIONAL `sti`, re-enabling interrupts inside
     an IF=0 path. `receiving-code-review`'s "verify at file:line" covered the
     finding's symptom, not the swapped primitive's semantics -- verify BOTH.
   - Apply the fix following the `kernel-code-quality` skill gates.
   - Build **via `bash scripts/overnight/run-artifact.sh fixloop -- bash
     scripts/build.sh` (deterministic, NO model): the iterative fix loop is
     where context burns (measured 21 in-context builds on one section). The
     envelope returns verdict + error lines + artifact path; you quote the
     `build/build.log` tail yourself and it must show `=== BUILD OK ===`.
     `diagnostic-digester` is for a FAILING artifact only, never a green run.
     Repeated symbol lookups across fix rounds go through
     lsp-bridge (`definition`/`references`), not inline `bash grep`; when the
     round's fix targets moved to new files, re-dispatch
     `Agent(subagent_type="review-evidence-mapper", ...)` for the fresh
     file:line map instead of re-reading whole files.
6. **For each "rejected" finding:**
   - No code change. Record the rejection reason in the Phase 4 summary table.
7. **For each "accepted" finding:**
   - No code change. The summary table (Phase 4) records the justification and any follow-up XREF.
8. **Commit all fixes** with a message referencing the review:
   ```
   fix: address Codex adversarial review findings -- [summary]
   ```

### Phase 3 -- Re-Review

> **G2 -- cap the perf-only chase.** Perf-only (non-Critical/High) suggestions have
> no natural stopping point: each round tends to surface a NEW marginal issue rather
> than confirm convergence. A 2026-07-12 section chased perf findings across ~8
> sequential dispatches (~30 min), netted zero on the lock type (irqsave in, irqsave
> out), and self-inflicted the G1 bug along the way. Cap live-implementation of
> perf-only suggestions at ~1-2 in-section rounds; beyond that, spin the remaining
> perf suggestions into an XREF'd follow-up item and stop -- do NOT keep iterating
> in-section. (Distinct from P2.1 convergence, which is about UNCHANGED inputs; here
> every round's input genuinely changed but the marginal value did not justify it.)

8b. **Pre-dispatch self-diff gate (before the re-review dispatch).** Read the fix diff against this round's + every prior round's findings and self-check the fix-then-regress shapes (canonical rule: review-todo-section step 6): scope creep, an operation reordered before its precondition, `==` where a bit-flag/mask test is required, sentinel/boundary handling (INVALID_HANDLE_VALUE / -1 / caps), and whether this fix re-opens a prior finding. This gate is what keeps the fix->re-review loop from becoming a fix-then-regress marathon (TODO-12 section 28: 15 dispatches, 67 min). A localized fix that passes the gate can be self-verified without a fresh round.
8c. **Convergence gate + round counter + standing evidence map (P2.1/P2.2/P2.3, canonical rule: review-todo-section step 6).** BEFORE re-dispatching kind K, `python3 .claude/hooks/review_convergence.py should-redispatch '<todo>#<section>' <K>` -- exit 1 = CONVERGED (skip K; its inputs did not move since its last verdict), exit 0 = redispatch; after K resolves, `... record '<todo>#<section>' <K>`. Per-kind scope: adversarial/perf source-only, consistency/design source+TODO (a docs/TODO-only fix skips the source-only kinds). After each re-dispatch, `python3 .claude/hooks/review_round_guard.py --bump '<todo>#<section>' --progress <new|none>`; exit 2 = CAPPED (stall, not a fixed cap) -> stop, spin unresolved findings to a follow-up. For rounds >= 4 (REQUIRED), verify at file:line via one `review-evidence-mapper` dispatch (the agent cache, now keyed on the scoped evidence set, reuses the map when the tree is unchanged), not inline re-reads of the same hot files.
9. **Push the commit.**
10. **Re-run the Codex adversarial review** with a focused prompt:
    ```
    Verify the following findings are resolved:
    1. [finding 1 summary]
    2. [finding 2 summary]
    3. [finding 3 summary]
    Focus on: [list of modified files]
    ```
11. **Evaluate the re-review output** -- the PostToolUse hook fires `receiving-code-review` reminder again on the re-review; follow it on any new findings.
    - **Crash != verdict.** If the re-dispatch exits non-zero or the `.out` shows a Codex crash (`app-server exited unexpectedly`, `rc=1`) instead of a structured verdict, RE-DISPATCH (crashes are intermittent, measured 2026-07-12); never read a crash as `all-clear`/`approve` or as a new finding. This round did not happen.
    - If verdict is `all-clear` or only has "accepted" items: **done**.
    - If new findings appear: go back to Phase 1 with the new findings -- verify each before fixing.
    - If original findings are "still live": the fix was incomplete -- go back to Phase 2. But also ask: did Codex re-flag a finding I already rejected? If so, re-verify; if I'm still right, the rejection stands and I document the disagreement, not the fix.

### Phase 4 -- Close

12. **Maximum 3 iterations.** If findings persist after 3 rounds, classify remaining as accepted with justification.
13. **Add a `## Codex Adversarial Review` section** to the TODO (before Verification) with a compact summary table:
    ```markdown
    ## Codex Adversarial Review

    > Reviewed YYYY-MM-DD by Codex. Scope: §X-§Y.
    > **Round 1:** N findings. All fixed.
    > **Round 2:** N new findings. N fixed, N accepted.
    > **Final verdict: resolved.** Build clean.

    | # | Severity | Finding | Status |
    |---|----------|---------|--------|
    | 1 | critical | One-line summary | **Fixed** -- what was done |
    | 2 | medium | One-line summary | **Accepted** -- why |
    | 3 | high | One-line summary | **Rejected** -- code evidence |
    ```
    This table is the permanent record. The verbose per-finding details from the review phase are replaced -- they served their purpose during triage.
14. **Final commit** with the resolved review status.

## Review Command

```bash
bash scripts/codex-dispatch.sh '[review-kind: re-adversarial] <todo-path> <focus prompt>'
```

Run in background for reviews touching > 3 files (wait per the double-poll
ban in codex-design-review "Wait discipline" -- one absorbing wait, no ~10s
re-poll clusters):
```bash
# In Bash tool with run_in_background: true
```

## Guardrails

- **Maximum 3 review iterations.** Do not loop forever. After 3 rounds, track remaining issues as TODO sections.
- **Do not weaken assertions.** If Codex says a lock is needed, add a lock -- don't remove the shared state or make it "okay by convention."
- **Do not patch around findings.** Fix the root cause. If the root cause is too large, track it as a TODO section with a clear scope.
- **Build must pass after every fix.** No committing broken code between iterations.
- **Present Codex output verbatim.** Do not reinterpret or soften findings.
- **Each iteration must make progress.** If a re-review returns the exact same finding with the exact same code, the fix didn't work -- investigate why before retrying.
- **Verify fixes don't break functionality.** Moving code to a thread, reordering init, or adding locks can break timing-dependent subsystems. Check CLAUDE.md "Bare Metal Gotchas" before applying structural fixes to boot-path code.
