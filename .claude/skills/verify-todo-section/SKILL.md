---
name: verify-todo-section
description: Verify an already-implemented TODO section through the full quality pipeline without implementing anything new. Runs evidence-based audit of each [x] claim, Codex adversarial review, fix loop, self-review, and produces a verified stamp. Use to audit completed work, re-verify after upstream changes, or confirm a section is truly done.
---

# Verify TODO Section

## Execution Discipline

> The TODO section IS the contract being verified.
> - **Follow the contract, don't improvise.** The checklist items define what to verify. Do not widen.
> - **Review checkpoints are mandatory.** Steps 8-12 are the HARD GATE -- never skip them.
> - **If the TODO is wrong, downgrade conservatively.** Verify-mode never marks new items `[x]`; it only DOWNGRADES `[x]` -> `[/]` or `[ ]` when evidence shows regression.
> - **One section = one (optional) commit.** If verification produces fixes, commit them. If clean, commit the stamp only.

## Use This Skill When

- A section is marked `[x]` and you want to confirm it's truly complete.
- Upstream code changed and you need to re-verify a section.
- The user asks "is this section really done?" or "verify §N for me."
- Before graduating a TODO file from active to documentation.

## Workflow

1. **Read the section** -- full text, notes, test checkpoint, warning boxes, every `-> XREF:` line. Inventory all `[x]` items (the claims to verify) and any `[ ]`/`[/]` items (NOT in scope).

2. **Resolve dependencies** -- verify every `-> XREF:` target is still valid and still `[x]` in the target TODO file. If a dependency regressed, that's a verification failure for THIS section -- flag it.

3. **Explore the codebase** -- Grep/Glob for every symbol, function, type, and constant referenced in the section's checklist items. Read the source files. Build an evidence map: "this checklist claim is satisfied by file:line through file:line".

4. **Spot-check the domain-appropriate code quality gates** -- the hook auto-selects based on path:
   - `src/boot/` -> `boot-code-quality` (UEFI error handling, EBS boundary, table safety, fallbacks)
   - `src/kernel/`, `include/kernel/` -> `kernel-code-quality` (SMP safety, memory rules, bare-metal)
   - `src/desktop/` -> `desktop-code-quality` | `src/shell/` -> `shell-code-quality` | `user/` -> `userland-code-quality`
   Walk the relevant gates AGAINST the existing implementation, not a plan.

5. **VERIFY each checklist item** -- walk every `- [x]` item and prove it via Grep/Read evidence. State each proof as "claim -> file:line -> snippet shows X".
   - **Done proof gate:** if evidence does NOT show implemented + wired + functional, downgrade from `[x]` to `[/]` (regression) or `[ ]` (never implemented). When in doubt, downgrade.
   - **Stale TODO patterns:** if a checklist item demands `POST16(...)` for non-boot-path code or tautological constant tests, treat as obsolete. Remove in step 7 with a note.
   - **Scope-gap audit (MANDATORY).** Grep the section's source files for undocumented gap markers: `TODO`, `FIXME`, `HACK`, `XXX`, `for now`, `placeholder`, `STUB(`, `STATUS_NOT_IMPLEMENTED`. For each hit, cross-check the TODO text: disclosed in a NOTE callout? If YES, record as known-deferred. If NO, verify-mode may (a) FIX inline if Branch A criteria met (under 1000 lines, routine, same subsystem), or (b) FLAG and downgrade affected items.
   - **STATUS_NOT_IMPLEMENTED completion policy.** When a function returns `STATUS_NOT_IMPLEMENTED` (or equivalent stub): if the function is **standalone** (self-contained, no deep dependency chain), IMPLEMENT it fully during verify -- features should not get lost behind stubs. If the function requires **significant new infrastructure** (new subsystem, new TODO-level scope), advise creating a new TODO section (Branch B) or new TODO file (Branch C) and record the gap in the stamp's Accepted field with a domain-qualified XREF.
   - **NEVER mark a `[ ]` item as `[x]`.** New work belongs in `implement-todo-section`.

6. **Build + verify tests** -- combined checkpoint:
   - `bash scripts/build.sh`, confirm `=== BUILD OK ===`. If build fails, verification has failed.
   - Read the TODO's **Unit Tests** section. Confirm test file exists, registration function exists in `test_runner.c`, `TEST_CAT_*` matches. If claimed count doesn't match reality, that's a verification failure.
   - **One test action allowed:** if a missing assertion for an existing function is routine, ADD it. Anything bigger -- flag and stop.
   - **HARD BAN: tests must NEVER call live boot infrastructure.** See CLAUDE.md "Test Code -- No Live Boot Infrastructure Calls."

   > **Note:** For comprehensive test coverage analysis across an entire subsystem, use `/codex-test-coverage` at the TODO file level rather than per-section. Per-section coverage is too narrow to catch cross-function gaps.

7. **Reconcile all tables** -- single pass over the entire TODO file:
   - **Checklist items:** `[x]` may become `[/]` or `[ ]` on regression evidence. `[ ]` items stay `[ ]`. Remove stale-pattern items with a note.
   - **SSDT claims:** verify service number <-> function <-> `ssdt_register()` <-> master table row consistency.
   - **Cross-TODO sync:** reconcile reciprocally (downgrade only).
   - **Deferred-item check:** scan earlier sections for "deferred to §N" where N is this section. If not resolved, flag.
   - **Implementation Order row:** must match section state. Downgrade conservatively.
   - **OS Comparison row:** must match section state. Downgrade if claimed Done but evidence shows Partial.
   - **Verified stamp:** if ALL `[x]` items survived (no downgrades), add or update IMMEDIATELY after the **Test checkpoint** paragraph (no blank line between stamps):
     ```
     > **Verified:** YYYY-MM-DD -- <summary>. Accepted: <out-of-scope items with owner refs, or "none">.
     ```
     The stamp format:
     - `<summary>`: what Codex found and what was fixed, or "Codex adversarial review clean"
     - `Accepted: none` when all findings were fixed or rejected
     - `Accepted: <brief> (-> XREF: NN-domain/TODO-XX §N)` for valid findings deferred to another owner -- ALWAYS include the domain prefix (e.g., `02-kernel-core/TODO-17`, not just `TODO-17`)
     - If re-verifying, REPLACE the existing `> **Verified:**` line (don't duplicate)
     - If any items were DOWNGRADED, do NOT add the stamp
     - **No blank line** between the Verified stamp and a Quality reviewed stamp (if one exists). Keep stamps as adjacent blockquote lines.
   - Preserve formatting. No N.M subnumbering.

## HARD GATE: Steps 8-12 are MANDATORY before step 13

> **You MUST NOT `git commit` or `git push` until steps 8 through 12 have all been completed.**

8. **Codex adversarial review** (MANDATORY) -- run via the Codex plugin, NOT self-review. Follow `codex-adversarial-review-section` skill: scope to section's files, list adversarial angles (concurrency, races, error paths, ABI, memory safety, security, performance, bare metal), request severity-labeled findings.
    ```bash
    node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<focus prompt>"
    ```

9. **Fix loop** (1 round mandatory; up to 3 if needed) -- apply `superpowers:receiving-code-review` discipline:
    - **Verify technically first.** Read the code Codex flagged. Is it correct?
    - **Valid finding:** fix root cause, rebuild.
    - **Wrong/misleading:** reject with concrete code evidence.
    - **Correct but out of scope:** accept with justification, add follow-up `-> XREF`. Record in stamp's `Accepted:` field.
    - Fix all valid Critical and High. Fix valid Medium unless explicitly accepted.
    - **Re-review via Codex only for STRUCTURAL fixes.** Surgical fixes: self-verify and proceed.
    - Unresolved Critical/High after round 3: do not stamp, downgrade affected items.

10. **Final self-review** (MANDATORY):
    - Regressions, race conditions, edge-case bugs, performance issues
    - Checklist item-by-item: is each `[x]` STILL true today?
    - Test checkpoint: does the expected serial/klog output still match the code?

11. **2nd final build** -- `bash scripts/build.sh`, confirm `=== BUILD OK ===`.

12. **Tie up loose ends** (MANDATORY):
    - Deferred items pointing to this section that weren't resolved
    - Stale warning boxes that should be NOTE (resolved)
    - Cross-TODO dependency notes now satisfied or broken
    - PE export table sync (if user-mode APIs)
    - SSDT audit trigger (if SSDT handlers)
    - Filed-in-owner check: follow-up `[ ]` items naming an owner must be filed reciprocally
    - N.M structure drift: flag `### N.M` or `**N.M**` for cleanup
    - **Deep-analysis (opt-in):** run these only when the user requests thorough verification or the section is hot-path code:
      - `codex-consistency-audit` -- struct offsets, constants, API contracts
      - `codex-dead-code` -- unreachable functions, unused defines
      - `codex-perf-review` -- ISR paths, spinlock hold times, O(n^2)

13. **Commit and push** -- the verified stamp always produces a TODO edit.
    - **PASS (clean):** `"verify: <TODO file> §N -- verified clean"`
    - **PASS-with-fixes:** `"verify: <TODO file> §N -- <summary of fixes>"`
    - **FAIL (downgrades):** `"verify: <TODO file> §N -- downgraded <items>"` (NO stamp)
    - Push to `origin/main` immediately.
    - Do NOT mark the section's `Commit:` checklist item `[x]` -- that belongs to implement-time.

## What This Skill Does NOT Do

- Does NOT implement new checklist items. Items marked `[ ]` stay `[ ]`.
- Does NOT create new source files (except routine gap-fill tests in step 6).
- Does NOT mark new items `[x]`. Verify only DOWNGRADES on regression evidence.
- Does NOT skip steps 8-12. The review pipeline IS what makes verify trustworthy.
- Does NOT modify the `Commit:` checklist line.

## What This Skill DOES Produce

- A `> **Verified:** YYYY-MM-DD -- ...` stamp on each section that passes. This is the permanent audit trail that the full Codex + build pipeline ran and passed.
- An `Accepted:` field in the stamp documenting any out-of-scope findings with owner cross-references, so deferred issues don't get lost.

## Guardrails

- **Conservative downgrade only.** Never widen scope. The scope-gap audit (step 5) and routine test gap-fill (step 6) are the TWO places verify-mode may touch code, both under Branch A constraints.
- **Steps 8-12 are blocking prerequisites for step 13.** This gate exists because review-skip is the failure mode.
- Do not weaken tests to make findings go away.
- Do not mark new items `[x]`. New work goes through `implement-todo-section`.
- For SSDT sections, verify the full chain before downgrading or accepting status.
- If the section was never implemented (all `[ ]`), use `implement-todo-section` instead.
- Do not turn this into a broad file-wide cleanup pass.
- Never introduce N.M subnumbering.
- Apply `superpowers:receiving-code-review` discipline to every Codex finding.

## Relationship to implement-todo-section

This skill was originally mirrored step-for-step with `implement-todo-section`. That coupling has been removed -- verify now has its own streamlined workflow (13 steps vs implement's 19). The two skills share:

- **Shared resources:** [scope-gap-protocol.md](../implement-todo-section/scope-gap-protocol.md), [build-evidence.md](../implement-todo-section/build-evidence.md)
- **Same HARD GATE concept:** Codex review + fixes + self-review + build before commit
- **Same guardrails:** conservative status edits, no N.M, no scope widening, no TODO/FIXME

They do NOT share step numbers. Editing one does not require editing the other.
