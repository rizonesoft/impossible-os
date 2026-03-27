# TODO-01 — Kernel Init Sequencing

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
- → XREF: `TODO-02-system-logging.md` — `klog_disk_enable()` is a Phase 2 gate; must follow VFS ready
- → XREF: `TODO-03-object-manager.md §1` — Object Manager init slot is Phase 2, after heap, before registry; §1 provides the `ob_init()` implementation
- → XREF: `04-drivers-hardware/INDEX.md` — all driver `_init()` functions must accept and return `boot_result_t`
- → XREF: `00-infrastructure/TODO-02-developer-tooling-stack.md` — headless QEMU serial log is the verification path
- → XREF: `TODO-06-irql-model-dpcs.md §4` — DPC subsystem init belongs in Phase 1, after timer; §4 is the DPC Object Type and Per-CPU Queue init

## Outcome

- `main.c` is reduced to four sequential phase calls: `boot_phase0()` → `boot_phase1()` → `boot_phase2()` → `boot_phase3()` → halt.
- Every subsystem init function returns `boot_result_t`; none return `void`.
- `kernel_subsystem_ready(SUBSYS_X)` correctly reports the live state of every registered subsystem.
- Serial log shows `[PHASE0]` … `[PHASE3]` markers with POST codes and timestamps.
- No `HV_BAR` pixel-write calls anywhere in the boot path.
- Boot tests run only when `debug=1`; release boots are silent on that path.
- A forced VFS failure produces a degraded-boot screen, not a BSOD or hang.

## Implementation Order

| ⭐  | Order | Deliverable                   | Depends On | Status |
| --- | :---: | ----------------------------- | ---------- | :----: |
| 💎  |   1   | Boot init infrastructure      | —          |  [x]   |
| 💎  |   2   | Phase 0 — critical init       | §1         |  [x]   |
| 💎  |   3   | Phase 1 — platform services   | §2         |  [/]   |
| 💎  |   4   | Phase 2 — system services     | §3         |  [/]   |
| 💎  |   5   | Phase 3 — user platform       | §4         |  [ ]   |
| 💎  |   6   | Dependency gates              | §1–5       |  [ ]   |
| 💎  |   7   | Failure policy                | §6         |  [ ]   |
| 💎  |   8   | Code cleanup                  | §2–5       |  [ ]   |
| ⭐  |   9   | Degraded-boot recovery screen | §7         |  [ ]   |
| ⭐  |  10   | POST code + UEFI variable log | §1         |  [ ]   |

> 💎 = parity — Windows NT and Linux both have formal init phase models; Impossible OS must match them.
> ⭐ = exclusive — degraded-boot recovery UI and UEFI NVRAM POST log are not present in either competitor.


## 1. Boot Init Infrastructure `[Sonnet]`

New header and source file providing the result type, readiness oracle, and progress tracker used by every phase.

**Files:** `include/kernel/boot_init.h`, `src/kernel/main/boot_init.c`

- [x] Define `boot_result_t`: `BOOT_OK = 0`, `BOOT_DEGRADED = 1`, `BOOT_FATAL = 2`
- [x] Define `kernel_subsys_t` enum — one entry per subsystem that others can depend on: `SUBSYS_SERIAL`, `SUBSYS_PMM`, `SUBSYS_VMM`, `SUBSYS_HEAP`, `SUBSYS_KLOG`, `SUBSYS_GDT`, `SUBSYS_IDT`, `SUBSYS_ACPI`, `SUBSYS_LAPIC`, `SUBSYS_IOAPIC`, `SUBSYS_TIMER`, `SUBSYS_RTC`, `SUBSYS_FB`, `SUBSYS_VFS`, `SUBSYS_REGISTRY`, `SUBSYS_SCHED`, `SUBSYS_IPC`, `SUBSYS_SMP`, `SUBSYS_EXEC`, `SUBSYS_DESKTOP`, `SUBSYS_COUNT`
- [x] Implement static `bool g_subsys_ready[SUBSYS_COUNT]` table in `boot_init.c`
- [x] Implement `bool kernel_subsystem_ready(kernel_subsys_t subsys)`
- [x] Implement `void kernel_subsystem_set_ready(kernel_subsys_t subsys, bool ok)`
- [x] Implement `void kernel_subsystem_dump(void)` — prints all subsystem states via `klog`
- [x] Define POST code constants for every major init step (`POSTCODE_PMM_INIT = 0x20`, etc.)
- [x] Implement `void boot_progress(uint8_t phase, const char *step, uint8_t postcode)` — writes `[PHASEn] step` to serial and records timestamp in `boot_timing.c`
- [x] Define `BOOT_REQUIRE(subsys)` macro — if `!kernel_subsystem_ready(subsys)`, logs the missing prerequisite and returns `BOOT_FATAL`
- [x] Define `BOOT_STEP(subsys, fn)` macro — calls `fn()`, sets readiness from result, calls `boot_progress()`
- [x] Expose `boot_phase0()`, `boot_phase1()`, `boot_phase2()`, `boot_phase3()` in `main_internal.h`

## 2. Phase 0 — Critical Init (Interrupts Disabled) `[Opus]`

Runs with interrupts off. Only serial, memory, and logging. No drivers, VFS, or network. Any failure in Phase 0 calls `boot_halt()` on serial — framebuffer is not yet available.

**File:** `src/kernel/main/boot_hw.c` (restructured as `boot_phase0`)

- [x] `serial_init()` — absolute first call; no dependencies; POST code 0x10
- [x] `boot_info_parse(magic, mbi)` — parse UEFI or Multiboot2 info; halt on unknown magic
- [x] `uefi_runtime_init()` — `SetVirtualAddressMap` + runtime props; BOOT_DEGRADED if unavailable
- [x] `uefi_vars_init()` — NVRAM variable enumeration; BOOT_DEGRADED if unavailable
- [x] `uefi_time_init()` — seed wall clock from UEFI RTC; BOOT_DEGRADED if unavailable
- [x] `uefi_secureboot_init()` — detect Secure Boot state; BOOT_DEGRADED if unavailable
- [x] `tpm_init()` — parse measured boot event log; BOOT_DEGRADED if no TPM
- [x] `tpm_integrity_init()` — PCR golden value check; BOOT_DEGRADED on mismatch
- [x] `pmm_init()` — physical memory manager; BOOT_FATAL if fails; POST code 0x20
- [x] `vmm_init()` — virtual memory manager; BOOT_FATAL if fails; BOOT_REQUIRE(SUBSYS_PMM)
- [x] `heap_init()` — kernel heap; BOOT_FATAL if fails; BOOT_REQUIRE(SUBSYS_VMM)
- [x] `klog_init()` — in-memory ring buffer only (no disk yet); BOOT_REQUIRE(SUBSYS_HEAP)
- [x] `cpuid_init()` — probe CPU features; BOOT_DEGRADED on very old CPU
- [x] `simd_enable()` — enable AVX2 or fall back to SSE2; BOOT_DEGRADED on no AVX2
- [x] `boot_config_parse()` — read `boot.conf` settings into `g_boot_info.config`; BOOT_DEGRADED on missing file (use defaults)
- [x] Remove driver includes (`ata.h`, `virtio_blk.h`, `ahci.h`) from this file — they belong in Phase 2
- [x] Remove SMBIOS, ESRT, UEFI conformance, GOP mode log from Phase 0 — move to Phase 1
- [x] Remove all `HV_BAR` macro definitions and usages (12 sites in `boot_hw.c`) — already removed in prior commit
- [x] Replace every `HV_BAR` site with `boot_progress(0, "step-name", postcode)` — already done in prior commit

## 3. Phase 1 — Platform Services (Interrupts Enabled at End) `[Opus]`

Hardware abstraction layer: GDT/IDT, interrupt controllers, timer, RTC, display. BOOT_FATAL halts; BOOT_DEGRADED logs and continues. Interrupts enabled with `sti` only after LAPIC/timer are ready.

**File:** `src/kernel/main/boot_interrupts.c` (restructured as `boot_phase1`)

- [x] `gdt_init()` — BOOT_FATAL; POST code 0x30
- [x] `idt_init()` + `irq_init()` — BOOT_FATAL; BOOT_REQUIRE(SUBSYS_GDT)
- [x] `acpi_init()` — MADT + FADT parsing only (CPU count, LAPIC base, IOAPIC base, PM1a port); BOOT_FATAL; BOOT_REQUIRE(SUBSYS_IDT)
- [x] `lapic_init()` — BOOT_FATAL on APIC-only platforms, BOOT_DEGRADED if PIC fallback available; BOOT_REQUIRE(SUBSYS_ACPI)
- [x] `ioapic_init()` — BOOT_DEGRADED if unavailable; BOOT_REQUIRE(SUBSYS_LAPIC)
- [x] `pic_disable_or_init()` — disable if IOAPIC took over; init if PIC is the only controller
- [x] `ahci_setup_interrupts()` — MSI routing only; BOOT_DEGRADED if fails; BOOT_REQUIRE(SUBSYS_LAPIC)
- [x] `timer_hal_init()` — select LAPIC or PIT backend, calibrate; BOOT_FATAL; BOOT_REQUIRE(SUBSYS_IDT)
- [ ] `dpc_init()` — per-CPU DPC queue and drain loop; BOOT_FATAL; BOOT_REQUIRE(SUBSYS_TIMER) — see TODO-06 (not yet implemented)
- [x] `rtc_init()` — BOOT_DEGRADED if unavailable; BOOT_REQUIRE(SUBSYS_IDT)
- [x] `keyboard_init()` + `mouse_init()` — BOOT_DEGRADED if unavailable
- [x] `smbios_init()` — POST code 0x40; BOOT_DEGRADED if unavailable; move here from Phase 0
- [x] `esrt_init()` + `mat_init()` + `uefi_conformance_init()` + `uefi_capsule_init()` + `uefi_crypto_agility_init()` — BOOT_DEGRADED; move here from Phase 0
- [x] `secureboot_keys_init()` — BOOT_DEGRADED; move here from Phase 0
- [x] `fb_init()` + `boot_splash_init()` — BOOT_DEGRADED; display is optional for kernel correctness
- [x] `boot_timing_report()` — log TSC + FPDT data after timer is calibrated
- [x] `__asm__ volatile ("sti")` — enable interrupts only after all of the above
- [x] `boot_splash_start_animation()` — after STI so LAPIC timer can drive the spinner
- [x] Remove `#include "kernel/fs/vfs.h"` and `#include "kernel/fs/partition.h"` from this file
- [x] Remove PCI scan, NIC init, DHCP from Phase 1 — move to Phase 2
- [x] Remove SMP AP bringup from Phase 1 — move to Phase 2 (already in boot_storage.c)
- [x] Remove all `HV_BAR` macro definitions and usages — already removed in prior commit
- [x] Replace every `HV_BAR` site with `boot_progress(1, "step-name", postcode)` — already done

## 4. Phase 2 — System Services `[Sonnet]`

Storage, VFS, filesystem mount, registry, network, and AP bringup. BOOT_FATAL only if VFS or registry are completely broken; everything else degrades.

**File:** `src/kernel/main/boot_storage.c` (restructured as `boot_phase2`)

- [x] `pci_scan()` — enumerate PCI/PCIe bus; BOOT_DEGRADED if no devices found; moved from Phase 1
- [ ] `object_manager_init()` — ObInit: bootstrap object type singletons and root namespace; BOOT_FATAL; BOOT_REQUIRE(SUBSYS_HEAP) — see TODO-03 (not yet implemented)
- [x] `xhci_init()` — USB host controller; BOOT_DEGRADED; moved from Phase 1
- [x] `ata_init()` + `virtio_blk_init()` + `ahci_init()` — storage drivers; BOOT_DEGRADED if all fail; moved from Phase 0
- [x] `blkdev_register_all()` — register block devices into the blkdev layer
- [x] `vfs_init()` — BOOT_FATAL if fails; BOOT_REQUIRE(SUBSYS_HEAP)
- [x] `partition_scan_all()` + `partition_mount_filesystems()` — BOOT_FATAL if no root partition mounts
- [x] `klog_disk_enable()` — open `C:\Impossible\System\Logs\kernel.log`; BOOT_DEGRADED; BOOT_REQUIRE(SUBSYS_VFS)
- [x] `registry_init()` — BOOT_FATAL if fails after VFS is up; BOOT_REQUIRE(SUBSYS_VFS)
- [x] `symtab_init()` — load symbol table from disk; BOOT_DEGRADED; BOOT_REQUIRE(SUBSYS_VFS)
- [x] `mmap_init()` — user-mode memory map subsystem; BOOT_REQUIRE(SUBSYS_VMM)
- [x] `rtl8139_init()` + `net_init()` — NIC + network stack; BOOT_DEGRADED; moved from Phase 1
- [x] `virtio_input_init()` + `vbox_mouse_init()` — BOOT_DEGRADED; moved from Phase 1
- [x] `dhcp_discover()` — fire-and-forget; BOOT_DEGRADED; BOOT_REQUIRE(SUBSYS_VFS) for lease file
- [x] `hw_dump()` — log full hardware summary after all drivers init
- [x] `smp_start_aps()` — bringup APs; BOOT_DEGRADED if any AP fails; BOOT_REQUIRE(SUBSYS_HEAP) + BOOT_REQUIRE(SUBSYS_REGISTRY); moved from Phase 1
- [ ] Consolidate all ACPI power management (`acpi_power_init`, `_S5` parse) into Phase 2 — deferred (ACPI power not yet implemented)
- [x] Remove `HV_BAR` macro from `boot_storage.c` if present — already removed in prior commit
- [x] Add `boot_progress(2, "step-name", postcode)` at each step

## 5. Phase 3 — User Platform `[Sonnet]`

Scheduler, IPC, exec loader, and desktop. The kernel is fully operational before this phase; failures here fall back to a text console, not BSOD.

**File:** `src/kernel/main/boot_desktop.c` (restructured as `boot_phase3`)

- [ ] `sched_init()` — start preemptive scheduler; BOOT_FATAL if fails; BOOT_REQUIRE(SUBSYS_HEAP) + BOOT_REQUIRE(SUBSYS_TIMER)
- [ ] `ipc_init()` — init pipe, shmem, and signal subsystems; BOOT_REQUIRE(SUBSYS_SCHED)
- [ ] `exec_loader_init()` — register ELF/PE32+/EIF format handlers; BOOT_DEGRADED; BOOT_REQUIRE(SUBSYS_VFS)
- [ ] `boot_tests_run()` — **only** if `g_boot_info.config.debug == 1`; skip entirely in release
- [ ] `boot_splash_complete()` — dismiss splash screen; requires SUBSYS_FB
- [ ] `font_init()` + `icon_init()` + `cursor_init()` — load assets from VFS; BOOT_DEGRADED
- [ ] `desktop_init()` — window manager + compositor init; BOOT_DEGRADED; fallback to serial console if fails
- [ ] `compositor_run()` — event loop (never returns under normal operation)
- [ ] On any BOOT_FATAL in Phase 3: do NOT BSOD — log via `klog(FATAL)` and drop to serial console loop
- [ ] Add `boot_progress(3, "step-name", postcode)` at each step

## 6. Dependency Gates `[Sonnet]`

- [ ] Add `BOOT_REQUIRE(subsys)` call at the top of every init function listed in phases 0–3 above
- [ ] Specifically verify these critical chains compile and enforce correctly at runtime:
  - `pmm_init` requires nothing; `vmm_init` requires PMM; `heap_init` requires VMM
  - `klog_init` requires HEAP; all later subsystems require KLOG for safe logging
  - `lapic_init` requires ACPI; `timer_hal_init` requires IDT + (LAPIC when APIC timer)
  - `vfs_init` requires HEAP; `registry_init` requires VFS; `klog_disk_enable` requires VFS
  - `sched_init` requires HEAP + TIMER; `ipc_init` requires SCHED
- [ ] Call `kernel_subsystem_dump()` on every BOOT_FATAL before halting so the serial log captures full state

## 7. Failure Policy `[Sonnet]`

| Phase | Failure       | Action                                  |
| ----- | ------------- | --------------------------------------- |
| 0     | Any           | `boot_halt()` — serial message + halt   |
| 1     | BOOT_FATAL    | BSOD + halt (FB available by end of P1) |
| 1     | BOOT_DEGRADED | `klog(WARN)` + mark not ready, continue |
| 2     | BOOT_FATAL    | BSOD; degraded-boot screen if VFS up    |
| 2     | BOOT_DEGRADED | `klog(WARN)` + continue                 |
| 3     | Any           | `klog(ERROR)` + serial console fallback |

- [ ] Implement `boot_halt(const char *reason)` — serial-only emergency stop for Phase 0
- [ ] Ensure `panic()` works correctly when called before `fb_init()` (serial-only path)
- [ ] Ensure `panic()` full BSOD path triggers only after FB is ready (end of Phase 1)
- [ ] Add `kernel_subsystem_dump()` call inside `boot_halt()` and `panic()`
- [ ] Define and document which BOOT_FATAL events trigger auto-restart vs permanent halt based on `boot.conf` restart policy

## 8. Code Cleanup `[Sonnet]`

These are bugs and structural violations that must be fixed as part of this TODO:

- [ ] Remove all `HV_BAR` macro definitions and usages — `boot_hw.c` (12 sites), `boot_interrupts.c` (14 sites); replace with `boot_progress()` calls
- [ ] Remove `#include "kernel/fs/vfs.h"` from `boot_interrupts.c` (line 31)
- [ ] Remove `#include "kernel/fs/partition.h"` from `boot_interrupts.c` (line 32)
- [ ] Consolidate ACPI: remove the second `acpi_init()` call in `boot_storage.c`; split into `acpi_platform_init()` (Phase 1: MADT/FADT) and `acpi_power_init()` (Phase 2: S-states)
- [ ] Move `pci_scan()`, `xhci_init()`, NIC, and DHCP out of `boot_interrupts.c` into `boot_storage.c`
- [ ] Move `smp_init()` AP bringup out of `boot_interrupts.c` into `boot_storage.c`
- [ ] Move `ata_init()`, `virtio_blk_init()`, `ahci_init()` out of `boot_hw.c` into `boot_storage.c`
- [ ] Move SMBIOS, ESRT, UEFI conformance, capsule, crypto agility, GOP log out of `boot_hw.c` into `boot_interrupts.c`
- [ ] Simplify `main.c` to exactly: `boot_phase0()` → `boot_phase1()` → `boot_phase2()` → `boot_phase3()` → halt
- [ ] Gate `boot_tests_run()` behind `g_boot_info.config.debug == 1` check
- [ ] Update all init functions in `sched/`, `ipc/`, `mm/`, `fs/` to return `boot_result_t` where they currently return `void`

## 9. Degraded-Boot Recovery Screen `[Opus]`

In-kernel graphical recovery UI shown when a Phase 2 subsystem fails non-fatally. Renders directly to the GOP framebuffer — no compositor, no window manager required. Displays which subsystem failed, its POST code, and a simple recovery menu (retry / boot to serial console / power off).

**Files:** `src/kernel/main/boot_recovery.c`, `include/kernel/boot_recovery.h`

- [ ] Design `boot_recovery_info_t` struct: failed subsystem, POST code, `boot_result_t`, phase number
- [ ] Implement `boot_recovery_show(boot_recovery_info_t *info)` — draws panel on framebuffer using `gfx_fill_rect`, `gfx_draw_text`; no heap alloc after Phase 1
- [ ] Implement simple three-option menu: **[R] Retry**, **[C] Serial console**, **[P] Power off**; poll keyboard via `keyboard_poll()`
- [ ] Hook into Phase 2 failure path: call `boot_recovery_show()` instead of `panic()` when `BOOT_FATAL` and `SUBSYS_FB` is ready
- [ ] Hook into Phase 3 failure path: call `boot_recovery_show()` for any BOOT_FATAL
- [ ] Ensure `boot_recovery_show()` is a no-op (falls through to `boot_halt()`) if `SUBSYS_FB` is not ready
- [ ] Add `boot_progress(9, "recovery-screen", 0xE0)` call on entry

## 10. POST Code + UEFI Variable Log `[Opus]`

Write the current POST code to a UEFI NVRAM variable (`ImpossiblePOST`) at every phase boundary via `uefi_runtime_services.SetVariable`. The value survives a reboot, allowing post-mortem boot failure diagnosis on real hardware even when serial is unavailable.

**Files:** `src/kernel/boot_timing.c`, `include/kernel/boot_timing.h`, `src/kernel/uefi_runtime.c`

- [ ] Define UEFI variable name: `ImpossiblePOST` in namespace GUID `{impossible-os-post-guid}`
- [ ] Implement `boot_post_write(uint8_t code)` — calls `gRT->SetVariable` with `EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS`; gracefully no-ops if runtime services unavailable
- [ ] Call `boot_post_write(postcode)` at each `boot_progress()` call site in phases 0–3
- [ ] On successful boot completion, write final code `0xFF` (`POSTCODE_BOOT_OK`)
- [ ] On `boot_halt()` / `panic()`, write `0xFE` (`POSTCODE_BOOT_FAILED`) before halting
- [ ] Implement `boot_post_read()` — reads last stored value; used during next boot to detect prior crash
- [ ] Log prior POST code to serial at Phase 0 start: `[POST] Last boot code: 0xNN`
- [ ] Add `boot_progress(10, "post-code-log", 0x11)` call after `serial_init()` in Phase 0

## OS Comparison


| ⭐  | Feature                              | 🪟 Windows NT / 11                         | 🐧 Linux                                   | 🚀 Impossible OS                                         |
| --- | ------------------------------------ | ------------------------------------------- | ------------------------------------------ | --------------------------------------------------------- |
| 💎  | Formal phase model                   | ✅ Phase 0 / Phase 1 init                  | ✅ initcall levels (early → late)          | 🔄 In progress — §2 done, §3 done (dpc pending), §4–5 planned |
| 💎  | Interrupt-disabled critical phase    | ✅ Phase 0 (no interrupts, no paging)      | ✅ `start_kernel` early before `sti`       | ✅ Done — §2; `boot_phase0()` runs with interrupts off   |
| 💎  | Dependency-ordered subsystem init    | ✅ Boot driver load groups + ordering      | ✅ initcall dependency ordering            | 🔄 In progress — §2–4 done, §6 gates pending             |
| 💎  | Typed init failure results           | ✅ `NTSTATUS` from every init routine      | ✅ `initcall_t` return codes               | ⬜ Planned — Step 1 (`boot_result_t`)                    |
| 💎  | Halt on critical subsystem failure   | ✅ Bugcheck + halt                         | ✅ `panic()` + halt                        | ⬜ Planned — Step 7                                      |
| 💎  | Degraded boot on non-critical fail   | ✅ Last-known-good, safe mode              | ✅ Emergency shell fallback                | ⬜ Planned — Step 7                                      |
| 💎  | Boot progress serial log             | ✅ `DebugPrint` / ETW early tracing        | ✅ `early_printk` / `earlyprintk=serial`   | ✅ Done — §2; `[PHASE0]` markers with POST codes          |
| 💎  | Boot config gating                   | ✅ `SYSTEM\CurrentControlSet\Control\`     | ✅ kernel cmdline / initrd config          | ✅ Done — §2; `boot.conf` parsed in Phase 0               |
| 💎  | Test-path separated from boot path   | ✅ Tests run in separate test OS builds    | ✅ `initcall_debug` opt-in                 | ⬜ Planned — Step 5 (debug flag gate)                    |
| ⭐  | Degraded-boot recovery UI screen     | ❌ Safe mode is a separate boot mode       | ❌ Emergency shell is text-only            | ⬜ **Planned — Step 9 — in-kernel graphical recovery**   |
| ⭐  | POST code written to UEFI NVRAM      | ❌ POST codes are firmware-only            | ❌ Not implemented                         | ⬜ **Planned — Step 10 — survives reboot for diagnosis** |
| ⭐  | Subsystem readiness oracle API       | ⚠️ Private internal only, not exposed      | ⚠️ `system_state` enum only                | ⬜ **Planned — Step 1 — `kernel_subsystem_ready()` API** |

> **After parity items:** Impossible OS matches Windows NT and Linux on formal phased init, typed results, and dependency ordering.
> **Exclusive items:** The degraded-boot recovery UI lets a user see exactly which subsystem failed and choose a recovery action — no other OS provides this at the kernel level. UEFI NVRAM POST codes survive a reboot, giving post-mortem boot failure diagnosis on real hardware even when serial is unavailable.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Headless QEMU serial log shows `[PHASE0]`…`[PHASE3]` markers in dependency order
- [ ] Serial log contains no `HV_BAR` or raw pixel-write output
- [ ] Serial log shows `kernel_subsystem_dump()` output before any halt
- [ ] Forcing PMM failure causes `boot_halt()` on serial — no framebuffer writes attempted
- [ ] Forcing VFS failure causes degraded-boot screen — kernel stays up, no BSOD
- [ ] `boot_tests_run()` does not appear in serial log when `debug=0`
- [ ] `boot_tests_run()` does appear in serial log when `debug=1`
- [ ] Commit: `"kernel: init-sequencing verified — phases, readiness oracle, dependency gates"`

