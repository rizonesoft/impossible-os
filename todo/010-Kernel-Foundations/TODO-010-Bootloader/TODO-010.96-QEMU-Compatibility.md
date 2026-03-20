# TODO-010.96 — QEMU/WHPX Compatibility Fixes

> **Goal:** Eliminate boot hangs and disk I/O stalls when running Impossible OS
> in QEMU on Windows with WHPX (Windows Hypervisor Platform) acceleration.
> These fixes address hardware emulation differences between WHPX, KVM, and TCG
> that caused the kernel to hang during LAPIC init, AHCI partition scanning,
> and filesystem mounting.

> [!IMPORTANT]
> **Motivation:** QEMU on Windows defaults to TCG (software emulation), which is
> unusably slow for daily development. WHPX provides near-native CPU performance
> but introduces subtle differences in interrupt delivery, MMIO handling, and
> timer behavior that exposed bugs in the AHCI driver and LAPIC initialization.

> [!WARNING]
> → XREF: `TODO-010.99-APIC-First-Boot.md` — LAPIC/IOAPIC init ordering
> → XREF: `TODO-010-Bootloader.md` — Boot sequence and storage init
> These fixes are tightly coupled with the APIC-first boot refactor and the
> StorVSC synthetic SCSI driver for Hyper-V Gen 2.

---

## TODO Completion Roadmap

> [!NOTE]
> **All sections are completed.** This TODO documents fixes already applied and
> committed. It serves as a reference for QEMU/WHPX gotchas to prevent future
> regressions.

### Phase-by-Phase Implementation Order

| ⭐ | Phase | Section                                   | What It Delivers                                          | Depends On    | Status |
| -- | :----: | ----------------------------------------- | --------------------------------------------------------- | ------------- | :----: |
| 💎 | **1** | §1 AHCI Interrupt Storm Fix               | Disable unhandled AHCI PCI interrupts                     | —             |   ✅   |
| 💎 | **1** | §2 AHCI Polling Loop PAUSE Removal        | Remove PAUSE instructions that trigger Hyper-V PLE        | —             |   ✅   |
| 💎 | **2** | §3 LAPIC Init Timeout / Single-CPU Guard  | Prevent hang during Init Level De-Assert IPI              | —             |   ✅   |
| 💎 | **2** | §4 AHCI port_start_cmd Timeout            | Add timeout to CR bit busy-wait loop                      | —             |   ✅   |
| 💎 | **3** | §5 QEMU Launch Script (PowerShell)        | WHPX-first acceleration, proper CPU model                 | —             |   ✅   |
| 💎 | **3** | §6 Hyper-V StorVSC Boot Order Fix         | Re-scan partitions after StorVSC registers block device   | §1            |   ✅   |

---

## 1. AHCI Interrupt Storm Fix ✅ *(agent)*

> [!IMPORTANT]
> **This was the primary fix that resolved the WHPX boot hang.** All other
> changes are secondary hardening.

> [!NOTE]
> **Root cause (2026-03-20):** The AHCI driver enabled per-port interrupts
> (`PxIE = 0x01`, D2H Register FIS interrupt) and global interrupts (`GHC.IE`)
> but **never registered a PCI IRQ handler**. Under KVM and TCG, unhandled AHCI
> interrupts are harmlessly dropped by QEMU's emulation layer. Under WHPX,
> the interrupt is injected into the guest CPU via Windows Hypervisor. Since no
> ISR clears the AHCI `PxIS` status register, the level-triggered interrupt
> fires continuously — creating an **interrupt storm** that starves the CPU.
> The polling loop in `port_issue_cmd()` never gets to run because the CPU is
> trapped in the default IDT handler servicing the uncleared AHCI IRQ.
>
> **Why IDENTIFY worked but partition scan didn't:** AHCI `IDENTIFY DEVICE`
> runs during `boot_hw_init()` at `[0.000]`, **before** the IOAPIC is
> initialized. With no IOAPIC routing, AHCI PCI interrupts can't reach the
> CPU, so the command completes normally via polling. Partition scanning runs
> at `[3.500]`, **after** IOAPIC routes are configured — now the AHCI
> interrupt CAN fire, and the storm begins.
>
> **Fix:** Disable AHCI interrupts entirely (`PxIE = 0x00`, `GHC.IE` cleared)
> since we use polling, not interrupt-driven I/O. Commit `a67d88f`.

- [x] Clear `PxIE` (per-port interrupt enable) to `0x00` in `port_init()`
- [x] Clear `GHC.IE` (global interrupt enable) in `ahci_init()`
- [x] Preserve `PxIS` clear-on-read after command completion (hygiene)
- [x] Verify: QEMU TCG smoke test passes — partitions scan, C:\ mounts
- [x] Verify: QEMU WHPX boots past partition scan without hanging
- [x] Commit: `"ahci: disable AHCI interrupts — we poll, no IRQ handler registered"` (`a67d88f`)

---

## 2. AHCI Polling Loop PAUSE Removal ✅ *(agent)*

> [!NOTE]
> **Root cause (2026-03-20):** The AHCI polling loops in `port_issue_cmd()`,
> `port_stop_cmd()`, and `port_start_cmd()` all contained
> `__asm__ volatile("pause")`. Under Hyper-V / WHPX, the hypervisor's **Pause
> Loop Exit (PLE)** detector recognizes spin loops containing PAUSE instructions
> and **suspends the vCPU for ~100ms per iteration** — a feature designed to
> handle spinlock contention in multi-vCPU VMs. This devastates tight MMIO
> polling loops: each iteration that should take nanoseconds instead takes
> 100ms, making a 5M-iteration timeout take days.
>
> **Fix:** Remove all `PAUSE` instructions from AHCI polling. The MMIO reads
> (`port_read(pregs, AHCI_PxCI)`) already cause VM exits on every iteration,
> providing natural yield points. `PAUSE` is unnecessary and
> counterproductive when MMIO reads are the loop body. Commit `d9402f2`.

- [x] Remove `__asm__ volatile("pause")` from `port_stop_cmd()` CR wait loop
- [x] Remove `__asm__ volatile("pause")` from `port_stop_cmd()` FR wait loop
- [x] Remove `__asm__ volatile("pause")` from `port_start_cmd()` CR wait loop
- [x] Remove `__asm__ volatile("pause")` from `port_issue_cmd()` CI poll loop
- [x] Add comments explaining why PAUSE is absent (Hyper-V PLE)
- [x] Commit: `"ahci: remove PAUSE from polling loops — fixes Hyper-V PLE stall"` (`d9402f2`)

---

## 3. LAPIC Init Timeout / Single-CPU Guard ✅ *(agent)*

> [!NOTE]
> **Root cause (2026-03-20):** `lapic_init()` sent an **Init Level De-Assert
> IPI** (ICR = `0x00088500`) and busy-waited for the delivery status bit to
> clear. On real hardware and under KVM, this completes instantly. Under WHPX,
> the delivery status bit sometimes **never clears**, causing an infinite hang.
>
> The Init Level De-Assert IPI is a legacy Pentium-era mechanism for
> synchronizing APIC arbitration priorities. It's a no-op on all post-P6
> CPUs (anything with an integrated APIC). On a single-CPU system, it's
> entirely pointless — there's nothing to de-assert.
>
> **Fix:** (1) Skip Init Level De-Assert entirely on single-CPU systems.
> (2) Add a 50,000-iteration timeout to the ICR delivery status busy-wait
> so it never hangs even on multi-CPU configs under WHPX. Commit `e08f805`.

- [x] Guard Init Level De-Assert with `if (cpu_count > 1)` check
- [x] Add `klog` breadcrumb: `"single CPU — skipping Init Level De-Assert"`
- [x] Add 50,000-iteration timeout to ICR delivery status wait in `lapic_init()`
- [x] Add similar timeout to `ipi_wait_delivery()` helper function
- [x] Log timeout as `LOG_WARN` (non-fatal — delivery is best-effort on modern CPUs)
- [x] Commit: `"lapic: timeout + single-CPU guard — fixes WHPX Init De-Assert hang"` (`e08f805`)

---

## 4. AHCI port_start_cmd Timeout ✅ *(agent)*

> [!NOTE]
> **Hardening (2026-03-20):** The `port_start_cmd()` function waited
> indefinitely for the Command Running (CR) bit to clear before starting the
> command engine. Under normal operation this clears in microseconds, but
> under WHPX with interrupt storms or MMIO delays, it could hang forever.
>
> **Fix:** Add a 500,000-iteration timeout to the CR wait loop. If it doesn't
> clear, proceed anyway — the AHCI spec says the CR bit MAY remain set if
> the port has pending commands, and starting ST with CR set is not
> catastrophic. Commit `e08f805`.

- [x] Add `uint32_t timeout = 500000` to `port_start_cmd()`
- [x] Change `while (CR set)` to `while (CR set && timeout--)`
- [x] Commit (with §3): `"lapic: timeout + single-CPU guard"` (`e08f805`)

---

## 5. QEMU Launch Script (PowerShell) ✅ *(agent)*

> [!NOTE]
> **Script improvements (2026-03-20):** The `run-qemu.ps1` PowerShell script
> had several issues that caused confusion and poor performance:
> - **Auto mode** passed no `-accel` flag, so QEMU silently fell back to TCG
> - **CPU model** was wrong: `qemu64` under WHPX causes feature warnings
> - **OVMF firmware** wasn't auto-copied from the WSL system path
>
> **Fix:** Explicit `-accel whpx -accel tcg` for auto mode (WHPX first,
> TCG fallback). Use `Haswell` CPU model for WHPX/auto mode. Auto-copy
> OVMF_CODE/VARS from `/usr/share/OVMF/` if not present in `build/`.

- [x] Auto mode: explicitly pass `-accel whpx -accel tcg`
- [x] CPU model: `Haswell` for WHPX/auto, `qemu64` for TCG
- [x] OVMF auto-copy: `wsl.exe -e bash -c "cp /usr/share/OVMF/..."` if missing
- [x] OVMF_VARS writable copy to `%TEMP%` (UEFI NVRAM needs write access)
- [x] Status lines show Accel, Timer, Disk, Device for quick diagnostics
- [x] `run-qemu-kvm.bat` calls `-Accel whpx` explicitly (not auto)
- [x] `run-qemu-tcg.bat` calls `-Accel tcg` explicitly
- [x] Commit: `"scripts: QEMU launch script improvements"` (multiple commits)

---

## 6. Hyper-V StorVSC Boot Order Fix ✅ *(agent)*

> [!NOTE]
> **Root cause (2026-03-20):** On Hyper-V Gen 2, the only disk is StorVSC
> (synthetic SCSI via VMBus). In `boot_storage.c`, the boot sequence was:
>
> ```
> L47:  partition_scan_all()          ← scans block devices (AHCI only)
> L49:  partition_mount_filesystems()  ← mounts C:\, X:\, etc.
>       ...
> L116: storvsc_init()                ← registers "hyperv0" ← TOO LATE!
> ```
>
> `storvsc_init()` registers its block device AFTER `partition_scan_all()`
> has already finished scanning. On Hyper-V Gen 2 (no AHCI), there are zero
> block devices at scan time → no partitions → no C:\ mount.
>
> **Fix:** After `storvsc_init()` succeeds, re-run `partition_scan_all()`
> and `partition_mount_filesystems()` to discover and mount the newly
> registered `hyperv0` device. Commit `b6101e1`.

- [x] Check `storvsc_init()` return value (0 = success, -1 = no disk)
- [x] On success: call `partition_scan_all()` to discover `hyperv0` partitions
- [x] On success: call `partition_mount_filesystems()` to mount IXFS → C:\
- [x] Add `boot_splash_status()` calls for Hyper-V scan/mount phases
- [x] Verify: TCG smoke test still boots (AHCI path unaffected)
- [x] Commit: `"boot: re-scan partitions after StorVSC init — fixes Hyper-V C:\ not mounted"` (`b6101e1`)

---

## Key Files

| File                                      | Change  | Purpose                                             |
| ----------------------------------------- | ------- | --------------------------------------------------- |
| `src/kernel/drivers/ahci.c`               | MODIFY  | Disabled interrupts, removed PAUSE, added timeouts  |
| `src/kernel/drivers/lapic.c`              | MODIFY  | Init Level De-Assert guard, ICR/IPI timeouts        |
| `src/kernel/main/boot_storage.c`          | MODIFY  | Re-scan partitions after StorVSC init               |
| `scripts/vm/run-qemu.ps1`                 | MODIFY  | WHPX acceleration, CPU model, OVMF auto-copy        |
| `scripts/vm/run-qemu-kvm.bat`             | MODIFY  | Explicit `-Accel whpx`                              |

---

## Gotchas & Lessons Learned

> [!CAUTION]
> **AHCI interrupts without a handler = silent interrupt storm.** If you enable
> `PxIE` or `GHC.IE` in the AHCI controller, you MUST register a PCI IRQ
> handler that clears `PxIS`. Otherwise, the level-triggered interrupt fires
> continuously. Under TCG this is masked by QEMU's emulation, but under WHPX
> and on real hardware it's catastrophic.

> [!CAUTION]
> **Never use `PAUSE` in MMIO polling loops.** Hyper-V's Pause Loop Exit (PLE)
> detector suspends the vCPU for ~100ms when it detects a PAUSE spin loop.
> Use `PAUSE` only in spinlocks (where PLE is beneficial). For MMIO polling,
> the MMIO read itself is the yield point.

> [!WARNING]
> **`kernel-irqchip=off` is broken on some QEMU/WHPX versions.** Despite being
> a documented WHPX fix, passing `-accel whpx,kernel-irqchip=off` fails with
> `hr=c0350005` on QEMU 8.x+ with Windows 11. Do not use.

> [!WARNING]
> **StorVSC block device registration happens AFTER partition scan.** Any new
> block device driver that registers via `blkdev_register()` during late boot
> (after `boot_hw_init()`) must trigger a re-scan of partitions and re-mount
> of filesystems. This pattern applies to any future hot-pluggable storage.

> [!NOTE]
> **Init Level De-Assert IPI is a no-op on modern CPUs.** Post-P6 CPUs with
> integrated APICs ignore this IPI entirely. It only matters on external 82489DX
> APICs (pre-Pentium). Safe to skip on single-CPU systems, safe to timeout on
> multi-CPU systems under WHPX.

---
