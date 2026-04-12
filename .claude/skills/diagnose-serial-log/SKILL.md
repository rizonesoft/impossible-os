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

2. **Extract platform context** -- parse the QEMU preamble (lines before `[BOOT]`) for `Accel:` (whpx/tcg), `CPUs:` (SMP vs single), `Timer:` (HW accel vs emulated), `Device:` (VGA/VirtIO-GPU). Record these -- WHPX-only issues suggest hypervisor quirks (see CLAUDE.md "Bare Metal Gotchas"), TCG-only suggests emulation limits, 1-CPU runs mask SMP races. If no preamble, note "bare metal or redirected serial."

3. **Check for truncation** -- look for the expected end markers:
   - `"=== TESTS COMPLETE ==="` or test summary line (`"N passed, N failed, N skipped"`)
   - `"=== BOOT OK ==="` or compositor start
   - If neither is present, the log is truncated. Note: "Log truncated -- analysis may be incomplete. Last line: `<last line>`". Check the last POST16 code to identify where the system stopped.

4. **Parse test results (structured)** -- if the log contains test output:
   - Parse category headers: `--- [mm] Memory Management ---` to build a per-category map.
   - Count PASS (`[ OK ] TEST:`), FAIL (`[FAIL] TEST:`), SKIP (`SKIP:`) per category.
   - Parse the summary: `N passed, M FAILED, K skipped (of TOTAL) (X.Ys)`.
   - If `failed > 0`, show per-category breakdown: `abi: 1 FAILED of 127 | mm: 0 of 52 | ...`

5. **Extract all diagnostic lines** -- grep for:
   - `[FAIL]` -- error-level klog output
   - `[WARN]` -- warning-level klog output
   - `[CRIT]` -- fatal/panic output
   - `PANIC` / `PAGE_FAULT` / `GUARD:` / `#GP` / `#PF` / `#UD` / `#DE` / `#NM` / `#SS` -- CPU exceptions
   - `ASSERT` / `TEST.*FAIL` -- test assertion failures
   - `timeout` / `hang` / `deadlock` -- behavioral issues
   - `heap full` / `no free frames` / `registry full` / `quota exhausted` -- resource exhaustion (outside test context)
   - `Heap:.*used.*total` -- extract heap utilization; flag if >80% as RESOURCE_PRESSURE
   - `Degraded subsystems:` -- extract degraded subsystem list
   - Garbled Unicode: `┬º` / `ΓÇö` / `┬` / `Γ`; encoding corruption (see CLAUDE.md "No Unicode Dashes"). Flag as ENCODING.

6. **Validate POST16 sequence** -- extract all `POST16` codes from the log. Verify:
   - Phase 0 codes (0x0000-0x0FFF) appear before Phase 1 (0x1000-0x1FFF)
   - Phase 1 before Phase 2 (0x2000-0x2FFF), Phase 2 before Phase 3 (0x3000-0x3FFF)
   - No missing entry/exit pairs (e.g., 0xD809 entry without 0xD80A exit = function crashed mid-execution)
   - Flag any out-of-order or missing POST codes

7. **Analyze boot timing** -- parse three timing sections near the end of the log:
   - **Boot step timing table** (`--- Boot step timing (N steps, base=step0) ---`): ordered init sequence with cumulative timestamps. Verify phase ordering (P0 < P1 < P2 < P3).
   - **Boot step durations** (`--- Boot step durations (sorted by time) ---`): top entry is the bottleneck. Flag any init >2s (except EXEC in test mode which includes test execution time).
   - **Boot perf comparison** (`--- Boot perf comparison (prev vs current) ---`): extract `[PERF] WARNING:` regression lines. Mark as PERF_REGRESSION. A regression >2s is significant; <500ms is noise from timing jitter.
   - **Boot-order issues:** Did subsystem X log before its dependency Y?

8. **Analyze SMP interleaving** -- check `[cpu:N]` tags in log lines:
   - **Double-init detection:** Two CPUs logging the same subsystem init (e.g., both cpu:0 and cpu:1 calling `exec_init()`)
   - **Order anomalies:** AP (cpu:1+) logging before BSP (cpu:0) completes Phase 2 init
   - **Interleaved output:** Log lines from different CPUs appearing mid-line (indicates klog is not atomic for multi-line output)

9. **Classify each diagnostic line:**
   - **Expected test noise** -- `[FAIL]` or `[WARN]` immediately followed by a `[ OK ] TEST:` line for the same subsystem. To auto-discover noise: grep `test_*.c` files for the exact log string -- if a test calls the function that produces it, it's noise. Mark as NOISE and skip.
   - **Expected platform limitation** -- `[WARN]` from a known platform gap (see Noise Patterns below). Mark as EXPECTED.
   - **Real error** -- `[FAIL]` with no corresponding test PASS. Mark as BUG.
   - **Real warning** -- `[WARN]` that indicates degraded behavior, not a test path or known limitation. Mark as INVESTIGATE.
   - **Crash** -- `PANIC`, `PAGE_FAULT`, `GUARD:`, CPU exception. Mark as CRITICAL.
   - **Regression** -- something that used to work (check git log) but now fails. Mark as REGRESSION.
   - **Race condition clue** -- intermittent failures, different behavior across runs, timing-dependent output order, SMP interleaving anomalies. Mark as RACE.
   - **Resource exhaustion** -- heap/PMM/handle/module registry full outside test context. Mark as LEAK.
   - **Performance regression** -- `[PERF] WARNING:` lines from boot timing comparison. Mark as PERF_REGRESSION.
   - **Encoding corruption** -- garbled Unicode bytes in log output. Mark as ENCODING.

10. **Present classification to user** -- show a table:
   ```
   | # | Timestamp | Line | Category | Subsystem | Summary |
   ```
   Ask: "Fix all? Or specific items?"

### Phase 2 -- Diagnose Each Issue

For each BUG / INVESTIGATE / CRITICAL / REGRESSION / RACE / LEAK / PERF_REGRESSION item:

11. **Trace to source** -- grep the codebase for the exact log string to find the `klog()` call site. Read the surrounding code to understand the context.

12. **Determine root cause:**
    - **For errors:** What condition triggered the error? Is the check correct? Is the input wrong, or is the handler wrong?
    - **For crashes:** Use the RIP/fault address from the log. Run `llvm-addr2line-19 -e build/kernel.exe -f <RIP>` to get the source location. Read the crashing function.
    - **For race conditions:** Identify the shared mutable state. Check for missing locks, missing atomics, or missing memory barriers. Check if the issue is timing-dependent (appears on one platform but not another).
    - **For regressions:** Use `git log` to find when the behavior changed. Read the commit that broke it.
    - **For resource leaks:** Trace the allocation path -- is the resource freed on the error path? Is there an unmatched alloc without a corresponding free?
    - **For deferred init failures:** Parse `[DEFERRED] name +Nms` lines. Verify all deferred inits completed. Check they ran inline on BSP (correct) vs threaded (bug per CLAUDE.md "No thread_create for deferred init").

13. **Platform comparison** -- if multiple logs exist (e.g., `all-tests-serial.log` WHPX, `all-tests-tcg-serial.log` TCG, `all-tests-1cpu-serial.log` single-CPU), diff `[FAIL]`/`[WARN]` lines across them:
    - Present in ALL = real kernel bug
    - WHPX only = hypervisor quirk (check CLAUDE.md "Bare Metal Gotchas" -- NVMe timing, Init Level De-Assert)
    - TCG only = emulation limitation
    - 1-CPU only = SMP masking a race condition (the race disappears with 1 CPU)
    - Bare metal only = real hardware sensitivity hidden by VM emulation

14. **Check for cascading failures** -- sort BUG/CRITICAL items by timestamp. For each item after the first, check if its subsystem depends on a subsystem that failed earlier. Common pattern: `[FAIL]` non-TEST line at T+0.01-0.02s after a TEST assertion, with a subsequent PASS for the same test suite = noise from the test exercising the error path (e.g., `uthread_create: PID 0 has no PEB` after `uthread rejects kernel task` test). Fix the root cause, not the cascade.

### Phase 3 -- Fix

15. **Fix each issue** -- invoke the `kernel-code-quality` skill gates for the fix:
    - Gate 2 (SMP) if touching shared state
    - Gate 4 (Boot-path) if touching init code
    - Gate 5 (Error handling) for every fix
    - Gate 6 (Bare metal) if the issue was platform-specific

16. **Build** -- `bash scripts/build.sh`, confirm `=== BUILD OK ===`.

17. **Verify fix against log** -- if the log came from a repeatable test run, re-run and check the specific lines are gone. If not repeatable, verify by code inspection that the fix addresses the exact condition.

### Phase 4 -- Report

18. **Health dashboard** -- present a 3-line summary first:
    ```
    Platform: WHPX 2-CPU | Boot: 13.5s | Tests: 1141/1142 (99.9%) | Heap: 24% | Degraded: TPM
    Phases: P0 OK | P1 OK | P2 OK | P3 OK | Deferred: 3/3 OK
    Perf regressions: 2 (DESKTOP_READY +1963ms, DEFERRED +1966ms)
    ```
    Adapt fields to what's available in the log. If no test block, omit Tests. If no perf comparison, omit that line.

19. **Detailed findings table:**
    ```
    | # | Category | Subsystem | Timestamp | Root Cause | Fix | Status |
    |---|----------|-----------|-----------|------------|-----|--------|
    | 1 | BUG      | pe        | 6.090     | Unbounded name | Added strnlen | Fixed |
    | 2 | NOISE    | exec      | 6.190     | Test exercises error path | N/A | Skipped |
    | 3 | RACE     | sched     | 3.200     | Missing atomic on flag | Added acquire/release | Fixed |
    | 4 | LEAK     | mm        | 11.500    | PMM frames not freed on exec fail | Added rollback | Fixed |
    ```
    Omit NOISE and EXPECTED rows unless the user asked for them. Focus on actionable items.

20. **Commit fixes** -- group related fixes into one commit per subsystem.

## Log Noise Patterns (Known Expected Output)

These patterns appear during unit tests and are NOT bugs. To auto-discover: grep the exact `[FAIL]`/`[WARN]` log string in `src/kernel/test/test_*.c` -- if a test calls the function that produces the message, it's expected noise.

Known test noise (grep `test_*.c` for the string to verify):
- `exec: Unknown binary format` -- test_exec_bad_magic
- `exec: exec_register_module: NULL module` -- test_module_register_null
- `exec: exec_register_module: invalid base/size` -- test_module_register_invalid
- `pe: 32-bit Optional Header` -- test_pe_validate_32bit
- `pe: 32-bit PE (i386) rejected` -- test_pe_validate_32bit
- `ob: PID 0 handle quota exhausted` -- handle quota test
- `TEST: (level pass test -- expected WARN)` -- klog level test
- `sched: uthread_create: PID 0 has no PEB` -- test_uthread_rejects_kernel_task exercises rejection path

Known platform limitations (not bugs -- mark as EXPECTED):
- `TPM: Measured boot degraded` -- no TPM hardware on QEMU
- `UEFI: MAT: W^X VIOLATION` -- QEMU OVMF firmware quirk (EFI runtime regions)
- `BOOT: boot-profile.log: cannot open for write` -- test mode, no writable C:\ mount yet
- `PERF: [PERF] WARNING: ... init regressed` -- informational perf delta between boots
- `boot: Degraded subsystems: - TPM` -- expected on QEMU (no TPM)
- `vbox: Not VirtualBox ... -- skipping PCI scan` -- correct behavior on non-VBox platforms

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
| ELF load failure | Parse `elf:` lines for segment vaddr/flags. Check NX stack, AT_PHDR derivation, auxv. Verify user page range (0x800000-0x900000). |
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
