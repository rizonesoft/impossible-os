# PCI Express (PCIe) -- Technical Specification for OS Implementation

## Overview and Architectural Context

PCI Express (PCIe) is the dominant high-speed serial interconnect standard for modern computing
platforms, replacing the shared parallel bus architecture of conventional PCI with a point-to-point,
switched, packet-based topology. Introduced in 2003 by the PCI Special Interest Group (PCI-SIG)
under collaborative development by Intel, Dell, HP, and IBM, PCIe serves as the primary interface
between the CPU/memory subsystem and all major peripherals: NVMe storage controllers, Network
Interface Controllers (NICs), Graphics Processing Units (GPUs), and USB host controllers.

For a bare-metal OS like Impossible OS, the PCIe bus driver is the foundational subsystem that all
hardware drivers depend upon. It is responsible for discovering devices, allocating memory resources,
configuring interrupts, and providing an internal kernel API used by every downstream device driver.

### Version History and Compatibility

All PCIe generations are fully backward-compatible in both link negotiation and software interfaces.
A PCIe 5.0 device in a PCIe 3.0 slot will auto-negotiate to Gen 3 speeds.

| Version  | Year | Encoding     | Transfer Rate | x1 Throughput | x16 Throughput |
| -------- | ---- | ------------ | ------------- | ------------- | -------------- |
| PCIe 1.0 | 2003 | 8b/10b       | 2.5 GT/s      | 250 MB/s      | 4 GB/s         |
| PCIe 1.1 | 2005 | 8b/10b       | 2.5 GT/s      | 250 MB/s      | 4 GB/s         |
| PCIe 2.0 | 2007 | 8b/10b       | 5.0 GT/s      | 500 MB/s      | 8 GB/s         |
| PCIe 3.0 | 2010 | 128b/130b    | 8.0 GT/s      | ~1 GB/s       | ~16 GB/s       |
| PCIe 4.0 | 2017 | 128b/130b    | 16.0 GT/s     | ~2 GB/s       | ~32 GB/s       |
| PCIe 5.0 | 2019 | 128b/130b    | 32.0 GT/s     | ~4 GB/s       | ~64 GB/s       |
| PCIe 6.0 | 2022 | 1b/1b (PAM4) | 64.0 GT/s     | ~8 GB/s       | ~128 GB/s      |

> [!NOTE]
> Throughput values are unidirectional. PCIe lanes are full-duplex (simultaneous TX + RX),
> so total bidirectional bandwidth is double the values shown. The `~` prefix indicates that
> 128b/130b encoding yields slightly less than a clean power-of-two ratio.

---

## Physical Layer and Link Architecture

### Serial Differential Signaling

Each PCIe lane consists of two differential signal pairs: one for transmit (TX+/TX−) and one for
receive (RX+/RX−). This dual-simplex arrangement enables simultaneous bidirectional communication
without contention or turnaround cycles.

Devices negotiate their link width during the Link Training and Status State Machine (LTSSM)
process. Common link widths are x1, x2, x4, x8, x12, and x16. A device designed for x16 can
operate in a narrower slot (e.g., x4) by negotiating down to the available width.

### Protocol Layer Stack

PCIe implements a three-layer protocol stack, analogous to network protocols:

| Layer             | Function                                                              |
| ----------------- | --------------------------------------------------------------------- |
| Transaction Layer | Creates/parses Transaction Layer Packets (TLPs) for reads, writes     |
| Data Link Layer   | Adds sequence numbers, LCRC; handles ACK/NAK and packet replay        |
| Physical Layer    | Electrical signaling, 8b/10b or 128b/130b encoding, lane bonding      |

The OS interacts exclusively with the Transaction Layer by triggering TLP creation through memory
reads/writes to device-mapped addresses. The Data Link Layer provides automatic error recovery
via CRC checking and replay -- transparent to software.

### Transaction Layer Packets (TLPs)

All data movement across the PCIe fabric uses TLPs. Key TLP types relevant to OS drivers:

| TLP Type                   | Direction       | Purpose                                          |
| -------------------------- | --------------- | ------------------------------------------------ |
| Memory Read (MRd)          | Requester → RC  | CPU reads device MMIO register                   |
| Memory Write (MWr)         | Requester → RC  | CPU writes device MMIO register; MSI delivery    |
| Completion (Cpl/CplD)      | Completer → Req | Response to a read request (with or without data) |
| Configuration Read (CfgRd) | RC → Device     | Read device configuration space                  |
| Configuration Write (CfgWr)| RC → Device     | Write device configuration space                 |
| Message (Msg/MsgD)         | Various         | In-band signaling (INTx emulation, PME, errors)  |

> [!IMPORTANT]
> Memory Write TLPs are **posted** -- the requester does not wait for a completion. Memory Read
> TLPs are **non-posted** -- the CPU stalls until a Completion TLP returns. This asymmetry is
> critical for driver performance: minimize MMIO reads, prefer writes.

---

## Hardware Topology

### Root Complex, Switches, and Endpoints

The PCIe topology forms a strict hierarchical tree:

```
         ┌──────────────┐
         │ Root Complex  │ ← CPU + Memory Controller
         └──┬───────┬───┘
            │       │
        ┌───┴──┐ ┌──┴───┐
        │Root  │ │Root  │ ← Root Ports (Type 1 headers)
        │Port 0│ │Port 1│
        └──┬───┘ └──┬───┘
           │        │
      ┌────┴────┐   │
      │  Switch  │   │
      │(virtual │   │
      │bridges) │   │
      └─┬────┬──┘   │
        │    │      │
     ┌──┴┐ ┌┴──┐ ┌─┴──┐
     │EP │ │EP │ │EP  │ ← Endpoints (Type 0 headers)
     │NVMe│ │NIC│ │GPU │
     └───┘ └───┘ └────┘
```

**Root Complex (RC):** Bridges CPU/memory to the PCIe fabric. Translates CPU memory accesses into
outbound TLPs. Receives inbound DMA TLPs and directs them to system RAM.

**Switches:** Multi-port packet routers. Each port appears to software as a virtual PCI-to-PCI
bridge (Type 1 configuration header). An upstream port connects toward the RC; downstream ports
connect to endpoints or further switches.

**Endpoints:** Terminal devices providing actual functionality (NVMe, NIC, GPU, USB xHCI). These
present Type 0 configuration headers.

> [!NOTE]
> The OS views each switch port as an independent bridge device. A 4-port switch appears as
> 1 upstream bridge + 3 downstream bridges in the enumeration tree.

---

## Firmware Dependencies: ACPI Tables

### MCFG -- Memory Mapped Configuration Table

The ECAM base address is platform-specific and must be obtained from firmware. On x86 UEFI/ACPI
systems, the ACPI MCFG table provides this information.

The MCFG table (signature `"MCFG"`) contains one or more allocation structures after the standard
ACPI header (36 bytes) and 8 bytes of reserved padding:

```c
struct mcfg_allocation {
    uint64_t base_address;       /* Physical base of ECAM region */
    uint16_t segment_group;      /* PCI segment group number     */
    uint8_t  start_bus;          /* First bus number covered      */
    uint8_t  end_bus;            /* Last bus number covered       */
    uint32_t reserved;
};
```

All fields are little-endian. The number of entries is calculated from the MCFG table length:
`count = (header.length - 44) / sizeof(struct mcfg_allocation)`.

> [!CAUTION]
> The 8-byte reserved field between the ACPI header and the first allocation entry is mandatory.
> Skipping it causes all subsequent entries to be parsed at incorrect offsets.

### ACPI _PRT -- Interrupt Routing

For legacy INTx interrupt routing (fallback only), the ACPI namespace provides `_PRT` (PCI Routing
Table) objects under each PCI host bridge device. These map device pin assertions to Global System
Interrupt (GSI) numbers. MSI/MSI-X bypasses `_PRT` entirely.

### ACPI DMAR -- DMA Remapping (Intel VT-d)

The DMAR table provides IOMMU hardware unit locations and reserved memory regions. See §12 for
IOMMU details.

---

## Configuration Space and ECAM

### Legacy Configuration Access (x86 Only)

Legacy PCI configuration access uses I/O ports `0xCF8` (address) and `0xCFC` (data), limited to
the first 256 bytes of configuration space. This mechanism is x86-specific and cannot access the
extended configuration space (offsets `0x100`–`0xFFF`).

```c
/* Legacy Mechanism 1 -- x86 only, first 256 bytes only */
#define PCI_CONFIG_ADDR  0x0CF8
#define PCI_CONFIG_DATA  0x0CFC

static uint32_t pci_legacy_read(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)dev  << 11)
                  | ((uint32_t)func << 8)
                  | (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}
```

> [!WARNING]
> Legacy I/O port access is **not available** on ARM64 or other RISC architectures. It cannot
> access offsets ≥ `0x100`. Use ECAM exclusively for cross-platform compatibility.

### Enhanced Configuration Access Mechanism (ECAM)

ECAM maps the entire 4 KiB configuration space of every device function directly into physical
memory. This allows configuration registers to be accessed via standard memory read/write
instructions on any architecture.

**Address calculation:**

```
physical_addr = ecam_base
              + ((bus - start_bus) << 20)
              + (device << 15)
              + (function << 12)
              + register_offset
```

Where:
- `ecam_base` and `start_bus` come from the MCFG allocation entry
- `bus` is 8 bits (0–255), `device` is 5 bits (0–31), `function` is 3 bits (0–7)
- `register_offset` is 12 bits (0–4095)

**Memory consumption:** A full 256-bus segment requires 256 × 32 × 8 × 4096 = **256 MiB** of
contiguous physical address space.

> [!CAUTION]
> The ECAM region **must** be mapped as strongly-ordered, uncacheable memory (e.g., `PCD=1, PWT=1`
> in x86 page tables, or Device-nGnRnE on ARM64). Caching configuration register reads will cause
> the CPU to return stale data, leading to catastrophic driver desynchronization.

```c
/* ECAM access -- architecture-independent */
static volatile void *ecam_base_virt;  /* Kernel virtual mapping of ECAM region */

static inline uint32_t pcie_cfg_read32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset) {
    uintptr_t addr = (uintptr_t)ecam_base_virt
                   + ((uint32_t)bus  << 20)
                   + ((uint32_t)dev  << 15)
                   + ((uint32_t)func << 12)
                   + offset;
    return *(volatile uint32_t *)addr;
}
```

---

## Configuration Space Layout

### Type 0 Header (Endpoints) -- Offsets 0x00–0x3F

| Offset | Size | Register                  | Description                                      |
| ------ | ---- | ------------------------- | ------------------------------------------------ |
| 0x00   | 2    | `Vendor ID`               | PCI-SIG assigned vendor identifier               |
| 0x02   | 2    | `Device ID`               | Vendor-assigned device identifier                 |
| 0x04   | 2    | `Command`                 | I/O, Memory, Bus Master enable bits               |
| 0x06   | 2    | `Status`                  | Capabilities list flag (bit 4), error flags        |
| 0x08   | 1    | `Revision ID`             | Device revision                                  |
| 0x09   | 3    | `Class Code`              | Programming IF, Subclass, Base Class              |
| 0x0C   | 1    | `Cache Line Size`         | System cache line size in DWORDs                  |
| 0x0D   | 1    | `Latency Timer`           | Legacy PCI latency timer (hardwired to 0 in PCIe) |
| 0x0E   | 1    | `Header Type`             | Bits 6:0 = type (0x00); Bit 7 = multi-function    |
| 0x0F   | 1    | `BIST`                    | Built-in self-test control/status                 |
| 0x10   | 4    | `BAR 0`                   | Base Address Register 0                           |
| 0x14   | 4    | `BAR 1`                   | Base Address Register 1                           |
| 0x18   | 4    | `BAR 2`                   | Base Address Register 2                           |
| 0x1C   | 4    | `BAR 3`                   | Base Address Register 3                           |
| 0x20   | 4    | `BAR 4`                   | Base Address Register 4                           |
| 0x24   | 4    | `BAR 5`                   | Base Address Register 5                           |
| 0x28   | 4    | `Cardbus CIS Pointer`     | Legacy CardBus (unused in PCIe)                   |
| 0x2C   | 2    | `Subsystem Vendor ID`     | Subsystem vendor identifier                       |
| 0x2E   | 2    | `Subsystem ID`            | Subsystem identifier                              |
| 0x30   | 4    | `Expansion ROM BAR`       | Expansion ROM base address                        |
| 0x34   | 1    | `Capabilities Pointer`    | Offset to first capability in linked list          |
| 0x35   | 3    | Reserved                  | --                                                |
| 0x38   | 4    | Reserved                  | --                                                |
| 0x3C   | 1    | `Interrupt Line`          | System IRQ (written by firmware/OS)                |
| 0x3D   | 1    | `Interrupt Pin`           | INTx pin used (1=A, 2=B, 3=C, 4=D, 0=none)       |
| 0x3E   | 1    | `Min Grant`               | Legacy (hardwired to 0 in PCIe)                   |
| 0x3F   | 1    | `Max Latency`             | Legacy (hardwired to 0 in PCIe)                   |

### Type 1 Header (Bridges) -- Offsets 0x00–0x3F

| Offset | Size | Register                  | Description                                       |
| ------ | ---- | ------------------------- | ------------------------------------------------- |
| 0x00   | 2    | `Vendor ID`               | PCI-SIG assigned vendor identifier                |
| 0x02   | 2    | `Device ID`               | Vendor-assigned device identifier                  |
| 0x04   | 2    | `Command`                 | I/O, Memory, Bus Master enable bits                |
| 0x06   | 2    | `Status`                  | Capabilities list flag (bit 4), error flags         |
| 0x08   | 1    | `Revision ID`             | Device revision                                   |
| 0x09   | 3    | `Class Code`              | Bridge class: `0x060400`                           |
| 0x0C   | 1    | `Cache Line Size`         | System cache line size in DWORDs                   |
| 0x0D   | 1    | `Latency Timer`           | Legacy (hardwired to 0 in PCIe)                    |
| 0x0E   | 1    | `Header Type`             | `0x01` for bridge; Bit 7 = multi-function           |
| 0x0F   | 1    | `BIST`                    | Built-in self-test                                 |
| 0x10   | 4    | `BAR 0`                   | Bridge BAR 0                                      |
| 0x14   | 4    | `BAR 1`                   | Bridge BAR 1                                      |
| 0x18   | 1    | `Primary Bus Number`      | Bus number of upstream side                        |
| 0x19   | 1    | `Secondary Bus Number`    | Bus number of downstream side                      |
| 0x1A   | 1    | `Subordinate Bus Number`  | Highest bus number downstream of this bridge        |
| 0x1B   | 1    | `Secondary Latency Timer` | Legacy (hardwired to 0 in PCIe)                    |
| 0x1C   | 1    | `I/O Base`                | Lower 8 bits of I/O aperture base                  |
| 0x1D   | 1    | `I/O Limit`               | Lower 8 bits of I/O aperture limit                 |
| 0x1E   | 2    | `Secondary Status`        | Status for secondary interface                     |
| 0x20   | 2    | `Memory Base`             | Bits 31:20 of non-prefetchable MMIO base           |
| 0x22   | 2    | `Memory Limit`            | Bits 31:20 of non-prefetchable MMIO limit          |
| 0x24   | 2    | `Prefetchable Memory Base`| Bits 31:20 of prefetchable MMIO base               |
| 0x26   | 2    | `Prefetchable Memory Lim` | Bits 31:20 of prefetchable MMIO limit              |
| 0x28   | 4    | `Prefetch Base Upper 32`  | Upper 32 bits of prefetchable base (64-bit)        |
| 0x2C   | 4    | `Prefetch Limit Upper 32` | Upper 32 bits of prefetchable limit (64-bit)       |
| 0x30   | 2    | `I/O Base Upper 16`       | Upper 16 bits of I/O base (32-bit I/O)             |
| 0x32   | 2    | `I/O Limit Upper 16`      | Upper 16 bits of I/O limit (32-bit I/O)            |
| 0x34   | 1    | `Capabilities Pointer`    | Offset to first capability                         |
| 0x35   | 3    | Reserved                  | --                                                 |
| 0x38   | 4    | `Expansion ROM BAR`       | Bridge expansion ROM                               |
| 0x3C   | 1    | `Interrupt Line`          | System IRQ                                         |
| 0x3D   | 1    | `Interrupt Pin`           | INTx pin                                           |
| 0x3E   | 2    | `Bridge Control`          | Secondary bus reset (bit 6), ISA enable, etc.      |

### Command Register (Offset 0x04) -- Bit Fields

| Bit   | Name                     | Description                                          |
| ----- | ------------------------ | ---------------------------------------------------- |
| 0     | I/O Space Enable         | Responds to I/O BAR accesses                         |
| 1     | Memory Space Enable      | Responds to Memory BAR accesses                      |
| 2     | Bus Master Enable        | Can generate memory read/write TLPs (DMA)            |
| 3     | Special Cycles           | Legacy (hardwired to 0 in PCIe)                      |
| 4     | MWI Enable               | Legacy (hardwired to 0 in PCIe)                      |
| 5     | VGA Palette Snoop        | Legacy (hardwired to 0 in PCIe)                      |
| 6     | Parity Error Response    | Enable PERR# signaling                               |
| 8     | SERR# Enable             | Enable SERR# for fatal/non-fatal errors              |
| 10    | INTx Disable             | Disable legacy INTx interrupt assertion               |

> [!IMPORTANT]
> **Bus Master Enable (bit 2)** must be set before a device can perform DMA. Forgetting this bit
> is one of the most common PCIe driver bugs -- the device silently drops all DMA TLPs.

---

## Bus Enumeration Algorithm

### Recursive Depth-First Scan

The OS enumerates the PCIe tree starting at Bus 0, Device 0, Function 0 using a recursive
depth-first search:

1. Read `Vendor ID` (offset 0x00) of the target BDF
2. If `Vendor ID == 0xFFFF` → no device present; skip
3. Read `Header Type` (offset 0x0E):
   - Bits 6:0 = `0x00` → Endpoint (Type 0). Record device, size BARs, continue
   - Bits 6:0 = `0x01` → Bridge (Type 1). Configure bridge, recurse into secondary bus
   - Bit 7 = 1 → Multi-function device; scan functions 1–7
4. For single-function devices (bit 7 of `Header Type` = 0), only scan function 0

**Bridge configuration during enumeration:**

```
for each bridge discovered:
    1. Write current_bus to Primary Bus Number (0x18)
    2. Assign next_free_bus to Secondary Bus Number (0x19)
    3. Temporarily set Subordinate Bus Number (0x1A) to 0xFF
    4. Recursively enumerate the secondary bus
    5. Update Subordinate Bus Number to the highest bus discovered downstream
```

> [!CAUTION]
> Always check bit 7 of `Header Type` on function 0 of every device. If clear, **do not scan**
> functions 1–7 -- reading non-existent functions on some hardware causes undefined behavior
> (platform hangs, machine check exceptions).

### Hotplug Bus Padding

For hotplug-capable bridge ports, the OS should reserve spare bus numbers and MMIO address space:

- Reserve 4–10 extra bus numbers beyond what is currently populated
- Reserve 128–256 MiB of unused MMIO window behind each hotplug bridge
- Set `Subordinate Bus Number` to include the reserved range

This allows hot-plugged devices to be enumerated into the reserved space without a system-wide
re-enumeration.

---

## Base Address Registers (BARs)

### BAR Layout

Type 0 headers contain six 32-bit BARs at offsets `0x10`–`0x24`. Type 1 headers contain two
BARs at offsets `0x10`–`0x14`.

**BAR bit fields (Memory BAR):**

| Bits  | Field          | Description                                             |
| ----- | -------------- | ------------------------------------------------------- |
| 0     | Space Type     | 0 = Memory, 1 = I/O                                    |
| 2:1   | Type           | 00 = 32-bit, 10 = 64-bit (consumes next BAR too)       |
| 3     | Prefetchable   | 1 = reads have no side effects; bridge may prefetch     |
| 31:4  | Base Address   | Aligned base address (lower 4 bits always 0)            |

**BAR bit fields (I/O BAR):**

| Bits  | Field          | Description                                             |
| ----- | -------------- | ------------------------------------------------------- |
| 0     | Space Type     | 1 = I/O                                                |
| 1     | Reserved       | --                                                      |
| 31:2  | Base Address   | Aligned I/O base address                                |

### BAR Sizing Algorithm

To determine the size of memory a device requires:

```c
/* Step 1: Save original BAR value */
uint32_t original = pcie_cfg_read32(bus, dev, func, bar_offset);

/* Step 2: Decode BAR type from lower bits */
bool is_io      = (original & 0x1);
bool is_64bit   = !is_io && ((original >> 1) & 0x3) == 0x2;
bool prefetch   = !is_io && ((original >> 3) & 0x1);

/* Step 3: Write all-ones to probe size */
pcie_cfg_write32(bus, dev, func, bar_offset, 0xFFFFFFFF);

/* Step 4: Read back mask */
uint32_t mask = pcie_cfg_read32(bus, dev, func, bar_offset);

/* Step 5: Restore original value */
pcie_cfg_write32(bus, dev, func, bar_offset, original);

/* Step 6: Calculate size */
if (is_io) {
    mask &= 0xFFFFFFFC;   /* Clear type bits */
} else {
    mask &= 0xFFFFFFF0;   /* Clear type/prefetch bits */
}
uint32_t size = (~mask) + 1;
```

For 64-bit BARs, perform the same probe on the next consecutive BAR register to get the upper
32 bits of the mask. Combine both halves into a 64-bit size value.

> [!CAUTION]
> **Disable interrupts and bus mastering** before probing BARs. While the BAR contains
> `0xFFFFFFFF`, the device's MMIO region is temporarily unmapped. If a DMA transfer or interrupt
> handler attempts to access the device during this window, the result is undefined.

---

## Capabilities Linked List

### Standard Capabilities (Offsets 0x00–0xFF)

If bit 4 of the `Status` register (offset 0x06) is set, the device supports capabilities. The
`Capabilities Pointer` (offset 0x34) gives the byte offset to the first capability node.

Each capability node has a 2-byte header:

| Byte | Field          | Description                                    |
| ---- | -------------- | ---------------------------------------------- |
| +0   | Capability ID  | PCI-SIG assigned ID for this capability         |
| +1   | Next Pointer   | Offset to next capability (0x00 = end of list)  |

**Standard Capability IDs relevant to OS drivers:**

| ID     | Name                           | Description                                  |
| ------ | ------------------------------ | -------------------------------------------- |
| `0x01` | PCI Power Management (PCI-PM)  | D-state transitions (D0–D3), PME wake events |
| `0x05` | Message Signaled Interrupts    | MSI: 1–32 vectors, shared address            |
| `0x10` | PCI Express Capability         | Link speed/width, MPS, MRRS negotiation      |
| `0x11` | MSI-X                          | Up to 2048 independent vectors, BAR table    |

### Extended Capabilities (Offsets 0x100–0xFFF)

Extended capabilities begin at offset `0x100` and use a 4-byte header:

| Bits   | Field                    | Description                               |
| ------ | ------------------------ | ----------------------------------------- |
| 15:0   | Extended Capability ID   | PCI-SIG assigned extended ID              |
| 19:16  | Capability Version       | Version of this capability structure       |
| 31:20  | Next Capability Offset   | Offset to next extended cap (0 = end)      |

**Key Extended Capability IDs:**

| ID       | Name                                | Description                            |
| -------- | ----------------------------------- | -------------------------------------- |
| `0x0001` | Advanced Error Reporting (AER)      | Granular error classification/logging  |
| `0x0002` | Virtual Channel (VC)                | Traffic class to VC mapping            |
| `0x0003` | Device Serial Number                | Unique 64-bit device serial number     |
| `0x0004` | Power Budgeting                     | Device power budget reporting          |
| `0x000D` | Access Control Services (ACS)       | P2P isolation, IOMMU enforcement       |
| `0x0010` | SR-IOV                              | Single Root I/O Virtualization          |
| `0x0019` | Secondary PCI Express               | Gen 3+ equalization settings           |
| `0x001E` | L1 PM Substates                     | L1.1/L1.2 low-power link states        |

---

## PCIe Capability Structure (ID 0x10)

The PCIe Capability (standard ID `0x10`) is mandatory for all PCIe devices. It provides link
status, device capabilities, and performance tuning registers.

### Key Registers

| Offset | Size | Register               | Description                                        |
| ------ | ---- | ---------------------- | -------------------------------------------------- |
| +0x00  | 1    | Capability ID          | `0x10`                                             |
| +0x01  | 1    | Next Capability Ptr    | Offset to next capability                          |
| +0x02  | 2    | PCIe Capabilities      | Device/Port type, interrupt message number          |
| +0x04  | 4    | Device Capabilities    | Max Payload Size supported, phantom funcs, tag bits |
| +0x08  | 2    | Device Control         | MPS setting, MRRS setting, error reporting enables  |
| +0x0A  | 2    | Device Status          | Correctable/Non-fatal/Fatal error detected flags    |
| +0x0C  | 4    | Link Capabilities      | Max link speed, max link width, ASPM support        |
| +0x10  | 2    | Link Control           | ASPM control, RCB, link disable, retrain            |
| +0x12  | 2    | Link Status            | Current speed, current width, training status       |

### Device/Port Types (PCIe Capabilities Register, bits 7:4)

| Value | Type                            |
| ----- | ------------------------------- |
| 0x0   | PCI Express Endpoint            |
| 0x1   | Legacy PCI Express Endpoint     |
| 0x4   | Root Port of Root Complex       |
| 0x5   | Upstream Port of Switch         |
| 0x6   | Downstream Port of Switch       |
| 0x7   | PCIe to PCI/PCI-X Bridge        |
| 0x8   | PCI/PCI-X to PCIe Bridge        |
| 0x9   | Root Complex Integrated Endpt   |
| 0xA   | Root Complex Event Collector    |

### Maximum Payload Size (MPS) and Maximum Read Request Size (MRRS)

**MPS** controls the largest TLP data payload. Both the Device Capabilities register and Device
Control register contain MPS fields:

- `Device Capabilities` bits 2:0 -- Max MPS Supported (encoded as 2^(value+7) bytes)
- `Device Control` bits 7:5 -- Current MPS (same encoding)

| Encoded Value | MPS (bytes) |
| ------------- | ----------- |
| 0             | 128         |
| 1             | 256         |
| 2             | 512         |
| 3             | 1024        |
| 4             | 2048        |
| 5             | 4096        |

**MRRS** (Device Control bits 14:12) uses the same encoding. MRRS may exceed MPS -- the completer
fragments the response into multiple Completion TLPs of MPS size.

> [!CAUTION]
> **MPS must be set consistently across the entire path** from Root Complex to Endpoint. If any
> node in the path has a lower MPS than the TLP payload, the packet is treated as malformed and
> silently dropped. The OS must traverse the entire tree and set MPS to the **minimum** of all
> devices on each path (or globally, for simplicity).

---

## Interrupt Mechanisms

### Legacy INTx Emulation

PCIe devices can emulate legacy INTx interrupts using in-band Assert_INTx and Deassert_INTx
Message TLPs. These are level-triggered, shared, and require the OS to poll device status
registers to identify the interrupt source -- identical to the conventional PCI performance problem.

**INTx should be disabled** (Command register bit 10 = 1) whenever MSI or MSI-X is active.

### Message Signaled Interrupts (MSI) -- Capability ID 0x05

MSI generates interrupts by performing a Memory Write TLP to the Local APIC address. The OS
programs the Message Address and Message Data registers within the MSI capability structure.

**MSI Capability Structure Layout:**

| Offset | Size | Register             | Description                                   |
| ------ | ---- | -------------------- | --------------------------------------------- |
| +0x00  | 1    | Capability ID        | `0x05`                                        |
| +0x01  | 1    | Next Pointer         | Next capability offset                        |
| +0x02  | 2    | Message Control      | Enable (bit 0), vectors requested/granted      |
| +0x04  | 4    | Message Address      | Target APIC address (lower 32 bits)            |
| +0x08  | 4    | Message Upper Addr   | Upper 32 bits (if 64-bit capable)              |
| +0x08/0x0C | 2 | Message Data       | Interrupt vector value                         |

MSI supports 1, 2, 4, 8, 16, or 32 vectors (power of two). The device modifies the lowest N bits
of Message Data to select the vector within the allocated range.

### MSI-X -- Capability ID 0x11

MSI-X provides up to **2048 independent vectors** per device, each with its own address and data.

**MSI-X Capability Structure:**

| Offset | Size | Register              | Description                                  |
| ------ | ---- | --------------------- | -------------------------------------------- |
| +0x00  | 1    | Capability ID         | `0x11`                                       |
| +0x01  | 1    | Next Pointer          | Next capability offset                       |
| +0x02  | 2    | Message Control       | Table size (bits 10:0), Function Mask, Enable |
| +0x04  | 4    | Table Offset / BIR    | Bits 2:0 = BAR index; bits 31:3 = offset     |
| +0x08  | 4    | PBA Offset / BIR      | Bits 2:0 = BAR index; bits 31:3 = offset     |

**MSI-X Table Entry (16 bytes each, in device BAR memory):**

| Offset | Size | Field                 | Description                                  |
| ------ | ---- | --------------------- | -------------------------------------------- |
| +0x00  | 4    | Message Address Low   | Lower 32 bits of target APIC address         |
| +0x04  | 4    | Message Address High  | Upper 32 bits of target APIC address         |
| +0x08  | 4    | Message Data          | Interrupt vector data                        |
| +0x0C  | 4    | Vector Control        | Bit 0 = Mask (1 = masked)                    |

**Pending Bit Array (PBA):** A read-only bit array where each bit corresponds to one MSI-X table
entry. Bit is set when an interrupt is pending but masked.

> [!IMPORTANT]
> **Initialization priority:** Always prefer MSI-X → MSI → INTx (last resort). MSI-X provides
> per-vector masking, independent address/data per vector, and supports NUMA-aware interrupt
> steering to specific CPU cores.

---

## Data Transfer: MMIO vs DMA

### Memory-Mapped I/O (MMIO)

MMIO maps device control registers (exposed via BARs) into the CPU's physical address space.
CPU reads/writes to these addresses generate Memory Read/Write TLPs.

**Key characteristics:**
- Reads are **synchronous** -- CPU stalls until Completion TLP returns
- Writes are **posted** -- CPU does not wait for acknowledgment
- MMIO pages must be mapped as **strongly-ordered, uncacheable**
- Use only for low-frequency control operations (register writes, doorbell rings)

### Direct Memory Access (DMA)

DMA is device-initiated: the device reads/writes system RAM independently, freeing the CPU.

**DMA setup flow:**
1. OS allocates a buffer in system RAM
2. OS provides the **physical address** of the buffer to the device via MMIO registers
3. Device generates Memory Read/Write TLPs to transfer data autonomously
4. Device signals completion via MSI/MSI-X interrupt

**Scatter-Gather DMA:**
For fragmented physical memory, the OS builds a descriptor ring -- a linked list of
`(physical_address, length)` entries. The device's DMA engine iterates the list, gathering data
from or scattering data across multiple disjoint physical pages.

### Cache Coherency

| Architecture | Coherency Model   | OS Requirement                                      |
| ------------ | ----------------- | --------------------------------------------------- |
| x86 / x86-64 | Hardware-coherent | Automatic: memory controller snoops all PCIe DMA    |
| ARM64        | Non-coherent      | Explicit cache flush before device read; invalidate  |
|              |                   | before CPU read of device-written buffer             |

> [!CAUTION]
> On ARM64 (if ported in future), a DMA-capable driver **must** call explicit cache maintenance
> ops. On x86-64 (Impossible OS's current target), hardware snooping handles coherency
> automatically -- no explicit cache management is needed for DMA buffers.

---

## IOMMU and Access Control (Security)

### Intel VT-d Architecture

The IOMMU sits between the PCIe Root Complex and system RAM, translating I/O Virtual Addresses
(IOVAs) used by devices into physical addresses. This prevents malicious or buggy devices from
performing arbitrary DMA to kernel memory.

**ACPI DMAR Table Structures:**

| Structure | Name                                      | Purpose                              |
| --------- | ----------------------------------------- | ------------------------------------ |
| DRHD      | DMA Remapping Hardware Unit Definition    | IOMMU register base addresses         |
| RMRR      | Reserved Memory Region Reporting          | Legacy devices needing fixed DMA maps |
| ATSR      | Root Port ATS Capability Reporting        | ATS-capable root ports                |

**VT-d Translation Hierarchy:**

```
Root Table (256 entries, indexed by bus)
  └─→ Context Table (256 entries, indexed by devfn)
        └─→ Second-Level Page Tables (4-level, like CPU page tables)
              └─→ Physical page frames
```

**DMA mapping flow:**
1. OS allocates a physical buffer
2. OS maps an IOVA range in the IOMMU page tables for the specific device
3. OS provides the IOVA (not the physical address) to the device
4. Device DMA uses IOVA → IOMMU translates to physical → access granted/denied

> [!WARNING]
> **RMRR regions must be identity-mapped** before enabling IOMMU translation. Legacy firmware
> features (USB keyboard emulation, VGA) perform DMA to fixed physical addresses. Failing to
> map RMRR regions causes immediate device failure.

### Access Control Services (ACS)

ACS (Extended Capability ID `0x000D`) prevents peer-to-peer DMA between devices, which would
bypass the IOMMU. The OS should enable ACS on all switch ports and root ports to force all TLPs
upstream through the IOMMU.

**ACS Control bits to enable:**

| Bit | Name                          | Effect                                    |
| --- | ----------------------------- | ----------------------------------------- |
| 0   | Source Validation              | Validate requester ID                     |
| 1   | Translation Blocking          | Block translated requests from P2P        |
| 2   | P2P Request Redirect          | Redirect P2P requests upstream to RC      |
| 3   | P2P Completion Redirect       | Redirect P2P completions upstream to RC   |
| 4   | Upstream Forwarding           | Forward all requests upstream              |
| 5   | P2P Egress Control            | Control egress of P2P traffic             |
| 6   | Direct Translated P2P         | Control direct-translated P2P             |

---

## Advanced Error Reporting (AER)

AER (Extended Capability ID `0x0001`) provides granular hardware error detection, classification,
and logging -- critical for system reliability.

### Error Classification

| Category       | Severity | Recovery                                              |
| -------------- | -------- | ----------------------------------------------------- |
| Correctable    | Low      | Hardware auto-recovered (LCRC replay); OS logs only   |
| Non-Fatal      | Medium   | Transaction failed; link stable; OS retries command   |
| Fatal          | Critical | Link compromised; requires Secondary Bus Reset        |

### AER Register Layout (at Extended Capability offset)

| Offset | Size | Register                       | Description                              |
| ------ | ---- | ------------------------------ | ---------------------------------------- |
| +0x00  | 4    | Extended Capability Header     | ID = `0x0001`, version, next offset      |
| +0x04  | 4    | Uncorrectable Error Status     | Bit flags for each uncorrectable error   |
| +0x08  | 4    | Uncorrectable Error Mask       | Mask bits (1 = masked)                   |
| +0x0C  | 4    | Uncorrectable Error Severity   | 0 = non-fatal, 1 = fatal                |
| +0x10  | 4    | Correctable Error Status       | Bit flags for each correctable error     |
| +0x14  | 4    | Correctable Error Mask         | Mask bits                                |
| +0x18  | 4    | Advanced Error Capabilities    | ECRC generation/check, first error ptr   |
| +0x1C  | 4×4  | Header Log                     | First 16 bytes of offending TLP          |
| +0x2C  | 4    | Root Error Command             | (Root ports only) reporting enables      |
| +0x30  | 4    | Root Error Status              | (Root ports only) error received flags   |
| +0x34  | 2    | Error Source Identification    | Requester ID of error source             |
| +0x38  | 2    | Correctable Source ID          | Requester ID of correctable error source |

**OS error handling flow:**
1. AER interrupt fires (MSI/MSI-X vector assigned to RC error reporting)
2. Read Root Error Status to determine error type
3. Read Error Source Identification for the offending device BDF
4. For correctable: log event, increment counter, monitor for escalation
5. For non-fatal uncorrectable: notify device driver; driver retries I/O
6. For fatal uncorrectable: assert Secondary Bus Reset (Bridge Control bit 6); after reset,
   re-enumerate and re-initialize the affected device

---

## Power Management (PCI-PM)

### D-State Transitions

The PCI Power Management capability (ID `0x01`) defines operational power states:

| State  | Description                                         | Resume Latency |
| ------ | --------------------------------------------------- | -------------- |
| D0     | Fully operational, all features active               | 0              |
| D1     | Optional light sleep (rarely implemented)            | Fast           |
| D2     | Optional deeper sleep (rarely implemented)           | Moderate       |
| D3hot  | Main logic powered down, link maintained             | ≤ 10 ms        |
| D3cold | Vcc removed, only Vaux (if present) remains          | ≤ 100 ms       |

**Power Management Control/Status Register** (within PCI-PM capability):

| Bits  | Field          | Description                                          |
| ----- | -------------- | ---------------------------------------------------- |
| 1:0   | Power State    | 00 = D0, 01 = D1, 10 = D2, 11 = D3hot               |
| 3     | No Soft Reset  | 1 = device preserves config across D3hot → D0        |
| 8     | PME Enable     | Enable Power Management Event signaling               |
| 15    | PME Status     | PME asserted; write 1 to clear                        |

> [!NOTE]
> After transitioning a device from D3hot → D0, the OS must wait at least 10 ms before
> accessing the device. After D3cold → D0, wait at least 100 ms. These are minimum recovery
> times defined by the PCIe specification.

---

## Internal Kernel API Design

The PCIe bus driver should expose a standardized API to device-specific drivers, abstracting ECAM
access, BAR math, and interrupt configuration.

### Driver Registration Model

```c
struct pci_device_id {
    uint16_t vendor_id;      /* PCI_ANY_ID (0xFFFF) = match any */
    uint16_t device_id;
    uint16_t subvendor_id;
    uint16_t subdevice_id;
    uint32_t class_code;     /* 24-bit class code, masked */
    uint32_t class_mask;
};

struct pci_driver {
    const char           *name;
    const struct pci_device_id *id_table;
    int  (*probe)(struct pci_device *dev);   /* Called on match */
    void (*remove)(struct pci_device *dev);  /* Called on removal/shutdown */
    int  (*err_handler)(struct pci_device *dev, enum pci_error_type error);
};

/* Registration */
int pci_register_driver(struct pci_driver *drv);
void pci_unregister_driver(struct pci_driver *drv);
```

### Resource Management API

| Function                   | Purpose                                                  |
| -------------------------- | -------------------------------------------------------- |
| `pci_enable_device(dev)`   | Set Memory Space + Bus Master bits in Command register   |
| `pci_disable_device(dev)`  | Clear enable bits                                        |
| `pci_request_regions(dev)` | Reserve BAR physical memory ranges (prevent conflicts)   |
| `pci_release_regions(dev)` | Release reserved BAR regions                             |
| `pci_iomap(dev, bar, len)` | Map BAR physical address into kernel virtual memory       |
| `pci_iounmap(dev, addr)`   | Unmap BAR virtual address                                |
| `pci_read_config_byte()`   | Read 8/16/32 bits from config space                      |
| `pci_write_config_byte()`  | Write 8/16/32 bits to config space                       |

### Interrupt API

| Function                           | Purpose                                          |
| ---------------------------------- | ------------------------------------------------ |
| `pci_enable_msix(dev, entries, n)` | Allocate N MSI-X vectors                         |
| `pci_enable_msi(dev)`              | Allocate MSI vectors                             |
| `pci_disable_msix(dev)`            | Release MSI-X vectors                            |
| `pci_irq_vector(dev, nr)`         | Get IRQ number for vector `nr`                   |

### DMA API

| Function                                  | Purpose                                     |
| ----------------------------------------- | ------------------------------------------- |
| `pci_alloc_consistent(dev, size, &dma_h)` | Allocate coherent (uncacheable) DMA buffer  |
| `pci_free_consistent(dev, size, va, dma)` | Free coherent DMA buffer                    |
| `pci_map_single(dev, va, size, dir)`      | Map streaming DMA buffer (returns IOVA/phys) |
| `pci_unmap_single(dev, dma, size, dir)`   | Unmap streaming DMA buffer                   |
| `pci_set_dma_mask(dev, mask)`             | Set device DMA address width (32/64 bit)     |

---

## QEMU Testing Configuration

### Basic PCIe Bus

```bash
qemu-system-x86_64 \
    -machine q35 \
    -cpu qemu64 \
    -m 256M \
    -drive file=build/system-disk.img,format=raw,if=none,id=disk0 \
    -device virtio-blk-pci,drive=disk0 \
    -serial stdio
```

> [!IMPORTANT]
> Use `-machine q35` for PCIe support. The default `i440fx` machine provides only conventional
> PCI. The Q35 chipset emulates an ICH9 southbridge with PCIe root ports.

### Multiple Devices on PCIe Bus

```bash
qemu-system-x86_64 \
    -machine q35 \
    -m 512M \
    -drive file=build/system-disk.img,format=raw,if=none,id=disk0 \
    -device virtio-blk-pci,drive=disk0,bus=pcie.0,addr=0x04 \
    -device e1000e,netdev=net0,bus=pcie.0,addr=0x05 \
    -netdev user,id=net0 \
    -device virtio-rng-pci,bus=pcie.0,addr=0x06 \
    -serial stdio
```

### PCIe Topology with Switches (Root Ports)

```bash
qemu-system-x86_64 \
    -machine q35 \
    -m 512M \
    -device pcie-root-port,id=rp1,slot=1,chassis=1 \
    -device pcie-root-port,id=rp2,slot=2,chassis=2 \
    -drive file=build/system-disk.img,format=raw,if=none,id=disk0 \
    -device virtio-blk-pci,drive=disk0,bus=rp1 \
    -device e1000e,netdev=net0,bus=rp2 \
    -netdev user,id=net0 \
    -serial stdio
```

### NVMe on PCIe (Storage Workload Testing)

```bash
qemu-system-x86_64 \
    -machine q35 \
    -m 512M \
    -drive file=build/system-disk.img,format=raw,if=none,id=boot \
    -device virtio-blk-pci,drive=boot \
    -drive file=/tmp/nvme-test.img,format=raw,if=none,id=nvme0 \
    -device nvme,serial=deadbeef,drive=nvme0 \
    -serial stdio
```

### IOMMU (Intel VT-d) Testing

```bash
qemu-system-x86_64 \
    -machine q35,kernel-irqchip=split \
    -device intel-iommu,intremap=on \
    -m 512M \
    -drive file=build/system-disk.img,format=raw,if=none,id=disk0 \
    -device virtio-blk-pci,drive=disk0 \
    -serial stdio
```

---

## Implementation Priorities for Impossible OS

| Priority | Feature                              | Rationale                                        |
| -------- | ------------------------------------ | ------------------------------------------------ |
| 🔴 P0    | ECAM discovery (MCFG parsing)        | Required for all PCIe device access              |
| 🔴 P0    | Bus enumeration (recursive DFS)      | Required to discover any device                  |
| 🔴 P0    | BAR sizing and allocation            | Required for MMIO access to any device           |
| 🔴 P0    | Command register enable (BM + MEM)   | Required for DMA and MMIO                        |
| 🔴 P0    | MSI-X support                        | Required for high-performance interrupt delivery |
| 🟠 P1    | MSI fallback                         | Fallback for devices without MSI-X               |
| 🟠 P1    | Capability list traversal            | Required for MSI/MSI-X/PM discovery              |
| 🟠 P1    | MPS/MRRS tuning                      | Significant throughput impact (10%+ gains)       |
| 🟠 P1    | Power management (D-states)          | Required for proper shutdown/sleep               |
| 🟡 P2    | AER subsystem                        | Resilience against hardware errors               |
| 🟡 P2    | Hotplug support (bus padding)        | Dynamic device addition                          |
| 🟡 P2    | Legacy I/O port fallback             | Compatibility with pre-ECAM BIOS                 |
| 🟢 P3    | IOMMU (Intel VT-d)                   | Security: DMA isolation                          |
| 🟢 P3    | ACS enforcement                      | Security: P2P isolation                          |
| 🔵 P4    | SR-IOV                               | Virtualization support                           |
| 🔵 P4    | Hotplug event handling               | Enterprise use case                              |

> [!NOTE]
> Impossible OS already has a basic PCI driver at `src/kernel/drivers/pci.c` with legacy I/O
> port access and bus enumeration. The PCIe upgrade path should extend this with ECAM support,
> MSI-X, and the standardized kernel API described above.
