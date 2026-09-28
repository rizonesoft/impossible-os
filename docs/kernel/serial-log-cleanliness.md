<!-- docs: covers=todo/02-kernel-core/TODO-34-serial-log-signal-to-noise.md sources=src/kernel/test/test_runner.c,include/kernel/test/test.h,src/kernel/klog.c,scripts/test-smoke.sh,scripts/test.sh,src/kernel/boot_perf_budget.c,src/kernel/sched/dpc.c reviewed=2026-09-28 order=34 -->
# Serial Log Cleanliness

## What is it?

The kernel serial log carries real defects and expected diagnostic output (fault-injection tests, refusal paths, reports about absent hardware) in the same `[WARN]`/`[FAIL]`/`[CRIT]` stream, with nothing at emission time telling one from the other. The roadmap records a 2026-07-27 audit of a 30,244-line capture of the kernel test layer under QEMU with WHPX that classified about forty distinct messages by hand and found one real defect among 2,712 `[WARN]` and 21 `[FAIL]` lines. This work is about making "the log was clean" something a script can check. Two pieces have shipped: the smoke test's log-cleanliness gate (section 3) and rate limiting of the DPC watchdog (part of section 5).

## How does it work?

The kernel test runner ([`test_runner.c`](../../src/kernel/test/test_runner.c)) keeps `passed`, `failed`, `skipped`, `pending`, `leaked` and `quota_leaked` counters in `test_state_t` ([`test.h`](../../include/kernel/test/test.h)). [`test.sh`](../../scripts/test.sh) parses the summary and fails the run when a leak counter is nonzero, but there is no counter for unexpected log output, because nothing marks a `[WARN]` or `[FAIL]` line as one a test deliberately provoked. Classifying a line still means reading the code that emitted it.

[`klog.c`](../../src/kernel/klog.c) defines the badges a reader sees: `LOG_INFO` prints `[ OK ]`, `LOG_WARN` prints `[WARN]`, `LOG_ERROR` prints `[FAIL]`, and `LOG_FATAL` prints `[CRIT]`. [`boot_perf_budget.c`](../../src/kernel/boot_perf_budget.c) reports a missed boot timing budget under the `BOOT-BUDGET` tag at `LOG_ERROR` or `LOG_WARN`, and nothing gates on those lines. [`test-smoke.sh`](../../scripts/test-smoke.sh) passes a boot on its `PASS_PATTERNS_ALL` markers and aborts early on the strings in `FAIL_PATTERNS`. After the boot it also checks every `[FAIL]` line in its own log against [`log-baseline.txt`](../../scripts/log-baseline.txt), a list of known-correct output with an owner named for each entry, and fails the run on any line not in it. `BOOT-BUDGET:` is baselined, which is why a missed timing budget does not fail a smoke run. The same file is reserved for the kernel test runner's future gate, so the two cannot drift apart.

The shipped piece is in [`dpc.c`](../../src/kernel/sched/dpc.c). The DPC watchdog keeps an unlocked per-CPU table of up to `DPC_WD_TRACKED_ROUTINES` (8) offending routines; the first `DPC_WD_WARN_BURST` (3) overruns by a routine still print a `LOG_WARN` line each, and later ones are counted silently. At the end of boot, `dpc_watchdog_report()` (called from [`boot_desktop.c`](../../src/kernel/main/boot_desktop.c)) prints a `--- DPC overrun summary ---` block with one line per offending routine, giving its overrun count and worst duration.

## What are its interfaces?

| Interface | Purpose |
| --- | --- |
| `TEST_ASSERT()`, `TEST_SKIP()`, `TEST_PENDING()` | The runner's assertion, skip and pending macros ([`test.h`](../../include/kernel/test/test.h)) |
| `test_state_t` counters | Per-run summary that the shell scripts parse ([`test_runner.c`](../../src/kernel/test/test_runner.c)) |
| `klog(level, tag, fmt, ...)` | Severity-tagged serial output with the `[WARN]`/`[FAIL]`/`[CRIT]` badges ([`klog.c`](../../src/kernel/klog.c)) |
| `dpc_watchdog_report()` | End-of-boot DPC overrun roll-up, the one shipped piece ([`dpc.c`](../../src/kernel/sched/dpc.c)) |
| `FAIL_PATTERNS`, `PASS_PATTERNS_ALL`, `UNEXPECTED_FAILS` | The smoke test's early-abort strings, pass markers, and unbaselined `[FAIL]` lines ([`test-smoke.sh`](../../scripts/test-smoke.sh)) |
| [`scripts/log-baseline.txt`](../../scripts/log-baseline.txt) | Known-correct `[FAIL]`/`[WARN]` substrings, one owner per entry |

## How do I use it?

Run the suites as usual:

```bash
bash scripts/test.sh          # fails on leaked/quota-leaked, not on unexpected [WARN]/[FAIL]
bash scripts/test-smoke.sh    # fails on FAIL_PATTERNS or on any [FAIL] not in log-baseline.txt
```

To see the shipped rate limiting, look for the `DPC overrun summary` block near the end of a serial capture from a platform where the DPC timing watchdog is armed (it needs a calibrated invariant TSC).

## What is not implemented yet?

- **Declaring expected output where it is emitted.** No `TEST_EXPECT_LOG` exists ([Mark Expected Test-Path Log Output at Emission](../../todo/02-kernel-core/TODO-34-serial-log-signal-to-noise.md#1-mark-expected-test-path-log-output-at-emission)).
- **A cleanliness gate in the test runner.** There is no unexpected-output counter for `test.sh` to read ([Log-Cleanliness Gate in the Kernel Test Runner](../../todo/02-kernel-core/TODO-34-serial-log-signal-to-noise.md#2-log-cleanliness-gate-in-the-kernel-test-runner)).
- **Reporting disabled checks as disabled.** A run where the DPC watchdog could not time anything looks the same as one where it found no overruns ([Surface the Platform Blind Spots in the Run Summary](../../todo/02-kernel-core/TODO-34-serial-log-signal-to-noise.md#4-surface-the-platform-blind-spots-in-the-run-summary)).
- **The rest of the rate-limiting sweep.** Other per-event warnings on hot paths have not been given the DPC treatment ([Rate-Limit the Repeat-Offender Warnings That Produce 86% of the Volume](../../todo/02-kernel-core/TODO-34-serial-log-signal-to-noise.md#5-rate-limit-the-repeat-offender-warnings-that-produce-86-of-the-volume)).

## How does it compare with Windows 11 and Linux?

Following the roadmap's comparison table, neither Windows 11 nor Linux lets a test declare expected diagnostic output where it is emitted, and neither separates "check disabled" from "check passed" in a summary; Windows relies on post-run HLK log scans and Linux kernel selftests compare `dmesg` output after the fact. The plan here is a runner-level gate that attributes unexpected output to the suite that produced it. Today the smoke test gates on unbaselined `[FAIL]` lines and the DPC watchdog is rate-limited; the runner-level gate does not exist yet.

## See also

- [Serial Log Signal-to-Noise roadmap](../../todo/02-kernel-core/TODO-34-serial-log-signal-to-noise.md)
- [System Logging (klog)](system-logging.md)
- [IRQL, DPCs and APCs](irql-dpc.md)
- [Kernel Logging v2](kernel-logging-v2.md)
