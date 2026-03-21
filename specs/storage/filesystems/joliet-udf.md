# Joliet and Universal Disk Format (UDF) — Technical Specification for OS Implementation

## Overview and Architectural Context

Optical media file systems extend beyond the baseline ISO 9660 (ECMA-119) standard through two
complementary technologies: the **Joliet** extension (Microsoft, 1995) and the **Universal Disk
Format** (UDF, OSTA). Joliet adds Unicode filename support to ISO 9660 while preserving backward
compatibility. UDF is a complete architectural replacement based on ECMA-167 / ISO/IEC 13346,
designed for DVD, Blu-Ray, and packet-written CD-RW media.

This specification covers read-only implementation of both file systems within a bare-metal
operating system. The scope spans SCSI/ATAPI transport abstraction, volume descriptor parsing,
directory traversal, text encoding translation, VFS integration, caching, and security hardening.

> [!IMPORTANT]
> Both Joliet and UDF sit above the same ATAPI/SCSI block device layer documented in
> [ATAPI/SCSI/MMC](../controllers/atapi-scsi-mmc.md). The transport layer is shared with
> ISO 9660 and is not duplicated here. This document focuses on the file system structures
> above the block device abstraction.

### Version History and Compatibility

#### Joliet

| Version   | Date      | Key Additions                                                  |
| --------- | --------- | -------------------------------------------------------------- |
| 1.0       | 1995      | Initial release with Windows 95; UCS-2 filenames via ISO 9660  |
|           |           | Supplementary Volume Descriptor                                |

#### UDF (OSTA Universal Disk Format)

| Revision | Date       | Key Additions                                                     |
| -------- | ---------- | ----------------------------------------------------------------- |
| 1.02     | 1996-08-30 | Base DVD-Video format; Plain build only                           |
| 1.50     | 1997-02-04 | VAT for CD-R/DVD-R; Sparing Tables for CD-RW/DVD-RW              |
| 2.00     | 1998-04-03 | Stream files, ACLs, extended VAT, real-time files                 |
| 2.01     | 2000-03-15 | Bugfix release for 2.00; resolved ambiguities                     |
| 2.50     | 2003-04-30 | Metadata Partition for crash recovery; used by HD-DVD, some BD    |
| 2.60     | 2005-03-01 | Pseudo OverWrite (POW) for BD-R write-once; mandatory metadata    |

> [!NOTE]
> DVD-Video discs are exclusively UDF 1.02. Blu-Ray data discs typically use UDF 2.50 or 2.60.
> A robust driver must support revisions 1.02 through 2.60 for full optical media compatibility.

---

## Hardware Transport and Block Addressing

Optical media interfaces through ATAPI/SCSI transport as documented in the
[ATAPI/SCSI/MMC specification](../controllers/atapi-scsi-mmc.md). The following SCSI commands
are required for Joliet and UDF file system operation:

| Hex    | Command            | Purpose for FS Driver                                           |
| ------ | ------------------ | --------------------------------------------------------------- |
| `0x03` | REQUEST SENSE      | Fetch sense data on CHECK CONDITION (media error, no disc)      |
| `0x1B` | START/STOP UNIT    | Spin up drive motor, eject/load tray                            |
| `0x25` | READ CAPACITY (10) | Determine last LBA and block size (needed for AVDP backup)      |
| `0x28` | READ (10)          | Standard read, 32-bit LBA, mandatory for all drives             |
| `0x88` | READ (16)          | 64-bit LBA for multi-layer BD exceeding 2 TiB                  |

### Sector Size

All data CDs (CD-ROM), DVDs, and Blu-Ray discs use a **2048-byte logical block size** for data
sectors. Both Joliet (via ISO 9660) and UDF mandate this sector size for all on-disk structures.

> [!CAUTION]
> Audio CDs (CD-DA) use 2352-byte raw sectors. The file system driver must never attempt to
> parse ISO 9660 or UDF structures from audio-only sessions. Always verify the session type
> via READ TOC (`0x43`) before attempting file system detection.

### READ(10) vs READ(16) Negotiation

The block device driver must implement a capability negotiation phase during device discovery.
Issue IDENTIFY PACKET DEVICE (`0xA1`) and check Word 0, bits 1:0 for CDB size support. If the
drive reports 16-byte CDB support and the media's last LBA exceeds `0xFFFFFFFF`, use READ(16).
Otherwise, fall back to READ(10) exclusively.

---

## ISO 9660 Foundation for Joliet

Joliet is an extension to the ISO 9660 (ECMA-119) file system. It does not replace ISO 9660
but adds a parallel directory hierarchy accessible through a **Supplementary Volume Descriptor**
(SVD). A Joliet-aware driver must parse both the Primary Volume Descriptor (PVD) for fallback
and the Joliet SVD for Unicode filenames.

### Volume Descriptor Set Layout

The ISO 9660 volume descriptor set begins at **sector 16** (byte offset `0x8000`), immediately
following the 32 KiB System Area. Each descriptor occupies exactly one 2048-byte sector.

| Offset | Size | Field              | Description                                       |
| ------ | ---- | ------------------ | ------------------------------------------------- |
| `0x00` | 1    | `Type`             | Descriptor type code                              |
| `0x01` | 5    | `Identifier`       | Always `"CD001"` (ASCII)                          |
| `0x06` | 1    | `Version`          | Always `0x01`                                     |
| `0x07` | 2041 | Type-specific data | Varies by descriptor type                         |

Volume descriptor type codes:

| Type | Name                              | Purpose                                   |
| ---- | --------------------------------- | ----------------------------------------- |
| 0    | Boot Record                       | El Torito boot catalog pointer            |
| 1    | Primary Volume Descriptor (PVD)   | Standard ISO 9660 directory hierarchy     |
| 2    | Supplementary Volume Descriptor   | Joliet uses this type for UCS-2 hierarchy |
| 255  | Volume Descriptor Set Terminator  | End of descriptor set                     |

The driver must iterate through all descriptors starting at sector 16 until it encounters a
type 255 terminator. Multiple SVDs may exist; the Joliet SVD is identified by its escape
sequences (see below).

---

## Joliet Volume Recognition and Identification

### Supplementary Volume Descriptor Escape Sequences

A Supplementary Volume Descriptor is positively identified as Joliet by examining the **Escape
Sequences** field at offset `0x58` (88 bytes, padded with zeros). The presence of any of the
following ISO 2022 escape sequences confirms UCS-2 encoding:

| UCS-2 Level | Escape Sequence (Hex)    | ASCII Representation | Description          |
| ----------- | ------------------------ | -------------------- | -------------------- |
| Level 1     | `(25)(2F)(40)`           | `%/@`                | UCS-2 Level 1       |
| Level 2     | `(25)(2F)(43)`           | `%/C`                | UCS-2 Level 2       |
| Level 3     | `(25)(2F)(45)`           | `%/E`                | UCS-2 Level 3       |

> [!IMPORTANT]
> The driver must also verify that **Bit 0** of the SVD **Volume Flags** field (offset `0x07`
> in the SVD) is explicitly set to **ZERO**. This serves as secondary confirmation that the
> escape sequences are registered per ISO 2375.

### SVD Structure for Joliet (Key Fields)

The Joliet SVD follows the standard ISO 9660 SVD layout (ECMA-119 §8.5). Key fields for the
file system driver:

| Offset   | Size | Field                        | Joliet Usage                                   |
| -------- | ---- | ---------------------------- | ---------------------------------------------- |
| `0x00`   | 1    | Volume Descriptor Type       | `0x02` (Supplementary)                         |
| `0x01`   | 5    | Standard Identifier          | `"CD001"`                                      |
| `0x06`   | 1    | Volume Descriptor Version    | `0x01`                                         |
| `0x07`   | 1    | Volume Flags                 | Bit 0 must be `0` for Joliet                   |
| `0x58`   | 32   | Escape Sequences             | UCS-2 escape sequence (see table above)        |
| `0x9E`   | 8    | Root Directory Record        | Points to Joliet root directory extent          |
| `0x8C`   | 4    | Path Table Size              | Size of Joliet path table in bytes (LE)        |
| `0x94`   | 4    | Location of L Path Table     | LBA of Type L (little-endian) path table       |
| `0x98`   | 4    | Location of Optional L PT    | LBA of optional second Type L path table       |
| `0x9C`   | 4    | Location of M Path Table     | LBA of Type M (big-endian) path table (BE)     |

> [!NOTE]
> When a Joliet SVD is detected, the driver should use the Joliet directory hierarchy
> (rooted at the SVD's Root Directory Record) in preference to the PVD hierarchy. The PVD
> hierarchy serves as a fallback for non-Joliet-aware systems.

---

## Joliet Naming Rules and Directory Hierarchy

### UCS-2 Character Encoding

All text fields in the Joliet directory hierarchy are encoded in **16-bit UCS-2**, stored in
**Big Endian** (most significant byte first) byte order, regardless of the host architecture's
native endianness.

On x86-64 (Little Endian), the driver must byte-swap each 16-bit word when reading Joliet
identifiers:

```c
uint16_t ucs2_char = (raw_bytes[i] << 8) | raw_bytes[i + 1];
```

### Allowed Characters

All UCS-2 code points are permitted **except** the following:

| Code Point Range | Description                                              |
| ---------------- | -------------------------------------------------------- |
| `0x0000–0x001F`  | Control characters                                       |
| `0x002A`         | Asterisk `*`                                             |
| `0x002F`         | Forward slash `/`                                        |
| `0x003A`         | Colon `:`                                                |
| `0x003B`         | Semicolon `;`                                            |
| `0x003F`         | Question mark `?`                                        |
| `0x005C`         | Backslash `\`                                            |

### Filename and Directory Identifier Limits

| Parameter                    | ISO 9660 Level 1 | Joliet                              |
| ---------------------------- | ----------------- | ----------------------------------- |
| Max file identifier length   | 31 bytes          | **128 bytes** (64 UCS-2 characters) |
| Max directory identifier     | 31 bytes          | **128 bytes** (64 UCS-2 characters) |
| Directory name extensions    | Not allowed       | **Allowed** (period + extension)    |
| Max directory depth          | 8 levels          | **Unlimited** (path length ≤ 240)   |

> [!IMPORTANT]
> The 128-byte limit for file and directory identifiers is measured in **bytes**, not
> characters. Since UCS-2 uses 2 bytes per character, this yields a maximum of 64 Unicode
> characters per identifier.

### Path Length Constraint

Although the 8-level directory depth limit is abolished, the total path length must not exceed
**240 bytes**. The path length is calculated as the sum of:

- The length of the file identifier (in bytes)
- The length of all directory identifiers in the path (in bytes)
- The number of relevant directories (one byte per separator)

### Special Directory Identifiers

The reserved directory identifiers for current directory (`0x00`) and parent directory (`0x01`)
remain as **8-bit single-byte values**, even on UCS-2 volumes. They are placeholders, not
graphic characters, and are not expanded to 16 bits.

### Separator Characters

SEPARATOR 1 (`.`, period, `0x2E`) and SEPARATOR 2 (`;`, semicolon, `0x3B`) are expanded to
their UCS-2 equivalents on Joliet volumes:

| Separator   | ISO 9660 Value | Joliet UCS-2 Value |
| ----------- | -------------- | ------------------ |
| SEPARATOR 1 | `(2E)`         | `(00)(2E)`         |
| SEPARATOR 2 | `(3B)`         | `(00)(3B)`         |

### Sort Ordering and Justification Padding

On Joliet volumes, the sort justification pad byte is redefined to `(00)` (zero) instead of
the ISO 9660 defaults of `(20)` for path table records and directory records, and `(30)` for
file version numbers. This simplifies byte-by-byte comparison of UCS-2 strings.

Correct sort ordering is **mandatory** on UCS-2 volumes. No natural-language collation is
performed on-disk; display-layer sorting is optional.

---

## Universal Disk Format (UDF) Volume Recognition

UDF is a comprehensive file system specification based on ECMA-167 / ISO/IEC 13346. Unlike
Joliet (which extends ISO 9660), UDF is an entirely separate file system with its own volume
structure, directory format, and allocation mechanisms.

### Volume Recognition Sequence (VRS)

The UDF Volume Recognition Sequence occupies the same region as ISO 9660 descriptors,
starting at **sector 16** (byte offset `0x8000`). The VRS contains ECMA-167 descriptors that
use the same general layout as ISO 9660 volume descriptors:

| Offset | Size | Field         | Description                                       |
| ------ | ---- | ------------- | ------------------------------------------------- |
| `0x00` | 1    | Structure Type | `0x00`                                           |
| `0x01` | 5    | Identifier    | Magic string identifying the descriptor type      |
| `0x06` | 1    | Version       | Structure version, typically `0x01`               |
| `0x07` | 2041 | Data          | Structure-specific data (usually zeros)           |

The driver must scan VRS descriptors for the following magic identifiers:

| Identifier | Meaning                                                        |
| ---------- | -------------------------------------------------------------- |
| `"BEA01"`  | Beginning Extended Area Descriptor — marks start of VRS        |
| `"NSR02"`  | ECMA-167 2nd Edition — confirms UDF revision ≤ 2.00           |
| `"NSR03"`  | ECMA-167 3rd Edition — confirms UDF revision ≥ 2.01           |
| `"TEA01"`  | Terminating Extended Area Descriptor — marks end of VRS        |

> [!IMPORTANT]
> The presence of `"NSR02"` or `"NSR03"` in the VRS is the **definitive** confirmation that a
> valid UDF file system exists on the volume. The driver must detect one of these before
> proceeding to read the Anchor Volume Descriptor Pointer.

### Anchor Volume Descriptor Pointer (AVDP)

The AVDP is the critical entry point for all UDF data retrieval. It is located at **logical
sector 256** (`0x100`). Backup copies are mandated at:

- The **last recorded sector** of the volume
- **256 sectors from the end** of the volume

If sector 256 is unreadable, the driver must attempt these backup locations.

#### AVDP Structure (32 bytes)

| Offset | Size | Field                                  | Description                           |
| ------ | ---- | -------------------------------------- | ------------------------------------- |
| `0x00` | 16   | `DescriptorTag`                        | Common Descriptor Tag (see §5.1)      |
| `0x10` | 8    | `MainVolumeDescriptorSequenceExtent`   | Extent of primary VDS (LBA + length)  |
| `0x18` | 8    | `ReserveVolumeDescriptorSequenceExtent` | Extent of backup VDS                 |

Each extent field uses the **Extent Structure**:

| Offset | Size | Field          | Description                                        |
| ------ | ---- | -------------- | -------------------------------------------------- |
| `0x00` | 4    | `Length`       | Length of extent in bytes (little-endian)           |
| `0x04` | 4    | `Location`     | Starting logical sector number (little-endian)     |

> [!CAUTION]
> All UDF multi-byte integer fields are stored in **Little Endian** byte order, unlike SCSI
> response payloads which use Big Endian. The driver must not confuse UDF on-disk endianness
> with SCSI transport endianness.

---

## UDF Common Descriptor Tag

Every UDF descriptor is prefixed with a 16-byte **Common Descriptor Tag** that identifies the
descriptor type and provides integrity verification. The driver must validate this tag before
trusting any enclosed pointers.

### Descriptor Tag Structure (16 bytes)

| Offset | Size | Field               | Description and Validation                             |
| ------ | ---- | ------------------- | ------------------------------------------------------ |
| `0x00` | 2    | `TagIdentifier`     | Descriptor type (e.g., `0x0002` = AVDP, `0x0105` = FE) |
| `0x02` | 2    | `DescriptorVersion` | UDF version compliance of this descriptor              |
| `0x04` | 1    | `TagChecksum`       | Modulo-256 sum of bytes 0–3 and 5–15 of this tag       |
| `0x05` | 1    | `Reserved`          | Must be `0x00`                                         |
| `0x06` | 2    | `TagSerialNumber`   | Serial number for ordering                             |
| `0x08` | 2    | `DescriptorCRC`     | CRC-ITU-T of descriptor body (excluding tag)           |
| `0x0A` | 2    | `DescriptorCRCLength` | Byte count covered by the CRC                        |
| `0x0C` | 4    | `TagLocation`       | Logical block number where this tag resides            |

#### Tag Identifier Values

| Value    | Descriptor Type                           | Context                        |
| -------- | ----------------------------------------- | ------------------------------ |
| `0x0001` | Primary Volume Descriptor                 | VDS                            |
| `0x0002` | Anchor Volume Descriptor Pointer          | Sector 256                     |
| `0x0003` | Volume Descriptor Pointer                 | VDS                            |
| `0x0004` | Implementation Use Volume Descriptor      | VDS                            |
| `0x0005` | Partition Descriptor                      | VDS                            |
| `0x0006` | Logical Volume Descriptor                 | VDS                            |
| `0x0007` | Unallocated Space Descriptor              | VDS                            |
| `0x0008` | Terminating Descriptor                    | VDS terminator                 |
| `0x0009` | Logical Volume Integrity Descriptor       | LVID sequence                  |
| `0x0100` | File Set Descriptor                       | File system root               |
| `0x0101` | File Identifier Descriptor                | Directory entry                |
| `0x0105` | File Entry                                | File/directory metadata        |
| `0x010A` | Extended File Entry                       | UDF ≥ 2.00 metadata           |

> [!CAUTION]
> The `TagChecksum` is calculated as: `sum(tag[0..3]) + sum(tag[5..15]) mod 256`, deliberately
> **skipping byte 4** (the checksum byte itself). A mismatch indicates sector corruption or
> a maliciously altered structure. Parsing must abort immediately on checksum failure.

### Tag Checksum Verification Algorithm

```c
uint8_t udf_verify_tag_checksum(const uint8_t *tag) {
    uint8_t sum = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4) continue;  /* skip checksum byte itself */
        sum += tag[i];
    }
    return sum;  /* compare against tag[4] */
}
```

---

## UDF Volume Descriptor Sequence and Root Directory Resolution

The UDF initialization sequence follows a strict pointer-chasing path from the AVDP to the
root directory:

```
AVDP (sector 256)
  └─→ Volume Descriptor Sequence (VDS)
        ├─→ Partition Descriptor (Tag 0x0005) — physical partition mapping
        └─→ Logical Volume Descriptor (Tag 0x0006)
              └─→ File Set Descriptor (Tag 0x0100)
                    └─→ Root Directory File Entry (Tag 0x0105)
```

### Volume Descriptor Sequence (VDS)

The VDS begins at the sector specified by the AVDP's `MainVolumeDescriptorSequenceExtent`.
It contains a contiguous series of descriptors with tag identifiers `0x0001` through `0x0009`,
terminated by a Terminating Descriptor (`0x0008`).

> [!NOTE]
> The UDF specification mandates a minimum VDS extent of **16 sectors**. Empty descriptor
> blocks after the Terminating Descriptor should be ignored.

The driver must locate two critical records:

**Partition Descriptor** (Tag `0x0005`): Maps the physical partition space.

| Offset | Size | Field                    | Description                              |
| ------ | ---- | ------------------------ | ---------------------------------------- |
| `0x00` | 16   | `DescriptorTag`          | Tag with identifier `0x0005`             |
| `0x16` | 2    | `VolumeDescriptorSeqNum` | Sequence number within VDS               |
| `0x18` | 2    | `PartitionFlags`         | Bit 0: space allocated                   |
| `0x1A` | 2    | `PartitionNumber`        | Partition index (usually `0`)            |
| `0x1C` | 32   | `PartitionContents`      | Entity identifier (e.g., `"+NSR02"`)     |
| `0xBC` | 4    | `AccessType`             | `1`=read-only, `2`=write-once, `3`=RW    |
| `0xC0` | 4    | `PartitionStartingLoc`   | Starting logical sector of partition     |
| `0xC4` | 4    | `PartitionLength`        | Length in sectors                         |

**Logical Volume Descriptor** (Tag `0x0006`): Contains the pointer to the File Set Descriptor.

| Offset | Size | Field                | Description                                   |
| ------ | ---- | -------------------- | --------------------------------------------- |
| `0x00` | 16   | `DescriptorTag`      | Tag with identifier `0x0006`                  |
| `0x54` | 4    | `LogicalBlockSize`   | Logical block size in bytes (typically 2048)   |
| `0xF8` | 16   | `FSD_ICB_Location`   | Long Allocation Descriptor pointing to FSD    |

### File Set Descriptor (Tag `0x0100`)

The FSD serves as the true root of the UDF file system tree. Its critical field is the
**Root Directory ICB** — a Long Allocation Descriptor that points to the root directory's
File Entry.

| Offset | Size | Field                       | Description                              |
| ------ | ---- | --------------------------- | ---------------------------------------- |
| `0x00` | 16   | `DescriptorTag`             | Tag with identifier `0x0100`             |
| `0x190` | 16  | `RootDirectoryICB`          | Long AD pointing to root dir File Entry  |

---

## UDF File Entries and Information Control Blocks

Every file, directory, and named stream in UDF is represented by an **Information Control
Block** (ICB), which manifests as either a **File Entry** (Tag `0x0105`) or an **Extended
File Entry** (Tag `0x010A`, UDF ≥ 2.00).

### File Entry Structure (Tag `0x0105`) — Key Fields

| Offset | Size | Field                  | Description                                    |
| ------ | ---- | ---------------------- | ---------------------------------------------- |
| `0x00` | 16   | `DescriptorTag`        | Tag with identifier `0x0105`                   |
| `0x10` | 20   | `ICBTag`               | File type, strategy, allocation type            |
| `0x24` | 4    | `Uid`                  | POSIX User ID                                  |
| `0x28` | 4    | `Gid`                  | POSIX Group ID                                 |
| `0x2C` | 4    | `Permissions`          | POSIX permission bits (User/Group/Other × RWX) |
| `0x30` | 2    | `FileLinkCount`        | Hard link count                                |
| `0x38` | 8    | `InformationLength`    | Exact file size in bytes (64-bit)              |
| `0x40` | 8    | `LogicalBlocksRecorded`| Blocks consumed by this file                   |
| `0x48` | 12   | `AccessTime`           | POSIX atime (UDF timestamp format)             |
| `0x54` | 12   | `ModificationTime`     | POSIX mtime                                    |
| `0x60` | 12   | `AttributeTime`        | POSIX ctime                                    |
| `0x74` | 4    | `LengthOfAllocationDescs` | Total size of allocation descriptors        |

The allocation descriptors follow immediately after the fixed fields and map to the actual
file data on disk.

### ICB Tag — File Type Values

The `FileType` field within the ICB Tag (offset `0x1B` within the ICB Tag) identifies the
entry type:

| Value  | File Type                                                    |
| ------ | ------------------------------------------------------------ |
| `0x04` | Directory                                                    |
| `0x05` | Regular file                                                 |
| `0x0C` | Symbolic link                                                |
| `0x0D` | Named pipe (FIFO)                                            |
| `0xFE` | Terminal entry (Extended Attribute ICB)                       |

### Allocation Descriptors

File data is located via Allocation Descriptors appended to the File Entry. The ICB Tag's
`AllocationDescriptorType` field (bits 2:0 of the Flags field) determines the format:

| Type | Name                         | Size   | Fields                              |
| ---- | ---------------------------- | ------ | ----------------------------------- |
| 0    | Short Allocation Descriptor  | 8 bytes | 4-byte length + 4-byte LBA position |
| 1    | Long Allocation Descriptor   | 16 bytes | 4-byte length + 6-byte LBA + 2-byte partition ref |
| 3    | Inline (embedded data)       | N/A    | File data is embedded in the FE     |

> [!IMPORTANT]
> For each Short Allocation Descriptor, bits 31:30 of the `Length` field encode the extent
> type: `0` = recorded and allocated, `1` = allocated but not recorded, `2` = not allocated
> (sparse), `3` = continuation extent pointing to next allocation extent.

### File Identifier Descriptor (Tag `0x0101`)

Within a UDF directory, each child entry is a **File Identifier Descriptor** (FID). Directory
content is a contiguous array of FIDs stored in the extent(s) referenced by the directory's
File Entry.

| Offset | Size    | Field                    | Description                              |
| ------ | ------- | ------------------------ | ---------------------------------------- |
| `0x00` | 16      | `DescriptorTag`          | Tag with identifier `0x0101`             |
| `0x10` | 2       | `FileVersionNumber`      | File version (typically `1`)             |
| `0x12` | 1       | `FileCharacteristics`    | Flags: bit 0=hidden, bit 1=directory,    |
|        |         |                          | bit 2=deleted, bit 3=parent              |
| `0x13` | 1       | `LengthOfFileIdentifier` | Byte length of filename                  |
| `0x14` | 16      | `ICB`                    | Long AD pointing to child's File Entry   |
| `0x24` | 2       | `LengthOfImplUse`        | Length of implementation use area         |
| `0x26` | varies  | `ImplementationUse`      | Vendor-specific data                     |
| varies | varies  | `FileIdentifier`         | OSTA Compressed Unicode filename         |

> [!CAUTION]
> The first FID in every directory is always the **parent directory entry** (bit 3 of
> `FileCharacteristics` set). The root directory's parent FID points back to itself.
> Each FID is padded to a 4-byte boundary. Total FID size = `0x26 + LengthOfImplUse +
> LengthOfFileIdentifier`, rounded up to the next multiple of 4.

---

## UDF Media Variations (Builds)

### Plain Build

The simplest format, supported across all UDF revisions. Used on media with true random
read/write access: HDD, USB flash, DVD-RAM, DVD+RW. Allocation Descriptor LBAs map
**directly** to physical sectors relative to the partition start.

### Virtual Allocation Table (VAT) Build

Designed for **write-once** media (CD-R, DVD-R, BD-R without POW). Modifying a file requires
writing a new File Entry to the next blank sector. The VAT provides an indirection layer:

- Allocation Descriptors use **virtual addresses** (not physical LBAs)
- The VAT is a hidden file containing a translation table: virtual → physical LBA
- The VAT's ICB is always the **last written sector** of the disc/session
- The driver must read the VAT at mount time and load it into memory

### Spared Build

Designed for **rewritable** media with limited overwrite endurance (DVD-RW restricted
overwrite). Implements **Sparing Tables** for defect management:

- The Sparable Partition Map (in the LVD) contains the Sparing Table locations
- Before each read, cross-reference the requested LBA against the Sparing Table
- Defective sectors are silently redirected to remapped sectors in a sparing zone

---

## Text Encoding and Unicode Translation

### Joliet UCS-2 to UTF-8 Conversion

Joliet encodes filenames in 16-bit UCS-2 (Big Endian). The driver must convert to UTF-8
before passing filenames to the VFS layer:

```c
/* Convert a single UCS-2 code point (already byte-swapped to native) to UTF-8 */
int ucs2_to_utf8(uint16_t cp, uint8_t *out) {
    if (cp <= 0x007F) {
        out[0] = (uint8_t)cp;
        return 1;
    } else if (cp <= 0x07FF) {
        out[0] = 0xC0 | (cp >> 6);
        out[1] = 0x80 | (cp & 0x3F);
        return 2;
    } else {
        out[0] = 0xE0 | (cp >> 12);
        out[1] = 0x80 | ((cp >> 6) & 0x3F);
        out[2] = 0x80 | (cp & 0x3F);
        return 3;
    }
}
```

> [!CAUTION]
> If an invalid or unmapped code point is encountered, substitute with the **Unicode
> Replacement Character** (U+FFFD, UTF-8 bytes: `0xEF 0xBF 0xBD`). Never panic or halt on
> encoding errors. The output buffer must be sized for worst-case expansion: up to 3× the
> input byte count.

### UDF OSTA Compressed Unicode Decompression

UDF filenames use OSTA Compressed Unicode (OSTA CS0). The first byte is a **Compression ID**:

| Compression ID | Mode    | Description                                            |
| -------------- | ------- | ------------------------------------------------------ |
| `8`            | 8-bit   | Each byte = lower 8 bits of UCS-2 (Latin-1/ASCII)     |
| `16`           | 16-bit  | Full UCS-2 values stored in Big Endian                 |
| `254`          | 8-bit   | Same as 8, used for OS-specific identifiers            |
| `255`          | 16-bit  | Same as 16, used for OS-specific identifiers           |

#### Decompression Algorithm

```c
/* Decompress OSTA CS0 string to UCS-2 array */
int osta_decompress(const uint8_t *src, int src_len, uint16_t *dst, int max_chars) {
    if (src_len < 1) return 0;
    uint8_t comp_id = src[0];
    int pos = 1, count = 0;

    while (pos < src_len && count < max_chars) {
        if (comp_id == 8 || comp_id == 254) {
            dst[count++] = src[pos++];        /* zero-extend to 16-bit */
        } else if (comp_id == 16 || comp_id == 255) {
            if (pos + 1 >= src_len) break;
            dst[count++] = (src[pos] << 8) | src[pos + 1];  /* Big Endian */
            pos += 2;
        } else {
            break;  /* unknown compression ID */
        }
    }
    return count;
}
```

After decompression to UCS-2, pipe the result through the same UCS-2 → UTF-8 converter
used for Joliet before passing to the VFS layer.

---

## VFS Integration and POSIX Semantics

### Permission Mapping

| Source       | Mapping Strategy                                               |
| ------------ | -------------------------------------------------------------- |
| UDF          | Direct: `Permissions` field maps to POSIX User/Group/Other RWX |
| Joliet       | Synthesized: files → `0444`, directories → `0555` (read-only) |

### Timestamp Mapping

UDF timestamps include microsecond resolution and UTC timezone information (from UDF 2.01
onward). The driver must:

1. Convert UDF timestamps to `time_t` (truncating microseconds)
2. Apply UTC offset for local time calculation (UDF ≥ 2.01 mandates UTC storage)
3. Map `ModificationTime` → `st_mtime`, `AccessTime` → `st_atime`,
   `AttributeTime` → `st_ctime`

Joliet timestamps use ISO 9660 format (BCD-encoded date/time with 15-minute timezone
granularity). These must be converted to `time_t` using the standard ISO 9660 date decoding.

### Inode Generation

| Source  | Inode Strategy                                                  |
| ------- | --------------------------------------------------------------- |
| UDF     | Use the 32-bit LBA of the File Entry's ICB as the inode number |
| Joliet  | Use the LBA of the directory record's extent as the inode       |

Both strategies guarantee uniqueness across the volume.

### Case Sensitivity

Both Joliet (UCS-2) and UDF (OSTA CS0) record **case-sensitive** filenames. The VFS must
treat these as case-sensitive, preserving the exact Unicode identifiers on disk. This
accurately reflects the binary nature of the recorded identifiers and prevents naming
collisions.

---

## Performance Optimization and Caching

### Multi-Tier Caching Architecture

| Cache Tier            | Layer           | Strategy                                   |
| --------------------- | --------------- | ------------------------------------------ |
| Raw Sector Cache      | Block layer     | LRU eviction, 2048-byte sectors            |
| Directory Entry Cache | VFS (dcache)    | Hash table: path → inode, O(1) lookup      |
| Read-Ahead Cache      | Block layer     | Sequential prefetch of sectors N+1..N+k    |

> [!IMPORTANT]
> Optical drives have seek times orders of magnitude slower than HDD or SSD. The driver must
> aggressively cache UDF descriptor chains (AVDP → VDS → LVD → FSD) at mount time, and
> cache all Joliet path tables in memory to avoid repeated seeks during directory traversal.

### VAT Caching

For UDF VAT builds (CD-R, DVD-R), the entire Virtual Allocation Table must be loaded into
kernel memory at mount time. This table maps virtual LBAs to physical LBAs and is essential
for every file read operation. Memory cost is approximately 4 bytes per virtual block entry.

---

## Security and Vulnerability Mitigation

### Extent Validation

Every Allocation Descriptor's `Length` field must be validated against:

1. The remaining size of the physical partition
2. The available kernel memory buffer size

If a malicious extent claims an impossible length (e.g., `0xFFFFFFFF`), the driver must
truncate to partition bounds or return `EIO`. Never blindly allocate based on untrusted
extent values.

### Descriptor Tag Verification

Before parsing any UDF descriptor, independently calculate and verify:

1. The 8-bit `TagChecksum` (modulo-256 of bytes 0–3 and 5–15)
2. The 16-bit `DescriptorCRC` of the descriptor body

A mismatch indicates corruption or tampering. Abort parsing immediately.

### String Buffer Bounding

When decompressing OSTA Compressed Unicode or translating Joliet UCS-2:

- Pre-calculate the maximum output size: `input_chars × 3` bytes for UTF-8
- Enforce a hard cap at **255 bytes** (POSIX `NAME_MAX`) for VFS filenames
- Reject strings that exceed the output buffer rather than truncating silently

### Circular Reference Detection

Malicious or corrupted UDF images can create circular directory references. The driver must
implement cycle detection via:

1. **Hard depth limit**: Abort traversal at 256 nested levels (return `ELOOP`)
2. **Ancestry tracking**: Maintain a stack of visited inode numbers; if a duplicate is
   encountered, halt immediately and return `ELOOP`

> [!WARNING]
> Historical CVEs (CVE-2023-37454, CVE-2015-4167) demonstrate real-world exploitation of
> UDF parsing bugs causing out-of-bounds reads and denial-of-service. Every pointer from
> disk must be bounds-checked before dereferencing.

---

## QEMU Testing Configuration

### ISO 9660 + Joliet Testing

Create a test ISO with Joliet extensions using `genisoimage` or `xorriso`:

```bash
# Create test ISO with Joliet
genisoimage -o test-joliet.iso -J -joliet-long -r /path/to/test/data

# Launch QEMU with IDE CD-ROM
qemu-system-x86_64 \
    -drive file=build/system-disk.img,format=raw,if=ide \
    -cdrom test-joliet.iso \
    -serial stdio -m 256
```

### UDF Testing

Create a UDF test image using `mkudffs`:

```bash
# Create UDF 2.01 test image (100 MB)
dd if=/dev/zero of=test-udf.img bs=1M count=100
mkudffs --media-type=dvd --udfrev=0x0201 test-udf.img

# Mount and populate on host
sudo mount -o loop test-udf.img /mnt/udf
sudo cp -r /path/to/test/data/* /mnt/udf/
sudo umount /mnt/udf

# Launch QEMU with IDE CD-ROM
qemu-system-x86_64 \
    -drive file=build/system-disk.img,format=raw,if=ide \
    -drive file=test-udf.img,format=raw,if=ide,media=cdrom \
    -serial stdio -m 256
```

### AHCI with CD-ROM

```bash
# AHCI mode with optical drive
qemu-system-x86_64 \
    -drive file=build/system-disk.img,format=raw,if=none,id=disk0 \
    -device ahci,id=ahci0 \
    -device ide-hd,drive=disk0,bus=ahci0.0 \
    -drive file=test-joliet.iso,format=raw,if=none,id=cdrom0,media=cdrom \
    -device ide-cd,drive=cdrom0,bus=ahci0.1 \
    -serial stdio -m 256
```

---

## Implementation Priorities for Impossible OS

| Priority | Component                                | Dependencies                    |
| -------- | ---------------------------------------- | ------------------------------- |
| 🔴 P0    | ATAPI block device driver (READ 10)      | AHCI driver, PCI enumeration    |
| 🔴 P0    | ISO 9660 PVD parsing                     | ATAPI block device              |
| 🔴 P0    | Joliet SVD detection and UCS-2 decoding  | ISO 9660 PVD parser             |
| 🟠 P1    | Joliet directory traversal + VFS mount   | Joliet SVD parser, VFS layer    |
| 🟠 P1    | UDF Volume Recognition (VRS + AVDP)      | ATAPI block device              |
| 🟠 P1    | UDF VDS → LVD → FSD → root resolution  | UDF VRS parser                  |
| 🟠 P1    | UDF File Entry parsing + allocation descs | UDF root resolution            |
| 🟠 P1    | OSTA Compressed Unicode → UTF-8         | UDF File Entry parser           |
| 🟡 P2    | UDF directory traversal (FID parsing)    | UDF FE parser, FID parser       |
| 🟡 P2    | Raw sector cache (LRU)                   | Block device layer              |
| 🟡 P2    | Directory entry cache (dcache)           | VFS layer                       |
| 🟢 P3    | VAT build support (CD-R/DVD-R)           | UDF Plain build complete        |
| 🟢 P3    | Spared build support (DVD-RW)            | UDF Plain build complete        |
| 🟢 P3    | Read-ahead caching                       | Sector cache                    |
| 🔵 P4    | Circular reference detection (ELOOP)     | Directory traversal             |
| 🔵 P4    | Extended File Entry (Tag 0x010A)         | UDF Basic FE parser             |
| 🔵 P4    | UDF Metadata Partition (rev 2.50+)       | UDF 2.01 support complete       |

> [!NOTE]
> The Impossible OS AHCI driver and VFS layer are already implemented. The ATAPI extension
> for optical media and the ISO 9660/Joliet/UDF parsers are new components that build on
> the existing block device and file system infrastructure.
