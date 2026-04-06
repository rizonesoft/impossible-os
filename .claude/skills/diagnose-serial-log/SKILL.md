---
name: diagnose-serial-log
description: Analyze a serial log for real bugs -- parse WARN/FAIL/crash lines, filter expected test output, trace each real issue to source code, diagnose root cause, fix, and verify. Use when the user pastes a serial log or points to a log file and wants issues diagnosed and fixed.
---

# Diagnose Serial Log

## Use This Skill When

- The user pastes serial output or points to a log file (e.g., `build/test-probe.log`, `debug-tmp/*.log`).
- Serial output shows unexpected `[WARN]`, `[FAIL]`, `[CRIT]`, crash dumps, or unexpected behavior.
- After a test run shows failures that need root-cause analysis.
- The user asks "what's wrong with this log?" or "fix these errors."

## Workflow

### Phase 1 -- Parse and Classify

1. **Read the log** -- if a file path is given, read it. If pasted, work from the paste.
2. **Extract all diagnostic lines** -- grep for:
   - `[FAIL]` -- error-level klog output
   - `[WARN]` -- warning-level klog output
   - `[CRIT]` -- fatal/panic output
   - `PANIC` / `PAGE_FAULT` / `GUARD:` / `#GP` / `#PF` / `#UD` / `#DE` -- CPU exceptions
   - `ASSERT` / `TEST.*FAIL` -- test assertion failures
   - `timeout` / `hang` / `deadlock` -- behavioral issues
3. **Classify each line:**
   - **Expected test noise** -- `[FAIL]` or `[WARN]` immediately followed by a `[ OK ] TEST:` line for the same subsystem. These are unit tests exercising error paths. Mark as NOISE and skip.
   - **Real error** -- `[FAIL]` with no corresponding test PASS. Mark as BUG.
   - **Real warning** -- `[WARN]` that indicates degraded behavior, not a test path. Mark as INVESTIGATE.
   - **Crash** -- `PANIC`, `PAGE_FAULT`, `GUARD:`, CPU exception. Mark as CRITICAL.
   - **Regression** -- something that used to work (check git log) but now fails. Mark as REGRESSION.
   - **Race condition clue** -- intermittent failures, different behavior across runs, timing-dependent output order. Mark as RACE.

4. **Present classification to user** -- show a table:
   ```
   | # | Line | Category | Subsystem | Summary |
   ```
   Ask: "Fix all? Or specific items?"

### Phase 2 -- Diagnose Each Issue

For each BUG / INVESTIGATE / CRITICAL / REGRESSION / RACE item:

5. **Trace to source** -- grep the codebase for the exact log string to find the `klog()` call site. Read the surrounding code to understand the context.
6. **Determine root cause:**
   - **For errors:** What condition triggered the error? Is the check correct? Is the input wrong, or is the handler wrong?
   - **For crashes:** Use the RIP/fault address from the log. Run `llvm-addr2line-19 -e build/kernel.exe -f <RIP>` to get the source location. Read the crashing function.
   - **For race conditions:** Identify the shared mutable state. Check for missing locks, missing atomics, or missing memory barriers. Check if the issue is timing-dependent (appears on one platform but not another).
   - **For regressions:** Use `git log` to find when the behavior changed. Read the commit that broke it.
7. **Check for related issues** -- does this error cascade from an earlier failure? Is there a dependency chain? Fix the root cause, not the symptom.

### Phase 3 -- Fix

8. **Fix each issue** -- apply the fix following kernel-code-quality gates (SMP safety, error handling, etc.).
9. **Build** -- `bash scripts/build.sh`, confirm `=== BUILD OK ===`.
10. **Verify fix against log** -- if the log came from a repeatable test run, re-run and check the specific lines are gone. If not repeatable, verify by code inspection that the fix addresses the exact condition.

### Phase 4 -- Report

11. **Report findings:**
    ```
    | # | Category | Subsystem | Root Cause | Fix | Status |
    |---|----------|-----------|------------|-----|--------|
    | 1 | BUG      | pe        | Unbounded name | Added strnlen | Fixed |
    | 2 | NOISE    | exec      | Test exercises error path | N/A | Skipped |
    | 3 | RACE     | sched     | Missing atomic on flag | Added acquire/release | Fixed |
    ```
12. **Commit fixes** -- group related fixes into one commit per subsystem.

## Log Noise Patterns (Known Expected Output)

These patterns appear during unit tests and are NOT bugs:

- `[WARN] exec: Unknown binary format` -- test_exec_bad_magic exercises this
- `[FAIL] exec: exec_register_module: NULL module` -- test_module_register_null exercises this
- `[FAIL] exec: exec_register_module: invalid base/size` -- test_module_register_invalid exercises this
- `[WARN] pe: 32-bit Optional Header` -- test_pe_validate_32bit exercises this
- `[WARN] pe: 32-bit PE (i386) rejected` -- test_pe_validate_32bit exercises this
- `[WARN] ob: PID 0 handle quota exhausted` -- handle quota test exercises this
- `[WARN] TEST: (level pass test -- expected WARN)` -- klog level test

> **Update this list** when new tests are added that exercise error paths with visible log output. If a `[FAIL]`/`[WARN]` line keeps appearing in logs and it's from a test, demote the klog call to `LOG_DEBUG` (preferred) or add it here.

## Crash Diagnosis Quick Reference

| Crash Type | First Step |
|-----------|------------|
| `PAGE_FAULT` at user address | Check if page has User bit set at all 4 levels. Check if CR3 is correct process PML4. |
| `PAGE_FAULT` at kernel address | Check if page is mapped. Check if it's a guard page (`GUARD:` label). |
| `#GP` in ring 3 | Check GDT segment selectors. Check SYSRET CS/SS computation. |
| `#GP` in ring 0 | Check IDT gate, TSS RSP0, or MSR write with bad value. |
| `#UD` | Check for CPUID-gated instructions (CLAC/STAC/XSAVE) without feature check. |
| `#DE` | Division by zero -- check for zero divisor in timer calibration or similar. |
| Triple fault (no output) | Check POST16 codes on serial. Last POST code before silence = failing function. |
| Hang (output stops) | Check for spinlock deadlock (IRQ context using non-irqsave lock). Check for infinite loop. |

## Guardrails

- Do NOT dismiss `[FAIL]` lines without checking if they're from tests. Grep for the exact string in test files first.
- Do NOT fix "noise" by removing the klog call -- demote to `LOG_DEBUG` instead so it's still visible in verbose mode.
- For crashes, ALWAYS use `llvm-addr2line-19` before guessing at the cause.
- For race conditions, ALWAYS identify the specific shared state and the specific missing synchronization before fixing.
- Do not batch unrelated fixes. One commit per subsystem or per root cause.
