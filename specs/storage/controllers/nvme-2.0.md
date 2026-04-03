# NVMe 2.0 -- Technical Specification for OS Implementation

## Overview and Architectural Context

Non-Volatile Memory Express (NVMe) is a high-performance host controller interface and storage
protocol designed specifically for solid-state storage over PCI Express (PCIe). Unlike legacy
protocols such as AHCI (designed around the mechanical latencies of spinning media), NVMe attaches
directly to the PCIe bus, providing an inherently parallel, low-latency, and high-throughput
interface tailored for NAND flash and other non-volatile memory technologies.

NVMe eliminates the single-queue bottleneck of AHCI (which supports only 1 command queue per port
with 32 entries) by supporting up to **65,535 I/O queue pairs**, each holding up to **65,536
entries**. Combined with lockless per-core queue isolation and zero-copy DMA transfers, NVMe enables
millions of IOPS and microsecond-class latencies on modern hardware.

The NVMe 2.0 revision (released June 2021) refactored the previously monolithic specification into
modular documents:

- **NVM Express Base Specification** -- controller registers, queue mechanics, admin commands
- **NVM Command Set Specification** -- read/write/flush/trim for block storage
- **Zoned Namespaces (ZNS) Command Set** -- zone-based storage for SSDs
- **Key Value (KV) Command Set** -- key-value pair storage
- **Transport Specifications** -- PCIe, RDMA (RoCE/iWARP), TCP transports

This specification covers the PCIe transport with the NVM Command Set, which is the mandatory
baseline for all NVMe controllers.

### Version History and Compatibility

| Version   | Date      | Key Additions                                                          |
| --------- | --------- | ---------------------------------------------------------------------- |
| NVMe 1.0  | Mar 2011  | Initial release: admin/NVM command sets, SQ/CQ queues, PRP            |
| NVMe 1.1  | Oct 2012  | Namespace management, reservations, AER improvements                  |
| NVMe 1.2  | Jun 2014  | SGL support, power management, host memory buffer                     |
| NVMe 1.3  | Jun 2017  | Sanitize, boot partitions, virtualization enhancements                 |
| NVMe 1.4  | Jun 2019  | Persistent Memory Region, multipath (ANA), ZNS preview                |
| NVMe 2.0  | Jun 2021  | Modular spec split, KV command set, I/O command set independent model  |
| NVMe 2.0c | Oct 2022  | Clarifications, Copy command, Flexible Data Placement (FDP) preview    |
| NVMe 2.1  | Aug 2024  | FDP ratified, enhanced error reporting, In-Band Authentication         |

---

## PCIe Device Identification and Discovery

An NVMe controller manifests as a standard PCIe endpoint device. The host OS discovers NVMe
controllers during PCIe bus enumeration by checking the Configuration Space header of each
device.

### PCIe Class Code Identification

| Field              | Value  | Meaning                              |
| ------------------ | ------ | ------------------------------------ |
| Class Code         | `0x01` | Mass Storage Controller              |
| Subclass Code      | `0x08` | Non-Volatile Memory Controller       |
| Programming I/F    | `0x02` | NVM Express                          |

> [!IMPORTANT]
> The Programming Interface byte `0x02` specifically identifies NVMe. Other values under
> subclass `0x08` may indicate non-NVMe NVM controllers.

### PCIe Command Register Initialization

After identifying the NVMe endpoint, the driver must configure the PCIe Command Register
(offset `0x04` in PCI Configuration Space) to enable the device:

| Bit | Name              | Required Value | Purpose                                       |
| --- | ----------------- | -------------- | --------------------------------------------- |
| 2   | Bus Master Enable | `1`            | Required for all DMA operations                |
| 1   | Memory Space      | `1`            | Enables MMIO access via BAR0/BAR1              |
| 10  | Interrupt Disable | `1`            | Disable legacy INTx (use MSI-X instead)        |

> [!CAUTION]
> Bus Master Enable **must** be set before any NVMe operation. The entire NVMe protocol
> depends on the controller performing autonomous DMA reads and writes to host memory.

---

## Base Address Registers (BARs) and MMIO Mapping

NVMe uses Memory-Mapped I/O (MMIO) for all controller register access. The physical base
address is encoded across BAR0 and BAR1 in the PCIe Configuration Space.

### BAR Layout

| PCI Offset | Register | Content                                                      |
| ---------- | -------- | ------------------------------------------------------------ |
| `0x10`     | BAR0     | Lower 32 bits of MMIO base address + type/prefetch flags     |
| `0x14`     | BAR1     | Upper 32 bits of MMIO base address (64-bit BAR)              |

The NVMe specification mandates a **64-bit memory BAR**. To reconstruct the physical address:

```c
uint64_t bar0_raw = pci_read32(dev, 0x10);
uint64_t bar1_raw = pci_read32(dev, 0x14);
uint64_t mmio_base = (bar0_raw & 0xFFFFFFF0) | ((uint64_t)bar1_raw << 32);
```

> [!CAUTION]
> The MMIO region **must** be mapped as uncacheable (write-through or uncached) in the page
> tables. Caching MMIO addresses causes the CPU to serve stale register values from cache,
> leading to catastrophic desynchronization with the controller. In x86-64, use the Page
> Attribute Table (PAT) or PCD/PWT bits to enforce uncached mapping.

### MMIO Region Size

The minimum MMIO region size is **16 KiB** (0x4000 bytes), consisting of:
- Controller registers: offsets `0x0000`–`0x0FFF`
- Doorbell registers: starting at offset `0x1000`

The actual size depends on the number of queues and the doorbell stride (`CAP.DSTRD`).

---

## Controller Register Map

All controller registers are located within the MMIO region mapped via BAR0/BAR1. All
multi-byte registers are **little-endian**.

| Offset   | Size | Register | Name                                    | Access     |
| -------- | ---- | -------- | --------------------------------------- | ---------- |
| `0x0000` | 8    | CAP      | Controller Capabilities                 | Read-Only  |
| `0x0008` | 4    | VS       | Version                                 | Read-Only  |
| `0x000C` | 4    | INTMS    | Interrupt Mask Set                      | Write-Only |
| `0x0010` | 4    | INTMC    | Interrupt Mask Clear                    | Write-Only |
| `0x0014` | 4    | CC       | Controller Configuration                | Read/Write |
| `0x001C` | 4    | CSTS     | Controller Status                       | Read-Only  |
| `0x0020` | 4    | NSSR     | NVM Subsystem Reset (optional)          | Read/Write |
| `0x0024` | 4    | AQA      | Admin Queue Attributes                  | Read/Write |
| `0x0028` | 8    | ASQ      | Admin Submission Queue Base Address      | Read/Write |
| `0x0030` | 8    | ACQ      | Admin Completion Queue Base Address      | Read/Write |
| `0x0038` | 4    | CMBLOC   | Controller Memory Buffer Location (opt) | Read-Only  |
| `0x003C` | 4    | CMBSZ    | Controller Memory Buffer Size (opt)     | Read-Only  |
| `0x0040` | 4    | BPINFO   | Boot Partition Info (optional)          | Read-Only  |
| `0x0044` | 4    | BPRSEL   | Boot Partition Read Select (optional)   | Read/Write |
| `0x0048` | 8    | BPMBL    | Boot Partition Memory Buffer Loc (opt)  | Read/Write |
| `0x0050` | 8    | CMBMSC   | CMB Memory Space Control (optional)     | Read/Write |
| `0x0058` | 4    | CMBSTS   | CMB Status (optional)                   | Read-Only  |
| `0x005C` | 4    | CMBEBS   | CMB Elasticity Buffer Size (optional)   | Read-Only  |
| `0x0060` | 4    | CMBSWTP  | CMB Sustained Write Throughput (opt)    | Read-Only  |
| `0x0E00` | 4    | PMRCAP   | Persistent Memory Region Cap (opt)      | Read-Only  |
| `0x0E04` | 4    | PMRCTL   | Persistent Memory Region Control (opt)  | Read/Write |
| `0x0E08` | 4    | PMRSTS   | Persistent Memory Region Status (opt)   | Read-Only  |
| `0x0E0C` | 4    | PMREBS   | PMR Elasticity Buffer Size (opt)        | Read-Only  |
| `0x0E10` | 4    | PMRSWTP  | PMR Sustained Write Throughput (opt)    | Read-Only  |
| `0x1000+` | 4   |          | Doorbell Registers (per queue)          | Write-Only |

> [!NOTE]
> Offsets `0x0018` and `0x001B` in the register space are **reserved**. Software must not
> read or write these locations. The gap between `CC` (0x14, 4 bytes) and `CSTS` (0x1C)
> contains 4 bytes of reserved space.

---

## Controller Capabilities Register (CAP) -- Offset 0x0000

The CAP register is a **64-bit read-only** register that defines the hardware limits of the
controller. The driver **must** parse this register before any configuration.

| Field    | Bits    | Description                                                            |
| -------- | ------- | ---------------------------------------------------------------------- |
| MQES     | 15:0    | Maximum Queue Entries Supported (0-based; 0xFFFF = 65,536 entries)     |
| CQR      | 16      | Contiguous Queues Required (`1` = queues must be physically contiguous)|
| AMS      | 18:17   | Arbitration Mechanism Supported (bit 17 = WRR+Urgent, bit 18 = Vendor)|
| TO       | 31:24   | Timeout in 500 ms units for `CSTS.RDY` transitions                     |
| DSTRD    | 35:32   | Doorbell Stride: stride = 2^(2+DSTRD) bytes between doorbells         |
| NSSRS    | 36      | NVM Subsystem Reset Supported (`1` = NSSR register is functional)      |
| CSS      | 44:37   | Command Sets Supported (bit 37 = NVM, bit 43 = Multiple I/O, etc.)     |
| BPS      | 45      | Boot Partition Support                                                  |
| CPS      | 47:46   | Controller Power Scope                                                  |
| MPSMIN   | 51:48   | Memory Page Size Minimum: min page = 2^(12+MPSMIN) bytes               |
| MPSMAX   | 55:52   | Memory Page Size Maximum: max page = 2^(12+MPSMAX) bytes               |
| PMRS     | 56      | Persistent Memory Region Supported                                      |
| CMBS     | 57      | Controller Memory Buffer Supported                                      |
| NSSS     | 58      | NVM Subsystem Shutdown Supported                                        |
| CRMS     | 60:59   | Controller Ready Modes Supported                                        |

> [!IMPORTANT]
> `CAP.TO` defines the **maximum** wait time for `CSTS.RDY` state transitions. If the
> controller does not respond within `CAP.TO × 500ms`, it is critically faulty and must
> be abandoned. Common values: `TO=0x28` (20 seconds), `TO=0x0A` (5 seconds).

> [!CAUTION]
> `CAP.MPSMIN` must be respected. If the OS page size is smaller than 2^(12+MPSMIN), the
> driver **cannot** operate. For x86-64 with 4 KiB pages, MPSMIN=0 (4 KiB) is required.

### Parsing Example

```c
uint64_t cap = mmio_read64(bar + 0x00);
uint16_t mqes      = (cap >>  0) & 0xFFFF;       /* max queue entries (0-based) */
uint8_t  cqr       = (cap >> 16) & 0x01;          /* contiguous queues required */
uint8_t  to        = (cap >> 24) & 0xFF;           /* timeout in 500ms units    */
uint8_t  dstrd     = (cap >> 32) & 0x0F;           /* doorbell stride           */
uint8_t  css       = (cap >> 37) & 0xFF;           /* command sets supported    */
uint8_t  mpsmin    = (cap >> 48) & 0x0F;           /* min page size exponent    */
uint8_t  mpsmax    = (cap >> 52) & 0x0F;           /* max page size exponent    */
```

---

## Controller Configuration Register (CC) -- Offset 0x0014

The CC register is a **32-bit read/write** register used to configure the controller before
enabling it. Most fields can only be modified when `CC.EN = 0`.

| Field   | Bits   | Description                                                             |
| ------- | ------ | ----------------------------------------------------------------------- |
| EN      | 0      | Enable (`1` = start processing, `0` = halt and reset)                   |
| CSS     | 6:4    | I/O Command Set Selected (`000b` = NVM Command Set)                     |
| MPS     | 10:7   | Memory Page Size: page size = 2^(12+MPS) bytes                         |
| AMS     | 13:11  | Arbitration Mechanism Selected (`000b` = Round Robin)                    |
| SHN     | 15:14  | Shutdown Notification (`00b` = none, `01b` = normal, `10b` = abrupt)    |
| IOSQES  | 19:16  | I/O Submission Queue Entry Size: entry = 2^IOSQES bytes (must be 6 = 64)|
| IOCQES  | 23:20  | I/O Completion Queue Entry Size: entry = 2^IOCQES bytes (must be 4 = 16)|
| CRIME   | 24     | Controller Ready Independent of Media Enable                             |

> [!IMPORTANT]
> `IOSQES` must be set to **6** (2^6 = 64 bytes per SQE) and `IOCQES` must be set to
> **4** (2^4 = 16 bytes per CQE) for the NVM Command Set. Failing to set these correctly
> causes undefined controller behavior.

> [!CAUTION]
> The `CC.MPS` field must be set to a value within the range `[CAP.MPSMIN, CAP.MPSMAX]`.
> Setting it outside this range is a fatal configuration error.

---

## Controller Status Register (CSTS) -- Offset 0x001C

The CSTS register is a **32-bit read-only** register indicating the current controller state.

| Field  | Bits  | Description                                                               |
| ------ | ----- | ------------------------------------------------------------------------- |
| RDY    | 0     | Ready (`1` = controller is ready to process commands)                     |
| CFS    | 1     | Controller Fatal Status (`1` = fatal error, controller is inoperable)     |
| SHST   | 3:2   | Shutdown Status (`00b` = normal, `01b` = processing, `10b` = complete)    |
| NSSRO  | 4     | NVM Subsystem Reset Occurred (`1` = subsystem reset detected)             |
| PP     | 5     | Processing Paused (`1` = controller paused due to resource exhaustion)    |
| ST     | 6     | Shutdown Type (indicates which shutdown type completed)                    |

> [!CAUTION]
> If `CSTS.CFS` is set to `1`, the controller has experienced a fatal internal error. No
> further commands will be processed. The driver must perform a full controller reset by
> clearing `CC.EN` to `0`, waiting for `CSTS.RDY = 0`, and re-initializing.

---

## Admin Queue Attributes Register (AQA) -- Offset 0x0024

The AQA register is a **32-bit read/write** register that specifies the sizes of the Admin
Submission Queue and Admin Completion Queue. Must be configured before setting `CC.EN = 1`.

| Field | Bits   | Description                                                              |
| ----- | ------ | ------------------------------------------------------------------------ |
| ASQS  | 11:0   | Admin Submission Queue Size (0-based; max 4095 = 4096 entries)           |
| ACQS  | 27:16  | Admin Completion Queue Size (0-based; max 4095 = 4096 entries)           |

> [!NOTE]
> The minimum queue size is 2 entries (value `0x1` in both fields). The maximum is 4096
> entries. A practical size for the admin queue is 32–64 entries.

---

## Admin Queue Base Address Registers

### ASQ -- Offset 0x0028 (Admin Submission Queue Base Address)

A **64-bit read/write** register containing the physical memory address of the Admin
Submission Queue. The address must be **page-aligned** (aligned to `CC.MPS`).

### ACQ -- Offset 0x0030 (Admin Completion Queue Base Address)

A **64-bit read/write** register containing the physical memory address of the Admin
Completion Queue. The address must be **page-aligned** (aligned to `CC.MPS`).

> [!CAUTION]
> Both ASQ and ACQ addresses must point to **physically contiguous** memory. The memory
> must remain allocated and stable for the entire lifetime of the controller.

---

## Doorbell Registers -- Offset 0x1000+

Doorbell registers are the mechanism by which the host notifies the controller of new
submissions and completed completions. Each queue gets two doorbells.

### Doorbell Offset Formulas

```
SQyTDBL (Submission Queue y Tail Doorbell):
    Offset = 0x1000 + (2y × (4 << CAP.DSTRD))

CQyHDBL (Completion Queue y Head Doorbell):
    Offset = 0x1000 + ((2y + 1) × (4 << CAP.DSTRD))
```

Where `y` is the queue identifier (0 = Admin Queue, 1+ = I/O Queues).

With `CAP.DSTRD = 0` (stride = 4 bytes, the most common value):

| Queue ID | SQ Tail Doorbell | CQ Head Doorbell |
| -------- | ---------------- | ---------------- |
| 0 (Admin)| `0x1000`         | `0x1004`         |
| 1        | `0x1008`         | `0x100C`         |
| 2        | `0x1010`         | `0x1014`         |
| N        | `0x1000 + 8×N`   | `0x1004 + 8×N`   |

> [!CAUTION]
> Doorbell registers are **write-only**. Reading them returns undefined, vendor-specific
> values. The host must track queue head/tail positions entirely in software.

> [!IMPORTANT]
> The host **must** insert a memory barrier (`mfence` on x86-64) between writing the SQE
> to host memory and writing the doorbell register. Without this barrier, the CPU may
> reorder the writes, causing the controller to fetch stale/garbage SQE data via DMA.

---

## Queue Architecture

NVMe uses paired circular ring buffers (Submission Queues and Completion Queues) residing in
host RAM. The controller accesses these queues via DMA.

### Queue Limits

| Parameter                     | Value                                     |
| ----------------------------- | ----------------------------------------- |
| Max I/O Submission Queues     | 65,535 (Queue IDs 1–65,535)               |
| Max I/O Completion Queues     | 65,535                                    |
| Max entries per queue         | 65,536 (limited by `CAP.MQES`)            |
| Admin Queue                   | 1 SQ + 1 CQ pair (Queue ID 0, mandatory) |
| SQE size                      | 64 bytes                                  |
| CQE size                      | 16 bytes                                  |

### Recommended Per-Core Queue Architecture

For maximum performance, allocate **one I/O SQ/CQ pair per CPU core**. This eliminates all
lock contention in the I/O submission path:

```
CPU Core 0 → SQ1/CQ1 → MSI-X Vector 1
CPU Core 1 → SQ2/CQ2 → MSI-X Vector 2
CPU Core 2 → SQ3/CQ3 → MSI-X Vector 3
...
CPU Core N → SQ(N+1)/CQ(N+1) → MSI-X Vector (N+1)
```

Multiple SQs can share a single CQ, but dedicating one CQ per SQ maintains optimal cache
locality and avoids completion queue contention.

---

## Submission Queue Entry (SQE) -- 64 Bytes

The SQE is the fixed-size command structure written by the host into Submission Queues.
All fields are **little-endian**.

```c
typedef struct {
    /* Command Dword 0 (CDW0) */
    uint32_t opcode   : 8;    /* command opcode                    */
    uint32_t fuse     : 2;    /* fused operation (0 = normal)      */
    uint32_t rsvd0    : 4;    /* reserved                          */
    uint32_t psdt     : 2;    /* PRP or SGL data transfer type     */
    uint32_t cid      : 16;   /* command identifier                */

    /* Command Dword 1 */
    uint32_t nsid;             /* namespace identifier              */

    /* Command Dwords 2–3 */
    uint32_t cdw2;             /* command-specific                  */
    uint32_t cdw3;             /* command-specific                  */

    /* Metadata Pointer (MPTR) -- Dwords 4–5 */
    uint64_t mptr;             /* metadata pointer                  */

    /* Data Pointer (DPTR) -- Dwords 6–9 */
    uint64_t prp1;             /* PRP Entry 1 or SGL first segment  */
    uint64_t prp2;             /* PRP Entry 2 or SGL second segment */

    /* Command Dwords 10–15 (command-specific) */
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} nvme_sqe_t;  /* 64 bytes total */
```

### SQE Field Details

| Field    | Offset | Size | Description                                                   |
| -------- | ------ | ---- | ------------------------------------------------------------- |
| CDW0     | 0x00   | 4    | Opcode (7:0), fuse (9:8), PSDT (15:14), CID (31:16)          |
| NSID     | 0x04   | 4    | Target namespace (0xFFFFFFFF = all namespaces)                |
| CDW2     | 0x08   | 4    | Command-specific (reserved for most commands)                 |
| CDW3     | 0x0C   | 4    | Command-specific (reserved for most commands)                 |
| MPTR     | 0x10   | 8    | Metadata pointer (physical address)                           |
| PRP1     | 0x18   | 8    | First PRP entry or first SGL segment descriptor               |
| PRP2     | 0x20   | 8    | Second PRP entry, PRP List pointer, or second SGL segment     |
| CDW10    | 0x28   | 4    | Command-specific parameter                                    |
| CDW11    | 0x2C   | 4    | Command-specific parameter                                    |
| CDW12    | 0x30   | 4    | Command-specific parameter                                    |
| CDW13    | 0x34   | 4    | Command-specific parameter                                    |
| CDW14    | 0x38   | 4    | Command-specific parameter                                    |
| CDW15    | 0x3C   | 4    | Command-specific parameter                                    |

---

## Completion Queue Entry (CQE) -- 16 Bytes

When the controller completes a command, it posts a CQE to the associated Completion Queue
via DMA write. All fields are **little-endian**.

```c
typedef struct {
    uint32_t cdw0;             /* command-specific result            */
    uint32_t rsvd;             /* reserved                           */
    uint16_t sq_head;          /* SQ Head Pointer (consumed entries) */
    uint16_t sq_id;            /* SQ Identifier                      */
    uint16_t cid;              /* Command Identifier (matches SQE)   */
    uint16_t status;           /* Status Field + Phase Tag           */
} nvme_cqe_t;  /* 16 bytes total */
```

### CQE Field Details

| Field    | Offset | Size | Description                                                    |
| -------- | ------ | ---- | -------------------------------------------------------------- |
| CDW0     | 0x00   | 4    | Command-specific result (e.g., created queue ID)               |
| Reserved | 0x04   | 4    | Reserved                                                        |
| SQHD     | 0x08   | 2    | SQ Head Pointer -- how far the controller has consumed the SQ   |
| SQID     | 0x0A   | 2    | Submission Queue ID this completion belongs to                  |
| CID      | 0x0C   | 2    | Command ID -- matches the CID from the original SQE              |
| Status   | 0x0E   | 2    | Phase tag (bit 0), Status Code Type (11:9), Status Code (15:1) |

### Status Field Layout (16 bits)

| Bits  | Field | Description                                                          |
| ----- | ----- | -------------------------------------------------------------------- |
| 0     | P     | Phase Tag -- toggles each time the CQ wraps around                    |
| 1     | SC.0  | Status Code bit 0                                                    |
| 8:1   | SC    | Status Code (8 bits)                                                 |
| 11:9  | SCT   | Status Code Type (`0` = Generic, `1` = Command Specific, `2` = Media)|
| 12    | CRD   | Command Retry Delay                                                  |
| 13    | M     | More -- additional status in Error Information log                    |
| 14    | DNR   | Do Not Retry (`1` = retrying will not succeed)                       |
| 15    | Rsvd  | Reserved                                                              |

> [!IMPORTANT]
> The Phase Tag bit is the **sole mechanism** for detecting new completions without reading
> controller registers across the PCIe bus. The host initializes all CQ memory to zero.
> When the controller posts a CQE, it writes bit 0 = `1`. On the next wrap-around of the
> circular CQ buffer, the controller flips to writing `0`, and so on. The host simply
> polls the expected phase bit in local RAM.

---

## Phase Tag Mechanism and Completion Detection

The phase tag eliminates the need for the host to read MMIO registers to detect completions.

### Lifecycle

1. Host initializes all CQ entries to zero (all phase bits = 0)
2. Host sets expected phase = 1
3. Controller writes CQE with phase = 1 when completing a command
4. Host polls `cqe[head].status & 0x01` -- if it matches expected phase, it is a new entry
5. Host processes the CQE and advances the head pointer
6. When head wraps to index 0, host flips expected phase (1 → 0, 0 → 1)
7. Host writes new head to CQ Head Doorbell to free consumed slots

```c
void nvme_poll_cq(nvme_cq_t *cq) {
    volatile nvme_cqe_t *entry = &cq->entries[cq->head];
    while ((entry->status & 0x01) == cq->phase) {
        /* Process completion: entry->cid, entry->status */
        nvme_process_completion(entry);

        cq->head++;
        if (cq->head >= cq->size) {
            cq->head = 0;
            cq->phase ^= 1;  /* flip expected phase on wrap */
        }
        entry = &cq->entries[cq->head];
    }
    /* Write new head to CQ Head Doorbell */
    mmio_write32(cq->doorbell, cq->head);
}
```

---

## Data Transfer Mechanisms

### Physical Region Pages (PRP)

PRPs are the mandatory, default mechanism for describing data buffer locations. A PRP entry
is a 64-bit physical memory address.

**PRP usage rules based on transfer size:**

| Transfer Size                  | PRP1                    | PRP2                              |
| ------------------------------ | ----------------------- | --------------------------------- |
| ≤ 1 memory page               | Data buffer address     | Unused (0)                        |
| Crosses 1 page boundary       | First page address      | Second page address               |
| Spans > 2 pages               | First page address      | Pointer to PRP List in host RAM   |

**PRP List** -- a physically contiguous array of 64-bit PRP entries pointing to subsequent
data pages. The list itself must be page-aligned and physically contiguous.

> [!CAUTION]
> PRP1 does **not** need to be page-aligned -- the offset within the page determines the
> starting byte. However, all subsequent PRP entries (in PRP2 or the PRP List) **must** be
> page-aligned.

### Scatter Gather Lists (SGL)

SGLs are an **optional** capability that allows arbitrary-length, byte-aligned memory
descriptors. Each SGL descriptor is 16 bytes:

```c
typedef struct {
    uint64_t address;     /* physical memory address   */
    uint32_t length;      /* byte count                */
    uint8_t  rsvd[3];     /* reserved                  */
    uint8_t  sgl_id;      /* descriptor type + subtype */
} nvme_sgl_desc_t;  /* 16 bytes */
```

SGL support is indicated by the `SGLS` field in the Identify Controller data structure.
If not supported, the driver **must** use PRPs exclusively.

---

## Initialization Sequence

The following sequence must be followed exactly to bring an NVMe controller from reset to
an operational state.

### Step-by-Step Controller Initialization

| Step | Action                                                                        |
| ---- | ----------------------------------------------------------------------------- |
| 1    | Map BAR0/BAR1 MMIO region into kernel virtual address space (uncached)        |
| 2    | Read `CAP` register -- record MQES, TO, DSTRD, MPSMIN, MPSMAX, CSS            |
| 3    | Read `VS` register -- verify controller NVMe version                           |
| 4    | Disable controller: write `CC.EN = 0`                                         |
| 5    | Wait for `CSTS.RDY = 0` (poll, timeout = `CAP.TO × 500 ms`)                  |
| 6    | Configure `CC`: set MPS, CSS=0 (NVM), AMS=0 (Round Robin), IOSQES=6, IOCQES=4|
| 7    | Allocate Admin SQ and Admin CQ in physically contiguous host memory           |
| 8    | Write Admin SQ physical address to `ASQ` register (offset 0x28)               |
| 9    | Write Admin CQ physical address to `ACQ` register (offset 0x30)               |
| 10   | Write queue sizes to `AQA` register (ASQS and ACQS fields)                    |
| 11   | Enable controller: set `CC.EN = 1`                                            |
| 12   | Wait for `CSTS.RDY = 1` (poll, timeout = `CAP.TO × 500 ms`)                  |
| 13   | If `CSTS.CFS = 1` after enable, controller is faulty -- abort initialization  |
| 14   | Issue Identify Controller (opcode 0x06, CNS=1) to get controller capabilities |
| 15   | Parse MDTS, SQES, CQES, NN (number of namespaces) from Identify data         |
| 16   | Issue Identify Namespace List (opcode 0x06, CNS=2) to enumerate namespaces    |
| 17   | Configure MSI-X interrupt vectors (1 per planned I/O CQ + 1 for Admin CQ)    |
| 18   | Create I/O CQs (Admin opcode 0x05) -- one per CPU core                        |
| 19   | Create I/O SQs (Admin opcode 0x01) -- one per CPU core, bound to its CQ       |
| 20   | Read namespace capacity via Identify Namespace (CNS=0) for each active NSID   |

> [!CAUTION]
> Steps 4–5 implement a **Controller Level Reset**. If `CSTS.RDY` does not transition to
> 0 within `CAP.TO × 500 ms`, the hardware is critically faulty. Do not proceed.

> [!IMPORTANT]
> The `MDTS` (Maximum Data Transfer Size) field from Identify Controller is expressed as a
> power of 2 multiplied by the minimum memory page size: `max_xfer = (1 << MDTS) × MPS`.
> If MDTS=0, there is no limit. The block layer **must** split I/O requests exceeding this
> limit into multiple SQEs.

---

## Graceful Shutdown Procedure

Proper shutdown is critical to prevent data loss. NVMe controllers use volatile DRAM write
caches internally; abrupt power loss without notification causes data corruption.

| Step | Action                                                                        |
| ---- | ----------------------------------------------------------------------------- |
| 1    | Complete all pending I/O commands (drain all I/O queues)                       |
| 2    | Delete all I/O SQs and CQs (Admin opcodes 0x00, 0x04)                        |
| 3    | Set `CC.SHN = 01b` (Normal Shutdown) or `CC.SHN = 10b` (Abrupt Shutdown)     |
| 4    | Poll `CSTS.SHST` until value = `10b` (Shutdown Processing Complete)           |
| 5    | Safe to cut power or reset the controller                                     |

> [!WARNING]
> If `CSTS.SHST` never reaches `10b`, the controller is hung. A hard reset is required.
> Each incomplete shutdown is recorded in the controller's SMART log as an "Unsafe Shutdown"
> and may degrade SSD lifespan.

---

## Admin Command Set

Admin commands operate exclusively on the Admin Queue (Queue ID 0). They manage controller
configuration, namespace topology, and health monitoring.

### Admin Command Opcodes

| Opcode | Command Name                   | Description                                        |
| ------ | ------------------------------ | -------------------------------------------------- |
| `0x00` | Delete I/O Submission Queue    | Removes an I/O SQ from the controller               |
| `0x01` | Create I/O Submission Queue    | Allocates an I/O SQ, binds to a CQ                  |
| `0x02` | Get Log Page                   | Retrieves error, SMART, and firmware log pages       |
| `0x04` | Delete I/O Completion Queue    | Removes an I/O CQ from the controller                |
| `0x05` | Create I/O Completion Queue    | Allocates an I/O CQ, assigns MSI-X vector            |
| `0x06` | Identify                       | Retrieves 4 KiB controller/namespace data structure  |
| `0x08` | Abort                          | Attempts to abort a previously submitted command     |
| `0x09` | Set Features                   | Configures operational parameters (arbitration, etc.)|
| `0x0A` | Get Features                   | Reads current feature configuration                  |
| `0x0C` | Asynchronous Event Request     | Registers for asynchronous hardware event alerts     |
| `0x10` | Firmware Commit                | Commits a firmware image to a specific slot          |
| `0x11` | Firmware Image Download        | Downloads a firmware image to the controller         |
| `0x80` | Format NVM                     | Low-level media format, cryptographic erase          |
| `0x81` | Security Send                  | Sends security protocol data to the controller       |
| `0x82` | Security Receive               | Receives security protocol data from the controller  |
| `0x84` | Sanitize                       | Irrecoverable data erasure (block, crypto, overwrite)|

### Create I/O Completion Queue (Opcode 0x05)

| CDW   | Bits   | Field                                                             |
| ----- | ------ | ----------------------------------------------------------------- |
| CDW10 | 15:0   | Queue Identifier (QID) -- unique ID for this CQ                   |
| CDW10 | 31:16  | Queue Size (QSIZE) -- 0-based (0 = 1 entry, 0xFFFF = 65,536)     |
| CDW11 | 0      | Physically Contiguous (PC) -- `1` if CQ memory is contiguous      |
| CDW11 | 1      | Interrupts Enabled (IEN) -- `1` to enable interrupts for this CQ  |
| CDW11 | 31:16  | Interrupt Vector (IV) -- MSI-X vector assigned to this CQ         |
| PRP1  |        | Physical address of the CQ memory                                 |

### Create I/O Submission Queue (Opcode 0x01)

| CDW   | Bits   | Field                                                             |
| ----- | ------ | ----------------------------------------------------------------- |
| CDW10 | 15:0   | Queue Identifier (QID) -- unique ID for this SQ                   |
| CDW10 | 31:16  | Queue Size (QSIZE) -- 0-based                                     |
| CDW11 | 0      | Physically Contiguous (PC) -- `1` if SQ memory is contiguous      |
| CDW11 | 2:1    | Queue Priority (QPRIO) -- `00b` Urgent, `01b` High, `10b`        |
|       |        | Medium, `11b` Low                                                 |
| CDW11 | 31:16  | Completion Queue Identifier (CQID) -- which CQ receives results   |
| PRP1  |        | Physical address of the SQ memory                                 |

> [!IMPORTANT]
> The CQ **must** be created before the SQ that references it. Attempting to create an SQ
> bound to a non-existent CQID results in an "Invalid Queue Identifier" error.

### Identify Command (Opcode 0x06)

The Identify command returns a 4,096-byte data structure. The `CNS` field in CDW10 selects
what to identify:

| CNS Value | Returns                                                               |
| --------- | --------------------------------------------------------------------- |
| `0x00`    | Namespace data structure for the specified NSID                       |
| `0x01`    | Controller data structure (capabilities, limits, vendor info)         |
| `0x02`    | Active Namespace List (array of NSIDs starting from CDW1.NSID)        |
| `0x03`    | Namespace Identification Descriptor list                              |
| `0x05`    | I/O Command Set specific Identify Namespace                           |
| `0x06`    | I/O Command Set specific Identify Controller                          |

**Key Identify Controller fields:**

| Offset | Size | Field    | Description                                                  |
| ------ | ---- | -------- | ------------------------------------------------------------ |
| 0      | 2    | VID      | PCI Vendor ID                                                |
| 2      | 2    | SSVID    | PCI Subsystem Vendor ID                                      |
| 4      | 20   | SN       | Serial Number (ASCII)                                        |
| 24     | 40   | MN       | Model Number (ASCII)                                         |
| 64     | 8    | FR       | Firmware Revision (ASCII)                                    |
| 77     | 1    | MDTS     | Max Data Transfer Size (power of 2 × MPS; 0 = no limit)     |
| 256    | 2    | OACS     | Optional Admin Command Support (bitmask)                     |
| 263    | 1    | FRMW     | Firmware Updates (number of slots, activation behavior)      |
| 512    | 1    | SQES     | SQ Entry Size (min/max as power of 2)                        |
| 513    | 1    | CQES     | CQ Entry Size (min/max as power of 2)                        |
| 516    | 4    | NN       | Number of Namespaces                                          |

**Key Identify Namespace fields:**

| Offset | Size | Field    | Description                                                  |
| ------ | ---- | -------- | ------------------------------------------------------------ |
| 0      | 8    | NSZE     | Namespace Size (total logical blocks)                        |
| 8      | 8    | NCAP     | Namespace Capacity (allocatable logical blocks)              |
| 16     | 8    | NUSE     | Namespace Utilization (currently used blocks)                |
| 25     | 1    | NLBAF    | Number of LBA Formats (0-based; max 63)                      |
| 26     | 1    | FLBAS    | Formatted LBA Size (bits 3:0 = active LBA format index)      |
| 128    | 64   | LBAF[]   | LBA Format array: each entry has RP (2 bits), LBADS (8 bits)|

> [!IMPORTANT]
> `LBAF[n].LBADS` gives the LBA data size as a power of 2 (e.g., 9 = 512 bytes, 12 = 4096
> bytes). The active LBA format index is bits 3:0 of the `FLBAS` field (offset 26).

---

## NVM Command Set (I/O Operations)

NVM I/O commands operate on I/O Queues (Queue IDs 1+) and perform actual data transfers.

### NVM I/O Command Opcodes

| Opcode | Command Name          | Description                                           |
| ------ | --------------------- | ----------------------------------------------------- |
| `0x00` | Flush                 | Commit all volatile write cache to non-volatile media  |
| `0x01` | Write                 | Write data from host memory to NVM                     |
| `0x02` | Read                  | Read data from NVM into host memory                    |
| `0x04` | Write Uncorrectable   | Mark LBA range as unreadable (returns error on read)   |
| `0x05` | Compare               | Compare NVM data with host buffer                      |
| `0x08` | Write Zeroes          | Set a range of LBAs to zero without data transfer      |
| `0x09` | Dataset Management    | TRIM/Deallocate hint for garbage collection            |
| `0x0C` | Verify                | Verify data integrity without returning data           |
| `0x0D` | Reservation Register  | Register a host with a namespace reservation           |
| `0x11` | Reservation Acquire   | Acquire an exclusive or shared reservation lock        |
| `0x15` | Reservation Release   | Release a previously acquired reservation              |
| `0x19` | Copy                  | Copy data within a namespace (added in NVMe 2.0)       |

### Read Command (Opcode 0x02) -- CDW Layout

| CDW   | Bits   | Field                                                             |
| ----- | ------ | ----------------------------------------------------------------- |
| CDW10 | 31:0   | Starting LBA (lower 32 bits)                                     |
| CDW11 | 31:0   | Starting LBA (upper 32 bits)                                     |
| CDW12 | 15:0   | Number of Logical Blocks (NLB) -- 0-based (0 = 1 block)           |
| CDW12 | 25     | Limited Retry (LR) -- `1` = limited retries on media error        |
| CDW12 | 26     | Force Unit Access (FUA) -- `1` = read from NVM, bypass cache      |
| PRP1  |        | Physical address of read destination buffer                       |
| PRP2  |        | Second PRP entry or PRP List pointer (for multi-page reads)       |

### Write Command (Opcode 0x01) -- CDW Layout

Identical to Read except data flows from host memory to NVM. Same CDW10–CDW12 layout.

### Flush Command (Opcode 0x00)

No additional parameters. NSID can be `0xFFFFFFFF` to flush all namespaces. The controller
commits all data from volatile write caches to non-volatile media.

> [!IMPORTANT]
> Flush must be issued after critical writes to ensure filesystem metadata consistency.
> Without Flush, data may be lost on unexpected power loss.

### Dataset Management / TRIM (Opcode 0x09)

CDW10 bits 7:0 = Number of Ranges (NR), 0-based. CDW11 bit 2 = Attribute Deallocate (AD).
The data buffer contains an array of 16-byte range descriptors:

```c
typedef struct {
    uint32_t cattr;        /* context attributes (optional)     */
    uint32_t nlb;          /* number of logical blocks           */
    uint64_t slba;         /* starting LBA                       */
} nvme_dsm_range_t;  /* 16 bytes */
```

---

## Log Pages and Diagnostics

The driver retrieves diagnostic data using the Get Log Page admin command (opcode 0x02).

### Mandatory Log Pages

| Log Page ID | Name                      | Description                                     |
| ----------- | ------------------------- | ----------------------------------------------- |
| `0x01`      | Error Information         | Circular queue of recent command failures        |
| `0x02`      | SMART / Health Information | Lifetime statistics, temperature, spare blocks   |
| `0x03`      | Firmware Slot Information  | Active firmware version, available update slots  |

### SMART / Health Information (Log Page 0x02) -- Key Fields

| Offset | Size | Field                       | Description                                |
| ------ | ---- | --------------------------- | ------------------------------------------ |
| 0      | 1    | Critical Warning            | Bitmask of active critical warnings        |
| 1      | 2    | Composite Temperature       | Temperature in Kelvin                      |
| 3      | 1    | Available Spare             | Remaining spare capacity (percentage)      |
| 4      | 1    | Available Spare Threshold   | Threshold for spare warning                |
| 5      | 1    | Percentage Used             | Estimated percentage of lifespan consumed  |
| 32     | 16   | Data Units Read             | Total 512-byte units read (in thousands)   |
| 48     | 16   | Data Units Written          | Total 512-byte units written (in thousands)|
| 64     | 16   | Host Read Commands          | Total read commands issued                 |
| 80     | 16   | Host Write Commands         | Total write commands issued                |
| 128    | 16   | Power Cycles                | Number of power on/off cycles              |
| 144    | 16   | Power On Hours              | Total hours powered on                     |
| 160    | 16   | Unsafe Shutdowns            | Count of shutdowns without CC.SHN          |
| 176    | 16   | Media and Data Integrity Errors | Unrecovered data integrity errors      |

> [!NOTE]
> Fields at offsets 32+ are 128-bit unsigned integers. For a bare-metal OS that does not
> support 128-bit arithmetic, reading the lower 64 bits is sufficient for practical use.

---

## Asynchronous Event Requests (AER)

AERs allow the controller to asynchronously notify the host of critical hardware events
without requiring constant log page polling.

### AER Protocol

1. During initialization, the host submits one or more AER commands (opcode 0x0C)
2. The controller holds these commands in a pending state (does not complete immediately)
3. When a critical event occurs, the controller completes one pending AER with the event type
4. The host processes the event and issues Get Log Page to get full details
5. The host immediately submits a fresh AER to replenish the pending pool

### AER Event Types (returned in CQE CDW0 bits 2:0)

| Type | Meaning                                                                   |
| ---- | ------------------------------------------------------------------------- |
| 0    | Error Status -- unrecoverable error occurred                               |
| 1    | SMART / Health Status -- threshold exceeded, temperature warning, etc.     |
| 2    | Notice -- namespace attribute changed, firmware activation, etc.           |
| 6    | I/O Command Set Specific                                                   |
| 7    | Vendor Specific                                                            |

The host can configure which events trigger AERs using Set Features (Feature ID `0x0B`).

---

## Interrupt Handling

### MSI-X (Recommended)

NVMe controllers support up to **2,048 MSI-X vectors**. Each I/O Completion Queue is bound
to a specific MSI-X vector during creation (Create I/O CQ, CDW11 bits 31:16).

**Configuration flow:**
1. Enumerate MSI-X capability in PCIe Capability List
2. Read MSI-X Table Size from Message Control register
3. Map MSI-X Table BAR and Pending Bit Array (PBA) BAR
4. For each vector: write target APIC address + data to MSI-X Table entry
5. Set Function Mask = 0, enable MSI-X in Message Control
6. Bind each CQ to its vector via Create I/O CQ CDW11

### Pin-Based and MSI Fallback

Legacy `INTx` and basic MSI are supported but severely limit performance. Use INTMS/INTMC
registers (offsets 0x0C/0x10) to mask/unmask interrupt vectors when using pin-based or
single-message MSI.

### Polling Mode (High-Performance)

For maximum IOPS, disable interrupts entirely for specific queues and use dedicated kernel
threads that continuously poll the CQ phase tag bit:

- **Advantage:** Eliminates context-switch overhead (~2–5 µs per interrupt saved)
- **Disadvantage:** Consumes 100% CPU on the polling core even when idle

A hybrid approach is recommended: use interrupt mode at low I/O depths, dynamically switch
to polling when queue depth exceeds a threshold.

---

## Namespace Management

NVMe controllers abstract physical storage into logical volumes called **Namespaces**, each
identified by a Namespace Identifier (NSID, 1-based).

### Namespace Properties

- Different namespaces can have different LBA sizes (512B, 4096B)
- Namespaces provide isolation for multi-tenancy
- A controller exposes at least 1 namespace (the `NN` field in Identify Controller)

### Multipath I/O (Asymmetric Namespace Access -- ANA)

In enterprise configurations, a single namespace may be accessible through multiple
controllers (dual-port SSDs, NVMe-oF). ANA reports the optimal path:

| ANA State        | Meaning                                                         |
| ---------------- | --------------------------------------------------------------- |
| Optimized        | Primary path -- lowest latency, full bandwidth                   |
| Non-Optimized    | Secondary path -- functional but higher latency                  |
| Inaccessible     | Path unavailable -- failover required                            |
| Persistent Loss  | Path permanently failed                                         |

### NVMe Reservations

For shared namespace access across multiple hosts, NVMe provides reservation commands to
prevent data corruption:

| Opcode | Command                | Purpose                                         |
| ------ | ---------------------- | ----------------------------------------------- |
| `0x0D` | Reservation Register   | Register host with unique reservation key        |
| `0x11` | Reservation Acquire    | Acquire exclusive or shared write lock            |
| `0x15` | Reservation Release    | Release write lock                                |

---

## Error Handling and Recovery

### Status Code Types (from CQE)

| SCT Value | Type                     | Description                                  |
| --------- | ------------------------ | -------------------------------------------- |
| `0x0`     | Generic Command Status   | Common errors (invalid opcode, invalid field) |
| `0x1`     | Command Specific Status  | Command-specific failures                     |
| `0x2`     | Media and Data Integrity | Unrecoverable read, write fault, CRC error    |
| `0x3`     | Path Related Status      | Multipath/ANA related errors                  |
| `0x7`     | Vendor Specific          | Vendor-defined status codes                    |

### Common Generic Status Codes (SCT=0)

| SC    | Name                           | Meaning                                   |
| ----- | ------------------------------ | ----------------------------------------- |
| `0x00`| Successful Completion          | Command completed without error            |
| `0x01`| Invalid Command Opcode         | Unrecognized opcode                        |
| `0x02`| Invalid Field in Command       | Bad CDW value or reserved bit set          |
| `0x05`| Aborted Due to SQ Deletion     | SQ was deleted while command was in flight |
| `0x0B`| Invalid Namespace or Format    | Bad NSID or namespace not attached         |
| `0x80`| LBA Out of Range               | Requested LBA exceeds namespace size       |
| `0x81`| Capacity Exceeded              | Write exceeds namespace capacity           |

### Controller Reset Recovery

If the controller enters a fatal state (`CSTS.CFS = 1`) or becomes unresponsive:

1. Clear `CC.EN = 0`
2. Wait for `CSTS.RDY = 0` (timeout = `CAP.TO × 500 ms`)
3. If timeout expires, perform a PCIe Function Level Reset (FLR)
4. Re-execute the full initialization sequence

---

## QEMU Testing Configuration

### Basic NVMe Device

```bash
qemu-system-x86_64 \
    -drive file=nvme-test.img,format=raw,if=none,id=nvme0 \
    -device nvme,serial=deadbeef,drive=nvme0
```

### Multi-Queue NVMe (Recommended for Multi-Core Testing)

```bash
qemu-system-x86_64 \
    -drive file=nvme-test.img,format=raw,if=none,id=nvme0 \
    -device nvme,serial=deadbeef,drive=nvme0,max_ioqpairs=4,msix_qsize=5
```

### Multiple Namespaces

```bash
qemu-system-x86_64 \
    -device nvme,serial=nvme0,id=nvme0 \
    -drive file=ns1.img,format=raw,if=none,id=ns1 \
    -device nvme-ns,drive=ns1,bus=nvme0,nsid=1 \
    -drive file=ns2.img,format=raw,if=none,id=ns2 \
    -device nvme-ns,drive=ns2,bus=nvme0,nsid=2
```

### Create Test Disk Images

```bash
qemu-img create -f raw nvme-test.img 1G
qemu-img create -f raw ns1.img 512M
qemu-img create -f raw ns2.img 256M
```

> [!NOTE]
> Use `max_ioqpairs` (not the deprecated `num_queues`) to control the number of I/O queue
> pairs. The default is 64. `msix_qsize` should be set to `max_ioqpairs + 1` to include
> the admin queue vector.

---

## Implementation Priorities for Impossible OS

| Priority | Feature                                     | Notes                                     |
| -------- | ------------------------------------------- | ----------------------------------------- |
| 🔴 P0    | PCIe enumeration and NVMe device detection  | Requires PCI driver (exists)              |
| 🔴 P0    | BAR mapping and controller register access  | Requires VMM uncached mapping             |
| 🔴 P0    | Controller reset and initialization         | CAP parsing, CC config, enable sequence   |
| 🔴 P0    | Admin Queue setup and Identify command      | First command to execute                  |
| 🔴 P0    | I/O Queue creation (1 pair initially)       | Create CQ then SQ                         |
| 🔴 P0    | Read/Write/Flush commands via PRP           | Core block I/O functionality              |
| 🟠 P1    | MSI-X interrupt handling                    | Requires MSI-X capability parsing         |
| 🟠 P1    | Per-core I/O queues (multi-queue)           | One SQ/CQ pair per CPU core               |
| 🟠 P1    | Block device layer integration              | Register as `blkdev`, mount filesystems   |
| 🟠 P1    | SMART / Health log page parsing             | Monitor drive health                      |
| 🟠 P1    | Graceful shutdown (CC.SHN)                  | Prevent data loss on OS shutdown          |
| 🟡 P2    | Dataset Management / TRIM                   | Improves SSD longevity                    |
| 🟡 P2    | Asynchronous Event Requests (AER)           | Proactive health monitoring               |
| 🟡 P2    | Multiple namespace support                  | Enumerate and expose N namespaces         |
| 🟡 P2    | Polling mode for high-performance I/O       | Dedicated polling threads                 |
| 🟢 P3    | SGL support (if hardware supports it)       | Alternative to PRP for fragmented memory  |
| 🟢 P3    | Namespace management (create/delete)        | Dynamic namespace provisioning            |
| 🟢 P3    | Firmware update commands                    | In-field firmware updates                 |
| 🟢 P3    | NVMe Reservations                           | Shared namespace access control           |
| 🔵 P4    | NVMe over Fabrics (RDMA/TCP transport)      | Networked NVMe storage                    |
| 🔵 P4    | Zoned Namespaces (ZNS) command set          | Zone-based SSD optimization               |
| 🔵 P4    | Key Value (KV) command set                  | KV-pair storage interface                 |
