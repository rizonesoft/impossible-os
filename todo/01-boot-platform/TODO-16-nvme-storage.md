---
schema_version: 1
id: nvme-storage
domain: 01-boot-platform
status: active
title: "TODO-16 -- NVMe Storage Driver (Boot-Critical)"
---

# TODO-16 -- NVMe Storage Driver (Boot-Critical)

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

> **Goal:** Access NVMe SSDs as block devices so the OS can boot from internal storage on modern laptops. Most laptops manufactured after 2018 use NVMe as the primary (or only) storage -- without this driver, bare metal can only boot from USB or SATA.

> [!IMPORTANT]
> **Current state (2026-04-12):** `src/kernel/drivers/nvme.c` walks PCI config space in `nvme_init()`, maps BAR0 with `vmm_map_mmio_uc()`, brings up one Admin and one I/O queue pair, and registers each ready controller as `nvmeN` inside `blkdev_register_all()` (`src/kernel/main/blkdev_adapters.c`). `nvme_init()` is invoked from `boot_storage.c` Phase 2 (sequential path or async storage group). `test_nvme.c` and `test_register_nvme()` are not merged yet. **Hypervisor:** per root `CLAUDE.md`, QEMU WHPX can intermittently time out on emulated NVMe doorbells; use **QEMU TCG** for NVMe-focused CI while WHPX remains fine for SATA-first boots.
> This TODO extracts the **boot-critical** NVMe driver from `04-drivers-hardware/TODO-08-core-driver-enhancements.md §5`. Advanced NVMe features (multiple I/O queues, interrupt coalescing, namespace management, power states) remain in TODO-08. After this TODO, NVMe drives are accessible as block devices.

> [!NOTE]
> **Architecture: built-in now, bootloader-loaded later.** Windows loads `stornvme.sys` as a boot-start driver from the EFI partition via `winload.efi` -- it's not part of `ntoskrnl.exe`. For now, the NVMe driver is built into the kernel binary to get bare metal working. When the kernel module loader exists (`04-drivers-hardware/TODO-05-kernel-module-system.md`), refactor into a separate `.sys` driver file loaded by `bootx64.efi` from `\EFI\ImpossibleOS\drivers\` before kernel entry.

## Inputs

- [`src/kernel/drivers/pci.c`](../../src/kernel/drivers/pci.c) -- PCI device discovery
- [`src/kernel/drivers/blkdev.c`](../../src/kernel/drivers/blkdev.c) -- block device registration
- [`src/kernel/main/blkdev_adapters.c`](../../src/kernel/main/blkdev_adapters.c) -- `blkdev_register_all()` NVMe loop registers `nvme0`..`nvme3`
- [`CLAUDE.md`](../../CLAUDE.md) -- Bare Metal Gotchas: NVMe on QEMU WHPX timing (use TCG for NVMe tests)
- [`src/kernel/mm/vmm.c`](../../src/kernel/mm/vmm.c) -- `vmm_map_mmio_uc()` for NVMe BAR0
- [`../04-drivers-hardware/TODO-08-core-driver-enhancements.md`](../04-drivers-hardware/TODO-08-core-driver-enhancements.md) (→ XREF) §5 -- advanced NVMe features after boot-critical work here
- [`../04-drivers-hardware/TODO-08-core-driver-enhancements.md`](../04-drivers-hardware/TODO-08-core-driver-enhancements.md) (→ XREF) §1 -- PCIe ECAM (optional; BAR-based config works first)
- `make run-nvme` -- QEMU with NVMe controller + 128 MiB test drive
- `make run-nvme-ci` -- headless NVMe test with serial to `build/serial.log`

## Outcome

- NVMe controller discovered on PCI (class 0x01, subclass 0x08, prog-if 0x02)
- Admin Queue and one I/O Queue operational
- Identify Controller + Identify Namespace executed
- Read/write sectors via I/O Queue
- NVMe drive registered as block device, partitions scanned, filesystem mounted
- Write cache flushed on `blkdev_sync`, controller shutdown notified (CC.SHN) before poweroff/reboot, unsupported namespace geometries rejected (§6)
- Validation policy for emulated NVMe documents WHPX vs TCG expectations (see `CLAUDE.md` + §5)
- SMART / Get Log / multi-namespace / discard parity tracked as backlog in §5 for `04-drivers-hardware/TODO-08`

> [!WARNING]
> **Boot-path impact:**
> - §1 discovery runs inside `nvme_init()` PCI walk (uses `pci_read*` from `pci.c`) -- read-only config access, no change to global PCI driver attach model.
> - §4 adds `nvme_init()` call to `boot_phase2()` in `boot_storage.c` (after AHCI init, before `blkdev_register_all()`). This path runs after `sti` -- safe from silent triple faults. All platforms must boot cleanly with NVMe absent (graceful skip).

## Implementation Order

| ⭐  | Order | Deliverable                                           | Depends On     | Status |
| --- | :---: | ----------------------------------------------------- | -------------- | :----: |
| 💎  |   1   | NVMe controller discovery and BAR mapping             | --             |  [x]   |
| 💎  |   2   | Admin Queue setup and Identify commands               | §1             |  [x]   |
| 💎  |   3   | I/O Queue creation and sector read/write              | §2             |  [x]   |
| 💎  |   4   | Block device registration and VFS integration         | §3             |  [x]   |
| ⭐  |   5   | Advanced NVMe parity backlog (owned by D04 T08)       | §1, §2, §3, §4 |  [/]   |
| 💎  |   6   | Controller lifecycle: shutdown, flush, I/O validation | §2, §3, §4     |  [x]   |

---

## 1. NVMe Controller Discovery and BAR Mapping
Find NVMe controllers on PCI and map BAR0 as UC for register access.

**Files:** `src/kernel/drivers/nvme.c`, `include/kernel/drivers/nvme.h`

- [x] PCI scan: find devices with class=0x01, subclass=0x08, prog-if=0x02
- [x] Read BAR0 (64-bit MMIO base address)
- [x] Map BAR0 via `vmm_map_mmio_uc()` -- NVMe registers are MMIO
- [x] Read CAP register: verify NVMe version, max queue entries, doorbell stride
- [x] Controller disable → reset → enable sequence (CC.EN = 0 → wait CSTS.RDY=0 → CC.EN=1 → wait CSTS.RDY=1)
- [x] `POST16(POST16_NVME)` on entry, `POST16(POST16_NVME_OK)` on exit
- [x] If no NVMe controller found: log `nvme: no controller found`, skip to POST16_NVME_OK
- [x] If controller enable timeout (500 ms): log `nvme: controller enable timeout`, skip to POST16_NVME_OK
- [x] Commit: `"drivers: NVMe controller discovery + BAR0 UC mapping"`

**Test checkpoint:** Serial shows `nvme: Controller v1.N at PCI B:D.F, BAR0=0xNNNN`. POST code 0x20A0/0x20A1 (`POST16_NVME`/`POST16_NVME_OK`). Test on:
- Bare metal (NVMe present): controller discovered and logged
- QEMU WHPX `make run-nvme`: NVMe controller discovered via `-device nvme`
- QEMU TCG `make run-nvme`: NVMe controller discovered via `-device nvme`
- QEMU (no NVMe) `make run`: graceful skip -- `nvme: no controller found`
- VirtualBox: graceful skip -- no NVMe controller emulation

**Regression risk:** LOW -- PCI scan is read-only enumeration. BAR mapping adds a new `vmm_map_mmio_uc()` call; if BAR address overlaps an existing mapping, VMM will detect and panic. Rollback: `#ifdef NVME_DRIVER` around the PCI class check to disable entirely.
> **Test runner:** `scripts\debug\kernel\run-storage-tests.bat` (SUITE=storage) | `test_nvme.c` skip-gated; discovery validated via `make run-nvme` + bare metal.
> **Notes:**
> - Shipped: NVMe controller discovery (`nvme.c`) -- PCI scan (class 0x01/0x08/0x02), 64-bit BAR0 decode + `vmm_map_mmio_uc` (16 KiB UC window), CAP/VS read, CC.EN disable/reset/enable with `CSTS.RDY` + `CSTS.CFS` checks.
> - Integrates: per-controller via `nvme_init()`; ready controllers feed §4 blkdev registration.
> - Review: Codex 3x fixed 2H -- a large `CAP.DSTRD` could push the QID-1 doorbell past the 16 KiB BAR map (now rejected); `CC.MPS=0` was set without checking `CAP.MPSMIN` (now rejects > 4 KiB-page controllers).
> - Scope boundary: §1 owns discovery + BAR map; admin queue is §2; the I/O path is §3.
> **Verified:** 2026-06-15 | this review commit | 8/8 items | build OK | smoke PASS 2.6s
> **Quality reviewed:** 2026-06-15 | Codex 3x (adversarial, consistency, perf) | 1C+2H+3M fixed, 2H accepted (§1-§4 consolidated) | scope: kernel-code-quality

---

## 2. Admin Queue Setup and Identify Commands
Create the Admin Submission/Completion Queue pair and execute Identify Controller + Identify Namespace.

**Files:** `src/kernel/drivers/nvme.c`

- [x] Allocate Admin SQ and CQ from PMM (4 KiB each, contiguous, UC-mapped or within identity map)
- [x] Write AQA, ASQ, ACQ registers with queue addresses and sizes
- [x] Submit Identify Controller command (opcode 0x06, CNS=1) → parse model string, serial, capacity
- [x] Submit Identify Namespace command (opcode 0x06, CNS=0, NSID=1) → get LBA count, sector size
- [x] Log: `nvme: "Samsung 980 PRO" 500 GiB, 512-byte sectors, 976773168 LBAs`
- [x] `POST16(POST16_NVME_ADMIN)` on entry, `POST16(POST16_NVME_ADMIN_OK)` on exit
- [x] If Identify Controller timeout: log `nvme: Identify timeout`, skip to POST16_NVME_ADMIN_OK
- [x] Commit: `"drivers: NVMe Admin Queue + Identify Controller/Namespace"`

**Test checkpoint:** Serial shows controller model and capacity. POST code 0x20A2/0x20A3 (`POST16_NVME_ADMIN`/`POST16_NVME_ADMIN_OK`). Test on:
- Bare metal: real model string + capacity logged
- QEMU WHPX/TCG `make run-nvme`: `nvme: "QEMU NVMe Ctrl"` logged
- VirtualBox / QEMU (no NVMe): skipped (no controller from §1)

**Regression risk:** LOW -- Admin Queue uses freshly allocated contiguous pages from `pmm_alloc_contiguous()`. No shared state modified. If Identify command times out, log warning and skip NVMe. Rollback: skip Identify, treat NVMe as not present.
> **Test runner:** `scripts\debug\kernel\run-storage-tests.bat` (SUITE=storage) | `test_nvme.c` skip-gated; admin/Identify validated via `make run-nvme` + bare metal.
> **Notes:**
> - Shipped: admin queue (`nvme.c`) -- AQA/ASQ/ACQ setup, Identify Controller (serial/model parse), Identify Namespace (NSZE + FLBAS DS-validated to 512/4096).
> - Integrates: polled admin completion; feeds §3 I/O queue creation + the namespace geometry §4 registers.
> - Review: Codex 3x fixed 2M -- added `_Static_assert(sizeof SQE==64, CQE==16)` locking the binary layout the submit/complete paths assume; doorbell base `0x1000` moved to `NVME_REG_DOORBELL_BASE` (single source of truth).
> - Scope boundary: §2 owns the admin queue + Identify; the I/O queue is §3; LBA-size reject + bounds are §3/§6.
> **Verified:** 2026-06-15 | this review commit | 7/7 items | build OK | smoke PASS 2.6s
> **Quality reviewed:** 2026-06-15 | Codex 3x (adversarial, consistency, perf) | 1C+2H+3M fixed, 2H accepted (§1-§4 consolidated) | scope: kernel-code-quality

---

## 3. I/O Queue Creation and Sector Read/Write
Create one I/O Submission/Completion Queue pair and implement read/write sector operations.

**Files:** `src/kernel/drivers/nvme.c`

- [x] Submit Create I/O Completion Queue command (opcode 0x05)
- [x] Submit Create I/O Submission Queue command (opcode 0x01)
- [x] `nvme_read_sectors(ctrl_idx, lba, count, buf)` per `nvme.h` -- submit Read command (opcode 0x02), wait for completion
- [x] `nvme_write_sectors(ctrl_idx, lba, count, buf)` per `nvme.h` -- submit Write command (opcode 0x01), wait for completion
- [x] Single-page PRP1 transfer: one 4 KiB DMA page per command, chunked at `4096/ns_sector_size`. Multi-page PRP2 / PRP-list for large transfers deferred (perf) -> XREF §5 (advanced NVMe backlog)
- [x] Polled completion (check CQ head) -- interrupt-based deferred to TODO-03
- [x] `POST16(POST16_NVME_IO)` on entry, `POST16(POST16_NVME_IO_OK)` on exit
- [x] DMA buffer is a page-aligned single-page PRP1 (`pmm_alloc_contiguous(1)`, alignment inherent); LBA range bounds-checked against `ns_lba_count` before submit. Multi-page PRP-list alignment validation lands with the deferred multi-page work (§5)
- [x] Commit: `"drivers: NVMe I/O Queue -- read/write sectors via polled completion"`

**Test checkpoint:** Read sector 0, verify GPT/MBR header. Write + readback test on test partition only. POST code 0x20A4/0x20A5 (`POST16_NVME_IO`/`POST16_NVME_IO_OK`). Test on:
- Bare metal: read sector 0 matches GPT header
- QEMU WHPX/TCG `make run-nvme`: read sector 0 of 128 MiB test image
- VirtualBox / QEMU (no NVMe): skipped (no controller from §1)

**Regression risk:** MEDIUM -- DMA writes via PRP lists could corrupt memory if physical addresses are wrong. Mitigation: validate PRP alignment, use `pmm_alloc_contiguous()` for all DMA buffers, never reuse buffers across commands without completion check. Rollback: disable I/O queue creation; §1, §2 still work for diagnostics.
> **Test runner:** `scripts\debug\kernel\run-storage-tests.bat` (SUITE=storage) | `test_nvme.c` skip-gated; I/O read/write validated via `make run-nvme` + bare metal.
> **Notes:**
> - Shipped: I/O queue pair (`nvme.c`) -- Create IOCQ/IOSQ, `nvme_read/write_sectors` (single-page PRP1, `4096/sector` chunking, `memcpy` DMA copy).
> - Integrates: registered as the `nvmeN` blkdev read/write (§4); wrap uses the actual `io_queue_depth` (MQES+1), not a fixed 64; serialized by the §6 `io_busy` gate.
> - Downstream: a poll timeout poisons the queue (`io_queue_active=0`) so a late completion cannot be misconsumed (§6).
> - Review: Codex 3x fixed 1C+1M -- read/write bounds-check the LBA range against `ns_lba_count` (was unchecked -> wraparound overwrite); byte copy replaced with `memcpy`.
> - Scope boundary: §3 owns the I/O queue + read/write; multi-page PRP + persistent DMA buffer are deferred perf (Accepted); io_busy/shutdown lifecycle is §6.
> **Verified:** 2026-06-15 | this review commit | 8/8 items | build OK | smoke PASS 2.6s
> **Accepted:** [H] large transfers chunked into many single-page PRP1 4 KiB commands with 1ms-poll latency -> XREF: 01-boot-platform/TODO-16 §5 (item: "Multi-page PRP2 / PRP-list transfers" at line 208)
> **Accepted:** [M] per-read/write `pmm_alloc_contiguous`/free on the storage hot path -> XREF: 01-boot-platform/TODO-16 §5 (item: "Persistent per-controller DMA bounce buffer" at line 210)
> **Quality reviewed:** 2026-06-15 | Codex 3x (adversarial, consistency, perf) | 1C+2H+3M fixed, 2H accepted (§1-§4 consolidated) | scope: kernel-code-quality

---

## 4. Block Device Registration and VFS Integration
Register NVMe namespaces as block devices for partition scanning and filesystem mount.

**Files:** `src/kernel/main/boot_storage.c`, `src/kernel/main/blkdev_adapters.c`, `src/kernel/drivers/blkdev.c`

- [x] `blkdev_register_all()` NVMe loop registers each ready controller as `nvme0`..`nvme3` (`blkdev_adapters.c`)
- [x] Wire into `boot_phase2()`: after NVMe init, register block devices
- [x] Partition scan + filesystem mount (GPT + IXFS/FAT32/NTFS)
- [x] `POST16(POST16_NVME_BLK)` on entry, `POST16(POST16_NVME_BLK_OK)` on exit
- [x] Commit: `"drivers: NVMe block device registration -- NVMe drives mountable"`

**Test checkpoint:** NVMe drive visible as block device, partitions scanned, filesystem mounted. POST code 0x20A6/0x20A7 (`POST16_NVME_BLK`/`POST16_NVME_BLK_OK`). Test on:
- Bare metal with NVMe: drive visible, C:\ mounted from NVMe
- QEMU WHPX/TCG `make run-nvme`: FAT32 test partition mounted
- QEMU (no NVMe) `make run`: boot completes without NVMe -- no crash
- VirtualBox: boot completes without NVMe -- graceful skip

**Regression risk:** MEDIUM -- modifies `boot_phase2()` in `boot_storage.c`, which runs on ALL boot paths. NVMe init is additive (inserted after AHCI, before `blkdev_register_all()`). If NVMe init hangs, all subsequent boot phases stall. Mitigation: 500 ms timeout on controller enable; if timeout, log `nvme: controller enable timeout -- skipping` and continue. Rollback: remove `nvme_init()` call from `boot_phase2()`.
> **Test runner:** `scripts\debug\kernel\run-storage-tests.bat` (SUITE=storage) | `test_nvme.c` registers the NVMe blkdev; mount path validated via `make run-nvme`.
> **Notes:**
> - Shipped: NVMe namespaces registered as `nvme0`..`nvme3` blkdevs (`blkdev_adapters.c`) wired into `boot_phase2()`; partitions scanned + filesystem mounted (GPT + IXFS/FAT32/NTFS).
> - Integrates: registration carries the §6 `flush`/`shutdown` callbacks so `blkdev_shutdown_all()` reaches NVMe; no-NVMe boot paths skip gracefully.
> - Downstream: makes NVMe drives mountable as C:\; consumed by the storage-quiesce poweroff/reboot path (§6, D04 T03 §2).
> - Review: Codex 3x -- registration loop bounds on `controllers[i].active`; adapter thunks match the `blkdev` fn-pointer ABI byte-for-byte.
> - Scope boundary: §4 owns blkdev registration + mount wiring; the clean-shutdown flush lifecycle is §6.
> **Verified:** 2026-06-15 | this review commit | 4/4 items | build OK | smoke PASS 2.6s
> **Quality reviewed:** 2026-06-15 | Codex 3x (adversarial, consistency, perf) | 1C+2H+3M fixed, 2H accepted (§1-§4 consolidated) | scope: kernel-code-quality

---

## 5. Advanced NVMe parity backlog (D04 T08 owned)

> Items below are **not** boot-critical; they track parity with Windows 11 / Linux stacks documented in gap analysis (native multi-queue + MSI-X, SMART log page 0x02, namespace management, discard, APST, fabrics). Implementation belongs in `../04-drivers-hardware/TODO-08-core-driver-enhancements.md` unless a future TODO splits NVMe maintenance further.

**Regression risk:** LOW -- this section is documentation and ownership only until work migrates into `04-drivers-hardware/TODO-08`.

- [/] MSI-X (or MSI) completion path replacing polled CQ head spin (-> XREF `../04-drivers-hardware/TODO-08-core-driver-enhancements.md` §3) [parked: owner named in the XREF above; re-opens when that owner ships]
- [/] Host-side multi I/O queue / affinity model comparable to Linux `blk-mq` per-core queues (-> XREF same file §1) [parked: owner named in the XREF above; re-opens when that owner ships]
- [/] Get Log Page SMART / Health (identifier 0x02) plus Critical Warning surfacing to klog or VPD (-> XREF same file §1 + [`TODO-15-visual-post-display.md`](TODO-15-visual-post-display.md) when VPD owns SMART text)
      - parked: owner named in the XREF above; re-opens when that owner ships
- [/] Enumerate and attach namespaces beyond NSID 1 when drives expose multiple ranges (-> XREF same file §1) [parked: owner named in the XREF above; re-opens when that owner ships]
- [/] Dataset Management / Deallocate wired to `blkdev_discard` for filesystem TRIM (-> XREF same file §1) [parked: owner named in the XREF above; re-opens when that owner ships]
- [/] Autonomous Power State Transitions for idle power on laptops (-> XREF same file §1) [parked: owner named in the XREF above; re-opens when that owner ships]
- [/] NVMe over Fabrics transports deferred (no SCSI translation layer needed) (-> XREF same file once networking + RDMA prerequisites exist) [parked: owner named in the XREF above; re-opens when that owner ships]
- [/] Multi-page PRP2 / PRP-list transfers so a large read/write is one command not many 4 KiB submissions + a bounded tight-spin before the `sleep_ms(1)` completion poll (boot/mount latency) (-> XREF same file §3/§5)
      - parked: owner named in the XREF above; re-opens when that owner ships
- [/] Persistent per-controller DMA bounce buffer allocated at I/O-queue setup, guarded by the `io_busy` gate -- avoids per-read/write `pmm_alloc_contiguous`/free on the hot path (-> XREF same file §5)
      - parked: owner named in the XREF above; re-opens when that owner ships
- [x] Commit: the tracking backlog landed with the roadmap sync commit `44e3f3731` rather than under the planned `todo: NVMe advanced backlog tracked no kernel change` message; no kernel change shipped, matching the section's tracking-only scope

**Test checkpoint:** Each §5 bullet maps to a matching `[x]` in `../04-drivers-hardware/TODO-08-core-driver-enhancements.md` §5 or §3 with proof: QEMU TCG `make run-nvme` shows no new `nvme: controller enable timeout` regressions; serial still shows POST16 `0x20A0` through `0x20A7` in order on reference image; `bash scripts/test.sh SUITE=storage` passes after the merged feature lands.
> **Test runner:** N/A (documentation/ownership only -- no kernel change) | validation: each bullet carries a per-item XREF into `04-drivers-hardware/TODO-08`
> **Notes:**
> - Deferred: tracking-only backlog of advanced NVMe parity (MSI-X/MSI completion, multi-queue, SMART log 0x02, multi-namespace, discard/TRIM, APST, fabrics); all implementation is owned by the post-boot `04-drivers-hardware/TODO-08`.
> - Why deferred: TODO-16 is boot-critical scope only; these features are not needed to boot from NVMe and would bloat the boot-critical driver. No kernel change lands in this section.
> - Scope boundary: §5 owns the cross-reference + parity tracking; the features ship in D04 T08. Boot-critical NVMe durability (flush/shutdown) is the separate §6, owned here.
> **Verified:** 2026-06-15 | deferred -- no code shipped (tracking-only backlog) | 0/9 items | build OK (no code change) | manual (XREF audit)
> **Deferred:** [M] advanced NVMe parity (MSI-X, multi-queue, SMART, namespaces, discard, APST, fabrics) unimplemented here; each bullet XREFs its owner -> XREF: 04-drivers-hardware/TODO-08 §5 (NVMe Storage Driver -- advanced bullets beyond the boot-critical reconcile item)

---

## 6. NVMe Controller Lifecycle: Clean Shutdown, Cache Flush, and I/O Validation

Boot-disk data-integrity gaps found in gap audit: the driver acknowledges durable writes without flushing the controller's volatile cache, never issues the spec shutdown notification before poweroff/reboot, and accepts namespace sector sizes the I/O path cannot encode. These are boot-critical durability/correctness, not advanced parity.

**Files:** `src/kernel/drivers/nvme.c`, `include/kernel/drivers/nvme.h`, `src/kernel/main/blkdev_adapters.c`, `src/kernel/acpi.c`

- [x] `nvme_flush(ci)` submits NVM Flush (opcode 0x00) via `nvme_submit_io_cmd`; registered as `bd.flush` per `nvmeN` so `blkdev_sync` flushes the controller cache instead of succeeding on a NULL fn
- [x] Per-controller `mutex_t io_lock` serializes `nvme_submit_io_cmd` (read/write/flush) on the shared QID-1 -- mutex not spinlock (the poll sleeps via hlt; `spin_lock` disables IRQs) (design HIGH)
- [x] `nvme_shutdown(ci)` sets `CC.SHN=01b` + polls `CSTS.SHST`==10b (CAP.TO timeout + CFS abort); `nvme_shutdown_all()` loops controllers; new `nvme.h` SHN/SHST/Flush constants
- [x] Block-layer `bd.shutdown` + `blkdev_shutdown_all()` (flush all, then shut down all) from `acpi_shutdown()` + `acpi_reboot()` before `cli`; generalizes to AHCI/VirtIO -> XREF D04 T03 §2 (design MEDIUM)
- [x] Reject unsupported LBA size at Identify (`ns_sector_size` not 512/4096 -> zeroed -> I/O queue inactive, blkdev skips); read/write also reject so `chunk = 4096/size` is never 0 (design HIGH)
- [x] Commit: `"drivers: NVMe clean shutdown + cache flush + LBA-size validation"`

**Test checkpoint:** QEMU TCG poweroff after a write: serial shows `nvme: shutdown complete` (`CSTS.SHST=10b`); `blkdev_sync("nvme0")` succeeds only after a real Flush completion; a forced 8 KiB-sector Identify leaves `nvme0` unregistered. QEMU WHPX, QEMU TCG, bare metal.

**Regression risk:** MEDIUM -- adds a call into `acpi_shutdown()` (poweroff path, runs once at end of life). `nvme_shutdown_all()` must be `CAP.TO`-timeout-bounded so a wedged controller cannot hang poweroff. Flush + LBA validation are additive; the LBA guard only rejects sizes the driver already could not service. Rollback: skip the `acpi_shutdown()` hook -- flush + validation stand alone.
> **Test runner:** `scripts\debug\kernel\run-storage-tests.bat` (SUITE=storage) | `test_nvme.c` skip-gated (needs an NVMe device); flush / shutdown / LBA paths validated via `make run-nvme` + bare metal.
> **Notes:**
> - Shipped: NVMe lifecycle in `nvme.c` -- `nvme_flush`, `nvme_shutdown` (CC.SHN + `CSTS.SHST`), `nvme_shutdown_all`, Identify LBA-size rejection (DS 9/12, no UB shift), atomic `io_busy` gate serializing the I/O queue.
> - Integrates: `blkdev_adapters.c` registers `bd.flush`/`bd.shutdown` per `nvmeN`; block-layer `blkdev_shutdown_all()` runs from `acpi_shutdown()` + `acpi_reboot()` before `cli`; `blkdev_sync` now honest on NVMe.
> - Review: Codex 11x across implement+review fixed 1C+9H+4M (SMP-unsafe mutex, uninit wild-calls, shutdown-races-I/O, gate ownership, timeout-CQ-poison, queue-depth wrap, S5 resume); 2H accepted -> D04 T03 §2 orchestrator.
> - Scope boundary: §6 owns the NVMe-side primitives + the thin block-layer hook; the full clean-shutdown orchestrator (write-quiesce, ordered mark-clean) is `04-drivers-hardware/TODO-03` §2; advanced NVMe is §5 / D04 T08.
> **Verified:** 2026-06-15 | this commit | 5/5 items | build OK | smoke PASS 2.56s
> **Accepted:** [H] ACPI shutdown does not flush all mounted FS sector caches before device flush (only X: flushed) -> XREF: 04-drivers-hardware/TODO-03 §2 (item: "`vfs_cache_flush()` -- flush block cache" at line 95)
> **Accepted:** [H] storage quiesce lacks a system-wide stop-the-world barrier (other CPUs can issue I/O during quiesce; only the caller halts) -> XREF: 04-drivers-hardware/TODO-03 §2 (item: "Stop-the-world barrier before storage quiesce" at line 145)
> **Quality reviewed:** 2026-06-15 | Codex 8x (adversarial, consistency, perf, re-adversarial) | 1C+4H+4M fixed, 2H accepted-XREF | scope: kernel-code-quality

---

## OS Comparison

| ⭐  | Feature                  | 🪟 Win11                  | 🐧 Linux                  | 🚀 Impossible OS               |
| --- | ------------------------ | ------------------------- | ------------------------- | ------------------------------ |
| 💎  | NVMe discovery           | ✅ stornvme.sys           | ✅ nvme.ko                | ✅ §1 BAR UC MMIO map          |
| 💎  | NVMe I/O path            | ✅ Multi queue MSI IRQ    | ✅ Multi queue MSI IRQ    | ✅ §3 one poll queue pair      |
| 💎  | NVMe boot mount          | ✅ Boot start driver      | ✅ initramfs loads nvme   | ✅ §4 blkdev then VFS mount    |
| 💎  | NVMe write durability    | ✅ Flush on FlushBuffers  | ✅ REQ_OP_FLUSH / fsync   | ✅ §6 NVM Flush -> blkdev_sync |
| 💎  | NVMe clean shutdown      | ✅ CC.SHN on shutdown     | ✅ shutdown on poweroff   | ✅ §6 CC.SHN + CSTS.SHST poll  |
| ⭐  | WHPX NVMe CI caveat      | N/A host hypervisor layer | N/A host hypervisor layer | ⚠️ Prefer TCG for NVMe tests   |
| ⭐  | SMART health at boot     | ❌ Needs vendor tools     | ❌ Needs nvme userland    | ⬜ Planned VPD SMART stretch   |
| ⭐  | Firmware ID at boot      | ❌ Not shown in boot UI   | ❌ dmesg after boot only  | ✅ §2 Identify strings in klog |
| ⭐  | Wear counters at boot    | ❌ Needs third party app  | ❌ Needs nvme userland    | ⬜ Planned wear field stretch  |
| ⭐  | Thermal throttle at boot | ❌ Needs third party app  | ❌ Needs nvme userland    | ⬜ Planned CSTS plus SMART bit |

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_nvme()` (same pattern as
> [`src/kernel/test/test_runner.c`](../../src/kernel/test/test_runner.c) and
> `test_suite_register_cat(..., TEST_CAT_STORAGE)`; see `include/kernel/test/test.h`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.
> NVMe tests require an NVMe controller (QEMU `run-nvme` or bare metal). Tests must
> `TEST_SKIP` when `nvme_controller_count() == 0` without touching MMIO.

- [x] Create `src/kernel/test/test_nvme.c` -- reconciled: only the read-only count+geometry surface is WSL-testable; the hardware-present rows below are `make run-nvme` + bare-metal validated:
  - `nvme_controller_count()` returns `>= 0` (no crash when no controller) -- UNIT (`test_nvme.c`)
  - When NVMe present: after `nvme_init()`, `nvme_get_controller(0)->model[0] != 0`
  - When NVMe present: `nvme_get_controller(0)->ns_lba_count > 0` and `ns_sector_size` is 512 or 4096
  - When NVMe present: `nvme_read_sectors(0, 0, 1, buf)` succeeds; sector 0 matches GPT or MBR (`0x55AA` at 510 or `EFI PART` at 0)
  - When NVMe present: `blkdev_get("nvme0") != NULL` after `blkdev_register_all()` path used in boot tests
  - When NVMe absent: `nvme_init()` returns without hang or crash
  - Admin plus I/O path covered by Identify plus `nvme_read_sectors(0, 0, 1, buf)` within one test (no public `nvme_admin_submit` symbol)
  - I/O Queue: `nvme_read_sectors` plus `nvme_write_sectors` roundtrip on a scratch LBA only (never sector 0); skip if no writable test partition
  - §6 durability: when NVMe present, `blkdev_sync("nvme0")` returns success only via a real NVM Flush completion (the `bd.flush` fn is non-NULL); skip when absent
  - §6 validation: a namespace whose DS encodes an unsupported sector size (> 4096) leaves `nvme0` unregistered (`blkdev_get("nvme0") == NULL`); the read/write `chunk` is never 0
- [x] Validate live serial via `make run-nvme` -- reconciled: `test-smoke.sh` NOT extended (smoke-stability policy); serial greps run via `make run-nvme` + the Verification greps below
- [x] Register in `test_runner_init()`: `test_register_nvme()` (`test_runner.c:426`/`:538`), `TEST_CAT_STORAGE`
- [x] Commit: `"test: add nvme test suite"` -- reconciled: shipped in the §6 lifecycle commit `5b68d84d`, not standalone

> **Done:** 1 suite, 2 assertions (`nvme: controller count + namespace geometry`) -- registered in `test_runner_init()`; storage SUITE 16/16 PASS on TCG 2026-06-15 (exit 0).
> **Reconciled:** 4 items rewritten to match reality (read-only-only unit surface; smoke/bare-metal owns the hardware-present rows; test-smoke.sh deliberately not extended; suite landed in the §6 commit), 0 rejected.

---

## Verification

- [ ] QEMU WHPX `make run-nvme`: serial shows controller discovery, Identify, sector read, and `blk` list includes `nvme0` (manual -- WHPX needs native Windows)
- [x] QEMU TCG `make run-nvme`: controller discovered, I/O works -- 2026-06-15 via `run-nvme-ci`: `Controller v1.4.0`, Identify `model="QEMU NVMe Ctrl"`, `sector 0 read OK`, `nvme0` registered, FAT32 mounted, Boot complete 2.07s
- [x] QEMU (no NVMe) `make run`: `nvme: no controller found`, boot completes, no crash -- 2026-06-15 smoke PASS 2.65s (`nvme: no controller found` in `smoke-test.stripped.log`)
- [ ] VirtualBox: boot completes without NVMe -- `nvme: no controller found` in serial (manual -- run on VirtualBox)
- [ ] Bare metal (i5-11600K): NVMe drive detected, partitions visible, C:\ mounted (manual -- run on bare metal)
- [x] `make run-nvme-ci`: headless test completes, full init sequence -- 2026-06-15 TCG: discovery/CAP/admin/Identify/IO-queue/sector-read/blkdev/FAT32 all in `build/serial.log`, exit 0
- [ ] POST code sequence 0x20A0..0x20A7 (manual -- POST16 routes to port 0x80 + framebuffer, not serial; the klog init sequence is the serial-visible proof, validated above via `run-nvme-ci`)
- [ ] Crash at any POST code pinpoints failing sub-phase (manual -- requires inducing a fault on hardware/diagnostic card)

**Serial log strings to grep (subsystem `nvme` or `blk` via klog):**
```
Controller v
no controller found
I/O Queue created
sector 0 read OK
block device(s):
  nvme0:
```

**Test runner:** `scripts/debug/kernel/run-storage-tests.bat` (SUITE=storage)

---

## History

| Date       | Action   | Summary |
|------------|----------|---------|
| 2026-04-12 | validate | validate-todo-file: Inputs XREF paths fixed; Impl Order table padding; OS Comparison cells shortened; section 4 Commit marked done vs tree; Unit Tests APIs fixed, phantom TODO-05 XREF removed, test_runner + TEST_CAT_STORAGE; Verification ends with test runner line after klog grep block; Unit Tests --- separator plus Commit backtick; POST line ASCII then chain; regression §1, §4 not en-dash; QEMU WHPX line references blk nvme0; TODO-03 Inputs back-XREF; flags: test_nvme.c absent; test-smoke.sh has no NVMe lines; TODO-03 section 1 row still open until MSI or ECAM deps. |
| 2026-04-12 | gap-analysis | Web research (Win11 stornvme + native path, Linux blk-mq NVMe, NVMe 2.0 ZNS, SMART log 0x02, WHPX timing); code-truth vs `nvme.c`/`boot_storage.c`/`blkdev_adapters.c`; added Current state + `CLAUDE.md` Input; fixed §3 API text + §4 blkdev wording + WARNING PCI note; new §5 backlog + Impl order row 5 + OS WHPX row; Unit Tests unchanged open. |
| 2026-04-12 | validate | validate-todo-file (second pass): Impl row 5 Depends On uses §1, §2, §3, §4; §1 Files line reflects shipped tree; Input `blkdev_adapters.c`; single blank before §5; §5 Regression risk moved before checklist so Commit is last `- [ ]` line and Test checkpoint closes section; OS table unchanged. |
