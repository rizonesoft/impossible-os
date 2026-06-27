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
- → XREF: `TODO-14-registry-completion.md §2` -- registry `LastWriteTime` conversion from PIT ticks to FILETIME uses `ticks_to_filetime()` from §3
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

| ⭐  | Order | Deliverable                                                            | Depends On          | Status |
| --- | :---: | ---------------------------------------------------------------------- | ------------------- | :----: |
| 💎  |   1   | `FILETIME` type, epoch constants, and conversion math                  | --                  |  [x]   |
| 💎  |   2   | Monotonic nanosecond clock source selection                            | §1                  |  [x]   |
| 💎  |   3   | Invariant TSC detection and per-CPU offset calibration                 | §2                  |  [/]   |
| 💎  |   4   | HPET standalone driver                                                 | --                  |  [x]   |
| 💎  |   5   | Wall clock init from UEFI GetTime / RTC                                | §1, §2              |  [x]   |
| 💎  |   6   | Kernel time service (`KeQuerySystemTime`, `KeSetSystemTime`)           | §5                  |  [x]   |
| 💎  |   7   | Interrupt time and unbiased interrupt time APIs                        | §2, §6              |  [x]   |
| 💎  |   8   | Timer resolution management (`NtSetTimerResolution`)                   | §6, TODO-12 §5      |  [x]   |
| 💎  |   9   | `NtQuerySystemTime` / `NtSetSystemTime` / `NtQueryPerformanceCounter`  | §6, TODO-12 §5      |  [x]   |
| 💎  |  10   | Precise system time (`KeQuerySystemTimePrecise`)                       | §6, §2              |  [x]   |
| 💎  |  11   | Timezone bias and DST management                                       | §6                  |  [x]   |
| 💎  |  12   | KUSER_SHARED_DATA time field updates from timer ISR                    | §6, §7, TODO-11 §11 |  [/]   |
| 💎  |  13   | Filesystem timestamp encoding (FAT32 + NTFS)                           | §6, §11             |  [x]   |
| 💎  |  14   | Suspend/hibernate time bias tracking                                   | §7, TODO-26 §3,§4   |  [ ]   |
| 💎  |  15   | Leap second policy                                                     | §1                  |  [ ]   |
| ⭐  |  16   | Coarse time fast path (lock-free cached time)                          | §6                  |  [ ]   |
| ⭐  |  17   | NTP clock adjustment hooks                                             | §6                  |  [ ]   |
| 💎  |  18   | Clocksource quality watchdog (drift demotion)                          | §2, §4              |  [ ]   |

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
> **Accepted:** [H] `kusd_update_time()` does precise PMTMR reads every tick (6 port reads/tick in the ISR) -> XREF: 02-kernel-core/TODO-08-time-filetime-management.md §16 (item: "Migrate `kusd_update_time()` ... to the coarse variants" at line 125)
> **Accepted:** [H] a multi-second tick quiesce can lose 24-bit PMTMR wraps (the monotonic floor blocks backward steps meanwhile) -> XREF: 02-kernel-core/TODO-08-time-filetime-management.md §18 (item: "PMTMR epoch refresh across tick quiesce" at line 334)
> **Quality reviewed:** 2026-06-27 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 2H+2M fixed, 2H accepted | scope: kernel-code-quality

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
- [ ] Atomic demote-on-warp + mono-wide floor (design review [H], shared with §18): atomic clocksource descriptor + a mono-wide monotonic floor (generalize §2 `pmtmr_mono_floor`) so demote/migration never step backward.
- [ ] Resume monotonicity -- BLOCKED on power-management S3; reuses this section's sync/demote machinery. -> XREF: 02-kernel-core/TODO-26-power-management.md §3
- [x] Commit: `"kernel: time -- invariant TSC detection and per-CPU offset calibration"`

**Test checkpoint:** Unit: existing `test_time.c` mono/PMTMR suites still pass (the ordered read changes only ordering, not values; RDTSCP/LFENCE serialization is not directly unit-testable). Runtime (WHPX / bare metal serial): `mono_ns()` advances monotonically; on invariant-TSC SMP no QPC backward step across thread migration. Verify on QEMU WHPX SMP, TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | time + sched suites, 0 failures (208 kernel + 16 user PASS, TCG)
> **Notes:**
> - Shipped: `rdtsc_ordered()` in `mono_clock.c` (RDTSCP gated on the all-online `cpu_feature_global_mask()`, else `LFENCE;RDTSC`), replacing bare `rdtsc` for `rdtsc_ns()` + the `mono_ns()` TSC branch.
> - How it runs: every TSC read serializes against prior instructions so a monotonic/QPC read never samples earlier than its surrounding code; the global-mask gate (not `cpu_has`) avoids an #UD on a feature-skewed AP.
> - Downstream: items 2-3 (AP TSC sync rendezvous + atomic-descriptor demotion + mono-wide floor) are a section-class SMP unit shared with the §18 watchdog -- deferred coherent. Codex adversarial adoption (RDTSCP AP-skew #UD) in the commit.
> - Canonical: `src/kernel/time/mono_clock.c`.
> - Scope boundary: §3 ships the ordered read; AP sync + demotion machinery is the deferred items (shared with §18); resume re-verify is power-management.
> **Verified:** 2026-06-27 | commit `13f02ffe` | 7/10 items | build OK | 208 kernel + 16 user PASS | smoke PASS (TCG)
> **Deferred:** [H] AP TSC sync rendezvous + atomic-descriptor demotion + mono-wide floor -- section-class SMP unit (design captured), shared with the watchdog -> XREF: 02-kernel-core/TODO-08-time-filetime-management.md §18 (item: "Atomic coherent demotion" at line 340)
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
> - Downstream: HPET is the QPC/mono fallback when no invariant TSC; the per-tick UC-MMIO cost in `kusd_update_time()` is accepted to §16 (coarse migration, now covers HPET + PMTMR).
> - Canonical: `src/kernel/drivers/hpet.c`.
> - Scope boundary: §4 owns the main-counter driver; HPET timer comparators (scheduler timer) are out of scope; coarse-read migration is §16.
> **Verified:** 2026-06-27 | commit `5c2ec338` | 6/6 items | build OK | 208 kernel + 16 user PASS | smoke PASS (HPET selected, TCG 2.5s)
> **Accepted:** [H] HPET source does live UC-MMIO reads in the per-tick `kusd_update_time()` ISR path -> XREF: 02-kernel-core/TODO-08-time-filetime-management.md §16 (item: "Migrate `kusd_update_time()` ... to the coarse time variants" at line 186)
> **Quality reviewed:** 2026-06-27 | Codex 4x (adversarial, consistency, perf, re-adversarial x2) | 2H+1M fixed, 1H accepted | scope: kernel-code-quality

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
- [x] `KeSetTimerResolution(desired, set)`: request/release with clamping; global arbitration (shortest wins)
- [x] `NtSetTimerResolution` (SSDT 0x00F4) + `NtQueryTimerResolution` (SSDT 0x00F3) -- registered via `timer_resolution_register_ssdt()`
- [x] 16-slot request table with per-entry active flag; `arbitrate()` scans for shortest
- [x] `KeQueryTimerResolution(max, min, current)` returns all three values
- [x] Per-process tracking: moved to `TODO-21-process-model-extensions.md §14` (Process Exit Cleanup) -- releases timer resolution requests on process exit
- [x] Timer ICR update: `lapic_timer_set_hz()` reprograms LAPIC ICR when resolution changes; called from `arbitrate()` in timer_resolution.c
- [x] Commit: `"kernel: time -- timer resolution management (NtSetTimerResolution)"`

---

## 9. `NtQuerySystemTime`, `NtSetSystemTime`, `NtQueryPerformanceCounter`
Register Win32-named syscalls in the SSDT (→ XREF TODO-12 §5).

> [!NOTE]
> QPC (`NtQueryPerformanceCounter`) returns RAW `mono_filetime_units()` and MUST stay independent of NTP / wall-clock discipline (Win11 contract: QPC is unaffected by Windows Time; Linux `CLOCK_MONOTONIC_RAW`). §17's NTP slew/frequency correction adjusts wall-clock conversion only -- see the §17 [H] gap-audit item.

- [x] `NtQuerySystemTime` (SSDT 0x00F0): calls `KeQuerySystemTime()`, writes FILETIME to user pointer
- [x] `NtSetSystemTime` (SSDT 0x00F1): calls `KeSetSystemTime()`; optional previous-time out-param
- [x] `NtQueryPerformanceCounter` (SSDT 0x00F2): returns `mono_filetime_units()` with fixed 10 MHz frequency
- [x] Registered via `wall_clock_register_ssdt()` in boot Phase 3
- [ ] User-pointer safety: the three `wall_clock.c` handlers deref user pointers directly; gate on `ssdt_previous_mode()` + `copy_from_user`/`copy_to_user` (a user `prev_ptr` can fault the kernel or write a FILETIME to a kernel address).
- [x] Commit: `"kernel: nt -- NtQuerySystemTime, NtSetSystemTime, NtQueryPerformanceCounter"`

---

## 10. Precise System Time (`KeQuerySystemTimePrecise`)
`GetSystemTimePreciseAsFileTime` (Win8+) returns sub-microsecond UTC wall time by interpolating between timer ticks using the TSC/HPET performance counter. Without this, `KeQuerySystemTime()` is only accurate to the timer tick period (1–15 ms). Databases, distributed systems, and logging all need sub-microsecond timestamps.

- [x] `KeQuerySystemTimePrecise()`: implemented -- currently identical to `KeQuerySystemTime()` since both use `mono_ns()` TSC/HPET interpolation; will diverge when §12 adds coarse KUSER_SHARED_DATA path
- [x] No separate SSDT entry -- `NtQuerySystemTime` (0x00F0) already returns precise time; Win8+ `GetSystemTimePreciseAsFileTime` uses the same syscall
- [x] Commit: `"kernel: time -- KeQuerySystemTimePrecise sub-microsecond wall time"`

---

## 11. Timezone Bias and DST Management
`GetLocalTime()` and FAT32 timestamps require a local-time offset. Store the bias in the registry (or a kernel global until registry is ready).

- [x] `struct tz_info` with `bias_minutes`, `dst_bias_minutes`, `dst_active` in `timezone.h`
- [x] `timezone_init()`: defaults to UTC; registry read deferred until registry integration
- [x] `timezone_set()` / `timezone_get()` / `timezone_total_bias()`
- [x] `filetime_to_local(utc)` / `filetime_from_local(local)` -- apply total bias (bias + DST)
- [x] Commit: `"kernel: time -- timezone bias and DST management"`

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
- [ ] Win11 24H2 KUSD QPC fast-path fields (gap-audit): populate `BaselineSystemTimeQpc`, `QpcSystemTimeIncrement`, `QpcBias`, `QpcBypassEnabled` so user-mode `QueryPerformanceCounter` reads the page syscall-free.
- [ ] Reconcile custom ABI header collision (gap-audit): `kusd.h` `AbiMagic` at 0x340 overlaps the Win11 24H2 QPC block (0x340-0x3c7); relocate past the real Windows tail -> XREF: D02 T04 §18.
- [x] Commit: `"kernel: time -- KUSER_SHARED_DATA time field updates from timer ISR"`

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

---

## 14. Suspend/Hibernate Time Bias Tracking
When the system enters S3 (suspend-to-RAM) or S4 (hibernate), the timer interrupt stops firing and `InterruptTime` freezes. On resume, the kernel must compute how long the system was asleep (from the RTC delta or UEFI time) and add that duration to `InterruptTimeBias` so that `KeQueryInterruptTime()` (which includes bias) continues to advance smoothly. `KeQueryUnbiasedInterruptTime()` deliberately excludes the bias, so it reflects actual CPU-awake time only.

- [ ] `ke_suspend_bias_update(uint64_t sleep_duration_100ns)` -- called by the S3/S4 resume path (→ XREF TODO-26); atomically adds `sleep_duration_100ns` to `g_interrupt_time_bias`
- [ ] On resume: read UEFI `GetTime()` or RTC; compute delta against pre-suspend wall time; pass to `ke_suspend_bias_update()`
- [ ] Update `wall_clock_t.base_mono_ns` to account for the monotonic clock gap during suspend
- [ ] Log the bias adjustment and total sleep duration on resume
- [ ] RTC wake alarm (gap-audit): program the RTC/ACPI wake timer to resume at a future time (Win11 `RtlSetSystemWakeTime`, Linux `wakealarm`). Owned by power management. -> XREF: 02-kernel-core/TODO-26-power-management.md §3
- [ ] Commit: `"kernel: time -- suspend/hibernate time bias tracking"`

---

## 15. Leap Second Policy
FILETIME does not count leap seconds -- this matches Windows behavior exactly. The 100 ns count since 1601-01-01 assumes every day has exactly 86400 seconds. `FileTimeToSystemTime` returns seconds 0–59 only; the 60th second during a leap event is never represented. This must be explicitly documented and enforced so that FILETIME arithmetic and round-trip conversions remain correct.

- [ ] Add `FILETIME_LEAP_SECOND_POLICY` comment block to `filetime.h` documenting that leap seconds are not counted (SI seconds, not UTC seconds)
- [ ] Ensure `filetime_from_unix_seconds()` does NOT apply leap second corrections -- Unix time also ignores leap seconds (POSIX mandates 86400 s/day)
- [ ] Ensure `filetime_to_string()` never outputs `:60` seconds
- [ ] Add a unit test: `filetime_from_unix_seconds(1483228799)` (2016-12-31T23:59:59Z, a leap second boundary) produces the correct value and `filetime_to_string` shows `23:59:59`, not `23:59:60`
- [ ] Commit: `"kernel: time -- document and enforce leap second policy"`

---

## 16. Coarse Time Fast Path
Hot paths like klog timestamping, scheduler accounting, and network packet timestamping call `KeQuerySystemTime()` thousands of times per second. A "coarse" variant avoids the seqlock read and TSC access by returning the last ISR-cached value. Linux provides `ktime_get_coarse()` for the same reason -- 10x faster than the precise variant, accurate to the timer tick period.

> [!TIP]
> Neither Windows nor Linux exposes a single API that explicitly distinguishes "coarse kernel time" -- Windows uses KUSER_SHARED_DATA reads implicitly, and Linux has `ktime_get_coarse()` internally. Impossible OS can provide a clean `KeQuerySystemTimeCoarse()` API for kernel drivers.

- [ ] Implement `KeQuerySystemTimeCoarse(FILETIME *out)` -- returns the last ISR-cached SystemTime value (no TSC read, no seqlock)
- [ ] Implement `KeQueryInterruptTimeCoarse()` -- returns the cached `g_interrupt_time` directly
- [ ] Migrate `kusd_update_time()` (every timer tick) to the coarse time variants -- the precise path does slow per-tick hardware reads in the ISR on PMTMR (port) + HPET (UC MMIO) (owns the §2/§4 per-tick ISR cost).
- [ ] Update `klog()` to use `KeQuerySystemTimeCoarse()` for timestamps once the time service is ready
- [ ] Migrate raw `system_get_ticks()` timestamp/window consumers to a rebased time source (`uptime_ns()` or the coarse API): `etw.c` event timestamps, `klog.c` boot/ratelimit stamps, `registry.c` header stamps, `wer.c` crash path, `virtio/blk_init.c`+`blk_telemetry.c` adaptive windows -- raw lifetime ticks are reinterpreted when `KeSetTimerResolution` changes the tick rate (filed from `01-boot-platform/TODO-11` §6 review; `uptime()` itself already migrated)
- [ ] Commit: `"kernel: time -- coarse time fast path for hot-path callers"`

---

## 17. NTP Clock Adjustment Hooks
The NTP protocol client (network stack TODO) needs a kernel interface to correct both the wall clock phase (offset) and frequency (skew). Provide the hooks now so the network stack can call them later.

- [ ] Define `ntp_adj_t` in `include/kernel/time/ntp_adj.h`:
  - `int64_t  offset_ns` -- signed nanosecond correction to apply to the wall clock phase
  - `int32_t  freq_ppb` -- parts-per-billion frequency correction (positive = clock running fast)
- [ ] Implement `ke_ntp_adjtime(const ntp_adj_t *adj)`:
  - Phase step: if `|offset_ns| > 1s`, call `KeSetSystemTime()` directly (step)
  - Slew: if `|offset_ns| <= 1s`, record the correction and spread it over the next N ticks by adjusting the WALL-CLOCK conversion only (the `base_time`/`base_mono_ns` anchor), never `mono_ns()` itself
  - Frequency: store `freq_ppb`; apply it to the wall-clock FILETIME conversion ONLY. MUST NOT touch `mono_filetime_units()` / `KeQueryPerformanceCounter` (gap-audit [H]): QPC + interrupt time are RAW monotonic (Win11 contract: QPC is independent of system time / Windows Time; Linux keeps `CLOCK_MONOTONIC_RAW` un-disciplined). NTP discipline applies to wall time only.
- [ ] Add `ke_ntp_get_status(struct ntp_status *out)` -- returns current offset, freq, last-sync FILETIME, and sync source string
- [ ] Document the hook contract: NTP client calls `ke_ntp_adjtime()` from thread context only; kernel does not initiate network calls
- [ ] Commit: `"kernel: time -- NTP clock phase and frequency adjustment hooks"`

---

## 18. Clocksource Quality Watchdog
Continuously cross-check the active monotonic clock source against a reference and demote a drifting source (an unstable TSC) to HPET/PMTMR, matching Linux `clocksource.c` (watchdog ~every 0.5 s, "Marking TSC unstable") and the Win11 HAL silent demotion. Replaces the boot-time `platform_is_tcg()` gate in `timer_hal_init()`. Split out of §2 by the gap-audit + design review.

- [ ] Boot-time source qualification (design review [H]): a conservative qualify-or-reject step in `timer_hal_init()` (Phase 1) replacing `platform_is_tcg()`; the ISR may sample only -- no drift compare/demotion/klog in interrupt context.
- [ ] Ongoing watchdog on the kworker pool (design review [H]): a Phase-3 PASSIVE periodic job reads the active source + a reference (HPET/PMTMR), computes drift over a window, and demotes past a threshold at thread context.
- [ ] Atomic coherent demotion (design review [H]): publish the active clocksource as ONE snapshot (seqlock or atomic descriptor swap covering source + num/den + mask + read-fn + epoch) so lock-free `mono_ns()` never sees a mixed source/scale.
- [ ] Anchor the new source to the old `mono_ns()` value on switch so monotonic time never jumps backward across a demotion.
- [ ] PMTMR epoch refresh across tick quiesce (review [H]): call `mono_clock_pmtmr_advance()` before masking the timer (UEFI runtime) + after resume so a quiesce past the ~4.7 s wrap loses no wraps (the §2 floor already blocks backward steps).
- [ ] Log the demotion with old/new source + measured drift in ppm.
- [ ] Commit: `"kernel: time -- clocksource quality watchdog with drift demotion"`

**Test checkpoint:** Unit (`TEST_CAT_SCHED`): the demotion snapshot swap is monotonic (a reader sampling across a simulated swap never sees time decrease); the drift compare classifies an injected > N ppm skew as unstable. Runtime (WHPX / bare metal serial, not unit-testable without live drift): boot logs the qualified source; an unstable TSC logs `"clocksource: demoting TSC -> HPET (N ppm)"`. Verify on QEMU WHPX, TCG, VirtualBox, bare metal.

---

## OS Comparison

| ⭐ | Feature                  | 🪟 Win11                 | 🐧 Linux                  | 🚀 Impossible OS          |
|----|--------------------------|---------------------------|---------------------------|----------------------------|
| 💎 | 100 ns wall time         | ✅ FILETIME API          | ⚠️ timespec, diff epoch   | ✅ §1,§5,§6 done          |
| 💎 | Monotonic counter        | ✅ QPC via TSC/HPET      | ✅ CLOCK_MONOTONIC vDSO   | ⚠️ §2 done, §3-§4 pending |
| 💎 | Invariant TSC detect     | ✅ CPUID 0x15            | ✅ tsc_khz calibration    | ⬜ §3                     |
| 💎 | Per-CPU TSC sync         | ✅ TSC sync at INIT      | ✅ check_tsc_sync         | ⬜ §3                     |
| 💎 | Clocksource watchdog     | ✅ HAL silent demote     | ✅ clocksource.c watchdog | ⬜ §18 -- drift demotion  |
| 💎 | Ordered TSC read         | ✅ QPC abstracts rdtscp  | ✅ rdtsc_ordered LFENCE   | ⬜ §3 -- bare rdtsc now   |
| 💎 | HPET fallback            | ✅ when TSC unreliable   | ✅ hpet_clocksource       | ✅ §4 -- hpet.c driver    |
| 💎 | Wall clock UEFI/RTC      | ✅ GetSystemTime         | ✅ efi_get_time           | ✅ §5 -- wall_clock_init  |
| 💎 | Interrupt time           | ✅ KeQueryInterruptTime  | ✅ CLOCK_BOOTTIME         | ✅ §7 -- biased+unbiased  |
| 💎 | Timer resolution         | ✅ NtSetTimerResolution  | ✅ timer_settime NO_HZ    | ✅ §8 -- SSDT 0xF3/0xF4   |
| 💎 | Precise wall time        | ✅ PreciseAsFileTime     | ✅ CLOCK_REALTIME vDSO    | ✅ §10 -- TSC interpolated|
| 💎 | Timezone + DST           | ✅ registry TZ info      | ✅ /etc/localtime         | ⬜ §11                    |
| 💎 | KUSD / vDSO time page    | ✅ KUSD 0x7FFE0000       | ✅ vDSO clock_gettime     | ✅ §12 -- ISR-updated     |
| 💎 | FAT32 timestamps         | ✅ kernel32 -> FAT dir   | ✅ fat inode time         | ✅ §13 -- FILETIME-based  |
| 💎 | NTFS FILETIME            | ✅ $STANDARD_INFO        | ✅ ntfs3 current_time     | ⬜ §13                    |
| 💎 | Suspend time bias        | ✅ InterruptTimeBias     | ✅ CLOCK_BOOTTIME         | ⬜ §14                    |
| 💎 | Leap second policy       | ✅ skips leap seconds    | ✅ 86400 s/day            | ⬜ §15                    |
| 💎 | NTP adjustment           | ✅ W32tm + SetSystemTime | ✅ adjtimex syscall       | ⬜ §17                    |
| ⭐ | Fixed 10 MHz QPC         | ⚠️ varies by hardware    | ❌ no fixed-freq API      | ✅ §9 -- 10 MHz fixed     |
| ⭐ | Coarse time API          | ⚠️ implicit KUSD         | ⚠️ ktime_get_coarse       | ⬜ §16 -- explicit API    |

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
