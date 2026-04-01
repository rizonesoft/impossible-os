# TODO-08 — NVMe Storage Driver (Boot-Critical)

> **Goal:** Access NVMe SSDs as block devices so the OS can boot from internal storage on modern laptops. Most laptops manufactured after 2018 use NVMe as the primary (or only) storage — without this driver, bare metal can only boot from USB or SATA.

> [!IMPORTANT]
> This TODO extracts the **boot-critical** NVMe driver from `04-drivers-hardware/TODO-02-core-driver-enhancements.md §1`. Advanced NVMe features (multiple I/O queues, interrupt coalescing, namespace management, power states) remain in TODO-02. After this TODO, NVMe drives are accessible as block devices.

> [!NOTE]
> **Architecture: built-in now, bootloader-loaded later.** Windows loads `stornvme.sys` as a boot-start driver from the EFI partition via `winload.efi` — it's not part of `ntoskrnl.exe`. For now, the NVMe driver is built into the kernel binary to get bare metal working. When the kernel module loader exists (`04-drivers-hardware/TODO-01`), refactor into a separate `.sys` driver file loaded by `bootx64.efi` from `\EFI\ImpossibleOS\drivers\` before kernel entry.

## Inputs

- [`src/kernel/drivers/pci.c`](../../src/kernel/drivers/pci.c) — PCI device discovery
- [`src/kernel/drivers/blkdev.c`](../../src/kernel/drivers/blkdev.c) — block device registration
- [`src/kernel/mm/vmm.c`](../../src/kernel/mm/vmm.c) — `vmm_map_mmio_uc()` for NVMe BAR0
- → XREF: `04-drivers-hardware/TODO-02-core-driver-enhancements.md §1` — full NVMe driver (this TODO implements the minimal boot subset)
- → XREF: `04-drivers-hardware/TODO-02-core-driver-enhancements.md §3` — PCIe ECAM (optional, BAR-based config works first)
- `make run-nvme` — QEMU with NVMe controller + 128 MiB test drive
- `make run-nvme-ci` — headless NVMe test with serial to `build/serial.log`

## Outcome

- NVMe controller discovered on PCI (class 0x01, subclass 0x08, prog-if 0x02)
- Admin Queue and one I/O Queue operational
- Identify Controller + Identify Namespace executed
- Read/write sectors via I/O Queue
- NVMe drive registered as block device, partitions scanned, filesystem mounted

> [!WARNING]
> **Boot-path impact:**
> - §1 adds NVMe class match to PCI scan loop (`pci.c`) — read-only enumeration, no existing driver affected.
> - §4 adds `nvme_init()` call to `boot_phase2()` in `boot_storage.c` (after AHCI init, before `blkdev_register_all()`). This path runs after `sti` — safe from silent triple faults. All platforms must boot cleanly with NVMe absent (graceful skip).

## Implementation Order

| ⭐  | Order | Deliverable                                    | Depends On | Status |
| --- | :---: | ---------------------------------------------- | ---------- | :----: |
| 💎  |   1   | NVMe controller discovery and BAR mapping      | —          |  [x]   |
| 💎  |   2   | Admin Queue setup and Identify commands        | §1         |  [x]   |
| 💎  |   3   | I/O Queue creation and sector read/write       | §2         |  [x]   |
| 💎  |   4   | Block device registration and VFS integration  | §3         |  [x]   |

---

## 1. NVMe Controller Discovery and BAR Mapping
Find NVMe controllers on PCI and map BAR0 as UC for register access.

**Files:** `src/kernel/drivers/nvme.c` (new), `include/kernel/drivers/nvme.h` (new)

- [x] PCI scan: find devices with class=0x01, subclass=0x08, prog-if=0x02
- [x] Read BAR0 (64-bit MMIO base address)
- [x] Map BAR0 via `vmm_map_mmio_uc()` — NVMe registers are MMIO
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
- QEMU (no NVMe) `make run`: graceful skip — `nvme: no controller found`
- VirtualBox: graceful skip — no NVMe controller emulation

**Regression risk:** LOW — PCI scan is read-only enumeration. BAR mapping adds a new `vmm_map_mmio_uc()` call; if BAR address overlaps an existing mapping, VMM will detect and panic. Rollback: `#ifdef NVME_DRIVER` around the PCI class check to disable entirely.

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

**Regression risk:** LOW — Admin Queue uses freshly allocated contiguous pages from `pmm_alloc_contiguous()`. No shared state modified. If Identify command times out, log warning and skip NVMe. Rollback: skip Identify, treat NVMe as not present.

## 3. I/O Queue Creation and Sector Read/Write
Create one I/O Submission/Completion Queue pair and implement read/write sector operations.

**Files:** `src/kernel/drivers/nvme.c`

- [x] Submit Create I/O Completion Queue command (opcode 0x05)
- [x] Submit Create I/O Submission Queue command (opcode 0x01)
- [x] `nvme_read_sectors(nsid, lba, count, buf)` — submit Read command (opcode 0x02), wait for completion
- [x] `nvme_write_sectors(nsid, lba, count, buf)` — submit Write command (opcode 0x01), wait for completion
- [x] PRP (Physical Region Page) list for multi-page transfers
- [x] Polled completion (check CQ head) — interrupt-based deferred to TODO-02
- [x] `POST16(POST16_NVME_IO)` on entry, `POST16(POST16_NVME_IO_OK)` on exit
- [x] Validate PRP list alignment before every DMA submit (must be page-aligned for multi-page)
- [x] Commit: `"drivers: NVMe I/O Queue — read/write sectors via polled completion"`

**Test checkpoint:** Read sector 0, verify GPT/MBR header. Write + readback test on test partition only. POST code 0x20A4/0x20A5 (`POST16_NVME_IO`/`POST16_NVME_IO_OK`). Test on:
- Bare metal: read sector 0 matches GPT header
- QEMU WHPX/TCG `make run-nvme`: read sector 0 of 128 MiB test image
- VirtualBox / QEMU (no NVMe): skipped (no controller from §1)

**Regression risk:** MEDIUM — DMA writes via PRP lists could corrupt memory if physical addresses are wrong. Mitigation: validate PRP alignment, use `pmm_alloc_contiguous()` for all DMA buffers, never reuse buffers across commands without completion check. Rollback: disable I/O queue creation; §1–§2 still work for diagnostics.

## 4. Block Device Registration and VFS Integration
Register NVMe namespaces as block devices for partition scanning and filesystem mount.

**Files:** `src/kernel/main/boot_storage.c`, `src/kernel/drivers/blkdev.c`

- [x] `nvme_register_blkdev()` — register each namespace as a block device
- [x] Wire into `boot_phase2()`: after NVMe init, register block devices
- [x] Partition scan + filesystem mount (GPT + IXFS/FAT32/NTFS)
- [x] `POST16(POST16_NVME_BLK)` on entry, `POST16(POST16_NVME_BLK_OK)` on exit
- [ ] Commit: `"drivers: NVMe block device registration — NVMe drives mountable"`

**Test checkpoint:** NVMe drive visible as block device, partitions scanned, filesystem mounted. POST code 0x20A6/0x20A7 (`POST16_NVME_BLK`/`POST16_NVME_BLK_OK`). Test on:
- Bare metal with NVMe: drive visible, C:\ mounted from NVMe
- QEMU WHPX/TCG `make run-nvme`: FAT32 test partition mounted
- QEMU (no NVMe) `make run`: boot completes without NVMe — no crash
- VirtualBox: boot completes without NVMe — graceful skip

**Regression risk:** MEDIUM — modifies `boot_phase2()` in `boot_storage.c`, which runs on ALL boot paths. NVMe init is additive (inserted after AHCI, before `blkdev_register_all()`). If NVMe init hangs, all subsequent boot phases stall. Mitigation: 500 ms timeout on controller enable; if timeout, log `nvme: controller enable timeout — skipping` and continue. Rollback: remove `nvme_init()` call from `boot_phase2()`.

---

## OS Comparison

| ⭐ | Feature                 | Win11                       | Linux                        | Impossible OS                     |
|----|-------------------------|-----------------------------|------------------------------|-----------------------------------|
| 💎 | NVMe controller        | ✅ stornvme.sys              | ✅ nvme.ko                  | ✅ §1 — discovery + BAR map      |
| 💎 | NVMe I/O               | ✅ Multi-queue + interrupt   | ✅ Multi-queue + interrupt  | ✅ §3 — single queue, polled     |
| 💎 | NVMe boot              | ✅ Automatic                 | ✅ initramfs + nvme.ko      | ✅ §4 — blkdev + VFS mount       |
| ⭐ | NVMe health at boot    | ❌ Requires tools            | ❌ Requires nvme-cli        | ⬜ Planned — SMART in VPD/log    |
| ⭐ | Firmware ver at boot   | ❌ Not displayed             | ❌ dmesg post-boot only     | ✅ §2 — Identify Controller log  |
| ⭐ | Drive wear at boot     | ❌ CrystalDiskInfo needed    | ❌ Requires nvme-cli        | ⬜ Planned — SMART wear field    |
| ⭐ | Thermal throttle detect| ❌ Third-party tools         | ❌ Requires nvme-cli        | ⬜ Planned — CSTS.CFS + SMART    |

## Unit Tests

> Wire into `test_runner_init()` via `test_register_nvme()` (-> XREF: `00-infrastructure/TODO-03 S1`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.
> NVMe tests require an NVMe controller (QEMU `run-nvme` or bare metal). Tests gracefully skip when no NVMe controller is found.

- [ ] Create `src/kernel/test/test_nvme.c` with:
  - `nvme_controller_count()` returns >= 0 (no crash when no controller)
  - When NVMe present: Identify Controller completed — model string is non-empty
  - When NVMe present: Identify Namespace returns `lba_count > 0` and `sector_size` is 512 or 4096
  - When NVMe present: `nvme_read_sectors(1, 0, 1, buf)` succeeds; sector 0 contains valid GPT/MBR signature (`0x55AA` at offset 510 or `"EFI PART"` at offset 0)
  - When NVMe present: block device registered — `blkdev_find("nvme0")` returns non-NULL
  - When NVMe absent: `nvme_init()` returns gracefully (no hang, no crash)
  - Admin Queue: `nvme_admin_submit()` + poll returns completion within 500ms timeout (no infinite wait)
  - I/O Queue: `nvme_read_sectors()` + `nvme_write_sectors()` roundtrip on test sector (read, write pattern, read back, verify match) -- only on test partition, never sector 0
- [ ] Add to `scripts/test-smoke.sh` (with `run-nvme` and `run-nvme-ci` targets):
  - Grep serial for `nvme: Controller v1.` (controller discovered)
  - Grep serial for `nvme: no controller found` (graceful skip when absent)
  - Grep serial for `nvme: block device registered` (VFS integration)
  - POST code sequence: `0x20A0` through `0x20A7` all present in serial
- [ ] Register in `test_runner_init()`: `test_register_nvme()`
- [ ] Commit: `"test: add nvme test suite"`

## Verification

- [ ] QEMU WHPX `make run-nvme`: serial shows controller discovery, Identify, sector read, block device registered
- [ ] QEMU TCG `make run-nvme`: same as WHPX — controller discovered, I/O works
- [ ] QEMU (no NVMe) `make run`: serial shows `nvme: no controller found` — boot completes, no crash
- [ ] VirtualBox: boot completes without NVMe — `nvme: no controller found` in serial
- [ ] Bare metal (i5-11600K): NVMe drive detected, partitions visible, C:\ mounted
- [ ] `make run-nvme-ci`: headless test completes, `grep "nvme:" build/serial.log` shows full init sequence
- [ ] POST code sequence in serial: 0x20A0 → 0x20A1 → 0x20A2 → 0x20A3 → 0x20A4 → 0x20A5 → 0x20A6 → 0x20A7
- [ ] Crash at any POST code pinpoints failing sub-phase (e.g., stuck at 0x20A2 = Admin Queue setup failed)

**Serial log strings to grep:**
```
nvme: Controller v1.
nvme: no controller found
nvme: ".*" .* GiB
nvme: I/O Queue created
nvme: sector 0 read OK
nvme: block device registered
```
