# TODO-07 -- Time & FILETIME Management

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
- → XREF: `TODO-13-registry-completion.md §1.3` -- registry `LastWriteTime` conversion from PIT ticks to FILETIME uses `ticks_to_filetime()` from §2
- [`src/kernel/fs/ntfs/ntfs_data_write.c`](../../src/kernel/fs/ntfs/ntfs_data_write.c) -- NTFS data write timestamp callsites
- → XREF: `TODO-01-kernel-init-sequencing.md §4` -- time service init (`wall_clock_init()`) belongs in Phase 2 (§4) after UEFI runtime; NTP wall clock adjustment belongs in Phase 3 (§5); `wall_clock_init()` is not yet listed in §4's checklist -- add before implementing
- → XREF: `TODO-05-native-api-ssdt.md §4` -- SSDT registration; time syscalls (`NtQuerySystemTime`, `NtSetSystemTime`, `NtQueryPerformanceCounter`) in §9, timer resolution (`NtSetTimerResolution`, `NtQueryTimerResolution`) in §8
- → XREF: `TODO-06-irql-model-dpcs.md §6` -- Timer/APIC scheduling path (§6) drives monotonic tick accumulation via `KiDispatchDpc()`; `DISPATCH_LEVEL` clock interrupt is the tick source
- → XREF: `TODO-06-irql-model-dpcs.md §9` -- Timer-DPC association: KTIMER objects carry an optional KDPC pointer; when the timer fires, the DPC is auto-queued via `KeInsertQueueDpc`
- → XREF: `TODO-04-peb-teb-user-abi.md §11` -- KUSER_SHARED_DATA time fields (SystemTime, InterruptTime, QpcFrequency) are populated by §12 of this TODO; `kusd_update_time()` is called from the timer ISR
- → XREF: `TODO-15-power-management.md §3,§4` -- S3/S4 resume path calls `ke_suspend_bias_update()` (§14 of this TODO) to adjust `InterruptTimeBias` by the sleep duration
- → XREF: `TODO-05-native-api-ssdt.md §18` -- `NtSetTimerResolution` (SSDT 0x00F4) is implemented in §8 of this TODO

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
| 💎  |   3   | Invariant TSC detection and per-CPU offset calibration                 | §2                  |  [x]   |
| 💎  |   4   | HPET standalone driver                                                 | --                  |  [x]   |
| 💎  |   5   | Wall clock init from UEFI GetTime / RTC                                | §1, §2              |  [x]   |
| 💎  |   6   | Kernel time service (`KeQuerySystemTime`, `KeSetSystemTime`)           | §5                  |  [x]   |
| 💎  |   7   | Interrupt time and unbiased interrupt time APIs                        | §2, §6              |  [x]   |
| 💎  |   8   | Timer resolution management (`NtSetTimerResolution`)                   | §6, TODO-05 §4      |  [x]   |
| 💎  |   9   | `NtQuerySystemTime` / `NtSetSystemTime` / `NtQueryPerformanceCounter`  | §6, TODO-05 §4      |  [x]   |
| 💎  |  10   | Precise system time (`KeQuerySystemTimePrecise`)                       | §6, §2              |  [x]   |
| 💎  |  11   | Timezone bias and DST management                                       | §6                  |  [x]   |
| 💎  |  12   | KUSER_SHARED_DATA time field updates from timer ISR                    | §6, §7, TODO-04 §11 |  [ ]   |
| 💎  |  13   | Filesystem timestamp encoding (FAT32 + NTFS)                           | §6, §11             |  [ ]   |
| 💎  |  14   | Suspend/hibernate time bias tracking                                   | §7, TODO-15 §3,§4   |  [ ]   |
| 💎  |  15   | Leap second policy                                                     | §1                  |  [ ]   |
| ⭐  |  16   | Coarse time fast path (lock-free cached time)                          | §6                  |  [ ]   |
| ⭐  |  17   | NTP clock adjustment hooks                                             | §6                  |  [ ]   |

> 💎 = parity -- Windows NT and Linux both provide these capabilities.
> ⭐ = exclusive -- coarse time gives O(1) cached reads for hot-path callers without reading hardware; NTP hook API is a first-class kernel-level adjustment interface, not a userspace-only workaround.

---

## 1. `FILETIME` Type, Epoch Constants, and Conversion Math
- [x] Created `include/kernel/nt/filetime.h` with `typedef uint64_t FILETIME`, epoch constants, tick constants, `FILETIME_NOW_PLACEHOLDER`
- [x] Inline helpers: `filetime_from_unix_seconds()`, `filetime_to_unix_seconds()`, `filetime_days_from_date()`, `filetime_to_dos_datetime()`, `filetime_from_dos_datetime()`
- [x] Non-inline (in `src/kernel/nt/filetime.c`): `filetime_from_rtc()`, `filetime_from_efi_time()` -- need full struct definitions
- [x] `FILETIME_NOW_PLACEHOLDER = 0` sentinel defined
- [x] 4 unit tests: epoch offset, round-trip, DOS date round-trip, ticks/sec constants
- [x] Commit: `"kernel: nt -- FILETIME type, epoch constants, and conversion math"`

## 2. Monotonic Nanosecond Clock Source Selection
Select the highest-resolution monotonic source available: invariant TSC → HPET → LAPIC counter.

- [x] Created `include/kernel/time/mono_clock.h` + `src/kernel/time/mono_clock.c`: `mono_ns()`, `mono_filetime_units()`, `mono_clock_source_name()`, `mono_clock_source_id()`
- [x] Selection: invariant TSC (CPU_FEATURE_TSC_INV + boot_timing_tsc_freq) -> LAPIC fallback (system_get_ticks). HPET stub ready for §4.
- [x] Scale factor: num/den pair for integer multiply without hot-path division; overflow-safe split for TSC
- [x] Logs selected source: `"Monotonic clock: TSC (N MHz, invariant)"` or `"LAPIC (N ticks/ms)"`
- [x] Commit: `"kernel: time -- monotonic nanosecond clock source selection"`

## 3. Invariant TSC Detection and Per-CPU Offset Calibration
On systems with invariant TSC (`CPUID 0x80000007 EDX[8]`), the TSC ticks at a constant rate regardless of C-states or frequency scaling, and is synchronized by the firmware across all cores at reset. Verify this and apply correction offsets where needed.

- [x] Invariant TSC detected via `CPU_FEATURE_TSC_INV` (already in cpuid.c); `mono_clock_init()` uses it for source selection
- [x] CPUID 0x15 TSC/crystal cross-check: `mono_clock_crosscheck_tsc()` compares CPUID value with `boot_timing_tsc_freq()`; logs OK or warns if >1% mismatch
- [x] Per-CPU `tsc_offset` field added to `per_cpu_data` struct; `mono_ns()` and `rdtsc_ns()` apply offset on AP cores
- [x] Per-AP TSC sync via IPI: deferred -- modern firmware synchronizes invariant TSC at reset; offset starts at 0; IPI sync will be added if real hardware shows drift
- [x] Fallback: if invariant TSC absent, falls through to LAPIC in `mono_clock_init()` with log warning
- [x] `rdtsc_ns()`: fast TSC read with per-CPU offset + overflow-safe scale conversion
- [x] Commit: `"kernel: time -- invariant TSC detection and per-CPU offset calibration"`

## 4. HPET Standalone Driver
The HPET provides a single 64-bit main counter that increments at a fixed frequency (typically 14–100 MHz). Used as the QPC fallback source when invariant TSC is absent.

- [x] Created `include/kernel/drivers/hpet.h` + `src/kernel/drivers/hpet.c`: `hpet_init()`, `hpet_available()`, `hpet_frequency_hz()`, `hpet_read_counter()`, `hpet_ns()`
- [x] Maps HPET MMIO as UC via `vmm_map_mmio_uc()`; reads `GCAP_ID` for period (fs/tick); computes frequency
- [x] Enables main counter via `ENABLE_CNF` bit; validates probe read and period range
- [x] Wired into `mono_clock_init()` as priority 2 (TSC > HPET > LAPIC) and `mono_ns()` switch
- [x] No timer comparators -- main counter only (comparators for scheduler timer TODO)
- [x] Commit: `"kernel: drivers -- HPET main counter driver"`

## 5. Wall Clock Init from UEFI GetTime / RTC
Seed the kernel wall clock at boot. The wall clock is a `FILETIME` anchor point paired with the monotonic counter reading at that instant.

- [x] Created `include/kernel/time/wall_clock.h` + `src/kernel/time/wall_clock.c`
- [x] `wall_clock_init()`: tries UEFI GetTime (year 2000-2100 plausibility check), falls back to RTC; logs seeded time with ISO-style format and source name
- [x] `KeQuerySystemTime()`: returns `base_time + (mono_ns() - base_mono_ns) / 100` -- seqlock lock-free read
- [x] `KeSetSystemTime(new_time)`: updates anchor + re-latches mono_ns(); seqlock write-protected
- [x] `wall_clock_ready()`: returns 1 after init
- [x] Protected by `seqlock_t` (SEQLOCK_INIT) for SMP-safe concurrent reads
- [x] Commit: `"kernel: time -- wall clock init from UEFI GetTime with RTC fallback"`

## 6. Kernel Time Service
Provide the stable kernel-level time API used by everything above PASSIVE_LEVEL: filesystems, logging, scheduler, and the native API layer.

- [x] `KeQuerySystemTime()` -- lock-free seqlock read, callable from any IRQL (verified in §5)
- [x] `KeQueryTickCount(tick_count)` -- returns `mono_filetime_units()` (100 ns since boot)
- [x] `KeQueryTimeIncrement(increment)` -- returns 100000 (10 ms at 100 Hz in 100 ns units)
- [x] `KeDelayExecutionThread(interval)` -- converts 100 ns units to ms, calls `sleep_ms()`
- [x] `time_service_ready()` -- returns 1 after `wall_clock_init()`
- [x] Commit: `"kernel: time -- kernel time service API"`

## 7. Interrupt Time and Unbiased Interrupt Time APIs
Windows exposes interrupt time (100 ns since boot, including sleep bias) and unbiased interrupt time (excluding sleep bias) as core kernel APIs. `GetTickCount64()`, scheduler deadlines, and driver timeouts all depend on these. Linux's `CLOCK_BOOTTIME` (includes suspend) vs `CLOCK_MONOTONIC` (excludes suspend) is the equivalent split.

- [x] `KeQueryInterruptTime()` -- returns `mono_filetime_units() + s_interrupt_time_bias` (includes suspend)
- [x] `KeQueryInterruptTimePrecise(qpc_value)` -- same + returns QPC value at same instant
- [x] `KeQueryUnbiasedInterruptTime()` -- returns `mono_filetime_units()` only (no bias)
- [x] `s_interrupt_time_bias` tracked as volatile atomic; `ke_suspend_bias_update()` for §14 resume path
- [x] Commit: `"kernel: time -- interrupt time and unbiased interrupt time APIs"`

## 8. Timer Resolution Management
Windows allows processes to request higher timer interrupt frequency (down to 0.5 ms) via `NtSetTimerResolution` / `timeBeginPeriod`. This affects scheduler quantum, `Sleep()` granularity, and multimedia timing. Win11 scopes this per-process; background/occluded processes don't get elevated resolution.

- [x] `timer_resolution.h`: constants `TIMER_RES_DEFAULT` (156250 = 15.625 ms) and `TIMER_RES_MINIMUM` (5000 = 0.5 ms)
- [x] `KeSetTimerResolution(desired, set)`: request/release with clamping; global arbitration (shortest wins)
- [x] `NtSetTimerResolution` (SSDT 0x00F4) + `NtQueryTimerResolution` (SSDT 0x00F3) -- registered via `timer_resolution_register_ssdt()`
- [x] 16-slot request table with per-entry active flag; `arbitrate()` scans for shortest
- [x] `KeQueryTimerResolution(max, min, current)` returns all three values
- [x] Per-process tracking: moved to `TODO-09-process-model-extensions.md §14` (Process Exit Cleanup) -- releases timer resolution requests on process exit
- [x] Timer ICR update: `lapic_timer_set_hz()` reprograms LAPIC ICR when resolution changes; called from `arbitrate()` in timer_resolution.c
- [x] Commit: `"kernel: time -- timer resolution management (NtSetTimerResolution)"`

## 9. `NtQuerySystemTime`, `NtSetSystemTime`, `NtQueryPerformanceCounter`
Register Win32-named syscalls in the SSDT (→ XREF TODO-05 §4).

- [x] `NtQuerySystemTime` (SSDT 0x00F0): calls `KeQuerySystemTime()`, writes FILETIME to user pointer
- [x] `NtSetSystemTime` (SSDT 0x00F1): calls `KeSetSystemTime()`; optional previous-time out-param
- [x] `NtQueryPerformanceCounter` (SSDT 0x00F2): returns `mono_filetime_units()` with fixed 10 MHz frequency
- [x] Registered via `wall_clock_register_ssdt()` in boot Phase 3
- [x] Commit: `"kernel: nt -- NtQuerySystemTime, NtSetSystemTime, NtQueryPerformanceCounter"`

## 10. Precise System Time (`KeQuerySystemTimePrecise`)
`GetSystemTimePreciseAsFileTime` (Win8+) returns sub-microsecond UTC wall time by interpolating between timer ticks using the TSC/HPET performance counter. Without this, `KeQuerySystemTime()` is only accurate to the timer tick period (1–15 ms). Databases, distributed systems, and logging all need sub-microsecond timestamps.

- [x] `KeQuerySystemTimePrecise()`: implemented -- currently identical to `KeQuerySystemTime()` since both use `mono_ns()` TSC/HPET interpolation; will diverge when §12 adds coarse KUSER_SHARED_DATA path
- [x] No separate SSDT entry -- `NtQuerySystemTime` (0x00F0) already returns precise time; Win8+ `GetSystemTimePreciseAsFileTime` uses the same syscall
- [x] Commit: `"kernel: time -- KeQuerySystemTimePrecise sub-microsecond wall time"`

## 11. Timezone Bias and DST Management
`GetLocalTime()` and FAT32 timestamps require a local-time offset. Store the bias in the registry (or a kernel global until registry is ready).

- [x] `struct tz_info` with `bias_minutes`, `dst_bias_minutes`, `dst_active` in `timezone.h`
- [x] `timezone_init()`: defaults to UTC; registry read deferred until registry integration
- [x] `timezone_set()` / `timezone_get()` / `timezone_total_bias()`
- [x] `filetime_to_local(utc)` / `filetime_from_local(local)` -- apply total bias (bias + DST)
- [x] Commit: `"kernel: time -- timezone bias and DST management"`

## 12. KUSER_SHARED_DATA Time Field Updates
The timer ISR must update the `KUSER_SHARED_DATA` time fields (SystemTime, InterruptTime, TickCount) on every tick so user-mode code can read time without a syscall. This is the hottest path in the entire time subsystem -- `GetTickCount64()`, ntdll's `NtQuerySystemTime` fast path, and `QueryInterruptTime` all read from this shared page. Linux's vDSO serves the same purpose.

> [!NOTE]
> The shared page structure and static field init are owned by `TODO-04-peb-teb-user-abi.md §11` (`kusd_init()`). This section provides only the ISR-driven time update function that §11 wires into the timer interrupt.

- [ ] Implement `kusd_update_time()` in `src/kernel/time/kusd_time.c`:
  - Write `InterruptTime` (0x008): `g_interrupt_time` from §7
  - Write `SystemTime` (0x014): `KeQuerySystemTime()` result from §6
  - Write `TickCountQuad` (0x320): `g_interrupt_time / KeTimeIncrement`
  - Write `TimeZoneBias` (0x020): current timezone offset from §11
  - Use the `KSYSTEM_TIME` triple-write protocol: `High1Time`, `LowPart`, `High2Time` -- for 32-bit reader compatibility
- [ ] Call `kusd_update_time()` from the timer ISR after updating `g_interrupt_time`
- [ ] Volatile writes only, no locks -- this runs at `CLOCK_LEVEL` IRQL
- [ ] Commit: `"kernel: time -- KUSER_SHARED_DATA time field updates from timer ISR"`

## 13. Filesystem Timestamp Encoding (FAT32 + NTFS)
Replace all zero/stub timestamps in FAT32 and NTFS with correctly computed values.

> [!NOTE]
> **Unblocks:** `05-storage-filesystems/TODO-04-fat32-hardening-vfs-semantics.md §5` -- FAT32 has interim `fat32_stamp_create()`/`fat32_stamp_modify()` helpers using `system_get_ticks()`. After this section, replace them with `filetime_to_dos_datetime(KeQuerySystemTime(), tz_bias, ...)` for spec-correct timestamps.

- [ ] **FAT32**: DOS date/time is local time, 2-second resolution. Fix all `fat32_write` callsites:
  - Replace any `0` or placeholder timestamp with `filetime_to_dos_datetime(KeQuerySystemTime(), tz_bias, &date, &time)`
  - Verify `DIR_CrtTime`, `DIR_WrtTime`, and `DIR_LstAccDate` are all populated on create and update
- [ ] **NTFS**: uses 8-byte FILETIME in UTC. Fix `ntfs_metadata.c` and `ntfs_data_write.c` callsites:
  - `$STANDARD_INFORMATION` attributes: `$Created`, `$Modified`, `$MFT_Modified`, `$Accessed` must use `KeQuerySystemTime()`
  - Ensure all four fields are distinct where semantically required (create ≠ modify on creation)
- [ ] **klog_disk**: if log entries currently use tick count or zero, replace with FILETIME timestamp string
- [ ] Add a `filetime_to_string(FILETIME ft, char *buf, size_t len)` helper -- ISO 8601 UTC format: `"2026-03-25T14:35:22.123Z"` -- for logging and debug output
- [ ] Commit: `"kernel: fs -- wire FILETIME timestamps into FAT32, NTFS, and klog"`

## 14. Suspend/Hibernate Time Bias Tracking
When the system enters S3 (suspend-to-RAM) or S4 (hibernate), the timer interrupt stops firing and `InterruptTime` freezes. On resume, the kernel must compute how long the system was asleep (from the RTC delta or UEFI time) and add that duration to `InterruptTimeBias` so that `KeQueryInterruptTime()` (which includes bias) continues to advance smoothly. `KeQueryUnbiasedInterruptTime()` deliberately excludes the bias, so it reflects actual CPU-awake time only.

- [ ] `ke_suspend_bias_update(uint64_t sleep_duration_100ns)` -- called by the S3/S4 resume path (→ XREF TODO-15); atomically adds `sleep_duration_100ns` to `g_interrupt_time_bias`
- [ ] On resume: read UEFI `GetTime()` or RTC; compute delta against pre-suspend wall time; pass to `ke_suspend_bias_update()`
- [ ] Update `wall_clock_t.base_mono_ns` to account for the monotonic clock gap during suspend
- [ ] Log the bias adjustment and total sleep duration on resume
- [ ] Commit: `"kernel: time -- suspend/hibernate time bias tracking"`

## 15. Leap Second Policy
FILETIME does not count leap seconds -- this matches Windows behavior exactly. The 100 ns count since 1601-01-01 assumes every day has exactly 86400 seconds. `FileTimeToSystemTime` returns seconds 0–59 only; the 60th second during a leap event is never represented. This must be explicitly documented and enforced so that FILETIME arithmetic and round-trip conversions remain correct.

- [ ] Add `FILETIME_LEAP_SECOND_POLICY` comment block to `filetime.h` documenting that leap seconds are not counted (SI seconds, not UTC seconds)
- [ ] Ensure `filetime_from_unix_seconds()` does NOT apply leap second corrections -- Unix time also ignores leap seconds (POSIX mandates 86400 s/day)
- [ ] Ensure `filetime_to_string()` never outputs `:60` seconds
- [ ] Add a unit test: `filetime_from_unix_seconds(1483228799)` (2016-12-31T23:59:59Z, a leap second boundary) produces the correct value and `filetime_to_string` shows `23:59:59`, not `23:59:60`
- [ ] Commit: `"kernel: time -- document and enforce leap second policy"`

## 16. Coarse Time Fast Path
Hot paths like klog timestamping, scheduler accounting, and network packet timestamping call `KeQuerySystemTime()` thousands of times per second. A "coarse" variant avoids the seqlock read and TSC access by returning the last ISR-cached value. Linux provides `ktime_get_coarse()` for the same reason -- 10x faster than the precise variant, accurate to the timer tick period.

> [!TIP]
> Neither Windows nor Linux exposes a single API that explicitly distinguishes "coarse kernel time" -- Windows uses KUSER_SHARED_DATA reads implicitly, and Linux has `ktime_get_coarse()` internally. Impossible OS can provide a clean `KeQuerySystemTimeCoarse()` API for kernel drivers.

- [ ] Implement `KeQuerySystemTimeCoarse(FILETIME *out)` -- returns the last ISR-cached SystemTime value (no TSC read, no seqlock)
- [ ] Implement `KeQueryInterruptTimeCoarse()` -- returns the cached `g_interrupt_time` directly
- [ ] Update `klog()` to use `KeQuerySystemTimeCoarse()` for timestamps once the time service is ready
- [ ] Commit: `"kernel: time -- coarse time fast path for hot-path callers"`

## 17. NTP Clock Adjustment Hooks
The NTP protocol client (network stack TODO) needs a kernel interface to correct both the wall clock phase (offset) and frequency (skew). Provide the hooks now so the network stack can call them later.

- [ ] Define `ntp_adj_t` in `include/kernel/time/ntp_adj.h`:
  - `int64_t  offset_ns` -- signed nanosecond correction to apply to the wall clock phase
  - `int32_t  freq_ppb` -- parts-per-billion frequency correction (positive = clock running fast)
- [ ] Implement `ke_ntp_adjtime(const ntp_adj_t *adj)`:
  - Phase step: if `|offset_ns| > 1s`, call `KeSetSystemTime()` directly (step)
  - Slew: if `|offset_ns| <= 1s`, record the correction and spread it over the next N ticks by adjusting the tick-to-FILETIME scale factor (slew rate)
  - Frequency: store `freq_ppb`; apply as a bias to `mono_filetime_units()` scale computation
- [ ] Add `ke_ntp_get_status(struct ntp_status *out)` -- returns current offset, freq, last-sync FILETIME, and sync source string
- [ ] Document the hook contract: NTP client calls `ke_ntp_adjtime()` from thread context only; kernel does not initiate network calls
- [ ] Commit: `"kernel: time -- NTP clock phase and frequency adjustment hooks"`

---

## OS Comparison

| ⭐ | Feature                  | 🪟 Win11                 | 🐧 Linux                  | 🚀 Impossible OS          |
|----|--------------------------|---------------------------|---------------------------|----------------------------|
| 💎 | 100 ns wall time         | ✅ FILETIME API          | ⚠️ timespec, diff epoch   | ⚠️ §1 done, §5-§6 pending |
| 💎 | Monotonic counter        | ✅ QPC via TSC/HPET      | ✅ CLOCK_MONOTONIC vDSO   | ⚠️ §2 done, §3-§4 pending |
| 💎 | Invariant TSC detect     | ✅ CPUID 0x15            | ✅ tsc_khz calibration    | ⬜ §3                     |
| 💎 | Per-CPU TSC sync         | ✅ TSC sync at INIT      | ✅ check_tsc_sync         | ⬜ §3                     |
| 💎 | HPET fallback            | ✅ when TSC unreliable   | ✅ hpet_clocksource       | ✅ §4 -- hpet.c driver    |
| 💎 | Wall clock UEFI/RTC      | ✅ GetSystemTime         | ✅ efi_get_time           | ✅ §5 -- wall_clock_init  |
| 💎 | Interrupt time           | ✅ KeQueryInterruptTime  | ✅ CLOCK_BOOTTIME         | ✅ §7 -- biased+unbiased  |
| 💎 | Timer resolution         | ✅ NtSetTimerResolution  | ✅ timer_settime NO_HZ    | ✅ §8 -- SSDT 0xF3/0xF4   |
| 💎 | Precise wall time        | ✅ PreciseAsFileTime     | ✅ CLOCK_REALTIME vDSO    | ✅ §10 -- TSC interpolated|
| 💎 | Timezone + DST           | ✅ registry TZ info      | ✅ /etc/localtime         | ⬜ §11                    |
| 💎 | KUSD / vDSO time page    | ✅ KUSD 0x7FFE0000       | ✅ vDSO clock_gettime     | ⬜ §12                    |
| 💎 | FAT32 timestamps         | ✅ kernel32 -> FAT dir   | ✅ fat inode time         | ⬜ §13                    |
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

> Wire into `test_runner_init()` via `test_register_time()` (XREF: `00-infrastructure/TODO-03-kernel-test-framework.md`).
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
- [ ] KUSER_SHARED_DATA `InterruptTime` at offset 0x008 advances over time (user-mode read, no syscall)
- [ ] KUSER_SHARED_DATA `SystemTime` at offset 0x014 matches `NtQuerySystemTime` within one tick
- [ ] FAT32 directory entry timestamps after file write show current date/time in local time, not zero
- [ ] NTFS `$STANDARD_INFORMATION` timestamps after file create show current UTC FILETIME, not zero
- [ ] klog entries carry an ISO 8601 timestamp prefix
- [ ] `ke_ntp_adjtime()` with a +500 ms phase offset advances the wall clock by 500 ms within the slew window
- [ ] Commit: `"kernel: time -- FILETIME, wall clock, QPC, interrupt time, timer resolution, timezone, and filesystem timestamps"`
