# User-Mode Test Environment Matrix

Canonical platform-expectation matrix for the user-mode test framework.
Written 2026-04-20 alongside the manifest + timeouts + TAP + SKIP work;
kept in sync with CLAUDE.md's *Bare Metal Gotchas* list and the
*Testing -- Category-Based Test Infrastructure* section.

The launcher itself lives in [src/kernel/test/test_usermode.c](../../src/kernel/test/test_usermode.c).
The user-side harness header is [user/include/test.h](../../user/include/test.h).
Per-binary deploy happens via the Makefile's `userland` target.

Governing roadmap: [TODO-04 -- User-Mode Test Framework](../../todo/00-infrastructure/TODO-04-usermode-test-framework.md).

---

## Platforms we target

| Platform        | Accel     | Primary role                                   | Typical overhead |
|-----------------|-----------|------------------------------------------------|------------------|
| Bare metal      | real CPU  | Authoritative -- ship gate                     | 1.0x (reference) |
| QEMU WHPX       | Hyper-V   | Windows-host regression gate                   | ~1.1x            |
| QEMU TCG        | software  | Device-emulation regression gate (NVMe, USB)   | ~8-12x           |
| QEMU KVM (WSL2) | Linux KVM | Fast iteration, local CI hook, smoke test      | ~1.2x            |
| VirtualBox NEM  | Hyper-V   | Secondary Windows-host cross-check             | ~1.3x            |

Bare metal is the target; every other row is an approximation. If
behavior diverges between rows, the bare-metal reading wins.

---

## User-mode test expectations per platform

| Expectation / feature             | Bare | WHPX | TCG  | KVM  | VBox | Notes                                       |
|-----------------------------------|:----:|:----:|:----:|:----:|:----:|---------------------------------------------|
| `test_*.exe` spawn + exit         |  x   |  x   |  x   |  x   |  x   | Core launcher spawn-and-wait path           |
| Manifest-driven order             |  x   |  x   |  x   |  x   |  x   | Deterministic on every platform             |
| Per-binary wall-clock timeout     |  x   |  x   |  x*  |  x   |  x   | *Default 10 s may trip under TCG device load |
| TAP `1..N` / `ok N` / `not ok N`  |  x   |  x   |  x   |  x   |  x   | Line format is platform-independent         |
| SKIP on `exit=77`                 |  x   |  x   |  x   |  x   |  x   | kselftest convention                        |
| `utest_filter=<glob>`             |  x   |  x   |  x   |  x   |  x   | Literal or single `*` wildcard              |
| ELF loader                        |  x   |  x   |  x   |  x   |  x   | Baseline format                             |
| PE32+ loader                      |  x   |  x   |  x   |  x   |  x   | Landing with the binary-format coverage section |
| EIF loader                        |  x   |  x   |  x   |  x   |  x   | Landing with the binary-format coverage section |
| Preemptive watchdog kill          |  x   |  x   |  x   |  x   |  x   | Needs timer IRQ + IDT -- works every platform |

No "TCG-only" or "WHPX-only" features. Any platform-specific skip must
be justified here.

---

## Known-flaky patterns and how to handle them

Linux kselftest and LKFT long ago learned that a test-runner needs
documented escape hatches, not a "make it pass or delete it" policy.
Four classes of known flakiness and the correct response:

### 1. NVMe / Storage-heavy tests on WHPX
WHPX processes NVMe doorbell writes through its event loop, so heavy
I/O bursts can spike into seconds of tail latency. Don't tighten the
default 10 s timeout for storage-heavy binaries; if a binary needs
headroom, set `utest_timeout_ms=30000` in boot.conf and document the
reason in its source file. Canonical write-up: [CLAUDE.md](../../CLAUDE.md)
-> *NVMe I/O unreliable on QEMU WHPX*.

**Raising `utest_timeout_ms` means raising the harness bound too.** Every
launcher wait carries a clock watchdog that escapes when the monotonic
clock stops advancing, and it is deliberately set to expire LATER than
the deadline it backs. It is NOT clamped to fit the harness, and must not
be: a bound below the configured deadline would end healthy tests on
exactly the slow configurations the override exists for.

How late it lands depends on whether the kernel has a trusted TSC rate,
and the two cases differ by more than an order of magnitude:

| TSC rate | Conversion | Default 10 s timeout | 30 s timeout |
|---|---|---|---|
| Trusted (`mono_tsc_hz()` non-zero) | measured, as measured; ~2x the timeout | ~20 s | ~60 s |
| Untrusted (never qualified, or drift-demoted) | `MONO_TSC_HZ_MAX` = 100 GHz; ~2x the timeout scaled by (100 GHz / real rate) | ~660 s on a 3 GHz part | ~2000 s |

`scripts/test.sh` bounds the whole boot at 60 s by default. So on the
trusted path, pair a 30 s per-binary timeout with `TIMEOUT=120 bash
scripts/test.sh`. **On the untrusted path the escape is always later than
any reasonable harness bound**, and a stall there surfaces as the runner's
generic no-summary timeout rather than as the named `[UTEST-WAIT-STALLED]`
record. The escape still terminates the guest-side wait; its diagnosis is
what does not reach the artifacts. Narrowing that would require the rate
the platform does not have, and firing early on a healthy fast TSC is the
worse error, so the bound stays true rather than convenient. A run that
demotes its clocksource is therefore also a run whose stall diagnosis is
degraded.

### 2. Timer-sensitive tests on TCG
TCG is ~10x slower than real hardware. Wall-clock assertions like
"sleep 100 ms should return in [99, 110] ms" will miss the upper
bound under TCG contention. Tests should assert "at least N ms
elapsed" (lower bound only) or `TEST_SKIP` with `SYS_UPTIME`-based
detection of a slow timer. Never widen both ends of a timing range
to "accept TCG" -- see CLAUDE.md *"Never paper over test failures with
platform workarounds"*.

### 3. First-boot cold-cache timings
First launch of a binary requires ELF parse + page fault cascade.
Repeatable timings measure the second invocation or later. Future
[Test Type Taxonomy](../../todo/00-infrastructure/TODO-04-usermode-test-framework.md#8-test-type-taxonomy-smoke--correctness--stress--perf)
perf binaries should run each measurement twice and discard the first
sample.

### 4. Optional hardware
NVMe, PCIe NICs, USB input on a given platform: `TEST_SKIP` (exit
`UTEST_EXIT_SKIP` = 77) when the device isn't present rather than
FAIL. Example: `test_storage_nvme.exe` SKIPs on VirtualBox because
VBox doesn't expose NVMe at all.

---

## What NOT to do

- **Do not add `accept two values` assertions** to make a flaky test
  pass on one emulator. Diagnose the underlying divergence. If the
  divergence is platform-fundamental, `TEST_SKIP` with a documented
  reason. This rule is load-bearing -- CLAUDE.md *"Never paper over
  test failures with platform workarounds"* has a 2026-04-13 incident
  report attached.
- **Do not silence the launcher's timeout log.** A timeout is
  evidence of either (a) a genuine regression the test caught or (b)
  an overloaded platform we should document here. Investigate; don't
  just bump the timeout to make it go away.
- **Do not remove a binary from the manifest** because it's flaky
  on one platform. SKIP it conditionally with `UTEST_EXIT_SKIP` or
  use `utest_filter` to exclude it locally. The manifest drives
  intent; the launcher applies policy.

---

## Cross-references

- **Launcher implementation:** [src/kernel/test/test_usermode.c](../../src/kernel/test/test_usermode.c)
- **Public API contract:** [include/kernel/test/test_usermode.h](../../include/kernel/test/test_usermode.h)
- **User-side harness:** [user/include/test.h](../../user/include/test.h)
- **Governing TODO:** [TODO-04 -- User-Mode Test Framework](../../todo/00-infrastructure/TODO-04-usermode-test-framework.md)
- **Bare-metal gotchas (authoritative):** [CLAUDE.md](../../CLAUDE.md) -- *Bare Metal Gotchas*
- **Kernel test category infrastructure:** [TODO-03 -- Kernel Test Harness](../../todo/00-infrastructure/TODO-03-kernel-test-harness.md)
- **[Binary format loader coverage](../../todo/00-infrastructure/TODO-04-usermode-test-framework.md#15-binary-format-loader-coverage-elf--pe32--eif):** `test_loader_elf.exe` / `test_loader_pe.exe` / `test_loader_eif.exe`
