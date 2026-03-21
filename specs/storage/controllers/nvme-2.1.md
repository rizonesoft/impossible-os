# NVMe 2.1 — Technical Specification for OS Implementation

## Overview and Architectural Context

Non-Volatile Memory Express (NVMe) is a high-performance host controller interface and storage protocol
designed explicitly for solid-state drives connected via the PCI Express (PCIe) bus. Unlike the legacy
Advanced Host Controller Interface (AHCI), which was originally conceived for rotational magnetic media
with high-latency, single-threaded mechanical operations, NVMe was engineered from inception for the
massively parallel internal architecture of NAND flash memory.

The NVMe specification is maintained by NVM Express, Inc.
(previously a working group under the INCITS T10 technical committee). The protocol operates at the
PCIe transport layer, eliminating the intermediate SATA HBA that AHCI requires. Data transfers occur
directly between the NVMe controller silicon and host system memory via bus-mastering DMA, with command
submission and completion managed through paired circular queues residing in pinned host memory.

### AHCI vs NVMe: Architectural Comparison

| Property                     | AHCI (SATA)                           | NVMe (PCIe)                                |
| ---------------------------- | ------------------------------------- | ------------------------------------------ |
| Maximum I/O queues           | 1 per port                            | 65,535 per controller                      |
| Maximum commands per queue   | 32                                    | 65,536                                     |
| Command submission           | Register-based (PxCI write)           | Doorbell write (single MMIO store)         |
| Data transfer mechanism      | PRDT scatter-gather                   | PRP / SGL lists                            |
| Interrupt support            | INTx, MSI                             | MSI-X (up to 2,048 vectors)               |
| Bus interface                | SATA III (6 Gb/s)                     | PCIe Gen3/4/5 (×1 to ×4 lanes)            |
| CPU overhead per I/O         | ~6 μs (register reads required)       | < 1 μs (single doorbell write)             |
| Multi-core optimization      | None (single queue, requires locking) | Per-core queue pairs (lock-free)           |

> [!IMPORTANT]
> Impossible OS already has a working AHCI driver. The NVMe driver is a new addition that will
> coexist with AHCI via the existing `blkdev` abstraction layer. Both drivers implement the same
> `block_device_t` interface, allowing the VFS to use either transparently.

### Version History and Compatibility

| Version | Release Date    | Key Additions                                                                  |
| ------- | --------------- | ------------------------------------------------------------------------------ |
| 1.0     | March 2011      | Base specification: queues, doorbells, admin/IO command sets, PRP              |
| 1.1     | October 2012    | Multi-path I/O, namespace sharing, arbitrary-length scatter-gather             |
| 1.2     | November 2014   | Host Memory Buffer (HMB), live firmware update, power management improvements  |
| 1.3     | May 2017        | Sanitize command, boot partitions, virtualization, Streams                     |
| 1.4     | June 2019       | Zoned Namespaces (ZNS), I/O determinism, persistent event log, rebuild assist  |
| 2.0     | May 2021        | Modular spec restructure, KV command set, rotational media support, Domains    |
| 2.1     | August 2024     | Live PCIe migration, host-directed data placement, Key Per I/O, NVMe-oF boot  |

> [!NOTE]
> For Impossible OS, the initial implementation targets NVMe 1.0 core functionality (admin commands,
> I/O read/write, PRP-based data transfer, MSI-X interrupts). Features from later versions
> (Sanitize, ZNS, HMB) are optional enhancements tracked in the implementation priorities table.

---

## PCIe Device Identification and BAR Mapping

NVMe controllers are standard PCIe endpoint devices. The OS identifies them during PCI bus enumeration
by examining the Class Code register at offset `0x09` in the PCI Configuration Space Header.

### PCI Class Identification

| Field                    | Offset   | Value  | Meaning                       |
| ------------------------ | -------- | ------ | ----------------------------- |
| Base Class Code (BCC)    | `0x0B`   | `0x01` | Mass Storage Controller       |
| Sub Class Code (SCC)     | `0x0A`   | `0x08` | Non-Volatile Memory (NVMe)    |
| Programming Interface    | `0x09`   | `0x02` | NVMe I/O controller           |

The full 24-bit class code `0x010802` uniquely identifies an NVMe controller. This is defined in the
Linux kernel as `PCI_CLASS_STORAGE_EXPRESS`.

> [!CAUTION]
> Do not confuse the NVMe programming interface `0x02` with other NVM subclass values. The subclass
> `0x08` alone is not sufficient — the programming interface distinguishes NVMe I/O controllers from
> NVMe administrative controllers (`0x03`).

### BAR0 Memory Mapping

The NVMe controller's registers are memory-mapped through **BAR0** (offset `0x10` in PCI config space).
If the BAR indicates a 64-bit memory region (BAR type bits `[2:1]` = `0b10`), BAR0 and BAR1 together
form the 64-bit base address.

```c
/* Read the NVMe BAR0 base address from PCI config space */
uint64_t bar0_low  = pci_read32(bus, dev, func, 0x10) & ~0xFULL;
uint64_t bar0_high = pci_read32(bus, dev, func, 0x14);
uint64_t nvme_base = bar0_low | (bar0_high << 32);
```

> [!CAUTION]
> The BAR0 region MUST be mapped as **uncacheable** (UC) memory. CPU caching of MMIO registers
> causes stale reads of hardware status registers (`CSTS`, doorbell values) and leads to
> unpredictable behavior. Use the VMM to set PTE cache-disable and write-through bits.

---

## Controller Register Map

All NVMe controller registers reside within the BAR0 memory region. Registers are little-endian.
The register space is divided into two areas: fixed controller registers (offsets `0x00`–`0xFFF`)
and doorbell registers (starting at offset `0x1000`).

### Fixed Controller Registers

| Offset   | Size   | Register                              | Access | Description                                              |
| -------- | ------ | ------------------------------------- | ------ | -------------------------------------------------------- |
| `0x00`   | 8 B    | **CAP** (Controller Capabilities)     | RO     | Max queue entries, doorbell stride, timeout, CSS          |
| `0x08`   | 4 B    | **VS** (Version)                      | RO     | NVMe spec version (major.minor.tertiary)                 |
| `0x0C`   | 4 B    | **INTMS** (Interrupt Mask Set)        | WO     | Set bits to mask interrupt vectors                        |
| `0x10`   | 4 B    | **INTMC** (Interrupt Mask Clear)      | WO     | Set bits to unmask interrupt vectors                      |
| `0x14`   | 4 B    | **CC** (Controller Configuration)     | RW     | Enable, I/O command set, MPS, arbitration, SQ/CQ sizes   |
| `0x18`   | 4 B    | Reserved                              | —      | —                                                        |
| `0x1C`   | 4 B    | **CSTS** (Controller Status)          | RO     | Ready, fatal, shutdown status, processing paused          |
| `0x20`   | 4 B    | **NSSR** (NVM Subsystem Reset)        | RW     | Optional: write `0x4E564D65` ("NVMe") to trigger reset   |
| `0x24`   | 4 B    | **AQA** (Admin Queue Attributes)      | RW     | Admin SQ size (bits 27:16), Admin CQ size (bits 11:0)    |
| `0x28`   | 8 B    | **ASQ** (Admin SQ Base Address)       | RW     | 64-bit physical address of Admin Submission Queue        |
| `0x30`   | 8 B    | **ACQ** (Admin CQ Base Address)       | RW     | 64-bit physical address of Admin Completion Queue        |

### CAP Register Bit Fields (Offset `0x00`, 64 bits)

| Bits     | Field      | Description                                                                 |
| -------- | ---------- | --------------------------------------------------------------------------- |
| 63:56    | Reserved   | —                                                                           |
| 55:52    | `MPSMAX`   | Maximum host memory page size supported (2^(12+MPSMAX) bytes)               |
| 51:48    | `MPSMIN`   | Minimum host memory page size supported (2^(12+MPSMIN) bytes)               |
| 47:45    | `BPS`      | Boot Partition Support (NVMe 1.3+)                                          |
| 44:37    | `CSS`      | Command Sets Supported bitmask (bit 0 = NVM command set)                    |
| 36       | `NSSRS`    | NVM Subsystem Reset Supported                                               |
| 35:32    | `DSTRD`    | Doorbell Stride: stride = 2^(2+DSTRD) bytes between doorbell registers      |
| 31:24    | `TO`       | Timeout: worst-case time for CSTS.RDY transition (in 500 ms units)          |
| 23:19    | Reserved   | —                                                                           |
| 18:17    | `AMS`      | Arbitration Mechanism Supported (bit 0 = weighted round robin)              |
| 16       | `CQR`      | Contiguous Queues Required                                                  |
| 15:0     | `MQES`     | Maximum Queue Entries Supported (0-based, actual max = MQES+1)              |

> [!IMPORTANT]
> `CAP.MQES` is **0-based**. A value of `0xFFFF` means the controller supports queues with up to
> 65,536 entries. The admin queue maximum is capped at 4,096 entries regardless of `MQES`.

### CC Register Bit Fields (Offset `0x14`, 32 bits)

| Bits     | Field    | Description                                                                   |
| -------- | -------- | ----------------------------------------------------------------------------- |
| 31:24    | Reserved | —                                                                             |
| 23:20    | `IOCQES` | I/O Completion Queue Entry Size (2^IOCQES bytes, must be ≥ 4 = 16 bytes)     |
| 19:16    | `IOSQES` | I/O Submission Queue Entry Size (2^IOSQES bytes, must be ≥ 6 = 64 bytes)     |
| 15:14    | `SHN`    | Shutdown Notification (00=none, 01=normal, 10=abrupt)                         |
| 13:11    | `AMS`    | Arbitration Mechanism Selected                                                |
| 10:7     | `MPS`    | Memory Page Size (host page = 2^(12+MPS) bytes)                              |
| 6:4      | `CSS`    | I/O Command Set Selected (000 = NVM command set)                              |
| 3:1      | Reserved | —                                                                             |
| 0        | `EN`     | Enable: set to 1 to activate controller, 0 to disable                        |

### CSTS Register Bit Fields (Offset `0x1C`, 32 bits)

| Bits     | Field    | Description                                                                   |
| -------- | -------- | ----------------------------------------------------------------------------- |
| 31:6     | Reserved | —                                                                             |
| 5        | `PP`     | Processing Paused                                                             |
| 4        | `NSSRO`  | NVM Subsystem Reset Occurred                                                  |
| 3:2      | `SHST`   | Shutdown Status (00=normal, 01=in progress, 10=complete)                      |
| 1        | `CFS`    | Controller Fatal Status (1 = fatal error occurred)                            |
| 0        | `RDY`    | Ready: controller is ready to process commands                                |

### Doorbell Registers (Offset `0x1000`+)

Doorbell registers begin at offset `0x1000` within BAR0. Each queue has a Submission Queue Tail
Doorbell (SQTDBL) and a Completion Queue Head Doorbell (CQHDBL). The stride between consecutive
doorbell registers is determined by `CAP.DSTRD`.

```
SQyTDBL offset = 0x1000 + (2y       × (4 << CAP.DSTRD))
CQyHDBL offset = 0x1000 + ((2y + 1) × (4 << CAP.DSTRD))
```

Where `y` is the queue identifier (0 = Admin Queue, 1+ = I/O Queues).

> [!NOTE]
> With `CAP.DSTRD = 0` (the common case), the stride is 4 bytes, and doorbells are packed
> contiguously: SQ0 Tail at `0x1000`, CQ0 Head at `0x1004`, SQ1 Tail at `0x1008`, etc.

---

## Controller Initialization Sequence

The NVMe initialization sequence is strictly ordered. The controller must be fully disabled before
configuration, and the host must wait for hardware acknowledgment at each transition.

### Step-by-Step Initialization

1. **Disable the controller**: Clear `CC.EN` to 0. Poll `CSTS.RDY` until it reads 0. The maximum
   wait time is `CAP.TO × 500 ms`.

2. **Read capabilities**: Read the `CAP` register to determine `MQES`, `DSTRD`, `MPSMIN`, `MPSMAX`,
   `CSS`, and `TO`.

3. **Configure the controller**: Write `CC` with:
   - `CC.EN` = 0 (still disabled)
   - `CC.CSS` = 0 (NVM command set)
   - `CC.MPS` = host page size (must be between `MPSMIN` and `MPSMAX`)
   - `CC.AMS` = 0 (round robin arbitration)
   - `CC.IOSQES` = 6 (64-byte SQ entries)
   - `CC.IOCQES` = 4 (16-byte CQ entries)

4. **Allocate Admin Queues**: Allocate physically contiguous, page-aligned memory for the Admin
   Submission Queue and Admin Completion Queue. Zero the memory.

5. **Program Admin Queue registers**:
   - Write the 64-bit physical address of the Admin SQ to `ASQ` (offset `0x28`)
   - Write the 64-bit physical address of the Admin CQ to `ACQ` (offset `0x30`)
   - Write `AQA` (offset `0x24`) with the queue sizes (0-based values in bits 27:16 and 11:0)

6. **Enable the controller**: Set `CC.EN` to 1. Poll `CSTS.RDY` until it reads 1. If `CSTS.CFS`
   becomes 1 during this wait, the controller has suffered a fatal error and must be reset.

7. **Identify the controller**: Submit an Identify Controller command (opcode `0x06`, CNS=1) through
   the Admin Queue to retrieve device capabilities.

8. **Create I/O queues**: Use admin commands to create I/O Completion Queues and I/O Submission
   Queues (see §I/O Queue Creation).

9. **Begin I/O operations**: The controller is now ready to accept NVM read/write commands.

> [!CAUTION]
> The `ASQ` and `ACQ` registers MUST be written while `CC.EN` is 0. Writing these registers while
> the controller is enabled results in undefined behavior. The memory backing these queues must
> remain pinned (non-pageable) for the lifetime of the controller.

---

## The Circular Queue Paradigm

NVMe communication occurs exclusively through paired Submission Queues (SQ) and Completion Queues
(CQ). These are fixed-size circular buffers in pinned host physical memory.

### Queue Structure

- **Submission Queue (SQ)**: Array of 64-byte Submission Queue Entries (SQE). The host writes
  commands here and notifies the controller via doorbell writes.
- **Completion Queue (CQ)**: Array of 16-byte Completion Queue Entries (CQE). The controller writes
  completion status here and notifies the host via interrupts.
- **Admin Queue**: Queue pair 0, reserved for controller management commands.
- **I/O Queues**: Queue pairs 1–65,535, used for NVM read/write/deallocate operations.

### Command Submission Flow

The NVMe command execution cycle follows a precise 7-step sequence:

1. **Construct SQE**: The host builds a 64-byte command entry at the current SQ tail index.
2. **Ring doorbell**: The host writes the new tail index to the SQ Tail Doorbell register (SQTDBL).
3. **Controller fetches**: The controller uses DMA to read the SQE from host memory.
4. **Execute command**: The controller processes the I/O against the NAND flash.
5. **Post CQE**: The controller writes a 16-byte completion entry to the CQ in host memory.
6. **Trigger interrupt**: The controller fires an MSI-X interrupt to notify the host.
7. **Process and advance**: The host reads the CQE, processes the result, and writes the new head
   index to the CQ Head Doorbell register (CQHDBL).

### Phase Tag Mechanism

The CQE contains a **Phase Tag (P)** bit that the controller toggles on each wrap-around of the
circular buffer. The host uses this bit to detect new completions without zeroing the CQ memory:

- Initially, the host sets all P bits to 0.
- The controller writes CQEs with P=1 on the first pass through the buffer.
- On the second pass (after wrap-around), the controller writes CQEs with P=0.
- The host knows a CQE is new when the P bit differs from the expected value.

### Submission Queue Entry (SQE) Format — 64 Bytes

```c
typedef struct nvme_sqe {
    uint32_t cdw0;          /* Opcode (7:0), FUSE (9:8), PSDT (15:14), CID (31:16) */
    uint32_t nsid;          /* Namespace Identifier                                 */
    uint64_t reserved;      /* Reserved                                             */
    uint64_t mptr;          /* Metadata Pointer                                     */
    uint64_t prp1;          /* PRP Entry 1 / SGL Descriptor                         */
    uint64_t prp2;          /* PRP Entry 2 / SGL Descriptor                         */
    uint32_t cdw10;         /* Command-specific DWORD 10                            */
    uint32_t cdw11;         /* Command-specific DWORD 11                            */
    uint32_t cdw12;         /* Command-specific DWORD 12                            */
    uint32_t cdw13;         /* Command-specific DWORD 13                            */
    uint32_t cdw14;         /* Command-specific DWORD 14                            */
    uint32_t cdw15;         /* Command-specific DWORD 15                            */
} __attribute__((packed)) nvme_sqe_t;
```

### Completion Queue Entry (CQE) Format — 16 Bytes

```c
typedef struct nvme_cqe {
    uint32_t dw0;           /* Command-specific result                              */
    uint32_t dw1;           /* Reserved                                             */
    uint16_t sq_head;       /* SQ Head Pointer (updated by controller)              */
    uint16_t sq_id;         /* SQ Identifier                                        */
    uint16_t cid;           /* Command Identifier (matches SQE.CDW0.CID)            */
    uint16_t status;        /* Phase (bit 0), Status Field (bits 15:1)              */
} __attribute__((packed)) nvme_cqe_t;
```

> [!IMPORTANT]
> The `status` field in the CQE packs the Phase Tag in bit 0 and the Status Field in bits 15:1.
> Status Code Type (SCT) is in bits 11:9, Status Code (SC) is in bits 8:1. A successful
> completion has SCT=0, SC=0 (status field = `0x0000` with P bit masked).

---

## I/O Queue Creation and Multi-Core Topology

After controller initialization, the OS must create I/O queue pairs via Admin commands before any
NVM read/write operations can proceed.

### Create I/O Completion Queue (Opcode `0x05`)

| CDW Field | Bits     | Description                                                      |
| --------- | -------- | ---------------------------------------------------------------- |
| CDW10     | 15:0     | Queue Identifier (QID): unique ID for this CQ (1–65,535)        |
| CDW10     | 31:16    | Queue Size (QSIZE): 0-based number of entries                   |
| CDW11     | 0        | Physically Contiguous (PC): 1 = queue is physically contiguous   |
| CDW11     | 1        | Interrupts Enabled (IEN): 1 = enable interrupts for this CQ     |
| CDW11     | 31:16    | Interrupt Vector (IV): MSI-X vector index for this CQ            |

### Create I/O Submission Queue (Opcode `0x01`)

| CDW Field | Bits     | Description                                                      |
| --------- | -------- | ---------------------------------------------------------------- |
| CDW10     | 15:0     | Queue Identifier (QID): unique ID for this SQ (1–65,535)        |
| CDW10     | 31:16    | Queue Size (QSIZE): 0-based number of entries                   |
| CDW11     | 0        | Physically Contiguous (PC): 1 = queue is physically contiguous   |
| CDW11     | 2:1      | Queue Priority (QPRIO): 00=urgent, 01=high, 10=medium, 11=low   |
| CDW11     | 31:16    | Completion Queue Identifier (CQID): associated CQ for this SQ   |

### Multi-Core Queue Allocation Strategy

For optimal performance on a multi-core system, the OS should create one I/O queue pair per logical
CPU core. This eliminates inter-core lock contention during I/O submission:

```
Core 0 → I/O SQ 1 + I/O CQ 1 (MSI-X vector 1 → Core 0 LAPIC)
Core 1 → I/O SQ 2 + I/O CQ 2 (MSI-X vector 2 → Core 1 LAPIC)
Core 2 → I/O SQ 3 + I/O CQ 3 (MSI-X vector 3 → Core 2 LAPIC)
...
Core N → I/O SQ N+1 + I/O CQ N+1 (MSI-X vector N+1 → Core N LAPIC)
```

> [!IMPORTANT]
> The number of I/O queues the controller actually supports may be less than 65,535.
> Query the controller's limits by issuing the Set Features command (opcode `0x09`) or Get Features
> command (opcode `0x0A`) with Feature Identifier `0x07` (Number of Queues) before creating queues.
> The controller returns the maximum I/O SQs (NSQA) and CQs (NCQA) it supports.

---

## Data Payload Management: PRPs and SGLs

NVMe uses Physical Region Pages (PRPs) or Scatter Gather Lists (SGLs) to describe the host memory
locations for data transfers.

### Physical Region Pages (PRP)

Each SQE contains two 64-bit PRP fields: `PRP1` and `PRP2`.

| Transfer Size           | PRP1                               | PRP2                                     |
| ----------------------- | ---------------------------------- | ---------------------------------------- |
| ≤ 1 memory page         | Physical address of data buffer    | Unused (0)                               |
| ≤ 2 memory pages        | Physical address of first page     | Physical address of second page          |
| > 2 memory pages        | Physical address of first page     | Physical address of PRP List             |

A **PRP List** is a page-aligned array of 64-bit physical addresses in host memory. Each entry
points to one page of data. If the list itself spans more than one page, the last entry of each
page points to the next page of the PRP List (chaining).

```c
/* PRP List entry layout — each entry is 8 bytes */
typedef struct nvme_prp_list {
    uint64_t entries[512];  /* For 4K pages: 512 entries × 8 bytes = 4096 bytes */
} __attribute__((packed)) nvme_prp_list_t;
```

> [!CAUTION]
> PRP entries must be **page-aligned** (the low bits corresponding to the page offset must be zero
> for all entries except PRP1). PRP1 may have a non-zero page offset, but the transfer then starts
> at that offset within the first page. The controller will report an Invalid Field error if
> alignment constraints are violated.

### Maximum Data Transfer Size (MDTS)

The `MDTS` field in the Identify Controller data structure (byte 77) specifies the maximum data
transfer size as a power of two, in units of the minimum memory page size (`2^(12+CAP.MPSMIN)`).

```
max_transfer_bytes = (1 << MDTS) × (1 << (12 + CAP.MPSMIN))
```

If `MDTS` is 0, there is no maximum (limited only by available PRP entries). The OS must split
any I/O request larger than this into multiple commands.

---

## Admin Command Set

Admin commands are submitted through the Admin Submission Queue (Queue 0) and manage controller
configuration, device identification, and queue lifecycle.

### Admin Command Opcodes

| Opcode   | Command Name                    | Description                                              |
| -------- | ------------------------------- | -------------------------------------------------------- |
| `0x00`   | Delete I/O Submission Queue     | Destroy an I/O SQ                                        |
| `0x01`   | Create I/O Submission Queue     | Create an I/O SQ paired with a CQ                        |
| `0x02`   | Get Log Page                    | Retrieve log pages (SMART, error, firmware)               |
| `0x04`   | Delete I/O Completion Queue     | Destroy an I/O CQ                                        |
| `0x05`   | Create I/O Completion Queue     | Create an I/O CQ with interrupt vector assignment         |
| `0x06`   | Identify                        | Retrieve controller/namespace identification data         |
| `0x09`   | Set Features                    | Configure controller features (queue count, etc.)         |
| `0x0A`   | Get Features                    | Query current feature settings                            |
| `0x0C`   | Asynchronous Event Request      | Register for asynchronous event notifications             |
| `0x10`   | Firmware Commit                 | Commit firmware image to slot                             |
| `0x11`   | Firmware Image Download         | Download firmware image segment                           |
| `0x84`   | Sanitize                        | Secure data erasure (NVMe 1.3+)                           |

---

## NVM I/O Command Set

I/O commands are submitted through I/O Submission Queues and operate on namespaces.

### I/O Command Opcodes

| Opcode   | Command Name              | Description                                                 |
| -------- | ------------------------- | ----------------------------------------------------------- |
| `0x00`   | Flush                     | Commit volatile write cache to non-volatile media           |
| `0x01`   | Write                     | Write data to specified LBA range                           |
| `0x02`   | Read                      | Read data from specified LBA range                          |
| `0x04`   | Write Uncorrectable       | Mark LBA range as unreadable                                |
| `0x05`   | Compare                   | Compare data in LBA range with host buffer                  |
| `0x08`   | Write Zeroes              | Write zeroes to LBA range (no data transfer)                |
| `0x09`   | Dataset Management        | Deallocate (TRIM), performance hints                        |

### Read/Write Command Format

For Read (opcode `0x02`) and Write (opcode `0x01`) commands, the SQE fields are:

| CDW Field | Bits     | Description                                                      |
| --------- | -------- | ---------------------------------------------------------------- |
| CDW0      | 7:0      | Opcode (`0x01` write, `0x02` read)                               |
| NSID      | 31:0     | Target namespace ID                                              |
| PRP1      | 63:0     | First PRP entry (data buffer address)                            |
| PRP2      | 63:0     | Second PRP entry or PRP List pointer                             |
| CDW10     | 31:0     | Starting LBA (lower 32 bits)                                     |
| CDW11     | 31:0     | Starting LBA (upper 32 bits)                                     |
| CDW12     | 15:0     | Number of Logical Blocks (NLB, 0-based: 0 = 1 block)            |
| CDW12     | 31:26    | Protection Information / FUA / LR flags                          |

> [!IMPORTANT]
> The NLB field in CDW12 is **0-based**. A value of 0 means transfer 1 logical block.
> The maximum NLB value allowed depends on the MDTS constraint calculated from the
> Identify Controller data structure.

---

## Device Identification

### Identify Controller (CNS = 1)

The Identify command (opcode `0x06`) with CNS=1 in CDW10 returns a 4,096-byte Identify Controller
Data Structure. Critical fields:

| Offset     | Size   | Field   | Description                                                       |
| ---------- | ------ | ------- | ----------------------------------------------------------------- |
| `0`–`1`    | 2 B    | `VID`   | PCI Vendor ID                                                     |
| `2`–`3`    | 2 B    | `SSVID` | PCI Subsystem Vendor ID                                           |
| `4`–`23`   | 20 B   | `SN`    | Serial Number (ASCII, space-padded)                               |
| `24`–`63`  | 40 B   | `MN`    | Model Number (ASCII, space-padded)                                |
| `64`–`71`  | 8 B    | `FR`    | Firmware Revision (ASCII)                                         |
| `72`       | 1 B    | `RAB`   | Recommended Arbitration Burst                                     |
| `73`–`75`  | 3 B    | `IEEE`  | IEEE OUI Identifier                                               |
| `77`       | 1 B    | `MDTS`  | Maximum Data Transfer Size (as power of 2, in MPSMIN units)       |
| `256`–`259`| 4 B    | `OACS`  | Optional Admin Command Support bitmask                            |
| `512`      | 1 B    | `SQES`  | SQ Entry Size: required (3:0) and maximum (7:4), as log2          |
| `513`      | 1 B    | `CQES`  | CQ Entry Size: required (3:0) and maximum (7:4), as log2          |
| `516`–`519`| 4 B    | `NN`    | Number of Namespaces                                              |

### Identify Namespace (CNS = 0)

The Identify command with CNS=0 and a target NSID returns a 4,096-byte Identify Namespace Data
Structure. Critical fields:

| Offset     | Size   | Field    | Description                                                      |
| ---------- | ------ | -------- | ---------------------------------------------------------------- |
| `0`–`7`    | 8 B    | `NSZE`   | Namespace Size (total logical blocks)                            |
| `8`–`15`   | 8 B    | `NCAP`   | Namespace Capacity (allocated logical blocks)                    |
| `16`–`23`  | 8 B    | `NUSE`   | Namespace Utilization (currently used logical blocks)            |
| `24`       | 1 B    | `NSFEAT` | Namespace Features                                               |
| `25`       | 1 B    | `NLBAF`  | Number of LBA Formats (0-based)                                  |
| `26`       | 1 B    | `FLBAS`  | Formatted LBA Size: active LBA format index (bits 3:0)           |
| `128`+     | varies | `LBAF[]` | LBA Format descriptors (4 bytes each)                            |

### LBA Format Descriptor

Each LBA Format (LBAF) entry is 4 bytes:

| Bits     | Field  | Description                                                           |
| -------- | ------ | --------------------------------------------------------------------- |
| 31:26    | `RP`   | Relative Performance (00=best, 01=better, 10=good, 11=degraded)       |
| 25:24    | Reserved | —                                                                   |
| 23:16    | `LBADS`| LBA Data Size: sector size = 2^LBADS bytes (e.g., 9=512B, 12=4096B)  |
| 15:0     | `MS`   | Metadata Size in bytes                                                |

> [!IMPORTANT]
> Modern NVMe SSDs commonly support both 512-byte (LBADS=9) and 4096-byte (LBADS=12) sector
> formats. The active format is determined by `FLBAS` bits 3:0, indexing into the `LBAF[]` array.
> **Always read the Identify Namespace to determine the actual sector size** — do not assume 512.

---

## Interrupt Handling: MSI-X

NVMe controllers universally support MSI-X for interrupt delivery. MSI-X replaces legacy INTx
pin-based interrupts with in-band PCIe memory writes to the target CPU's Local APIC.

### MSI-X Configuration

The MSI-X capability is located via the PCI Capabilities Linked List. The MSI-X capability
structure contains:

| Offset | Size | Field                                                                  |
| ------ | ---- | ---------------------------------------------------------------------- |
| +0     | 2 B  | Capability ID (`0x11` for MSI-X)                                       |
| +1     | 1 B  | Next Capability Pointer                                                |
| +2     | 2 B  | Message Control (table size, function mask, MSI-X enable)              |
| +4     | 4 B  | Table Offset and BIR (BAR Indicator Register)                          |
| +8     | 4 B  | PBA Offset and BIR                                                     |

Each MSI-X Table Entry is 16 bytes:

| Offset | Size | Field                                                                  |
| ------ | ---- | ---------------------------------------------------------------------- |
| +0     | 4 B  | Message Address (lower 32 bits — LAPIC address)                        |
| +4     | 4 B  | Message Upper Address (upper 32 bits, usually 0 for x86-64)            |
| +8     | 4 B  | Message Data (interrupt vector number)                                 |
| +12    | 4 B  | Vector Control (bit 0 = mask)                                          |

### Vector-to-Queue Affinity

For per-core I/O queue pairs, configure MSI-X vectors so each CQ's interrupt routes to the
corresponding CPU core's LAPIC:

```
CQ 1 → MSI-X Vector 1 → Core 0 LAPIC (address 0xFEE00000, data = vector_1)
CQ 2 → MSI-X Vector 2 → Core 1 LAPIC (address 0xFEE00000, data = vector_2)
...
```

The Interrupt Vector is specified in CDW11 bits 31:16 of the Create I/O Completion Queue command.

> [!NOTE]
> For initial single-core implementation, a single MSI-X vector shared across all CQs is
> acceptable. Per-core vector affinity can be added when SMP support matures.

---

## SMART / Health Information (Log Page `0x02`)

The SMART/Health Information log page provides standardized telemetry for monitoring drive health.
Retrieved via the Get Log Page admin command (opcode `0x02`) with Log Identifier `0x02`.

### SMART Log Structure (512 bytes)

| Offset     | Size    | Field                        | Description                                    |
| ---------- | ------- | ---------------------------- | ---------------------------------------------- |
| `0`        | 1 B     | `critical_warning`           | Bitmask: spare, temp, reliability, RO, backup  |
| `1`–`2`    | 2 B     | `composite_temp`             | Temperature in Kelvin (subtract 273 for °C)    |
| `3`        | 1 B     | `avail_spare`                | Available spare capacity (0–100%)              |
| `4`        | 1 B     | `avail_spare_thresh`         | Spare threshold (triggers warning when below)  |
| `5`        | 1 B     | `percent_used`               | Estimated percent of life consumed             |
| `6`–`31`   | 26 B    | Reserved                     | —                                              |
| `32`–`47`  | 16 B    | `data_units_read`            | 128-bit count, in units of 1000 × 512 bytes    |
| `48`–`63`  | 16 B    | `data_units_written`         | 128-bit count, in units of 1000 × 512 bytes    |
| `64`–`79`  | 16 B    | `host_read_commands`         | 128-bit count of read commands issued          |
| `80`–`95`  | 16 B    | `host_write_commands`        | 128-bit count of write commands issued         |
| `96`–`111` | 16 B    | `controller_busy_time`       | 128-bit count in minutes                       |
| `112`–`127`| 16 B    | `power_cycles`               | 128-bit power cycle count                      |
| `128`–`143`| 16 B    | `power_on_hours`             | 128-bit power-on hours count                   |
| `144`–`159`| 16 B    | `unsafe_shutdowns`           | 128-bit unsafe shutdown count                  |
| `160`–`175`| 16 B    | `media_errors`               | 128-bit media and data integrity error count   |
| `176`–`191`| 16 B    | `num_err_log_entries`        | 128-bit error log entry count                  |

### Critical Warning Bitmask (Byte 0)

| Bit  | Warning                                                                   |
| ---- | ------------------------------------------------------------------------- |
| 0    | Available spare below threshold                                           |
| 1    | Temperature above or below threshold                                      |
| 2    | NVM subsystem reliability degraded (media wear or internal errors)        |
| 3    | Media placed in read-only mode                                            |
| 4    | Volatile memory backup device has failed                                  |
| 5–7  | Reserved                                                                  |

---

## Block Deallocation (TRIM / Dataset Management)

The Dataset Management command (opcode `0x09`) with the Deallocate attribute informs the NVMe
controller that specific LBA ranges are no longer in use. This is the NVMe equivalent of
SATA TRIM.

### Command Format

| CDW Field | Bits     | Description                                                      |
| --------- | -------- | ---------------------------------------------------------------- |
| CDW10     | 7:0      | Number of Ranges (NR, 0-based: 0 = 1 range)                     |
| CDW11     | 2        | Deallocate (AD): set to 1 to deallocate the specified ranges     |

The data buffer (pointed to by PRP1/PRP2) contains an array of Dataset Management Range
descriptors, each 16 bytes:

```c
typedef struct nvme_dsm_range {
    uint32_t cattr;         /* Context Attributes                                   */
    uint32_t nlb;           /* Number of Logical Blocks (length of range)            */
    uint64_t slba;          /* Starting LBA                                         */
} __attribute__((packed)) nvme_dsm_range_t;
```

> [!IMPORTANT]
> Issuing Deallocate commands regularly is critical for maintaining SSD performance and longevity.
> The file system must batch freed LBA ranges and submit them periodically. Without explicit
> deallocation, the SSD's internal garbage collection suffers severe write amplification.

---

## Storage Security

### NVMe Sanitize Command (NVMe 1.3+)

The Sanitize command (admin opcode `0x84`) permanently destroys data across the entire NVM
subsystem. Three sanitize actions are defined:

| Action Code | Name                  | Mechanism                                                  |
| ----------- | --------------------- | ---------------------------------------------------------- |
| `0x01`      | Block Erase           | Erase all user data blocks                                 |
| `0x02`      | Overwrite             | Overwrite all user data with a fixed pattern               |
| `0x04`      | Crypto Erase          | Destroy the internal encryption key (instant, preferred)   |

> [!NOTE]
> Crypto Erase is the fastest and most secure method. Modern SSDs perform inline AES-256
> encryption at the controller level. Destroying the Data Encryption Key (DEK) renders all
> data mathematically irrecoverable in milliseconds.

The OS must check `Identify Controller` → `SANICAP` (Sanitize Capabilities, offset 328–331)
before issuing a Sanitize command to verify hardware support.

### TCG Opal 2.0 (Self-Encrypting Drives)

Drives implementing the Trusted Computing Group (TCG) Opal 2.0 specification provide hardware-
level full-disk encryption. Key features relevant to OS implementation:

- **LBA Locking Ranges**: Partition the SSD into independently encrypted regions with unique keys
- **Shadow MBR**: During cold boot, the SSD presents a separate unencrypted boot partition for
  pre-boot authentication; after successful authentication, the real data partitions are unlocked
- **PSID Revert**: Factory reset using the Physical Security ID printed on the drive label

> [!NOTE]
> TCG Opal support requires implementing the TCG Storage Security Subsystem Class (SSC)
> protocol, which communicates via NVMe Security Send (opcode `0x81`) and Security Receive
> (opcode `0x82`) admin commands. This is a P3 feature for Impossible OS.

---

## Error Handling and Recovery

### Status Codes in CQE

The CQE `status` field (bits 15:1) encodes the result of each command:

| SCT (bits 11:9) | Category              | Examples                                        |
| ---------------- | --------------------- | ----------------------------------------------- |
| `0x0`            | Generic               | Success, Invalid Opcode, Invalid Field          |
| `0x1`            | Command Specific      | Invalid Queue, Max Queues Exceeded              |
| `0x2`            | Media/Data Integrity  | Unrecovered Read Error, Write Fault             |

### Controller Reset Procedure

If `CSTS.CFS` (Controller Fatal Status) becomes 1, the controller has encountered an
unrecoverable error. Recovery requires:

1. Set `CC.EN` to 0
2. Wait for `CSTS.RDY` to become 0 (or timeout at `CAP.TO × 500 ms`)
3. Re-initialize from scratch (reconfigure CC, reallocate queues, re-enable)

> [!CAUTION]
> If `CSTS.RDY` does not clear within the timeout period, the controller is unresponsive.
> A PCI Function Level Reset (FLR) via the PCIe Advanced Error Reporting capability may be
> needed, or a full system reset as a last resort.

---

## QEMU Testing Configuration

QEMU provides comprehensive NVMe controller emulation suitable for driver development.

### Basic NVMe Device

```bash
qemu-system-x86_64 \
    -drive file=nvme-test.img,format=raw,if=none,id=nvme-drive \
    -device nvme,serial=deadbeef,drive=nvme-drive
```

### Multi-Namespace NVMe (QEMU 6.0+)

```bash
qemu-system-x86_64 \
    -device nvme,id=nvme0,serial=deadbeef \
    -drive file=ns1.img,format=raw,if=none,id=ns1 \
    -device nvme-ns,drive=ns1,bus=nvme0,nsid=1 \
    -drive file=ns2.img,format=raw,if=none,id=ns2 \
    -device nvme-ns,drive=ns2,bus=nvme0,nsid=2
```

### Multi-Queue Testing

```bash
qemu-system-x86_64 \
    -smp 4 \
    -device nvme,serial=deadbeef,drive=nvme-drive,max_ioqpairs=4
```

### NVMe with MSI-X Debug Output

```bash
qemu-system-x86_64 \
    -device nvme,serial=deadbeef,drive=nvme-drive \
    -trace "nvme_*" \
    -d guest_errors
```

### Full Impossible OS Test Command

```bash
# Create a 512 MiB NVMe test disk
qemu-img create -f raw nvme-test.img 512M

# Boot Impossible OS with both AHCI (system disk) and NVMe (test disk)
qemu-system-x86_64 \
    -machine q35 \
    -cpu qemu64 \
    -m 256M \
    -bios /usr/share/OVMF/OVMF_CODE.fd \
    -drive file=build/system-disk.img,format=raw,if=none,id=bootdisk \
    -device ahci,id=ahci0 \
    -device ide-hd,drive=bootdisk,bus=ahci0.0 \
    -drive file=nvme-test.img,format=raw,if=none,id=nvmedisk \
    -device nvme,serial=IMPOSSIBLE01,drive=nvmedisk \
    -serial stdio
```

> [!NOTE]
> The `q35` machine type provides a modern PCIe root complex, required for proper NVMe
> device enumeration. The default `i440fx` machine type does not support PCIe.

---

## Implementation Priorities for Impossible OS

| Priority | Feature                                    | NVMe Version | Notes                                         |
| -------- | ------------------------------------------ | ------------ | --------------------------------------------- |
| 🔴 P0    | PCIe device identification (class 0x0108)  | 1.0          | Extend existing PCI enumerator                |
| 🔴 P0    | BAR0 MMIO mapping (uncacheable)            | 1.0          | Use VMM to map controller registers           |
| 🔴 P0    | Controller init sequence (CC/CSTS/AQA/ASQ) | 1.0          | Disable → configure → enable → poll RDY       |
| 🔴 P0    | Admin Queue (create, submit, complete)     | 1.0          | Single admin queue pair                       |
| 🔴 P0    | Identify Controller + Namespace            | 1.0          | Parse MDTS, sector size, capacity             |
| 🔴 P0    | Single I/O Queue pair                      | 1.0          | One SQ + one CQ, interrupt-driven             |
| 🔴 P0    | PRP-based read/write                       | 1.0          | Single-page and PRP List transfers            |
| 🔴 P0    | `blkdev` integration                       | —            | Implement `block_device_t` interface          |
| 🟠 P1    | MSI-X interrupt configuration              | 1.0          | Replace INTx with MSI-X vectors               |
| 🟠 P1    | Multi-queue (per-core queue pairs)         | 1.0          | Scale with CPU topology                       |
| 🟠 P1    | Dataset Management / Deallocate (TRIM)     | 1.0          | Required for SSD health                       |
| 🟠 P1    | SMART health monitoring (Log Page 0x02)    | 1.0          | Periodic background polling                   |
| 🟡 P2    | Error recovery and controller reset        | 1.0          | Handle CFS, timeout, re-init                  |
| 🟡 P2    | 4Kn sector support                         | 1.0          | Align I/O to native 4K sectors                |
| 🟡 P2    | Shutdown notification (CC.SHN)             | 1.0          | Notify controller before power-off            |
| 🟢 P3    | Sanitize command (Crypto Erase)            | 1.3          | Secure data destruction                       |
| 🟢 P3    | Flush command                              | 1.0          | Volatile write cache commit                   |
| 🔵 P4    | Namespace management                       | 1.1          | Create/delete namespaces                      |
| 🔵 P4    | TCG Opal 2.0 SED support                   | —            | Full-disk encryption management               |
| 🔵 P4    | Zoned Namespaces (ZNS)                     | 1.4+         | Advanced flash management                     |
| 🔵 P4    | NVMe over Fabrics (NVMe-oF)               | 1.0+         | Network-attached NVMe                         |
