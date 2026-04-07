---
name: implement-todo-section
description: Execute one bounded TODO section, resolve XREF dependencies, run Codex adversarial review, self-review for regressions, validate section, tie up loose ends, and commit. Use when implementing a specific TODO section or a clearly scoped subset of one.
---

# Implement TODO Section

> **MIRRORED with `verify-todo-section`.** Step numbering, phase boundaries, and the HARD GATE are kept identical between the two skills. Step N here corresponds to step N there. Only the per-step CONTENT differs: implement-mode WRITES code; verify-mode AUDITS existing code with grep/read evidence. See the "Mirror with verify-todo-section" section at the bottom for the sync rules.

## Execution Discipline

> The TODO section IS the plan. Apply `superpowers:executing-plans` principles:
> - **Follow the plan, don't improvise.** The checklist items define scope. Do not widen.
> - **Review checkpoints are mandatory.** Steps 13-18 are review checkpoints -- never skip them.
> - **If the plan is wrong, update the plan first.** If reality conflicts with the checklist, update the TODO section text before implementing a different approach.
> - **One section = one commit.** Each section is a milestone with a clean commit boundary.

## Workflow

1. **Read the section** -- full text, notes, test checkpoint, warning boxes. If the section has > 10 checklist items, flag it: suggest splitting into two sections before implementing. A 15-item section is two sections pretending to be one.
2. **Resolve dependencies** -- follow every `-> XREF:` line. Stop and ask if a prerequisite is incomplete or scope conflicts with reality. If part is implementable and part is blocked, implement only the unblocked subset; keep blocked items `[ ]` or `[/]` with explicit blocker notes.
3. **Explore the codebase** -- Grep/Glob for symbols, call-graph tracing, cross-file discovery. Read relevant source files to understand the integration surface.
4. **Codex design review** (for complex/high-risk sections) -- if the section touches SMP-sensitive code, boot-path, page tables, interrupt handling, or security-critical logic, dispatch a design review via the Codex plugin BEFORE writing code. Follow the `codex-design-review` skill: send the plan + integration surface + constraints, evaluate for blockers/warnings. Skip for straightforward sections (simple struct definitions, single-function additions, test-only work).
    ```bash
    node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<design review prompt>"
    ```
5. **Run `kernel-code-quality` skill** -- walk the gates BEFORE writing code. If touching `src/kernel/`, `include/kernel/`, `src/boot/`, `src/desktop/`, or `src/shell/`: the skill applies. Do not skip. Past incidents: SMP race, memory leak, wrong test assertion -- all caused by writing code before checking quality gates.
6. **Implement** -- bounded scope only.
   - Freestanding kernel: `#include "kernel/types.h"` -- no `<stdint.h>`, `<string.h>`.
   - No `malloc()`/`printf()` -- use `kmalloc()` (<=4 KB), `pmm_alloc_contiguous()` (larger), `printk()`.
   - Assembly: NASM x86-64 only. UEFI-era, Long Mode, APIC -- no BIOS/VGA/PIC.
   - API surface: Win32 native. Windows-style canonical paths (`C:\Impossible\System32\`).
   - **POST16 codes are for BOOT-PATH code ONLY.** POST16 was designed for pre-`sti` triple-fault diagnostics where klog isn't yet running. Use POST16 only when the code might trip a triple fault before klog is initialized: Phase 0/1/2 boot, hardware init (CPUID, GDT, IDT, page tables, APIC, ACPI, SMP AP startup), or any function called from `boot_init.c` before `boot_phase3()` returns. Do NOT add POST16 to scheduler, syscall handlers, file I/O, ELF loading, exec, network, or any code that runs after Phase 3 completes -- a single `klog(LOG_INFO, ...)` line is more useful there. If `boot_init.h` defines a POST16 constant, check `boot_phase0/1/2.c` for the call site before assuming new code needs one.
   - **Scope-gap protocol (MANDATORY).** If while writing code you find yourself about to add a `// TODO`/`// FIXME`/`// HACK`/`// for now`/`// placeholder` comment, a `STATUS_NOT_IMPLEMENTED` return, a stub function body, or a conditional that narrows supported input below what the section's user-visible behavior promises -- STOP. You have hit a scope gap. Walk the decision tree in [scope-gap-protocol.md](scope-gap-protocol.md): Branch A (inline expansion under 1000 lines with `[x]` checklist item + NOTE callout), Branch B (new section in same TODO), Branch C (new TODO via `/create-todo` after dedup sweep), or Branch D (add to existing TODO found during dedup). Gate 10 of `kernel-code-quality` forbids the TODO comment; the scope-gap protocol is how you resolve the gap without breaking Gate 10. Report the branch you took in chat for clear cases; ASK the user which branch if sizing is borderline (900-1100 lines, unclear subsystem boundary).
   - **Stale TODO patterns:** if the section's checklist explicitly demands `POST16(...)` codes for non-boot-path code (e.g., scheduler/exec/syscall/auxv work), treat that as a stale guideline from before this rule was added. SKIP the POST16 work AND remove the stale checklist item from the TODO -- do not satisfy obsolete patterns. The same applies to "test POST16 constants are 0xDDNN" items: those are tautological (the compiler enforces #define values; the real protection is the boot-time uniqueness check). Document the removal in the commit message.
7. **Build** -- `bash scripts/build.sh`, confirm `tail -1 build/build.log` shows `=== BUILD OK ===`.
8. **Wire unit tests** -- before writing any test code, read the TODO file's **Unit Tests** section (if one exists) to find:
   - The expected test file name (e.g. `test_exec.c`)
   - The expected registration function (e.g. `test_register_exec()`)
   - The expected test category (e.g. `TEST_CAT_EXEC`)
   - Which specific assertions are required for this section
   
   Create the test file / registration function if it doesn't exist yet. Do NOT piggy-back tests onto an unrelated test file just because it's convenient. If the subsystem doesn't fit existing `TEST_CAT_*` categories, create a new one (enum in `test.h`, names/labels in `test_runner.c`, `make test-*` target in Makefile). Then add/update assertions and confirm build passes.
   - **No tautological constant tests.** A test like `TEST_ASSERT_EQ(POST16_FOO, 0xDF20, "POST16_FOO == 0xDF20")` only verifies that you typed `0xDF20` in the `#define` -- the compiler already enforces that. The real protection is the boot-time uniqueness check that scans `boot_init.h` for duplicate codes. Same goes for `TEST_ASSERT_EQ(SOME_DEFINE, expected_value)` where the assertion just echoes the literal: skip it. Tests should exercise behavior, not re-state literals.
   - **HARD BAN: tests must NEVER call live boot infrastructure.** WSL has no working QEMU, so runtime regressions in tests are not caught until the user boots on native Windows or bare metal -- 3 incidents to date. Forbidden in `src/kernel/test/test_*.c`: `boot_progress(`, `boot_post_write16(`, `boot_post_nvram_write16(`, `post_display16(`, `vpd_stage_*(`, `vpd_init(`, `boot_splash_*(`, `boot_halt(`, `panic(`, `KeBugCheckEx(`, any subsystem `_init(` (`pmm_init`, `vmm_init`, `heap_init`, `serial_init`, `klog_early_init`, `klog_disk_enable`, `acpi_init`, `lapic_init`, `ioapic_init`, `timer_hal_init`, `gdt_init`, `idt_init`). **Allowed alternatives:** pure constant checks, save/restore wrappers around `kernel_subsystem_set_ready`/`_ready`, direct calls to PURE data helpers (`boot_timing_record_step()` is OK -- in-memory append only; `boot_progress()` is NOT because it ALSO updates VPD/framebuffer), BOOT_REQUIRE/BOOT_STEP via wrapper functions, read-only oracle queries. The pre-commit hook in `settings.json` enforces this -- a test file with forbidden calls cannot be committed. See `feedback_test_no_live_boot_calls` memory and CLAUDE.md "Test Code -- No Live Boot Infrastructure Calls."
9. **Codex test coverage analysis** -- after wiring tests, dispatch a test coverage gap analysis to catch missing assertions before the adversarial review finds them. Follow the `codex-test-coverage` skill: list public functions, existing tests, and ask Codex to find untested error paths, boundaries, and negative cases. **If Codex recommends testing a forbidden function, REJECT with code evidence** -- Codex doesn't know WSL constraints. Test the underlying pure helper, or accept the gap with a `**Note:**` line in the TODO's Unit Tests section. Add any other missing tests found. Skip for trivial sections (< 3 test assertions).
    ```bash
    node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<test coverage prompt>"
    ```
10. **Update TODO section** -- mark items with evidence:
    - **Done proof gate:** `[x]` only when implemented + wired + functional on normal path. Never `[x]` for stubs or `STATUS_NOT_IMPLEMENTED` placeholders.
    - SSDT claims: verify service number <-> function <-> `ssdt_register()` <-> master table row consistency. If SSDT handlers were implemented, update `s_kernel32_exports[]`/`s_ntdll_exports[]` in `pe.c` for any Win32 wrapper names.
    - Blocked items: keep `[ ]` or `[/]`, add missing prerequisite/ownership items with owner scope and `-> XREF`.
    - **Deferred-item resolution:** scan earlier sections in the SAME TODO for items marked "deferred to section N" where N is this section. If the work was done, mark them `[x]`. Deferred items are promises.
    - **Cross-TODO sync:** when this section references or satisfies external TODO requirements, update those TODOs in the same run.
    - Preserve existing formatting (table headers, icons, column structure).
11. **Update Implementation Order table** -- `[x]` (fully done) or `[/]` (in progress).
12. **Update OS Comparison table** -- replace placeholders with concrete descriptions. `Planned` -> `Done` or `Partial`. If new research was done during implementation, update the `<!-- Sources: ... -->` comment after the table.
13. **Codex adversarial review** (MANDATORY) -- run via the Codex plugin, NOT self-review. Self-review has implementation bias; Codex reads code fresh and catches things you rationalized away. Proven: section 1 Codex found 2 Critical issues (SMP race, TOCTOU) that self-review missed. Follow the `codex-adversarial-review-section` skill workflow: scope to changed files, list adversarial angles, request severity-labeled findings. Command:
    ```bash
    node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<focus prompt>"
    ```
14. **Fix loop** (1 round mandatory; up to 3 if needed) -- apply `superpowers:receiving-code-review` discipline: do NOT blindly implement every Codex finding. For each finding:
    - **Verify technically first.** Read the code Codex flagged. Is the finding correct? Codex can be wrong -- it doesn't have full runtime context.
    - **If the finding is valid:** fix the root cause, not the surface symptom. Rebuild.
    - **If the finding is wrong or misleading:** reject with a concrete technical reason (not "I disagree" -- explain WHY it's wrong with code evidence).
    - **If the finding is correct but out of scope:** accept with justification and note it as a follow-up item.
    - Fix all valid Critical and High. Fix valid Medium unless explicitly accepted.
    - **Re-review via Codex is REQUIRED only when fixes are STRUCTURAL** (function signature changes, control-flow rewrites, new locking, lifecycle changes, refactors that ripple to multiple files). For SURGICAL fixes (single-line bug, contract clarification, small logic correction, comment fix, regression test only), self-verification is sufficient: re-read the cited code and confirm the fix matches what Codex flagged, then proceed to step 15. The self-verify path saves 1-3 minutes per round and avoids re-review noise on trivial corrections.
    - If unresolved Critical/High remain after round 3: do not mark section complete, keep `[/]` or `[ ]`, add follow-up items with `→ XREF`.
15. **Final self-review** (MANDATORY) -- this catches what Codex misses at the integration level:
    - Regressions: did any existing functionality break?
    - Race conditions: any new shared mutable state without synchronization?
    - Bugs: edge cases, off-by-one, null pointer paths?
    - Performance: unnecessary allocations, O(n^2) where O(n) suffices?
    - Industry standards: no hacks, no patches, no workarounds, no TODO/FIXME in new code.
    - **Checklist item-by-item:** walk every `- [ ]` item in the section. For each one: is it implemented and wired (`[x]`), explicitly deferred with justification (`[/]`), or blocked with notes (`[ ]` + blocker)? If any item was silently skipped, go back and address it now.
    - Deferred items: are there items from earlier sections that were "deferred to this section"? If so, were they resolved?
16. **2nd final build** -- `bash scripts/build.sh`, confirm `=== BUILD OK ===`. This catches anything broken by the fix loop or self-review changes.
17. **Validate section** (MANDATORY) -- invoke `validate-todo-section` skill. Evidence-based checklist classification. Catches stale/optimistic status claims. Re-checks cross-TODO synchronization.
18. **Tie up loose ends** (MANDATORY) -- scan the ENTIRE TODO file and any XREF'd TODO files for:
    - Deferred items pointing to this section that weren't resolved in step 10.
    - Stale warning boxes that should be updated to NOTE (resolved).
    - Implementation Order rows that need status updates.
    - Cross-TODO dependency notes that are now satisfied.
    - Any checklist items in other sections affected by this implementation.
    - **PE export table sync:** if this section implemented new public APIs callable from user-mode, verify they're in `s_kernel32_exports[]`/`s_ntdll_exports[]` in `pe.c`.
    - **SSDT audit trigger:** if this section implemented or modified SSDT handlers, recommend running `/audit-ssdt` after commit to verify master table consistency.
19. **Commit and push** -- only after steps 13-18 are ALL complete.
    - Use the section's `Commit:` line as the commit message.
    - Stage all changed source files, headers, the updated TODO file(s), and test changes together.
    - Push to `origin/main` immediately after successful commit.
    - Mark the section's `- [ ] Commit: "..."` item `[x]`.

## HARD GATE: Steps 13-18 are MANDATORY before step 19

> **You MUST NOT `git commit` or `git push` until steps 13 through 18 have all been completed.**
>
> This is not optional. This is not skippable for "simple" changes. Every section goes through this pipeline. No exceptions.
>
> If you find yourself about to run `git add` and you have not yet:
> - (13) Dispatched Codex adversarial review and received severity-labeled findings
> - (14) Fixed all Critical/High findings with build evidence
> - (15) Done final self-review for regressions, races, bugs, completeness
> - (16) Confirmed 2nd final build passes
> - (17) Invoked `validate-todo-section` skill and reconciled status
> - (18) Scanned for and tied up loose ends in current and XREF'd TODO files
>
> then **STOP and go back to step 13**.
>
> **Pattern to watch for:** "Build passes, looks straightforward, I'll just commit." That thought is the signal to STOP. The straightforward changes are exactly the ones where review catches the bug you didn't think about.

## Mirror with verify-todo-section

This skill is the IMPLEMENT half of a mirrored pair with `verify-todo-section`. Step numbering, phase boundaries, and the HARD GATE are kept identical between the two files so improvements ported into either skill can be ported back to the other.

**Sync rules:**
- Step N here ↔ step N in `verify-todo-section/SKILL.md`. Adding a step in one file requires adding the matching step in the other at the same position.
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
| 6 | **WRITE code** (the implementation) | **VERIFY each `[x]` claim with grep/read evidence** (the audit) |
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

## Guardrails

- **Commit after each section.** Never batch multiple sections into one commit.
- **Steps 13-18 are blocking prerequisites for step 19.** This guardrail exists because it was violated multiple times -- the pattern of "build passes, skip review, commit" must be broken.
- Do not mark items `[x]` from intent, partial progress, or stubs. Require implementation + wiring evidence.
- For SSDT sections, verify service number <-> function <-> registration <-> master table consistency before editing status.
- If prerequisite work is missing, keep items open and add explicit prerequisite ownership items.
- Do not auto-close referenced TODO items without `ID`/`SATISFIES` mapping plus full acceptance proof.
- Do not create new TODO files here.
- Do not turn this into a broad file-wide cleanup pass.
- Do not silently widen scope when requirements conflict with reality. If the section is missing work the user-visible behavior requires, invoke the scope-gap protocol (step 6) -- never patch with a `// TODO` comment or a `STATUS_NOT_IMPLEMENTED` stub.

## Additional Resources

- [build-evidence.md](build-evidence.md) -- shared with verify-todo-section (single source of truth)
- [scope-gap-protocol.md](scope-gap-protocol.md) -- shared with verify-todo-section (Branches A/B/C/D decision tree for scope gaps)
