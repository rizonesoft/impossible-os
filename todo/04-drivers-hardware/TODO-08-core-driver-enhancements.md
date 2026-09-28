---
schema_version: 1
id: core-driver-enhancements
domain: 04-drivers-hardware
status: active
title: "TODO-08 -- Core Built-in Driver Enhancements"
---

# TODO-08 -- Core Built-in Driver Enhancements

> **Goal:** Complete the remaining built-in (statically linked) driver gaps that must be available before or without a filesystem: HPET timer, PCIe ECAM extended config, capability chain scanner, MSI/MSI-X interrupt routing, and PCIe hot-plug detection.
>
> → **Boot-critical NVMe driver extracted to `01-boot-platform/TODO-16-nvme-storage.md`.** This TODO covers advanced NVMe features (multi-queue, interrupt coalescing, power states) after the boot-critical §1 is done there.
>
> **Current state:** the HPET clock already ships as `hpet_init()`/`hpet_ns()` (`src/kernel/drivers/hpet.c`; the roadmap's `hpet_read_ns()` name was never used) and LAPIC calibration measures against it (`cal_try_hpet()`, `src/kernel/drivers/lapic.c`); a polled single-I/O-queue NVMe driver ships under TODO-16. §4 and §5 rows stay open until reconciled with that code.

> [!IMPORTANT]
> All sections here are **built-in only** -- they may be needed before the IXFS mounts or are too performance-sensitive to load late. Loadable module infrastructure lives in `TODO-05`. All DMA buffers (NVMe queues, MSI-X tables) must use `pmm_alloc_contiguous()` -- never `kmalloc` for anything > 4 KB.

## Inputs

- [`src/kernel/drivers/pci.c`](../../src/kernel/drivers/pci.c), [`include/kernel/drivers/pci.h`](../../include/kernel/drivers/pci.h) -- existing flat PCI driver (281 lines, CF8/CFC only, locked); extended in §1/§2/§3/§6
- [`src/kernel/drivers/ahci/`](../../src/kernel/drivers/ahci/) -- reference for DMA queue pattern used in §5
- [`src/kernel/acpi.c`](../../src/kernel/acpi.c) -- `acpi_get_hpet_base()` consumed by §4; `acpi_get_mcfg()` does not exist yet and is added by §1
- Port base for §5: SerenityOS `Kernel/Devices/Storage/NVMe/` (BSD-2-Clause)
- [`../01-boot-platform/TODO-16-nvme-storage.md`](../01-boot-platform/TODO-16-nvme-storage.md) (→ XREF) -- boot-critical NVMe discovery, queues, blkdev; this file §5 defers implementation there
- → XREF: `01-boot-platform/TODO-11-interrupt-timer-arch.md §6` -- HPET register layout and unified `uptime_ns()` HAL consumed by §4; LAPIC calibration call site also lives there
- → XREF: `TODO-01-pci-pcie-pnp-resource-manager.md` -- PCI enumeration and bus scan that §1–§6 extend
- → XREF: `04-drivers-hardware/TODO-05-kernel-module-system.md §1` -- `EXPORT_SYMBOL` for `nvme_*`, `hpet_read_ns`, `pci_read_config32_ext`, `pci_find_capability`, `pci_enable_msi/msix`
- Spec refs: NVMe 1.4 spec; PCI Local Bus 3.0 spec; PCIe Base 4.0 spec; ACPI 6.5 §5.2.6 (MCFG)

## Outcome

- NVMe controllers (PCI class `0x01/0x08/0x02`) are detected at boot; a single Admin Queue + I/O Queue pair supports `nvme_read/write_sectors`; the namespace is registered via `blkdev_register` so VFS can mount an NVMe boot partition.
- HPET main counter is mapped and running at boot; `hpet_read_ns()` returns nanoseconds; LAPIC timer is calibrated using HPET as reference, replacing the hardcoded ICR.
- PCIe ECAM (from ACPI MCFG) is the primary config access path (4 KiB/function, MMIO); legacy I/O CF8/CFC is the fallback for devices that do not appear in MCFG.
- `pci_find_capability()` and `pcie_find_extended_capability()` walk standard (offset 0x34) and extended (offset 0x100) capability chains; `pcie_get_link_speed/width()` decode the Link Status register.
- MSI and MSI-X are enabled per device via `pci_enable_msi()` and `pci_enable_msix()`; all PCIe devices use MSI/MSI-X instead of shared legacy INTx pins.
- PCIe hot-plug interrupts are subscribed via the Slot Control register; Presence Detect Changed events trigger device enumeration or removal and call `driver_probe_all()` for newly arrived hardware.

## Implementation Order

| ⭐  | Order | Deliverable                                        | Depends On                        | Status |
| --- | :---: | -------------------------------------------------- | --------------------------------- | :----: |
| 💎  |   1   | §1 PCI enhanced config space -- ECAM via ACPI MCFG | ACPI MCFG parsed                  |  [ ]   |
| 💎  |   2   | §2 PCIe capability chain scanner                   | §5 (ECAM for extended caps)       |  [ ]   |
| 💎  |   3   | §3 MSI / MSI-X support                             | §4 (capability pointer lookup)    |  [ ]   |
| 💎  |   4   | §4 HPET timer driver                               | ACPI HPET table, VMM map          |  [ ]   |
| 💎  |   5   | §5 NVMe storage driver                             | §5, §1 (MSI vector), ACPI         |  [ ]   |
| 💎  |   6   | §6 PCIe hot-plug                                   | §4 (Slot cap), §1 (MSI interrupt) |  [ ]   |

> All six rows are 💎 parity: Windows and Linux both support NVMe, HPET, ECAM, capability scanning, MSI/MSI-X, and hot-plug. These are the minimum gaps between a working QEMU boot and real modern hardware.

---

## 1. PCI Enhanced Config Space (ECAM) `[Sonnet]`

Read the ACPI MCFG table to find the PCIe ECAM base address. Map the region and implement `pci_read_config32_ext(bus, dev, fn, reg)` / `pci_write_config32_ext(...)` using MMIO at `ecam_base + ((bus << 20) | (dev << 15) | (fn << 12) | reg)`. Fall back to legacy I/O CF8/CFC for buses not in the MCFG table.

**Files:** `src/kernel/drivers/pci.c`, `include/kernel/drivers/pci.h`

> [!CAUTION]
> ECAM pages must be mapped as strongly-ordered, uncacheable (`PCD=1, PWT=1`). Caching config register reads causes stale data. A full 256-bus segment is 256 MiB of physical address space -- only map the segment(s) listed in MCFG.

- [ ] `acpi_get_mcfg()` returns `{ phys_base, start_bus, end_bus }` for each MCFG entry
- [ ] `pci_ecam_init()`: for each MCFG entry, `vmm_map_mmio_uncached(phys_base, (end_bus - start_bus + 1) << 20)`
- [ ] `pci_cfg_addr_mmio(bus, dev, fn, reg)` -- return `ecam_base + ((bus << 20) | (dev << 15) | (fn << 12) | reg)`
- [ ] `pci_read_config8/16/32_ext` / `pci_write_config8/16/32_ext` -- use MMIO if bus in MCFG range; else legacy CF8/CFC
- [ ] Retrofit existing `pci_read_config32` / `pci_write_config32` to route through `_ext` so all callers transparently use ECAM
- [ ] Boot log: `[PCI] ECAM: %u segment(s) mapped, %u buses`; `[PCI] ECAM: not available, using legacy I/O` if no MCFG
- [ ] Commit: `"drivers: PCIe ECAM config access via ACPI MCFG -- pci_read/write_config32_ext, CF8 fallback"`

## 2. PCIe Capability Chain Scanner `[Sonnet]`

Walk the PCI standard capability list (pointer at config offset 0x34) and the PCIe extended capability list (starting at 0x100 in ECAM space). Expose `pci_find_capability(dev, cap_id)` and `pcie_find_extended_capability(dev, ext_cap_id)` returning the offset of the matching capability structure, or 0 if absent. Decode the PCIe Link Status register.

**Files:** `src/kernel/drivers/pci.c`, `include/kernel/drivers/pci.h`

- [ ] `pci_find_capability(dev, cap_id)` -- read capability pointer at offset `0x34`; walk linked list via `next_ptr` byte at `cap_offset + 1`; return `cap_offset` where `cap_id` matches, or 0
- [ ] Guard against cycles: bail after 48 iterations (maximum valid chain length per spec)
- [ ] `pcie_find_extended_capability(dev, ext_cap_id)` -- start at ECAM offset `0x100`; each header `uint32_t` encodes `ext_cap_id[15:0]` + `version[19:16]` + `next[31:20]`; walk until `next == 0`
- [ ] `pci_has_capability(dev, cap_id)` -- convenience bool wrapper around `pci_find_capability`
- [ ] Decode Link Status (PCIe cap, offset +`0x12`): `pcie_get_link_speed(dev)` returns `{2.5, 5.0, 8.0, 16.0, 32.0}` GT/s per Gen; `pcie_get_link_width(dev)` returns `x1/x2/x4/x8/x16`
- [ ] Log per PCIe device at enumeration: `[PCI] %04x:%04x PCIe Gen%u x%u`
- [ ] Commit: `"drivers: PCIe capability chain scanner -- pci_find_capability, pcie_find_extended_capability, link speed"`

## 3. MSI / MSI-X Interrupt Support `[Opus]`

Enable Message Signalled Interrupts for PCIe devices. MSI writes a single `uint32_t` to a LAPIC-format address; MSI-X writes per-entry tables in device MMIO BAR space. Both eliminate shared legacy INTx pins and are required for multi-queue NVMe and high-bandwidth NICs.

**Files:** `src/kernel/drivers/pci.c`, `include/kernel/drivers/pci.h`

> [!IMPORTANT]
> MSI Message Address format: `0xFEE[Destination<<12][RH<<3][DM<<2]`; Message Data: `[trigger mode<<15][level<<14][delivery<<11:8][vector<<7:0]`. Write both atomically -- disable the MSI capability, write address and data, then re-enable. For MSI-X, disable masking bit in each table entry only after writing address and data.
> Never mix INTx and MSI on the same device. Once MSI/MSI-X is enabled, mask the INTx via `Command.Interrupt_Disable`.

- [ ] `pci_enable_msi(dev, vector)`:
  - [ ] `pci_find_capability(dev, 0x05)` → `msi_cap_offset`; fail if absent
  - [ ] Read `Message Control` word; determine 32-bit vs 64-bit address format
  - [ ] Write `Message Address` = `0xFEE00000 | (lapic_id << 12)`; `Message Data` = `vector | 0x4000` (level, fixed)
  - [ ] Set `MSI Enable` bit in `Message Control`; set `Command.Interrupt_Disable`
- [ ] `pci_disable_msi(dev)` -- clear `MSI Enable` bit; re-enable INTx
- [ ] `pci_enable_msix(dev, vector_base)`:
  - [ ] `pci_find_capability(dev, 0x11)` → `msix_cap_offset`; read `Table Size` (`N+1` entries) and BAR index + offset
  - [ ] Map MSI-X BAR MMIO via `vmm_map_mmio_uncached(bar + table_offset, (N+1) * 16)`
  - [ ] For each entry: write `Message Address` + `Message Data` + `Vector Control = 0` (unmask) at `table_base + entry * 16`
  - [ ] Set `MSI-X Enable` bit; clear `Function Mask`; set `Command.Interrupt_Disable`
- [ ] `pci_msix_mask_vector(dev, entry)` / `pci_msix_unmask_vector(dev, entry)` -- per-entry mask bit management
- [ ] Register each MSI/MSI-X vector via `idt_register_handler(vector, handler)`
- [ ] Boot log per device: `[PCI] %04x:%04x MSI-X x%u vectors allocated (base vec %u)` or `MSI vec %u`
- [ ] Gate every MSI/MSI-X enable on `acpi_msi_supported()` (FADT IAPC_BOOT_ARCH bit 3); today it has no production caller and xHCI, AHCI, VirtIO enable MSI unconditionally
- [ ] Commit: `"drivers: MSI/MSI-X -- pci_enable_msi/msix, LAPIC message format, INTx disable"`

## 4. HPET Timer Driver `[Opus]`

Map the HPET MMIO registers from the ACPI HPET table base address. Parse the capabilities register for comparator count and `COUNTER_CLK_PERIOD`. Enable the main counter. Expose `hpet_read_ns()` returning monotonic nanoseconds. Use HPET as the reference clock for LAPIC timer calibration, replacing the hardcoded ICR.

**Files:** `src/kernel/drivers/hpet.c` (new), `include/kernel/drivers/hpet.h` (new)

> [!IMPORTANT]
> → XREF: `01-boot-platform/TODO-11-interrupt-timer-arch.md §6` -- the unified timer HAL and LAPIC calibration call site live there. This section implements the hardware read path; the calibration integration is a one-line change in the LAPIC init path to call `hpet_read_ns()` instead of a PIT-based fallback.
> HPET registers are 64-bit wide but must be read with 32-bit accesses on some firmware implementations. Use `mmio_read32_lo` + `mmio_read32_hi` with a re-read loop to handle counter wrap.

- [ ] `acpi_get_hpet_base()` returns the HPET MMIO base; `vmm_map_mmio(base, 0x400)` as uncacheable
- [ ] Read General Capabilities register (offset `0x000`): parse `NUM_TIM_CAP` (comparator count), `COUNTER_CLK_PERIOD` (femtoseconds), `REV_ID`
- [ ] `g_hpet_period_fs = COUNTER_CLK_PERIOD` (period in femtoseconds; 1 ns = 1 000 000 fs)
- [ ] Enable main counter: set `GEN_CONF.ENABLE_CNF` (bit 0 of offset `0x010`); leave legacy routing disabled
- [ ] `hpet_read_ns()`: read General Counter (offset `0x0F0`) as two 32-bit halves with re-read check; return `counter * g_hpet_period_fs / 1_000_000`
- [ ] LAPIC calibration hook: in `lapic_calibrate_timer()`, call `hpet_read_ns()` before and after known interval to derive bus frequency; write derived ICR
- [ ] Boot log: `[HPET] %u comparators, period=%u fs (%.3f MHz)`
- [ ] Commit: `"drivers: HPET timer -- MMIO map, hpet_read_ns(), LAPIC calibration reference"`

## 5. NVMe Storage Driver (Built-in) `[Opus]`

Port from SerenityOS BSD-2 (`Kernel/Devices/Storage/NVMe/`) and adapt to the Impossible OS driver model. Detect PCI class `0x01/0x08/0x02`, map BAR0 MMIO, initialise Admin Queue, run Identify commands, create one I/O Queue pair, and register the namespace via `blkdev_register`. Must complete before IXFS mounts.

**Files:** `src/kernel/drivers/nvme.c` (new), `include/kernel/drivers/nvme.h` (new)

- [ ] Reconcile §1 checklist with shipped `../01-boot-platform/TODO-16-nvme-storage.md` §1..§4 (kernel already has minimal NVMe); close superseded Serenity-port bullets or mark N/A. Post-boot storage-controller parity is owned by `TODO-13-storage-controller-device-drivers.md`.

> [!CAUTION]
> Admin SQ, Admin CQ, I/O SQ, I/O CQ, Identify buffers, and all PRP lists are DMA buffers -- allocate each via `pmm_alloc_contiguous(pages)`. All must be physically contiguous and 4 KB aligned. Physical page addresses go directly into controller registers -- do not pass virtual addresses.

> [!IMPORTANT]
> Controller reset sequence is strict: (1) clear `CC.EN` → wait `CSTS.RDY == 0`; (2) write `AQA`, `ASQ`, `ACQ`; (3) set `CC.EN` → wait `CSTS.RDY == 1`. Any deviation results in a controller firmware abort. Timeout: 500 ms per ready poll.

- [ ] Detect NVMe via PCI class `0x01/0x08/0x02`; read BAR0; `vmm_map_mmio(bar0, 0x4000)` (uncacheable)
- [ ] Controller reset: clear `CC.EN` (offset `0x14`); poll `CSTS.RDY == 0` (offset `0x1C`); timeout 500 ms
- [ ] Admin Queue setup: allocate 4 KB SQ + 4 KB CQ via `pmm_alloc_contiguous(1)` each; write physical addresses to `ASQ` (offset `0x28`) / `ACQ` (offset `0x30`); write `AQA` (offset `0x24`) with `ASQS=63, ACQS=63`
- [ ] Re-enable: set `CC.EN`, `CC.CSS=0` (NVM command set), `CC.IOSQES=6`, `CC.IOCQES=4`; poll `CSTS.RDY == 1`
- [ ] Identify Controller (`opcode 0x06`, CNS=1): allocate 4 KB DMA buffer; submit SQE; poll ACE; read `MDTS`, `NN` (namespace count)
- [ ] Identify Namespace (`opcode 0x06`, CNS=0, NSID=1): read `NSZE` (total sectors), `LBAF[FLBAS].LBADS` (sector size)
- [ ] Create I/O CQ (`opcode 0x05`, Admin): 4 KB DMA, QID=1, QSIZE=63, physically contiguous, MSI vector
- [ ] Create I/O SQ (`opcode 0x01`, Admin): 4 KB DMA, QID=1, CQID=1, QSIZE=63
- [ ] `nvme_read_sectors(lba, count, buf)` / `nvme_write_sectors(lba, count, buf)`: build SQE (opcode 0x02/0x01, NSID=1, SLB A=lba, NLB=count-1, PRP1=buf_phys); ring SQ tail doorbell; wait on CQE (poll or MSI)
- [ ] IRQ handler: read CQE from CQ head; check `SF.P` phase bit; process status; advance head; ring CQ head doorbell
- [ ] `blkdev_register(&nvme_blkops, ns_size_sectors, sector_size)`
- [ ] Boot log: `[NVMe] Controller ready: %s, %llu GB, %u B/sector`
- [ ] Commit: `"drivers: NVMe 1.4 built-in driver -- Admin+IO queues, Identify, read/write, blkdev_register"`

## 6. PCIe Hot-Plug `[Opus]`

Subscribe to the Hot-Plug interrupt via the PCIe Slot Control register. On a Presence Detect Changed event, scan the downstream port for a new device or mark the departed device removed. Call `driver_probe_all()` for newly arrived devices to trigger driver `probe()`.

**Files:** `src/kernel/drivers/pci.c`, `include/kernel/drivers/pci.h`

> [!IMPORTANT]
> Hot-plug interrupts arrive via MSI (§3 must be complete). The event handler runs in interrupt context -- it must not call `kmalloc`, `mutex_lock`, or any sleeping primitive. Queue the enumeration work via `workqueue_enqueue(&sys_wq, hotplug_work)` and return from the ISR immediately.

- [ ] `pcie_hotplug_init(dev)` -- find PCIe Slot Capabilities (`pcie_find_extended_capability` or standard PCIe cap `0x01`, Slot Cap at `+0x14`); check `Hot-Plug Capable` bit
- [ ] Enable HP interrupts: set `Presence Detect Changed Enable` + `Hot-Plug Interrupt Enable` bits in Slot Control (`+0x18`); route via MSI (§3)
- [ ] ISR: read Slot Status (`+0x1A`); if `Presence Detect Changed` bit set, clear it; enqueue `hotplug_work` with `(bus, dev, fn, arrived=PDC_state)`
- [ ] `hotplug_work` handler (runs in workqueue, sleepable context): if `arrived`: `pci_enumerate_bus(downstream_bus)` + `driver_probe_all()`; if `departed`: call `driver.remove(dev)` for all drivers attached to that device; free `pci_device_t`
- [ ] `pcie_hotplug_init()` called for all downstream ports during PCI enumeration
- [ ] Test on QEMU `-device pcie-root-port,hotplug=on`: `device_add` at runtime triggers enumeration log
- [ ] Boot log per hot-plug-capable port: `[PCIe] Slot %u: hot-plug capable, HP MSI registered`
- [ ] Commit: `"drivers: PCIe hot-plug -- Slot Control, PDC interrupt, workqueue enumeration, driver_probe_all"`

---

## OS Comparison


| ⭐  | Feature                                            | 🪟 Win11                                            | 🐧 Linux                                                             | 🚀 Impossible OS                                                   |
| --- | -------------------------------------------------- | --------------------------------------------------- | -------------------------------------------------------------------- | ------------------------------------------------------------------ |
| 💎  | NVMe storage driver -- Admin + I/O queue           | ✅ `storport.sys` + `stornvme.sys`; multi-queue     | ✅ `drivers/nvme/host/`; multi-queue, io_uring                       | ⬜ §5 -- Admin+1×IO queue, Identify, read/write, `blkdev_register` |
| 💎  | HPET timer -- `hpet_read_ns()` + LAPIC calibration | ✅ HAL uses HPET for TSC                            | ✅ `arch/x86/kernel/hpet.c`; LAPIC calibration reference             | ⬜ §4 -- MMIO map, `hpet_read_ns()`, LAPIC ICR                     |
| 💎  | PCIe ECAM (ACPI MCFG) -- 4 KiB config per device   | ✅ HAL reads MCFG; MMIO config                      | ✅ `pci_mcfg.c`; ECAM primary; `pci_read_config_word` routes         | ⬜ §1 -- `pci_read/write_config32_ext`, CF8 fallback, MCFG segment |
| 💎  | PCIe capability chain scanner                      | ✅ `PciFindCapOffsetRtn`; extended caps via ECAM    | ✅ `pci_find_capability()`; `pci_find_ext_capability()`              | ⬜ §2 -- standard + extended walker, `pcie_get_link_speed/width`   |
| 💎  | MSI / MSI-X -- per-device vectors, no shared INTx  | ✅ `HalGetInterruptVectorForMsi`; all PCIe uses MSI | ✅ `pci_enable_msi()`/`pci_enable_msix()`; IRQ affinity              | ⬜ §3 -- `pci_enable_msi/msix()`, LAPIC message format, INTx       |
| 💎  | PCIe hot-plug -- Presence Detect Changed interrupt | ✅ PCI Hot-Plug Service; device tree                | ✅ `pciehp` driver; `pciehp_isr()` → `pciehp_handle_presence_change` | ⬜ §6 -- Slot Control MSI, workqueue enumeration,                  |

> **After §1–6:** Impossible OS reaches parity with Windows NT and Linux on the core built-in hardware infrastructure needed to boot on and interact with modern x86-64 hardware. NVMe enables booting from current-generation SSDs. HPET eliminates boot-time timer jitter. ECAM + capability scanning + MSI/MSI-X form the complete PCIe programming model. Hot-plug enables rack-server and enterprise workstation use cases without rebooting.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] QEMU `-drive if=none,id=nvme0,file=disk.img -device nvme,drive=nvme0,serial=1234`: boot log shows `[NVMe] Controller ready: ..., blkdev registered`; VFS mounts IXFS from NVMe
- [ ] HPET: boot log shows `[HPET] N comparators, period=X fs`; `[SCHED] tick calibrated: ICR=N, tick=1000000 ns` (LAPIC calibrated via HPET)
- [ ] ECAM: boot log shows `[PCI] ECAM: N segment(s) mapped`; `pci_read_config32_ext` on a PCIe device returns same value as legacy CF8 read
- [ ] Capability scanner: boot log shows `[PCI] 10EC:8139 PCIe Gen1 x1` (or whatever the QEMU model reports)
- [ ] MSI: NVMe completion arrives via IDT vector (not legacy IRQ line); serial log shows `[NVMe] IRQ via MSI vec N`
- [ ] Hot-plug: QEMU `(qemu) device_add nvme,...` at runtime → serial log shows `[PCIe] Slot N: PDC arrived → driver_probe_all`
- [ ] Commit: `"drivers: core built-in enhancements -- NVMe, HPET, ECAM, capability scan, MSI/MSI-X, hot-plug"`
