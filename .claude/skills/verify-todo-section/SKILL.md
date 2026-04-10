---
name: verify-todo-section
description: Verify an already-implemented TODO section through the full quality pipeline without implementing anything new. Mirrors implement-todo-section 1:1 with implementation steps replaced by evidence-based verification. Runs Codex adversarial review, test coverage analysis, self-review checklist walk, section validation, and loose-end scan. Use to audit completed work, re-verify after upstream changes, or confirm a section is truly done before marking a TODO complete.
---

# Verify TODO Section

> **MIRRORED with `implement-todo-section`.** Step numbering, phase boundaries, and the HARD GATE are kept identical between the two skills. Step N here corresponds to step N there. Only the per-step CONTENT differs: implement-mode WRITES code; verify-mode AUDITS existing code with grep/read evidence. See the "Mirror with implement-todo-section" section at the bottom for the sync rules.

## Execution Discipline

> The TODO section IS the contract being verified. Apply `superpowers:executing-plans` principles in audit form:
> - **Follow the plan, don't improvise.** The checklist items define what to verify. Do not widen.
> - **Review checkpoints are mandatory.** Steps 13-18 are review checkpoints -- never skip them. The whole point of verify is the pipeline.
> - **If the TODO is wrong, downgrade conservatively.** Verify-mode never marks new items `[x]`; it only DOWNGRADES `[x]` -> `[/]` or `[ ]` when evidence shows regression.
> - **One section = one (optional) commit.** If verification produces fixes, commit them under the section's scope. If clean, no commit.

## Use This Skill When

- A section is marked `[x]` and you want to confirm it's truly complete.
- Upstream code changed (refactor, dependency update) and you need to re-verify a section.
- The user asks "is this section really done?" or "verify §N for me."
- Auditing a section implemented in a previous session.
- Before graduating a TODO file from active to documentation.

## Workflow

1. **Read the section** -- full text, notes, test checkpoint, warning boxes, every `-> XREF:` line. Inventory all `[x]` items (the claims to verify) and any `[ ]`/`[/]` items (NOT in scope -- this skill does not implement them). If the section has > 10 checklist items, expect a longer verification pass.
2. **Resolve dependencies** -- verify every `-> XREF:` target is still valid and still `[x]` in the target TODO file. If a dependency regressed, that's a verification failure for THIS section (the section depends on something that's no longer true) -- flag it for the report.
3. **Explore the codebase** -- Grep/Glob for every symbol, function, type, and constant referenced in the section's checklist items. Read the source files. Build a mental model of "this checklist claim is satisfied by file:line through file:line". This is your evidence map for step 6.
4. **Codex design review** -- NOT applicable in verify mode. The code already exists; design-time review happens at implement time only. If the implementation is structurally broken, the adversarial review at step 13 will catch it. SKIP this step in verify -- it is intentionally a no-op so the step numbering stays aligned with implement-todo-section.
5. **Spot-check `kernel-code-quality` gates against existing code** -- walk the gates AGAINST the implementation rather than against a plan. For each gate, audit the section's source files: SMP safety (any new shared mutable state without sync?), 5-layer defense (cross-file invariants protected by `_Static_assert` + runtime check + test + canary + doc?), memory rules (`kmalloc <= 4 KB`, NULL checks on every allocation?), bare-metal correctness (MMIO via `vmm_map_mmio_uc/wc`, GS_BASE set, no `clac`/`stac` without SMAP CPUID, no LAPIC TPR writes in ISR), boot-path POST16 hygiene (only on Phase 0/1/2 functions, never on scheduler/exec/syscall code), error handling (every allocation NULL-checked, no silent failures), Gate 10 production quality (no TODO/FIXME/HACK in shipped code).
6. **VERIFY each checklist item** -- this is the verify-mode replacement for implement-mode "Implement". Walk every `- [x]` item in the section and prove it via Grep/Read evidence. State each proof as "claim → file:line → snippet shows X".
   - **Done proof gate (verify mode):** if the evidence does NOT show implemented + wired + functional on normal path, the item must be conservatively downgraded from `[x]` to `[/]` (regression) or `[ ]` (never implemented). When in doubt, downgrade.
   - **Stale TODO patterns:** if a checklist item demands `POST16(...)` codes for non-boot-path code (scheduler/exec/syscall/auxv work) or "test POST16 constant equals 0xDDNN" tautologies, treat the item as obsolete. Document the removal in the verification report and remove the stale checklist line in step 10. Do not satisfy obsolete patterns.
   - **Scope-gap audit (MANDATORY).** Grep the section's source files for undocumented gap markers: `TODO`, `FIXME`, `HACK`, `XXX`, `for now`, `placeholder`, `STUB(`, `STATUS_NOT_IMPLEMENTED`, `kernel_unimplemented`, `E_NOTIMPL`. For each hit, cross-check the section's TODO text: was the gap disclosed in a NOTE callout or `Follow-up (Branch ...)` line during the original implement run? If YES, it is a known-deferred item -- record it in the verify report. If NO, the original implement run violated the scope-gap protocol: this is a verification failure. Verify-mode then either (a) FIXES the gap inline IF it meets Branch A criteria (under 1000 lines, routine, same subsystem) AND adds the NOTE callout retroactively (this is the ONE implementation action verify-mode is allowed in step 6, identical in spirit to step 8's routine test gap-fill allowance); or (b) FLAGS the gap, downgrades affected items from `[x]` to `[/]`, and produces a list of follow-up items for the user to triage via implement-mode on a later run. Never (c) silently re-stub the workaround. See [scope-gap-protocol.md](../implement-todo-section/scope-gap-protocol.md).
   - **NEVER mark a `[ ]` item as `[x]` in verify-mode.** New work belongs in `implement-todo-section`. If verification reveals an unimplemented item that should have been `[x]`, flag it -- do not silently complete it.
7. **Build** -- `bash scripts/build.sh`, confirm `tail -1 build/build.log` shows `=== BUILD OK ===`. If the existing code doesn't even build, verification has already failed; report and stop.
8. **Verify unit tests exist and are wired** -- read the TODO file's **Unit Tests** section to find:
   - The expected test file name (e.g. `test_exec.c`)
   - The expected registration function (e.g. `test_register_exec()`)
   - The expected test category (e.g. `TEST_CAT_EXEC`)
   - The assertion count or specific assertions claimed for this section

   Confirm the test file exists, the registration function exists in `test_runner.c`, and the `TEST_CAT_*` matches. If the section claims "9 suites" but only 5 are registered, that's a verification failure -- downgrade. **Do NOT piggy-back missing tests onto an unrelated test file.**
   - **No tautological constant tests.** If existing tests include `TEST_ASSERT_EQ(SOME_CONSTANT, literal)` patterns, flag them for removal as part of the verify cleanup. The compiler enforces `#define` values; the boot-time uniqueness check is the real protection.
   - **One implementation action allowed:** if Codex (step 9) finds a missing assertion for an existing function and the gap is routine, ADD the missing test as part of verify. Anything bigger (new test file, new TEST_CAT, new test runner wiring) is out of scope -- flag and stop.
   - **HARD BAN: tests must NEVER call live boot infrastructure.** WSL has no working QEMU, so runtime regressions in tests are not caught until the user boots on native Windows or bare metal -- 3 incidents to date. Forbidden in `src/kernel/test/test_*.c`: `boot_progress(`, `boot_post_write16(`, `boot_post_nvram_write16(`, `post_display16(`, `vpd_stage_*(`, `vpd_init(`, `boot_splash_*(`, `boot_halt(`, `panic(`, `KeBugCheckEx(`, any subsystem `_init(` (`pmm_init`, `vmm_init`, `heap_init`, `serial_init`, `klog_early_init`, `klog_disk_enable`, `acpi_init`, `lapic_init`, `ioapic_init`, `timer_hal_init`, `gdt_init`, `idt_init`). **Allowed alternatives:** pure constant checks, save/restore wrappers around `kernel_subsystem_set_ready`/`_ready`, direct calls to PURE data helpers (`boot_timing_record_step()` is OK -- in-memory append only; `boot_progress()` is NOT because it ALSO updates VPD/framebuffer), BOOT_REQUIRE/BOOT_STEP via wrapper functions, read-only oracle queries. The pre-commit hook in `settings.json` enforces this -- a test file with forbidden calls cannot be committed. See `feedback_test_no_live_boot_calls` memory and CLAUDE.md "Test Code -- No Live Boot Infrastructure Calls."
9. **Codex test coverage analysis** -- dispatch to Codex via the `codex-test-coverage` skill. List public functions, existing tests, and ask Codex to find untested error paths, boundaries, negative cases. Apply `superpowers:receiving-code-review` discipline: verify each gap is real and reachable before adding a test. **If Codex recommends testing a function in the forbidden list above, REJECT with code evidence** -- Codex doesn't know WSL constraints. Test the underlying pure helper, or accept the gap with a `**Note:**` line in the TODO's Unit Tests section. Add tests for verified gaps; reject false positives with code evidence.
    ```bash
    node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<test coverage prompt>"
    ```
    Skip for trivial sections (< 3 test assertions claimed).
10. **Reconcile TODO section** -- walk every checklist item one more time after step 6 evidence + step 9 Codex findings. Make ONLY conservative status edits:
    - `[x]` may become `[/]` or `[ ]` if evidence shows regression.
    - `[ ]` items stay `[ ]` -- never marked done in verify-mode.
    - Stale-pattern items (POST16-in-non-boot, tautological constant tests) are REMOVED from the checklist with a note in the verify report.
    - SSDT claims: verify service number <-> function <-> `ssdt_register()` <-> master table row consistency. Downgrade the SSDT row only if the chain is broken.
    - **Cross-TODO sync:** if the section's claims affect external TODO file checklists, reconcile those reciprocally (downgrade only -- never mark new items `[x]` in another file from verify).
    - **Deferred-item resolution check:** scan earlier sections in the SAME TODO for items marked "deferred to §N" where N is this section. If §N didn't actually do the deferred work, the deferral is broken -- flag for the report and downgrade the deferring item if appropriate.
    - Preserve formatting (table headers, icons, column structure).
11. **Verify Implementation Order table row** -- the IO row for this section must match the section state after step 10. If §N has any `[/]` or `[ ]` items, the IO row cannot stay `[x]`; downgrade conservatively. Never upgrade an IO row in verify-mode.
12. **Verify OS Comparison table row** -- the row for this section must match the section state. If a feature was claimed Done but evidence shows Partial, downgrade. Update the `<!-- Sources: ... -->` comment only if the row changed.
13. **Codex adversarial review** (MANDATORY) -- run via the Codex plugin, NOT self-review. Self-review has implementation/verification bias; Codex reads code fresh and catches things you rationalized away. Follow the `codex-adversarial-review-section` skill workflow: scope to changed files, list adversarial angles (concurrency, races, error paths, regressions, wiring, ABI, memory safety, functional correctness, security, code quality, performance, bare metal), request severity-labeled findings.
    ```bash
    node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<focus prompt>"
    ```
14. **Fix loop** (1 round mandatory; up to 3 if needed) -- apply `superpowers:receiving-code-review` discipline: do NOT blindly fix every Codex finding. For each finding:
    - **Verify technically first.** Read the code Codex flagged. Is the finding correct? Codex can be wrong -- it lacks runtime context.
    - **If the finding is valid:** fix the root cause, not the surface symptom. Rebuild.
    - **If the finding is wrong or misleading:** reject with concrete technical reason and code evidence (caller already holds lock X at file:line, path is single-threaded by construction because Y, etc.).
    - **If the finding is correct but out of scope:** accept with justification and add a follow-up TODO checklist item with `→ XREF`.
    - Fix all valid Critical and High. Fix valid Medium unless explicitly accepted.
    - **Re-review via Codex is REQUIRED only when fixes are STRUCTURAL** (function signature changes, control-flow rewrites, new locking, lifecycle changes). For SURGICAL fixes (single-line bug, contract clarification, small logic correction, comment fix), self-verification is sufficient: re-read the cited code and confirm the fix matches what Codex flagged, then proceed to step 15.
    - If unresolved Critical/High remain after round 3: do not call the section verified, downgrade affected items to `[/]` or `[ ]`, add follow-up items with `→ XREF`.
15. **Final self-review** (MANDATORY) -- this catches what Codex misses at the integration level:
    - Regressions: did any existing functionality break since this was implemented?
    - Race conditions: any shared mutable state without synchronization?
    - Bugs: edge cases, off-by-one, null pointer paths?
    - Performance: unnecessary allocations, O(n^2) where O(n) suffices?
    - Industry standards: no hacks, no patches, no workarounds, no TODO/FIXME in shipped code.
    - **Checklist item-by-item:** walk every `- [x]` and `- [/]` item one final time. For each one: is it STILL implemented and wired today? Could upstream changes have broken it? If yes, downgrade.
    - **Test checkpoint verification:** does the test checkpoint's expected serial output / klog line / behavior still hold against the current code?
16. **2nd final build** -- `bash scripts/build.sh`, confirm `=== BUILD OK ===`. Catches anything broken by the fix loop or self-review changes.
17. **Validate section** (MANDATORY) -- invoke `validate-todo-section` skill. Evidence-based checklist classification. Catches stale/optimistic status claims that survived steps 6 + 10. Re-checks cross-TODO synchronization.
18. **Tie up loose ends** (MANDATORY) -- scan the ENTIRE TODO file and any XREF'd TODO files for:
    - Deferred items pointing to this section that weren't resolved.
    - Stale warning boxes that should be updated to NOTE (resolved).
    - Implementation Order rows that need status reconciliation.
    - Cross-TODO dependency notes that are now satisfied or broken.
    - Any checklist items in other sections affected by this section's reality.
    - **PE export table sync:** if this section provides public APIs callable from user-mode, verify they are in `s_kernel32_exports[]`/`s_ntdll_exports[]` in `pe.c`.
    - **SSDT audit trigger:** if this section involves SSDT handlers, invoke `audit-ssdt` skill to verify master table consistency. (Verify mode runs the audit; implement mode only recommends it.)
    - **Filed-in-owner check:** any follow-up `[ ]` item that names an owner (e.g., "tracked in TODO-XX §N", "owner: TODO-YY") must ALSO be filed as a checklist item in that owner section with reciprocal `→ XREF`. If the owner section doesn't exist yet, find or create one via scope-gap protocol Branch C/D before filing. A note here alone is a dead-end paper trail.
    - **Verify-mode deep-analysis pass** (extra in verify; not in implement because the code is fresh there):
      - `codex-consistency-audit` -- struct offsets, constants, API contracts across header/implementation/test/asm files.
      - `codex-dead-code` -- unreachable functions, unused defines, orphaned types after the section has had time to drift.
      - `codex-perf-review` -- only for hot-path sections (ISR paths, scheduler, compositor, spinlock-protected regions, per-tick code). Skip for non-hot-path sections.
19. **Commit and push** -- only if any phase produced fixes, test additions, or status downgrades.
    - **If no changes were made:** report PASS / PASS-with-fixes / FAIL to the user. Skip git entirely.
    - **If changes were made:** commit message format `"verify: <TODO file> §N -- <summary of fixes/downgrades>"`. Stage all changed source files, test changes, and TODO updates together. Push to `origin/main` immediately after successful commit.
    - Do NOT mark the section's `Commit:` checklist item `[x]` -- that line belongs to the implement-time commit and stays as it was.

## HARD GATE: Steps 13-18 are MANDATORY before step 19

> **You MUST NOT `git commit` or `git push` until steps 13 through 18 have all been completed.**
>
> This is not optional. This is not skippable for "the code already exists." The code exists, but the verification IS the pipeline. Steps 13-18 are how you prove the section is still right today.
>
> If you find yourself about to run `git add` and you have not yet:
> - (13) Dispatched Codex adversarial review and received severity-labeled findings
> - (14) Fixed all Critical/High findings with build evidence (or rejected with code evidence)
> - (15) Done final self-review for regressions, races, bugs, completeness
> - (16) Confirmed 2nd final build passes
> - (17) Invoked `validate-todo-section` skill and reconciled status
> - (18) Scanned for and tied up loose ends in current and XREF'd TODO files (including the verify-mode deep-analysis pass)
>
> then **STOP and go back to step 13**.
>
> **Pattern to watch for:** "It's just a verify, the code didn't change, let me skip the review." That thought is the signal to STOP. Verifications skip Codex adversarial review at exactly the moment regressions slip through.

## Mirror with implement-todo-section

This skill is the VERIFY half of a mirrored pair with `implement-todo-section`. Step numbering, phase boundaries, and the HARD GATE are kept identical between the two files so improvements ported into either skill can be ported back to the other.

**Sync rules:**
- Step N here ↔ step N in `implement-todo-section/SKILL.md`. Adding a step in one file requires adding the matching step in the other at the same position.
- The HARD GATE always cites steps 13-18 in BOTH files. If the gate moves, it moves in both.
- The Execution Discipline block at the top is the same shape (4 bullets) in both files.
- The Guardrails section is the same shape in both, with verify-mode adding the conservative-downgrade rule.

**Per-step content rules** (this is what differs between the two files):

| Step | implement-mode | verify-mode |
|---|---|---|
| 1 | Read the section as a plan to execute | Read the section as a contract to audit |
| 2 | Resolve XREFs to know what's available | Verify XREFs are still satisfied |
| 3 | Explore codebase to plan changes | Explore codebase to map evidence |
| 4 | Codex design review BEFORE writing | SKIP -- code exists; intentional no-op to keep numbering aligned |
| 5 | Walk kernel-code-quality gates BEFORE writing | Spot-check kernel-code-quality gates AGAINST existing code |
| 6 | **WRITE code** + walk scope-gap protocol on detection (Branches A/B/C/D) | **VERIFY each `[x]` claim** + scope-gap audit (flag undocumented workarounds) |
| 7 | Build -- catches what you wrote | Build -- catches what's already broken |
| 8 | Wire NEW unit tests | AUDIT existing test wiring; add only routine gap fills |
| 9 | Codex test coverage of new tests | Codex test coverage of existing tests |
| 10 | Mark `[x]` as items complete | DOWNGRADE `[x]` to `[/]`/`[ ]` only on regression evidence |
| 11 | Update IO row to `[x]`/`[/]` | Reconcile IO row downward only |
| 12 | Update OS Comparison row | Reconcile OS Comparison row downward only |
| 13-17 | Codex review, fix loop, self-review, build, validate | **SAME** -- the review pipeline is identical |
| 18 | Tie up loose ends | Tie up loose ends + run deep-analysis Codex skills (extra) |
| 19 | Commit the implementation | Commit only if fixes were made |

**When you edit one file, edit the other.** A hook fires on edits to either `SKILL.md` reminding you. The mirror is enforced by convention + hook, not by content equality.

## What This Skill Does NOT Do

- Does NOT implement new checklist items. Items marked `[ ]` stay `[ ]`.
- Does NOT create new source files (except routine gap-fill tests in step 8).
- Does NOT mark new items `[x]`. Verify only DOWNGRADES on regression evidence.
- Does NOT skip steps 13-18. The verification pipeline IS what makes verify trustworthy.
- Does NOT modify the `Commit:` checklist line -- that belongs to the implement-time commit.

## Guardrails

- **Conservative downgrade only.** Never widen scope to "fix while you're in there." Exception: the scope-gap audit in step 6 and the routine test gap-fill in step 8 are the TWO places verify-mode is allowed to touch new code, and both require the same size/subsystem constraints as Branch A of the scope-gap protocol. Anything beyond that -- flag and stop.
- **Steps 13-18 are blocking prerequisites for step 19.** Same as implement-mode: this gate exists because review-skip is the failure mode.
- Do not weaken tests to make findings go away.
- Do not mark new items `[x]` from verify-mode evidence. New work goes through `implement-todo-section`.
- For SSDT sections, verify service number <-> function <-> registration <-> master table consistency before downgrading or accepting any status.
- Commit only if actual fixes/test additions/status downgrades occurred. A clean PASS produces no commit.
- If the section was never implemented (all `[ ]`), this skill is wrong -- use `implement-todo-section` instead.
- Do not turn this into a broad file-wide cleanup pass.
- Apply `superpowers:receiving-code-review` discipline to every Codex finding -- verify before fixing, reject false positives with evidence.

## Additional Resources

- [build-evidence.md](../implement-todo-section/build-evidence.md) -- shared with implement-todo-section (single source of truth)
- [scope-gap-protocol.md](../implement-todo-section/scope-gap-protocol.md) -- shared with implement-todo-section (Branches A/B/C/D decision tree for scope gaps)
