# TODO-15 — Power Management (S1–S4 & Device D-States)

> **Goal:** Implement the complete ACPI power management stack beyond the
> S5 shutdown that already works. This covers S1 CPU-halt idle, S3 suspend-to-RAM, S4 hibernate-to-disk, PCI/device D-states (D0–D3cold), the ACPI Embedded Controller (EC) driver required for every laptop, battery and AC adapter status, power button and lid-close event handling, driver power callbacks with correct resume ordering, connected standby (S0ix / Modern Standby), and the power-plan UI. Without this, Impossible OS has no viable story on laptops or any real hardware that expects ACPI power events.

> [!IMPORTANT]
> **Current state:** `src/kernel/acpi.c` (618 lines) implements:
> RSDP→RSDT/XSDT chain walk, FADT extraction (PM1a control port, PM timer), MADT (LAPIC/IOAPIC/ISA overrides), `acpi_shutdown()` via `\_S5_` AML parse + PM1a_CNT write, and ACPI reset register. Everything else in this TODO is
> **greenfield**: no `\_S3_`/`\_S4_` parsing, no sleep-state entry, no CPU
> state save/restore, no hibernation image, no EC driver, no battery, no power button events, no device D-states.

> [!CAUTION]
> **Memory rule:** Hibernation image buffers can be multi-gigabyte — always
> use `pmm_alloc_contiguous()` for hibernation scratch pages. Never `kmalloc` anything > 4 KiB in the suspend/hibernate paths.

> [!IMPORTANT]
> **Scope boundary with `04-drivers-hardware/TODO-04-acpi-power-management.md`:** TODO-04 is the authoritative ACPI power management implementation (ACPICA-based AML interpreter). §1, §3, §4, §5, §6, and §7 of this TODO are the *pre-ACPICA* implementation path — they use hand-rolled AML parsing (same pattern as the existing `\_S5_` parser) and provide usable functionality before TODO-04 §1 (ACPICA) is complete. Once TODO-04 §1 lands, TODO-04 §2 (S3), §3 (battery), §5 (power button), and §8 (S4) supersede the equivalent sections here. **§2 (S1/idle thread), §8 (PCI D-states), §9 (driver callbacks), §10 (S0ix), and §11 (powercfg/UI) are kernel-core responsibilities not covered by TODO-04 and remain authoritative.**
> → XREF: `04-drivers-hardware/TODO-04-acpi-power-management.md §1` — ACPICA integration; when complete, replaces hand-rolled AML parsing in §1 of this TODO
> → XREF: `04-drivers-hardware/TODO-04-acpi-power-management.md §2` — authoritative S3 suspend/resume (ACPICA path); §3 here is the pre-ACPICA fallback
> → XREF: `04-drivers-hardware/TODO-04-acpi-power-management.md §3` — authoritative battery `_BST`/`_BIF` (ACPICA path); §6 here is the pre-ACPICA fallback
> → XREF: `04-drivers-hardware/TODO-04-acpi-power-management.md §5` — authoritative power button SCI (ACPICA path); §7 here is the pre-ACPICA fallback
> → XREF: `04-drivers-hardware/TODO-04-acpi-power-management.md §8` — authoritative S4 hibernate (ACPICA path); §4 here is the pre-ACPICA fallback

---

## Inputs

- `src/kernel/acpi.c` — existing ACPI parser; extend for new sleep objects
- `include/kernel/acpi.h` — ACPI types (FADT, RSDP, MADT)
- `src/kernel/drivers/pci.c` — PCI config space read/write (D-state §8)
- `src/kernel/sched/task.c` — scheduler freeze for S3/S4
- `src/kernel/mm/vmm.c` — page table save for S3 wakeup identity map
- → XREF: `TODO-06-irql-model-dpcs.md §3` — DPCs and IRQL transitions must be quiesced before entering any sleep state; `KeLowerIrql(PASSIVE_LEVEL)` required on resume
- → XREF: `TODO-07-time-filetime-management.md §3` — TSC must be recalibrated after S3/S0ix wake (clock drift); `acpi_pm_timer_read()` used as reference; §3 = Invariant TSC Detection and Per-CPU Offset Calibration
- → XREF: `TODO-07-time-filetime-management.md §14` — S3/S4 resume path must call `ke_suspend_bias_update()` to adjust `InterruptTimeBias` by the sleep duration; §14 = Suspend/Hibernate Time Bias Tracking
- → XREF: `TODO-01-kernel-init-sequencing.md §3` — S4 resume check runs early in Phase 1; must distinguish cold boot from hibernate resume via hibernation signature
- → XREF: `04-drivers-hardware/TODO-01-kernel-module-system.md §4` — driver model HAL vtables required for USB xHCI to register power callbacks; xHCI D3cold→D0 handled via callback registered in §9
- → XREF: `05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md §1` — IXFS WAL journal (`ixfs_journal_begin`/`commit`/`abort`) must be verified (§1 Subsystem Verification) before S4 journal-flush dependency is safe; storage driver must reach D0 before journal replay on resume
- → XREF: `TODO-05-native-api-ssdt.md §4` — SSDT indices 0x00D7 (NtShutdownSystem) and 0x0140–0x0145 (NtSetSystemPowerState, NtInitiatePowerAction, NtPowerInformation, NtGetDevicePowerState, NtSetThreadExecutionState, NtRequestWakeupLatency) reserved for this TODO; §12 wires them into the SSDT

---

## Outcome

- S1 (CPU halt) reduces power during idle; no visible effect on software state.
- S3 (suspend to RAM) saves/restores CPU registers and device state within
  < 2 s on modern hardware; resumes to the desktop without reboot.
- S4 (hibernate) writes a compressed RAM image to the swap/hibernate partition; resumes from power-off faster than a cold boot for typical working sets.
- ACPI EC driver enables all ACPI battery/lid/hotkey events on laptops.
- Battery status (charge %, AC/DC, time remaining) appears in the system tray.
- Power button and lid close trigger configurable actions (sleep/hibernate/ shutdown/lock) read from Registry.
- Every PCI device has a D-state machine; drivers register sleep/wake callbacks; resume ordering (storage before filesystem before scheduler) is enforced.
- Connected Standby (S0ix) enables network-keepalive standby on supported Intel/AMD platforms.
- `powercfg` shell command and Power Options `sysdm.cpl` tab let users configure power plans.

---

## Implementation Order

| ⭐  | Order | Deliverable                                         | Depends On               | Status |
| --- | :---: | --------------------------------------------------- | ------------------------ | :----: |
| 💎  |   1   | ACPI sleep object parsing & PM1 state machine       | —                        |  [ ]   |
| 💎  |   2   | S1: CPU halt / idle thread integration              | 1                        |  [ ]   |
| 💎  |   3   | S3: suspend to RAM (CPU state + driver freeze)      | 1, 2, TODO-06-irql-model-dpcs.md §3 |  [ ]   |
| 💎  |   4   | S4: hibernate to disk (image write + resume)        | 3                     |  [ ]   |
| 💎  |   5   | ACPI Embedded Controller (EC) driver                | 1                        |  [ ]   |
| 💎  |   6   | Battery & AC adapter (`_BIF`/`_BST`)                | 5                        |  [ ]   |
| 💎  |   7   | Power button & lid-close events                     | 5                        |  [ ]   |
| 💎  |   8   | PCI device D-states (D0–D3cold)                     | 1                        |  [ ]   |
| 💎  |   9   | Driver power callbacks & resume ordering            | 3, 8                     |  [ ]   |
| ⭐  |  10   | Connected Standby (S0ix / Modern Standby)           | 2, 9, TODO-06-irql-model-dpcs.md §3 |  [ ]   |
| 💎  |  11   | Power plan UI & `powercfg`                          | 6, 7, 9                  |  [ ]   |
| 💎  |  12   | Power syscalls wired to SSDT                        | §2, §6, TODO-05 §4      |  [ ]   |

> 💎 = parity work — matches what Windows 11 and Linux already do.
> ⭐ = exclusive work — Impossible OS is superior or first.

---

## 1. ACPI Sleep Object Parsing & PM1 State Machine `[Opus]`

### 1.1 Sleep type values for S1–S4

- [ ] Consolidate ACPI init: split into `acpi_platform_init()` (Phase 1: MADT/FADT parsing, existing) and `acpi_power_init()` (Phase 2: S-state discovery, new) — moved from TODO-01 §8
- [ ] Extend `src/kernel/acpi.c` to parse `\_S1_`, `\_S3_`, and `\_S4_` AML objects using the same pattern as the existing `\_S5_` parser:
  ```c
  static uint16_t slp_typa_s1 = ACPI_SLP_TYPE_INVALID;
  static uint16_t slp_typa_s3 = ACPI_SLP_TYPE_INVALID;
  static uint16_t slp_typa_s4 = ACPI_SLP_TYPE_INVALID;
  #define ACPI_SLP_TYPE_INVALID 0xFFFF
  ```
- [ ] `acpi_parse_sleep_objects()` — scan DSDT bytecode for `"_S1_"`, `"_S3_"`, `"_S4_"` name operations; extract `SLP_TYPa`/`SLP_TYPb` byte values from the Package; called from `acpi_init()` after DSDT is located
- [ ] `acpi_sleep_supported(n)` — returns `true` if `slp_typa_sN != ACPI_SLP_TYPE_INVALID`; used by the power manager to populate the list of available sleep states

### 1.2 PM1 sleep entry

- [ ] `acpi_enter_sleep_state(uint8_t state)` — generic sleep entry:
  1. Disable all non-wakeup interrupts (mask IOAPIC, disable PIC)
  2. Clear `SLP_EN` bit in PM1a_CNT
  3. Write `(slp_typa_sN << 10) | SLP_EN` to PM1a_CNT; repeat for PM1b_CNT if present
  4. `__asm__ volatile("hlt")` — CPU stops here; wakeup resumes after this point for S1; for S3/S4 the CPU loses context and resumes at the wakeup vector

### 1.3 ACPI fixed events

- [ ] Enable the relevant PM1 fixed-event enable bits in `acpi_init()`:
  - `PWRBTN_EN (bit 8)` in PM1a_EN — power button press
  - `SLPBTN_EN (bit 9)` in PM1a_EN — sleep button press
  - `WAK_STS (bit 15)` in PM1a_STS — clear wake status on resume
- [ ] `acpi_pm1_isr()` — handle SCI interrupt (ACPI System Control Interrupt, typically IRQ 9); read PM1a_STS; dispatch to `acpi_power_button_event()` or `acpi_sleep_button_event()` (§7)

### 1.4 Commit

- [ ] Commit: `"kernel/acpi: S1/S3/S4 sleep type parsing, PM1 state machine, fixed-event ISR"`

---

## 2. S1: CPU Halt / Idle Thread Integration `[Sonnet]`

### 2.1 S1 entry

- [ ] S1 is a low-latency power-saving state: the CPU executes `HLT` but retains all register state and cache; system bus power is reduced
- [ ] `acpi_enter_s1()`:
  - Call `acpi_enter_sleep_state(1)`
  - On resume (next interrupt wakes the CPU): re-enable interrupts and return immediately — no state restore needed for S1
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

- [ ] `pm_enter_s3()` — called by the power manager when the user requests sleep; runs at `DISPATCH_LEVEL` (→ XREF `TODO-06-irql-model-dpcs.md §3`):
  1. Broadcast `PO_CB_SYSTEM_STATE_LOCK` to all registered power callbacks (§9): let drivers flush queues and reach D3hot/D3cold (§8)
  2. Freeze the scheduler (`sched_freeze_all()`) — no new threads start; all CPUs except the one doing suspend park themselves at a spin barrier
  3. Flush VFS page cache and IXFS journal (→ XREF `05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md §1`)
  4. Save APIC state (LVT registers, LAPIC base MSR) to a per-CPU save area
  5. Save `IDTR`, `GDTR`, `CR0`, `CR3`, `CR4`, `EFER` per CPU
  6. Save all CPU general-purpose and SSE registers for the BSP (`struct s3_cpu_state` allocated in pinned physical memory)
  7. Write the physical address of `pm_s3_wakeup_entry` into the ACPI wakeup vector (`FACS->FirmwareWakingVector`)
  8. Call `acpi_enter_sleep_state(3)` — system loses power to RAM row refresh; wake on power button / RTC alarm resumes at §3.2

### 3.2 S3 wakeup path

- [ ] `pm_s3_wakeup_entry` (real-mode compatible entry stub in `src/kernel/acpi_wakeup.asm`):
  - BIOS/UEFI firmware jumps here in real mode; stub switches to protected and then long mode (re-using the bootloader's page tables at `0x70000`)
  - Calls `pm_s3_resume()` in C with the saved state pointer
- [ ] `pm_s3_resume()`:
  1. Restore IDTR, GDTR, CR0, CR3, CR4, EFER, APIC from save area
  2. Re-initialise IOAPIC routing (MADT-based)
  3. Restore BSP general-purpose + SSE registers
  4. Wake AP CPUs: write `INIT`→`SIPI`→`SIPI` IPI sequence; each AP restores its own saved state and un-parks from the spin barrier
  5. Recalibrate TSC (→ XREF `TODO-07-time-filetime-management.md §3`) — PM timer used as reference
  6. Compute sleep duration from RTC/UEFI time delta; call `ke_suspend_bias_update()` (→ XREF `TODO-07-time-filetime-management.md §14`)
  7. Call `pm_notify_resume()` (§9) — drivers transition back D3→D0
  8. Unfreeze scheduler; resume from the instruction after `acpi_enter_sleep_state(3)`

### 3.3 Wakeup sources

- [ ] Power button physical press → PM1 fixed event (§1.3) generates SCI; firmware raises the CPU from S3
- [ ] RTC alarm: `acpi_set_wakeup_alarm(seconds)` — programs CMOS RTC alarm registers (port 0x70/0x71), sets `RTC_EN` in PM1a_EN; used for timed wake (→ `Task Scheduler` integration, future)
- [ ] USB device activity: `XHCI_S3_WAKEUP_EN` — xHCI remote-wakeup enable bit in the USB port status register (→ XREF `04-drivers-hardware/TODO-09-usb-stack.md`)

### 3.4 Commit

- [ ] Commit: `"kernel/acpi: S3 suspend-to-RAM, wakeup vector, CPU state save/restore, AP re-init"`

---

## 4. S4: Hibernate to Disk `[Opus]`

### 4.1 Hibernation image format

- [ ] Hibernation image header in `include/kernel/pm/hibernate.h`:
  ```c
  #define HIBER_MAGIC  0x4945424F524150 /* "RAPOBRIE" — "Reboot Impossible" */
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
  5. Call `acpi_enter_sleep_state(4)` — system powers off; same as S5 but firmware knows to look for hibernation image on next boot

### 4.3 Hibernation resume on boot

- [ ] In kernel init (→ XREF `TODO-01-kernel-init-sequencing.md §3`), early in Phase 1: check the hibernation partition for a valid `HIBR_HEADER.magic`; if found and `kernel_version` matches: enter hibernation resume path
- [ ] `pm_hibernate_resume()`:
  1. Read all compressed chunks from the partition; decompress into a separate bounce buffer
  2. CRC32C verify the full image; halt with `KERNEL_HIBERNATE_CORRUPT` (→ XREF `TODO-16-crash-dump-generation.md §1`) if mismatch
  3. Copy pages from bounce buffer back to their original physical addresses; restore CR3, RSP, RIP from `HIBR_HEADER`
  4. Jump to resume RIP — execution resumes from inside `pm_hibernate_write()` as if `acpi_enter_sleep_state(4)` just returned
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

- [ ] `ec_read(uint8_t reg, uint8_t *val)` — polling path:
  1. Wait for `IBF=0` (EC ready to receive) with timeout 100 µs × 1 000 retries
  2. `outb(EC_SC, EC_CMD_READ)` then wait `IBF=0`; `outb(EC_DATA, reg)`
  3. Wait `OBF=1` (response ready); `*val = inb(EC_DATA)`
- [ ] `ec_write(uint8_t reg, uint8_t val)` — same pattern, `EC_CMD_WRITE`
- [ ] `ec_burst_mode_enter()` / `ec_burst_mode_exit()` — use burst mode for multi-byte reads to avoid partial reads during battery polling
- [ ] Interrupt-driven path: ACPI SCI fires with `SCI_EVT` set → `outb(EC_SC, EC_CMD_QUERY)` → read `OBF` byte → dispatch to registered EC query handler (battery change, lid event, hotkey) by query number

### 5.3 ECDT discovery and init

- [ ] `acpi_ec_init()` — called from `acpi_init()` after MADT:
  1. Search for ECDT table via `acpi_find_table("ECDT")`; extract `ec_control` (SC port) and `ec_data` (DATA port) and `uid`
  2. If no ECDT: walk DSDT for `_HID "PNP0C09"` device node; read `_CRS` for EC I/O port addresses
  3. Register `acpi_ec_sci_handler()` as the SCI dispatch target for EC query events
- [ ] `acpi_ec_ready()` — boolean; used by battery/lid/hotkey drivers to check readiness before calling `ec_read`/`ec_write`

### 5.4 Commit

- [ ] Commit: `"kernel/acpi: Embedded Controller driver, ECDT discovery, burst-mode EC transactions"`

---

## 6. Battery & AC Adapter `[Sonnet]`

### 6.1 ACPI battery methods

- [ ] Evaluate `\_SB.BAT0._BIF` (Battery Info) on init:
  - Returns: design capacity, full charge capacity, technology (primary/secondary), design voltage, warn capacity, low capacity, granularity, model, serial, chemistry
- [ ] Evaluate `\_SB.BAT0._BST` (Battery Status) every 30 s or on EC event:
  - Returns: power state (discharging=1, charging=2, critical=4), present rate (mW), remaining capacity (mWh), present voltage (mV)
- [ ] `bat_update()` — reads `_BST`; computes `charge_pct = remaining / full * 100`; `time_remaining_min = remaining / rate * 60`; stores in `bat_state_t`
- [ ] Evaluate `\_SB.ACAD._PSR` (AC Adapter Power Source): 0=offline, 1=online
- [ ] Register Registry key `HKLM\SYSTEM\Battery\Status` (REG_BINARY) updated on each `bat_update()` call; apps can use `RegNotifyChangeKeyValue` to watch for power changes (→ XREF `TODO-13-registry-completion.md §3`)

### 6.2 Low-battery warnings

- [ ] `pm_check_battery_warn()` — called from `bat_update()`:
  - `charge_pct ≤ warn_pct` (Registry `PowerWarnPercent`, default 10%): post `WM_POWERBROADCAST (PBT_APMBATTERYLOWAGE)` to all top-level windows
  - `charge_pct ≤ critical_pct` (Registry `PowerCriticalPercent`, default 5%): trigger `pm_enter_s4()` (hibernate) or OS shutdown based on `PowerCriticalAction` Registry value

### 6.3 Battery tray icon

- [ ] System tray icon: `⚡` charging, `🔋` n% discharging, `🔌` AC plugged in
- [ ] Tooltip: `"Battery: 73% — 2h 14m remaining"` or `"Plugged in, charging"`
- [ ] Click → flyout with charge bar, current rate (W), temperature if `_BTP` supported, last full charge capacity vs. design capacity (battery wear indicator)

### 6.4 Commit

- [ ] Commit: `"kernel/acpi: battery _BIF/_BST, AC adapter, low-battery warnings, tray icon"`

---

## 7. Power Button & Lid-Close Events `[Sonnet]`

### 7.1 Power button

- [ ] PM1 fixed event (§1.3) fires SCI with `PWRBTN_STS` set
- [ ] `acpi_power_button_event()` — dispatch based on Registry `HKLM\SYSTEM\PowerControl\PowerButtonAction`:
  - `0` = do nothing (ignore)
  - `1` = sleep (S3)
  - `2` = hibernate (S4)
  - `3` = shutdown (S5) — default
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

- [ ] `pci_pmcap_find(dev)` — walk PCI Capabilities linked list (cap ID `0x01` = Power Management) in config space; return cap offset or -1
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
- [ ] `pci_set_d_state(dev, state)` — write `state & 0x3` to `PMCSR` power state bits; wait 10 ms for D3hot→D0 transition (PCI spec minimum); return `PCI_DX_OK` or `PCI_DX_UNSUPPORTED` if no PM capability

### 8.2 D3cold via ACPI _PR3/_PS3

- [ ] D3cold (power completely removed) requires platform support: evaluate `\_SB.PCI0.DEV._PS3` ACPI method (if present) to cut VCC to the device; `_PS0` to restore power for D3cold→D0
- [ ] `pci_d3cold_enter(dev)` — call `pci_set_d_state(dev, 3)` first (D3hot), then evaluate `_PS3`; note: device config space is inaccessible in D3cold
- [ ] `pci_d3cold_exit(dev)` — evaluate `_PS0`; wait `_D0D3COLD_DELAY` ms (ACPI `_DSM` if present, else 100 ms default); then `pci_set_d_state(dev, 0)`

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
- [ ] `pm_register_device(dev, on_sleep, on_wake)` — called by each PCI driver at probe time; adds to the global `pm_device_list`

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

- [ ] `pm_notify_sleep(state)` — iterates `pm_device_list` in **reverse** priority order (user-space → graphics → input → USB → network → storage):
  1. Call `cb->on_sleep(state, ctx)` — driver flushes queues, stops DMA, calls `pci_set_d_state(dev, 3)` for D3hot
  2. Wait for `on_sleep` to return (max 2 s per driver; if it hangs, log `[WARN] pm: driver sleep callback timeout` and proceed)

### 9.3 Post-resume ordering (forward priority)

- [ ] `pm_notify_resume(state)` — iterates in **forward** priority order (storage first, then network, then USB, then graphics, then user-space):
  1. Call `pci_set_d_state(dev, 0)` — device back to D0
  2. Call `cb->on_wake(state, ctx)` — driver re-initialises DMA, re-arms interrupts, re-establishes network/USB links
  3. Wait up to 5 s for storage drivers (`PM_PRI_STORAGE`) before allowing the scheduler to unfreeze; critical to prevent filesystem access before the disk controller is ready

### 9.4 Commit

- [ ] Commit: `"kernel/acpi: pm_register_power_callback, sleep/wake ordering, driver notification"`

---

## 10. Connected Standby (S0ix / Modern Standby) `[Opus]`

### 10.1 S0ix detection

- [ ] Check FADT `LOW_POWER_S0_IDLE_CAPABLE` flag (bit 21 of `Flags` field, ACPI 5.0+); if set: the platform supports connected standby and S3 may not be in the `\_Sx_` objects at all
- [ ] `acpi_s0ix_supported()` — returns true if `LOW_POWER_S0_IDLE_CAPABLE` and the `_DSM` with `{GUID: S0ix}` is present in the DSDT
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

## 11. Power Plan UI & `powercfg` `[Sonnet]`

### 11.1 Power plan Registry schema

- [ ] Stored under `HKLM\SYSTEM\PowerPlans\{GUID}\`:
  - `Name` (REG_SZ): `"Balanced"`, `"Power Saver"`, `"High Performance"`
  - `SleepTimeout` (REG_DWORD): seconds to S3 on idle (0=never)
  - `HibernateTimeout` (REG_DWORD): seconds to S4 after S3 (0=never)
  - `DisplayOffTimeout` (REG_DWORD): seconds to blank display
  - `PowerButtonAction` (REG_DWORD): same codes as §7.1
  - `LidCloseAction` (REG_DWORD): same codes as §7.3
- [ ] `pm_apply_plan(guid)` — reads plan Registry values; sets idle timers in the scheduler's DPC timer (→ XREF `TODO-06-irql-model-dpcs.md §3`)
- [ ] Default plan GUIDs match Windows 11's well-known GUIDs:
  - Balanced: `{381b4222-f694-41f0-9685-ff5bb260df2e}`
  - Power Saver: `{a1841308-3541-4fab-bc81-f71556f20b4a}`
  - High Performance: `{8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c}`

### 11.2 powercfg shell command

- [ ] `powercfg /list` — print all power plans with active indicator (`*`)
- [ ] `powercfg /setactive {GUID}` — activate a plan; calls `pm_apply_plan`
- [ ] `powercfg /query` — print current plan's settings (timeout values, button actions)
- [ ] `powercfg /hibernate on|off` — enable/disable S4; writes `HKLM\SYSTEM\PowerControl\HibernateEnabled`
- [ ] `powercfg /sleep on|off` — enable/disable S3; writes `HKLM\SYSTEM\PowerControl\SleepEnabled`
- [ ] `powercfg /batteryreport` — print battery design capacity, full charge capacity, wear level (%), current charge, and last 24 h discharge history

### 11.3 Power Options in System Properties

- [ ] `sysdm.cpl` Power tab (or dedicated `powercpl.cpl`):
  - Plan selector radio buttons
  - "Change plan settings" expands to sliders: Screen off / Sleep / Hibernate timeouts
  - "Advanced settings" tree: power button action, lid action, low-battery action (matches the Registry schema)
- [ ] Changes written to Registry via `RegSetValueEx`; `pm_apply_plan()` called immediately after save

### 11.4 Commit

- [ ] Commit: `"kernel/acpi: powercfg shell command, power plan Registry schema, Power Options UI"`

## 12. Power Syscalls Wired to SSDT
Register all power management NtXxx entry points in the SSDT so user-mode code can invoke them via `syscall`. (→ XREF: TODO-05-native-api-ssdt.md §4, §21)

- [ ] `NtShutdownSystem(Action)` → SSDT 0x00D7: call `pm_shutdown()` / `pm_reboot()` based on action; requires `SeShutdownPrivilege`
- [ ] `NtSetSystemPowerState(SystemAction, LightestSystemState, Flags)` → SSDT 0x0140: route through ACPI S-state transition (§2)
- [ ] `NtInitiatePowerAction(SystemAction, LightestSystemState, Flags, Asynchronous)` → SSDT 0x0141: async power action initiation
- [ ] `NtPowerInformation(InformationLevel, InputBuffer, InputLen, OutputBuffer, OutputLen)` → SSDT 0x0142: return battery state, processor info, S-state capabilities from §6
- [ ] `NtGetDevicePowerState(Device, State)` → SSDT 0x0143: query PCI device D-state (§8)
- [ ] `NtSetThreadExecutionState(NewFlags, PreviousFlags)` → SSDT 0x0144: `ES_SYSTEM_REQUIRED` / `ES_DISPLAY_REQUIRED` prevents idle sleep
- [ ] `NtRequestWakeupLatency(Latency)` → SSDT 0x0145: hint to power manager about acceptable wake latency
- [ ] All functions return `NTSTATUS`; use codes from `include/kernel/nt/ntstatus.h` (TODO-05 §1)
- [ ] Commit: `"kernel/acpi: wire power syscalls to SSDT (0x00D7, 0x0140–0x0145)"`

**Test checkpoint:** `NtShutdownSystem(ShutdownReboot)` triggers ACPI reset. `NtPowerInformation(SystemPowerCapabilities)` returns valid S-state mask. `NtSetThreadExecutionState(ES_SYSTEM_REQUIRED)` prevents idle sleep during long operation.

---

## OS Comparison


| ⭐ | Feature                                | 🪟 Win11                             | 🐧 Linux                             | 🚀 Impossible OS                  |
|----|----------------------------------------|-----------------------------------|-----------------------------------|--------------------------------|
| 💎 | S5 shutdown via ACPI PM1a              | ✅ Full                           | ✅ Full                           | ✅ Done — `acpi_shutdown()`    |
| 💎 | S1 CPU halt / idle                     | ✅ Full                           | ✅ Full (`cpuidle`)               | ⬜ §2                          |
| 💎 | S3 suspend to RAM                      | ✅ Full                           | ✅ Full (systemd-sleep)           | ⬜ §3                          |
| 💎 | S4 hibernate to disk                   | ✅ Full                           | ✅ Full (hibernate image)         | ⬜ §4                          |
| 💎 | ACPI Embedded Controller (EC) driver   | ✅ Full                           | ✅ Full (`acpi_ec`)               | ⬜ §5                          |
| 💎 | Battery status & AC adapter            | ✅ Full                           | ✅ Full (`upower`, `UPower DBus`) | ⬜ §6                          |
| 💎 | Power button & lid-close events        | ✅ Full                           | ✅ Full (`logind`)                | ⬜ §7                          |
| 💎 | PCI D-states                           | ✅ Full                           | ✅ Full (PCI PM)                  | ⬜ §8                          |
| 💎 | Driver sleep/wake callbacks            | ✅ Full (WDM `IRP_MJ_POWER`)      | ✅ Full (driver `pm_ops`)         | ⬜ §9                          |
| 💎 | Connected Standby / S0ix               | ✅ Modern Standby (S0 Low Power)  | ⚠️ Partial (Intel-specific)       | ⬜ §10                         |
| 💎 | `powercfg` CLI                         | ✅ Full                           | ⚠️ `systemctl suspend` (no plans) | ⬜ §11                         |
| 💎 | Power Options GUI                      | ✅ `powercpl.dll` / Control Panel | ⚠️ GNOME Settings (basic)         | ⬜ §11 — .3                    |
| ⭐ | Battery wear indicator in tray tooltip | ❌ Requires Settings app          | ❌ Requires `upower -i`           | ⬜ §6 — .3 🚀                  |
| ⭐ | `powercfg /batteryreport` with history | ✅ HTML only                      | ❌ Not available                  | ⬜ §11 — .2 (CLI + plain text) |

After §1–9, Impossible OS reaches full Windows 11 and Linux parity for every power-management scenario that matters on real hardware — S1/S3/S4, EC, battery, power/lid events, D-states, and driver callbacks. Linux relies on a patchwork of kernel drivers, `systemd-logind`, and `upower` D-Bus; the integration is fragmented and the UI is distribution-dependent. Impossible OS delivers a single coherent stack from hardware events to Registry-backed power plans to a consistent `powercfg` CLI. The battery-wear indicator in the tray tooltip and the plain-text `powercfg /batteryreport` are quality-of-life exclusives that require navigating menus or parsing HTML on Windows.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_power()` (→ XREF: `00-infrastructure/TODO-03-kernel-test-framework.md`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_power.c` with:
  - ACPI sleep type lookup: `acpi_get_slp_typ(S5)` returns valid SLP_TYPa/b values
  - Power state query: `PoGetSystemPowerState()` returns `PowerSystemWorking` during boot
  - Device D-state: `PoSetDevicePowerState(dev, D0)` succeeds for active device
  - Idle detection: CPU idle counter increments when no work scheduled
  - Thermal zone: `acpi_get_temperature()` returns plausible value (20–100°C) or graceful skip if no zone
  - Shutdown path: `acpi_shutdown()` writes correct PM1a_CNT value (verify register, don't actually shut down)
  - Reboot path: `acpi_reboot()` writes to correct reset register
- [ ] Register in `test_runner_init()`: `test_register_power()`
- [ ] Commit: `"test: add power management test suite"`

---

## Verification

- [ ] **S1 idle**: run `powercfg /query`; set `SleepTimeout=0`, `HibernateTimeout=0`; confirm CPU stays at `HLT` when scheduler is idle (verify via PMU idle counter).
- [ ] **S3 round-trip in QEMU**: QEMU supports S3 with `-machine q35,acpi=on`; call `pm_enter_s3()` from the shell; verify system re-appears at the desktop with all tasks intact and TSC recalibrated (serial log shows `[TSC] recalibrated after S3 wake`).
- [ ] **S4 round-trip in QEMU**: enable a hibernation partition; call `pm_enter_s4()`; power off QEMU; restart; verify `[HIBER] resuming from image` in serial log and desktop restores to pre-hibernate state.
- [ ] **EC smoke test**: on a real laptop or QEMU with DSDT that includes `PNP0C09`; `acpi_ec_ready()` returns true; `ec_read(0x10, &val)` completes without timeout.
- [ ] **Battery**: `powercfg /batteryreport` reports non-zero design capacity and remaining charge on a machine with ACPI `_BIF`/`_BST`.
- [ ] **Power button**: press power button → S5 shutdown executes within 10 s.
- [ ] **D-state**: after S3 resume, verify AHCI controller is back in D0 via `pci_get_d_state(ahci_dev)` returning `0`.
- [ ] **Remaining limits**: connected standby (§10) requires Intel LPSS hardware; QEMU does not support `LOW_POWER_S0_IDLE_CAPABLE` — skip in CI; D3cold `_PS3` ACPI method evaluation deferred until the AML interpreter is complete.
- [ ] Commit: `"kernel/acpi: S1/S3/S4 power management, EC driver, battery, D-states, driver callbacks, powercfg"`
