---
name: review-todo-section
description: Full review of a TODO section -- adversarial Codex, dead code, consistency, performance, domain code quality, feature completeness. Runs after implementation or standalone. No steps skipped.
---

# Review TODO Section

> **NO CODE SHIPS WITHOUT BEING EXAMINED FROM EVERY ANGLE.**
>
> You have been observed cutting corners: skipping Codex dispatches, self-reviewing instead of dispatching, accepting "clean" without running quality gates, folding separate concerns into a single shallow pass. This skill exists because your judgment about which steps to skip has been wrong repeatedly.
>
> Every step runs. Every time. Mechanically. No judgment calls about skipping.

## Pipeline

### Phase 1: Evidence + Verify

1. **Evidence map** -- for each `[x]` item, prove via Grep/Read: "claim -> file:line". Downgrade on regression.
2. **Scope-gap audit** -- grep for `TODO`, `FIXME`, `HACK`, `STATUS_NOT_IMPLEMENTED`. Standalone stubs: implement. Infrastructure stubs: Accept with XREF.
3. **Test checkpoint verification** -- read the Test checkpoint paragraph. Grep source for each expected serial/klog message.
4. **Build** -- `bash scripts/build.sh`, confirm `=== BUILD OK ===`.

### Phase 2: Adversarial Review

5. **MANDATORY Codex adversarial review** -- dispatch to Codex. No exceptions.
   ```bash
   node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<prompt>"
   ```
   **Mandatory angles:** integer overflow, buffer overread, NULL deref, SMP races, resource leaks, ABI mismatch, bounds on untrusted data.
   Apply `superpowers:receiving-code-review` to EVERY finding.
6. **Fix adversarial findings** -- fix all valid Critical/High/Medium. Rebuild.

### Phase 3: Quality Audit

7. **Domain code quality gates** -- INVOKE the domain-appropriate skill and walk its gates against the code:
   - `src/boot/` -> read and walk `boot-code-quality` (all 10 gates)
   - `src/kernel/` -> read and walk `kernel-code-quality` (all 10 gates)
   - `src/desktop/` -> `desktop-code-quality` | `src/shell/` -> `shell-code-quality` | `user/` -> `userland-code-quality`
   Record any gate failures as findings.

8. **MANDATORY Codex quality review** -- single dispatch covering dead code + consistency + performance. No exceptions.
   ```bash
   node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<prompt>"
   ```
   **CRITICAL -- Dead code (mandatory):** unreachable functions, unused defines, orphaned types, stale declarations.
   **CRITICAL -- Consistency (mandatory):** struct layout matches, constants in one place, API contracts, error code mapping.
   **CRITICAL -- Performance (mandatory):** allocations in hot paths, O(n^2), lock hold times, byte-at-a-time ops.
   Apply `superpowers:receiving-code-review` to EVERY finding.

9. **Industry standards + Win11/Linux parity** -- spec compliance, concrete function/file references.

10. **Feature completeness** -- grep for `STATUS_NOT_IMPLEMENTED`, partial implementations, dead API promises.

11. **Self-review** -- regressions, races, edge cases, resource leaks.

12. **Fix ALL findings** from steps 7-11. Priority: spec violations > incomplete stubs > dead code > quality gate failures > consistency > parity > perf. Rebuild after each batch.

### Phase 4: Stamp + Commit

13. **Reconcile tables** -- IO row, OS Comparison row.
14. **Stamps** -- Verified + Quality reviewed. No blank line between. Domain-qualified XREFs in Accepted field.
15. **Commit and push** -- `"review: <TODO> §N -- <summary>"`

## Rules

- **TWO Codex dispatches per review.** Step 5 (adversarial) and step 8 (quality). Both mandatory.
- **Domain code quality skill walked explicitly** in step 7. Not just the hook -- read the skill and check every gate.
- `superpowers:receiving-code-review` on every Codex finding.
- All stamps use domain-qualified XREFs.
- No blank line between stamps.
