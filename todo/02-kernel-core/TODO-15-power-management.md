# TODO-15 -- Power Management (S-States, D-States, Thermal & Idle)

> **Goal:** Implement the complete ACPI power management stack beyond the
> S5 shutdown that already works. This covers S1 CPU-halt idle, S3 suspend-to-RAM, S4 hibernate-to-disk, fast startup (hybrid shutdown / hiberboot), PCI/device D-states (D0–D3cold), runtime device idle management, the ACPI Embedded Controller (EC) driver required for every laptop, battery and AC adapter status (`_BIF`/`_BIX`/`_BST`), power button and lid-close event handling, driver power callbacks with query/veto and correct resume ordering, ACPI thermal zone management (`_TMP`/`_CRT`/`_HOT`/`_PSV`/`_ACx`) with passive and active cooling, CPU idle governor framework (C-states via `_CST`/`MWAIT`), CPU frequency scaling governor framework (HWP/CPPC/`_PSS`), connected standby (S0ix / Modern Standby), power request tracking, wake source management, and the power-plan UI. Without this, Impossible OS has no viable story on laptops or any real hardware that expects ACPI power events.

> [!IMPORTANT]
> **Current state:** `src/kernel/acpi.c` (618 lines) implements:
> RSDP→RSDT/XSDT chain walk, FADT extraction (PM1a control port, PM timer), MADT (LAPIC/IOAPIC/ISA overrides), `acpi_shutdown()` via `\_S5_` AML parse + PM1a_CNT write, and ACPI reset register. Everything else in this TODO is
> **greenfield**: no `\_S3_`/`\_S4_` parsing, no sleep-state entry, no CPU
> state save/restore, no hibernation image, no EC driver, no battery, no power button events, no device D-states.

> [!CAUTION]
> **Memory rule:** Hibernation image buffers can be multi-gigabyte -- always
> use `pmm_alloc_contiguous()` for hibernation scratch pages. Never `kmalloc` anything > 4 KiB in the suspend/hibernate paths.

> [!IMPORTANT]
> **Scope boundary with `04-drivers-hardware/TODO-04-acpi-power-management.md`:** TODO-04 is the authoritative ACPI power management implementation (ACPICA-based AML interpreter). §1, §3, §4, §5, §6, and §7 of this TODO are the *pre-ACPICA* implementation path -- they use hand-rolled AML parsing (same pattern as the existing `\_S5_` parser) and provide usable functionality before TODO-04 §1 (ACPICA) is complete. Once TODO-04 §1 lands, TODO-04 §2 (S3), §3 (battery), §5 (power button), and §8 (S4) supersede the equivalent sections here. **§2 (S1/idle thread), §8 (PCI D-states), §9 (driver callbacks), §10 (S0ix), and §11 (powercfg/UI) are kernel-core responsibilities not covered by TODO-04 and remain authoritative.**
> → XREF: `04-drivers-hardware/TODO-04-acpi-power-management.md §1` -- ACPICA integration; when complete, replaces hand-rolled AML parsing in §1 of this TODO
> → XREF: `04-drivers-hardware/TODO-04-acpi-power-management.md §2` -- authoritative S3 suspend/resume (ACPICA path); §3 here is the pre-ACPICA fallback
> → XREF: `04-drivers-hardware/TODO-04-acpi-power-management.md §3` -- authoritative battery `_BST`/`_BIF` (ACPICA path); §6 here is the pre-ACPICA fallback
> → XREF: `04-drivers-hardware/TODO-04-acpi-power-management.md §5` -- authoritative power button SCI (ACPICA path); §7 here is the pre-ACPICA fallback
> → XREF: `04-drivers-hardware/TODO-04-acpi-power-management.md §8` -- authoritative S4 hibernate (ACPICA path); §4 here is the pre-ACPICA fallback

---

## Inputs

- `src/kernel/acpi.c` -- existing ACPI parser; extend for new sleep objects
- `include/kernel/acpi.h` -- ACPI types (FADT, RSDP, MADT)
- `src/kernel/drivers/pci.c` -- PCI config space read/write (D-state §8)
- `src/kernel/sched/task.c` -- scheduler freeze for S3/S4
- `src/kernel/mm/vmm.c` -- page table save for S3 wakeup identity map
- → XREF: `TODO-06-irql-model-dpcs.md §3` -- DPCs and IRQL transitions must be quiesced before entering any sleep state; `KeLowerIrql(PASSIVE_LEVEL)` required on resume
- → XREF: `TODO-07-time-filetime-management.md §3` -- TSC must be recalibrated after S3/S0ix wake (clock drift); `acpi_pm_timer_read()` used as reference; §3 = Invariant TSC Detection and Per-CPU Offset Calibration
- → XREF: `TODO-07-time-filetime-management.md §14` -- S3/S4 resume path must call `ke_suspend_bias_update()` to adjust `InterruptTimeBias` by the sleep duration; §14 = Suspend/Hibernate Time Bias Tracking
- → XREF: `TODO-01-kernel-init-sequencing.md §3` -- S4 resume check runs early in Phase 1; must distinguish cold boot from hibernate resume via hibernation signature
- → XREF: `04-drivers-hardware/TODO-01-kernel-module-system.md §4` -- driver model HAL vtables required for USB xHCI to register power callbacks; xHCI D3cold→D0 handled via callback registered in §9
- → XREF: `05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md §1` -- IXFS WAL journal (`ixfs_journal_begin`/`commit`/`abort`) must be verified (§1 Subsystem Verification) before S4 journal-flush dependency is safe; storage driver must reach D0 before journal replay on resume
- → XREF: `TODO-05-native-api-ssdt.md §4` -- SSDT indices 0x00D7 (NtShutdownSystem) and 0x0140–0x0145 (NtSetSystemPowerState, NtInitiatePowerAction, NtPowerInformation, NtGetDevicePowerState, NtSetThreadExecutionState, NtRequestWakeupLatency) reserved for this TODO; §20 wires them into the SSDT
- → XREF: `01-boot-platform/TODO-06-interrupt-timer-arch.md §7` -- LAPIC timer recalibration required after HWP/CPPC frequency changes (§15 CPU frequency scaling)
- → XREF: `04-drivers-hardware/TODO-04-acpi-power-management.md §4` -- ACPICA-based `_PSS` P-state parsing; §15 here owns the kernel-core governor framework that consumes it
- → XREF: `04-drivers-hardware/TODO-04-acpi-power-management.md §6` -- ACPICA-based `IA32_THERM_STATUS` per-core temp; §14 here owns the ACPI thermal zone framework
- → XREF: `04-drivers-hardware/TODO-04-acpi-power-management.md §7` -- ACPICA-based `_CST` C-state parsing; §16 here owns the kernel-core idle governor
- → XREF: `03-memory-concurrency/TODO-05-scheduler-enhancement.md §9` -- `cpufreq_register_driver()` vtable consumed by §15; scheduler provides load metrics for governor
- → XREF: `02-kernel-core/TODO-19-x86-64-architecture.md §7` -- Intel hybrid P/E-core detection feeds §15 HWP/CPPC governor with core asymmetry data

---

## Outcome

- S1 (CPU halt) reduces power during idle; no visible effect on software state.
- S3 (suspend to RAM) saves/restores CPU registers and device state within
  < 2 s on modern hardware; resumes to the desktop without reboot.
- S4 (hibernate) writes a compressed RAM image to the swap/hibernate partition; resumes from power-off faster than a cold boot for typical working sets.
- Fast startup (hybrid shutdown) hibernates only the kernel session for < 5 s boot times.
- ACPI EC driver enables all ACPI battery/lid/hotkey events on laptops.
- Battery status (charge %, AC/DC, time remaining, wear level via `_BIX`) appears in the system tray.
- Power button and lid close trigger configurable actions (sleep/hibernate/ shutdown/lock) read from Registry.
- Every PCI device has a D-state machine; drivers register sleep/wake callbacks with query/veto support; resume ordering (storage before filesystem before scheduler) is enforced.
- Runtime device idle puts individual devices into low-power states when unused, without full system sleep.
- ACPI thermal zones enforce passive cooling (CPU throttle) and active cooling (fan control) with trip points (`_CRT`/`_HOT`/`_PSV`/`_ACx`).
- CPU idle governor selects optimal C-state (C1 HLT through C10 MWAIT) based on predicted idle duration and latency budget.
- CPU frequency governor switches P-states via HWP/CPPC or `_PSS`/`IA32_PERF_CTL` based on scheduler load.
- Connected Standby (S0ix) enables network-keepalive standby on supported Intel/AMD platforms.
- Power request tracking shows which applications are preventing sleep (`powercfg /requests`).
- Wake source registry provides `powercfg /lastwake` and `powercfg /waketimers` diagnostics.
- `powercfg` shell command and Power Options `sysdm.cpl` tab let users configure power plans.

---

## Implementation Order

| ⭐  | Order | Deliverable                                         | Depends On               | Status |
| --- | :---: | --------------------------------------------------- | ------------------------ | :----: |
| 💎  |   1   | §1 ACPI sleep object parsing & PM1 state machine    | --                        |  [ ]   |
| 💎  |   2   | §2 S1: CPU halt / idle thread integration           | 1                        |  [ ]   |
| 💎  |   3   | §3 S3: suspend to RAM (CPU state + driver freeze)   | 1, 2, D02T06§3           |  [ ]   |
| 💎  |   4   | §4 S4: hibernate to disk (image write + resume)     | 3                        |  [ ]   |
| 💎  |   5   | §5 ACPI Embedded Controller (EC) driver             | 1                        |  [ ]   |
| 💎  |   6   | §6 Battery & AC adapter (`_BIF`/`_BIX`/`_BST`)     | 5                        |  [ ]   |
| 💎  |   7   | §7 Power button & lid-close events                  | 5                        |  [ ]   |
| 💎  |   8   | §8 PCI device D-states (D0--D3cold)                 | 1                        |  [ ]   |
| 💎  |   9   | §9 Driver power callbacks & resume ordering         | 3, 8                     |  [ ]   |
| ⭐  |  10   | §10 Connected Standby (S0ix / Modern Standby)       | 2, 9, D02T06§3           |  [ ]   |
| 💎  |  11   | §11 Fast Startup (hybrid shutdown / hiberboot)      | 4, 9                     |  [ ]   |
| 💎  |  12   | §12 Runtime device idle management                  | 8, 9                     |  [ ]   |
| 💎  |  13   | §13 Power request tracking & wake source management | 9, 12                    |  [ ]   |
| 💎  |  14   | §14 ACPI thermal zone management                    | 5, D04T04§1              |  [ ]   |
| 💎  |  15   | §15 CPU frequency scaling governor framework        | 2, D04T04§4, D03T05§9   |  [ ]   |
| 💎  |  16   | §16 CPU idle governor framework                     | 2, D04T04§7              |  [ ]   |
| 💎  |  17   | §17 Driver power query & veto (IRP_MN_QUERY_POWER)  | 9                        |  [ ]   |
| 💎  |  18   | §18 Power plan UI & `powercfg`                      | 6, 7, 13, 14, 15, 16    |  [ ]   |
| ⭐  |  19   | §19 Energy-aware scheduling integration             | 15, 16, D02T19§7        |  [ ]   |
| 💎  |  20   | §20 Power syscalls wired to SSDT                    | §2, §6, D02T05§4        |  [ ]   |
| 💎  |  21   | Unit Tests                                          | §1--§20                  |  [ ]   |

> 💎 = parity work -- matches what Windows 11 and Linux already do.
> ⭐ = exclusive work -- Impossible OS is superior or first.
> Compact XREF notation: D=domain, T=TODO, §=section (e.g. D02T06§3 = domain 02, TODO-06, §3).

---

## 1. ACPI Sleep Object Parsing & PM1 State Machine `[Opus]`

### 1.1 Sleep type values for S1–S4

- [ ] Consolidate ACPI init: split into `acpi_platform_init()` (Phase 1: MADT/FADT parsing, existing) and `acpi_power_init()` (Phase 2: S-state discovery, new) -- moved from TODO-01 §8
- [ ] Extend `src/kernel/acpi.c` to parse `\_S1_`, `\_S3_`, and `\_S4_` AML objects using the same pattern as the existing `\_S5_` parser:
  ```c
  static uint16_t slp_typa_s1 = ACPI_SLP_TYPE_INVALID;
  static uint16_t slp_typa_s3 = ACPI_SLP_TYPE_INVALID;
  static uint16_t slp_typa_s4 = ACPI_SLP_TYPE_INVALID;
  #define ACPI_SLP_TYPE_INVALID 0xFFFF
  ```
- [ ] `acpi_parse_sleep_objects()` -- scan DSDT bytecode for `"_S1_"`, `"_S3_"`, `"_S4_"` name operations; extract `SLP_TYPa`/`SLP_TYPb` byte values from the Package; called from `acpi_init()` after DSDT is located
- [ ] `acpi_sleep_supported(n)` -- returns `true` if `slp_typa_sN != ACPI_SLP_TYPE_INVALID`; used by the power manager to populate the list of available sleep states

### 1.2 PM1 sleep entry

- [ ] `acpi_enter_sleep_state(uint8_t state)` -- generic sleep entry:
  1. Disable all non-wakeup interrupts (mask IOAPIC, disable PIC)
  2. Clear `SLP_EN` bit in PM1a_CNT
  3. Write `(slp_typa_sN << 10) | SLP_EN` to PM1a_CNT; repeat for PM1b_CNT if present
  4. `__asm__ volatile("hlt")` -- CPU stops here; wakeup resumes after this point for S1; for S3/S4 the CPU loses context and resumes at the wakeup vector

### 1.3 ACPI fixed events

- [ ] Enable the relevant PM1 fixed-event enable bits in `acpi_init()`:
  - `PWRBTN_EN (bit 8)` in PM1a_EN -- power button press
  - `SLPBTN_EN (bit 9)` in PM1a_EN -- sleep button press
  - `WAK_STS (bit 15)` in PM1a_STS -- clear wake status on resume
- [ ] `acpi_pm1_isr()` -- handle SCI interrupt (ACPI System Control Interrupt, typically IRQ 9); read PM1a_STS; dispatch to `acpi_power_button_event()` or `acpi_sleep_button_event()` (§7)

### 1.4 Commit

- [ ] Commit: `"kernel/acpi: S1/S3/S4 sleep type parsing, PM1 state machine, fixed-event ISR"`

---

## 2. S1: CPU Halt / Idle Thread Integration `[Sonnet]`

### 2.1 S1 entry

- [ ] S1 is a low-latency power-saving state: the CPU executes `HLT` but retains all register state and cache; system bus power is reduced
- [ ] `acpi_enter_s1()`:
  - Call `acpi_enter_sleep_state(1)`
  - On resume (next interrupt wakes the CPU): re-enable interrupts and return immediately -- no state restore needed for S1
- [ ] S1 is entered only if `acpi_sleep_supported(1)`; otherwise fall back to a plain `HLT` loop

### 2.2 Idle thread integration

- [ ] Replace the scheduler's idle busy-wait loop with a power-aware path:
  ```c
  void sched_idle_cpu(void) {
      while (true) {
          if (pm_deep_idle_allowed())
              acpi_enter_s1();     /* halts until next interrupt */
          else
              __asm__ volatile ("hlt"); /* plain halt */
      }
  }
  ```
- [ ] `pm_deep_idle_allowed()` returns true if: no pending DPCs, no high-priority runnable tasks, S1 sleep type is available, and the `PowerIdleEnable` Registry value is non-zero
- [ ] Per-CPU idle tracking: accumulate `idle_tsc_cycles` counter per CPU; exposed via `NtQuerySystemInformation(SystemProcessorIdleInformation)` for power-usage telemetry

### 2.3 Commit

- [ ] Commit: `"kernel/acpi: S1 CPU halt, idle thread power-saving integration"`

---

## 3. S3: Suspend to RAM `[Opus]`

### 3.1 Pre-suspend sequence

- [ ] `pm_enter_s3()` -- called by the power manager when the user requests sleep; runs at `DISPATCH_LEVEL` (→ XREF `TODO-06-irql-model-dpcs.md §3`):
  1. Broadcast `PO_CB_SYSTEM_STATE_LOCK` to all registered power callbacks (§9): let drivers flush queues and reach D3hot/D3cold (§8)
  2. Freeze the scheduler (`sched_freeze_all()`) -- no new threads start; all CPUs except the one doing suspend park themselves at a spin barrier
  3. Flush VFS page cache and IXFS journal (→ XREF `05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md §1`)
  4. Save APIC state (LVT registers, LAPIC base MSR) to a per-CPU save area
  5. Save `IDTR`, `GDTR`, `CR0`, `CR3`, `CR4`, `EFER` per CPU
  6. Save all CPU general-purpose and SSE registers for the BSP (`struct s3_cpu_state` allocated in pinned physical memory)
  7. Write the physical address of `pm_s3_wakeup_entry` into the ACPI wakeup vector (`FACS->FirmwareWakingVector`)
  8. Call `acpi_enter_sleep_state(3)` -- system loses power to RAM row refresh; wake on power button / RTC alarm resumes at §3.2

### 3.2 S3 wakeup path

- [ ] `pm_s3_wakeup_entry` (real-mode compatible entry stub in `src/kernel/acpi_wakeup.asm`):
  - BIOS/UEFI firmware jumps here in real mode; stub switches to protected and then long mode (re-using the bootloader's page tables at `0x70000`)
  - Calls `pm_s3_resume()` in C with the saved state pointer
- [ ] `pm_s3_resume()`:
  1. Restore IDTR, GDTR, CR0, CR3, CR4, EFER, APIC from save area
  2. Re-initialise IOAPIC routing (MADT-based)
  3. Restore BSP general-purpose + SSE registers
  4. Wake AP CPUs: write `INIT`→`SIPI`→`SIPI` IPI sequence; each AP restores its own saved state and un-parks from the spin barrier
  5. Recalibrate TSC (→ XREF `TODO-07-time-filetime-management.md §3`) -- PM timer used as reference
  6. Compute sleep duration from RTC/UEFI time delta; call `ke_suspend_bias_update()` (→ XREF `TODO-07-time-filetime-management.md §14`)
  7. Call `pm_notify_resume()` (§9) -- drivers transition back D3→D0
  8. Unfreeze scheduler; resume from the instruction after `acpi_enter_sleep_state(3)`

### 3.3 Wakeup sources

- [ ] Power button physical press → PM1 fixed event (§1.3) generates SCI; firmware raises the CPU from S3
- [ ] RTC alarm: `acpi_set_wakeup_alarm(seconds)` -- programs CMOS RTC alarm registers (port 0x70/0x71), sets `RTC_EN` in PM1a_EN; used for timed wake (→ `Task Scheduler` integration, future)
- [ ] USB device activity: `XHCI_S3_WAKEUP_EN` -- xHCI remote-wakeup enable bit in the USB port status register (→ XREF `04-drivers-hardware/TODO-09-usb-stack.md`)

### 3.4 Commit

- [ ] Commit: `"kernel/acpi: S3 suspend-to-RAM, wakeup vector, CPU state save/restore, AP re-init"`

---

## 4. S4: Hibernate to Disk `[Opus]`

### 4.1 Hibernation image format

- [ ] Hibernation image header in `include/kernel/pm/hibernate.h`:
  ```c
  #define HIBER_MAGIC  0x4945424F524150 /* "RAPOBRIE" -- "Reboot Impossible" */
  typedef struct {
      uint64_t magic;
      uint64_t kernel_version;    /* must match resume kernel */
      uint64_t image_pages;       /* number of 4 KiB pages saved */
      uint64_t resume_cr3;        /* page table root to restore */
      uint64_t resume_rsp;        /* kernel stack pointer */
      uint64_t resume_rip;        /* resume return address */
      uint64_t checksum;          /* CRC32C of all pages */
      uint8_t  reserved[4032];    /* pad to 4 KiB */
  } HIBR_HEADER;
  ```
- [ ] Pages saved: all physical pages that are in use (PMM used-bit scan) excluding the hibernation scratch buffer itself
- [ ] Compression: LZ4 block compression (`src/libs/miniz` or a simple LZ4 kernel implementation) applied per 64-page (256 KiB) chunk; reduces image size by ~50% for typical workloads

### 4.2 Writing the hibernation image

- [ ] `pm_hibernate_write()`:
  1. Pre-suspend sequence identical to §3.1 (drivers to D3, scheduler freeze, journal flush)
  2. Open the hibernation partition: IXFS raw block device (C:\ partition reserved region), or a dedicated swap partition identified by GPT type GUID `{HIBER-GUID}`; retrieve via `blkdev_open_by_gpt_type(HIBER_GUID)`
  3. Walk PMM used-page list; for each page: compress 64-page chunk with LZ4; write to hibernation partition via DMA (must reach D0 device state first)
  4. Write `HIBR_HEADER` at offset 0 with final `image_pages` count and CRC32C
  5. Call `acpi_enter_sleep_state(4)` -- system powers off; same as S5 but firmware knows to look for hibernation image on next boot

### 4.3 Hibernation resume on boot

- [ ] In kernel init (→ XREF `TODO-01-kernel-init-sequencing.md §3`), early in Phase 1: check the hibernation partition for a valid `HIBR_HEADER.magic`; if found and `kernel_version` matches: enter hibernation resume path
- [ ] `pm_hibernate_resume()`:
  1. Read all compressed chunks from the partition; decompress into a separate bounce buffer
  2. CRC32C verify the full image; halt with `KERNEL_HIBERNATE_CORRUPT` (→ XREF `TODO-16-crash-dump-generation.md §1`) if mismatch
  3. Copy pages from bounce buffer back to their original physical addresses; restore CR3, RSP, RIP from `HIBR_HEADER`
  4. Jump to resume RIP -- execution resumes from inside `pm_hibernate_write()` as if `acpi_enter_sleep_state(4)` just returned
  5. Run §3.2 steps 4–7 (recalibrate TSC, notify drivers, unfreeze scheduler)
- [ ] If `kernel_version` mismatches (updated kernel after hibernate): discard the image; cold boot; log `[HIBER] image version mismatch`

### 4.4 Commit

- [ ] Commit: `"kernel/acpi: S4 hibernation image write/resume, LZ4 compression, version guard"`

---

## 5. ACPI Embedded Controller (EC) Driver `[Opus]`

### 5.1 EC registers

- [ ] The Embedded Controller is mandatory on all laptops; it mediates battery, thermal, lid, and hotkey events. Interface: two I/O ports
  - `EC_SC` (status/command) port, `EC_DATA` (data) port; read from the ACPI `ECDT` table (Embedded Controller Boot Resources Table) or from the `\_SB.PCI0.LPCB.EC0` ACPI device node in the DSDT
- [ ] Define in `include/kernel/drivers/acpi_ec.h`:
  ```c
  #define EC_SC_OBF     0x01  /* Output Buffer Full (data ready for host) */
  #define EC_SC_IBF     0x02  /* Input Buffer Full (EC busy) */
  #define EC_SC_BURST   0x10  /* Burst mode enabled */
  #define EC_SC_SCI_EVT 0x20  /* SCI event pending */
  #define EC_CMD_READ   0x80  /* EC read command */
  #define EC_CMD_WRITE  0x81  /* EC write command */
  #define EC_CMD_BURST  0x82  /* Enter burst mode */
  #define EC_CMD_NBURST 0x83  /* Exit burst mode */
  #define EC_CMD_QUERY  0x84  /* Query event */
  ```

### 5.2 EC I/O transactions

- [ ] `ec_read(uint8_t reg, uint8_t *val)` -- polling path:
  1. Wait for `IBF=0` (EC ready to receive) with timeout 100 µs × 1 000 retries
  2. `outb(EC_SC, EC_CMD_READ)` then wait `IBF=0`; `outb(EC_DATA, reg)`
  3. Wait `OBF=1` (response ready); `*val = inb(EC_DATA)`
- [ ] `ec_write(uint8_t reg, uint8_t val)` -- same pattern, `EC_CMD_WRITE`
- [ ] `ec_burst_mode_enter()` / `ec_burst_mode_exit()` -- use burst mode for multi-byte reads to avoid partial reads during battery polling
- [ ] Interrupt-driven path: ACPI SCI fires with `SCI_EVT` set → `outb(EC_SC, EC_CMD_QUERY)` → read `OBF` byte → dispatch to registered EC query handler (battery change, lid event, hotkey) by query number

### 5.3 ECDT discovery and init

- [ ] `acpi_ec_init()` -- called from `acpi_init()` after MADT:
  1. Search for ECDT table via `acpi_find_table("ECDT")`; extract `ec_control` (SC port) and `ec_data` (DATA port) and `uid`
  2. If no ECDT: walk DSDT for `_HID "PNP0C09"` device node; read `_CRS` for EC I/O port addresses
  3. Register `acpi_ec_sci_handler()` as the SCI dispatch target for EC query events
- [ ] `acpi_ec_ready()` -- boolean; used by battery/lid/hotkey drivers to check readiness before calling `ec_read`/`ec_write`

### 5.4 Commit

- [ ] Commit: `"kernel/acpi: Embedded Controller driver, ECDT discovery, burst-mode EC transactions"`

---

## 6. Battery & AC Adapter `[Sonnet]`

### 6.1 ACPI battery methods

- [ ] Evaluate `\_SB.BAT0._BIX` (Battery Information Extended, ACPI 4.0+) on init; fall back to `_BIF` (deprecated) if `_BIX` is not present:
  - `_BIX` returns: revision, power unit, design capacity, last full charge capacity, technology, design voltage, design capacity of warning, design capacity of low, cycle count, measurement accuracy, max sampling time, min sampling time, max/min average interval, battery capacity granularity, model number, serial number, battery type, OEM info
  - `_BIF` returns: design capacity, full charge capacity, technology, design voltage, warn capacity, low capacity, granularity, model, serial, chemistry
- [ ] Evaluate `\_SB.BAT0._BST` (Battery Status) every 30 s or on EC event:
  - Returns: power state (discharging=1, charging=2, critical=4), present rate (mW), remaining capacity (mWh), present voltage (mV)
- [ ] `bat_update()` -- reads `_BST`; computes `charge_pct = remaining / full * 100`; `time_remaining_min = remaining / rate * 60`; stores in `bat_state_t`
- [ ] Battery cycle count and wear level: `wear_pct = (1 - last_full_cap / design_cap) * 100`; available from `_BIX.cycle_count` and capacity ratio
- [ ] Evaluate `\_SB.ACAD._PSR` (AC Adapter Power Source): 0=offline, 1=online
- [ ] Register Registry key `HKLM\SYSTEM\Battery\Status` (REG_BINARY) updated on each `bat_update()` call; apps can use `RegNotifyChangeKeyValue` to watch for power changes (→ XREF `TODO-13-registry-completion.md §3`)

### 6.2 Low-battery warnings

- [ ] `pm_check_battery_warn()` -- called from `bat_update()`:
  - `charge_pct ≤ warn_pct` (Registry `PowerWarnPercent`, default 10%): post `WM_POWERBROADCAST (PBT_APMBATTERYLOWAGE)` to all top-level windows
  - `charge_pct ≤ critical_pct` (Registry `PowerCriticalPercent`, default 5%): trigger `pm_enter_s4()` (hibernate) or OS shutdown based on `PowerCriticalAction` Registry value

### 6.3 Battery tray icon

- [ ] System tray icon: `⚡` charging, `🔋` n% discharging, `🔌` AC plugged in
- [ ] Tooltip: `"Battery: 73% -- 2h 14m remaining"` or `"Plugged in, charging"`
- [ ] Click → flyout with charge bar, current rate (W), temperature if `_BTP` supported, last full charge capacity vs. design capacity (battery wear indicator)

### 6.4 Commit

- [ ] Commit: `"kernel/acpi: battery _BIF/_BST, AC adapter, low-battery warnings, tray icon"`

---

## 7. Power Button & Lid-Close Events `[Sonnet]`

### 7.1 Power button

- [ ] PM1 fixed event (§1.3) fires SCI with `PWRBTN_STS` set
- [ ] `acpi_power_button_event()` -- dispatch based on Registry `HKLM\SYSTEM\PowerControl\PowerButtonAction`:
  - `0` = do nothing (ignore)
  - `1` = sleep (S3)
  - `2` = hibernate (S4)
  - `3` = shutdown (S5) -- default
  - `4` = lock screen
- [ ] If an interactive user session is active: first post `WM_QUERYENDSESSION` to all windows (give apps a chance to save); wait up to 5 s; then execute the action regardless

### 7.2 Sleep button

- [ ] `SLPBTN_STS` fixed event → `acpi_sleep_button_event()`:
  - Default action: S3 suspend (Registry `HKLM\SYSTEM\PowerControl\SleepButtonAction`, default `1`)

### 7.3 Lid close / open

- [ ] EC event (§5.2) or ACPI GPE fires when lid state changes; read `\_SB.LID0._LID`: 0=closed, 1=open
- [ ] Lid close action (Registry `PowerLidCloseAction`, default `1`=sleep): same action table as power button
- [ ] Lid open: if system is in S3/S4, trigger wakeup (the EC event itself causes the hardware to resume; software sees `WAKE_STS` in PM1a_STS →
  §3.2 or §4.3 resume path)
- [ ] Display-off on lid close before entering sleep: call `gfx_blank_display()` to cut video output immediately, reducing flicker during the sleep entry sequence

### 7.4 Commit

- [ ] Commit: `"kernel/acpi: power button, sleep button, lid-close events, configurable actions"`

---

## 8. PCI Device D-States (D0–D3cold) `[Sonnet]`

### 8.1 PCI Power Management Capability

- [ ] `pci_pmcap_find(dev)` -- walk PCI Capabilities linked list (cap ID `0x01` = Power Management) in config space; return cap offset or -1
- [ ] `pci_pmcap_read(dev)` → `PCI_PMCAP` struct:
  ```c
  typedef struct {
      uint16_t cap_id;      /* 0x0001 */
      uint16_t next_cap;
      uint16_t pmcap;       /* capabilities: D1/D2 support, PME capable */
      uint16_t pmcsr;       /* Power Management Control/Status Register */
  } PCI_PMCAP;
  #define PMCSR_POWER_STATE_MASK 0x0003  /* D0=0, D1=1, D2=2, D3hot=3 */
  #define PMCSR_PME_EN           0x0100
  #define PMCSR_PME_STATUS       0x8000
  ```
- [ ] `pci_set_d_state(dev, state)` -- write `state & 0x3` to `PMCSR` power state bits; wait 10 ms for D3hot→D0 transition (PCI spec minimum); return `PCI_DX_OK` or `PCI_DX_UNSUPPORTED` if no PM capability

### 8.2 D3cold via ACPI _PR3/_PS3

- [ ] D3cold (power completely removed) requires platform support: evaluate `\_SB.PCI0.DEV._PS3` ACPI method (if present) to cut VCC to the device; `_PS0` to restore power for D3cold→D0
- [ ] `pci_d3cold_enter(dev)` -- call `pci_set_d_state(dev, 3)` first (D3hot), then evaluate `_PS3`; note: device config space is inaccessible in D3cold
- [ ] `pci_d3cold_exit(dev)` -- evaluate `_PS0`; wait `_D0D3COLD_DELAY` ms (ACPI `_DSM` if present, else 100 ms default); then `pci_set_d_state(dev, 0)`

### 8.3 D-state registry for all devices

- [ ] `pm_device_t` struct registered per PCI device:
  ```c
  typedef struct {
      uint8_t  bus, dev, fn;   /* PCI BDF */
      uint8_t  current_d_state; /* 0–3 */
      uint8_t  target_d_state;  /* requested by power manager */
      uint8_t  d3cold_capable;
      void    *driver_ctx;
      pm_power_callback_t on_sleep;  /* §9 */
      pm_power_callback_t on_wake;   /* §9 */
  } pm_device_t;
  ```
- [ ] `pm_register_device(dev, on_sleep, on_wake)` -- called by each PCI driver at probe time; adds to the global `pm_device_list`

### 8.4 Commit

- [ ] Commit: `"kernel/acpi: PCI D-state machine, D0/D3hot/D3cold transitions, pm_register_device"`

---

## 9. Driver Power Callbacks & Resume Ordering `[Sonnet]`

### 9.1 Callback registration

- [ ] `pm_register_power_callback(priority, on_sleep, on_wake, ctx)`:
  - `priority`: `PM_PRI_STORAGE=0`, `PM_PRI_NETWORK=1`, `PM_PRI_USB=2`, `PM_PRI_INPUT=3`, `PM_PRI_GRAPHICS=4`, `PM_PRI_USER=5`
  - Static array of 64 callback slots; sorted by priority
- [ ] Each major driver registers in its `init()`:
  - `ahci_init()` → `PM_PRI_STORAGE`
  - `xhci_init()` → `PM_PRI_USB`
  - `rtl8139_init()` → `PM_PRI_NETWORK`
  - `framebuffer_init()` → `PM_PRI_GRAPHICS`

### 9.2 Pre-sleep ordering (reverse priority)

- [ ] `pm_notify_sleep(state)` -- iterates `pm_device_list` in **reverse** priority order (user-space → graphics → input → USB → network → storage):
  1. Call `cb->on_sleep(state, ctx)` -- driver flushes queues, stops DMA, calls `pci_set_d_state(dev, 3)` for D3hot
  2. Wait for `on_sleep` to return (max 2 s per driver; if it hangs, log `[WARN] pm: driver sleep callback timeout` and proceed)

### 9.3 Post-resume ordering (forward priority)

- [ ] `pm_notify_resume(state)` -- iterates in **forward** priority order (storage first, then network, then USB, then graphics, then user-space):
  1. Call `pci_set_d_state(dev, 0)` -- device back to D0
  2. Call `cb->on_wake(state, ctx)` -- driver re-initialises DMA, re-arms interrupts, re-establishes network/USB links
  3. Wait up to 5 s for storage drivers (`PM_PRI_STORAGE`) before allowing the scheduler to unfreeze; critical to prevent filesystem access before the disk controller is ready

### 9.4 Commit

- [ ] Commit: `"kernel/acpi: pm_register_power_callback, sleep/wake ordering, driver notification"`

---

## 10. Connected Standby (S0ix / Modern Standby) `[Opus]`

### 10.1 S0ix detection

- [ ] Check FADT `LOW_POWER_S0_IDLE_CAPABLE` flag (bit 21 of `Flags` field, ACPI 5.0+); if set: the platform supports connected standby and S3 may not be in the `\_Sx_` objects at all
- [ ] `acpi_s0ix_supported()` -- returns true if `LOW_POWER_S0_IDLE_CAPABLE` and the `_DSM` with `{GUID: S0ix}` is present in the DSDT
- [ ] On such platforms, `pm_enter_s3()` (§3) is replaced by `pm_enter_s0ix()` transparently; the sleep state entry point remains the same for the rest of the OS

### 10.2 S0ix entry

- [ ] `pm_enter_s0ix()`:
  1. Move all CPUs to the lowest available Intel C-state (`MWAIT` with C7/C10 hint via `cpuid` extended topology leaf)
  2. Gate DRAM self-refresh: set `MC_PM_STS` power gate bit in the Memory Controller MMIO space (Intel-specific; skip on AMD)
  3. Notify platform firmware via `\_OSC` (OS Capabilities) ACPI method that the OS is entering S0ix
  4. `__asm__ volatile ("mwait" : : "a"(MWAIT_HINT_C10) : "memory")` on each CPU; the hardware enters the deepest idle state; wakeup restores execution after `mwait`
- [ ] Network keepalive: the NIC (if `_DSM` advertises DRIPS/D0ix support) remains powered in D0i3 state for ARP/IPv6 NS replies and WoL packets; `rtl8139_d0i3_enter()` / `rtl8139_d0i3_exit()` stubs (full implementation depends on the specific NIC driver)

### 10.3 Resume from S0ix

- [ ] Any interrupt or I/O wakes the CPU from `mwait`; execution resumes immediately after the `mwait` instruction; no page table or register restore needed (unlike S3)
- [ ] Call `pm_notify_resume(PM_RESUME_S0IX)` (§9) to un-gate devices
- [ ] TSC recalibration (→ XREF `TODO-07-time-filetime-management.md §3`) may be needed if `mwait` C10 was held for > 1 second (TSC stops in deep C-states on some CPUs)

### 10.4 Commit

- [ ] Commit: `"kernel/acpi: connected standby S0ix, MWAIT C10, D0i3 network keepalive"`

---

## 11. Fast Startup (Hybrid Shutdown / Hiberboot) `[Opus]`

Windows 11's fast startup hibernates only the kernel session (no user processes) on shutdown, enabling < 5 s boot times by restoring the kernel image instead of cold-booting. This is a significant competitive feature -- Linux has no equivalent.

### 11.1 Hiberboot image format

- [ ] `HIBERBOOT_HEADER` -- same layout as `HIBR_HEADER` (§4.1) but with `type = HIBER_TYPE_FAST_STARTUP` flag to distinguish from full S4 hibernate
- [ ] Only kernel session pages are saved: kernel heap, PMM metadata, loaded driver images, Registry hives, VFS cache (no user-process address spaces)
- [ ] `pm_hiberboot_page_filter(phys_addr)` -- returns true if the page belongs to kernel session; skips user-mode process pages, reducing image size by 60--80%

### 11.2 Fast shutdown sequence

- [ ] `pm_fast_shutdown()`:
  1. Log off all user sessions (close all user processes; same as normal shutdown)
  2. Flush Registry hives and VFS page cache
  3. Freeze scheduler; park APs
  4. Walk PMM used-page list with `pm_hiberboot_page_filter()` -- compress and write only kernel-session pages to hibernation partition
  5. Write `HIBERBOOT_HEADER` with `type = HIBER_TYPE_FAST_STARTUP`
  6. Power off via `acpi_enter_sleep_state(5)` (S5)
- [ ] Registry key `HKLM\SYSTEM\PowerControl\FastStartupEnabled` (REG_DWORD, default 1): enables/disables fast startup
- [ ] `powercfg /hibernate on` must be enabled for fast startup to work (reuses hibernation partition)

### 11.3 Fast startup resume path

- [ ] Bootloader detects `HIBERBOOT_HEADER` with fast startup flag; sets `boot_info.flags |= BOOT_FAST_STARTUP`
- [ ] `pm_fast_startup_resume()` -- same as §4.3 hibernate resume but skips user-process page restoration; kernel drivers see `IRP_MN_SET_POWER(S0)` with `SystemPowerAction = PowerActionHibernate` (same as hibernate wake) -- drivers must call `PoFxReportDevicePoweredOn()` equivalent
- [ ] After kernel restore: `smss.exe` / session manager starts fresh user sessions from scratch (unlike hibernate where user sessions are restored)
- [ ] Distinguish fast startup from hibernate wake: check `HIBERBOOT_HEADER.type`; expose `PoGetSystemPowerStateFlags(FAST_STARTUP)` for drivers

### 11.4 Commit

- [ ] Commit: `"kernel/pm: fast startup -- hiberboot image, kernel-only page filter, resume path"`

---

## 12. Runtime Device Idle Management `[Opus]`

Per-device runtime idle management -- equivalent to Windows PoFx (Power Management Framework) component-level idle and Linux runtime PM (`pm_runtime_get`/`pm_runtime_put`). Devices autonomously enter low-power states when idle without requiring full system sleep.

### 12.1 Runtime PM device registration

- [ ] `pm_runtime_register(dev, ops, idle_timeout_ms)` -- register a device for runtime PM:
  ```c
  typedef struct {
      int (*runtime_suspend)(void *ctx);  /* transition to low-power */
      int (*runtime_resume)(void *ctx);   /* transition to active */
      int (*runtime_idle)(void *ctx);     /* check if device can suspend */
  } pm_runtime_ops_t;
  ```
- [ ] Per-device state: `PM_RT_ACTIVE`, `PM_RT_SUSPENDING`, `PM_RT_SUSPENDED`, `PM_RT_RESUMING`
- [ ] Reference counter per device: `pm_runtime_get(dev)` increments and ensures device is active; `pm_runtime_put(dev)` decrements and starts idle timer when count reaches 0

### 12.2 Autosuspend

- [ ] When reference count reaches 0: start a DPC timer with `idle_timeout_ms` delay (→ XREF `TODO-06-irql-model-dpcs.md §3`)
- [ ] Timer expiry: call `ops->runtime_idle(ctx)`; if returns 0 (device can suspend): call `ops->runtime_suspend(ctx)` to transition to low-power state
- [ ] `pm_runtime_set_autosuspend_delay(dev, ms)` -- adjustable per device; storage controllers use longer delays (2000 ms); input devices use shorter (500 ms)
- [ ] `pm_runtime_get(dev)` on a suspended device: call `ops->runtime_resume(ctx)` synchronously before returning

### 12.3 Integration with system sleep

- [ ] Before system S3/S4 entry: all runtime-active devices are suspended via `ops->runtime_suspend()`; runtime-suspended devices remain suspended
- [ ] On system resume: only devices that were runtime-active before system sleep are resumed; runtime-suspended devices stay suspended until next `pm_runtime_get()`
- [ ] Boot log: `[PM_RT] %s: autosuspend after %u ms idle`

### 12.4 Commit

- [ ] Commit: `"kernel/pm: runtime device idle -- pm_runtime_get/put, autosuspend, DPC timer"`

---

## 13. Power Request Tracking & Wake Source Management `[Sonnet]`

Track which applications and drivers are preventing system idle sleep, and provide a complete wake source registry for diagnostics.

### 13.1 Power request tracking

- [ ] `pm_power_request_t` -- tracks active power requests:
  ```c
  typedef struct {
      uint32_t flags;         /* ES_SYSTEM_REQUIRED, ES_DISPLAY_REQUIRED, ES_AWAYMODE_REQUIRED */
      uint32_t pid;           /* requesting process */
      const char *reason;     /* human-readable reason string */
      uint64_t timestamp;     /* when request was created */
  } pm_power_request_t;
  ```
- [ ] `pm_create_power_request(flags, reason)` -- called from `NtSetThreadExecutionState` (§20); adds to global request list
- [ ] `pm_release_power_request(req)` -- called on thread exit or explicit release
- [ ] `pm_check_idle_allowed()` -- returns false if any active `ES_SYSTEM_REQUIRED` request exists; called by idle timer before initiating S3
- [ ] Display timeout inhibited if any `ES_DISPLAY_REQUIRED` request is active (video playback, presentations)

### 13.2 Wake source registry

- [ ] `pm_wake_source_t` -- registered wake sources:
  ```c
  typedef struct {
      const char *name;       /* "Power Button", "RTC Alarm", "USB xHCI", "NIC WoL" */
      uint8_t    type;        /* PM_WAKE_FIXED_EVENT, PM_WAKE_GPE, PM_WAKE_PCI_PME, PM_WAKE_TIMER */
      bool       enabled;     /* can this source wake from S3/S4? */
      uint64_t   last_wake;   /* timestamp of last wake event */
  } pm_wake_source_t;
  ```
- [ ] `pm_register_wake_source(name, type)` -- called by EC driver (§5), PCI D-state code (§8), RTC alarm (§3.3)
- [ ] `pm_record_wake_event(source)` -- called on S3/S4 resume path; records which source triggered the wake
- [ ] Wake timers: `pm_set_wake_timer(seconds, callback)` -- programs CMOS RTC or HPET comparator; adds to wake timer list

### 13.3 powercfg diagnostics

- [ ] `powercfg /requests` -- print all active power requests (pid, flags, reason string)
- [ ] `powercfg /lastwake` -- print the wake source that triggered the most recent S3/S4 resume
- [ ] `powercfg /waketimers` -- print all active wake timers with their expiry times
- [ ] `powercfg /devicequery wake_armed` -- list all devices enabled to wake the system
- [ ] `powercfg /energy` -- 60-second trace of power usage; report idle violations, devices not entering low-power states, excessive timer resolution requests

### 13.4 Commit

- [ ] Commit: `"kernel/pm: power request tracking, wake source registry, powercfg diagnostics"`

---

## 14. ACPI Thermal Zone Management `[Opus]`

Implement the OSPM thermal policy engine per ACPI spec chapter 11. This is the kernel-core framework that processes thermal zones from ACPI namespace (`_TZ`), evaluates temperature (`_TMP`), and enforces passive cooling (CPU throttle) and active cooling (fan control) based on trip points.

> [!IMPORTANT]
> **Scope boundary with D04T04§6:** TODO-04 §6 covers per-core MSR-based thermal monitoring (`IA32_THERM_STATUS`, LAPIC Thermal LVT). This section covers the ACPI thermal zone framework that sits above it -- processing `_TMP`/`_CRT`/`_HOT`/`_PSV`/`_ACx` objects and coordinating cooling responses. Both are needed for full parity.
> → XREF: `04-drivers-hardware/TODO-04-acpi-power-management.md §6` -- MSR-based per-core thermal; this section adds ACPI thermal zones

### 14.1 Thermal zone discovery

- [ ] Walk ACPI namespace for `ThermalZone` objects (`\_TZ.*` or `\_SB.*.TZ*`)
- [ ] For each thermal zone, evaluate:
  - `_TMP` -- current temperature (returns tenths of Kelvin; convert: `celsius = (tmp - 2732) / 10`)
  - `_CRT` -- critical shutdown temperature (OSPM must shut down immediately)
  - `_HOT` -- hot temperature (OSPM should initiate S4 hibernate)
  - `_PSV` -- passive cooling threshold (start CPU throttling)
  - `_AC0`..`_AC9` -- active cooling thresholds (turn on fan at level 0--9)
  - `_TSP` -- thermal sampling period (how often to poll `_TMP`)
  - `_TC1`, `_TC2` -- passive cooling algorithm coefficients
  - `_SCP` -- set cooling policy (0=active preferred, 1=passive preferred)
- [ ] Store in `thermal_zone_t { name, tmp, crt, hot, psv, ac[10], tsp, tc1, tc2, cooling_policy }`

### 14.2 Thermal polling and trip point evaluation

- [ ] Poll each thermal zone every `_TSP` milliseconds via DPC timer (→ XREF `TODO-06-irql-model-dpcs.md §3`)
- [ ] On each poll, evaluate `_TMP`; compare against trip points:
  - `temp >= _CRT` → `system_shutdown(SHUTDOWN_THERMAL_EMERGENCY)` -- immediate power-off
  - `temp >= _HOT` → `pm_enter_s4()` -- emergency hibernate to preserve state
  - `temp >= _PSV` → initiate passive cooling (§14.3)
  - `temp >= _ACx` → initiate active cooling at level x (§14.4)
- [ ] Thermal event notification: ACPI `Notify(thermal_zone, 0x80)` (temperature change) triggers immediate re-evaluation instead of waiting for next poll

### 14.3 Passive cooling (CPU throttling)

- [ ] Passive cooling reduces CPU power dissipation by throttling frequency/performance:
  - `delta_perf = _TC1 * (temp - prev_temp) + _TC2 * (temp - _PSV)` -- ACPI spec passive cooling equation
  - Apply performance reduction via `cpufreq_set_max_pstate()` (→ XREF §15)
- [ ] `_PSL` (Passive List) -- list of processor objects to throttle; if absent, throttle all CPUs
- [ ] When temperature drops below `_PSV`: gradually restore full performance over 3 polling intervals
- [ ] Boot log: `[THERMAL] Zone %s: passive cooling active, target %u C, current %u C`

### 14.4 Active cooling (fan control)

- [ ] `_ALx` (Active List) -- list of `FAN` device objects to activate at trip level x
- [ ] `FAN._ON()` / `FAN._OFF()` ACPI methods -- turn fan on/off
- [ ] Multi-level fan: `_AC0` is the highest threshold (all fans max); `_AC9` is lowest (gentle fan)
- [ ] Fan hysteresis: do not turn off fan until temperature drops 3 C below the `_ACx` threshold (prevent rapid on/off cycling)
- [ ] If fan device supports `_FPS` (Fan Performance States): set fan speed as a percentage instead of on/off

### 14.5 Thermal zone Registry and telemetry

- [ ] Write `HKLM\HARDWARE\Thermal\Zone<N>\Temperature`, `CriticalTemp`, `PassiveTemp` on each poll
- [ ] System tray: temperature indicator when any zone is above `_PSV`
- [ ] `powercfg /energy` includes thermal zone status in energy report
- [ ] Boot log: `[THERMAL] Zone %s: _CRT=%u C, _HOT=%u C, _PSV=%u C, _AC0=%u C, polling=%u ms`

### 14.6 Commit

- [ ] Commit: `"kernel/pm: ACPI thermal zones -- _TMP/_CRT/_HOT/_PSV/_ACx, passive/active cooling"`

---

## 15. CPU Frequency Scaling Governor Framework `[Opus]`

Kernel-core governor framework that sits between the scheduler's load metrics and the ACPI/HWP/CPPC frequency control hardware. This is the kernel-core responsibility -- the ACPI `_PSS` parsing lives in D04T04§4; the scheduler's `cpufreq_register_driver()` vtable lives in D03T05§9.

> [!IMPORTANT]
> **Scope boundary:** D04T04§4 parses `_PSS` P-state tables via ACPICA and provides the hardware driver. D03T05§9 provides the scheduler hook. This section owns the policy layer -- governor algorithms, HWP/CPPC native support, and the connection between them.
> → XREF: `04-drivers-hardware/TODO-04-acpi-power-management.md §4` -- `_PSS` P-state hardware driver
> → XREF: `03-memory-concurrency/TODO-05-scheduler-enhancement.md §9` -- `cpufreq_register_driver()` and load metrics
> → XREF: `01-boot-platform/TODO-06-interrupt-timer-arch.md §7` -- LAPIC timer recalibration after frequency change
> → XREF: `02-kernel-core/TODO-19-x86-64-architecture.md §7` -- Intel hybrid P/E-core topology data

### 15.1 Governor framework

- [ ] `cpufreq_governor_t` interface:
  ```c
  typedef struct {
      const char *name;           /* "performance", "powersave", "balanced", "schedutil" */
      void (*start)(uint32_t cpu);
      void (*stop)(uint32_t cpu);
      void (*update)(uint32_t cpu, uint64_t load_pct, uint64_t idle_ns);
  } cpufreq_governor_t;
  ```
- [ ] Built-in governors:
  - `performance` -- always request max P-state; ignore load
  - `powersave` -- always request min P-state; ignore load
  - `balanced` -- scale linearly with load; hysteresis: go to max if load > 80%, step down if load < 20% for > 100 ms
  - `schedutil` -- use scheduler's PELT (Per-Entity Load Tracking) utilization signal for P-state selection (Linux EAS-compatible approach)
- [ ] `cpufreq_set_governor(name)` -- switch active governor; called by power plan (§18)
- [ ] `cpufreq_get_current_freq(cpu)` / `cpufreq_get_available_freqs()` -- query current state

### 15.2 Intel HWP (Hardware P-States) support

- [ ] Detect HWP: `CPUID.06H:EAX[7]` (HWP base), `CPUID.06H:EAX[8]` (HWP notification), `CPUID.06H:EAX[9]` (HWP activity window)
- [ ] Enable HWP: `wrmsr(IA32_PM_ENABLE, 1)` per CPU
- [ ] `IA32_HWP_CAPABILITIES (0x771)` -- read guaranteed/min/max/most_efficient frequencies
- [ ] `IA32_HWP_REQUEST (0x774)` -- write min/max/desired/energy_perf_preference per CPU:
  - `performance` governor: min=max=highest_perf, EPP=0 (max performance)
  - `powersave` governor: min=lowest, max=guaranteed, EPP=255 (max efficiency)
  - `balanced` governor: min=lowest, max=highest, EPP=128 (balanced)
- [ ] `IA32_HWP_STATUS (0x777)` -- read current performance state for telemetry
- [ ] Boot log: `[CPUFREQ] HWP enabled: %u--%u MHz, guaranteed=%u MHz`

### 15.3 AMD CPPC (Collaborative Processor Performance Control) support

- [ ] Detect CPPC: `CPUID.80000008H:EBX[25]` (CPPC)
- [ ] Evaluate ACPI `_CPC` (Continuous Performance Control) package per CPU
- [ ] `PERF_CTL` MSR / ACPI `SystemIO` writes for requested performance level
- [ ] Same governor integration as HWP: min/max/desired/energy_pref mapped to CPPC registers
- [ ] Boot log: `[CPUFREQ] CPPC enabled: %u--%u nominal performance units`

### 15.4 Frequency scaling telemetry

- [ ] Per-CPU frequency transition histogram: `g_cpufreq_stats[cpu].transitions_total`, `time_in_state[pstate]`
- [ ] `/sys/cpufreq` VFS file: columns `CPU  Governor  CurFreq  MinFreq  MaxFreq  Transitions`
- [ ] Registry: `HKLM\HARDWARE\CPU\Frequency\Core<N>` updated every 5 s

### 15.5 Commit

- [ ] Commit: `"kernel/pm: cpufreq governor framework -- HWP, CPPC, performance/powersave/balanced/schedutil"`

---

## 16. CPU Idle Governor Framework `[Opus]`

Kernel-core idle governor that selects the optimal C-state based on predicted idle duration and latency constraints. The C-state hardware interface (`_CST`, `MWAIT`) is owned by D04T04§7; this section owns the idle prediction and selection policy.

> [!IMPORTANT]
> **Scope boundary:** D04T04§7 parses `_CST` and provides `cpuidle_enter(cpu, cstate)`. This section owns the governor that decides *which* C-state to enter. §2 of this TODO provides the basic S1/HLT idle path; this section replaces it with a full governor.
> → XREF: `04-drivers-hardware/TODO-04-acpi-power-management.md §7` -- `_CST` parsing and `cpuidle_enter()` hardware interface

### 16.1 Idle governor interface

- [ ] `cpuidle_governor_t`:
  ```c
  typedef struct {
      const char *name;       /* "menu", "ladder", "teo" */
      int (*select)(uint32_t cpu, uint64_t *predicted_idle_ns);
      void (*reflect)(uint32_t cpu, int selected_state, uint64_t actual_idle_ns);
  } cpuidle_governor_t;
  ```
- [ ] `select()` returns the C-state index to enter; `reflect()` is called after wakeup to improve future predictions

### 16.2 Menu governor (default)

- [ ] Prediction based on: expected next timer interrupt, recent idle history (exponential weighted moving average), interrupt rate
- [ ] `predicted_idle_ns` computed; select deepest C-state whose `exit_latency_ns ≤ predicted_idle_ns / 2` (latency must be < 50% of predicted idle to be worthwhile)
- [ ] Skip C-states that exceed the power plan's `idle_latency_budget_ns` (§18)
- [ ] Correction factor: if the CPU woke earlier than predicted on the last 4 invocations, bias toward shallower C-states

### 16.3 Ladder governor (alternative)

- [ ] Start at C1; promote to deeper C-state if idle duration exceeded promotion threshold on N consecutive entries; demote if actual idle was less than demotion threshold
- [ ] Useful for latency-sensitive workloads where menu governor oscillates

### 16.4 Idle statistics

- [ ] Per-CPU, per-C-state: `entries`, `total_residency_ns`, `rejected` (selected but actual idle was too short)
- [ ] `/sys/cpuidle` VFS file: columns `CPU  C0%  C1%  C2%  C3%  Governor  AvgIdleUs`
- [ ] Boot log: `[CPUIDLE] Governor: %s, max C-state: C%u, latency budget: %u us`

### 16.5 Commit

- [ ] Commit: `"kernel/pm: CPU idle governor -- menu/ladder algorithms, C-state selection, idle stats"`

---

## 17. Driver Power Query & Veto (IRP_MN_QUERY_POWER) `[Sonnet]`

Before changing system or device power state, query all affected drivers and allow them to veto the transition. Windows uses `IRP_MN_QUERY_POWER`; Linux uses `prepare()` callbacks. Without this, a driver with in-flight DMA or unsaved state can lose data during sleep.

### 17.1 Power query mechanism

- [ ] `pm_query_power_state(target_state)` -- called before `pm_notify_sleep()` (§9):
  1. Iterate all registered power callbacks in reverse priority order
  2. Call `cb->on_query(target_state, ctx)` for each callback
  3. If any callback returns `STATUS_DEVICE_BUSY` or `STATUS_INSUFFICIENT_RESOURCES`: abort the power transition; log `[PM] Power query vetoed by %s`; return error to caller
  4. If all callbacks return `STATUS_SUCCESS`: proceed with `pm_notify_sleep()`
- [ ] `pm_power_callback_t` extended:
  ```c
  typedef struct {
      int (*on_query)(uint8_t target_state, void *ctx);  /* veto opportunity */
      int (*on_sleep)(uint8_t state, void *ctx);
      int (*on_wake)(uint8_t state, void *ctx);
  } pm_power_callback_t;
  ```
- [ ] If a driver does not register `on_query`, it is assumed to accept all transitions (backward compatible)

### 17.2 Cancel after query

- [ ] If the user cancels a sleep request (e.g. clicks "Cancel" on shutdown dialog) after `pm_query_power_state()` succeeded but before `pm_notify_sleep()`:
  - Call `cb->on_query_cancel(ctx)` for all drivers that were queried -- lets drivers release resources they reserved for the transition

### 17.3 Commit

- [ ] Commit: `"kernel/pm: driver power query/veto -- pm_query_power_state, on_query callback"`

---

## 18. Power Plan UI & `powercfg` `[Sonnet]`

### 18.1 Power plan Registry schema

- [ ] Stored under `HKLM\SYSTEM\PowerPlans\{GUID}\`:
  - `Name` (REG_SZ): `"Balanced"`, `"Power Saver"`, `"High Performance"`
  - `SleepTimeout` (REG_DWORD): seconds to S3 on idle (0=never)
  - `HibernateTimeout` (REG_DWORD): seconds to S4 after S3 (0=never)
  - `DisplayOffTimeout` (REG_DWORD): seconds to blank display
  - `PowerButtonAction` (REG_DWORD): same codes as §7.1
  - `LidCloseAction` (REG_DWORD): same codes as §7.3
  - `CpuFreqGovernor` (REG_SZ): `"performance"`, `"balanced"`, `"powersave"`, `"schedutil"`
  - `IdleLatencyBudgetUs` (REG_DWORD): max C-state exit latency in microseconds
  - `MaxProcessorState` (REG_DWORD): 0--100% cap on CPU frequency (§15)
  - `MinProcessorState` (REG_DWORD): 0--100% floor on CPU frequency
  - `CoolingPolicy` (REG_DWORD): 0=active preferred (fan first), 1=passive preferred (throttle first)
- [ ] `pm_apply_plan(guid)` -- reads plan Registry values; sets idle timers in the scheduler's DPC timer (→ XREF `TODO-06-irql-model-dpcs.md §3`); configures governor (§15), idle budget (§16), thermal policy (§14)
- [ ] Default plan GUIDs match Windows 11's well-known GUIDs:
  - Balanced: `{381b4222-f694-41f0-9685-ff5bb260df2e}`
  - Power Saver: `{a1841308-3541-4fab-bc81-f71556f20b4a}`
  - High Performance: `{8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c}`

### 18.2 powercfg shell command

- [ ] `powercfg /list` -- print all power plans with active indicator (`*`)
- [ ] `powercfg /setactive {GUID}` -- activate a plan; calls `pm_apply_plan`
- [ ] `powercfg /query` -- print current plan's settings (timeout values, button actions, governor, CPU limits)
- [ ] `powercfg /hibernate on|off` -- enable/disable S4; writes `HKLM\SYSTEM\PowerControl\HibernateEnabled`
- [ ] `powercfg /sleep on|off` -- enable/disable S3; writes `HKLM\SYSTEM\PowerControl\SleepEnabled`
- [ ] `powercfg /batteryreport` -- print battery design capacity, full charge capacity, wear level (%), cycle count, current charge, and last 24 h discharge history
- [ ] `powercfg /requests` -- print active power requests preventing sleep (§13)
- [ ] `powercfg /lastwake` -- print last wake source (§13)
- [ ] `powercfg /waketimers` -- print scheduled wake timers (§13)
- [ ] `powercfg /devicequery wake_armed` -- list wake-enabled devices (§13)
- [ ] `powercfg /energy` -- 60 s power efficiency trace; report violations (§13)
- [ ] `powercfg /sleepstudy` -- Modern Standby session report; show DRIPS %, top offenders (§10)
- [ ] `powercfg /availablesleepstates` -- print S-states supported by hardware (from §1)

### 18.3 Power Options in System Properties

- [ ] `sysdm.cpl` Power tab (or dedicated `powercpl.cpl`):
  - Plan selector radio buttons
  - "Change plan settings" expands to sliders: Screen off / Sleep / Hibernate timeouts
  - "Advanced settings" tree: power button action, lid action, low-battery action, CPU min/max, cooling policy, USB selective suspend
- [ ] Changes written to Registry via `RegSetValueEx`; `pm_apply_plan()` called immediately after save

### 18.4 Commit

- [ ] Commit: `"kernel/pm: powercfg shell command, power plan Registry schema, Power Options UI"`

---

## 19. Energy-Aware Scheduling Integration `[Opus]`

On heterogeneous CPU topologies (Intel Alder Lake+ P/E-cores, future ARM big.LITTLE), the scheduler should place tasks on the most energy-efficient core that can meet the task's performance requirements. This is Impossible OS's competitive edge -- integrating power and scheduling into a single decision loop rather than layering them separately.

> [!IMPORTANT]
> **Scope boundary:** D02T19§7 detects Intel hybrid P/E-core topology and Intel Thread Director (ITD) / Hardware Feedback Interface (HFI). D03T05§9 provides the scheduler's load metrics. This section integrates those signals with the CPU frequency governor (§15) to make energy-aware placement decisions.
> → XREF: `02-kernel-core/TODO-19-x86-64-architecture.md §7` -- P/E-core detection, HFI capability data
> → XREF: `03-memory-concurrency/TODO-05-scheduler-enhancement.md §9` -- scheduler load tracking, PELT utilization

### 19.1 Energy model

- [ ] `em_cpu_t` per logical CPU:
  ```c
  typedef struct {
      uint32_t max_freq_mhz;    /* from HWP/CPPC/CPUID */
      uint32_t base_freq_mhz;
      uint32_t efficiency;      /* perf-per-watt score (higher = more efficient) */
      uint8_t  core_type;       /* 0=P-core, 1=E-core, 2=homogeneous */
      uint8_t  cluster_id;      /* which frequency domain */
  } em_cpu_t;
  ```
- [ ] Populate from: HFI performance/efficiency capabilities table (if Intel hybrid), `_CPC` nominal/lowest (if AMD), or uniform values on homogeneous systems
- [ ] `em_energy_cost(cpu, util_pct)` -- estimated energy cost to run a task at `util_pct` utilization on `cpu`; uses `freq_at_util * voltage^2` model (cubic scaling)

### 19.2 Task placement policy

- [ ] For each runnable task, the scheduler's `select_cpu()` considers:
  - `em_energy_cost(candidate_cpu, task_util)` -- prefer lower cost
  - Wake latency: do not migrate to a CPU in deep C-state if the migration cost exceeds the energy savings
  - Task priority: real-time tasks always go to P-cores; background/idle tasks prefer E-cores
- [ ] Intel Thread Director hints: if HFI `perf_capability[cpu]` < task requirement, avoid that CPU
- [ ] Fallback on homogeneous systems: no energy-aware placement; revert to standard load balancing

### 19.3 Per-cluster frequency coordination

- [ ] On Intel hybrid CPUs, P-cores and E-cores have independent frequency domains
- [ ] `em_cluster_set_freq(cluster_id, target_freq)` -- coordinate frequency for all CPUs in the cluster
- [ ] When all tasks on a cluster are low-utilization: scale down the cluster frequency; when any task is high-utilization: scale up

### 19.4 Commit

- [ ] Commit: `"kernel/pm: energy-aware scheduling -- energy model, task placement, cluster frequency"`

---

## 20. Power Syscalls Wired to SSDT `[Sonnet]`

Register all power management NtXxx entry points in the SSDT so user-mode code can invoke them via `syscall`. (→ XREF: TODO-05-native-api-ssdt.md §4, §21)

- [ ] `NtShutdownSystem(Action)` → SSDT 0x00D7: call `pm_shutdown()` / `pm_reboot()` based on action; requires `SeShutdownPrivilege`
- [ ] `NtSetSystemPowerState(SystemAction, LightestSystemState, Flags)` → SSDT 0x0140: route through ACPI S-state transition (§2)
- [ ] `NtInitiatePowerAction(SystemAction, LightestSystemState, Flags, Asynchronous)` → SSDT 0x0141: async power action initiation
- [ ] `NtPowerInformation(InformationLevel, InputBuffer, InputLen, OutputBuffer, OutputLen)` → SSDT 0x0142: return battery state, processor info, S-state capabilities, thermal zone data from §6, §14, §15
- [ ] `NtGetDevicePowerState(Device, State)` → SSDT 0x0143: query PCI device D-state (§8) or runtime PM state (§12)
- [ ] `NtSetThreadExecutionState(NewFlags, PreviousFlags)` → SSDT 0x0144: `ES_SYSTEM_REQUIRED` / `ES_DISPLAY_REQUIRED` / `ES_AWAYMODE_REQUIRED` prevents idle sleep; creates power request (§13)
- [ ] `NtRequestWakeupLatency(Latency)` → SSDT 0x0145: hint to power manager about acceptable wake latency; feeds idle governor (§16)
- [ ] All functions return `NTSTATUS`; use codes from `include/kernel/nt/ntstatus.h` (TODO-05 §1)
- [ ] Commit: `"kernel/pm: wire power syscalls to SSDT (0x00D7, 0x0140--0x0145)"`

**Test checkpoint:** `NtShutdownSystem(ShutdownReboot)` triggers ACPI reset. `NtPowerInformation(SystemPowerCapabilities)` returns valid S-state mask. `NtSetThreadExecutionState(ES_SYSTEM_REQUIRED)` prevents idle sleep during long operation. `NtPowerInformation(ProcessorPowerInformation)` returns current CPU frequency per core.

---

## OS Comparison

| ⭐ | Feature                                 | 🪟 Win11                              | 🐧 Linux                              | 🚀 Impossible OS                    |
|----|----------------------------------------|-----------------------------------|-----------------------------------|--------------------------------|
| 💎 | S5 shutdown via ACPI PM1a              | ✅ Full                           | ✅ Full                           | ✅ Done -- `acpi_shutdown()`    |
| 💎 | S1 CPU halt / idle                     | ✅ Full                           | ✅ Full (`cpuidle`)               | ⬜ §2                          |
| 💎 | S3 suspend to RAM                      | ✅ Full                           | ✅ Full (systemd-sleep)           | ⬜ §3                          |
| 💎 | S4 hibernate to disk                   | ✅ Full                           | ✅ Full (swsusp)                  | ⬜ §4                          |
| 💎 | Fast startup / hiberboot               | ✅ Default since Win8             | ❌ No equivalent                  | ⬜ §11                         |
| 💎 | ACPI Embedded Controller (EC)          | ✅ Full                           | ✅ Full (`acpi_ec`)               | ⬜ §5                          |
| 💎 | Battery `_BIX`/`_BST` + AC adapter    | ✅ `battc.sys`, `_BIX`            | ✅ `upower`, `_BIX`               | ⬜ §6                          |
| 💎 | Power button & lid-close events        | ✅ Full                           | ✅ Full (`logind`)                | ⬜ §7                          |
| 💎 | PCI D-states (D0--D3cold)              | ✅ Full                           | ✅ Full (PCI PM)                  | ⬜ §8                          |
| 💎 | Driver sleep/wake callbacks            | ✅ WDM `IRP_MJ_POWER`             | ✅ `pm_ops`                        | ⬜ §9                          |
| 💎 | Driver power query/veto                | ✅ `IRP_MN_QUERY_POWER`            | ✅ `prepare()` callback           | ⬜ §17                         |
| 💎 | Runtime device idle (PoFx/RPM)         | ✅ PoFx Fx-states                  | ✅ `pm_runtime_get/put`           | ⬜ §12                         |
| 💎 | Power request tracking                 | ✅ `powercfg /requests`            | ⚠️ `wake_lock` (Android only)     | ⬜ §13                         |
| 💎 | Wake source management                 | ✅ `powercfg /lastwake`            | ⚠️ `dmesg` grep (no tool)        | ⬜ §13                         |
| 💎 | ACPI thermal zones                     | ✅ `ACPI.sys` thermal policy      | ✅ `thermal_zone` sysfs           | ⬜ §14                         |
| 💎 | Passive/active cooling policy          | ✅ `_PSV`/`_ACx` full             | ✅ `step_wise`/`bang_bang`        | ⬜ §14                         |
| 💎 | CPU frequency scaling (DVFS)           | ✅ PPM, HWP                       | ✅ `cpufreq`, `intel_pstate`      | ⬜ §15                         |
| 💎 | CPU idle governor (C-states)           | ✅ PPM idle                       | ✅ `menu`/`teo` governors         | ⬜ §16                         |
| 💎 | Connected Standby / S0ix              | ✅ Modern Standby                 | ⚠️ Partial (Intel-specific)       | ⬜ §10                         |
| 💎 | `powercfg` CLI                         | ✅ Full (50+ subcommands)         | ⚠️ `systemctl suspend` (basic)    | ⬜ §18                         |
| 💎 | Power Options GUI                      | ✅ `powercpl.dll`                 | ⚠️ GNOME Settings (basic)         | ⬜ §18.3                       |
| ⭐ | Energy-aware scheduling                | ⚠️ Thread Director (HW only)     | ✅ EAS (ARM only)                 | ⬜ §19 -- unified P/E + EAS    |
| ⭐ | Battery wear in tray tooltip           | ❌ Requires Settings app          | ❌ Requires `upower -i`           | ⬜ §6.3                        |
| ⭐ | `powercfg /batteryreport` plain text   | ✅ HTML only                      | ❌ Not available                  | ⬜ §18.2 (CLI + plain text)    |
| ⭐ | `powercfg /energy` efficiency audit    | ✅ Full                           | ❌ No equivalent                  | ⬜ §13.3                       |
| ⭐ | `powercfg /sleepstudy` standby report  | ✅ Full (Modern Standby)          | ❌ No equivalent                  | ⬜ §18.2                       |

After §1--20, Impossible OS reaches full Windows 11 and Linux parity for every power-management scenario that matters on real hardware -- S-states, D-states, runtime idle, thermal, DVFS, C-states, EC, battery, power/lid events, driver callbacks with query/veto, fast startup, and power request tracking. Linux relies on a patchwork of kernel drivers, `systemd-logind`, `upower` D-Bus, and `cpufreq`/`cpuidle` subsystems; the integration is fragmented and the UI is distribution-dependent. Windows has the most complete stack but its energy-aware scheduling relies entirely on Intel Thread Director hardware hints -- no software EAS. Impossible OS delivers a unified stack from hardware events to Registry-backed power plans to a consistent `powercfg` CLI, and adds a software energy-aware scheduler that works on both Intel hybrid and AMD heterogeneous CPUs. The battery-wear tray tooltip, plain-text battery report, energy audit, and sleep study are quality-of-life exclusives that match or exceed Windows while Linux offers no equivalents.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_power()` (→ XREF: `00-infrastructure/TODO-03-kernel-test-framework.md`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_power.c` with:
  - ACPI sleep type lookup: `acpi_get_slp_typ(S5)` returns valid SLP_TYPa/b values
  - Power state query: `PoGetSystemPowerState()` returns `PowerSystemWorking` during boot
  - Device D-state: `PoSetDevicePowerState(dev, D0)` succeeds for active device
  - Idle detection: CPU idle counter increments when no work scheduled
  - Thermal zone: `acpi_get_temperature()` returns plausible value (20--100 C) or graceful skip if no zone
  - Shutdown path: `acpi_shutdown()` writes correct PM1a_CNT value (verify register, don't actually shut down)
  - Reboot path: `acpi_reboot()` writes to correct reset register
  - Runtime PM: `pm_runtime_get(dev)` / `pm_runtime_put(dev)` ref counting returns correct state
  - Power request: `pm_create_power_request(ES_SYSTEM_REQUIRED)` prevents `pm_check_idle_allowed()`
  - Frequency governor: `cpufreq_set_governor("performance")` accepted; current governor name matches
  - Idle governor: `cpuidle_get_governor()` returns non-NULL; select returns valid C-state index
  - Thermal trip parse: `thermal_zone_get_crt()` returns value > 0 or graceful skip if no zone
  - Energy model: `em_energy_cost(0, 50)` returns non-zero on heterogeneous systems, 0 on homogeneous (skip)
- [ ] Register in `test_runner_init()`: `test_register_power()`
- [ ] Commit: `"test: add power management test suite"`

---

## Verification

- [ ] **S1 idle**: run `powercfg /query`; set `SleepTimeout=0`, `HibernateTimeout=0`; confirm CPU stays at `HLT` when scheduler is idle (verify via PMU idle counter).
- [ ] **S3 round-trip in QEMU**: QEMU supports S3 with `-machine q35,acpi=on`; call `pm_enter_s3()` from the shell; verify system re-appears at the desktop with all tasks intact and TSC recalibrated (serial log shows `[TSC] recalibrated after S3 wake`).
- [ ] **S4 round-trip in QEMU**: enable a hibernation partition; call `pm_enter_s4()`; power off QEMU; restart; verify `[HIBER] resuming from image` in serial log and desktop restores to pre-hibernate state.
- [ ] **Fast startup**: `powercfg /hibernate on`; perform shutdown via `pm_fast_shutdown()`; restart; serial log shows `[BOOT] Fast startup resume` and kernel state is restored without user sessions.
- [ ] **EC smoke test**: on a real laptop or QEMU with DSDT that includes `PNP0C09`; `acpi_ec_ready()` returns true; `ec_read(0x10, &val)` completes without timeout.
- [ ] **Battery**: `powercfg /batteryreport` reports non-zero design capacity, cycle count, wear level on a machine with ACPI `_BIX`/`_BST`.
- [ ] **Power button**: press power button -> S5 shutdown executes within 10 s.
- [ ] **D-state**: after S3 resume, verify AHCI controller is back in D0 via `pci_get_d_state(ahci_dev)` returning `0`.
- [ ] **Runtime PM**: `pm_runtime_put(ahci_dev)` -> after idle timeout, device enters low-power; `pm_runtime_get(ahci_dev)` resumes it.
- [ ] **Power requests**: `powercfg /requests` shows active requests; killing the requesting process removes the entry.
- [ ] **Thermal**: QEMU with ACPI thermal zone -> `powercfg /energy` reports thermal zone status; temperature reads plausible value.
- [ ] **Frequency**: `cpufreq_get_current_freq(0)` returns non-zero; governor switch from balanced to performance logged.
- [ ] **Idle governor**: `/sys/cpuidle` shows per-CPU C-state residency percentages summing to ~100%.
- [ ] **Query/veto**: register a test driver that vetoes S3 -> `pm_enter_s3()` fails with `STATUS_DEVICE_BUSY`.
- [ ] **Energy-aware**: on heterogeneous CPU (Intel hybrid), low-priority task placed on E-core; verify via `/sys/sched` per-CPU affinity.
- [ ] **Remaining limits**: connected standby (§10) requires Intel LPSS hardware; QEMU does not support `LOW_POWER_S0_IDLE_CAPABLE` -- skip in CI; D3cold `_PS3` ACPI method evaluation deferred until the AML interpreter is complete.
- [ ] Commit: `"kernel/pm: full power management verification -- S-states, D-states, thermal, DVFS, idle, runtime PM"`
