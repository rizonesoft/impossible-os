---
name: implement-unit-tests
description: Implement the Unit Tests section of a TODO file -- create the test file, register it in the test runner, build, and mark the section complete with notes. Use when a TODO has a Unit Tests section ready to implement.
---

# Implement Unit Tests

## When to Use

- A TODO file has a `## Unit Tests` section with `[ ]` checkboxes
- The code under test already exists and builds
- The user asks to "implement the unit tests" or "add tests for" a TODO

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
```

- On pass: increments `passed` counter, logs `[ OK ] TEST: suite :: msg` (unless quiet mode)
- On fail: increments `failed` counter, logs `[FAIL] TEST: suite :: msg (file:line)` with got/expected for EQ/NEQ
- On skip: increments `skipped` counter, logs `[WARN] TEST: suite :: SKIP: msg`
- There are no `TEST_PASS` or `TEST_FAIL` macros -- success is implicit when assertions pass

### Registration API

```c
// Preferred -- register with a specific category
void test_suite_register_cat(const char *name, test_fn_t fn, test_category_t cat);

// Legacy -- registers with TEST_CAT_ALL (runs under any filter)
void test_suite_register(const char *name, test_fn_t fn);
```

Always use `test_suite_register_cat()` with the appropriate category.

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
```

### 7. Fix any bugs found during test development

If writing a test reveals a bug, fix the bug in the code under test. This is expected.

### 8. Mark the TODO section complete

Update the TODO file's `## Unit Tests` section:
- Mark all `[ ]` checkboxes as `[x]`
- Add a note:
  ```
  > **Done:** N suites, M assertions -- registered in `test_runner_init()` (YYYY-MM-DD)
  ```

If the TODO has an **Implementation Order table** with a row for unit tests, update its Status to `[x]`.

### 9. Commit

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

- Only implement tests listed in the `## Unit Tests` section -- do not invent additional tests
- Do not modify the code under test unless a bug is found during testing
- Do not change other TODO sections
- Do not skip assertions listed in the TODO -- implement all of them
- If an assertion cannot be tested (e.g., requires hardware), note it in the TODO and mark it `[x]` with `(skipped: reason)`

## Existing Test Files (Reference)

| File | Category | What it tests |
|------|----------|---------------|
| `test_pmm.c` | `TEST_CAT_MM` | Physical memory allocation |
| `test_heap.c` | `TEST_CAT_MM` | Heap allocation, edge cases |
| `test_vmm.c` | `TEST_CAT_MM` | Virtual memory mapping |
| `test_swap.c` | `TEST_CAT_MM` | Swap space management |
| `test_mmap.c` | `TEST_CAT_MM` | Memory-mapped regions |
| `test_vfs.c` | `TEST_CAT_FS` | VFS create/read/write/delete |
| `test_sched.c` | `TEST_CAT_SCHED` | Scheduler task management |
| `test_registry.c` | `TEST_CAT_ABI` | Win32 registry operations |
| `test_boot_init.c` | `TEST_CAT_BOOT` | Boot init macros, state |
| `test_klog.c` | `TEST_CAT_BOOT` | Kernel logging subsystem |
| `test_ob.c` | `TEST_CAT_OB` | Object Manager (handles, refs, namespace) |
| `test_security.c` | `TEST_CAT_SECURITY` | Security subsystem |
| `test_peb_teb.c` | `TEST_CAT_ABI` | PEB/TEB ABI compatibility |
| `test_ipc.c` | `TEST_CAT_IPC` | IPC mechanisms |
| `test_storage.c` | `TEST_CAT_STORAGE` | Storage driver operations |
