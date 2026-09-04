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

| ⭐  | Order | Deliverable                                         | Depends On                 | Status |
| --- | :---: | --------------------------------------------------- | -------------------------- | :----: |
| 💎  |   1   | §1 ACPI sleep object parsing & PM1 state machine    | (none)                     |  [x]   |
| 💎  |   2   | §2 C1 idle entry: race-safe HLT + idle accounting   | §1                         |  [x]   |
| 💎  |   3   | §3 S3: suspend to RAM (CPU state + driver freeze)   | §1, §2, §26, §9, D04T03§1  |  [/]   |
| 💎  |   4   | §4 S4: hibernate to disk (image write + resume)     | §3                         |  [ ]   |
| 💎  |   5   | §5 ACPI Embedded Controller (EC) driver             | §1                         |  [ ]   |
| 💎  |   6   | §6 Battery & AC adapter (`_BIF`/`_BIX`/`_BST`)      | §5                         |  [ ]   |
| 💎  |   7   | §7 Power button & lid-close events                  | §5                         |  [ ]   |
| 💎  |   8   | §8 PCI device D-states (D0--D3cold)                 | §1                         |  [ ]   |
| 💎  |   9   | §9 Driver power callbacks & resume ordering         | §3, §8                     |  [ ]   |
| ⭐  |  10   | §10 Connected Standby (S0ix / Modern Standby)       | §2, §9, D02T06§3           |  [ ]   |
| 💎  |  11   | §11 Fast Startup (hybrid shutdown / hiberboot)      | §4, §9                     |  [ ]   |
| 💎  |  12   | §12 Runtime device idle management                  | §8, §9                     |  [ ]   |
| 💎  |  13   | §13 Power request tracking & wake source management | §9, §12                    |  [ ]   |
| 💎  |  14   | §14 ACPI thermal zone management                    | §5, D04T03§4               |  [ ]   |
| 💎  |  15   | §15 CPU frequency scaling governor framework        | §2, D04T03§6, D03T06§9     |  [ ]   |
| 💎  |  16   | §16 CPU idle governor framework                     | §2, D04T03§8               |  [ ]   |
| 💎  |  17   | §17 Driver power query & veto (IRP_MN_QUERY_POWER)  | §9                         |  [ ]   |
| 💎  |  18   | §18 Power plan UI & `powercfg`                      | §6, §7, §13, §14, §15, §16 |  [ ]   |
| ⭐  |  19   | §19 Energy-aware scheduling integration             | §15, §16, D02T09§9         |  [ ]   |
| 💎  |  20   | §20 Power syscalls wired to SSDT                    | §2, §6, D02T12§4           |  [ ]   |
| 💎  |  21   | §21 Linux `/sys/power` suspend variant parity       | §1, §10                    |  [ ]   |
| 💎  |  22   | §22 User-interaction-aware QoS throttling           | §15, §19, §20              |  [ ]   |
| 💎  |  23   | §23 PCIe ASPM and L1 substates                      | §8, §9, §12                |  [ ]   |
| 💎  |  24   | §24 ACPI general-purpose event (GPE) blocks         | §1, §5, §7                 |  [ ]   |
| 💎  |  25   | §25 Per-CPU idle accounting via NtQuerySystemInfo   | §2                         |  [ ]   |
| 💎  |  26   | §26 Stop-the-world CPU rendezvous for sleep         | §2                         |  [x]   |
| 💎  |  27   | §27 Rendezvous safety residue: seam + retract gap   | §26                        |  [ ]   |

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
  - Making those reads well-defined did NOT make the pre-existing exact-delta assertions in `test_dpc_insert_remove()` interference-proof, and that residue is filed with its owner rather than fixed here -> XREF: `02-kernel-core/TODO-07-irql-model-dpcs.md` §19 (item: "Give the depth assertions an observation that cannot be perturbed by unrelated queue traffic").
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
> **Accepted:** [M] `test_dpc_insert_remove()`'s exact-delta depth assertions are not interference-proof; this section made the reads well-defined and changed no assertion (reason: DPC test-surface design belongs to its owner) -> XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §19 (item: "Give the depth assertions an observation that cannot be perturbed by unrelated queue traffic")
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
- [ ] Evaluate `_PTS(4)` before the S4 PM1 write and `_WAK(4)` on resume
  - Same contract as the S3 path and for the same reason. Needs the AML evaluator.
  - -> XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md` §1 (item: "`acpi_evaluate(path, args, result)` wrapper around `AcpiEvaluateObject`")
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
- [ ] `bat_set_charge_limit(uint8_t pct)` -- refuses (`STATUS_NOT_SUPPORTED`) unless the platform is capability-matched; default 100% (no limit)
  - Codex adversarial finding (2026-09-03): a raw `ec_write(EC_REG_CHARGE_END, pct)` at a vendor-guessed offset (0xB1 ThinkPad, 0xE4 Dell, 0xBD ASUS) is unsafe on an unmatched EC map -- the same offset can control unrelated firmware state on a different vendor's controller.
  - Identify the platform (DMI/SMBIOS vendor+model string, or an ACPI `_DSM`/OEM method if the DSDT exposes one) and match against a per-model quirks table before any write; unmatched platforms stay refused, never a best-effort guess.
  - Validate + read back after write (range-check `pct`, confirm the EC actually latched the value) before reporting success.
- [ ] Registry `HKLM\SYSTEM\Battery\ChargeLimitPercent` (REG_DWORD, default 100); set to 80 for battery longevity
- [ ] Smart Charging auto-mode: if laptop has been plugged in for > 4 hours continuously and battery > 80%, auto-hold at 80%; release limit when unplugged; Registry `SmartChargingEnabled` (default 1)
- [ ] Tray tooltip addition: `"Charging limited to 80%"` when charge limit active
- [ ] `powercfg /batteryreport` includes charge limit status and smart charging history
- [ ] Enumerate every `_HID "PNP0C0A"` battery device in the namespace (not a fixed `\_SB.BAT0`) -- dual-battery and docked systems have separate ACPI objects per battery
  - Codex gap-audit finding (2026-09-03): a single fixed-path read reports the wrong percentage and can trigger critical-power action from the wrong source on multi-battery hardware. -> XREF: ACPI 6.6 §10 Power Source and Power Meter Devices.
- [ ] Track insertion/removal per battery device (ACPI device-check notify); a hot-removed battery drops out of the composite view rather than reporting stale data
- [ ] `bat_composite_state()` -- aggregates all present batteries into one system-wide charge/time/critical state (Windows composite-battery model); tray/tooltip/flyout/warnings read the composite, not one battery
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
  - Not the same signal as §22's user-interaction-aware QoS throttling: HPD needs presence-sensing hardware, QoS-based throttling is software-only and needs no sensor -> XREF: `02-kernel-core/TODO-26-power-management.md` §22
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
- [ ] TSC recalibration (→ XREF: `TODO-08-time-filetime-management.md §3`) may be needed if `mwait` C10 was held for > 1 second (TSC stops in deep C-states on some CPUs)
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
- [ ] Wake-source attribution: record WHICH GPE resumed the machine, so wake reporting has a real source instead of the bare `PM_WAKE_GPE` tag §13 carries today. -> XREF: `02-kernel-core/TODO-26-power-management.md` §13
- [ ] PM1b-only platform support, END TO END
  - `acpi_pm1_control_owned()` already reads SCI_EN from both control blocks ORed (matching `AcpiHwReadMultiple`).
  - But `acpi_enable_fixed_events()` returns early without a PM1a EVENT block and `acpi_sci_process()` returns without a PM1a status port.
  - So such a machine could enter S1 and then never acknowledge or count its wake -- a false resume failure with the level-triggered SCI left asserted. `acpi_enter_sleep_state()` therefore REQUIRES PM1a today, deliberately.
  - Work: initialise and service either event block independently, require at least one valid event block rather than PM1a specifically, and drop the PM1a-only early return from the SCI path.
  - Filed from the section-1 review (round 7), 2026-09-03: it is a feature, not a review fix, and half-supporting it is worse than requiring PM1a.
  - Work: initialise and service either event block independently, require at least one valid event block rather than PM1a specifically, and drop the PM1a-only early return from the SCI path.
  - Filed from the §1 review (round 7), 2026-09-03: it is a feature, not a review fix, and half-supporting it is worse than requiring PM1a.
- [ ] Unit tests over synthetic GPE block images through a test-only entry point
  - Same shape as `acpi_parse_sleep_type_test`.
  - Cover the status/enable split, an asserted-but-unenabled bit left alone, an unhandled GPE still acknowledged, and a block length that cannot be halved.
- [ ] Commit: `"kernel/acpi: GPE block enable, dispatch, and wake-source attribution"`

**Test checkpoint:** a GPE block splits into equal status/enable halves and a non-halvable length is refused; an asserted-and-enabled GPE bit is acknowledged exactly once; an asserted-but-disabled bit is untouched; a GPE with no registered handler is still acknowledged so the SCI line drops. Test on: QEMU TCG (raises no GPEs -- structural tests only), bare metal (the only place the real path is exercised).

> **Notes:**
> - **Why this is a new section rather than an item somewhere:** `grep -rn GPE todo/` finds two passing mentions and no owner -- §7 assumes "ACPI GPE fires when lid state changes" and §13 carries a bare `PM_WAKE_GPE` enum tag -- and `04-drivers-hardware/TODO-03` has no GPE mention at all across its ten sections. Nothing owns block discovery, enable, or acknowledgement, so this is ownerless work, not a duplicate of existing coverage.
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
- [ ] Bracket every SLP_EN write with the §26 rendezvous at a deadlock-safe point
  - §26 shipped the barrier and NOTHING calls it: `acpi_enter_sleep_state()` still writes PM1 SLP_EN with other processors live, so the hazard §2 recorded is unchanged in behaviour even though the primitive it needs now exists. A shipped primitive with no caller closes no hazard.
  - The bracket must go where the contract allows: callbacks, device quiesce and any cache or journal flush BEFORE `smp_rendezvous_begin()`, with the barrier replacing the bare `cli` immediately before the hardware transition.
  - Applies to every sleep path, not only S3: S1 goes through the same function today and inherits the same gap -> XREF: `02-kernel-core/TODO-26` §3 (item: "`pm_enter_s3()` -- runs on a dedicated `PASSIVE_LEVEL` worker, NOT `DISPATCH_LEVEL`")
- [ ] Commit: `"kernel/smp: rendezvous test seam, mutation-proving tests, retract-side quiescence"`

**Test checkpoint:** deleting any one of the four named lines (the fail-closed release, `lapic_eoi()` in the handler, the second generation re-read in `park_step`, the active-round refusal in `smp_publish_cpu_online()`) makes a specific named test FAIL. `bash scripts/test.sh SUITE=x86` green. Test on: QEMU TCG + KVM, 1 and 2 CPUs (`scripts/test-smoke-matrix.sh`).

---

## OS Comparison

| ⭐  | Feature                         | 🪟 Win11        | 🐧 Linux         | 🚀 Impossible OS |
| --- | ------------------------------- | --------------- | ---------------- | ---------------- |
| 💎  | S5 ACPI shutdown                | ✅ Full         | ✅ Full          | ✅ Done §1       |
| 💎  | ACPI S-state discovery          | ✅ ACPI.sys     | ✅ acpi_sleep    | ✅ Done §1       |
| 💎  | PM1 fixed-event SCI             | ✅ ACPI.sys     | ✅ acpi_sci      | ✅ Done §1       |
| 💎  | C1 idle / HLT                   | ✅ Full         | ✅ cpuidle       | ⬜ §2            |
| 💎  | S3 suspend RAM                  | ✅ Full         | ✅ sleep         | ⬜ §3            |
| 💎  | Stop-the-world CPU rendezvous   | ✅ KeIpiGeneric | ✅ stop_machine  | ✅ Done §26      |
| 💎  | Stop-the-world fault injection  | ✅ Internal     | ✅ ftrace stress | ⬜ §27           |
| 💎  | S4 hibernate disk               | ✅ Full         | ✅ swsusp        | ⬜ §4            |
| 💎  | Fast startup hiberboot          | ✅ Default      | ❌ None          | ⬜ §11           |
| 💎  | ACPI EC driver                  | ✅ Full         | ✅ acpi_ec       | ⬜ §5            |
| 💎  | Battery `_BIX` / `_BST`         | ✅ Full         | ✅ upower        | ⬜ §6            |
| 💎  | Power lid button events         | ✅ Full         | ✅ logind        | ⬜ §7            |
| 💎  | PCI D-states D0--D3cold         | ✅ Full         | ✅ PCI PM        | ⬜ §8            |
| 💎  | Driver sleep wake callbacks     | ✅ WDM          | ✅ pm_ops        | ⬜ §9            |
| 💎  | Driver query veto power         | ✅ QUERY_POWER  | ✅ prepare       | ⬜ §17           |
| 💎  | Runtime idle PoFx RPM           | ✅ PoFx         | ✅ runtime_pm    | ⬜ §12           |
| 💎  | Power requests tracking         | ✅ powercfg     | ⚠️ wake_lock     | ⬜ §13           |
| 💎  | Wake source lastwake            | ✅ powercfg     | ⚠️ dmesg         | ⬜ §13           |
| 💎  | ACPI thermal zones              | ✅ ACPI.sys     | ✅ thermal       | ⬜ §14           |
| 💎  | Passive active cooling          | ✅ Full         | ✅ step_wise     | ⬜ §14           |
| 💎  | CPU DVFS cpufreq                | ✅ PPM HWP      | ✅ cpufreq       | ⬜ §15           |
| 💎  | CPU idle C-states               | ✅ PPM          | ✅ menu teo      | ⬜ §16           |
| 💎  | Connected standby S0ix          | ✅ Modern       | ⚠️ Partial       | ⬜ §10           |
| 💎  | mem_sleep s2idle deep           | ✅ S0 idle      | ✅ sysfs         | ⬜ §21           |
| 💎  | powercfg CLI surface            | ✅ 50 cmds      | ⚠️ systemctl     | ⬜ §18           |
| 💎  | Power Options GUI               | ✅ powercpl     | ⚠️ GNOME basic   | ⬜ §18           |
| ⭐  | Energy aware scheduling         | ⚠️ HW ITD       | ✅ EAS ARM       | ⬜ §19           |
| ⭐  | Battery wear tray hint          | ❌ Settings     | ❌ CLI only      | ⬜ §6            |
| ⭐  | batteryreport plain text        | ✅ HTML         | ❌ None          | ⬜ §18           |
| ⭐  | energy audit trace              | ✅ Full         | ❌ None          | ⬜ §13           |
| ⭐  | sleepstudy DRIPS report         | ✅ Full         | ❌ None          | ⬜ §18           |
| 💎  | PoFx F-states components        | ✅ Per Fx       | ❌ Device only   | ⬜ §12           |
| 💎  | Directed PoFx DRIPS             | ✅ PoFx v3      | ❌ None          | ⬜ §10           |
| 💎  | USB suspend U1 U2 LPM           | ✅ Full         | ✅ autosuspend   | ⬜ §12           |
| 💎  | NVMe APST idle states           | ✅ On           | ✅ sysfs         | ⬜ §12           |
| 💎  | PCIe ASPM L1 substates          | ✅ Plans        | ✅ pcie_aspm     | ⬜ §23           |
| 💎  | SATA ALPM link power            | ✅ HIPM         | ✅ sysfs         | ⬜ §12           |
| 💎  | NIC ARP NS offload S0ix         | ✅ NDIS         | ⚠️ Firmware      | ⬜ §10           |
| 💎  | Smart charge 80 percent         | ✅ OEM          | ⚠️ TLP           | ⬜ §6            |
| 💎  | RAPL power cap sysfs            | ✅ Internal     | ✅ powercap      | ⬜ §15           |
| 💎  | AMD P-State EPP                 | ✅ Driver       | ✅ amd_pstate    | ⬜ §15           |
| 💎  | Energy Saver adaptive           | ✅ Win11        | ⚠️ profiles      | ⬜ §18           |
| ⭐  | Human presence HPD wake         | ✅ Platform     | ❌ None          | ⬜ §7            |
| 💎  | HID-idle QoS throttle (fg-only) | ✅ 25H2         | ❌ None          | ⬜ §22           |
| 💎  | ACPI GPE block dispatch         | ✅ ACPI.sys     | ✅ acpi_ev_gpe   | ⬜ §24           |

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
