---
name: implement-todo-section
description: Execute one bounded TODO section, resolve XREF dependencies, run Codex adversarial review, self-review for regressions, validate section, tie up loose ends, and commit. Use when implementing a specific TODO section or a clearly scoped subset of one.
---

# Implement TODO Section

> `verify-todo-section` has its own streamlined workflow (13 steps). The two skills share principles (HARD GATE, Codex review, scope-gap protocol, guardrails) but NOT step numbers. Editing this file does not require editing verify.

## Execution Discipline

> **NO CODE SHIPS WITHOUT BEING EXAMINED FROM EVERY ANGLE.**
>
> You cut corners: skipping Codex, self-reviewing instead of dispatching, accepting "clean" without walking quality gates, skipping the post-implementation review pipeline. Your judgment about which steps to skip has been wrong. Follow every step mechanically.
>
> - **Follow the plan, don't improvise.** The checklist items define scope. Do not widen.
> - **Every Codex dispatch is mandatory.** Step 13 adversarial + step 20 review pipeline. No exceptions.
> - **Every finding gets fixed.** Not noted. Not accepted unless it truly needs missing infrastructure.
> - **Domain code quality gates get walked explicitly.** Not just the hook reminder.
> - **One section = one commit.** Each section is a milestone with a clean commit boundary.

## Workflow

1. **Read the section** -- full text, notes, test checkpoint, warning boxes. If the section has > 10 checklist items, flag it: suggest splitting into **two top-level `##` sections** (new section numbers) before implementing. Never split with `N.M` sublabels (`17.1`, `### 17.2`, `**17.3 Foo**`) -- that pattern is forbidden; see `validate-todo-file`. A 15-item section is two sections pretending to be one.
2. **Resolve dependencies** -- follow every `-> XREF:` line. Stop and ask if a prerequisite is incomplete or scope conflicts with reality. If part is implementable and part is blocked, implement only the unblocked subset; keep blocked items `[ ]` or `[/]` with explicit blocker notes.
3. **Explore the codebase** -- Grep/Glob for symbols, call-graph tracing, cross-file discovery. Read relevant source files to understand the integration surface.
4. **Codex design review** (for complex/high-risk sections) -- if the section touches SMP-sensitive code, boot-path, page tables, interrupt handling, or security-critical logic, dispatch a design review via the Codex plugin BEFORE writing code. Follow the `codex-design-review` skill: send the plan + integration surface + constraints, evaluate for blockers/warnings. Skip for straightforward sections (simple struct definitions, single-function additions, test-only work).
    ```bash
    node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<design review prompt>"
    ```
5. **Run the domain-appropriate code quality skill** -- walk the gates BEFORE writing code. The hook auto-selects based on path:
   - `src/boot/` -> `boot-code-quality` (UEFI error handling, EBS boundary, table safety, fallbacks)
   - `src/kernel/`, `include/kernel/` -> `kernel-code-quality` (SMP safety, memory rules, bare-metal)
   - `src/desktop/` -> `desktop-code-quality` (WC mapping, compositor loop, pixel format)
   - `src/shell/` -> `shell-code-quality` (Win32 console API, path conventions)
   - `user/`, `src/apps/` -> `userland-code-quality` (syscall interface, no kernel headers)
   Do not skip. Past incidents: SMP race, memory leak, wrong test assertion, FrameBufferBase crash -- all caused by writing code before checking quality gates.
6. **Implement** -- bounded scope only.
   - Freestanding kernel: `#include "kernel/types.h"` -- no `<stdint.h>`, `<string.h>`.
   - No `malloc()`/`printf()` -- use `kmalloc()` (<=4 KB), `pmm_alloc_contiguous()` (larger), `printk()`.
   - Assembly: NASM x86-64 only. UEFI-era, Long Mode, APIC -- no BIOS/VGA/PIC.
   - API surface: Win32 native. Windows-style canonical paths (`C:\Impossible\System32\`).
   - **POST16 codes are for BOOT-PATH code ONLY.** POST16 was designed for pre-`sti` triple-fault diagnostics where klog isn't yet running. Use POST16 only when the code might trip a triple fault before klog is initialized: Phase 0/1/2 boot, hardware init (CPUID, GDT, IDT, page tables, APIC, ACPI, SMP AP startup), or any function called from `boot_init.c` before `boot_phase3()` returns. Do NOT add POST16 to scheduler, syscall handlers, file I/O, ELF loading, exec, network, or any code that runs after Phase 3 completes -- a single `klog(LOG_INFO, ...)` line is more useful there. If `boot_init.h` defines a POST16 constant, check `boot_phase0/1/2.c` for the call site before assuming new code needs one.
   - **Scope-gap protocol (MANDATORY).** If while writing code you find yourself about to add a `// TODO`/`// FIXME`/`// HACK`/`// for now`/`// placeholder` comment, a `STATUS_NOT_IMPLEMENTED` return, a stub function body, or a conditional that narrows supported input below what the section's user-visible behavior promises -- STOP. You have hit a scope gap. Walk the decision tree in [scope-gap-protocol.md](scope-gap-protocol.md): Branch A (inline expansion under 1000 lines with `[x]` checklist item + NOTE callout), Branch B (new section in same TODO), Branch C (new TODO via `/create-todo` after dedup sweep), or Branch D (add to existing TODO found during dedup). Gate 10 of `kernel-code-quality` forbids the TODO comment; the scope-gap protocol is how you resolve the gap without breaking Gate 10. Report the branch you took in chat for clear cases; ASK the user which branch if sizing is borderline (900-1100 lines, unclear subsystem boundary).
   - **STATUS_NOT_IMPLEMENTED completion-first policy.** The aim is to write complete code so features do not get lost behind stubs. If a function would return `STATUS_NOT_IMPLEMENTED`: (1) if it is **standalone** (self-contained, under 1000 lines, no deep dependency chain), implement it fully via Branch A; (2) if it requires **significant new infrastructure**, use Branch B/C/D to create a tracked TODO item with a domain-qualified XREF (e.g., `-> XREF: 02-kernel-core/TODO-17 §N`). Never leave a `STATUS_NOT_IMPLEMENTED` stub without a tracked follow-up.
   - **Stale TODO patterns:** if the section's checklist explicitly demands `POST16(...)` codes for non-boot-path code (e.g., scheduler/exec/syscall/auxv work), treat that as a stale guideline from before this rule was added. SKIP the POST16 work AND remove the stale checklist item from the TODO -- do not satisfy obsolete patterns. The same applies to "test POST16 constants are 0xDDNN" items: those are tautological (the compiler enforces #define values; the real protection is the boot-time uniqueness check). Document the removal in the commit message.
7. **Build** -- `bash scripts/build.sh`, confirm `tail -1 build/build.log` shows `=== BUILD OK ===`.
8. **Wire unit tests** -- before writing any test code, read the TODO file's **Unit Tests** section (if one exists) to find:
   - The expected test file name (e.g. `test_exec.c`)
   - The expected registration function (e.g. `test_register_exec()`)
   - The expected test category (e.g. `TEST_CAT_EXEC`)
   - Which specific assertions are required for this section
   
   Create the test file / registration function if it doesn't exist yet. Do NOT piggy-back tests onto an unrelated test file just because it's convenient. If the subsystem doesn't fit existing `TEST_CAT_*` categories, create a new one (enum in `test.h`, names/labels in `test_runner.c`, `make test-*` target in Makefile, `bootx64.c` test_suite parser, AND `scripts/debug/run-<cat>-tests.bat`). Then add/update assertions and confirm build passes.
   
   > **CRITICAL (incident 2026-04-12): Every section MUST have at least one unit test wired.** Multiple sections (TODO-03 §1-§12, TODO-17 §1-§3) shipped without tests, requiring after-the-fact test creation. Tests catch real bugs -- the TODO-19 §1 review found 3 critical FPU context switch bugs that unit tests would have caught earlier. If the section has no testable surface (pure bootloader UEFI code with no kernel-side fields), document why in the TODO section with `**Note:** No kernel test surface -- validation via serial log on WHPX.`
   
   - **After wiring tests, add a note to the TODO section** documenting which bat file to run and expected results:
     ```
     > **Test runner:** `scripts\debug\run-<category>-tests.bat` (SUITE=<cat>)
     > **Expected:** N suites, 0 failures
     ```
   - This note goes after the Test checkpoint paragraph and before the stamps. It tells the user exactly how to validate on Windows QEMU.
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
    - **No N.M subnumbering:** never add `### N.M`, `**N.M ...**`, or extra heading levels that carve one `## N.` into sub-chapters. Keep **one continuous** `- [ ]` list under each `## N.`; put grouping in bullet wording. Need more structure -- add a new `##` section with the next number, not `17.1`/`17.2`.
11. **Update Implementation Order table** -- `[x]` (fully done) or `[/]` (in progress).
12. **Update OS Comparison table** -- replace placeholders with concrete descriptions. `Planned` -> `Done` or `Partial`. **Do not** add or extend `<!-- Sources: ... -->` URL comment blocks; cite new research in the PR or chat only.
13. **Codex adversarial review** (MANDATORY -- NO EXCEPTIONS) -- dispatch to Codex plugin. If Codex responds with "no diff available", re-dispatch with actual file content (read 100-200 relevant lines). A shallow response requires re-prompting.
    ```bash
    node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<prompt with ALL mandatory angles>"
    ```
    **Mandatory angles (include ALL):** integer overflow, buffer overread, NULL deref, SMP races, resource leaks, ABI mismatch, bounds on untrusted data.

    After Codex responds, apply `superpowers:receiving-code-review` to EVERY finding. **Codex can be wrong** -- it reads code without runtime context and makes incorrect inferences. For EACH finding:
    - **Verify technically first.** Read the actual code at the cited file:line. Does the finding match reality? Check callers, check locks held, check whether the path is reachable.
    - **If valid:** fix root cause (not surface symptom), rebuild. No performative agreement ("great catch") -- just state the fix.
    - **If wrong/misleading:** reject with concrete code evidence (caller already holds lock X at file:line, path is unreachable because Y, buffer is bounded by Z). Do NOT blindly implement a fix for a wrong finding.
    - **If out of scope:** accept with domain-qualified XREF. Must truly need missing infrastructure -- not a lazy deferral.
14. **Self-review BEFORE fixing** -- catches what Codex misses at the integration level:
    - Regressions: did any existing functionality break?
    - Race conditions: any new shared mutable state without synchronization?
    - Edge cases: off-by-one, null pointer paths, boundary values?
    - Resource leaks on error paths?
    - Industry standards: no hacks, no TODO/FIXME in new code.
    - **Checklist item-by-item:** is each item `[x]`, `[/]`, or `[ ]` with explicit justification?
    - Deferred items from earlier sections resolved?
15. **Fix loop** (1 round mandatory; up to 3 if needed):
    - Fix all valid Critical and High from BOTH Codex (step 13) and self-review (step 14).
    - Fix valid Medium unless explicitly accepted.
    - Rebuild after fixes.
    - Re-dispatch Codex if fixes are STRUCTURAL. Surgical fixes: self-verify and proceed.
    - Unresolved Critical/High after round 3: do not mark section complete.
16. **2nd final build** -- `bash scripts/build.sh`, confirm `=== BUILD OK ===`. This catches anything broken by the fix loop or self-review changes.
17. **Validate section** (MANDATORY) -- invoke `validate-todo-section` skill. Evidence-based checklist classification. Catches stale/optimistic status claims. Re-checks cross-TODO synchronization.
18. **Tie up loose ends** (MANDATORY) -- scan the ENTIRE TODO file and any XREF'd TODO files for:
    - **N.M structure drift:** if any `## N.` uses forbidden `### N.M` or `**N.M ...**`, merge to one checklist per section per `validate-todo-file`.
    - Deferred items pointing to this section that weren't resolved in step 10.
    - Stale warning boxes that should be updated to NOTE (resolved).
    - Implementation Order rows that need status updates.
    - Cross-TODO dependency notes that are now satisfied.
    - **Inbound `> **Accepted:**` sweep (MANDATORY).** Any `[x]` item this section just closed is almost certainly the target of `> **Accepted:**` stamps in other TODO sections. Run `grep -rn "TODO-XX §N"` (and, if the item name was quoted, the quoted item name too) across `todo/` to find every inbound reference. For each match:
        - If the inbound `Accepted` entry's concern is FULLY resolved by this section's work: DELETE the entire `> **Accepted:**` line. If it was the only Accepted entry on that line, remove the line entirely; keep the Verified + Quality reviewed stamps adjacent.
        - If the inbound entry is PARTIALLY resolved (you closed one of several concerns on the line): rewrite the line, dropping the resolved concern while preserving the remaining XREFs.
        - If uncertain, leave the entry and note the ambiguity in chat so the user can decide.

      **Why this step exists:** otherwise `> **Accepted:**` lines accumulate indefinitely and lose their value as a "what's still deferred on this section" scan target. The XREF target moving to `[x]` is exactly when the inbound reference becomes stale.
    - Any checklist items in other sections affected by this implementation.
    - **PE export table sync:** if this section implemented new public APIs callable from user-mode, verify they're in `s_kernel32_exports[]`/`s_ntdll_exports[]` in `pe.c`.
    - **SSDT audit trigger:** if this section implemented or modified SSDT handlers, recommend running `/audit-ssdt` after commit to verify master table consistency.
    - **Filed-in-owner check:** any follow-up `[ ]` item that names an owner (e.g., "tracked in TODO-XX §N", "owner: TODO-YY") must ALSO be filed as a checklist item in that owner section with reciprocal `→ XREF`. If the owner section doesn't exist yet, find or create one via scope-gap protocol Branch C/D before filing. A note here alone is a dead-end paper trail.
    - **Accepted-XREF concreteness check (MANDATORY):** for every Codex finding that step 13 marked "Accepted with XREF" (out-of-scope deferral), open the XREF target and verify a **concrete `[ ]` checklist item** exists that would close the gap when checked. A section title, an enum definition, or prose mention is NOT concrete. If the target lacks such an item, create one NOW: write a checklist item that names the source file/function to fix, the helper to add (with signature), and the validation behavior. If no owner section exists or fits, follow scope-gap protocol Branch C/D to create one BEFORE marking the section complete. Update the Accepted XREF in any chat output and in TODO stamps to reference the concrete item by name/line. **Why this matters:** "Accepted with XREF: TODO-XX §N" with no concrete item there is a paper trail that someone later finds empty. Every accepted finding must be exactly one `[x]` away from being fully closed.
19. **Commit and push** -- only after steps 13-18 are ALL complete.
    - Use the section's `Commit:` line as the commit message.
    - Stage all changed source files, headers, the updated TODO file(s), and test changes together.
    - Push to `origin/main` immediately after successful commit.
    - Mark the section's `- [ ] Commit: "..."` item `[x]`.

## Phase 2: Post-Implementation Review (MANDATORY -- NO EXCEPTIONS)

> After the implementation commit, invoke the review pipeline to verify and quality-review the freshly implemented section. This catches issues that implementation-time Codex missed because it reviewed the diff in isolation.
>
> **This step is NEVER skippable.** Not for "straightforward plumbing." Not for "Codex already ran in step 13." Not for "same pattern as the previous section." Step 13 reviews the diff in isolation during implementation; step 20 reviews the committed code in full context -- they catch different classes of bugs. The pattern of "looks simple, I'll skip the review" is the exact signal to NOT skip. (Incident 2026-04-12: §2 review skipped, user had to ask for it.)

20. **Invoke `/review-todo-section`** on the section just committed. This runs:
    - Phase 1: evidence mapping + test checkpoint verification (skips adversarial Codex since step 13 already ran it)
    - Phase 2: quality review with MANDATORY Codex perf/consistency/dead-code dispatch
    - Fixes from the review are committed separately (the skill handles its own commit).

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

## Relationship to verify-todo-section

`verify-todo-section` has its own streamlined 13-step workflow optimized for auditing existing code. The two skills share principles (HARD GATE, Codex review, scope-gap protocol, guardrails) and resources ([scope-gap-protocol.md](scope-gap-protocol.md), [build-evidence.md](build-evidence.md)) but NOT step numbers. Editing this file does not require editing verify, and vice versa.

## Guardrails

- **Commit after each section.** Never batch multiple sections into one commit.
- **Steps 13-18 are blocking prerequisites for step 19.** This guardrail exists because it was violated multiple times -- the pattern of "build passes, skip review, commit" must be broken.
- Do not mark items `[x]` from intent, partial progress, or stubs. Require implementation + wiring evidence.
- For SSDT sections, verify service number <-> function <-> registration <-> master table consistency before editing status.
- If prerequisite work is missing, keep items open and add explicit prerequisite ownership items.
- Do not auto-close referenced TODO items without `ID`/`SATISFIES` mapping plus full acceptance proof.
- Do not create new TODO files here.
- Do not turn this into a broad file-wide cleanup pass.
- Never introduce N.M subnumbering in TODO markdown (`17.1`, `### 3.2`, bold `**4.1**` pseudo-headings). Full-file rule: `validate-todo-file`.
- Do not silently widen scope when requirements conflict with reality. If the section is missing work the user-visible behavior requires, invoke the scope-gap protocol (step 6) -- never patch with a `// TODO` comment or a `STATUS_NOT_IMPLEMENTED` stub.

## Additional Resources

- [build-evidence.md](build-evidence.md) -- shared with verify-todo-section (single source of truth)
- [scope-gap-protocol.md](scope-gap-protocol.md) -- shared with verify-todo-section (Branches A/B/C/D decision tree for scope gaps)
