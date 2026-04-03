# Architectural Specification and Implementation Guide for Master Boot Record (MBR) Partitioning

---

## 1. Introduction

The Master Boot Record (MBR) represents one of the most foundational and enduring data structures in the history of computer science and operating system development. Publicly introduced in 1983 alongside IBM PC DOS 2.0, the MBR architecture was explicitly designed to manage the division of a physical storage disk into independent, isolated logical volumes, while simultaneously providing the initial executable instruction set required to bootstrap an operating system into memory. For decades, it stood as the absolute standard for IBM PC-compatible systems, dictating how storage media was addressed, mapped, and booted.

Although modern computing environments and enterprise servers have largely transitioned to the Unified Extensible Firmware Interface (UEFI) and the GUID Partition Table (GPT) standard to accommodate storage drives exceeding the 2 TiB addressable limit inherent to MBR geometry, legacy MBR support remains a mandatory, non-negotiable component of modern operating system development. Even within purely UEFI-based software ecosystems, backward compatibility protocols, hybrid booting environments, and the deployment of Protective MBR structures necessitate a rigorous, bit-accurate understanding of the 512-byte MBR sector, its partition table structures, and its cascading logical partition chains.

For software engineers, kernel developers, and systems architects implementing a custom operating system, writing a low-level storage stack, or engineering a stage-one bootloader, interpreting the MBR requires absolute precision. A single misinterpreted bit within the legacy Cylinder-Head-Sector (CHS) tuple, an incorrect byte alignment offset, or a miscalculated logical block pointer within an Extended Boot Record (EBR) linked list can result in catastrophic data corruption, unmountable filesystems, or completely unbootable media.

This comprehensive technical specification delineates:

1. The exact anatomical layout of the Master Boot Record
2. The bitwise logic governing its myriad metadata fields
3. The mathematical translation required to convert between legacy CHS addressing and modern Logical Block Addressing (LBA)
4. The programmatic algorithms required to traverse extended partition chains safely
5. The implementation directives necessary for robust OS integration

---

## 2. Anatomical Layout of the 512-Byte Master Boot Record Sector

The Master Boot Record is exclusively and permanently located at the absolute physical and logical beginning of a partitioned storage device: **Cylinder 0, Head 0, Sector 1** in legacy geometry, or **Logical Block Address (LBA) 0** in modern addressing. This initial sector is strictly **512 bytes** in length.

It is structurally divided into three primary functional zones that operate in tandem to initialize the system hardware and identify the storage layout:

1. The **Master Bootstrap Loader** (containing executable machine code)
2. The **Master Partition Table** (containing static geometric metadata)
3. The **Boot Record Signature** (serving as a hardware validation marker)

### 2.1 Overall Sector Layout

| Offset (Hex) | Offset (Dec) | Length    | Component                              |
| ------------ | ------------ | --------- | -------------------------------------- |
| `0x000`      | 0            | 440 bytes | Executable Bootstrap Code              |
| `0x1B8`      | 440          | 4 bytes   | 32-bit Unique Disk Signature           |
| `0x1BC`      | 444          | 2 bytes   | Reserved (Copy Protection / AAP)       |
| `0x1BE`      | 446          | 16 bytes  | Partition Entry 1                      |
| `0x1CE`      | 462          | 16 bytes  | Partition Entry 2                      |
| `0x1DE`      | 478          | 16 bytes  | Partition Entry 3                      |
| `0x1EE`      | 494          | 16 bytes  | Partition Entry 4                      |
| `0x1FE`      | 510          | 2 bytes   | Boot Record Signature (`0xAA55`)       |

### 2.2 BIOS Boot Sequence

When a computer system powers on, the system firmware (BIOS) performs its Power-On Self-Test (POST) and then searches the hardware devices listed in the CMOS boot sequence. When it identifies a potentially bootable hard disk, the BIOS:

1. Physically reads the single 512-byte sector (LBA 0) from the storage medium
2. Loads it directly into physical RAM at address **`0x7C00`**
3. Populates the **DL** CPU register with the physical drive number
4. Transfers execution control via a jump instruction to `0x0000:0x7C00`

From that point forward, the code embedded within the MBR dictates the fate of the machine.

---

## 3. The Executable Bootstrap Code Area (Offsets `0x000` to `0x1B7`)

The first **440 bytes** of the MBR (offsets `0x000` to `0x1B7`) represent the Master Bootstrap Loader code area. Because the entire 512-byte sector is loaded at address `0x7C00`, the instruction pointer begins execution here.

### 3.1 Bootstrap Execution Flow

A standard MBR bootstrap program performs the following operations:

1. **Self-relocation:** Copies itself from `0x7C00` to a lower memory address (typically `0x0600`) using a memory copy and a far jump. This frees the `0x7C00` address for loading the Volume Boot Record (VBR).

2. **Partition table scan:** Iterates through the 64-byte partition table, examining each of the four primary partition entries, searching for a **Boot Indicator** byte flagged as active (`0x80`).

3. **VBR loading:** Upon identifying the active partition, extracts its Starting LBA (or CHS tuple), uses BIOS Interrupt `13h` to read the first sector of that partition (the VBR) into memory at `0x7C00`.

4. **Control transfer:** Jumps to `0x7C00`, ending the MBR's operational lifespan for that boot cycle.

> [!IMPORTANT]
> Bootloaders for custom operating systems must be meticulously optimized, often written in raw assembly language, to fit the entire operational logic within the strict **440-byte** constraint.

---

## 4. The 32-bit Unique Disk Signature (Offsets `0x1B8` to `0x1BB`)

Historically, the bootstrap code extended uninterrupted to offset 445 (a full 446 bytes of executable space). However, this changed significantly with the introduction of **Windows NT**, which co-opted the 4 bytes at offsets `0x1B8` through `0x1BB` (440 to 443) to store a **32-bit Unique Disk Signature**.

This 32-bit identifier acts as a persistent serial number or pseudo-UUID for the physical storage medium, allowing the operating system kernel to uniquely identify individual disks regardless of which physical SATA or SCSI port they are plugged into.

> [!CAUTION]
> The stage-one bootloader payload must **strictly never exceed 440 bytes**. Overwriting this 32-bit field with executable bootstrap code will corrupt the signature.
>
> If this signature changes, is overwritten with random executable bytes, or is zeroed out in a system that relies on it, the operating system's storage stack will fail to correlate the physical disk with its internal volume mount registry. In the Windows NT family, this results in catastrophic boot failures -- fatal stop errors (Blue Screen of Death).

Since Windows Vista, non-Windows environments (including the Linux kernel and the GRUB bootloader) have also adopted and supported this signature to ensure stable block device mapping across reboots. Consequently, modern generic bootloaders (Syslinux, GRUB) strictly constrain their compiled stage-one binaries to 440 bytes to preserve this crucial metadata.

---

## 5. The Copy Protection and AAP Marker (Offsets `0x1BC` to `0x1BD`)

The two bytes at offsets `0x1BC` to `0x1BD` (444 to 445) function as an intermediary buffer zone before the partition table begins.

| Value    | Meaning                                                                      |
| -------- | ---------------------------------------------------------------------------- |
| `0x0000` | Standard -- unused (the vast majority of MBR implementations)                 |
| `0x5A5A` | Obsolete commercial copy-protection mechanism active                         |
| Other    | Advanced Active Partition (AAP) MBR layout (PTS-DOS 7, DR-DOS 7.07)         |

> [!NOTE]
> The AAP layout directly conflicts with the modern 32-bit Disk Signature, making the two architectures mutually exclusive. For modern custom OS development, treat these two bytes as reserved and enforce `0x0000` when formatting a new disk.

---

## 6. The Master Partition Table (Offsets `0x1BE` to `0x1FD`)

Beginning precisely at offset **446 (`0x1BE`)**, the Master Boot Record allocates a strictly contiguous **64-byte** region for the Master Partition Table. This table is symmetrically subdivided into exactly **four distinct 16-byte** data structures. Each defines the geometric location, total size, status, and file system type of a single primary partition or extended partition container.

The hardcoded limitation of exactly four entries is the fundamental reason that standard MBR-formatted disks can natively support a **maximum of only four primary partitions**.

### 6.1 Partition Entry Offsets

| Entry             | Offset (Hex) | Offset (Dec) |
| ----------------- | ------------ | ------------ |
| Partition Entry 1 | `0x1BE`      | 446          |
| Partition Entry 2 | `0x1CE`      | 462          |
| Partition Entry 3 | `0x1DE`      | 478          |
| Partition Entry 4 | `0x1EE`      | 494          |

> [!NOTE]
> The sequential naming (Partition 1 through 4) is purely semantic. The firmware does **not** require partitions to be ordered geometrically. Partition Entry 4 could define a volume at the beginning of the disk while Entry 1 defines one at the end. A robust OS parser must sort partitions by Starting LBA if sequential physical ordering is required for display.

---

## 7. The Boot Record Signature (Offsets `0x1FE` to `0x1FF`)

The final two bytes at offsets **510 (`0x1FE`)** and **511 (`0x1FF`)** constitute the Boot Record Signature (the "magic number"). This signature must strictly evaluate to **`0xAA55`**.

Because x86 architecture is **little-endian**, this 16-bit word is stored on disk as:

| Offset  | Byte Value |
| ------- | ---------- |
| `0x1FE` | `0x55`     |
| `0x1FF` | `0xAA`     |

During initialization, the BIOS explicitly checks for this sequence. If absent, corrupted by a single bit, or malformed, the BIOS aborts the boot process for that device and proceeds to the next device in the CMOS boot sequence.

> [!IMPORTANT]
> Any custom OS installation routine must explicitly write `0x55` to offset 510 and `0xAA` to offset 511 to finalize MBR sector creation.

---

## 8. Structural Specification of the 16-Byte Partition Entry

The core metadata defining a logical volume is encapsulated within a compact **16-byte** data structure.

### 8.1 Partition Entry Layout

| Offset (Hex) | Offset (Dec) | Length  | Field                      | Allowed Values                                                              |
| ------------ | ------------ | ------- | -------------------------- | --------------------------------------------------------------------------- |
| `0x00`       | 0            | 1 byte  | Boot Indicator Flag        | `0x80` = Active/Bootable. `0x00` = Inactive. All others invalid             |
| `0x01`       | 1            | 3 bytes | Starting CHS Address       | Encoded physical CHS of partition start (legacy BIOS only)                  |
| `0x04`       | 4            | 1 byte  | Partition Type (System ID) | Hex code indicating filesystem format or partition role                     |
| `0x05`       | 5            | 3 bytes | Ending CHS Address         | Encoded physical CHS of partition end                                       |
| `0x08`       | 8            | 4 bytes | Starting LBA               | Absolute linear sector distance from disk start to partition start          |
| `0x0C`       | 12           | 4 bytes | Total Sectors              | Total partition size in sectors (32-bit unsigned)                           |

### 8.2 The Boot Indicator Flag (Offset `0x00`)

The first byte dictates the active status. The MBR bootstrap code iterates through entries searching for `0x80`:

- **`0x80`**: Bit 7 set -- partition is active/bootable
- **`0x00`**: Inactive -- bypassed during boot
- **Any other value**: Partition table is considered corrupted by standard-compliant loaders

> [!WARNING]
> If multiple entries possess the `0x80` flag simultaneously, standard-compliant bootstrap loaders will consider the partition table **dangerously corrupted** and halt the boot sequence. An unused entry must have all 16 bytes zeroed.

### 8.3 The 32-bit LBA Fields (Offsets `0x08` and `0x0C`)

Modern operating systems rely **exclusively** on these two 32-bit LBA fields. The legacy CHS fields are largely ignored by any OS produced in the last two decades.

**Starting LBA** (offset `0x08`): The sector index where the partition begins. Constrained to a 32-bit unsigned integer:

```text
Max LBA = 2^32 - 1 = 4,294,967,295 sectors
Max Capacity = 4,294,967,295 × 512 bytes = 2,199,023,255,040 bytes ≈ 2 TiB
```

**Total Sectors** (offset `0x0C`): Similarly constrained, limiting any single partition to 2 TiB.

> [!WARNING]
> **Little-Endian Byte Ordering:** A developer reading these 4-byte fields from a raw disk image must perform byte reversal. If a hex editor shows `3F 00 00 00` at offset `0x08`, the actual LBA is `0x0000003F` = **63** in decimal. Failure to handle little-endian byte swapping is one of the most common causes of data corruption in custom OS storage drivers.

---

## 9. Cylinder-Head-Sector (CHS) Addressing and Bit-Packing Geometry

Despite their practical obsolescence, the Starting CHS (offset `0x01`) and Ending CHS (offset `0x05`) fields **must be properly formulated** by partitioning tools for backward compatibility.

### 9.1 The CHS Coordinate Model

The CHS system is a three-dimensional coordinate model mapping to physical drive geometry:

- **Cylinder** (10 bits): Vertical intersection through the platter stack -- max 1024
- **Head** (8 bits): Specific platter surface -- max 256
- **Sector** (6 bits): Angular arc block along a track -- max 63 (1-based numbering)

### 9.2 Bit-Packed 3-Byte Format

The 24-bit CHS tuple is compressed into 3 bytes in a **highly irregular, fractured** format:

| Byte       | Content                                            | Bits                                     |
| ---------- | -------------------------------------------------- | ---------------------------------------- |
| **Byte 1** | Head value                                         | Full 8 bits (0–255)                      |
| **Byte 2** | Sector (bits 0–5) + Cylinder high (bits 6–7)       | 6-bit sector + 2 MSBs of cylinder        |
| **Byte 3** | Cylinder low                                       | Lower 8 bits of cylinder                 |

### 9.3 Extraction Algorithm

```c
/* Extract CHS from 3 raw bytes */
uint8_t  head     = byte1;
uint8_t  sector   = byte2 & 0x3F;           /* Bits 0-5 */
uint16_t cylinder = ((byte2 & 0xC0) << 2)   /* Bits 6-7 → bits 8-9 */
                  | byte3;                   /* Bits 0-7 */
```

### 9.4 Encoding Algorithm

```c
/* Encode CHS into 3 bytes */
byte1 = head;
byte2 = (sector & 0x3F)                     /* Lower 6 bits: sector */
      | ((cylinder >> 2) & 0xC0);           /* Upper 2 bits: cylinder MSBs */
byte3 = cylinder & 0xFF;                    /* Lower 8 bits: cylinder LSBs */
```

### 9.5 Practical Example

Raw CHS bytes: `FE 7F 04`

- **Byte 1** = `0xFE` → Head **254**
- **Byte 2** = `0x7F` (`01111111`). Mask `0x3F` → `00111111` → Sector **63**
- **Byte 2** high bits: `01` (isolated via `0xC0`). **Byte 3** = `0x04` (`00000100`)
- Cylinder = `(01 << 8) | 00000100` = `0100000100` = **260**
- Decoded CHS: **(Cylinder 260, Head 254, Sector 63)**

### 9.6 CHS-to-LBA Conversion Formula

```text
LBA = (Cylinder × HeadsPerCylinder + Head) × SectorsPerTrack + (Sector - 1)
```

### 9.7 LBA-to-CHS Conversion Formula

```text
Cylinder = LBA / (HeadsPerCylinder × SectorsPerTrack)
Head     = (LBA / SectorsPerTrack) % HeadsPerCylinder
Sector   = (LBA % SectorsPerTrack) + 1
```

---

## 10. The 8.4 Gigabyte CHS Barrier

The rigid CHS fields max out at exactly **1024 cylinders × 256 heads × 63 sectors**:

```text
1024 × 256 × 63 × 512 = 8,422,686,720 bytes ≈ 8.4 GB
```

### 10.1 Overflow Dummy Tuple

When a partition begins or ends beyond the 8.4 GB boundary, the actual disk geometry cannot be represented within 24 bits. The universally accepted solution is to write the **dummy CHS tuple**: **`FE FF FF`**.

| Byte                                   | Value  | Decoded |
| -------------------------------------- | ------ | ------- |
| `0xFE`                                 | Head   | 254     |
| `0xFF` (lower 6 bits)                  | Sector | 63      |
| `0xFF` combined with upper 2 bits      | Cylinder | 1023  |

This encodes the maximum representable CHS address: **(Cylinder 1023, Head 254, Sector 63)**.

> [!NOTE]
> Head value 254 (`0xFE`) is used instead of 255 (`0xFF`) because the INT 13h BIOS interface historically restricted heads to 0–254 (255 heads total). Some legacy BIOSes treated Head 255 as invalid. The standard dummy tuple is strictly `FE FF FF`; a compliant parser encountering this value must **ignore CHS geometry** and rely exclusively on the 32-bit LBA fields.

---

## 11. Logical Block Addressing (LBA) and Sector Alignment

### 11.1 The Historical 63-Sector Alignment Rule

In legacy DOS and early Windows environments, partitions were aligned to physical drive geometry boundaries. Standard drives featured 63 sectors per track, so the first primary partition was offset by exactly 63 sectors:

```text
Starting LBA = 63
Partition data begins at byte offset: 63 × 512 = 32,256
```

Hex dump: `3F 00 00 00` at offset `0x1C6` (little-endian).

### 11.2 Modern 1-MiB Boundary Alignment

With Advanced Format drives (4096-byte internal sectors) and SSDs (128–512 KiB erase blocks), the legacy 63-sector alignment creates **severe performance degradation**:

- **32,256 bytes is NOT divisible by 4096** -- every cluster operation straddles two physical sector boundaries
- Forces read-modify-write loops, reducing I/O throughput and increasing SSD wear

Modern alignment rule: **Start at LBA 2048**:

```text
1,048,576 bytes (1 MiB) / 512 bytes = 2048 sectors
```

> [!CAUTION]
> Mixing legacy 63-sector alignment tools (e.g., Windows XP Disk Management) with modern 1-MiB aligned drives can lead to silent, unprompted deletion of extended partitions due to conflicting internal alignment assumptions.

---

## 12. Extended Boot Records (EBR) and Linked Lists

The most severe architectural constraint of the MBR is the 64-byte partition table, hardcoding a maximum of **four primary partitions**. To circumvent this, IBM and Microsoft introduced the **Extended Partition** framework.

### 12.1 Creating an Extended Partition

An Extended Partition is created by designating one of the four primary MBR entries with a specific Partition Type code:

| Type Code | Description                          |
| --------- | ------------------------------------ |
| `0x05`    | Extended Partition (CHS addressing)  |
| `0x0F`    | Extended Partition (LBA addressing)  |

This entry acts as an architectural envelope -- it does **not** hold a standard filesystem. The space it defines is subdivided into **Logical Partitions** using Extended Boot Records (EBRs).

### 12.2 EBR Structure

Each EBR is exactly **512 bytes** with the same layout as the MBR, but:

- **Bootstrap code area** (first 446 bytes): Generally unused, zeroed
- **Only the first two partition entries are used**: entries 3 and 4 must be zeroed
- **Must terminate with `0xAA55`** signature at offset 510

| Entry       | Offset  | Purpose                                                         |
| ----------- | ------- | --------------------------------------------------------------- |
| **Entry 1** | `0x1BE` | Describes the logical partition associated with this EBR        |
| **Entry 2** | `0x1CE` | Pointer to the **next** EBR in the chain (linked list)          |
| **Entry 3** | `0x1DE` | Unused -- must be zeroed                                         |
| **Entry 4** | `0x1EE` | Unused -- must be zeroed                                         |

### 12.3 Linked List Traversal Example

```text
MBR → Entry 4 = Extended Partition (LBA Start)
  └─→ EBR 1 (at Extended Start):
       Entry 1 → Logical Drive 5
       Entry 2 → pointer to EBR 2
       └─→ EBR 2:
            Entry 1 → Logical Drive 6
            Entry 2 → pointer to EBR 3
            └─→ EBR 3:
                 Entry 1 → Logical Drive 7
                 Entry 2 → all zeros (END OF CHAIN)
```

### 12.4 Complex Relative LBA Addressing in EBR Chains

> [!CAUTION]
> The addressing rules for EBR entries are the **most technically demanding** aspect of MBR parsing. The mathematical anchor point shifts depending on which entry is being read.

#### Rule 1: Resolving the Local Logical Volume (EBR Entry 1)

The Starting LBA in Entry 1 is **relative to the current EBR sector**:

```text
Absolute_Volume_LBA = Absolute_LBA_of_Current_EBR + Entry1.Starting_LBA
```

This offset is typically 63 (legacy) or 2048 (modern), creating the gap between the EBR metadata and the filesystem data.

#### Rule 2: Resolving the Next EBR Pointer (EBR Entry 2)

The Starting LBA in Entry 2 is **relative to the FIRST EBR** in the entire extended partition -- never relative to the current EBR, never an absolute address:

```text
Absolute_LBA_of_Next_EBR = Absolute_LBA_of_FIRST_EBR + Entry2.Starting_LBA
```

The Total Sectors field in Entry 2 defines the total encompassing size of the next logical partition, **including** the next EBR sector and its alignment gap.

> [!IMPORTANT]
> The OS parser must **cache the absolute LBA of the first EBR** in memory permanently to successfully walk the entire linked list. Failure to apply these two different relative anchor points will result in reading out-of-bounds sectors, crashing the kernel during boot.

---

## 13. Comprehensive Reference of Partition Types (System IDs)

The single byte at offset `0x04` of every partition entry is the **Partition Type** (System ID), indicating the filesystem format.

### 13.1 DOS and Windows FAT Family

| Hex Code | Description              | Notes                                                           |
| -------- | ------------------------ | --------------------------------------------------------------- |
| `0x00`   | Empty / Unallocated      | All subsequent fields must be zero                              |
| `0x01`   | FAT12 (Primary)          | Legacy 12-bit FAT, restricted to first 32 MB                   |
| `0x04`   | FAT16 (up to 32 MB)      | Legacy 16-bit FAT, MS-DOS 3.0                                  |
| `0x06`   | FAT16B (over 32 MB)      | Large File System, MS-DOS 3.31+                                 |
| `0x0B`   | FAT32 (CHS)              | Windows 95 OSR2, limited by CHS bounds                         |
| `0x0C`   | FAT32 (LBA)              | FAT32 with INT 13h LBA extensions, bypasses 8.4 GB barrier     |
| `0x0E`   | FAT16 (LBA)              | FAT16 with LBA extensions                                      |

### 13.2 Advanced File Systems and Extended Containers

| Hex Code | Description                        | Notes                                                                          |
| -------- | ---------------------------------- | ------------------------------------------------------------------------------ |
| `0x05`   | Extended Partition (CHS)           | Envelope for EBR logical drives, legacy CHS addressing                         |
| `0x0F`   | Extended Partition (LBA)           | Envelope for EBR logical drives, LBA addressing. Replaces `0x05` beyond 8.4 GB |
| `0x07`   | NTFS / HPFS / exFAT                | Windows NT File System, OS/2 HPFS, or exFAT                                   |
| `0x27`   | Windows Recovery Environment       | Hidden utility/diagnostic partition -- OS should not auto-mount                 |

### 13.3 Unix, Linux, and Alternative Operating Systems

| Hex Code | Description            | Notes                                                                 |
| -------- | ---------------------- | --------------------------------------------------------------------- |
| `0x82`   | Linux Swap / Solaris   | Virtual memory swap space or Solaris system volumes                   |
| `0x83`   | Linux Native           | Standard identifier for ext2, ext3, ext4, JFS, ReiserFS, XFS         |
| `0xEB`   | BeOS File System       | Dedicated for BeOS BFS                                               |
| `0xFB`   | VMware VMFS            | VMware raw disk virtualization layer                                  |

### 13.4 The GPT Protective MBR (Type `0xEE`)

When a parser encounters Partition Type **`0xEE`** in the first entry of Sector 0, it indicates:

1. The entire MBR partition table is a **"fake" Protective MBR**
2. True partition data resides in the **GPT headers starting at LBA 1**
3. The Protective MBR creates a single massive partition spanning the disk (capped at 2 TiB), assigned `0xEE`

This prevents legacy tools (like MS-DOS FDISK) from treating a GPT disk as unformatted free space and accidentally overwriting GPT metadata.

> [!IMPORTANT]
> A custom OS storage driver must **always** check the first partition type. If `0xEE` is detected, abort MBR parsing and redirect to the GPT parser.

---

## 14. Implementation Directives

### Phase 1: Controlled Payload Execution

Hard-limit the stage-one assembly bootloader to exactly **440 bytes**. Expanding into offsets 440–445 overwrites the Disk Signature, breaking:

- Dual-boot Windows NT configurations
- GRUB persistence
- Volume tracking across hardware restarts

### Phase 2: LBA Exclusivity

- Device addressing must rely **exclusively** on 32-bit LBA
- CHS is a deprecated artifact -- populate CHS fields for backward compatibility only
- When a partition exceeds CHS bounds, write the dummy tuple `FE FF FF`
- Storage drivers should **never** use CHS arithmetic for actual I/O

### Phase 3: Modern Alignment Enforcement

- Default partition alignment to **LBA 2048** (1 MiB boundary)
- Optimizes 4K-sector Advanced Format drives and SSD erase blocks
- Vastly improves I/O throughput compared to legacy 63-sector alignment

### Phase 4: Caching Linked List Anchors

- Treat Extended Partitions (`0x05` / `0x0F`) as opaque geometric envelopes
- **Cache** the absolute LBA of the first EBR permanently in memory
- EBR Entry 1 LBA: relative to current EBR → add to current EBR's absolute LBA
- EBR Entry 2 LBA: relative to first EBR → add to cached first EBR absolute LBA
- Failure to apply these two distinct anchor points crashes the kernel

---

## 15. Summary of Critical Constants

| Constant                | Value         | Description                                        |
| ----------------------- | ------------- | -------------------------------------------------- |
| MBR sector location     | LBA 0         | Always the first sector on disk                    |
| MBR sector size         | 512 bytes     | Fixed, non-negotiable                              |
| Bootstrap code limit    | 440 bytes     | Must not exceed to preserve Disk Signature         |
| Partition table offset  | 446 (`0x1BE`) | Start of the 64-byte partition table               |
| Partition entry size    | 16 bytes      | Each of 4 entries                                  |
| Boot signature          | `0xAA55`      | Little-endian: `0x55` at 510, `0xAA` at 511       |
| Active boot flag        | `0x80`        | Marks partition as bootable                        |
| Max MBR addressable     | 2 TiB         | 2³² sectors × 512 bytes                           |
| CHS max capacity        | ~8.4 GB       | 1024 × 256 × 63 × 512 bytes                       |
| CHS overflow tuple      | `FE FF FF`    | Signals "ignore CHS, use LBA"                     |
| Modern start alignment  | LBA 2048      | 1 MiB boundary for 4K/SSD optimization            |
| Extended CHS type       | `0x05`        | Extended partition, legacy CHS                     |
| Extended LBA type       | `0x0F`        | Extended partition, modern LBA                     |
| GPT Protective type     | `0xEE`        | Signals GPT -- abort MBR parsing                   |
| EBR used entries        | 2 of 4        | Entry 1 = logical volume, Entry 2 = next EBR ptr  |
