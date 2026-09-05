---
schema_version: 1
id: power-management
domain: 02-kernel-core
status: active
title: "TODO-26 -- Power Management (S-States, D-States, Thermal & Idle)"
---

# TODO-26 -- Power Management (S-States, D-States, Thermal & Idle)

> **Validated:** 2026-09-03 | validate-todo-file clean; §1's missing Notes block added; 8 mistargeted compact/full XREFs corrected (each verified against the real target section heading -- D02T19§9->D02T09§9, D04T04§1/§4/§6/§7->D04T03§4/§6/§4/§8, D03T05§9->D03T06§9, D02T05§4->D02T12§4, TODO-08§1->§3, TODO-12§5->§4, TODO-11§2->§7) plus the self-contradicting legend example; todo-graph 10/10; no hard-wrap; no code-block bloat; bat runner present
> **Gap-audited:** 2026-09-03 | gap-audit + codex-gap-audit; confirmatory parity pass (existing plan already comprehensive from its 2026-04-13 gap-analysis) found 1 new Win11 25H2 gap; codex-gap-audit red-teamed the diff and found 2 pre-existing High findings (S3 orchestration specified at DISPATCH_LEVEL where it blocks; charge-limit item wrote a vendor-guessed EC register with no capability check) plus corrected the new item's own policy model (was global C-state bias, should be per-process QoS per Microsoft's actual mechanism), a backwards ACPICA ownership boundary, and 2 more coverage gaps (composite/multi-battery, PCIe ASPM+L1SS); all fixed. 2 new sections added (§22 QoS throttling, §23 PCIe ASPM), 3 items added to §6, 21->23 sections total.

> **Goal:** Implement the complete ACPI power management stack beyond the S5 shutdown that already works. This covers C1 processor idle (`HLT`), S3 suspend-to-RAM, S4 hibernate-to-disk, fast startup (hybrid shutdown / hiberboot), PCI/device D-states (D0--D3cold), runtime device idle management, the ACPI Embedded Controller (EC) driver required for every laptop, battery and AC adapter status (`_BIF`/`_BIX`/`_BST`), power button and lid-close event handling, driver power callbacks with query/veto and correct resume ordering, ACPI thermal zone management (`_TMP`/`_CRT`/`_HOT`/`_PSV`/`_ACx`) with passive and active cooling, CPU idle governor framework (C-states via `_CST`/`MWAIT`), CPU frequency scaling governor framework (HWP/CPPC/`_PSS`), connected standby (S0ix / Modern Standby), power request tracking, wake source management, and the power-plan UI. Without this, Impossible OS has no viable story on laptops or any real hardware that expects ACPI power events.

> [!IMPORTANT]
> **Current state:** `src/kernel/acpi.c` implements RSDP through XSDT walk, FADT (PM1a control, PM timer), MADT, `acpi_shutdown()` / `acpi_reboot()`, and **`acpi_power_init()`** which parses `\_S1_`, `\_S3_`, and `\_S4_` from the DSDT via **`parse_sleep_type()`**, plus **`acpi_sleep_supported()`** / **`acpi_get_slp_typa()`** and the `Sleep states: S1=...` klog line. **`acpi_enter_sleep_state()`** performs the ACPI PM1a/b SLP_TYP+SLP_EN sequence then `sti; hlt` (S1-style wake only today; S3/S4 still need §3/§4 state save, FACS vector, and firmware resume). **`acpi_enable_fixed_events()`** and **`acpi_register_sci()`** / **`acpi_sci_process()`** enable the PM1a+PM1b fixed-event SCI path; the ISR acknowledges the hardware and records event counts only (it must not log -- `klog()` reaches disk I/O), and §7 owns the deferred user-visible dispatch. Phase 2 **`boot_storage.c`** wires `acpi_power_init()`, `acpi_enable_fixed_events()`, and `acpi_register_sci()` after timer init. **`src/kernel/test/test_acpi_power.c`** covers §1 discovery and unsupported-state rejection. **Still greenfield:** EC (§5), battery (§6), S3/S4 (§3/§4), C1 processor idle entry (§2), PCI D-states (§8), governors and `powercfg` (§15+), power syscalls (§20), Linux sysfs parity doc (§21).

> [!CAUTION]
> **Memory rule:** Hibernation image buffers can be multi-gigabyte: always use `pmm_alloc_contiguous()` for hibernation scratch pages. Never `kmalloc` anything > 4 KiB in the suspend/hibernate paths.

> [!IMPORTANT]
> **Scope boundary with `04-drivers-hardware/TODO-03-acpi-power-management.md` (corrected 2026-09-03, Codex gap-audit -- the ownership direction was backwards):** `04-drivers-hardware/TODO-03` owns the ACPICA-based AML interpreter integration and becomes authoritative for the ACPI-driven paths below once its sections land. Until then, THIS file's hand-rolled parsers (same pattern as the existing `\_S5_` parser) are the shipped implementation: §1 (sleep-object parsing) is superseded by D04T03§1 (ACPICA integration); §3 (S3 suspend) by D04T03§9; §4 (S4 hibernate) by D04T03§10; §6 (battery) by D04T03§5; §7 (power button) by D04T03§3. **§2 (C1 idle entry/accounting), §8 (PCI D-states), §9 (driver callbacks), §10 (S0ix), and §18 (power plan UI / `powercfg`) are kernel-core responsibilities NOT covered by TODO-03 and remain authoritative here regardless of ACPICA status.**
> → XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md §1` -- ACPICA integration; supersedes this file's hand-rolled §1 sleep-object parser
> → XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md §9` -- authoritative S3 suspend/resume (ACPICA path); §3 here is the pre-ACPICA fallback
> → XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md §5` -- authoritative battery `_BST`/`_BIF` (ACPICA path); §6 here is the pre-ACPICA fallback
> → XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md §3` -- authoritative power button SCI (ACPICA path); §7 here is the pre-ACPICA fallback
> → XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md §10` -- authoritative S4 hibernate (ACPICA path); §4 here is the pre-ACPICA fallback

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
- → XREF: `TODO-08-time-filetime-management.md §3` -- TSC must be recalibrated after S3/S0ix wake (clock drift); `acpi_pm_timer_read()` used as reference; §3 = Invariant TSC Detection and Per-CPU Offset Calibration
- → XREF: `TODO-08-time-filetime-management.md §14` -- S3/S4 resume path must call `ke_suspend_bias_update()` to adjust `InterruptTimeBias` by the sleep duration; §14 = Suspend/Hibernate Time Bias Tracking
- → XREF: `TODO-01-kernel-init-sequencing.md §3` -- S4 resume check runs early in Phase 1; must distinguish cold boot from hibernate resume via hibernation signature
- → XREF: `04-drivers-hardware/TODO-05-kernel-module-system.md §3` -- driver model HAL vtables required for USB xHCI to register power callbacks; xHCI D3cold->D0 handled via callback registered in §9
- → XREF: `05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md §1` -- IXFS WAL journal (`ixfs_journal_begin`/`commit`/`abort`) must be verified (§1 Subsystem Verification) before S4 journal-flush dependency is safe; storage driver must reach D0 before journal replay on resume
- → XREF: `TODO-12-native-api-ssdt.md §4` -- SSDT indices 0x00D7 (NtShutdownSystem) and 0x0140--0x0145 (NtSetSystemPowerState, NtInitiatePowerAction, NtPowerInformation, NtGetDevicePowerState, NtSetThreadExecutionState, NtRequestWakeupLatency) reserved for this TODO; TODO-12 §22 wires power syscalls into the SSDT
- → XREF: `01-boot-platform/TODO-11-interrupt-timer-arch.md §7` -- LAPIC timer recalibration required after HWP/CPPC frequency changes (§15 CPU frequency scaling)
- → XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md §6` -- ACPICA-based `_PSS` P-state parsing; §15 here owns the kernel-core governor framework that consumes it
- → XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md §4` -- ACPICA-based `IA32_THERM_STATUS` per-core temp; §14 here owns the ACPI thermal zone framework
- → XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md §8` -- ACPICA-based `_CST` C-state parsing; §16 here owns the kernel-core idle governor
- → XREF: `03-memory-concurrency/TODO-06-scheduler-enhancement.md §9` -- `cpufreq_register_driver()` vtable consumed by §15; scheduler provides load metrics for governor
- → XREF: `02-kernel-core/TODO-09-x86-64-architecture.md §9` -- Intel hybrid P/E-core detection feeds §15 HWP/CPPC governor with core asymmetry data
- → XREF: `04-drivers-hardware/TODO-17-gpu-display-drivers.md` -- GPU power management (DPMS, RTD3 runtime D3, Panel Self-Refresh) owned by GPU TODO; §18 `DisplayOffTimeout` triggers DPMS via `gfx_set_dpms(DPMS_OFF)`

---

## Outcome

- C1 (`HLT`) reduces power during processor idle; no visible effect on software state. S1 is a system sleep state and is NOT the idle path -- see §2.
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

| ⭐  | Order | Deliverable                                                        | Depends On                  | Status |
| --- | :---: | ------------------------------------------------------------------ | --------------------------- | :----: |
| 💎  |   1   | §1 ACPI sleep object parsing & PM1 state machine                   | (none)                      |  [x]   |
| 💎  |   2   | §2 C1 idle entry: race-safe HLT + idle accounting                  | §1                          |  [x]   |
| 💎  |   3   | §3 S3: suspend to RAM (CPU state + driver freeze)                  | §1, §2, §26, §9, D04T03§1   |  [/]   |
| 💎  |   4   | §4 S4 hibernation image format + LZ4 chunk codec                   | (none)                      |  [x]   |
| 💎  |   5   | §5 ACPI EC driver (discovery+transactions; GATED OFF, needs §24)   | §1                          |  [/]   |
| 💎  |   6   | §6 Battery & AC adapter ACPI source layer (`_BIX`/`_BST`/`_PSR`)   | §29, D04T03§1               |  [/]   |
| 💎  |   7   | §7 Power & sleep button event dispatch                             | §1                          |  [x]   |
| 💎  |   8   | §8 PCI PM capability + D0--D3hot state machine                     | §1                          |  [x]   |
| 💎  |   9   | §9 Driver power callbacks & resume ordering                        | §3, §8                      |  [/]   |
| ⭐  |  10   | §10 S0ix firmware advertisement + MWAIT capability layer           | §2                          |  [x]   |
| 💎  |  11   | §11 Fast Startup (hybrid shutdown / hiberboot)                     | §4, §9, §28                 |  [/]   |
| 💎  |  12   | §12 Runtime device idle management                                 | §8, §9                      |  [ ]   |
| 💎  |  13   | §13 Power request tracking & wake source management                | §9, §12                     |  [ ]   |
| 💎  |  14   | §14 ACPI thermal zone management                                   | §5, §24, D04T03§4           |  [ ]   |
| 💎  |  15   | §15 CPU frequency scaling governor framework                       | §2, D04T03§6, D03T06§9      |  [ ]   |
| 💎  |  16   | §16 CPU idle governor framework                                    | §2, D04T03§8                |  [ ]   |
| 💎  |  17   | §17 Driver power query & veto (IRP_MN_QUERY_POWER)                 | §9                          |  [ ]   |
| 💎  |  18   | §18 Power plan UI & `powercfg`                                     | §7, §13, §14, §15, §16, §29 |  [ ]   |
| ⭐  |  19   | §19 Energy-aware scheduling integration                            | §15, §16, D02T09§9          |  [ ]   |
| 💎  |  20   | §20 Power syscalls wired to SSDT                                   | §2, §6, D02T12§4            |  [ ]   |
| 💎  |  21   | §21 Linux `/sys/power` suspend variant parity                      | §1, §10                     |  [ ]   |
| 💎  |  22   | §22 User-interaction-aware QoS throttling                          | §15, §19, §20               |  [ ]   |
| 💎  |  23   | §23 PCIe ASPM and L1 substates                                     | §8, §9, §12                 |  [ ]   |
| 💎  |  24   | §24 ACPI general-purpose event (GPE) blocks                        | §1, §5, §7                  |  [ ]   |
| 💎  |  25   | §25 Per-CPU idle accounting via NtQuerySystemInfo                  | §2                          |  [ ]   |
| 💎  |  26   | §26 Stop-the-world CPU rendezvous for sleep                        | §2                          |  [x]   |
| 💎  |  27   | §27 Rendezvous safety residue: seam + retract gap                  | §26                         |  [ ]   |
| 💎  |  28   | §28 S4 hibernation write path + resume consumer                    | §4, §3, §9, D02T27§7        |  [/]   |
| 💎  |  29   | §29 Composite battery model, warn policy, registry publish         | (none)                      |  [ ]   |
| 💎  |  30   | §30 Battery charge limiting and smart charging                     | §5, §24, §29                |  [/]   |
| 💎  |  31   | §31 Lid state and lid-close policy                                 | §5, §7, §24, D04T03§1       |  [/]   |
| ⭐  |  32   | §32 Human presence detection (wake on approach, lock on leave)     | §7, D04T03§1                |  [/]   |
| 💎  |  33   | §33 PM device registry (`pm_device_t`, `pm_register_device`)       | §8                          |  [ ]   |
| 💎  |  34   | §34 PCI D3cold via ACPI `_PS0`/`_PS3` platform methods             | §8, §33, D04T03§1           |  [ ]   |
| ⭐  |  35   | §35 Directed power (DFx) stack walk + DRIPS residency accounting   | §9, §10, §12                |  [ ]   |
| 💎  |  36   | §36 NIC wake offloads (ARP/NS reply, WoL, wake patterns, D0i3)     | §10, §12                    |  [ ]   |
| ⭐  |  37   | §37 System connected standby entry (all-CPU S0ix transition)       | §10, §27, §9, §28           |  [/]   |
| 💎  |  38   | §38 ACPI table discovery: validate extents before checksum/publish | §1                          |  [x]   |
| 💎  |  39   | §39 ACPI root-pointer integrity + validator unification            | §38                         |  [ ]   |

> 💎 = parity work: matches what Windows 11 and Linux already do.
> ⭐ = exclusive work: Impossible OS is superior or first.
> Compact XREF notation: D=domain, T=TODO, §=section (e.g. D02T06§3 = domain 02, TODO-06, §3).

---

## 1. ACPI Sleep Object Parsing & PM1 State Machine

- [x] `acpi_power_init()` added (Phase 2): parses `\_S1_`, `\_S3_`, `\_S4_` from DSDT using generalized `parse_sleep_type()` (refactored from `parse_s5_from_dsdt`)
- [x] `slp_typa_s1/s3/s4` with `ACPI_SLP_TYPE_INVALID = 0xFFFF` sentinel -- and a matching `slp_typb_*` per state, since ACPI 6.5 section 7.4.2 makes SLP_TYPa and SLP_TYPb independent per-register values
- [x] `acpi_table_valid()` gates the DSDT before any AML scan trusts its length
  - Signature, a length bounded by `ACPI_MAX_TABLE_LENGTH` (16 MiB), containment in one UEFI memory-map descriptor of an ACPI-bearing class, then the checksum.
  - The header's own containment is proven BEFORE `sig_match()` or the length field are read: both are dereferences, so checking after them is checking too late.
  - The DSDT is reached through the FADT's `dsdt` field and so never passed through `find_table_*()`'s checksum.
- [x] `aml_read_integer()` decodes ZeroOp/OneOp/OnesOp/Byte/Word/DWord/QWord prefixes (ACPI 6.5 section 20.2.3); values above 7 are rejected as malformed since SLP_TYP is a 3-bit PM1_CNT field
- [x] `acpi_sleep_supported(n)` -- returns 1 if sleep state N has a valid SLP_TYPa
- [x] `acpi_get_slp_typa(n)` -- returns the SLP_TYPa value for sleep state N
- [x] Wired into Phase 2 boot (`boot_storage.c`) after time subsystem init
- [x] Serial log: `"Sleep states: S1=yes/no S3=yes/no S4=yes/no S5=yes"`
- [x] `acpi_enter_sleep_state(uint8_t state)` -- generic sleep entry:
  1. Validates state support via `acpi_get_slp_typa()`
  2. `cli` -- disables interrupts
  3. Clears a stale `WAK_STS` in both status blocks so the post-halt check can tell a real resume from any interrupt that merely released the halt
  4. Writes SLP_TYP + SLP_EN into PM1a_CNT as a read-modify-write that PRESERVES the rest of the register (ACPI 6.5 section 4.8.3.2: PM1_CNT also carries SCI_EN bit 0 and BM_RLD bit 1; a wholesale write dropped the machine out of ACPI mode at the moment it was asked to sleep). PM1b_CNT gets its own SLP_TYPb when present
  5. `sti; hlt`, then confirms the wake through `WAK_STS` rather than assuming it -- returns -1 if the halt was released without a sleep-state exit
  6. S3/S4 are REFUSED (return -1): they are discovered by this section but entering either loses processor or DRAM state with no AP shutdown, device quiesce, cache flush, waking vector, or hibernation image in place. QEMU reports both supported, so without the refusal a caller gets a hung or reset machine instead of an error. -> XREF: `02-kernel-core/TODO-26-power-management.md` §3 (S3 suspend to RAM), §4 (S4 hibernate to disk)
  7. S5 is REFUSED: soft-off must go through `acpi_shutdown()`, which runs the storage durability barrier first and never returns
- [x] `acpi_enable_fixed_events()`: PWRBTN_EN + SLPBTN_EN in BOTH PM1a_EN and PM1b_EN
  - Clears pending status in each block and validates `PM1_EVT_LEN >= 4` before deriving either enable-register offset.
  - ACPI 6.5 section 4.8.3.1 lets a fixed-event bit live in either block, so a PM1a-only driver leaves a PM1b-implemented power button both disabled and unacknowledged.
  - Enables EXACTLY the bits the ISR services: an OR-update would preserve firmware-set TMR/GBL/RTC/PCIEXP_WAKE enables that nothing acknowledges, holding a level-triggered SCI asserted.
- [x] `acpi_register_sci()`: GSI routing on IOAPIC systems, ISA vector on PIC-only
  - IOAPIC: `irq_request_gsi_ex()`, translating an ISA `SCI_INT < 16` to its GSI through the MADT overrides, level-triggered active-low.
  - PIC-only: `isa_irq_to_vector()` + `idt_register_handler()` + `pic_unmask_irq()`, and it REFUSES rather than stealing a vector another driver already owns.
  - The vector is assigned dynamically, not `32 + sci_interrupt`: ISA IRQ 9 delivers at 0x71 after the slave-PIC remap, and the old fixed install received nothing on IOAPIC systems.
  - Idempotent -- a repeat call is refused instead of appending a second handler to the shared GSI chain.
- [x] `acpi_sci_process()`: acknowledges both PM1 blocks in ONE write-1-to-clear each, and does NOT log
  - ORs PM1a_STS with PM1b_STS, then clears every serviced bit per block in a single write; clearing bit by bit left the line asserted for the duration of the work between writes.
  - `klog()` reaches `klog_disk_flush()` -> `vfs_open`/`vfs_write` whenever live disk logging is armed (`klog.c:1881`), and `klog_disk_enable()` runs at `boot_storage.c:940`, BEFORE `acpi_register_sci()` at `:1151`.
  - So the first SCI could perform disk I/O in hard-IRQ context and deadlock against the storage completion interrupt the write waits on.
  - `acpi_power_button_count()` / `acpi_sleep_button_count()` / `acpi_wake_event_count()` are the thread-level readers; policy dispatch belongs to the power-button section.
- [x] Wired into Phase 2 boot after `acpi_power_init()`
- [x] Commit: `"kernel/acpi: S1/S3/S4 sleep type parsing, PM1 state machine, fixed-event ISR"`

**Test checkpoint:** `acpi_sleep_supported(5)` agrees with `acpi_get_slp_typa(5)` (both reflect whether `\_S5` actually parsed -- no fabricated type 0). `acpi_enter_sleep_state(2)` returns -1 (unsupported); `(6)` returns -1 (invalid); `(3)`/`(4)`/`(5)` return -1 (refused). Synthetic-DSDT tests cover Byte/Word prefix and ZeroOp/OneOp decoding, an independent SLP_TYPb, out-of-range rejection, a truncated package, an absent object, and the table validator's bad-checksum / oversized-length / short-length / wrong-signature refusals. 28 tests / 50 assertions in `test_acpi_power.c`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Verified:** 2026-09-03 | 14/14 items | build OK | 32902 kernel + 17 user-mode tests, 0 failures | smoke matrix 4/4 (TCG+KVM x 1+2 CPU) | boot log: `Sleep states: S1=no S3=yes S4=yes S5=yes`, `ACPI mode established (SCI_EN set)`, `SCI registered (SCI_INT 9, GSI 9, vec 0x32)`
> **Accepted:** [H] GPE-raised SCIs are never serviced or acknowledged, so a level-triggered SCI stays asserted and the shared-IRQ layer quarantines the GSI after 1000 all-NONE dispatches (`irq.c:511-513`) -> XREF: 02-kernel-core/TODO-26 §24 (item: "`acpi_gpe_init()` -- split each block into its status and enable halves" at line 886)
> **Accepted:** [H] `\_Sx` byte scan misses SSDT-defined, Method-defined, reference-element and conditionally-defined objects that a real AML evaluator resolves -> XREF: 04-drivers-hardware/TODO-03 §1 (item: "Resolve `\_Sx` through the namespace evaluator, not a byte scan" at line 103)
> **Accepted:** [M] `_PTS`/`_GTS` before the sleep write and `_WAK` on resume are never evaluated; skipping them is a known cause of laptops that appear to enter S3 but fail to power peripherals down -> XREF: 02-kernel-core/TODO-26 §3 (item: "Evaluate `_PTS(3)` BEFORE the PM1 SLP_TYP+SLP_EN write" at line 210)
> **Accepted:** [M] FADT extended GAS register fields (`X_PM1a_EVT_BLK`, `X_PM1a_CNT_BLK`) are still unread, so firmware publishing only those loses the SCI silently -> XREF: 04-drivers-hardware/TODO-03 §1 (item: "Prefer the FADT extended GAS register fields over their 32-bit counterparts" at line 104)
> **Accepted:** [M] `klog_disk_append` mutates the shared FAT32 staging buffer with a non-atomic position increment from any CPU and from interrupt context -> XREF: 02-kernel-core/TODO-04-system-logging §15 (item: "`klog_disk_append` mutates the shared FAT32 staging buffer with no lock" at line 513)
> **Accepted:** [M] ACPI Global Lock (`GBL_EN` + the FACS lock word) arbitrates OS-versus-SMM access and is required by the OSL contract once ACPICA drives the hardware -> XREF: 04-drivers-hardware/TODO-03 §1 (item: "ACPI Global Lock support in the OSL" at line 105)
> **Deferred:** [H] S1 is entered with every AP still running behind only a local `cli`; no AP park/rendezvous facility exists to fix it with -> XREF: 02-kernel-core/TODO-26 §2 (item: "Quiesce the APs before any SLP_EN write" at line 175)
> **Deferred:** [M] PM1b-only platforms are refused rather than half-supported: the control path handles them but the event and SCI paths remain PM1a-dependent -> XREF: 02-kernel-core/TODO-26 §24 (item: "PM1b-only platform support, END TO END" at line 893)
> **Quality reviewed:** 2026-09-03 | Codex 10x (adversarial, consistency, perf, re-adversarial x7) | 16H+9M+2L fixed, 1 rejected, 8 accepted/deferred | scope: kernel-code-quality + kernel-quality-auditor + concurrency-evidence-mapper + parity-research-analyst
> **Notes:**
> - **What shipped:** `acpi_power_init()`/`acpi_sleep_supported()`/`acpi_enable_fixed_events()`/`acpi_register_sci()`/`acpi_enter_sleep_state()` (`src/kernel/acpi.c:681-925`) -- PM1 sleep-type parsing + fixed-event SCI dispatch.
> - **How it integrates:** wired into Phase 2 boot in `boot_storage.c` after timer init, ahead of every §2+ consumer in this file.
> - **Downstream effects:** §2-§21 build on `acpi_sleep_supported()`/`acpi_enter_sleep_state()`; S3/S4 still need their own state-save/wakeup-vector work (§3/§4).
> - **Canonical doc:** `include/kernel/acpi.h`; `src/kernel/test/test_acpi_power.c` (28 tests / 50 assertions, `TEST_CAT_BOOT`; run via `scripts/debug/kernel/run-boot-tests.bat`).
> - **Scope boundary:** §1 owns discovery + the PM1 register sequence only; S3/S4 resume, EC, battery, governors are the sections below.

---

## 2. C1 Idle Entry: Race-Safe HLT + Per-CPU Idle Accounting

> **Spawned-by:** root

> [!IMPORTANT]
> **Redesigned 2026-09-03, from "S1: CPU Halt / Idle Thread Integration", by the pre-implementation Codex design review (5 findings, all verified at source and accepted).** The original spec called `acpi_enter_sleep_state(1)` from the scheduler idle loop. That is not implementable here, and would not be correct if it were: ACPI 6.5 section 8.1 defines C1-Cn as the per-processor idle states inside G0/S0 and gives them no meaning under S1-S4, so a PM1 `SLP_EN` write is a whole-machine firmware transition and not a deeper `HLT`. The shipped code says the same thing twice: `acpi_enter_sleep_state()` refuses every caller off the BSP (`src/kernel/acpi.c:1613`), so an AP could never take that path at all; and it confirms the wake through `WAK_STS` (`src/kernel/acpi.c:1667-1694`), which an ordinary timer tick does not set, so a real idle loop would take the `-1` "halt released without a wake event" branch and its `LOG_WARN` on essentially every idle episode, plus a `LOG_INFO` on each success -- timer-rate logging that `src/kernel/acpi.c` itself notes can reach VFS disk I/O. This section therefore ships the C1 primitive that §16 later governs, which is exactly what §16's own scope boundary already says §2 provides.

- [x] `pm_idle_c1()` in `src/kernel/pm_idle.c` -- the one-shot C1 entry that every idle site calls instead of a bare `sti; hlt`:
  - Save the caller's IF and `cli` FIRST, so the sample excludes LOCAL interrupt-side changes and the value reported is taken at the halt rather than stale from before it.
  - That is the whole of what masking buys, and the section says so rather than overclaiming: `cli` affects only the calling CPU, and `dpc_insert_core()` can target ANY CPU and increment its queue depth under that queue's lock (`src/kernel/sched/dpc.c:586-598`, `:648`). A remote enqueue can therefore land immediately after the sample. The predicate is ADVISORY, which is harmless here only because it is reported and never gates the halt -- §16 carries the blocking precondition for the interlocked idle-entry protocol and wake IPI it would need before gating anything.
  - Every access to that depth field uses ONE discipline (relaxed atomics on both the lock-held writers and this lock-free reader), because a plain write racing an atomic read is undefined however benign the emitted code looks.
  - Making those reads well-defined did NOT make the pre-existing exact-delta assertions in `test_dpc_insert_remove()` interference-proof, and that residue is filed with its owner rather than fixed here -> XREF: `02-kernel-core/TODO-07-irql-model-dpcs.md` §19 (item: "Gave the depth assertions an observation that cannot be perturbed by unrelated queue traffic: the SEAM, not a private queue").
  - Halt with the `sti; hlt` pair, whose STI interrupt shadow defers delivery until the HLT has begun. It is the only shape with no wake-lost window; a `sti` and a later `hlt` is not the same instruction sequence.
  - ALWAYS halt when the caller has interrupts enabled, and RETURN the sampled predicate rather than acting on it. A false predicate must never refuse the halt, which is the single most important property in this section and was learned by getting it wrong: an idle AP has no DPC drain trigger at all (`src/kernel/sched/ktimer.c:248-251` -- its LAPIC timer is masked and there is no DPC IPI), so a stranded DPC holds the predicate false forever and a refusing `for(;;)` park loop burns a whole logical CPU. Returning to a scheduler that can RUN the work is what a refusal is for, and no such caller exists until per-CPU run queues do.
  - Return 0 WITHOUT halting in exactly one case: the caller arrived with interrupts already masked, where halting would need `sti` and would run an ISR inside a region the caller believes is interrupt-free.
  - Read the TSC either side of the halt and accumulate the delta into this CPU's own `idle_tsc_cycles`, guarded by `t1 > t0` so a backwards sample cannot add ~2^64, then restore the caller's IF.
  - Allocation-free and log-free by contract: this runs at timer rate on an otherwise idle machine.
- [x] `pm_deep_idle_allowed()` -- the readiness predicate, exported for §16's governor and for the unit tests
  - False when this CPU's DPC queue is non-empty (`dpc_this_cpu_queue()->depth`, `include/kernel/sched/dpc.h:223`) or when the cached `PowerIdleEnable` flag is zero.
  - It REPORTS; it does not gate the halt. §16 inherits one contract, not two: the predicate says whether this CPU was idle by policy and had no queued DPC, and the governor decides what to do about it once there is a run queue to return to.
- [x] Registry knob `HKLM\System\CurrentControlSet\Control\Power\PowerIdleEnable` (`REG_DWORD`, Windows-parity path), defaulting to 1 when the value is absent:
  - Read ONCE into a cached flag at init, NEVER from the idle path. `src/kernel/registry.c` acquires no lock today (its own comments at lines 1043 and 1238 flag this as unfinished), so a per-idle read would race a concurrent writer at timer rate.
  - Publish and read that cached flag with release/acquire atomics. `smp_init()` runs at `src/kernel/main/boot_storage.c:271` and `pm_idle_init()` at `:1090`, both inside `boot_phase2()`, so every AP is already parked in `pm_idle_c1()` reading the word while the BSP writes it, and local interrupt masking orders nothing across CPUs.
- [x] `uint64_t idle_tsc_cycles` appended at the TAIL of `struct per_cpu_data` (`include/kernel/smp.h`), written only by its owning CPU:
  - The tail is required, not stylistic: `include/kernel/smp.h` pins several field offsets with `_Static_assert`, so a field inserted ahead of those shifts every one of them.
- [x] Wire the two sites where a CPU actually stops today, since the scheduler has no idle context of its own:
  - `src/kernel/main/compositor.c` -- BOTH of the BSP's terminal idle points, reached from `compositor_run()` where PID 0 comes to rest: the normal loop's end-of-frame halt, and the headless steady-state `for(;;)` park. Each held a bare `sti; hlt` before this section.
  - NOT the input-batching halt in the same file, and the exclusion is deliberate rather than an oversight: it is a bounded wait to let input accumulate (up to `max_batch` iterations per frame, 12 under TCG), so it is not time the CPU has nothing to do, and routing it would pay a predicate sample plus two `rdtsc` reads per iteration on a per-frame path. The accounting contract is therefore "terminal idle halts", not "every `hlt` in the tree", and `idle_tsc_cycles` must be read that way.
  - `src/kernel/smp/smp.c` -- the AP park loop, `sti; for(;;) hlt` before this section and `sti; for(;;) pm_idle_c1()` after it.
  - NOT the five FATAL parks in that same file (`cli; hlt` at `smp.c:149`, `:233`, `:250`, `:296`, `:318` -- GS self-pointer mismatch, impostor LAPIC id, panic-safe-id publish failure, and two bringup-abandonment paths). Stated because silence reads as an oversight: those are unrecoverable dead-CPU parks, not idle waits. They halt with interrupts MASKED and never wake, so `pm_idle_c1()` would refuse them anyway, and a CPU that failed its own identity check must not be running the predicate or touching per-CPU state. Found by the `review-evidence-mapper` sweep of every `hlt` in the two wired files, 2026-09-03.
  - `find_next_task()` returns the CURRENT thread when nothing else is runnable (`src/kernel/sched/task.c:317-320`, `:358-361`), and both scheduling paths then return the current interrupt frame, so there is no "nothing runnable" call site to hook and an infinite idle loop must never be called from `schedule()`.
- [x] Commit: `"kernel/pm: race-safe C1 idle entry, per-CPU idle cycle accounting"`

**Test checkpoint:** `pm_idle_c1()` STILL HALTS when the predicate is false and returns 0 (the never-spin property -- a refusal there would peg an AP forever); it returns 1 when idle; it returns 0 without halting only for an interrupts-masked caller; `pm_deep_idle_allowed()` is false when `PowerIdleEnable` is 0; `idle_tsc_cycles` advances across every halt and never on a backwards TSC sample; the caller's IF is restored on every exit. `bash scripts/test.sh SUITE=boot` green; `tail -1 build/build.log` is `=== BUILD OK ===`; `make KERNEL_TESTS=off` links, because the test build cannot catch a declaration that only the release build misses. Test on: QEMU TCG + KVM, 1 and 2 CPUs.

> **Verified:** 2026-09-04 | 6/6 items ([/] x1 parked: the AP rendezvous; the accounting-precision and S1-entry residues moved to the OPEN sections that own them, §25 and §3, rather than being parked into a section this commit closes) | build OK | 32923 kernel + 17 user-mode tests, 0 failures | sched 522, ipc 379 | smoke matrix 4/4 (TCG+KVM x 1+2 CPU) | `make KERNEL_TESTS=off` links (the release-only defect round 1 caught)
> **Deferred:** [M] the accounting is an UPPER BOUND, not halted time: the instruction after `hlt` retires only once the waking ISR has returned, and on the BSP the scheduler may switch away first. Safe to ship only because nothing consumes it yet -> XREF: 02-kernel-core/TODO-26 §25 (item: "`SYSTEM_PROCESSOR_IDLE_INFORMATION` -- one fixed-size entry per processor")
> **Deferred:** [H] S1 entry from any idle path, and `acpi_enter_s1()` itself: S1 is a system sleep transition needing the orchestration §3 owns, and a wrapper today would be dead code (`acpi_enter_sleep_state()` is BSP-only and single-flight) -> XREF: 02-kernel-core/TODO-26 §3 (item: "`pm_enter_s3()` -- runs on a dedicated `PASSIVE_LEVEL` worker, NOT `DISPATCH_LEVEL`")
> **Deferred:** [H] AP quiesce before any SLP_EN write: an online-mask popcount is NOT a substitute (`smp_retract_cpu_online()` clears the bit before the CPU stops). The PRIMITIVE shipped 2026-09-04 as §26; the INTEGRATION has not, so this stays deferred -- `acpi_enter_sleep_state()` still writes SLP_EN with no barrier around it -> XREF: 02-kernel-core/TODO-26 §27 (item: "Bracket every SLP_EN write with the §26 rendezvous at a deadlock-safe point")
> **Accepted:** [M] `pm_deep_idle_allowed()` is ADVISORY: a remote `dpc_insert_core()` can enqueue immediately after the sample, so it may not gate deeper idle until an interlocked idle-entry protocol and DPC wake IPI exist (reason: harmless while the predicate never gates the halt) -> XREF: 02-kernel-core/TODO-26 §16 (item: "BLOCKING PRECONDITION: `pm_deep_idle_allowed()` (§2) is ADVISORY")
> **Accepted:** [M] `test_dpc_insert_remove()`'s exact-delta depth assertions are not interference-proof; this section made the reads well-defined and changed no assertion (reason: DPC test-surface design belongs to its owner). RESOLVED by the owner 2026-09-04: the deltas were replaced with a single-lock `dpc_sample_queue()` snapshot -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §19 (item: "Gave the depth assertions an observation that cannot be perturbed by unrelated queue traffic: the SEAM, not a private queue")
> **Quality reviewed:** 2026-09-04 | Codex 11 rounds (design, adversarial x6, consistency x5, perf x2) + kernel-explorer + review-evidence-mapper | 6H+14M+2L fixed, 1 rejected, 6 accepted/deferred | scope: kernel-code-quality gates, SMP, release-vs-test build parity
> **Notes:**
> - **What shipped:** `pm_idle_c1()`/`pm_deep_idle_allowed()`/`pm_idle_cycles()`/`pm_idle_delta()`/`pm_idle_init()` (`src/kernel/pm_idle.c`, `include/kernel/pm.h`), a tail-appended `per_cpu_data.idle_tsc_cycles`, and the `PowerIdleEnable` policy cache. Wired at both BSP terminal idle points in `compositor.c` and the AP park loop in `smp.c`. 8 tests in `test_pm_idle.c`.
> - **The design was rewritten before any code:** the section specified `acpi_enter_sleep_state(1)` in the idle loop. That is unimplementable here -- the function refuses every caller off the BSP (`acpi.c:1613`) and confirms its wake through `WAK_STS`, which a timer tick never sets -- and wrong regardless, since ACPI 6.5 section 8.1 makes C1-Cn the processor idle states and S1 a whole-machine transition. §16's own scope boundary already said §2 provides the basic idle path.
> - **The load-bearing correction:** the first implementation let a false predicate REFUSE the halt. An idle AP has no DPC drain trigger (`ktimer.c:248-251`), so a stranded DPC would have pinned the predicate false forever and spun a whole logical CPU. `pm_idle_c1()` therefore always halts and only REPORTS readiness.
> - **How it integrates:** `pm_idle_init()` runs in `boot_phase2()` after registry population; APs park in `pm_idle_c1()` long before that, which is why the policy cache is release/acquire published and an unread cache reads as enabled -- exactly the unconditional `hlt` every site did before.
> - **Scope boundary:** the C1 primitive only. §16 replaces the fixed C1 choice with a governor, §25 exposes the counter through `NtQuerySystemInformation`, and S1 as a system-sleep operation stays with §3.
> - **Four halt oracles were tried before one held:** elapsed TSC cycles (passes with the `hlt` deleted), `per_cpu_data.irq_count` (a dead field -- zeroed in four places, incremented nowhere), an advancing tick counter (non-causal both ways), and a 512-byte opcode SEARCH (runs past a 0x97-byte function). What shipped is a global label emitted at the halt site plus a per-CPU reachability counter, so removing the halt is a LINK error and skipping it fails a test.

- [/] Quiesce the APs before any SLP_EN write. The PRIMITIVE shipped as §26; the INTEGRATION has not, so the hazard stands
  - `acpi_enter_sleep_state()` disables interrupts on the CALLING CPU only, so every other processor keeps taking interrupts and driving devices across the transition.
  - `include/kernel/smp.h` has only the test-only `smp_test_park_cpu`. -> XREF: `01-boot-platform/TODO-10-bare-metal-hardening.md` (live CPU online lifecycle / park)
  - The function's own refusal block cites "no APs are stopped" as a blocker for S3/S4, and S1 falls straight through it carrying the identical deficiency.
  - Harmless today only because no production caller exists. Found by `kernel-quality-auditor` during the section-1 review, 2026-09-03.
  - A live online-mask popcount is NOT a substitute, and the 2026-09-03 design review rejected exactly that proposal: `smp_retract_cpu_online()` clears the mask bit BEFORE the retiring CPU has stopped executing, so a popcount of 1 never proves the other processors are parked. What is owed is a generation-tagged stop-the-world rendezvous that blocks CPU admission, collects an acknowledgement from every target CPU, and holds them in a RESUMABLE barrier across the `SLP_EN` write.
  - OWNER ASSIGNED 2026-09-04: the rendezvous is now §26, split out of §3 because it was the only part of the S3 orchestration not blocked on an absent prerequisite. §26 SHIPPED the same day.
  - STILL OPEN, and the distinction matters: `acpi_enter_sleep_state()` writes PM1 SLP_EN directly (`src/kernel/acpi.c`) and has NO call to the barrier -- the only caller in the tree is §26's own live test. So an SMP S1 transition still runs with other processors live, exactly as before. Marking this `[x]` on the strength of the primitive alone was false completeness, corrected by the 2026-09-04 consistency review. -> XREF: `02-kernel-core/TODO-26` §27 (item: "Bracket every SLP_EN write with the §26 rendezvous at a deadlock-safe point")

## 3. S3: Suspend to RAM

> **Deferred:** the S3 orchestration cannot be entered on this tree, and the three missing prerequisites are all owned elsewhere. (1) The resumable stop-the-world CPU rendezvous every step past the `cli` depends on was SPLIT OUT of this section and SHIPPED the same day as §26, so that prerequisite is met and is no longer what blocks this section. (2) ACPI namespace evaluation does not run: ACPICA is vendored and linked, but `AcpiInitializeSubsystem()` / `AcpiLoadTables()` / `AcpiEnableSubsystem()` have ZERO call sites anywhere outside `src/kernel/acpica/` (verified 2026-09-04 by grep over `src/` + `include/`), so `AcpiGbl_FACS` is never populated, `FACS->FirmwareWakingVector` is unreachable, and `_PTS`/`_GTS`/`_WAK` cannot be evaluated. (3) The driver power callbacks steps 1 and 7 broadcast to are §9 and unimplemented: `pm_notify_resume` and `PO_CB_SYSTEM_STATE_LOCK` return 0 matches across `src/`. The S3 refusal at `src/kernel/acpi.c:1573-1578` therefore stays, and its comment ("no APs are stopped, no devices are quiesced, no caches are flushed, no firmware waking vector is installed") remains an accurate description of this tree. -> XREF: `02-kernel-core/TODO-26` §26 (item: "`smp_rendezvous_begin(uint32_t timeout_ms)` in `src/kernel/smp/smp.c`"), `04-drivers-hardware/TODO-03-acpi-power-management.md` §1 (item: "`acpi_evaluate(path, args, result)` wrapper around `AcpiEvaluateObject`"), `02-kernel-core/TODO-26` §9 (item: "`pm_notify_resume()`")

- [/] `pm_enter_s3()` -- runs on a dedicated `PASSIVE_LEVEL` worker, NOT `DISPATCH_LEVEL` (steps 1+3 block; DISPATCH_LEVEL forbids blocking/paging/mutex per the IRQL contract) -> XREF: `TODO-07-irql-model-dpcs.md` §7:
  1. Broadcast `PO_CB_SYSTEM_STATE_LOCK` to all registered power callbacks (§9): let drivers flush queues and reach D3hot/D3cold (§8)
  2. Flush VFS page cache and IXFS journal FIRST, while locks may still be taken (→ XREF: `05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md §1`)
  3. Freeze the scheduler so no new threads start
  4. `smp_rendezvous_begin()` REPLACES the bare `cli` here: it masks interrupts AND parks every other CPU, and from this line nothing may take a lock, allocate, log, run a callback, or perform a synchronous cross-CPU operation (§26's deadlock contract). The flush is at step 2 and not here for exactly that reason -- a journal flush after the barrier deadlocks against a lock a parked AP is holding, which is what the original ordering of this list would have produced
  5. Save APIC state (LVT registers, LAPIC base MSR) to a per-CPU save area
  6. Save `IDTR`, `GDTR`, `CR0`, `CR3`, `CR4`, `EFER` per CPU
  7. Save all CPU general-purpose and SSE registers for the BSP (`struct s3_cpu_state` allocated in pinned physical memory)
  8. Write the physical address of `pm_s3_wakeup_entry` into the ACPI wakeup vector (`FACS->FirmwareWakingVector`)
  9. Call `acpi_enter_sleep_state(3)` -- system loses power to RAM row refresh; wake on power button / RTC alarm resumes in §3
  - BLOCKED on §9's callback registry only. The AP quiesce this step needs SHIPPED 2026-09-04 as §26 (`smp_rendezvous_begin`/`smp_rendezvous_end`), so step 2 is now buildable and step 1 is not. -> XREF: `02-kernel-core/TODO-26` §9 (item: "`pm_notify_resume()`")
- [/] `pm_s3_wakeup_entry` (real-mode compatible entry stub in `src/kernel/acpi_wakeup.asm`):
  - BIOS/UEFI firmware jumps here in real mode; stub switches to protected and then long mode (re-using the bootloader's page tables at `0x70000`)
  - Calls `pm_s3_resume()` in C with the saved state pointer
  - BLOCKED: there is nothing to resume until `pm_enter_s3()` saves state, and the vector cannot be published because the FACS is never located (no kernel-native FACS mapper in `src/kernel/acpi.c`, and ACPICA's namespace is never loaded). -> XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md` §1
- [/] `pm_s3_resume()`:
  1. Restore IDTR, GDTR, CR0, CR3, CR4, EFER, APIC from save area
  2. Re-initialise IOAPIC routing (MADT-based)
  3. Restore BSP general-purpose + SSE registers
  4. Wake AP CPUs: write `INIT`->`SIPI`->`SIPI` IPI sequence; each AP restores its own saved state and un-parks from the spin barrier
  5. Recalibrate TSC (→ XREF: `TODO-08-time-filetime-management.md §3`) -- PM timer used as reference
  6. Compute sleep duration from RTC/UEFI time delta; call `ke_suspend_bias_update()` (→ XREF: `TODO-08-time-filetime-management.md §14`)
  7. Call `pm_notify_resume()` (§9) -- drivers transition back D3->D0
  8. Unfreeze scheduler; resume from the instruction after `acpi_enter_sleep_state(3)`
  - BLOCKED on §9 (step 7) and on AP re-init after power loss, which is new code: the only INIT/SIPI/SIPI sequence in the tree is the one-shot boot loop inside `smp_init()`. Step 4's unpark is NO LONGER a blocker -- §26 shipped the barrier's release path. -> XREF: `02-kernel-core/TODO-26` §9 (item: "`pm_notify_resume()`")
- [/] Call `uefi_secureboot_refresh()` after runtime services come back online and before user threads unblock, i.e. after `pm_notify_resume()` has returned storage and registry to D0
  - An attacker with physical access can clear SetupMode and re-add Secure Boot keys while the machine sleeps, so the boot-time snapshot is stale the moment S3 returns.
  - The refresh re-reads SecureBoot / SetupMode / AuditMode / DeployedMode / PK / KEK and, on mismatch with the boot snapshot, emits `LOG_FATAL` and sets `HKLM\SYSTEM\SecureBoot\Drift = 1`.
  - Filed 2026-05-01 from [`01-boot-platform/TODO-02-uefi-hardening-secureboot.md §5`](../01-boot-platform/TODO-02-uefi-hardening-secureboot.md#5-secure-boot-state-detection).
  - BLOCKED: the function already exists (`src/kernel/uefi_runtime.c:1647`); what is missing is the resume path to call it from. Unblocks with `pm_s3_resume()` above, not separately.
- [/] Expose S1 as an explicit system-sleep operation once this section's orchestration exists, and NOT before
  - S1 needs the same machinery S3 does, minus the state save: callbacks broadcast, scheduler frozen, APs quiesced, devices to D3. `acpi_enter_sleep_state(1)` already performs the PM1 write and confirms the wake through `WAK_STS`; what is missing is everything around it.
  - It is deliberately NOT reachable from the idle path. §2 ships C1 (`HLT`) as the processor idle state and documents why a PM1 `SLP_EN` write can never be a deeper `HLT` -> XREF: `02-kernel-core/TODO-26` §2 (item: "`pm_idle_c1()` in `src/kernel/pm_idle.c`").
  - Blocked on the same AP rendezvous §2 parks against: entering any sleep state with other processors live behind a local `cli` is the [H] hazard §1 recorded.
  - UNBLOCKED 2026-09-04: §26 shipped the rendezvous this item was waiting on, so S1 now needs only the callbacks-and-devices half of the orchestration above. -> XREF: `02-kernel-core/TODO-26` §9 (item: "`pm_notify_resume()`")
- [/] Power button physical press -> PM1 fixed event (§1) generates SCI; firmware raises the CPU from S3
  - BLOCKED: the PM1 fixed-event SCI path already ships (§1, `acpi_sci_process()` at `src/kernel/acpi.c:1354`); what is missing is an S3 to be woken FROM. Unblocks with `pm_enter_s3()`.
- [/] RTC alarm: `acpi_set_wakeup_alarm(seconds)` -- programs CMOS RTC alarm registers (port 0x70/0x71), sets `RTC_EN` in PM1a_EN; used for timed wake (-> `Task Scheduler` integration, future)
  - BLOCKED only on having a consumer: the CMOS port I/O itself depends on nothing absent, but arming a wake alarm with no sleep path to wake from is untestable and would ship dead code. Unblocks with `pm_enter_s3()`.
- [/] USB device activity: `XHCI_S3_WAKEUP_EN` -- xHCI remote-wakeup enable bit in the USB port status register (→ XREF: `04-drivers-hardware/TODO-10-usb-stack.md`)
  - BLOCKED: no xHCI driver exists in the tree. -> XREF: `04-drivers-hardware/TODO-10-usb-stack.md`
- [/] Evaluate `_PTS(3)`/`_GTS` before the PM1 sleep write and `_WAK(3)` on resume
  - Linux does this in `drivers/acpi/sleep.c` (`acpi_pm_prepare` / `acpi_pm_finish`); Windows evaluates them from `ACPI.sys`. `_GTS` is deprecated but still evaluated by both.
  - Skipping them is a well-known cause of laptops that appear to enter S3 but never power peripherals down, or that hang or corrupt state on wake.
  - Needs the AML evaluator. -> XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md` §1 (item: "`acpi_evaluate(path, args, result)` wrapper around `AcpiEvaluateObject`")
  - BLOCKED: confirmed 2026-09-04 that `AcpiEvaluateObject` is compiled and linked (`src/kernel/acpica/components/namespace/nsxfeval.c:325`) but unreachable, because nothing initializes the ACPICA subsystem or loads the namespace. -> XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md` §1
- [/] Flush the CPU caches before the SLP_EN write for any state below S4
  - ACPI 6.5; ACPICA does it in `hwsleep.c` via `ACPI_FLUSH_CPU_CACHE`.
  - Invisible under every emulator -- the same cross-boot durability class as the bare-metal gotchas doc.
  - Becomes a precondition the moment the S3 refusal in `acpi_enter_sleep_state()` is lifted.
  - BLOCKED only on the S3 entry path: `cache_writeback_range()` and `wbinvd` already exist (`src/kernel/cache.c:150-153`), so this is one call in a sequence that does not yet exist. Unblocks with `pm_enter_s3()`.
- [/] Commit: `"kernel/acpi: S3 suspend-to-RAM, wakeup vector, CPU state save/restore, AP re-init"`
  - BLOCKED: nothing to commit while every deliverable above is parked.

**Test checkpoint:** `struct s3_cpu_state` saves/restores all GPRs + CR0/CR3/CR4/EFER. `FACS->FirmwareWakingVector` set to `pm_s3_wakeup_entry` physical address. AP re-init SIPI sequence completes. TSC recalibrated after wake. `ke_suspend_bias_update()` adjusts `InterruptTimeBias`. Test on: QEMU TCG (`-machine q35,acpi=on`).

---

## 4. S4: Hibernation Image Format and Compression Codec

> **Spawned-by:** root

> [!NOTE] Ownership boundary (TODO-26 gap-audit 2026-06-17; SPLIT 2026-09-04): the bootloader (`01-boot-platform/TODO-26`) owns hibernation image DISCOVERY, eligibility policy (Secure Boot/db/topology/slot invalidation), anti-replay validation, integrity checking, and the boot_info handoff. This section owns the authoritative on-disk FORMAT and the pure codec that produces and verifies it. The S4 write path, AEAD encryption with a TPM-sealed key, and the kernel resume consumer were split out to §28 because each is blocked on a prerequisite owned elsewhere; keeping them here would have held the codec behind ACPI namespace bring-up. -> XREF: `02-kernel-core/TODO-26` §28, `01-boot-platform/TODO-26 §1,§2,§4,§5`.

- [x] Hibernation image header in `include/kernel/pm/hibernate.h`: `hiber_header_t`, exactly 4 KiB, `_Static_assert` on `sizeof` and on all 19 field offsets (a cross-binary on-disk ABI, same discipline as `boot_info`)
  - Fields: magic, format_version, header_bytes, image_bytes, page_count, chunk_count, resume_type, boot_info_version, flags, root_volume_id, resume_generation, `kernel_id[32]`, aead_cipher_id, aead_key_id, `aead_nonce[12]`, `aead_tag[16]`, payload_crc32c, header_crc32c, cpu_count_present, total_ram_pages, reserved. Supersedes the legacy `HIBR_HEADER` sketch, which had no version, no volume identity and no encryption metadata. -> XREF: `01-boot-platform/TODO-26` §1 (item: "Define header with magic, version, kernel build id, boot_info ABI version, root volume id, image size, checksum, flags.")
  - `cpu_count_present` and `total_ram_pages` came from the post-ship parity review: Windows treats a resume onto FEWER CPUs as its own bugcheck class (`0xBD INVALID_HIBERNATED_STATE`, Param1 = 1) rather than as a wrong-image failure, and Linux's `swsusp_info` carries `num_physpages` for a cheap up-front sanity gate. Neither was expressible. The FIELD is this section's (it owns the format); the check against live state is §28's. -> XREF: `02-kernel-core/TODO-26` §28
  - Adding them moved the reserved region from 140 to 152, so `HIBER_FORMAT_VERSION` went to **2**. The first attempt kept version 1 on the reasoning that no image had ever been persisted; that was wrong, because the v1 layout was already pushed and anything built from it produces real v1 images. Two mutually rejecting streams both calling themselves v1 turns a clean version mismatch into two different malformed-image errors, which is exactly what the field exists to prevent.
  - Under `HIBER_CIPHER_NONE` the whole AEAD block must be zero, refused as `HIBER_ERR_RESERVED` otherwise. Without it 32 CRC-covered bytes sat outside the reserved-must-be-zero rule the format states, and a plaintext image could arrive already carrying a key id, nonce and tag for a later cipher-aware reader to be tempted by.
  - `kernel_id` is a 32-byte OPAQUE identity compared byte for byte, not a build number plus a short commit hash: the design review measured the latter at roughly 32 bits (`Makefile:197-198`), which two different kernel binaries can collide on. Producing a strong value is §28's. -> XREF: `02-kernel-core/TODO-26` §28
  - The AEAD fields are part of the ABI and defined here; the codec writes `HIBER_CIPHER_NONE` so plaintext is a stated state rather than an omission.
- [x] Canonical byte stream pinned in the header's normative comment and enforced by the decoder, not left to the struct layout alone
  - Little-endian fixed-width fields; payload starting at exactly 4096; `image_bytes` counting the header; reserved bytes zero; `header_crc32c` over all 4096 bytes with its own field read as zero; unknown flag bits REJECTED; no trailing data.
  - Pinned offsets alone do not define a byte stream: two mirrors can each satisfy their own `_Static_assert`s and still parse different bytes. This is the contract that makes a future UEFI mirror checkable.
- [x] Chunk framing: `hiber_chunk_desc_t` (24 bytes) inline before each chunk, carrying `start_pfn`, `page_count` (1..64), `uncompressed_len`, `stored_len` and flags; a chunk that does not shrink is stored verbatim
  - `start_pfn` makes the chunk sequence itself the DESTINATION MAP. A hibernation image restores sparse PMM-used pages, and lengths alone cannot say which frame a chunk belongs to; extents are strictly ascending, non-overlapping, and bounded by `HIBER_MAX_PFN` INCLUSIVE (that frame's page is fully addressable) so a PFN always converts to a byte address without wrapping. Both sides compare the LAST INCLUDED frame, because a bound the header calls inclusive and the code treats as exclusive is exactly the divergence a separately compiled mirror would inherit.
  - Compression is the codec already vendored in the tree: `lz4_compress()` / `lz4_decompress()` / `lz4_compress_bound()` (`include/libs/lz4.h`). The compressor is capped one byte below its input, so "did not shrink" is a refusal from LZ4 itself and an incompressible image never grows.
- [x] Streaming encoder in `src/kernel/pm/hibernate_image.c`: `hibernate_image_begin()`, `hibernate_image_append_chunk()`, `hibernate_image_finalize()`, plus `hibernate_image_encoded_bound()` for output sizing
  - Payload CRC-32C accumulates across descriptors AND data with `kcrc32c_cont()` (`include/kernel/kchecksum.h`), so a tampered destination PFN is a CRC failure and not merely an ordering failure.
  - No allocation and no scratch buffer: the encoder compresses straight into the caller's image buffer and falls back to a verbatim copy in place. Nothing is global, so two CPUs may encode two images at once.
  - Both `begin` functions zero the caller's state struct FIRST, before any validation can return, so a caller that ignores a failed `begin` gets `HIBER_ERR_STATE` from every later call instead of operating on stack garbage. The encoder's first write is through `enc->out`, which made this a wild-write path rather than a wild-read one.
- [x] Decoder half: `hibernate_image_header_validate()`, `hibernate_image_decode_begin()`, `hibernate_image_read_chunk()` and the mandatory `hibernate_image_decode_finish()` completion gate
  - `header_validate` returns a POINTER into the image rather than copying 4 KiB onto an 8 KiB kernel stack, and runs the header CRC before trusting any other field.
  - `read_chunk` bounds-checks every descriptor against the remaining image, rejects unknown flags, requires `uncompressed_len == page_count * 4096`, and hands LZ4 the DECLARED output length as its capacity rather than the caller's buffer size, so a forged one-page descriptor carrying a multi-page block is refused before the extra bytes are written.
  - `decode_finish` proves the declared chunk count, page count and encoded length were each exhausted exactly and that the payload CRC matches. Decoded bytes are documented PROVISIONAL until it returns `HIBER_OK`: without it a caller can decode every individually valid chunk and never establish whole-image integrity.
- [x] Kernel-identity guard `hibernate_image_kernel_matches()` with distinct reason codes so a caller discards the image and cold-boots
  - `HIBER_IDENT_KERNEL_ARTIFACT_MISMATCH`, `HIBER_IDENT_BOOT_INFO_ABI_MISMATCH`, `HIBER_IDENT_FORMAT_VERSION_MISMATCH`, `HIBER_IDENT_RESUME_TYPE_MISMATCH` and `HIBER_IDENT_CPU_TOPOLOGY_MISMATCH` stay separate: a different kernel binary, a changed handoff ABI and a machine that lost a CPU are different operator-facing failures. A NULL argument refuses rather than defaulting to a match.
  - The topology check is an INEQUALITY, not an equality: fewer CPUs than the image was captured on is fatal because the image carries per-CPU state for processors that no longer exist, while more is harmless.
  - The header states which fields the guard deliberately does NOT compare and who owns each -- `root_volume_id` (bootloader eligibility), `resume_generation` (bootloader anti-replay against a TPM-NV counter) and `total_ram_pages` (needs the live memory map). A caller filling in every field would otherwise reasonably assume every field is checked.
- [x] The codec is PURE -- no disk IO, no ACPI, no scheduler interaction -- which is what makes it fully testable on this tree and what unblocks the bootloader-side parser
  - The bootloader's metadata-format section is deferred waiting for a writer to define the format ("gated on the kernel hibernation WRITER"), so shipping this half is what breaks that deadlock. -> XREF: `01-boot-platform/TODO-26` §1
  - The encoder is deliberately NOT wired to any disk sink: an image on disk is confidential kernel memory, and AEAD encryption is §28's. -> XREF: `02-kernel-core/TODO-26` §28 (item: "AEAD-encrypt the image (AES-GCM) with a TPM-sealed key")
- [x] Commit: `"kernel/pm: S4 hibernation image format + LZ4 chunk codec"`

**Test checkpoint:** `sizeof(hiber_header_t)` is 4096 and all 19 field offsets assert at compile time. The ENCODED bytes carry magic at 0, format_version at 8, image_bytes at 16, page_count at 24, chunk_count at 32. A compressible chunk round-trips smaller than its raw pages; an incompressible chunk takes the stored-verbatim path and encodes to exactly one descriptor plus its pages. A flipped payload byte passes per-chunk decode and is caught only by the completion gate, as are an overstated page count and unconsumed trailing bytes. A truncated header, a bad magic, a flipped header byte, a non-zero reserved byte and an unknown flag are each refused with their own code; an unknown resume type and an unknown cipher id both refuse as `HIBER_ERR_FORMAT`, which is one class on purpose (the header names a value this build does not support) and is what the tests assert. A forged descriptor that over-expands, decodes short, or names a PFN past the addressable ceiling is refused, and nothing is written past the declared output. Sparse extents keep their destination PFNs; descending and overlapping extents are refused. A kernel id differing by one bit refuses the image, and resuming onto FEWER CPUs than were captured refuses while more is accepted. A full 64-page (256 KiB) chunk round-trips, so the production chunk size is exercised and not merely defined. Under `HIBER_CIPHER_NONE` a non-zero key id, nonce or tag is refused, a superseded version-1 header is refused by version, and a failed `begin` leaves state that refuses every later call. A buffer LARGER than the image decodes normally and its undeclared suffix is ignored, because `img_len` is a capacity and `image_bytes` is what says where the image ends. `bash scripts/test.sh SUITE=boot` green; `tail -1 build/build.log` is `=== BUILD OK ===`.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | `src/kernel/test/test_hibernate_image.c`, 22 suites, 0 failures
> **Notes:**
> - Shipped `include/kernel/pm/hibernate.h` (format v2, 4 KiB header, 29 `_Static_assert`s) and `src/kernel/pm/hibernate_image.c` (pure streaming encoder + stateful decoder, LZ4 per 64-page chunk, CRC-32C, identity guard).
> - Integrates by reusing what the tree already had: `lz4_compress`/`lz4_decompress` from the vendored codec and `kcrc32c_cont` from the kernel checksum layer. Nothing global, nothing allocated, no disk or ACPI contact.
> - Unblocks the bootloader-side metadata-format section, which was deferred waiting for a writer to define the format; its Deferred stamps now name §28 as the writer.
> - The destination map IS the chunk sequence: each descriptor carries `start_pfn`, extents are ascending, non-overlapping and bounded by an inclusive `HIBER_MAX_PFN`.
> - Scope boundary: this section owns the FORMAT and the CODEC only. The disk write path, AEAD encryption with a TPM-sealed key, and the resume consumer are §28 and blocked.
> **Verified:** 2026-09-04 | commit `967b974e6` + review fixes | 8/8 items | build OK | 33303 kernel + 17 user-mode tests pass, smoke matrix 4/4 legs (kvm 1+2 cpu, tcg 1+2 cpu)
> **Deferred:** [H] the encoder can write a near-incompressible chunk twice, because LZ4 emits sequences before it discovers the output budget is exhausted (reason: the workspace alternative removes that but adds a copy to every SUCCESSFUL chunk, and only a real page mix can say which wins) -> XREF: `02-kernel-core/TODO-26` §28 (item: "Benchmark the encoder's compress-in-place trade-off against a workspace variant on a real page mix, and adopt whichever wins" at line 1137)
> **Quality reviewed:** 2026-09-04 | Codex 12x (design, test-coverage, adversarial, consistency, perf, re-adversarial) | 1C+6H+10M+7L fixed, 1 deferred | scope: kernel-code-quality + kernel-quality-auditor + parity-research-analyst

## 5. ACPI Embedded Controller (EC) Driver
- [x] The Embedded Controller is mandatory on all laptops; it mediates battery, thermal, lid, and hotkey state behind two I/O ports
  - `EC_SC` (status/command) and `EC_DATA`, both taken from the ACPI `ECDT` via `acpi_get_raw_table()`. The `\_SB.PCI0.LPCB.EC0` DSDT route is parked below: it needs an ACPI namespace this kernel does not build.
  - The TODO originally named `acpi_find_table("ECDT")`; no such symbol exists. `acpi_get_raw_table()` (`include/kernel/acpi.h:448`) is the exported lookup, and it is what `watchdog.c` and `tpm_transport.c` already use.
- [x] `include/kernel/drivers/acpi_ec.h` defines the register bits and command set, checked against ACPI 6.x sect. 12.2.1 and 12.3
  ```c
  #define EC_SC_OBF     0x01  /* Output Buffer Full (data ready for host) */
  #define EC_SC_IBF     0x02  /* Input Buffer Full (EC busy) */
  #define EC_SC_CMD     0x08  /* Last EC_DATA write was a command byte */
  #define EC_SC_BURST   0x10  /* Burst mode enabled */
  #define EC_SC_SCI_EVT 0x20  /* SCI event pending */
  #define EC_SC_SMI_EVT 0x40  /* SMI event pending */
  #define EC_CMD_READ   0x80  /* RD_EC */
  #define EC_CMD_WRITE  0x81  /* WR_EC */
  #define EC_CMD_BURST  0x82  /* BE_EC */
  #define EC_CMD_NBURST 0x83  /* BD_EC */
  #define EC_CMD_QUERY  0x84  /* QR_EC */
  #define EC_BURST_ACK  0x90  /* burst-entry acknowledge byte */
  ```
  - `EC_SC_CMD` is bit 3, not the 0x04 this TODO originally specified. Bit 2 is ignored by the spec, and a CMD at bit 2 would leave bit 3 unaccounted for with BURST at bit 4. A `_Static_assert` now pins the whole layout.
- [x] `acpi_ec_read(addr, *val)` -- polled RD_EC: wait IBF clear, write `EC_CMD_READ`, wait IBF clear, write the address, wait OBF set, read the byte
  - Every wait bounds itself on ELAPSED TIME from `mono_ns()` against `ACPI_EC_WAIT_TIMEOUT_NS` (100 ms), plus a clock-independent iteration ceiling, because a deadline is only a bound while the clock advances and `mono_ns()` reads 0 with no source. The TODO's "100 us x 1,000 retries" is not a spec value; ACPI chapter 12 states no host-side polling timeout at all, and it is recorded as an implementation choice.
  - Stale output is drained before every transaction. Firmware can hand over with a byte latched behind OBF, and without the flush the read's OBF wait is satisfied instantly by the previous owner's data.
- [x] `acpi_ec_write(addr, val)` -- polled WR_EC, same handshake, and it does not return until the EC has taken the data byte
- [x] Burst shipped as ONE transaction-wide operation, `acpi_ec_read_block(first, out, count)`, not as composable `enter`/`exit` primitives
  - Composable primitives cannot be made safe: releasing exclusion between enter and the reads lets another CPU into the burst window, and re-entering through the public read deadlocks on the mutex the sequence already holds.
  - It validates the `0x90` acknowledge byte rather than trusting the BURST bit, rechecks BURST before each byte because sect. 12.3.3 lets the EC leave burst at any time, and finishes the remainder unburst if it does.
  - An acknowledgement that TIMES OUT is treated differently from a wrong one: the wrong byte is a known state and the block completes unburst, but a timeout may still be in flight, so it fails closed and sends BD_EC unconditionally. Sampling BURST before deciding to clean up is the bug that leaves an EC bursting for the rest of the boot.
- [x] `acpi_ec_init()` -- Phase 2 (`src/kernel/main/boot_storage.c`), NOT inside `acpi_init()` as originally written
  - ECDT lookup, full field validation, firmware-quirk correction, then publication. It performs NO EC I/O at all.
  - The placement is load-bearing: `mono_ns()` returns 0 until `mono_clock_init()` runs in Phase 2, so discovering the EC in Phase 1 would give every handshake wait a meaningless deadline and let a wedged controller hang the boot.
  - Validation rejects rather than repairs: non-SystemIO address space, a register not 8 bits wide, a nonzero bit offset, an access width neither undefined nor byte, a zero or above-0xFFFF port, and a table naming one port for both registers.
- [x] `acpi_ec_ready()` gates every consumer; `acpi_ec_discovered()` separates "no EC on this machine" from "EC found but not drivable"; `acpi_ec_get_ports()` reports the validated pair
- [x] `FW_QUIRK_EC_ECDT_PORTS_SWAPPED` added to the firmware-quirk database, applied by `acpi_ec_apply_port_quirk()` before the ports are published
  - Some firmware publishes the ECDT with EC_CONTROL and EC_DATA transposed, so an OS trusting the table writes command bytes into the data register.
  - The matching predicate stays synthetic, following that table's stated convention that real-world predicates land "as known-bad firmware combinations are confirmed"; every existing entry is synthetic too. The real model string is parked below.
- [x] Firmware-ABI constants pinned by VALUE, not just by relation (review round: kernel-quality-auditor)
  - The offset asserts were all relative, so a uniform shift of the ECDT anchor left every assert and every test green while the parser read the wrong bytes; the anchor is now pinned to `sizeof(struct acpi_sdt_header)` and the GAS macros are tied to `struct acpi_gas` by `__builtin_offsetof`.
  - The old `OR == 0x7B` check on the EC_SC bits was permutation-invariant (transposing OBF and IBF passed it) and the unit suite is symbolic throughout, so a transposed pair would have passed Layer 1 and Layer 3 together and inverted every handshake on real hardware. Every bit and command value is now asserted individually.
  - `ECDT_MIN_LENGTH` was asserted `>= ECDT_OFF_DATA + ACPI_GAS_SIZE` (60) while the parser dereferences the GPE at 64: correct only by coincidence. Now `> ECDT_OFF_GPE`.
- [x] An ECDT overlapping the i8042 ports (0x60/0x64) is refused (review rounds 4-6)
  - `acpi_ec_ports_conflict_i8042(ports, i8042_present)` is pure and applied AFTER the port-swap quirk, so it sees final roles; ECDT parsing stays structural and platform-free.
  - Production fails closed (passes 1). Two authorities were tried and rejected with reasons recorded in the header: the FADT `IAPC_BOOT_ARCH.8042` bit is documented unreliable in this tree (`keyboard.c:304-307`, QEMU WHPX reports it clear for a working i8042), and `HW_REDUCED_ACPI` is a statement about fixed ACPI hardware rather than about port-60/64 ownership.
- [x] Both readiness blockers are independent predicates, so clearing one cannot open the gate alone (review round 1)
  - `ec_gpe_ack_supported()` and `ec_global_lock_satisfied()` are separate. Folding them would have meant §24 replacing the GPE predicate silently enabled unarbitrated EC traffic on every machine whose EC declares `_GLK`.
- [x] A block read is bounded as a WHOLE operation, by elapsed time and by a clock-independent probe allowance (review rounds 5-6)
  - Per-wait deadlines multiply: 256 bytes at three waits each permitted 76.8 seconds with the transaction mutex held. The time budget is checked at byte boundaries with the residual overshoot stated honestly in the header rather than claimed away.
  - A stalled clock defeats any elapsed-time bound, and the per-wait iteration ceiling bounds one wait rather than an operation built from hundreds, so a shared probe allowance covers burst entry and every byte, with a separate bounded reserve for the mandatory burst exit and abort.
- [x] Clock sampling is batched one per 16 status probes (review round: perf)
  - `mono_ns()` on a PMTMR source is a glitch-filtered three-port read, so sampling per iteration turned one status probe into four port transactions and paid three even on a wait satisfied immediately.
- [/] EC hardware access is GATED OFF. BLOCKED on GPE acknowledgement. -> XREF: `02-kernel-core/TODO-26-power-management.md` §24 (item: "Service GPEs in `acpi_sci_process()` alongside the PM1 fixed events")
  - `acpi_ec_ready()` returns 0 even on a machine with a valid ECDT, so every serialized entry point returns `ACPI_EC_UNAVAIL`.
  - This is the section's hardest constraint and it is not conservatism. RD_EC, WR_EC, BE_EC and BD_EC all raise the EC's GPE, not just the SCI_EVT event path, so a polled transaction on a machine whose firmware left that GPE enabled leaves a level-triggered SCI asserted with nothing able to clear it.
  - `irq.c:511-513` then quarantines the shared GSI after `IRQ_STORM_ALLNONE_LIMIT` all-NONE dispatches, killing the power button, the lid, and every ACPI event for the rest of that boot. Shipping the driver enabled would have provoked that at boot on every laptop.
  - `ec_gpe_ack_supported()` in `src/kernel/drivers/acpi_ec.c` is the single named predicate §24 replaces to turn the driver on.
- [/] Interrupt-driven query path. BLOCKED on the same GPE work. -> XREF: `02-kernel-core/TODO-26-power-management.md` §24 (item: "Acknowledge a GPE that has no handler yet rather than leaving the line asserted")
  - Shape: SCI arrives with `SCI_EVT` set -> issue `EC_CMD_QUERY` -> dispatch the returned query byte to a handler registered by number.
  - Draining a query inside `acpi_sci_process()` is separately wrong: that function is documented hard-IRQ context (`src/kernel/acpi.c:1341-1349`) and a QR_EC transaction polls for milliseconds. It also returns early at `:1361` and `:1370` when no PM1 fixed event is asserted, which is exactly the EC-only case, so a hook appended there is unreachable.
- [/] Recovery from a desynchronized controller. BLOCKED: needs a controller-reset boundary. -> XREF: `02-kernel-core/TODO-26-power-management.md` §24 (item: "`acpi_gpe_enable(n)` / `acpi_gpe_disable(n)` so a driver arms only the events it services")
  - A transaction failing after its command byte is issued sets a TERMINAL desync flag and every later transaction is refused, because proving the EC idle at an instant cannot prove a late response will not arrive.
  - `acpi_ec_quiesce_io()` is the bounded idle proof that boundary will build on; it deliberately does not clear the flag and is not called from the transaction path.
- [/] DSDT `_HID "PNP0C09"` / `_CRS` discovery fallback, for machines with no ECDT. BLOCKED on an evaluable ACPI namespace. -> XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md` §1 (item: "`acpi_evaluate(path, args, result)`")
  - This kernel does not build a namespace: the vendored ACPICA is not wired into the boot path.
  - Doing this work also ends the single-controller assumption, so it carries a refactor: ECDT-only discovery can describe exactly one EC, which is why the file-scope `g_ports`/`g_state`/`g_ec_lock` singleton is correct today, but a namespace walk can find several and needs them moved into a per-instance struct. Linux distinguishes a boot EC from namespace-discovered ones for exactly this reason.
- [/] Confirmed per-model predicate for `FW_QUIRK_EC_ECDT_PORTS_SWAPPED`. `operator-gated`: confirming a model string needs the affected hardware.
  - The mechanism ships and is tested; only the SMBIOS match is missing, so the quirk is currently inert. Linux carries an exact Micro-Star match in its EC driver.
  - A guessed predicate either never matches or transposes the ports on an innocent machine, which is why this waits for hardware rather than being filled in speculatively.
- [x] Commit: `"kernel/acpi: Embedded Controller driver, ECDT discovery, burst-mode EC transactions"`

**Test checkpoint:** `bash scripts/test.sh SUITE=boot` reports every `ACPI EC:` case PASS. The suite drives the transaction engine against a simulated controller, so it covers what a real EC cannot be made to do on demand: malformed ECDT fields, a stall at each individual handshake phase, an EC that never answers, a stale output byte, a controller that re-arms OBF forever, a frozen and a backward clock, a refused burst, a burst dropped mid-sequence, a burst that will not clear, and an acknowledgement arriving after its deadline. On a machine with no ECDT, or any machine at all while the GPE gate is closed, the serialized API returns `ACPI_EC_UNAVAIL`. Test on: QEMU TCG/KVM (no ECDT, structural coverage), bare metal (the only place a live round trip will happen, once §24 opens the gate).

> **Test runner:** `bash scripts/test.sh SUITE=boot` -- 41 `ACPI EC:` cases, all PASS as part of the full kernel suite.

> **Notes:**
> - Shipped the POLLED half of the EC interface in `src/kernel/drivers/acpi_ec.c`: ECDT discovery and validation, RD_EC/WR_EC transactions, a burst-wrapped block read, a firmware-quirk port correction, and Phase 2 init.
> - Hardware access is deliberately gated off until GPE acknowledgement exists, because polled commands raise the EC's GPE and an unacknowledged level-triggered SCI gets the shared GSI quarantined (`irq.c:511-513`).
> - Transactions serialize on a mutex, never a spinlock: `src/kernel/sched/spinlock.c:93-95` documents a ~100 ns hold rule with interrupts disabled, and one EC transaction can poll for milliseconds.
> - The engine takes its port I/O, clock, and cross-call state as explicit parameters rather than calling hardware directly, which is what makes every timeout, stale-output and burst-unwind path reachable from CI instead of only from a laptop.
> - A post-command failure is terminal rather than auto-recovering: an idle observation cannot prove a late response will not arrive, so recovery would move the cross-delivery window rather than close it.
> - This section ships NO live capability: `acpi_ec_ready()` is 0 on every machine, so nothing can observe an EC byte until `§24` opens the gate. `§6` battery and `§14` thermal additionally need an AML EC operation-region handler, filed in `04-drivers-hardware/TODO-03` §1.

> **Verified:** 2026-09-04 | commit `4cbcc16ef` + review fixes | 14/22 items (8 parked with named blockers) | build OK | 33478 kernel + 17 user tests pass | smoke matrix 4/4 (KVM+TCG, 1+2 CPU)
> **Deferred:** [H] EC hardware access is gated off: readiness needs GPE acknowledgement and ACPI Global Lock arbitration, neither of which exists (reason: polled commands raise the EC GPE; an unacked level-triggered SCI gets the shared GSI quarantined) -> XREF: `02-kernel-core/TODO-26-power-management.md` §24 (item: "Open the EC gate once GPE acknowledgement works" at line 1059)
> **Deferred:** [M] Interrupt-driven QR_EC query dispatch (reason: same GPE blocker) -> XREF: `02-kernel-core/TODO-26-power-management.md` §24 (item: "Acknowledge a GPE that has no handler yet rather than leaving the line asserted" at line 1054)
> **Deferred:** [M] Recovery from a desynchronized controller, and EC behaviour across S3/S4 resume (reason: both need a controller-reset boundary that does not exist) -> XREF: `02-kernel-core/TODO-26-power-management.md` §24 (item: "Open the EC gate once GPE acknowledgement works" at line 1059)
> **Accepted:** [H] AML `OperationRegion(EmbeddedControl)` address-space handler, without which `_BST`/`_BIF`/`_TMP` cannot execute at all (reason: belongs to ACPICA integration, not to this driver) -> XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md` §1 (item: "Install the Embedded Controller address-space handler so AML `OperationRegion(EmbeddedControl)` accesses reach the EC driver" at line 104)
> **Accepted:** [M] DSDT `_HID "PNP0C09"` / `_CRS` discovery for machines with no ECDT (reason: needs an evaluable ACPI namespace this kernel does not build) -> XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md` §1 (item: "`acpi_evaluate(path, args, result)` wrapper around `AcpiEvaluateObject`" at line 102)
> **Quality reviewed:** 2026-09-04 | Codex 9x (design, adversarial x4, test-coverage, consistency, perf, re-adversarial x4 -- converged round 6, zero findings) | 6H+11M+5L fixed, 0 open | scope: kernel-code-quality + kernel-quality-auditor + concurrency-evidence-mapper + parity-research-analyst

---

## 6. Battery & AC Adapter (ACPI Source Layer)

> **Spawned-by:** root

Evaluate the ACPI battery and AC-adapter objects and publish each one into the §29 composite model as a provider. This section owns ONLY the namespace-facing half. The provider-agnostic state model, warning policy and registry publication were split out to §29 (implementable without the namespace); charge limiting and smart charging were split out to §30 (blocked on the EC write gate instead).

- [ ] Enumerate every `_HID "PNP0C0A"` battery device in the namespace, not a fixed `\_SB.BAT0` path -- dual-battery and docked systems carry a separate ACPI object per battery
  - Codex gap-audit finding (2026-09-03): a single fixed-path read reports the wrong percentage and can trigger a critical-power action from the wrong source on multi-battery hardware. -> XREF: ACPI 6.6 §10 Power Source and Power Meter Devices.
- [ ] Evaluate `_BIX` (Battery Information Extended, ACPI 4.0+) per device on init; fall back to `_BIF` (deprecated) when `_BIX` is absent:
  - `_BIX` returns: revision, power unit, design capacity, last full charge capacity, technology, design voltage, design capacity of warning, design capacity of low, cycle count, measurement accuracy, max sampling time, min sampling time, max/min average interval, battery capacity granularity, model number, serial number, battery type, OEM info
  - `_BIF` returns: design capacity, full charge capacity, technology, design voltage, warn capacity, low capacity, granularity, model, serial, chemistry
  - Map the parsed result onto `bat_info_t` (§29) and hand it back from the provider's `get_info` hook; ACPI's `0xFFFFFFFF` unknown sentinel becomes `BAT_UNKNOWN`
- [ ] Evaluate `_BST` (Battery Status) per device every 30 s or on an EC event, and drive `bat_update()` (§29) from it:
  - Returns: power state (discharging=1, charging=2, critical=4), present rate (mW), remaining capacity (mWh), present voltage (mV)
  - Normalise the `_BIX.power_unit` mA/mW distinction before publishing: a mA-reporting battery is converted with `present_voltage` so the composite model only ever sees mW/mWh
- [ ] Evaluate `\_SB.ACAD._PSR` (AC Adapter Power Source, `_HID "ACPI0003"`): 0=offline, 1=online; publish through `bat_ac_set_online()` (§29)
- [ ] Register each enumerated battery with `bat_register()` and drop it with `bat_unregister()` on removal, so the §29 composite view is the only aggregation point
- [ ] Track insertion/removal per battery device (ACPI `Notify` device-check 0x81 / bus-check 0x00); a hot-removed battery leaves the composite view rather than reporting stale data
- [ ] `_BTP` (Battery Trip Point) where the device exposes it, so a capacity crossing raises a notify instead of being found by the 30 s poll
- [ ] Commit: `"kernel/acpi: battery _BIX/_BIF/_BST evaluation, AC adapter _PSR, per-device enumeration"`

**Test checkpoint:** every `PNP0C0A` device in the namespace is enumerated and registered as a §29 provider. `_BIX` is parsed, with `_BIF` fallback on a device that lacks it. `_PSR` drives the AC-online flag. A device-check notify adds/removes a provider and the composite state follows. Test on: QEMU TCG (skip when the namespace exposes no battery), then bare metal on a laptop.

> **Test runner:** N/A (deferred -- no code shipped) | validation: deferred until the namespace blocker below clears

> **Deferred:** [H] every item on this section evaluates an ACPI control method, and ACPI namespace evaluation does not run on this tree. ACPICA is vendored and linked, but `AcpiInitializeSubsystem()` / `AcpiLoadTables()` / `AcpiEnableSubsystem()` have ZERO call sites outside `src/kernel/acpica/` and no `acpi_evaluate()` wrapper exists (re-verified 2026-09-04 by grep over `src/` + `include/`), so `_BIX`, `_BIF`, `_BST` and `_PSR` are all unreachable. The provider-agnostic half was split into §29 and IS implementable now; the charge-limit half was split into §30. -> XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md` §1 (item: "Initialisation sequence in `acpi_init()`"), `02-kernel-core/TODO-26` §29, `02-kernel-core/TODO-26` §30

---

## 7. Power & Sleep Button Event Dispatch

> **Spawned-by:** root

The thread-level consumer the §1 SCI ISR was deliberately split against: `acpi_sci_process()` acknowledges `PWRBTN_STS` / `SLPBTN_STS` and counts them in hard-IRQ context precisely because it may not log or act there, and it already XREFs `acpi_power_button_event()` as the missing half. Lid handling moved to §31 and human-presence detection to §32: both need prerequisites this half does not.

**Files:** `src/kernel/acpi.c`, `include/kernel/acpi.h`, `src/kernel/test/test_acpi_power.c`

- [x] `acpi_power_button_event()` -- performs the cached `HKLM\SYSTEM\PowerControl\PowerButtonAction` action, naming it in the log
  - Codes `0` ignore, `1` sleep (S3), `2` hibernate (S4), `3` shutdown (S5, the default), `4` lock, as `ACPI_BTN_ACTION_*` in `include/kernel/acpi.h`.
- [x] `acpi_sleep_button_event()` on `SLPBTN_STS`, same action table, `SleepButtonAction` (default `1` = sleep)
- [x] `acpi_btn_resolve_action(raw, present, fallback)` resolves an absent, unreadable or out-of-range value to that button's DEFAULT, never to "do nothing": a typo in the hive must not silently disable the power button
- [x] An in-range action that is currently UNAVAILABLE is refused with one log line and nothing else happens; it is NOT redirected to the default
  - This reverses the original draft, on a design-review finding: promoting a configured "sleep" to the shutdown default destroys the session the setting exists to preserve, and for the sleep button the redirect is circular, because S3 is its own default. The firmware long-press cutoff remains the emergency power-off path on such a machine.
  - Malformed policy and unavailable policy are therefore different things, decided in different functions: `acpi_btn_resolve_action()` owns the first, `acpi_btn_action_available()` the second.
- [x] Drain by EDGE, not by level: `acpi_btn_drain()` compares the ISR's monotonic count against the dispatcher's watermark, so a burst collapses to one action and a missed wake cannot lose an event
  - The design review found the counters were `uint32_t`, so a wrap lands back on a stale watermark and discards the whole interval; a stuck or reasserted SCI is exactly what reaches that count. All three counters and their public readers are now `uint64_t`.
- [x] The consumer runs at thread level, never in the SCI path: a threaded `KDPC` (PASSIVE_LEVEL, so the shutdown quiesce may sleep) queued with `KeInsertQueueDpcOnCpu` pinned to the CPU-0 service queue
- [x] Re-entrancy: `acpi_btn_gate_take()` / `acpi_btn_gate_release()` admit one holder, so a press during an in-flight action does not stack a second one; its count survives for the next drain
- [x] Serialize the whole PM1 read / acknowledge / count / enqueue sequence under `s_pm1_evt_lock`
  - Design-review finding, verified in tree: `src/kernel/irq.c` installs no KINTERRUPT wrapper on the shared path (it binds exclusive vectors only) and `irq_set_affinity()` retargets the IOAPIC without draining an in-flight dispatch, so two CPUs can be inside the handler at once. Unserialized they double-count one event and insert the SAME KDPC concurrently, which the DPC contract forbids.
- [x] The policy is sampled ONCE, before the PM1 enable bits are written, and cached
  - Design-review finding, verified in tree: the registry has no SMP lock and `RegSetValueEx` publishes type, size and data through separate unsynchronized stores, so a per-dispatch read could pick a torn value for a safety-critical decision.
- [x] Boot log line naming the resolved action for each button and whether it is available, so a machine that ignores its power button says why
- [x] `acpi_btn_plan()` is the whole drain-and-decide pass, split out so the dispatcher decision is testable end to end without a real PM1 event or the real actuator
- [x] `acpi_enter_sleep_state()`'s wake snapshot widened with the counters, pinned by a `_Static_assert` against the accessor's own return width
  - Re-adversarial finding, verified: `wake_before` was left `uint32_t` while `acpi_wake_event_count()` became `uint64_t`, so past 2^32 the S1 wake comparison is unequal on every pass and any interrupt that merely releases the halt reads as a confirmed wake. The assert is the defence, because the compare sits inside a halt path no test can call.
- [x] Unit tests: eighteen `test_acpi_btn_*` cases in `src/kernel/test/test_acpi_power.c`
  - Resolver: in-range preserved, present-zero (ignore) distinguished from absent, out-of-range to the CALLER's default for both buttons.
  - Availability per action, and the power default is itself actionable.
  - Drain: burst, full width above 2^32, 64-bit wrap, NULL watermark.
  - Plan: burst collapses to one action, an idle pass decides nothing, a press during an action survives, both buttons drain every pass, NULL outputs, the no-action sentinel is outside the action range.
  - Gate admits one holder; the cached action matches what the hive holds, not merely a valid code.
- [x] Disarm the PM1 enables on every SCI-registration failure path (review, adversarial)
  - `acpi_enable_fixed_events()` sets `PWRBTN_EN`/`SLPBTN_EN` before `acpi_register_sci()` runs, so a failed registration (unroutable GSI, no IOAPIC for a GSI-valued `SCI_INT`, no vector, vector already claimed) left a level-triggered source enabled with no handler to acknowledge it. `acpi_disable_fixed_events()` now clears both enable and status registers on all four paths.
- [x] Seed `HKLM\SYSTEM\PowerControl` in `registry_populate_defaults()` (review, kernel-quality auditor)
  - The key was created nowhere in the tree, so the entire registry half of this policy was unreachable and every boot took the "no key" branch. A configurable surface whose key never exists is not configurable.
- [x] Re-entry guard on the policy init (review, kernel-quality auditor)
  - A second `acpi_enable_fixed_events()` would run `KeInitializeThreadedDpc` on a possibly-QUEUED KDPC, zeroing its link fields and unlinking it from the middle of a live queue. `acpi_register_sci()` 70 lines below already carried exactly this guard.
- [x] Detect the control-method button and say the fixed-event path cannot fire (review, parity)
  - FADT `PWR_BUTTON` / `SLP_BUTTON` (ACPI 6.5 Table 5-10) mean the button is a `PNP0C0C`/`PNP0C0E` namespace device on a GPE, so `PWRBTN_STS` never asserts. Logging the resolved actions without this reads as "the power button works", which on that hardware is the opposite of the truth. The control-method dispatch itself is owned elsewhere -> XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md` §3 (item: "**Control-method button devices**")
- [x] Header-own the registry path and both value names, and replace the tautological cache test (review, consistency + kernel-quality auditor)
  - The path was typed once in the source and again in the test, and the test compared the cache against a resolve of whatever the hive held, which cannot fail: with the key absent both sides evaluate to the same default. It now asserts the key and BOTH values exist, then compares each cached action against a live read by name.
- [x] Give `acpi_btn_dispatch_count()` a reader, and log a gate refusal instead of dropping silently (review)
- [x] Replace the `sizeof(accessor())` static assert with a named `acpi_event_count_t` (review, cppcheck)
  - The assert pinned the invariant but was a function call inside `sizeof` and still let a caller declare the wrong type and learn only at the assert. A shared typedef makes a narrowing a compile-time type change instead.
- [/] `WM_QUERYENDSESSION` to all windows with a 5 s grace before the action executes -- blocked, no in-kernel path can broadcast a window message to top-level windows
  - The primitive it needs is `BroadcastSystemMessage`, which the user32 master table still lists as `NO_OWNING_TODO`, so this park has no owner to wait on yet and that is the honest state -> XREF: `10-platform-services/TODO-A-user32-export-master-table.md` (item: "`BroadcastSystemMessage`")
- [/] `4` = lock screen: no lock screen or session state exists to lock, so the action parses, resolves and is refused rather than performed -> XREF: `09-desktop-shell/TODO-06-security-accounts.md` §7 (item: "`void lock_screen_show(void)`")
- [/] Route the shutdown action through an SMP-safe shutdown once one exists -> XREF: `02-kernel-core/TODO-26` §27 (item: "Park every other CPU before the shutdown storage quiesce")
  - Accepted from the design review rather than fixed here: `acpi_shutdown()` quiesces storage but halts only the calling CPU, so another CPU can still submit I/O during the quiesce. It is pre-existing and already the path taken by `SYS_SHUTDOWN` (`src/kernel/sched/syscall.c:1036`), `nt_syscall.c:1591` and the desktop power menu (`src/desktop/desktop.c:1082`); refusing it for the button alone would leave this section a no-op while changing nothing about the risk.
- [x] Commit: `"kernel/acpi: power and sleep button event dispatch with registry action policy"`

**Test checkpoint:** `PowerButtonAction=3` resolves to shutdown, `=0` to ignore, `=99` to the default with one log line, and `=1` (sleep) resolves to sleep and is then REFUSED with one log line rather than falling back to shutdown. `SleepButtonAction` resolves through its own default. A burst of counts produces exactly one action per button, and a press during an action is found by the next pass. Test on: QEMU TCG.

> **Test runner:** `bash scripts/test.sh SUITE=boot` -- the twenty-one `ACPI: button *` / `ACPI: no-action sentinel *` cases pass with the rest of `test_acpi_power.c` (`scripts/debug/kernel/run-boot-tests.bat` on Windows).

> **Notes:**
> - Shipped the thread-level half of the §1 SCI split: a threaded DPC queued from `acpi_sci_process()` drains the fixed-event counters by watermark and performs the cached registry action.
> - The policy core is pure and therefore testable without the actuator: resolve, availability, drain, plan and gate are separate functions the new cases drive directly.
> - Downstream: the three fixed-event counters and their public readers widened from `uint32_t` to `uint64_t`; no caller outside `src/kernel/acpi.c` consumed them.
> - Malformed policy takes the default, unavailable policy is refused; that split reversed the original draft and is the section's load-bearing behavioural decision.
> - Scope boundary: lid events are §31 and human-presence detection §32, both split out because their event sources do not exist on this tree.

> **Verified:** 2026-09-04 | commit `eb72bfdbf` | 21/21 items | build OK | suite 33537 kernel + 17 user-mode pass | smoke matrix 4/4 legs
> **Accepted:** [H] the shutdown action reaches `acpi_shutdown()`, which quiesces storage but halts only the calling CPU, and several initiators can enter it concurrently (reason: pre-existing and already the path taken by `SYS_SHUTDOWN`, `nt_syscall.c` and the desktop power menu, so refusing it for the button alone would change nothing about the risk) -> XREF: `02-kernel-core/TODO-26` §27 (item: "Park every other CPU before the shutdown storage quiesce")
> **Accepted:** [H] the control-method button device (`PNP0C0C` / `PNP0C0E`) is detected and reported but not dispatched; QEMU's own FADT sets `SLP_BUTTON`, so this is live hardware behaviour rather than a hypothetical (reason: the ACPICA namespace path owns it and this file is the pre-ACPICA fixed-event fallback) -> XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md` §3 (item: "**Control-method button devices**")
> **Accepted:** [M] no short-press versus long-press distinction; a PM1 status bit carries no edge timestamps (reason: the owning section already specifies a press-duration timer) -> XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md` §3 (item: "`acpi_button_work`: if `uptime_ns() - press_time < 2_000_000_000`")
> **Accepted:** [M] this section's flat policy key and §18's planned per-plan copy are two homes for one setting, with no precedence and no AC/DC axis (reason: the power-plan surface owns the reconciliation) -> XREF: `02-kernel-core/TODO-26` §18 (item: "`PowerButtonAction` (REG_DWORD): same codes as §7")
> **Quality reviewed:** 2026-09-04 | Codex 7x (design, test-coverage, adversarial x2, re-adversarial, consistency, perf) | 8H+5M+2L fixed, 4 open | scope: kernel-code-quality (kernel-quality-auditor + concurrency-evidence-mapper + parity-research-analyst)

---

## 8. PCI Power Management Capability and D0--D3hot State Machine

> **Spawned-by:** root

The config-space-only half of the PCI D-state work: capability discovery and the D0/D1/D2/D3hot transitions defined entirely within PMCSR. Two pieces split out of this section because they depend on machinery this one does not: the per-device power registry (§33) and D3cold, which needs ACPI control-method evaluation (§34).
> → XREF: `02-kernel-core/TODO-26-power-management.md` §33 (item: "`pm_register_device(dev, on_sleep, on_wake)` -- called by each PCI driver at probe time; adds to the global `pm_device_list`")
> → XREF: `02-kernel-core/TODO-26-power-management.md` §34 (item: "`pci_d3cold_enter(dev)` -- `pci_set_d_state(dev, 3)` first (D3hot), then evaluate `_PS3`")

- [x] `pci_pmcap_find(bus, dev, fn)` -- walks the capability list for cap ID `0x01` and returns its config offset, in `src/kernel/drivers/pci_pm.c`
  - Gated on `PCI_STATUS` bit 4, TTL-bounded at `PCI_CAP_WALK_MAX` (48, exactly the number of DWORD-aligned node positions), and header-type aware: `pci_cap_ptr_offset()` reads the pointer at `0x34` for types 0/1 and `0x14` for CardBus, and refuses an undefined type outright.
  - Misaligned pointers are REJECTED rather than masked, because masking silently redirects the walk into an unrelated register. Node bound and PM footprint bound are separate: a node may sit at `0xFC`, a PM capability may not (its PMCSR at +4 would not fit), so a legal list whose last entry is non-PM is no longer reported malformed.
  - CardBus lower bound is `0x48`, not `0x40`: type 2 keeps subsystem IDs at `0x40` and the legacy-mode base at `0x44`, and a hostile pointer there reading `0x01` would otherwise be taken for a PM capability.
- [x] `pci_pmcap_read()` fills `PCI_PMCAP` {`cap_id`, `next_cap`, `pmc`, `pmcsr`, `cap_off`}, declared in `include/kernel/drivers/pci_pm.h`
  - Field widths are the hardware's (byte ID and next pointer), not the uniform 16-bit words this item originally drafted. Constants shipped: `PMCSR_POWER_STATE_MASK`, `PMCSR_NO_SOFT_RESET`, `PMCSR_PME_EN`, `PMCSR_PME_STATUS`, `PMC_D1_SUPPORT`, `PMC_D2_SUPPORT`.
  - Reads into a LOCAL snapshot and validates `cap_id`, PMCSR plausibility and PM revision before publishing it, so a device that stops answering partway through the four config transactions cannot hand the caller a mixture of real and floating values.
- [x] `pci_set_d_state(bus, dev, fn, state, &reinit_required)` -- performs the transition and verifies it latched
  - Returns `PCI_DX_OK` or a negative `pci_dx_status_t`; `PCI_DX_UNSUPPORTED` covers both no PM capability and a D1/D2 the `PMCAP` bits do not advertise.
  - Every PMCSR write goes through `pci_pmcsr_write_value()`, which clears `PMCSR_PME_STATUS` out of the write. That bit is write-1-to-clear, so a naive read-modify-write ACKNOWLEDGES and destroys a pending wake event while preserving `PME_En` and the reserved fields.
- [x] Recovery delays follow the transition, not one blanket wait: any transition with D3hot at either end waits 10 ms, any with D2 at either end waits 200 us, D0 <-> D1 is immediate
  - Applied INSIDE `pci_set_d_state()` and on the way DOWN as well as up, so even the readback that confirms the write waits. `pci_pm_recovery_delay_us()` is pure and the full matrix is asserted.
  - Timed on `mono_ns()` with elapsed subtraction. `pci_pm_timebase_ready()` qualifies the SOURCE first and accepts only TSC/HPET/PMTMR: the tick-derived LAPIC source stops advancing with interrupts off, which is exactly the context a transition may run in. An unqualified source refuses with `PCI_DX_NO_TIMEBASE` BEFORE the write, so the device is never left in a state nothing may touch.
- [x] `pci_get_d_state()` reads the PMCSR state field back, so the hardware is the source of truth rather than a value a driver believes it wrote
- [x] Transition legality is enforced, not assumed: a device may move DEEPER or to D0, never part-way out
  - `pci_pm_transition_legal()` refuses D3hot->D1, D3hot->D2 and D2->D1. Matches the transition table the Linux PCI documentation publishes.
- [x] Non-response is never read as a D-state -- `pci_pm_pmcsr_plausible()` rejects `0xFFFF` and any value with the required-zero PMCSR bits 7:4 set
  - `0xFFFF & PMCSR_POWER_STATE_MASK` is D3hot, so without this an absent, removed or wedged device CONFIRMS the suspend it failed to perform and the caller carries on believing DMA and interrupts are quiesced. `0xFFFB` is the same hazard without being all-ones. Bit 2 is deliberately not required to read zero: its reset value is device-specific, and refusing a legitimate suspend is not the safe direction.
  - `pci_pm_pmc_version_supported()` accepts PM revisions 1..3 and gates both the setter and the getter, so a reserved revision is never driven. Discovery stays structural.
- [x] `No_Soft_Reset` is honoured: a D3hot->D0 on a device with the bit clear lands in D0 Uninitialized, and the caller is told so
  - Reported through a MANDATORY `int *reinit_required` out-parameter rather than a distinguished return value, so it cannot be missed by a caller that only tests `rc < 0`. It is assigned as soon as the write happens, so the failure paths after it carry it too.
  - A genuine no-op reports NO obligation. Claiming one whenever `No_Soft_Reset` is clear was tried and withdrawn: that bit says what a D3hot->D0 WOULD do, not that this D0 came from one, so an actively running controller would be told to tear down its BARs and interrupts against live DMA. Telling a failed-transition retry from an ordinary D0 request needs history this section does not keep -> XREF: `02-kernel-core/TODO-26-power-management.md` §33 (item: "Persistent per-BDF reinitialisation-pending state, so a failed D3hot->D0 cannot lose its obligation across a retry")
  - The device is polled for a response (`PCI_VENDOR_ID` != `0xFFFF`, bounded by `PCI_PM_D0_READY_MAX_US`) before its PMCSR is read back, because a device that came back uninitialised can take longer than the spec minimum to answer config cycles at all.
- [x] Transitions are serialised per device across the WHOLE sequence -- claim, read, decide, write, recover, verify, release
  - An 8-entry in-flight table under `s_pci_pm_lock`; a second caller for the same BDF gets `PCI_DX_BUSY` rather than blocking, since a spinlock cannot span a 10 ms recovery interval. The claim is taken BEFORE capability discovery, which itself reads config space.
  - Serialising only the write was not enough: a caller planning from a state the first has already left performs a prohibited transition and waits the wrong delay.
- [x] A transition that cannot observe its recovery interval POISONS the module rather than reporting success
  - `pci_pm_delay_us()` ends on `PCI_PM_CLOCK_STALL_SPINS` identical clock samples with `PCI_DX_CLOCK_STALLED`; the flag is machine-wide because the only cause is a qualified hardware counter stopping, which is not a property of the device being transitioned. `pci_pm_clear_poison()` is the only way back and takes no arguments, so no set of dead devices can starve a healthy one.
  - Poison is an ADMISSION GATE, not a flag consulted separately: every config access goes through `pci_pm_guarded_read8/read16/write16`, which test the flag and perform the access in ONE critical section. A gate that tests and acts separately does not gate.
- [x] `pci_pm_bdf_valid()` rejects device >= 32 and function >= 8 at every entry point, before any claim or config access
  - The CF8 address format packs the fields adjacently, so an out-of-range device does not fail, it ALIASES another one: `00:20.0` and `01:00.0` compute the same address while the claim table treats them as different devices.
- [x] `src/kernel/drivers/pci.c`: every config accessor now holds a global IRQ-safe spinlock across the WHOLE CF8/CFC transaction
  - The mechanism is a pair of shared PORTS, so without this another CPU replaces CF8 between the address write and the data access and the second half lands on a different device. Eleven files call these accessors, from both thread and interrupt context. This is the root fix for the read-modify-write corruption the PMCSR path would otherwise have suffered.
- [x] `pci_write16()` issues a sized 16-bit write instead of a read-modify-write of the enclosing DWORD (review: kernel-quality-auditor)
  - The RMW rewrote the ADJACENT register, so writing `PCI_COMMAND` (0x04) wrote back the just-read `PCI_STATUS` (0x06) whose Master Abort, Target Abort, SERR and parity bits are write-1-to-clear. Every `pci_enable_bus_mastering()` and ten driver call sites were silently acknowledging latched bus errors nobody had read. Same defect class as the PMCSR hazard this section was built around, one function away, pre-existing.
- [x] Transitions refuse above PASSIVE_LEVEL with `PCI_DX_IRQL`, checked before any claim or write (review: perf)
  - The recovery wait and readiness poll busy-spin for up to a second; at DISPATCH_LEVEL that holds off every lower-priority interrupt and DPC. Documented as a contract on consumers, not just a check.
- [x] The D0 readiness allowance is 1 s, and vendor ID `0x0001` (PCIe Request Retry Status) counts as NOT ready (review: adversarial)
  - 60 ms was short enough to declare a compliant storage or USB controller failed during resume, and treating anything but all-ones as ready accepted an explicitly not-ready device and touched it mid-reset.
- [x] Poisoning emits a one-shot diagnostic through `serial_write_recoverable()` naming the device and the failing status (review: adversarial)
  - `klog()` was tried and withdrawn: it reaches disk I/O, and the device whose transition just failed may BE the storage controller. Plain `serial_write()` was withdrawn too, because it polls THRE unbounded and a wedged UART would turn a bounded clock stall into a permanent hang holding the serial lock.
- [x] `virtio.c` uses the shared `PCI_STATUS` / `PCI_STATUS_CAP_LIST` / `PCI_CAP_PTR_TYPE01` instead of its own private duplicates (review: consistency)
- [x] Named `PCI_CFG_NO_RESPONSE` for the 0xFFFF sentinel and pinned the capability TTL with a `_Static_assert` (review: consistency, kernel-quality-auditor)
  - The TTL of 48 is exactly the count of legal DWORD-aligned node positions, which is what lets the walk tell a maximal legal list from a cyclic one; that relation was asserted in prose only.
  - Also corrected two comments that had stopped describing the code: one still documented the removed raw-TSC delay design, the other claimed a config transaction sits "well inside" the spinlock hold budget when it does not.
- [x] Commit: `"kernel/pci: PM capability discovery, D0/D1/D2/D3hot state machine, spec recovery delays"`

**Test checkpoint:** `pci_pmcap_find()` returns a valid offset for a PM-capable device and -1 both for a device without the capability and for one whose `PCI_STATUS` capability bit is clear. `pci_set_d_state(dev, 3)` writes PMCSR and `pci_get_d_state()` reads 3 back. `pci_set_d_state(dev, 0)` restores D0 after the 10 ms recovery delay. A D1 request against a device whose `PMCAP` does not advertise D1 returns `PCI_DX_UNSUPPORTED` rather than writing. Test on: QEMU TCG + WHPX.

> **Test runner:** `scripts/debug/kernel/run-boot-tests.bat` (or `bash scripts/test.sh SUITE=boot`) -- 54 `PCI PM: *` cases, all PASS; the decision logic is pure, so none of them needs a PM-capable device on the bus, and `pci_pm_set_poisoned_for_test()` is the fault-injection seam that makes the admission gate provable (same shape as `pmm_alloc_fail_next`).

> **Notes:**
> - Shipped `src/kernel/drivers/pci_pm.c` + `include/kernel/drivers/pci_pm.h` (capability walk, D0/D1/D2/D3hot state machine, pure transition planner) and hardened `src/kernel/drivers/pci.c` with a CF8/CFC transaction lock.
> - Integrates through `pci_read*`/`pci_write16` only; no init hook, no boot-path code, no ACPI dependency, so it is callable from any driver at probe time.
> - §9 and §12 consume `pci_set_d_state()`; §23 hangs ASPM policy off the same devices; the per-device registry that records their state is §33.
> - Canonical contract is the header comment block in `include/kernel/drivers/pci_pm.h`: PME_Status is write-1-to-clear, recovery intervals forbid ALL access, the only exit from a low-power state is D0, and No_Soft_Reset clear means D3hot->D0 lands uninitialised.
> - Scope boundary: D3cold (§34) and the `pm_device_t` registry (§33) are deliberately NOT here; the claim serialises D-state callers only, and a gate binding every config client is §33's.
> **Verified:** 2026-09-04 | commit `9d40a3ba6` + review fixes | 13/13 items | build OK | tests 33709 kernel + 17 user-mode PASS | smoke matrix 4/4 legs (KVM/TCG x 1/2 CPU) | lint 0 errors
> **Accepted:** [H] the per-device claim binds D-state callers only, so an unrelated driver can touch a device inside another CPU's recovery interval -> XREF: `02-kernel-core/TODO-26-power-management.md` §33 (item: "Per-device access gate that EVERY config-space client honours, so no driver touches a device during another caller's D-state recovery interval")
> **Accepted:** [H] a retry after a failed D3hot->D0 cannot be told from an ordinary D0 request, so the reinitialisation obligation is not tracked exactly (reason: needs per-device history this section does not keep) -> XREF: `02-kernel-core/TODO-26-power-management.md` §33 (item: "Persistent per-BDF reinitialisation-pending state, so a failed D3hot->D0 cannot lose its obligation across a retry")
> **Accepted:** [M] every state query re-walks the capability list, up to ~50 config transactions for one read -> XREF: `02-kernel-core/TODO-26-power-management.md` §33 (item: "Cache the validated PM capability offset and revision per device, invalidated on removal or re-probe")
> **Accepted:** [M] the machine-wide poison latch has no production caller able to clear it, so a real stall would refuse PCI PM for the rest of the boot -> XREF: `02-kernel-core/TODO-26-power-management.md` §33 (item: "A production owner for PCI PM poison recovery: a bus rescan or re-probe path that calls `pci_pm_clear_poison()` after re-establishing the affected devices")
> **Accepted:** [M] `pci_set_d_state()` is PASSIVE_LEVEL only, which an ordinary KDPC-driven autosuspend would fail -> XREF: `02-kernel-core/TODO-26-power-management.md` §12 (item: "Timer expiry: call `ops->runtime_idle(ctx)`; if returns 0 (device can suspend): call `ops->runtime_suspend(ctx)` to transition to low-power state")
> **Quality reviewed:** 2026-09-04 | Codex 17x (design, adversarial x12, consistency, perf, test-coverage, re-adversarial x4) + kernel-quality-auditor + concurrency-evidence-mapper | 12H+9M fixed, 5 accepted | scope: kernel-code-quality


---

## 9. Driver Power Callbacks & Resume Ordering
- [x] EC-specific sleep/resume handling, registered like any other driver callback. -> XREF: `02-kernel-core/TODO-26-power-management.md` §5 (item: "`acpi_ec_init()` -- Phase 2")
  - Shipped as `acpi_ec_on_wake()` in `src/kernel/drivers/acpi_ec.c`, registered at `PM_PRI_INPUT` from `acpi_ec_init()` the moment an EC is DISCOVERED rather than once it is drivable: readiness can change after discovery and the callback re-checks `acpi_ec_ready()` at dispatch time, so registering on the ready path only would silently skip the idle proof on every machine whose EC becomes drivable later.
  - The resume half re-runs the bounded idle proof (`acpi_ec_quiesce_io()`) under the same `g_ec_lock` the public transaction wrappers take, and reports a failed proof rather than swallowing it.
  - Registered with NO sleep half, deliberately. The pre-sleep work this driver needs is disabling and re-arming the EC GPE around the transition, and nothing in this kernel owns a GPE yet -- a sleep callback that did nothing would tell the dispatcher the EC was quiesced when it was not. -> XREF: `02-kernel-core/TODO-26-power-management.md` §24 (item: "`acpi_gpe_enable(n)` / `acpi_gpe_disable(n)` so a driver arms only the events it services")
  - Filed from the §5 parity pass, 2026-09-04: neither §3, §28 nor `04-drivers-hardware/TODO-03` §9 mentioned the EC at a sleep transition, so this was ownerless.
- [x] `pm_register_power_callback(priority, on_sleep, on_wake, ctx, name)`:
  - `priority`: `PM_PRI_STORAGE=0`, `PM_PRI_NETWORK=1`, `PM_PRI_USB=2`, `PM_PRI_INPUT=3`, `PM_PRI_GRAPHICS=4`, `PM_PRI_USER=5`
  - Static array of 64 callback slots, append-only and NOT sorted: the walk takes priority as its outer loop instead. Sorting on insert would move published entries under a concurrent reader, and buys nothing a 6-bucket outer loop does not already give.
  - Refuses a duplicate `(on_sleep, on_wake, ctx)` triple so a re-run init cannot double-notify, refuses a slot with neither callback, and refuses entirely while a transaction is in flight.
  - Two layers: `pm_cb_table_*()` is pure mechanics over a caller-supplied table (injectable clock, no globals) and `pm_*()` is the production singleton. The split is what lets the tests run at all -- see the Unit Tests note below.
- [/] Each major driver registers in its `init()` -- EC done, the other four BLOCKED on a per-driver quiesce primitive that does not exist yet
  - `acpi_ec_init()` -> `PM_PRI_INPUT` -- SHIPPED (wake half; see the first item).
  - `ahci_init()` -> `PM_PRI_STORAGE`, `xhci_init()` -> `PM_PRI_USB`, `rtl8139_init()` -> `PM_PRI_NETWORK`, `fb_init()` -> `PM_PRI_GRAPHICS` (the section said `framebuffer_init()`; the real symbol is `fb_init()` at `include/kernel/drivers/framebuffer.h:15`).
  - BLOCKED, and deliberately not stubbed: none of those four drivers exposes a stop/start or save/restore primitive today (verified 2026-09-04 by reading their headers -- `ahci.h`, `xhci.h`, `rtl8139.h` export init/IO entry points only). Registering a callback that did nothing would be worse than not registering: `pm_notify_sleep()` would report the boot disk quiesced to §3 when its DMA is still live. The dispatcher is ready for them the moment the primitives exist.
  - -> XREF: `04-drivers-hardware/TODO-13-storage-controller-device-drivers.md` §2 (item: "Add power-management callbacks for link state and suspend/resume") -- the AHCI quiesce primitive, already owned there
  - -> XREF: `04-drivers-hardware/TODO-10-usb-stack.md` §1 (item: "Add `suspend(hcd)` / `resume(hcd)` to `usb_hcd_ops_t`") -- the xHCI quiesce primitive, filed there by this section
  - `rtl8139_init()` and `fb_init()` have no owning section: TODO-14 covers e1000/RTL8169/igc/RTL8125 but not the legacy RTL8139, and a linear framebuffer has no DMA queue to stop (an S3 resume needs a GPU re-POST this kernel cannot perform). Both follow once AHCI and xHCI establish the primitive shape.
- [x] `pm_notify_sleep(state)` -- walks the callback registry in **reverse** priority order (user-space -> graphics -> input -> USB -> network -> storage):
  1. Call `cb->on_sleep(state, ctx)`. The DRIVER owns any `pci_set_d_state(dev, 3)` call, not the dispatcher: a callback carries an untyped `void *ctx` and no bus/dev/fn, so the dispatcher has nothing to address a device with. Generic dispatcher-driven D-state stepping needs the per-device registry. -> XREF: `02-kernel-core/TODO-26-power-management.md` §33 (item: "`pm_register_device(dev, on_sleep, on_wake)` -- called by each PCI driver at probe time; adds to the global `pm_device_list`")
  2. Measure each callback and log `[WARN] pm: <phase> callback '<name>' overran budget` past 2 s. This is a POST-RETURN DIAGNOSTIC and the section's original "if it hangs, log and proceed" wording was not implementable: a callback is a plain indirect call, nothing in this kernel can preempt or abandon one, so a callback that genuinely hangs hangs the transition and emits no warning at all. The header says so explicitly rather than leaving a caller to infer a liveness guarantee that does not exist.
  3. STOP at the first failure and UNWIND -- wake every slot already quiesced, in resume order, and return `PM_CB_CALLBACK_FAILED` so the caller does not enter the platform sleep state. Continuing past a failed quiesce would leave part of the machine live with its driver believing it is suspended.
  4. If the UNWIND itself fails the table does not return to idle: it enters `PM_TXN_DEGRADED`, retains the bits it could not recover, and returns `PM_CB_UNWIND_FAILED`. Going back to idle there would advertise a clean machine while a device is still down and let the next transaction stack on top of it; only an explicit re-init clears it.
- [x] `pm_notify_resume(state)` -- walks in **forward** priority order (storage, network, USB, input, graphics, user-space):
  1. Wakes exactly the slots the matching sleep walk made ELIGIBLE, tracked in a `uint64_t` bitmap, so a driver that registered between the two walks is neither slept nor woken. Registration is refused for the whole sleep-to-resume transaction for the same reason. Eligible is not the same set as quiesced: a wake-only registration (the EC) never ran a sleep half, so it is woken by a completed resume but is NOT touched by a sleep abort, which unwinds only what actually quiesced. The two sets are separate masks in the implementation for exactly that reason.
  2. Call `cb->on_wake(state, ctx)` -- driver re-initialises DMA, re-arms interrupts, re-establishes network/USB links. The driver owns its own `pci_set_d_state(dev, 0)`, same argument as the sleep side.
  3. Does NOT stop at the first failure: there is no "do not proceed" left to protect and abandoning the walk would strand every remaining device powered down. A `PM_PRI_STORAGE` wake failure returns the distinct `PM_CB_STORAGE_FAILED` (not the generic `PM_CB_CALLBACK_FAILED`), which is the signal the caller MUST NOT unfreeze the scheduler on -- a return value rather than a report field, because the production entry point does not hand the report back and the one caller who must act on it could not otherwise see it -- releasing filesystem threads against a controller that did not come back is the failure this ordering exists to prevent. The 5 s storage budget is the same post-return diagnostic as the sleep side, not a wait.
  4. ANY failed wake, storage or not, enters `PM_TXN_DEGRADED` and RETAINS the slots that did not come back, exactly as a failed unwind does: a callback that reported failure is still in its pre-wake state, so returning to idle would erase which controller is down at the moment the caller was told not to unfreeze. Registration and both walks are then refused with the distinct `PM_CB_DEGRADED` (never plain `PM_CB_BUSY`, or a caller retrying a transient refusal would spin forever), and `pm_cb_table_unrecovered()` names the slots.
  5. `pm_cb_table_recover()` / `pm_power_callback_recover()` is the way out, and it is LOCKED (unlike `pm_cb_table_init()`, which is construction-only and clears the lock word itself). Without it the production singleton had no exit at all -- it is never `pm_cb_table_init()`ed, so one failed wake would have pinned power management for the rest of the boot. Calling it asserts the caller has accounted for the hardware the failed walk left down; the registry cannot know that itself.
- [x] Commit: `"kernel/pm: pm_register_power_callback, sleep/wake ordering, EC resume idle proof"`

> **Verified:** 2026-09-05 | commit `ba2268f03` | 5/6 items | build OK | 33830 kernel + 17 user-mode tests pass
> **Accepted:** [M] a trylock miss on the EC wake returns success, so the idle proof is reported as taken when it was skipped; the honest third outcome ("not proven, nothing broken") needs a pre-sleep admission gate that drains the lock owner while scheduling still runs -> XREF: `02-kernel-core/TODO-26-power-management.md` §24 (item: "EC pre-sleep admission gate, and a \"not proven\" wake outcome for the power dispatcher" at line 1159)
> **Deferred:** [M] `ahci_init`/`xhci_init`/`rtl8139_init`/`fb_init` do not register: none exposes a quiesce primitive, and a no-op storage callback would report the boot disk quiesced while its DMA is live -> XREF: `04-drivers-hardware/TODO-10-usb-stack.md` §1 (item: "Add `suspend(hcd)` / `resume(hcd)` to `usb_hcd_ops_t`" at line 88)
> **Quality reviewed:** 2026-09-05 | Codex 7x (design, adversarial, consistency, perf, re-adversarial x3) | 5H+9M+3L fixed, 1 open | scope: kernel-code-quality
> **Notes:** Dispatch is synchronous and uncancellable, so the 2 s / 5 s budgets are post-return diagnostics and a hung callback hangs the transition with no warning at all -- the header says so rather than leaving a caller to infer a liveness guarantee. A sleep/resume pair is one transaction with two masks: `quiesced_mask` (on_sleep succeeded) drives the abort unwind, `wake_mask` (quiesced plus wake-only) drives the resume. Any failed unwind or failed wake enters the terminal `PM_TXN_DEGRADED`, retains the unrecovered slots and refuses with the distinct `PM_CB_DEGRADED`; `pm_cb_table_recover()` is the locked way out, added because the production singleton is never `pm_cb_table_init()`ed and so had no exit at all. The sleep-abort diagnostic is emitted AFTER the unwind, because klog reaches serial_write whose normal path spins unbounded on UART THRE: a diagnostic ahead of the unwind could let a wedged UART block the recovery it only describes. The residual (klog can stall on a wedged UART at all) is deliberately NOT filed -- it is a property of every klog caller in the tree, not something this section introduced, and the bounded emergency sink already exists and is owned. The EC registers a wake half only -- its sleep half needs GPE control this kernel lacks, and a no-op would have claimed a quiesce that never happened.

**Test checkpoint:** `pm_register_power_callback(PM_PRI_STORAGE, ...)` accepted. Sleep notification iterates in reverse priority (user->storage). Resume iterates forward (storage->user). A callback exceeding its budget is COUNTED and logged (never "skipped" -- see the sleep item above). 64 callback slots max, slot 65 refused. Test on: QEMU TCG.

---

## 10. S0ix Firmware Advertisement and MWAIT Capability Layer

> **Spawned-by:** §10 (split)

Scope was rewritten from the original "Connected Standby" draft after the pre-implementation design review, which found four of the draft's checklist items factually wrong and the system-entry half unimplementable on this tree. What ships here is the half that is real today and can be PROVEN today: firmware advertisement (does the platform claim S0ix) plus the MWAIT capability and hint-selection layer, all of it pure and unit-testable without MWAIT-capable hardware. Everything that actually EXECUTES `MWAIT` is §37, because it shares one unresolved guarantee: nothing in this tree stores to a monitored line when a CPU becomes non-idle, so the wake source for a monitored wait is not yet established.

- [x] `acpi_fadt_s0ix_capable(fadt)` + `acpi_s0ix_supported()` -- FADT `Flags` bit 21 (`LOW_POWER_S0`, ACPI 5.0+), shipped at `src/kernel/acpi.c:2459` and `:2468`, declared `include/kernel/acpi.h:525-526`
  - Length-gated on `header.length >= ACPI_FADT_LEN_FLAGS` before the dword is read; NULL and short tables both refuse. Same two-tier shape as `acpi_fadt_hw_reduced()`.
  - The flag asserts only that S0 idle saves as much as or more than S3. It does NOT assert S3 is absent: a missing `\_S3_` is an OEM namespace fact found by evaluating `\_S3_`, never inferred from this bit. The original draft claimed otherwise and was wrong.
- [x] `CPU_FEATURE_MONITOR` (CPUID.01H:ECX[3]) added to the feature enum and probed in the leaf-1 ECX block
  - Shipped as enum value 65 (`include/kernel/cpuid.h:126`, `CPU_FEATURE_COUNT` 65 -> 66) probed at `src/kernel/cpuid.c:131`.
  - This is the ring-0 `MONITOR`/`MWAIT` gate and is a DIFFERENT instruction family from the existing `CPU_FEATURE_WAITPKG` (`UMONITOR`/`UMWAIT`, ring 3). Neither implies the other; do not conflate them.
- [x] CPUID leaf 5 capability constants at `include/kernel/pm.h:134-138`
  - `PM_MWAIT_LEAF5_ECX_EXT` (`ECX[0]`, extended hints) and `PM_MWAIT_LEAF5_ECX_IRQ_BREAK` (`ECX[1]`, masked-interrupt break), plus `PM_MWAIT_ECX_IRQ_BREAK` for the MWAIT `ECX` operand itself.
  - The EDX sub-state counts are consumed by `pm_mwait_deepest_hint()` rather than exposed as a separate probe: the layer takes the CPUID word as an ARGUMENT, which is what makes every case testable on a host reporting no leaf 5.
- [x] `pm_mwait_hint_encode(cclass, substate)` -- the SDM Table 4-11 EAX encoding `((cclass - 1) << 4) | substate`, shipped at `src/kernel/pm_idle.c:160`
  - MWAIT hint classes are processor-specific, not ACPI C-states, and leaf 5 enumerates only through C7, so a hardcoded `MWAIT_HINT_C10` has no architectural basis and is not used.
- [x] `pm_mwait_deepest_hint(leaf5_ecx, leaf5_edx, out_hint)` -- derives the deepest legal hint from the sub-state counts, shipped at `src/kernel/pm_idle.c:170`
  - Class `n` = 1..7 with count `c` > 0 yields substates `0 <= s < c`, so the deepest index is `c - 1`. C0 is the running state and is never encoded.
  - Gated on `CPUID.05H:ECX[0]` (`PM_MWAIT_LEAF5_ECX_EXT`) FIRST: with that bit clear, `EDX` is not architecturally a table of sub-state counts, so reading nibbles out of it would name states from whatever the register held. Added by the post-ship review.
  - Returns failure when no class reports a non-zero count rather than inventing one, and leaves `*out_hint` untouched on every refusal path.
- [x] `pm_mwait_idle_allowed(if_set, irq_break_supported)` -- the interrupt-state rule, shipped at `src/kernel/pm_idle.c:212`
  - With interrupts masked, a masked interrupt breaks the wait only when the CPU reports `CPUID.05H:ECX[1]` and the caller sets MWAIT `ECX[0]`; without that pairing the predicate returns 0.
  - It is ONE precondition, not the whole permission check: a caller must separately establish `cpu_has(CPU_FEATURE_MONITOR)`, a CPUID max leaf of at least 5, and an armed `MONITOR`. And a 0 means "this caller has not established a wake source it controls", NOT "the CPU could never wake" -- a store into the monitored range, an NMI and an SMI all remain break events. The first draft of this text claimed otherwise and the post-ship review corrected it.
- [x] Commit: `"kernel/pm: S0ix firmware advertisement, CPUID MONITOR gate, MWAIT capability layer"`

**Test checkpoint:** `acpi_fadt_s0ix_capable()` returns 0 for NULL and for a FADT whose `length` is below `ACPI_FADT_LEN_FLAGS`, and reads bit 21 correctly for a synthetic FADT with the bit set and clear. `pm_mwait_hint_encode()` matches the SDM encoding for representative classes. `pm_mwait_deepest_hint()` picks the deepest non-zero class, returns failure on an all-zero `EDX`, and never encodes a substate past its class count. `pm_mwait_idle_allowed()` refuses `IF=0` without masked-interrupt-break and permits it with. Every one of these is a pure function over supplied inputs, so the suite proves them on any host regardless of what the CPU underneath reports. Test on: QEMU TCG.

---

> **Test runner:** `bash scripts/test.sh SUITE=boot` -- 12 new cases (9 `PM: MWAIT *` in `src/kernel/test/test_pm_idle.c`, 3 `ACPI: S0ix *` in `src/kernel/test/test_acpi_power.c`); suite green at 33871 kernel + 17 user-mode tests.

> **Notes:**
> - Ships the S0ix capability layer only: `acpi_fadt_s0ix_capable()`/`acpi_s0ix_supported()` (FADT flags bit 21, length-gated), `CPU_FEATURE_MONITOR`, the leaf-5 capability constants, and the three pure MWAIT hint/refusal functions.
> - The MWAIT layer takes CPUID words as arguments instead of executing CPUID, so the suite proves the SDM Table 4-11 encoding and the refusal rules on hosts whose CPU reports no leaf 5 -- which is every CI host this repo runs on.
> - `CPU_FEATURE_MONITOR` is in the AP probe set, not BSP-only, so `cpu_feature_global_has()` answers the all-CPU question a monitored wait on an AP will need.
> - Nothing here executes `MONITOR` or `MWAIT`; the instruction path and the all-CPU transition are §37, split out because no monitored line in this tree has an established writer.
> - The pre-implementation design review found four of the original draft's items factually wrong (the mwait asm had no MONITOR and no ECX, `MWAIT_HINT_C10` is per-microarchitecture, `MC_PM_STS` has no public contract, and the one-second TSC heuristic is circular); the section text was corrected rather than implemented as written.
> - Canonical doc: Intel SDM Vol. 2B (MWAIT, Table 4-11) and ACPI 5.0+ FADT `Flags` bit 21.
> - Scope boundary: firmware advertisement plus capability selection. It does NOT claim the platform will enter S0ix, and `acpi_s0ix_supported()` is deliberately not sufficient on its own -- the LPS0 `_DSM` gate is §37.
> **Verified:** 2026-09-05 | commit `0c1e8cd08` + review `a073f35ec` | 7/7 items | build OK | 33871 kernel + 17 user tests | smoke matrix 4/4 legs (kvm 1+2 cpu, tcg 1+2 cpu) | lint 0 errors
> **Accepted:** [H] ACPI table discovery checksums over a firmware-declared length and publishes `fadt_ptr` without validating the table's backing extent (pre-existing; this section only added a reader that inherits the pointer, and its own declared-length guard cannot establish an extent) -> XREF: `02-kernel-core/TODO-26` §38 (item: "`find_table_xsdt()` (`src/kernel/acpi.c:183-189`) dereferences each entry pointer and checksums over `hdr->length`" at line 1619)
> **Accepted:** [L] Nothing in the tree reports S0ix capability, the MONITOR gate or the derived hint observably; the readout needs a guarded live CPUID leaf-5 probe, which is more than a log line -> XREF: `02-kernel-core/TODO-26` §18 (item: "`powercfg /a` (`/availablesleepstates`)" at line 818)
> **Quality reviewed:** 2026-09-05 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 3M+8L fixed, 0 open | scope: kernel-code-quality

## 11. Fast Startup (Hybrid Shutdown / Hiberboot)

Windows 11's fast startup hibernates only the kernel session (no user processes) on shutdown, enabling < 5 s boot times by restoring the kernel image instead of cold-booting. This is a significant competitive feature -- Linux has no equivalent.
- [/] `HIBERBOOT_HEADER` -- same layout as `HIBR_HEADER` (§4) but with `type = HIBER_TYPE_FAST_STARTUP` flag to distinguish from full S4 hibernate
- [/] Only kernel session pages are saved: kernel heap, PMM metadata, loaded driver images, Registry hives, VFS cache (no user-process address spaces)
- [/] `pm_hiberboot_page_filter(phys_addr)` -- returns true if the page belongs to kernel session; skips user-mode process pages, reducing image size by 60--80%
- [/] `pm_fast_shutdown()`:
  1. Log off all user sessions (close all user processes; same as normal shutdown)
  2. Flush Registry hives and VFS page cache
  3. Freeze scheduler; park APs
  4. Walk PMM used-page list with `pm_hiberboot_page_filter()` -- compress and write only kernel-session pages to hibernation partition
  5. Write `HIBERBOOT_HEADER` with `type = HIBER_TYPE_FAST_STARTUP`
  6. Power off via `acpi_enter_sleep_state(5)` (S5)
- [/] Registry key `HKLM\SYSTEM\PowerControl\FastStartupEnabled` (REG_DWORD, default 1): enables/disables fast startup
- [/] `powercfg /hibernate on` must be enabled for fast startup to work (reuses hibernation partition)
- [/] Bootloader detects `HIBERBOOT_HEADER` with fast startup flag; sets `boot_info.flags |= BOOT_FAST_STARTUP`
- [/] `pm_fast_startup_resume()` -- same as §4 hibernate resume but skips user-process page restoration
  - Kernel drivers see `IRP_MN_SET_POWER(S0)` with `SystemPowerAction = PowerActionHibernate`, the same as a hibernate wake, so each must call the `PoFxReportDevicePoweredOn()` equivalent.
- [/] After kernel restore: `smss.exe` / session manager starts fresh user sessions from scratch (unlike hibernate where user sessions are restored)
- [/] Distinguish fast startup from hibernate wake: check `HIBERBOOT_HEADER.type`; expose `PoGetSystemPowerStateFlags(FAST_STARTUP)` for drivers
- [/] Commit: `"kernel/pm: fast startup -- hiberboot image, kernel-only page filter, resume path"`

**Test checkpoint:** `HIBERBOOT_HEADER.type == HIBER_TYPE_FAST_STARTUP`. `pm_hiberboot_page_filter()` returns true for kernel heap pages, false for user-process pages. Image size < full hibernate (page filter reduces by 60%+). `PoGetSystemPowerStateFlags(FAST_STARTUP)` distinguishes from full hibernate. Test on: QEMU TCG.

> **Test runner:** N/A (deferred -- no code shipped) | validation: deferred until the blockers below clear

> **Deferred:** every item needs a prerequisite owned elsewhere, all four verified against this tree on 2026-09-05. (1) There is no hibernation WRITE PATH: `pm_hibernate`, `hiber_write` and `blkdev_open_by_gpt_type` return zero matches across `src/` + `include/`. Fast startup is the kernel-session-only variant of exactly that write, so it cannot precede it; §28 owns the write path and is itself deferred on four prerequisites. (2) Bootloader detection and resume entry are not populated: `src/boot/uefi/bootx64.c:18233` states "Resume metadata (S4 hibernation): not populated yet", so no `HIBERBOOT_HEADER` is ever discovered. `BOOT_PATH_FAST_STARTUP` and `BOOT_REASON_FAST_STARTUP_HIT` already exist in the decision table (`src/kernel/main/boot_decision.c:274`), but nothing produces the signal that selects them. (3) There is no session manager: "`smss.exe` starts fresh user sessions" has no owner in this tree, so the step that distinguishes fast startup from hibernate wake cannot be exercised. (4) The kernel image has **306 bytes** of `.rodata` headroom before it crosses `USER_BASE` (`python3 scripts/overnight/bss-headroom.py`, section-exact, 2026-09-05), so a new translation unit of this size cannot link at all; that ceiling is TODO-33 §3 and is deferred on operator decision Q3. The header-type half of item 1 already shipped with §4: `HIBER_RESUME_FAST_STARTUP` is accepted at `src/kernel/pm/hibernate_image.c:67`. -> XREF: `02-kernel-core/TODO-26` §28 (item: "`pm_hibernate_resume()`: read + decompress chunks into a bounce buffer"), `02-kernel-core/TODO-26` §4, `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §3 (item: "Move the LMA to `MM_KERNEL_PHYS_BASE` (`0x200000`) -- NOT the historical `0x100000`"), `01-boot-platform/TODO-26` §2-§5

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
  - The suspend hook must NOT run on the DPC itself: hand off to a threaded DPC or a PASSIVE_LEVEL worker first. `pci_set_d_state()` refuses above PASSIVE_LEVEL with `PCI_DX_IRQL` because it busy-waits up to a second for the recovery interval and readiness, so calling it from an ordinary KDPC callback at DISPATCH_LEVEL would refuse every autosuspend rather than transition anything.
  - → XREF: `02-kernel-core/TODO-26-power-management.md` §8 (item: "`pci_set_d_state(bus, dev, fn, state, &reinit_required)` -- performs the transition and verifies it latched")
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
- [ ] `powercfg /a` (`/availablesleepstates`) -- report which sleep states this platform actually supports, including the S0ix capability line Win11 prints as "Standby (S0 Low Power Idle)"
  - Every input already exists as a pure function: `acpi_s0ix_supported()` for the FADT advertisement, `cpu_feature_global_has(CPU_FEATURE_MONITOR)` for the all-CPU MWAIT gate, and `pm_mwait_deepest_hint()` for the deepest legal hint. What is missing is a guarded live CPUID leaf-5 read (a max-leaf check first: leaf 5 absent returns the highest leaf's values, which is the classic trap) plus the boot-time klog line.
  - This is the tree's only observable for S0ix capability today: grep finds no klog or printk reporting the FADT bit, the MONITOR gate, or a derived hint anywhere. Linux prints the equivalent to `dmesg` from `s2idle.c` and exposes per-state data under `cpuidle` sysfs. -> XREF: `02-kernel-core/TODO-26` §10 (item: "`pm_mwait_deepest_hint(leaf5_edx, out_hint)`" -- shipped, supplies the derivation this readout formats)
- [ ] `powercfg /waketimers` -- print all active wake timers with their expiry times
- [ ] `powercfg /devicequery wake_armed` -- list all devices enabled to wake the system
- [ ] `powercfg /energy` -- 60-second trace of power usage; report idle violations, devices not entering low-power states, excessive timer resolution requests
- [ ] Commit: `"kernel/pm: power request tracking, wake source registry, powercfg diagnostics"`

**Test checkpoint:** `pm_create_power_request(ES_SYSTEM_REQUIRED)` prevents `pm_check_idle_allowed()`. `pm_release_power_request()` re-enables idle. `pm_register_wake_source("test")` succeeds. `powercfg /requests` shows active request. `powercfg /lastwake` reports last source. Test on: QEMU TCG.

---

## 14. ACPI Thermal Zone Management

Implement the OSPM thermal policy engine per ACPI spec chapter 11. This is the kernel-core framework that processes thermal zones from ACPI namespace (`_TZ`), evaluates temperature (`_TMP`), and enforces passive cooling (CPU throttle) and active cooling (fan control) based on trip points.

> [!IMPORTANT]
> **Scope boundary with D04T03§4:** TODO-26 §6 covers per-core MSR-based thermal monitoring (`IA32_THERM_STATUS`, LAPIC Thermal LVT). This section covers the ACPI thermal zone framework that sits above it -- processing `_TMP`/`_CRT`/`_HOT`/`_PSV`/`_ACx` objects and coordinating cooling responses. Both are needed for full parity.
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

Kernel-core governor framework that sits between the scheduler's load metrics and the ACPI/HWP/CPPC frequency control hardware. This is the kernel-core responsibility -- the ACPI `_PSS` parsing lives in D04T03§6; the scheduler's `cpufreq_register_driver()` vtable lives in D03T06§9.

> [!IMPORTANT]
> **Scope boundary:** D04T03§6 parses `_PSS` P-state tables via ACPICA and provides the hardware driver. D03T06§9 provides the scheduler hook. This section owns the policy layer -- governor algorithms, HWP/CPPC native support, and the connection between them.
> → XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md §6` -- `_PSS` P-state hardware driver
> → XREF: `03-memory-concurrency/TODO-06-scheduler-enhancement.md §9` -- `cpufreq_register_driver()` and load metrics
> → XREF: `01-boot-platform/TODO-11-interrupt-timer-arch.md §7` -- LAPIC timer recalibration after frequency change
> → XREF: `02-kernel-core/TODO-09-x86-64-architecture.md §9` -- Intel hybrid P/E-core topology data
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

Kernel-core idle governor that selects the optimal C-state based on predicted idle duration and latency constraints. The C-state hardware interface (`_CST`, `MWAIT`) is owned by D04T03§8; this section owns the idle prediction and selection policy.

> [!IMPORTANT]
> **Scope boundary:** D04T03§8 parses `_CST` and provides `cpuidle_enter(cpu, cstate)`. This section owns the governor that decides *which* C-state to enter. §2 of this TODO provides the basic C1 (`HLT`) idle path and the `pm_deep_idle_allowed()` readiness predicate; this section replaces the fixed C1 choice with a full governor. §2 deliberately does NOT enter S1: that is a system sleep transition, not a CPU idle state.
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
- [ ] BLOCKING PRECONDITION: `pm_deep_idle_allowed()` (§2) is ADVISORY and must not be treated as a synchronisation point by this governor until an interlocked idle-entry protocol exists.
  - `dpc_insert_core()` can target any CPU, taking that queue's lock and incrementing its depth (`src/kernel/sched/dpc.c:586-598`, `:642`), so a DPC can land immediately after the predicate is sampled. §2's read is a relaxed atomic, which makes the READ well-defined but cannot make the ANSWER authoritative.
  - Harmless in §2 because the predicate is only reported and the halt is unconditional -- a plain `HLT` wakes on the next interrupt regardless. It stops being harmless HERE, where a stale "ready" would select a state with a real exit latency and delay queued work, and there is no DPC wake IPI to cut it short.
  - What is owed before this governor may select deeper than C1: a cross-CPU enqueue must observe that the target is entering idle and send a wake IPI, and the SMP remote-enqueue case needs a test. -> XREF: `02-kernel-core/TODO-26` §2 (item: "`pm_deep_idle_allowed()` -- the readiness predicate")
- [ ] Kernel-debugger interaction: when `kd_present` is true, never select a C-state deeper than C1 while `kd_breakin_requested` is set, and call `kd_poll()` before descending, so WinDbg breakin bytes are not delayed behind a deep-sleep exit latency.
  - Received here 2026-09-03 from `02-kernel-core/TODO-29` §15's mirror item, which had been aimed at §2 and no longer described anything §2 does: §2's C1 halt is unconditional by design and a plain `HLT` wakes on the COM IRQ regardless, so there is nothing to suppress until a state with a real exit latency can be chosen. -> XREF: `02-kernel-core/TODO-29-kernel-debugger-kd-protocol.md` (item: "Mirror `TODO-26-power-management.md §16`")
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
    - Reconcile the two homes rather than adding a second source of truth: §7 already reads and caches `HKLM\SYSTEM\PowerControl\PowerButtonAction` once at init, and this section plans a per-plan copy. State the precedence, migrate or subsume the flat key, and decide whether the Windows AC/DC split (`ACSettingIndex` / `DCSettingIndex`) is in scope -- a laptop cannot currently hibernate on battery and sleep on AC. -> XREF: `02-kernel-core/TODO-26` §7 (item: "The policy is sampled ONCE, before the PM1 enable bits are written, and cached")
  - `LidCloseAction` (REG_DWORD): same codes as §7 -> XREF: `02-kernel-core/TODO-26` §31
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
> **Scope boundary:** D02T09§9 detects Intel hybrid P/E-core topology and Intel Thread Director (ITD) / Hardware Feedback Interface (HFI). D03T06§9 provides the scheduler's load metrics. This section integrates those signals with the CPU frequency governor (§15) to make energy-aware placement decisions.
> → XREF: `02-kernel-core/TODO-09-x86-64-architecture.md §9` -- P/E-core detection, HFI capability data
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

Register all power management NtXxx entry points in the SSDT so user-mode code can invoke them via `syscall`. See TODO-12-native-api-ssdt.md §4 (SSDT layout) and §22 (power syscalls).

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

## 22. User-Interaction-Aware QoS Throttling

> **Spawned-by:** §16 (review)
> **User impact:** without this, background-idle machine time (compiles, benchmarks, servers) gets throttled by any HID-idle heuristic that is not scoped to the foreground app; with the wrong model, an unattended workload loses performance for no reason. Windows 11 25H2 ships this as a per-process QoS demotion, not a global cap, and Impossible OS needs the same scoping to be correct.

Windows 11 25H2 lowers the FOREGROUND application's QoS to Medium after a period with no keyboard/mouse/touch input, on battery; QoS then feeds scheduler placement and per-thread frequency policy, with a documented opt-out (`DisableUserPresenceQos`). No Linux equivalent (`power-profiles-daemon` and TLP tune per-workload, not per-input-presence). This is a gap-audit correction: an earlier draft of this item (filed in §16, since removed) modeled it as biasing every CPU's C-state/P-state globally regardless of load, which would throttle an unattended compile or benchmark just because nobody touched a HID device.

- [ ] `pm_user_presence_state_t`: tracks time since last keyboard/mouse/touch event system-wide (not per-window)
- [ ] `PowerHidIdleThresholdMs` (Registry, default matches Win11's own broadcast interval): past this threshold AND on battery, mark user-presence ABSENT
- [ ] On ABSENT: reclassify the FOREGROUND process's QoS to Medium via the existing power-QoS handle mechanism (§20 syscalls); background processes are UNAFFECTED
- [ ] QoS Medium feeds §19's `select_cpu()` (prefer E-cores) and §15's per-thread frequency ceiling for that process's threads only -- never a global cap
- [ ] On HID event or presence returning: revert the foreground process's QoS immediately
- [ ] `DisableUserPresenceQos` (Registry, default 0): documented opt-out, matching Win11's own escape hatch
- [ ] AC-only systems and non-foreground work are never throttled by this mechanism -- test explicitly proves a background high-load thread's frequency is unaffected while foreground QoS is demoted
- [ ] Commit: `"kernel/pm: user-interaction-aware QoS throttling -- foreground-only, battery-only, DisableUserPresenceQos"`

**Test checkpoint:** `pm_user_presence_state_t` transitions to ABSENT after `PowerHidIdleThresholdMs` on battery with no HID events. Foreground process QoS demotes to Medium; a background thread's measured frequency ceiling is UNCHANGED (proves no global cap). A HID event immediately reverts QoS. `DisableUserPresenceQos=1` disables the whole mechanism. Test on: QEMU TCG.

---

## 23. PCIe ASPM and L1 Substates

> **Spawned-by:** §12 (review)
> **User impact:** without link-level PCIe power management, every PCIe endpoint (NVMe, Wi-Fi, most modern devices) keeps its link at full power even when the device itself reaches D3/APST/ALPM -- the device-level work in §12 caps out well short of the idle power Win11 and Linux both reach, and NVMe's own APST guidance assumes L1 substates are available underneath it.

Codex gap-audit finding (2026-09-03): §12 covers device D-states, USB LPM, NVMe APST, and SATA ALPM, but has no owner for the PCIe LINK itself -- ASPM L0s/L1 and the L1.1/L1.2 substates that PCI-SIG describes as enabling dramatically lower idle link power, and that Microsoft's own NVMe power guidance depends on. Only the FADT `NO_ASPM` flag is referenced anywhere in the repo today.

- [ ] `pci_aspm_cap_find(dev)` -- walk PCI Express Capability (cap ID `0x10`) for the Link Control/Link Capabilities registers; return offset or -1 if the device is not PCIe
- [ ] Respect FADT `NO_ASPM` (platform firmware disables ASPM entirely) before touching any link
- [ ] `pci_aspm_set_policy(dev, policy)` -- `ASPM_DISABLED`/`ASPM_L0S`/`ASPM_L1`/`ASPM_L1SS`; writes Link Control ASPM Control bits on BOTH the endpoint and its upstream root/switch port (ASPM is a link-level agreement, not per-device)
- [ ] L1 PM Substates (L1.1/L1.2) via the L1 PM Substates Extended Capability, gated on `CLKREQ#` support advertised by both link partners
- [ ] Latency-aware policy: read each function's `_DSM`/Latency Tolerance Reporting (LTR) where present; do not enable a substate whose exit latency exceeds the device's tolerated latency
- [ ] Power plan mapping: `PowerSaver`=L1SS enabled, `Balanced`=L1 only, `Performance`=ASPM disabled (mirrors §12's SATA/USB policy mapping)
- [ ] Resume ordering: restore each link's ASPM policy AFTER the device itself reaches D0 (§9), never before -- an armed link on a not-yet-ready device can stall config-space access
- [ ] Safe refusal: if either link partner's capability register disagrees with what firmware advertised, leave ASPM at its firmware-configured default rather than guessing
- [ ] Boot log: `[ASPM] %02x:%02x.%x: L0s=%s L1=%s L1SS=%s`
- [ ] Commit: `"kernel/pci: ASPM L0s/L1 + L1 PM Substates, latency-aware policy, power-plan mapping"`

**Test checkpoint:** `pci_aspm_cap_find(dev)` returns a valid offset for a PCIe device (or -1 for legacy PCI). `pci_aspm_set_policy(dev, ASPM_L1)` sets Link Control bits on both endpoint and upstream port. `NO_ASPM` firmware flag disables the whole mechanism. A device advertising no `CLKREQ#` is never offered L1SS. Test on: QEMU TCG + WHPX.

---

## 24. ACPI General-Purpose Event (GPE) Blocks

> **Spawned-by:** §1 (review)
> **User impact:** On real hardware most of what raises the SCI is a GPE, not one of the PM1 fixed events §1 services. Every GPE-raised SCI is therefore never acknowledged, and because the line is level-triggered it stays asserted: on the IOAPIC path `irq_shared_dispatch_wrapper` quarantines and permanently masks the GSI after `IRQ_STORM_ALLNONE_LIMIT` (1000) consecutive all-NONE dispatches (`src/kernel/irq.c:92`, `:511-513`), so the power button, lid switch and every EC event die for the rest of that boot; on a PIC-only machine there is no storm protection at the IDT layer at all and the CPU livelocks. QEMU/OVMF raise no GPEs, so none of this is visible under emulation.

- [ ] Extend `struct acpi_fadt` with the GPE fields, offsets pinned by `_Static_assert` as `x_dsdt` already is
  - `gpe0_block` / `gpe0_block_length` / `gpe0_base`, `gpe1_block` / `gpe1_block_length` / `gpe1_base`, plus the `X_GPE0_BLK` / `X_GPE1_BLK` GAS forms.
- [ ] `acpi_gpe_init()` -- split each block into its status and enable halves
  - `block_length / 2` bytes each, one bit per GPE.
  - Reject a length that cannot be halved, the way `pm1_en_port()` already rejects `PM1_EVT_LEN < 4`, and clear every status bit before enabling anything.
- [ ] Byte-wide register accessors: a GPE block is an array of bytes, not the 16-bit words PM1 uses, so `inw_acpi` / `outw_acpi` do not apply
- [ ] Service GPEs in `acpi_sci_process()` alongside the PM1 fixed events
  - Read both status halves, acknowledge every asserted-AND-enabled bit, and count them per GPE number.
  - The ISR stays free of logging and AML evaluation for the reason that function already documents.
- [ ] Acknowledge a GPE that has no handler yet rather than leaving the line asserted
  - This is the whole point of splitting the hardware layer from the AML layer: it keeps the storm quarantine from firing while the dispatch half is still unbuilt.
- [ ] Deferred `_Lxx` (level) / `_Exx` (edge) method dispatch at thread level, keyed by GPE number
  - Needs the AML evaluator. -> XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md` §1 (item: "`acpi_evaluate(path, args, result)` wrapper around `AcpiEvaluateObject`")
- [ ] `acpi_gpe_enable(n)` / `acpi_gpe_disable(n)` so a driver arms only the events it services -- the same "never enable what you will not acknowledge" rule `acpi_enable_fixed_events()` now follows for PM1
- [ ] EC pre-sleep admission gate, and a "not proven" wake outcome for the power dispatcher
  - `acpi_ec_on_wake()` (§9, `src/kernel/drivers/acpi_ec.c`) takes `g_ec_lock` with `mutex_trylock` because the resume walk runs with the scheduler frozen, and on a miss it returns SUCCESS. That is the least-wrong answer available today but it is not a true one: the proof was skipped, so the EC is reported as having come back when nobody established that. Returning failure instead is worse -- the dispatcher escalates any failed wake to a terminal `PM_TXN_DEGRADED`, so a transient contention would disable power management for the rest of the boot.
  - Two halves fix it properly, and both need this section's GPE control: (1) a pre-sleep admission gate that refuses new EC transactions and DRAINS the current `g_ec_lock` owner while scheduling still runs, so the wake half can take the lock outright; (2) a third dispatcher outcome meaning "not proven, nothing broken", distinct from both success and a device that failed to return.
  - -> XREF: `02-kernel-core/TODO-26-power-management.md` §9 (item: "EC-specific sleep/resume handling, registered like any other driver callback.")
- [ ] Give the EC driver a controller-reset boundary, then reconsider its terminal desync policy. -> XREF: `02-kernel-core/TODO-26-power-management.md` §5 (item: "Recovery from a desynchronized controller")
  - §5 treats the first post-command timeout as fatal for the rest of the boot, because proving the controller idle at an instant cannot prove a late response will not arrive. Linux `ec_poll()` instead retries a stalled transaction up to five times with a fresh delay window before giving up.
  - Terminal is the right default while nothing can recover the controller; once a reset exists, a bounded retry BEFORE declaring desync becomes answerable. Without it, one transient SMI or firmware-busy moment permanently kills battery, thermal and lid reporting for that boot.
- [ ] Open the EC gate once GPE acknowledgement works. -> XREF: `02-kernel-core/TODO-26-power-management.md` §5 (item: "EC hardware access is GATED OFF")
  - Replace `ec_gpe_ack_supported()` in `src/kernel/drivers/acpi_ec.c` so `acpi_ec_ready()` can return true, and mask/ack the ECDT GPE around polled EC traffic.
  - §5 discovers and validates the EC and implements the full polled transaction engine, then deliberately refuses to drive it: RD_EC/WR_EC/BE_EC/BD_EC all raise the EC's GPE, so without acknowledgement the first transaction strands a level-triggered SCI and this section's own storm-quarantine consequence fires.
  - The ECDT publishes the EC's GPE bit and `acpi_ec_get_ports()` reports it, so the number this needs is already parsed and available.
  - Also unblocks §5's interrupt-driven QR_EC query path and gives its terminal desync flag a reset boundary to recover through.
- [ ] Wake-source attribution: record WHICH GPE resumed the machine, so wake reporting has a real source instead of the bare `PM_WAKE_GPE` tag §13 carries today. -> XREF: `02-kernel-core/TODO-26-power-management.md` §13
- [ ] PM1b-only platform support, END TO END
  - `acpi_pm1_control_owned()` already reads SCI_EN from both control blocks ORed (matching `AcpiHwReadMultiple`).
  - But `acpi_enable_fixed_events()` returns early without a PM1a EVENT block and `acpi_sci_process()` returns without a PM1a status port.
  - So such a machine could enter S1 and then never acknowledge or count its wake -- a false resume failure with the level-triggered SCI left asserted. `acpi_enter_sleep_state()` therefore REQUIRES PM1a today, deliberately.
  - Work: initialise and service either event block independently, require at least one valid event block rather than PM1a specifically, and drop the PM1a-only early return from the SCI path.
  - Filed from the section-1 review (round 7), 2026-09-03: it is a feature, not a review fix, and half-supporting it is worse than requiring PM1a.
- [ ] Unit tests over synthetic GPE block images through a test-only entry point
  - Same shape as `acpi_parse_sleep_type_test`.
  - Cover the status/enable split, an asserted-but-unenabled bit left alone, an unhandled GPE still acknowledged, and a block length that cannot be halved.
- [ ] Commit: `"kernel/acpi: GPE block enable, dispatch, and wake-source attribution"`

**Test checkpoint:** a GPE block splits into equal status/enable halves and a non-halvable length is refused; an asserted-and-enabled GPE bit is acknowledged exactly once; an asserted-but-disabled bit is untouched; a GPE with no registered handler is still acknowledged so the SCI line drops. Test on: QEMU TCG (raises no GPEs -- structural tests only), bare metal (the only place the real path is exercised).

> **Notes:**
> - **Why this is a new section rather than an item somewhere:** `grep -rn GPE todo/` finds two passing mentions and no owner -- §31 assumes "ACPI GPE fires when lid state changes" and §13 carries a bare `PM_WAKE_GPE` enum tag -- and `04-drivers-hardware/TODO-03` has no GPE mention at all across its ten sections. Nothing owns block discovery, enable, or acknowledgement, so this is ownerless work, not a duplicate of existing coverage.
> - **How it was found:** independently by `kernel-quality-auditor` (which traced the storm-quarantine consequence to `irq.c:511-513`) and `parity-research-analyst` (which identified GPEs as the channel Linux and Windows actually use for EC, lid, dock and wake) during §1's post-ship review, 2026-09-03.
> - **Scope boundary:** this section owns the GPE HARDWARE layer only -- block discovery, the status/enable registers, acknowledgement, and per-GPE counts. The `_Lxx`/`_Exx` AML evaluation layered on top belongs to the ACPICA integration. -> XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md` §1

---

## 25. Per-CPU Idle-Time Accounting via `NtQuerySystemInformation`

> **Spawned-by:** §2 (split)

Split out of §2 at implementation time: §2 owns the C1 idle primitive and the per-CPU `idle_tsc_cycles` counter that feeds this; this section owns EXPOSING that counter through the NT information-class surface, which is a different subsystem (`src/kernel/nt/nt_syscall.c` dispatch and user-facing marshalling) with a different failure mode (an ABI shape and a class number that must match Windows, not an SMP-safety question). Neither half is useful without the other, but they are reviewed against different things. -> XREF: `02-kernel-core/TODO-26` §2 (item: "`uint64_t idle_tsc_cycles` appended at the TAIL of `struct per_cpu_data`").

- [ ] Verify the numeric `SystemProcessorIdleInformation` class value against an authoritative Windows/ReactOS source BEFORE picking one, and cite what was checked:
  - The neighbouring classes in `src/kernel/nt/nt_syscall.c` are real Win32 values rather than arbitrary local numbers, so a guessed value is a silent parity break that no build, test or boot would catch.
  - The class is not defined anywhere in the tree today, so there is no existing value to be consistent with.
- [ ] `SYSTEM_PROCESSOR_IDLE_INFORMATION` -- one fixed-size entry per processor, Win32-parity field order and widths, populated from each CPU's `idle_tsc_cycles` (§2).
- [ ] BLOCKING PRECONDITION: do NOT expose the counter until §2's accounting measures halted cycles rather than halt-to-frame-resumption
  - Today it also includes the waking ISR, and on the BSP possibly another thread's quantum, so exporting it now would ship a Win32-visible value wrong by up to a scheduling quantum. -> XREF: `02-kernel-core/TODO-26` §2 (item: "`pm_idle_c1()` in `src/kernel/pm_idle.c`")
- [ ] Add the `case` to the `NtQuerySystemInformation()` dispatch switch in `src/kernel/nt/nt_syscall.c`
  - Follow the existing probe-then-`copy_to_user` pattern: size-check against `buf_size`, write `*return_length`, and return `STATUS_INFO_LENGTH_MISMATCH` on a short buffer rather than a partial copy.
- [ ] Bound the entry count by `MAX_CPUS` and filter with `smp_cpu_is_online()`, NOT by `smp_cpu_count()`: logical CPU slots are sparse, so a count is never a valid slot bound (CLAUDE.md "A CPU COUNT is never a slot bound").
- [ ] Take ONE `smp_online_mask()` snapshot for the whole reply, so the entry count and the entries themselves cannot disagree with each other when a CPU parks mid-call.
- [ ] `struct per_cpu_data.irq_count` is DEAD and must be either driven or deleted before any information class reports interrupt counts:
  - It is zeroed at `src/kernel/smp/smp.c:125`, `:260`, `:375` and `:399` and incremented NOWHERE in the tree (verified by grep across `src/kernel/`, 2026-09-03), so it reads a constant 0 on every CPU while looking exactly like a maintained counter.
  - Found while implementing §2: it was used as the "did this CPU actually halt" probe in `src/kernel/test/test_pm_idle.c` and both halt assertions failed against it, which is the only reason it surfaced. It was NOT replaced by `system_get_ticks()` -- that was tried next and rejected as non-causal in both directions. The oracle that shipped is an exact-address check on a global label emitted at the halt site, plus a test-only reachability counter.
  - The cost of leaving it is that the next consumer to reach for it, most likely this section or a `SystemProcessorPerformanceInformation` sibling, ships a Win32-visible zero rather than a count. Its owning TODO (`01-boot-platform/TODO-10-bare-metal-hardening.md`) is stamped DONE, so the item is filed here where it is reachable, against the section that would consume it.
- [ ] Commit: `"kernel/nt: SystemProcessorIdleInformation -- per-CPU idle time via NtQuerySystemInformation"`

**Test checkpoint:** the class returns `STATUS_SUCCESS` with `*return_length` equal to `entries * sizeof(SYSTEM_PROCESSOR_IDLE_INFORMATION)`; a short buffer returns `STATUS_INFO_LENGTH_MISMATCH` and copies nothing; reported idle cycles are monotonic across two consecutive calls. `bash scripts/test.sh SUITE=boot` green. Test on: QEMU TCG + KVM, 1 and 2 CPUs.

---

## 26. Stop-the-World CPU Rendezvous for Sleep Transitions

> **Spawned-by:** §3 (split)

The resumable, generation-tagged barrier that every system-sleep transition is built on, split out of §3 because it is the one deliverable in that section blocked on nothing, and because its failure mode (SMP liveness, lock order, memory ordering) is the opposite of the rest of §3 (ACPI table programming and firmware handoff). §2 recorded the requirement with no owner; this section is that owner. -> XREF: `02-kernel-core/TODO-26` §2 (item: "Quiesce the APs before any SLP_EN write. BLOCKED: no AP park/rendezvous facility exists")

- [x] `smp_rendezvous_begin(uint32_t timeout_ms)` in `src/kernel/smp/smp.c`, declared in `include/kernel/smp.h`: BSP-only, single-flight, returns 0 only once every OTHER online CPU has acknowledged; RETURNS WITH INTERRUPTS MASKED
  - Take ONE `smp_online_mask()` snapshot as the target set and hold it for the whole rendezvous. `smp_cpu_count()` is LIVE and falls as CPUs park, so a popcount can never be the completion test, and `smp_retract_cpu_online()` (`src/kernel/smp/smp.c:873`) clears a bit before the CPU has stopped executing.
  - Bump a global generation counter BEFORE the IPI goes out, and have each CPU acknowledge WITH the generation it observed, so a late acknowledgement from a previous round cannot satisfy this one.
  - Close CPU admission for the duration: a CPU publishing its online bit while the barrier is closing would be a target that was never signalled.
  - Fail CLOSED on timeout: release every CPU already parked, reopen admission, return non-zero. A partial rendezvous reported as success is the hazard the whole section exists to prevent.
- [x] `smp_rendezvous_end(void)` in `src/kernel/smp/smp.c`: releases the barrier and restores the owner's entry interrupt state; refuses any CPU that did not arm the round
  - Every parked CPU resumes where it spun with its entry interrupt state restored. RESUMABLE is the point: `smp_test_park_cpu()` (`src/kernel/smp/smp.c:925`) parks the bookkeeping, not the CPU, has no unpark path, and is `KERNEL_TESTS`-only.
  - Publish with a release store and have the AP side observe with an acquire load, so writes the initiator made before the release are visible to the resuming CPU.
- [x] The AP-side barrier handler `rendezvous_ipi_handler()` in `src/kernel/smp/smp.c`, on new vector `VECTOR_IPI_RENDEZVOUS` 0xF9 (`include/kernel/vectors.h`): EOI, then spin with interrupts disabled
  - It runs in hard-IRQ context with the world stopping around it, so it must not allocate, must not call `klog()`, and must not take any lock another CPU could be holding when it parked.
  - The acknowledgement is a release store of the observed generation; the spin is an acquire load, with a pause hint in the loop.
- [x] The DEADLOCK CONTRACT is stated at the declaration (`include/kernel/smp.h`): thread context, no spinlock held, and no lock/allocation/klog/callback/synchronous cross-CPU operation inside the window
  - Without this stated the primitive is a deadlock generator: any lock a parked CPU holds is held for the entire barrier, so a caller that then takes it wedges the machine with interrupts off.
- [x] 28 tests in `src/kernel/test/test_smp_rendezvous.c` (`TEST_CAT_X86`), registered in `src/kernel/test/test_runner.c`: 27 pure-protocol cases plus one LIVE begin/end round trip
  - Prove the generation tag rejects a stale acknowledgement, that a timeout leaves no CPU parked and admission reopened, that a non-BSP caller is refused, and that a second concurrent begin is refused rather than corrupting the target set.
  - Single-CPU behaviour is a real case, not a skip: with no other online CPU the rendezvous succeeds immediately and `end` must still be balanced.
- [x] Commit: `"kernel/smp: resumable generation-tagged stop-the-world CPU rendezvous"`

**Test checkpoint:** what the 28 tests ACTUALLY exercise, stated narrowly because the first draft of this line claimed more than they prove. Over caller-supplied state: a stale-generation acknowledgement never completes a round; a late acknowledgement published after a re-arm is ignored; only the owner can release; an empty target set completes immediately; the generation advances past `0xFFFFFFFF` rather than wrapping; `park_step` acknowledges a generation CHANGE and not every iteration. Live, once: `begin()` stops the world and `end()` restarts it, interrupts are masked inside the window and restored exactly after, and the online mask is identical before and after -- and on a uniprocessor boot that case SKIPS rather than passing vacuously, so the log distinguishes a proving run from a trivial one. NOT proven here, and owned by §27: the `begin()` timeout branch, the handler's `lapic_eoi()`, the split-snapshot re-read and the admission refusal are all mutation survivors. `bash scripts/test.sh SUITE=x86` green. Test on: QEMU TCG + KVM, 1 and 2 CPUs (`scripts/test-smoke-matrix.sh`).

> **Test runner:** `scripts/debug/kernel/run-x86-tests.bat` -- expect 152 x86 suites, 371 assertions, 0 failures, including `smp_rendezvous: LIVE begin/end stops and restarts the world`.
> **Verified:** 2026-09-04 | commit `383b32805` + review fixes | 6/6 items (the review residues are filed as §27, not parked here, because a park into a section being stamped is never revisited) | build OK | 33054 kernel + 17 user-mode tests, 0 failures | x86 152 suites / 373 assertions | smoke matrix 4/4 (TCG+KVM x 1+2 CPU) | lint rc=0 | release-flavor compile clean with `KERNEL_TESTS` undefined | live serial shows `stop-the-world rendezvous armed (vector 0xf9)`
> **Deferred:** [M] the `begin()` timeout branch, the handler's `lapic_eoi()`, the `park_step` split-snapshot re-read and the admission refusal are MUTATION SURVIVORS -- each runs only on a path driven by the live clock or the live LAPIC, which a test may not drive -> XREF: 02-kernel-core/TODO-26 §27 (item: "Route the owner's wait through an injectable clock and the IPI send through an injectable dispatcher")
> **Deferred:** [M] a CPU retracting through the panic path clears its online-mask bit before it stops executing, so a snapshot taken in that window omits a running CPU; reachable only from an already-panicking machine -> XREF: 02-kernel-core/TODO-26 §27 (item: "Give a retracting CPU a terminal-quiescent publication point the rendezvous snapshot can see")
> **Deferred:** [H] nothing calls the barrier: `acpi_enter_sleep_state()` still writes PM1 SLP_EN with other processors live, so the primitive shipped but the hazard §2 recorded is unchanged in behaviour -> XREF: 02-kernel-core/TODO-26 §27 (item: "Bracket every SLP_EN write with the §26 rendezvous at a deadlock-safe point")
> **Accepted:** [L] the set of STOPPED CPUs can be a strict superset of the target set, because `park_step` parks on "a round is open" rather than "I am targeted" (reason: safe by construction -- completion reads targeted slots only -- and now stated in the header contract rather than left implicit) -> XREF: 02-kernel-core/TODO-26 §27 (item: "Route the owner's wait through an injectable clock and the IPI send through an injectable dispatcher")
> **Quality reviewed:** 2026-09-04 | Codex 8x (design, adversarial x3, test-coverage x2, consistency, perf, re-adversarial x2) + kernel-quality-auditor + concurrency-evidence-mapper | 5H+16M+6L fixed, 2 accepted, 3 deferred | scope: kernel-code-quality all 10 gates walked by the Opus auditor
> **Deferred:** [M] the timeout, EOI, split-snapshot and admission-refusal paths are MUTATION SURVIVORS -- deleting any of the four leaves the suite green, because each only runs on a path driven by the live clock or the live LAPIC and a test may not drive those -> XREF: `02-kernel-core/TODO-26` §27 (item: "Route the owner's wait through an injectable clock and the IPI send through an injectable dispatcher")
> **Deferred:** [M] a CPU retracting through the panic path clears its online-mask bit before it stops executing, so a snapshot taken in that window omits a running CPU; reachable only from an already-panicking machine, which is why it is parked and not a ship blocker -> XREF: `02-kernel-core/TODO-26` §27 (item: "Give a retracting CPU a terminal-quiescent publication point the rendezvous snapshot can see")
> **Notes:**
> - **What shipped:** a resumable generation-tagged barrier -- `smp_rendezvous_begin/end/in_progress` plus the pure protocol (`round_active/arm/set_targets/ack/complete/release/park_step`) in `src/kernel/smp/smp.c`, vector 0xF9 in `include/kernel/vectors.h`, and 28 tests in `test_smp_rendezvous.c`.
> - **How it integrates:** armed from `smp_init()` on BOTH the SMP and the single-CPU path; APs take the vector from their existing `pm_idle_c1()` park loop; `smp_publish_cpu_online()` now returns a verdict and the AP bringup caller abandons an AP whose publication is refused.
> - **The protocol correction that shaped it:** acknowledgement is a per-CPU 64-bit GENERATION, never a bit in a shared mask, because a mask lets a CPU that stalled through a timeout and re-arm satisfy the next round while still running; the counters are 64-bit because a 32-bit wrap re-creates that exact ABA at `UINT32_MAX`.
> - **The owner runs the window with interrupts masked:** a preemptible owner could be switched out by its own LAPIC timer with every AP already parked, leaving nothing running to observe the timeout or release them.
> - **Downstream:** this closes §2's parked AP-quiesce item and is the prerequisite §3 was deferred on; nothing calls it yet, which is why the one LIVE test exists -- the smoke matrix alone would only prove the handler registers.
> - **Ordering that review forced:** the owner ARMS FIRST with an empty target set and names its targets afterwards, because closing admission after the snapshot leaves a window where a CPU joins between the two and the round completes while it runs; the timeout is also sampled before the IPI sends, which can each spin a million ICR polls.
> - **Scope boundary:** the barrier only. Saving CPU state, the firmware waking vector, and the SLP_EN write stay with §3; this section adds no ACPI code.

---

## 27. Rendezvous Safety Residue: Test Seam and the Retract-Side Window

> **Spawned-by:** §26 (review)
> **User impact:** a machine that hangs or drops interrupts when it is put to sleep, with nothing in the test suite that would have caught it. Four safety paths in the barrier §26 shipped are MUTATION SURVIVORS -- deleting the fail-closed release, the handler's `lapic_eoi()`, the split-snapshot re-read, or the admission refusal leaves the whole suite green -- and separately a CPU retracting through the panic path can be omitted from a snapshot while it is still executing, which is the one remaining way `smp_rendezvous_begin()` can report a stopped world that is not stopped.

Both residues are §26's own surface, filed here rather than parked into §26 because a park into a section being stamped is stranded work: the fixpoint loop never revisits a DONE section. -> XREF: `02-kernel-core/TODO-26` §26 (item: "`smp_rendezvous_begin(uint32_t timeout_ms)` in `src/kernel/smp/smp.c`, declared in `include/kernel/smp.h`")

- [ ] Route the owner's wait through an injectable clock and the IPI send through an injectable dispatcher, so a caller-owned fake can drive a PARTIAL TIMEOUT deterministically
  - The four survivors all share one cause: each runs only on a path driven by the live clock or the live LAPIC, and a kernel test may drive neither. More assertions cannot reach them; a seam can.
  - Keep the seam pure and caller-supplied, matching how the rest of the protocol is already testable: no global hook, no `#ifdef KERNEL_TESTS` branch inside the shipping path.
  - This is a dependency-injection change to a primitive that stops every CPU, so it wants its own design review rather than being bolted onto §26.
- [ ] `test_rv_begin_timeout_releases_partial_round`: one target silent, assert -1, `generation == released_gen`, every acknowledged target's next `park_step` returns 0, interrupts restored, and another round can arm
  - Today `test_rv_timeout_release_leaves_nobody_parked` performs the cleanup it claims to verify: it calls `smp_rendezvous_release()` itself and never enters the timeout branch, so deleting the fail-closed release in `smp_rendezvous_begin()` does not fail it.
- [ ] `test_rv_park_step_release_rearm_between_loads`: pause after the first generation read, release and re-arm, resume, assert the CPU stays parked and acknowledges the new generation
  - The existing split-snapshot test performs both transitions BEFORE calling `park_step`, so its first load already sees the new generation and the second re-read is never exercised.
- [ ] `test_rv_active_round_refuses_publication_without_mutation`: assert an open round leaves claim, `is_online` and the online mask byte-identical, and that publication succeeds after release
  - Nothing exercises the admission backstop: `test_smp_lifecycle.c` deliberately never calls `smp_publish_cpu_online()`, so deleting the active-round refusal escapes the suite entirely.
- [ ] Strengthen the single live test to prove AP RESUMPTION and reusable IPI wiring, not just that the BSP survived
  - `smp_cpu_count() >= 1` proves only that the BSP is still counted. Deleting `lapic_eoi()` from the handler leaves vector 0xF9 in-service and still passes every current assertion.
  - Preserve the exact pre-round online mask, run two consecutive rounds, and observe bounded evidence that each targeted AP executed after each release. Stay within ONE live test: a second one doubles the wedge surface for no extra proof.
- [ ] Give a retracting CPU a terminal-quiescent publication point the rendezvous snapshot can see
  - `smp_retract_cpu_online()` clears the online-mask bit BEFORE the CPU stops, and its panic caller keeps doing shared-state, serial and evidence work afterwards, so a snapshot taken in that window omits a CPU that is still running.
  - The publish-side twin of this was closed in §26 by arming before snapshotting; the retract side cannot be fixed the same way, because the mask bit is cleared deliberately early so no consumer counts a dying CPU as live.
  - Reachable only from an already-panicking machine, which is why §26 shipped without it -> XREF: `01-boot-platform/TODO-10-bare-metal-hardening.md` (live CPU online lifecycle / park)
- [ ] Park every other CPU before the shutdown storage quiesce, then halt them all if firmware power-off fails
  - `acpi_shutdown()` (`src/kernel/acpi.c:1769`) runs `acpi_storage_quiesce()` and then `acpi_poweroff_now()`, both on the calling CPU only. Another CPU can submit I/O while storage is being quiesced, and if the firmware power-off silently fails the other CPUs keep running against dead storage.
  - Filed 2026-09-04 from the §7 design review, which found the hazard while reviewing the power-button dispatcher. It is NOT specific to the button: `SYS_SHUTDOWN` (`src/kernel/sched/syscall.c:1036`), `nt_syscall.c:1591` and the desktop power menu (`src/desktop/desktop.c:1082`) all take the same path today.
  - Single-entry admission is part of it, not a separate job: the button DPC, `SYS_SHUTDOWN`, `nt_syscall.c` and the desktop power menu can all enter the same unsynchronized quiesce concurrently, so a second initiator can dirty storage after the first marked it clean.
  - Also decouple the outcome from the caller: `acpi_poweroff_now()` halts only the calling CPU, and for a button press that CPU is the single global threaded-DPC worker, so a machine where `\_S5` and both port fallbacks fail is left running with storage quiesced AND every other threaded DPC permanently dead.
  - The §26 rendezvous is the primitive; this is the same bracket the SLP_EN item below needs, applied to the S5 path. -> XREF: `02-kernel-core/TODO-26` §7 (item: "Route the shutdown action through an SMP-safe shutdown once one exists")
- [ ] Bracket every SLP_EN write with the §26 rendezvous at a deadlock-safe point
  - §26 shipped the barrier and NOTHING calls it: `acpi_enter_sleep_state()` still writes PM1 SLP_EN with other processors live, so the hazard §2 recorded is unchanged in behaviour even though the primitive it needs now exists. A shipped primitive with no caller closes no hazard.
  - The bracket must go where the contract allows: callbacks, device quiesce and any cache or journal flush BEFORE `smp_rendezvous_begin()`, with the barrier replacing the bare `cli` immediately before the hardware transition.
  - Applies to every sleep path, not only S3: S1 goes through the same function today and inherits the same gap -> XREF: `02-kernel-core/TODO-26` §3 (item: "`pm_enter_s3()` -- runs on a dedicated `PASSIVE_LEVEL` worker, NOT `DISPATCH_LEVEL`")
- [ ] Commit: `"kernel/smp: rendezvous test seam, mutation-proving tests, retract-side quiescence"`

**Test checkpoint:** deleting any one of the four named lines (the fail-closed release, `lapic_eoi()` in the handler, the second generation re-read in `park_step`, the active-round refusal in `smp_publish_cpu_online()`) makes a specific named test FAIL. `bash scripts/test.sh SUITE=x86` green. Test on: QEMU TCG + KVM, 1 and 2 CPUs (`scripts/test-smoke-matrix.sh`).

---

## 28. S4 Orchestration: Hibernation Image Write and Resume Path

> **Spawned-by:** §4 (split)

> [!NOTE] Split out of §4 on 2026-09-04. §4 keeps the on-disk format and the pure codec, which are implementable on this tree; every item below needs a prerequisite that does not exist yet, so leaving them in one section would have blocked the codec (and with it the bootloader's format parser) behind ACPI namespace bring-up. -> XREF: `02-kernel-core/TODO-26` §4.

- [/] `pm_hibernate_write()` in `src/kernel/pm/hibernate.c`: §3 pre-suspend sequence, PMM used-page walk, chunk-encode via §4's codec, write to the hibernation partition, then `acpi_enter_sleep_state(4)`
  - BLOCKED on exactly the prerequisites §3 is deferred on: ACPICA's namespace is never loaded (no call site for `AcpiInitializeSubsystem()` / `AcpiLoadTables()` / `AcpiEnableSubsystem()` outside `src/kernel/acpica/`), and the driver power callbacks the freeze step broadcasts to are §9 and unimplemented. -> XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md` §1 (item: "`acpi_evaluate(path, args, result)` wrapper around `AcpiEvaluateObject`"), `02-kernel-core/TODO-26` §9 (item: "`pm_notify_resume()`")
  - The S4 refusal at `src/kernel/acpi.c:1568-1578` is the current, accurate behaviour and stays until this item can replace it with a real pipeline.
- [/] Open the hibernation partition by GPT type GUID: no `blkdev_open_by_gpt_type()` exists in the tree, and the GPT-scan helper that would provide it is owned by the crash-dump sink
  - Reuse that helper rather than adding a second GPT scanner. -> XREF: `02-kernel-core/TODO-27-crash-dump-generation.md` §7 (item: "**Prerequisite:** no `blkdev_open_by_gpt_type` exists in tree today; implement `dump_sink_probe()`")
- [/] AEAD-encrypt the image (AES-GCM) with a TPM-sealed key and populate the §4 header's cipher id / key id / nonce / tag so the bootloader can require encryption and refuse plaintext
  - Not blocked on crypto primitives: TPM NV storage and the monotonic/write-lock primitives both shipped. It is blocked because there is no write path to encrypt and the bootloader validator that would consume the metadata is itself deferred. -> XREF: `01-boot-platform/TODO-26` §4 (item: "Require an encrypted image: validate the AEAD metadata (cipher/key-id/nonce) and decrypt-verify with the TPM-sealed key")
- [/] Write the `resume_generation` anti-replay counter from the TPM-NV monotonic primitive at image-write time, so a stale-but-valid image is rejected on the next boot
  - The primitive exists (`01-boot-platform/TODO-13` §17, shipped); the consumer that compares it does not. -> XREF: `01-boot-platform/TODO-26` §4 (item: "Reject a valid-but-STALE image: compare the header `resume_generation` against the current TPM-NV/NVRAM monotonic value")
- [/] `pm_hibernate_resume()`: read + decompress chunks into a bounce buffer, CRC32C-verify the whole image, halt with `KERNEL_HIBERNATE_CORRUPT` on mismatch, restore pages / CR3 / RSP / RIP, then run §3 resume steps 4-7
  - BLOCKED on the handoff that would reach it: resume ENTRY is the bootloader's, and its discovery / validation / selection sections are deferred. The boot payload record is present in the ABI but explicitly unpopulated (`src/boot/uefi/bootx64.c:18233`, "Resume metadata (S4 hibernation): not populated yet"). -> XREF: `01-boot-platform/TODO-26` §2-§5
  - Corruption halt path. -> XREF: `TODO-27-crash-dump-generation.md` §1
- [/] Evaluate `_PTS(4)` before the S4 PM1 write and `_WAK(4)` on resume, same contract as the S3 path
  - BLOCKED: `AcpiEvaluateObject` is compiled and linked (`src/kernel/acpica/components/namespace/nsxfeval.c:325`) but unreachable while the namespace is never loaded. -> XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md` §1
- [/] Supply the §4 header's 32-byte `kernel_id` from an exact-artifact digest, so the identity guard compares something collision-resistant rather than something merely distinct
  - The codec already compares all 32 bytes; nothing yet PRODUCES a strong value. The two routes are a linker `--build-id=sha256` note read at runtime (a `Makefile` change, which the unattended run may not make) or a loader-measured kernel digest carried in `boot_info`. Build id and short commit stay diagnostics either way. -> XREF: `01-boot-platform/TODO-26` §1 (item: "Define header with magic, version, kernel build id, boot_info ABI version, root volume id, image size, checksum, flags.")
- [/] Add a kernel-versus-UEFI manifest comparison gate for the hibernation header, in the shape `tools/boot-info-manifest/` already uses for `boot_info`
  - §4 pins the canonical byte stream and every field offset on the kernel side, but no UEFI mirror exists yet to compare against (`src/boot/uefi/bootx64.c:18233` still reads "Resume metadata (S4 hibernation): not populated yet"), so the gate is built when the parser is. -> XREF: `01-boot-platform/TODO-26` §1
- [/] Benchmark the encoder's compress-in-place trade-off against a workspace variant on a real page mix, and adopt whichever wins
  - §4 compresses straight into the image buffer, so a successful chunk is written exactly once. LZ4 emits sequences before it discovers the budget is exhausted (`src/libs/lz4/lz4.c:1210` and `:1314` return 0 with the output pointer already advanced), so a near-incompressible chunk is written twice: once partially by the compressor, then wholly by the verbatim fallback. Compressing into a caller-owned workspace and copying the winner in once removes that, at the cost of a copy on every SUCCESSFUL chunk. Which is cheaper depends on the compressible / incompressible mix of a real image, which only this section can produce. Codex perf review 2026-09-04 [high], recorded rather than guessed at. -> XREF: `02-kernel-core/TODO-26` §4 (item: "Streaming encoder in `src/kernel/pm/hibernate_image.c`")
- [/] Commit: `"kernel/pm: S4 hibernation write path + resume consumer"`

**Test checkpoint:** `pm_hibernate_write()` produces an image the §4 decoder validates end to end; a resume with a mismatched kernel build id discards the image and cold-boots; a corrupted payload halts with `KERNEL_HIBERNATE_CORRUPT`; a plaintext image is refused when encryption is required. Test on: QEMU TCG (with a hibernation partition), then bare metal.

> **Test runner:** N/A (deferred -- no code shipped) | validation: deferred until the blockers below clear

> **Deferred:** every item needs a prerequisite owned elsewhere, all four verified against this tree on 2026-09-04. (1) The pre-suspend and resume orchestration is §3's, which is itself deferred: ACPICA is vendored and linked but its namespace is never initialized, so `_PTS`/`_WAK` cannot be evaluated. (2) The §9 driver power callbacks the freeze and thaw steps broadcast to do not exist. (3) There is no `blkdev_open_by_gpt_type()` and no GPT-scan helper in the tree; that helper is owned by the crash-dump sink. (4) Resume entry, image discovery, and integrity/anti-replay validation belong to the bootloader, whose sections are deferred and whose `BOOT_PAYLOAD_HIBERNATION_META` record is explicitly not populated. The format and codec half was split into §4 and is implementable now. -> XREF: `02-kernel-core/TODO-26` §4, `02-kernel-core/TODO-26` §9 (item: "`pm_notify_resume()`"), `04-drivers-hardware/TODO-03-acpi-power-management.md` §1 (item: "`acpi_evaluate(path, args, result)` wrapper around `AcpiEvaluateObject`"), `02-kernel-core/TODO-27-crash-dump-generation.md` §7 (item: "**Prerequisite:** no `blkdev_open_by_gpt_type` exists in tree today; implement `dump_sink_probe()`"), `01-boot-platform/TODO-26` §2-§5

---

## 29. Composite Battery State Model, Power Policy, and Registry Publication

> **Spawned-by:** §6 (split)

The provider-agnostic half of §6. Windows models a machine with N batteries as ONE composite battery (the class driver aggregates every miniport), and every consumer (tray, `powercfg`, `GetSystemPowerStatus`, the critical-power action) reads that composite rather than a device. This section owns the model, the aggregation, the warning policy and the registry publication; §6 supplies the ACPI-backed providers and §30 the charge-limit policy. It is implementable with no ACPI namespace because a provider is an ops table, not a control method.

**Files:** `include/kernel/pm/battery.h` (new), `src/kernel/pm/battery.c` (new), `src/kernel/test/test_battery.c` (new)

- [ ] `bat_info_t` / `bat_status_t`: the per-battery static description and live reading, in mW/mWh normalised units, with `BAT_UNKNOWN` (`0xFFFFFFFF`) as the single unknown sentinel every field uses
- [ ] `bat_provider_ops_t { get_info, get_status }` plus `bat_register(name, ops, ctx, *handle)` / `bat_unregister(handle)`
  - A provider is any source of battery readings, so the ACPI layer, a synthetic test source and a future EC-only source all attach the same way.
- [ ] `bat_ac_set_online(int)` / `bat_ac_online()` for the AC-adapter side, which is a single machine-wide fact rather than a per-device one
- [ ] `bat_update(void)` -- re-polls every registered provider under the table lock, caches each reading, and recomputes the composite in one pass so no consumer sees a torn mix of old and new readings
- [ ] `bat_composite_state(bat_composite_t *out)` -- Windows composite-battery semantics over every present battery
  - Capacities sum; `charge_pct = remaining_total * 100 / full_total`.
  - The composite is CHARGING when any battery charges, and CRITICAL when the AGGREGATE crosses the critical threshold, not when one cell does.
- [ ] `time_remaining_min = remaining_total * 60 / rate_total` on discharge only, `BAT_UNKNOWN` when the aggregate rate is zero or any contributing battery reports an unknown rate
- [ ] `wear_pct = (design_total - last_full_total) * 100 / design_total`, `BAT_UNKNOWN` when design capacity is unknown or zero
- [ ] All ratio math in `uint64_t` intermediates: `remaining_mwh` is a 32-bit field and `x * 100` overflows a `uint32_t` above ~42.9 M mWh, which a summed multi-battery total can reach
- [ ] SMP: one spinlock covers the provider table and the cached composite
  - `bat_register`/`bat_unregister` are safe against a concurrent `bat_update()`.
  - A provider callback is NEVER invoked with the lock held: a real ACPI evaluation can sleep.
- [ ] `pm_check_battery_warn()` -- reads `PowerWarnPercent` (default 10) and `PowerCriticalPercent` (default 5) from the registry
  - Edge-triggered: a level is announced once per crossing, not on every poll.
  - Returns the policy action rather than performing it, so the actuator stays owned by whoever can actually run it.
- [ ] `PowerCriticalAction` (default hibernate) resolves to an action enum; the actuators are parked, not invented here -- hibernate is §28 and the `WM_POWERBROADCAST` fan-out needs a window manager
- [ ] Registry publication on each `bat_update()`: `HKLM\SYSTEM\Battery\Status` (REG_BINARY, packed composite) and `HKLM\HARDWARE\Battery\Percentage` (REG_DWORD)
  - The DWORD is the value the tray consumer already specifies it reads, so publishing only the packed record would leave that consumer broken.
- [ ] Publication is best-effort: a registry failure degrades to a klog warning and never fails `bat_update()`, because the composite model is consumed in-kernel and must not depend on the hive
- [ ] Unit tests over a synthetic provider: single battery, dual battery aggregation, one battery unknown-rate, hot-unregister mid-life, zero design capacity, the 32-bit overflow boundary, and each warn/critical edge fired exactly once
- [/] Tray icon, tooltip and flyout (charge bar, current rate, wear indicator) -- owned by the system tray, not by kernel PM
  - The tray section already specifies the `HKLM\HARDWARE\Battery\Percentage` read this section publishes -> XREF: `08-graphics-ui/TODO-11-startmenu-tray-notifications.md` §4 (item: "`void systray_draw(gfx_surface_t *s, int32_t x, int32_t y, int32_t w)`")
- [/] `WM_POWERBROADCAST` / `PBT_APMBATTERYLOW` fan-out to top-level windows on a warn crossing -- blocked, no in-kernel path to broadcast a window message exists
  - `pm_check_battery_warn()` returns the action for a future broadcaster -> XREF: `02-kernel-core/TODO-26` §17 (item: "`IRP_MN_QUERY_POWER`")
- [ ] Commit: `"kernel/pm: composite battery model, provider registration, warn policy, registry publication"`

**Test checkpoint:** `bat_composite_state()` aggregates two synthetic providers into one charge percentage and one time estimate. An unknown rate yields `BAT_UNKNOWN` rather than a divide-by-zero. `bat_unregister()` drops a battery from the composite in the same update. The warn and critical edges each fire once per crossing. `HKLM\HARDWARE\Battery\Percentage` matches the composite percentage after `bat_update()`. Test on: QEMU TCG.

---

## 30. Battery Charge Limiting and Smart Charging

> **Spawned-by:** §6 (split)

> **User impact:** a laptop left plugged in charges to 100% and holds there, which is the single largest controllable contributor to lithium-ion wear; both Windows 11 (OEM smart charging) and Linux (`charge_control_end_threshold`) expose a limit.

Charge limiting is a WRITE to the embedded controller at a vendor-specific register, so it is gated on different prerequisites from the rest of §6: the EC write path, and a platform match that says which register this machine actually uses.

- [ ] `bat_set_charge_limit(uint8_t pct)` -- refuses with `STATUS_NOT_SUPPORTED` unless the platform is capability-matched; default 100 (no limit)
  - Codex adversarial finding (2026-09-03): a raw `ec_write(EC_REG_CHARGE_END, pct)` at a vendor-guessed offset (0xB1 ThinkPad, 0xE4 Dell, 0xBD ASUS) is unsafe on an unmatched EC map -- the same offset can control unrelated firmware state on another vendor's controller.
  - Identify the platform (DMI/SMBIOS vendor+model, or an ACPI `_DSM`/OEM method the DSDT exposes) and match a per-model quirks table before any write; an unmatched platform stays refused, never a best-effort guess.
  - Range-check `pct` and read the register back after the write to confirm the EC latched it, before reporting success.
- [ ] Registry `HKLM\SYSTEM\Battery\ChargeLimitPercent` (REG_DWORD, default 100); 80 is the longevity setting
- [ ] Smart-charging auto mode: after > 4 h continuously on AC with charge > 80%, hold at 80%; release the limit when unplugged; `SmartChargingEnabled` (REG_DWORD, default 1)
- [ ] Tray tooltip addition `"Charging limited to 80%"` while a limit is active -> XREF: `08-graphics-ui/TODO-11-startmenu-tray-notifications.md` §4
- [ ] `powercfg /batteryreport` reports charge-limit status and smart-charging history -> XREF: `02-kernel-core/TODO-26` §18
- [ ] Commit: `"kernel/pm: platform-matched battery charge limiting and smart charging"`

**Test checkpoint:** `bat_set_charge_limit()` refuses on an unmatched platform and never writes the EC. On a matched platform the write is range-checked and read back. The smart-charging hold engages after the AC dwell threshold and releases on unplug. Test on: QEMU TCG for the refusal path, bare metal on a matched laptop for the write path.

> **Test runner:** N/A (deferred -- no code shipped) | validation: deferred until the EC write gate below clears

> **Deferred:** [H] EC hardware access is gated off at the driver: §5 shipped the transactions but its own Deferred stamp records that readiness needs GPE acknowledgement and Global Lock arbitration, neither of which exists, so no `ec_write()` may run. [M] the platform quirks table also needs a DMI/SMBIOS vendor+model accessor that this tree does not expose to the PM layer (`src/kernel/firmware_tables.c` parses SMBIOS but publishes no vendor/model query). Both are prerequisites, not implementation choices: guessing the register is exactly the unsafe write the adversarial finding above rejects. -> XREF: `02-kernel-core/TODO-26` §24 (item: "Open the EC gate once GPE acknowledgement works" at line 1059), `02-kernel-core/TODO-26` §5

---

## 31. Lid State and Lid-Close Policy

> **Spawned-by:** §7 (split)

- [ ] An EC event (§5) or an ACPI GPE fires on a lid transition; read `\_SB.LID0._LID` (0=closed, 1=open)
- [ ] Lid-close action from registry `PowerLidCloseAction` (default `1` = sleep), sharing the §7 action table rather than a second copy of it
- [ ] Lid open while in S3/S4 resumes: the EC event resumes the hardware and software observes `WAKE_STS` in PM1a_STS -> §3 or §28 resume path
- [ ] Display-off before the sleep entry sequence: `gfx_blank_display()` cuts video output immediately so the panel does not flicker through the transition
- [ ] Docked/clamshell suppression: a lid close with an external display attached must not sleep the machine, which is the behaviour every laptop OS ships
- [ ] Commit: `"kernel/acpi: lid state, lid-close policy, display blank on sleep entry"`

**Test checkpoint:** a lid-close notify reads `_LID` and resolves `PowerLidCloseAction`. Closing with an external display attached suppresses the sleep. Lid open from S3 reaches the resume path. Test on: QEMU TCG for the policy table, bare metal on a laptop for the transition.

> **Test runner:** N/A (deferred -- no code shipped) | validation: deferred until the blockers below clear

> **Deferred:** [H] `_LID` is an ACPI control method and namespace evaluation does not run on this tree (`AcpiInitializeSubsystem`/`AcpiLoadTables`/`AcpiEnableSubsystem` have zero call sites outside `src/kernel/acpica/`, verified 2026-09-04). [H] the other event source, the EC, is gated off by §5's own Deferred stamp pending GPE acknowledgement. [M] the sleep and resume actions are §3 and §28, both deferred. Nothing here is a policy choice: there is no way to learn the lid state. -> XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md` §1 (item: "Initialisation sequence in `acpi_init()`"), `02-kernel-core/TODO-26` §24 (item: "Open the EC gate once GPE acknowledgement works" at line 1059), `02-kernel-core/TODO-26` §7

---

## 32. Human Presence Detection (HPD)

> **Spawned-by:** §7 (split)

Wake-on-approach and lock-on-leave. This is presence-sensing HARDWARE, and deliberately not the same signal as §22's user-interaction-aware QoS throttling, which is software-only and needs no sensor -> XREF: `02-kernel-core/TODO-26` §22.

- [ ] Detect a compatible IR/ToF camera or Wi-Fi sensing device via ACPI `_HID "INTC1070"` (Intel HPD) or HID usage `HID_DEVICE_SYSTEM_HUMAN_PRESENCE` (0x000D0011)
- [ ] `hpd_register_sensor(dev, ops)` -- register an HPD sensor driver with presence and absence callbacks
- [ ] Wake on Approach: with the display off from an idle timeout, `PRESENCE_DETECTED` powers the display on and optionally unlocks (biometric); registry `HPDWakeOnApproach` (default 1)
- [ ] Lock on Leave: `ABSENCE_DETECTED` sustained beyond `HPDAbsenceTimeout` (default 30 s) blanks the display and locks; registry `HPDLockOnLeave` (default 1)
- [ ] Attention-aware dimming: `GAZE_AWAY` dims the backlight after 10 s and `GAZE_DETECTED` restores it, on sensors that report gaze at all
- [ ] Privacy: presence state is a sensor reading about a person, so it is never persisted and never leaves the kernel except as the display/lock action it causes
- [ ] Boot log: `[HPD] Sensor: %s, wake-on-approach=%s, lock-on-leave=%s`
- [ ] Commit: `"kernel/pm: human presence detection, wake on approach, lock on leave"`

**Test checkpoint:** a synthetic sensor registers and its presence/absence callbacks drive the display and lock actions. Absence shorter than `HPDAbsenceTimeout` does not lock. No HPD device present skips cleanly rather than failing init. Test on: QEMU TCG with a synthetic sensor (no emulated HPD hardware exists).

> **Test runner:** N/A (deferred -- no code shipped) | validation: deferred until the blockers below clear

> **Deferred:** [H] sensor discovery is an ACPI `_HID` namespace walk or a HID usage-page match, and neither exists on this tree: namespace evaluation does not run, and no HID class driver publishes usage pages to consumers. [H] every action the sensor drives (display power, backlight level, lock screen) has no owner in tree either. A synthetic-sensor-only implementation would be an interface with no producer and no consumer, which is why this is parked rather than half-shipped. -> XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md` §1 (item: "Initialisation sequence in `acpi_init()`"), `09-desktop-shell/TODO-13-explorer-shell-host.md` §1

---


## 33. PM Device Registry (`pm_device_t`, `pm_register_device`)

> **Spawned-by:** §8 (split)

The per-device power-state record and the registry every later power path walks. Split out of §8 because it is a kernel-core data structure with its own lifetime and locking questions, not part of the PCI config-space state machine: §12 uses it for runtime idle, and §23 hangs ASPM policy off it. §8 lands first so a registered device has a real D-state to record.
> §9 does NOT walk this registry, and that boundary is now settled rather than pending: §9 shipped a CALLBACK registry (priority-ordered `pm_register_power_callback` slots) and its dispatcher deliberately owns no device addressing, because a callback carries an untyped `void *ctx` and no bus/dev/fn. This section is what a generic dispatcher-driven D-state step would need, and it is the reason §9 leaves `pci_set_d_state()` to each driver callback.
> → XREF: `02-kernel-core/TODO-26-power-management.md` §9 (item: "`pm_notify_sleep(state)` -- walks the callback registry in **reverse** priority order (user-space -> graphics -> input -> USB -> network -> storage)")
> → XREF: `02-kernel-core/TODO-26-power-management.md` §12 (item: "`pm_runtime_register(dev, ops, idle_timeout_ms)` -- register a device for runtime PM") -- runtime idle keys off the same per-device record

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
  - Reject a duplicate BDF rather than adding a second record, so a re-probe cannot produce two entries the notification walk would call twice.
- [ ] Fixed-capacity static array (no `kmalloc` on the probe path) with a documented bound and a refusal return when it is full; log the refusal once rather than once per device
- [ ] SMP: registration happens at probe time on any CPU while a consumer is walking the registry, so it needs a spinlock and the walk needs a stable snapshot
  - The consumer is NOT §9. §9 shipped a separate CALLBACK registry and walks that; this device registry's readers are §12 (runtime idle), §13 (wake sources) and §23 (ASPM policy), and they are what the locking has to be designed against. -> XREF: `02-kernel-core/TODO-26-power-management.md` §12 (item: "`pm_runtime_register(dev, ops, idle_timeout_ms)` -- register a device for runtime PM")
  - State the ownership rule in the header: the registry owns the record, the driver owns `driver_ctx`.
- [ ] `pm_device_find(bus, dev, fn)` and `pm_device_count()` -- lookup and enumeration for §12/§13/§23 and for the tests
- [ ] Per-device access gate that EVERY config-space client honours, so no driver touches a device during another caller's D-state recovery interval
  - §8's claim serialises D-state callers only: an unrelated driver reading its own BARs during a 10 ms D3hot recovery is not prevented by it, and cannot be, because §8 has no registry to hang the state on. The registry is where a device's "do not touch" state can live and be consulted by the whole PCI layer.
  - → XREF: `02-kernel-core/TODO-26-power-management.md` §8 (item: "Transitions are serialised per device across the WHOLE sequence -- claim, read, decide, write, recover, verify, release")
- [ ] Persistent per-BDF reinitialisation-pending state, so a failed D3hot->D0 cannot lose its obligation across a retry
  - §8 reports the obligation on every post-write outcome and, conservatively, on any D0 no-op where `No_Soft_Reset` is clear. That is safe but imprecise: it cannot tell a device that was genuinely never reset from one whose reset nobody saw finish. The registry is where a pending flag can live until a driver acknowledges it.
  - → XREF: `02-kernel-core/TODO-26-power-management.md` §8 (item: "`No_Soft_Reset` is honoured: a D3hot->D0 on a device with the bit clear lands in D0 Uninitialized, and the caller is told so")
- [ ] Cache the validated PM capability offset and revision per device, invalidated on removal or re-probe
  - `pci_get_d_state()` re-walks the capability list on every call. A PM capability at the last of 48 node positions costs ~50 config transactions, each taking two locks, for one state read. The runtime-idle consumer in §12 would inherit that amplification on a polling path.
  - → XREF: `02-kernel-core/TODO-26-power-management.md` §12 (item: "`pm_runtime_register(dev, ops, idle_timeout_ms)` -- register a device for runtime PM")
- [ ] A production owner for PCI PM poison recovery: a bus rescan or re-probe path that calls `pci_pm_clear_poison()` after re-establishing the affected devices
  - §8 latches a machine-wide refusal when a transition cannot observe its recovery interval, and deliberately provides no automatic expiry because nothing can know when an unobserved interval ended. Today nothing in the tree ever clears it, so the refusal would last the rest of the boot.
  - → XREF: `02-kernel-core/TODO-26-power-management.md` §8 (item: "A transition that cannot observe its recovery interval POISONS the module rather than reporting success")
- [ ] Commit: `"kernel/pm: pm_device_t registry, pm_register_device, BDF-keyed lookup"`

**Test checkpoint:** `pm_register_device()` adds to the global list and `pm_device_count()` reflects it. A duplicate BDF registration is refused rather than duplicated. Filling the array returns the refusal code and does not write past the bound. `pm_device_find()` returns the registered record and NULL for an unregistered BDF. Test on: QEMU TCG.

---

## 34. PCI D3cold via ACPI `_PS0`/`_PS3` Platform Methods

> **Spawned-by:** §8 (split)

D3cold removes VCC from the device, so it is not a PMCSR write at all: it is an ACPI control method the platform evaluates. Split out of §8 for exactly that reason -- §8 needs nothing but config space, this needs AML method evaluation, and merging them would have held the shippable half behind the blocked half.
> → XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md` §1 (item: "Initialisation sequence in `acpi_init()`") -- ACPICA namespace init is the prerequisite; `AcpiEvaluateObject` has no initialised namespace to evaluate against until it lands
> → XREF: `02-kernel-core/TODO-26-power-management.md` §8 (item: "`pci_set_d_state(dev, state)` -- write `state & 0x3` to the `PMCSR` power state bits") -- the D3hot step D3cold entry builds on

- [ ] D3cold (power completely removed) requires platform support: evaluate the device's `_PS3` ACPI method (if present) to cut VCC, and `_PS0` to restore power for the D3cold->D0 transition
  - Resolve the method off the device's own namespace node, not a hardcoded `\_SB.PCI0.DEV` path -- that path is correct for exactly one firmware layout.
- [ ] `pci_d3cold_capable(dev)` -- report capability honestly from the presence of `_PS3`/`_PS0` on the node plus `_PR3` power resources; absent methods mean D3hot is the deepest state, and the answer is no rather than a silent downgrade
- [ ] `pci_d3cold_enter(dev)` -- `pci_set_d_state(dev, 3)` first (D3hot), then evaluate `_PS3`; config space is inaccessible afterwards, so nothing may read the device back to confirm
- [ ] `pci_d3cold_exit(dev)` -- evaluate `_PS0`, wait the `_DSM`-reported `D0D3COLD_DELAY` (else the 100 ms default), then `pci_set_d_state(dev, 0)`
  - The device has lost all config state across D3cold, so the caller restores BARs, command register, and interrupt line.
- [ ] Record the config-space restore contract in the header: D3cold is the one transition after which a driver may NOT assume its device is where it left it
- [ ] Commit: `"kernel/pci: D3cold entry/exit via ACPI _PS0/_PS3, config-space restore contract"`

**Test checkpoint:** `pci_d3cold_capable()` is false for a device with no `_PS3` and does not claim D3cold. `pci_d3cold_enter()` reaches D3hot before evaluating `_PS3`. `pci_d3cold_exit()` applies the delay before touching config space. Test on: QEMU TCG (no emulated D3cold hardware exists; the ACPI method path is what is exercised).

---

## 35. Directed Power (DFx) and DRIPS Residency Accounting

> **Spawned-by:** §10 (split)

- [ ] Directed PoFx (DFx, PoFx v3): the power manager *directs* entire device stacks to enter low-power during Modern Standby idle when no activator-brokered activity; unlike runtime PM where the device self-idles, DFx is top-down OS-directed
- [ ] `pm_dfx_power_down(dev_stack)` -- OS calls `PO_FX_DIRECTED_POWER_DOWN_CALLBACK` on each driver in the stack; driver must save state, stop DMA, enter D3
- [ ] `pm_dfx_power_up(dev_stack)` -- called on activator wake or system exit from S0ix; driver restores state
- [ ] Stack ordering is the inverse of §9 resume ordering: power DOWN leaf-first, power UP root-first, so a child never runs against a powered-off parent
- [ ] A driver that returns failure from a directed power-down aborts the stack walk and unwinds the already-powered-down children, same contract as the §9 suspend unwind
- [ ] DRIPS (Deepest Runtime Idle Platform State) tracking: `drips_pct = time_all_devices_idle / total_s0ix_time * 100`; target > 95% for good battery life; exposed via `powercfg /sleepstudy`
- [ ] Devices that block DRIPS logged: `[S0IX] DRIPS blocker: %s (active for %u ms)` -- helps diagnose battery drain
- [ ] DRIPS accounting must be monotonic-clock based and overflow-safe: a residency counter that wraps must not produce a percentage above 100 or a negative idle span
- [ ] Commit: `"kernel/pm: directed power (DFx) stack walk and DRIPS residency accounting"`

**Test checkpoint:** `pm_dfx_power_down()` walks a fixture stack leaf-first and `pm_dfx_power_up()` root-first. A mid-stack failure unwinds exactly the devices already powered down. `drips_pct` is 0 with no idle time, 100 with fully idle time, and never exceeds 100 across a counter wrap. Blocker log names the offending device. Test on: QEMU TCG (fixture stacks; no real S0ix platform).

---

## 36. NIC Wake Offloads for Connected Standby (ARP/NS/WoL/Patterns)

> **Spawned-by:** §10 (split)

- [ ] Network keepalive: the NIC (if `_DSM` advertises DRIPS/D0ix support) stays powered in D0i3 state for ARP/IPv6 NS replies and WoL packets
  - `rtl8139_d0i3_enter()` / `rtl8139_d0i3_exit()` stubs; the full implementation depends on the specific NIC driver
- [ ] ARP offload: program NIC hardware to respond to ARP requests while CPU sleeps; `nic_add_arp_offload(ipv4_addr)` writes to NIC offload registers (driver-specific)
- [ ] IPv6 Neighbor Solicitation offload: `nic_add_ns_offload(ipv6_addr)` -- NIC responds to NS without waking CPU
- [ ] Wake-on-LAN: `nic_set_wol(dev, WAKE_MAGIC | WAKE_PATTERN)` -- configure Magic Packet wake and pattern-match wake via NIC `WOL_CR` register
- [ ] Wake-on-Pattern: `nic_add_wake_pattern(dev, pattern, mask, offset)` -- wake CPU on specific packet match (e.g. incoming VoIP SIP INVITE)
- [ ] Offload slots are a bounded hardware resource: registration past the device's slot count fails with a distinct status rather than silently overwriting an existing offload
- [ ] NIC D0i3 entry/exit callbacks registered via §12 runtime PM; NIC maintains minimal firmware for offload processing
- [ ] Commit: `"kernel/net: NIC wake offloads -- ARP/NS reply, WoL magic and pattern match, D0i3"`

**Test checkpoint:** `nic_add_arp_offload()` and `nic_add_ns_offload()` reject a registration past the slot bound with a distinct status and leave existing slots intact. `nic_set_wol()` composes the magic+pattern mask without clobbering unrelated `WOL_CR` bits. `nic_add_wake_pattern()` validates offset+length against the pattern buffer. Test on: QEMU TCG (no emulated NIC wake hardware; the register-composition and bounds paths are what is exercised).

---

## 37. System Connected Standby Entry (All-CPU S0ix Transition)

> **Spawned-by:** §10 (split)

The system-entry half of the original §10 draft. §10 ships the firmware advertisement and the pure MWAIT capability and hint-selection layer; this section owns BOTH the single-CPU instruction execution and the all-CPU transition, because they share one unresolved prerequisite. Nothing that executes `MONITOR` or `MWAIT` shipped in §10. Split out rather than stubbed because the pre-implementation design review found each piece blocked on infrastructure that does not exist yet, and a stub would have claimed S0ix without residency, which the review named a correctness failure of the API rather than a power shortfall.

- [/] All-CPU MWAIT transition driven through `smp_rendezvous_begin()`/`smp_rendezvous_end()` -- BLOCKED on §27
  - The rendezvous has no production caller yet, and its AP handler spins on `pause` (`src/kernel/smp/smp.c:1096`), so making APs MWAIT is a protocol change to that handler, not a call into it.
  - Needs, per the design review: a monitored generation/release handshake, BSP wake routing (a wake delivered only to a parked AP cannot release the BSP), an audited lockless window, and balanced `end()` on every abort and wake path.
  - -> XREF: `02-kernel-core/TODO-26` §27 (item: "Strengthen the single live test to prove AP RESUMPTION and reusable IPI wiring, not just that the BSP survived" at line 1284)
- [/] `pm_idle_mwait()` -- the single-CPU primitive that actually issues `MONITOR`/`MWAIT` -- BLOCKED on the same wake guarantee
  - `pm_deep_idle_allowed()` reads the DPC depth, but a thread made runnable elsewhere stores nothing this CPU monitors, so the monitored line has no established writer.
  - §10 ships the capability layer this needs, so what remains here is the instruction issue and its wake contract, not the gating.
  - Must also settle whether `sti; mwait` inherits the STI interrupt shadow the way `sti; hlt` does; `pm_idle_c1()` depends on that shadow and the MWAIT path cannot assume it without evidence.
  - Parity evidence for that question: Linux does NOT refuse. `mwait_idle_with_hints()` (`arch/x86/include/asm/mwait.h`) issues `__mwait` directly when the CPU reports `CPUID.05H:ECX[1]`, and otherwise falls back to `__sti_mwait` -- relying on exactly that one-instruction STI shadow. So the likely correct target here is a fallback, not the hard refusal §10's predicate encodes; §10 ships the interrupt-state RULE, and choosing the executor behaviour is this section's call.
  - Do NOT treat `pm_mwait_deepest_hint()` output as the hint to always request. It is the deepest LEGAL class, with no exit-latency, target-residency or per-SKU errata information; Linux selects from per-microarchitecture tables in `intel_idle` and consults leaf 5 only to confirm MWAIT exists. A real governor also needs the full set of legal (class, substate) pairs, which the current helper does not enumerate.
  - Perf guidance from the section-10 review: derive and cache the hint once during each CPU capability init rather than re-running the search on every idle entry (measured at 17-29 instructions for a successful search), and cache the ACPI capability answer if it ever enters the per-idle path.
  - -> XREF: `02-kernel-core/TODO-26` §10 (item: "`pm_mwait_idle_allowed(if_flag, irq_break_supported)`" -- shipped, provides the refusal predicate this item must call)
- [/] Paired `pm_notify_sleep()` before the barrier and `pm_notify_resume()` after it -- BLOCKED on the transaction contract
  - `pm_notify_resume()` refuses with `PM_CB_NO_TRANSACTION` unless the table is `PM_TXN_ASLEEP` (`src/kernel/pm/power_callback.c:417`), and both calls require PASSIVE_LEVEL with interrupts enabled, which the barrier has already masked.
  - -> XREF: `02-kernel-core/TODO-26` §9 (item: "Each major driver registers in its `init()`" at line 637)
- [/] Intel LPS0 `_DSM` evaluation -- BLOCKED on AML method evaluation
  - GUID `c4eb40a0-6cd2-11e2-bcfd-0800200c9a66`, plus the Microsoft Modern Standby GUID `11e00d56-ce64-47ce-837b-1f898f9aa461`.
  - This tree's ACPI parser is hand-rolled and the ACPICA namespace is not loaded, so no `_DSM` can be evaluated at all.
  - Gating on BOTH the FADT bit and the `_DSM` is the documented-correct behaviour: a platform can set bit 21 without implementing the `_DSM` entry points.
  - -> XREF: `04-drivers-hardware/TODO-03` §4 (item: "`acpi_evaluate(path, args, result)` wrapper around `AcpiEvaluateObject`" at line 102)
- [/] `_OSC` platform-capability negotiation -- BLOCKED on the same AML gap
  - Note the draft's error: `_OSC` negotiates capabilities, it is NOT a generic "entering S0ix now" notification, and must not be substituted for the LPS0 `_DSM` protocol.
  - -> XREF: `04-drivers-hardware/TODO-03` §4 (item: "`acpi_evaluate(path, args, result)` wrapper around `AcpiEvaluateObject`" at line 102)
- [-] Memory-controller `MC_PM_STS` DRAM self-refresh gating -- REJECTED, not deferred
  - The register has no definition in this tree and no public per-generation contract (controller identification, offset, access width, writable mask, restoration sequence).
  - "Intel-specific, skip on AMD" cannot authorise a blind MMIO write that could disturb live memory traffic. Admit it only under an exact supported-platform contract backed by its register specification.
- [/] DRIPS residency measurement before any code claims S0ix was entered -- BLOCKED on the transition above existing at all
  - -> XREF: `02-kernel-core/TODO-26` §35 (item: "DRIPS (Deepest Runtime Idle Platform State) tracking" at line 1528)
- [ ] standing: re-check on each ACPICA namespace milestone whether the `_DSM`/`_OSC` blockers above have cleared, and re-open the parked items when they have

**Test checkpoint:** No shippable checkpoint until the blockers clear. When they do: every AP's resume counter advances after a wake, the release handshake holds across every MONITOR/check/MWAIT interleaving, and measured package residency is non-zero before the API reports success. A QEMU graceful skip proves refusal behaviour only, never MWAIT liveness or S0ix residency.


> **Deferred:** [High] 2026-09-05 -- created already-blocked, from the §10 pre-implementation design review, and stamped here rather than left open so a later pass does not spend a full pipeline re-deriving blockers this section already names at file:line. Three independent prerequisites, none of them ownable here: (1) AML method evaluation, which the LPS0 `_DSM` and `_OSC` items both need and which this tree's hand-rolled ACPI parser cannot do while the ACPICA namespace is unloaded -> XREF: `04-drivers-hardware/TODO-03` §4 (item: "`acpi_evaluate(path, args, result)` wrapper around `AcpiEvaluateObject`" at line 102); (2) the rendezvous production-caller seam, since `rendezvous_ipi_handler()` spins on `pause` (`src/kernel/smp/smp.c:1096`) and driving APs into MWAIT is a change to that handler's contract, not a call into it -> XREF: `02-kernel-core/TODO-26` §27 (item: "Strengthen the single live test to prove AP RESUMPTION and reusable IPI wiring, not just that the BSP survived" at line 1284); (3) the sleep/resume transaction pairing, because `pm_notify_resume()` refuses with `PM_CB_NO_TRANSACTION` outside `PM_TXN_ASLEEP` (`src/kernel/pm/power_callback.c:417`) and both calls require PASSIVE_LEVEL with interrupts enabled, which the barrier has masked -> XREF: `02-kernel-core/TODO-26` §9 (item: "Each major driver registers in its `init()`" at line 637). The `MC_PM_STS` item is REJECTED outright rather than parked: it has no public per-generation register contract, so it has no re-open condition. Re-open when AML evaluation lands and §27 has established the rendezvous seam; the `standing:` item above is the recurring check.

---

## 38. ACPI Table Discovery: Validate Extents Before Checksumming and Publishing

> **Spawned-by:** §10 (review)
> **User impact:** On a machine whose firmware ships a corrupt or hostile ACPI table length, the kernel reads outside the table while computing its checksum -- into unrelated RAM, into MMIO (where the read itself is a device side effect), or into an unmapped page (a boot fault with no handler yet). If the checksum happens to pass over that adjacent memory, the bad table is cached and every later consumer inherits it. This is a bare-metal-only failure: emulator firmware is well-formed, so no amount of QEMU testing reaches it.

Found by the §10 post-ship adversarial review, which traced `acpi_get_fadt()` back through discovery. The defect is PRE-EXISTING and independent of §10 -- that section only added a reader that inherits the pointer. Filed as a new section because §1 owns discovery and is already `[x]` + stamped, so an item added there would be invisible to every later pass.

The tree already HAS the right primitive: `acpi_table_valid()` (§1) checks signature, a length bounded by `ACPI_MAX_TABLE_LENGTH`, containment in one UEFI memory-map descriptor of an ACPI-bearing class, and only then the checksum. Discovery does not call it. This section is about routing discovery through the check that exists, not writing a new one.

- [x] `find_table_xsdt()` (`src/kernel/acpi.c:183-189`) dereferences each entry pointer and checksums over `hdr->length` with no validation of the pointer or its backing extent
  - Validate the candidate header and its full readable extent BEFORE `acpi_checksum()` runs over it. The checksum walk is itself the out-of-bounds read, so a check placed after it is too late.
- [x] `find_table_rsdt()` (`src/kernel/acpi.c:165-166`) has the identical defect on the 32-bit entry path and must be fixed in the same change, not left as the surviving copy
- [x] Validate the XSDT/RSDT root header and its entry count before walking it -- the entry count is derived from `header.length`, so a corrupt root length controls how far the loop reads
- [x] `acpi_init()` publishes `fadt_ptr` (`src/kernel/acpi.c:961`) after checking only `length >= 116`; publish only once the FADT passes full validation, so no consumer can inherit an unvalidated table
- [x] Regression tests for malformed firmware, four shapes:
  - a length crossing a memory-map descriptor boundary; an entry pointer outside any ACPI-bearing descriptor; an undersized root header; and a table whose checksum passes only because the walk ran past its declared end
- [x] Commit: `"kernel/acpi: validate table extents before checksum and before publishing fadt_ptr"`

**Test checkpoint:** Each malformed-table fixture is refused, and refused BEFORE any read past the declared extent (assert on the refusal, not merely on the absence of a crash -- a passing read into adjacent RAM is the failure this section exists to stop). `fadt_ptr` stays NULL for every refused FADT. The existing `acpi_table_valid()` suite in `test_acpi_power.c` continues to pass. Test on: QEMU TCG; the real-firmware case is bare metal and cannot be reproduced under emulation.

> **Test runner:** `bash scripts/test.sh SUITE=boot` -- "ACPI: discovery extent guard" in `src/kernel/test/test_acpi_power.c` | full suite: 33,886 kernel + 17 user-mode tests pass | smoke matrix 4/4 legs (kvm/tcg x 1/2 cpu)

> **Verified:** discovery now routes both root headers and every RSDT/XSDT entry through `acpi_table_valid()`, so the header extent is proven mapped before the signature is read and the FULL declared extent is proven mapped before `acpi_checksum()` walks it. `acpi_root_entry_count()` bounds the entry loop and returns 0 on an undersized root, where the old inline `(length - 36) / stride` underflowed to ~4 G. `find_acpi_table()` no longer walks a zero `rsdt_addr`. Positive control: the FADT and MADT are still discovered on a real boot (`build/smoke-test.stripped.log`: "acpi: ACPI: FADT at 0x000000007f779000"), with no containment-rejection warnings -- a tightening that refused everything would still have booted to `C:\>`, so the smoke pass alone would not have caught it.

> **Quality reviewed:** 17 Codex legs across 9 rounds, ending 0 findings on every leg, plus an Opus `kernel-quality-auditor` pass that returned 10 findings and drove three further fix rounds. Each round past the third returned a MEASURED defect in the previous round's fix, which is the shape that earns another round rather than a spiral: round 5 found the diagnostics gate failing open, round 6 found the length snapshot missing from the ROOT validator after the child was fixed, round 7 confirmed clean. Round 1 [high] adversarial: the truncated-map benefit of the doubt was granted to an extent that STRADDLES a known descriptor, so a table at the end of an ACPI region with a forged length was checksummed into the adjacent MMIO region -- device-register reads. Fixed by recording partial overlap as positive evidence and deciding it before the missing-evidence fallback, plus wrap guards on the requested extent and on each descriptor. Round 1 [high] consistency: `acpi_validate_root()`/`acpi_validate_child()` in the enumerator path were a THIRD unvalidated copy of the same defect; both now prove containment before the length read and before the checksum. Perf: approved, boot-only cost, bounded at 512 descriptors per scan.

> **Quality reviewed:** the `kernel-quality-auditor` pass found five issues worth fixing here, all applied. [H] routing the ROOT through `acpi_table_valid()` made a root checksum mismatch fatal to ALL discovery, where the pre-change code never checksummed a root at all -- a new refusal mode that would boot a quirky machine single-core with no ACPI, and the opposite of what the vendored reference does (`src/kernel/acpica/include/acconfig.h` sets `ACPI_CHECKSUM_ABORT FALSE`). Roots now take `acpi_table_extent_valid()`: containment is the safety property, the checksum is a heuristic, and every ENTRY still carries the full check the old code applied. [M] the containment warning was reachable from unprivileged user mode via `NtQuerySystemInformation(SystemFirmwareTableInformation)`, twice per call over up to `ACPI_ROOT_ENTRY_MAX` children, each line holding the klog and serial locks -- a user-triggerable log flood this change introduced; now gated on `!acpi_ready`, so it diagnoses at boot and is silent at runtime. [M] the straddle rule refused a table spanning two ADMISSIBLE descriptors, which the loader leaves split whenever their EFI_MEMORY_* attributes differ even though the type matches, so a legitimate FADT could have been refused; a straddle is now evidence only when the neighbour is an inadmissible class. [L] `hdr->length` was read twice, once for the extent check and again for the checksum, so the walk could run over a length nothing proved; the validated length is snapshotted and passed through. [L] the enumerator's hardcoded 16 MiB cap now uses `ACPI_MAX_TABLE_LENGTH`.

> **Quality reviewed:** round 4 caught three consequences of the round-3 fixes themselves, all applied. [M] the `!acpi_ready` gate on the containment warnings FAILS OPEN exactly where it matters: `acpi_ready` is set only on `acpi_init()`'s success path, so a machine whose FADT is refused leaves it 0 for the life of the system while boot continues and the user-reachable enumerator keeps warning. The window is now closed by an `acpi_init()` wrapper around `acpi_init_inner()`, so it ends on every exit including the failures. [M] refusing a straddle was still too strong on a COMPLETE map: an extent wholly covered by a CONTIGUOUS run of admissible descriptors is legitimate, because the loader coalesces neighbours only when type AND attribute match, so a table spanning two ACPI ranges split by an attribute difference was refused on most machines rather than only on truncated-map ones. `acpi_extent_covered()` now admits it on the evidence. [M] the length snapshot stopped at `acpi_table_valid()`; `acpi_validate_child()` still read `hdr->length` four separate times, so its extent check, its checksum and its returned size could describe three different lengths. All three now use one read.

> **Quality reviewed:** round 5 found the surviving copy of the snapshot defect: `acpi_validate_child()` had been fixed while `acpi_validate_root()` still re-read `root->length` between proving the extent and checksumming it, and the reviewer confirmed a fresh load in the clang-19 -O2 output rather than asserting it from the source. Both public firmware-table accessors reach that validator, so it was the runtime-reachable half of the same bug. The root now snapshots once and uses it for the size limit, the entry-count arithmetic, the containment proof and the checksum. Round 6's finding was procedural and worth recording: the fix was in the working tree and NOT in the index, and the reviewer reads the index -- a fix that is not staged is not the candidate, however green the tree looks. Round 7 reviewed the staged bytes and approved.

> **Quality reviewed:** the perf leg, re-run against the final content, found the one cost this change introduced. `acpi_extent_covered()` restarts its descriptor scan at zero for every boundary crossed, so it is O(descriptors^2) per call -- and `acpi_enumerate_signatures()` reaches it from an unprivileged `NtQuerySystemInformation` that runs the validators over up to `ACPI_ROOT_ENTRY_MAX` children TWICE, once to count and once to fill. On a 512-descriptor map with a long admissible run and children claiming one byte past its end, that is hundreds of millions of descriptor visits per syscall, repeatable at will. Bounded by capping the CROSSING count at `ACPI_COVER_MAX_SPANS` (8): the loader already coalesces neighbours, so a split survives only where two differ in EFI_MEMORY_* attributes and a table spanning more than a handful is not a shape real firmware produces. Past the cap the extent falls through to the evidence rules rather than being admitted, which is the safe direction. The asymptotic fix is a sorted coalesced range index, filed in section 39 and blocked on image size (`.bss` headroom 1,235 bytes against roughly 8 KB for the table).

> **Accepted:** five further audit findings are real but sit outside this section's scope -- the RSDP's own extent is never proven, the ACPI 2.0 extended checksum over 36 bytes is never computed although `xsdt_addr` is read past the 20 bytes that are, the two root-walk rule sets still disagree, `acpi_extent_mapped()` compares a virtual pointer against physical descriptor bases, and `X_DSDT` is never consulted. The first two need a refusal-policy decision validated on bare metal (an EBDA-resident RSDP can legitimately sit in a class this kernel excludes, so a naive check refuses ACPI on real hardware while passing under OVMF). -> XREF: `02-kernel-core/TODO-26` §39 (item: "Prove the RSDP's own extent before checksumming it")

> **Accepted:** the containment policy logs only on the rejected-class branch, so a straddling or out-of-map refusal is silent on the serial log. Not fixed here: the kernel image has 18 bytes of `.rodata` headroom, so a new diagnostic string cannot link. -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §3 (item: "Move the LMA to `MM_KERNEL_PHYS_BASE` (`0x200000`) -- NOT the historical `0x100000`")

---

## 39. ACPI Root-Pointer Integrity and Validator Unification

> **Spawned-by:** §38 (review)
> **User impact:** The RSDP is the root of the whole table chain and is the ONE firmware pointer §38 did not reach. On a machine whose firmware publishes a corrupt RSDP, the kernel checksums 20 bytes at a loader-supplied address nothing proved readable, then dereferences a 64-bit `xsdt_addr` that sits 12 bytes PAST the region that checksum covers -- so every extent guard §38 added downstream is anchored to an unverified pointer. Separately, a machine whose XSDT length is not an exact multiple of 8 gets its tables from boot discovery but NOT from the enumerator, silently losing the watchdog (WDAT), the EC (ECDT) and TPM2 with no diagnostic.

Filed by the §38 post-ship kernel quality audit. §38 hardened everything reachable FROM the RSDP; these are the pointer itself, the two validators that disagree about the same root, and two capability gaps the audit surfaced while tracing the chain. None is a regression from §38 -- all are pre-existing -- but each sits on the path §38 now guards, so leaving them makes that guarantee only as strong as its weakest anchor.

- [ ] Prove the RSDP's own extent before checksumming it (`src/kernel/acpi.c` `acpi_init()`, and the four runtime readers that verify no checksum at all)
  - The read is `acpi_checksum(rsdp, 20)` over `g_boot_info.acpi_rsdp_addr` with no `acpi_extent_mapped()` call in front of it. Route it through the same header-then-extent ordering §38 established.
  - **Decide the refusal policy deliberately, and validate on bare metal before shipping.** An RSDP in the EBDA can legitimately sit in a class this kernel's admissible set excludes, so a naive containment check REFUSES ACPI outright on real hardware while passing under OVMF. The conservative shape is to widen the admissible set for the RSDP specifically, or to warn rather than refuse, following the same reasoning §38 used for the root checksum.
- [ ] Compute the ACPI 2.0 extended checksum over 36 bytes before trusting `xsdt_addr`
  - `find_acpi_table()` reads `rsdp2->xsdt_addr` at RSDP offset 24, which is outside the 20 bytes `acpi_checksum(rsdp, 20)` covers, and nothing anywhere computes the v2 checksum the specification defines over the full 36-byte structure.
  - Gate it on `revision >= 2` and treat a mismatch the way the vendored reference does (`src/kernel/acpica/include/acconfig.h` sets `ACPI_CHECKSUM_ABORT FALSE`, so ACPICA warns and proceeds), not as a hard refusal.
- [ ] Unify the two root-walk rule sets so one RSDT/XSDT cannot be accepted by boot discovery and rejected by the enumerator
  - `acpi_root_entry_count_len()` uses truncating division with no stride-multiple requirement and the 16 MiB `ACPI_MAX_TABLE_LENGTH` cap; `acpi_validate_root()` demands `entries_bytes % stride == 0`, caps entries at `ACPI_ROOT_ENTRY_MAX` and length at 1 MiB.
  - The divergence is silent and costs real capability: a root the enumerator refuses takes WDAT, ECDT, TPM2 and the firmware-tables catalog with it while boot discovery reports everything fine.
- [ ] Make the physical-versus-virtual assumption in `acpi_extent_mapped()` explicit rather than implied
  - It is handed `(uint64_t)(uintptr_t)hdr`, a VIRTUAL pointer, and compares it against `boot_mmap_entry.base_addr`, which is PHYSICAL. That is correct only while ACPI ranges stay identity-mapped, which nothing asserts.
  - Add the containment assert against the owning window from `include/kernel/mm/memmap.h`, or convert explicitly. The guard becomes silently wrong the day ACPI ranges move to the HHDM, and a wrong containment check reads exactly like a working one.
- [ ] Prefer `X_DSDT` (FADT offset 140) over the 32-bit `dsdt` field when it is non-zero
  - ACPI requires OSPM to prefer the extended field. Firmware that publishes a DSDT above 4 GiB with `dsdt` zeroed currently falls into the failure branch and loses S1/S3/S4/S5 entirely -- a silent capability loss on exactly the modern machines this roadmap targets.
- [ ] Replace `acpi_extent_covered()`'s restart-from-zero scan with a sorted, coalesced admissible-range index built once
  - The walk is bounded today by `ACPI_COVER_MAX_SPANS` (8 crossings), which caps the cost but is a heuristic: a legitimate table spanning more descriptors than that is refused. A sorted index makes coverage a bounded lookup and removes the cap entirely.
  - It also collapses the double map scan `acpi_table_valid()` performs (header extent, then full extent): the header lookup can return its containing range so the full-length proof reuses it when it fits.
  - **Blocked on image size, not design.** The index needs a static array the kernel has no room for -- `.bss` headroom was 1,235 bytes when this was filed, against roughly 8 KB for a 512-entry range table. -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §3 (item: "Move the LMA to `MM_KERNEL_PHYS_BASE` (`0x200000`) -- NOT the historical `0x100000`")
- [ ] Commit: `"kernel/acpi: RSDP extent + v2 checksum, unified root validators, X_DSDT"`

**Test checkpoint:** A synthetic RSDP outside every admissible descriptor is handled by the policy this section picks (refused, or admitted with a diagnostic) rather than checksummed blindly. A v2 RSDP whose extended checksum fails is diagnosed. One XSDT image produces the SAME accept/reject verdict from boot discovery and from `acpi_enumerate_signatures()`. A FADT carrying only `X_DSDT` reaches `parse_s5_from_dsdt()`. Test on: QEMU TCG; the RSDP-placement cases are bare metal and cannot be reproduced under emulation.

---

## OS Comparison

| ⭐  | Feature                           | 🪟 Win11        | 🐧 Linux          | 🚀 Impossible OS |
| --- | --------------------------------- | --------------- | ----------------- | ---------------- |
| 💎  | S5 ACPI shutdown                  | ✅ Full         | ✅ Full           | ✅ Done §1       |
| 💎  | ACPI S-state discovery            | ✅ ACPI.sys     | ✅ acpi_sleep     | ✅ Done §1       |
| 💎  | PM1 fixed-event SCI               | ✅ ACPI.sys     | ✅ acpi_sci       | ✅ Done §1       |
| 💎  | C1 idle / HLT                     | ✅ Full         | ✅ cpuidle        | ⬜ §2            |
| 💎  | S3 suspend RAM                    | ✅ Full         | ✅ sleep          | ⬜ §3            |
| 💎  | Stop-the-world CPU rendezvous     | ✅ KeIpiGeneric | ✅ stop_machine   | ✅ Done §26      |
| 💎  | Stop-the-world fault injection    | ✅ Internal     | ✅ ftrace stress  | ⬜ §27           |
| 💎  | Hibernation image format codec    | ✅ hiberfil.sys | ✅ swsusp image   | ✅ Done §4       |
| 💎  | S4 hibernate disk                 | ✅ Full         | ✅ swsusp         | ⬜ §28           |
| 💎  | Fast startup hiberboot            | ✅ Default      | ❌ None           | ⬜ §11           |
| 💎  | ACPI EC discovery + transactions  | ✅ Full         | ✅ acpi_ec        | ✅ Done §5       |
| 💎  | ACPI EC enabled for real traffic  | ✅ Full         | ✅ acpi_ec        | ⬜ §5 + §24      |
| 💎  | ACPI EC event (QR_EC) dispatch    | ✅ Full         | ✅ acpi_ec query  | ⬜ §5 + §24      |
| 💎  | Battery `_BIX` / `_BST`           | ✅ Full         | ✅ upower         | ⬜ §6            |
| 💎  | Power/sleep button events         | ✅ Full         | ✅ logind         | ✅ Done §7       |
| 💎  | Lid-close events                  | ✅ Full         | ✅ logind         | ⬜ §31           |
| 💎  | PCI D-states D0--D3hot            | ✅ Full         | ✅ PCI PM         | ✅ Done §8       |
| 💎  | PCI D3cold via ACPI `_PS0`/`_PS3` | ✅ Full         | ✅ pci_pm_d3cold  | ⬜ §34           |
| 💎  | PM capability + PMCSR integrity   | ✅ Full         | ✅ pci_pm_init    | ✅ Done §8       |
| 💎  | Driver sleep wake callbacks       | ✅ WDM          | ✅ pm_ops         | ✅ Registry §9   |
| 💎  | Driver query veto power           | ✅ QUERY_POWER  | ✅ prepare        | ⬜ §17           |
| 💎  | Runtime idle PoFx RPM             | ✅ PoFx         | ✅ runtime_pm     | ⬜ §12           |
| 💎  | Power requests tracking           | ✅ powercfg     | ⚠️ wake_lock      | ⬜ §13           |
| 💎  | Wake source lastwake              | ✅ powercfg     | ⚠️ dmesg          | ⬜ §13           |
| 💎  | ACPI thermal zones                | ✅ ACPI.sys     | ✅ thermal        | ⬜ §14           |
| 💎  | Passive active cooling            | ✅ Full         | ✅ step_wise      | ⬜ §14           |
| 💎  | CPU DVFS cpufreq                  | ✅ PPM HWP      | ✅ cpufreq        | ⬜ §15           |
| 💎  | CPU idle C-states                 | ✅ PPM          | ✅ menu teo       | ⬜ §16           |
| 💎  | Connected standby S0ix            | ✅ Modern       | ⚠️ Partial        | ⬜ §37           |
| 💎  | S0ix firmware advertisement       | ✅ FADT DSM     | ✅ FADT DSM       | ✅ §10           |
| 💎  | MWAIT C-state hint selection      | ✅ PPM          | ✅ intel_idle     | ⚠️ §10 §37       |
| 💎  | mem_sleep s2idle deep             | ✅ S0 idle      | ✅ sysfs          | ⬜ §21           |
| 💎  | powercfg CLI surface              | ✅ 50 cmds      | ⚠️ systemctl      | ⬜ §18           |
| 💎  | Power Options GUI                 | ✅ powercpl     | ⚠️ GNOME basic    | ⬜ §18           |
| ⭐  | Energy aware scheduling           | ⚠️ HW ITD       | ✅ EAS ARM        | ⬜ §19           |
| ⭐  | Battery wear tray hint            | ❌ Settings     | ❌ CLI only       | ⬜ §6            |
| ⭐  | batteryreport plain text          | ✅ HTML         | ❌ None           | ⬜ §18           |
| ⭐  | energy audit trace                | ✅ Full         | ❌ None           | ⬜ §13           |
| ⭐  | sleepstudy DRIPS report           | ✅ Full         | ❌ None           | ⬜ §18           |
| 💎  | PoFx F-states components          | ✅ Per Fx       | ❌ Device only    | ⬜ §12           |
| 💎  | Directed PoFx DRIPS               | ✅ PoFx v3      | ❌ None           | ⬜ §35           |
| 💎  | USB suspend U1 U2 LPM             | ✅ Full         | ✅ autosuspend    | ⬜ §12           |
| 💎  | NVMe APST idle states             | ✅ On           | ✅ sysfs          | ⬜ §12           |
| 💎  | PCIe ASPM L1 substates            | ✅ Plans        | ✅ pcie_aspm      | ⬜ §23           |
| 💎  | SATA ALPM link power              | ✅ HIPM         | ✅ sysfs          | ⬜ §12           |
| 💎  | NIC ARP NS offload S0ix           | ✅ NDIS         | ⚠️ Firmware       | ⬜ §36           |
| 💎  | Smart charge 80 percent           | ✅ OEM          | ⚠️ TLP            | ⬜ §6            |
| 💎  | RAPL power cap sysfs              | ✅ Internal     | ✅ powercap       | ⬜ §15           |
| 💎  | AMD P-State EPP                   | ✅ Driver       | ✅ amd_pstate     | ⬜ §15           |
| 💎  | Energy Saver adaptive             | ✅ Win11        | ⚠️ profiles       | ⬜ §18           |
| ⭐  | Human presence HPD wake           | ✅ Platform     | ❌ None           | ⬜ §32           |
| 💎  | HID-idle QoS throttle (fg-only)   | ✅ 25H2         | ❌ None           | ⬜ §22           |
| 💎  | ACPI GPE block dispatch           | ✅ ACPI.sys     | ✅ acpi_ev_gpe    | ⬜ §24           |
| 💎  | ACPI table extent validation      | ✅ ACPI.sys     | ✅ acpi_tb_verify | ✅ Done §38      |
| 💎  | RSDP v2 extended checksum         | ✅ ACPI.sys     | ✅ acpi_tb_check  | ⬜ §39           |

After §1 through §21, Impossible OS reaches parity for laptop-grade power on real hardware: S-states, D-states, runtime idle including component F-states, USB LPM, NVMe APST, SATA ALPM, thermal, DVFS with HWP CPPC EPP RAPL, C-states, EC, battery with smart charging, power lid HPD events, driver callbacks with query veto, DFx for Modern Standby DRIPS, fast startup, Energy Saver, NIC offloads, power request tracking, and an explicit Linux `mem_sleep` vocabulary map for suspend diagnostics. Linux splits this across drivers, logind, upower, cpufreq, and cpufreq sysfs; Windows is the most integrated reference. Impossible OS adds a software energy model on hybrid CPUs, HPD wake and lock policies Linux lacks, adaptive Energy Saver, and convenient battery wear plus plain-text `powercfg /batteryreport`.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_power()` (see `src/kernel/test/test_runner.c` and `include/kernel/test/test.h`; same pattern as `TODO-11-peb-teb-user-abi.md` Unit Tests).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [/] Create `src/kernel/test/test_power.c` with:
  - §4 did NOT go here. Its 22 suites live in `src/kernel/test/test_hibernate_image.c` (`test_register_hibernate_image()`, `TEST_CAT_BOOT`), named after the source they cover per the repo convention against piggy-backing tests onto an unrelated file. The bullets below are the `pm_*` / `acpi_*` / `cpufreq_*` API surface of §5 through §25 and stay open. -> XREF: `02-kernel-core/TODO-26` §4
  - §7 did NOT go here either. Its eighteen `ACPI: button *` cases live in `src/kernel/test/test_acpi_power.c`, named after the source they cover, same convention as §4 above. -> XREF: `02-kernel-core/TODO-26` §7
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
- [ ] Register in `test_runner_init()`: `test_register_power()` -- extern + call wired in `src/kernel/test/test_runner.c` beside `test_register_pm_idle()`
  - Was marked `[x]` and is NOT done: `grep -c "test_register_power(" src/kernel/test/test_runner.c` returns 0 and `src/kernel/test/test_power.c` does not exist (verified 2026-09-04). Corrected to `[ ]` rather than left claiming a wiring no reader could find.
  - Every §4 through §9 suite that HAS shipped followed the repo convention of naming the test file after the source it covers, so each wired its own registrar instead: `test_register_hibernate_image()` (§4), `test_register_acpi_ec()` (§5), `test_register_acpi_power()` (§7), `test_register_pci_pm()` (§8) and `test_register_pm_callback()` (§9, `src/kernel/test/test_pm_callback.c`, 26 cases). This item now covers only the still-unshipped `pm_*`/`cpufreq_*` surface of §10 onward.
- [ ] Commit: `"test: add power management test suite"`

**Test checkpoint:** `bash scripts/test.sh SUITE=boot` reports every `test_power_*` (and existing `test_acpi_power_*`) case PASS once `test_power.c` lands; until then, `test_acpi_power.c` suite green; `tail -1 build/build.log` is `=== BUILD OK ===`. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## Verification

- [ ] **C1 idle**: with `SleepTimeout=0` and `HibernateTimeout=0`, confirm the CPU stays at `HLT` when the machine is idle and `pm_idle_cycles()` advances on every online CPU (§2)
  - Run `powercfg /query` to read the current policy. NOT an S1 check: §2 deliberately never enters S1 from the idle path.
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
