---
schema_version: 1
id: power-management
domain: 02-kernel-core
status: active
title: "TODO-26 -- Power Management (S-States, D-States, Thermal & Idle)"
---

# TODO-26 -- Power Management (S-States, D-States, Thermal & Idle)

> **Goal:** Implement the complete ACPI power management stack beyond the S5 shutdown that already works. This covers S1 CPU-halt idle, S3 suspend-to-RAM, S4 hibernate-to-disk, fast startup (hybrid shutdown / hiberboot), PCI/device D-states (D0--D3cold), runtime device idle management, the ACPI Embedded Controller (EC) driver required for every laptop, battery and AC adapter status (`_BIF`/`_BIX`/`_BST`), power button and lid-close event handling, driver power callbacks with query/veto and correct resume ordering, ACPI thermal zone management (`_TMP`/`_CRT`/`_HOT`/`_PSV`/`_ACx`) with passive and active cooling, CPU idle governor framework (C-states via `_CST`/`MWAIT`), CPU frequency scaling governor framework (HWP/CPPC/`_PSS`), connected standby (S0ix / Modern Standby), power request tracking, wake source management, and the power-plan UI. Without this, Impossible OS has no viable story on laptops or any real hardware that expects ACPI power events.

> [!IMPORTANT]
> **Current state:** `src/kernel/acpi.c` implements RSDP through XSDT walk, FADT (PM1a control, PM timer), MADT, `acpi_shutdown()` / `acpi_reboot()`, and **`acpi_power_init()`** which parses `\_S1_`, `\_S3_`, and `\_S4_` from the DSDT via **`parse_sleep_type()`**, plus **`acpi_sleep_supported()`** / **`acpi_get_slp_typa()`** and the `Sleep states: S1=...` klog line. **`acpi_enter_sleep_state()`** performs the ACPI PM1a/b SLP_TYP+SLP_EN sequence then `sti; hlt` (S1-style wake only today; S3/S4 still need §3/§4 state save, FACS vector, and firmware resume). **`acpi_enable_fixed_events()`** and **`acpi_register_sci()`** / **`acpi_sci_handler()`** enable PM1 fixed-event SCI path (logs today; §7 owns user-visible dispatch). Phase 2 **`boot_storage.c`** wires `acpi_power_init()`, `acpi_enable_fixed_events()`, and `acpi_register_sci()` after timer init. **`src/kernel/test/test_acpi_power.c`** covers §1 discovery and unsupported-state rejection. **Still greenfield:** EC (§5), battery (§6), S3/S4 (§3/§4), scheduler S1 idle (§2), PCI D-states (§8), governors and `powercfg` (§15+), power syscalls (§20), Linux sysfs parity doc (§21).

> [!CAUTION]
> **Memory rule:** Hibernation image buffers can be multi-gigabyte: always use `pmm_alloc_contiguous()` for hibernation scratch pages. Never `kmalloc` anything > 4 KiB in the suspend/hibernate paths.

> [!IMPORTANT]
> **Scope boundary with `04-drivers-hardware/TODO-03-acpi-power-management.md`:** TODO-26 is the authoritative ACPI power management implementation (ACPICA-based AML interpreter). §1, §3, §4, §5, §6, and §7 of this TODO are the *pre-ACPICA* implementation path; they use hand-rolled AML parsing (same pattern as the existing `\_S5_` parser) and provide usable functionality before TODO-26 §1 (ACPICA) is complete. Once TODO-26 §1 lands, TODO-26 §2 (S3), §3 (battery), §5 (power button), and §8 (S4) supersede the equivalent sections here. **§2 (S1/idle thread), §8 (PCI D-states), §9 (driver callbacks), §10 (S0ix), and §18 (power plan UI / `powercfg`) are kernel-core responsibilities not covered by TODO-26 and remain authoritative.**
> → XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md §1` -- ACPICA integration; when complete, replaces hand-rolled AML parsing in §1 of this TODO
> → XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md §9` -- authoritative S3 suspend/resume (ACPICA path); §5 here is the pre-ACPICA fallback
> → XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md §5` -- authoritative battery `_BST`/`_BIF` (ACPICA path); §4 here is the pre-ACPICA fallback
> → XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md §3` -- authoritative power button SCI (ACPICA path); §8 here is the pre-ACPICA fallback
> → XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md §10` -- authoritative S4 hibernate (ACPICA path); §6 here is the pre-ACPICA fallback

---

## Inputs

- `src/kernel/acpi.c` -- existing ACPI parser; extend for new sleep objects
- `include/kernel/pm/sleep.h` (planned, not in tree yet): `pm_sleep_variant_t`, `pm_get_sleep_variants()` (§21)
- `docs/kernel/pm-linux-sysfs-parity.md` (planned, not in tree yet): Linux `mem_sleep` parity doc (§21; create under `todo/02-kernel-core/` if `docs/kernel/` not present)
- `include/kernel/acpi.h` -- ACPI types (FADT, RSDP, MADT)
- `src/kernel/drivers/pci.c` -- PCI config space read/write (D-state §8)
- `src/kernel/sched/task.c` -- scheduler freeze for S3/S4
- `src/kernel/mm/vmm.c` -- page table save for S3 wakeup identity map
- → XREF: `TODO-07-irql-model-dpcs.md §3` -- DPCs and IRQL transitions must be quiesced before entering any sleep state; `KeLowerIrql(PASSIVE_LEVEL)` required on resume
- → XREF: `TODO-08-time-filetime-management.md §1` -- TSC must be recalibrated after S3/S0ix wake (clock drift); `acpi_pm_timer_read()` used as reference; §1 = Invariant TSC Detection and Per-CPU Offset Calibration
- → XREF: `TODO-08-time-filetime-management.md §14` -- S3/S4 resume path must call `ke_suspend_bias_update()` to adjust `InterruptTimeBias` by the sleep duration; §14 = Suspend/Hibernate Time Bias Tracking
- → XREF: `TODO-01-kernel-init-sequencing.md §3` -- S4 resume check runs early in Phase 1; must distinguish cold boot from hibernate resume via hibernation signature
- → XREF: `04-drivers-hardware/TODO-05-kernel-module-system.md §3` -- driver model HAL vtables required for USB xHCI to register power callbacks; xHCI D3cold->D0 handled via callback registered in §9
- → XREF: `05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md §1` -- IXFS WAL journal (`ixfs_journal_begin`/`commit`/`abort`) must be verified (§1 Subsystem Verification) before S4 journal-flush dependency is safe; storage driver must reach D0 before journal replay on resume
- → XREF: `TODO-12-native-api-ssdt.md §5` -- SSDT indices 0x00D7 (NtShutdownSystem) and 0x0140--0x0145 (NtSetSystemPowerState, NtInitiatePowerAction, NtPowerInformation, NtGetDevicePowerState, NtSetThreadExecutionState, NtRequestWakeupLatency) reserved for this TODO; TODO-12 §22 wires power syscalls into the SSDT
- → XREF: `01-boot-platform/TODO-11-interrupt-timer-arch.md §2` -- LAPIC timer recalibration required after HWP/CPPC frequency changes (§15 CPU frequency scaling)
- → XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md §6` -- ACPICA-based `_PSS` P-state parsing; §15 here owns the kernel-core governor framework that consumes it
- → XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md §4` -- ACPICA-based `IA32_THERM_STATUS` per-core temp; §14 here owns the ACPI thermal zone framework
- → XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md §8` -- ACPICA-based `_CST` C-state parsing; §16 here owns the kernel-core idle governor
- → XREF: `03-memory-concurrency/TODO-06-scheduler-enhancement.md §9` -- `cpufreq_register_driver()` vtable consumed by §15; scheduler provides load metrics for governor
- → XREF: `02-kernel-core/TODO-09-x86-64-architecture.md §10` -- Intel hybrid P/E-core detection feeds §15 HWP/CPPC governor with core asymmetry data
- → XREF: `04-drivers-hardware/TODO-17-gpu-display-drivers.md` -- GPU power management (DPMS, RTD3 runtime D3, Panel Self-Refresh) owned by GPU TODO; §18 `DisplayOffTimeout` triggers DPMS via `gfx_set_dpms(DPMS_OFF)`

---

## Outcome

- S1 (CPU halt) reduces power during idle; no visible effect on software state.
- S3 (suspend to RAM) saves/restores CPU registers and device state within < 2 s on modern hardware; resumes to the desktop without reboot.
- S4 (hibernate) writes a compressed RAM image to the swap/hibernate partition; resumes from power-off faster than a cold boot for typical working sets.
- Fast startup (hybrid shutdown) hibernates only the kernel session for < 5 s boot times.
- ACPI EC driver enables all ACPI battery/lid/hotkey events on laptops.
- Battery status (charge %, AC/DC, time remaining, wear level via `_BIX`) appears in the system tray.
- Power button and lid close trigger configurable actions (sleep, hibernate, shutdown, lock) read from Registry.
- Every PCI device has a D-state machine; drivers register sleep/wake callbacks with query/veto support; resume ordering (storage before filesystem before scheduler) is enforced.
- Runtime device idle puts individual devices into low-power states when unused, without full system sleep.
- ACPI thermal zones enforce passive cooling (CPU throttle) and active cooling (fan control) with trip points (`_CRT`/`_HOT`/`_PSV`/`_ACx`).
- CPU idle governor selects optimal C-state (C1 HLT through C10 MWAIT) based on predicted idle duration and latency budget.
- CPU frequency governor switches P-states via HWP/CPPC or `_PSS`/`IA32_PERF_CTL` based on scheduler load.
- Connected Standby (S0ix) enables network-keepalive standby on supported Intel/AMD platforms.
- Power request tracking shows which applications are preventing sleep (`powercfg /requests`).
- Wake source registry provides `powercfg /lastwake` and `powercfg /waketimers` diagnostics.
- `powercfg` shell command and Power Options `sysdm.cpl` tab let users configure power plans.
- Documented parity mapping from Linux `/sys/power` suspend variants (`mem_sleep` s2idle/shallow/deep) to Impossible OS S0ix/S1/S3 policy knobs (§21).

---

## Implementation Order

| ⭐   | Order | Deliverable                              | Depends On                 | Status |
| --- | :---: | ---------------------------------------- | -------------------------- | :----: |
| 💎   |   1   | §1 ACPI sleep object parsing & PM1 state machine | (none)                     |  [x]   |
| 💎   |   2   | §2 S1: CPU halt / idle thread integration | §1                         |  [ ]   |
| 💎   |   3   | §3 S3: suspend to RAM (CPU state + driver freeze) | §1, §2, D02T06§3           |  [ ]   |
| 💎   |   4   | §4 S4: hibernate to disk (image write + resume) | §3                         |  [ ]   |
| 💎   |   5   | §5 ACPI Embedded Controller (EC) driver  | §1                         |  [ ]   |
| 💎   |   6   | §6 Battery & AC adapter (`_BIF`/`_BIX`/`_BST`) | §5                         |  [ ]   |
| 💎   |   7   | §7 Power button & lid-close events       | §5                         |  [ ]   |
| 💎   |   8   | §8 PCI device D-states (D0--D3cold)      | §1                         |  [ ]   |
| 💎   |   9   | §9 Driver power callbacks & resume ordering | §3, §8                     |  [ ]   |
| ⭐   |  10   | §10 Connected Standby (S0ix / Modern Standby) | §2, §9, D02T06§3           |  [ ]   |
| 💎   |  11   | §11 Fast Startup (hybrid shutdown / hiberboot) | §4, §9                     |  [ ]   |
| 💎   |  12   | §12 Runtime device idle management       | §8, §9                     |  [ ]   |
| 💎   |  13   | §13 Power request tracking & wake source management | §9, §12                    |  [ ]   |
| 💎   |  14   | §14 ACPI thermal zone management         | §5, D04T04§1               |  [ ]   |
| 💎   |  15   | §15 CPU frequency scaling governor framework | §2, D04T04§4, D03T05§9     |  [ ]   |
| 💎   |  16   | §16 CPU idle governor framework          | §2, D04T04§7               |  [ ]   |
| 💎   |  17   | §17 Driver power query & veto (IRP_MN_QUERY_POWER) | §9                         |  [ ]   |
| 💎   |  18   | §18 Power plan UI & `powercfg`           | §6, §7, §13, §14, §15, §16 |  [ ]   |
| ⭐   |  19   | §19 Energy-aware scheduling integration  | §15, §16, D02T19§9         |  [ ]   |
| 💎   |  20   | §20 Power syscalls wired to SSDT         | §2, §6, D02T05§4           |  [ ]   |
| 💎   |  21   | §21 Linux `/sys/power` suspend variant parity | §1, §10                    |  [ ]   |

> 💎 = parity work: matches what Windows 11 and Linux already do.
> ⭐ = exclusive work: Impossible OS is superior or first.
> Compact XREF notation: D=domain, T=TODO, §=section (e.g. D02T06§3 = domain 02, TODO-17, §3).

---

## 1. ACPI Sleep Object Parsing & PM1 State Machine

- [x] `acpi_power_init()` added (Phase 2): parses `\_S1_`, `\_S3_`, `\_S4_` from DSDT using generalized `parse_sleep_type()` (refactored from `parse_s5_from_dsdt`)
- [x] `slp_typa_s1/s3/s4` with `ACPI_SLP_TYPE_INVALID = 0xFFFF` sentinel
- [x] `acpi_sleep_supported(n)` -- returns 1 if sleep state N has a valid SLP_TYPa
- [x] `acpi_get_slp_typa(n)` -- returns the SLP_TYPa value for sleep state N
- [x] Wired into Phase 2 boot (`boot_storage.c`) after time subsystem init
- [x] Serial log: `"Sleep states: S1=yes/no S3=yes/no S4=yes/no S5=yes"`
- [x] `acpi_enter_sleep_state(uint8_t state)` -- generic sleep entry:
  1. Validates state support via `acpi_get_slp_typa()`
  2. `cli` -- disables interrupts
  3. Clears `SLP_EN` bit in PM1a_CNT (ACPI spec requirement before write)
  4. Writes `(SLP_TYPa << 10) | SLP_EN` to PM1a_CNT; repeats for PM1b_CNT if present
  5. `sti; hlt` -- CPU halts; S1 resumes on wakeup interrupt; S3/S4 require wakeup vector (§3/§4)
- [x] `acpi_enable_fixed_events()`: enables PWRBTN_EN (bit 8) + SLPBTN_EN (bit 9) in PM1a_EN; clears pending status
- [x] `acpi_register_sci()`: registers SCI ISR on vector 32+sci_interrupt (typically IRQ 9 = vec 41)
- [x] `acpi_sci_handler()`: reads PM1a_STS, dispatches PWRBTN_STS/SLPBTN_STS/WAK_STS, clears status bits, EOI
- [x] Wired into Phase 2 boot after `acpi_power_init()`
- [x] Commit: `"kernel/acpi: S1/S3/S4 sleep type parsing, PM1 state machine, fixed-event ISR"`

**Test checkpoint:** `acpi_sleep_supported(5)` returns 1. `acpi_get_slp_typa(S5)` != 0xFFFF. `acpi_enter_sleep_state(2)` returns -1 (unsupported). `acpi_enter_sleep_state(6)` returns -1 (invalid). 7 tests in `test_acpi_power.c`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 2. S1: CPU Halt / Idle Thread Integration

- [ ] S1 is a low-latency power-saving state: the CPU executes `HLT` but retains all register state and cache; system bus power is reduced
- [ ] `acpi_enter_s1()`:
  - Call `acpi_enter_sleep_state(1)`
  - On resume (next interrupt wakes the CPU): re-enable interrupts and return immediately; no state restore needed for S1
- [ ] S1 is entered only if `acpi_sleep_supported(1)`; otherwise fall back to a plain `HLT` loop
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
- [ ] When `TODO-29-kernel-debugger-kd-protocol.md` ships `kd_present` / `kd_breakin_requested`, treat any pending breakin as not-deep-idle-safe: return false (or run `kd_poll()` once) before `acpi_enter_s1()` so WinDbg breakin bytes are not delayed behind a halted CPU (-> XREF `TODO-29-kernel-debugger-kd-protocol.md §4` `kd_poll`, §15)
- [ ] Per-CPU idle tracking: accumulate `idle_tsc_cycles` counter per CPU; exposed via `NtQuerySystemInformation(SystemProcessorIdleInformation)` for power-usage telemetry
- [ ] Commit: `"kernel/acpi: S1 CPU halt, idle thread power-saving integration"`

**Test checkpoint:** `acpi_enter_s1()` halts CPU; resumes on next interrupt. `sched_idle_cpu()` uses S1 when available, falls back to `HLT`. `pm_deep_idle_allowed()` returns false when DPCs pending. Per-CPU `idle_tsc_cycles` counter increments during idle. Test on: QEMU TCG + WHPX.

---

## 3. S3: Suspend to RAM
- [ ] `pm_enter_s3()` -- called by the power manager when the user requests sleep; runs at `DISPATCH_LEVEL` (→ XREF: `TODO-07-irql-model-dpcs.md §3`):
  1. Broadcast `PO_CB_SYSTEM_STATE_LOCK` to all registered power callbacks (§9): let drivers flush queues and reach D3hot/D3cold (§8)
  2. Freeze the scheduler (`sched_freeze_all()`) -- no new threads start; all CPUs except the one doing suspend park themselves at a spin barrier
  3. Flush VFS page cache and IXFS journal (→ XREF: `05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md §1`)
  4. Save APIC state (LVT registers, LAPIC base MSR) to a per-CPU save area
  5. Save `IDTR`, `GDTR`, `CR0`, `CR3`, `CR4`, `EFER` per CPU
  6. Save all CPU general-purpose and SSE registers for the BSP (`struct s3_cpu_state` allocated in pinned physical memory)
  7. Write the physical address of `pm_s3_wakeup_entry` into the ACPI wakeup vector (`FACS->FirmwareWakingVector`)
  8. Call `acpi_enter_sleep_state(3)` -- system loses power to RAM row refresh; wake on power button / RTC alarm resumes in §3
- [ ] `pm_s3_wakeup_entry` (real-mode compatible entry stub in `src/kernel/acpi_wakeup.asm`):
  - BIOS/UEFI firmware jumps here in real mode; stub switches to protected and then long mode (re-using the bootloader's page tables at `0x70000`)
  - Calls `pm_s3_resume()` in C with the saved state pointer
- [ ] `pm_s3_resume()`:
  1. Restore IDTR, GDTR, CR0, CR3, CR4, EFER, APIC from save area
  2. Re-initialise IOAPIC routing (MADT-based)
  3. Restore BSP general-purpose + SSE registers
  4. Wake AP CPUs: write `INIT`->`SIPI`->`SIPI` IPI sequence; each AP restores its own saved state and un-parks from the spin barrier
  5. Recalibrate TSC (→ XREF: `TODO-08-time-filetime-management.md §1`) -- PM timer used as reference
  6. Compute sleep duration from RTC/UEFI time delta; call `ke_suspend_bias_update()` (→ XREF: `TODO-08-time-filetime-management.md §14`)
  7. Call `pm_notify_resume()` (§9) -- drivers transition back D3->D0
  8. Unfreeze scheduler; resume from the instruction after `acpi_enter_sleep_state(3)`
- [ ] **Call `uefi_secureboot_refresh()` after runtime services come back online and before re-entering userspace** (filed 2026-05-01 from [`01-boot-platform/TODO-02-uefi-hardening-secureboot.md §5`](../01-boot-platform/TODO-02-uefi-hardening-secureboot.md#5-secure-boot-state-detection)): an attacker with physical access can clear SetupMode and re-add SecureBoot keys while the OS sleeps; the refresh API re-reads SecureBoot/SetupMode/AuditMode/DeployedMode/PK/KEK and emits LOG_FATAL + sets `HKLM\SYSTEM\SecureBoot\Drift = 1` on mismatch with the boot snapshot. Must run after `pm_notify_resume()` finishes (storage + registry back to D0) and before user threads unblock.
- [ ] Power button physical press -> PM1 fixed event (§1) generates SCI; firmware raises the CPU from S3
- [ ] RTC alarm: `acpi_set_wakeup_alarm(seconds)` -- programs CMOS RTC alarm registers (port 0x70/0x71), sets `RTC_EN` in PM1a_EN; used for timed wake (-> `Task Scheduler` integration, future)
- [ ] USB device activity: `XHCI_S3_WAKEUP_EN` -- xHCI remote-wakeup enable bit in the USB port status register (→ XREF: `04-drivers-hardware/TODO-10-usb-stack.md`)
- [ ] Commit: `"kernel/acpi: S3 suspend-to-RAM, wakeup vector, CPU state save/restore, AP re-init"`

**Test checkpoint:** `struct s3_cpu_state` saves/restores all GPRs + CR0/CR3/CR4/EFER. `FACS->FirmwareWakingVector` set to `pm_s3_wakeup_entry` physical address. AP re-init SIPI sequence completes. TSC recalibrated after wake. `ke_suspend_bias_update()` adjusts `InterruptTimeBias`. Test on: QEMU TCG (`-machine q35,acpi=on`).

---

## 4. S4: Hibernate to Disk

> [!NOTE] Ownership boundary (TODO-26 gap-audit 2026-06-17): the bootloader (`01-boot-platform/TODO-26`) owns hibernation image DISCOVERY, eligibility policy (Secure Boot/db/topology/slot invalidation), anti-replay, integrity, and the boot_info handoff. This section owns image WRITING (`pm_hibernate_write`, AEAD encryption, the on-disk header per TODO-26 §1) and the kernel RESUME CONSUMER (`pm_hibernate_resume`) reached via the `BOOT_PAYLOAD_HIBERNATION_META` handoff -- NOT a Phase-1 partition scan + policy decision. The legacy `HIBR_HEADER` below is superseded by TODO-26 §1's authoritative metadata format (adds boot_info ABI version, root volume id, Secure Boot/PCR state, a `resume_generation` anti-replay counter, and AEAD encryption metadata). -> XREF: `01-boot-platform/TODO-26 §1,§2,§4,§5`.

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
- [ ] `pm_hibernate_write()`:
  1. Pre-suspend sequence identical to §3 (drivers to D3, scheduler freeze, journal flush)
  2. Open the hibernation partition: IXFS raw block device (C:\ partition reserved region), or a dedicated swap partition identified by GPT type GUID `{HIBER-GUID}`; retrieve via `blkdev_open_by_gpt_type(HIBER_GUID)`
  3. Walk PMM used-page list; for each page: compress 64-page chunk with LZ4; write to hibernation partition via DMA (must reach D0 device state first)
  4. Write `HIBR_HEADER` at offset 0 with final `image_pages` count and CRC32C
  5. Call `acpi_enter_sleep_state(4)` -- system powers off; same as S5 but firmware knows to look for hibernation image on next boot
- [ ] Encryption: AEAD-encrypt the image (AES-GCM) with a TPM-sealed key; write cipher/key-id/nonce/tag into the header so the bootloader can require it and refuse plaintext. -> XREF: `01-boot-platform/TODO-26 §4`
- [ ] Resume ENTRY is the bootloader's job (supersedes the old Phase-1 scan): it discovers + validates + selects, then hands off via `BOOT_PAYLOAD_HIBERNATION_META`. -> XREF: `01-boot-platform/TODO-26 §2-§5`
- [ ] `pm_hibernate_resume()`:
  1. Read all compressed chunks from the partition; decompress into a separate bounce buffer
  2. CRC32C verify the full image; halt with `KERNEL_HIBERNATE_CORRUPT` (→ XREF: `TODO-27-crash-dump-generation.md §1`) if mismatch
  3. Copy pages from bounce buffer back to their original physical addresses; restore CR3, RSP, RIP from `HIBR_HEADER`
  4. Jump to resume RIP -- execution resumes from inside `pm_hibernate_write()` as if `acpi_enter_sleep_state(4)` just returned
  5. Run §3 resume steps 4--7 (recalibrate TSC, notify drivers, unfreeze scheduler)
- [ ] If `kernel_version` mismatches (updated kernel after hibernate): discard the image; cold boot; log `[HIBER] image version mismatch`
- [ ] Commit: `"kernel/acpi: S4 hibernation image write/resume, LZ4 compression, version guard"`

**Test checkpoint:** `sizeof(HIBR_HEADER)` == 4096. `HIBR_HEADER.magic == HIBER_MAGIC`. LZ4 compress/decompress round-trips test page. CRC32C mismatch triggers `KERNEL_HIBERNATE_CORRUPT`. Version mismatch discards image and cold boots. Test on: QEMU TCG (with hibernation partition).

---

## 5. ACPI Embedded Controller (EC) Driver
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
- [ ] `ec_read(uint8_t reg, uint8_t *val)` -- polling path:
  1. Wait for `IBF=0` (EC ready to receive) with timeout 100 µs × 1 000 retries
  2. `outb(EC_SC, EC_CMD_READ)` then wait `IBF=0`; `outb(EC_DATA, reg)`
  3. Wait `OBF=1` (response ready); `*val = inb(EC_DATA)`
- [ ] `ec_write(uint8_t reg, uint8_t val)` -- same pattern, `EC_CMD_WRITE`
- [ ] `ec_burst_mode_enter()` / `ec_burst_mode_exit()` -- use burst mode for multi-byte reads to avoid partial reads during battery polling
- [ ] Interrupt-driven path: ACPI SCI fires with `SCI_EVT` set -> `outb(EC_SC, EC_CMD_QUERY)` -> read `OBF` byte -> dispatch to registered EC query handler (battery change, lid event, hotkey) by query number
- [ ] `acpi_ec_init()` -- called from `acpi_init()` after MADT:
  1. Search for ECDT table via `acpi_find_table("ECDT")`; extract `ec_control` (SC port) and `ec_data` (DATA port) and `uid`
  2. If no ECDT: walk DSDT for `_HID "PNP0C09"` device node; read `_CRS` for EC I/O port addresses
  3. Register `acpi_ec_sci_handler()` as the SCI dispatch target for EC query events
- [ ] `acpi_ec_ready()` -- boolean; used by battery/lid/hotkey drivers to check readiness before calling `ec_read`/`ec_write`
- [ ] Commit: `"kernel/acpi: Embedded Controller driver, ECDT discovery, burst-mode EC transactions"`

**Test checkpoint:** `acpi_ec_ready()` returns true (or skip if no ECDT/PNP0C09). `ec_read(0x10, &val)` completes without timeout. `ec_write(reg, val)` round-trips. Burst mode enter/exit toggles `EC_SC_BURST`. SCI query dispatches to registered handler. Test on: bare metal (real EC required); QEMU skip.

---

## 6. Battery & AC Adapter
- [ ] Evaluate `\_SB.BAT0._BIX` (Battery Information Extended, ACPI 4.0+) on init; fall back to `_BIF` (deprecated) if `_BIX` is not present:
  - `_BIX` returns: revision, power unit, design capacity, last full charge capacity, technology, design voltage, design capacity of warning, design capacity of low, cycle count, measurement accuracy, max sampling time, min sampling time, max/min average interval, battery capacity granularity, model number, serial number, battery type, OEM info
  - `_BIF` returns: design capacity, full charge capacity, technology, design voltage, warn capacity, low capacity, granularity, model, serial, chemistry
- [ ] Evaluate `\_SB.BAT0._BST` (Battery Status) every 30 s or on EC event:
  - Returns: power state (discharging=1, charging=2, critical=4), present rate (mW), remaining capacity (mWh), present voltage (mV)
- [ ] `bat_update()` -- reads `_BST`; computes `charge_pct = remaining / full * 100`; `time_remaining_min = remaining / rate * 60`; stores in `bat_state_t`
- [ ] Battery cycle count and wear level: `wear_pct = (1 - last_full_cap / design_cap) * 100`; available from `_BIX.cycle_count` and capacity ratio
- [ ] Evaluate `\_SB.ACAD._PSR` (AC Adapter Power Source): 0=offline, 1=online
- [ ] Register Registry key `HKLM\SYSTEM\Battery\Status` (REG_BINARY) updated on each `bat_update()` call; apps can use `RegNotifyChangeKeyValue` to watch for power changes (→ XREF: `TODO-14-registry-completion.md §4`)
- [ ] `pm_check_battery_warn()` -- called from `bat_update()`:
  - `charge_pct <= warn_pct` (Registry `PowerWarnPercent`, default 10%): post `WM_POWERBROADCAST (PBT_APMBATTERYLOWAGE)` to all top-level windows
  - `charge_pct <= critical_pct` (Registry `PowerCriticalPercent`, default 5%): trigger `pm_enter_s4()` (hibernate) or OS shutdown based on `PowerCriticalAction` Registry value
- [ ] System tray icon: `[CHG]` charging, `[BAT] n%` discharging, `[AC]` AC plugged in
- [ ] Tooltip: `"Battery: 73% -- 2h 14m remaining"` or `"Plugged in, charging"`
- [ ] Click -> flyout with charge bar, current rate (W), temperature if `_BTP` supported, last full charge capacity vs. design capacity (battery wear indicator)
- [ ] `bat_set_charge_limit(uint8_t pct)` -- write charge threshold via EC: `ec_write(EC_REG_CHARGE_END, pct)` (register address is vendor-specific; common: 0xB1 ThinkPad, 0xE4 Dell, 0xBD ASUS); default 100% (no limit)
- [ ] Registry `HKLM\SYSTEM\Battery\ChargeLimitPercent` (REG_DWORD, default 100); set to 80 for battery longevity
- [ ] Smart Charging auto-mode: if laptop has been plugged in for > 4 hours continuously and battery > 80%, auto-hold at 80%; release limit when unplugged; Registry `SmartChargingEnabled` (default 1)
- [ ] Tray tooltip addition: `"Charging limited to 80%"` when charge limit active
- [ ] `powercfg /batteryreport` includes charge limit status and smart charging history
- [ ] Commit: `"kernel/acpi: battery _BIF/_BST, AC adapter, smart charging, low-battery warnings, tray icon"`

**Test checkpoint:** `bat_update()` computes `charge_pct` and `time_remaining_min`. `_BIX` parsed (or `_BIF` fallback). `_PSR` returns AC status. `bat_set_charge_limit(80)` writes EC register. Low-battery warning fires at `warn_pct`. Registry `Battery\Status` updated. Test on: QEMU TCG (skip battery tests if no `_BST`).

---

## 7. Power Button & Lid-Close Events
- [ ] PM1 fixed event (§1) fires SCI with `PWRBTN_STS` set
- [ ] `acpi_power_button_event()` -- dispatch based on Registry `HKLM\SYSTEM\PowerControl\PowerButtonAction`:
  - `0` = do nothing (ignore)
  - `1` = sleep (S3)
  - `2` = hibernate (S4)
  - `3` = shutdown (S5) -- default
  - `4` = lock screen
- [ ] If an interactive user session is active: first post `WM_QUERYENDSESSION` to all windows (give apps a chance to save); wait up to 5 s; then execute the action regardless
- [ ] `SLPBTN_STS` fixed event -> `acpi_sleep_button_event()`:
  - Default action: S3 suspend (Registry `HKLM\SYSTEM\PowerControl\SleepButtonAction`, default `1`)
- [ ] EC event (§5) or ACPI GPE fires when lid state changes; read `\_SB.LID0._LID`: 0=closed, 1=open
- [ ] Lid close action (Registry `PowerLidCloseAction`, default `1`=sleep): same action table as power button
- [ ] Lid open: if system is in S3/S4, trigger wakeup (the EC event itself causes the hardware to resume; software sees `WAKE_STS` in PM1a_STS -> §3 or §4 resume path)
- [ ] Display-off on lid close before entering sleep: call `gfx_blank_display()` to cut video output immediately, reducing flicker during the sleep entry sequence
- [ ] HPD sensor integration: detect compatible IR/ToF camera or Wi-Fi sensing via ACPI `_HID "INTC1070"` (Intel HPD) or `HID_DEVICE_SYSTEM_HUMAN_PRESENCE` (0x000D0011)
- [ ] `hpd_register_sensor(dev, ops)` -- register HPD sensor driver with presence/absence callbacks
- [ ] Wake on Approach: when display is off (idle timeout) and HPD reports `PRESENCE_DETECTED`, power on display and optionally unlock (biometric); Registry `HPDWakeOnApproach` (default 1)
- [ ] Lock on Leave: when HPD reports `ABSENCE_DETECTED` for > `HPDAbsenceTimeout` seconds (default 30), trigger display-off + lock screen; Registry `HPDLockOnLeave` (default 1)
- [ ] Attention-Aware Dimming: if HPD reports `GAZE_AWAY` (supported sensors only), dim backlight after 10 s; restore on `GAZE_DETECTED`
- [ ] Boot log: `[HPD] Sensor: %s, wake-on-approach=%s, lock-on-leave=%s`
- [ ] Commit: `"kernel/acpi: power button, sleep button, lid-close, HPD presence events"`

**Test checkpoint:** Power button SCI dispatches `acpi_power_button_event()`. Registry `PowerButtonAction=3` triggers shutdown. `SLPBTN_STS` triggers sleep. Lid close reads `_LID` via EC. HPD sensor registration accepted (or skip if no HPD device). Test on: QEMU TCG (power button SCI testable; lid/HPD skip).

---

## 8. PCI Device D-States (D0--D3cold)
- [ ] `pci_pmcap_find(dev)` -- walk PCI Capabilities linked list (cap ID `0x01` = Power Management) in config space; return cap offset or -1
- [ ] `pci_pmcap_read(dev)` -> `PCI_PMCAP` struct:
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
- [ ] `pci_set_d_state(dev, state)` -- write `state & 0x3` to `PMCSR` power state bits; wait 10 ms for D3hot->D0 transition (PCI spec minimum); return `PCI_DX_OK` or `PCI_DX_UNSUPPORTED` if no PM capability
- [ ] D3cold (power completely removed) requires platform support: evaluate `\_SB.PCI0.DEV._PS3` ACPI method (if present) to cut VCC to the device; `_PS0` to restore power for D3cold->D0
- [ ] `pci_d3cold_enter(dev)` -- call `pci_set_d_state(dev, 3)` first (D3hot), then evaluate `_PS3`; note: device config space is inaccessible in D3cold
- [ ] `pci_d3cold_exit(dev)` -- evaluate `_PS0`; wait `_D0D3COLD_DELAY` ms (ACPI `_DSM` if present, else 100 ms default); then `pci_set_d_state(dev, 0)`
- [ ] `pm_device_t` struct registered per PCI device:
  ```c
  typedef struct {
      uint8_t  bus, dev, fn;   /* PCI BDF */
      uint8_t  current_d_state; /* 0--3 */
      uint8_t  target_d_state;  /* requested by power manager */
      uint8_t  d3cold_capable;
      void    *driver_ctx;
      pm_power_callback_t on_sleep;  /* §9 */
      pm_power_callback_t on_wake;   /* §9 */
  } pm_device_t;
  ```
- [ ] `pm_register_device(dev, on_sleep, on_wake)` -- called by each PCI driver at probe time; adds to the global `pm_device_list`
- [ ] Commit: `"kernel/acpi: PCI D-state machine, D0/D3hot/D3cold transitions, pm_register_device"`

**Test checkpoint:** `pci_pmcap_find(dev)` returns valid offset for PM-capable device (or -1 for device without PM cap). `pci_set_d_state(dev, 3)` writes PMCSR. `pci_set_d_state(dev, 0)` restores D0 after 10 ms delay. `pm_register_device()` adds to global list. Test on: QEMU TCG + WHPX.

---

## 9. Driver Power Callbacks & Resume Ordering
- [ ] `pm_register_power_callback(priority, on_sleep, on_wake, ctx)`:
  - `priority`: `PM_PRI_STORAGE=0`, `PM_PRI_NETWORK=1`, `PM_PRI_USB=2`, `PM_PRI_INPUT=3`, `PM_PRI_GRAPHICS=4`, `PM_PRI_USER=5`
  - Static array of 64 callback slots; sorted by priority
- [ ] Each major driver registers in its `init()`:
  - `ahci_init()` -> `PM_PRI_STORAGE`
  - `xhci_init()` -> `PM_PRI_USB`
  - `rtl8139_init()` -> `PM_PRI_NETWORK`
  - `framebuffer_init()` -> `PM_PRI_GRAPHICS`
- [ ] `pm_notify_sleep(state)` -- iterates `pm_device_list` in **reverse** priority order (user-space -> graphics -> input -> USB -> network -> storage):
  1. Call `cb->on_sleep(state, ctx)` -- driver flushes queues, stops DMA, calls `pci_set_d_state(dev, 3)` for D3hot
  2. Wait for `on_sleep` to return (max 2 s per driver; if it hangs, log `[WARN] pm: driver sleep callback timeout` and proceed)
- [ ] `pm_notify_resume(state)` -- iterates in **forward** priority order (storage first, then network, then USB, then graphics, then user-space):
  1. Call `pci_set_d_state(dev, 0)` -- device back to D0
  2. Call `cb->on_wake(state, ctx)` -- driver re-initialises DMA, re-arms interrupts, re-establishes network/USB links
  3. Wait up to 5 s for storage drivers (`PM_PRI_STORAGE`) before allowing the scheduler to unfreeze; critical to prevent filesystem access before the disk controller is ready
- [ ] Commit: `"kernel/acpi: pm_register_power_callback, sleep/wake ordering, driver notification"`

**Test checkpoint:** `pm_register_power_callback(PM_PRI_STORAGE, ...)` accepted. Sleep notification iterates in reverse priority (user->storage). Resume iterates forward (storage->user). Callback timeout (> 2 s) logged and skipped. 64 callback slots max. Test on: QEMU TCG.

---

## 10. Connected Standby (S0ix / Modern Standby)
- [ ] Check FADT `LOW_POWER_S0_IDLE_CAPABLE` flag (bit 21 of `Flags` field, ACPI 5.0+); if set: the platform supports connected standby and S3 may not be in the `\_Sx_` objects at all
- [ ] `acpi_s0ix_supported()` -- returns true if `LOW_POWER_S0_IDLE_CAPABLE` and the `_DSM` with `{GUID: S0ix}` is present in the DSDT
- [ ] On such platforms, `pm_enter_s3()` (§3) is replaced by `pm_enter_s0ix()` transparently; the sleep state entry point remains the same for the rest of the OS
- [ ] `pm_enter_s0ix()`:
  1. Move all CPUs to the lowest available Intel C-state (`MWAIT` with C7/C10 hint via `cpuid` extended topology leaf)
  2. Gate DRAM self-refresh: set `MC_PM_STS` power gate bit in the Memory Controller MMIO space (Intel-specific; skip on AMD)
  3. Notify platform firmware via `\_OSC` (OS Capabilities) ACPI method that the OS is entering S0ix
  4. `__asm__ volatile ("mwait" : : "a"(MWAIT_HINT_C10) : "memory")` on each CPU; the hardware enters the deepest idle state; wakeup restores execution after `mwait`
- [ ] Network keepalive: the NIC (if `_DSM` advertises DRIPS/D0ix support) remains powered in D0i3 state for ARP/IPv6 NS replies and WoL packets; `rtl8139_d0i3_enter()` / `rtl8139_d0i3_exit()` stubs (full implementation depends on the specific NIC driver)
- [ ] Any interrupt or I/O wakes the CPU from `mwait`; execution resumes immediately after the `mwait` instruction; no page table or register restore needed (unlike S3)
- [ ] Call `pm_notify_resume(PM_RESUME_S0IX)` (§9) to un-gate devices
- [ ] TSC recalibration (→ XREF: `TODO-08-time-filetime-management.md §1`) may be needed if `mwait` C10 was held for > 1 second (TSC stops in deep C-states on some CPUs)
- [ ] Directed PoFx (DFx, PoFx v3): the power manager *directs* entire device stacks to enter low-power during Modern Standby idle when no activator-brokered activity; unlike runtime PM where the device self-idles, DFx is top-down OS-directed
- [ ] `pm_dfx_power_down(dev_stack)` -- OS calls `PO_FX_DIRECTED_POWER_DOWN_CALLBACK` on each driver in the stack; driver must save state, stop DMA, enter D3
- [ ] `pm_dfx_power_up(dev_stack)` -- called on activator wake or system exit from S0ix; driver restores state
- [ ] DRIPS (Deepest Runtime Idle Platform State) tracking: `drips_pct = time_all_devices_idle / total_s0ix_time * 100`; target > 95% for good battery life; exposed via `powercfg /sleepstudy`
- [ ] Devices that block DRIPS logged: `[S0IX] DRIPS blocker: %s (active for %u ms)` -- helps diagnose battery drain
- [ ] ARP offload: program NIC hardware to respond to ARP requests while CPU sleeps; `nic_add_arp_offload(ipv4_addr)` writes to NIC offload registers (driver-specific)
- [ ] IPv6 Neighbor Solicitation offload: `nic_add_ns_offload(ipv6_addr)` -- NIC responds to NS without waking CPU
- [ ] Wake-on-LAN: `nic_set_wol(dev, WAKE_MAGIC | WAKE_PATTERN)` -- configure Magic Packet wake and pattern-match wake via NIC `WOL_CR` register
- [ ] Wake-on-Pattern: `nic_add_wake_pattern(dev, pattern, mask, offset)` -- wake CPU on specific packet match (e.g. incoming VoIP SIP INVITE)
- [ ] NIC D0i3 entry/exit callbacks registered via §12 runtime PM; NIC maintains minimal firmware for offload processing
- [ ] Commit: `"kernel/acpi: connected standby S0ix, DFx directed power, MWAIT C10, network offloads"`

**Test checkpoint:** `acpi_s0ix_supported()` reads FADT `LOW_POWER_S0_IDLE_CAPABLE` flag. MWAIT with C10 hint accepted on supported CPU. DFx `pm_dfx_power_down()` transitions device stack to D3. DRIPS % computed. ARP/NS offload registers written (or skip if no NIC). Test on: QEMU TCG (S0ix flag not set -- graceful skip expected).

---

## 11. Fast Startup (Hybrid Shutdown / Hiberboot)

Windows 11's fast startup hibernates only the kernel session (no user processes) on shutdown, enabling < 5 s boot times by restoring the kernel image instead of cold-booting. This is a significant competitive feature -- Linux has no equivalent.
- [ ] `HIBERBOOT_HEADER` -- same layout as `HIBR_HEADER` (§4) but with `type = HIBER_TYPE_FAST_STARTUP` flag to distinguish from full S4 hibernate
- [ ] Only kernel session pages are saved: kernel heap, PMM metadata, loaded driver images, Registry hives, VFS cache (no user-process address spaces)
- [ ] `pm_hiberboot_page_filter(phys_addr)` -- returns true if the page belongs to kernel session; skips user-mode process pages, reducing image size by 60--80%
- [ ] `pm_fast_shutdown()`:
  1. Log off all user sessions (close all user processes; same as normal shutdown)
  2. Flush Registry hives and VFS page cache
  3. Freeze scheduler; park APs
  4. Walk PMM used-page list with `pm_hiberboot_page_filter()` -- compress and write only kernel-session pages to hibernation partition
  5. Write `HIBERBOOT_HEADER` with `type = HIBER_TYPE_FAST_STARTUP`
  6. Power off via `acpi_enter_sleep_state(5)` (S5)
- [ ] Registry key `HKLM\SYSTEM\PowerControl\FastStartupEnabled` (REG_DWORD, default 1): enables/disables fast startup
- [ ] `powercfg /hibernate on` must be enabled for fast startup to work (reuses hibernation partition)
- [ ] Bootloader detects `HIBERBOOT_HEADER` with fast startup flag; sets `boot_info.flags |= BOOT_FAST_STARTUP`
- [ ] `pm_fast_startup_resume()` -- same as §4 hibernate resume but skips user-process page restoration; kernel drivers see `IRP_MN_SET_POWER(S0)` with `SystemPowerAction = PowerActionHibernate` (same as hibernate wake) -- drivers must call `PoFxReportDevicePoweredOn()` equivalent
- [ ] After kernel restore: `smss.exe` / session manager starts fresh user sessions from scratch (unlike hibernate where user sessions are restored)
- [ ] Distinguish fast startup from hibernate wake: check `HIBERBOOT_HEADER.type`; expose `PoGetSystemPowerStateFlags(FAST_STARTUP)` for drivers
- [ ] Commit: `"kernel/pm: fast startup -- hiberboot image, kernel-only page filter, resume path"`

**Test checkpoint:** `HIBERBOOT_HEADER.type == HIBER_TYPE_FAST_STARTUP`. `pm_hiberboot_page_filter()` returns true for kernel heap pages, false for user-process pages. Image size < full hibernate (page filter reduces by 60%+). `PoGetSystemPowerStateFlags(FAST_STARTUP)` distinguishes from full hibernate. Test on: QEMU TCG.

---

## 12. Runtime Device Idle Management

Per-device runtime idle management -- equivalent to Windows PoFx (Power Management Framework) component-level idle and Linux runtime PM (`pm_runtime_get`/`pm_runtime_put`). Devices autonomously enter low-power states when idle without requiring full system sleep.
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
- [ ] When reference count reaches 0: start a DPC timer with `idle_timeout_ms` delay (→ XREF: `TODO-07-irql-model-dpcs.md §3`)
- [ ] Timer expiry: call `ops->runtime_idle(ctx)`; if returns 0 (device can suspend): call `ops->runtime_suspend(ctx)` to transition to low-power state
- [ ] `pm_runtime_set_autosuspend_delay(dev, ms)` -- adjustable per device; storage controllers use longer delays (2000 ms); input devices use shorter (500 ms)
- [ ] `pm_runtime_get(dev)` on a suspended device: call `ops->runtime_resume(ctx)` synchronously before returning
- [ ] Before system S3/S4 entry: all runtime-active devices are suspended via `ops->runtime_suspend()`; runtime-suspended devices remain suspended
- [ ] On system resume: only devices that were runtime-active before system sleep are resumed; runtime-suspended devices stay suspended until next `pm_runtime_get()`
- [ ] Boot log: `[PM_RT] %s: autosuspend after %u ms idle`
- [ ] Windows PoFx extends beyond device-level to per-component power states (F0=active, F1..Fn=progressively deeper idle); a single device can have independently managed components (e.g. audio playback vs recording engine)
- [ ] `pm_component_t` per subdevice component:
  ```c
  typedef struct {
      uint32_t component_index;
      uint8_t  current_f_state;  /* F0=active, F1..Fn=idle */
      uint8_t  deepest_f_state;  /* max supported Fx */
      uint32_t idle_timeout_ms;
      uint64_t residency_req_ns; /* min time in Fx to be worthwhile */
      uint64_t latency_req_ns;   /* max acceptable F0 restore latency */
  } pm_component_t;
  ```
- [ ] `pm_fx_register_device(dev, component_count, components[])` -- register multi-component device
- [ ] `pm_fx_activate_component(dev, idx)` -- transition component to F0; blocks until active
- [ ] `pm_fx_idle_component(dev, idx)` -- mark component idle; starts Fx timer
- [ ] `pm_fx_set_component_latency(dev, idx, latency_ns)` -- constrains deepest Fx per QoS
- [ ] USB selective suspend: per-device idle policy; when USB device has no pending transfers for `idle_timeout_ms` (default: 5000 ms HID, 15000 ms storage), transition port to suspended state via xHCI Port Status register `PLS=U3`
- [ ] `usb_idle_register(dev, callback, timeout_ms)` -- driver registers idle callback; callback returns 0 to allow suspend, -1 to veto
- [ ] USB 3.x Link Power Management: configure U1 (fast exit ~2 us) and U2 (slow exit ~2 ms) link states in xHCI `PORTPMSC` register; hardware-autonomous transitions when link is idle; saves 0.5--1 W per idle USB device
- [ ] `usb_lpm_enable(dev, u1_timeout, u2_timeout)` -- set per-device U1/U2 inactivity timeouts; 0 = disabled
- [ ] Power plan setting: `HKLM\SYSTEM\PowerPlans\{GUID}\USBSelectiveSuspend` (REG_DWORD, default 1)
- [ ] NVMe APST allows the drive firmware to autonomously transition between power states (PS0--PS4) based on idle time thresholds configured by the host; reduces NVMe idle power from 3--5 W to < 0.01 W
- [ ] `nvme_configure_apst(dev)` -- read `Identify Controller` for supported power states; build APST table mapping idle time -> target PS; send `Set Features (0x0C)` command to program the table
- [ ] Default APST policy: PS0 active; after 100 ms idle -> PS1 (if exit latency < 5 ms); after 500 ms -> PS3 (if exit latency < 100 ms)
- [ ] `nvme_apst_disable(dev)` -- clear APST table before S3/S4 entry (firmware must not auto-transition during suspend sequence)
- [ ] AHCI ALPM: SATA PHY link enters Partial (10 us exit latency) or Slumber (10 ms exit) when no commands pending; saves 0.5--2 W per idle SATA port
- [ ] `ahci_alpm_set_policy(port, policy)` -- write `AHCI_PxSCTL.IPM` bits: `0`=no restrictions, `1`=Partial disabled, `2`=Slumber disabled, `3`=both disabled; `AHCI_PxCMD.ALPE`=1 to enable ALPM
- [ ] HIPM (Host-Initiated) + DIPM (Device-Initiated) power management both supported; DIPM enabled via `AHCI_PxSCTL.DET` device detection control
- [ ] Power plan mapping: `PowerSaver`=Slumber allowed, `Balanced`=Partial only, `Performance`=ALPM disabled
- [ ] Commit: `"kernel/pm: runtime device idle -- pm_runtime_get/put, PoFx F-states, USB suspend, NVMe APST, SATA ALPM"`

**Test checkpoint:** `pm_runtime_get(dev)` increments ref count; `pm_runtime_put(dev)` decrements. Autosuspend fires after timeout when ref=0. `pm_fx_activate_component(dev, 0)` transitions to F0. `pm_fx_idle_component(dev, 0)` starts Fx timer. `nvme_configure_apst()` programs APST table. `ahci_alpm_set_policy()` writes AHCI registers. Test on: QEMU TCG.

---

## 13. Power Request Tracking & Wake Source Management

Track which applications and drivers are preventing system idle sleep, and provide a complete wake source registry for diagnostics.
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
- [ ] `pm_wake_source_t` -- registered wake sources:
  ```c
  typedef struct {
      const char *name;       /* "Power Button", "RTC Alarm", "USB xHCI", "NIC WoL" */
      uint8_t    type;        /* PM_WAKE_FIXED_EVENT, PM_WAKE_GPE, PM_WAKE_PCI_PME, PM_WAKE_TIMER */
      bool       enabled;     /* can this source wake from S3/S4? */
      uint64_t   last_wake;   /* timestamp of last wake event */
  } pm_wake_source_t;
  ```
- [ ] `pm_register_wake_source(name, type)` -- called by EC driver (§5), PCI D-state code (§8), RTC alarm (§3)
- [ ] `pm_record_wake_event(source)` -- called on S3/S4 resume path; records which source triggered the wake
- [ ] Wake timers: `pm_set_wake_timer(seconds, callback)` -- programs CMOS RTC or HPET comparator; adds to wake timer list
- [ ] `powercfg /requests` -- print all active power requests (pid, flags, reason string)
- [ ] `powercfg /lastwake` -- print the wake source that triggered the most recent S3/S4 resume
- [ ] `powercfg /waketimers` -- print all active wake timers with their expiry times
- [ ] `powercfg /devicequery wake_armed` -- list all devices enabled to wake the system
- [ ] `powercfg /energy` -- 60-second trace of power usage; report idle violations, devices not entering low-power states, excessive timer resolution requests
- [ ] Commit: `"kernel/pm: power request tracking, wake source registry, powercfg diagnostics"`

**Test checkpoint:** `pm_create_power_request(ES_SYSTEM_REQUIRED)` prevents `pm_check_idle_allowed()`. `pm_release_power_request()` re-enables idle. `pm_register_wake_source("test")` succeeds. `powercfg /requests` shows active request. `powercfg /lastwake` reports last source. Test on: QEMU TCG.

---

## 14. ACPI Thermal Zone Management

Implement the OSPM thermal policy engine per ACPI spec chapter 11. This is the kernel-core framework that processes thermal zones from ACPI namespace (`_TZ`), evaluates temperature (`_TMP`), and enforces passive cooling (CPU throttle) and active cooling (fan control) based on trip points.

> [!IMPORTANT]
> **Scope boundary with D04T04§6:** TODO-26 §6 covers per-core MSR-based thermal monitoring (`IA32_THERM_STATUS`, LAPIC Thermal LVT). This section covers the ACPI thermal zone framework that sits above it -- processing `_TMP`/`_CRT`/`_HOT`/`_PSV`/`_ACx` objects and coordinating cooling responses. Both are needed for full parity.
> → XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md §4` -- MSR-based per-core thermal; this section adds ACPI thermal zones
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
- [ ] Poll each thermal zone every `_TSP` milliseconds via DPC timer (→ XREF: `TODO-07-irql-model-dpcs.md §3`)
- [ ] On each poll, evaluate `_TMP`; compare against trip points:
  - `temp >= _CRT` -> `system_shutdown(SHUTDOWN_THERMAL_EMERGENCY)` -- immediate power-off
  - `temp >= _HOT` -> `pm_enter_s4()` -- emergency hibernate to preserve state
  - `temp >= _PSV` -> initiate passive cooling (§14)
  - `temp >= _ACx` -> initiate active cooling at level x (§14)
- [ ] Thermal event notification: ACPI `Notify(thermal_zone, 0x80)` (temperature change) triggers immediate re-evaluation instead of waiting for next poll
- [ ] Passive cooling reduces CPU power dissipation by throttling frequency/performance:
  - `delta_perf = _TC1 * (temp - prev_temp) + _TC2 * (temp - _PSV)` -- ACPI spec passive cooling equation
  - Apply performance reduction via `cpufreq_set_max_pstate()` (see §15)
- [ ] `_PSL` (Passive List) -- list of processor objects to throttle; if absent, throttle all CPUs
- [ ] When temperature drops below `_PSV`: gradually restore full performance over 3 polling intervals
- [ ] Boot log: `[THERMAL] Zone %s: passive cooling active, target %u C, current %u C`
- [ ] `_ALx` (Active List) -- list of `FAN` device objects to activate at trip level x
- [ ] `FAN._ON()` / `FAN._OFF()` ACPI methods -- turn fan on/off
- [ ] Multi-level fan: `_AC0` is the highest threshold (all fans max); `_AC9` is lowest (gentle fan)
- [ ] Fan hysteresis: do not turn off fan until temperature drops 3 C below the `_ACx` threshold (prevent rapid on/off cycling)
- [ ] If fan device supports `_FPS` (Fan Performance States): set fan speed as a percentage instead of on/off
- [ ] Write `HKLM\HARDWARE\Thermal\Zone<N>\Temperature`, `CriticalTemp`, `PassiveTemp` on each poll
- [ ] System tray: temperature indicator when any zone is above `_PSV`
- [ ] `powercfg /energy` includes thermal zone status in energy report
- [ ] Boot log: `[THERMAL] Zone %s: _CRT=%u C, _HOT=%u C, _PSV=%u C, _AC0=%u C, polling=%u ms`
- [ ] Commit: `"kernel/pm: ACPI thermal zones -- _TMP/_CRT/_HOT/_PSV/_ACx, passive/active cooling"`

**Test checkpoint:** `thermal_zone_get_crt()` returns value > 0 (or skip if no zone). `_TMP` reads plausible temperature (20--100 C). Passive cooling triggers `cpufreq_set_max_pstate()`. Fan hysteresis prevents rapid on/off. Registry `Zone<N>\Temperature` updated. Test on: QEMU TCG (with ACPI thermal zone in DSDT).

---

## 15. CPU Frequency Scaling Governor Framework

Kernel-core governor framework that sits between the scheduler's load metrics and the ACPI/HWP/CPPC frequency control hardware. This is the kernel-core responsibility -- the ACPI `_PSS` parsing lives in D04T04§4; the scheduler's `cpufreq_register_driver()` vtable lives in D03T05§9.

> [!IMPORTANT]
> **Scope boundary:** D04T04§4 parses `_PSS` P-state tables via ACPICA and provides the hardware driver. D03T05§9 provides the scheduler hook. This section owns the policy layer -- governor algorithms, HWP/CPPC native support, and the connection between them.
> → XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md §6` -- `_PSS` P-state hardware driver
> → XREF: `03-memory-concurrency/TODO-06-scheduler-enhancement.md §9` -- `cpufreq_register_driver()` and load metrics
> → XREF: `01-boot-platform/TODO-11-interrupt-timer-arch.md §2` -- LAPIC timer recalibration after frequency change
> → XREF: `02-kernel-core/TODO-09-x86-64-architecture.md §10` -- Intel hybrid P/E-core topology data
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
- [ ] Detect HWP: `CPUID.06H:EAX[7]` (HWP base), `CPUID.06H:EAX[8]` (HWP notification), `CPUID.06H:EAX[9]` (HWP activity window)
- [ ] Enable HWP: `wrmsr(IA32_PM_ENABLE, 1)` per CPU
- [ ] `IA32_HWP_CAPABILITIES (0x771)` -- read guaranteed/min/max/most_efficient frequencies
- [ ] `IA32_HWP_REQUEST (0x774)` -- write min/max/desired/energy_perf_preference per CPU:
  - `performance` governor: min=max=highest_perf, EPP=0 (max performance)
  - `powersave` governor: min=lowest, max=guaranteed, EPP=255 (max efficiency)
  - `balanced` governor: min=lowest, max=highest, EPP=128 (balanced)
- [ ] `IA32_HWP_STATUS (0x777)` -- read current performance state for telemetry
- [ ] Boot log: `[CPUFREQ] HWP enabled: %u--%u MHz, guaranteed=%u MHz`
- [ ] Timer recalibration on frequency transition: when a governor/HWP P-state change can alter the APIC bus or TSC rate, refresh `lapic_timer_calibrate()` + `mono_clock_crosscheck_tsc()` and rebase the tick epoch (`mono_clock_tick_rebase`) -- closes the recalibrate-hook deferral from `01-boot-platform/TODO-11` §7
- [ ] Detect CPPC: `CPUID.80000008H:EBX[25]` (CPPC)
- [ ] Evaluate ACPI `_CPC` (Continuous Performance Control) package per CPU
- [ ] `PERF_CTL` MSR / ACPI `SystemIO` writes for requested performance level
- [ ] Same governor integration as HWP: min/max/desired mapped to CPPC registers
- [ ] AMD EPP (Energy Performance Preference): `CPPC_REQ.energy_perf_pref` (0x00=performance, 0xFF=efficiency); mapped from governor: `performance`=0x00, `powersave`=0xFF, `balanced`=0x80; written per-CPU via `_CPC` SystemIO or `MSR_AMD_CPPC_REQ`
- [ ] Boot log: `[CPUFREQ] CPPC enabled: %u--%u nominal performance units, EPP=%u`
- [ ] Per-CPU frequency transition histogram: `g_cpufreq_stats[cpu].transitions_total`, `time_in_state[pstate]`
- [ ] `/sys/cpufreq` VFS file: columns `CPU  Governor  CurFreq  MinFreq  MaxFreq  Transitions`
- [ ] Registry: `HKLM\HARDWARE\CPU\Frequency\Core<N>` updated every 5 s
- [ ] Intel RAPL (Running Average Power Limit): read `MSR_PKG_ENERGY_STATUS` (0x611), `MSR_DRAM_ENERGY_STATUS` (0x619) for package/DRAM energy counters; units from `MSR_RAPL_POWER_UNIT` (0x606)
- [ ] `rapl_read_energy_uj(domain)` -- returns cumulative energy in microjoules for Package, Core, Uncore, DRAM domains
- [ ] `rapl_set_power_limit(domain, watts, time_window_us)` -- writes `MSR_PKG_POWER_LIMIT` (0x610); clamps CPU power dissipation to thermal budget; gated on `pkg_pwr_lim_lock` bit (firmware may lock)
- [ ] `powercfg /energy` uses RAPL counters for per-subsystem power attribution
- [ ] AMD equivalent: `MSR_PWR_UNIT` (0xC0010299) / `MSR_CORE_ENERGY_STAT` (0xC001029A); same API, different MSRs
- [ ] Commit: `"kernel/pm: cpufreq governor framework -- HWP, CPPC, performance/powersave/balanced/schedutil"`

**Test checkpoint:** `cpufreq_set_governor("performance")` accepted. `cpufreq_get_current_freq(0)` returns non-zero. HWP detect via CPUID.06H:EAX[7]. AMD EPP written per-CPU. `rapl_read_energy_uj(PKG)` returns non-zero (or skip if no RAPL). Test on: QEMU TCG + WHPX.

---

## 16. CPU Idle Governor Framework

Kernel-core idle governor that selects the optimal C-state based on predicted idle duration and latency constraints. The C-state hardware interface (`_CST`, `MWAIT`) is owned by D04T04§7; this section owns the idle prediction and selection policy.

> [!IMPORTANT]
> **Scope boundary:** D04T04§7 parses `_CST` and provides `cpuidle_enter(cpu, cstate)`. This section owns the governor that decides *which* C-state to enter. §2 of this TODO provides the basic S1/HLT idle path; this section replaces it with a full governor.
> → XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md §8` -- `_CST` parsing and `cpuidle_enter()` hardware interface
- [ ] `cpuidle_governor_t`:
  ```c
  typedef struct {
      const char *name;       /* "menu", "ladder", "teo" */
      int (*select)(uint32_t cpu, uint64_t *predicted_idle_ns);
      void (*reflect)(uint32_t cpu, int selected_state, uint64_t actual_idle_ns);
  } cpuidle_governor_t;
  ```
- [ ] `select()` returns the C-state index to enter; `reflect()` is called after wakeup to improve future predictions
- [ ] Prediction based on: expected next timer interrupt, recent idle history (exponential weighted moving average), interrupt rate
- [ ] `predicted_idle_ns` computed; select deepest C-state whose `exit_latency_ns <= predicted_idle_ns / 2` (latency must be < 50% of predicted idle to be worthwhile)
- [ ] Skip C-states that exceed the power plan's `idle_latency_budget_ns` (§18)
- [ ] Correction factor: if the CPU woke earlier than predicted on the last 4 invocations, bias toward shallower C-states
- [ ] Start at C1; promote to deeper C-state if idle duration exceeded promotion threshold on N consecutive entries; demote if actual idle was less than demotion threshold
- [ ] Useful for latency-sensitive workloads where menu governor oscillates
- [ ] Per-CPU, per-C-state: `entries`, `total_residency_ns`, `rejected` (selected but actual idle was too short)
- [ ] `/sys/cpuidle` VFS file: columns `CPU  C0%  C1%  C2%  C3%  Governor  AvgIdleUs`
- [ ] Boot log: `[CPUIDLE] Governor: %s, max C-state: C%u, latency budget: %u us`
- [ ] Commit: `"kernel/pm: CPU idle governor -- menu/ladder algorithms, C-state selection, idle stats"`

**Test checkpoint:** `cpuidle_get_governor()` returns non-NULL. `select()` returns valid C-state index (0 <= idx <= max_cstate). `reflect()` updates prediction. Ladder governor promotes after N consecutive deep idles. Test on: QEMU TCG.

---

## 17. Driver Power Query & Veto (IRP_MN_QUERY_POWER)

Before changing system or device power state, query all affected drivers and allow them to veto the transition. Windows uses `IRP_MN_QUERY_POWER`; Linux uses `prepare()` callbacks. Without this, a driver with in-flight DMA or unsaved state can lose data during sleep.
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
- [ ] If the user cancels a sleep request (e.g. clicks "Cancel" on shutdown dialog) after `pm_query_power_state()` succeeded but before `pm_notify_sleep()`:
  - Call `cb->on_query_cancel(ctx)` for all drivers that were queried -- lets drivers release resources they reserved for the transition
- [ ] Commit: `"kernel/pm: driver power query/veto -- pm_query_power_state, on_query callback"`

**Test checkpoint:** Register test driver with `on_query` returning `STATUS_DEVICE_BUSY`; `pm_query_power_state(S3)` returns error. Register permissive driver; query succeeds. Cancel after query calls `on_query_cancel`. Test on: QEMU TCG.

---

## 18. Power Plan UI & `powercfg`
- [ ] Stored under `HKLM\SYSTEM\PowerPlans\{GUID}\`:
  - `Name` (REG_SZ): `"Balanced"`, `"Power Saver"`, `"High Performance"`
  - `SleepTimeout` (REG_DWORD): seconds to S3 on idle (0=never)
  - `HibernateTimeout` (REG_DWORD): seconds to S4 after S3 (0=never)
  - `DisplayOffTimeout` (REG_DWORD): seconds to blank display
  - `PowerButtonAction` (REG_DWORD): same codes as §7
  - `LidCloseAction` (REG_DWORD): same codes as §7
  - `CpuFreqGovernor` (REG_SZ): `"performance"`, `"balanced"`, `"powersave"`, `"schedutil"`
  - `IdleLatencyBudgetUs` (REG_DWORD): max C-state exit latency in microseconds
  - `MaxProcessorState` (REG_DWORD): 0--100% cap on CPU frequency (§15)
  - `MinProcessorState` (REG_DWORD): 0--100% floor on CPU frequency
  - `CoolingPolicy` (REG_DWORD): 0=active preferred (fan first), 1=passive preferred (throttle first)
- [ ] `pm_apply_plan(guid)` -- reads plan Registry values; sets idle timers in the scheduler's DPC timer (→ XREF: `TODO-07-irql-model-dpcs.md §3`); configures governor (§15), idle budget (§16), thermal policy (§14)
- [ ] Default plan GUIDs match Windows 11's well-known GUIDs:
  - Balanced: `{381b4222-f694-41f0-9685-ff5bb260df2e}`
  - Power Saver: `{a1841308-3541-4fab-bc81-f71556f20b4a}`
  - High Performance: `{8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c}`
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
- [ ] `sysdm.cpl` Power tab (or dedicated `powercpl.cpl`):
  - Plan selector radio buttons
  - "Change plan settings" expands to sliders: Screen off / Sleep / Hibernate timeouts
  - "Advanced settings" tree: power button action, lid action, low-battery action, CPU min/max, cooling policy, USB selective suspend
- [ ] Changes written to Registry via `RegSetValueEx`; `pm_apply_plan()` called immediately after save
- [ ] `pm_energy_saver_t` -- replaces Battery Saver with workload-adaptive mode (Win11 24H2+):
  - Works on AC power too (not just battery) -- triggered by `EnergySaverEnabled` Registry key or charge_pct <= `EnergySaverThreshold` (default 20%)
  - Reduces visual effects (compositor effects budget), pauses background sync tasks, caps CPU to `MaxProcessorState=50%`
- [ ] Adaptive Energy Saver: `pm_energy_saver_auto()` -- monitors 60 s rolling CPU utilization; auto-enables for light tasks (< 15% avg), auto-disables for heavy workloads (> 50% avg); Registry `AdaptiveEnergySaver` (default 1)
- [ ] `powercfg /energysaver on|off|auto` -- CLI control
- [ ] System tray: leaf icon when Energy Saver active; tooltip shows reason ("Battery below 20%" or "Adaptive -- light workload")
- [ ] Commit: `"kernel/pm: powercfg shell command, power plan Registry schema, Energy Saver, Power Options UI"`

**Test checkpoint:** `pm_apply_plan(GUID_BALANCED)` reads Registry values. `powercfg /list` shows 3 default plans. `powercfg /setactive` switches governor. Energy Saver auto-enables at low charge. `powercfg /availablesleepstates` lists supported S-states. Test on: QEMU TCG.

---

## 19. Energy-Aware Scheduling Integration

On heterogeneous CPU topologies (Intel Alder Lake+ P/E-cores, future ARM big.LITTLE), the scheduler should place tasks on the most energy-efficient core that can meet the task's performance requirements. This is Impossible OS's competitive edge -- integrating power and scheduling into a single decision loop rather than layering them separately.

> [!IMPORTANT]
> **Scope boundary:** D02T19§9 detects Intel hybrid P/E-core topology and Intel Thread Director (ITD) / Hardware Feedback Interface (HFI). D03T05§9 provides the scheduler's load metrics. This section integrates those signals with the CPU frequency governor (§15) to make energy-aware placement decisions.
> → XREF: `02-kernel-core/TODO-09-x86-64-architecture.md §10` -- P/E-core detection, HFI capability data
> → XREF: `03-memory-concurrency/TODO-06-scheduler-enhancement.md §9` -- scheduler load tracking, PELT utilization
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
- [ ] For each runnable task, the scheduler's `select_cpu()` considers:
  - `em_energy_cost(candidate_cpu, task_util)` -- prefer lower cost
  - Wake latency: do not migrate to a CPU in deep C-state if the migration cost exceeds the energy savings
  - Task priority: real-time tasks always go to P-cores; background/idle tasks prefer E-cores
- [ ] Intel Thread Director hints: if HFI `perf_capability[cpu]` < task requirement, avoid that CPU
- [ ] Fallback on homogeneous systems: no energy-aware placement; revert to standard load balancing
- [ ] On Intel hybrid CPUs, P-cores and E-cores have independent frequency domains
- [ ] `em_cluster_set_freq(cluster_id, target_freq)` -- coordinate frequency for all CPUs in the cluster
- [ ] When all tasks on a cluster are low-utilization: scale down the cluster frequency; when any task is high-utilization: scale up
- [ ] Commit: `"kernel/pm: energy-aware scheduling -- energy model, task placement, cluster frequency"`

**Test checkpoint:** `em_energy_cost(e_core, 50)` < `em_energy_cost(p_core, 50)` on hybrid CPU. Low-priority task placed on E-core via `select_cpu()`. `em_cluster_set_freq()` accepted. Homogeneous systems return 0 cost (skip). Test on: QEMU TCG (homogeneous).

---

## 20. Power Syscalls Wired to SSDT

Register all power management NtXxx entry points in the SSDT so user-mode code can invoke them via `syscall`. See TODO-12-native-api-ssdt.md §5 (SSDT layout) and §22 (power syscalls).

- [ ] `NtShutdownSystem(Action)` -> SSDT 0x00D7: call `pm_shutdown()` / `pm_reboot()` based on action; requires `SeShutdownPrivilege`
- [ ] `NtSetSystemPowerState(SystemAction, LightestSystemState, Flags)` -> SSDT 0x0140: route through ACPI S-state transition (§2)
- [ ] `NtInitiatePowerAction(SystemAction, LightestSystemState, Flags, Asynchronous)` -> SSDT 0x0141: async power action initiation
- [ ] `NtPowerInformation(InformationLevel, InputBuffer, InputLen, OutputBuffer, OutputLen)` -> SSDT 0x0142: return battery state, processor info, S-state capabilities, thermal zone data from §6, §14, §15
- [ ] `NtGetDevicePowerState(Device, State)` -> SSDT 0x0143: query PCI device D-state (§8) or runtime PM state (§12)
- [ ] `NtSetThreadExecutionState(NewFlags, PreviousFlags)` -> SSDT 0x0144: `ES_SYSTEM_REQUIRED` / `ES_DISPLAY_REQUIRED` / `ES_AWAYMODE_REQUIRED` prevents idle sleep; creates power request (§13)
- [ ] `NtRequestWakeupLatency(Latency)` -> SSDT 0x0145: hint to power manager about acceptable wake latency; feeds idle governor (§16)
- [ ] All functions return `NTSTATUS`; use codes from `include/kernel/nt/ntstatus.h` (TODO-12 §1)
- [ ] Commit: `"kernel/pm: wire power syscalls to SSDT (0x00D7, 0x0140--0x0145)"`

**Test checkpoint:** `NtShutdownSystem(ShutdownReboot)` triggers ACPI reset. `NtPowerInformation(SystemPowerCapabilities)` returns valid S-state mask. `NtSetThreadExecutionState(ES_SYSTEM_REQUIRED)` prevents idle sleep during long operation. `NtPowerInformation(ProcessorPowerInformation)` returns current CPU frequency per core.

---

## 21. Linux `/sys/power` Suspend Variant Parity

Linux exposes system suspend through `/sys/power/state` plus `/sys/power/mem_sleep` (`s2idle`, `shallow`, `deep`) and hibernation mode through `/sys/power/disk`. Windows 11 documents S0 low-power idle (Modern Standby) separately from ACPI S1 through S3. This section is the **policy and diagnostics parity** layer so developers know how Impossible OS maps those models onto ACPI and our §2/§3/§10 paths.

- [ ] Author `docs/kernel/pm-linux-sysfs-parity.md` (or equivalent under `todo/02-kernel-core/` if docs tree is not ready): table columns `Linux interface`, `Typical ACPI mapping`, `Impossible OS owner (TODO-26 §N)`, `Notes` (include default `mem_sleep_default=s2idle` vs `deep` firmware behavior)
- [ ] Document `freeze` vs `mem`+`s2idle` equivalence for suspend-to-idle per upstream sleep-states.rst semantics
- [ ] Document `disk` file modes (`platform`, `shutdown`, `reboot`, `suspend`, `test_resume`) and map `platform` to ACPI S4 vs power-off fallback
- [ ] Add `pm_sleep_variant_t` enum in `include/kernel/pm/sleep.h` with `PM_SLEEP_S2IDLE`, `PM_SLEEP_STANDBY`, `PM_SLEEP_STR`, `PM_SLEEP_HIBER_PLATFORM`, `PM_SLEEP_HIBER_SHUTDOWN` mirroring the Linux strings we support first
- [ ] `pm_get_sleep_variants(char *buf, size_t cap)`: returns a comma-separated list analogous to reading `mem_sleep` (used by future `powercfg /availablesleepstates` and shell introspection)
- [ ] Wire klog at INFO once after `acpi_power_init()`: log FADT `LOW_POWER_S0_IDLE_CAPABLE` when present and how it affects default sleep choice vs S3 (`acpi_s0ix_supported()` from §10 when implemented)
- [ ] Cross-link from §18 `powercfg /availablesleepstates` checklist to this doc so CLI output stays aligned with Linux vocabulary where useful
- [ ] Commit: `"docs/pm: Linux sysfs sleep variant parity table + pm_sleep_variant_t"`

**Test checkpoint:** Doc builds or renders in-tree; `pm_get_sleep_variants()` returns a non-empty string on QEMU (at least `PM_SLEEP_STR` or `PM_SLEEP_S2IDLE` token when gated by `acpi_sleep_supported()`). klog line appears once per boot when `debug=1`. QEMU WHPX, QEMU TCG, VirtualBox.

---

## OS Comparison

| ⭐   | Feature                     | 🪟 Win11       | 🐧 Linux        | 🚀 Impossible OS |
| --- | --------------------------- | ------------- | -------------- | --------------- |
| 💎   | S5 ACPI shutdown            | ✅ Full        | ✅ Full         | ✅ Done §1       |
| 💎   | S1 idle / HLT               | ✅ Full        | ✅ cpuidle      | ⬜ §2            |
| 💎   | S3 suspend RAM              | ✅ Full        | ✅ sleep        | ⬜ §3            |
| 💎   | S4 hibernate disk           | ✅ Full        | ✅ swsusp       | ⬜ §4            |
| 💎   | Fast startup hiberboot      | ✅ Default     | ❌ None         | ⬜ §11           |
| 💎   | ACPI EC driver              | ✅ Full        | ✅ acpi_ec      | ⬜ §5            |
| 💎   | Battery `_BIX` / `_BST`     | ✅ Full        | ✅ upower       | ⬜ §6            |
| 💎   | Power lid button events     | ✅ Full        | ✅ logind       | ⬜ §7            |
| 💎   | PCI D-states D0--D3cold     | ✅ Full        | ✅ PCI PM       | ⬜ §8            |
| 💎   | Driver sleep wake callbacks | ✅ WDM         | ✅ pm_ops       | ⬜ §9            |
| 💎   | Driver query veto power     | ✅ QUERY_POWER | ✅ prepare      | ⬜ §17           |
| 💎   | Runtime idle PoFx RPM       | ✅ PoFx        | ✅ runtime_pm   | ⬜ §12           |
| 💎   | Power requests tracking     | ✅ powercfg    | ⚠️ wake_lock   | ⬜ §13           |
| 💎   | Wake source lastwake        | ✅ powercfg    | ⚠️ dmesg       | ⬜ §13           |
| 💎   | ACPI thermal zones          | ✅ ACPI.sys    | ✅ thermal      | ⬜ §14           |
| 💎   | Passive active cooling      | ✅ Full        | ✅ step_wise    | ⬜ §14           |
| 💎   | CPU DVFS cpufreq            | ✅ PPM HWP     | ✅ cpufreq      | ⬜ §15           |
| 💎   | CPU idle C-states           | ✅ PPM         | ✅ menu teo     | ⬜ §16           |
| 💎   | Connected standby S0ix      | ✅ Modern      | ⚠️ Partial     | ⬜ §10           |
| 💎   | mem_sleep s2idle deep       | ✅ S0 idle     | ✅ sysfs        | ⬜ §21           |
| 💎   | powercfg CLI surface        | ✅ 50 cmds     | ⚠️ systemctl   | ⬜ §18           |
| 💎   | Power Options GUI           | ✅ powercpl    | ⚠️ GNOME basic | ⬜ §18           |
| ⭐   | Energy aware scheduling     | ⚠️ HW ITD     | ✅ EAS ARM      | ⬜ §19           |
| ⭐   | Battery wear tray hint      | ❌ Settings    | ❌ CLI only     | ⬜ §6            |
| ⭐   | batteryreport plain text    | ✅ HTML        | ❌ None         | ⬜ §18           |
| ⭐   | energy audit trace          | ✅ Full        | ❌ None         | ⬜ §13           |
| ⭐   | sleepstudy DRIPS report     | ✅ Full        | ❌ None         | ⬜ §18           |
| 💎   | PoFx F-states components    | ✅ Per Fx      | ❌ Device only  | ⬜ §12           |
| 💎   | Directed PoFx DRIPS         | ✅ PoFx v3     | ❌ None         | ⬜ §10           |
| 💎   | USB suspend U1 U2 LPM       | ✅ Full        | ✅ autosuspend  | ⬜ §12           |
| 💎   | NVMe APST idle states       | ✅ On          | ✅ sysfs        | ⬜ §12           |
| 💎   | SATA ALPM link power        | ✅ HIPM        | ✅ sysfs        | ⬜ §12           |
| 💎   | NIC ARP NS offload S0ix     | ✅ NDIS        | ⚠️ Firmware    | ⬜ §10           |
| 💎   | Smart charge 80 percent     | ✅ OEM         | ⚠️ TLP         | ⬜ §6            |
| 💎   | RAPL power cap sysfs        | ✅ Internal    | ✅ powercap     | ⬜ §15           |
| 💎   | AMD P-State EPP             | ✅ Driver      | ✅ amd_pstate   | ⬜ §15           |
| 💎   | Energy Saver adaptive       | ✅ Win11       | ⚠️ profiles    | ⬜ §18           |
| ⭐   | Human presence HPD wake     | ✅ Platform    | ❌ None         | ⬜ §7            |

After §1 through §21, Impossible OS reaches parity for laptop-grade power on real hardware: S-states, D-states, runtime idle including component F-states, USB LPM, NVMe APST, SATA ALPM, thermal, DVFS with HWP CPPC EPP RAPL, C-states, EC, battery with smart charging, power lid HPD events, driver callbacks with query veto, DFx for Modern Standby DRIPS, fast startup, Energy Saver, NIC offloads, power request tracking, and an explicit Linux `mem_sleep` vocabulary map for suspend diagnostics. Linux splits this across drivers, logind, upower, cpufreq, and cpufreq sysfs; Windows is the most integrated reference. Impossible OS adds a software energy model on hybrid CPUs, HPD wake and lock policies Linux lacks, adaptive Energy Saver, and convenient battery wear plus plain-text `powercfg /batteryreport`.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_power()` (see `src/kernel/test/test_runner.c` and `include/kernel/test/test.h`; same pattern as `TODO-11-peb-teb-user-abi.md` Unit Tests).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_power.c` with:
  - Linux parity: `pm_get_sleep_variants()` returns a comma list including at least one token matching `acpi_sleep_supported()` for S1/S3 when firmware advertises those states (or skip when none)
  - ACPI sleep type lookup: `acpi_get_slp_typa(5)` returns valid SLP_TYPa/b values
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
  - PoFx F-state: `pm_fx_activate_component(dev, 0)` transitions to F0; `pm_fx_idle_component(dev, 0)` starts timer
  - USB selective suspend: `usb_idle_register(dev, cb, 5000)` accepted; port enters U3 after timeout
  - NVMe APST: `nvme_configure_apst(dev)` programs APST table (or skip if no NVMe)
  - SATA ALPM: `ahci_alpm_set_policy(port, ALPM_PARTIAL)` writes AHCI PxSCTL bits
  - DFx: `pm_dfx_power_down(stack)` transitions devices to D3; DRIPS % computed
  - Smart charging: `bat_set_charge_limit(80)` writes EC register (or skip if no EC)
  - RAPL: `rapl_read_energy_uj(PKG)` returns non-zero (or skip if no RAPL MSR)
  - AMD EPP: CPPC `energy_perf_pref` written for each governor mode (or skip if no AMD CPPC)
  - Energy Saver: `pm_energy_saver_auto()` enables below threshold; disables above
  - HPD: `hpd_register_sensor()` accepted (or skip if no HPD device)
  - Power query/veto: `pm_query_power_state(S3)` vetoed by test driver returning `STATUS_DEVICE_BUSY`
- [ ] Register in `test_runner_init()`: `test_register_power()`
- [ ] Commit: `"test: add power management test suite"`

**Test checkpoint:** `bash scripts/test.sh SUITE=boot` reports every `test_power_*` (and existing `test_acpi_power_*`) case PASS once `test_power.c` lands; until then, `test_acpi_power.c` suite green; `tail -1 build/build.log` is `=== BUILD OK ===`. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

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

**Test checkpoint:** Every Verification bullet above passes where hardware allows; `bash scripts/test.sh SUITE=boot` green for ACPI power tests; `tail -1 build/build.log` is `=== BUILD OK ===`. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

**Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot); `test_acpi_power.c` uses `TEST_CAT_BOOT`. When `test_power.c` lands, add `TEST_CAT_POWER` and `scripts\debug\kernel\run-power-tests.bat` (see Unit Tests section).

---

## History

| Date       | Action   | Summary |
| ---------- | -------- | ------- |
| 2026-04-13 | gap-analysis | 9 web searches + 2 doc fetches; Current state refreshed vs `acpi_power_init`/`acpi_enter_sleep_state`/SCI; new §21 Linux `mem_sleep` parity + OS row + Unit Tests bullet; oversized section split deferred (see report); TODO-26 Inputs back-XREF §21. |
| 2026-04-16 | validate | `→ XREF` normalized; `§1 through §20`; Unit Tests + Verification **Test checkpoint**; wire note uses `test_runner.c`; D02T19 hybrid XREF §8→§9; double blank before Unit Tests removed; §1 checkpoint adds VirtualBox + bare metal; §2 prose `--` fix; History added. |
| 2026-04-17 | validate | Callout prose `--` fixes (CAUTION, scope, legend); Impl Order row 1 Depends On `(none)`; Inputs mark §21 paths planned; Outcome list punctuation; `---` before `## Unit Tests`; continuation-line rg hits are fenced code only; XREF targets spot-checked; `run-boot-tests.bat` present; parity rows include §21 mem_sleep. |
