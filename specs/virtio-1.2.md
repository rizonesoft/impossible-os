# VirtIO 1.2 Block Device — Technical Specification for OS Implementation

## Overview and Architectural Context

The Virtual I/O Device (VirtIO) specification defines a standard interface for paravirtualized devices in virtual machines, enabling efficient communication between guest operating systems and hypervisors. Ratified as an OASIS standard, VirtIO 1.2 (published July 2022) consolidates and extends the VirtIO 1.0 (2016) and VirtIO 1.1 (2019) specifications while maintaining full backward compatibility. The specification covers a family of device types — network, block, console, GPU, input, and more — but this document focuses exclusively on the **VirtIO Block Device (device type 2)** and the **PCI transport** mechanisms required to implement a production-quality block driver in a custom operating system kernel.

VirtIO achieves near-native I/O performance by eliminating the complex emulation overhead of traditional device models. Rather than emulating real hardware (e.g., an Intel ICH9 AHCI controller), VirtIO defines a minimal, purpose-built protocol where the guest driver and the hypervisor cooperate through shared memory structures called **virtqueues**. The host can DMA directly to/from guest memory, avoiding costly VM exits for each I/O operation.

The VirtIO block device (`virtio-blk`) is the simplest VirtIO device type and serves as the canonical example for understanding the transport layer. It exposes a raw block device (similar to `/dev/sda`) that the guest can read from and write to using sector-addressed requests.

### Version History and Compatibility

| Version | Date | Key Additions |
|---------|------|---------------|
| VirtIO 1.0 | April 2016 | Modern PCI transport, split virtqueues, feature negotiation, `VIRTIO_F_VERSION_1` |
| VirtIO 1.1 | February 2019 | Packed virtqueues (`VIRTIO_F_RING_PACKED`), in-order completion (`VIRTIO_F_IN_ORDER`) |
| VirtIO 1.2 | July 2022 | Multi-queue block (`VIRTIO_BLK_F_MQ`), secure erase, lifetime metrics, zone append (zoned storage) |

A modern VirtIO 1.0+ driver negotiates `VIRTIO_F_VERSION_1` (feature bit 32) to signal compliance with the modern spec. Legacy (pre-1.0) devices used PIO-based I/O ports; modern devices use MMIO via PCI capabilities.

---

## PCI Transport Layer

### PCI Device Identification

VirtIO PCI devices are identified by their Vendor ID and Device ID in the PCI configuration space:

| Field | Value | Notes |
|-------|-------|-------|
| Vendor ID | `0x1AF4` | Red Hat / VirtIO |
| Device ID (Transitional) | `0x1000`–`0x103F` | Legacy + modern support. Block = `0x1001` |
| Device ID (Modern) | `0x1040` + device_type | Modern-only. Block = `0x1042` (0x1040 + 2) |
| Subsystem Vendor ID | `0x1AF4` | |
| Subsystem Device ID | device_type | Block = `0x0002` |
| Revision ID | `0x01` | VirtIO 1.0+ compliant |

**Detection algorithm:**
1. Scan PCI for vendor `0x1AF4`
2. Accept device ID `0x1042` (modern block) or `0x1001` (transitional block)
3. For transitional devices (`0x1001`), verify Subsystem ID == `0x0002` to confirm block type
4. Enable Bus Mastering (PCI Command register bit 2) and Memory Space (bit 1)

### Modern PCI Capabilities

Modern VirtIO devices expose their configuration through **VirtIO Structure PCI Capabilities** in the PCI capability list. Each capability has a standard PCI capability header followed by VirtIO-specific fields:

```c
struct virtio_pci_cap {
    uint8_t  cap_vndr;     /* PCI cap ID: 0x09 (vendor-specific) */
    uint8_t  cap_next;     /* Next capability offset */
    uint8_t  cap_len;      /* Length of this capability (>= 16) */
    uint8_t  cfg_type;     /* VirtIO structure type (1-5) */
    uint8_t  bar;          /* BAR index (0-5) containing this structure */
    uint8_t  id;           /* Capability instance ID (for multiple of same type) */
    uint8_t  padding[2];   /* Alignment padding */
    uint32_t offset;       /* Offset within the BAR */
    uint32_t length;       /* Length of the structure in bytes */
};
```

The driver walks the PCI capability list starting from PCI config offset `0x34`, following `cap_next` links. For each capability with `cap_vndr == 0x09`, inspect `cfg_type`:

| `cfg_type` | Name | Purpose |
|------------|------|---------|
| 1 | `VIRTIO_PCI_CAP_COMMON_CFG` | Common VirtIO configuration (status, features, queue setup) |
| 2 | `VIRTIO_PCI_CAP_NOTIFY_CFG` | Virtqueue notification (doorbell) registers |
| 3 | `VIRTIO_PCI_CAP_ISR_CFG` | Interrupt Status Register (legacy INTx acknowledgment) |
| 4 | `VIRTIO_PCI_CAP_DEVICE_CFG` | Device-specific configuration (block device config) |
| 5 | `VIRTIO_PCI_CAP_PCI_CFG` | PCI configuration access (alternative to BAR mapping) |

For each capability, the driver maps the PCI BAR specified by `bar` into kernel virtual memory, then accesses the structure at `BAR_base + offset`.

### Common Configuration Structure (cfg_type 1)

The common configuration is the central control plane for the VirtIO device. All register accesses are MMIO (32-bit aligned reads/writes unless otherwise noted):

| Offset | Size | Name | Access | Description |
|--------|------|------|--------|-------------|
| `0x00` | 4 | `device_feature_select` | R/W | Feature page selector (0 = bits 0-31, 1 = bits 32-63) |
| `0x04` | 4 | `device_feature` | R | Device-offered features for selected page |
| `0x08` | 4 | `driver_feature_select` | R/W | Feature page selector for driver acceptance |
| `0x0C` | 4 | `driver_feature` | R/W | Driver-accepted features for selected page |
| `0x10` | 2 | `config_msix_vector` | R/W | MSI-X vector for config changes (`0xFFFF` = none) |
| `0x12` | 2 | `num_queues` | R | Maximum number of virtqueues supported |
| `0x14` | 1 | `device_status` | R/W | Device initialization status register |
| `0x15` | 1 | `config_generation` | R | Configuration atomicity counter |
| `0x16` | 2 | `queue_select` | R/W | Select which virtqueue to configure (0-indexed) |
| `0x18` | 2 | `queue_size` | R/W | Virtqueue size (number of descriptors, must be power of 2) |
| `0x1A` | 2 | `queue_msix_vector` | R/W | MSI-X vector for this queue (`0xFFFF` = none) |
| `0x1C` | 2 | `queue_enable` | R/W | Set to 1 to activate the virtqueue |
| `0x1E` | 2 | `queue_notify_off` | R | Notification offset multiplier for this queue |
| `0x20` | 8 | `queue_desc` | R/W | Physical address of descriptor table (64-bit) |
| `0x28` | 8 | `queue_driver` | R/W | Physical address of available ring (64-bit) |
| `0x30` | 8 | `queue_device` | R/W | Physical address of used ring (64-bit) |
| `0x38` | 2 | `queue_notify_data` | R | Extra notification data (VirtIO 1.2+) |
| `0x3A` | 2 | `queue_reset` | R/W | Queue reset control (VirtIO 1.2+) |

### Notification Structure (cfg_type 2)

The notification capability extends `virtio_pci_cap` with an additional field:

```c
struct virtio_pci_notify_cap {
    struct virtio_pci_cap cap;
    uint32_t notify_off_multiplier;  /* Multiplier for queue_notify_off */
};
```

To notify the device that a virtqueue has new buffers:

```c
notify_address = BAR_base + cap.offset + (queue_notify_off * notify_off_multiplier);
mmio_write16(notify_address, queue_index);
```

### ISR Status Register (cfg_type 3)

A single byte register for legacy INTx interrupt acknowledgment:

| Bit | Meaning |
|-----|---------|
| 0 | Virtqueue interrupt (used ring updated) |
| 1 | Device configuration change |

Reading the ISR register clears the interrupt. When using MSI-X, this register is not needed.

### MSI-X Interrupt Configuration

Production drivers should use MSI-X for interrupt delivery instead of legacy INTx pins. MSI-X avoids shared interrupt lines and enables per-queue interrupt vectors:

1. Check the PCI MSI-X capability (cap ID `0x11`) in the PCI capability list
2. Map the MSI-X Table BAR and Pending Bit Array BAR
3. For each virtqueue: assign a unique MSI-X vector via `queue_msix_vector`
4. Assign a vector for configuration changes via `config_msix_vector`
5. Enable MSI-X in the PCI MSI-X control register (set bit 15)
6. Each MSI-X table entry contains: message address (LAPIC destination), message data (interrupt vector), and a mask bit

Benefits of MSI-X over legacy INTx:
- Direct LAPIC delivery (no PIC/IOAPIC routing needed)
- Per-queue vectors (no shared interrupt disambiguation)
- Lower latency (no ISR register read required)

---

## Split Virtqueue Architecture

The **split virtqueue** is the fundamental data transport mechanism in VirtIO (the default since 1.0; the packed virtqueue from 1.1 is an optional alternative). Each virtqueue consists of three physically separate, page-aligned memory regions allocated by the **driver** (guest OS) in contiguous physical memory:

### Descriptor Table

An array of `queue_size` descriptors, each 16 bytes:

```c
struct virtq_desc {
    uint64_t addr;    /* Physical address of buffer */
    uint32_t len;     /* Length of buffer in bytes */
    uint16_t flags;   /* VIRTQ_DESC_F_NEXT, VIRTQ_DESC_F_WRITE, VIRTQ_DESC_F_INDIRECT */
    uint16_t next;    /* Index of next descriptor in chain (if NEXT flag set) */
};
```

| Flag | Value | Meaning |
|------|-------|---------|
| `VIRTQ_DESC_F_NEXT` | `0x01` | Another descriptor follows in the chain |
| `VIRTQ_DESC_F_WRITE` | `0x02` | Buffer is device-writable (device → driver) |
| `VIRTQ_DESC_F_INDIRECT` | `0x04` | Buffer contains an indirect descriptor table |

Descriptors are chained via `next` indices to form multi-buffer requests. The `flags` field determines data direction: device-readable (driver → device) buffers have `WRITE` cleared; device-writable (device → driver) buffers have `WRITE` set.

### Available Ring (Driver → Device)

The driver uses this ring to submit descriptor chain heads for processing:

```c
struct virtq_avail {
    uint16_t flags;        /* VIRTQ_AVAIL_F_NO_INTERRUPT = 0x01 */
    uint16_t idx;          /* Next slot to write (wraps modulo queue_size) */
    uint16_t ring[];       /* Array of queue_size descriptor head indices */
    /* uint16_t used_event; — only if VIRTIO_F_EVENT_IDX negotiated */
};
```

**Submission protocol:**
1. Write the head descriptor index into `ring[idx % queue_size]`
2. Memory barrier (`mfence` on x86-64)
3. Increment `idx`
4. Memory barrier
5. Write to the notification register to kick the device

### Used Ring (Device → Driver)

The device uses this ring to return completed requests:

```c
struct virtq_used_elem {
    uint32_t id;     /* Head descriptor index of the completed chain */
    uint32_t len;    /* Total bytes written by device */
};

struct virtq_used {
    uint16_t flags;          /* VIRTQ_USED_F_NO_NOTIFY = 0x01 */
    uint16_t idx;            /* Next slot the device will write */
    struct virtq_used_elem ring[];  /* Array of queue_size elements */
    /* uint16_t avail_event; — only if VIRTIO_F_EVENT_IDX negotiated */
};
```

**Completion protocol:**
1. Check if `used->idx != last_seen_used` (new completions available)
2. Memory barrier
3. Read `used->ring[last_seen_used % queue_size]` for the completed descriptor head + bytes written
4. Increment `last_seen_used`
5. Free the descriptor chain

### Memory Layout and Alignment

Each region must be physically contiguous and aligned:

| Region | Size | Alignment |
|--------|------|-----------|
| Descriptor Table | `16 × queue_size` bytes | 16 bytes |
| Available Ring | `6 + 2 × queue_size` bytes (+ 2 if EVENT_IDX) | 2 bytes |
| Used Ring | `6 + 8 × queue_size` bytes (+ 2 if EVENT_IDX) | 4 bytes |

For a typical `queue_size = 256`:
- Descriptor Table: 4,096 bytes (1 page)
- Available Ring: 518 bytes
- Used Ring: 2,054 bytes
- Total: ~6.5 KB per virtqueue

### Indirect Descriptors

When `VIRTIO_F_RING_INDIRECT_DESC` is negotiated, a single descriptor can point to a buffer containing an array of indirect descriptors. This allows submitting large scatter-gather lists without consuming entries from the main descriptor table:

```c
/* Primary descriptor */
desc[0].addr  = physical_address_of_indirect_table;
desc[0].len   = num_indirect * sizeof(struct virtq_desc);
desc[0].flags = VIRTQ_DESC_F_INDIRECT;
```

The indirect table itself is an array of `virtq_desc` entries with the same format, but `VIRTQ_DESC_F_INDIRECT` must not be set within indirect entries.

---

## Device Initialization Sequence

The VirtIO specification mandates a strict initialization protocol (§3.1.1). The driver communicates progress through the **device status** register:

### Status Register Bit Definitions

| Bit | Name | Value | Meaning |
|-----|------|-------|---------|
| 0 | `ACKNOWLEDGE` | `0x01` | Driver has found and recognized the device |
| 1 | `DRIVER` | `0x02` | Driver knows how to operate this device type |
| 3 | `FEATURES_OK` | `0x08` | Feature negotiation complete |
| 2 | `DRIVER_OK` | `0x04` | Driver is ready, device is live |
| 6 | `DEVICE_NEEDS_RESET` | `0x40` | Device experienced unrecoverable error |
| 7 | `FAILED` | `0x80` | Driver gave up on the device |

### Initialization Steps

```
1. RESET        — Write 0 to device_status → device resets all state
2. ACKNOWLEDGE  — Set ACKNOWLEDGE bit → "I found you"
3. DRIVER       — Set DRIVER bit → "I know what you are"
4. FEATURES     — Read device_feature (pages 0 and 1)
                   Write driver_feature (accept subset)
5. FEATURES_OK  — Set FEATURES_OK bit
                   Re-read device_status — if FEATURES_OK is cleared,
                   the device rejected the feature set → FAILED
6. QUEUE SETUP  — For each virtqueue (block device has 1 request queue):
                   a. Write queue_select = queue_index
                   b. Read queue_size (max descriptors supported)
                   c. Optionally write a smaller queue_size
                   d. Allocate descriptor table, available ring, used ring
                   e. Write physical addresses to queue_desc, queue_driver, queue_device
                   f. Write queue_enable = 1
                   g. Optionally assign MSI-X vector via queue_msix_vector
7. DRIVER_OK    — Set DRIVER_OK bit → device is now live
```

> [!CAUTION]
> The driver **must not** access device-specific configuration (cfg_type 4) until after `FEATURES_OK` is set and confirmed. The device-specific layout may differ based on negotiated features.

---

## Feature Bit Negotiation

Feature bits are the extensibility mechanism of VirtIO. The device advertises all features it supports; the driver accepts only those it understands. Unrecognized feature bits must be cleared by the driver.

### Generic Transport Features (Bits 24–41)

| Bit | Name | Description |
|-----|------|-------------|
| 24 | `VIRTIO_F_NOTIFY_ON_EMPTY` | Device notifies when queue is fully consumed |
| 28 | `VIRTIO_F_RING_INDIRECT_DESC` | Indirect descriptor support |
| 29 | `VIRTIO_F_RING_EVENT_IDX` | Used/avail event index (interrupt coalescing) |
| 32 | `VIRTIO_F_VERSION_1` | **Mandatory for modern transport.** Indicates VirtIO 1.0+ compliance |
| 33 | `VIRTIO_F_ACCESS_PLATFORM` | Device requires platform-specific IOMMU/IOTLB access |
| 34 | `VIRTIO_F_RING_PACKED` | Packed virtqueue format (VirtIO 1.1+) |
| 35 | `VIRTIO_F_IN_ORDER` | Device processes descriptors in submission order |
| 36 | `VIRTIO_F_ORDER_PLATFORM` | Memory ordering follows platform conventions |
| 37 | `VIRTIO_F_SR_IOV` | Single Root I/O Virtualization |
| 38 | `VIRTIO_F_NOTIFICATION_DATA` | Extra notification data (queue_notify_data) |
| 39 | `VIRTIO_F_NOTIF_CONFIG_DATA` | Driver passes notification data in notify write |
| 40 | `VIRTIO_F_RING_RESET` | Individual virtqueue reset (VirtIO 1.2+) |

### Block Device Features (Bits 0–15)

| Bit | Name | Description |
|-----|------|-------------|
| 0 | `VIRTIO_BLK_F_SIZE_MAX` | Maximum segment size in `size_max` config field |
| 1 | `VIRTIO_BLK_F_SEG_MAX` | Maximum segments per request in `seg_max` config field |
| 2 | `VIRTIO_BLK_F_GEOMETRY` | Legacy disk geometry available in config (cylinders/heads/sectors) |
| 4 | `VIRTIO_BLK_F_RO` | Device is read-only |
| 5 | `VIRTIO_BLK_F_BLK_SIZE` | Block size available in `blk_size` config field (may differ from 512) |
| 6 | `VIRTIO_BLK_F_FLUSH` | Cache flush command (`VIRTIO_BLK_T_FLUSH`) supported |
| 7 | `VIRTIO_BLK_F_TOPOLOGY` | Optimal I/O alignment info in config (physical block exponent, alignment offset, min/opt I/O sizes) |
| 9 | `VIRTIO_BLK_F_CONFIG_WCE` | Writeback cache enable is negotiable (config field `writeback`) |
| 11 | `VIRTIO_BLK_F_DISCARD` | Discard (TRIM) command supported |
| 12 | `VIRTIO_BLK_F_WRITE_ZEROES` | Write-zeroes command supported |
| 13 | `VIRTIO_BLK_F_LIFETIME` | Device lifetime metrics available |
| 14 | `VIRTIO_BLK_F_SECURE_ERASE` | Secure erase command supported (VirtIO 1.2+) |
| 15 | `VIRTIO_BLK_F_ZONED` | Zoned block device support (VirtIO 1.2+) |
| 22 | `VIRTIO_BLK_F_MQ` | Multi-queue support (VirtIO 1.2+) |

### Minimum Feature Set for a Production Driver

A functional block driver should negotiate at minimum:

```
Required:   VIRTIO_F_VERSION_1          — modern transport
Strongly recommended:
            VIRTIO_BLK_F_BLK_SIZE       — correct sector size
            VIRTIO_BLK_F_SEG_MAX        — scatter-gather limits
            VIRTIO_BLK_F_SIZE_MAX       — max segment size
            VIRTIO_BLK_F_FLUSH          — write durability
            VIRTIO_BLK_F_TOPOLOGY       — optimal I/O alignment

Production enhancements:
            VIRTIO_BLK_F_MQ             — per-CPU request queues
            VIRTIO_BLK_F_DISCARD        — SSD TRIM support
            VIRTIO_BLK_F_WRITE_ZEROES   — efficient zeroing
            VIRTIO_F_RING_INDIRECT_DESC — large scatter-gather
            VIRTIO_F_RING_EVENT_IDX     — interrupt coalescing
```

---

## Block Device Configuration Space (cfg_type 4)

The device-specific configuration is accessed via the `VIRTIO_PCI_CAP_DEVICE_CFG` capability. All fields are little-endian. The layout depends on which feature bits were negotiated:

| Offset | Size | Name | Feature Gate | Description |
|--------|------|------|--------------|-------------|
| `0x00` | 8 | `capacity` | Always | Disk size in 512-byte sectors (regardless of `blk_size`) |
| `0x08` | 4 | `size_max` | `F_SIZE_MAX` | Maximum size of any single segment (bytes) |
| `0x0C` | 4 | `seg_max` | `F_SEG_MAX` | Maximum number of segments per request |
| `0x10` | 2 | `geometry.cylinders` | `F_GEOMETRY` | Legacy CHS cylinders |
| `0x12` | 1 | `geometry.heads` | `F_GEOMETRY` | Legacy CHS heads |
| `0x13` | 1 | `geometry.sectors` | `F_GEOMETRY` | Legacy CHS sectors per track |
| `0x14` | 4 | `blk_size` | `F_BLK_SIZE` | Logical block size in bytes (replaces 512 assumption) |
| `0x18` | 1 | `topology.physical_block_exp` | `F_TOPOLOGY` | log₂(physical_block_size / logical_block_size) |
| `0x19` | 1 | `topology.alignment_offset` | `F_TOPOLOGY` | Offset of first aligned logical block (in logical blocks) |
| `0x1A` | 2 | `topology.min_io_size` | `F_TOPOLOGY` | Minimum I/O size (in logical blocks) |
| `0x1C` | 4 | `topology.opt_io_size` | `F_TOPOLOGY` | Optimal I/O size (in logical blocks) |
| `0x20` | 1 | `writeback` | `F_CONFIG_WCE` | 0 = writethrough, 1 = writeback caching |
| `0x22` | 2 | `num_queues` | `F_MQ` | Number of request queues (1 per vCPU typically) |
| `0x24` | 4 | `max_discard_sectors` | `F_DISCARD` | Max sectors per discard command |
| `0x28` | 4 | `max_discard_seg` | `F_DISCARD` | Max segments in a discard request |
| `0x2C` | 4 | `discard_sector_alignment` | `F_DISCARD` | Required alignment for discard (sectors) |
| `0x30` | 4 | `max_write_zeroes_sectors` | `F_WRITE_ZEROES` | Max sectors per write-zeroes command |
| `0x34` | 4 | `max_write_zeroes_seg` | `F_WRITE_ZEROES` | Max segments in a write-zeroes request |
| `0x38` | 1 | `write_zeroes_may_unmap` | `F_WRITE_ZEROES` | Device may deallocate zeroed regions |
| `0x3C` | 4 | `max_secure_erase_sectors` | `F_SECURE_ERASE` | Max sectors per secure erase (VirtIO 1.2+) |
| `0x40` | 4 | `max_secure_erase_seg` | `F_SECURE_ERASE` | Max segments in secure erase (VirtIO 1.2+) |
| `0x44` | 4 | `secure_erase_sector_alignment` | `F_SECURE_ERASE` | Alignment for secure erase (VirtIO 1.2+) |

> [!IMPORTANT]
> The `capacity` field reports size in **512-byte sectors** even when `blk_size` is not 512. The driver must accout for this: `disk_bytes = capacity * 512`. Data I/O requests also use 512-byte sector addressing in the request header.

### Configuration Atomicity

The `config_generation` counter in common_cfg enables atomic reads of multi-field device config:

```c
uint32_t gen_before, gen_after;
do {
    gen_before = mmio_read8(common_cfg + 0x15);  /* config_generation */
    /* ... read all device_cfg fields ... */
    gen_after = mmio_read8(common_cfg + 0x15);
} while (gen_before != gen_after);
```

---

## Block I/O Request Protocol

All block I/O is performed through the request virtqueue (queue 0, or queues 0 through `num_queues - 1` if `VIRTIO_BLK_F_MQ` is negotiated). Each request is a **3-descriptor chain**:

### Request Layout (3-Descriptor Chain)

```
Descriptor 0: Request Header     (device-readable, 16 bytes)
Descriptor 1: Data Buffer         (device-readable for writes, device-writable for reads)
Descriptor 2: Status Byte         (device-writable, 1 byte)
```

### Request Header Structure

```c
struct virtio_blk_req {
    uint32_t type;       /* Request type (see table below) */
    uint32_t reserved;   /* Must be 0 */
    uint64_t sector;     /* Starting sector (512-byte units) */
};
```

### Request Types

| Type Constant | Value | Description | Feature Gate |
|---------------|-------|-------------|--------------|
| `VIRTIO_BLK_T_IN` | `0x00000000` | Read sectors from device into buffer | Always |
| `VIRTIO_BLK_T_OUT` | `0x00000001` | Write sectors from buffer to device | Always |
| `VIRTIO_BLK_T_FLUSH` | `0x00000004` | Flush volatile write cache to persistent storage | `F_FLUSH` |
| `VIRTIO_BLK_T_GET_ID` | `0x00000008` | Get device serial number (20 bytes ASCII) | Always |
| `VIRTIO_BLK_T_DISCARD` | `0x0000000B` | Discard (TRIM) sectors — hint to reclaim space | `F_DISCARD` |
| `VIRTIO_BLK_T_WRITE_ZEROES` | `0x0000000D` | Write zeros to sectors without data transfer | `F_WRITE_ZEROES` |
| `VIRTIO_BLK_T_SECURE_ERASE` | `0x0000000E` | Cryptographically erase sectors (VirtIO 1.2+) | `F_SECURE_ERASE` |

### Status Byte Values

The device writes exactly 1 byte to the status descriptor:

| Value | Name | Meaning |
|-------|------|---------|
| `0x00` | `VIRTIO_BLK_S_OK` | Request completed successfully |
| `0x01` | `VIRTIO_BLK_S_IOERR` | Device I/O error |
| `0x02` | `VIRTIO_BLK_S_UNSUPP` | Request type not supported by this device |

### Read Request Example

```
Descriptor 0: { addr = &req_header, len = 16,   flags = NEXT }
                req_header.type   = VIRTIO_BLK_T_IN (0)
                req_header.sector = target_lba

Descriptor 1: { addr = data_buffer, len = count*512, flags = NEXT | WRITE }
                (WRITE flag = device fills this buffer)

Descriptor 2: { addr = &status_byte, len = 1,   flags = WRITE }
                (device writes completion status here)
```

### Write Request Example

```
Descriptor 0: { addr = &req_header, len = 16,   flags = NEXT }
                req_header.type   = VIRTIO_BLK_T_OUT (1)
                req_header.sector = target_lba

Descriptor 1: { addr = data_buffer, len = count*512, flags = NEXT }
                (no WRITE flag = device reads from this buffer)

Descriptor 2: { addr = &status_byte, len = 1,   flags = WRITE }
```

### Flush Request

Flush has no data buffer — the `sector` field is ignored:

```
Descriptor 0: { addr = &req_header, len = 16, flags = NEXT }
                req_header.type   = VIRTIO_BLK_T_FLUSH

Descriptor 1: { addr = &status_byte, len = 1, flags = WRITE }
```

> [!NOTE]
> Flush can use a 2-descriptor chain (header + status). The data descriptor is omitted.

### Get ID Request

Returns up to 20 bytes of ASCII device identification:

```
Descriptor 0: { addr = &req_header, len = 16,  flags = NEXT }
                req_header.type = VIRTIO_BLK_T_GET_ID

Descriptor 1: { addr = id_buffer,  len = 20,   flags = NEXT | WRITE }

Descriptor 2: { addr = &status,    len = 1,    flags = WRITE }
```

### Discard and Write-Zeroes Requests

These use a segment descriptor array instead of raw data:

```c
struct virtio_blk_discard_write_zeroes {
    uint64_t sector;       /* Starting sector */
    uint32_t num_sectors;  /* Number of sectors */
    uint32_t flags;        /* Bit 0: unmap (write-zeroes only) */
};
```

The data descriptor contains one or more of these segment structs (up to `max_discard_seg` or `max_write_zeroes_seg`).

---

## Multi-Queue Support (VirtIO 1.2+)

When `VIRTIO_BLK_F_MQ` is negotiated, the device exposes `num_queues` request queues (read from device config at offset `0x22`). Each queue operates independently with its own descriptor table, available ring, and used ring.

### Multi-Queue Architecture

```
Queue 0: Request queue (CPU 0)
Queue 1: Request queue (CPU 1)
  ...
Queue N-1: Request queue (CPU N-1)
```

**Implementation strategy:**
- Assign one request queue per CPU core
- Each CPU submits I/O to its local queue — no locking required
- Each queue gets its own MSI-X interrupt vector
- The device processes all queues in parallel

Benefits:
- Eliminates lock contention on the virtqueue
- Enables true parallel I/O across CPUs
- Matches the multi-queue model of modern NVMe hardware

---

## Interrupt Handling Strategies

### Legacy INTx (Basic)

1. Register IRQ handler for the PCI interrupt line (config offset `0x3C`)
2. In the handler: read ISR status register (cfg_type 3) to acknowledge
3. If bit 0 set: process used ring completions
4. If bit 1 set: re-read device configuration

> [!WARNING]
> Legacy INTx uses shared interrupt lines and requires masking/unmasking via the PIC or IOAPIC. This is the simplest path but has the worst performance. Per `rules.md`, Impossible OS routes all hardware interrupts via LAPIC/IOAPIC — legacy PIC masking must not be used.

### MSI-X (Recommended)

1. Discover MSI-X capability in PCI config
2. Allocate MSI-X vectors (1 per queue + 1 for config changes)
3. Program MSI-X table entries with LAPIC address/data
4. Assign vectors: `queue_msix_vector` for each queue, `config_msix_vector` for config
5. Enable MSI-X in PCI Message Control register
6. Each vector triggers a dedicated ISR — no disambiguation needed

### Polling (Fallback)

For simple implementations, skip interrupts entirely:
1. After submitting a request, spin-wait on `used->idx != last_seen_used`
2. Insert a brief I/O delay (`inb $0x80`) between polls
3. Set a timeout (e.g., 5 seconds) to avoid infinite hangs

This is the approach used by the current Impossible OS driver. It works but blocks the CPU during I/O.

---

## Error Handling and Device Reset

### Error Detection

- Status byte `VIRTIO_BLK_S_IOERR` → retry the request (limited retries, then report to caller)
- Status byte `VIRTIO_BLK_S_UNSUPP` → the device doesn't support this request type
- Device status bit 6 (`DEVICE_NEEDS_RESET`) → the device experienced an unrecoverable error
- I/O timeout → device is hung, needs full reset

### Device Reset Protocol

When the device signals `DEVICE_NEEDS_RESET` or a timeout occurs:

1. Write `0` to `device_status` → full device reset
2. Wait for `device_status` to read back `0`
3. Re-run the full initialization sequence (steps 1–7)
4. Resubmit any pending I/O requests

### Individual Queue Reset (VirtIO 1.2+)

When `VIRTIO_F_RING_RESET` is negotiated, individual queues can be reset without resetting the entire device:

1. Write `1` to `queue_reset` for the target queue
2. Wait for `queue_reset` to read back `1` (device acknowledged)
3. Free old virtqueue memory
4. Reallocate descriptor table, available ring, used ring
5. Write new addresses to `queue_desc`, `queue_driver`, `queue_device`
6. Write `0` to `queue_reset` to re-enable

---

## QEMU Testing Configuration

### Basic Block Device

```bash
qemu-system-x86_64 \
  -drive file=disk.img,format=raw,if=none,id=disk0 \
  -device virtio-blk-pci,drive=disk0
```

### Multi-Queue Block Device

```bash
qemu-system-x86_64 \
  -drive file=disk.img,format=raw,if=none,id=disk0 \
  -device virtio-blk-pci,drive=disk0,num-queues=4
```

### Discard / Write-Zeroes Support

```bash
qemu-system-x86_64 \
  -drive file=disk.img,format=raw,if=none,id=disk0,discard=unmap \
  -device virtio-blk-pci,drive=disk0,discard=on
```

### Read-Only Block Device

```bash
qemu-system-x86_64 \
  -drive file=disk.img,format=raw,if=none,id=disk0,readonly=on \
  -device virtio-blk-pci,drive=disk0
```

### Serial Output for Debugging

```bash
qemu-system-x86_64 \
  -serial stdio \
  -drive file=disk.img,format=raw,if=none,id=disk0 \
  -device virtio-blk-pci,drive=disk0
```

---

## Implementation Priorities for Impossible OS

Based on the gap analysis of the current `virtio_blk.c` driver:

| Priority | Enhancement | Impact |
|----------|-------------|--------|
| 🔴 P0 | Fix PIC IRQ routing → use IOAPIC/MSI-X | Rules compliance — current driver violates APIC-only mandate |
| 🔴 P0 | Negotiate `F_FLUSH` + implement flush request | Data integrity — FAT32/IXFS need write barriers |
| 🟠 P1 | Negotiate `F_BLK_SIZE` + read `blk_size` config | Correctness — 4K-sector drives will break without this |
| 🟠 P1 | Negotiate `F_SEG_MAX` / `F_SIZE_MAX` | Correctness — driver currently assumes unlimited segment size |
| 🟠 P1 | Negotiate `F_TOPOLOGY` + respect `opt_io_size` | Performance — aligned I/O is significantly faster |
| 🟠 P1 | Implement `VIRTIO_BLK_T_GET_ID` | Feature — device serial number for blkdev identification |
| 🟡 P2 | Asynchronous I/O (interrupt-driven instead of polling) | Performance — unblocks CPU during disk I/O |
| 🟡 P2 | Negotiate `F_DISCARD` + implement TRIM | SSD optimization — reclaim unused blocks |
| 🟡 P2 | Negotiate `F_WRITE_ZEROES` | Performance — efficient large zeroing |
| 🟢 P3 | Multi-queue support (`F_MQ`) | Scalability — per-CPU request queues |
| 🟢 P3 | Indirect descriptors (`F_RING_INDIRECT_DESC`) | Scalability — large scatter-gather lists |
| 🟢 P3 | Event index (`F_RING_EVENT_IDX`) | Performance — interrupt coalescing |
| 🔵 P4 | Packed virtqueue (`F_RING_PACKED`) | Performance — better cache utilization |
| 🔵 P4 | Secure erase (`F_SECURE_ERASE`) | Feature — cryptographic data erasure |
