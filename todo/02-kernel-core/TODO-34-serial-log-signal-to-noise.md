---
schema_version: 1
id: serial-log-signal-to-noise
domain: 02-kernel-core
status: draft
title: "TODO-34 -- Serial Log Signal-to-Noise and Log-Cleanliness Gate"
---

# TODO-34 -- Serial Log Signal-to-Noise and Log-Cleanliness Gate

> **Goal:** Make the kernel serial log ASSERTABLE. Today a full test run emits 2712 `[WARN]` and 21 `[FAIL]` lines of which
> essentially all are correct behavior, so no automated check can say "this run was clean" and a genuine regression would land
> in the middle of thousands of expected lines and be seen by nobody. This TODO does not fix warnings -- an audit found only
> one real defect among roughly forty distinct messages. It fixes the fact that expected output and real failures are
> indistinguishable, and adds the gate that turns a clean log into a testable property.

> [!IMPORTANT]
> **Current state (measured 2026-07-27, native QEMU WHPX kernel-test layer, 30244-line capture).** 2712 `[WARN]` + 21 `[FAIL]`
> lines across ~40 distinct messages. A `serial-log-auditor` pass classified every distinct message at its emitting file:line:
> **1 real defect**, ~11 environment reports (no TPM, no RDRAND, OVMF firmware-table quirks), and ~30 deliberate test-path
> messages -- fault injection, refusal paths, and `TEST_SKIP` / `TEST_PENDING`, several of which name the test in the message
> text (`test-bad`, `NoSuchCategory`, `test_irql_require_at_most`). Two issues account for 86% of the volume by themselves
> (1776 DPC-watchdog + 567 quota pressure-tick lines). **Correction 2026-07-28: "owned elsewhere" was false when written -- a search of `todo/` found no owner for either, so both had been counted and never assigned. The perf half is now `02-kernel-core/TODO-35 §3` (item: "Bring `quota_pressure_tick` under the 100 us single-DPC watchdog threshold"); the log-noise half is section 5 of THIS file.** The single real defect is the `boot_trend`
> rename, already filed. So the log is CLEAN and completely unassertable at the same time -- that is the defect this TODO owns.

## Inputs

- [`src/kernel/test/test_runner.c`](../../src/kernel/test/test_runner.c) -- `_test_assert*`, `_test_skip`, `_test_pending`, the PASS run-length collapse, suite driver loop
- [`include/kernel/test/test.h`](../../include/kernel/test/test.h) -- `TEST_ASSERT` / `TEST_SKIP` / `TEST_PENDING` macros, `TEST_KLOG_SUPPRESS`
- [`src/kernel/klog.c`](../../src/kernel/klog.c) -- severity levels, the test-runner coloring path, live disk flush on the write path
- [`scripts/test-smoke.sh`](../../scripts/test-smoke.sh) -- `PASS_PATTERNS` (2 markers) and `FAIL_PATTERNS` (9 fatal strings)
- [`scripts/test.sh`](../../scripts/test.sh) -- summary parse, `[FAIL]` display path, per-suite result rendering
- [`src/kernel/boot_perf_budget.c`](../../src/kernel/boot_perf_budget.c) -- emits `[FAIL] BOOT-BUDGET` lines that no gate reads
- [`src/kernel/sched/dpc.c`](../../src/kernel/sched/dpc.c) -- DPC watchdog; its timing is disabled without a calibrated TSC
- → XREF: `01-boot-platform/TODO-29-boot-perf-health-observability.md §18` -- owns the one real defect this audit found (`vfs_rename_ex` failing on `boot-trend.json.tmp`) and the EXEC -> DESKTOP_READY milestone
- → XREF: `02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md §9` -- owns the quota pressure-tick overrun (567 lines here)
- → XREF: `02-kernel-core/TODO-08-time-filetime-management.md` -- TSC calibration; without it the DPC watchdog cannot time anything

## Outcome

- Deliberate test-path output is MARKED at emission, so a reader and a script can both tell "a test asked for this" from "something broke".
- The kernel test runner fails a run on an UNEXPECTED `[WARN]` / `[FAIL]`, against a reviewed baseline -- a new one is a test failure, not a line nobody reads.
- `scripts/test-smoke.sh` stops passing runs whose own log contains `[FAIL]`, closing the hole that let `[FAIL] BOOT-BUDGET` through a green smoke test.
- The dev-host blind spot is documented and surfaced: a run that cannot time DPCs says so in its summary instead of silently reporting nothing.
- Operators get a per-run signal summary (expected vs unexpected counts) rather than a 30k-line file to eyeball.

## Implementation Order

| ⭐   | Order | Deliverable                              | Depends On | Status |
| --- | :---: | ---------------------------------------- | ---------- | :----: |
| ⭐   |   1   | Mark expected test-path log output at emission | --         |  [ ]   |
| 💎   |   2   | Log-cleanliness gate in the kernel test runner | §1         |  [ ]   |
| 💎   |   3   | Smoke test fails on any `[FAIL]` in its own log | --         |  [ ]   |
| ⭐   |   4   | Surface the platform blind spots in the run summary | §2         |  [ ]   |
| 💎   |   5   | Rate-limit the repeat-offender warnings (86% of vol) | --         |  [/]   |

> 💎 = parity -- Linux kernel selftests and Windows WHQL both gate on unexpected log output.
> ⭐ = exclusive -- neither treats "expected diagnostic output" as a declared, asserted property of a test.

---

## 1. Mark Expected Test-Path Log Output at Emission

~30 of the ~40 distinct WARN/FAIL messages are a test deliberately exercising a refusal or fault-injection path. The message is
correct and its severity is correct FOR PRODUCTION -- `ex: lookaside use-after-free` must be `[FAIL]` when it happens for real.
The defect is that nothing records that a test ASKED for it, so the only way to classify a line today is to read the emitting
source, which is what the 2026-07-27 audit had to do for all forty.

Do NOT lower these severities. A test-only severity would hide the production line that must still fire, which is the trap the
audit explicitly warned against. Mark the WINDOW instead: a test declares the output it expects, and lines emitted inside that
window are tagged as expected while everything else stays exactly as it is.

- [ ] Add `TEST_EXPECT_LOG(substr, reason)` scoping a declaration that lines matching `substr` are expected until the suite ends; auto-cleared by the existing action-drain so no test can leak a suppression into the next one.
- [ ] Tag matched lines at emission with a marker the log carries (e.g. `[WARN*]` / a trailing `(expected: <reason>)`) so a human reading raw serial sees the classification without a tool.
- [ ] Count expected vs unexpected per suite in `test_state_t`; a declared expectation that never matched is itself a failure (the test stopped exercising the path it claims to).
- [ ] Convert the ~30 audited test-induced messages to declare their expectation, working from the classification table in the History section below.
- [ ] Commit: `"test: declare expected log output at the emission site"`

**Test checkpoint:** a suite declaring `TEST_EXPECT_LOG("returning more than charged", ...)` runs with zero unexpected lines; the same suite with the declaration removed reports one unexpected `[FAIL]`; a declaration that matches nothing fails the suite. Test on: QEMU TCG, QEMU WHPX; bare metal.

---

## 2. Log-Cleanliness Gate in the Kernel Test Runner

Once §1 makes expectation explicit, "the log was clean" becomes checkable. Today it is not: `scripts/test.sh` parses the
framework's own pass/fail counters and displays `[FAIL]` lines without ever failing on them, so a run reporting
`PASS: 25509 kernel tests passed` can and does contain unexpected `[FAIL]` output.

The gate belongs in the KERNEL runner, not in the shell script. The runner is the only place that knows which suite was active
when a line was emitted, and it already owns the summary the shell parses. A shell-side regex over the whole log cannot
attribute a line to a suite and would re-introduce the same "which of these forty is real" problem at a different layer.

- [ ] Extend the run summary with `unexpected` alongside `passed/failed/skipped/pending/leaked/quota-leaked`, and make a non-zero count fail the run.
- [ ] Baseline file for the environment-class lines (no TPM, no RDRAND, OVMF table quirks) that are correct on QEMU and would otherwise fail every local run; entries carry a reason and are reviewed, not auto-generated.
- [ ] Teach `scripts/test.sh` to parse the new counter and fail on it, matching how it already fails on the `leaked` counter.
- [ ] Commit: `"test: fail a run on unexpected serial output"`

**Test checkpoint:** a deliberately-injected unexpected `[WARN]` fails the run and names its suite; the same line declared via §1 passes; the summary reports `unexpected=0` on a clean run and `scripts/test.sh` propagates a non-zero exit. Test on: QEMU TCG, QEMU WHPX; bare metal.

---

## 3. Smoke Test Fails on Any `[FAIL]` in Its Own Log

> **PARTIALLY OVERTAKEN 2026-07-28 (`a6a2b828`) -- read before implementing.** The FATAL half of this section shipped: `test-smoke.sh` now fails on generic `[CRIT]` / `FATAL -- system halted` / `system halted` anywhere in the capture (not just the nine-string allowlist), re-checks the fail patterns AFTER the grace window (a crash 2.5s past the pass markers used to be invisible), asserts the log ENDS clean to catch shapes nobody enumerated, and boots `-smp 2` where it had silently run one CPU forever. What remains here is specifically the `[FAIL]`-line half -- BOOT-BUDGET and friends -- which is a different judgement call.
>
> **Do not naively "fail on ANY `[FAIL]`" without settling the third item below first.** A current WHPX capture carries `[FAIL] BOOT-BUDGET: TOTAL boot took 16246ms (target 4000ms)` and several more; those are performance budgets missed on a slow accelerator, not correctness failures, and gating a smoke run on them would make the gate red on every WHPX boot and train everyone to ignore it. Decide the BOOT-BUDGET question first, then implement.


`scripts/test-smoke.sh` passes on two markers (`Boot complete in`, `C:\>`) and fails only on a nine-string fatal allowlist. A
`[FAIL]` outside that list passes straight through. Demonstrated 2026-07-27: a local smoke run reported `SMOKE TEST PASSED`
while its own log held `[FAIL] BOOT-BUDGET: EXEC -> DESKTOP_READY took 1139ms (target 100ms)` and 32 `[WARN]` lines. This is
the specific reason an operator's reasonable assumption -- that smoke testing after each section would catch these -- did not hold.

- [ ] Fail the smoke run on ANY `[FAIL]` in the captured log, not only the nine fatal strings; the nine stay as the fast-path early abort.
- [ ] Allowlist the environment-class `[FAIL]`/`[WARN]` lines by exact reason, sharing the §2 baseline file so the two gates cannot drift apart.
- [ ] Decide and document whether `[FAIL] BOOT-BUDGET` should gate a smoke run at all, or be demoted to `[WARN]` because a wall-clock budget is platform-dependent by nature; today it is a `[FAIL]` that nothing reads, which is the worst of both.
- [ ] Commit: `"smoke: fail on unexpected [FAIL] output, not just the fatal allowlist"`

**Test checkpoint:** a smoke run whose log contains an unexpected `[FAIL]` exits non-zero and prints the offending line; a run containing only baselined lines still passes; the nine fatal patterns still abort early rather than waiting for the timeout. Test on: QEMU KVM, QEMU TCG; bare metal.

---

## 4. Surface the Platform Blind Spots in the Run Summary

65% of the warnings in the 2026-07-27 capture (1776 DPC-watchdog lines) CANNOT appear on the development host. The watchdog
needs a calibrated invariant TSC and prints `single-DPC threshold 100 us (no TSC -- timing off)` without one, timing nothing.

**This is NOT just a TCG limitation** -- corrected 2026-07-27 after re-checking. It is off on BOTH local tiers: `/dev/kvm` is
writable on the dev host and `scripts/test-smoke.sh` does select KVM, yet `build/smoke-test.log` and `build/test.log` both carry
the `no TSC -- timing off` banner. So neither the KVM inner loop nor the CI-parity tier can observe DPC timing at all, and the
only place those 1776 lines exist is a native WHPX or bare-metal run that no gate covers. A developer runs the full local suite,
sees green, and concludes the DPC path is healthy when the measurement was switched off on every engine available to them.

The same class of gap hides `BOOT-BUDGET` wherever `boot_timing_tsc_freq()` reads below 1000. A check that is silently disabled
is worse than one that fails, because it reports success -- and here two of them are disabled on every tier a developer can run.

- [ ] Report DISABLED checks in the run summary, not just results: a run that could not time DPCs says so, in the same block as the pass/fail counts.
- [ ] Apply the same treatment to every other capability-gated check (boot perf budget, MSR probes, anything gated on `cpu_has`), so "not run" is never presented as "passed".
- [ ] Record which checks are structurally unavailable per platform (TCG / KVM / WHPX / bare metal) so an operator knows what a green local run does and does not cover.
- [ ] Commit: `"test: report disabled checks so 'not run' never reads as 'passed'"`

**Test checkpoint:** a TCG run's summary names the DPC-timing check as disabled with its reason; a WHPX run reports it active; the per-platform coverage note matches what each platform actually ran. Test on: QEMU TCG, QEMU KVM, QEMU WHPX; bare metal.

---

## 5. Rate-Limit the Repeat-Offender Warnings That Produce 86% of the Volume

Two reporters produce the overwhelming majority of every capture's warning lines, and neither throttles. `dpc.c:215` emits `watchdog: DPC %p on CPU %u ran %u us` unconditionally on every single overrun -- there is no rate limiting of any kind at that call site. The quota sampler's own `pressure tick overran` report does carry partial suppression ("N further overrun(s) suppressed while that report waited"), which is why it produced 567 lines where the DPC watchdog produced 1776 for the same underlying events.

This section owns the LOG-NOISE half only. The underlying performance defect -- `quota_pressure_tick` genuinely running 100-481 us against a 100 us threshold -- is a real bug and must not be silenced instead of fixed; it is owned by `02-kernel-core/TODO-35 §3`. Rate-limiting here is for the case where a slow DPC is legitimately being reported: the operator needs to know it happened and how often, not to receive one line per occurrence forever.

- [x] Rate-limit the DPC-watchdog warning per offending routine: report the first N, then collapse to a periodic summary naming the routine, the count, and the worst observed duration.
      **Shipped 2026-07-28 (`41a09b70`):** first 3 crossings per routine warn, the rest are counted silently. Per-CPU table with no lock -- safe because `drain_queue()` is reached only via `smp_this_cpu()->cpu_id` from both callers (verified, not assumed), which matters because the report path calls `klog()` and holding a spinlock across serial output is forbidden. The table lives OUTSIDE `struct dpc_watchdog`, which an existing `_Static_assert` pins to one cache line.
- [x] Emit a single end-of-boot roll-up of DPC overruns per routine, so a capture states "quota_pressure_tick: 1776 overruns, worst 481 us" once instead of 1776 times.
      **Shipped 2026-07-28 (`41a09b70`):** `dpc_watchdog_report()`, wired into the boot report block in `boot_desktop.c`. Verified on WHPX where the flood occurred -- **3 per-occurrence lines against 1776 before**, plus the roll-up naming the routine, count and worst case.
- [ ] Apply the same shape to any other unconditional per-event `LOG_WARN` in a hot path (audit for the pattern rather than fixing only the two known instances).
      **Still open 2026-07-28** -- the DPC instance was fixed directly; the AUDIT for other instances was not performed, so this is the remaining work in this section.
- [ ] Commit: `"dpc,klog: rate-limit repeat-offender warnings, roll up per routine"`

**Test checkpoint:** a boot whose sampler overruns hundreds of times produces a bounded number of DPC-watchdog lines plus one roll-up naming the routine and count; the underlying overrun is still visible and still attributable. Test on: QEMU TCG, QEMU KVM, WHPX.

---

## OS Comparison

| ⭐   | Feature                              | 🪟 Win11                         | 🐧 Linux                                | 🚀 Impossible OS                          |
| --- | ------------------------------------ | ------------------------------- | -------------------------------------- | ---------------------------------------- |
| 💎   | Gate on unexpected kernel log output | ⚠️ WHQL/HLK scans, post-hoc     | ⚠️ selftests + `dmesg` diff, per-suite | ⬜ §2 runner-side, suite-attributed       |
| ⭐   | Expected diagnostic output declared  | ❌ no equivalent                 | ❌ no equivalent                        | ⬜ §1 declared at the emission site       |
| 💎   | Smoke gate reads its own log         | ✅ boot-critical ETW checks      | ✅ CI greps `dmesg` for oops/WARN       | ⚠️ §3 today: 2 markers + 9 fatal strings |
| ⭐   | "Check disabled" reported distinctly | ⚠️ HLK marks unsupported as N/A | ⚠️ selftests print SKIP inconsistently | ⬜ §4 disabled != passed, in the summary  |

## Unit Tests

- [ ] `TEST_EXPECT_LOG` scoping: matched line counts as expected, unmatched declaration fails the suite, declaration does not leak past the action drain.
- [ ] `unexpected` counter: increments on an undeclared `[WARN]`/`[FAIL]`, stays zero on a declared one, appears in the summary line `scripts/test.sh` parses.
- [ ] Baseline file: a baselined environment line does not fail the run; a non-baselined one does; a malformed baseline entry is rejected rather than silently ignored.
- [ ] Disabled-check reporting: with DPC timing off the summary names it disabled; with it on the summary does not.
- [ ] Pure helpers only -- no live boot infrastructure per [docs/infrastructure/test-policy.md](../../docs/infrastructure/test-policy.md).

## Verification

- [ ] `bash scripts/test.sh` reports `unexpected=0` and exits 0 on a clean tree.
- [ ] An injected unexpected `[WARN]` fails the run, names its suite, and is removed cleanly afterwards.
- [ ] `bash scripts/test-smoke.sh` fails on a log containing an unexpected `[FAIL]`.
- [ ] A native WHPX capture of the full kernel layer contains no undeclared WARN/FAIL outside the baseline.
- [ ] Full suite + smoke green on QEMU TCG and KVM; a native WHPX run confirms the DPC-timing path is active there.

## History

**2026-07-27 -- filed from a native-QEMU serial audit.** Source capture: `run-all-tests.bat` kernel layer, 30244 lines, 2712
`[WARN]` + 21 `[FAIL]`. A `serial-log-auditor` pass classified all ~40 distinct messages at their emitting file:line; three
classifications were independently spot-checked against the test source before this TODO was written.

Result: **1 real defect** (`boot_trend` `vfs_rename_ex` failure leaving a stray `.tmp` -- filed to `01-boot-platform/TODO-29 §18`,
not owned here), ~11 environment-class (absent TPM/RDRAND, OVMF firmware-table placement, stale dirty FAT32 bit), ~30
test-induced (fault injection, refusal paths, `TEST_SKIP`/`TEST_PENDING`). Excluded from the count as already-owned: 1776
DPC-watchdog and 567 quota pressure-tick lines, together 86% of all warning volume.

The audit is the reason this TODO exists in its current form. The obvious reading of a 2712-warning log is "fix the warnings";
the evidence says almost nothing is broken and the real defect is that a clean log is not a testable property. Two contemporaneous
fixes came out of the same capture and are NOT items here: repeated-PASS run-length collapse in `test_runner.c` (the AVL suite
alone dropped 3500 lines to 25), and `BOOT-BUDGET` naming both interval endpoints after "VFS took 3212ms" turned out to be the
VFS -> PARTITION interval rather than `vfs_init`.
