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

## Debugging Discipline

> Apply `superpowers:systematic-debugging` principles:
> - **Observe before theorizing.** Read the FULL log, not just the first error. Later errors often explain earlier ones.
> - **One variable at a time.** Fix one issue, rebuild, re-run. Don't batch-fix 5 things and hope.
> - **Verify the fix eliminates the symptom.** Don't just verify the code looks right -- verify the log line is gone.
> - **Check your assumptions.** "This can't be the problem" is usually wrong. Use `llvm-addr2line-19`, read the code, check the data.

## Workflow

### Phase 1 -- Parse and Classify

1. **Read the log** -- if a file path is given, read it. If pasted, work from the paste.

2. **Check for truncation** -- look for the expected end markers:
   - `"=== TESTS COMPLETE ==="` or test summary line (`"N passed, N failed, N skipped"`)
   - `"=== BOOT OK ==="` or compositor start
   - If neither is present, the log is truncated. Note: "Log truncated -- analysis may be incomplete. Last line: `<last line>`". Check the last POST16 code to identify where the system stopped.

3. **Parse test result summary** -- if the log contains a test summary line (e.g., `"328 passed, 0 failed, 2 skipped"`), extract it immediately. If `failed > 0`, flag it upfront before detailed analysis.

4. **Extract all diagnostic lines** -- grep for:
   - `[FAIL]` -- error-level klog output
   - `[WARN]` -- warning-level klog output
   - `[CRIT]` -- fatal/panic output
   - `PANIC` / `PAGE_FAULT` / `GUARD:` / `#GP` / `#PF` / `#UD` / `#DE` / `#NM` / `#SS` -- CPU exceptions
   - `ASSERT` / `TEST.*FAIL` -- test assertion failures
   - `timeout` / `hang` / `deadlock` -- behavioral issues
   - `heap full` / `no free frames` / `registry full` / `quota exhausted` -- resource exhaustion (outside test context)

5. **Validate POST16 sequence** -- extract all `POST16` codes from the log. Verify:
   - Phase 0 codes (0x0000-0x0FFF) appear before Phase 1 (0x1000-0x1FFF)
   - Phase 1 before Phase 2 (0x2000-0x2FFF), Phase 2 before Phase 3 (0x3000-0x3FFF)
   - No missing entry/exit pairs (e.g., 0xD809 entry without 0xD80A exit = function crashed mid-execution)
   - Flag any out-of-order or missing POST codes

6. **Analyze timestamps** -- serial log lines have timestamps (`[ 5.780]`). Check for:
   - **Boot-order issues:** Did subsystem X log before its dependency Y? (e.g., exec logging before PMM is ready)
   - **Timing anomalies:** Unreasonable gaps between consecutive init steps (> 5s between boot phases suggests a hang or busy-wait)
   - **Performance regressions:** Compare timing against known baselines if available. Flag any init step taking > 1s unless it's expected (e.g., NVMe enumeration)

7. **Analyze SMP interleaving** -- check `[cpu:N]` tags in log lines:
   - **Double-init detection:** Two CPUs logging the same subsystem init (e.g., both cpu:0 and cpu:1 calling `exec_init()`)
   - **Order anomalies:** AP (cpu:1+) logging before BSP (cpu:0) completes Phase 2 init
   - **Interleaved output:** Log lines from different CPUs appearing mid-line (indicates klog is not atomic for multi-line output)

8. **Classify each diagnostic line:**
   - **Expected test noise** -- `[FAIL]` or `[WARN]` immediately followed by a `[ OK ] TEST:` line for the same subsystem. To auto-discover noise: grep `test_*.c` files for the exact log string -- if a test calls the function that produces it, it's noise. Mark as NOISE and skip.
   - **Real error** -- `[FAIL]` with no corresponding test PASS. Mark as BUG.
   - **Real warning** -- `[WARN]` that indicates degraded behavior, not a test path. Mark as INVESTIGATE.
   - **Crash** -- `PANIC`, `PAGE_FAULT`, `GUARD:`, CPU exception. Mark as CRITICAL.
   - **Regression** -- something that used to work (check git log) but now fails. Mark as REGRESSION.
   - **Race condition clue** -- intermittent failures, different behavior across runs, timing-dependent output order, SMP interleaving anomalies. Mark as RACE.
   - **Resource exhaustion** -- heap/PMM/handle/module registry full outside test context. Mark as LEAK.

9. **Present classification to user** -- show a table:
   ```
   | # | Timestamp | Line | Category | Subsystem | Summary |
   ```
   Ask: "Fix all? Or specific items?"

### Phase 2 -- Diagnose Each Issue

For each BUG / INVESTIGATE / CRITICAL / REGRESSION / RACE / LEAK item:

10. **Trace to source** -- grep the codebase for the exact log string to find the `klog()` call site. Read the surrounding code to understand the context.

11. **Determine root cause:**
    - **For errors:** What condition triggered the error? Is the check correct? Is the input wrong, or is the handler wrong?
    - **For crashes:** Use the RIP/fault address from the log. Run `llvm-addr2line-19 -e build/kernel.exe -f <RIP>` to get the source location. Read the crashing function.
    - **For race conditions:** Identify the shared mutable state. Check for missing locks, missing atomics, or missing memory barriers. Check if the issue is timing-dependent (appears on one platform but not another).
    - **For regressions:** Use `git log` to find when the behavior changed. Read the commit that broke it.
    - **For resource leaks:** Trace the allocation path -- is the resource freed on the error path? Is there an unmatched alloc without a corresponding free?

12. **Platform comparison** -- if logs from multiple platforms are available (WHPX, TCG, VBox, bare metal), compare them:
    - Issue appears on ALL platforms = real kernel bug
    - Issue appears on WHPX only = likely hypervisor quirk (check CLAUDE.md "Bare Metal Gotchas")
    - Issue appears on TCG only = likely TCG emulation limitation (e.g., NVMe timing)
    - Issue appears on bare metal only = likely real hardware sensitivity hidden by VM emulation

13. **Check for cascading failures** -- does this error cascade from an earlier failure? Is there a dependency chain? Fix the root cause, not the symptom.

### Phase 3 -- Fix

14. **Fix each issue** -- invoke the `kernel-code-quality` skill gates for the fix:
    - Gate 2 (SMP) if touching shared state
    - Gate 4 (Boot-path) if touching init code
    - Gate 5 (Error handling) for every fix
    - Gate 6 (Bare metal) if the issue was platform-specific

15. **Build** -- `bash scripts/build.sh`, confirm `=== BUILD OK ===`.

16. **Verify fix against log** -- if the log came from a repeatable test run, re-run and check the specific lines are gone. If not repeatable, verify by code inspection that the fix addresses the exact condition.

### Phase 4 -- Report

17. **Report findings:**
    ```
    | # | Category | Subsystem | Timestamp | Root Cause | Fix | Status |
    |---|----------|-----------|-----------|------------|-----|--------|
    | 1 | BUG      | pe        | 6.090     | Unbounded name | Added strnlen | Fixed |
    | 2 | NOISE    | exec      | 6.190     | Test exercises error path | N/A | Skipped |
    | 3 | RACE     | sched     | 3.200     | Missing atomic on flag | Added acquire/release | Fixed |
    | 4 | LEAK     | mm        | 11.500    | PMM frames not freed on exec fail | Added rollback | Fixed |
    ```

18. **Commit fixes** -- group related fixes into one commit per subsystem.

## Log Noise Patterns (Known Expected Output)

These patterns appear during unit tests and are NOT bugs. To auto-discover: grep the exact `[FAIL]`/`[WARN]` log string in `src/kernel/test/test_*.c` -- if a test calls the function that produces the message, it's expected noise.

Known patterns:
- `exec: Unknown binary format` -- test_exec_bad_magic
- `exec: exec_register_module: NULL module` -- test_module_register_null
- `exec: exec_register_module: invalid base/size` -- test_module_register_invalid
- `pe: 32-bit Optional Header` -- test_pe_validate_32bit
- `pe: 32-bit PE (i386) rejected` -- test_pe_validate_32bit
- `ob: PID 0 handle quota exhausted` -- handle quota test
- `TEST: (level pass test -- expected WARN)` -- klog level test

> **Self-updating:** Instead of maintaining this list manually, the classification step (8) greps `test_*.c` for the exact string. If found in a test, it's noise regardless of whether it's in this list. This list is a fast-path cache, not the authority.
>
> **Preferred fix for persistent noise:** Demote the klog call from `LOG_WARN`/`LOG_ERROR` to `LOG_DEBUG` so it only appears in verbose mode. Do NOT remove the log call entirely.

## Crash Diagnosis Quick Reference

| Crash Type | First Step |
|-----------|------------|
| `PAGE_FAULT` at user address | Check if page has User bit set at all 4 levels. Check if CR3 is correct process PML4. |
| `PAGE_FAULT` at kernel address | Check if page is mapped. Check if it's a guard page (`GUARD:` label). |
| `#GP` in ring 3 | Check GDT segment selectors. Check SYSRET CS/SS computation. |
| `#GP` in ring 0 | Check IDT gate, TSS RSP0, or MSR write with bad value. |
| `#UD` | Check for CPUID-gated instructions (CLAC/STAC/XSAVE) without feature check. |
| `#DE` | Division by zero -- check for zero divisor in timer calibration or similar. |
| `#NM` (Device Not Available) | FPU/SIMD used before XSAVE init, or CR0.TS set without proper `#NM` handler to do lazy FPU restore. |
| `#SS` (Stack Segment Fault) | Stack overflow past guard page, or corrupt RSP pointing outside valid stack range. Check `GUARD:` labels. |
| Triple fault (no output) | Check POST16 codes on serial. Last POST code before silence = failing function. |
| Hang (output stops) | Check for spinlock deadlock (IRQ context using non-irqsave lock). Check for infinite loop. |

## Guardrails

- Do NOT dismiss `[FAIL]` lines without checking if they're from tests. Grep for the exact string in test files first.
- Do NOT fix "noise" by removing the klog call -- demote to `LOG_DEBUG` instead so it's still visible in verbose mode.
- For crashes, ALWAYS use `llvm-addr2line-19` before guessing at the cause.
- For race conditions, ALWAYS identify the specific shared state and the specific missing synchronization before fixing.
- Do not batch unrelated fixes. One commit per subsystem or per root cause.
- If the log is from a specific platform (WHPX/TCG/VBox/bare metal), note it in the report. Platform-specific issues need platform-specific investigation.
- If the log is truncated, say so explicitly. Do not claim "no issues found" on a partial log.
