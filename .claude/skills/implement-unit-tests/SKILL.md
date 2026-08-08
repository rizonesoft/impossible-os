---
name: implement-unit-tests
description: Implement the Unit Tests section of a TODO file -- create the test file, register it in the test runner, build, and mark the section complete with notes. Use when a TODO has a Unit Tests section ready to implement.
---

# Implement Unit Tests

> **External-Reviewer Contract:** This skill dispatches Codex as part of its workflow. Every finding goes through `superpowers:receiving-code-review` (verify at file:line, Fix / Reject / Accept, never blind-implement). Canonical contract: [docs/infrastructure/ai-system.md#external-reviewer-contract-codex](../../../docs/infrastructure/ai-system.md#external-reviewer-contract-codex).

> **Pair with `superpowers:test-driven-development` for greenfield work.** When the function under test does NOT yet exist (or is being rewritten), invoke `superpowers:test-driven-development` BEFORE implementing the function: write the failing test first, see the red, write the minimal code to make it green, refactor with green tests as the rail. The flow this skill normally documents (test EXISTING code) is the post-hoc coverage path; the TDD skill is the inversion that catches design issues before they ossify. Per `feedback_mandatory_unit_tests` (3 critical FPU bugs surfaced when tests finally ran), this matters most for SMP-sensitive / context-switch / bare-metal code where the bug class is hard to observe after the fact.

## Use This Skill When

- A TODO file has a `## Unit Tests` section with `[ ]` checkboxes ready to implement.
- The code under test already exists and builds successfully.
- The user asks to "implement the unit tests" or "add tests for" a TODO.
- The `/implement-todo-section` pipeline reaches Stage 4 (test creation).
- Do NOT use for test gap analysis -- use `/codex-test-coverage` instead.

## Test Framework Reference

### File Locations

| Role | Path |
|------|------|
| Public API + macros | `include/kernel/test/test.h` |
| Runner + registration | `src/kernel/test/test_runner.c` |
| Boot dispatch | `src/kernel/main/boot_tests.c` |
| Test files | `src/kernel/test/test_*.c` |

### Categories

Every test suite must be registered with a category. Categories are defined in `test.h`:

```c
typedef enum {
    TEST_CAT_MM = 0,    // Memory Management (PMM, heap, VMM, swap, mmap)
    TEST_CAT_FS,        // Filesystem (VFS)
    TEST_CAT_SCHED,     // Scheduler
    TEST_CAT_OB,        // Object Manager
    TEST_CAT_SECURITY,  // Security
    TEST_CAT_IPC,       // IPC
    TEST_CAT_BOOT,      // Boot & Logging
    TEST_CAT_ABI,       // ABI Compatibility (registry, PEB/TEB)
    TEST_CAT_STORAGE,   // Storage Drivers
    TEST_CAT_EXEC,      // Binary System (exec, EIF, PE, modules)
    TEST_CAT_X86,       // x86 architecture (CPU features, MSRs)
    TEST_CAT_DESKTOP,   // Desktop compositor / UI
    TEST_CAT_EX,        // Executive support (ex* primitives)
    TEST_CAT_NLS,       // National language support
    TEST_CAT_KNF,       // Kernel notification facility
    TEST_CAT_EXCEPT,    // Exception dispatch / SEH
    TEST_CAT_QUOTA,     // Resource accounting & quotas
    TEST_CAT_COUNT,
    TEST_CAT_ALL = 0xFF, // Runs under any filter
} test_category_t;
```

> This block is a convenience copy. `include/kernel/test/test.h` is the source of truth -- read it before adding a category, and update this block in the same commit that grows the enum.

The `cat_names[]` and `cat_labels[]` arrays in `test_runner.c` must stay aligned with this enum. If adding a new category, update all three locations plus the `bootx64.c` parser.

### Assertion Macros

```c
TEST_ASSERT(cond, msg)             // boolean condition
TEST_ASSERT_EQ(a, b, msg)         // a == b (both cast to uint64_t)
TEST_ASSERT_NEQ(a, b, msg)        // a != b (both cast to uint64_t)
TEST_ASSERT_NULL(p, msg)          // p == NULL
TEST_ASSERT_NOT_NULL(p, msg)      // p != NULL
TEST_SKIP(msg)                    // skip this check (counted separately)
TEST_PENDING(cond, msg)           // condition holds AND feature is intentionally
                                  //   deferred (slot reserved before its
                                  //   subsystem ships) -- counts in `pending`
                                  //   bucket, logs as [STUB] line
```

- On pass: increments `passed` counter, logs `[ OK ] TEST: suite :: msg` (unless quiet mode)
- On fail: increments `failed` counter, logs `[FAIL] TEST: suite :: msg (file:line)` with got/expected for EQ/NEQ
- On skip: increments `skipped` counter, logs `[WARN] TEST: suite :: SKIP: msg`
- On pending (`TEST_PENDING(true, ...)`): increments `pending` counter, logs `[WARN] TEST: suite :: [STUB] msg`
- On pending broken (`TEST_PENDING(false, ...)`): increments `failed` -- the deferred contract was supposed to hold and did not (e.g. mis-registered slot returned wrong status)
- End-of-run summary: `=== N tests passed, F failed, S skipped, P pending (X.Xs) ===`. The pending count is the canonical answer to "how many features are reserved-but-unimplemented?"
- There are no `TEST_PASS` or `TEST_FAIL` macros -- success is implicit when assertions pass

### When to use TEST_PENDING vs TEST_ASSERT (MANDATORY)

If a test asserts `STATUS_NOT_IMPLEMENTED` (or any equivalent "intentionally deferred" sentinel) as the EXPECTED outcome -- you are testing a stub. Use `TEST_PENDING` instead of `TEST_ASSERT`:

```c
/* WRONG -- treats deferred as success, hides the incomplete feature */
TEST_ASSERT(st == STATUS_NOT_IMPLEMENTED, "NtAlpcCreatePort returns deferred");

/* WRONG -- TODO ref drifts; verbose explanation belongs in source comment, not the runtime line */
TEST_PENDING(st == STATUS_NOT_IMPLEMENTED,
             "NtAlpcCreatePort (0x10f): ALPC engine deferred (TODO-12 s8 ships real handler)");

/* RIGHT -- name + slot + brief gap. Suite name already says "ALPC pending". */
TEST_PENDING(st == STATUS_NOT_IMPLEMENTED,
             "NtAlpcCreatePort (0x10f): no ALPC engine yet");
```

### Message format rules (MANDATORY)

1. **NO TODO / section refs in the runtime message.** They drift. Future-you renames §8 to §9 and the boot log shows wrong info forever. The TODO ref belongs in the SOURCE COMMENT next to the test. The runtime message describes the gap in concrete terms ("no ALPC engine yet", "no APC queue yet").

2. **NO Unicode in the runtime message.** ASCII only. Section sign `§` (U+00A7) and en/em dashes (U+2013, U+2014) garble in Windows serial terminals AND, when packed into long strings, can overflow the snprintf -> klog buffer chain and freeze boot. See CLAUDE.md "No Unicode Dashes".

3. **Keep messages short** -- under 50 characters when possible. The 31-slot LPC + ALPC pending sweep can trip klog's 100-msg-per-window rate limit; shorter messages reduce serial-log pressure and keep the boot log readable. Use `char msg[64]` not `char msg[96]` -- if your formatted message needs more than 64 bytes you are over-explaining.

4. **Suite name carries the subsystem** -- "OB: NT ALPC pending features" already says "this is an ALPC stub test". Don't repeat "ALPC" in every message; just name the function and the missing thing.

### Why this matters

- **Single source of truth.** The `pending` count tells you at a glance how many features are incomplete; `[STUB]` lines list each one. No need for parallel runtime klog warnings in the stub body itself -- doing both creates duplicate signal in the boot log (one [WARN] from the stub, one [ OK ] from the test, both saying the same thing).
- **No stale refs.** TODO refs in source comments stay synchronized with renumbering; refs baked into runtime klog strings do not. An audit script (`scripts/audit-stubs.sh`) can walk source comments and verify TODO refs resolve.
- **Visible incompleteness.** A regression where someone "implements" an SSDT slot that returns the wrong status is caught -- `TEST_PENDING` flips to FAIL when the deferred contract breaks, instead of silently passing.

If the stub body itself logs a runtime warning (e.g. an old per-call klog line), DELETE the runtime log when you add the `TEST_PENDING` test. Two signals for the same fact = noise.

### Registration API

```c
// Preferred -- register with a specific category
void test_suite_register_cat(const char *name, test_fn_t fn, test_category_t cat);

// Legacy -- registers with TEST_CAT_ALL (runs under any filter)
void test_suite_register(const char *name, test_fn_t fn);
```

Always use `test_suite_register_cat()` with the appropriate category.

## TDD Principles

> Apply `superpowers:test-driven-development` where possible:
> - **Write test assertions first** from the TODO's test case list BEFORE reading the implementation deeply. This ensures tests reflect the spec, not the code.
> - **Run the tests and watch them fail** -- confirms the assertions are actually checking something. A test that passes before implementation is wired is a useless test.
> - **Then read the implementation** to verify the tests match real behavior. Adjust only if the spec (TODO) is wrong, not because the code does something different.
> - If writing a test reveals a bug in the code under test, **fix the bug** using `kernel-code-quality` gates and `superpowers:systematic-debugging` discipline. A test-revealed SMP race needs proper diagnosis, not a quick patch.

**A fixture that cannot fail proves nothing, and it will pass.** Four inert probes shipped green in ONE section review on 2026-08-08, each inert a different way, so "watch it fail" above is not enough on its own -- name which of these you did:

- **Assert the SETUP took.** A fixture rewrote a cache as `d["nodes"]` when this repo's cache root IS the node array; the rewrite raised, the fixture silently read an ordinary file instead, and passed. Read the mutated artifact BACK and assert on it.
- **Reproduce the LIVE condition, not a convenient stand-in.** A coalescing fixture closed its pipe writer; EOF keeps a descriptor readable, so the drain "worked" -- under a real writer that never closes, the same drain returned 0 of 49 queued events. It tested EOF handling while claiming to test coalescing.
- **A mutation must edit the PRODUCTION source and run THAT.** A "mutation check" that re-implements the old algorithm inside the test watches its own copy fail; reverting the real call site leaves it green.
- **Chase a contradiction; do not ship past it.** When the mutant and the original measure the same, either the probe or the mutation is inert. If it cannot be explained, say so and LABEL the fixture as the structural pin it actually is rather than presenting it as a discriminating guard.

The same rule covers the ad-hoc measurements taken while DEBUGGING a fixture, which is where three more inert probes lived on the same day: every probe needs a control that MUST fire, and the filing/claim says that it did.

## Workflow

### 1. Read the Unit Tests section

> **Dispatch `Agent(subagent_type="test-coverage-mapper", ...)` BY DEFAULT first** -- it returns the test-writing brief (spec list with exists/partial/missing status, exact signatures/enums quoted from headers, behavior notes, `test_runner.c` wiring points) in a throwaway context. Verify quoted signatures against the header before writing (trust contract). Skip only when adding 1-2 assertions to an existing test file.

Read the exact `## Unit Tests` section in the target TODO file. It specifies:
- The test file to create (e.g., `src/kernel/test/test_boot_init.c`)
- The registration function name (e.g., `test_register_boot_init()`)
- The specific assertions to implement

### 2. Explore the code under test

Before writing any test code:
- Read the header file for the API being tested -- get exact function signatures, enum values, macro definitions
- Read the implementation to understand edge cases (NULL handling, out-of-range, etc.)
- Read an existing test file to match the exact pattern

### 2a. Spec-vs-reality reconciliation (MANDATORY before writing any code)

The TODO is a plan. The already-implemented test is reality. When they
disagree, the plan does NOT automatically win. Before touching any test
code, walk each `[ ]` item in the Unit Tests section and classify it
against the current tree:

1. **Item already implemented, spec matches reality** -- mark `[x]` with
   an inline note `-- already at src/kernel/test/test_<file>.c:LINE`.
   Do not re-write; do not modify the existing test. Move on.

2. **Item not yet implemented, spec matches reality contract** -- normal
   case. Proceed to step 3 and implement as described.

3. **Item already implemented, spec DIFFERS from reality** -- STOP. Do
   not edit the test to match the TODO yet. Classify the difference:

   a. **TODO is stale; implementation is correct.** The TODO was written
      against an older API, a different error code, a renamed function,
      or an assertion pattern that the project has since improved. The
      test works, covers the right behavior, and shipping the TODO diff
      would either break the test or weaken its invariant. **ACTION:**
      rewrite the TODO item text to match reality (same structure, same
      item shape, different words/assertion/expected value). Mark `[x]`
      with an inline note `-- reconciled: TODO item updated to match
      <what-actually-ships>; previous text <one-line old spec>`.
      Explain the reconciliation in the commit message body. Do NOT
      silently flip the item without updating its text -- a future
      reviewer reading the TODO should see what the test actually asserts.

   b. **TODO is correct; test is a quick patch or outdated.** Rare, but
      possible. The test passes, but it's asserting the wrong value (or
      an older value the API used to return). The TODO captured a
      deliberate API update that the test hasn't been revised for yet.
      **ACTION:** rewrite the test to match the TODO, rebuild, confirm
      the new assertion passes against the current API. If the new
      assertion FAILS against the current API, you're in case (a)
      instead, or the spec and the code are BOTH wrong (case d).

   c. **Partial overlap: test covers some of the spec, spec adds more.**
      **ACTION:** extend the existing test with the missing assertions
      (keep every assertion already present -- they prove behavior that
      someone intentionally added). Mark `[x]` only after the TODO's
      full assertion list is covered.

   d. **TODO change would cause a REGRESSION of real behavior.** The
      spec's new assertion is less strict, weaker, or would mark a bug
      as acceptable. Example: TODO says `TEST_ASSERT(ret == 0)` but the
      current test says `TEST_ASSERT(ret == STATUS_VALID_BUT_DEGRADED)`
      and the function actually returns `STATUS_VALID_BUT_DEGRADED`
      today as a deliberate downgrade signal. Applying the TODO diff
      would either (1) silently pass on a degraded return the project
      used to catch, or (2) fail on correct code. **NEVER APPLY THIS
      DIFF.** Leave the test untouched. Mark the TODO item with
      `[ ]` still; add an inline `> **Rejected:**` note under the item:
      `> **Rejected:** TODO item would regress existing coverage.
      Reality: test at <file:line> asserts <what>; current implementation
      returns <actual>. Applying the TODO diff would <specific regression>.
      TODO item dropped; see commit <hash> for rationale.` Flag in chat
      so the user can confirm the rejection, and **do not mark `[x]`
      without user sign-off on the rejection.** Regressions dressed up
      as "implementing the TODO" is exactly the failure mode this step
      exists to prevent.

4. **Item is ambiguous or contradicts itself.** E.g. the spec says "test
   X returns NULL on bad input" but also "test X returns -1 on bad
   input" in the same bullet. **ACTION:** grep the current code for
   what X actually returns; pick the one that matches. Rewrite the TODO
   item to remove the contradiction. Mark `[x]` with a reconciliation
   note.

**Core principle:** the TODO is a plan; the committed code is reality.
When they disagree and applying the plan would weaken tested behavior,
the plan is wrong. Keep the code. Update the TODO text to match reality.
Never introduce a regression to satisfy a stale checklist.

**What NOT to do in this step:**
- Do not silently flip `[x]` when the test already exists but asserts
  something different -- that hides the spec drift from future readers.
- Do not "upgrade" a stronger existing assertion down to a weaker TODO
  version. If the stronger version was a quiet improvement, the TODO
  needs to be updated to match.
- Do not delete existing tests just because the TODO's plan doesn't
  include them. Existing tests are evidence that SOMEONE added that
  assertion deliberately; the TODO is a wish list, not a scope fence.

**Worked example:** TODO says `TEST_ASSERT(st == STATUS_NOT_IMPLEMENTED, "...")`,
but the test file already has `TEST_PENDING(st == STATUS_NOT_IMPLEMENTED, "...")`.
The TODO is stale (predates the TEST_PENDING macro introduction 2026-04-07).
Case (a): rewrite the TODO item to use `TEST_PENDING`, mark `[x]`,
inline note `-- reconciled: TODO updated from TEST_ASSERT to TEST_PENDING
per feedback_mandatory_unit_tests / 2026-04-07 macro introduction`.

### 3. Create the test file

Create `src/kernel/test/test_<name>.c` following this pattern:

```c
/* ============================================================================
 * test_<name>.c -- <Subsystem> unit tests
 *
 * Tests <what is being tested>.
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/<header_under_test>.h"

/* ---- <Test group description> ---- */

static void test_<descriptive_name>(void)
{
    /* Setup */
    void *result = some_api_call();

    /* Assertions */
    TEST_ASSERT_NOT_NULL(result, "some_api_call returns non-NULL");
    TEST_ASSERT_EQ(some_value, EXPECTED, "value matches expected");

    /* Cleanup */
    cleanup(result);
}

static void test_<another_test>(void)
{
    TEST_ASSERT(condition, "description of what is being verified");
}

/* ---- Registration ---- */

void test_register_<name>(void)
{
    test_suite_register_cat("<PREFIX>: descriptive name", test_<descriptive_name>, TEST_CAT_<CATEGORY>);
    test_suite_register_cat("<PREFIX>: another test",     test_<another_test>,     TEST_CAT_<CATEGORY>);
}

#endif /* KERNEL_TESTS */
```

**Rules:**
- Wrap the entire file in `#ifdef KERNEL_TESTS` / `#endif`
- Include `"kernel/test/test.h"` first, then headers for the code under test
- Each test function is `static void`, takes no args, returns nothing
- Use descriptive suite names with a consistent prefix (e.g., `"OB: handle table"`, `"PMM: alloc frame"`)
- Always use `test_suite_register_cat()` with the correct `TEST_CAT_*`
- Save and restore global state if tests modify shared kernel state
- If a macro uses `return` (like `BOOT_REQUIRE`), test it via a wrapper function with the matching return type
- The registration function `test_register_<name>()` is NOT static -- it's `extern`'d from `test_runner.c`
- Max 64 suites total across all test files (`TEST_MAX_SUITES`)

> **CRITICAL -- NO LIVE BOOT INFRASTRUCTURE CALLS.** A pre-commit hook BLOCKS edits to test files containing the function calls below. WSL has no working QEMU; runtime regressions in tests are not caught until the user boots on native Windows or bare metal. **3 incidents to date** -- the latest froze WHPX boot when `test_boot_progress_records_step` called `boot_progress("VERIFY_TEST", 0xCAFE)`.
>
> **Forbidden in `src/kernel/test/test_*.c`:**
> - `boot_progress(`, `boot_post_write16(`, `boot_post_nvram_write16(`, `post_display16(` -- live VPD/framebuffer/I/O port/NVRAM
> - `vpd_stage_begin(`, `vpd_stage_done(`, `vpd_stage_fail(`, `vpd_init(` -- VPD state machine
> - `boot_splash_init(`, `boot_splash_status(`, `boot_splash_finish(`, `boot_splash_start_animation(`
> - `boot_halt(`, `panic(`, `KeBugCheckEx(`
> - Any subsystem `_init(` (`pmm_init`, `vmm_init`, `heap_init`, `serial_init`, `klog_early_init`, `klog_disk_enable`, `acpi_init`, `lapic_init`, `ioapic_init`, `timer_hal_init`, `gdt_init`, `idt_init`)
>
> **Allowed alternatives:**
> - Pure constant checks (`TEST_ASSERT_EQ(SUBSYS_OB, 20, ...)`)
> - Save/restore wrappers around `kernel_subsystem_set_ready/_ready` on a specific slot (`SUBSYS_PMM` is fine -- existing tests use it)
> - Direct calls to PURE data helpers -- `boot_timing_record_step()` is OK because it just appends to an in-memory buffer; `boot_progress()` is NOT because it ALSO updates VPD/framebuffer/serial
> - BOOT_REQUIRE/BOOT_STEP via wrapper functions returning the expected `boot_result_t`
> - Read-only oracle queries (`kernel_subsystem_ready()`, `boot_timing_get_steps()`)
>
> **If Codex test-coverage in step 7 recommends testing a forbidden function, REJECT** with code evidence (`receiving-code-review` discipline). Codex doesn't know about the WSL constraint. Test the underlying pure helper, or accept the gap with a `**Note:**` line in the TODO's Unit Tests section explaining why no test exists.
>
> **Opt-out for legitimate cases** (panic recovery testing in a controlled context, hardware fault simulation): add `/* TEST-SIDE-EFFECT-ALLOWED: <one-line reason> */` in the test function body. The hook honors this sentinel.

### 4. Register in the test runner

Edit `src/kernel/test/test_runner.c`:

1. Add `extern void test_register_<name>(void);` with the other extern declarations (near line 177)
2. Add `test_register_<name>();` call inside `test_runner_init()` under the appropriate category comment:
   ```c
   void test_runner_init(void)
   {
       klog(LOG_INFO, "TEST", "============ KERNEL UNIT TESTS ============");

       /* MM */
       test_register_pmm();
       test_register_heap();
       ...

       /* FS */
       test_register_vfs();

       /* Sched */
       test_register_sched();

       /* ABI */
       test_register_registry();
       test_register_peb_teb();

       /* Boot */
       test_register_boot_init();
       test_register_klog();

       /* OB */
       test_register_ob();

       /* Security */
       test_register_security();

       /* IPC */
       test_register_ipc();

       /* Storage */
       test_register_storage();

       // ADD YOUR NEW REGISTRATION HERE under the right category
   }
   ```

### 5. Build and verify

```bash
bash scripts/build.sh
tail -1 build/build.log  # must show === BUILD OK ===
```

The test file is auto-discovered by the Makefile (`find ... -name '*.c'`), so **no Makefile changes needed**.

`-DKERNEL_TESTS` is always in CFLAGS -- test code compiles unconditionally. Tests only _run_ when `test=1` or `debug=1` in boot.conf.

### 6. Run the tests

```bash
bash scripts/test.sh                        # all categories
bash scripts/test.sh SUITE=<category>       # just the new category
make test-<category>                        # verify the make target works too
```

Verify the category filter shows ONLY the expected tests, not all tests. If `make test-<category>` doesn't exist, add it to the Makefile.

If tests fail, use `/diagnose-serial-log` on the test output to classify and fix failures systematically rather than guessing at the cause.

### 7. Codex test coverage analysis

After wiring tests, dispatch a test coverage gap analysis to catch missing assertions. Apply `superpowers:receiving-code-review` discipline -- verify Codex suggestions technically before adding them.

```bash
bash scripts/codex-dispatch.sh '[review-kind: test-coverage] <todo-path> <test coverage prompt>'
```

Focus: untested error paths, missing boundary tests, missing negative tests. Add any valid missing tests found. Skip for trivial test suites (< 3 assertions).

### 8. Fix any bugs found during test development

If writing a test reveals a bug, fix the bug in the code under test using `kernel-code-quality` gates. For non-trivial bugs (SMP races, memory corruption, boot-order issues), apply `superpowers:systematic-debugging` -- diagnose root cause properly, don't quick-patch.

### 9. Mark the TODO section complete

Update the TODO file's `## Unit Tests` section:
- Mark `[x]` only items that are satisfied by evidence (either by code that shipped this run OR by the step 2a reconciliation when an existing test already satisfied the item). An item from case 2a(d) (rejected regression) stays `[ ]` with the `> **Rejected:**` note until the user signs off.
- For items whose text was rewritten in step 2a (cases a, c, d), the inline `-- reconciled: ...` note must appear alongside the `[x]` so a future reader can see that the item was updated to match reality, not silently flipped.
- Add a summary note at the bottom of the section. Include the reconciliation count so drift is visible at a glance:
  ```
  > **Done:** N suites, M assertions -- registered in `test_runner_init()` (YYYY-MM-DD)
  > **Reconciled:** K items rewritten to match reality (case a/c/d), R items rejected as regressions
  ```
  Omit the `Reconciled:` line when K=0 and R=0. Omit the `Done:` line when the whole section was already-implemented cases 2a(1) -- write `> **Reconciled: N/N items already implemented (2026-MM-DD)`.

If the TODO has an **Implementation Order table** with a row for unit tests, update its Status to `[x]` only when every Unit Tests item reached `[x]` (including the reconciled ones). A single `[ ]` rejected-as-regression item means the row stays `[/]` (partial) until the user confirms the rejection.

### 10. Commit

Use the commit message from the `Commit:` line in the Unit Tests section. Stage:
- The new test file (`src/kernel/test/test_<name>.c`)
- The modified `test_runner.c`
- The updated TODO file
- Any bug fixes found during test development

## Adding a New Category

If the existing categories don't fit, add a new one:

1. **`include/kernel/test/test.h`**: Add `TEST_CAT_<NAME>` to the enum before `TEST_CAT_COUNT`
2. **`src/kernel/test/test_runner.c`**: Add entries to both `cat_names[]` and `cat_labels[]` at the matching index
3. **`src/boot/uefi/bootx64.c`**: The bootloader parses `test_suite=<name>` strings to category indices -- add the new short name to the parser if you want `SUITE=<name>` to work from the command line
4. **`Makefile`**: Add a `test-<name>:` phony target: `@bash scripts/test.sh SUITE=<name>`
5. **`scripts/debug/kernel/run-<name>-tests.bat`**: Author the Windows-side bat runner so the new category can be invoked selectively from native Windows. Mirror the existing one-liner shape (e.g. `scripts/debug/kernel/run-mm-tests.bat`):
   ```bat
   @echo off
   :: run-<name>-tests.bat -- <one-line category description>
   powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -TestSuite <name>
   pause
   ```
   The `%~dp0..\..\machines\run-qemu.ps1` relative path is correct for the `scripts/debug/kernel/` subdir (the extra `..\` accounts for the directory split done 2026-04-20). DO NOT put the bat at `scripts/debug/run-<name>-tests.bat` (that root location is reserved for the cross-category aggregate `run-all-tests.bat`).

## Authoring runners for non-kernel tests

This skill primarily owns kernel-side `TEST_CAT_*` tests. When the TODO's Unit Tests section is for a different test layer, the bat-runner subdir changes:

- **Kernel TEST_CAT_* suite** (this skill's primary case): `scripts/debug/kernel/run-<cat>-tests.bat`. Compile-built into the kernel image, runs at boot under the in-kernel test runner, scraped via `[TEST]`/`[ OK ]`/`[FAIL]` serial lines. The "Adding a New Category" section above covers this.
- **User-mode test binary** (`user/test/test_*.exe`): `scripts/debug/usermode/run-<binary>.bat`. One bat per binary, each passing `utest_filter=<binary>` to QEMU so the kernel test launcher runs ONLY that binary. Owned by the user-mode test framework TODO; this skill does NOT author these directly.
- **Desktop UI test**: `scripts/debug/desktop/run-<test>.bat`. Empty subdir today; bats land when the desktop UI test framework TODO ships. This skill does NOT author these directly.

For all three subdirs, the root-level `scripts/debug/run-all-tests.bat` aggregate runner already chains the per-category aggregates (`kernel/run-all-kernel-tests.bat`, `usermode/run-all-usermode-tests.bat`, `desktop/run-all-desktop-tests.bat`) with cleanly-skipped no-ops where the category aggregate does not yet exist. New per-category bats land beside the aggregate; the aggregate picks them up automatically via `if exist ...`.

## Guardrails

- Only implement tests listed in the `## Unit Tests` section -- do not invent additional tests (Codex coverage analysis in step 7 may suggest additions, but apply `receiving-code-review` discipline)
- Do not modify the code under test unless a bug is found during testing
- Do not change other TODO sections
- Do not skip assertions listed in the TODO -- implement all of them
- If an assertion cannot be tested (e.g., requires hardware), note it in the TODO and mark it `[x]` with `(skipped: reason)`
- **NEVER introduce a regression to satisfy a stale TODO item.** If the Unit Tests section asks for a weaker assertion than the existing test already has, or asks to change an assertion in a way that would mask real behavior, the TODO item is wrong (see step 2a case d). Reject the diff, leave the existing test untouched, flag the rejection in chat, and wait for user sign-off before any further action. Every TODO item that reaches `[x]` must strictly INCREASE or preserve test coverage -- never decrease it.
- **Reconciliation notes are mandatory when the TODO item text changes.** If step 2a updates a TODO item's text to match the shipped test (cases a, c, d), the new text must be visible to a future reader and the inline `-- reconciled: ...` note explains WHY it changed. A silent flip `[x]` without a note is indistinguishable from a blind "marked done" lie.
- If the TODO lists 30+ test cases, consider splitting into multiple test files grouped by sub-feature rather than one giant file
- **Test isolation:** some subsystems (e.g., module registry `s_modules[]`, SSDT dispatch table) have no reset/cleanup mechanism. Tests that modify global state may affect later tests. Document this limitation in a comment and use unique values (e.g., unique base addresses) to avoid collisions with prior tests.
- If tests add POST16 codes, verify they don't conflict with existing codes in `boot_init.h`
- **Per-iteration assertion messages (MANDATORY for any TEST_ASSERT in a loop).** A `for`/`while` loop containing `TEST_ASSERT(cond, "literal message")` makes every passing iteration log the same line, so a mid-loop failure cannot be diagnosed from the boot log -- "LPC SSDT slot registered" 15 times tells you nothing about which slot. Two patterns:

  **Small-N loops (under ~50 iterations, each meaningful):** build the message per iteration with `snprintf` so the log identifies which case is being exercised. Use a `struct { ... key; const char *name; }` table so `cond` and `name` stay in lockstep:

  ```c
  static const struct { uint32_t svc; const char *name; } slots[] = {
      { SSDT_NtCreatePort,  "NtCreatePort"  },
      { SSDT_NtConnectPort, "NtConnectPort" },
      ...
  };
  char msg[96];
  for (i = 0; i < N; i++) {
      snprintf(msg, sizeof(msg), "%s (0x%x) registered",
               slots[i].name, (uint64_t)slots[i].svc);
      TEST_ASSERT(check(slots[i].svc), msg);
  }
  ```

  **High-N loops (hundreds of iterations -- byte/page scans):** per-iteration messages would explode the log. Track the first failing index inside the loop, then assert ONCE outside with the index baked into the message:

  ```c
  uint32_t mismatch_at = N;  /* sentinel: no mismatch */
  for (i = 0; i < N; i++) {
      if (dst[i] != src[i]) { mismatch_at = i; break; }
  }
  char msg[96];
  snprintf(msg, sizeof(msg),
           "memcpy_avx: dst matches src at every byte (first diff @ %u of %u)",
           (uint64_t)mismatch_at, (uint64_t)N);
  TEST_ASSERT(mismatch_at == N, msg);
  ```

  Both patterns require `extern int snprintf(char *buf, size_t size, const char *fmt, ...);` declared near the top of the test file (snprintf is not in freestanding kernel headers; see ob_section.c / ob_timer.c for the convention).

  The PostToolUse hook in `.claude/settings.json` (matcher `Edit|Write|MultiEdit` on `src/kernel/test/test_*.c`) flags new occurrences of this pattern at edit time. The exception list is small: tautological constant comparisons (already banned -- skip them entirely) and tests with a single `TEST_ASSERT` in the loop body whose message already contains a `%` formatter.

## Existing Test Files (Reference)

> **Keep this table current** when adding new test files. Run `ls src/kernel/test/test_*.c` to verify.

| File | Category | What it tests |
|------|----------|---------------|
| `test_pmm.c` | `TEST_CAT_MM` | Physical memory allocation |
| `test_heap.c` | `TEST_CAT_MM` | Heap allocation, edge cases |
| `test_vmm.c` | `TEST_CAT_MM` | Virtual memory mapping |
| `test_swap.c` | `TEST_CAT_MM` | Swap space management |
| `test_mmap.c` | `TEST_CAT_MM` | Memory-mapped regions |
| `test_vfs.c` | `TEST_CAT_FS` | VFS create/read/write/delete |
| `test_ixfs.c` | `TEST_CAT_FS` | IXFS filesystem operations |
| `test_sched.c` | `TEST_CAT_SCHED` | Scheduler task management |
| `test_registry.c` | `TEST_CAT_ABI` | Win32 registry operations |
| `test_peb_teb.c` | `TEST_CAT_ABI` | PEB/TEB ABI compatibility |
| `test_nt_types.c` | `TEST_CAT_ABI` | NT type sizes, SSDT constants |
| `test_boot_init.c` | `TEST_CAT_BOOT` | Boot init macros, state |
| `test_klog.c` | `TEST_CAT_BOOT` | Kernel logging subsystem |
| `test_ob.c` | `TEST_CAT_OB` | Object Manager (handles, refs, namespace) |
| `test_security.c` | `TEST_CAT_SECURITY` | Security subsystem |
| `test_ipc.c` | `TEST_CAT_IPC` | IPC mechanisms |
| `test_storage.c` | `TEST_CAT_STORAGE` | Storage driver operations |
| `test_blackbox.c` | `TEST_CAT_STORAGE` | BlackBox partition logging |
| `test_acpi_power.c` | `TEST_CAT_STORAGE` | ACPI power management |
| `test_bulletproof.c` | `TEST_CAT_ABI` | Kernel bulletproofing invariants |
| `test_exec.c` | `TEST_CAT_EXEC` | Binary system (exec, EIF, PE, modules) |
