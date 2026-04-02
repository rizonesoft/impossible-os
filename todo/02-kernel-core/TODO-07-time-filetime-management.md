# TODO-07 — Time & FILETIME Management

> **Goal:** Build the kernel time layer that all Win32 APIs, filesystems, logging, and the scheduler depend on. This means: a `FILETIME` type with correct epoch and 100 ns resolution; a monotonic nanosecond clock backed by invariant TSC or HPET; a wall clock seeded from UEFI `GetTime()` (RTC fallback); `NtQuerySystemTime`/`NtSetSystemTime`; `QueryPerformanceCounter`; timezone/DST bias; and filesystem timestamp encoders (FAT32 local-time / NTFS FILETIME). Without this, every `CreateFile` timestamp, every log entry, and every scheduler deadline is either zero or fabricated.

> [!IMPORTANT]
> **Current state:** `timer.c` provides `sleep_ms()`, `system_get_ticks()`, and `uptime()` in whole seconds. `boot_timing_tsc_freq()` gives the bootloader-measured TSC frequency. `uefi_get_time()` can return wall time with nanosecond precision and timezone data. `acpi_get_hpet_base()` returns the HPET MMIO base. `rtc_read()` gives CMOS date/time as a fallback. None of these are connected into a unified time service and there is no `FILETIME`, no wall clock, and no `QueryPerformanceCounter`.

## Inputs

- [`include/kernel/timer.h`](../../include/kernel/timer.h), [`src/kernel/timer.c`](../../src/kernel/timer.c)
- [`include/kernel/drivers/rtc.h`](../../include/kernel/drivers/rtc.h), [`src/kernel/drivers/rtc.c`](../../src/kernel/drivers/rtc.c)
- [`include/kernel/uefi_runtime.h`](../../include/kernel/uefi_runtime.h) — `efi_time`, `uefi_get_time()`, `uefi_set_time()`
- [`include/kernel/boot_timing.h`](../../include/kernel/boot_timing.h) — `boot_timing_tsc_freq()`
- [`include/kernel/acpi.h`](../../include/kernel/acpi.h) — `acpi_get_hpet_base()`
- [`include/kernel/drivers/lapic.h`](../../include/kernel/drivers/lapic.h) — calibrated ticks/ms
- [`include/kernel/cpuid.h`](../../include/kernel/cpuid.h) — `CPU_FEATURE_RDTSCP`, invariant TSC detection
- [`src/kernel/fs/fat32/fat32_ops.c`](../../src/kernel/fs/fat32/fat32_ops.c) — current FAT32 timestamp callsites
- [`src/kernel/fs/ntfs/ntfs_metadata.c`](../../src/kernel/fs/ntfs/ntfs_metadata.c) — NTFS timestamp callsites
- [`src/kernel/fs/ntfs/ntfs_data_write.c`](../../src/kernel/fs/ntfs/ntfs_data_write.c) — NTFS data write timestamp callsites
- → XREF: `TODO-01-kernel-init-sequencing.md §4` — time service init (`wall_clock_init()`) belongs in Phase 2 (§4) after UEFI runtime; NTP wall clock adjustment belongs in Phase 3 (§5); `wall_clock_init()` is not yet listed in §4's checklist — add before implementing
- → XREF: `TODO-05-native-api-ssdt.md §4` — SSDT registration; time syscalls (`NtQuerySystemTime`, `NtSetSystemTime`, `NtQueryPerformanceCounter`, `NtQueryTimerResolution`) are added to the SSDT table in §7 of this TODO
- → XREF: `TODO-06-irql-model-dpcs.md §6` — Timer/APIC scheduling path (§6) drives monotonic tick accumulation via `KiDispatchDpc()`; `DISPATCH_LEVEL` clock interrupt is the tick source
- → XREF: `TODO-06-irql-model-dpcs.md §9` — Timer-DPC association: KTIMER objects carry an optional KDPC pointer; when the timer fires, the DPC is auto-queued via `KeInsertQueueDpc`
- → XREF: `TODO-04-peb-teb-user-abi.md §11` — KUSER_SHARED_DATA time fields (SystemTime, InterruptTime, QpcFrequency) are populated by §6 of this TODO; `kusd_update_time()` is called from the timer ISR

## Outcome

- `FILETIME` is a first-class kernel type: 100 ns intervals since `1601-01-01T00:00:00Z`.
- `KeQuerySystemTime()` returns UTC wall time as a `FILETIME` from anywhere in the kernel.
- `KeQueryPerformanceCounter()` returns a monotonic 64-bit counter backed by invariant TSC (HPET fallback, LAPIC last resort).
- Per-CPU TSC offset correction is applied on SMP so `RDTSC` reads are coherent across cores.
- The wall clock is seeded from UEFI `GetTime()` at boot, or CMOS RTC if UEFI time is unavailable.
- `NtQuerySystemTime`, `NtSetSystemTime`, `NtQueryPerformanceCounter`, and `NtQueryPerformanceFrequency` are implemented and registered in the SSDT.
- FAT32 writes use the correctly computed DOS date/time in local time.
- NTFS writes use the correctly computed FILETIME in UTC.
- Timezone bias and DST adjustment are configurable and applied by `GetLocalTime()` and FAT32.
- NTP adjustment hooks exist so a future network time client can correct the wall clock and frequency.

## Implementation Order

| ⭐  | Order | Deliverable                                                           | Depends On        | Status |
| --- | :---: | --------------------------------------------------------------------- | ----------------- | :----: |
| 💎  |   1   | `FILETIME` type, epoch constants, and conversion math                 | —                 |  [ ]   |
| 💎  |   2   | Monotonic nanosecond clock source selection                           | 1                 |  [ ]   |
| 💎  |   3   | Invariant TSC detection and per-CPU offset calibration                | 2                 |  [ ]   |
| 💎  |   4   | HPET standalone driver                                                | —                 |  [ ]   |
| 💎  |   5   | Wall clock init from UEFI GetTime / RTC                               | 1, 2              |  [ ]   |
| 💎  |   6   | Kernel time service (`KeQuerySystemTime`, `KeSetSystemTime`)          | 5                 |  [ ]   |
| 💎  |   7   | `NtQuerySystemTime` / `NtSetSystemTime` / `NtQueryPerformanceCounter` | 6, TODO-05 §4     |  [ ]   |
| 💎  |   8   | Timezone bias and DST management                                      | 6                 |  [ ]   |
| 💎  |   9   | Filesystem timestamp encoding (FAT32 + NTFS)                          | 6, 8              |  [ ]   |
| ⭐  |  10   | NTP clock adjustment hooks                                            | 6                 |  [ ]   |

> 💎 = parity — Windows NT and Linux both provide these capabilities.
> ⭐ = exclusive — the NTP hook API is a first-class kernel-level adjustment interface, not a userspace-only workaround.

---

## 1. `FILETIME` Type, Epoch Constants, and Conversion Math `[Sonnet]`

- [ ] Create `include/kernel/nt/filetime.h`:
  - `typedef uint64_t FILETIME` — 100 ns intervals since `1601-01-01T00:00:00Z`
  - `FILETIME_EPOCH_OFFSET_SECONDS  11644473600ULL` — delta from Unix epoch (Jan 1, 1970) to FILETIME epoch
  - `FILETIME_EPOCH_OFFSET_100NS    116444736000000000ULL` — same offset in 100 ns units
  - `FILETIME_TICKS_PER_SECOND      10000000ULL` — 100 ns ticks per second
  - `FILETIME_TICKS_PER_MS          10000ULL`
  - `FILETIME_TICKS_PER_US          10ULL`
- [ ] Implement pure conversion helpers (no hardware access):
  - `filetime_from_unix_seconds(uint64_t unix_sec)` — convert Unix epoch seconds to FILETIME
  - `filetime_to_unix_seconds(FILETIME ft)` — convert FILETIME to Unix epoch seconds
  - `filetime_from_rtc(const struct rtc_time *t)` — convert CMOS RTC struct to FILETIME (UTC, no timezone)
  - `filetime_from_efi_time(const struct efi_time *t)` — convert UEFI `EFI_TIME` to FILETIME (apply timezone offset)
  - `filetime_to_dos_datetime(FILETIME ft, int tz_bias_minutes, uint16_t *date_out, uint16_t *time_out)` — FAT32 DOS date/time encoding in local time
  - `filetime_from_dos_datetime(uint16_t date, uint16_t time, int tz_bias_minutes)` — FAT32 → FILETIME
- [ ] Add `FILETIME_NOW_PLACEHOLDER` sentinel for callers before the wall clock is initialized
- [ ] Unit-test the epoch offset: `filetime_from_unix_seconds(0)` must equal `116444736000000000ULL`
- [ ] Commit: `"kernel: nt — FILETIME type, epoch constants, and conversion math"`

## 2. Monotonic Nanosecond Clock Source Selection `[Sonnet]`

Select the highest-resolution monotonic source available: invariant TSC → HPET → LAPIC counter.

- [ ] Create `include/kernel/time/mono_clock.h` and `src/kernel/time/mono_clock.c`:
  - `uint64_t mono_ns(void)` — returns nanoseconds since boot (never wraps for 584 years)
  - `uint64_t mono_filetime_units(void)` — returns 100 ns units since boot (for FILETIME arithmetic)
- [ ] Selection priority at `mono_clock_init()`:
  1. Invariant TSC (`CPU_FEATURE_RDTSCP` + `IA32_TSC_INVARIANT` CPUID bit) — read via `RDTSC`/`RDTSCP` with known frequency from `boot_timing_tsc_freq()`
  2. HPET (§4) — use main counter directly if TSC fails or frequency unknown
  3. LAPIC tick count — last resort using `lapic_timer_ticks_per_ms()`
- [ ] Store selected source ID, its frequency, and a scale factor (numerator/denominator pair for integer multiply-shift conversion without division in the hot path)
- [ ] `mono_clock_source_name()` — returns string `"TSC"`, `"HPET"`, or `"LAPIC"` for log/diagnostics
- [ ] Commit: `"kernel: time — monotonic nanosecond clock source selection"`

## 3. Invariant TSC Detection and Per-CPU Offset Calibration `[Opus]`

On systems with invariant TSC (`CPUID 0x80000007 EDX[8]`), the TSC ticks at a constant rate regardless of C-states or frequency scaling, and is synchronized by the firmware across all cores at reset. Verify this and apply correction offsets where needed.

- [ ] Detect invariant TSC via `CPUID 0x80000007 EDX[8]` in `cpuid_init()` or `mono_clock_init()`; log result
- [ ] Read TSC frequency from CPUID 0x15 (`TSC/crystal ratio`) if available; cross-check with `boot_timing_tsc_freq()`; use the CPUID value if both agree within 1%
- [ ] On SMP boot: measure per-AP TSC delta against BSP via a synchronized INIT-IPI + RDTSC exchange:
  - BSP records `T_bsp = RDTSC` and broadcasts a known-delay rendezvous
  - Each AP records `T_ap = RDTSC` at the rendezvous; computes `offset = T_bsp - T_ap`
  - Store per-CPU TSC offset in the per-CPU structure; `mono_ns()` adds this offset on AP cores
- [ ] If invariant TSC is absent (pre-Nehalem): fall back to HPET or LAPIC; log a warning
- [ ] Provide `rdtsc_ns()` as a fast inline using the pre-computed scale factor from §2
- [ ] Commit: `"kernel: time — invariant TSC detection and per-CPU offset calibration"`

## 4. HPET Standalone Driver `[Sonnet]`

The HPET provides a single 64-bit main counter that increments at a fixed frequency (typically 14–100 MHz). Used as the QPC fallback source when invariant TSC is absent.

- [ ] Create `include/kernel/drivers/hpet.h` and `src/kernel/drivers/hpet.c`
- [ ] Read HPET MMIO base from `acpi_get_hpet_base()`; map into kernel address space; log if absent
- [ ] Read `GCAP_ID` register: extract main counter period (femtoseconds per tick) and compute frequency
- [ ] Enable the main counter: set `ENABLE_CNF` bit in `GEN_CONF` register; `LEG_RT_CNF` only if needed
- [ ] Implement `hpet_read_counter(void)` — reads `MAIN_CNT_VAL` register (64-bit)
- [ ] Implement `hpet_ns(void)` — converts counter value to nanoseconds using precomputed scale
- [ ] Expose `hpet_available()` and `hpet_frequency_hz()` for use by `mono_clock_init()` §2
- [ ] Do NOT use HPET timer comparators here — comparators are for the HPET timer TODO in `03-memory-concurrency`; this section covers the main counter only
- [ ] Commit: `"kernel: drivers — HPET main counter driver"`

## 5. Wall Clock Init from UEFI GetTime / RTC `[Sonnet]`

Seed the kernel wall clock at boot. The wall clock is a `FILETIME` anchor point paired with the monotonic counter reading at that instant.

- [ ] Create `include/kernel/time/wall_clock.h` and `src/kernel/time/wall_clock.c`
- [ ] Define `wall_clock_t` — `FILETIME base_time` (UTC) + `uint64_t base_mono_ns` (monotonic reading at seeding)
- [ ] `wall_clock_init()`:
  1. Try `uefi_get_time()` — if `UEFI_SUCCESS` and year is plausible (2000–2100), call `filetime_from_efi_time()` to get UTC FILETIME, record `base_mono_ns = mono_ns()`
  2. Fallback: `rtc_read()` + `filetime_from_rtc()` — assume UTC; log that UEFI time was unavailable
  3. Log the seeded wall time and source on the serial channel
- [ ] `KeQuerySystemTime(FILETIME *out)` — computes `base_time + (mono_ns() - base_mono_ns) / 100` — lock-free read
- [ ] `KeSetSystemTime(FILETIME new_time)` — updates `base_time` and re-latches `base_mono_ns`; acquires a seqlock write for concurrent readers
- [ ] Protect `wall_clock_t` with `seqlock_t` (already in `include/kernel/sched/seqlock.h`) for lock-free reads and safe writes
- [ ] Commit: `"kernel: time — wall clock init from UEFI GetTime with RTC fallback"`

## 6. Kernel Time Service `[Sonnet]`

Provide the stable kernel-level time API used by everything above PASSIVE_LEVEL: filesystems, logging, scheduler, and the native API layer.

- [ ] Ensure `KeQuerySystemTime()` is callable from any IRQL (lock-free seqlock read path; §5)
- [ ] Add `KeQueryTickCount(uint64_t *tick_count)` — returns 100 ns tick count since boot (same as `mono_filetime_units()`)
- [ ] Add `KeQueryTimeIncrement(uint32_t *increment)` — returns the periodic interrupt increment in 100 ns units (set at timer HAL init from `1/timer_freq * 10^7`)
- [ ] Add `KeDelayExecutionThread(KIRQL irql, bool alertable, FILETIME *interval)` stub — uses `sleep_ms()` internally until full APC alertable waits exist
- [ ] Expose `time_service_ready()` predicate for use by `BOOT_REQUIRE` gates
- [ ] Commit: `"kernel: time — kernel time service API"`

## 7. `NtQuerySystemTime`, `NtSetSystemTime`, `NtQueryPerformanceCounter`, `NtQueryTimerResolution` `[Sonnet]`

Register Win32-named syscalls in the SSDT (→ XREF TODO-05 §4).

- [ ] `NtQuerySystemTime(FILETIME *SystemTime)`: call `KeQuerySystemTime()`; validate user pointer at CPL=3
- [ ] `NtSetSystemTime(FILETIME *SystemTime, FILETIME *PreviousTime)`: call `KeSetSystemTime()`; requires `SE_SYSTEMTIME_PRIVILEGE`; optional previous-time out-param
- [ ] `NtQueryPerformanceCounter(LARGE_INTEGER *PerformanceCount, LARGE_INTEGER *PerformanceFrequency)`:
  - `PerformanceCount` = current `mono_filetime_units()` value (or raw TSC/HPET scaled to 100 ns)
  - `PerformanceFrequency` = `FILETIME_TICKS_PER_SECOND` (10,000,000) — fixed; avoids apps having to handle variable QPC frequency
- [ ] `NtQueryTimerResolution(ULONG *MaximumResolution, ULONG *MinimumResolution, ULONG *CurrentResolution)`:
  - Returns timer tick resolution in 100 ns units; `CurrentResolution = KeQueryTimeIncrement()`
- [ ] Register all four in the SSDT with stable service numbers consistent with NT 6.x numbering
- [ ] Commit: `"kernel: nt — NtQuerySystemTime, NtSetSystemTime, NtQueryPerformanceCounter"`

## 8. Timezone Bias and DST Management `[Sonnet]`

`GetLocalTime()` and FAT32 timestamps require a local-time offset. Store the bias in the registry (or a kernel global until registry is ready).

- [ ] Define `struct tz_info` in `include/kernel/time/timezone.h`:
  - `int32_t  bias_minutes` — UTC offset in minutes (negative = west; e.g. UTC-5 = -300)
  - `int32_t  dst_bias_minutes` — additional DST offset (typically 60 or 0)
  - `uint8_t  dst_active` — 1 if DST is currently in effect
- [ ] `timezone_init()`: read from `HKLM\SYSTEM\CurrentControlSet\Control\TimeZoneInformation\Bias` if registry is ready; default to UTC (bias=0) if not
- [ ] `timezone_set(const struct tz_info *tz)` — updates the kernel-global and persists to registry when available
- [ ] `timezone_get(struct tz_info *out)` — copies current settings
- [ ] `filetime_to_local(FILETIME utc, const struct tz_info *tz)` — applies bias + DST to produce local FILETIME
- [ ] `filetime_from_local(FILETIME local, const struct tz_info *tz)` — inverse
- [ ] Commit: `"kernel: time — timezone bias and DST management"`

## 9. Filesystem Timestamp Encoding (FAT32 + NTFS) `[Sonnet]`

Replace all zero/stub timestamps in FAT32 and NTFS with correctly computed values.

- [ ] **FAT32**: DOS date/time is local time, 2-second resolution. Fix all `fat32_write` callsites:
  - Replace any `0` or placeholder timestamp with `filetime_to_dos_datetime(KeQuerySystemTime(), tz_bias, &date, &time)`
  - Verify `DIR_CrtTime`, `DIR_WrtTime`, and `DIR_LstAccDate` are all populated on create and update
- [ ] **NTFS**: uses 8-byte FILETIME in UTC. Fix `ntfs_metadata.c` and `ntfs_data_write.c` callsites:
  - `$STANDARD_INFORMATION` attributes: `$Created`, `$Modified`, `$MFT_Modified`, `$Accessed` must use `KeQuerySystemTime()`
  - Ensure all four fields are distinct where semantically required (create ≠ modify on creation)
- [ ] **klog_disk**: if log entries currently use tick count or zero, replace with FILETIME timestamp string
- [ ] Add a `filetime_to_string(FILETIME ft, char *buf, size_t len)` helper — ISO 8601 UTC format: `"2026-03-25T14:35:22.123Z"` — for logging and debug output
- [ ] Commit: `"kernel: fs — wire FILETIME timestamps into FAT32, NTFS, and klog"`

## 10. NTP Clock Adjustment Hooks `[Opus]`

The NTP protocol client (network stack TODO) needs a kernel interface to correct both the wall clock phase (offset) and frequency (skew). Provide the hooks now so the network stack can call them later.

- [ ] Define `ntp_adj_t` in `include/kernel/time/ntp_adj.h`:
  - `int64_t  offset_ns` — signed nanosecond correction to apply to the wall clock phase
  - `int32_t  freq_ppb` — parts-per-billion frequency correction (positive = clock running fast)
- [ ] Implement `ke_ntp_adjtime(const ntp_adj_t *adj)`:
  - Phase step: if `|offset_ns| > 1s`, call `KeSetSystemTime()` directly (step)
  - Slew: if `|offset_ns| <= 1s`, record the correction and spread it over the next N ticks by adjusting the tick-to-FILETIME scale factor (slew rate)
  - Frequency: store `freq_ppb`; apply as a bias to `mono_filetime_units()` scale computation
- [ ] Add `ke_ntp_get_status(struct ntp_status *out)` — returns current offset, freq, last-sync FILETIME, and sync source string
- [ ] Document the hook contract: NTP client calls `ke_ntp_adjtime()` from thread context only; kernel does not initiate network calls
- [ ] Commit: `"kernel: time — NTP clock phase and frequency adjustment hooks"`

---

## OS Comparison


| ⭐ | Feature                               | 🪟 Win11                                      | 🐧 Linux                                            | 🚀 Impossible OS                                  |
|----|---------------------------------------|--------------------------------------------|--------------------------------------------------|------------------------------------------------|
| 💎 | 100 ns epoch-anchored wall time       | ✅ `FILETIME`, `GetSystemTimeAsFileTime()` | ⚠️ `timespec` (ns) since 1970; different         | ⬜ §1 — , §5–§6                                |
| 💎 | High-resolution monotonic counter     | ✅ `QueryPerformanceCounter` via TSC/HPET  | ✅ `clock_gettime(CLOCK_MONOTONIC)` via vDSO     | ⬜ §2–§4                                       |
| 💎 | Invariant TSC frequency detection     | ✅ CPUID 0x15 + TSC_INVARIANT              | ✅ `tsc_khz` calibration at boot                 | ⬜ §3                                          |
| 💎 | Per-CPU TSC synchronization on SMP    | ✅ TSC synchronization at INIT             | ✅ `check_tsc_sync_source()` + offset correction | ⬜ §3                                          |
| 💎 | HPET driver for QPC fallback          | ✅ used when TSC unreliable                | ✅ `hpet_clocksource`                            | ⬜ §4                                          |
| 💎 | Wall clock from UEFI GetTime          | ✅ `EfiGetVariable` / `GetSystemTime()`    | ✅ `efi_get_time()` in drivers/firmware/efi/     | ⬜ §5                                          |
| 💎 | Timezone + DST offset management      | ✅ registry `TimeZoneInformation`          | ✅ `/etc/localtime` + kernel `sys_tz`            | ⬜ §8                                          |
| 💎 | FAT32 local-time timestamps on write  | ✅ kernel32 → FAT32 dir entry              | ✅ fat32 inode time via `current_time()`         | ⬜ §9                                          |
| 💎 | NTFS UTC FILETIME timestamps on write | ✅ `$STANDARD_INFORMATION` FILETIME fields | ✅ ntfs3 uses `current_time()` → FILETIME        | ⬜ §9                                          |
| 💎 | NTP kernel adjustment interface       | ✅ `W32tm` → `NtSetSystemTime` + clock     | ✅ `adjtimex()` syscall (phase + freq)           | ⬜ §10                                         |
| ⭐ | Fixed QPC frequency                   | ⚠️ frequency varies by hardware (not       | ❌ no fixed-frequency monotonic API              | ⬜ §7 — stable, portable, hardware-independent |

> **After §1–§9:** Impossible OS matches Windows NT exactly on FILETIME semantics, QPC, timezone handling, and filesystem timestamp accuracy.
> **§7** locks `QueryPerformanceFrequency` to 10 MHz (FILETIME ticks/second), making it constant and hardware-independent — Windows still returns variable hardware frequencies and apps must handle this; Linux has no equivalent fixed-frequency API.

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
  - `filetime_to_local` with `bias_minutes = -300` subtracts 5 hours from UTC FILETIME
  - `filetime_from_local` is inverse of `filetime_to_local` for same timezone
  - `filetime_to_string` produces ISO 8601 format string with `T` and `Z` markers
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
- [ ] FAT32 directory entry timestamps after file write show current date/time in local time, not zero
- [ ] NTFS `$STANDARD_INFORMATION` timestamps after file create show current UTC FILETIME, not zero
- [ ] klog entries carry an ISO 8601 timestamp prefix
- [ ] `ke_ntp_adjtime()` with a +500 ms phase offset advances the wall clock by 500 ms within the slew window
- [ ] Commit: `"kernel: time — FILETIME, wall clock, QPC, timezone, and filesystem timestamps"`
