---
name: implement-unit-tests
description: Implement the Unit Tests section of a TODO file -- create the test file, register it in the test runner, build, and mark the section complete with notes. Use when a TODO has a Unit Tests section ready to implement.
---

# Implement Unit Tests

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
    TEST_CAT_COUNT,
    TEST_CAT_ALL = 0xFF, // Runs under any filter
} test_category_t;
```

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

/* RIGHT -- counts in pending bucket, [STUB] line names the gap */
TEST_PENDING(st == STATUS_NOT_IMPLEMENTED,
             "NtAlpcCreatePort: ALPC engine deferred (TODO-12 s8)");
```

Why this matters:
- **Single source of truth.** The `pending` count tells you at a glance how many features are incomplete; `[STUB]` lines list each one. No need for parallel runtime klog warnings in the stub body itself -- doing both creates duplicate signal in the boot log (one [WARN] from the stub, one [ OK ] from the test, both saying the same thing).
- **Stable references.** Embed a TODO ref in the message string ONLY if you're OK with it going stale; the audit script (`scripts/audit-stubs.sh`) walks `TEST_PENDING` call sites and verifies the references resolve. The TODO ref in the source comment next to the test (not the message) is the durable one.
- **Visible incompleteness.** A regression where someone "implements" an SSDT slot that returns the wrong status is caught -- `TEST_PENDING` flips to FAIL when the deferred contract breaks, instead of silently passing.

If the stub body itself logs a runtime warning (e.g. an old `KSTUB_WARN_ONCE` pattern), DELETE the runtime log when you add the `TEST_PENDING` test. Two signals for the same fact = noise.

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

## Workflow

### 1. Read the Unit Tests section

Read the exact `## Unit Tests` section in the target TODO file. It specifies:
- The test file to create (e.g., `src/kernel/test/test_boot_init.c`)
- The registration function name (e.g., `test_register_boot_init()`)
- The specific assertions to implement

### 2. Explore the code under test

Before writing any test code:
- Read the header file for the API being tested -- get exact function signatures, enum values, macro definitions
- Read the implementation to understand edge cases (NULL handling, out-of-range, etc.)
- Read an existing test file to match the exact pattern

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
node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<test coverage prompt>"
```

Focus: untested error paths, missing boundary tests, missing negative tests. Add any valid missing tests found. Skip for trivial test suites (< 3 assertions).

### 8. Fix any bugs found during test development

If writing a test reveals a bug, fix the bug in the code under test using `kernel-code-quality` gates. For non-trivial bugs (SMP races, memory corruption, boot-order issues), apply `superpowers:systematic-debugging` -- diagnose root cause properly, don't quick-patch.

### 9. Mark the TODO section complete

Update the TODO file's `## Unit Tests` section:
- Mark all `[ ]` checkboxes as `[x]`
- Add a note:
  ```
  > **Done:** N suites, M assertions -- registered in `test_runner_init()` (YYYY-MM-DD)
  ```

If the TODO has an **Implementation Order table** with a row for unit tests, update its Status to `[x]`.

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

## Guardrails

- Only implement tests listed in the `## Unit Tests` section -- do not invent additional tests (Codex coverage analysis in step 7 may suggest additions, but apply `receiving-code-review` discipline)
- Do not modify the code under test unless a bug is found during testing
- Do not change other TODO sections
- Do not skip assertions listed in the TODO -- implement all of them
- If an assertion cannot be tested (e.g., requires hardware), note it in the TODO and mark it `[x]` with `(skipped: reason)`
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
