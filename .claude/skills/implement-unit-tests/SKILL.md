---
name: implement-unit-tests
description: Implement the Unit Tests section of a TODO file — create the test file, register it in the test runner, build, and mark the section complete with notes. Use when a TODO has a Unit Tests section ready to implement.
---

# Implement Unit Tests

## When to Use

- A TODO file has a `## Unit Tests` section with `[ ]` checkboxes
- The code under test already exists and builds
- The user asks to "implement the unit tests" or "add tests for" a TODO

## Workflow

### 1. Read the Unit Tests section

Read the exact `## Unit Tests` section in the target TODO file. It specifies:
- The test file to create (e.g., `src/kernel/test/test_boot_init.c`)
- The registration function name (e.g., `test_register_boot_init()`)
- The specific assertions to implement
- The XREF to `00-infrastructure/TODO-03 §1`

### 2. Explore the code under test

Before writing any test code:
- Read the header file for the API being tested — get exact function signatures, enum values, macro definitions
- Read the implementation to understand edge cases (NULL handling, out-of-range, etc.)
- Read an existing test file (e.g., `src/kernel/test/test_pmm.c`) to match the exact pattern

### 3. Create the test file

Create `src/kernel/test/test_<name>.c` following the established pattern:

```c
#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/<header_under_test>.h"

static void test_<name>(void)
{
    TEST_ASSERT(condition, "description of what is being verified");
}

void test_register_<name>(void)
{
    test_suite_register("Suite: test name", test_<name>);
}

#endif /* KERNEL_TESTS */
```

**Rules:**
- Wrap everything in `#ifdef KERNEL_TESTS` / `#endif`
- Include `"kernel/test/test.h"` first
- Each test function is `static void`, takes no args
- Use `TEST_ASSERT(cond, "message")` for assertions
- Save and restore global state if tests modify shared kernel state (e.g., subsystem readiness)
- If a macro uses `return` (like `BOOT_REQUIRE`), test it via a wrapper function with the matching return type
- The registration function is **not** static — it's `extern`'d from `test_runner.c`

### 4. Register in the test runner

Edit `src/kernel/test/test_runner.c`:
1. Add `extern void test_register_<name>(void);` with the other extern declarations
2. Add `test_register_<name>();` call inside `test_runner_init()`

### 5. Fix any bugs found during test development

If writing a test reveals a bug (e.g., NULL pointer crash, missing bounds check), fix the bug in the code under test. This is expected — tests often expose issues.

### 6. Build and verify

```bash
bash scripts/build.sh
tail -1 build/build.log  # must show === BUILD OK ===
```

The test file is auto-discovered by the Makefile (`find ... -name '*.c'`), so no Makefile changes needed.

### 7. Mark the TODO section complete

Update the TODO file's `## Unit Tests` section:
- Mark all `[ ]` checkboxes as `[x]`
- Add a note after the checklist with the test count:
  ```
  > **Done:** N suites, M assertions — registered in `test_runner_init()` (YYYY-MM-DD)
  ```

If the TODO has an **Implementation Order table** with a row for unit tests, update its Status to `[x]`.

### 8. Commit

Use the commit message specified in the Unit Tests section (typically `"test: add <subsystem> test suite"`). Stage:
- The new test file (`src/kernel/test/test_<name>.c`)
- The modified `test_runner.c`
- The updated TODO file
- Any bug fixes found during test development

## Guardrails

- Only implement tests listed in the `## Unit Tests` section — do not invent additional tests
- Do not modify the code under test unless a bug is found during testing
- Do not change other TODO sections
- Do not skip assertions listed in the TODO — implement all of them
- If an assertion cannot be tested (e.g., requires hardware), note it in the TODO and mark it `[x]` with `(skipped: reason)`

## Test Pattern Reference

**Existing test files to reference:**
- `src/kernel/test/test_pmm.c` — memory allocation tests
- `src/kernel/test/test_heap.c` — heap allocation tests (includes edge cases)
- `src/kernel/test/test_vfs.c` — filesystem tests (create/read/write/delete)
- `src/kernel/test/test_sched.c` — scheduler tests
- `src/kernel/test/test_registry.c` — Win32 registry tests
- `src/kernel/test/test_boot_init.c` — boot init tests (save/restore state, wrapper functions for macros)

**Test framework API** (`include/kernel/test/test.h`):
- `test_suite_register(name, fn)` — register a test suite
- `TEST_ASSERT(cond, msg)` — assert with file:line on failure
- `test_runner_init()` — called once, registers all suites
- `test_runner_run()` — runs all registered suites, prints summary

**Both `test=1` and `debug=1` run all registered unit tests.**
