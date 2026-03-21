# USB Mass Storage Class — Technical Specification for OS Implementation

## Overview and Architectural Context

The USB Mass Storage Class (MSC) defines a standard protocol for accessing block storage devices
over the Universal Serial Bus. It enables the operating system to read and write sectors on USB
flash drives, external hard disks, and multi-slot card readers using a layered architecture that
maps neatly onto the existing VFS and block device infrastructure.

The subsystem spans five vertically integrated layers:

```
┌─────────────────────────────────────┐
│  VFS / Block Device Abstraction     │  ← Standard open/read/write/close
├─────────────────────────────────────┤
│  SCSI Command Translator           │  ← READ(10), WRITE(10), INQUIRY
├─────────────────────────────────────┤
│  BOT Protocol Transport Driver      │  ← CBW / CSW encapsulation
├─────────────────────────────────────┤
│  USB Core Subsystem                 │  ← Enumeration, URBs, hub traversal
├─────────────────────────────────────┤
│  xHCI Host Controller Interface     │  ← PCIe MMIO, TRB rings, DMA
└─────────────────────────────────────┘
```

The Bulk-Only Transport (BOT) protocol is the sole transport mechanism for modern USB mass storage
devices. The legacy Control/Bulk/Interrupt (CBI) transport is deprecated by USB-IF for High-Speed
and above and is excluded from this specification.

### Version History and Compatibility

| Specification                   | Version | Date       | Key Additions                                       |
| ------------------------------- | ------- | ---------- | --------------------------------------------------- |
| USB Mass Storage Class (BOT)    | 1.0     | 1999-09-31 | Initial BOT protocol, CBW/CSW format                |
| USB Mass Storage Overview       | 1.4     | 2010-02-19 | Clarified subclass codes, updated class codes        |
| xHCI (Host Controller)          | 1.0     | 2010-05-21 | USB 3.0 SuperSpeed 5 Gbit/s, TRB rings              |
| xHCI                            | 1.1     | 2013-12-21 | USB 3.1 SuperSpeed+ 10 Gbit/s                       |
| xHCI                            | 1.2     | 2019-05    | USB 3.2 Gen 2x2 20 Gbit/s, multi-lane               |
| USB 2.0                         | 2.0     | 2000-04-27 | High-Speed 480 Mbit/s                                |
| USB 3.0                         | 3.0     | 2008-11-12 | SuperSpeed 5 Gbit/s                                  |
| USB 3.2                         | 3.2     | 2017-09-22 | Gen 1/2/2x2 naming, 20 Gbit/s multi-lane            |
| SCSI Primary Commands (SPC-4)   | 4       | 2014       | INQUIRY, TEST UNIT READY, REQUEST SENSE              |
| SCSI Block Commands (SBC-3)     | 3       | 2014       | READ(10), WRITE(10), READ CAPACITY(10)               |

---

## xHCI Host Controller Discovery and Identification

### PCI Configuration Space Fingerprint

The xHCI controller is discovered by scanning the PCIe configuration space for the following
class code triple:

| Field            | Value  | Meaning                       |
| ---------------- | ------ | ----------------------------- |
| Class Code       | `0x0C` | Serial Bus Controller         |
| Subclass         | `0x03` | USB Controller                |
| Programming I/F  | `0x30` | xHCI (USB 3.0+)              |

> [!NOTE]
> Legacy USB controllers use Programming Interface `0x00` (UHCI), `0x10` (OHCI), or `0x20`
> (EHCI). A bare-metal OS targeting modern hardware should focus on xHCI (`0x30`), which
> provides backward compatibility with USB 2.0 and USB 1.1 devices.

### Base Address Extraction

The xHCI MMIO base address is formed by concatenating `BAR0` and `BAR1` from the PCI
configuration header (offsets `0x10` and `0x14`) into a single 64-bit physical address.

> [!CAUTION]
> The page table entries mapping the xHCI MMIO space **must** be marked as uncacheable
> (`PCD=1`, `PWT=1` or use the PAT UC type). Using cached mappings causes the CPU to read
> stale register values from its cache instead of observing asynchronous hardware state changes,
> leading to catastrophic synchronization failures.

---

## xHCI MMIO Register Layout

The xHCI MMIO space is organized into four distinct regions, each at a dynamically computed
offset from the BAR base.

### Memory Map Overview

```
BAR Base + 0x00    ┌───────────────────────┐
                   │  Capability Registers  │  (read-only, fixed layout)
BAR Base + CAPLENGTH┌───────────────────────┐
                   │  Operational Registers │  (read/write, command & status)
BAR Base + RTSOFF  ┌───────────────────────┐
                   │  Runtime Registers     │  (event ring, interrupters)
BAR Base + DBOFF   ┌───────────────────────┐
                   │  Doorbell Registers    │  (one per device slot)
                   └───────────────────────┘
```

### Capability Registers (offset `0x00`)

These read-only registers describe the controller's structural parameters. All offsets are
relative to the BAR base address.

| Offset | Size | Register       | Description                                                    |
| ------ | ---- | -------------- | -------------------------------------------------------------- |
| `0x00` | 1    | `CAPLENGTH`    | Length of Capability Register space in bytes                   |
| `0x02` | 2    | `HCIVERSION`   | xHCI interface version (e.g., `0x0110` = 1.1)                 |
| `0x04` | 4    | `HCSPARAMS1`   | Structural: max device slots (bits 7:0), max interrupters      |
|        |      |                | (bits 18:8), max ports (bits 31:24)                            |
| `0x08` | 4    | `HCSPARAMS2`   | Structural: IST, ERST max, scratchpad bufs high/low            |
| `0x0C` | 4    | `HCSPARAMS3`   | Structural: U1/U2 device exit latency                          |
| `0x10` | 4    | `HCCPARAMS1`   | Capability: 64-bit addressing (bit 0), context size (bit 2)   |
| `0x14` | 4    | `DBOFF`        | Doorbell Array offset (relative to BAR base)                   |
| `0x18` | 4    | `RTSOFF`       | Runtime Register Space offset (relative to BAR base)           |
| `0x1C` | 4    | `HCCPARAMS2`   | Extended capabilities (LEC, CIC, etc.)                         |

### Operational Registers (offset `CAPLENGTH`)

All offsets below are relative to `BAR base + CAPLENGTH`.

| Offset | Size | Register   | Description                                                      |
| ------ | ---- | ---------- | ---------------------------------------------------------------- |
| `0x00` | 4    | `USBCMD`   | USB Command: Run/Stop (bit 0), HCRST (bit 1), INTE (bit 2)      |
| `0x04` | 4    | `USBSTS`   | USB Status: HCH (bit 0), HSE (bit 2), EINT (bit 3), PCD (bit 4) |
| `0x08` | 4    | `PAGESIZE` | Supported page sizes (bit N → 2^(N+12) bytes)                   |
| `0x14` | 4    | `DNCTRL`   | Device Notification Control                                      |
| `0x18` | 8    | `CRCR`     | Command Ring Control Register (64-bit physical address)          |
| `0x30` | 8    | `DCBAAP`   | Device Context Base Address Array Pointer (64-bit, 64-byte align)|
| `0x38` | 4    | `CONFIG`   | Max Device Slots Enabled (bits 7:0)                              |

> [!IMPORTANT]
> **Port Registers** start at offset `0x400` within the Operational space. Each port occupies
> 16 bytes: `PORTSC` (0x00), `PORTPMSC` (0x04), `PORTLI` (0x08), `PORTHLPMC` (0x0C).
> Port N registers are at `0x400 + (N × 0x10)`.

### Runtime Registers (offset `RTSOFF`)

All offsets below are relative to `BAR base + RTSOFF`.

| Offset | Size | Register      | Description                                         |
| ------ | ---- | ------------- | --------------------------------------------------- |
| `0x00` | 4    | `MFINDEX`     | Microframe index counter                            |
| `0x20` | 32   | Interrupter 0 | First Interrupter Register Set (see below)          |

Each Interrupter Register Set (32 bytes, starting at `0x20 + (N × 0x20)`):

| Offset | Size | Register | Description                                               |
| ------ | ---- | -------- | --------------------------------------------------------- |
| `0x00` | 4    | `IMAN`   | Interrupter Management: IP (bit 0), IE (bit 1)            |
| `0x04` | 4    | `IMOD`   | Interrupter Moderation: interval (bits 15:0), counter     |
| `0x08` | 4    | `ERSTSZ` | Event Ring Segment Table Size                             |
| `0x10` | 8    | `ERSTBA` | Event Ring Segment Table Base Address (64-bit, 64B align) |
| `0x18` | 8    | `ERDP`   | Event Ring Dequeue Pointer (64-bit)                       |

### Doorbell Registers (offset `DBOFF`)

An array of 32-bit registers, one per device slot (plus slot 0 for the host controller).
Writing to `Doorbell[slot]` notifies the xHCI that new TRBs are enqueued on that slot's
transfer ring. The doorbell value encodes the target endpoint (bits 7:0) and stream ID
(bits 31:16).

---

## Transfer Request Block (TRB) Architecture

All communication between software and the xHCI controller occurs through 16-byte Transfer
Request Blocks organized into circular ring buffers in physically contiguous DMA memory.

### TRB Generic Layout

```c
struct xhci_trb {
    uint64_t parameter;      /* Bytes 0–7:  data buffer pointer or parameter  */
    uint32_t status;         /* Bytes 8–11: transfer length, TD size, etc.    */
    uint32_t control;        /* Bytes 12–15: cycle bit, TRB type, flags       */
};
```

All fields are **little-endian**. The `control` field layout:

| Bits   | Field      | Description                                               |
| ------ | ---------- | --------------------------------------------------------- |
| 0      | Cycle (C)  | Producer/consumer ownership toggle                        |
| 1      | ENT        | Evaluate Next TRB (for multi-TRB operations)              |
| 4      | Chain (CH) | Links this TRB to the next as part of the same TD         |
| 5      | IOC        | Interrupt On Completion — generate event when done        |
| 15:10  | TRB Type   | Identifies the TRB kind (see table below)                 |

### TRB Type Codes

| Code | Name                    | Ring      | Description                              |
| ---- | ----------------------- | --------- | ---------------------------------------- |
| 1    | Normal                  | Transfer  | Bulk/interrupt/isoch data transfer       |
| 2    | Setup Stage             | Transfer  | Control transfer setup packet            |
| 3    | Data Stage              | Transfer  | Control transfer data phase              |
| 4    | Status Stage            | Transfer  | Control transfer status phase            |
| 6    | Link                    | Any       | Points to next ring segment              |
| 9    | Enable Slot Command     | Command   | Allocate a device slot                   |
| 11   | Address Device Command  | Command   | Assign USB address to device             |
| 12   | Configure Endpoint Cmd  | Command   | Configure endpoint contexts              |
| 32   | Transfer Event          | Event     | Transfer completion notification         |
| 33   | Command Completion      | Event     | Command ring completion                  |
| 34   | Port Status Change      | Event     | Port connect/disconnect/reset            |

### Ring Types and Memory Requirements

| Ring            | Count           | Alignment | Boundary   | Description                        |
| --------------- | --------------- | --------- | ---------- | ---------------------------------- |
| Command Ring    | 1 per controller| 64 bytes  | 64 KiB     | Software → controller commands     |
| Event Ring      | 1 per interrupter| 64 bytes | 64 KiB     | Controller → software events       |
| Transfer Ring   | 1 per endpoint  | 16 bytes  | 64 KiB     | Data transfer TRBs per endpoint    |

> [!CAUTION]
> A single TRB's data buffer **must not cross a 64 KiB physical address boundary**. When
> transferring large payloads, the driver must split buffers into fragments ≤ 64 KiB that
> each stay within a single 64 KiB-aligned region, chaining them with the Chain bit.

### Event Ring Segment Table Entry (ERST)

```c
struct xhci_erst_entry {
    uint64_t ring_segment_base;    /* Physical address of event ring segment, 64B aligned */
    uint32_t ring_segment_size;    /* Number of TRBs in this segment                      */
    uint32_t reserved;
};
```

---

## xHCI Controller Initialization Sequence

The following steps must be executed **in order** to bring the xHCI controller to an
operational state:

1. **Discover controller** via PCI enumeration (Class `0x0C`, Subclass `0x03`, PI `0x30`).
2. **Map MMIO space** — read `BAR0`/`BAR1`, map as uncacheable pages.
3. **Read capabilities** — parse `CAPLENGTH`, `HCSPARAMS1`, `HCCPARAMS1` to determine
   max slots, max ports, 64-bit support, and context size.
4. **Halt controller** — set `USBCMD.RS = 0`, wait for `USBSTS.HCH = 1`.
5. **Reset controller** — set `USBCMD.HCRST = 1`, wait for `USBCMD.HCRST = 0` AND
   `USBSTS.CNR = 0`.
6. **Configure max slots** — write `CONFIG.MaxSlotsEn` with desired device count.
7. **Allocate DCBAA** — allocate 64-byte-aligned, physically contiguous array of
   `(MaxSlots + 1)` entries. Write physical address to `DCBAAP`.
8. **Allocate scratchpad buffers** — if `HCSPARAMS2` indicates scratchpad count > 0,
   allocate page-aligned buffers and store pointers at `DCBAA[0]`.
9. **Allocate Command Ring** — allocate 64-byte-aligned TRB ring. Write physical address
   to `CRCR` (with cycle bit in bit 0).
10. **Allocate Event Ring** — allocate ERST entries and event ring segments. Write to
    `ERSTSZ`, `ERSTBA`, and initialize `ERDP`.
11. **Enable interrupts** — set `USBCMD.INTE = 1` and `IMAN.IE = 1` for interrupter 0.
    Configure MSI/MSI-X via PCI capability structures.
12. **Start controller** — set `USBCMD.RS = 1`, wait for `USBSTS.HCH = 0`.

---

## USB Device Enumeration and Descriptor Parsing

When the xHCI controller detects a device connection, it generates a Port Status Change
Event TRB. The driver must then perform the USB enumeration sequence to identify the device.

### Enumeration Sequence

1. **Detect port event** — read Port Status Change Event from Event Ring.
2. **Reset port** — set `PORTSC.PR = 1`, wait for Port Reset Change.
3. **Enable Slot** — submit Enable Slot Command TRB on Command Ring.
4. **Address Device** — submit Address Device Command with Input Context containing
   Slot Context and Endpoint 0 Context (max packet size from port speed).
5. **GET_DESCRIPTOR (Device)** — control transfer on EP0 to read 18-byte Device Descriptor.
6. **GET_DESCRIPTOR (Configuration)** — two-stage: first read 9-byte header to get
   `wTotalLength`, then allocate and read full descriptor tree.
7. **SET_CONFIGURATION** — select the desired configuration.
8. **Configure Endpoint** — submit Configure Endpoint Command with all discovered endpoints.

### USB Descriptor Structures

#### Device Descriptor (18 bytes, `bDescriptorType = 0x01`)

```c
struct usb_device_descriptor {
    uint8_t  bLength;              /* 18                                       */
    uint8_t  bDescriptorType;      /* 0x01                                     */
    uint16_t bcdUSB;               /* USB spec version (BCD), e.g., 0x0200     */
    uint8_t  bDeviceClass;         /* 0x00 = per-interface class               */
    uint8_t  bDeviceSubClass;      /* 0x00 for MSC                             */
    uint8_t  bDeviceProtocol;      /* 0x00 for MSC                             */
    uint8_t  bMaxPacketSize0;      /* Max packet size for EP0 (8,16,32,64)     */
    uint16_t idVendor;             /* Vendor ID (USB-IF assigned)              */
    uint16_t idProduct;            /* Product ID (manufacturer assigned)       */
    uint16_t bcdDevice;            /* Device release number (BCD)              */
    uint8_t  iManufacturer;        /* String descriptor index                  */
    uint8_t  iProduct;             /* String descriptor index                  */
    uint8_t  iSerialNumber;        /* String descriptor index                  */
    uint8_t  bNumConfigurations;   /* Number of configurations                 */
};
```

All multi-byte fields are **little-endian**.

#### Configuration Descriptor (9-byte header, `bDescriptorType = 0x02`)

```c
struct usb_config_descriptor {
    uint8_t  bLength;              /* 9                                        */
    uint8_t  bDescriptorType;      /* 0x02                                     */
    uint16_t wTotalLength;         /* Total length of all descriptors          */
    uint8_t  bNumInterfaces;       /* Number of interfaces                     */
    uint8_t  bConfigurationValue;  /* Value to select this configuration       */
    uint8_t  iConfiguration;       /* String descriptor index                  */
    uint8_t  bmAttributes;         /* Self-powered (bit 6), remote wakeup (5)  */
    uint8_t  bMaxPower;            /* Max current in 2 mA units                */
};
```

#### Interface Descriptor (9 bytes, `bDescriptorType = 0x04`)

```c
struct usb_interface_descriptor {
    uint8_t  bLength;              /* 9                                        */
    uint8_t  bDescriptorType;      /* 0x04                                     */
    uint8_t  bInterfaceNumber;     /* Zero-based interface index               */
    uint8_t  bAlternateSetting;    /* Alternate setting index                  */
    uint8_t  bNumEndpoints;        /* Number of endpoints (excl. EP0)          */
    uint8_t  bInterfaceClass;      /* 0x08 = Mass Storage                      */
    uint8_t  bInterfaceSubClass;   /* 0x06 = SCSI Transparent Command Set      */
    uint8_t  bInterfaceProtocol;   /* 0x50 = Bulk-Only Transport (BOT)         */
    uint8_t  iInterface;           /* String descriptor index                  */
};
```

#### Endpoint Descriptor (7 bytes, `bDescriptorType = 0x05`)

```c
struct usb_endpoint_descriptor {
    uint8_t  bLength;              /* 7                                        */
    uint8_t  bDescriptorType;      /* 0x05                                     */
    uint8_t  bEndpointAddress;     /* Endpoint number (bits 3:0) + dir (bit 7) */
    uint8_t  bmAttributes;         /* Transfer type: 0x02 = Bulk               */
    uint16_t wMaxPacketSize;       /* Max packet size for this endpoint        */
    uint8_t  bInterval;            /* Polling interval (ignored for bulk)      */
};
```

### Mass Storage Class Identification

A USB interface is identified as a BOT Mass Storage device when its Interface Descriptor
matches **all three** of the following:

| Field                  | Value  | Meaning                             |
| ---------------------- | ------ | ----------------------------------- |
| `bInterfaceClass`      | `0x08` | Mass Storage Class                  |
| `bInterfaceSubClass`   | `0x06` | SCSI Transparent Command Set        |
| `bInterfaceProtocol`   | `0x50` | Bulk-Only Transport (BOT)           |

> [!IMPORTANT]
> For most USB flash drives and external HDDs, `bDeviceClass`, `bDeviceSubClass`, and
> `bDeviceProtocol` in the Device Descriptor are all `0x00`, deferring class identification
> to the Interface Descriptor. Always check the interface level.

### Endpoint Requirements

A compliant BOT device exposes exactly three endpoints:

| Endpoint       | Type    | Direction | Purpose                            |
| -------------- | ------- | --------- | ---------------------------------- |
| EP0            | Control | Bidir     | Enumeration, class requests        |
| Bulk-IN        | Bulk    | IN        | Data from device → host            |
| Bulk-OUT       | Bulk    | OUT       | Commands and data from host → device|

> [!NOTE]
> If an interrupt endpoint appears on a BOT interface, **ignore it**. BOT operates
> exclusively via bulk transfers. The interrupt endpoint belongs to the legacy CBI transport.

---

## Class-Specific Control Requests

### Get Max LUN (`bRequest = 0xFE`)

Discovers the number of Logical Unit Numbers (independent media slots) behind this interface.

| Field            | Value                    | Description                          |
| ---------------- | ------------------------ | ------------------------------------ |
| `bmRequestType`  | `0xA1`                   | Class, Interface, Device-to-Host     |
| `bRequest`       | `0xFE`                   | Get Max LUN                          |
| `wValue`         | `0x0000`                 | Reserved                             |
| `wIndex`         | Interface Number         | Target interface                     |
| `wLength`        | `0x0001`                 | 1 byte response                      |

Response: a single byte containing the maximum LUN index (`0x00`–`0x0F`). A value of `0x03`
means 4 LUNs (0, 1, 2, 3).

> [!NOTE]
> If the device **STALLs** this request, assume `Max LUN = 0` (single LUN at index 0).
> Clear the STALL with a ClearFeature(ENDPOINT_HALT) on EP0.

### Bulk-Only Mass Storage Reset (`bRequest = 0xFF`)

Resets the BOT state machine on the device without affecting the USB bus connection.

| Field            | Value                    | Description                          |
| ---------------- | ------------------------ | ------------------------------------ |
| `bmRequestType`  | `0x21`                   | Class, Interface, Host-to-Device     |
| `bRequest`       | `0xFF`                   | Mass Storage Reset                   |
| `wValue`         | `0x0000`                 | Reserved                             |
| `wIndex`         | Interface Number         | Target interface                     |
| `wLength`        | `0x0000`                 | No data                              |

The device preserves its bulk data toggle bits and endpoint STALL conditions. It will NAK
the status stage until the reset is complete.

---

## Bulk-Only Transport (BOT) Protocol

Every BOT transaction consists of up to three strictly ordered phases:

```
Host → Device:  Command Block Wrapper (CBW)     [31 bytes, Bulk-OUT]
                        ↓
Host ↔ Device:  Data Phase (optional)            [Bulk-IN or Bulk-OUT]
                        ↓
Device → Host:  Command Status Wrapper (CSW)     [13 bytes, Bulk-IN]
```

### Command Block Wrapper (CBW) — 31 bytes

All multi-byte fields are **little-endian**.

```c
struct usb_msc_cbw {
    uint32_t dCBWSignature;        /* Must be 0x43425355 ("USBC")              */
    uint32_t dCBWTag;              /* Unique transaction ID from host          */
    uint32_t dCBWDataTransferLength; /* Expected data phase byte count         */
    uint8_t  bmCBWFlags;           /* Bit 7: 0=Data-Out, 1=Data-In (0x80)     */
    uint8_t  bCBWLUN;              /* Target LUN (bits 3:0), bits 7:4 reserved */
    uint8_t  bCBWCBLength;         /* Valid length of CBWCB (1–16)             */
    uint8_t  CBWCB[16];            /* SCSI Command Descriptor Block            */
};
```

| Offset | Size | Field                    | Description                                |
| ------ | ---- | ------------------------ | ------------------------------------------ |
| `0x00` | 4    | `dCBWSignature`          | `0x43425355` — identifies packet as CBW    |
| `0x04` | 4    | `dCBWTag`                | Unique tag, echoed in CSW for correlation  |
| `0x08` | 4    | `dCBWDataTransferLength` | Bytes expected in Data Phase (0 = no data) |
| `0x0C` | 1    | `bmCBWFlags`             | Bit 7: direction. `0x80`=IN, `0x00`=OUT    |
| `0x0D` | 1    | `bCBWLUN`                | Target Logical Unit Number (0–15)          |
| `0x0E` | 1    | `bCBWCBLength`           | Length of SCSI CDB (1–16 bytes)            |
| `0x0F` | 16   | `CBWCB`                  | SCSI Command Descriptor Block payload      |

### Command Status Wrapper (CSW) — 13 bytes

All multi-byte fields are **little-endian**.

```c
struct usb_msc_csw {
    uint32_t dCSWSignature;        /* Must be 0x53425355 ("USBS")              */
    uint32_t dCSWTag;              /* Must match dCBWTag of initiating CBW     */
    uint32_t dCSWDataResidue;      /* Difference: expected − actual bytes      */
    uint8_t  bCSWStatus;           /* 0x00=Passed, 0x01=Failed, 0x02=PhaseErr  */
};
```

| Offset | Size | Field              | Description                                      |
| ------ | ---- | ------------------ | ------------------------------------------------ |
| `0x00` | 4    | `dCSWSignature`    | `0x53425355` — identifies packet as CSW          |
| `0x04` | 4    | `dCSWTag`          | Must match the `dCBWTag` of the originating CBW  |
| `0x08` | 4    | `dCSWDataResidue`  | `dCBWDataTransferLength` minus bytes processed   |
| `0x0C` | 1    | `bCSWStatus`       | Final command status (see table below)           |

### CSW Status Codes

| Value  | Name          | Meaning                                                   |
| ------ | ------------- | --------------------------------------------------------- |
| `0x00` | Command Passed| Command executed successfully                             |
| `0x01` | Command Failed| Logical failure — issue REQUEST SENSE for details         |
| `0x02` | Phase Error   | Protocol desync — immediate Reset Recovery required       |

### CSW Validation Rules

Before accepting a CSW, the driver **must** verify:

1. `dCSWSignature == 0x53425355`
2. `dCSWTag` matches the `dCBWTag` of the active pending request
3. CSW is exactly 13 bytes

If any check fails, the CSW is invalid — initiate Reset Recovery.

### The Thirteen Cases: Host/Device Expectation Matrix

The BOT spec defines 13 cases for resolving mismatches between the host's declared expectations
(in the CBW) and the device's actual intent:

| Host Expects    | Device: No Data (Dn)              | Device: Data-In (Di)               | Device: Data-Out (Do)              |
| --------------- | --------------------------------- | ----------------------------------- | ---------------------------------- |
| No Data (Hn)    | **Case 1:** Optimal. Proceed.     | **Case 2:** Phase Error.            | **Case 3:** Phase Error.           |
| Data-In (Hi)    | **Case 4:** STALL, clear, CSW.    | **Case 5** (Hi>Di): Short.          | **Case 8:** Phase Error.           |
|                 |                                   | **Case 6** (Hi=Di): Optimal.        |                                    |
|                 |                                   | **Case 7** (Hi<Di): Phase Error.    |                                    |
| Data-Out (Ho)   | **Case 9:** STALL, clear, CSW.    | **Case 10:** Phase Error.           | **Case 11** (Ho>Do): Short.        |
|                 |                                   |                                     | **Case 12** (Ho=Do): Optimal.      |
|                 |                                   |                                     | **Case 13** (Ho<Do): Phase Error.  |

**Short transfer handling (Cases 4, 5, 9, 11):** Device STALLs the endpoint after processing
its portion. Driver must issue `ClearFeature(ENDPOINT_HALT)` on the stalled endpoint, then
read the CSW normally.

**Phase Error (Cases 2, 3, 7, 8, 10, 13):** Immediate Reset Recovery is mandatory.

---

## SCSI Transparent Command Set

While BOT handles transport, actual storage operations use SCSI Command Descriptor Blocks (CDBs)
embedded in the `CBWCB` field.

> [!CAUTION]
> **Critical endianness trap:** USB structures (CBW, CSW, descriptors) use **little-endian**
> byte order. SCSI CDBs and their response data use **big-endian** (network byte order).
> The driver must implement byte-swapping for all multi-byte fields when crossing between
> USB and SCSI layers.

### Mandatory SCSI Commands

#### INQUIRY (`0x12`) — 6-byte CDB

Identifies the device type and vendor/product strings.

| Byte | Field           | Value / Description                                   |
| ---- | --------------- | ----------------------------------------------------- |
| 0    | Operation Code  | `0x12`                                                |
| 1    | EVPD            | `0x00` (standard inquiry), `0x01` (vital page)        |
| 2    | Page Code       | `0x00` for standard inquiry                           |
| 3–4  | Allocation Len  | Big-endian. Typically `0x0024` (36 bytes minimum)     |
| 5    | Control         | `0x00`                                                |

CBW parameters: `bmCBWFlags = 0x80` (Data-In), `dCBWDataTransferLength = 36`,
`bCBWCBLength = 6`.

Standard INQUIRY response (first 36 bytes):

| Offset | Size | Field                  | Description                               |
| ------ | ---- | ---------------------- | ----------------------------------------- |
| 0      | 1    | Peripheral Device Type | Bits 4:0: `0x00` = direct-access block    |
| 1      | 1    | RMB                    | Bit 7: `1` = removable media              |
| 2      | 1    | Version                | SCSI version compliance                   |
| 3      | 1    | Response Data Format   | Should be `0x02` (SPC-2 and later)        |
| 4      | 1    | Additional Length      | `N - 4`, where N = total response length  |
| 8–15   | 8    | Vendor Identification  | ASCII, space-padded                       |
| 16–31  | 16   | Product Identification | ASCII, space-padded                       |
| 32–35  | 4    | Product Rev Level      | ASCII, space-padded                       |

#### TEST UNIT READY (`0x00`) — 6-byte CDB

Polls the device to check if the medium is inserted and ready.

| Byte | Field           | Value / Description                |
| ---- | --------------- | ---------------------------------- |
| 0    | Operation Code  | `0x00`                             |
| 1–4  | Reserved        | `0x00`                             |
| 5    | Control         | `0x00`                             |

CBW parameters: `bmCBWFlags = 0x00`, `dCBWDataTransferLength = 0`, `bCBWCBLength = 6`.

No data phase. If `bCSWStatus = 0x01`, issue REQUEST SENSE to determine the reason
(e.g., medium not present, device busy spinning up).

#### READ CAPACITY (10) (`0x25`) — 10-byte CDB

Returns the disk geometry: last LBA and block size.

| Byte | Field           | Value / Description                |
| ---- | --------------- | ---------------------------------- |
| 0    | Operation Code  | `0x25`                             |
| 1    | Reserved        | `0x00`                             |
| 2–5  | LBA             | Big-endian. `0x00000000` for max   |
| 6–7  | Reserved        | `0x00`                             |
| 8    | PMI             | `0x00`                             |
| 9    | Control         | `0x00`                             |

CBW parameters: `bmCBWFlags = 0x80` (Data-In), `dCBWDataTransferLength = 8`,
`bCBWCBLength = 10`.

Response (8 bytes, **big-endian**):

| Offset | Size | Field               | Description                                |
| ------ | ---- | ------------------- | ------------------------------------------ |
| 0–3    | 4    | Last Logical Block  | Last accessible LBA (big-endian uint32)    |
| 4–7    | 4    | Block Length         | Sector size in bytes (typically 512)       |

Total capacity = `(Last_LBA + 1) × Block_Length`.

> [!IMPORTANT]
> Common sector sizes: 512, 1024, 2048, 4096 bytes. Always read this value from the device;
> never hardcode 512.

#### READ (10) (`0x28`) — 10-byte CDB

Reads contiguous blocks from the storage medium.

| Byte | Field           | Value / Description                     |
| ---- | --------------- | --------------------------------------- |
| 0    | Operation Code  | `0x28`                                  |
| 1    | Flags           | `0x00` (DPO=0, FUA=0)                  |
| 2–5  | LBA             | Starting block address (**big-endian**) |
| 6    | Group Number    | `0x00`                                  |
| 7–8  | Transfer Length | Block count (**big-endian**)            |
| 9    | Control         | `0x00`                                  |

CBW parameters: `bmCBWFlags = 0x80` (Data-In),
`dCBWDataTransferLength = transfer_length × block_size`, `bCBWCBLength = 10`.

#### WRITE (10) (`0x2A`) — 10-byte CDB

Writes contiguous blocks to the storage medium.

| Byte | Field           | Value / Description                     |
| ---- | --------------- | --------------------------------------- |
| 0    | Operation Code  | `0x2A`                                  |
| 1    | Flags           | `0x00` (DPO=0, FUA=0)                  |
| 2–5  | LBA             | Starting block address (**big-endian**) |
| 6    | Group Number    | `0x00`                                  |
| 7–8  | Transfer Length | Block count (**big-endian**)            |
| 9    | Control         | `0x00`                                  |

CBW parameters: `bmCBWFlags = 0x00` (Data-Out),
`dCBWDataTransferLength = transfer_length × block_size`, `bCBWCBLength = 10`.

#### REQUEST SENSE (`0x03`) — 6-byte CDB

Retrieves error details after a `bCSWStatus = 0x01` (Command Failed).

| Byte | Field            | Value / Description                |
| ---- | ---------------- | ---------------------------------- |
| 0    | Operation Code   | `0x03`                             |
| 1    | DESC             | `0x00` (fixed format sense data)   |
| 2–3  | Reserved         | `0x00`                             |
| 4    | Allocation Length | `0x12` (18 bytes minimum)          |
| 5    | Control          | `0x00`                             |

CBW parameters: `bmCBWFlags = 0x80` (Data-In), `dCBWDataTransferLength = 18`,
`bCBWCBLength = 6`.

Fixed Format Sense Data (18+ bytes, **big-endian** where multi-byte):

| Offset | Size | Field                     | Description                          |
| ------ | ---- | ------------------------- | ------------------------------------ |
| 0      | 1    | Response Code             | `0x70` (current) or `0x71` (deferred)|
| 2      | 1    | Sense Key                 | Bits 3:0: error category             |
| 7      | 1    | Additional Sense Length   | Remaining bytes of sense data        |
| 12     | 1    | ASC                       | Additional Sense Code                |
| 13     | 1    | ASCQ                      | Additional Sense Code Qualifier      |

### Common Sense Key / ASC / ASCQ Combinations

| Sense Key   | ASC    | ASCQ   | Meaning                             |
| ----------- | ------ | ------ | ----------------------------------- |
| `0x02`      | `0x3A` | `0x00` | Medium Not Present                  |
| `0x06`      | `0x28` | `0x00` | Not Ready to Ready Transition       |
| `0x07`      | `0x27` | `0x00` | Write Protected                     |
| `0x03`      | `0x11` | `0x00` | Unrecovered Read Error              |
| `0x03`      | `0x0C` | `0x00` | Write Error                         |
| `0x05`      | `0x20` | `0x00` | Invalid Command Operation Code      |
| `0x05`      | `0x24` | `0x00` | Invalid Field in CDB                |

#### START STOP UNIT (`0x1B`) — 6-byte CDB

Used for safe eject — spins down platters or places flash in quiescent state.

| Byte | Field           | Value / Description                           |
| ---- | --------------- | --------------------------------------------- |
| 0    | Operation Code  | `0x1B`                                        |
| 1    | Immed           | Bit 0: `1` = return immediately               |
| 2–3  | Reserved        | `0x00`                                        |
| 4    | Start/LoEj      | Bit 0: Start, Bit 1: LoEj. `0x02` = eject    |
| 5    | Control         | `0x00`                                        |

---

## Error Handling and Reset Recovery

### Standard Command Failure Flow

When `bCSWStatus = 0x01` (Command Failed):

1. Issue `REQUEST SENSE` to read error details (Sense Key, ASC, ASCQ).
2. Decode the sense data to determine the failure cause.
3. Take appropriate action: retry, remount read-only, or report to user.

### Reset Recovery Sequence

When `bCSWStatus = 0x02` (Phase Error), or CSW validation fails, or endpoints are
persistently stalled, execute this exact three-step sequence:

```
Step 1: Bulk-Only Mass Storage Reset     (Control EP0, bRequest=0xFF)
Step 2: ClearFeature(ENDPOINT_HALT)      (Control EP0, target=Bulk-IN)
Step 3: ClearFeature(ENDPOINT_HALT)      (Control EP0, target=Bulk-OUT)
```

> [!CAUTION]
> These three steps **must** be executed in this exact order. Omitting any step or reordering
> them frequently leaves the device in an unrecoverable zombie state where it ignores all
> further CBWs, requiring physical re-insertion.

#### ClearFeature Setup Packet

| Field            | Value                    | Description                          |
| ---------------- | ------------------------ | ------------------------------------ |
| `bmRequestType`  | `0x02`                   | Standard, Endpoint, Host-to-Device   |
| `bRequest`       | `0x01`                   | CLEAR_FEATURE                        |
| `wValue`         | `0x0000`                 | ENDPOINT_HALT feature selector       |
| `wIndex`         | Endpoint address         | Target endpoint (e.g., `0x81`, `0x02`)|
| `wLength`        | `0x0000`                 | No data                              |

### Retry Policy

| Error Type                | Max Retries | Action After Exhaustion       |
| ------------------------- | ----------- | ----------------------------- |
| Endpoint STALL            | 3           | Reset Recovery                |
| CSW tag mismatch          | 0           | Immediate Reset Recovery      |
| CSW signature invalid     | 0           | Immediate Reset Recovery      |
| Phase Error               | 0           | Immediate Reset Recovery      |
| Command Failed (sense)    | 3           | Report error to VFS           |
| Transfer timeout          | 2           | Reset Recovery                |

---

## Interrupt and DMA Architecture

### Interrupt-Driven vs Polling

A production OS **must** use interrupt-driven I/O. Polling the xHCI Event Ring in a tight
loop wastes 100% of CPU cycles, destroys power efficiency, and prevents multitasking.

### MSI/MSI-X Configuration

Modern xHCI controllers support Message Signaled Interrupts:

1. Scan PCI Capability List for MSI (`Cap ID = 0x05`) or MSI-X (`Cap ID = 0x11`).
2. Configure message address and data registers to route to a CPU interrupt vector.
3. Enable MSI/MSI-X in the PCI capability structure.
4. Set `USBCMD.INTE = 1` and `IMAN.IE = 1` in xHCI registers.

### Hybrid Deferred Execution Model

```
┌──────────┐     ┌──────────────┐     ┌───────────────────┐
│ Hardware  │────→│  Top-Half    │────→│  Bottom-Half      │
│ Interrupt │     │  ISR         │     │  Worker Thread     │
└──────────┘     │  (minimal)   │     │  (BOT state machine│
                 │  - Ack IRQ   │     │   CSW parsing,     │
                 │  - Advance   │     │   request complete, │
                 │    ERDP      │     │   wake caller)     │
                 │  - Queue     │     └───────────────────┘
                 │    bottom-   │
                 │    half      │
                 └──────────────┘
```

The ISR must:
1. Read the Event Ring for completed TRBs.
2. Advance the Event Ring Dequeue Pointer (`ERDP`).
3. Write `1` to `IMAN.IP` to clear the interrupt pending bit.
4. Queue a deferred handler for actual BOT state machine processing.

---

## Hot-Plug and Surprise Removal

### Device Connection

1. xHCI generates Port Status Change Event TRB.
2. Driver reads `PORTSC` to confirm connection.
3. Begin enumeration sequence (reset, enable slot, address, configure).
4. Identify MSC interface, probe LUNs, read capacity.
5. Register block device with VFS, mount filesystems.

### Surprise Removal Teardown Sequence

When a device is physically disconnected mid-operation:

1. **Quarantine** — mark device as offline immediately. Reject all pending I/O with
   `ENODEV` error.
2. **Abort transfers** — walk Transfer Rings, abort all pending TRBs for the disconnected
   device's endpoints.
3. **Halt endpoints** — issue Stop Endpoint Commands via the xHCI Command Ring.
4. **Free slot** — issue Disable Slot Command to release the device slot.
5. **Notify VFS** — trigger unmount of associated filesystems.
6. **Deallocate** — once all file handles are closed (refcount → 0), free DMA buffers,
   endpoint contexts, and device tracking structures.

> [!WARNING]
> If the OS does not quarantine the device immediately upon disconnect, pending DMA
> transfers will fail catastrophically, potentially corrupting kernel memory or causing
> a page fault in the DMA engine.

---

## Security: Defensive Descriptor Parsing

### Threat Model

USB devices cannot be trusted. Rogue devices (e.g., "BadUSB" reprogrammed microcontrollers)
can feed the host:
- Malformed descriptors with impossible length fields
- Oversized payloads designed to overflow kernel buffers
- Unsolicited CSW packets to desynchronize the BOT state machine

### Mandatory Bounds Checks

| Check Point                       | Validation Rule                                            |
| --------------------------------- | ---------------------------------------------------------- |
| Device Descriptor                 | `bLength == 18`, `bDescriptorType == 0x01`                 |
| Config Descriptor header          | `bLength == 9`, `wTotalLength ≤ MAX_CONFIG_SIZE` (4096)    |
| Config Descriptor full read       | Actual bytes received ≤ allocated buffer size              |
| Interface Descriptor              | `bLength == 9`, `bInterfaceClass` within valid range       |
| Endpoint Descriptor               | `bLength == 7`, `bEndpointAddress` has valid direction bit |
| String Descriptors                | Cap all string fetches to kernel's max string buffer       |
| CBW tag echo                      | `dCSWTag == active_request.tag` (exact match)              |
| CSW signature                     | `dCSWSignature == 0x53425355` (exact match)                |
| CSW size                          | Received exactly 13 bytes                                  |
| Data residue                      | `dCSWDataResidue ≤ dCBWDataTransferLength`                 |

### Two-Stage Configuration Descriptor Read

1. First control transfer: request only 9 bytes (Configuration Descriptor header).
2. Parse `wTotalLength` — validate it against a sane maximum (e.g., 4096 bytes).
3. Allocate buffer of `wTotalLength` bytes.
4. Second control transfer: request full `wTotalLength` bytes.
5. Verify actual bytes received ≤ allocated buffer size.

> [!CAUTION]
> Never use the device-reported `wTotalLength` to allocate unbounded memory. A malicious
> device could report `0xFFFF` (65535 bytes) to exhaust kernel heap. Enforce a hard cap.

### IOMMU Integration

If the platform supports an IOMMU (Intel VT-d, AMD-Vi):
- Configure DMA remapping to restrict the xHCI controller's DMA access to only the
  specific physical pages allocated for TRB rings and data buffers.
- Any DMA access outside these regions is blocked by hardware, preventing a compromised
  controller from reading/writing arbitrary kernel memory.

---

## DMA Memory Requirements

### Allocation Summary

| Structure           | Alignment    | Contiguity           | Boundary Constraint  |
| ------------------- | ------------ | -------------------- | -------------------- |
| DCBAA               | 64 bytes     | Physically contiguous| Must not cross 4 KiB |
| Device Context      | 64 bytes     | Physically contiguous| Must not cross page   |
| Command Ring        | 64 bytes     | Per-segment contiguous| 64 KiB boundary     |
| Event Ring          | 64 bytes     | Per-segment contiguous| 64 KiB boundary     |
| Transfer Ring       | 16 bytes     | Per-segment contiguous| 64 KiB boundary     |
| ERST                | 64 bytes     | Physically contiguous| Must not cross 64 KiB|
| Scratchpad Buffers  | Page-aligned | Per-buffer contiguous| N/A                  |
| Data Buffers (I/O)  | No alignment | Physically contiguous| Each TRB ≤ 64 KiB   |

### Scatter-Gather for Large Transfers

When a VFS request exceeds 64 KiB or the buffer crosses a 64 KiB physical boundary:

1. Split the request into fragments ≤ 64 KiB, each within a single 64 KiB region.
2. Create one Normal TRB per fragment, setting the Chain bit on all but the last.
3. Set IOC (Interrupt On Completion) on the final TRB only.
4. Ensure total fragment count does not exceed Transfer Ring capacity.

---

## QEMU Testing Configuration

### Basic USB Mass Storage with xHCI

```bash
qemu-system-x86_64 \
    -drive file=build/system-disk.img,format=raw,if=none,id=boot \
    -device virtio-blk-pci,drive=boot \
    -device qemu-xhci,id=xhci \
    -drive file=test-usb.img,format=raw,if=none,id=usbdisk \
    -device usb-storage,bus=xhci.0,drive=usbdisk \
    -serial stdio
```

### Create a Test USB Disk Image

```bash
# Create a 64 MiB FAT32 USB disk image
dd if=/dev/zero of=test-usb.img bs=1M count=64
mkfs.vfat -F 32 test-usb.img
```

### Multiple USB Devices (Multi-LUN Testing)

```bash
qemu-system-x86_64 \
    -drive file=build/system-disk.img,format=raw,if=none,id=boot \
    -device virtio-blk-pci,drive=boot \
    -device qemu-xhci,id=xhci \
    -drive file=usb1.img,format=raw,if=none,id=usb1 \
    -device usb-storage,bus=xhci.0,drive=usb1 \
    -drive file=usb2.img,format=raw,if=none,id=usb2 \
    -device usb-storage,bus=xhci.0,drive=usb2 \
    -serial stdio
```

### Hot-Plug Testing via QEMU Monitor

```bash
# Add -monitor telnet:127.0.0.1:4444,server,nowait to QEMU args
# Then connect: telnet 127.0.0.1 4444

# Hot-plug a USB device:
drive_add 0 file=hotplug.img,format=raw,if=none,id=hotdisk
device_add usb-storage,bus=xhci.0,drive=hotdisk,id=hotusb

# Hot-unplug:
device_del hotusb
```

### Alternative: nec-usb-xhci Controller

```bash
# Some QEMU versions use nec-usb-xhci instead of qemu-xhci
-device nec-usb-xhci,id=xhci
```

---

## Implementation Priorities for Impossible OS

| Priority | Component                              | Description                              |
| -------- | -------------------------------------- | ---------------------------------------- |
| 🔴 P0    | xHCI PCI discovery & MMIO mapping      | Find controller, map registers           |
| 🔴 P0    | xHCI controller init & ring setup      | Reset, DCBAA, Command/Event rings        |
| 🔴 P0    | USB device enumeration                 | Port detect, slot enable, address device |
| 🔴 P0    | Descriptor parsing & MSC identification| Parse device/config/interface/endpoint   |
| 🔴 P0    | BOT protocol: CBW/CSW transport        | Send CBW, read CSW, handle data phase    |
| 🔴 P0    | SCSI: INQUIRY + TEST UNIT READY        | Device identification and readiness      |
| 🔴 P0    | SCSI: READ CAPACITY + READ(10)         | Read disk geometry and sectors           |
| 🟠 P1    | SCSI: WRITE(10)                        | Write support for USB storage            |
| 🟠 P1    | Reset Recovery (3-step)                | Handle phase errors and stalled EPs      |
| 🟠 P1    | REQUEST SENSE error decoding           | Detailed error reporting                 |
| 🟠 P1    | MSI/MSI-X interrupt handling           | Replace polling with interrupt-driven    |
| 🟡 P2    | Hot-plug / surprise removal            | Dynamic connect + safe disconnect        |
| 🟡 P2    | Multi-LUN support                      | Get Max LUN, per-LUN block devices       |
| 🟡 P2    | VFS block device registration          | Expose as drive letter (e.g., `D:\`)     |
| 🟡 P2    | Scatter-gather for large I/O           | Split buffers across TRBs at 64 KiB     |
| 🟢 P3    | Defensive descriptor validation        | Bounds checking, two-stage config read   |
| 🟢 P3    | Safe eject (START STOP UNIT)           | User-initiated unmount + media eject     |
| 🟢 P3    | USB hub support                        | Traverse hub topology for nested devices |
| 🔵 P4    | IOMMU DMA isolation                   | Restrict xHCI DMA to allocated pages     |
| 🔵 P4    | USB 3.0 streams                        | SuperSpeed bulk streams for throughput   |

### Current Codebase State

No USB or xHCI code exists in the Impossible OS codebase. The implementation will build on:
- **PCI subsystem** (`src/kernel/drivers/pci.c`) — for xHCI controller discovery
- **PMM** (`src/kernel/mm/pmm.c`) — for DMA-safe physically contiguous allocation
- **VMM** (`src/kernel/mm/vmm.c`) — for uncacheable MMIO page mapping
- **VFS** (`src/kernel/fs/vfs.c`) — for block device registration
- **Interrupt subsystem** — for MSI/MSI-X vector mapping
