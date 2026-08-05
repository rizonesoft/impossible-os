---
schema_version: 1
id: time-filetime-management
domain: 02-kernel-core
status: active
title: "TODO-08 -- Time & FILETIME Management"
---

# TODO-08 -- Time & FILETIME Management

> **Validated:** 2026-06-26 | validate-todo-file clean (structure / IO table / XREF / test wiring)
> **Gap-audited:** 2026-06-26 | gap-audit + codex-gap-audit (4 HIGH); filed: §2 ACPI-PM/watchdog, §3 ordered-rdtsc + AP-TSC-sync, §12 KUSD QPC fields + ABI-collision, §17 NTP-QPC-independence, §14 RTC-wake XREF; §2/§3/§12 downgraded [x]->[/]

> **Goal:** Build the kernel time layer that all Win32 APIs, filesystems, logging, and the scheduler depend on. This means: a `FILETIME` type with correct epoch and 100 ns resolution; a monotonic nanosecond clock backed by invariant TSC or HPET; interrupt time and unbiased interrupt time APIs; a wall clock seeded from UEFI `GetTime()` (RTC fallback); precise sub-microsecond system time via TSC interpolation; `NtQuerySystemTime`/`NtSetSystemTime`; `QueryPerformanceCounter`; timer resolution management (`NtSetTimerResolution`); timezone/DST bias; filesystem timestamp encoders (FAT32 local-time / NTFS FILETIME); suspend/hibernate time bias tracking; and KUSER_SHARED_DATA time field updates. Without this, every `CreateFile` timestamp, every log entry, and every scheduler deadline is either zero or fabricated.

> [!IMPORTANT]
> **Current state:** `timer.c` provides `sleep_ms()`, `system_get_ticks()`, and `uptime()` in whole seconds. `boot_timing_tsc_freq()` gives the bootloader-measured TSC frequency. `uefi_get_time()` can return wall time with nanosecond precision and timezone data. `acpi_get_hpet_base()` returns the HPET MMIO base. `rtc_read()` gives CMOS date/time as a fallback. None of these are connected into a unified time service and there is no `FILETIME`, no wall clock, and no `QueryPerformanceCounter`.

## Inputs

- [`include/kernel/timer.h`](../../include/kernel/timer.h), [`src/kernel/timer.c`](../../src/kernel/timer.c)
- [`include/kernel/drivers/rtc.h`](../../include/kernel/drivers/rtc.h), [`src/kernel/drivers/rtc.c`](../../src/kernel/drivers/rtc.c)
- [`include/kernel/uefi_runtime.h`](../../include/kernel/uefi_runtime.h) -- `efi_time`, `uefi_get_time()`, `uefi_set_time()`
- [`include/kernel/boot_timing.h`](../../include/kernel/boot_timing.h) -- `boot_timing_tsc_freq()`
- [`include/kernel/acpi.h`](../../include/kernel/acpi.h) -- `acpi_get_hpet_base()`
- [`include/kernel/drivers/lapic.h`](../../include/kernel/drivers/lapic.h) -- calibrated ticks/ms
- [`include/kernel/cpuid.h`](../../include/kernel/cpuid.h) -- `CPU_FEATURE_RDTSCP`, invariant TSC detection
- [`src/kernel/fs/fat32/fat32_ops.c`](../../src/kernel/fs/fat32/fat32_ops.c) -- current FAT32 timestamp callsites
- [`src/kernel/fs/ntfs/ntfs_metadata.c`](../../src/kernel/fs/ntfs/ntfs_metadata.c) -- NTFS timestamp callsites
- → XREF: `TODO-14-registry-completion.md §1` -- registry `LastWriteTime` conversion from PIT ticks to FILETIME uses this file's FILETIME conversion math (§1)
- [`src/kernel/fs/ntfs/ntfs_data_write.c`](../../src/kernel/fs/ntfs/ntfs_data_write.c) -- NTFS data write timestamp callsites
- → XREF: `TODO-01-kernel-init-sequencing.md §4` -- time service init (`wall_clock_init()`) belongs in Phase 2 (§4) after UEFI runtime; NTP wall clock adjustment belongs in Phase 3 (§5); `wall_clock_init()` is not yet listed in §4's checklist -- add before implementing
- → XREF: `TODO-12-native-api-ssdt.md §5` -- SSDT registration; time syscalls (`NtQuerySystemTime`, `NtSetSystemTime`, `NtQueryPerformanceCounter`) in §9, timer resolution (`NtSetTimerResolution`, `NtQueryTimerResolution`) in §7
- → XREF: `TODO-07-irql-model-dpcs.md §7` -- Timer/APIC scheduling path (§7) drives monotonic tick accumulation via `KiDispatchDpc()`; `DISPATCH_LEVEL` clock interrupt is the tick source
- → XREF: `TODO-07-irql-model-dpcs.md §6` -- Timer-DPC association: KTIMER objects carry an optional KDPC pointer; when the timer fires, the DPC is auto-queued via `KeInsertQueueDpc`
- → XREF: `TODO-11-peb-teb-user-abi.md §11` -- KUSER_SHARED_DATA time fields (SystemTime, InterruptTime, QpcFrequency) are populated by §12 of this TODO; `kusd_update_time()` is called from the timer ISR
- → XREF: `TODO-26-power-management.md §3,§4` -- S3/S4 resume path calls `ke_suspend_bias_update()` (§14 of this TODO) to adjust `InterruptTimeBias` by the sleep duration
- → XREF: `TODO-12-native-api-ssdt.md §19` -- `NtSetTimerResolution` (SSDT 0x00F4) is implemented in §7 of this TODO

## Outcome

- `FILETIME` is a first-class kernel type: 100 ns intervals since `1601-01-01T00:00:00Z`.
- `KeQuerySystemTime()` returns UTC wall time as a `FILETIME` from anywhere in the kernel.
- `KeQuerySystemTimePrecise()` returns sub-microsecond UTC wall time via TSC interpolation.
- `KeQueryPerformanceCounter()` returns a monotonic 64-bit counter backed by invariant TSC (HPET fallback, LAPIC last resort).
- `KeQueryInterruptTime()` / `KeQueryUnbiasedInterruptTime()` provide interrupt-time APIs matching the Windows contract (with and without suspend bias).
- Per-CPU TSC offset correction is applied on SMP so `RDTSC` reads are coherent across cores.
- The wall clock is seeded from UEFI `GetTime()` at boot, or CMOS RTC if UEFI time is unavailable.
- `NtQuerySystemTime`, `NtSetSystemTime`, `NtQueryPerformanceCounter`, `NtSetTimerResolution`, and `NtQueryTimerResolution` are implemented and registered in the SSDT.
- Timer resolution is controllable per-process (0.5–15.625 ms range), matching the Win11 behavior.
- The timer ISR updates `KUSER_SHARED_DATA` time fields (SystemTime, InterruptTime, TickCount) so user-mode reads need no syscall.
- FAT32 writes use the correctly computed DOS date/time in local time.
- NTFS writes use the correctly computed FILETIME in UTC.
- Timezone bias and DST adjustment are configurable and applied by `GetLocalTime()` and FAT32.
- Suspend/hibernate time bias is tracked so `InterruptTime` remains correct across S3/S4 transitions.
- NTP adjustment hooks exist so a future network time client can correct the wall clock and frequency.
- Leap seconds are explicitly not counted in FILETIME (matching Windows behavior), with a documented policy.

## Implementation Order

| ⭐  | Order | Deliverable                              | Depends On          | Status |
| --- | :---: | ---------------------------------------- | ------------------- | :----: |
| 💎  |   1   | `FILETIME` type, epoch constants, and conversion math | --                  |  [x]   |
| 💎  |   2   | Monotonic nanosecond clock source selection | §1                  |  [x]   |
| 💎  |   3   | Invariant TSC detection and per-CPU offset calibration | §2                  |  [/]   |
| 💎  |   4   | HPET standalone driver                   | --                  |  [x]   |
| 💎  |   5   | Wall clock init from UEFI GetTime / RTC  | §1, §2              |  [x]   |
| 💎  |   6   | Kernel time service (`KeQuerySystemTime`, `KeSetSystemTime`) | §5                  |  [x]   |
| 💎  |   7   | Interrupt time and unbiased interrupt time APIs | §2, §6              |  [x]   |
| 💎  |   8   | Timer resolution management (`NtSetTimerResolution`) | §6, TODO-12 §5      |  [x]   |
| 💎  |   9   | `NtQuerySystemTime` / `NtSetSystemTime` / `NtQueryPerformanceCounter` | §6, TODO-12 §5      |  [x]   |
| 💎  |  10   | Precise system time (`KeQuerySystemTimePrecise`) | §6, §2              |  [x]   |
| 💎  |  11   | Timezone bias and DST management         | §6                  |  [x]   |
| 💎  |  12   | KUSER_SHARED_DATA time field updates from timer ISR | §6, §7, TODO-11 §11 |  [/]   |
| 💎  |  13   | Filesystem timestamp encoding (FAT32 + NTFS) | §6, §11             |  [x]   |
| 💎  |  14   | Suspend/hibernate time bias tracking     | §7, TODO-26 §3,§4   |  [/]   |
| 💎  |  15   | Leap second policy                       | §1                  |  [x]   |
| ⭐  |  16   | Coarse time fast path (lock-free cached time) | §6                  |  [x]   |
| ⭐  |  17   | NTP clock adjustment hooks               | §6                  |  [/]   |
| 💎  |  18   | Clocksource quality watchdog (drift demotion) | §2, §3              |  [/]   |
| 💎  |  19   | NTP continuous wall-time discipline      | §17, §18            |  [x]   |

> 💎 = parity -- Windows NT and Linux both provide these capabilities.
> ⭐ = exclusive -- coarse time gives O(1) cached reads for hot-path callers without reading hardware; NTP hook API is a first-class kernel-level adjustment interface, not a userspace-only workaround.

---

## 1. `FILETIME` Type, Epoch Constants, and Conversion Math
- [x] Created `include/kernel/nt/filetime.h` with `typedef uint64_t FILETIME`, epoch constants, tick constants, `FILETIME_NOW_PLACEHOLDER`
- [x] Inline helpers: `filetime_from_unix_seconds()`, `filetime_to_unix_seconds()`, `filetime_days_from_date()`, `filetime_to_dos_datetime()`, `filetime_from_dos_datetime()`
- [x] Non-inline (in `src/kernel/nt/filetime.c`): `filetime_from_rtc()`, `filetime_from_efi_time()` -- need full struct definitions
- [x] `FILETIME_NOW_PLACEHOLDER = 0` sentinel defined
- [x] 7 unit tests: epoch offset, round-trip, DOS date round-trip, ticks/sec constants, pre-epoch guard, DOS low clamp, days year guard
- [x] Commit: `"kernel: nt -- FILETIME type, epoch constants, and conversion math"`

> **Verified:** 2026-04-14 -- `include/kernel/nt/filetime.h` and `src/kernel/nt/filetime.c` confirmed at HEAD; all 7 unit tests registered in `test_nt_types.c` under `TEST_CAT_ABI`; build clean (`=== BUILD OK ===`). Epoch constants verified: 11644473600 s = 134,774 days x 86400. DOS encode/decode round-trips correctly. `filetime_to_string` used in klog_disk.c and wer.c. No dead exports.
> **Quality reviewed:** 2026-04-14 -- Two Codex dispatches (adversarial + dead-code/consistency/perf). Four findings fixed: (1) `filetime_to_unix_seconds` pre-Unix-epoch unsigned underflow -- guard added (`ft < FILETIME_EPOCH_OFFSET_100NS` returns 0); (2) `filetime_to_dos_datetime` negative `local_ticks` wrapping -- saturation to DOS min on `local_ticks <= 0`; (3) `filetime_days_from_date_fn` year < 1601 wraps `uint64_t y = year - 1601` -- guard added; (4) DOS clamp was year-only, leaving month/day/time from wrong year -- full tuple saturation on year bounds. Accepted LOW: NULL deref on ptr args (kernel-internal callers hold non-NULL by invariant); `filetime_from_unix_seconds` overflow for unix_sec >= 1.8e12 (year 60,000+, no actionable risk). No dead code. No perf concerns (calendar loop ~425 iterations, negligible vs FAT32 I/O). **Test gap:** `filetime_from_rtc`, `filetime_from_efi_time`, `filetime_to_string` not exercised in unit tests -- covered by full `test_time.c` in Unit Tests section.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi), 7 FILETIME suites, 0 failures expected

---

## 2. Monotonic Nanosecond Clock Source Selection
Select the best monotonic source available: invariant TSC → HPET → ACPI PMTMR → LAPIC counter.

- [x] Created `include/kernel/time/mono_clock.h` + `src/kernel/time/mono_clock.c`: `mono_ns()`, `mono_filetime_units()`, `mono_clock_source_name()`, `mono_clock_source_id()`
- [x] Selection: invariant TSC (CPU_FEATURE_TSC_INV + boot_timing_tsc_freq) -> LAPIC fallback (system_get_ticks). HPET stub ready for §4.
- [x] Scale factor: num/den pair for integer multiply without hot-path division; overflow-safe split for TSC
- [x] Logs selected source: `"Monotonic clock: TSC (N MHz, invariant)"` or `"LAPIC (N ticks/ms)"`
- [x] Commit: `"kernel: time -- monotonic nanosecond clock source selection"`
- [x] ACPI PM timer (PMTMR) clocksource before LAPIC (gap-audit): `MONO_SRC_PMTMR` priority-3 (3.579545 MHz) in `mono_clock_init`; LAPIC is a per-CPU interrupt counter, not a standalone counter (watchdog split to §18).
- [x] Wrap-safe 64-bit PMTMR epoch (design review [H]): seqlock `s_pmtmr_epoch_ns`/`last_raw` advanced from the BSP timer ISR every 8 ticks (far under the ~4.7 s 24-bit wrap); `mono_ns()` is a lock-free seqlock reader via `mono_pmtmr_delta_ns()`.
- [x] Monotonicity guards (adversarial [H]x2 + [M]): `mono_ns()` clamps through a global atomic floor (`pmtmr_mono_floor`) so a glitch/missed-wrap never steps backward; `mono_clock_init` seeds the epoch + RELEASE-publishes `s_source` last.
- [x] Centralize glitch + cheap hot path: median-of-3 `acpi_pmtimer_read_value()` (precise `mono_ns`/csprng/advance); the `read_ns`/`uptime_ns` path uses cached `mono_ns_coarse()` (no port I/O); `lapic.c` cal-loop refactor rejected.

**Test checkpoint:** Unit (`TEST_CAT_SCHED` in `test_time.c`): `mono_pmtmr_delta_ns` converts 3 579 545 counts to 1 s, extends across the 24-bit wrap (1 tick ~279 ns) and half-range ~2.34 s; `mono_lapic_ticks_to_ns` scaling. Runtime (WHPX / bare metal serial): on a no-TSC/no-HPET platform the boot log shows `"Monotonic clock: PMTMR (3.579545 MHz, 24-bit)"`, `mono_ns()` advances monotonically, and `sys_yield` stays under budget. Verify on QEMU TCG, WHPX, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | 3 time suites + sched, 0 failures (208 kernel + 16 user PASS, TCG)
> **Notes:**
> - Shipped: `MONO_SRC_PMTMR` in `mono_clock.{c,h}` (TSC>HPET>PMTMR>LAPIC), a seqlock 64-bit PMTMR epoch (`mono_pmtmr_delta_ns` wrap-extend) advanced from the BSP timer ISR, plus `acpi_pmtimer_read_raw()` + median-of-3 `acpi_pmtimer_read_value()`.
> - How it runs: `mono_clock_init` picks PMTMR when no invariant TSC + no HPET; the LAPIC/PIT timer ISR calls `mono_clock_pmtmr_advance()` (no-op unless PMTMR active); `mono_ns()` reads epoch + a single live sample lock-free.
> - Downstream: the `read_ns`/`uptime_ns` hot path uses cached `mono_ns_coarse()` (`sys_yield` 25k cycles); precise `mono_ns()` is median + monotonic-floor. Codex design (4) + adversarial (3 monotonicity) adoptions in the commit.
> - Canonical: `src/kernel/time/mono_clock.c`.
> - Scope boundary: §2 owns source selection + the PMTMR clocksource; the drift watchdog is §18; ordered-`rdtsc` / AP-TSC-sync is §3.
> **Verified:** 2026-06-27 | commit `54345492` | 8/8 items | build OK | 208 kernel + 16 user PASS | smoke PASS (TCG 2.6s)
> **Accepted:** [H] a multi-second tick quiesce can lose 24-bit PMTMR wraps (the monotonic floor blocks backward steps meanwhile) -> XREF: 02-kernel-core/TODO-08-time-filetime-management.md §18 (item: "PMTMR epoch refresh across tick quiesce" at line 334)
> **Quality reviewed:** 2026-06-27 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 2H+2M fixed, 1H accepted (per-tick PMTMR-read deferral resolved in §16: read retained, amortized) | scope: kernel-code-quality

---

## 3. Invariant TSC Detection and Per-CPU Offset Calibration
On systems with invariant TSC (`CPUID 0x80000007 EDX[8]`), the TSC ticks at a constant rate regardless of C-states or frequency scaling, and is synchronized by the firmware across all cores at reset. Verify this and apply correction offsets where needed.

- [x] Invariant TSC detected via `CPU_FEATURE_TSC_INV` (already in cpuid.c); `mono_clock_init()` uses it for source selection
- [x] CPUID 0x15 TSC/crystal cross-check: `mono_clock_crosscheck_tsc()` compares CPUID value with `boot_timing_tsc_freq()`; logs OK or warns if >1% mismatch
- [x] Per-CPU `tsc_offset` field added to `per_cpu_data` struct; `mono_ns()` and `rdtsc_ns()` apply offset on AP cores
- [x] Per-AP TSC sync via IPI: deferred -- modern firmware synchronizes invariant TSC at reset; offset starts at 0; IPI sync will be added if real hardware shows drift
- [x] Fallback: if invariant TSC absent, falls through to LAPIC in `mono_clock_init()` with log warning
- [x] `rdtsc_ns()`: fast TSC read with per-CPU offset + overflow-safe scale conversion
> [!NOTE]
> **Design review (adopted, pre-code).** (1) Ordered read: a single `rdtsc_ordered()` helper -- `RDTSCP` gated on `CPU_FEATURE_RDTSCP` (capture/clobber ECX TSC_AUX), else `LFENCE;RDTSC` -- used by `rdtsc_ns()` AND the `mono_ns()` TSC branch. (2) AP sync: a BSP/AP rendezvous BEFORE the AP publishes `is_online` (shared per-AP state, bounded ping-pong samples, tolerance from round-trip latency, NO AP-side serial/logging, timeout; demotion completes before the AP services interrupts). (3) Demotion + migration: an atomic clocksource descriptor (source+scale+epoch+generation, release-published, seeded from the current mono value) PLUS generalizing §2's `pmtmr_mono_floor` into a MONO-WIDE atomic floor over TSC/HPET/PMTMR/LAPIC/coarse so demote + migration never step backward -- this machinery is SHARED with the §18 watchdog demotion. S3 resume re-verify is BLOCKED on power-management.

- [x] Ordered TSC read: `rdtsc_ordered()` -- `RDTSCP` gated on `cpu_feature_global_mask()` (the all-online intersection, NOT `cpu_has()`, so a feature-skewed AP cannot #UD), else `LFENCE;RDTSC` -- used by `rdtsc_ns()` + the `mono_ns()` TSC branch.
- [ ] AP TSC sync + warp detection (design review [H]): BSP/AP rendezvous before `is_online` -- bounded ping-pong, latency tolerance, no AP logging, timeout (Linux `tsc_sync.c` parity).
- [x] Atomic demote-on-warp + mono-wide floor (design review [H], shipped in §18): immutable per-source `mono_desc` table + atomic `s_active_src` index + mono-wide `mono_floor()` so demote/migration never step backward.
- [ ] Resume monotonicity -- BLOCKED on power-management S3; reuses this section's sync/demote machinery. -> XREF: 02-kernel-core/TODO-26-power-management.md §3
- [x] Commit: `"kernel: time -- invariant TSC detection and per-CPU offset calibration"`

**Test checkpoint:** Unit: existing `test_time.c` mono/PMTMR suites still pass (the ordered read changes only ordering, not values; RDTSCP/LFENCE serialization is not directly unit-testable). Runtime (WHPX / bare metal serial): `mono_ns()` advances monotonically; on invariant-TSC SMP no QPC backward step across thread migration. Verify on QEMU WHPX SMP, TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | time + sched suites, 0 failures (208 kernel + 16 user PASS, TCG)
> **Notes:**
> - Shipped: `rdtsc_ordered()` in `mono_clock.c` (RDTSCP gated on the all-online `cpu_feature_global_mask()`, else `LFENCE;RDTSC`), replacing bare `rdtsc` for `rdtsc_ns()` + the `mono_ns()` TSC branch.
> - How it runs: every TSC read serializes against prior instructions so a monotonic/QPC read never samples earlier than its surrounding code; the global-mask gate (not `cpu_has`) avoids an #UD on a feature-skewed AP.
> - Downstream: the atomic-descriptor demotion + mono-wide floor shipped in the §18 watchdog; AP TSC sync rendezvous remains the one deferred item. Codex adversarial adoption (RDTSCP AP-skew #UD) in the commit.
> - Canonical: `src/kernel/time/mono_clock.c`.
> - Scope boundary: §3 ships the ordered read; AP sync + demotion machinery is the deferred items (shared with §18); resume re-verify is power-management.
> **Verified:** 2026-06-27 | commit `13f02ffe` | 7/10 items | build OK | 208 kernel + 16 user PASS | smoke PASS (TCG)
> **Deferred:** [H] AP TSC sync rendezvous -- BSP/AP bounded ping-pong (Linux `tsc_sync.c` parity); the atomic-descriptor + mono-wide-floor portion shipped in §18 -> XREF: 02-kernel-core/TODO-08-time-filetime-management.md §3 (item: "AP TSC sync + warp detection" at line 144)
> **Deferred:** [M] resume TSC re-verify blocked on power-management S3 -> XREF: 02-kernel-core/TODO-26-power-management.md §3
> **Quality reviewed:** 2026-06-27 | Codex 2x (design, adversarial) | 1H fixed (RDTSCP AP-skew gate), 2H+1M deferred | scope: kernel-code-quality

---

## 4. HPET Standalone Driver
The HPET provides a single 64-bit main counter that increments at a fixed frequency (typically 14–100 MHz). Used as the QPC fallback source when invariant TSC is absent.

- [x] Created `include/kernel/drivers/hpet.h` + `src/kernel/drivers/hpet.c`: `hpet_init()`, `hpet_available()`, `hpet_frequency_hz()`, `hpet_read_counter()`, `hpet_ns()`
- [x] Maps HPET MMIO as UC via `vmm_map_mmio_uc()`; reads `GCAP_ID` for period (fs/tick); computes frequency
- [x] Enables main counter via `ENABLE_CNF` bit; validates probe read and period range
- [x] Wired into `mono_clock_init()` priority 2 (TSC > HPET > PMTMR > LAPIC); `hpet_init()` is now CALLED in `boot_storage.c` before it (gated on `acpi_is_ready()`) -- the review found it was never invoked, so HPET was unreachable.
- [x] `hpet_ns()` overflow-safe (review): split the `ticks * period_fs / 1e6` so the intermediate cannot overflow u64 (it did after ~5 h on the lifetime counter).
- [x] No timer comparators -- main counter only (comparators for scheduler timer TODO)
- [x] Commit: `"kernel: drivers -- HPET main counter driver"`

**Test checkpoint:** Unit: existing `test_time.c` mono suites pass. Runtime (WHPX / bare metal serial): on a no-invariant-TSC box with an HPET, the boot log shows `hpet: HPET: N MHz ... counter enabled` then `Monotonic clock: HPET (N MHz)` (confirmed in smoke: HPET 100 MHz selected); `mono_ns()` advances monotonically for hours (no overflow). Verify on QEMU WHPX, TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | time + sched suites, 0 failures (208 kernel + 16 user PASS, TCG)
> **Notes:**
> - Shipped earlier: `hpet.{c,h}` standalone driver (`hpet_init`/`hpet_available`/`hpet_frequency_hz`/`hpet_read_counter`/`hpet_ns`), UC-mapped via `vmm_map_mmio_uc`, period from `GCAP_ID`, `ENABLE_CNF`.
> - Review fixes: `hpet_init()` is now actually called in `boot_storage.c` (gated on `acpi_is_ready()` so degraded ACPI falls through to PMTMR/LAPIC) -- it was dead before; `hpet_ns()` is overflow-safe (split division).
> - Downstream: HPET is the QPC/mono fallback when no invariant TSC; the per-tick UC-MMIO read in `kusd_update_time()` is retained (resolved in §16: one read feeds KUSD + the coarse cache, amortized across all coarse readers).
> - Canonical: `src/kernel/drivers/hpet.c`.
> - Scope boundary: §4 owns the main-counter driver; HPET timer comparators (scheduler timer) are out of scope; coarse-read migration is §16.
> **Verified:** 2026-06-27 | commit `5c2ec338` | 6/6 items | build OK | 208 kernel + 16 user PASS | smoke PASS (HPET selected, TCG 2.5s)
> **Quality reviewed:** 2026-06-27 | Codex 4x (adversarial, consistency, perf, re-adversarial x2) | 2H+1M fixed (1H per-tick-read deferral resolved in §16: read retained for KUSD accuracy, amortized) | scope: kernel-code-quality

---

## 5. Wall Clock Init from UEFI GetTime / RTC
Seed the kernel wall clock at boot. The wall clock is a `FILETIME` anchor point paired with the monotonic counter reading at that instant.

- [x] Created `include/kernel/time/wall_clock.h` + `src/kernel/time/wall_clock.c`
- [x] `wall_clock_init()`: tries UEFI GetTime (year 2000-2100 plausibility check), falls back to RTC; logs seeded time with ISO-style format and source name
- [x] `KeQuerySystemTime()`: returns `base_time + (mono_ns() - base_mono_ns) / 100` -- seqlock lock-free read
- [x] `KeSetSystemTime(new_time)`: updates anchor + re-latches mono_ns(); seqlock write-protected
- [x] `wall_clock_ready()`: returns 1 after init
- [x] Protected by `seqlock_t` (SEQLOCK_INIT) for SMP-safe concurrent reads
- [x] CMOS-RTC absence gating: hard gate in `rtc.c` `cmos_read()` + fail-closed `s_rtc_available` latch + status-bearing `rtc_try_read()`/`rtc_time_plausible()`; `wall_clock_init()` + `klog_disk` seed via `rtc_try_read()`. Consumer of `D01 T10 §4`.

> [!NOTE]
> **Design review (adopted, pre-code).** (1) HARD GATE in `rtc.c` `cmos_read()` -- the single `outb 0x70` / `inb 0x71` site is the safety boundary; refuse before ANY port I/O when unavailable, so a missed consumer can never touch the ports (callers include `wall_clock.c:48`, `compositor.c:220`, `desktop.c:584` -- more than the original list). Consumer gates only choose fallback behavior, not hardware safety. (2) FAIL-CLOSED latch: `s_rtc_available` defaults UNAVAILABLE; set true only when `acpi_is_ready() && acpi_has_cmos_rtc()` -- `acpi_has_cmos_rtc()` returns present on `!fadt_ptr`/short-FADT (fail-open), so the latch must add the readiness gate. (3) STATUS-BEARING read: add `rtc_try_read()` returning success + full date-tuple validation; `rtc_read()` (void) zero-fills look like year-2000 (passes `wall_clock`'s 2000-2100 check) and underflow `filetime_from_rtc` on `day-1`. `wall_clock_init()` seeds from RTC only on `rtc_try_read()` success, else stays UEFI-only / unavailable.

- [x] Commit: `"kernel: time -- wall clock init from UEFI GetTime with RTC fallback"`

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | 5 time suites (PMTMR/LAPIC + 2 rtc_time_plausible), 0 failures (221 kernel + 16 user PASS, TCG)
> **Notes:**
> - Shipped: CMOS-absence gating -- `cmos_read()` hard gate (the lone 0x70/0x71 site) + fail-closed `s_rtc_available` latch + status-bearing `rtc_try_read()` + pure `rtc_time_plausible()` validator in `rtc.c`/`rtc.h`.
> - How it runs: latch set by `rtc_probe_availability()` (`acpi_is_ready() && acpi_has_cmos_rtc()`); `rtc_read()` zero-fills a year-0 sentinel when unavailable; `wall_clock_init()` seeds from RTC only on `rtc_try_read()` success.
> - Downstream: `klog_disk` `pick_log_number()` reads via `rtc_try_read()` with a parser-valid 00/01/01 fallback (Codex H: year-0 broke daily sequencing + retention on RTC-less platforms; wall clock not yet seeded there).
> - Canonical: `src/kernel/drivers/rtc.c`; consumer seam `src/kernel/time/wall_clock.c`.
> - Scope boundary: the hard gate is the hardware-safety boundary for all CMOS consumers (compositor/desktop call `rtc_read()` directly, protected, no port I/O); §6+ own the higher-level time service.
> **Verified:** 2026-06-27 | commit `d2930860` | 7/7 items | build OK | 229 kernel + 16 user PASS | smoke PASS (TCG 2.56s, RTC present -> Serial_26062701.log)
> **Accepted:** [M] no-RTC serial-log rotation deletes by seq not recency (cap bounds growth, so not a leak; needs a cross-boot counter) -> XREF: 02-kernel-core/TODO-04-system-logging.md §4 (item: "No-RTC serial-log recency rotation" at line 170)
> **Quality reviewed:** 2026-06-27 | Codex 5x (adversarial, consistency, perf, re-adversarial x2) + kernel-quality-auditor | 2H+2M+1L fixed, 1M accepted-XREF | scope: kernel-code-quality

---

## 6. Kernel Time Service
Provide the stable kernel-level time API used by everything above PASSIVE_LEVEL: filesystems, logging, scheduler, and the native API layer.

- [x] `KeQuerySystemTime()` -- lock-free seqlock read, callable from any IRQL (verified in §5)
- [x] `KeQueryTickCount(tick_count)` -- returns `system_get_ticks()` (timer-tick counter; review fix: was 100 ns units, breaking `tick * increment`)
- [x] `KeQueryTimeIncrement(increment)` -- returns 100000 (10 ms at 100 Hz in 100 ns units)
- [x] `KeDelayExecutionThread(int64_t interval)` -- NT semantics via pure `ke_delay_interval_to_ms()` (negative=relative, positive=absolute), PASSIVE_LEVEL IRQL guard (review fix: was unsigned -> 50-day hang)
- [x] `time_service_ready()` -- returns 1 after `wall_clock_init()`
- [x] Commit: `"kernel: time -- kernel time service API"`

**Test checkpoint:** Unit (`TEST_CAT_SCHED` in `test_time.c`): `ke_delay_interval_to_ms` resolves NT intervals (relative -10ms -> 10ms, sub-ms round-up, zero/expired -> 0, absolute deadline vs now, UINT32_MAX clamp). Runtime (WHPX / bare metal serial): `KeQuerySystemTime()` advances monotonically; `KeQueryTickCount() * KeQueryTimeIncrement()` tracks uptime; a relative `KeDelayExecutionThread` sleeps the requested ms (not 49 days). Verify on QEMU TCG, WHPX, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | time + sched suites, 0 failures (236 kernel + 16 user PASS, TCG)
> **Notes:**
> - Shipped: kernel time service in `wall_clock.c` -- `KeQuerySystemTime` (lock-free seqlock), `KeQueryTickCount`, `KeQueryTimeIncrement`, `KeDelayExecutionThread`, `time_service_ready`.
> - Review fixes: `KeQueryTickCount` -> tick counter; `KeDelayExecutionThread` signed NT interval + PASSIVE_LEVEL guard (was unsigned -> 50-day hang); seqlock seq u64; `s_ready`/`s_time_sourced` atomic; mono backward-clamp.
> - Downstream: absolute deadlines gated on a real wall-clock source + placeholder rejection in `KeSetSystemTime`/`NtSetSystemTime` so a bogus anchor can't re-open the 49-day clamp.
> - Canonical: `src/kernel/time/wall_clock.c`.
> - Scope boundary: §6 owns the Ke* service; the Nt* SSDT handlers' user-pointer probing is §9; the coarse cached-read fast path is §16.
> **Verified:** 2026-06-27 | ship `8ba23931` + review fixes | 6/6 items | build OK | 236 kernel + 16 user PASS | smoke PASS (TCG 2.47s)
> **Accepted:** [H] the three time SSDT handlers deref user pointers without probe/copy (a `prev_ptr` can fault the kernel or write to a kernel address) -> XREF: 02-kernel-core/TODO-08-time-filetime-management.md §9 (item: "User-pointer safety" at line 267)
> **Quality reviewed:** 2026-06-27 | Codex 7x (adversarial, consistency, perf, re-adversarial x4) + kernel-quality-auditor | 3H+3M+2L fixed, 1H accepted-XREF | scope: kernel-code-quality

---

## 7. Interrupt Time and Unbiased Interrupt Time APIs
Windows exposes interrupt time (100 ns since boot, including sleep bias) and unbiased interrupt time (excluding sleep bias) as core kernel APIs. `GetTickCount64()`, scheduler deadlines, and driver timeouts all depend on these. Linux's `CLOCK_BOOTTIME` (includes suspend) vs `CLOCK_MONOTONIC` (excludes suspend) is the equivalent split.

- [x] `KeQueryInterruptTime()` -- returns `mono_filetime_units() + s_interrupt_time_bias` (includes suspend)
- [x] `KeQueryInterruptTimePrecise(qpc_value)` -- same + returns QPC value at same instant
- [x] `KeQueryUnbiasedInterruptTime()` -- returns `mono_filetime_units()` only (no bias)
- [x] `s_interrupt_time_bias` tracked as an atomic counter (`__atomic_fetch_add`/`__atomic_load_n` SEQ_CST); `ke_suspend_bias_update()` for §14 resume path
- [x] Commit: `"kernel: time -- interrupt time and unbiased interrupt time APIs"`

**Test checkpoint:** Unit: covered by the `test_time.c` monotonic helpers (the interrupt-time APIs themselves read the live `mono_filetime_units()` source, not unit-testable without mocking the clock). Runtime (WHPX / bare metal serial): `KeQueryInterruptTime()` >= `KeQueryUnbiasedInterruptTime()`; both advance monotonically; `GetTickCount64()` tracks. Verify on QEMU TCG, WHPX, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | time + sched suites, 0 failures (236 kernel + 16 user PASS, TCG)
> **Notes:**
> - Shipped: interrupt-time APIs in `wall_clock.c` -- `KeQueryInterruptTime` (mono + bias), `KeQueryInterruptTimePrecise` (+ QPC out), `KeQueryUnbiasedInterruptTime` (mono only), `ke_suspend_bias_update`.
> - Review fixes: `KeQueryInterruptTimePrecise` takes ONE mono sample so interrupt-time + QPC share an instant; `s_interrupt_time_bias` read via `__atomic_load_n` (matched to the writer; dropped `volatile`).
> - Downstream: biased vs unbiased split = Win11 InterruptTime/UnbiasedInterruptTime, Linux CLOCK_BOOTTIME/CLOCK_MONOTONIC; the bias is fed by the §14 S3/S4 resume path.
> - Canonical: `src/kernel/time/wall_clock.c`.
> - Scope boundary: §7 owns the interrupt-time APIs; suspend-bias production (resume delta) is §14; the QPC raw source is §2/§9.
> **Verified:** 2026-06-27 | ship `96f4c305` + review fixes | 5/5 items | build OK | 236 kernel + 16 user PASS | smoke PASS (TCG 2.74s)
> **Test gap:** the interrupt-time query APIs read the live monotonic source and are not unit-testable without mocking `mono_ns()`; covered by runtime serial validation.
> **Quality reviewed:** 2026-06-27 | Codex 4x (adversarial, consistency, perf, re-adversarial) + kernel-quality-auditor | 1H+1M fixed | scope: kernel-code-quality

---

## 8. Timer Resolution Management
Windows allows processes to request higher timer interrupt frequency (down to 0.5 ms) via `NtSetTimerResolution` / `timeBeginPeriod`. This affects scheduler quantum, `Sleep()` granularity, and multimedia timing. Win11 scopes this per-process; background/occluded processes don't get elevated resolution.

- [x] `timer_resolution.h`: constants `TIMER_RES_DEFAULT` (156250 = 15.625 ms) and `TIMER_RES_MINIMUM` (5000 = 0.5 ms)
- [x] `KeSetTimerResolution(desired, set, *actual)` -> NTSTATUS: per-process request/release (records pid, releases by owner), clamp-normalized both paths, shortest-wins arbitration, full-slot rollback on refused transition
- [x] `NtSetTimerResolution` (SSDT 0x00F4) + `NtQueryTimerResolution` (SSDT 0x00F3) registered; out-pointers via `ProbeForWriteIfUser` + `copy_to_user` (set probes before mutating)
- [x] 16-slot request table; per-process coalescing (one slot per pid); `arbitrate()` shortest-wins; table-full -> `STATUS_INSUFFICIENT_RESOURCES`
- [x] `KeQueryTimerResolution(max, min, current)` returns all three values
- [x] Per-process tracking: moved to `TODO-21-process-model-extensions.md §14` (Process Exit Cleanup) -- releases timer resolution requests on process exit
- [x] Timer ICR update: `lapic_timer_set_hz()` reprograms LAPIC ICR when resolution changes; called from `arbitrate()` in timer_resolution.c
- [x] Commit: `"kernel: time -- timer resolution management (NtSetTimerResolution)"`

**Test checkpoint:** Unit: the arbitration path uses live task context + `timer_set_tick_hz`, not unit-testable without mocking both. Runtime (WHPX / bare metal serial): `NtSetTimerResolution(5000,TRUE,&a)` raises the LAPIC rate (boot log "Timer resolution: 500 us"), the matching release reverts to 15625 us, a 17th distinct-owner request returns `STATUS_INSUFFICIENT_RESOURCES`. Verify on QEMU TCG, WHPX, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | time + sched suites, 0 failures (236 kernel + 16 user PASS, TCG)
> **Notes:**
> - Shipped: timer-resolution arbitration in `timer_resolution.c` -- `KeSetTimerResolution`/`KeQueryTimerResolution` + `NtSetTimerResolution` (0x00F4) / `NtQueryTimerResolution` (0x00F3), 16-slot table, shortest-wins `arbitrate()`.
> - Review fixes (detail in commit): SSDT out-pointers probed+copied (were raw user writes); per-pid ownership/release/coalescing; clamp both paths; full-slot rollback; atomic resolution; transition-only log; table-full status.
> - Downstream: `KeSetTimerResolution` is `NTSTATUS` with `*actual` (no external callers); SSDT set-handler is non-transactional (mutation authoritative, actual-copy best-effort).
> - Canonical: `src/kernel/time/timer_resolution.c`.
> - Scope boundary: §8 owns arbitration + its two SSDT handlers; process-exit reaping is TODO-21 §14; fault-safe usercopy is TODO-23 §13.
> **Verified:** 2026-06-27 | ship `c5b618ec` + review fixes | 8/8 items | build OK | 236 kernel + 16 user PASS | smoke PASS (TCG 2.49s)
> **Accepted:** [H] per-process nested begin/end refcount for timer resolution -- a repeated `KeSetTimerResolution` set/release does not stack (interim one-coalesced-slot-per-pid model). (Process-exit reaping of leaked requests shipped in TODO-21 §14 via `timer_resolution_release_process`.)
> **Accepted:** [H] `copy_to_user` is not fault-safe (a probed-but-unmapped user page #PFs in the kernel) -- systemic across all SSDT handlers, not §8-specific -> XREF: 02-kernel-core/TODO-23-exception-dispatch-seh.md §13 (item: "`include/kernel/probe.h` -- `try_copy_to_user`" at line 376)
> **Test gap:** arbitration needs live task + timer context; not unit-testable without mocking. Covered by runtime serial validation.
> **Quality reviewed:** 2026-06-27 | Codex 6x (adversarial, consistency, perf, re-adversarial x3) + kernel-quality-auditor | 1C+5H+3M+1L fixed, 2H accepted-XREF | scope: kernel-code-quality

---

## 9. `NtQuerySystemTime`, `NtSetSystemTime`, `NtQueryPerformanceCounter`
Register Win32-named syscalls in the SSDT (→ XREF TODO-12 §5).

> [!NOTE]
> QPC (`NtQueryPerformanceCounter`) returns RAW `mono_filetime_units()` and MUST stay independent of NTP / wall-clock discipline (Win11 contract: QPC is unaffected by Windows Time; Linux `CLOCK_MONOTONIC_RAW`). §17's NTP slew/frequency correction adjusts wall-clock conversion only -- see the §17 [H] gap-audit item.

- [x] `NtQuerySystemTime` (SSDT 0x00F0): calls `KeQuerySystemTime()`, writes FILETIME to user pointer
- [x] `NtSetSystemTime` (SSDT 0x00F1): calls `KeSetSystemTime()`; optional previous-time out-param
- [x] `NtQueryPerformanceCounter` (SSDT 0x00F2): returns `mono_filetime_units()` with fixed 10 MHz frequency
- [x] Registered via `wall_clock_register_ssdt()` in boot Phase 3
- [x] User-pointer safety: the three `wall_clock.c` handlers use `ProbeForRead/WriteIfUser` + `copy_from_user`/`copy_to_user` (`write_u64_out`); `NtSetSystemTime` also kernel-only (`ASSERT_KERNEL_CALLER`) pending `SeSystemtimePrivilege`.
- [x] Commit: `"kernel: nt -- NtQuerySystemTime, NtSetSystemTime, NtQueryPerformanceCounter"`

**Test checkpoint:** Unit: covered by the §1 FILETIME conversion + the mono helpers (the SSDT handlers wrap `KeQuerySystemTime`/`mono_filetime_units` + user-buffer guards, not unit-testable without a syscall harness). Runtime (WHPX / bare metal serial): `NtQuerySystemTime`/`NtQueryPerformanceCounter` return monotonic values to a user buffer; an invalid user pointer yields `STATUS_ACCESS_VIOLATION` (not a kernel fault); `NtSetSystemTime` from user mode returns `STATUS_PRIVILEGE_NOT_HELD`. Verify on QEMU TCG, WHPX, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | FILETIME + time syscall suites, 0 failures (236 kernel + 16 user PASS, TCG)
> **Notes:**
> - Shipped: time SSDT handlers in `wall_clock.c` -- `NtQuerySystemTime` (0x00F0), `NtSetSystemTime` (0x00F1), `NtQueryPerformanceCounter` (0x00F2), registered in Phase 3 via `wall_clock_register_ssdt()`.
> - Review fixes: out-pointers via `ProbeFor*IfUser` + `copy_from_user`/`copy_to_user` (`write_u64_out`) not raw deref; `NtSetSystemTime` rejects placeholder + gated kernel-only pending `SeSystemtimePrivilege`.
> - Downstream: QPC stays raw `mono_filetime_units` (NTP/wall discipline never applies); fixed 10 MHz frequency is the cross-app stability edge.
> - Canonical: `src/kernel/time/wall_clock.c`.
> - Scope boundary: §9 owns the three SSDT handlers + their user-buffer safety; the `SeSystemtimePrivilege` check is TODO-15 §8; fault-safe usercopy is TODO-23 §13.
> **Verified:** 2026-06-27 | ship `d9b7cc13` + review fixes | 6/6 items | build OK | 236 kernel + 16 user PASS | smoke PASS (TCG 2.52s)
> **Accepted:** [H] `NtSetSystemTime` is kernel-only pending a real `SeSystemtimePrivilege` check (no SMP-safe per-token privilege check wired yet) -> XREF: 02-kernel-core/TODO-15-security-reference-monitor.md §8 (item: "`SeSinglePrivilegeCheck(Privilege, AccessMode)`" at line 471)
> **Accepted:** [H] `copy_to_user` is not fault-safe (a probed-but-unmapped user page #PFs in the kernel) -- systemic across all SSDT handlers -> XREF: 02-kernel-core/TODO-23-exception-dispatch-seh.md §13 (item: "`include/kernel/probe.h` -- `try_copy_to_user`" at line 376)
> **Test gap:** the SSDT handlers wrap live time sources + the syscall user-buffer path; not unit-testable without a syscall harness. Covered by runtime serial validation.
> **Quality reviewed:** 2026-06-27 | Codex 4x (adversarial, consistency, perf, re-adversarial) + kernel-quality-auditor | 2M fixed, 2H accepted-XREF | scope: kernel-code-quality

---

## 10. Precise System Time (`KeQuerySystemTimePrecise`)
`GetSystemTimePreciseAsFileTime` (Win8+) returns sub-microsecond UTC wall time by interpolating between timer ticks using the TSC/HPET performance counter. Without this, `KeQuerySystemTime()` is only accurate to the timer tick period (1–15 ms). Databases, distributed systems, and logging all need sub-microsecond timestamps.

- [x] `KeQuerySystemTimePrecise()`: implemented -- currently identical to `KeQuerySystemTime()` since both use `mono_ns()` TSC/HPET interpolation; will diverge when §12 adds coarse KUSER_SHARED_DATA path
- [x] No separate SSDT entry -- `NtQuerySystemTime` (0x00F0) already returns precise time; Win8+ `GetSystemTimePreciseAsFileTime` uses the same syscall
- [x] Commit: `"kernel: time -- KeQuerySystemTimePrecise sub-microsecond wall time"`

**Test checkpoint:** Unit: precision is delivered by `mono_ns()` (covered by the §2 mono helpers); the wrapper has no own logic to unit-test. Runtime (WHPX / bare metal serial): on a TSC/HPET/PMTMR box `KeQuerySystemTimePrecise()` advances at sub-microsecond granularity; `GetSystemTimePreciseAsFileTime` (via `NtQuerySystemTime` 0x00F0) returns the same. Verify on QEMU TCG, WHPX, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | FILETIME + time syscall suites, 0 failures (236 kernel + 16 user PASS, TCG)
> **Notes:**
> - Shipped: `KeQuerySystemTimePrecise()` in `wall_clock.c` -- sub-microsecond UTC via the same `mono_ns()` interpolation as `KeQuerySystemTime()`; no separate SSDT entry (`NtQuerySystemTime` 0x00F0 already returns precise time).
> - How it runs: precision tracks the selected clocksource -- sub-us on TSC/HPET/PMTMR, tick-granular only on the LAPIC last-resort source (where no sub-us source exists, so nothing can do better); review clarified the contract in the code comment.
> - Downstream: diverges from `KeQuerySystemTime()` only when the §12 coarse KUSER_SHARED_DATA tick path lands (precise keeps full `mono_ns()`); satisfies `GetSystemTimePreciseAsFileTime` (Win8+).
> - Canonical: `src/kernel/time/wall_clock.c`.
> - Scope boundary: §10 owns the precise read; the coarse/precise split is §12; the mono clocksource is §2.
> **Verified:** 2026-06-27 | ship `946d3654` + review fix | 2/2 items | build OK | 236 kernel + 16 user PASS | smoke PASS (TCG 2.77s)
> **Quality reviewed:** 2026-06-27 | Codex 3x (adversarial, consistency, perf) + kernel-quality-auditor | 1M fixed (precision-source contract clarified in comment), 0 open | scope: kernel-code-quality; re-adversarial skipped (comment-only fix)

---

## 11. Timezone Bias and DST Management
`GetLocalTime()` and FAT32 timestamps require a local-time offset. Store the bias in the registry (or a kernel global until registry is ready).

- [x] `struct tz_info` with `bias_minutes`, `dst_bias_minutes`, `dst_active` in `timezone.h`
- [x] `timezone_init()`: defaults to UTC; registry read deferred until registry integration
- [x] `timezone_set()` / `timezone_get()` / `timezone_total_bias()`
- [x] `filetime_to_local(utc)` / `filetime_from_local(local)` -- apply total bias (bias + DST)
- [x] Commit: `"kernel: time -- timezone bias and DST management"`

**Test checkpoint:** Unit (`TEST_CAT_SCHED` in `test_time.c`): `timezone_set(-300)` -> `total_bias -300`, `filetime_to_local` = UTC-5h, `from_local` inverts; DST active adds the DST bias; placeholder passes through; an out-of-range bias clamps to +/-24h. Runtime (serial): boot log shows `Timezone: UTC`; FAT32/NTFS stamps reflect the set bias. Verify on QEMU TCG, WHPX, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | time + sched suites incl. timezone, 0 failures (243 kernel + 16 user PASS, TCG)
> **Notes:**
> - Shipped: timezone bias/DST in `timezone.c` -- `timezone_init/set/get`, `timezone_total_bias`, `filetime_to_local`/`from_local`; default UTC.
> - Review fixes: `s_tz` now ONE atomic u64 snapshot (coherent lock-free read for the CLOCK_LEVEL kusd consumer); `set` range-clamps; converters pass placeholder + clamp underflow; signed klog; documented west-negative convention.
> - Downstream: consumed by FAT32/NTFS stamping (§13), KUSD TimeZoneBias (§12), and the desktop clock; registry import (positive-west) is deferred to registry integration.
> - Canonical: `src/kernel/time/timezone.c`.
> - Scope boundary: §11 owns the bias model + UTC<->local math; registry-backed tz load + Win32 positive-west import is registry-integration scope.
> **Verified:** 2026-06-27 | ship `00f207c3` + review fixes | 4/4 items | build OK | 243 kernel + 16 user PASS | smoke PASS (TCG 2.55s)
> **Quality reviewed:** 2026-06-27 | Codex 4x (adversarial, consistency, perf, re-adversarial) + kernel-quality-auditor | 2H+2M fixed, 0 open | scope: kernel-code-quality

---

## 12. KUSER_SHARED_DATA Time Field Updates
The timer ISR must update the `KUSER_SHARED_DATA` time fields (SystemTime, InterruptTime, TickCount) on every tick so user-mode code can read time without a syscall. This is the hottest path in the entire time subsystem -- `GetTickCount64()`, ntdll's `NtQuerySystemTime` fast path, and `QueryInterruptTime` all read from this shared page. Linux's vDSO serves the same purpose.

> [!NOTE]
> Full static field population (NtSystemRoot, ProcessorFeatures, Cookie, etc.) is owned by `TODO-11-peb-teb-user-abi.md §11` (`kusd_init()`). This section provides the page allocation, minimal static fields, and ISR-driven time update.

- [x] `kusd_page_init()` in `kusd_time.c`: allocates physical frame, maps at `0x7FFE0000` user read-only, kernel writes via identity-mapped alias
- [x] Minimal static fields: `TickCountMultiplier`, `NtMajorVersion` (10), `NtMinorVersion` (0), `NtBuildNumber`
- [x] `KSYSTEM_TIME` triple-write protocol in `ksystem_time_write()` inline helper (High1Time, LowPart, High2Time)
- [x] `kusd_update_time()` writes `InterruptTime` (0x008), `SystemTime` (0x014), `TimeZoneBias` (0x020), `TickCount` (0x320)
- [x] Wired into LAPIC timer ISR (`lapic.c`) and PIT ISR (`pit.c`) -- called on every tick
- [x] Volatile writes only, no locks -- runs at CLOCK_LEVEL IRQL
- [x] Also wired `mono_clock_init()`, `wall_clock_init()`, `timezone_init()` into Phase 2 boot (were previously defined but never called)
- [/] Win11 24H2 KUSD QPC fast-path fields -- DEFERRED (design): `QpcBypassEnabled` stays 0 (bypass not needed; NtQueryPerformanceCounter returns raw mono); gated on the user-mode `QueryPerformanceCounter` reader (10-platform-services/TODO-09 §1).
- [/] Reconcile ABI-header / Win11-QPC-tail collision -- DEFERRED (design): move the Impossible-OS ABI header OUT of `KUSER_SHARED_DATA` to a private page (a higher offset only postpones it); needs the authoritative Win11 24H2 tail.
- [x] Commit: `"kernel: time -- KUSER_SHARED_DATA time field updates from timer ISR"`

**Test checkpoint:** Unit: the ISR updater + KUSD page writes are not unit-testable without a live timer/page; the underlying time math is covered by the §2/§6/§11 suites. Runtime (WHPX / bare metal serial): boot log shows `KUSER_SHARED_DATA: user=0x7FFE0000 ...`; user-mode `GetTickCount64()`/`NtQuerySystemTime` fast path advances; a user write to 0x7FFE0000 faults. Verify on QEMU TCG, WHPX, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | time + sched suites, 0 failures (243 kernel + 16 user PASS, TCG)
> **Notes:**
> - Shipped: KUSD page in `kusd_time.c` -- `kusd_page_init` (frame at 0x7FFE0000 user-RO + kernel alias, static fields, AbiMagic last), `kusd_update_time` (CLOCK_LEVEL ISR triple-write of InterruptTime/SystemTime/TimeZoneBias/TickCount).
> - Review fixes: one `wall_clock_snapshot()` mono sample/tick (was two); TimeZoneBias now Win32 positive-west; TickCountMultiplier 10<<24; `ksystem_time_write` `smp_wmb()`; split-return checked.
> - Downstream: user-mode time fast path (no syscall); the QPC-bypass + ABI-header relocation stay deferred (below).
> - Canonical: `src/kernel/time/kusd_time.c`; ABI `include/kernel/nt/kusd.h`.
> - Scope boundary: §12 owns the page + ISR time update; full static-field population is TODO-11 §11; coarse cached reads are §16; the QPC fast path is gated on 10-platform-services/TODO-09 §1.
> **Verified:** 2026-06-27 | ship `da9c78f3` + review fixes | 7/9 items | build OK | 243 kernel + 16 user PASS | smoke PASS (TCG 2.49s)
> **Accepted:** [H] the KUSD huge-page split leaves the 511 identity siblings user-accessible (the KUSD leaf itself is user-RO) -- the global SMEP-disabled boot map (User on all identity pages), not §12-specific -> XREF: 03-memory-concurrency/TODO-02-memory-security.md §3 (item: "set CR4 bit 20 (`CR4.SMEP`)" at line 109)
> **Deferred:** [H] move the Impossible-OS ABI header out of `KUSER_SHARED_DATA` to a private page (Win11-QPC-tail collision) -> XREF: 02-kernel-core/TODO-08-time-filetime-management.md §12 (item: "Reconcile ABI-header / Win11-QPC-tail collision" at line 386)
> **Deferred:** [M] Win11 24H2 QPC-bypass fields (QpcBypassEnabled stays 0 until a user-mode reader exists) -> XREF: 02-kernel-core/TODO-08-time-filetime-management.md §12 (item: "Win11 24H2 KUSD QPC fast-path fields" at line 385)
> **Quality reviewed:** 2026-06-27 | Codex 6x (design, adversarial, consistency, perf, re-adversarial x2) + kernel-quality-auditor | 1H+4M+1L fixed, 1H accepted-XREF, 1H+1M deferred | scope: kernel-code-quality

---

## 13. Filesystem Timestamp Encoding (FAT32 + NTFS)
Replace all zero/stub timestamps in FAT32 and NTFS with correctly computed values.

> [!NOTE]
> **Unblocks:** `05-storage-filesystems/TODO-04-fat32-hardening-vfs-semantics.md §5` -- FAT32 interim timestamps replaced with spec-correct FILETIME-based encoding.

- [x] **FAT32**: `fat32_stamp_create()` / `fat32_stamp_modify()` now use `KeQuerySystemTime()` + `filetime_to_dos_datetime()` with timezone bias; `CrtTimeTenth` populated with 10ms sub-second component
- [x] **NTFS**: `ntfs_data_write.c` -- `unix_to_filetime(0)` placeholder replaced with `KeQuerySystemTime()` for real UTC timestamps in `$STANDARD_INFORMATION` on all write/truncate paths
- [x] **klog_disk**: `format_entry()` now uses ISO 8601 timestamps via `filetime_to_string()` when wall clock is ready, falls back to tick count for early boot entries
- [x] `filetime_to_string(FILETIME ft, char *buf, uint32_t len)` -- ISO 8601 UTC format: `"2026-03-25T14:35:22.123Z"` with millisecond precision
- [x] Commit: `"kernel: fs -- wire FILETIME timestamps into FAT32, NTFS, and klog"`

**Test checkpoint:** Unit: `filetime_to_string` calendar math + bounds are exercised by the §1 FILETIME suite; the FS stamp helpers need a live volume (not unit-testable). Runtime (serial / mounted FS): a file created/modified shows a real local DOS date in FAT32 + a UTC `$STANDARD_INFORMATION` time in NTFS; `kernel.log` lines carry per-entry ISO 8601 timestamps. Verify on QEMU TCG, WHPX, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | FILETIME suites, 0 failures (243 kernel + 16 user PASS, TCG)
> **Notes:**
> - Shipped: FILETIME timestamp wiring -- FAT32 `fat32_stamp_create/modify` (local DOS date + CrtTimeTenth), NTFS `$STANDARD_INFORMATION` UTC FILETIME, `klog_disk` ISO 8601 lines, `filetime_to_string` formatter.
> - Review fixes: `klog_disk format_entry` reconstructs the EVENT FILETIME from `e->timestamp` (was flush-time, collapsing batched lines); FAT32 `CrtTimeTenth` now `(sec&1)*100 + centiseconds` (was `(ms/10)*2`, even-only + dropped odd second).
> - Downstream: unblocks 05-storage/TODO-04 §5 FAT32 timestamps; FAT is local time, NTFS is UTC (correct split).
> - Canonical: `src/kernel/nt/filetime.c` (`filetime_to_string`); FS sites in `fat32_dir.c` / `ntfs_data_write.c` / `klog_disk.c`.
> - Scope boundary: §13 wires the timestamps; rate-stable klog capture is TODO-04 §9; NTFS compressed-write stamping is 05-storage/TODO-02 §2.
> **Verified:** 2026-06-27 | ship `2bf8ba15` + review fixes | 5/5 items | build OK | 243 kernel + 16 user PASS | smoke PASS (TCG 2.53s)
> **Accepted:** [H] NTFS compressed-write path skips the `$STANDARD_INFORMATION` timestamp update -> XREF: 05-storage-filesystems/TODO-02-ntfs-readwrite.md §2 (item: "Compressed-write `$STANDARD_INFORMATION` timestamps" at line 84)
> **Accepted:** [M] klog disk-log event-time reconstruction assumes 100 Hz (drifts after NtSetTimerResolution); needs a rate-stable capture -> XREF: 02-kernel-core/TODO-04-system-logging.md §9 (item: "Rate-stable `klog_entry_t.timestamp`" at line 456)
> **Quality reviewed:** 2026-06-27 | Codex 4x (adversarial, consistency, perf, re-adversarial) + kernel-quality-auditor | 1H+1M fixed, 1H+1M accepted-XREF | scope: kernel-code-quality

---

## 14. Suspend/Hibernate Time Bias Tracking
When the system enters S3 (suspend-to-RAM) or S4 (hibernate), the timer interrupt stops firing and `InterruptTime` freezes. On resume, the kernel must compute how long the system was asleep (from the RTC delta or UEFI time) and add that duration to `InterruptTimeBias` so that `KeQueryInterruptTime()` (which includes bias) continues to advance smoothly. `KeQueryUnbiasedInterruptTime()` deliberately excludes the bias, so it reflects actual CPU-awake time only.

- [x] `ke_suspend_bias_update(uint64_t bias_100ns)` -- shipped in `wall_clock.c` (atomic `__atomic_fetch_add` to `s_interrupt_time_bias`); the S3/S4 resume path is its caller (→ XREF TODO-26)
- [/] On resume: read UEFI `GetTime()` or RTC; compute delta against pre-suspend wall time; pass to `ke_suspend_bias_update()` -- DEFERRED: needs the S3/S4 resume path (greenfield in power management)
- [/] Update `wall_clock` base_mono to account for the monotonic clock gap during suspend -- DEFERRED: same resume-path dependency
- [/] Log the bias adjustment and total sleep duration on resume -- DEFERRED: same resume-path dependency
- [/] RTC wake alarm (gap-audit): program the RTC/ACPI wake timer to resume at a future time (Win11 `RtlSetSystemWakeTime`, Linux `wakealarm`). Owned by power management. -> XREF: 02-kernel-core/TODO-26-power-management.md §3
- [x] Commit: `"kernel: time -- suspend/hibernate time bias tracking (primitive; resume path deferred)"`

> **Test runner:** N/A (resume-path blocked) | the shipped `ke_suspend_bias_update` primitive is exercised by the §7 interrupt-time suite.
> **Notes:**
> - Shipped: `ke_suspend_bias_update(uint64_t)` in `wall_clock.c` -- atomic add into the suspend bias `s_interrupt_time_bias` that `KeQueryInterruptTime` includes (already reviewed under §7).
> - Blocked: the actual resume bias tracking (read RTC/UEFI delta, advance the bias + wall-clock base, log, RTC wake alarm) needs the S3/S4 resume path, which is greenfield in power management.
> - Downstream: once the resume path lands, it calls `ke_suspend_bias_update` with the measured sleep duration so biased interrupt time advances across suspend.
> - Canonical: `src/kernel/time/wall_clock.c` (primitive); resume owner is power management.
> - Scope boundary: §14 owns the bias-application primitive; the S3/S4 resume hook + RTC wake alarm are TODO-26 (power management).
> **Verified:** 2026-06-27 | primitive shipped (§7) | 1/5 items | build OK | resume path deferred
> **Deferred:** [H] resume bias tracking (RTC/UEFI sleep-delta -> `ke_suspend_bias_update`, wall-clock base advance, logging, RTC wake alarm) -- needs the S3/S4 resume path -> XREF: 02-kernel-core/TODO-26-power-management.md §3 (item: "`pm_s3_wakeup_entry`" at line 170)
> **Quality reviewed:** 2026-06-27 | scope: N/A (infra-blocked defer; the shipped `ke_suspend_bias_update` primitive was quality-reviewed under §7)

---

## 15. Leap Second Policy
FILETIME does not count leap seconds -- this matches Windows behavior exactly. The 100 ns count since 1601-01-01 assumes every day has exactly 86400 seconds. `FileTimeToSystemTime` returns seconds 0–59 only; the 60th second during a leap event is never represented. This must be explicitly documented and enforced so that FILETIME arithmetic and round-trip conversions remain correct.

- [x] `FILETIME_LEAP_SECOND_POLICY` doc block added to `filetime.h` -- FILETIME counts SI seconds (86400 s/day), leap seconds never counted (Win32 + POSIX), NTP disciplines wall time only
- [x] `filetime_from_unix_seconds()` confirmed leap-correction-free (pure linear `(unix+offset)*1e7`) + one-line policy reference comment
- [x] `filetime_to_string()` confirmed never emits `:60` (seconds = `secs_in_day % 60`, always 0..59) + one-line policy comment in `filetime.c`
- [x] Enforce at conversion (review): `filetime_clamp_second()` (+ `FILETIME_MAX_SECOND`) routes RTC/EFI/DOS converters so a `:60` (incl. DOS field 30/31 = 60/62 s) collapses onto :59, never rolling into the next minute
- [x] Unit test `test_filetime_leap_second_policy` (`test_time.c`): `1483228799` -> `2016-12-31T23:59:59.000Z`, next second -> `2017-01-01T00:00:00.000Z`, plus efi/rtc/DOS `:60` clamp-to-:59 cases
- [x] Commit: `"kernel: time -- document and enforce leap second policy"`

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | time suite +1 (`time: FILETIME leap second policy`), 252 kernel tests, 0 failures
> **Notes:**
> - Shipped: `FILETIME_LEAP_SECOND_POLICY` doc block + `filetime_clamp_second()`/`FILETIME_MAX_SECOND` in `filetime.h`; policy comments at `filetime_from_unix_seconds`/`filetime_to_string`; `test_filetime_leap_second_policy`.
> - Enforced at conversion: review added the clamp so every seconds-ingest converter (RTC, EFI GetTime, DOS datetime) maps a `:60` onto :59 instead of advancing wall time by a second; valid 0..59 inputs unchanged.
> - Downstream: locks the contract the §17 NTP hooks rely on -- wall-time discipline only, no leap smearing, monotonic clock stays raw.
> - Canonical: `include/kernel/nt/filetime.h` leap-second policy block.
> - Scope boundary: §15 owns the SI-second invariant; broader EFI/RTC field-range validation (minute/hour/day) lives in §5's `rtc_try_read`/seed path, not here.
> **Verified:** 2026-06-27 | ship `f4891b42` + review clamp fixes | 5/5 items | build OK | tests 252 PASS
> **Quality reviewed:** 2026-06-27 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 2M fixed | scope: kernel-code-quality

---

## 16. Coarse Time Fast Path
Hot paths like klog timestamping, scheduler accounting, and network packet timestamping call `KeQuerySystemTime()` thousands of times per second. A "coarse" variant avoids the seqlock read and TSC access by returning the last ISR-cached value. Linux provides `ktime_get_coarse()` for the same reason -- 10x faster than the precise variant, accurate to the timer tick period.

> [!TIP]
> Neither Windows nor Linux exposes a single API that explicitly distinguishes "coarse kernel time" -- Windows uses KUSER_SHARED_DATA reads implicitly, and Linux has `ktime_get_coarse()` internally. Impossible OS can provide a clean `KeQuerySystemTimeCoarse()` API for kernel drivers.

- [x] `FILETIME KeQuerySystemTimeCoarse(void)` in `wall_clock.c` -- lock-free `__atomic` ACQUIRE read of the ISR-cached SystemTime (no clocksource read, no seqlock); return-by-value matching `KeQuerySystemTime`/`Precise`
- [x] `uint64_t KeQueryInterruptTimeCoarse(void)` -- same lock-free ISR-cached read via `s_coarse_interrupt_time` (the spec's `g_interrupt_time` never existed; interrupt time is derived)
- [x] `wall_clock_tick_cache()` takes ONE precise `mono_ns()` per tick, publishes both coarse statics, and feeds KUSD; the read is amortized across KUSD + every coarse reader (a `mono_ns_coarse` cache would freeze KUSD ~80 ms on PMTMR)
- [x] `klog()` ring + crash-dump timestamps use `KeQueryInterruptTimeCoarse()` (10 ms units) -- lock-free single atomic read (panic-safe, no seqlock hang) + rate-safe; `klog_disk` reconstructs from the same coarse interrupt-time unit
- [x] Migrated raw `system_get_ticks()` consumers to rebased sources: `etw.c`, `registry.c` (`reg_get_uptime_ns`), `wer.c`, `virtio` adaptive window (absolute-ns) -- removes the `KeSetTimerResolution` rewind
- [x] Commit: `"kernel: time -- coarse time fast path for hot-path callers"`

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | time suite +1 (`time: coarse time fast path`), 255 kernel tests, 0 failures
> **Notes:**
> - Shipped: `KeQuerySystemTimeCoarse`/`KeQueryInterruptTimeCoarse` + `wall_clock_tick_cache` in `wall_clock.c` (BSP-ISR-published lock-free coarse cache fed by ONE precise `mono_ns()` per tick).
> - How it runs: the BSP timer ISR (`kusd_update_time`) is the single publisher of the cache (`__atomic` RELEASE, one precise `mono_ns()` sample); readers do one ACQUIRE load -- the fast path is reader-side (no per-call clocksource/seqlock read); coarse reflects a clock set within one tick.
> - Downstream: closes the `TODO-11 §6` rate-change rewind -- klog/etw/registry/wer/virtio stamps use rebased `uptime_ns()`/coarse, not raw `system_get_ticks()`.
> - Canonical: `include/kernel/time/wall_clock.h` Ke*Coarse APIs.
> - Scope boundary: §16 owns the coarse read path + consumer migration; the clocksource demotion feeding `mono_ns_coarse` is §18; precise sub-us reads stay on `KeQuerySystemTime`/`...Precise`.
> **Verified:** 2026-06-27 | ship `377c2aec` + review fixes | 5/5 items | build OK | tests 255 sched + 422 storage PASS | smoke PASS (TCG 2.66s)
> **Quality reviewed:** 2026-06-27 | Codex 7x (adversarial, consistency, perf, re-adversarial) + kernel-explorer + kernel-quality-auditor | 1H+4M+2L fixed | scope: kernel-code-quality

---

## 17. NTP Clock Adjustment Hooks
The NTP protocol client (network stack TODO) needs a kernel interface to correct both the wall clock phase (offset) and frequency (skew). Provide the hooks now so the network stack can call them later.

- [x] Defined `ntp_adj_t` + `struct ntp_status` in `include/kernel/time/ntp_adj.h`:
  - `int64_t  offset_ns` -- signed wall-clock phase correction
  - `int32_t  freq_ppb` -- parts-per-billion frequency correction (positive = clock running fast)
- [x] `ke_ntp_adjtime()` (`wall_clock.c`) -- THREAD-context hook; `mono_ns`/QPC/interrupt time stay RAW (gap-audit [H]: QPC independent of system time):
  - Phase step: `|offset_ns| > 1s` -> `KeSetSystemTime()` (monotonicity-safe), clears pending slew
  - Frequency + slew: clamped (freq to `+/-NTP_FREQ_MAX_PPB`) and STORED for the status query + the watchdog's continuous discipline
- [x] `ke_ntp_get_status(struct ntp_status *out)` -- slew-remaining offset, freq, last-sync FILETIME, source (`"ntp"`/`"ntp-pending"`/`"none"`)
- [x] Contract documented in `ntp_adj.h` + the wall_clock.c block: `ke_ntp_adjtime()` is THREAD (PASSIVE) context only; the kernel never initiates network calls
- [x] Honest discipline-state via pure tested `ntp_source_for_correction(off,freq)`: `"ntp"` only when fully applied (pure step / zero no-op); any stored slew or nonzero freq (incl. mixed step+freq) -> `"ntp-pending"`
- [x] All anchor writers coordinate on `s_ntp_lock` (order `s_ntp_lock -> s_lock`): a non-NTP `KeSetSystemTime` clears NTP status (`source="none"`, slew/freq 0) atomically with the swap, so a manual set never leaves a stale `"ntp"`
- [x] Continuous per-tick freq/slew APPLICATION: shipped in §19 (`ke_ntp_discipline_tick` applier behind the generation-aware wall floor).
- [x] Commit: `"kernel: time -- NTP clock phase and frequency adjustment hooks"`

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | NTP tests (tick adjust, step-target valid, reject preserves, source label, manual-set invalidates) + absurd-set reject, 283 kernel tests, 0 failures
> **Notes:**
> - Shipped: `include/kernel/time/ntp_adj.h` (ntp_adj_t/ntp_status + API) + `ke_ntp_adjtime`/`ke_ntp_get_status` + pure tested `ntp_tick_adjust_ns`/`ntp_step_target_valid`/`ntp_source_for_correction` in `wall_clock.c`.
> - How it runs: `ke_ntp_adjtime` (thread context) steps the wall clock for a large offset (`KeSetSystemTime`, monotonicity-safe) and stores a clamped freq + slew; the monotonic clock (`mono_ns`/QPC/`KeQueryInterruptTime`) is never touched.
> - Honest state: step -> `source="ntp"`; stored slew/freq -> `"ntp-pending"` (never read as completed sync); `KeSetSystemTime` rejects placeholder/`>FILETIME_MAX_PLAUSIBLE` so a corrupt anchor cannot be sourced.
> - Deferred: the continuous per-tick freq/slew application needs a wall-time monotonic floor (a discrete anchor nudge regresses precise wall reads) -- built with the clocksource watchdog's mono-wide floor (§18).
> - Canonical: `include/kernel/time/ntp_adj.h`.
> - Scope boundary: §17 owns the adjustment hooks; the NTP protocol client + network I/O are a network-stack TODO; the mono-wide floor is §18; the continuous wall-time discipline + wall floor are §19.
> **Verified:** 2026-06-27 | commit `0fb1743f` | 7/8 items | build OK | tests 283/283 PASS
> **Quality reviewed:** 2026-06-27 | Codex 3x (adversarial, consistency, perf) | 2M+1L fixed (doc/contract), 2 deferred | scope: kernel-code-quality

---

## 18. Clocksource Quality Watchdog
Continuously cross-check the active monotonic clock source against a reference and demote a drifting source (an unstable TSC) to HPET/PMTMR, matching Linux `clocksource.c` (watchdog ~every 0.5 s, "Marking TSC unstable") and the Win11 HAL silent demotion. Replaces the boot-time `platform_is_tcg()` gate in `timer_hal_init()`. Split out of §2 by the gap-audit + design review.

- [x] Boot-time source qualification (design review [H]): `mono_source_qualify()` rejects an implausible-frequency candidate so `mono_clock_init()` selects the highest-priority QUALIFIED source by measurement, not a platform guess.
- [x] Ongoing watchdog on the kworker pool (design review [H]): `mono_clock_watchdog_init` registers `mono_watchdog_tick` (~0.5 s Phase-3 PASSIVE) comparing active-TSC vs an HPET reference (wrap-free), demoting after 2 over-threshold windows.
- [x] Atomic coherent demotion (design review [H], shared with §3): immutable per-source `mono_desc` table + atomic `s_active_src` index (loaded once by `mono_ns`) so source+scale never tear; demotion re-anchors then RELEASE-publishes the index last.
- [x] Anchor new source to old `mono_ns()` on switch: mono-wide `mono_floor()` ALWAYS applied (demotion raises it before publish) + per-source `mono_source_reanchor` (HPET offset / PMTMR + LAPIC epoch) so time never steps back across a demotion.
- [x] PMTMR epoch refresh across tick quiesce (review [H]): `mono_clock_pmtmr_sync()` from `timer_hal_quiesce`/`timer_hal_resume` around the mask so a sub-wrap quiesce loses no wraps (the mono floor blocks longer-quiesce backward steps).
- [x] Log the demotion with old/new source + measured drift ppm in `mono_clock_demote()` (`LOG_WARN "clocksource: demoting %s -> %s (%u ppm)"`).
- [ ] PMTMR-reference watchdog monitoring (review [H], deferred): bounding the 24-bit PMTMR wrap needs a TSC-independent + rate-correct + mask-continuous clock (none exists), so auto-monitoring is HPET-reference-only for now.
- [ ] Optimization (perf review [M]): a per-CPU / batched `mono_floor` fast path for steady-state TSC reads -- the always-on global `cmpxchg` on `s_mono_floor` bounces a cacheline under cross-CPU polling; monotonicity MUST stay global.
- [x] Continuous NTP freq/slew APPLICATION + per-tick lock-hold audit: shipped in §19 (the §17-deferred wall-time discipline, behind the generation-aware wall floor).
- [x] Commit: `"kernel: time -- clocksource quality watchdog with drift demotion"`

**Test checkpoint:** Unit (`TEST_CAT_SCHED` in `test_time.c`): `mono_drift_ppm` classifies a +/-600 ppm skew as over-threshold, a 100 ppm skew as under, saturates a >=100 % diff, and returns 0 for a zero window. Runtime (WHPX / bare metal serial, not unit-testable without live drift): boot logs the qualified source; an unstable TSC logs `"clocksource: demoting TSC -> HPET (N ppm)"` and `mono_ns()` stays monotonic across the demotion. Verify on QEMU WHPX, TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | time suites incl. `clocksource watchdog drift ppm classify`, 291 kernel tests, 0 failures
> **Notes:**
> - Shipped: immutable per-source `mono_desc` table + atomic `s_active_src` index, mono-wide `mono_floor()`, `mono_clock_demote()` + `mono_watchdog_tick`/`mono_clock_watchdog_init`, pure `mono_drift_ppm`, `mono_clock_pmtmr_sync` -- all in `mono_clock.c`.
> - How it runs: `mono_ns()` acquire-loads `s_active_src` once for a coherent scale; the Phase-3 kworker (~0.5 s) compares active TSC vs an HPET reference and after 2 over-threshold windows demotes via re-anchor + floor + RELEASE-publish.
> - Downstream: satisfies §3's deferred atomic-descriptor + mono-wide-floor unit; the §17-deferred continuous NTP application is split to §19. Codex design adoptions (immutable descriptor over seqlock; wall floor for NTP) in the commit.
> - Canonical: `src/kernel/time/mono_clock.c`.
> - Scope boundary: §18 owns clocksource qualification + drift demotion; NTP wall-time discipline is §19; AP TSC sync stays §3-deferred; S3 resume is power-management.
> **Verified:** 2026-06-27 | commit `10613f9a` | 6/9 items | build OK | tests 291/291 PASS | smoke PASS (TCG 2.53s)
> **Deferred:** [H] PMTMR-reference watchdog monitoring (auto-monitoring is HPET-reference-only; PMTMR wrap unboundable by an independent continuous clock) -> XREF: 02-kernel-core/TODO-08 §18 (item: "PMTMR-reference watchdog monitoring" at line 543)
> **Deferred:** [M] per-CPU/batched `mono_floor` fast path for the always-on TSC read -> XREF: 02-kernel-core/TODO-08 §18 (item: "Optimization (perf review [M])" at line 544)
> **Quality reviewed:** 2026-06-27 | Codex 12x (design, adversarial, consistency, perf, re-adversarial) | 8H+3M+1L fixed, 3 deferred | scope: kernel-code-quality

---

## 19. NTP Continuous Wall-Time Discipline
Apply the stored NTP frequency/slew correction (from §17 `ke_ntp_adjtime`) to the wall clock continuously, so a disciplined clock actually tracks the reference instead of only storing the correction. The §17 hooks store freq/slew and report `source="ntp-pending"`; this section adds the per-tick applier (built on the §18 mono floor) that consumes them and flips the status to `"ntp"`. Split from §18 because it needs its own wall-time floor and design pass. -> XREF: 02-kernel-core/TODO-08 §17 (item: "Continuous per-tick freq/slew APPLICATION deferred")

- [x] Wall-time floor (design review [H]): generation-aware WRITER-ONLY `s_wall_floor`/`s_wall_floor_gen`, read-only `wall_floor_read` clamps `KeQuerySystemTime`/`Coarse`/`Precise`; `KeSetSystemTimeEx` bumps gen + resets it (no stale-reader poison).
- [x] Per-tick NTP applier `ke_ntp_discipline_tick` (~0.25 s PASSIVE kworker) samples `mono_ns()` outside `s_ntp_lock`, then under `s_ntp_lock -> s_lock` raises the floor + re-anchors `s_base_time` via `ntp_tick_adjust_ns()` + decrements slew.
- [x] Flip §17 status `"ntp-pending"` -> `"ntp"` once applied; a manual `KeSetSystemTimeEx` still resets to `"none"` (the §17 invalidation path).
- [x] Unit tests (`TEST_CAT_SCHED`): pure `wall_floor_clamp` -- same-gen clamps up across a backward re-anchor, gen-mismatch passes through (no poison).
- [x] Commit: `"kernel: time -- NTP continuous wall-time discipline"`

**Test checkpoint:** Unit (`TEST_CAT_SCHED` in `test_time.c`): pure `wall_floor_clamp` clamps a backward candidate up to the floor at the same generation and passes a candidate through on a generation mismatch (so a backward manual set stands without poisoning). Runtime (WHPX / bare metal serial): after `ke_ntp_adjtime` with a sub-second slew, `ke_ntp_get_status` reports `"ntp"` and `KeQuerySystemTime` advances monotonically while the slew is consumed. Verify on QEMU WHPX, TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | time suites incl. `NTP wall-floor clamp + generation gate`, 296 kernel tests, 0 failures
> **Notes:**
> - Shipped: generation-aware WRITER-ONLY wall floor (`s_wall_floor`/`s_wall_floor_gen`, pure `wall_floor_clamp` + read-only `wall_floor_read`) + the `ke_ntp_discipline_tick` kworker applier (`ke_ntp_discipline_init`) in `wall_clock.c`.
> - How it runs: a ~0.25 s Phase-3 PASSIVE kworker (mono sampled outside `s_ntp_lock`) re-anchors `s_base_time` by `ntp_tick_adjust_ns()` under `s_ntp_lock -> s_lock`; a negative correction is a brief floor-clamped slow, then status flips to `"ntp"`.
> - Downstream: closes the §17-deferred continuous NTP application + per-tick lock-hold audit; the floor is gen-reset by `KeSetSystemTimeEx` so a backward manual set is not clamped. Codex design + adversarial adoptions in the commit.
> - Canonical: `src/kernel/time/wall_clock.c`.
> - Scope boundary: §19 owns the applier + wall floor; the NTP protocol client + network I/O are a network-stack TODO; the mono floor is §18.
> **Verified:** 2026-06-27 | commit `1b3085bd` | 4/4 items | build OK | tests 296/296 PASS | smoke PASS (TCG 2.85s)
> **Quality reviewed:** 2026-06-27 | Codex 7x (design, adversarial, consistency, perf, re-adversarial) | 3H+2M fixed | scope: kernel-code-quality

---

## OS Comparison

| ⭐  | Feature               | 🪟 Win11                 | 🐧 Linux                  | 🚀 Impossible OS           |
| --- | --------------------- | ------------------------ | ------------------------- | -------------------------- |
| 💎  | 100 ns wall time      | ✅ FILETIME API          | ⚠️ timespec, diff epoch    | ✅ §1,§5,§6 done           |
| 💎  | Monotonic counter     | ✅ QPC via TSC/HPET      | ✅ CLOCK_MONOTONIC vDSO   | ✅ §2/§4/§18, AP-sync §3   |
| 💎  | Invariant TSC detect  | ✅ CPUID 0x15            | ✅ tsc_khz calibration    | ✅ §3 -- invariant+offset  |
| 💎  | Per-CPU TSC sync      | ✅ TSC sync at INIT      | ✅ check_tsc_sync         | ⬜ §3 -- AP sync deferred  |
| 💎  | Clocksource watchdog  | ✅ HAL silent demote     | ✅ clocksource.c watchdog | ✅ §18 -- drift demotion   |
| 💎  | Ordered TSC read      | ✅ QPC abstracts rdtscp  | ✅ rdtsc_ordered LFENCE   | ✅ §3 -- rdtsc_ordered     |
| 💎  | NTP clock discipline  | ✅ W32Time service       | ✅ adjtimex / ntpd        | ✅ §17 hooks + §19 apply   |
| 💎  | HPET fallback         | ✅ when TSC unreliable   | ✅ hpet_clocksource       | ✅ §4 -- hpet.c driver     |
| 💎  | Wall clock UEFI/RTC   | ✅ GetSystemTime         | ✅ efi_get_time           | ✅ §5 -- wall_clock_init   |
| 💎  | Interrupt time        | ✅ KeQueryInterruptTime  | ✅ CLOCK_BOOTTIME         | ✅ §7 -- biased+unbiased   |
| 💎  | Timer resolution      | ✅ NtSetTimerResolution  | ✅ timer_settime NO_HZ    | ✅ §8 -- SSDT 0xF3/0xF4    |
| 💎  | Precise wall time     | ✅ PreciseAsFileTime     | ✅ CLOCK_REALTIME vDSO    | ✅ §10 -- TSC interpolated |
| 💎  | Timezone + DST        | ✅ registry TZ info      | ✅ /etc/localtime         | ⬜ §11                     |
| 💎  | KUSD / vDSO time page | ✅ KUSD 0x7FFE0000       | ✅ vDSO clock_gettime     | ✅ §12 -- ISR-updated      |
| 💎  | FAT32 timestamps      | ✅ kernel32 -> FAT dir   | ✅ fat inode time         | ✅ §13 -- FILETIME-based   |
| 💎  | NTFS FILETIME         | ✅ $STANDARD_INFO        | ✅ ntfs3 current_time     | ⬜ §13                     |
| 💎  | Suspend time bias     | ✅ InterruptTimeBias     | ✅ CLOCK_BOOTTIME         | ⬜ §14                     |
| 💎  | Leap second policy    | ✅ skips leap seconds    | ✅ 86400 s/day            | ✅ documented + tested     |
| 💎  | NTP adjustment        | ✅ W32tm + SetSystemTime | ✅ adjtimex syscall       | ✅ ke_ntp_adjtime hooks    |
| ⭐  | Fixed 10 MHz QPC      | ⚠️ varies by hardware     | ❌ no fixed-freq API      | ✅ §9 -- 10 MHz fixed      |
| ⭐  | Coarse time API       | ⚠️ implicit KUSD          | ⚠️ ktime_get_coarse        | ✅ explicit Ke*Coarse      |

> **After §1–§15:** Impossible OS matches Windows NT exactly on FILETIME semantics, QPC, interrupt time, timer resolution, precise time, timezone handling, KUSD time updates, suspend bias, and filesystem timestamp accuracy.
> **§9** locks `QueryPerformanceFrequency` to 10 MHz (FILETIME ticks/second), making it constant and hardware-independent -- Windows still returns variable hardware frequencies and apps must handle this; Linux has no equivalent fixed-frequency API.
> **§16** provides a clean `KeQuerySystemTimeCoarse()` API that neither Windows nor Linux explicitly exposes as a named kernel function -- drivers and hot-path code get a documented O(1) cached-read path.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_time()` (XREF: `00-infrastructure/TODO-03-kernel-test-harness.md`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_time.c` with:
  - `filetime_from_unix_seconds(0)` returns `116444736000000000ULL` (epoch offset)
  - `filetime_to_unix_seconds(116444736000000000ULL)` returns `0`
  - `filetime_from_unix_seconds(1)` returns `116444736010000000ULL` (1 second = 10M ticks)
  - Round-trip: `filetime_to_unix_seconds(filetime_from_unix_seconds(N)) == N` for N = 0, 1000, 1700000000
  - `filetime_to_dos_datetime` on known FILETIME produces correct DOS date and time words
  - `filetime_from_dos_datetime` round-trips back to the original FILETIME (within 2-second FAT32 resolution)
  - `filetime_from_rtc` on a known `rtc_time` struct produces the expected FILETIME
  - `filetime_from_efi_time` on a known `efi_time` struct produces the expected FILETIME
  - `mono_ns()` returns monotonically increasing values across two consecutive calls
  - `mono_clock_source_name()` returns one of `"TSC"`, `"HPET"`, or `"LAPIC"` (not NULL)
  - `KeQuerySystemTime` returns a plausible FILETIME (year 2024+ encoded)
  - `KeQuerySystemTime` called twice ~1 ms apart returns increasing values
  - `KeQueryPerformanceCounter` returns non-zero, monotonically increasing value
  - `KeQueryInterruptTime` returns non-zero, monotonically increasing value
  - `KeQueryUnbiasedInterruptTime` returns value <= `KeQueryInterruptTime` (no suspend bias yet)
  - `KeQuerySystemTimePrecise` returns value >= `KeQuerySystemTime` (interpolation adds precision, never subtracts)
  - `KeQuerySystemTimeCoarse` returns a plausible FILETIME (may lag precise by up to one tick)
  - `NtQueryTimerResolution` returns minimum <= current <= maximum
  - `filetime_to_local` with `bias_minutes = -300` subtracts 5 hours from UTC FILETIME
  - `filetime_from_local` is inverse of `filetime_to_local` for same timezone
  - `filetime_to_string` produces ISO 8601 format string with `T` and `Z` markers
  - `filetime_from_unix_seconds(1483228799)` produces a value where `filetime_to_string` shows `23:59:59` (not `:60`, leap second policy)
- [ ] Register in `test_runner_init()`: `test_register_time()`
- [ ] Commit: `"test: add time and FILETIME management test suite"`

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] `filetime_from_unix_seconds(0)` returns `116444736000000000ULL`
- [ ] `filetime_from_efi_time()` on a known `efi_time` struct produces the correct FILETIME
- [ ] Boot log shows time source (`TSC`, `HPET`, or `LAPIC`) and wall clock seed value
- [ ] `KeQuerySystemTime()` called twice 1 second apart returns values differing by ~`10000000` (1 s at 100 ns resolution)
- [ ] `NtQuerySystemTime` from ring 3 returns `STATUS_SUCCESS` and a plausible UTC FILETIME (year 2024+)
- [ ] `NtQueryPerformanceCounter` returns monotonically increasing values across repeated calls
- [ ] `NtQueryPerformanceFrequency` returns `10000000` (fixed 10 MHz)
- [ ] `KeQueryInterruptTime()` increases monotonically across calls
- [ ] `KeQueryInterruptTimePrecise()` has sub-tick precision (differs from coarse by < one tick period)
- [ ] `NtSetTimerResolution(5000, TRUE, &actual)` returns `STATUS_SUCCESS` and `actual <= 5000`
- [ ] `NtQueryTimerResolution` reports minimum <= current <= maximum
- [ ] `KeQuerySystemTimePrecise()` returns sub-microsecond precision timestamps
- [ ] KUSER_SHARED_DATA `InterruptTime` at offset 0x008 advances over time (user-mode read, no syscall) -- page mapped, ISR updates wired
- [ ] KUSER_SHARED_DATA `SystemTime` at offset 0x014 matches `NtQuerySystemTime` within one tick -- page mapped, ISR updates wired
- [ ] FAT32 directory entry timestamps after file write show current date/time in local time, not zero
- [ ] NTFS `$STANDARD_INFORMATION` timestamps after file create show current UTC FILETIME, not zero
- [ ] klog entries carry an ISO 8601 timestamp prefix
- [ ] `ke_ntp_adjtime()` with a +500 ms phase offset advances the wall clock by 500 ms within the slew window
- [ ] Commit: `"kernel: time -- FILETIME, wall clock, QPC, interrupt time, timer resolution, timezone, and filesystem timestamps"`
