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

| ⭐  | Order | Deliverable                                                            | Depends On        | Status |
| --- | :---: | ---------------------------------------------------------------------- | ----------------- | :----: |
| 💎  |   1   | `FILETIME` type, epoch constants, and conversion math                  | --                 |  [ ]   |
| 💎  |   2   | Monotonic nanosecond clock source selection                            | 1                 |  [ ]   |
| 💎  |   3   | Invariant TSC detection and per-CPU offset calibration                 | 2                 |  [ ]   |
| 💎  |   4   | HPET standalone driver                                                 | --                 |  [ ]   |
| 💎  |   5   | Wall clock init from UEFI GetTime / RTC                                | 1, 2              |  [ ]   |
| 💎  |   6   | Kernel time service (`KeQuerySystemTime`, `KeSetSystemTime`)           | 5                 |  [ ]   |
| 💎  |   7   | Interrupt time and unbiased interrupt time APIs                        | 2, 6              |  [ ]   |
| 💎  |   8   | Timer resolution management (`NtSetTimerResolution`)                   | 6, TODO-05 §4     |  [ ]   |
| 💎  |   9   | `NtQuerySystemTime` / `NtSetSystemTime` / `NtQueryPerformanceCounter`  | 6, TODO-05 §4     |  [ ]   |
| 💎  |  10   | Precise system time (`KeQuerySystemTimePrecise`)                       | 6, 2              |  [ ]   |
| 💎  |  11   | Timezone bias and DST management                                       | 6                 |  [ ]   |
| 💎  |  12   | KUSER_SHARED_DATA time field updates from timer ISR                    | 6, 7, TODO-04 §11 |  [ ]   |
| 💎  |  13   | Filesystem timestamp encoding (FAT32 + NTFS)                           | 6, 11             |  [ ]   |
| 💎  |  14   | Suspend/hibernate time bias tracking                                   | 7, TODO-15 §3,§4  |  [ ]   |
| 💎  |  15   | Leap second policy                                                     | 1                 |  [ ]   |
| ⭐  |  16   | Coarse time fast path (lock-free cached time)                          | 6                 |  [ ]   |
| ⭐  |  17   | NTP clock adjustment hooks                                             | 6                 |  [ ]   |

> 💎 = parity -- Windows NT and Linux both provide these capabilities.
> ⭐ = exclusive -- coarse time gives O(1) cached reads for hot-path callers without reading hardware; NTP hook API is a first-class kernel-level adjustment interface, not a userspace-only workaround.

---

## 1. `FILETIME` Type, Epoch Constants, and Conversion Math
- [ ] Create `include/kernel/nt/filetime.h`:
  - `typedef uint64_t FILETIME` -- 100 ns intervals since `1601-01-01T00:00:00Z`
  - `FILETIME_EPOCH_OFFSET_SECONDS  11644473600ULL` -- delta from Unix epoch (Jan 1, 1970) to FILETIME epoch
  - `FILETIME_EPOCH_OFFSET_100NS    116444736000000000ULL` -- same offset in 100 ns units
  - `FILETIME_TICKS_PER_SECOND      10000000ULL` -- 100 ns ticks per second
  - `FILETIME_TICKS_PER_MS          10000ULL`
  - `FILETIME_TICKS_PER_US          10ULL`
- [ ] Implement pure conversion helpers (no hardware access):
  - `filetime_from_unix_seconds(uint64_t unix_sec)` -- convert Unix epoch seconds to FILETIME
  - `filetime_to_unix_seconds(FILETIME ft)` -- convert FILETIME to Unix epoch seconds
  - `filetime_from_rtc(const struct rtc_time *t)` -- convert CMOS RTC struct to FILETIME (UTC, no timezone)
  - `filetime_from_efi_time(const struct efi_time *t)` -- convert UEFI `EFI_TIME` to FILETIME (apply timezone offset)
  - `filetime_to_dos_datetime(FILETIME ft, int tz_bias_minutes, uint16_t *date_out, uint16_t *time_out)` -- FAT32 DOS date/time encoding in local time
  - `filetime_from_dos_datetime(uint16_t date, uint16_t time, int tz_bias_minutes)` -- FAT32 → FILETIME
- [ ] Add `FILETIME_NOW_PLACEHOLDER` sentinel for callers before the wall clock is initialized
- [ ] Unit-test the epoch offset: `filetime_from_unix_seconds(0)` must equal `116444736000000000ULL`
- [ ] Commit: `"kernel: nt -- FILETIME type, epoch constants, and conversion math"`

## 2. Monotonic Nanosecond Clock Source Selection
Select the highest-resolution monotonic source available: invariant TSC → HPET → LAPIC counter.

- [ ] Create `include/kernel/time/mono_clock.h` and `src/kernel/time/mono_clock.c`:
  - `uint64_t mono_ns(void)` -- returns nanoseconds since boot (never wraps for 584 years)
  - `uint64_t mono_filetime_units(void)` -- returns 100 ns units since boot (for FILETIME arithmetic)
- [ ] Selection priority at `mono_clock_init()`:
  1. Invariant TSC (`CPU_FEATURE_RDTSCP` + `IA32_TSC_INVARIANT` CPUID bit) -- read via `RDTSC`/`RDTSCP` with known frequency from `boot_timing_tsc_freq()`
  2. HPET (§4) -- use main counter directly if TSC fails or frequency unknown
  3. LAPIC tick count -- last resort using `lapic_timer_ticks_per_ms()`
- [ ] Store selected source ID, its frequency, and a scale factor (numerator/denominator pair for integer multiply-shift conversion without division in the hot path)
- [ ] `mono_clock_source_name()` -- returns string `"TSC"`, `"HPET"`, or `"LAPIC"` for log/diagnostics
- [ ] Commit: `"kernel: time -- monotonic nanosecond clock source selection"`

## 3. Invariant TSC Detection and Per-CPU Offset Calibration
On systems with invariant TSC (`CPUID 0x80000007 EDX[8]`), the TSC ticks at a constant rate regardless of C-states or frequency scaling, and is synchronized by the firmware across all cores at reset. Verify this and apply correction offsets where needed.

- [ ] Detect invariant TSC via `CPUID 0x80000007 EDX[8]` in `cpuid_init()` or `mono_clock_init()`; log result
- [ ] Read TSC frequency from CPUID 0x15 (`TSC/crystal ratio`) if available; cross-check with `boot_timing_tsc_freq()`; use the CPUID value if both agree within 1%
- [ ] On SMP boot: measure per-AP TSC delta against BSP via a synchronized INIT-IPI + RDTSC exchange:
  - BSP records `T_bsp = RDTSC` and broadcasts a known-delay rendezvous
  - Each AP records `T_ap = RDTSC` at the rendezvous; computes `offset = T_bsp - T_ap`
  - Store per-CPU TSC offset in the per-CPU structure; `mono_ns()` adds this offset on AP cores
- [ ] If invariant TSC is absent (pre-Nehalem): fall back to HPET or LAPIC; log a warning
- [ ] Provide `rdtsc_ns()` as a fast inline using the pre-computed scale factor from §2
- [ ] Commit: `"kernel: time -- invariant TSC detection and per-CPU offset calibration"`

## 4. HPET Standalone Driver
The HPET provides a single 64-bit main counter that increments at a fixed frequency (typically 14–100 MHz). Used as the QPC fallback source when invariant TSC is absent.

- [ ] Create `include/kernel/drivers/hpet.h` and `src/kernel/drivers/hpet.c`
- [ ] Read HPET MMIO base from `acpi_get_hpet_base()`; map into kernel address space; log if absent
- [ ] Read `GCAP_ID` register: extract main counter period (femtoseconds per tick) and compute frequency
- [ ] Enable the main counter: set `ENABLE_CNF` bit in `GEN_CONF` register; `LEG_RT_CNF` only if needed
- [ ] Implement `hpet_read_counter(void)` -- reads `MAIN_CNT_VAL` register (64-bit)
- [ ] Implement `hpet_ns(void)` -- converts counter value to nanoseconds using precomputed scale
- [ ] Expose `hpet_available()` and `hpet_frequency_hz()` for use by `mono_clock_init()` §2
- [ ] Do NOT use HPET timer comparators here -- comparators are for the HPET timer TODO in `03-memory-concurrency`; this section covers the main counter only
- [ ] Commit: `"kernel: drivers -- HPET main counter driver"`

## 5. Wall Clock Init from UEFI GetTime / RTC
Seed the kernel wall clock at boot. The wall clock is a `FILETIME` anchor point paired with the monotonic counter reading at that instant.

- [ ] Create `include/kernel/time/wall_clock.h` and `src/kernel/time/wall_clock.c`
- [ ] Define `wall_clock_t` -- `FILETIME base_time` (UTC) + `uint64_t base_mono_ns` (monotonic reading at seeding)
- [ ] `wall_clock_init()`:
  1. Try `uefi_get_time()` -- if `UEFI_SUCCESS` and year is plausible (2000–2100), call `filetime_from_efi_time()` to get UTC FILETIME, record `base_mono_ns = mono_ns()`
  2. Fallback: `rtc_read()` + `filetime_from_rtc()` -- assume UTC; log that UEFI time was unavailable
  3. Log the seeded wall time and source on the serial channel
- [ ] `KeQuerySystemTime(FILETIME *out)` -- computes `base_time + (mono_ns() - base_mono_ns) / 100` -- lock-free read
- [ ] `KeSetSystemTime(FILETIME new_time)` -- updates `base_time` and re-latches `base_mono_ns`; acquires a seqlock write for concurrent readers
- [ ] Protect `wall_clock_t` with `seqlock_t` (already in `include/kernel/sched/seqlock.h`) for lock-free reads and safe writes
- [ ] Commit: `"kernel: time -- wall clock init from UEFI GetTime with RTC fallback"`

## 6. Kernel Time Service
Provide the stable kernel-level time API used by everything above PASSIVE_LEVEL: filesystems, logging, scheduler, and the native API layer.

- [ ] Ensure `KeQuerySystemTime()` is callable from any IRQL (lock-free seqlock read path; §5)
- [ ] Add `KeQueryTickCount(uint64_t *tick_count)` -- returns 100 ns tick count since boot (same as `mono_filetime_units()`)
- [ ] Add `KeQueryTimeIncrement(uint32_t *increment)` -- returns the periodic interrupt increment in 100 ns units (set at timer HAL init from `1/timer_freq * 10^7`)
- [ ] Add `KeDelayExecutionThread(KIRQL irql, bool alertable, FILETIME *interval)` stub -- uses `sleep_ms()` internally until full APC alertable waits exist
- [ ] Expose `time_service_ready()` predicate for use by `BOOT_REQUIRE` gates
- [ ] Commit: `"kernel: time -- kernel time service API"`

## 7. Interrupt Time and Unbiased Interrupt Time APIs
Windows exposes interrupt time (100 ns since boot, including sleep bias) and unbiased interrupt time (excluding sleep bias) as core kernel APIs. `GetTickCount64()`, scheduler deadlines, and driver timeouts all depend on these. Linux's `CLOCK_BOOTTIME` (includes suspend) vs `CLOCK_MONOTONIC` (excludes suspend) is the equivalent split.

- [ ] Implement `KeQueryInterruptTime()` -- returns 100 ns units since boot, updated each timer tick; includes suspend bias so it always advances even across S3/S4
- [ ] Implement `KeQueryInterruptTimePrecise(uint64_t *qpc_value)` -- same but interpolated to sub-tick precision using the monotonic clock (TSC/HPET); also returns QPC value at the same instant
- [ ] Implement `KeQueryUnbiasedInterruptTime(uint64_t *unbiased)` -- returns interrupt time minus `InterruptTimeBias`; pauses during suspend (equivalent to Linux `CLOCK_MONOTONIC`)
- [ ] Track `g_interrupt_time` as a volatile 64-bit counter incremented by `KeTimeIncrement` in the timer ISR
- [ ] Track `g_interrupt_time_bias` as the cumulative time added for suspend compensation (initially 0; updated by §14)
- [ ] Commit: `"kernel: time -- interrupt time and unbiased interrupt time APIs"`

## 8. Timer Resolution Management
Windows allows processes to request higher timer interrupt frequency (down to 0.5 ms) via `NtSetTimerResolution` / `timeBeginPeriod`. This affects scheduler quantum, `Sleep()` granularity, and multimedia timing. Win11 scopes this per-process; background/occluded processes don't get elevated resolution.

- [ ] Define `timer_resolution_t` in `include/kernel/time/timer_resolution.h`:
  - `uint32_t minimum_resolution` -- hardware minimum in 100 ns units (typically 5000 = 0.5 ms)
  - `uint32_t maximum_resolution` -- default in 100 ns units (typically 156250 = 15.625 ms)
  - `uint32_t current_resolution` -- active resolution (highest request wins)
- [ ] `KeSetTimerResolution(uint32_t desired_100ns, bool set)` -- kernel API; `set=true` requests, `set=false` releases
- [ ] `NtSetTimerResolution(ULONG DesiredTime, BOOLEAN SetResolution, PULONG ActualTime)` -- SSDT 0x00F4; validates user pointer; calls `KeSetTimerResolution`
- [ ] Per-process tracking: each process stores its requested resolution; on process exit, automatically release
- [ ] Global arbitration: the shortest requested resolution from any active process becomes `current_resolution`; update the timer ICR when resolution changes
- [ ] `NtQueryTimerResolution(PULONG MaximumTime, PULONG MinimumTime, PULONG CurrentTime)` -- SSDT 0x00F3; returns the three resolution values
- [ ] Commit: `"kernel: time -- timer resolution management (NtSetTimerResolution)"`

## 9. `NtQuerySystemTime`, `NtSetSystemTime`, `NtQueryPerformanceCounter`
Register Win32-named syscalls in the SSDT (→ XREF TODO-05 §4).

- [ ] `NtQuerySystemTime(FILETIME *SystemTime)`: call `KeQuerySystemTime()`; validate user pointer at CPL=3
- [ ] `NtSetSystemTime(FILETIME *SystemTime, FILETIME *PreviousTime)`: call `KeSetSystemTime()`; requires `SE_SYSTEMTIME_PRIVILEGE`; optional previous-time out-param
- [ ] `NtQueryPerformanceCounter(LARGE_INTEGER *PerformanceCount, LARGE_INTEGER *PerformanceFrequency)`:
  - `PerformanceCount` = current `mono_filetime_units()` value (or raw TSC/HPET scaled to 100 ns)
  - `PerformanceFrequency` = `FILETIME_TICKS_PER_SECOND` (10,000,000) -- fixed; avoids apps having to handle variable QPC frequency
- [ ] Register in the SSDT with stable service numbers consistent with NT 6.x numbering
- [ ] Commit: `"kernel: nt -- NtQuerySystemTime, NtSetSystemTime, NtQueryPerformanceCounter"`

## 10. Precise System Time (`KeQuerySystemTimePrecise`)
`GetSystemTimePreciseAsFileTime` (Win8+) returns sub-microsecond UTC wall time by interpolating between timer ticks using the TSC/HPET performance counter. Without this, `KeQuerySystemTime()` is only accurate to the timer tick period (1–15 ms). Databases, distributed systems, and logging all need sub-microsecond timestamps.

- [ ] Implement `KeQuerySystemTimePrecise(FILETIME *out)`:
  - Read `KeQuerySystemTime()` base value (last tick's wall time)
  - Read TSC/HPET via `mono_ns()` and compute the fractional tick elapsed since the last timer interrupt
  - Add the fractional offset to the base value for sub-microsecond precision
- [ ] Expose as `NtQuerySystemTimePrecise` syscall (user-mode equivalent of `GetSystemTimePreciseAsFileTime`)
- [ ] Commit: `"kernel: time -- KeQuerySystemTimePrecise sub-microsecond wall time"`

## 11. Timezone Bias and DST Management
`GetLocalTime()` and FAT32 timestamps require a local-time offset. Store the bias in the registry (or a kernel global until registry is ready).

- [ ] Define `struct tz_info` in `include/kernel/time/timezone.h`:
  - `int32_t  bias_minutes` -- UTC offset in minutes (negative = west; e.g. UTC-5 = -300)
  - `int32_t  dst_bias_minutes` -- additional DST offset (typically 60 or 0)
  - `uint8_t  dst_active` -- 1 if DST is currently in effect
- [ ] `timezone_init()`: read from `HKLM\SYSTEM\CurrentControlSet\Control\TimeZoneInformation\Bias` if registry is ready; default to UTC (bias=0) if not
- [ ] `timezone_set(const struct tz_info *tz)` -- updates the kernel-global and persists to registry when available
- [ ] `timezone_get(struct tz_info *out)` -- copies current settings
- [ ] `filetime_to_local(FILETIME utc, const struct tz_info *tz)` -- applies bias + DST to produce local FILETIME
- [ ] `filetime_from_local(FILETIME local, const struct tz_info *tz)` -- inverse
- [ ] Commit: `"kernel: time -- timezone bias and DST management"`

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

| ⭐ | Feature                                | 🪟 Win11                                       | 🐧 Linux                                         | 🚀 Impossible OS                                 |
|----|----------------------------------------|-----------------------------------------------|--------------------------------------------------|--------------------------------------------------|
| 💎 | 100 ns epoch-anchored wall time        | ✅ `FILETIME`, `GetSystemTimeAsFileTime()`    | ⚠️ `timespec` (ns) since 1970; different        | ⬜ §1, §5–§6                                    |
| 💎 | High-resolution monotonic counter      | ✅ `QueryPerformanceCounter` via TSC/HPET     | ✅ `clock_gettime(CLOCK_MONOTONIC)` via vDSO    | ⬜ §2–§4                                        |
| 💎 | Invariant TSC frequency detection      | ✅ CPUID 0x15 + TSC_INVARIANT                 | ✅ `tsc_khz` calibration at boot                | ⬜ §3                                           |
| 💎 | Per-CPU TSC synchronization on SMP     | ✅ TSC synchronization at INIT                | ✅ `check_tsc_sync_source()` + offset           | ⬜ §3                                           |
| 💎 | HPET driver for QPC fallback           | ✅ used when TSC unreliable                   | ✅ `hpet_clocksource`                           | ⬜ §4                                           |
| 💎 | Wall clock from UEFI GetTime           | ✅ `EfiGetVariable` / `GetSystemTime()`       | ✅ `efi_get_time()`                             | ⬜ §5                                           |
| 💎 | Interrupt time (biased + unbiased)     | ✅ `KeQueryInterruptTime` / `Unbiased`        | ✅ `CLOCK_BOOTTIME` / `CLOCK_MONOTONIC`         | ⬜ §7                                           |
| 💎 | Timer resolution control               | ✅ `NtSetTimerResolution` (per-process)       | ✅ `timer_settime()` + `NO_HZ`                  | ⬜ §8                                           |
| 💎 | Sub-microsecond precise wall time      | ✅ `GetSystemTimePreciseAsFileTime`           | ✅ `clock_gettime(CLOCK_REALTIME)` via vDSO     | ⬜ §10                                          |
| 💎 | Timezone + DST offset management       | ✅ registry `TimeZoneInformation`             | ✅ `/etc/localtime` + kernel `sys_tz`           | ⬜ §11                                          |
| 💎 | KUSD / vDSO time page (no syscall)     | ✅ `KUSER_SHARED_DATA` at 0x7FFE0000         | ✅ vDSO `clock_gettime`                         | ⬜ §12                                          |
| 💎 | FAT32 local-time timestamps            | ✅ kernel32 → FAT32 dir entry                 | ✅ fat32 inode time                              | ⬜ §13                                          |
| 💎 | NTFS UTC FILETIME timestamps           | ✅ `$STANDARD_INFORMATION` fields             | ✅ ntfs3 `current_time()` → FILETIME            | ⬜ §13                                          |
| 💎 | Suspend/hibernate time bias            | ✅ `InterruptTimeBias` on resume              | ✅ `CLOCK_BOOTTIME` accounts for suspend        | ⬜ §14                                          |
| 💎 | Leap second policy (not counted)       | ✅ FILETIME skips leap seconds                | ✅ POSIX mandates 86400 s/day                   | ⬜ §15                                          |
| 💎 | NTP kernel adjustment interface        | ✅ `W32tm` → `NtSetSystemTime`                | ✅ `adjtimex()` syscall (phase + freq)          | ⬜ §17                                          |
| ⭐ | Fixed QPC frequency (10 MHz)           | ⚠️ varies by hardware                        | ❌ no fixed-frequency monotonic API             | ⬜ §9 -- stable, hardware-independent            |
| ⭐ | Explicit coarse time API               | ⚠️ implicit via KUSD reads                   | ⚠️ `ktime_get_coarse()` (internal only)        | ⬜ §16 -- `KeQuerySystemTimeCoarse()`            |

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
