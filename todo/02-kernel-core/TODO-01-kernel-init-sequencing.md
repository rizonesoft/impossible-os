# TODO-01 -- Kernel Init Sequencing

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
- → XREF: `TODO-02-system-logging.md` -- `klog_disk_enable()` is a Phase 2 gate; must follow VFS ready
- → XREF: `TODO-03-object-manager.md §1` -- Object Manager init slot is Phase 2, after heap, before registry; §1 provides the `ob_init()` implementation
- → XREF: `04-drivers-hardware/INDEX.md` -- all driver `_init()` functions must accept and return `boot_result_t`
- → XREF: `scripts/test.sh` -- headless QEMU serial log is the verification path
- → XREF: `TODO-06-irql-model-dpcs.md §4` -- DPC subsystem init belongs in Phase 1, after timer; §4 is the DPC Object Type and Per-CPU Queue init
- → XREF: `TODO-07-time-filetime-management.md §5` -- `wall_clock_init()` belongs in Phase 2, after UEFI runtime services; NTP wall clock adjustment (§17) belongs in Phase 3
- → XREF: `01-boot-platform/TODO-16-boot-watchdog.md` -- watchdog timer integrates with `boot_progress()` calls; detects hung subsystem init
- → XREF: `04-drivers-hardware/TODO-11-security-hardware.md §4` -- TPM2 `PCR_Extend` for measured boot; extends the PCR event log parsed in Phase 0
- → XREF: `01-boot-platform/TODO-04-cpu-boot-sequencing.md §2` -- CPU security activation order (EFER/CR4 hardening) slots into Phase 0 between serial init and PMM

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

| ⭐  | Order | Deliverable                                | Depends On | Status |
| --- | :---: | ------------------------------------------ | ---------- | :----: |
| 💎  |   1   | Boot init infrastructure                   | --         |  [x]   |
| 💎  |   2   | Phase 0 -- critical init                   | §1         |  [x]   |
| 💎  |   3   | Phase 1 -- platform services               | §2         |  [/]   |
| 💎  |   4   | Phase 2 -- system services                 | §3         |  [/]   |
| 💎  |   5   | Phase 3 -- user platform                   | §4         |  [/]   |
| 💎  |   6   | Dependency gates                           | §1--§5     |  [x]   |
| 💎  |   7   | Failure policy                             | §6         |  [/]   |
| 💎  |   8   | Code cleanup                               | §2--§5     |  [/]   |
| ⭐  |   9   | Degraded-boot recovery screen              | §7         |  [x]   |
| ⭐  |  10   | POST code + UEFI variable log              | §1         |  [/]   |
| 💎  |  11   | Deferred init for non-critical subsystems  | §5         |  [x]   |
| ⭐  |  12   | Boot performance regression detection      | §10        |  [/]   |
| ⭐  |  13   | Async subsystem init (SMP parallel)        | §6, §11    |  [/]   |

> 💎 = parity -- Windows NT and Linux both have formal init phase models; Impossible OS must match them.
> ⭐ = exclusive -- degraded-boot recovery UI, UEFI NVRAM POST log, boot perf regression detection, and SMP parallel init.

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
- [ ] **PAT MSR constant + AP propagation bug** -- `boot_hw.c:270` writes `0x0007040600010406`. Decoding bits [23:16] (PA2): 0x07 → 0x01. The comment says "entry 1: WT(04)→WC(01)" but the actual change is at PA2, not PA1. Meanwhile `vmm_map_mmio_wc()` uses `VMM_FLAG_WRITETHROUGH` only (PWT=1, PCD=0, PAT bit=0) which selects PAT index 1 = still 0x04 (WT). **Net effect: framebuffer is mapped WT, not WC -- silent perf regression.** Compounding bug: `ap_entry()` (smp.c:110) does NOT mirror the PAT MSR write, so APs have default PAT (PA2 = UC-) while BSP has PA2 = WC -- cross-CPU cache-type skew if any AP touches a `vmm_map_mmio_wc()` page (e.g., async storage drivers under `async_init=1`). Tracked in [01-boot-platform/TODO-04-cpu-boot-sequencing.md §8](../01-boot-platform/TODO-04-cpu-boot-sequencing.md) which now has a fix-first item for the constant decode bug AND the AP sync work.
- [ ] **UEFI handoff pointer validation** -- `boot_hw.c:62-67` byte-copies `sizeof(struct boot_info)` from `(struct boot_info *)mbi` with no header magic, version, or size validation. The post-hoc cmdline ASCII check at lines 79-91 catches struct shifts but not wild pointers. Tracked in `01-boot-platform/TODO-02-bootloader-error-recovery.md` **§15** (header + bootloader populate) and **§16** (`boot_info_validate()` + `boot_hw.c` + unit tests): [`TODO-02-bootloader-error-recovery.md`](../01-boot-platform/TODO-02-bootloader-error-recovery.md).

**Resolved (implement 2026-04-08):** Phase 0 BOOT_DEGRADED propagation. Added 4 new readiness slots (`SUBSYS_UEFI_VARS`, `SUBSYS_UEFI_TIME`, `SUBSYS_SECUREBOOT`, `SUBSYS_TPM`) and wired `uefi_vars_init`, `uefi_time_init`, `uefi_secureboot_init`, `tpm_init` + `tpm_integrity_init` to set them via captured `boot_result_t`. Items 4-8 above are now `[x]`. New unit test `test_subsys_phase0_propagation_slots` exercises the oracle round-trip on the new slot indices via the save/restore pattern (no live boot calls, per the test side-effect ban). `test_subsys_enum_layout` updated for new SUBSYS_COUNT == 25.

**Codex finding REJECTED (verify 2026-04-08):** Codex flagged `cpu_verify_hardening()` as advisory (logs WARN instead of halting on SMEP/SMAP missing). This matches the documented architectural state -- SMEP/SMAP enablement is gated on `hv_supports_cr4_smep_smap()` which always returns 0 until per-process page tables exist. The verify routine is correctly informational. The proper fix is per-process PML4 (covered by other TODOs), not Phase 0 hardening enforcement.

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
- [x] DPC bootstrap split: `dpc_init_queues()` in Phase 1 before `sti`, and `dpc_init()`/`dpc_start_threads()` in Phase 3 after scheduler start (→ XREF: [TODO-06 §4](./TODO-06-irql-model-dpcs.md))
- [x] `rtc_init()` -- BOOT_DEGRADED if unavailable; BOOT_REQUIRE(SUBSYS_IDT)
- [x] `keyboard_init()` + `mouse_init()` -- BOOT_DEGRADED if unavailable
- [x] `smbios_init()` -- POST code 0x40; BOOT_DEGRADED if unavailable; move here from Phase 0
- [x] `esrt_init()` + `mat_init()` + `uefi_conformance_init()` + `uefi_capsule_init()` + `uefi_crypto_agility_init()` -- BOOT_DEGRADED; move here from Phase 0
- [x] `secureboot_keys_init()` -- BOOT_DEGRADED; move here from Phase 0
- [x] `fb_init()` + `boot_splash_init()` -- BOOT_DEGRADED; display is optional for kernel correctness
- [x] `boot_timing_init()` + boot timing report log path after timer calibration
- [ ] Wire `except_init()` in Phase 1 after IDT/IRQ setup (→ XREF: [TODO-10-exception-dispatch-seh.md §1](./TODO-10-exception-dispatch-seh.md))
- [x] `__asm__ volatile ("sti")` -- enable interrupts only after all of the above
- [x] `boot_splash_start_animation()` -- after STI so LAPIC timer can drive the spinner
- [x] Remove `#include "kernel/fs/vfs.h"` and `#include "kernel/fs/partition.h"` from this file
- [x] Remove PCI scan, NIC init, DHCP from Phase 1 -- move to Phase 2
- [x] Remove SMP AP bringup from Phase 1 -- move to Phase 2 (already in boot_storage.c)
- [x] Remove all `HV_BAR` macro definitions and usages -- already removed in prior commit
- [x] Replace every `HV_BAR` site with `boot_progress(1, "step-name", postcode)` -- already done

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
- [/] `registry_init()` is wired after `SUBSYS_VFS`, but typed BOOT_FATAL propagation is still missing because `registry_init()` is `void` today
- [x] `symtab_init()` -- load symbol table from disk; BOOT_DEGRADED; BOOT_REQUIRE(SUBSYS_VFS)
- [x] `mmap_init()` -- user-mode memory map subsystem; BOOT_REQUIRE(SUBSYS_VMM)
- [x] Time service bootstrap in Phase 2: `mono_clock_init()` + `wall_clock_init()` + `timezone_init()` + `kusd_init()` (→ XREF: [TODO-07 §5](./TODO-07-time-filetime-management.md))
- [x] `rtl8139_init()` + `net_init()` -- NIC + network stack; BOOT_DEGRADED; moved from Phase 1
- [x] `virtio_input_init()` + `vbox_mouse_init()` -- BOOT_DEGRADED; moved from Phase 1
- [x] `dhcp_discover()` -- fire-and-forget; BOOT_DEGRADED; BOOT_REQUIRE(SUBSYS_VFS) for lease file
- [x] `hw_dump()` -- log full hardware summary after all drivers init
- [x] `smp_init()` AP bringup is executed in Phase 2 (moved from Phase 1) and marks `SUBSYS_SMP` ready
- [x] `acpi_power_init()` in Phase 2: parses S1/S3/S4 sleep objects from DSDT (implemented in [TODO-15 §1](./TODO-15-power-management.md))
- [x] Remove `HV_BAR` macro from `boot_storage.c` if present -- already removed in prior commit
- [x] Add `boot_progress(2, "step-name", postcode)` at each step
- [ ] Enforce typed fatal/degraded decisions from Phase 2 init return values (current gap: several Phase 2 init APIs are still `void` and have no boot_result_t propagation):
  - `vfs_init()` return-path ownership → [05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md §2](../05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md)
  - `registry_init()` return-path ownership → [TODO-13-registry-completion.md §1](./TODO-13-registry-completion.md)
  - `partition_mount_filesystems()` root-mount success/failure must be validated explicitly before continuing to registry

## 5. Phase 3 -- User Platform
Scheduler, IPC, exec loader, and desktop. The kernel is fully operational before this phase; failures here fall back to a text console, not BSOD.

**File:** `src/kernel/main/boot_desktop.c` (restructured as `boot_phase3`)

- [x] `task_init()` starts preemptive scheduler; BOOT_FATAL prerequisite path checks `SUBSYS_HEAP` + `SUBSYS_TIMER`
- [x] Phase 3 currently marks `SUBSYS_IPC` ready (implicit IPC bootstrap; no dedicated `ipc_init()` symbol wired here)
- [ ] Add explicit `ipc_init()` boot hook with `boot_result_t` error propagation before setting `SUBSYS_IPC` ready (→ XREF: [TODO-12-alpc-message-ports.md §1](./TODO-12-alpc-message-ports.md))
- [x] Phase 3 currently marks `SUBSYS_EXEC` ready (no dedicated `exec_loader_init()` bootstrap symbol wired here)
- [ ] Wire explicit exec-loader bootstrap before setting `SUBSYS_EXEC` ready (→ XREF: [TODO-08-binary-system.md §1](./TODO-08-binary-system.md))
- [x] `boot_tests_run()` executes only when `debug=1` or `test=1`; release path skips tests
- [x] `boot_splash_finish()` dismisses splash once desktop is ready
- [x] `ttf_mgr_init()` + `icon_store_init()` + `cursor_init()` load desktop assets
- [x] `desktop_init()` -- window manager + compositor init; BOOT_DEGRADED; fallback to serial console if fails
- [x] `compositor_run()` -- event loop (never returns under normal operation)
- [x] Phase 3 fatal-path behavior uses recovery screen + `boot_halt()` on prerequisite guards (same pattern as Phase 2); §7 failure policy table updated to match (2026-04-10)
- [x] Add `boot_progress(3, "step-name", postcode)` at each step
- [ ] Move syscall/SSDT fast-path bootstrap (`ssdt_init`, `syscall_init_fast`, `syscall_init`) to Phase 1 OR document/accept Phase 3 ownership and update [TODO-05-native-api-ssdt.md](./TODO-05-native-api-ssdt.md) XREF contract

## 6. Dependency Gates
- [x] Add dependency guards in phase orchestrators for all critical chains (inline checks + `boot_halt()` + `kernel_subsystem_dump()`)
- [x] Specifically verify these critical chains compile and enforce correctly at runtime:
  - `pmm_init` requires nothing; `vmm_init` requires PMM; `heap_init` requires VMM (Phase 0)
  - `klog_init` requires HEAP; all later subsystems require KLOG for safe logging (Phase 0)
  - `lapic_init` requires ACPI; `timer_hal_init` requires IDT (Phase 1)
  - `vfs_init` requires HEAP; `registry_init` requires VFS (Phase 2)
  - `sched_init` requires HEAP + TIMER (Phase 3)
- [x] Call `kernel_subsystem_dump()` on every BOOT_FATAL before halting -- added to `boot_halt()` and `panic_screen()`

## 7. Failure Policy
| Phase | Failure       | Action                                  |
| ----- | ------------- | --------------------------------------- |
| 0     | Any           | `boot_halt()` -- serial message + halt  |
| 1     | BOOT_FATAL    | `boot_halt()` (FB may not be ready yet)  |
| 1     | BOOT_DEGRADED | `klog(WARN)` + mark not ready, continue |
| 2     | BOOT_FATAL    | Recovery screen + `boot_halt()` (FB up) |
| 2     | BOOT_DEGRADED | `klog(WARN)` + continue                 |
| 3     | BOOT_FATAL    | Recovery screen + `boot_halt()` (same as Phase 2) |
| 3     | BOOT_DEGRADED | `klog(WARN)` + continue to desktop      |

- [x] Implement `boot_halt(const char *reason)` -- serial-only emergency stop for Phase 0 (already existed)
- [x] Ensure `panic()` works correctly when called before `fb_init()` (serial-only path) -- added SUBSYS_FB guard, falls back to serial+halt
- [x] Ensure `panic()` full BSOD path triggers only after FB is ready (end of Phase 1) -- guards on `kernel_subsystem_ready(SUBSYS_FB)`
- [x] Add `kernel_subsystem_dump()` call inside `boot_halt()` and `panic()` -- done in §6
- [x] Define and document which BOOT_FATAL events trigger auto-restart vs permanent halt based on `boot.conf` restart policy -- documented in boot_halt.c
- [x] Align failure-policy matrix with implemented code paths -- updated §7 table to match actual behavior: Phase 1 uses `boot_halt()` (FB may not be ready), Phase 2 uses recovery screen + `boot_halt()`, Phase 3 uses recovery screen + `boot_halt()` (same as Phase 2). Updated 2026-04-10.

## 8. Code Cleanup
These are bugs and structural violations that must be fixed as part of this TODO:

- [x] Remove all `HV_BAR` macro definitions and usages -- already removed in prior commits
- [x] Remove `#include "kernel/fs/vfs.h"` from `boot_interrupts.c`
- [x] Remove `#include "kernel/fs/partition.h"` from `boot_interrupts.c`
- [x] ACPI init split: `acpi_init()` Phase 1 (MADT/FADT), `acpi_power_init()` Phase 2 (S-states) -- implemented in [TODO-15 §1](./TODO-15-power-management.md)
- [x] Move `pci_scan()`, `xhci_init()`, NIC, and DHCP out of `boot_interrupts.c` into `boot_storage.c`
- [x] Move `smp_init()` AP bringup out of `boot_interrupts.c` into `boot_storage.c`
- [x] Move `ata_init()`, `virtio_blk_init()`, `ahci_init()` out of `boot_hw.c` into `boot_storage.c`
- [x] Move SMBIOS, ESRT, UEFI conformance, capsule, crypto agility, GOP log out of `boot_hw.c` into `boot_interrupts.c`
- [x] Simplify `main.c` to exactly: `boot_phase0()` → `boot_phase1()` → `boot_phase2()` → `boot_phase3()` → halt
- [x] Gate `boot_tests_run()` behind `g_boot_info.config.debug == 1 || g_boot_info.config.test == 1`
- [ ] Update init functions to return `boot_result_t` -- distributed to per-subsystem TODOs:
  - [x] `task_init()` -- done (returns `boot_result_t`)
  - [ ] `pipe_init()` → [TODO-12 §1](./TODO-12-alpc-message-ports.md)
  - [ ] `pmm/vmm/heap_init()` → [03-memory/TODO-01 §1](../03-memory-concurrency/TODO-01-vmm-memory-protection.md)
  - [ ] `vfs_init()` → [05-storage/TODO-06 §2](../05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md)
  - [ ] `registry_init()` → [TODO-13 §1](./TODO-13-registry-completion.md)

## 9. Degraded-Boot Recovery Screen
In-kernel graphical recovery UI shown when a Phase 2 subsystem fails non-fatally. Renders directly to the GOP framebuffer -- no compositor, no window manager required. Displays which subsystem failed, its POST code, and a simple recovery menu (retry / boot to serial console / power off).

**Files:** `src/kernel/main/boot_recovery.c`, `include/kernel/boot_recovery.h`

- [x] Design `boot_recovery_info_t` struct: failed subsystem, POST code, `boot_result_t`, phase number
- [x] Implement `boot_recovery_show(boot_recovery_info_t *info)` -- draws panel on framebuffer using inline 8x8 font; no heap alloc
- [x] Implement simple three-option menu: **[R] Retry**, **[C] Serial console**, **[P] Power off**; poll PS/2 keyboard via port 0x60/0x64
- [x] Hook into Phase 2 failure path: VFS and Registry guards call `boot_recovery_show()` before `boot_halt()`
- [x] Hook into Phase 3 failure path: Scheduler guard calls `boot_recovery_show()` before `boot_halt()`
- [x] Ensure `boot_recovery_show()` is a no-op (falls through to `boot_halt()`) if `SUBSYS_FB` is not ready
- [x] Add `boot_progress(9, "recovery-screen", 0xE0)` call on entry

## 10. POST Code + UEFI Variable Log
Write the current POST code to a UEFI NVRAM variable (`ImpossiblePOST`) at every phase boundary via `uefi_runtime_services.SetVariable`. The value survives a reboot, allowing post-mortem boot failure diagnosis on real hardware even when serial is unavailable.

**Files:** `src/kernel/boot_timing.c`, `include/kernel/boot_timing.h`, `src/kernel/uefi_runtime.c`

- [x] Define UEFI variable name: `ImpossiblePOST` in namespace GUID `{494D504F-5354-4F53-504F-535447554944}`
- [/] Implement 16-bit POST persistence helpers: `boot_post_write16()`, `boot_post_nvram_write16()` (runtime-availability handling is wired; `postcode=0` NVRAM gating still needs explicit handling)
- [x] `boot_progress()` path writes POST via `boot_post_write16(postcode)` for phase progress tracking
- [x] On successful boot completion, write final 16-bit code `0xFF00` (`POST16_BOOT_OK`) in `boot_phase3()`
- [x] On `boot_halt()` / `panic()`, write final 16-bit failure code `0xFFFE` (`POST16_BOOT_FAILED`) before halting
- [x] Implement `boot_post_read16()` -- reads last stored value; returns -1 if unavailable
- [x] Log prior POST code to serial at Phase 0 start: `[POST] Last boot code: 0xNN` with status interpretation
- [x] Add `boot_progress(10, "post-code-log", 0x11)` call after `uefi_runtime_init()` in Phase 0
- [ ] Add explicit `boot_post_nvram_write16()` gating for `boot.conf postcode=0` (currently only on-screen POST display is gated)

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
- [x] Commit: `"kernel: deferred init -- non-critical subsystems after desktop"`

**Test checkpoint:** Boot with default config → desktop appears → deferred inits run → `[DEFERRED]` lines appear in serial log after `[PHASE3] desktop_init`. Boot with `deferred=0` → no `[DEFERRED]` lines, all subsystems init in-phase. Verify on QEMU WHPX, TCG, VirtualBox, bare metal.

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
- [/] Add `bootperf` shell command -- deferred until `TODO-05-native-api-ssdt.md §10` implements `NtQuerySystemInformation(SystemBootPerformanceInformation)` for user-mode NVRAM access
- [x] Add debug POST codes: `POST16(0xDC00)` entry, `POST16(0xDC01)` NVRAM read, `POST16(0xDC02)` comparison done, `POST16(0xDC03)` NVRAM write
- [x] Add 3 unit tests: perf record size (24 bytes), BOOT_PERF_MAGIC value, bootperf POST code uniqueness
- [x] Commit: `"kernel: boot performance regression detection via UEFI NVRAM"`

**Test checkpoint:** Boot twice → second boot serial log shows `[PERF]` comparison lines. Inject a `sleep_ms(1000)` in a subsystem init → second boot shows `[PERF] WARNING` for that subsystem. Verify on QEMU WHPX, TCG, VirtualBox, bare metal. On bare metal, verify NVRAM read/write works with real UEFI firmware.

> [!NOTE]
> UEFI NVRAM has limited write endurance (~100K cycles). Current successful-boot path writes `ImpossiblePOST` 5 times (`POST16_SERIAL`, `POST16_SIMD_OK`, `POST16_TIMER_OK`, `POST16_REGISTRY_OK`, `POST16_BOOT_OK`) plus `ImpossibleBootPerf` once, for 6 writes/boot. This should be reduced or explicitly budgeted.

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
- [ ] Commit: `"kernel: async subsystem init -- SMP parallel Phase 2"`

**Test checkpoint:** Boot with `async_init=1` → storage/input/network init on different CPUs → serial log shows `[ASYNC]` entries with different CPU numbers. Total Phase 2 time decreases vs sequential. Boot with `async_init=0` → sequential behavior unchanged. Verify on QEMU WHPX (2+ vCPUs), TCG, VirtualBox, bare metal. Bare metal is critical -- per-AP fault isolation must work on real hardware.

## OS Comparison

| ⭐ | Feature              | 🪟 Win11           | 🐧 Linux              | 🚀 Impossible OS         |
|----|----------------------|---------------------|-----------------------|---------------------------|
| 💎 | Formal phase model   | ✅ Phase 0/1       | ✅ initcall levels    | ✅ §2--§5 4 phases       |
| 💎 | Interrupts-off phase | ✅ Phase 0         | ✅ early start_kernel | ✅ §2 boot_phase0        |
| 💎 | Dependency ordering  | ✅ Boot load groups| ✅ initcall deps      | ✅ §6 gates done         |
| 💎 | Typed init results   | ✅ NTSTATUS        | ✅ initcall_t         | ⚠️ §1 boot_result_t      |
| 💎 | Halt on critical     | ✅ Bugcheck        | ✅ panic()            | ✅ §7 boot_halt          |
| 💎 | Degraded boot        | ✅ Safe mode       | ✅ Emergency shell    | ✅ §7 BOOT_DEGRADED      |
| 💎 | Boot serial log      | ✅ DebugPrint/ETW  | ✅ early_printk       | ✅ §2 [PHASE0] markers   |
| 💎 | Boot config gating   | ✅ Registry        | ✅ cmdline            | ✅ §2 boot.conf          |
| 💎 | Tests separated      | ✅ Separate env    | ✅ initcall_debug     | ✅ §5 debug/test gate    |
| ⭐ | Recovery UI at boot  | ❌ Separate WinRE  | ❌ Text-only shell    | ✅ §9 graphical recovery |
| ⭐ | POST to UEFI NVRAM   | ❌ Firmware-only   | ❌ Not implemented    | ✅ §10 ImpossiblePOST    |
| ⭐ | Readiness oracle API | ⚠️ Private internal| ⚠️ system_state only  | ✅ §1 public API         |
| 💎 | Deferred init        | ✅ Delayed services| ✅ deferred_initcall  | ✅ §11 boot_defer()      |
| ⭐ | Boot perf regression | ❌ Manual ETW      | ❌ Manual bootchart   | ✅ §12 auto NVRAM diff   |
| ⭐ | Parallel kernel init | ⚠️ DLL load only   | ⚠️ async_schedule     | ✅ §13 IPI async group   |

> After parity items, Impossible OS matches Windows NT and Linux on phased init.
> Exclusive: graphical recovery UI, UEFI NVRAM POST codes, public readiness oracle, auto boot perf regression, SMP parallel init.

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

## Codex Adversarial Review

> Reviewed 2026-04-05 by Codex (o3). Scope: §1-§7, §9-§13.
> **Round 1:** 4 findings (1 critical, 3 high). All addressed.
> **Round 2:** 2 findings. 1 fixed (GSI mask), 1 accepted (deferred fault isolation).
> **Final verdict: resolved.** Build clean.

| # | Severity | Finding | Status |
|---|----------|---------|--------|
| 1 | critical | `s_subsys_names[]` missing SUBSYS_OB -- OOB read in panic diagnostic path | **Fixed** -- added "OB" entry + `_Static_assert` in both tables |
| 2 | high | `boot_recovery_show()` return value ignored -- menu is cosmetic | **Fixed** -- all call sites branch on RECOVERY_POWEROFF -> `acpi_shutdown()` |
| 3 | high | Deferred init not fault-isolated -- optional driver panic aborts Phase 3 | **Reverted** -- thread starved deferred inits (broke mouse); inline call restored; isolation deferred to TODO-10 SEH |
| 4 | high | Recovery keyboard polling races active PS/2 IRQ handler | **Fixed** -- `kbd_poll_begin()`: cli + mask routed GSI + drain |
| 5 | high | Recovery masks raw IRQ1 instead of ACPI-remapped GSI | **Fixed** -- uses `ioapic_isa_to_gsi(1)` for correct GSI |
| 6 | high | Deferred init not fault-contained | **Accepted** -- thread approach reverted (starved mouse drivers); inline call restored; per-thread isolation requires TODO-10 SEH |

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
