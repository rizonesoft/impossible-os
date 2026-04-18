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
   - **OS Comparison row:** must match section state. Downgrade if claimed Done but evidence shows Partial. If the Implementation Order table has an `[x]` entry for this section and the OS Comparison table is MISSING a row for it entirely, add one (missing row = drift, same as stale row). Also check that the post-OS-Comparison summary sentences cover this section; extend them if they cap out at an earlier §.
   - **Verified stamp + optional Accepted/Deferred stamp:** if ALL `[x]` items survived (no downgrades), add or update IMMEDIATELY after the **Test checkpoint** paragraph. Compact pipe-separated format -- each stamp is ONE line of pipe-separated fields, not a paragraph. No blank lines between stamp lines.
     ```
     > **Verified:** YYYY-MM-DD | commit `<hash>` | N/M items | build OK[ | <evidence token>]
     > **Accepted:** [<sev>] <one-line finding> [(reason: <short>)] -> XREF: NN-domain/TODO-XX §N (item: "..." at line N)
     > **Deferred:** [<sev>] <one-line finding> [(reason: <short>)] -> XREF: NN-domain/TODO-XX §N (item: "..." at line N)
     > **Quality reviewed:** YYYY-MM-DD | Codex Nx (<kinds>) | <H>H+<M>M+<L>L fixed, <D> open | scope: <skill or "N/A (reason)">
     ```
     Field rules:
     - `N/M items` -- checklist items `[x]` vs total `[ ]+[/]+[x]` in the section (not including the `Commit:` line).
     - `build OK` | `build FAIL` -- single token. Do NOT re-paste `=== BUILD OK ===`.
     - **Evidence token vocabulary (pick one; keep scannable):** `smoke PASS (<platform> <time>)` for boot-path work; `tests N/M PASS` when a dedicated suite ran; `<N> sentinels` / `<N> fields` / `<N> rows` for structural counts; `manual` when validation is manual walkthrough. Free-form strings are still allowed but prefer the vocabulary for scannability. Skip the token entirely if nothing is surprising to report.
     - Codex line: `Nx` is the dispatch count; `<kinds>` names the dispatches (`adversarial`, `quality`, `consistency`, `dead-code`, `perf`). Findings as `<H>H+<M>M+<L>L fixed, <D> open` (use actual counts; drop terms that are 0, e.g. `2M fixed, 0 open`). `scope:` names the domain code-quality skill applied OR `N/A (<reason>)` (examples: `N/A (docs-only)`, `N/A (bash + docs)`). Do NOT write the full "No domain code-quality skill applies (...)" sentence.
     - **`Accepted:` vs `Deferred:` -- pick the right label; the semantic split matters for triage.** Both are hook-enforced identically, but they answer different triage questions:
       - **`Accepted:`** -- finding valid but **out-of-scope for this section**; ownership is elsewhere. The XREF points to a concrete item in ANOTHER TODO section or file. Grep `Accepted:` when auditing ownership-transfer risk.
       - **`Deferred:`** -- finding valid, **in-scope** for this TODO, but bigger than this commit. The XREF points to a later item in THIS section (or elsewhere in this TODO file). Grep `Deferred:` when auditing "we promised to come back to this."
     - **Severity tag** `[Critical]` / `[H]` / `[M]` / `[L]` is REQUIRED on every Accepted and Deferred line. Matches the Codex finding severity. Enables `grep -r "Accepted:\s*\[H\]" todo/` for fast scope-risk triage.
     - **Optional `(reason: <short>)`** -- include when the defer-rationale is not obvious from the finding text alone. Three common reasons worth naming: `scope` (finding valid but belongs to another owner), `infra` (needs bigger work unit; this commit can't absorb it), `not-functional-today` (hardware/deployment guarantees mean we're not racing a real bug). Free-form reasons are allowed; keep under ~10 words. Omit the parenthetical when the finding text alone explains the defer.
     - **One XREF per concern.** If more than one finding is deferred, repeat the whole `> **Accepted:**` or `> **Deferred:**` line. Domain-qualified TODO name (e.g. `02-kernel-core/TODO-17`, never bare `TODO-17`). `(item: "<name>" at line N)` parenthetical is MANDATORY and hook-enforced -- a pre-commit hook rejects any Accepted/Deferred XREF without `item:` / `at line` / `retrofit` / `helper;` inside a parenthetical. OMIT the line entirely when nothing is deferred -- do NOT write `Accepted: none` or `Deferred: none`.
     - If re-verifying, REPLACE existing stamp lines in place; never duplicate. If any items were DOWNGRADED, do NOT add any stamp.
     - **Escape hatch (rare):** if per-finding prose is genuinely load-bearing (cross-review context the commit message won't capture), append a `<details>` block after the stamps. Example:
       ```
       > <details><summary>Finding detail</summary>
       >
       > - H1 <one line> -> fixed at file:line
       > - M1 <one line> -> deferred (Deferred above)
       > </details>
       ```
       Default is NO `<details>` block. Use only when the commit message and `[x]` marks genuinely can't carry the context.
   - Preserve formatting. No N.M subnumbering.

## HARD GATE: Steps 8-12 are MANDATORY before step 13

> **You MUST NOT `git commit` or `git push` until steps 8 through 12 have all been completed.**

8. **Codex adversarial review** (MANDATORY -- NO EXCEPTIONS) -- dispatch to Codex plugin. Self-review is NOT a substitute. If Codex responds with "no diff available", re-dispatch with actual file content (read 100-200 relevant lines). A shallow response requires re-prompting with specific angles.
    ```bash
    node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<prompt with ALL mandatory angles below>"
    ```
    **Mandatory adversarial angles (include ALL in every prompt):**
    - Integer overflow / underflow in arithmetic
    - Buffer overread / overwrite on untrusted input
    - Missing NULL checks before dereference
    - SMP race conditions on shared mutable state
    - Error paths that leak resources (handles, memory, locks)
    - ABI mismatch between bootloader and kernel structs
    - Missing bounds checks on firmware-provided data

    After Codex responds, apply `superpowers:receiving-code-review` to EVERY finding:
    - **Verify technically first.** Read the cited code. Is it correct?
    - **Valid:** fix root cause, rebuild.
    - **Wrong/misleading:** reject with concrete code evidence.
    - **Out of scope:** accept with domain-qualified XREF. Record in stamp's `Accepted:` field.

9. **Self-review BEFORE fixing** -- walk the section's source files looking for what Codex missed:
    - Regressions from recent changes in other sections
    - Race conditions that only manifest under specific scheduling
    - Edge cases at boundary values (0, 1, MAX, overflow)
    - Resource leaks on error paths
    - Checklist item-by-item: is each `[x]` STILL true today?
    - Test checkpoint: does the expected serial/klog output still match the code?

10. **Fix loop** (1 round mandatory; up to 3 if needed):
    - Fix all valid Critical and High from BOTH Codex (step 8) and self-review (step 9).
    - Fix valid Medium unless explicitly accepted.
    - Rebuild after fixes.
    - Re-dispatch Codex if fixes are STRUCTURAL. Surgical fixes: self-verify and proceed.
    - Unresolved Critical/High after round 3: do not stamp, downgrade affected items.

11. **2nd final build** -- `bash scripts/build.sh`, confirm `=== BUILD OK ===`.

12. **Tie up loose ends** (MANDATORY):
    - Deferred items pointing to this section that weren't resolved
    - Stale warning boxes that should be NOTE (resolved)
    - Cross-TODO dependency notes now satisfied or broken
    - **Inbound `> **Accepted:**` sweep (MANDATORY).** If this verify run just confirmed items `[x]` that other sections list in an `> **Accepted:**` line as their deferred-gap XREF target, those stamps are now stale. Grep `todo/` for this TODO/section identifier and any quoted item name; for each match, DELETE the fully-resolved Accepted entry, or rewrite it to drop only the closed concern. If all Accepted entries on a line go away, remove the whole line -- keep Verified and Quality reviewed adjacent. This is the closed-loop counterpart to the implement-todo-section Accepted-XREF concreteness check.
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
