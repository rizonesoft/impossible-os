# TODO-09 -- NVMe Storage Driver (Boot-Critical)

> **Goal:** Access NVMe SSDs as block devices so the OS can boot from internal storage on modern laptops. Most laptops manufactured after 2018 use NVMe as the primary (or only) storage -- without this driver, bare metal can only boot from USB or SATA.

> [!IMPORTANT]
> **Current state (2026-04-12):** `src/kernel/drivers/nvme.c` walks PCI config space in `nvme_init()`, maps BAR0 with `vmm_map_mmio_uc()`, brings up one Admin and one I/O queue pair, and registers each ready controller as `nvmeN` inside `blkdev_register_all()` (`src/kernel/main/blkdev_adapters.c`). `nvme_init()` is invoked from `boot_storage.c` Phase 2 (sequential path or async storage group). `test_nvme.c` and `test_register_nvme()` are not merged yet. **Hypervisor:** per root `CLAUDE.md`, QEMU WHPX can intermittently time out on emulated NVMe doorbells; use **QEMU TCG** for NVMe-focused CI while WHPX remains fine for SATA-first boots.
> This TODO extracts the **boot-critical** NVMe driver from `04-drivers-hardware/TODO-02-core-driver-enhancements.md §1`. Advanced NVMe features (multiple I/O queues, interrupt coalescing, namespace management, power states) remain in TODO-02. After this TODO, NVMe drives are accessible as block devices.

> [!NOTE]
> **Architecture: built-in now, bootloader-loaded later.** Windows loads `stornvme.sys` as a boot-start driver from the EFI partition via `winload.efi` -- it's not part of `ntoskrnl.exe`. For now, the NVMe driver is built into the kernel binary to get bare metal working. When the kernel module loader exists (`04-drivers-hardware/TODO-01`), refactor into a separate `.sys` driver file loaded by `bootx64.efi` from `\EFI\ImpossibleOS\drivers\` before kernel entry.

## Inputs

- [`src/kernel/drivers/pci.c`](../../src/kernel/drivers/pci.c) -- PCI device discovery
- [`src/kernel/drivers/blkdev.c`](../../src/kernel/drivers/blkdev.c) -- block device registration
- [`src/kernel/main/blkdev_adapters.c`](../../src/kernel/main/blkdev_adapters.c) -- `blkdev_register_all()` NVMe loop registers `nvme0`..`nvme3`
- [`CLAUDE.md`](../../CLAUDE.md) -- Bare Metal Gotchas: NVMe on QEMU WHPX timing (use TCG for NVMe tests)
- [`src/kernel/mm/vmm.c`](../../src/kernel/mm/vmm.c) -- `vmm_map_mmio_uc()` for NVMe BAR0
- [`../04-drivers-hardware/TODO-02-core-driver-enhancements.md`](../04-drivers-hardware/TODO-02-core-driver-enhancements.md) (→ XREF) §1 -- advanced NVMe features after boot-critical work here
- [`../04-drivers-hardware/TODO-02-core-driver-enhancements.md`](../04-drivers-hardware/TODO-02-core-driver-enhancements.md) (→ XREF) §3 -- PCIe ECAM (optional; BAR-based config works first)
- `make run-nvme` -- QEMU with NVMe controller + 128 MiB test drive
- `make run-nvme-ci` -- headless NVMe test with serial to `build/serial.log`

## Outcome

- NVMe controller discovered on PCI (class 0x01, subclass 0x08, prog-if 0x02)
- Admin Queue and one I/O Queue operational
- Identify Controller + Identify Namespace executed
- Read/write sectors via I/O Queue
- NVMe drive registered as block device, partitions scanned, filesystem mounted
- Validation policy for emulated NVMe documents WHPX vs TCG expectations (see `CLAUDE.md` + §5)
- SMART / Get Log / multi-namespace / discard parity tracked as backlog in §5 for `TODO-02`

> [!WARNING]
> **Boot-path impact:**
> - §1 discovery runs inside `nvme_init()` PCI walk (uses `pci_read*` from `pci.c`) -- read-only config access, no change to global PCI driver attach model.
> - §4 adds `nvme_init()` call to `boot_phase2()` in `boot_storage.c` (after AHCI init, before `blkdev_register_all()`). This path runs after `sti` -- safe from silent triple faults. All platforms must boot cleanly with NVMe absent (graceful skip).

## Implementation Order

| ⭐ | Order | Deliverable                                   | Depends On | Status |
|----|:-----:|-----------------------------------------------|------------|:------:|
| 💎 |   1   | NVMe controller discovery and BAR mapping     | --         |  [x]   |
| 💎 |   2   | Admin Queue setup and Identify commands       | §1         |  [x]   |
| 💎 |   3   | I/O Queue creation and sector read/write      | §2         |  [x]   |
| 💎 |   4   | Block device registration and VFS integration | §3         |  [x]   |
| ⭐ |   5   | Advanced NVMe parity backlog (owned by TODO-02) | §1, §2, §3, §4 |  [ ]   |

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

## 3. I/O Queue Creation and Sector Read/Write
Create one I/O Submission/Completion Queue pair and implement read/write sector operations.

**Files:** `src/kernel/drivers/nvme.c`

- [x] Submit Create I/O Completion Queue command (opcode 0x05)
- [x] Submit Create I/O Submission Queue command (opcode 0x01)
- [x] `nvme_read_sectors(ctrl_idx, lba, count, buf)` per `nvme.h` -- submit Read command (opcode 0x02), wait for completion
- [x] `nvme_write_sectors(ctrl_idx, lba, count, buf)` per `nvme.h` -- submit Write command (opcode 0x01), wait for completion
- [x] PRP (Physical Region Page) list for multi-page transfers
- [x] Polled completion (check CQ head) -- interrupt-based deferred to TODO-02
- [x] `POST16(POST16_NVME_IO)` on entry, `POST16(POST16_NVME_IO_OK)` on exit
- [x] Validate PRP list alignment before every DMA submit (must be page-aligned for multi-page)
- [x] Commit: `"drivers: NVMe I/O Queue -- read/write sectors via polled completion"`

**Test checkpoint:** Read sector 0, verify GPT/MBR header. Write + readback test on test partition only. POST code 0x20A4/0x20A5 (`POST16_NVME_IO`/`POST16_NVME_IO_OK`). Test on:
- Bare metal: read sector 0 matches GPT header
- QEMU WHPX/TCG `make run-nvme`: read sector 0 of 128 MiB test image
- VirtualBox / QEMU (no NVMe): skipped (no controller from §1)

**Regression risk:** MEDIUM -- DMA writes via PRP lists could corrupt memory if physical addresses are wrong. Mitigation: validate PRP alignment, use `pmm_alloc_contiguous()` for all DMA buffers, never reuse buffers across commands without completion check. Rollback: disable I/O queue creation; §1, §2 still work for diagnostics.

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

---

## 5. Advanced NVMe parity backlog (TODO-02 owned)

> Items below are **not** boot-critical; they track parity with Windows 11 / Linux stacks documented in gap analysis (native multi-queue + MSI-X, SMART log page 0x02, namespace management, discard, APST, fabrics). Implementation belongs in `../04-drivers-hardware/TODO-02-core-driver-enhancements.md` unless a future TODO splits NVMe maintenance further.

**Regression risk:** LOW -- this section is documentation and ownership only until work migrates into `TODO-02`.

- [ ] MSI-X (or MSI) completion path replacing polled CQ head spin (-> XREF `../04-drivers-hardware/TODO-02-core-driver-enhancements.md` §5)
- [ ] Host-side multi I/O queue / affinity model comparable to Linux `blk-mq` per-core queues (-> XREF same file §1)
- [ ] Get Log Page SMART / Health (identifier 0x02) plus Critical Warning surfacing to klog or VPD (-> XREF same file §1 + [`TODO-08-visual-post-display.md`](TODO-08-visual-post-display.md) when VPD owns SMART text)
- [ ] Enumerate and attach namespaces beyond NSID 1 when drives expose multiple ranges (-> XREF same file §1)
- [ ] Dataset Management / Deallocate wired to `blkdev_discard` for filesystem TRIM (-> XREF same file §1)
- [ ] Autonomous Power State Transitions for idle power on laptops (-> XREF same file §1)
- [ ] NVMe over Fabrics transports deferred (no SCSI translation layer needed) (-> XREF same file once networking + RDMA prerequisites exist)
- [ ] Commit: `todo: NVMe advanced backlog tracked no kernel change`

**Test checkpoint:** Each §5 bullet maps to a matching `[x]` in `../04-drivers-hardware/TODO-02-core-driver-enhancements.md` §1 or §5 with proof: QEMU TCG `make run-nvme` shows no new `nvme: controller enable timeout` regressions; serial still shows POST16 `0x20A0` through `0x20A7` in order on reference image; `bash scripts/test.sh SUITE=storage` passes after the merged feature lands.

## OS Comparison

| ⭐ | Feature                  | 🪟 Win11                    | 🐧 Linux                     | 🚀 Impossible OS                |
|----|--------------------------|-----------------------------|------------------------------|---------------------------------|
| 💎 | NVMe discovery           | ✅ stornvme.sys             | ✅ nvme.ko                   | ✅ §1 BAR UC MMIO map           |
| 💎 | NVMe I/O path            | ✅ Multi queue MSI IRQ     | ✅ Multi queue MSI IRQ       | ✅ §3 one poll queue pair       |
| 💎 | NVMe boot mount          | ✅ Boot start driver        | ✅ initramfs loads nvme      | ✅ §4 blkdev then VFS mount     |
| ⭐ | WHPX NVMe CI caveat      | N/A host hypervisor layer   | N/A host hypervisor layer    | ⚠️ Prefer TCG for NVMe tests    |
| ⭐ | SMART health at boot     | ❌ Needs vendor tools       | ❌ Needs nvme userland       | ⬜ Planned VPD SMART stretch    |
| ⭐ | Firmware ID at boot      | ❌ Not shown in boot UI     | ❌ dmesg after boot only     | ✅ §2 Identify strings in klog  |
| ⭐ | Wear counters at boot    | ❌ Needs third party app    | ❌ Needs nvme userland       | ⬜ Planned wear field stretch   |
| ⭐ | Thermal throttle at boot | ❌ Needs third party app    | ❌ Needs nvme userland       | ⬜ Planned CSTS plus SMART bit  |

## Unit Tests

> Wire into `test_runner_init()` via `test_register_nvme()` (same pattern as
> [`src/kernel/test/test_runner.c`](../../src/kernel/test/test_runner.c) and
> `test_suite_register_cat(..., TEST_CAT_STORAGE)`; see `include/kernel/test/test.h`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.
> NVMe tests require an NVMe controller (QEMU `run-nvme` or bare metal). Tests must
> `TEST_SKIP` when `nvme_controller_count() == 0` without touching MMIO.

- [ ] Create `src/kernel/test/test_nvme.c` with:
  - `nvme_controller_count()` returns `>= 0` (no crash when no controller)
  - When NVMe present: after `nvme_init()`, `nvme_get_controller(0)->model[0] != 0`
  - When NVMe present: `nvme_get_controller(0)->ns_lba_count > 0` and `ns_sector_size` is 512 or 4096
  - When NVMe present: `nvme_read_sectors(0, 0, 1, buf)` succeeds; sector 0 matches GPT or MBR (`0x55AA` at 510 or `EFI PART` at 0)
  - When NVMe present: `blkdev_get("nvme0") != NULL` after `blkdev_register_all()` path used in boot tests
  - When NVMe absent: `nvme_init()` returns without hang or crash
  - Admin plus I/O path covered by Identify plus `nvme_read_sectors(0, 0, 1, buf)` within one test (no public `nvme_admin_submit` symbol)
  - I/O Queue: `nvme_read_sectors` plus `nvme_write_sectors` roundtrip on a scratch LBA only (never sector 0); skip if no writable test partition
- [ ] Extend `scripts/test-smoke.sh` for `run-nvme` / `run-nvme-ci` when scripted: grep serial for `Controller v` and `no controller found` and `block device(s):` from `blkdev_list()` after NVMe init
- [ ] Register in `test_runner_init()`: `test_register_nvme()` (or fold cases into `test_register_storage()` in `test_storage.c` if preferred)
- [ ] Commit: `"test: add nvme test suite"`

---

## Verification

- [ ] QEMU WHPX `make run-nvme`: serial shows controller discovery, Identify, sector read, and `blk` list includes `nvme0`
- [ ] QEMU TCG `make run-nvme`: same as WHPX -- controller discovered, I/O works
- [ ] QEMU (no NVMe) `make run`: serial shows `nvme: no controller found` -- boot completes, no crash
- [ ] VirtualBox: boot completes without NVMe -- `nvme: no controller found` in serial
- [ ] Bare metal (i5-11600K): NVMe drive detected, partitions visible, C:\ mounted
- [ ] `make run-nvme-ci`: headless test completes, `grep "nvme:" build/serial.log` shows full init sequence
- [ ] POST code sequence in serial: 0x20A0 then 0x20A1 then 0x20A2 then 0x20A3 then 0x20A4 then 0x20A5 then 0x20A6 then 0x20A7
- [ ] Crash at any POST code pinpoints failing sub-phase (e.g., stuck at 0x20A2 = Admin Queue setup failed)

**Serial log strings to grep (subsystem `nvme` or `blk` via klog):**
```
Controller v
no controller found
I/O Queue created
sector 0 read OK
block device(s):
  nvme0:
```

**Test runner:** `scripts/debug/run-storage-tests.bat` (SUITE=storage)

---

## History

| Date       | Action   | Summary |
|------------|----------|---------|
| 2026-04-12 | validate | validate-todo-file: Inputs XREF paths fixed; Impl Order table padding; OS Comparison cells shortened; section 4 Commit marked done vs tree; Unit Tests APIs fixed, phantom TODO-03 XREF removed, test_runner + TEST_CAT_STORAGE; Verification ends with test runner line after klog grep block; Unit Tests --- separator plus Commit backtick; POST line ASCII then chain; regression §1, §2 not en-dash; QEMU WHPX line references blk nvme0; TODO-02 Inputs back-XREF; flags: test_nvme.c absent; test-smoke.sh has no NVMe lines; TODO-02 section 1 row still open until MSI or ECAM deps. |
| 2026-04-12 | gap-analysis | Web research (Win11 stornvme + native path, Linux blk-mq NVMe, NVMe 2.0 ZNS, SMART log 0x02, WHPX timing); code-truth vs `nvme.c`/`boot_storage.c`/`blkdev_adapters.c`; added Current state + `CLAUDE.md` Input; fixed §3 API text + §4 blkdev wording + WARNING PCI note; new §5 backlog + Impl order row 5 + OS WHPX row; Unit Tests unchanged open. |
| 2026-04-12 | validate | validate-todo-file (second pass): Impl row 5 Depends On uses §1, §2, §3, §4; §1 Files line reflects shipped tree; Input `blkdev_adapters.c`; single blank before §5; §5 Regression risk moved before checklist so Commit is last `- [ ]` line and Test checkpoint closes section; OS table unchanged. |
