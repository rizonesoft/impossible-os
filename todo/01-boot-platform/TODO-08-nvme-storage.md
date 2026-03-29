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

## Outcome

- NVMe controller discovered on PCI (class 0x01, subclass 0x08, prog-if 0x02)
- Admin Queue and one I/O Queue operational
- Identify Controller + Identify Namespace executed
- Read/write sectors via I/O Queue
- NVMe drive registered as block device, partitions scanned, filesystem mounted

## Implementation Order

| ⭐  | Order | Deliverable                                    | Depends On | Status |
| --- | :---: | ---------------------------------------------- | ---------- | :----: |
| 💎  |   1   | NVMe controller discovery and BAR mapping      | —          |  [ ]   |
| 💎  |   2   | Admin Queue setup and Identify commands        | §1         |  [ ]   |
| 💎  |   3   | I/O Queue creation and sector read/write       | §2         |  [ ]   |
| 💎  |   4   | Block device registration and VFS integration  | §3         |  [ ]   |

---

## 1. NVMe Controller Discovery and BAR Mapping
Find NVMe controllers on PCI and map BAR0 as UC for register access.

**Files:** `src/kernel/drivers/nvme.c` (new), `include/kernel/drivers/nvme.h` (new)

- [ ] PCI scan: find devices with class=0x01, subclass=0x08, prog-if=0x02
- [ ] Read BAR0 (64-bit MMIO base address)
- [ ] Map BAR0 via `vmm_map_mmio_uc()` — NVMe registers are MMIO
- [ ] Read CAP register: verify NVMe version, max queue entries, doorbell stride
- [ ] Controller disable → reset → enable sequence (CC.EN = 0 → wait CSTS.RDY=0 → CC.EN=1 → wait CSTS.RDY=1)
- [ ] Commit: `"drivers: NVMe controller discovery + BAR0 UC mapping"`

**Test checkpoint:** Serial shows `nvme: Controller v1.N at PCI B:D.F, BAR0=0xNNNN`. POST code 0xDE00. Test on: bare metal (NVMe present), QEMU (no NVMe — graceful skip).

## 2. Admin Queue Setup and Identify Commands
Create the Admin Submission/Completion Queue pair and execute Identify Controller + Identify Namespace.

**Files:** `src/kernel/drivers/nvme.c`

- [ ] Allocate Admin SQ and CQ from PMM (4 KiB each, contiguous, UC-mapped or within identity map)
- [ ] Write AQA, ASQ, ACQ registers with queue addresses and sizes
- [ ] Submit Identify Controller command (opcode 0x06, CNS=1) → parse model string, serial, capacity
- [ ] Submit Identify Namespace command (opcode 0x06, CNS=0, NSID=1) → get LBA count, sector size
- [ ] Log: `nvme: "Samsung 980 PRO" 500 GiB, 512-byte sectors, 976773168 LBAs`
- [ ] Commit: `"drivers: NVMe Admin Queue + Identify Controller/Namespace"`

**Test checkpoint:** Serial shows controller model and capacity. POST code 0xDE01. Test on: bare metal.

## 3. I/O Queue Creation and Sector Read/Write
Create one I/O Submission/Completion Queue pair and implement read/write sector operations.

**Files:** `src/kernel/drivers/nvme.c`

- [ ] Submit Create I/O Completion Queue command (opcode 0x05)
- [ ] Submit Create I/O Submission Queue command (opcode 0x01)
- [ ] `nvme_read_sectors(nsid, lba, count, buf)` — submit Read command (opcode 0x02), wait for completion
- [ ] `nvme_write_sectors(nsid, lba, count, buf)` — submit Write command (opcode 0x01), wait for completion
- [ ] PRP (Physical Region Page) list for multi-page transfers
- [ ] Polled completion (check CQ head) — interrupt-based deferred to TODO-02
- [ ] Commit: `"drivers: NVMe I/O Queue — read/write sectors via polled completion"`

**Test checkpoint:** Read sector 0, verify GPT/MBR header. Write + readback test. POST code 0xDE02. Test on: bare metal.

## 4. Block Device Registration and VFS Integration
Register NVMe namespaces as block devices for partition scanning and filesystem mount.

**Files:** `src/kernel/main/boot_storage.c`, `src/kernel/drivers/blkdev.c`

- [ ] `nvme_register_blkdev()` — register each namespace as a block device
- [ ] Wire into `boot_phase2()`: after NVMe init, register block devices
- [ ] Partition scan + filesystem mount (GPT + IXFS/FAT32/NTFS)
- [ ] Commit: `"drivers: NVMe block device registration — NVMe drives mountable"`

**Test checkpoint:** Bare metal with NVMe: drive visible, partitions scanned, C:\ mounted. POST code 0xDE03.

---

## OS Comparison

| ⭐ | Feature                 | Win11                       | Linux                        | Impossible OS                    |
|----|-------------------------|-----------------------------|------------------------------|----------------------------------|
| 💎 | NVMe controller        | ✅ stornvme.sys              | ✅ nvme.ko                    | ⬜ §1 — discovery + BAR map      |
| 💎 | NVMe I/O               | ✅ Multi-queue + interrupt   | ✅ Multi-queue + interrupt    | ⬜ §3 — single queue, polled     |
| 💎 | NVMe boot              | ✅ Automatic                 | ✅ initramfs + nvme.ko        | ⬜ §4 — boot-critical path       |
| ⭐ | NVMe health at boot    | ❌ Requires tools            | ❌ Requires nvme-cli          | ⬜ Planned — SMART in VPD/log    |

## Verification

- [ ] Bare metal (i5-11600K): NVMe drive detected, partitions visible, C:\ mounted
- [ ] QEMU: graceful skip when no NVMe present (no crash, no warning)
