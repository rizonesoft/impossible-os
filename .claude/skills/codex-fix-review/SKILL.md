---
name: codex-fix-review
description: Fix all findings from a Codex adversarial review, then re-run the review until Codex confirms resolution. Iterates fix-review-fix until all high/medium findings are resolved or tracked.
---

# Codex Fix and Re-Review Loop

## Use This Skill When

- A Codex adversarial review has returned findings that need fixing.
- The user says "fix the Codex findings" or "resolve the review issues."
- After `/codex-review-todo` produces a needs-attention verdict.

## Workflow

### Phase 1 -- Triage Findings

1. **Read the Codex review output.** List every finding with severity.
2. **Classify each finding:**
   - **Fix now** -- correctness bugs, race conditions, missing error handling, architecture issues solvable with a small refactor.
   - **Accepted** -- not a bug, or inherent design trade-off. Document why.
3. **Create a task list** with one item per finding.

### Phase 2 -- Fix

4. **For each "fix now" finding:**
   - Read the relevant source file at the cited line numbers.
   - Understand the root cause (not just the symptom).
   - Apply the fix following the `kernel-code-quality` skill gates.
   - Build: `bash scripts/build.sh` -- must show `=== BUILD OK ===`.
5. **For each "accepted" finding:**
   - No code or file changes needed. The summary table (Phase 4) records the justification.
6. **Commit all fixes** with a message referencing the review:
   ```
   fix: address Codex adversarial review findings -- [summary]
   ```

### Phase 3 -- Re-Review

8. **Push the commit.**
9. **Re-run the Codex adversarial review** with a focused prompt:
   ```
   Verify the following findings are resolved:
   1. [finding 1 summary]
   2. [finding 2 summary]
   3. [finding 3 summary]
   Focus on: [list of modified files]
   ```
10. **Evaluate the re-review output:**
    - If verdict is `all-clear` or only has "accepted" items: **done**.
    - If new findings appear: go back to Phase 1 with the new findings.
    - If original findings are "still live": the fix was incomplete -- go back to Phase 2.

### Phase 4 -- Close

11. **Maximum 3 iterations.** If findings persist after 3 rounds, classify remaining as accepted with justification.
12. **Add a `## Codex Adversarial Review` section** to the TODO (before Verification) with a compact summary table:
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
    ```
    This table is the permanent record. The verbose per-finding details from the review phase are replaced -- they served their purpose during triage.
13. **Final commit** with the resolved review status.

## Review Command

```bash
node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<focus prompt>"
```

Run in background for reviews touching > 3 files:
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
