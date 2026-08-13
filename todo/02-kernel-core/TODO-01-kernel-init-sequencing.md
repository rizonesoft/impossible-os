---
schema_version: 1
id: kernel-init-sequencing
domain: 02-kernel-core
status: active
title: "TODO-01 -- Kernel Init Sequencing"
---

# TODO-01 -- Kernel Init Sequencing

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

> **Goal:** Replace the ad-hoc 5-phase boot sequence with a formal, dependency-gated init model. Every subsystem declares prerequisites, returns a typed result, and the kernel halts or degrades gracefully on failure. Phase boundaries are explicit, testable, and match the hardware bring-up contract expected by a production OS.

> [!IMPORTANT]
> **Current state:** `kernel_main()` calls five sequential functions with no error returns, no dependency checks, and `HV_BAR` pixel-write debug cruft in every file. ACPI/SMP are split across `boot_interrupts.c` and `boot_storage.c`. VFS headers are included inside the interrupts phase. Tests run unconditionally in the boot path.

## Inputs

- [`src/kernel/main.c`](../../src/kernel/main.c)
- [`src/kernel/main/boot_hw.c`](../../src/kernel/main/boot_hw.c)
- [`src/kernel/main/boot_interrupts.c`](../../src/kernel/main/boot_interrupts.c)
- [`src/kernel/main/boot_storage.c`](../../src/kernel/main/boot_storage.c)
- [`src/kernel/main/boot_desktop.c`](../../src/kernel/main/boot_desktop.c)
- [`src/kernel/main/boot_tests.c`](../../src/kernel/main/boot_tests.c)
- [`src/kernel/panic.c`](../../src/kernel/panic.c)
- [`src/kernel/boot_timing.c`](../../src/kernel/boot_timing.c)
- → XREF: `TODO-04-system-logging.md` -- `klog_disk_enable()` is a Phase 2 gate; must follow VFS ready
- → XREF: `TODO-05-object-manager.md §1` -- Object Manager init slot is Phase 2, after heap, before registry; §1 provides the `ob_init()` implementation
- → XREF: `04-drivers-hardware/INDEX.md` -- all driver `_init()` functions must accept and return `boot_result_t`
- Verification path: [`scripts/test.sh`](../../scripts/test.sh) runs headless QEMU; serial log is the primary verification artifact (not an XREF target, see §Verification)
- → XREF: `TODO-07-irql-model-dpcs.md §4` -- DPC subsystem init belongs in Phase 1, after timer; §4 is the DPC Object Type and Per-CPU Queue init
- → XREF: `TODO-08-time-filetime-management.md §3` -- `wall_clock_init()` belongs in Phase 2, after UEFI runtime services; NTP wall clock adjustment (§17) belongs in Phase 3
- → XREF: `01-boot-platform/TODO-23-boot-watchdog.md` -- watchdog timer integrates with `boot_progress()` calls; detects hung subsystem init
- → XREF: `TODO-02-kernel-configuration-policy.md §2, §4, §5` -- Phase 0 config snapshot publication, control-set success criteria, and Safe Mode policy consume these phase boundaries
- → XREF: `04-drivers-hardware/TODO-04-security-hardware.md §6` -- TPM2 `PCR_Extend` for measured boot; extends the PCR event log parsed in Phase 0
- → XREF: `01-boot-platform/TODO-09-cpu-boot-sequencing.md §2` -- CPU security activation order (EFER/CR4 hardening) slots into Phase 0 between serial init and PMM
- → XREF: `TODO-29-kernel-debugger-kd-protocol.md` §4, §15 -- `kd_init()` placement relative to COM IRQ + IDT and transport coexistence notes

## Outcome

- `main.c` is reduced to four sequential phase calls: `boot_phase0()` → `boot_phase1()` → `boot_phase2()` → `boot_phase3()` → halt.
- Every subsystem init function returns `boot_result_t`; none return `void`.
- `kernel_subsystem_ready(SUBSYS_X)` correctly reports the live state of every registered subsystem.
- Serial log shows `[PHASE0]` … `[PHASE3]` markers with POST codes and timestamps.
- No `HV_BAR` pixel-write calls anywhere in the boot path.
- Boot tests run only when `debug=1`; release boots are silent on that path.
- A forced VFS failure produces a degraded-boot screen, not a BSOD or hang.
- Non-critical subsystems (NIC, DHCP, VBox mouse) defer init to after desktop appears, reducing time-to-desktop.
- Boot performance regression detection compares timing between runs and flags slowdowns.
- Independent subsystems within a phase can init concurrently on SMP systems.

## Implementation Order

| ⭐  | Order | Deliverable                               | Depends On | Status |
| --- | :---: | ----------------------------------------- | ---------- | :----: |
| 💎  |   1   | Boot init infrastructure                  | --         |  [x]   |
| 💎  |   2   | Phase 0 -- critical init                  | §1         |  [x]   |
| 💎  |   3   | Phase 1 -- platform services              | §2         |  [/]   |
| 💎  |   4   | Phase 2 -- system services                | §3         |  [/]   |
| 💎  |   5   | Phase 3 -- user platform                  | §4         |  [x]   |
| 💎  |   6   | Dependency gates                          | §1--§5     |  [/]   |
| 💎  |   7   | Failure policy                            | §6         |  [/]   |
| 💎  |   8   | Code cleanup                              | §2--§5     |  [/]   |
| ⭐  |   9   | Degraded-boot recovery screen             | §7         |  [/]   |
| ⭐  |  10   | POST code + UEFI variable log             | §1         |  [x]   |
| 💎  |  11   | Deferred init for non-critical subsystems | §5         |  [/]   |
| ⭐  |  12   | Boot performance regression detection     | §10        |  [/]   |
| ⭐  |  13   | Async subsystem init (SMP parallel)       | §6, §11    |  [/]   |

> 💎 = parity -- Windows NT and Linux both have formal init phase models; Impossible OS must match them.
> ⭐ = exclusive -- degraded-boot recovery UI, UEFI NVRAM POST log, boot perf regression detection, and SMP parallel init.

---

## 1. Boot Init Infrastructure
New header and source file providing the result type, readiness oracle, and progress tracker used by every phase.

**Files:** `include/kernel/boot_init.h`, `src/kernel/main/boot_init.c`

- [x] Define `boot_result_t`: `BOOT_OK = 0`, `BOOT_DEGRADED = 1`, `BOOT_FATAL = 2`, `BOOT_DEFERRED = 3`
- [x] Define `kernel_subsys_t` enum -- one entry per subsystem that others can depend on: `SUBSYS_SERIAL`, `SUBSYS_PMM`, `SUBSYS_VMM`, `SUBSYS_HEAP`, `SUBSYS_KLOG`, `SUBSYS_GDT`, `SUBSYS_IDT`, `SUBSYS_ACPI`, `SUBSYS_LAPIC`, `SUBSYS_IOAPIC`, `SUBSYS_TIMER`, `SUBSYS_RTC`, `SUBSYS_FB`, `SUBSYS_VFS`, `SUBSYS_REGISTRY`, `SUBSYS_SCHED`, `SUBSYS_IPC`, `SUBSYS_SMP`, `SUBSYS_EXEC`, `SUBSYS_DESKTOP`, `SUBSYS_OB`, `SUBSYS_COUNT`
- [x] Implement static `bool g_subsys_ready[SUBSYS_COUNT]` table in `boot_init.c`
- [x] Implement `bool kernel_subsystem_ready(kernel_subsys_t subsys)`
- [x] Implement `void kernel_subsystem_set_ready(kernel_subsys_t subsys, bool ok)`
- [x] Implement `void kernel_subsystem_dump(void)` -- prints all subsystem states via `klog`
- [x] Define POST code constants for every major init step (`POSTCODE_PMM_INIT = 0x20`, etc.)
- [x] Implement `void boot_progress(uint8_t phase, const char *step, uint16_t postcode)` -- writes `[PHASEn] step` to serial and records timestamp in `boot_timing.c`
- [x] Define `BOOT_REQUIRE(subsys)` macro -- if `!kernel_subsystem_ready(subsys)`, logs the missing prerequisite and returns `BOOT_FATAL`
- [x] Define `BOOT_STEP(subsys, fn)` macro -- calls `fn()`, marks subsystem ready iff result is `BOOT_OK` or `BOOT_DEGRADED` (caller invokes `boot_progress()` separately)
- [x] Expose `boot_phase0()`, `boot_phase1()`, `boot_phase2()`, `boot_phase3()` in `main_internal.h`

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | oracle/enum/macros covered by `test_boot_init.c` (boot_result_t values, oracle round-trip, BOOT_REQUIRE, enum layout, apply_result dual-channel), 0 failures
>
> **Notes:**
> - Boot init infra (`boot_init.h`/`boot_init.c`): `boot_result_t` (4 codes), `kernel_subsys_t` (SUBSYS_COUNT=27), `g_subsys_ready[]` oracle, `boot_progress()` serial+TSC emitter, `BOOT_REQUIRE`/`BOOT_STEP` macros, `kernel_subsystem_apply_result`.
> - Readiness oracle is lock-free: `__atomic` acquire/release per uint8 slot; safe for AP + many-subsystem reads (BSP-only writes during init).
> - Consumed by every phase (`boot_phase0-3`) + per-section `BOOT_REQUIRE` gates; dependency-chain enforcement lives in §6.
> - Canonical contract: `include/kernel/boot_init.h` (`_Static_assert` pins the enum count + `degraded_mask` <= 32 width).
> - Scope: §1 owns the result type / oracle / macros; `boot_async_group` lives in `boot_init.c` but is owned by §13 (carries this review's timeout-quiescence + DEFERRED-rank findings).
>
> **Verified:** 2026-06-20 | commit `0d72fce6` | 11/11 items | build OK
> **Accepted:** [H] async-timeout AP not quiesced before sequential fallback (driver-global corruption, `async_init=1`) -> XREF: 02-kernel-core/TODO-01 §13 (item: "Quiesce the AP on async timeout before sequential fallback" at line 543) (still deferred -- `async_init=1`-only, see §13 Deferred stamp)
> **Accepted:** [M] `boot_async_group` numeric worst-pick lets BOOT_DEFERRED outrank BOOT_FATAL -> XREF: 02-kernel-core/TODO-01 §13 (item: "Explicit `boot_result_t` severity rank in `boot_async_group`" at line 544) (RESOLVED 2026-06-20 by §13 commit 308eb661: `boot_result_severity()` FATAL>DEGRADED>DEFERRED>OK applied at all 3 worst-pick sites)
> **Quality reviewed:** 2026-06-20 | Codex 3x (adversarial, consistency, perf) | 1M fixed, 1H+1M accepted-XREF | scope: kernel-code-quality

---

## 2. Phase 0 -- Critical Init (Interrupts Disabled)
Runs with interrupts off. Only serial, memory, and logging. No drivers, VFS, or network. Any failure in Phase 0 calls `boot_halt()` on serial -- framebuffer is not yet available.

**File:** `src/kernel/main/boot_hw.c` (restructured as `boot_phase0`)

- [x] `serial_init()` -- absolute first call; no dependencies; POST code 0x10
- [x] `boot_info_parse(magic, mbi)` -- parse UEFI or Multiboot2 info; halt on unknown magic
- [x] `uefi_runtime_init()` -- `SetVirtualAddressMap` + runtime props; BOOT_DEGRADED if unavailable
- [x] `uefi_vars_init()` -- NVRAM variable enumeration; BOOT_DEGRADED if unavailable. Returns `boot_result_t`; `boot_phase0()` captures the value, sets `SUBSYS_UEFI_VARS` ready iff BOOT_OK or BOOT_DEGRADED, logs WARN on degraded.
- [x] `uefi_time_init()` -- seed wall clock from UEFI RTC; BOOT_DEGRADED if unavailable. Same propagation pattern; sets `SUBSYS_UEFI_TIME`.
- [x] `uefi_secureboot_init()` -- detect Secure Boot state; BOOT_DEGRADED if unavailable. Same propagation pattern; sets `SUBSYS_SECUREBOOT`.
- [x] `tpm_init()` -- parse measured boot event log; BOOT_DEGRADED if no TPM. Returns `boot_result_t`; combined with `tpm_integrity_init()` to set `SUBSYS_TPM` ready (both must be OK or DEGRADED). `tpm.c:59`.
- [x] `tpm_integrity_init()` -- PCR golden value check; BOOT_DEGRADED on mismatch. Only called when `tpm_init()` returned OK/DEGRADED. Result combined into `SUBSYS_TPM` readiness. `tpm.c:243`.
- [x] `pmm_init()` -- physical memory manager; BOOT_FATAL if fails; POST code 0x20
- [x] `vmm_init()` -- virtual memory manager; BOOT_FATAL if fails; BOOT_REQUIRE(SUBSYS_PMM)
- [x] `heap_init()` -- kernel heap; BOOT_FATAL if fails; BOOT_REQUIRE(SUBSYS_VMM)
- [x] `klog_early_init()` -- in-memory ring buffer only (no disk yet); BOOT_REQUIRE(SUBSYS_HEAP)
- [x] `cpuid_init()` -- probe CPU features; BOOT_DEGRADED on very old CPU
- [x] `simd_enable_avx()` -- enable AVX2 or fall back to SSE2; BOOT_DEGRADED on no AVX2
- [x] boot.conf delivery -- parsed by the **UEFI bootloader** (not the kernel) and delivered in `g_boot_info.config`; kernel consumes + logs the values via `boot_hw.c:93-102` and falls back to defaults via the `config_found` flag. (Originally listed as `boot_config_parse()` -- corrected to reflect that no kernel-side parser symbol exists; the work is in the bootloader.)
- [x] Remove driver includes (`ata.h`, `virtio_blk.h`, `ahci.h`) from this file -- they belong in Phase 2
- [x] Remove SMBIOS, ESRT, UEFI conformance, GOP mode log from Phase 0 -- move to Phase 1
- [x] Remove all `HV_BAR` macro definitions and usages (12 sites in `boot_hw.c`) -- already removed in prior commit
- [x] Replace every `HV_BAR` site with `boot_progress(0, "step-name", postcode)` -- already done in prior commit

> [!NOTE]
> **Implementation drift (verify 2026-04-08):** `boot_phase0()` performs five steps that were added after this checklist was written and are not enumerated above: (1) `smp_early_bsp_init()` first thing -- sets GS_BASE before any interrupt fires, (2) `cpu_harden()` -- enables NX before page tables are touched, (3) `vmm_apply_nx_policy()` -- applies NX to page tables, (4) PAT MSR reprogramming with readback verify -- intent is WC framebuffer mapping (see follow-up below for actual behavior), (5) `cpu_harden_post_pagetable()` + `cpu_verify_hardening()` -- intended to enable SMEP/SMAP, but `hv_supports_cr4_smep_smap()` currently returns 0 globally (CLAUDE.md "No SMEP/SMAP until per-process page tables"), so this is a documented architectural no-op until per-process PML4 is implemented. Plus the LAPIC UC mapping smoke test (validates `vmm_map_mmio_uc()`) and `boot_perf_read_prev()` + `vpd_init()` calls.

**Verify follow-ups (Codex 2026-04-08, out of §2 scope -- defer to owning subsystem):**
- [x] **PAT MSR constant + AP propagation bug** -- RESOLVED in `01-boot-platform/TODO-09 §8` (2026-05-24): `PAT_WC_VALUE` -> `0x0007040600070106` (PA1=WC, `_Static_assert`-pinned); AP PAT via single §4 registry-replay write + readback verify.
- [x] **UEFI handoff pointer validation** -- RESOLVED in `01-boot-platform/TODO-03 §16`: `boot_phase0` validates the handoff via `boot_info_validate_addr()` + `boot_version_classify()` (`boot_hw.c:111,132`) before touching it.

**Resolved (implement 2026-04-08):** Phase 0 BOOT_DEGRADED propagation. Added 4 new readiness slots (`SUBSYS_UEFI_VARS`, `SUBSYS_UEFI_TIME`, `SUBSYS_SECUREBOOT`, `SUBSYS_TPM`) and wired `uefi_vars_init`, `uefi_time_init`, `uefi_secureboot_init`, `tpm_init` + `tpm_integrity_init` to set them via captured `boot_result_t`. Items 4-8 above are now `[x]`. New unit test `test_subsys_phase0_propagation_slots` exercises the oracle round-trip on the new slot indices via the save/restore pattern (no live boot calls, per the test side-effect ban). `test_subsys_enum_layout` updated for new SUBSYS_COUNT == 25.

**Codex finding REJECTED (verify 2026-04-08):** Codex flagged `cpu_verify_hardening()` as advisory (logs WARN instead of halting on SMEP/SMAP missing). This matches the documented architectural state -- SMEP/SMAP enablement is gated on `hv_supports_cr4_smep_smap()` which always returns 0 until per-process page tables exist. The verify routine is correctly informational. The proper fix is per-process PML4 (covered by other TODOs), not Phase 0 hardening enforcement.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | Phase-0 propagation slots covered by `test_subsys_phase0_propagation_slots` + `test_subsys_apply_result_dual_channel` (test_boot_init.c); boot-path validated via smoke test (boot reaches `C:\>`)
>
> **Notes:**
> - Phase 0 critical init (`boot_hw.c` `boot_phase0`): interrupts-off bring-up of serial, validated `boot_info` handoff, UEFI services, TPM, PMM/VMM/heap (FATAL->`boot_halt`), klog, cpuid, SIMD, NX/PAT/hardening.
> - BOOT_DEGRADED propagated to SUBSYS_UEFI_VARS/TIME/SECUREBOOT/TPM via `kernel_subsystem_apply_result` (oracle + degraded_mask).
> - Both prior Verify-follow-ups resolved by owners: PAT constant + AP-sync -> TODO-09 §8 (FIXED); UEFI handoff validation -> TODO-03 §16 (`boot_info_validate_addr` + `boot_version_classify`).
> - LAPIC UC MMIO smoke test now gated behind `debug` (Codex perf+consistency: was unconditional pre-`sti` debug work emitting raw `0xD1xx` POST codes).
> - Scope: §2 owns Phase-0 init order + readiness propagation (CLAUDE.md "boot_info ABI"); handoff defense-in-depth hardening is owned by TODO-10 §7.
>
> **Verified:** 2026-06-20 | commit `e8a8069b` | 21/21 items | build OK | smoke PASS (KVM 2.65s)
> **Accepted:** [H] boot_phase0 handoff hardening beyond the documented 2-stage validate (canonical addr-pin, fb-geometry validation, bulk-copy) -> XREF: 01-boot-platform/TODO-10 §7 (item: "Harden boot_phase0 handoff" at line 341)
> **Quality reviewed:** 2026-06-20 | Codex 3x (adversarial, consistency, perf) | 2M fixed, 2H+1M accepted-XREF | scope: kernel-code-quality

---

## 3. Phase 1 -- Platform Services (Interrupts Enabled at End)
Hardware abstraction layer: GDT/IDT, interrupt controllers, timer, RTC, display. BOOT_FATAL halts; BOOT_DEGRADED logs and continues. Interrupts enabled with `sti` only after LAPIC/timer are ready.

**File:** `src/kernel/main/boot_interrupts.c` (restructured as `boot_phase1`)

- [x] `gdt_init()` -- BOOT_FATAL; POST code 0x30
- [x] `idt_init()` + `irq_init()` -- BOOT_FATAL; BOOT_REQUIRE(SUBSYS_GDT)
- [x] `acpi_init()` -- MADT + FADT parsing only (CPU count, LAPIC base, IOAPIC base, PM1a port); BOOT_FATAL; BOOT_REQUIRE(SUBSYS_IDT)
- [x] `lapic_init()` -- BOOT_FATAL on APIC-only platforms, BOOT_DEGRADED if PIC fallback available; BOOT_REQUIRE(SUBSYS_ACPI)
- [x] `ioapic_init()` -- BOOT_DEGRADED if unavailable; BOOT_REQUIRE(SUBSYS_LAPIC)
- [x] `pic_disable()`/`pic_init()` policy -- disable if IOAPIC took over; init if PIC is the only controller
- [x] Move `ahci_setup_interrupts()` out of Phase 1; call it in Phase 2 after storage driver init and PCI discovery
- [x] `timer_hal_init()` -- select LAPIC or PIT backend, calibrate; BOOT_FATAL; BOOT_REQUIRE(SUBSYS_IDT)
- [x] DPC bootstrap split: `dpc_init_queues()` in Phase 1 before `sti`, and `dpc_init()`/`dpc_start_threads()` in Phase 3 after scheduler start (→ XREF: [TODO-07 §4](./TODO-07-irql-model-dpcs.md))
- [x] `rtc_init()` -- BOOT_DEGRADED if unavailable; BOOT_REQUIRE(SUBSYS_IDT)
- [x] `keyboard_init()` -- BOOT_DEGRADED if unavailable (PS/2 `mouse_init()` is deferred to `boot_run_deferred()`, not Phase 1; owned by [§11](#11-deferred-init-for-non-critical-subsystems))
- [x] `smbios_init()` -- POST code 0x40; BOOT_DEGRADED if unavailable; move here from Phase 0
- [x] `esrt_init()` + `mat_init()` + `uefi_conformance_init()` + `uefi_capsule_init()` + `uefi_crypto_agility_init()` -- BOOT_DEGRADED; move here from Phase 0
- [x] `secureboot_keys_init()` -- BOOT_DEGRADED; move here from Phase 0
- [x] `fb_init()` + `boot_splash_init()` -- BOOT_DEGRADED; display is optional for kernel correctness
- [x] `boot_timing_init()` + boot timing report log path after timer calibration
- [x] Wire `except_init()` in Phase 1 after IDT/IRQ setup (`boot_phase1()` calls it right after `idt_init()`; must be post-clear) (→ XREF: [TODO-23-exception-dispatch-seh.md §3](./TODO-23-exception-dispatch-seh.md))
- [/] Call `kd_init()` from Phase 1 after the COM IRQ path and IDT vectors KD relies on are registered, gated by boot args / registry per [TODO-29-kernel-debugger-kd-protocol.md §4](./TODO-29-kernel-debugger-kd-protocol.md) and ordering notes in §15 (-> XREF `TODO-29-kernel-debugger-kd-protocol.md §15`)
- [x] `__asm__ volatile ("sti")` -- enable interrupts only after all of the above
- [x] `boot_splash_start_animation()` -- after STI so LAPIC timer can drive the spinner
- [x] Remove `#include "kernel/fs/vfs.h"` and `#include "kernel/fs/partition.h"` from this file
- [x] Remove PCI scan, NIC init, DHCP from Phase 1 -- move to Phase 2
- [x] Remove SMP AP bringup from Phase 1 -- move to Phase 2 (already in boot_storage.c)
- [x] Remove all `HV_BAR` macro definitions and usages -- already removed in prior commit
- [x] Replace every `HV_BAR` site with `boot_progress(1, "step-name", postcode)` -- already done

**Test checkpoint:** Boot reaches Phase 1 -- serial shows `[GDT]`, `[IDT]`, `[ACPI]`, `[LAPIC_IOAPIC]`, `[TIMER]` progress lines, `sti` enables interrupts, splash spinner animates, `[KEYBOARD]` ready; `[PHASE1] complete` logs before Phase 2. PS/2 mouse comes up later via `[DEFERRED]` input after desktop. Verify on QEMU WHPX, TCG, VirtualBox, bare metal.

> **Test runner:** boot suite `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot, 2836 kernel tests) + per-subsystem driver suites; `boot_phase1` integration validated via smoke test (boot reaches `C:\>`, POST16 core 31/31).
>
> **Notes:**
> - Phase 1 HAL bring-up (`boot_interrupts.c` `boot_phase1`): GDT/IDT/IRQ, ACPI MADT/FADT, LAPIC/IOAPIC-vs-PIC policy, `timer_hal_init` + `dpc_init_queues`, then `sti`, then post-`sti` TPM transport + CSPRNG seed + keyboard.
> - Review fix (adversarial M): `esrt_init()` now bounds the firmware ESRT config-table header + entry-span read via `firmware_table_mmap_contains()`, matching the MAT/conformance paths -- closes a Phase-1 firmware overread/`#PF`.
> - Review fix (consistency M): removed the false Phase-1 `POST16_MOUSE_OK` / `boot_progress(1,"MOUSE")` emission -- PS/2 `mouse_init()` runs in `deferred_input_init()` (§11), so Phase 1 no longer certifies mouse readiness before the device is probed.
> - re-adversarial skipped: fix diff is a bounds-check mirroring the existing MAT pattern + removal of a misleading POST emission; no locking/ISR/lifecycle/state-machine surface, < 50 lines.
> - Scope: §3 owns Phase 1 init order + `sti` placement; per-subsystem typed fatal/degraded propagation is §2/§4, deferred PS/2 mouse is §11.
>
> **Verified:** 2026-06-20 | commit `840d0ea8` | 23/25 items | build OK | smoke PASS (KVM 2.66s)
> **Accepted:** [M] `kd_init()` Phase-1 wiring blocked -- callee absent from tree -> XREF: 02-kernel-core/TODO-29 §4 (item: "`kd_init()` -- called from Phase 1 kernel init" at line 186)
> **Quality reviewed:** 2026-06-20 | Codex 3x (adversarial, consistency, perf) | 2M fixed | scope: kernel-code-quality

---

## 4. Phase 2 -- System Services
Storage, VFS, filesystem mount, registry, network, and AP bringup. BOOT_FATAL only if VFS or registry are completely broken; everything else degrades.

**File:** `src/kernel/main/boot_storage.c` (restructured as `boot_phase2`)

- [x] `pci_scan()` -- enumerate PCI/PCIe bus; BOOT_DEGRADED if no devices found; moved from Phase 1
- [x] Object Manager gate in Phase 2 uses `ob_init()` (11 built-in types, 6 root directories) with recovery-screen + `boot_halt()` fatal path
- [x] `xhci_init()` -- USB host controller; BOOT_DEGRADED; moved from Phase 1
- [x] `ata_init()` + `virtio_blk_init()` + `ahci_init()` -- storage drivers; BOOT_DEGRADED if all fail; moved from Phase 0
- [x] `blkdev_register_all()` -- register block devices into the blkdev layer
- [x] `vfs_init()` runs in Phase 2 after HEAP prerequisite checks and readiness gating
- [x] `partition_scan_all()` + `partition_mount_filesystems()` wired in Phase 2 before registry and symbol-table init
- [x] `ahci_setup_interrupts()` is called in Phase 2 after storage driver init
- [x] `klog_disk_enable()` is wired in Phase 2 after VFS mount path (degraded behavior is handled inside logging path)
- [x] `registry_init()` returns `boot_result_t` -- BOOT_FATAL on root-key alloc failure wired to the OB/EX recovery branch in `boot_storage.c` (readiness published post-population for panic-path safety). Shipped in TODO-14 §8.
- [x] `symtab_init()` -- load symbol table from disk; BOOT_DEGRADED; BOOT_REQUIRE(SUBSYS_VFS)
- [x] `mmap_init()` -- user-mode memory map subsystem; BOOT_REQUIRE(SUBSYS_VMM)
- [x] Time service bootstrap in Phase 2: `mono_clock_init()` + `wall_clock_init()` + `timezone_init()` + `kusd_init()` (→ XREF: [TODO-08 §3](./TODO-08-time-filetime-management.md))
- [x] `rtl8139_init()` + `net_init()` -- NIC + network stack; BOOT_DEGRADED; moved from Phase 1
- [x] `virtio_input_init()` + `vbox_mouse_init()` -- BOOT_DEGRADED; moved from Phase 1
- [x] `dhcp_discover()` -- fire-and-forget; BOOT_DEGRADED; BOOT_REQUIRE(SUBSYS_VFS) for lease file
- [x] `hw_dump()` -- log full hardware summary after all drivers init
- [x] `smp_init()` AP bringup is executed in Phase 2 (moved from Phase 1) and marks `SUBSYS_SMP` ready
- [x] `acpi_power_init()` in Phase 2: parses S1/S3/S4 sleep objects from DSDT (implemented in [TODO-26 §1](./TODO-26-power-management.md))
- [x] Remove `HV_BAR` macro from `boot_storage.c` if present -- already removed in prior commit
- [x] Add `boot_progress(2, "step-name", postcode)` at each step
- [ ] Enforce typed fatal/degraded decisions from Phase 2 init return values (current gap: several Phase 2 init APIs are still `void` and have no boot_result_t propagation):
  - `vfs_init()` return-path ownership → [05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md §2](../05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md)
  - `registry_init()` return-path: `void`→`boot_result_t` SHIPPED (TODO-14 §8, root-key alloc fatal); remaining hive-mount-failure semantics defer with `registry_load_hives()` boot wiring → [TODO-14-registry-completion.md §8](./TODO-14-registry-completion.md)
  - `partition_mount_filesystems()` root-mount success/failure must be validated explicitly before continuing to registry

**Test checkpoint:** Boot reaches Phase 2 -- serial shows `[PHASE2]` progress lines `PCI_NET`/`STORAGE_DRV`/`VFS`/`PARTITION`/`BLACKBOX`/`KLOG_DISK`/`OB`/`REGISTRY`/`SYS_SVC`, `C:`/`X:` mount, `[PHASE2] complete`; desktop follows. Verify on QEMU WHPX, TCG, VirtualBox, bare metal.

> **Test runner:** boot suite `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot, 2836 kernel tests) + per-subsystem storage/fs/registry suites; `boot_phase2` integration validated via smoke test (boot reaches `C:\>`).
>
> **Notes:**
> - Phase 2 system services (`boot_storage.c` `boot_phase2`): PCI, OB gate, storage drivers, VFS, partition mount, BlackBox `X:\`, registry, symtab/mmap/time, SMP, network, ACPI power.
> - Review fix (consistency+perf M): added 4 `boot_progress(2,...)` WDAT-watchdog pets across the unpetted slow VFS->OB + post-registry windows; closes starvation gaps + makes the per-step claim true.
> - Deferred (in-scope): typed fatal/degraded from `void` Phase 2 inits + root-mount gate; A/B mark-good-refused-on-mismatch already forces next-boot rollback, §9 owns recovery-fire.
> - Accepted (owned elsewhere): VirtIO registry-before-init (TODO-13 §4), BlackBox path-truncation + unbounded cleanup (TODO-24 §10), FAT32 `cli` `vol->lock` across disk I/O (TODO-04 §17).
> - re-adversarial skipped: fix is 4 additive `boot_progress` pets, no locking/ISR/lifecycle surface.
> - Scope: §4 owns Phase 2 init order; `void`->`boot_result_t` signatures owned by 05-storage/TODO-06 §2 + TODO-14 §8.
>
> **Verified:** 2026-06-20 | commit `e51737a0` | 20/22 items | build OK | smoke PASS (KVM 2.69s)
> **Accepted:** [M] VirtIO-blk registry exposure/tuning runs before `registry_init` (silent HKLM loss) -> XREF: 04-drivers-hardware/TODO-13 §4 (item: "Gate VirtIO-blk registry exposure + tuning reads" at line 68)
> **Accepted:** [M] BlackBox cleanup `path[64]` truncation can unlink wrong file -> XREF: 01-boot-platform/TODO-24 §10 (item: "Harden cleanup path builders" at line 318)
> **Accepted:** [H] BlackBox low-space cleanup unbounded -> WDAT watchdog starvation -> XREF: 01-boot-platform/TODO-24 §10 (item: "Budget + batch the unbounded Logs\ delete loop" at line 319)
> **Accepted:** [H] FAT32 `vol->lock` (`cli`) held across disk I/O in unlink/rename/write -> XREF: 05-storage-filesystems/TODO-04 §17 (item: "Restructure FAT32 unlink/rename/write/fsck" at line 432)
> **Deferred:** [H] Phase 2 marks SUBSYS_VFS/REGISTRY ready after unchecked `void` inits; rootless boot reaches desktop (A/B rollback mitigates) -> XREF: 02-kernel-core/TODO-01 §4 (item: "Enforce typed fatal/degraded decisions from Phase 2 init return values" at line 235)
> **Quality reviewed:** 2026-06-20 | Codex 3x (adversarial, consistency, perf) | 2M fixed, 2H+2M accepted-XREF, 1H deferred | scope: kernel-code-quality

---

## 5. Phase 3 -- User Platform
Scheduler, IPC, exec loader, and desktop. The kernel is fully operational before this phase; failures here fall back to a text console, not BSOD.

**File:** `src/kernel/main/boot_desktop.c` (restructured as `boot_phase3`)

- [x] `task_init()` starts preemptive scheduler; BOOT_FATAL prerequisite path checks `SUBSYS_HEAP` + `SUBSYS_TIMER`
- [x] `boot_phase3` captures `pipe_init`/`alpc_init` typed results: FATAL from either -> recovery+halt (both all-or-nothing; ALPC partial state non-recoverable, NtAlpc published unconditionally); else `apply_result(SUBSYS_IPC)`; POST16_IPC entry
- [x] `exec_init()` returns `boot_result_t` (ELF-reg failure FATAL, EIF/PE DEGRADED); `boot_phase3` branches recovery+halt on FATAL else `apply_result(SUBSYS_EXEC)`; POST16_EXEC entry written before init
- [x] `boot_tests_run()` executes only when `debug=1` or `test=1`; release path skips tests
- [x] `boot_splash_finish()` dismisses splash once desktop is ready
- [x] `ttf_mgr_init()` + `icon_store_init()` + `cursor_init()` load desktop assets
- [x] `desktop_init()` -- window manager + compositor init; BOOT_DEGRADED; fallback to serial console if fails
- [x] `compositor_run()` -- event loop (never returns under normal operation)
- [x] Phase 3 fatal-path behavior uses recovery screen + `boot_halt()` on prerequisite guards (same pattern as Phase 2); §7 failure policy table updated to match (2026-04-10)
- [x] Add `boot_progress(3, "step-name", postcode)` at each step
- [x] Syscall/SSDT bootstrap (`ssdt_init`/`syscall_init_fast`/`syscall_init`, `boot_desktop.c:156-161`) accepted in Phase 3 (works as shipped, consistent with TODO-12 §4); reconciled TODO-12's stale "Phase 1" wording -> Phase 3

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | propagation mechanism covered by `test_subsys_apply_result_dual_channel`; §5 severity/branch logic runs inside `boot_phase3`/`exec_init` (live boot infra, not unit-testable) -- validated via smoke test (boot reaches `C:\>`)
>
> **Notes:**
> - IPC/exec typed-init propagation shipped: `boot_phase3` now captures `pipe_init`/`alpc_init`/`exec_init` `boot_result_t` instead of `(void)`-casting, closing the "fatal IPC/exec init silently swallowed" gap (Codex F3).
> - Severity: pipe OR ALPC failure = FATAL -> recovery+halt (ALPC is all-or-nothing -- its partial state is non-recoverable per `alpc_port.h` and NtAlpc is published unconditionally, so a half-init ALPC must not be advertised ready; adversarial review overrode the design pass's demote-to-degraded); ELF-reg failure = FATAL, EIF/PE = DEGRADED.
> - Named Phase-3 POST16 codes added (`POST16_IPC` 0x3012/_OK, `POST16_EXEC` 0x3014/_OK), entry code written before init so a fault inside init attributes to the right stage; mapped in `vpd.c` name table (replaces colliding raw 0x0061/0x0062).
> - Syscall/SSDT bootstrap accepted in Phase 3 (conservative -- works as shipped; TODO-12 §2/Inputs/Verification "Phase 1" wording reconciled to Phase 3). Registry/vfs/exec-signature typed propagation stays owned by §4/§8 + TODO-06 §2/TODO-14 §8.
>
> **Verified:** 2026-06-20 | commit `59469135` | 11/11 items | build OK | smoke PASS (KVM 2.58s)
> **Quality reviewed:** 2026-06-20 | Codex 6x (design, adversarial, re-adversarial, consistency, perf) | 1H+1M fixed | scope: kernel-code-quality

---

## 6. Dependency Gates
- [x] Add dependency guards in phase orchestrators for all critical chains (inline checks + `boot_halt()` + `kernel_subsystem_dump()`)
- [x] Specifically verify these critical chains compile and enforce correctly at runtime:
  - `pmm_init` requires nothing; `vmm_init` requires PMM; `heap_init` requires VMM (Phase 0)
  - `klog_init` requires HEAP; all later subsystems require KLOG for safe logging (Phase 0)
  - `lapic_init` requires ACPI; `timer_hal_init` requires IDT (Phase 1)
  - `vfs_init` requires HEAP; `registry_init` requires VFS (Phase 2)
  - `sched_init` requires HEAP + TIMER (Phase 3)
- [x] Call `kernel_subsystem_dump()` on every BOOT_FATAL before halting -- added to `boot_halt()` and `panic_screen()`
- [ ] Enforce lapic<-acpi gate: `boot_phase1` ignores `acpi_init()` return, marks `SUBSYS_ACPI` ready unconditionally (`boot_interrupts.c:247`); capture via `apply_result`, gate `lapic_init` on `kernel_subsystem_ready(SUBSYS_ACPI)`. (Codex §6 2H)
- [ ] `klog(LOG_FATAL)` must dump readiness before halting (`klog.c:1206`): `gdt_init` + `compositor_run`-return fatal paths skip `kernel_subsystem_dump()`; route through `boot_halt()` or dump first. (Codex §6 H/M)

> **Notes:**
> - Dependency-gate enforcement: inline `BOOT_REQUIRE`/readiness guards in phase orchestrators + `kernel_subsystem_dump()` on `boot_halt()`/`panic_screen()`. Most chains (pmm/vmm/heap, klog, timer/idt, vfs/registry, sched) enforced.
> - Two confirmed false-completeness gaps remain (Codex §6, 2 reviewers), deferred to a fresh-context fix: lapic<-acpi gate not enforced; `klog(LOG_FATAL)` halts without the dump.
> - Scope: §6 owns cross-phase dependency enforcement; per-subsystem typed-init propagation lives in §2/§4/§5.
>
> **Deferred:** [H] dependency-gate false-completeness (lapic<-acpi gate unenforced + LOG_FATAL halt skips dump) -> XREF: 02-kernel-core/TODO-01 §6 (item: "Enforce lapic<-acpi gate" at line 301)

---

## 7. Failure Policy
| Phase | Failure       | Action                                            |
| ----- | ------------- | ------------------------------------------------- |
| 0     | Any           | `boot_halt()` -- serial message + halt            |
| 1     | BOOT_FATAL    | `boot_halt()` (FB may not be ready yet)           |
| 1     | BOOT_DEGRADED | `klog(WARN)` + mark not ready, continue           |
| 2     | BOOT_FATAL    | Recovery screen + `boot_halt()` (FB up)           |
| 2     | BOOT_DEGRADED | `klog(WARN)` + continue                           |
| 3     | BOOT_FATAL    | Recovery screen + `boot_halt()` (same as Phase 2) |
| 3     | BOOT_DEGRADED | `klog(WARN)` + continue to desktop                |

- [x] Implement `boot_halt(const char *reason)` -- serial-only emergency stop for Phase 0 (already existed)
- [x] Ensure `panic()` works correctly when called before `fb_init()` (serial-only path) -- added SUBSYS_FB guard, falls back to serial+halt
- [x] Ensure `panic()` full BSOD path triggers only after FB is ready (end of Phase 1) -- guards on `kernel_subsystem_ready(SUBSYS_FB)`
- [x] Add `kernel_subsystem_dump()` call inside `boot_halt()` and `panic()` -- done in §6
- [x] Define and document which BOOT_FATAL events trigger auto-restart vs permanent halt based on `boot.conf` restart policy -- documented in boot_halt.c
- [x] Align failure-policy matrix with implemented code paths -- Phase 1 `boot_halt()`, Phase 2/3 recovery screen + `boot_halt()`. Updated 2026-04-10.
- [ ] Centralize fatal halts behind one panic-safe primitive (lockless serial, pre/post-FB modes); route `klog(LOG_FATAL)`, ISR-integrity, `gdt_init`, `compositor_run`-return through it with dump + POST16_BOOT_FAILED. (Codex §7 4H)
- [ ] Pre-FB halt strictly serial-only: gate `boot_halt`/`panic_screen` fb+vpd writes on `SUBSYS_FB`; panic serial dump must be lockless + not re-enter klog lock (`panic.c:1126`,`:1212`). (Codex §7 2H+2M)
- [ ] Phase-1 `cpu_pcid_enable` BOOT_FATAL must `boot_halt` (or reclassify DEGRADED in code+matrix) -- currently downgraded to continue (`boot_interrupts.c:176`). (Codex §7 H)
- [ ] `boot.conf restart_on_halt`: implement (field + parser + ABI mirror + bounded restart) or remove the §7 claim -- no field exists today (`boot_halt.c:240`). (Codex §7 M)
- [ ] Panic fatal path must not call blocking firmware `SetVariable` (`panic.c:1210` POST-NVRAM write after `cli` -> `uefi_set_variable` mutex, no deadline); use port/RAM POST shadow or bounded emergency trylock. (Codex §7 perf H)
- [ ] Panic BSOD path must not do live VFS crash-dump writes after `cli` (`write_crash_dump` `vfs_*`, no deadline -> FS/storage/heap deadlock); persist to reserved RAM + write next boot, or nonblocking best-effort. (Codex §7 perf H)

> **Notes:**
> - Failure-policy matrix + central halts (`boot_halt`/`panic`/`panic_screen` + recovery screen) shipped and matrix-aligned (2026-04-10); degraded paths klog-WARN + continue.
> - §7 review (Codex adversarial + consistency + perf) found the matrix bypassed by several fatal paths, a panic-path lock-reentry hazard, and blocking firmware/VFS I/O in the panic path; deferred to a dedicated panic-safe-fatal fix pass (6 items above).
> - Scope: §7 owns the failure-policy contract; per-phase recovery-screen wiring is §9, the readiness dump is §6.
>
> **Deferred:** [H] failure-policy false-completeness: pre-FB fb/vpd writes, panic-path serial/klog lock-reentry, blocking firmware SetVariable + VFS crash-dump in panic path, LOG_FATAL/ISR/raw-hlt bypass matrix+dump, PCID-fatal downgrade, boot.conf restart unimplemented (Codex §7 adversarial+consistency+perf) -> XREF: 02-kernel-core/TODO-01 §7 (item: "Centralize fatal halts behind one panic-safe primitive" at line 330)

---

## 8. Code Cleanup
These are bugs and structural violations that must be fixed as part of this TODO:

- [x] Remove all `HV_BAR` macro definitions and usages -- already removed in prior commits
- [x] Remove `#include "kernel/fs/vfs.h"` from `boot_interrupts.c`
- [x] Remove `#include "kernel/fs/partition.h"` from `boot_interrupts.c`
- [x] ACPI init split: `acpi_init()` Phase 1 (MADT/FADT), `acpi_power_init()` Phase 2 (S-states) -- implemented in [TODO-26 §1](./TODO-26-power-management.md)
- [x] Move `pci_scan()`, `xhci_init()`, NIC, and DHCP out of `boot_interrupts.c` into `boot_storage.c`
- [x] Move `smp_init()` AP bringup out of `boot_interrupts.c` into `boot_storage.c`
- [x] Move `ata_init()`, `virtio_blk_init()`, `ahci_init()` out of `boot_hw.c` into `boot_storage.c`
- [x] Move SMBIOS, ESRT, UEFI conformance, capsule, crypto agility, GOP log out of `boot_hw.c` into `boot_interrupts.c`
- [x] Simplify `main.c` to exactly: `boot_phase0()` → `boot_phase1()` → `boot_phase2()` → `boot_phase3()` → halt
- [x] Gate `boot_tests_run()` behind `g_boot_info.config.debug == 1 || g_boot_info.config.test == 1`
- [ ] Update init functions to return `boot_result_t` -- distributed to per-subsystem TODOs:
  - [x] `task_init()` -- done (returns `boot_result_t`)
  - [x] `pipe_init()` → [TODO-24 §1](./TODO-24-alpc-message-ports.md)
  - [ ] `pmm/vmm/heap_init()` → [03-memory/TODO-01 §1](../03-memory-concurrency/TODO-01-vmm-memory-protection.md)
  - [ ] `vfs_init()` → [05-storage/TODO-06 §2](../05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md)
  - [x] `registry_init()` → [TODO-14 §8](./TODO-14-registry-completion.md) -- returns `boot_result_t`, BOOT_FATAL on root-key alloc failure wired to boot recovery branch

**Test checkpoint:** Grep confirms zero `HV_BAR` in `src/kernel/`, no `fs/vfs.h`/`fs/partition.h` in `boot_interrupts.c`, `kernel_main` = phase0->1->2->3; boot reaches `C:\>` (smoke). Verify on QEMU WHPX, TCG, VirtualBox, bare metal.

> **Test runner:** boot suite `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot, 2836 kernel tests); structural cleanup is grep/Read-verified + `boot_phase2` network path validated via smoke test.
>
> **Notes:**
> - §8 structural boot-path cleanup: `HV_BAR` removal, stale-include removal, ACPI Phase 1/2 split, subsystem relocation (boot_interrupts/boot_hw <-> boot_storage), `main.c` phase0->3 shape, `boot_tests_run` debug/test-gated.
> - Review fix (adversarial H): NIC-less `deferred=0` boots transmitted through an uninitialized NIC; gated `net_init`/`dhcp_discover` on `nic >= 0` + added an `rtl8139` `s_nic_ready` send guard.
> - Review fix (consistency 2M): NIC-less branch now emits a truthful terminal POST + suppresses the DHCP milestone when DHCP did not run; `s_nic_ready` cleared at `rtl8139_init` entry so a failed re-init cannot leave a stale ready flag.
> - re-adversarial skipped: fix is a NIC-presence gate + a readiness flag, no locking/ISR/lifecycle/state-machine surface, 3 functions.
> - Scope: §8 owns the structural cleanup; the `void`->`boot_result_t` typed-init signatures are distributed to per-subsystem owner TODOs.
>
> **Verified:** 2026-06-20 | commit `dc3054a7` | 10/11 items | build OK | smoke PASS (KVM 2.73s)
> **Deferred:** [M] init functions still `void` (typed `boot_result_t` propagation distributed to 03-memory/TODO-01 §1 + 05-storage/TODO-06 §2 + TODO-14 §8) -> XREF: 02-kernel-core/TODO-01 §8 (item: "Update init functions to return `boot_result_t`" at line 359)
> **Quality reviewed:** 2026-06-20 | Codex 3x (adversarial, consistency, perf) | 1H+2M fixed | scope: kernel-code-quality

---

## 9. Degraded-Boot Recovery Screen
In-kernel graphical recovery UI shown when a Phase 2 subsystem fails non-fatally. Renders directly to the GOP framebuffer -- no compositor, no window manager required. Displays which subsystem failed, its POST code, and a simple recovery menu (retry / boot to serial console / power off).

**Files:** `src/kernel/main/boot_recovery.c`, `include/kernel/boot_recovery.h`

- [x] Design `boot_recovery_info_t` struct: failed subsystem, POST code, `boot_result_t`, phase number
- [x] Implement `boot_recovery_show(boot_recovery_info_t *info)` -- draws panel on framebuffer using inline 8x8 font; no heap alloc
- [x] Implement three-option menu **[R] Retry** / **[C] Halt to serial log** / **[P] Power off**; bounded PS/2 poll + drain so a headless boot cannot hang; render to the FB back buffer so `fb_swap()` presents it
- [x] Wire recovery actions via one `boot_recovery_act()` dispatcher: **[R]** -> `acpi_reset_now()`, **[P]** -> `acpi_poweroff_now()` (IF-off-safe, quiesce-free -- the screen runs under `cli`), **[C]** -> serial-banner halt
- [ ] Implement the interactive **[C]** degraded-boot serial console (read-eval over serial); today `RECOVERY_CONSOLE` halts with a banner
- [x] Hook into Phase 2 missing-prerequisite path: VFS and Registry prereq guards call `boot_recovery_show()` before `boot_halt()` (`boot_storage.c:305`, `:731`)
- [ ] Observe actual Phase 2 init FAILURE, not just missing prereq: today `SUBSYS_VFS`/`SUBSYS_REGISTRY` are set ready unconditionally (void/ignored inits) so recovery never fires on a real mount/registry failure. Blocked on §4 typed propagation
- [x] Hook into Phase 3 failure path: Scheduler guard calls `boot_recovery_show()` before `boot_halt()`
- [x] Ensure `boot_recovery_show()` is a no-op (falls through to `boot_halt()`) if `SUBSYS_FB` is not ready
- [x] Add `boot_progress(9, "recovery-screen", 0xE0)` call on entry

**Test checkpoint:** No automated test surface -- the recovery screen only renders on a Phase 2/3 BOOT_FATAL, which the smoke test never triggers. Validation is by fault injection + serial/bare-metal: force a Phase 2 prereq fail, confirm the panel is visible (back buffer), `[R]`/`[P]` reset/poweroff, headless boot halts (no hang) with `[RECOVERY] no keyboard input`. Verify on QEMU WHPX/TCG + bare metal.

> **Test runner:** No kernel test surface (fatal-path graphical UI) -- validation via serial log + fault injection on WHPX/bare metal; build + smoke confirm the normal `acpi_reboot`/`acpi_shutdown` wrappers and boot path are unregressed.
>
> **Notes:**
> - In-kernel graphical recovery UI (`boot_recovery.c`): GOP-framebuffer panel, inline font, three-option menu, PS/2 polling, Phase 2/3 fatal hooks, `SUBSYS_FB` no-op fall-through.
> - Review fix (adversarial 3H): renders to the FB back buffer (was hidden by `fb_swap`); bounded PS/2 poll + drain (was an infinite headless hang); `[R]`/`[P]` wired via one `boot_recovery_act()` dispatcher.
> - Review fix (re-adversarial H): `[R]`/`[P]` use new IF-off-safe `acpi_reset_now()`/`acpi_poweroff_now()` -- the old `acpi_reboot`/`acpi_shutdown` quiesce `hlt`-hangs under `cli`.
> - Review fix (consistency M + perf M): `[C]` menu relabeled "Halt to serial log"; `kbd_poll_begin` drain bounded to 64 reads.
> - Deferred (in-TODO): interactive `[C]` serial console; observe a real Phase 2 init failure (blocked on §4 typed propagation).
> - Scope: §9 owns the recovery UI; firing-on-real-failure needs §4's `void`->`boot_result_t` propagation.
>
> **Verified:** 2026-06-20 | commit `bc60dac7` | 8/10 items | build OK | smoke PASS (KVM 2.72s)
> **Deferred:** [M] interactive `[C]` degraded-boot serial console not implemented (halts with a banner today) -> XREF: 02-kernel-core/TODO-01 §9 (item: "Implement the interactive **[C]** degraded-boot serial console" at line 392)
> **Deferred:** [H] recovery never fires on a real Phase 2 mount/registry failure (void inits set SUBSYS_VFS/REGISTRY ready unconditionally) -> XREF: 02-kernel-core/TODO-01 §9 (item: "Observe actual Phase 2 init FAILURE" at line 394)
> **Quality reviewed:** 2026-06-20 | Codex 9x (adversarial x2, consistency x2, perf x2, re-adversarial x3) | 4H+4M fixed | scope: kernel-code-quality

---

## 10. POST Code + UEFI Variable Log
Write the current POST code to a UEFI NVRAM variable (`ImpossiblePOST`) at every phase boundary via `uefi_runtime_services.SetVariable`. The value survives a reboot, allowing post-mortem boot failure diagnosis on real hardware even when serial is unavailable.

**Files:** `src/kernel/boot_timing.c`, `include/kernel/boot_timing.h`, `src/kernel/uefi_runtime.c`

- [x] Define UEFI variable name: `ImpossiblePOST` in namespace GUID `{494D504F-5354-4F53-504F-535447554944}`
- [x] Implement 16-bit POST persistence helpers: `boot_post_write16()`, `boot_post_nvram_write16()` (runtime-availability handling wired; `postcode=0` now gates both the NVRAM write and the prior-value read/banner)
- [x] `boot_progress()` path writes POST via `boot_post_write16(postcode)` for phase progress tracking
- [x] On successful boot completion, write final 16-bit code `0xFF00` (`POST16_BOOT_OK`) in `boot_phase3()`
- [x] On `boot_halt()` / `panic()`, write final 16-bit failure code `0xFFFE` (`POST16_BOOT_FAILED`) before halting
- [x] Implement `boot_post_read16()` -- reads last stored value; returns -1 if unavailable
- [x] Log prior POST code to serial at Phase 0 start: `[POST] Last boot code: 0xNN` with status interpretation
- [x] Add `boot_progress(10, "post-code-log", 0x11)` call after `uefi_runtime_init()` in Phase 0
- [x] Add explicit `boot_post_nvram_write16()` gating for `boot.conf postcode=0` -- write skips when `config_found && !postcode`; `boot_hw.c` gates the prior-value read/banner with the same predicate

**Test checkpoint:** Boot with `postcode=1` (default) -> serial shows `[POST] Last boot ...` + `ImpossiblePOST` written at each phase boundary; next boot reads `POST16_BOOT_OK`. Boot with `postcode=0` -> no NVRAM write AND no prior-value banner. Verify on QEMU WHPX, TCG, VirtualBox, bare metal (real UEFI NVRAM).

> **Test runner:** No dedicated unit test (the path is `uefi_set_variable`/`uefi_get_variable`-bound, validated against real firmware) -- boot-path validated via smoke test (boot reaches `C:\>`); NVRAM round-trip verified on WHPX/bare metal serial.
>
> **Notes:**
> - POST persistence (`boot_init.c` `boot_post_write16`/`boot_post_nvram_write16`/`boot_post_read16`): writes `ImpossiblePOST` to UEFI NVRAM at phase boundaries for next-boot post-mortem (final `0xFF00` success / `0xFFFE` fail).
> - Implemented the §10 open item: `boot.conf postcode=0` now gates BOTH the NVRAM write and the prior-value read/banner, so a persistence-off boot neither writes nor reports a stale code.
> - Review fix (consistency M): `boot_post_read16` rejects values with non-canonical attributes (untrusted/stale); a failed `SetVariable` now warns on serial (skipping the panic mark).
> - Review fix (perf M): removed the redundant per-call LAPIC mask -- `uefi_set_variable` quiesces the timer after the RT mutex, so the outer mask only spanned the mutex wait.
> - Accepted: the panic-path `boot_post_nvram_write16` blocking-RT-mutex hazard is owned by the panic-safe-fatal rewrite (Failure Policy section).
> - Scope: §10 owns POST persistence; panic-path NVRAM safety is the Failure Policy section's panic-safe primitive.
>
> **Verified:** 2026-06-20 | commit `b54714ab` | 10/10 items | build OK | smoke PASS (KVM 2.92s)
> **Accepted:** [H] panic-path `boot_post_nvram_write16` takes the blocking RT mutex after `cli` (can hang the panic owner) -> XREF: 02-kernel-core/TODO-01 §7 (item: "Panic fatal path must not call blocking firmware `SetVariable`" at line 334)
> **Quality reviewed:** 2026-06-20 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 5M fixed, 1H accepted-XREF | scope: kernel-code-quality

---

## 11. Deferred Init for Non-Critical Subsystems
Move non-critical subsystem init out of the blocking boot path so the desktop appears faster. Win11 defers non-critical services until after Explorer starts; Linux has `deferred_initcall()` for post-boot init.

**Files:** `src/kernel/main/boot_storage.c`, `src/kernel/main/boot_desktop.c`, `include/kernel/boot_init.h`

- [x] Define `BOOT_DEFERRED` return code in `boot_result_t` -- subsystem skipped, will init later
- [x] Add `g_deferred_inits[]` array in `boot_init.c` -- stores function pointers + names for deferred subsystems (max 16 slots)
- [x] Add `boot_defer(const char *name, boot_result_t (*fn)(void))` -- registers a subsystem for post-desktop init
- [x] Move `rtl8139_init()` + `net_init()` + `dhcp_discover()` from Phase 2 blocking path to deferred -- network is not needed for desktop; wrap in `static boot_result_t deferred_net_init(void)` since originals return `int`/`void`
- [x] Move `virtio_input_init()` + `vbox_mouse_init()` from Phase 2 blocking path to deferred -- PS/2 mouse is sufficient for initial desktop; wrap in `static boot_result_t deferred_input_init(void)` since originals return `int`
- [x] Add `boot_run_deferred()` call in Phase 3 after `desktop_init()` but before `compositor_run()` -- runs inline (NOT on a thread -- compositor starves threads, breaking mouse drivers; see Codex F3 revert 2026-04-05)
- [x] Each deferred init logs `[DEFERRED] name +NNNms` via klog with POST code range `POST16(0xD000)`--`POST16(0xD005)` (deferred net + input)
- [x] If a deferred init fails, log `klog(WARN)` -- no halt, no BSOD
- [x] Add `deferred=0` boot.conf option to disable deferral (all subsystems init in-phase, old behavior) for debugging
- [x] Add 3 unit tests: BOOT_DEFERRED value, boot_defer registration, deferred POST codes
- [ ] Run `boot_run_deferred()` from a compositor one-shot after the first frame (not inline before `compositor_run`) so `mouse_init()`'s 300ms-2s PS/2 BAT no longer delays the first desktop frame (`boot_desktop.c:351`). (Codex §11 perf H)
- [x] Commit: `"kernel: deferred init -- non-critical subsystems after desktop"`

**Test checkpoint:** Boot with default config → desktop appears → deferred inits run → `[DEFERRED]` lines appear in serial log after `[PHASE3] desktop_init`. Boot with `deferred=0` → no `[DEFERRED]` lines, all subsystems init in-phase. Verify on QEMU WHPX, TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 3 deferred-init tests (BOOT_DEFERRED value, boot_defer registration, deferred POST codes) in `test_boot_init.c`; boot-path validated via smoke test.
>
> **Notes:**
> - Deferred-init machinery (`boot_init.c`: `g_deferred[]`/`boot_defer`/`boot_run_deferred`) moves non-critical net + input init out of blocking Phase 2 to Phase 3 post-desktop, inline (NOT threaded -- the compositor starves threads, F3 revert).
> - Review fix (adversarial M): `boot_run_deferred` is now idempotent (run-once `g_deferred_ran` latch; `boot_defer` rejects post-run registration) so a retry/diagnostic call cannot double-init drivers (IRQ re-registration, device reset).
> - Review fix (adversarial M): `deferred_input_init` records a `BOOT_LOAD_CLASS_INPUT` status (LOADED/SKIPPED) instead of unconditional `BOOT_OK`, so input outcomes surface in the boot-load summary.
> - Deferred (perf H): the inline placement still runs before the first compositor frame, so `mouse_init`'s PS/2 BAT delays it -- moving to a post-first-frame compositor one-shot is filed.
> - Scope: §11 owns the deferred-init mechanism; the placement-vs-first-frame latency is the filed enhancement.
>
> **Verified:** 2026-06-20 | commit `a29bf210` | 10/11 items | build OK | smoke PASS (KVM 2.67s)
> **Deferred:** [H] deferred init runs inline before the first compositor frame, so `mouse_init`'s 300ms-2s PS/2 BAT delays it -> XREF: 02-kernel-core/TODO-01 §11 (item: "Run `boot_run_deferred()` from a compositor one-shot after the first frame" at line 466)
> **Quality reviewed:** 2026-06-20 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 2M fixed, 1H deferred | scope: kernel-code-quality

---

## 12. Boot Performance Regression Detection
Compare `boot_timing` data across reboots to detect init regressions. Win11 uses ETW boot traces with XPerf analysis; Linux uses `systemd-analyze blame` + `bootchart`. Impossible OS stores structured timing per-boot and compares automatically.

> [!TIP]
> Neither Win11 nor Linux alerts automatically when a subsystem gets slower between boots. Impossible OS detects regressions at boot time and logs a warning -- no external tooling needed.

**Files:** `src/kernel/boot_timing.c`, `include/kernel/boot_timing.h`

- [x] Extend `boot_timing.c` with `boot_perf_record_t` struct (name[16], elapsed_ms, phase) -- fixed-size for NVRAM layout
- [x] After Phase 3 completes, `boot_perf_save()` writes header + records to UEFI NVRAM variable `ImpossibleBootPerf` (NV+BS+RT)
- [x] On next boot, `boot_perf_read_prev()` reads `ImpossibleBootPerf` from NVRAM into `s_prev_records[]` (called in Phase 0 after uefi_runtime_init)
- [x] `boot_perf_compare()` compares current vs previous: if >200% OR >500ms regression, logs `[PERF] WARNING: <step> init regressed: <prev>ms -> <cur>ms`
- [x] `boot_perf_dump()` prints all step durations as a sorted-by-time table to serial
- [/] Add `bootperf` shell command -- blocked on a user-mode boot-perf query path (`NtQuerySystemInformationEx` 0x0313 in `TODO-A-SSDT-Master-Table.md`, still `[ ]`); the data is already on serial + `boot_perf_dump()` today.
- [ ] Wear-budget the unconditional `ImpossibleBootPerf` write (`boot_timing.c:630`): add a `boot.conf bootperf=0` opt-out (mirroring `postcode=0`) and/or skip-unchanged so reboot/test cycles do not burn the ~100K-cycle variable store. (Codex §12 M)
- [x] Add debug POST codes: `POST16(0xDC00)` entry, `POST16(0xDC01)` NVRAM read, `POST16(0xDC02)` comparison done, `POST16(0xDC03)` NVRAM write
- [x] Add 3 unit tests: perf record size (24 bytes), BOOT_PERF_MAGIC value, bootperf POST code uniqueness
- [x] Commit: `"kernel: boot performance regression detection via UEFI NVRAM"`

**Test checkpoint:** Boot twice → second boot serial log shows `[PERF]` comparison lines. Inject a `sleep_ms(1000)` in a subsystem init → second boot shows `[PERF] WARNING` for that subsystem. Verify on QEMU WHPX, TCG, VirtualBox, bare metal. On bare metal, verify NVRAM read/write works with real UEFI firmware.

> [!NOTE]
> UEFI NVRAM has limited write endurance (~100K cycles). Current successful-boot path writes `ImpossiblePOST` 5 times (`POST16_SERIAL`, `POST16_SIMD_OK`, `POST16_TIMER_OK`, `POST16_REGISTRY_OK`, `POST16_BOOT_OK`) plus `ImpossibleBootPerf` once, for 6 writes/boot. This should be reduced or explicitly budgeted. (`postcode=0` now disables the 5 POST writes; the `ImpossibleBootPerf` wear budget is filed above.)

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 3 boot-perf tests (record size 24 bytes, `BOOT_PERF_MAGIC`, bootperf POST-code uniqueness) in `test_boot_init.c`; NVRAM round-trip validated against real firmware on WHPX/bare metal.
>
> **Notes:**
> - Boot-perf regression detection (`boot_timing.c`): per-step timings to NVRAM `ImpossibleBootPerf` after Phase 3, compared next boot (>200% OR >500ms -> `[PERF] WARNING`). Gated on `boot_perf_enabled`.
> - Review fix (adversarial 2M): `boot_perf_read_prev` NUL-terminates imported names (`str_eq` overread risk on untrusted NVRAM); `boot_perf_compare` math promoted to 64-bit (overflow on untrusted values).
> - Review fix (consistency M): the `elapsed_ms` ceiling is now a shared `BOOT_PERF_MS_SANITY_CAP` applied on save/read/compare so NVRAM records round-trip.
> - Deferred: `ImpossibleBootPerf` wear budget (M); the `bootperf` user-mode shell command (L, blocked on `NtQuerySystemInformationEx`).
> - Scope: §12 owns the kernel-side boot-perf NVRAM machinery; the user-mode query API + wear budget are filed follow-ups.
>
> **Verified:** 2026-06-20 | commit `76b076f6` | 8/10 items | build OK | smoke PASS (KVM 2.67s)
> **Deferred:** [M] `ImpossibleBootPerf` is an unconditional 1 write/normal-boot with no wear budget -> XREF: 02-kernel-core/TODO-01 §12 (item: "Wear-budget the unconditional `ImpossibleBootPerf` write" at line 500)
> **Deferred:** [L] `bootperf` shell command blocked on a user-mode boot-perf query API -> XREF: 02-kernel-core/TODO-01 §12 (item: "Add `bootperf` shell command" at line 499)
> **Quality reviewed:** 2026-06-20 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 3M fixed, 1M+1L deferred | scope: kernel-code-quality

---

## 13. Async Subsystem Init (SMP Parallel)
Allow independent subsystems within a phase to initialize concurrently on different CPUs. Win11 uses parallel DLL loading (1--16 worker threads). Linux has `async_schedule()` for device probing. Impossible OS can parallelize Phase 2, where many subsystems (storage, NIC, USB) have no mutual dependencies.

> [!TIP]
> Win11 parallelizes DLL loading but not kernel init. Linux parallelizes device probing but not initcalls. Impossible OS can parallelize subsystem init at the kernel level with correct dependency tracking -- designed for SMP from day one.

**Files:** `src/kernel/main/boot_init.c`, `include/kernel/boot_init.h`

- [x] SMP moved before storage drivers in Phase 2 so APs are online for async dispatch
- [x] `boot_async_init()` registers IPI handler (vector 0xFC) for AP work dispatch
- [x] `boot_async_group(name, steps, count)` dispatches steps across APs via IPI; BSP runs step 0, APs run step 1+; includes 10s timeout barrier
- [x] Storage drivers (ATA, AHCI, NVMe, VirtIO-blk) wired as async group when `async_init=1`; Groups B/C (input, network) already deferred to post-desktop via §11
- [x] `async_init=0` in boot.conf (default) -- sequential behavior unchanged; `async_init=1` enables parallel
- [x] Per-AP fault isolation in `panic_screen()` -- if AP faults during async work, records BOOT_FATAL and parks AP without crashing BSP
- [x] Logs `[ASYNC] <step> started on CPU<n>` and `[ASYNC] <step> completed on CPU<n> in <N>ms`
- [x] POST codes: `POST16(0xDD00)` dispatch, `POST16(0xDD01)` AP entry, `POST16(0xDD02)` barrier, `POST16(0xDD03)` done
- [x] 2 unit tests: async POST code uniqueness, IPI vector value (0xFC) and no collision with 0xFD/0xFE
- [ ] Quiesce the AP on async timeout before sequential fallback (`boot_init.c:365`): timeout marks AP done+FATAL without stopping it, so `boot_phase2` re-runs storage init concurrently (driver corruption). (Codex §1 H; `async_init=1` only)
- [x] Explicit `boot_result_t` severity rank in `boot_async_group` worst-pick: added `boot_result_severity()` (FATAL>DEGRADED>DEFERRED>OK) used at all 3 worst-pick sites, so a `BOOT_DEFERRED` step no longer masks a `BOOT_FATAL` one (`boot_init.c`)
- [ ] Run async AP storage init in an IF-enabled worker, not from `async_ipi_handler` (`boot_init.c:243`): the IPI gate clears IF so NVMe `sleep_ms`/`hlt` hangs the AP into the 10s timeout. Keep `async_init=1` experimental. (Codex §13 H)
- [ ] Define async-group `BOOT_DEFERRED` aggregation: it ranks below DEGRADED and `boot_phase2` has no DEFERRED branch, so a not-ready async step records LOADED. Latent (no step returns DEFERRED yet); add aggregation tests. (Codex §13 M)
- [x] Commit: `"kernel: async subsystem init -- SMP parallel Phase 2"`

**Test checkpoint:** Boot with `async_init=1` → storage/input/network init on different CPUs → serial log shows `[ASYNC]` entries with different CPU numbers. Total Phase 2 time decreases vs sequential. Boot with `async_init=0` → sequential behavior unchanged. Verify on QEMU WHPX (2+ vCPUs), TCG, VirtualBox, bare metal. Bare metal is critical -- per-AP fault isolation must work on real hardware.

> **Verified:** 2026-06-20 | commit `308eb661` | 10/13 items | build OK | tests 2836+16 PASS
> **Deferred:** [H] async timeout marks the AP done+FATAL without quiescing it, so `boot_phase2` reruns storage concurrently (driver corruption) -> XREF: 02-kernel-core/TODO-01 §13 (item: "Quiesce the AP on async timeout before sequential fallback" at line 543)
> **Deferred:** [H] async storage init runs inside `async_ipi_handler` (interrupt gate, IF cleared), so a sleepable driver (NVMe `sleep_ms`/`hlt`) hangs the AP into the 10s timeout -> XREF: 02-kernel-core/TODO-01 §13 (item: "Run async AP storage init in an IF-enabled worker" at line 545)
> **Deferred:** [M] async-group `BOOT_DEFERRED` aggregation ranks below DEGRADED with no `boot_phase2` DEFERRED branch, so a not-ready async step records LOADED (latent; no async step returns DEFERRED yet) -> XREF: 02-kernel-core/TODO-01 §13 (item: "Define async-group `BOOT_DEFERRED` aggregation" at line 546)
> **Quality reviewed:** 2026-06-20 | Codex 6x (adversarial x2, consistency, perf, re-adversarial x2) | 1H+2M fixed, 2H+1M deferred | scope: kernel-code-quality

The default `async_init=0` path (sequential, shipped + tested) is unaffected; all three deferred items gate only the experimental `async_init=1` parallel path. The timeout-publication race (a late AP completion overwriting a fired timeout's FATAL) was fixed this pass with a BSP-local sticky `timed_out` flag.

---

## OS Comparison

| ⭐  | Feature              | 🪟 Win11            | 🐧 Linux              | 🚀 Impossible OS         |
| --- | -------------------- | ------------------- | --------------------- | ------------------------ |
| 💎  | Formal phase model   | ✅ Phase 0/1        | ✅ initcall levels    | ✅ §2--§5 4 phases       |
| 💎  | Interrupts-off phase | ✅ Phase 0          | ✅ early start_kernel | ✅ §2 boot_phase0        |
| 💎  | Dependency ordering  | ✅ Boot load groups | ✅ initcall deps      | ✅ §6 gates done         |
| 💎  | Typed init results   | ✅ NTSTATUS         | ✅ initcall_t         | ⚠️ §1 boot_result_t      |
| 💎  | Halt on critical     | ✅ Bugcheck         | ✅ panic()            | ✅ §7 boot_halt          |
| 💎  | Degraded boot        | ✅ Safe mode        | ✅ Emergency shell    | ✅ §7 BOOT_DEGRADED      |
| 💎  | Boot serial log      | ✅ DebugPrint/ETW   | ✅ early_printk       | ✅ §2 [PHASE0] markers   |
| 💎  | Boot config gating   | ✅ Registry         | ✅ cmdline            | ✅ §2 boot.conf          |
| 💎  | Tests separated      | ✅ Separate env     | ✅ initcall_debug     | ✅ §5 debug/test gate    |
| ⭐  | Recovery UI at boot  | ❌ Separate WinRE   | ❌ Text-only shell    | ✅ §9 graphical recovery |
| ⭐  | POST to UEFI NVRAM   | ❌ Firmware-only    | ❌ Not implemented    | ✅ §10 ImpossiblePOST    |
| ⭐  | Readiness oracle API | ⚠️ Private internal | ⚠️ system_state only  | ✅ §1 public API         |
| 💎  | Deferred init        | ✅ Delayed services | ✅ deferred_initcall  | ✅ §11 boot_defer()      |
| ⭐  | Boot perf regression | ❌ Manual ETW       | ❌ Manual bootchart   | ✅ §12 auto NVRAM diff   |
| ⭐  | Parallel kernel init | ⚠️ DLL load only    | ⚠️ async_schedule     | ✅ §13 IPI async group   |

> After parity items, Impossible OS matches Windows NT and Linux on phased init.
> Exclusive: graphical recovery UI, UEFI NVRAM POST codes, public readiness oracle, auto boot perf regression, SMP parallel init.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_boot_init()` (see `src/kernel/test/test_runner.c`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [x] Create `src/kernel/test/test_boot_init.c` with:
  - `boot_result_t` values: assert `BOOT_OK == 0`, `BOOT_DEGRADED == 1`, `BOOT_FATAL == 2`
  - `kernel_subsystem_set_ready(SUBSYS_PMM, true)` then `kernel_subsystem_ready(SUBSYS_PMM)` returns true
  - `kernel_subsystem_set_ready(SUBSYS_PMM, false)` then `kernel_subsystem_ready(SUBSYS_PMM)` returns false
  - All `SUBSYS_COUNT` entries default to not-ready before any `set_ready` call
  - `BOOT_REQUIRE(SUBSYS_PMM)` returns `BOOT_FATAL` when PMM is not ready
  - `BOOT_REQUIRE(SUBSYS_PMM)` does not return `BOOT_FATAL` when PMM is ready
  - `boot_progress()` does not crash with NULL step name
  - POST code constants are non-zero and unique across phases
- [x] Register in `test_runner_init()`: `test_register_boot_init()`
- [x] Commit: `"test: add boot-init sequencing test suite"` (included in earlier commits)
- [x] Add deferred init tests (§11):
  - `BOOT_DEFERRED == 3` value assertion
  - `boot_defer("test_deferred", fn)` returns 0 on success
  - Deferred POST code constants non-zero and unique
- [x] Add boot perf regression tests (§12):
  - `boot_perf_record_t` size is 24 bytes (NVRAM layout stability)
  - `BOOT_PERF_MAGIC == 0x50455246` ("PERF")
  - Bootperf POST codes non-zero, unique, no overlap with deferred range

> **Done:** 23 suites registered in `test_runner_init()` (2026-04-03; +4 §1 gap-fill suites added 2026-04-07 by verify-mode -- enum layout, BOOT_STEP mapping, dump smoke, POSTCODE constants; +2 §2 propagation suites added 2026-04-08 by implement-mode -- Phase 0 propagation slots, apply_result dual channel). A 5th §1 gap-fill (boot_progress recording) was attempted but reverted -- calling boot_progress() from a unit test froze boot on QEMU WHPX after BOOT_STEP test on the user's i5-11600K box. Tests must NEVER call boot_progress() -- it has live VPD/framebuffer/serial side effects that cannot be validated in WSL.
>
> **§2 propagation tests (2026-04-08):** `test_subsys_phase0_propagation_slots` exercises the readiness oracle round-trip on the four new SUBSYS_UEFI_VARS / UEFI_TIME / SECUREBOOT / TPM slots via save/restore wrappers. `test_subsys_apply_result_dual_channel` exercises `kernel_subsystem_apply_result()` for all four `boot_result_t` cases (OK, DEGRADED, FATAL, DEFERRED) plus out-of-range -- proves the dual-channel (oracle + degraded_mask) lockstep semantics that the §2 propagation fix relies on. Both tests use the save/restore-on-existing-slot pattern (no live boot infrastructure calls), per the test side-effect ban in CLAUDE.md.

---

## Codex Adversarial Review

> Reviewed 2026-04-05 by Codex (o3). Scope: §1-§7, §9-§13.
> **Round 1:** 4 findings (1 critical, 3 high). All addressed.
> **Round 2:** 2 findings. 1 fixed (GSI mask), 1 accepted (deferred fault isolation).
> **Final verdict: resolved.** Build clean.

| #   | Severity | Finding                                                                   | Status                                                                                                                            |
| --- | -------- | ------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------- |
| 1   | critical | `s_subsys_names[]` missing SUBSYS_OB -- OOB read in panic diagnostic path | **Fixed** -- added "OB" entry + `_Static_assert` in both tables                                                                   |
| 2   | high     | `boot_recovery_show()` return value ignored -- menu is cosmetic           | **Fixed** -- all call sites branch on RECOVERY_POWEROFF -> `acpi_shutdown()`                                                      |
| 3   | high     | Deferred init not fault-isolated -- optional driver panic aborts Phase 3  | **Reverted** -- thread starved deferred inits (broke mouse); inline call restored; isolation deferred to TODO-23 SEH              |
| 4   | high     | Recovery keyboard polling races active PS/2 IRQ handler                   | **Fixed** -- `kbd_poll_begin()`: cli + mask routed GSI + drain                                                                    |
| 5   | high     | Recovery masks raw IRQ1 instead of ACPI-remapped GSI                      | **Fixed** -- uses `ioapic_isa_to_gsi(1)` for correct GSI                                                                          |
| 6   | high     | Deferred init not fault-contained                                         | **Accepted** -- thread approach reverted (starved mouse drivers); inline call restored; per-thread isolation requires TODO-23 SEH |

---

## Verification

- [x] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===` -- PASS: clean build succeeded (2026-04-02)
- [x] QEMU WHPX: serial log shows `[PHASE0]`…`[PHASE3]` markers in dependency order -- PASS: all 4 phases present, dependency order correct (SERIAL→PMM→VMM→…→SCHED→DESKTOP_READY), 2 CPUs, boot complete 33.5s (WHPX, 2026-04-02)
- [x] QEMU TCG: 135 tests passed, 0 failed, 1 skipped (VirtIO-blk), boot 4.45s, 1 CPU, perf NVRAM round-trip OK (2026-04-03)
- [x] VirtualBox: boot completes with all phases logged, 1920x1080 VMSVGA, 4 CPUs, C:\ mounted, IXFS OK, perf NVRAM saved (2026-04-03)
- [x] Serial log contains no `HV_BAR` or raw pixel-write output -- PASS: neither string found in WHPX serial log (2026-04-02)
- [x] Serial log shows `kernel_subsystem_dump()` output before any halt -- PASS: code verified -- `boot_halt()` calls `kernel_subsystem_dump()` at boot_halt.c:263 before any framebuffer writes (2026-04-01)
- [x] Forcing PMM failure causes `boot_halt()` on serial -- no framebuffer writes attempted -- PASS: code verified -- boot_hw.c:146 checks `SUBSYS_PMM` ready, calls `boot_halt()` which writes serial first, only touches fb if `fb_available` (2026-04-01)
- [/] Forcing VFS failure shows degraded-boot screen and then halts via `boot_halt()` (current behavior) -- verify whether "continue degraded boot" or "halt after recovery UI" is intended policy
- [x] `boot_tests_run()` does not appear in serial log when `debug=0` -- PASS: booted with debug=0, grep found 0 boot test references (2026-04-01)
- [x] `boot_tests_run()` does appear in serial log when `debug=1` -- PASS: `--- Boot Tests ---` at 7.700s, `=== 50 tests passed, 0 failed ===` at 8.440s (WHPX, 2026-04-02)
- [ ] Bare metal: all phases complete, POST codes visible on VPD -- (manual: requires physical hardware)
- [ ] Commit: `"kernel: init-sequencing verified -- phases, readiness oracle, dependency gates"`

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | boot suite registered via `test_register_boot_init()`, 0 failures (KVM 2026-06-20: 2836 kernel + 16 user-mode PASS, exit=0; historical TCG 2026-04-03: 135 tests, 0 failed, 1 skipped)
