---
name: diagnose-serial-log
description: Analyze serial logs with a structured event model, collapse duplicate noise, identify the first causal failure, trace to source, fix real bugs, and verify the symptom is gone. Use when the user pastes a log or points to serial output files.
---

# Diagnose Serial Log

## Use This Skill When

- The user pastes serial output or points to a log file (for example `build/test-probe.log`, `debug-tmp/*.log`).
- Serial output shows unexpected `[WARN]`, `[FAIL]`, `[CRIT]`, crash dumps, hangs, or degraded boot behavior.
- A test run failed and the user wants the actual kernel bug, not a grep dump of every scary-looking line.
- Multiple platform logs exist and the user wants to know what is real, what is platform-specific, and what is test noise.

## Debugging Discipline

> Apply `superpowers:systematic-debugging` principles:
> - **Observe before theorizing.** Read the full log first. The first visible error is often not the causal one.
> - **Model the log, do not just grep it.** Build a normalized event list, then classify events from that model.
> - **Collapse repeats.** Twenty copies of the same warning are one finding with a repeat count, not twenty findings.
> - **Fix the earliest causal failure.** Later failures are often fallout.
> - **Verify the symptom disappeared.** Do not stop at "the code change looks right."

## Working Model

Build a normalized event stream before classifying anything. Each log line should be mapped, when possible, to:

```text
event {
  line_no
  raw
  timestamp_ms?      // parsed from log prefix when available
  cpu?               // from [cpu:N]
  subsystem?         // from "mm:", "sched:", "exec:" style prefixes
  level?             // BOOT, INFO, WARN, FAIL, CRIT, TEST, PERF, POST16
  kind               // boot, test, crash, perf, post16, deferred, heap, degraded, generic
  phase?             // P0/P1/P2/P3 if derivable
  key                // dedup key: subsystem + canonicalized message + kind
}
```

Reduce events into findings:

```text
finding {
  category           // BUG, CRITICAL, RACE, REGRESSION, LEAK, PERF_REGRESSION, EXPECTED, NOISE, ENCODING
  confidence         // 0.00 - 1.00
  first_line
  first_timestamp_ms?
  subsystem
  summary
  evidence[]         // representative lines, counts, related test name, POST16 pair, source path
  repeats
  root_cause_rank    // 1 = fix first
}
```

Do not present raw lines first. Present findings derived from the model.

## Workflow

### Phase 1 -- Parse, Normalize, Classify

1. **Read all relevant logs first**
   - If a file path is given, read it.
   - If multiple candidate logs exist nearby (`*-serial.log`, `*-tcg*.log`, `*-1cpu*.log`, `*-baremetal*.log`), read all of them. Multi-log comparison is first-class, not optional.
   - If the user pasted only a fragment, say so and mark the analysis partial.

2. **Extract platform context**
   - Parse the preamble before `[BOOT]` for `Accel:` (whpx/tcg), `CPUs:`, `Timer:`, `Device:`, `QEMU`, `VBox`, or bare-metal hints.
   - Record a per-log platform header. If no preamble exists, note `bare metal or redirected serial`.
   - Use this later for platform diffing, not as an excuse to dismiss a bug.

3. **Build the normalized event stream**
   - Parse timestamps, CPU tags, subsystem prefixes, severity tags, POST16 lines, test result lines, deferred-init lines, heap lines, degraded subsystem lines, and perf comparison lines.
   - Canonicalize variable fields in messages for dedup keys:
     - Hex addresses -> `<addr>`
     - Decimal IDs -> `<id>`
     - Durations -> `<ms>`
     - Paths -> `<path>`
   - Keep the raw line in evidence, but classify from the canonicalized key.

4. **Check completeness and truncation**
   - Look for end markers:
     - `=== TESTS COMPLETE ===`
     - summary lines such as `N passed, M failed, K skipped`
     - `=== BOOT OK ===`
     - compositor or shell start markers
   - If neither a test-complete nor boot-complete marker exists, mark the log truncated and record:
     - last line
     - last timestamp
     - last POST16 code
     - last completed phase

5. **Parse structured sections**
   - **Tests:** category headers, PASS/FAIL/SKIP counts, final summary, per-category totals.
   - **POST16:** ordered code list, entry/exit pairs, phase transitions, last-good code.
   - **Boot timing:** timing table, duration table, perf comparison warnings.
   - **Deferred init:** expected start/done pairs and completion counts.
   - **Heap/resource pressure:** utilization, exhaustion messages, degraded subsystem list.

6. **Collapse duplicates before classification**
   - Group events by dedup key.
   - For each group, keep:
     - first occurrence
     - last occurrence
     - repeat count
     - distinct CPUs
     - adjacent test names, if any
   - A repeated warning is one finding with `repeats=N`.

7. **Validate boot ordering and identify the first bad transition**
   - Verify phase ordering: P0 -> P1 -> P2 -> P3.
   - Check POST16 entry/exit pairs.
   - Flag the earliest missing exit or earliest out-of-order phase as `FIRST_BAD_TRANSITION`.
   - Report:
     - `Last good POST16`
     - `First missing POST16 exit`
     - `First subsystem that failed after a good transition`
   - For boot hangs or triple-fault style silence, this block is mandatory.

8. **Analyze SMP and ordering anomalies**
   - Use `[cpu:N]` tags to detect:
     - double-init by multiple CPUs
     - AP logging before BSP completed a required earlier phase
     - mixed mid-line output indicating non-atomic logging
   - If the same canonicalized init line appears on multiple CPUs, treat that as a race clue, not two bugs.

9. **Classify findings with confidence and reasons**
   - **NOISE (0.90-0.99):**
     - A warning/failure that occurs inside a test exercising that error path, with nearby `[ OK ] TEST:` completion or matching test ownership.
   - **EXPECTED (0.80-0.95):**
     - Known platform limitations or intentional degraded mode.
   - **CRITICAL (0.95-1.00):**
     - `PANIC`, `PAGE_FAULT`, `GUARD:`, CPU exceptions, or missing POST16 exit followed by silence.
   - **BUG (0.75-0.95):**
     - Real `[FAIL]` not attributable to tests, expected platform limits, or fallout from an earlier failure.
   - **INVESTIGATE / RACE / LEAK / PERF_REGRESSION / ENCODING:**
     - Use only when the evidence supports that specific category.
   - Every finding must include a one-line reason: `why this category, why this confidence`.

10. **Rank by likely root cause**
   - Sort findings by:
     1. earliest CRITICAL or first bad transition
     2. earliest dependency failure
     3. earliest BUG with high confidence
     4. RACE clues
     5. LEAK / PERF_REGRESSION / ENCODING
     6. repeats and fallout
   - The first item in the ranked list is the issue to diagnose first.

11. **Present a compact findings table**
   - Default to actionable items only. Omit NOISE and EXPECTED unless the user asked for them.
   - Use this shape:
     ```
     | # | Rank | Category | Confidence | Subsystem | First Seen | Repeats | Summary |
     ```
   - Do not stop to ask "Fix all?" unless the user explicitly requested triage only. Default behavior is: fix all actionable items in ranked order.

### Phase 2 -- Diagnose the Ranked Findings

For each actionable finding, in ranked order:

12. **Trace the finding to source**
   - Search first by exact log string.
   - If exact-string search fails, search by subsystem prefix plus stable message fragment.
   - If the line is dynamic, search for nearby constant text or the enclosing subsystem logger.
   - For crashes, use RIP/fault address with:
     ```bash
     llvm-addr2line-19 -e build/kernel.exe -f <RIP>
     ```

13. **Determine the root cause**
   - **Errors:** identify the failing check, the bad input, and the caller that supplied it.
   - **Crashes:** read the crashing function and the code just before it; do not guess from the exception name alone.
   - **Races:** identify the exact shared mutable state and the missing lock, atomic, or barrier.
   - **Leaks:** identify alloc/free imbalance and the failing rollback path.
   - **Perf regressions:** identify which init step moved, what dependency changed, and whether the slowdown is causal or noise.
   - **Deferred init failures:** verify all `[DEFERRED] name +Nms` lines have matching completion and that they ran inline on BSP.

14. **Do explicit platform comparison when multiple logs exist**
   - Build a finding-presence matrix by canonicalized key:
     - present on all logs -> real kernel bug
     - WHPX only -> likely hypervisor interaction
     - TCG only -> likely emulation limit or CPUID gating bug
     - SMP only -> likely race or ordering bug
     - bare metal only -> real hardware sensitivity hidden by VM emulation
   - Present the matrix, not just a sentence.

15. **Prune cascades**
   - For each finding after the first ranked root cause candidate, ask:
     - did it happen after a dependency failure?
     - is it the same canonicalized message repeating?
     - is it from a test intentionally exercising a failure path?
   - Mark fallout explicitly:
     - `CASCADE_OF #1`
     - `REPEAT_OF #2`
     - `TEST_NOISE`
   - Do not fix cascades first.

### Phase 3 -- Fix

16. **Fix one root cause at a time**
   - Invoke `kernel-code-quality` gates that apply:
     - Gate 2 (SMP) for shared state
     - Gate 4 (Boot-path) for init ordering / POST16 issues
     - Gate 5 (Error handling) for every fix
     - Gate 6 (Bare metal) for platform-specific or hardware-touching fixes
   - Prefer the smallest fix that closes the actual root cause, not a broad workaround.

17. **Build**
   - Run:
     ```bash
     bash scripts/build.sh
     ```
   - Confirm:
     ```bash
     tail -1 build/build.log
     ```
     shows `=== BUILD OK ===`.

18. **Verify against the original symptom**
   - Re-run the relevant test or boot path when possible.
   - Confirm the original finding is gone or demoted correctly.
   - If the log is not reproducible in the current environment, verify by code path and state transition, then say that runtime confirmation is pending.

19. **Repeat only if more actionable findings remain**
   - Re-rank after each fix.
   - If a later finding disappears as fallout, record that instead of claiming a second independent fix.

### Phase 4 -- Report

20. **Health dashboard first**
   - Prefer a compact summary:
     ```
     Platform: WHPX 2-CPU | Boot: 13.5s | Tests: 1141/1142 | Heap: 24% | Degraded: TPM
     Phases: P0 OK | P1 OK | P2 OK | P3 FAIL at POST16 0xD809 -> missing 0xD80A
     Top cause: BUG sched (0.93) | Fallout: 3 cascades | Perf regressions: 1
     ```
   - Omit unavailable fields rather than inventing them.

21. **Detailed findings table**
   - Use:
     ```
     | # | Rank | Category | Confidence | Subsystem | Root Cause | Evidence | Fix | Status |
     ```
   - Include `repeats=N` and cross-platform presence where relevant.
   - By default, omit NOISE and EXPECTED rows.

22. **Commit fixes**
   - Group by subsystem or single root cause.
   - Do not batch unrelated fixes into one commit.

## Noise and Expected-Output Rules

Treat this section as rules, not loose prose. The list below is a fast-path cache only. The authority is still the actual test ownership and surrounding log context.

### Matching Order

1. **Test ownership beats severity text**
   - If the log line is emitted while a test is intentionally exercising that path, classify as `NOISE` even if the line says `[FAIL]`.
2. **Known platform limitation beats generic WARN**
   - Example: missing TPM on QEMU.
3. **Earlier causal failure beats later downstream warnings**
   - Downstream fallout is a cascade, not a new bug.
4. **Exact-string matches are optional**
   - Prefer subsystem + stable fragment + nearby test/result context when wording changes.

### Known Test Noise

| Pattern | Owner / rationale |
|---|---|
| `exec: Unknown binary format` | `test_exec_bad_magic` |
| `exec: exec_register_module: NULL module` | `test_module_register_null` |
| `exec: exec_register_module: invalid base/size` | `test_module_register_invalid` |
| `pe: 32-bit Optional Header` | `test_pe_validate_32bit` |
| `pe: 32-bit PE (i386) rejected` | `test_pe_validate_32bit` |
| `ob: PID 0 handle quota exhausted` | handle quota test |
| `TEST: (level pass test -- expected WARN)` | klog level test |
| `sched: uthread_create: PID 0 has no PEB` | `test_uthread_rejects_kernel_task` |

### Known Expected Platform Limitations

| Pattern | Why expected |
|---|---|
| `TPM: Measured boot degraded` | no TPM hardware on QEMU |
| `UEFI: MAT: W^X VIOLATION` | QEMU OVMF firmware quirk |
| `BOOT: boot-profile.log: cannot open for write` | test mode, no writable `C:\` mount yet |
| `PERF: [PERF] WARNING:` | informational perf delta; only actionable if persistent and significant |
| `boot: Degraded subsystems: - TPM` | expected on QEMU without TPM |
| `vbox: Not VirtualBox ... -- skipping PCI scan` | correct behavior on non-VBox platforms |

> **Preferred fix for persistent noise:** demote the log site from `LOG_WARN` or `LOG_ERROR` to `LOG_DEBUG` when the line is expected during tests. Do not delete the log.

## Crash Diagnosis Quick Reference

| Crash Type | First Step |
|-----------|------------|
| `PAGE_FAULT` at user address | Check User bit at all 4 levels and the process CR3. |
| `PAGE_FAULT` at kernel address | Check mapping and guard-page ownership. |
| `#GP` in ring 3 | Check GDT selectors and SYSRET CS/SS computation. |
| `#GP` in ring 0 | Check IDT gate, TSS RSP0, or invalid MSR write. |
| `#UD` | Check CPUID-gated instructions such as CLAC/STAC/XSAVE. |
| `#DE` | Check for zero divisor in timer/math paths. |
| `#NM` | Check FPU/SIMD init and CR0.TS handling. |
| `#SS` | Check stack overflow, guard page, or corrupt RSP. |
| ELF load failure | Parse `elf:` lines, segment flags, NX stack, auxv, and user range. |
| Triple fault / silence | Use POST16: last good code, first missing exit, and boot phase. |
| Hang | Check spinlock deadlock, infinite loop, or blocked deferred init. |

## Guardrails

- Do not classify from raw grep hits alone. Normalize first, then classify.
- Do not count duplicate lines as separate findings unless they have distinct causes.
- Do not dismiss `[FAIL]` lines until test ownership and cascade analysis are done.
- Do not fix "noise" by deleting logs. Demote expected test-path logs to `LOG_DEBUG` if needed.
- For crashes, always use `llvm-addr2line-19` before guessing.
- For races, name the exact shared state and synchronization gap before editing code.
- If multiple logs exist, do platform diffing before declaring a platform-specific bug.
- If the log is truncated, say so explicitly and anchor the report on the first bad transition.
- Default to fixing all actionable findings in ranked order unless the user explicitly asked for triage only.
