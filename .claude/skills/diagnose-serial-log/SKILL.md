---
name: diagnose-serial-log
description: Analyze serial logs with a structured event model. Detect crashes, bugs, races, leaks, perf regressions, POLICY violations (from POLICIES.md), ACCURACY drift (log claims vs code reality), REGRESSION against a baseline log, and SCOPE_CREEP outside a declared TODO section. Fix root causes, run Codex adversarial review on every fix, verify the symptom is gone. Use when the user pastes a log or points to serial output files.
---

# Diagnose Serial Log

> **External-Reviewer Contract:** This skill dispatches Codex as part of its workflow. Every finding goes through `superpowers:receiving-code-review` (verify at file:line, Fix / Reject / Accept, never blind-implement). Canonical contract: [docs/infrastructure/ai-system.md#external-reviewer-contract-codex](../../../docs/infrastructure/ai-system.md#external-reviewer-contract-codex).

## Use This Skill When

- The user pastes serial output or points to a log file (for example `build/test-probe.log`, `debug-tmp/*.log`, `build/smoke-test.stripped.log`).
- Serial output shows unexpected `[WARN]`, `[FAIL]`, `[CRIT]`, crash dumps, hangs, or degraded boot behavior.
- A test run failed and the user wants the actual kernel bug, not a grep dump of every scary-looking line.
- Multiple platform logs exist and the user wants to know what is real, what is platform-specific, and what is test noise.
- You want to audit a shipped commit for POLICY drift, ACCURACY drift, or REGRESSION against a stored baseline.

## Arguments

The skill accepts the following optional arguments (inline in the invocation):

- `--strict` -- enable strict-only detectors in POLICIES.md (higher false-positive rate; use when auditing a stable commit, not mid-debug).
- `--scope "<description>"` -- pin the expected TODO scope for scope-creep detection (for example `--scope "TODO-01 \u00a710 POST16 manifest"`). Without this, scope-creep detection is skipped.
- `--baseline <path>` -- override default baseline log for regression diffing. Default baseline path: `build/smoke-test.baseline.log`. If absent and no override, baseline regression pass is skipped with a `NOTE`.
- `--policies-only` -- run only the policy-scan pass; skip crash/race/perf/accuracy analysis. Useful for CI auditing.
- `--skip-fix` -- triage only, no code changes or commits. Useful when reporting findings to the user for human decision.

When arguments conflict (for example `--policies-only` and `--skip-fix` both set), the narrower scope wins (`--policies-only` runs its pass and stops).

## Debugging Discipline

> Apply `superpowers:systematic-debugging` principles:
> - **Observe before theorizing.** Read the full log first. The first visible error is often not the causal one.
> - **Model the log, do not just grep it.** Build a normalized event list, then classify events from that model.
> - **Collapse repeats.** Twenty copies of the same warning are one finding with a repeat count, not twenty findings.
> - **Fix the earliest causal failure.** Later failures are often fallout.
> - **Verify the symptom disappeared.** Do not stop at "the code change looks right."
> - **Policy, accuracy, and regression are first-class.** A POLICY violation that didn't crash today is still a finding. An `[OK]` line claiming success while its POST16 exit pair is missing is ACCURACY drift. A log that matches the old baseline in structure but has a new 200ms phase is REGRESSION.

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

Reduce events into findings. Finding categories have been expanded:

```text
finding {
  category           // BUG, CRITICAL, RACE, REGRESSION, REGRESSION_BASELINE, LEAK,
                     // PERF_REGRESSION, POLICY, ACCURACY, SCOPE_CREEP, EXPECTED,
                     // NOISE, ENCODING
  confidence         // 0.00 - 1.00
  first_line
  first_timestamp_ms?
  subsystem
  summary
  evidence[]         // representative lines, counts, related test name, POST16 pair, source path
  repeats
  root_cause_rank    // 1 = fix first
  policy_ref?        // P1.1 / P2.2 / etc. when category == POLICY
  baseline_delta?    // { new_lines: [], missing_lines: [], time_delta_ms } when category == REGRESSION_BASELINE
  accuracy_claim?    // { log_claim, code_reality } when category == ACCURACY
  scope_expected?    // the --scope string when category == SCOPE_CREEP
}
```

Category glossary:

- **CRITICAL** -- panic, page fault, triple fault, missing POST16 exit followed by silence.
- **BUG** -- real `[FAIL]` not attributable to tests or platform limits.
- **RACE** -- SMP/ordering anomaly with evidence of shared mutable state.
- **LEAK** -- alloc/free imbalance, resource exhaustion.
- **PERF_REGRESSION** -- timing anomaly specific to this boot.
- **POLICY** -- violation of a rule in POLICIES.md (e.g. POST16 after Phase 3, mojibake in serial).
- **ACCURACY** -- log claim contradicts code or manifest reality (e.g. `PMM [OK]` emitted but POST16 `0x0021` exit pair missing).
- **REGRESSION_BASELINE** -- current log differs from `build/smoke-test.baseline.log` in ways beyond noise thresholds.
- **SCOPE_CREEP** -- subsystem / init activity outside the declared `--scope` appears in the log.
- **EXPECTED** -- known platform limitation (e.g. missing TPM on QEMU).
- **NOISE** -- log line inside a test exercising that error path.
- **ENCODING** -- Unicode-dash / section-sign / mojibake issues (subset of POLICY.P1, kept as its own category for back-compat).

Do not present raw lines first. Present findings derived from the model.

## Workflow

### Phase 1 -- Parse, Normalize, Classify, Baseline-Diff

1. **Parse arguments** (step 0.5 before any file reading)
   - Extract `--strict`, `--scope`, `--baseline`, `--policies-only`, `--skip-fix` from the invocation.
   - Record them into the working context so later passes can honor them.

2. **Read all relevant logs first**
   - If a file path is given, read it.
   - If multiple candidate logs exist nearby (`*-serial.log`, `*-tcg*.log`, `*-1cpu*.log`, `*-baremetal*.log`), read all of them. Multi-log comparison is first-class, not optional.
   - If the user pasted only a fragment, say so and mark the analysis partial.

3. **Extract platform context**
   - Parse the preamble before `[BOOT]` for `Accel:` (whpx/tcg), `CPUs:`, `Timer:`, `Device:`, `QEMU`, `VBox`, or bare-metal hints.
   - Record a per-log platform header. If no preamble exists, note `bare metal or redirected serial`.
   - Use this later for platform diffing, not as an excuse to dismiss a bug.

4. **Build the normalized event stream**
   - Parse timestamps, CPU tags, subsystem prefixes, severity tags, POST16 lines, test result lines, deferred-init lines, heap lines, degraded subsystem lines, and perf comparison lines.
   - Canonicalize variable fields in messages for dedup keys:
     - Hex addresses become `<addr>`
     - Decimal IDs become `<id>`
     - Durations become `<ms>`
     - Paths become `<path>`
   - Keep the raw line in evidence, but classify from the canonicalized key.

5. **Check completeness and truncation**
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

6. **Parse structured sections**
   - **Tests:** category headers, PASS/FAIL/SKIP counts, final summary, per-category totals.
   - **POST16:** ordered code list, entry/exit pairs, phase transitions, last-good code. Load `build/post16-manifest.env` if present so codes can be named in the report.
   - **Boot timing:** timing table, duration table, perf comparison warnings.
   - **Deferred init:** expected start/done pairs and completion counts.
   - **Heap/resource pressure:** utilization, exhaustion messages, degraded subsystem list.

7. **Collapse duplicates before classification**
   - Group events by dedup key.
   - For each group, keep:
     - first occurrence
     - last occurrence
     - repeat count
     - distinct CPUs
     - adjacent test names, if any
   - A repeated warning is one finding with `repeats=N`.

8. **Validate boot ordering and identify the first bad transition**
   - Verify phase ordering: P0 -> P1 -> P2 -> P3.
   - Check POST16 entry/exit pairs against `build/post16-manifest.env`'s `POST16_REQUIRED_CODES` if available.
   - Flag the earliest missing exit or earliest out-of-order phase as `FIRST_BAD_TRANSITION`.
   - Report:
     - `Last good POST16`
     - `First missing POST16 exit`
     - `First subsystem that failed after a good transition`
   - For boot hangs or triple-fault style silence, this block is mandatory.

9. **Baseline regression pass (Phase 1b)** -- MANDATORY when a baseline exists
   - Determine baseline path: `--baseline <path>` arg, else `build/smoke-test.baseline.log`. If file missing: emit a single `NOTE: baseline regression pass skipped (no <path>)` and continue. No error.
   - Normalize the baseline through the same pipeline (steps 4, 6, 7) with canonicalization.
   - Compute diffs:
     - **New log lines:** canonicalized keys present in current log but not in baseline.
     - **Missing log lines:** keys present in baseline but not in current log (regression of expected output).
     - **Timing drift:** for every named phase, compute `current_ms - baseline_ms`; flag phases whose delta exceeds the phase budget (default: 20% of baseline, or 100ms absolute, whichever is larger).
     - **POST16 coverage delta:** required codes present in baseline but missing now = REGRESSION_BASELINE Critical. Required codes present now but missing in baseline = new coverage (log as info, not a finding).
     - **Test count delta:** new FAIL, new SKIP; regressed PASS count.
   - Emit one `REGRESSION_BASELINE` finding per material delta. Trivial differences (single-line timestamp jitter under 5ms, device probe ordering with no functional change) are ignored.

10. **Analyze SMP and ordering anomalies**
    - Use `[cpu:N]` tags to detect:
      - double-init by multiple CPUs
      - AP logging before BSP completed a required earlier phase
      - mixed mid-line output indicating non-atomic logging
    - If the same canonicalized init line appears on multiple CPUs, treat that as a race clue, not two bugs.

11. **Classify findings with confidence and reasons**
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

12. **Policy-scan pass (Phase 2a)** -- MANDATORY
    - Read [POLICIES.md](POLICIES.md) (local to this skill directory).
    - Walk each detector (P1.1 through P8.3). Skip strict-only detectors unless `--strict` was passed.
    - **MECHANICAL RULE: run the detection regex, do NOT reason about whether hits exist.** For each detector, execute the exact grep/python scan from its Detection field against the full log. LLM-style "I read the log and didn't see any" judgment is FORBIDDEN -- it has failed before (2026-04-18: a mojibake `┬º` hit on a TEST-owned line was missed because the policy scan was qualitative, not mechanical). A single byte-level scan with `python3 -c "import sys; [print(i,l) for i,l in enumerate(open(path,'rb').read().splitlines(),1) if any(b>0x7E for b in l)]"` catches every non-ASCII hit in seconds; cheap, exhaustive, zero false-negatives.
    - Recommended mechanical sweep (run once, covers P1.1/P1.2/P1.3):
      ```bash
      python3 -c "
      import sys
      for i, line in enumerate(open('<log>', 'rb').read().splitlines(), 1):
          if any(b > 0x7E and b != 0x09 for b in line):
              print(f'{i}: {line[:120].decode(\"utf-8\",errors=\"replace\")}')
      "
      ```
      This returns every line with non-ASCII bytes. If zero hits: P1.1/P1.2/P1.3 all pass. If non-zero hits: classify each individually.
    - For structured detectors (P2, P3, P6, P7, P8), run the relevant grep/count command:
      - P2.2 entry/exit pairs: `grep -oE "POST 0x[0-9A-F]{4}"` then walk pairs against manifest
      - P3.1 thread_create: `grep -nE "thread_create|kthread_create" <log>`
      - P3.5 deferred pairs: `grep -E "DEFERRED\] .* (start|done)"` then pair by name
      - P6.1 AP count: `grep -oE "SMP: [0-9]+ CPUs up"` vs `boot_info.cpu_count`
      - P7.1 #NM: `grep -nE "#NM|vector 7"`
      - P8.1 boot time: `grep -oE "Boot complete in [0-9.]+s"` -> compare to budget
      - P8.3 repeat rate: canonicalize + dedup, flag groups with repeats > 100
    - For each hit, emit a `POLICY` finding with:
      - `policy_ref` set to the detector id (e.g. `P2.1`)
      - `severity` copied from the detector
      - `summary` set to the detector's Rule line
      - `evidence` including the first matching log line and the repeat count
      - `remediation` copied verbatim from the detector's Remediation field
    - Do NOT reinterpret the detector; follow what POLICIES.md says. If a detector's regex produces a false positive on the current log, record the finding with `confidence=0.50` and flag it for user review rather than silently dropping it.
    - **Report each detector's scan result explicitly in the analysis** -- even "P1.2 section-sign: 0 hits" should appear in the dashboard, so it's visible that the detector ran. Silent omission looks identical to "forgot to check." Add a per-detector scan-result row to the report when running under `--strict` or at user request.

13. **Accuracy cross-check pass (Phase 2b)** -- MANDATORY for non-truncated logs
    - For every `[OK]` / `[DONE]` / success line emitted by a subsystem, verify the matching POST16 exit pair is present (when `build/post16-manifest.env` is loaded). Mismatch = `ACCURACY` finding with `accuracy_claim = { log_claim: "PMM OK", code_reality: "POST16 0x0021 missing" }`.
    - Verify `Boot complete in Ns` pairs with a matching `C:\>` or compositor-ready marker. Lone `Boot complete in` without shell prompt = ACCURACY (kernel claimed complete but userland failed).
    - Verify `SMP: N CPUs up` matches `boot_info.cpu_count` from preamble if visible.
    - Verify test-runner summary `N passed, M failed, K skipped` equals the observed PASS/FAIL/SKIP count in the parsed test section.
    - Verify degraded-subsystems report matches actual WARN/ERROR lines (a subsystem missing from the degraded list that emitted CRITICAL earlier = ACCURACY).
    - Report `ACCURACY` findings at `Medium` severity by default; promote to `High` if the mismatch hides a real subsystem failure.

14. **Scope-creep pass (Phase 2c)** -- runs only when `--scope` was provided
    - Parse the `--scope` argument to extract owning TODO and subsystem keywords (e.g. `"TODO-01 \u00a710 POST16 manifest"` -> subsystems = {post16, smoke, manifest}).
    - Against the event stream, identify subsystems/init functions active outside the scope whose OUTPUT changed from baseline (if baseline exists) or whose activity is not plausibly required to boot to the declared scope's test surface.
    - Emit `SCOPE_CREEP` findings with `scope_expected` set to the user's declared string, and `summary` describing the out-of-scope subsystem that ran / changed.
    - False positives are expected early: the first run for a given `--scope` string is a best-effort report. Strictness improves as baselines accumulate.
    - When no `--scope` was given: emit `NOTE: scope-creep pass skipped (no --scope)` and continue.

15. **Rank by likely root cause**
    - Sort findings by:
      1. earliest CRITICAL or first bad transition
      2. earliest dependency failure
      3. earliest BUG with high confidence
      4. POLICY Critical + POLICY High
      5. ACCURACY High
      6. REGRESSION_BASELINE Critical
      7. RACE clues
      8. LEAK / PERF_REGRESSION / ENCODING
      9. POLICY Medium + POLICY Low
      10. ACCURACY Medium, SCOPE_CREEP
      11. repeats and fallout
    - The first item in the ranked list is the issue to diagnose first.

16. **Present a compact findings table**
    - Default to actionable items only. Omit NOISE and EXPECTED unless the user asked for them.
    - Use this shape:
      ```
      | # | Rank | Category | Sev | Conf | Subsystem | First Seen | Repeats | Summary |
      ```
    - Add a per-category count summary row above the table: `CRITICAL: 0  BUG: 2  POLICY: 3  ACCURACY: 1  REGRESSION_BASELINE: 0  PERF_REGRESSION: 1`.
    - Do not stop to ask "Fix all?" unless the user explicitly requested triage only. Default behavior is: fix all actionable items in ranked order.
    - If `--skip-fix` was passed, stop here and report; do not proceed to Phase 2.
    - If `--policies-only` was passed, report only POLICY findings and stop.

### Phase 2 -- Diagnose the Ranked Findings

For each actionable finding, in ranked order:

17. **Trace the finding to source**
    - Search first by exact log string.
    - If exact-string search fails, search by subsystem prefix plus stable message fragment.
    - If the line is dynamic, search for nearby constant text or the enclosing subsystem logger.
    - For crashes, use RIP/fault address with:
      ```bash
      llvm-addr2line-19 -e build/kernel.exe -f <RIP>
      ```
    - For POLICY findings, the detector's Remediation line already points at the fix shape.

18. **Determine the root cause**
    - **Errors:** identify the failing check, the bad input, and the caller that supplied it.
    - **Crashes:** read the crashing function and the code just before it; do not guess from the exception name alone.
    - **Races:** identify the exact shared mutable state and the missing lock, atomic, or barrier.
    - **Leaks:** identify alloc/free imbalance and the failing rollback path.
    - **Perf regressions:** identify which init step moved, what dependency changed, and whether the slowdown is causal or noise.
    - **Deferred init failures:** verify all `[DEFERRED] name +Nms` lines have matching completion and that they ran inline on BSP.
    - **Policy violations:** follow the Remediation line in POLICIES.md verbatim. If the remediation does not fit the observed case (rare), record the mismatch and escalate to the user before editing code.
    - **Accuracy drift:** find the code that emits the `[OK]` line; confirm whether the log is lying (missing POST16 exit) or the check is misclassified (POST16 exit actually ran under a different code path).
    - **Baseline regression:** find the commit that introduced the delta via `git log` and the delta's subsystem(s); if the regression is intentional, update the baseline rather than chasing a phantom fix.
    - **Scope creep:** report the out-of-scope subsystem; do not fix unless the user confirms the scope expansion.

19. **Do explicit platform comparison when multiple logs exist**
    - Build a finding-presence matrix by canonicalized key:
      - present on all logs = real kernel bug
      - WHPX only = likely hypervisor interaction
      - TCG only = likely emulation limit or CPUID gating bug
      - SMP only = likely race or ordering bug
      - bare metal only = real hardware sensitivity hidden by VM emulation
    - Present the matrix, not just a sentence.

20. **Prune cascades**
    - For each finding after the first ranked root cause candidate, ask:
      - did it happen after a dependency failure?
      - is it the same canonicalized message repeating?
      - is it from a test intentionally exercising a failure path?
    - Mark fallout explicitly:
      - `CASCADE_OF #1`
      - `REPEAT_OF #2`
      - `TEST_NOISE`
    - Do not fix cascades first.

### Phase 3 -- Fix + Codex Review

21. **Fix one root cause at a time**
    - Invoke `kernel-code-quality` or `boot-code-quality` gates that apply:
      - Gate 2 (SMP) for shared state
      - Gate 4 / Gate 10 (Boot-path / POST16) for init ordering / POST16 issues
      - Gate 5 (Error handling) for every fix
      - Gate 6 (Bare metal) for platform-specific or hardware-touching fixes
    - Prefer the smallest fix that closes the actual root cause, not a broad workaround.
    - For POLICY fixes, the POLICIES.md Remediation line is the authoritative shape. Do not improvise a different shape.

22. **Build**
    - Run:
      ```bash
      bash scripts/build.sh
      ```
    - Confirm:
      ```bash
      tail -1 build/build.log
      ```
      shows `=== BUILD OK ===`.

23. **Codex adversarial review on the fix (MANDATORY)** -- NO EXCEPTIONS
    - After each root-cause fix (or each coherent batch of related fixes), dispatch a Codex adversarial review before committing. This catches the "plausible-looking fix that doesn't actually close the root cause" case, which has happened before.
    - Dispatch with:
      ```bash
      node "$HOME/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<prompt naming the finding, the root cause, the fix, and the expected log signal that confirms the fix>"
      ```
    - **Mandatory angles for the prompt:** (1) does the fix actually close the finding's root cause or only the visible symptom? (2) are there error paths, SMP cases, or boundary values the fix misses? (3) does the fix introduce a regression in adjacent code? (4) for POLICY fixes: does the fix match the POLICIES.md Remediation shape?
    - Apply `superpowers:receiving-code-review` to every Codex finding. Codex can be wrong; verify each finding against code before acting.
    - Unresolved Critical/High Codex findings block commit. Iterate: fix -> rebuild -> re-dispatch, up to 3 rounds. After round 3 with still-unresolved Critical/High, stop, downgrade the finding's fix status to "attempted", and escalate to the user.
    - Skip only when `--skip-fix` was passed (no fixes means no review).

24. **Verify against the original symptom**
    - Re-run the relevant test or boot path when possible (`bash scripts/test-smoke.sh` for boot-path fixes, `bash scripts/test.sh SUITE=<cat>` for test-path fixes).
    - Confirm the original finding is gone or demoted correctly.
    - For REGRESSION_BASELINE findings, re-diff against the baseline to confirm the delta closed.
    - For POLICY findings, re-run the policy-scan pass (step 12) on the fresh log and confirm the detector no longer fires.
    - If the log is not reproducible in the current environment, verify by code path and state transition, then say that runtime confirmation is pending.

25. **Repeat only if more actionable findings remain**
    - Re-rank after each fix.
    - If a later finding disappears as fallout, record that instead of claiming a second independent fix.

### Phase 4 -- Report

26. **Health dashboard first**
    - Prefer a compact summary:
      ```
      Platform: WHPX 2-CPU | Boot: 13.5s | Tests: 1141/1142 | Heap: 24% | Degraded: TPM
      Phases: P0 OK | P1 OK | P2 OK | P3 FAIL at POST16 0xD809 -> missing 0xD80A
      Baseline: +2 new lines, -0 missing, +200ms VMM phase (delta flagged)
      Scope: --scope "TODO-01 \u00a710 POST16 manifest" -> 1 out-of-scope activity (sched init re-fired)
      Top cause: BUG sched (0.93) | Fallout: 3 cascades | Perf regressions: 1 | Policy: 2 (P2.1, P4.2)
      ```
    - Omit unavailable fields rather than inventing them.

27. **Detailed findings table**
    - Use:
      ```
      | # | Rank | Category | Sev | Conf | Subsystem | Root Cause | Evidence | Fix | Codex | Status |
      ```
    - Include `repeats=N` and cross-platform presence where relevant.
    - By default, omit NOISE and EXPECTED rows.
    - `Codex` column: `PASS` if the Codex review approved, `FIX N` if it required N rounds, `BLOCK` if unresolved.

28. **Commit fixes**
    - Group by subsystem or single root cause.
    - Do not batch unrelated fixes into one commit.
    - Commit message format: `"<subsystem>: <one-line summary> (diagnose-serial-log finding #N)"`. Attach the Codex review verdict in the commit body.
    - Skip when `--skip-fix` was passed.

## Noise and Expected-Output Rules

Treat this section as rules, not loose prose. The list below is a fast-path cache only. The authority is still the actual test ownership and surrounding log context.

### Matching Order

1. **Test ownership beats severity text**
   - If the log line is emitted while a test is intentionally exercising that path, classify as `NOISE` even if the line says `[FAIL]`.
2. **Known platform limitation beats generic WARN**
   - Example: missing TPM on QEMU.
3. **Earlier causal failure beats later downstream warnings**
   - Downstream fallout is a cascade, not a new bug.
4. **POLICY findings are not NOISE.** A Unicode-dash in serial is a POLICY violation even inside a test run. Test ownership demotes severity-text classification, not encoding/policy rules.
5. **Exact-string matches are optional**
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

Retired entries (silenced at source by `TEST_KLOG_SUPPRESS` -- no longer
appear in serial; delete any review report matching these):

- `sched: uthread_create: PID 0 has no PEB` (was owned by
  `test_uthread_rejects_kernel_task`; wrapped 2026-04-19 kernel-test-
  harness roadmap).
- `boot: boot_payload: ...` (16 variants owned by
  `test_boot_info.c` payload-negative tests; wrapped 2026-04-19 same).

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
- **POLICY findings follow POLICIES.md remediation verbatim.** Do not improvise; the detector knows the fix shape.
- **ACCURACY findings never get silenced by updating the claim.** If `PMM [OK]` is wrong, fix the init, not the log line.
- **REGRESSION_BASELINE findings check the commit that moved the needle.** Use `git log --since=<baseline timestamp>` to narrow the diff before blaming a specific change.
- **SCOPE_CREEP findings are reports, not fixes.** The user confirms scope expansion; the skill never silently expands scope.
- **Every fix gets Codex adversarial review before commit** unless `--skip-fix` was passed. No exceptions for "simple" POLICY demotions or "obvious" typo fixes; Codex has caught plausible-looking fixes that didn't close root cause.
