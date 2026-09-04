---
schema_version: 1
id: acpi-power-management
domain: 04-drivers-hardware
status: active
title: "TODO-03 -- ACPI Full Subsystem & Power Management"
---

# TODO-03 -- ACPI Full Subsystem & Power Management

> **Goal:** Evolve from the working minimal ACPI stub (RSDP/XSDT/MADT/FADT/shutdown-reboot) into a complete OSPM system: ACPICA AML interpreter, S3 suspend/resume, battery status, CPU DVFS, power button SCI, per-core thermal monitoring, C-state idle, S4 hibernate, a clean shutdown orchestrator, and Registry-backed power profiles.

> [!IMPORTANT]
> **Already done:** RSDP location, MADT parsing (LAPIC/IOAPIC/ISO entries), FADT field reads, `acpi_shutdown()` / `acpi_reboot()` via PM1a/RESET_REG. This TODO builds on top of those foundations. Every section that evaluates `_BST`, `_PSS`, `_CST`, or `_TZ` methods depends on §1 (ACPICA) being complete first.

## Inputs

- -> XREF: `02-kernel-core/TODO-26-power-management.md` -- pre-ACPICA S1/S3/S4, EC, battery, power button, and thermal-zone checklist path until §1 here completes; merge plan in TODO-16 scope box
- -> XREF: `02-kernel-core/TODO-26-power-management.md` section 21 Linux sysfs mem_sleep parity and pm_sleep_variant_t enum; complements this file when both land; no AML interpreter prerequisite
- [`src/kernel/acpi.c`](../../src/kernel/acpi.c), [`include/kernel/acpi.h`](../../include/kernel/acpi.h)
- ACPICA source (Intel ACPI CA OR BSD-3-Clause OR GPL-2.0; BSD arm taken -- NOT Apache-2.0, corrected 2026-08-17): `src/kernel/acpica/` -- imported 2026-08-17
- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c) -- hibernate detection in §10 requires a bootloader-side hiberfil.sys check
- → XREF: `04-drivers-hardware/TODO-08-core-driver-enhancements.md §4` -- `hpet_read_ns()` used by S3 resume timing check in §9
- → XREF: `04-drivers-hardware/TODO-02-apic-interrupt-routing.md §1` -- x2APIC re-init on S3 resume path in §9; Thermal LVT in §4
- → XREF: `04-drivers-hardware/TODO-02-apic-interrupt-routing.md §3` -- `apic_alloc_msi_vector()` reused for SCI vector allocation in §3
- → XREF: `03-memory-concurrency/TODO-06-scheduler-enhancement.md §9` -- `cpufreq_register_driver()` callback called from §6 DVFS after `_PSS` is parsed
- → XREF: `05-storage-filesystems` domain -- VFS `cache_flush()` and unmount called in §2 shutdown orchestrator; hibernation image written to IXFS in §10
- Spec refs: ACPI 6.5 spec; Intel 64 Arch SDM (IA32_THERM_STATUS, IA32_PERF_CTL, MWAIT); ACPICA Programmer's Reference

## Outcome

- ACPICA AML interpreter is integrated; `_BST`, `_BIF`, `_PSS`, `_CST`, `_TZ0`, `_PTC` can all be evaluated without hand-rolled AML parsing.
- S3 suspend writes PM1a `SLP_TYP_S3 | SLP_EN`, parks CPUs; resume re-enters long mode via a 1 MiB real-mode trampoline and restores full kernel state.
- System tray battery icon shows percentage and time-remaining polled from `_BST` every 30 s.
- CPU switches P-states via `IA32_PERF_CTL` or AMD `PERF_CTL` MSR based on scheduler load; performance governor when queue non-empty, powersave on 100 ms idle.
- Power button SCI fires vector from FADT `SCI_INT`; triggers `system_shutdown()` orchestrator: WM_CLOSE → 5 s force-kill → flush → `acpi_poweroff()`.
- Per-core temperature read from `IA32_THERM_STATUS`; color-coded bars in Task Manager; emergency shutdown above 100 °C.
- Idle CPU enters deepest available C-state (`hlt` for C1, `mwait` for C2/C3); idle depth tracked in `/sys/cpuidle`.
- S4 hibernate serialises RAM to `C:\Impossible\System\hiberfil.sys`, powers off; bootloader detects and restores image on next boot.
- `system_shutdown()` is a deterministic orchestrator with per-step timeouts and Registry-configurable power profiles (Balanced, Performance, Power Saver).

## Implementation Order

| ⭐  | Order | Deliverable                                            | Depends On                                                                                  | Status |
| --- | :---: | ------------------------------------------------------ | ------------------------------------------------------------------------------------------- | :----: |
| 💎  |   1   | §1 ACPICA AML interpreter integration                  | --                                                                                          |  [ ]   |
| 💎  |   2   | §2 Clean shutdown sequence orchestrator                | §1, VFS flush, Registry                                                                     |  [ ]   |
| 💎  |   3   | §3 ACPI power button SCI                               | §1, §2 (shutdown orchestrator), IOAPIC                                                      |  [ ]   |
| 💎  |   4   | §4 Thermal monitoring (`IA32_THERM_STATUS`, Task Mgr)  | §1 (TjMax from AML), APIC Thermal LVT                                                       |  [ ]   |
| 💎  |   5   | §5 Battery status (`_BST`/`_BIF`)                      | §1                                                                                          |  [ ]   |
| 💎  |   6   | §6 CPU frequency scaling (DVFS, `_PSS` P-states)       | §1, `cpufreq_register_driver` (`03-memory-concurrency/TODO-06-scheduler-enhancement.md §9`) |  [ ]   |
| 💎  |   7   | §7 Power profiles -- Balanced/Performance/Power Saver  | §4, §6, §8                                                                                  |  [ ]   |
| 💎  |   8   | §8 ACPI C-states idle (`hlt`/`mwait`, `_CST`)          | §1, §7 (policy)                                                                             |  [ ]   |
| 💎  |   9   | §9 S3 suspend/resume -- trampoline, state save/restore | §1, §2 (flush), APIC re-init                                                                |  [ ]   |
| 💎  |  10   | §10 Hibernate (S4) -- hiberfil.sys, bootloader restore | §9 (S3 path), filesystem                                                                    |  [ ]   |

> All ten rows are 💎 parity: Windows NT and Linux both ship a complete ACPI OSPM, AML interpreter, S3/S4, DVFS, thermal, and C-state implementation. Closing these gaps is required to run Impossible OS on real laptops and power-managed hardware.

---

## 1. ACPICA AML Interpreter Integration `[Opus]`

Integrate ACPICA (Intel's open-source AML interpreter; triple-licensed Intel ACPI CA / BSD-3-Clause / GPL-2.0, BSD arm taken) as a static library. Replace the current hand-rolled table parsing stubs with ACPICA's `AcpiInitializeSubsystem()` → `AcpiLoadTables()` → `AcpiEnableSubsystem()` sequence. Provide the required OS Services Layer (`acpi_osl.c`) to connect ACPICA to Impossible OS memory, I/O, and synchronisation primitives.

**Files:** `src/kernel/acpica/` (imported), `src/kernel/acpi_osl.c` (new), `src/kernel/acpi_global_lock.{c,h}` (new), `include/freestanding/ctype.h` (new)

> [!IMPORTANT]
> ACPICA requires an OS Services Layer (`AcpiOs*` functions): `AcpiOsAllocate`/`Free`, `AcpiOsReadPort`/`WritePort`, `AcpiOsReadMemory`/`WriteMemory`, `AcpiOsInstallInterruptHandler`, `AcpiOsCreateMutex`/`DeleteMutex`/`AcquireMutex`/`ReleaseMutex`, and `AcpiOsPhysicalTableOverride`. Map these to `kmalloc`/`kfree`, `inb`/`outb`, `vmm_map_mmio`, IDT registration, and `mutex_t` respectively. Do not link ACPICA against any libc -- it must compile with `-ffreestanding`.

- [x] Import ACPICA source tree to `src/kernel/acpica/`; add to Makefile with `-DACPI_DEBUG_OUTPUT=0 -DACPI_APPLICATION=0 -ffreestanding`
  - Vendored 2026-08-17 at upstream `077d74a4`, ACPI_CA_VERSION 20260408: 164 `.c` files across dispatcher/events/executer/hardware/namespace/parser/resources/tables/utilities, plus 62 headers
  - Platform header `acimpossible.h` authored (modelled on `aczephyr.h`); the ONLY upstream edit is one `#elif defined(__IMPOSSIBLE_OS__)` branch in `acenv.h`
  - `ACPI_USE_SYSTEM_CLIBRARY` + `ACPI_USE_STANDARD_HEADERS` so ACPICA uses the kernel's SSE/AVX-tuned mem/str rather than shipping a second `memcpy`; needed a new `include/freestanding/ctype.h` shim
  - `ACPI_SINGLE_THREADED` deliberately NOT set (SMP-safe by default); `rsdump.c` and `utprint.c` excluded, see the Makefile for the measured reasons
  - Release image links 753 ACPICA symbols + 47 `AcpiOs*` and ends at 0x52C000, 2.8 MiB under the firmware floor
- [ ] Implement `src/kernel/acpi/acpi_osl.c` covering all `AcpiOs*` hooks:
  - Memory: `AcpiOsAllocate(size)` → `kmalloc`; `AcpiOsFree` → `kfree`; `AcpiOsMapMemory(phys, len)` → `vmm_map_mmio`
  - I/O ports: `AcpiOsReadPort`/`AcpiOsWritePort` → `inb/w/l`, `outb/w/l`
  - Sync: `AcpiOsCreateMutex` → `mutex_t *`; Acquire/Release → `mutex_lock`/`mutex_unlock`
  - Interrupt: `AcpiOsInstallInterruptHandler(gsi, handler)` → `ioapic_route_irq` + `idt_register_handler`
- [ ] Initialisation sequence in `acpi_init()`:
  - `AcpiInitializeSubsystem()` → `AcpiInitializeTables(NULL, 32, FALSE)` → `AcpiLoadTables()` → `AcpiEnableSubsystem(ACPI_FULL_INITIALIZATION)` → `AcpiInitializeObjects()`
- [ ] **Kernel semaphore is not SMP-safe** -- blocks ACPICA mutexes; verified at `src/kernel/sched/semaphore.c:29-52`
  - `sem_wait()` reads `s->count` and writes `s->waiter_tasks[]`, `s->num_waiters++` and `s->count--` with no lock and no atomics
  - ACPICA's default `ACPI_BINARY_SEMAPHORE` mutex model routes every mutex acquire through `AcpiOsWaitSemaphore` -> `sem_wait`/`sem_trywait`, so two CPUs can both observe a binary count of 1 and both succeed
  - That lets the AML interpreter run concurrently while ACPICA believes the mutex is held. PRE-EXISTING defect affecting every semaphore user, not introduced by the OSL, which is why it is filed rather than fixed inside this section
  - Found by the Codex adversarial review of the OSL, 2026-08-17
- [ ] **PCI config space has no CF8/CFC lock** -- verified at `src/kernel/drivers/pci.c:60-64`
  - `pci_read32()` does `outl(PCI_CONFIG_ADDR, ...)` then `inl(PCI_CONFIG_DATA)` with nothing serialising the pair; `pci_write32()` is the same shape
  - Another CPU or an interrupt can reprogram 0xCF8 between the two accesses, so the read returns another device's register or the write lands on it
  - Also: `pci_write16()` at line 85 composes a dword read-modify-write, which replays write-1-to-clear bits in neighbouring registers (a 16-bit Command write at 0x04 reads Status at 0x06 and writes the observed bits back, silently acknowledging errors). A native `pci_write8()` does not exist at all
  - Repair shape: one IRQ-saving config lock in the PCI driver held across each complete transaction, plus native-width write helpers. The OSL refuses 8-bit config writes today rather than synthesising an unsafe RMW
  - PRE-EXISTING, affects every PCI driver. Found by the Codex adversarial review of the OSL, 2026-08-17
- [ ] **ACPICA is release-flavor only** -- the dev/test image does not fit under the firmware floor, so the unit suite does not exercise ACPICA
  - The kernel loads at 0x100000 and ACPIMemoryNVS begins at exactly 0x800000 on this platform, giving the image a hard 7.0 MiB budget
  - Measured 2026-08-17 from the bootloader's own PT_LOAD rejection: `type=ACPIMemoryNVS at=0x00800000`
  - RELEASE (`KERNEL_TESTS=off`) ends at 0x52C000 with ACPICA linked, 2.8 MiB clear; the dev image compiles the full suite in (~487 KiB of test-only `.bss`) and would land 94 KiB over
  - `ACPICA` therefore follows `KERNEL_TESTS` in the Makefile; override with `make ACPICA=on|off`
  - The real fix is relocating the kernel above the firmware regions, which retires the conditional entirely
  - Until then the global-lock algorithm is split into `src/kernel/acpi_global_lock.c` so at least that stays covered in the default build
- [ ] Remove hand-rolled `acpi_find_table()`, `acpi_get_hpet_base()`, `acpi_get_mcfg()` -- replace with `AcpiGetTable("HPET", ...)`, `AcpiGetTable("MCFG", ...)`; keep the header API but back them with ACPICA
- [ ] `acpi_evaluate(path, args, result)` wrapper around `AcpiEvaluateObject` for use by §5–§10
  - Also unblocks EC discovery on machines with no ECDT: the `_HID "PNP0C09"` / `_CRS` walk needs an evaluable namespace, and that item is parked on this one. -> XREF: `02-kernel-core/TODO-26-power-management.md` §5 (item: "DSDT `_HID \"PNP0C09\"` / `_CRS` discovery fallback")
- [ ] Resolve `\_Sx` through the namespace evaluator, not a byte scan
  - `src/kernel/acpi.c` `parse_sleep_type()` scans the DSDT image for the literal bytes `_ S <digit> _` followed by a `PackageOp`.
  - It therefore misses every shape a real interpreter handles: an `\_Sx` defined in an SSDT rather than the DSDT (common on OEM firmware -- the scan never reads SSDTs at all), one defined as a `Method` instead of a static `Name`+`Package`, a package element that is a reference rather than an inline integer, and an object gated behind an `If`/`_OSI` branch.
  - Each of those reports the state UNSUPPORTED today. -> XREF: `02-kernel-core/TODO-26-power-management.md` §1
- [ ] Prefer the FADT extended GAS register fields over their 32-bit counterparts when non-zero
  - ACPI 6.5 section 5.2.9: `X_PM1a_EVT_BLK` (offset 148), `X_PM1a_CNT_BLK` (172) and their PM1b twins.
  - `include/kernel/acpi.h` now declares and offset-asserts `X_FIRMWARE_CTRL`/`X_DSDT`, but the PM1 register accesses still take the 32-bit block fields only, so firmware publishing only the GAS forms loses the SCI silently.
  - -> XREF: `02-kernel-core/TODO-26-power-management.md` §1
- [ ] ACPI Global Lock support in the OSL
  - `GBL_EN` (PM1_EN bit 5) and the FACS lock word arbitrate OS-versus-SMM access to shared ACPI/EC hardware.
  - Currently inert because `acpi_enable_fixed_events()` deliberately enables only the bits it services, so `GBL_EN` is never set -- but the OSL contract requires it once ACPICA drives the hardware.
  - `src/kernel/acpi_global_lock.c` already holds the algorithm.
- [ ] TPM2 ACPI start method (2/8): retrofit `tpm_transport_init()` start-method dispatch to invoke the TPM2 table's AML start method (replaces degrade-with-WARN). -> XREF: `01-boot-platform/TODO-13` §2 (consumer)
- [ ] Boot log: `[ACPI] ACPICA %s initialised; namespace: %u objects`
- [ ] Commit: `"acpi: ACPICA AML interpreter -- OSL, AcpiInitializeSubsystem, namespace load"`

## 2. Clean Shutdown Sequence `[Sonnet]`

`system_shutdown(reason)` is a deterministic multi-stage orchestrator: broadcast `WM_CLOSE` to all user processes, wait up to 5 s and force-kill stragglers, flush filesystem caches, flush Registry, release DHCP lease, unmount all volumes, then call `acpi_poweroff()` or `acpi_reboot()`.

**Files:** `src/kernel/main/shutdown.c` (new), `include/kernel/shutdown.h` (new)

- [ ] Define `shutdown_reason_t`: `SHUTDOWN_POWEROFF`, `SHUTDOWN_REBOOT`, `SHUTDOWN_RESTART`, `SHUTDOWN_THERMAL_EMERGENCY`, `SHUTDOWN_HIBERNATE`
- [ ] `system_shutdown(reason)`:
  - Broadcast `WM_CLOSE` (or equivalent message) to all GUI processes; 5 s deadline
  - After deadline: `task_kill_all_user()` -- force-terminate remaining processes
  - `Registry_flush_all()` -- write all dirty hive pages to disk
  - `vfs_cache_flush()` -- flush block cache
  - `dhcp_release()` (if network up)
  - `vfs_unmount_all()` -- unmount in reverse mount order
  - Storage controller shutdown: flush device caches + issue per-controller shutdown notification (NVMe `CC.SHN`) before power-off -> XREF `01-boot-platform/TODO-16` §6 provides `nvme_shutdown_all()` + the `bd.flush` cache-flush fn (interim hook in `acpi_shutdown()` until this orchestrator lands)
  - Dispatch: `POWEROFF` → `acpi_poweroff()`; `REBOOT` → `acpi_reboot()`; `HIBERNATE` → `acpi_hibernate()`
- [ ] `system_flush()` (no shutdown) -- steps 3–5 only; called by §9 S3 and §10 hibernate
- [ ] Per-step timeout: if step stalls > 10 s, log `[SHUTDOWN] step timed out, forcing` and continue
- [ ] Registry key `HKLM\SYSTEM\Shutdown\TimeoutSeconds` (default 5 for WM_CLOSE wait)
- [ ] Boot log: `[SHUTDOWN] Initiating %s -- reason: %s`; each step logged
- [ ] Stop-the-world barrier before storage quiesce: global shutting-down state rejecting new FS/block I/O, drain writers, park other CPUs (IPI), halt ALL CPUs on poweroff failure -> XREF `01-boot-platform/TODO-16` §6
- [ ] Commit: `"kernel: clean shutdown orchestrator -- WM_CLOSE, flush, unmount, acpi_poweroff"`

## 3. ACPI Power Button SCI `[Sonnet]`

Configure the System Control Interrupt from FADT `SCI_INT`, route via IOAPIC as level-triggered, install an ISR. On PM1a `PWRBTN_STS` assertion, initiate the clean shutdown sequence in §9. Support short-press (shutdown) vs. long-press (force power-off) distinction via press duration timer.

**Files:** `src/kernel/acpi/acpi_button.c` (new), `include/kernel/acpi/acpi_button.h` (new)

- [ ] Read `SCI_INT` (16-bit) from FADT; `ioapic_route_irq(sci_gsi, sci_vector, LEVEL, ACTIVE_LOW)`; `idt_register_handler(sci_vector, acpi_sci_isr)`
- [ ] `acpi_sci_isr()`: read PM1a Status register (`PM1a_EVT_BLK + 0`); check bit 8 (`PWRBTN_STS`); clear by writing 1; if set: record `press_time = uptime_ns()`; queue `acpi_button_work`
- [ ] `acpi_button_work`: if `uptime_ns() - press_time < 2_000_000_000` (2 s) → `system_shutdown(SHUTDOWN_POWEROFF)`; else → `acpi_poweroff()` immediately
- [ ] Enable PM1a Enable register bit 8 (`PWRBTN_EN`) to unmask the button interrupt
- [ ] Enter ACPI mode the way the spec defines it: write `FADT.ACPI_ENABLE` to `FADT.SMI_CMD`, then POLL `PM1_CNT.SCI_EN`
  - OSPM never writes `SCI_EN` directly (ACPI 6.5 section 4.8.3.2 / 5.2.9). The wording here previously named a direct `SCI_EN` write, which is the wrong mechanism.
  - `src/kernel/acpi.c` `acpi_pm1_control_owned()` already implements the handshake for the pre-ACPICA path; this item is the ACPICA replacement (`AcpiEnableSubsystem` -> `acpi_hw_set_mode`).
  - Corrected 2026-09-03 from the TODO-26 §1 review. -> XREF: `02-kernel-core/TODO-26-power-management.md` §1
- [ ] Boot log: `[ACPI] Power button SCI: GSI %u, vector 0x%02X`
- [ ] Commit: `"acpi: power button SCI -- FADT SCI_INT routing, PWRBTN_STS, shutdown trigger"`

## 4. Thermal Monitoring `[Sonnet]`

Read per-core temperature from `IA32_THERM_STATUS` MSR (`0x19C`): `Tj_max - digital_readout`. Configure the LAPIC Thermal LVT to deliver a thermal interrupt at Tj_max - 10 °C. Expose `QueryCpuTemperature(id)` syscall and color-coded per-core bars in Task Manager. Emergency shutdown above 100 °C.

**Files:** `src/kernel/acpi/acpi_thermal.c` (new), `include/kernel/acpi/acpi_thermal.h` (new)

> [!IMPORTANT]
> `Tj_max` (junction temperature maximum) is read from `MSR_TEMPERATURE_TARGET (0x1A2)`, bits 23:16. This is model-specific -- fall back to 100 °C if the MSR is not available (`CPUID` feature check via `CPUID.06H:EAX[0]` DTS bit before reading).

- [ ] `acpi_thermal_init()`: check `CPUID.06H:EAX[0]` (DTS); read `MSR_TEMPERATURE_TARGET (0x1A2)` bits 23:16 → `g_tj_max`; default 100 if unavailable
- [ ] `thermal_read_core_temp(cpu_id)` → `rdmsr(IA32_THERM_STATUS, cpu_id)` per-CPU; extract bits 22:16 (`digital_readout`); return `g_tj_max - digital_readout`
- [ ] Configure LAPIC Thermal LVT (offset `0x330`): delivery=FIXED, vector=`THERMAL_VECTOR`, masked=0 when `g_tj_max - 10` threshold reached
- [ ] `thermal_interrupt_handler`: read `IA32_THERM_STATUS`; log `[THERMAL] CPU%u: %u °C (threshold hit)`; if temp > 100: `system_shutdown(SHUTDOWN_THERMAL_EMERGENCY)`
- [ ] `SYS_QUERY_CPU_TEMP(cpu_id)` → return `thermal_read_core_temp(cpu_id)`
- [ ] Write `HKLM\HARDWARE\CPU\Temperature\Core<N>` on each sample (polled every 5 s via workqueue)
- [ ] Task Manager hook: per-core temperature bars; green < 60 °C, yellow 60–80, orange 80–95, red > 95
- [ ] Boot log: `[THERMAL] DTS active; Tj_max=%u °C`
- [ ] Commit: `"acpi: thermal monitoring -- IA32_THERM_STATUS, Tj_max, LAPIC thermal LVT, Task Manager"`

## 5. Battery Status (`_BST` / `_BIF`) `[Sonnet]`

Evaluate ACPI `_BST` (Battery Status) and `_BIF` (Battery Information) methods via ACPICA every 30 s. Expose battery percentage, state (charging/discharging/critical), and estimated time remaining to the system tray and Registry.

**Files:** `src/kernel/acpi/acpi_battery.c` (new), `include/kernel/acpi/acpi_battery.h` (new)

- [ ] Discover battery device: walk ACPI namespace for devices with `_HID == "PNP0C0A"` via `AcpiGetDevices`
- [ ] `acpi_battery_update()`: `AcpiEvaluateObject(dev, "_BST", ...)` → parse `{ state, present_rate, remaining_cap, present_voltage }`
- [ ] `AcpiEvaluateObject(dev, "_BIF", ...)` → parse `{ design_cap, last_full_cap, tech, design_voltage, ... }` for design capacity
- [ ] `battery_percentage = (remaining_cap * 100) / last_full_cap`; clamp to 0–100
- [ ] `time_remaining_min = remaining_cap / present_rate * 60` when discharging; `-1` when charging
- [ ] Poll every 30 s via `workqueue_enqueue_delayed(&sys_wq, battery_update_work, 30000)`
- [ ] Write to Registry: `HKLM\HARDWARE\Battery\Percentage`, `State` (`"charging"/"discharging"/"critical"`), `TimeRemainingMin`
- [ ] System tray integration: set tray battery icon + tooltip string (→ XREF `09-desktop-shell` domain)
- [ ] Commit: `"acpi: battery status -- _BST/_BIF evaluation, percentage, tray icon, Registry"`

## 6. CPU Frequency Scaling -- DVFS `[Opus]`

Parse the `_PSS` (Performance Supported States) package for each processor via ACPICA. Detect Intel SpeedStep (`CPUID[01h].ECX[7]`) or AMD ACPI performance control. Register a `cpufreq_governor_t` driver (→ XREF `03-memory-concurrency/TODO-06-scheduler-enhancement.md §9`) that switches P-states by writing `IA32_PERF_CTL` MSR (Intel) or `PERF_CTL` (AMD). Performance governor when scheduler run-queue is non-empty; powersave on 100 ms idle.

**Files:** `src/kernel/acpi/acpi_cpufreq.c` (new), `include/kernel/acpi/acpi_cpufreq.h` (new)

> [!IMPORTANT]
> `IA32_PERF_CTL` writes must follow `_PTC` (Performance Throttle Control) method for the correct I/O vs. MSR access path -- not all platforms use MSR directly. Evaluate `_PTC` first; use its `PTC_control` register descriptor to determine access type. Fall back to MSR `0x199` for Intel systems that omit `_PTC`.

- [ ] `acpi_cpufreq_init()`: for each CPU, `AcpiEvaluateObject(cpu_dev, "_PSS", ...)` → parse array of `{ CoreFreq, Power, TransLatency, BmLatency, Control, Status }`; store in `g_pss_table[cpu][pstate]`
- [ ] Detect `_PTC`: read access type (`FFixedHW` = MSR, `SystemIO` = I/O port); store `ptc_control_reg`
- [ ] `acpi_cpufreq_set_pstate(cpu, pstate)`: write `pss.Control` to `ptc_control_reg`; poll `ptc_status_reg` for `pss.Status` within transition latency
- [ ] Register `cpufreq_driver_t { .set_pstate = acpi_cpufreq_set_pstate, .max_pstate = pss_count - 1 }` via `cpufreq_register_driver()`
- [ ] Per-CPU P-state transitions logged at `LOG_DEBUG`: `[CPUFREQ] CPU%u P%u → P%u (%u MHz)`
- [ ] Boot log: `[CPUFREQ] ACPI P-states: %u levels, %u–%u MHz`
- [ ] Commit: `"acpi: DVFS -- _PSS P-state table, IA32_PERF_CTL/PTC, cpufreq_register_driver"`

## 7. Power Profiles `[Sonnet]`

Three named power profiles -- Balanced, Performance, Power Saver -- configure the cpufreq governor, C-state latency budget, and display timeout. Active profile stored in Registry; `powercfg.cpl` stub allows selection.

**Files:** `src/kernel/acpi/acpi_power_profile.c` (new), `include/kernel/acpi/acpi_power_profile.h` (new)

- [ ] Define `power_profile_t { const char *name; const char *cpufreq_governor; uint32_t idle_latency_budget_ns; uint32_t display_timeout_s; }`
- [ ] Built-in profiles:
  - `Balanced`: governor=`"powersave"`, `idle_latency_ns=500000` (500 µs), display timeout=5 min
  - `Performance`: governor=`"performance"`, `idle_latency_ns=50000` (50 µs, no C3), display timeout=never
  - `Power Saver`: governor=`"powersave"`, `idle_latency_ns=UINT32_MAX` (all C-states), display timeout=1 min
- [ ] `power_profile_apply(profile)`: call `cpufreq_set_governor(profile->cpufreq_governor)`; set `g_idle_latency_budget_ns = profile->idle_latency_budget_ns`; set display timeout
- [ ] Active profile read from `HKLM\SYSTEM\Power\Profile` (`REG_SZ`); apply at boot and on Registry change
- [ ] `powercfg.cpl` stub: shell command `powercfg /setactive Balanced|Performance|PowerSaver`
- [ ] System tray power icon reflects active profile + battery percentage (consolidated display)
- [ ] Boot log: `[POWER] Profile: %s (governor=%s, idle_budget=%u µs)`
- [ ] Commit: `"acpi: power profiles -- Balanced/Performance/PowerSaver, Registry, powercfg stub"`

---

## 8. ACPI C-States Idle `[Opus]`

Read the `_CST` (C-States) package for each processor via ACPICA. Enter C1 via `hlt` on idle, C2/C3 via `mwait` with the appropriate hint from the `_CST` register descriptor. Track per-CPU idle depth histogram in `/sys/cpuidle`.

**Files:** `src/kernel/acpi/acpi_cpuidle.c` (new), `include/kernel/acpi/acpi_cpuidle.h` (new), `src/kernel/sched/sched.c`

> [!IMPORTANT]
> `mwait` requires the address hint (`eax`) to match the C-state sub-state from `_CST`; the monitor address (`ecx`) must be cache-line aligned. `mwait` wakes on any store to the monitor address -- the scheduler's `need_resched` flag is a good monitor target. C3 requires `WBINVD` or `BM_RLD` before entry per ACPI spec §8.1.

- [ ] `acpi_cpuidle_init()`: `AcpiEvaluateObject(cpu_dev, "_CST", ...)` → parse `{ count, cstate[N]{ register, type, latency, power } }`; store in `g_cst[cpu][N]`
- [ ] `cpuidle_enter(cpu, cstate)`: C1 → `__asm__("hlt")`; C2/C3 → `__asm__("monitor; mwait")` with `eax = cstate.sub_state`, `ecx = 0`; C3 pre-entry: `WBINVD` if `_CST` register type requires it
- [ ] Idle thread body: select deepest C-state whose `latency ≤ g_idle_latency_budget_ns`; call `cpuidle_enter(this_cpu(), selected)`
- [ ] `g_idle_latency_budget_ns` set by power profile §7: Balanced=500 µs, Performance=50 µs, Power Saver=no limit
- [ ] Per-CPU idle stats: `g_cpuidle_stats[cpu][cstate]{ entry_count, residency_ns }` updated on each exit
- [ ] `/sys/cpuidle` VFS file: columns `CPU  C0%  C1%  C2%  C3%  deepest_entered`
- [ ] Commit: `"acpi: C-states idle -- _CST parsing, hlt C1, mwait C2/C3, /sys/cpuidle stats"`

## 9. ACPI S3 Suspend / Resume `[Opus]`

Enter S3 (Suspend to RAM): flush filesystem caches and Registry, save CPU register state + GDT/IDT/CR3/EFER, park APs, write `SLP_TYP_S3 | SLP_EN` to PM1a_CNT. On resume, the BIOS/firmware jumps to the 1 MiB wakeup trampoline; the trampoline re-enters long mode and calls `acpi_resume()` which restores full kernel state, re-inits APIC and drivers.

**Files:** `src/kernel/acpi/acpi_sleep.c` (new), `src/kernel/smp/smp.c`, `src/boot/acpi_wakeup.asm` (new)

> [!CAUTION]
> The wakeup vector must be a real-mode-compatible address (< 1 MiB). Write it to FACS `FirmwareWakingVector` (32-bit) and/or `XFirmwareWakingVector` (64-bit). The trampoline at that address runs in 16-bit real mode immediately after resume -- it must set up GDT, enable Protected Mode, then Long Mode, then call `acpi_resume()` in 64-bit code. This is identical to the SMP AP trampoline pattern.

- [ ] `acpi_suspend_s3()`:
  - Call `system_flush()` (→ §2): VFS `cache_flush()`, Registry flush, `DHCP_release()`
  - Park all APs: `lapic_send_ipi_all_but_self(IPI_HALT)`
  - Save BSP state: `struct cpu_sleep_state { rsp, rbp, rbx, r12–r15, cr3, cr4, efer, gdt_base, idt_base }`
  - Write wakeup vector physical address to FACS `FirmwareWakingVector`
  - Clear PM1a STS; write `SLP_TYP_S3 | SLP_EN` to PM1a_CNT; `hlt` loop
- [ ] `src/boot/acpi_wakeup.asm` -- 16-bit trampoline: set CS/DS/SS; load 32-bit GDT; enable CR0.PE; jump to 32-bit stub; enable PAE + long mode EFER; load saved CR3; enable CR0.PG; jump to 64-bit `acpi_resume()`
- [ ] `acpi_resume()`: restore `cpu_sleep_state` from saved struct; re-call `lapic_init_ap()` for each AP; re-init IOAPIC routing; call `hpet_init()`, disk/NIC re-init callbacks; return to interrupted kernel code via saved `rip`
- [ ] Re-init driver list: each driver with an `suspend`/`resume` callback in `driver_t` vtable; call `resume()` in registration order
- [ ] Commit: `"acpi: S3 suspend/resume -- trampoline, CPU state save/restore, driver re-init"`

## 10. Hibernate (S4) `[Opus]`

Serialise all physical RAM pages to `C:\Impossible\System\hiberfil.sys`, write an S4 signature header, then power off via S5. On next boot, the bootloader detects the hiberfil signature and passes `BOOT_HIBERNATE_RESUME=1` in `boot_info`; the kernel reads the file, restores all pages, and jumps to the saved resume instruction pointer.

**Files:** `src/kernel/acpi/acpi_hibernate.c` (new), `src/boot/uefi/bootx64.c`, `include/kernel/acpi/acpi_hibernate.h` (new)

> [!CAUTION]
> Hibernate writes the **entire** physical RAM image -- this may be gigabytes. Use DMA-capable sequential block writes (AHCI/NVMe); never use `kmalloc`-buffered I/O for this. Page compression (LZNT1 or LZ4 frame per page) is strongly recommended but optional for the initial implementation. The hiberfil header must encode `total_pages`, `cr3` snapshot, and `rip` resume point.

- [ ] `acpi_hibernate()`:
  - `system_flush()` (→ §2): VFS flush, Registry flush
  - Park all APs; disable interrupts on BSP
  - Open `C:\Impossible\System\hiberfil.sys` for write; write `HIBERFIL_HEADER { magic, version, total_pages, resume_rip, resume_cr3 }`
  - Walk PMM: for each present physical page, write page data sequentially; skip ACPI reclaimable and reserved regions
  - Write `SLP_TYP_S5 | SLP_EN` to PM1a_CNT (power off)
- [ ] Bootloader §1 check: at boot, after VFS init, open `C:\Impossible\System\hiberfil.sys`; if `magic` matches: set `boot_info.flags |= BOOT_HIBERNATE_RESUME`; pass file handle
- [ ] `acpi_hibernate_resume()` (called early in `kernel_main` if flag set): read pages back to their physical addresses; restore GDT/IDT/CR3/EFER from header; `jmp resume_rip`
- [ ] After successful hibernate: delete hiberfil.sys (or truncate) to prevent stale resume on next cold boot
- [ ] Boot log on cold boot: no hiberfil → normal boot; on resume: `[ACPI] Hibernate resume: restoring %u MB...`
- [ ] Commit: `"acpi: hibernate S4 -- hiberfil.sys write, bootloader detection, memory restore, jmp resume"`

## OS Comparison


| ⭐  | Feature                                            | 🪟 Win11                                                           | 🐧 Linux                                                           | 🚀 Impossible OS                                                             |
| --- | -------------------------------------------------- | ------------------------------------------------------------------ | ------------------------------------------------------------------ | ---------------------------------------------------------------------------- |
| 💎  | AML interpreter                                    | ✅ `ACPI.sys` -- Microsoft AML interpreter                         | ✅ `drivers/acpi/` -- ACPICA static                                | 🔶 §1 -- ACPICA + OSL vendored/linked (release flavor), `AcpiEvaluateObject` |
| 💎  | S3 suspend/resume -- trampoline + state restore    | ✅ `ntoskrnl` power manager; `PO_S3_RESUME` wakeup                 | ✅ `kernel/power/suspend.c`; `arch/x86/power/hibernate_asm_64.S`   | ⬜ §9 -- 1 MiB real-mode trampoline, `cpu_sleep_state`,                      |
| 💎  | Battery `_BST`/`_BIF` → tray percentage            | ✅ `battc.sys`; `PoQueryBatteryStatus`; Windows tray icon          | ✅ `drivers/acpi/battery.c`; `upower` userspace daemon             | ⬜ §5 -- 30 s poll, system tray                                              |
| 💎  | CPU DVFS -- `_PSS` / `IA32_PERF_CTL` P-states      | ✅ `processor.sys`; processor power policy; `PPM`                  | ✅ `drivers/cpufreq/acpi-cpufreq.c`; `ondemand`/`performance` govs | ⬜ §6 -- `_PSS` parse, `IA32_PERF_CTL`/`_PTC`, `cpufreq_register_driver`     |
| 💎  | Power button SCI → clean shutdown                  | ✅ `ACPI.sys` SCI ISR; `PoRequestPowerIrp(PowerActionShutdown)`    | ✅ `drivers/acpi/button.c`; `PWRBTN_STS` → `kernel_power_off()`    | ⬜ §3 -- FADT `SCI_INT` route, PWRBTN_STS, `system_shutdown()`               |
| 💎  | Per-core thermal -- `IA32_THERM_STATUS` + Task Mgr | ✅ `ACPI.sys` thermal zone; Task Manager                           | ✅ `drivers/hwmon/coretemp.c`; `sensors` tool; `_TZ` via           | ⬜ §4 -- `IA32_THERM_STATUS`, LAPIC Thermal LVT, color                       |
| 💎  | C-state idle -- `hlt` C1, `mwait` C2/C3            | ✅ `processor.sys`; `PROC_IDLE_STATE_INFO`; governor selects depth | ✅ `drivers/cpuidle/`; `menu` governor; `mwait` sub-states;        | ⬜ §8 -- `_CST` parse, `mwait` C2/C3, `/sys/cpuidle`                         |
| 💎  | Hibernate S4 -- hiberfil.sys + bootloader restore  | ✅ `hiberfil.sys`; `ntldr`/`winload` resumes from file             | ✅ `kernel/power/hibernate.c`; `swsusp_write()`; `initrd` restores | ⬜ §10 -- sequential PMM page write, bootloader                              |
| 💎  | Clean shutdown sequence with per-step timeout      | ✅ Session Manager orchestrates WM_CLOSE →                         | ✅ `systemd` shutdown: `SIGTERM` → `SIGKILL`                       | ⬜ §2 -- `system_shutdown()`, WM_CLOSE, 5 s kill,                            |
| 💎  | Named power profiles -- Balanced/Performance/Saver | ✅ Windows power plans; `powercfg /setactive`                      | ✅ `cpupower` / `tlp`; `power_profile` kernel                      | ⬜ §7 -- 3 built-in profiles, Registry `SYSTEM\Power\Profile`,               |

> **After §1–10:** Impossible OS achieves full ACPI OSPM parity with Windows 11 and Linux for laptop and desktop hardware. Every gap -- AML evaluation, S3/S4, battery, DVFS, thermal, C-states, clean shutdown -- is closed. The Task Manager temperature display (§4) provides a visual differentiator: per-core color-coded bars that neither Linux `sensors` nor Windows Task Manager's basic CPU pane expose in a single integrated view.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Boot log: `[ACPI] ACPICA X.Y.Z initialised; namespace: N objects`
- [ ] QEMU `-machine q35`: `acpi_evaluate("\_SB.PCI0._CRS", ...)` returns a resource descriptor (AML evaluation working)
- [ ] S3: QEMU `system_suspend` → serial shows `[ACPI] Entering S3`; resume shows APIC re-init and `[OK]` driver lines
- [ ] Battery: QEMU `-battery` or virtual battery: tray shows `85% (charging)` after `_BST` evaluation
- [ ] DVFS: `_PSS` with 4 states; log shows `[CPUFREQ] ACPI P-states: 4 levels`; stress test toggles to P0; idle falls to P3
- [ ] Power button: QEMU sends ACPI power button event → serial shows `[SHUTDOWN] Initiating poweroff` → clean shutdown
- [ ] Thermal: `thermal_read_core_temp(0)` returns plausible value (25–95 °C in QEMU)
- [ ] Hibernate: `system_shutdown(SHUTDOWN_HIBERNATE)` → hiberfil.sys written → power-off; cold boot → `[ACPI] Hibernate resume: restoring N MB` → desktop restored
- [ ] `powercfg /setactive Performance` → `[POWER] Profile: Performance` in log; scheduler stays at max P-state under load
- [ ] Commit: `"acpi: full OSPM -- ACPICA, S3, S4, battery, DVFS, thermal, C-states, shutdown, profiles"`
