# 044-PCI — PCI/PCIe Bus Infrastructure

> **Goal:** Evolve from the current minimal PCI stub (legacy I/O port config
> access, flat bus scan, `pci_find_device`) into a complete PCI/PCIe subsystem
> with proper bus enumeration, BAR allocation, capabilities parsing, MSI/MSI-X
> interrupt support, bridge handling, PCIe ECAM, error reporting, and power
> management — providing the bus infrastructure that all hardware drivers (AHCI,
> NIC, GPU, USB) depend on.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (MSI-X tables, ECAM mappings, device arrays). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). See `rules.md` Known Gotchas.

> [!IMPORTANT]
> **Spec Reference:** All register offsets, command encodings, and capability IDs reference the
> [PCI Local Bus Spec 3.0](file:///home/derickpayne/impossible-os/specs/bus/pci-3.0.md)
> summary in the repo at `specs/bus/pci-3.0.md`. PCIe ECAM references [TODO-012-ACPI §1.3](../010-Kernel-Foundations/TODO-012-ACPI.md).

> [!NOTE]
> **Cross-references:**
> - [TODO-012-ACPI.md §1.3](../010-Kernel-Foundations/TODO-012-ACPI.md) — MCFG table for PCIe ECAM base address
> - [TODO-012-ACPI.md §8.1](../010-Kernel-Foundations/TODO-012-ACPI.md) — ACPI `_PRT` for PCI interrupt routing
> - [TODO-041-AHCI.md](TODO-041-AHCI.md) — AHCI SATA controller (PCI class 01:06)
> - [TODO-063-Drivers.md](TODO-063-Drivers.md) — All hardware drivers depend on PCI enumeration
> - [TODO-043-x86-64.md §7.1](TODO-043-x86-64.md) — MSR management (APIC base for MSI targeting)

---

## 1. PCI Device Model

### 1.1 Proper Device Enumeration ✅ (Partial)

**Prompt:** The current `pci_scan()` does a flat brute-force scan of bus 0 only, printing devices to the console. Replace this with a proper recursive bus enumeration that: discovers all buses (following PCI-to-PCI bridges), builds a device tree, correctly handles multi-function devices (Header Type bit 7), and stores all discovered devices in a global device list. Each device entry should store: bus/device/function, vendor/device IDs, class/subclass/progif, header type, interrupt pin/line, and all 6 BARs. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"pci: proper device enumeration"`. Add notes directly in this TODO section.

- [x] Read config space header: Vendor ID (0x00), Device ID (0x02), Class/Subclass (0x0A/0x0B)
- [x] Scan bus 0: iterate devices 0–31, check vendor != 0xFFFF
- [x] Check multi-function: Header Type (0x0E) bit 7 → scan functions 1–7
- [ ] Define `struct pci_device`:
  - [ ] Bus, device, function (BDF address)
  - [ ] Vendor ID, Device ID, Revision ID
  - [ ] Class code, Subclass, Programming Interface
  - [ ] Header Type (0 = endpoint, 1 = bridge, 2 = CardBus)
  - [ ] Interrupt Pin (0x3D), Interrupt Line (0x3C)
  - [ ] BAR[0–5] — decoded base address, size, type (memory/IO/prefetchable)
  - [ ] Capabilities pointer (0x34)
  - [ ] Parent bridge pointer (for tree structure)
- [ ] Build device linked list / array (global `pci_devices[]`)
- [ ] Recursive bridge enumeration:
  - [ ] If Header Type == 1 (PCI-to-PCI bridge):
    - [ ] Read Secondary Bus Number (0x19)
    - [ ] Recursively scan the secondary bus
  - [ ] Follow all bridges to discover full bus hierarchy
- [ ] BAR probing:
  - [ ] Write 0xFFFFFFFF to BAR, read back, mask lower bits
  - [ ] Bit 0: Memory (0) vs I/O (1)
  - [ ] Bits 1–2: 32-bit (00) vs 64-bit (10) memory
  - [ ] Bit 3: Prefetchable
  - [ ] Calculate size: `~(readback & mask) + 1`
  - [ ] For 64-bit BARs: consume two consecutive BAR slots
- [ ] Log: `[PCI] Found 12 devices on 3 buses`
- [ ] Log per device: `[PCI] 00:03.0 Intel AHCI [01:06:01] BAR5=0x81043000 (4K)`
- [ ] Commit: `"pci: proper device enumeration"`

### 1.2 PCI Device Lookup API

**Prompt:** Provide multiple ways to find PCI devices: by class/subclass (e.g., all storage controllers), by vendor/device ID (e.g., Intel 8086:2922 AHCI), and by BDF address. The current `pci_find_device(vendor, device)` only searches by ID on bus 0. This needs to work across all enumerated buses and return a proper `struct pci_device*`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"pci: device lookup API"`. Add notes directly in this TODO section.

- [ ] `pci_find_device(vendor_id, device_id)` — return first match (update existing)
- [ ] `pci_find_class(class, subclass)` — return first device matching class code
- [ ] `pci_find_all_class(class, subclass, out[], max)` — return all matching devices
- [ ] `pci_get_device(bus, dev, func)` — direct BDF lookup
- [ ] `pci_for_each_device(callback, ctx)` — iterate all devices
- [ ] All functions search multi-bus device list (not re-scanning hardware)
- [ ] Commit: `"pci: device lookup API"`

---

## 2. Capabilities List Parsing

### 2.1 Capabilities Walker

**Prompt:** PCI devices advertise extended features via a linked list of Capability structures starting at the Capabilities Pointer (offset 0x34). Each entry has: Capability ID (1 byte), Next pointer (1 byte), and capability-specific registers. The walker reads Status register bit 4 (Capabilities List Present), then follows the linked list. Cache discovered capabilities in the device struct for fast lookup. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"pci: capabilities list parser"`. Add notes directly in this TODO section.

- [ ] Check PCI Status register (0x06) bit 4: Capabilities List present
- [ ] Read Capabilities Pointer at offset 0x34 (bottom 2 bits reserved, must mask)
- [ ] Walk linked list: read Cap ID (byte 0), Next Pointer (byte 1) at each entry
- [ ] Terminate when Next Pointer == 0x00
- [ ] Store capabilities in device struct: array of `{ id, offset }`
- [ ] Implement `pci_find_capability(dev, cap_id)` — return offset or 0
- [ ] Standard Capability IDs to recognize:
  - [ ] `0x01` — Power Management Interface (PMI)
  - [ ] `0x05` — MSI (Message Signaled Interrupts)
  - [ ] `0x10` — PCIe (PCI Express Capability)
  - [ ] `0x11` — MSI-X (Message Signaled Interrupts Extended)
  - [ ] `0x12` — SATA (Serial ATA)
  - [ ] `0x13` — Advanced Features (AF)
- [ ] Log: `[PCI] 00:03.0 capabilities: PMI MSI-X PCIe`
- [ ] Commit: `"pci: capabilities list parser"`

---

## 3. MSI / MSI-X Interrupt Support

### 3.1 MSI Support (Basic)

**Prompt:** MSI (Message Signaled Interrupts, Capability ID 0x05) replaces legacy INTx# pin-based interrupts with in-band memory write transactions to the LAPIC. The device writes a message (containing the interrupt vector) to a message address (LAPIC address). MSI supports up to 32 vectors per device. The OS must: find the MSI capability, program the Message Address (LAPIC base + destination), Message Data (vector number), and enable MSI by setting the MSI Enable bit. After enabling MSI, legacy INTx# is automatically disabled. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"pci: MSI interrupt support"`. Add notes directly in this TODO section.

- [ ] Find MSI capability via `pci_find_capability(dev, 0x05)`
- [ ] Parse MSI capability structure:
  - [ ] Message Control register: 64-bit capable, per-vector masking, multiple message capable
  - [ ] Message Address register (4 or 8 bytes depending on 64-bit capable)
  - [ ] Message Data register (16-bit interrupt vector encoding)
- [ ] Program MSI:
  - [ ] Message Address: `0xFEE00000 | (dest_apic_id << 12)` (fixed LAPIC base)
  - [ ] Message Data: `vector & 0xFF` (interrupt vector, edge-triggered, fixed delivery)
  - [ ] Set MSI Enable bit in Message Control
- [ ] Implement `pci_enable_msi(dev, vector)` — configure and enable MSI for a device
- [ ] Implement `pci_disable_msi(dev)` — disable MSI, re-enable legacy INTx#
- [ ] Disable legacy INTx#: set Command register bit 10 (Interrupt Disable)
- [ ] Register vector with IDT via `idt_register_handler(vector, handler)`
- [ ] Commit: `"pci: MSI interrupt support"`

### 3.2 MSI-X Support (Extended)

**Prompt:** MSI-X (Capability ID 0x11) extends MSI to support up to 2,048 independent interrupt vectors per device. Unlike MSI (which stores message address/data in config space), MSI-X uses a memory-mapped table in the device's BAR space. Each table entry has: Message Address (8 bytes), Message Data (4 bytes), and Vector Control (4 bytes, includes per-vector mask bit). A separate Pending Bit Array (PBA) tracks pending interrupts. MSI-X is critical for multi-queue devices (NVMe, high-speed NICs). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"pci: MSI-X interrupt support"`. Add notes directly in this TODO section.

- [ ] Find MSI-X capability via `pci_find_capability(dev, 0x11)`
- [ ] Parse MSI-X capability:
  - [ ] Message Control: Table Size (bits 10:0, actual size = value + 1), Function Mask, MSI-X Enable
  - [ ] Table Offset + BIR (3-bit BAR Indicator Register): which BAR + offset
  - [ ] PBA Offset + BIR: Pending Bit Array location
- [ ] Map MSI-X table: read BIR → get BAR base address → add table offset → MMIO map
- [ ] Map PBA: same approach with PBA BIR + offset
- [ ] Each MSI-X table entry (16 bytes):
  - [ ] `msg_addr_lo` (4 bytes) — lower 32 bits of LAPIC target
  - [ ] `msg_addr_hi` (4 bytes) — upper 32 bits (0 for x86)
  - [ ] `msg_data` (4 bytes) — interrupt vector
  - [ ] `vector_ctrl` (4 bytes) — bit 0 = Mask (1 = masked)
- [ ] Implement `pci_enable_msix(dev)` — enable MSI-X globally
- [ ] Implement `pci_msix_set_entry(dev, index, vector, cpu)` — program one table entry
- [ ] Implement `pci_msix_mask(dev, index)` / `pci_msix_unmask(dev, index)`
- [ ] Set MSI-X Enable bit in Message Control, clear Function Mask
- [ ] AHCI integration: use MSI-X for per-port interrupt vectors (see TODO-041 §1.1)
- [ ] Commit: `"pci: MSI-X interrupt support"`

---

## 4. PCIe Extended Configuration (ECAM)

### 4.1 PCIe ECAM Access

**Prompt:** Legacy PCI config access via I/O ports (0xCF8/0xCFC) only supports 256 bytes of config space per device. PCIe extends this to 4096 bytes per function via Memory-Mapped Configuration (ECAM). The ECAM base address comes from the ACPI MCFG table (see TODO-042 §1.3). Each function gets a 4K page at offset `(bus << 20) | (dev << 15) | (func << 12)` from the ECAM base. This enables access to PCIe extended capabilities (AER, SR-IOV, LTR, etc.) at offsets 0x100–0xFFF. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"pci: PCIe ECAM config access"`. Add notes directly in this TODO section.

- [ ] Get ECAM base from MCFG table (see TODO-012-ACPI §1.3)
- [ ] Calculate per-function address: `ecam_base + (bus << 20) | (dev << 15) | (func << 12)`
- [ ] Map ECAM region: identity-map the ECAM range (256 MB for 256 buses)
- [ ] Implement `pcie_read32(bus, dev, func, offset)` — MMIO read from ECAM
- [ ] Implement `pcie_write32(bus, dev, func, offset, value)` — MMIO write to ECAM
- [ ] Support offsets 0x000–0xFFF (full 4096-byte extended config space)
- [ ] Fallback: if MCFG not present, use legacy I/O port mechanism (current behavior)
- [ ] PCIe extended capabilities at offset 0x100+:
  - [ ] Walk PCIe extended capability list (ID + version + next pointer)
  - [ ] `0x0001` — AER (Advanced Error Reporting)
  - [ ] `0x0010` — SR-IOV (Single Root I/O Virtualization)
  - [ ] `0x0018` — LTR (Latency Tolerance Reporting)
- [ ] Commit: `"pci: PCIe ECAM config access"`

---

## 5. PCI Command & Status Register Management

### 5.1 Device Command Control

**Prompt:** The PCI Command register (offset 0x04) controls device behavior: memory space enable, I/O space enable, bus master enable, interrupt disable, SERR# enable, parity error response. The current `pci_enable_bus_mastering()` only sets the bus master bit. Provide a proper API for enabling/disabling each bit, and ensure BAR regions are enabled before driver access. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"pci: command register management"`. Add notes directly in this TODO section.

- [x] Bus Master Enable (bit 2) — `pci_enable_bus_mastering()` *(done)*
- [ ] Memory Space Enable (bit 1) — required before accessing memory BARs
- [ ] I/O Space Enable (bit 0) — required before accessing I/O BARs
- [ ] Interrupt Disable (bit 10) — set when using MSI/MSI-X
- [ ] SERR# Enable (bit 8) — enable system error reporting
- [ ] Parity Error Response (bit 6) — enable parity checking
- [ ] Implement `pci_set_command(dev, bits_to_set, bits_to_clear)`
- [ ] Auto-enable Memory/IO Space when BAR is programmed by driver
- [ ] Commit: `"pci: command register management"`

### 5.2 Status Register & Error Checking

**Prompt:** The PCI Status register (offset 0x06) reports error conditions: Detected Parity Error (bit 15), Signaled System Error (bit 14), Received Master Abort (bit 13), Received Target Abort (bit 12), and Data Parity Error Detected (bit 8). Status bits are "write-1-to-clear" (W1C). The kernel should check status after failed transactions and provide a `pci_check_errors(dev)` function. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"pci: status register error checking"`. Add notes directly in this TODO section.

- [ ] Read PCI Status register (offset 0x06)
- [ ] Check error bits:
  - [ ] Bit 15: Detected Parity Error
  - [ ] Bit 14: Signaled System Error (SERR#)
  - [ ] Bit 13: Received Master Abort
  - [ ] Bit 12: Received Target Abort
  - [ ] Bit 8: Data Parity Error Detected
- [ ] Implement `pci_check_errors(dev)` — read status, log errors, clear W1C bits
- [ ] Implement `pci_clear_errors(dev)` — write-1 to all error bits to clear
- [ ] Log: `[PCI] 00:03.0 ERROR: Received Master Abort`
- [ ] Commit: `"pci: status register error checking"`

---

## 6. PCI-to-PCI Bridge Configuration

### 6.1 Bridge Bus Number Programming

**Prompt:** PCI-to-PCI bridges (Header Type 1) connect bus segments. Each bridge has: Primary Bus Number (0x18), Secondary Bus Number (0x19), and Subordinate Bus Number (0x1A). The firmware (UEFI) typically programs these, but if the OS rescans or hotplugs a bridge, it must know how to configure them. Also needed: bridge memory windows (Memory Base/Limit at 0x20–0x22), I/O windows (I/O Base/Limit at 0x1C–0x1D), and prefetchable memory windows (0x24–0x2B). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"pci: bridge configuration"`. Add notes directly in this TODO section.

- [ ] For Header Type 1 devices, parse bridge-specific registers:
  - [ ] Primary Bus Number (0x18), Secondary Bus Number (0x19), Subordinate Bus Number (0x1A)
  - [ ] I/O Base (0x1C) / I/O Limit (0x1D) — I/O window
  - [ ] Memory Base (0x20) / Memory Limit (0x22) — non-prefetchable memory window
  - [ ] Prefetchable Memory Base (0x24) / Prefetchable Memory Limit (0x26)
  - [ ] Bridge Control register (0x3E): SERR# forwarding, ISA enable, VGA enable
- [ ] Store bridge info in `struct pci_bridge` (extends `struct pci_device`)
- [ ] Build bus topology tree: each bridge points to its secondary bus devices
- [ ] Log: `[PCI] Bridge 00:1E.0 → bus 1 (subordinate 3)`
- [ ] Commit: `"pci: bridge configuration"`

---

## 7. PCI Power Management

### 7.1 PCI PM Capability (D-States)

**Prompt:** PCI devices support power states D0 (fully on) through D3hot (software off, registers lost) via the Power Management Capability (ID 0x01). The PMCSR (Power Management Control/Status Register) allows the OS to transition devices between power states. D3cold (hardware power removed) requires platform support (ACPI `_PS3` or power rail control). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"pci: power management D-states"`. Add notes directly in this TODO section.

- [ ] Find PM capability via `pci_find_capability(dev, 0x01)`
- [ ] Parse PM Capabilities register:
  - [ ] D1/D2 support bits (some devices skip directly to D3)
  - [ ] PME# support from each D-state
  - [ ] Aux current required in D3cold
- [ ] Parse/write PMCSR (PM Control/Status Register):
  - [ ] PowerState bits (1:0): 00=D0, 01=D1, 10=D2, 11=D3hot
  - [ ] PME_En bit: enable PME# generation
  - [ ] PME_Status bit: PME# has been asserted (W1C)
- [ ] Implement `pci_set_power_state(dev, state)`:
  - [ ] D0→D3: save config space, write PowerState bits
  - [ ] D3→D0: write PowerState bits, wait recovery time (10ms for D3→D0), restore config
- [ ] Implement `pci_get_power_state(dev)` — read PMCSR PowerState
- [ ] Integration: call during system suspend/resume (see TODO-100 §7)
- [ ] Commit: `"pci: power management D-states"`

---

## 8. PCI Resource Allocation

### 8.1 BAR Assignment & MMIO Mapping

**Prompt:** After enumeration, some BARs may need (re-)programming if the firmware left them unassigned or if we need to remap them. The OS should manage a physical address allocator for PCI MMIO regions, assigning non-overlapping ranges to each device's BARs. For most QEMU/UEFI setups, firmware pre-assigns BARs — just read and use them. But on real hardware, BARs may be unassigned. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"pci: BAR assignment and MMIO mapping"`. Add notes directly in this TODO section.

- [ ] During enumeration, read all BARs and store decoded info:
  - [ ] Base address (physical)
  - [ ] Size (from BAR probing)
  - [ ] Type: memory 32-bit, memory 64-bit, I/O
  - [ ] Prefetchable flag
- [ ] If BAR is 0 (unassigned): allocate from MMIO range allocator
- [ ] MMIO range allocator: manage a pool of physical addresses above 0x80000000
  - [ ] Track allocated ranges to prevent overlap
  - [ ] Align base to BAR size (natural alignment requirement)
- [ ] `pci_map_bar(dev, bar_index)` — return kernel-accessible virtual address
  - [ ] Identity-mapped for now (physical == virtual)
  - [ ] For future VMM: create page table mapping
- [ ] Commit: `"pci: BAR assignment and MMIO mapping"`

---

## Priority Order

| Priority | Section                          | Description                                                   |
|----------|----------------------------------|---------------------------------------------------------------|
| 🔴 P0    | 1.1 Proper Device Enumeration    | Foundation — all drivers need device discovery                 |
| 🔴 P0    | 1.2 Device Lookup API            | Every driver uses `pci_find_device` / `pci_find_class`         |
| 🔴 P0    | 2.1 Capabilities List Walker     | Required for MSI, MSI-X, PM, PCIe detection                   |
| 🟠 P1    | 3.1 MSI Support                  | Replace legacy INTx# — eliminates IRQ sharing                  |
| 🟠 P1    | 3.2 MSI-X Support                | Multi-queue interrupts for AHCI, NVMe, NICs                    |
| 🟠 P1    | 5.1 Command Register Management  | Proper device enable/disable, BAR access control               |
| 🟡 P2    | 5.2 Status Register Errors       | Error detection and recovery (Master/Target Abort)             |
| 🟡 P2    | 6.1 Bridge Configuration         | Multi-bus topologies, secondary bus discovery                   |
| 🟡 P2    | 4.1 PCIe ECAM Access             | Extended config space (4096 bytes), AER, SR-IOV                |
| 🟡 P2    | 8.1 BAR Assignment & MMIO        | Resource allocation for unassigned BARs                         |
| 🟢 P3    | 7.1 PCI Power Management         | D-state transitions, suspend/resume                             |

---

## OS Comparison

| Feature                              | 🪟 Windows 11 (PCI.sys)             | 🐧 Linux (drivers/pci/)              | 🚀 Impossible OS                         |
| ------------------------------------ | ----------------------------------- | ------------------------------------- | ---------------------------------------- |
| Config space (I/O ports 0xCF8)       | ✅                                   | ✅                                     | ✅ Done (basic)                           |
| Recursive bus enumeration            | ✅ Full PnP                          | ✅ `pci_scan_bus()`                    | ⚠️ Bus 0 only — §1.1 P0                 |
| Multi-function detection             | ✅                                   | ✅                                     | ✅ Done                                   |
| BAR probing & size detection         | ✅                                   | ✅ `pci_read_bases()`                  | ⬜ §1.1 P0                               |
| Device lookup (class/vendor)         | ✅ PnP manager                       | ✅ `pci_get_device()`                  | ⚠️ Vendor/ID only — §1.2 P0             |
| Capabilities list parser             | ✅                                   | ✅ `pci_find_capability()`             | ⬜ §2.1 P0                               |
| MSI support                          | ✅ Full                              | ✅ `pci_enable_msi()`                  | ⬜ §3.1 P1                               |
| MSI-X (2048 vectors)                 | ✅ Full                              | ✅ `pci_enable_msix_range()`           | ⬜ §3.2 P1                               |
| PCIe ECAM (4K config)                | ✅ via MCFG                          | ✅ `pci_mmcfg_init()`                  | ⬜ §4.1 P2                               |
| PCIe extended capabilities           | ✅ AER, LTR, SR-IOV                  | ✅ `pci_find_ext_capability()`         | ⬜ §4.1 P2                               |
| Command register control             | ✅                                   | ✅ `pci_set_master()`                  | ⚠️ Bus master only — §5.1 P1            |
| Error reporting (PERR/SERR)          | ✅ WER integration                   | ✅ AER driver                          | ⬜ §5.2 P2                               |
| PCI-to-PCI bridge support            | ✅ Full hierarchy                    | ✅ `pci_scan_bridge()`                 | ⬜ §6.1 P2                               |
| PCI power management (D-states)      | ✅ ACPI + PCI PM                     | ✅ `pci_set_power_state()`             | ⬜ §7.1 P3                               |
| BAR resource allocation              | ✅ PnP Manager + arbiter             | ✅ `pci_assign_resource()`             | ⬜ §8.1 P2                               |
| **INTx# legacy pin interrupts**     | ✅ (fallback)                        | ✅ (fallback)                          | ✅ **Current mechanism (Interrupt Line)** |
| **Hot-plug support**                 | ✅ Full                              | ✅ `pciehp` driver                     | ⬜ Not planned yet                        |
