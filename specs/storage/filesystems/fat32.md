# Exhaustive Technical Specification of the FAT32 File System for Operating System Implementation

## Architectural Foundations and Historical Context

The File Allocation Table (FAT) file system family represents one of the most widely supported and ubiquitous storage architectures in computing history, presenting a mandatory implementation requirement for custom operating system developers seeking interoperability across diverse hardware media. Evolving from its early origins in 1977 for Standalone Disk BASIC-80, the architecture progressed through FAT12 in August 1980 (initially utilized in SCP QDOS), and FAT16 in August 1984 with IBM PC DOS 3.0. The 32-bit manifestation, FAT32, was officially introduced in August 1996 with the release of Windows 95 OSR2, engineered specifically to overcome the volumetric limitations of its 16-bit predecessor while retaining the structural simplicity that defined the FAT lineage.

The fundamental design goal of the FAT32 file system is to manage the allocation of the data area through a centralized array of pointers known as the File Allocation Table. Two of the enduring strengths of FAT-based file systems are their relative simplicity and ease of implementation compared to modern journaling file systems. Unlike highly complex modern architectures (such as NTFS, ext4, or APFS) that utilize balanced B-trees, extensive transaction journaling, and complex metadata structures, FAT32 relies on a straightforward linked-list structural paradigm for file allocation. This inherent simplicity guarantees high interoperability and data exchange capabilities across computers and mobile devices of almost any type and age. For kernel developers, implementing a robust FAT32 driver requires a meticulous understanding of on-disk data structures, endianness constraints, mathematical cluster mappings, and the backward-compatible directory entry schemes utilized for extended character sets.

While FAT32 is immensely popular, its design parameters impose specific volumetric limits. The maximum volume size for a FAT32 partition scales with the physical sector size of the underlying storage medium. With standard 512-byte sectors, the theoretical maximum volume size is 2 Terabytes (TB). Advanced implementations utilizing 2 Kilobyte (KB) sectors and 32 KB clusters can format volumes up to 8 TB, while architectures employing 4 KB sectors alongside 64 KB clusters can address up to 16 TB. Regardless of the volume size, the absolute maximum file size permitted within the FAT32 specification is rigidly capped at 4,294,967,295 bytes, which equates to 4 Gibibytes (GiB) minus one byte. The file system can host a maximum of 268,173,300 files when utilizing 32 KB clusters.

## Logical Volume Memory Map

A fully initialized FAT32 logical volume is divided sequentially into distinct, continuous regions. Understanding the exact boundary conditions of these regions is the first prerequisite for disk traversal and memory mapping within a custom kernel. A FAT logical volume consists of three primary areas located in strict sequential order: the Reserved Region, the FAT Region, and the Data Region.

- **Reserved Region:** This area begins at the very first sector of the partition (Logical Block Address 0 relative to the partition start). It contains vital volume configuration data, principally the Boot Sector (which includes the BIOS Parameter Block), the File System Information (FSInfo) sector, the Backup Boot Sector, and optionally additional reserved sectors. The size of this region is strictly defined by a configuration parameter encoded within the Boot Sector.

- **FAT Region:** Immediately following the termination of the Reserved Region, this space is occupied by the File Allocation Tables themselves. To ensure redundancy and mitigate catastrophic data loss from localized bad sectors, standard formatting dictates the presence of two identical FAT copies (designated as FAT #1 and FAT #2), although the official specification technically permits varying numbers of redundant tables. The operating system storage driver must update all copies simultaneously during write operations to maintain structural integrity.

- **Data Region:** Spanning the entire remainder of the partition, the Data Region contains the actual payload: the binary contents of all directories and files. A fundamental architectural shift from FAT12 and FAT16 to FAT32 is the elimination of the statically allocated Root Directory Area. In legacy FAT systems, a fixed-size root directory was wedged strictly between the FAT Region and the Data Region. FAT32 abandons this constraint; instead, the FAT32 root directory is treated as an ordinary, dynamically expandable cluster chain residing entirely within the Data Region alongside standard files.

## Sub-Type Determination and Volumetric Constraints

A critical architectural mandate is that the specific file system sub-type--whether FAT12, FAT16, or FAT32--is determined **exclusively** by the total count of addressable clusters in the Data Region. An operating system driver must never trust descriptive strings (such as `"FAT32   "` or `"FAT16   "`) found in the Boot Sector to determine the FAT type. The string fields are purely cosmetic and unreliable. The dynamic calculation of the cluster count isolates the true file system type based on the following rigid mathematical boundaries:

- **FAT12 Definition:** A volume with a total data cluster count less than or equal to 4,085 is strictly categorized as FAT12.
- **FAT16 Definition:** A volume containing between 4,086 and 65,525 clusters inclusive is strictly categorized as FAT16.
- **FAT32 Definition:** A volume containing 65,526 or more clusters is strictly categorized as FAT32.

For a volume to legitimately qualify as FAT32, the data region must contain at least 65,526 clusters. Practically, Microsoft Windows format utilities enforce a minimum volume size of 512 Megabytes (MB) for FAT32 formatting. Formatting a smaller drive as FAT32 results in inefficient cluster allocation that severely wastes disk space. Therefore, while users can forcefully format drives smaller than 512 MB as FAT32 using third-party tools, standard operating system drivers optimize smaller capacities by utilizing FAT16. To ensure maximum compatibility across varying driver implementations, formatting utilities are advised to maintain cluster counts at least 16 clusters away from these strict sub-type boundaries.

## Endianness and Hardware Portability

When developing a low-level driver, hardware portability is a paramount concern. The FAT32 file system strictly represents all multibyte integer data structures in **little-endian byte order**. In a little-endian architecture, the least significant byte (LSB) of a multibyte integer is stored at the lowest physical memory address on the disk, and the most significant byte (MSB) is stored at the highest address within the sequence.

For operating systems compiled to execute on big-endian hardware architectures (such as certain PowerPC chips, SPARC processors, or systems employing network byte order over IP), the kernel driver must explicitly perform byte-swapping operations on all 16-bit and 32-bit integers read from the storage medium prior to their programmatic evaluation. Similarly, these values must be byte-swapped back into little-endian format before being flushed to the disk. For instance, a 16-bit value representing a standard 512 bytes per sector is stored sequentially on the disk array as `0x00, 0x02`. When read into memory, this sequence must be interpreted mathematically as `0x0200`, which converts to the decimal value 512. Conversely, string characters and text encodings occupy only a single byte per discrete element and therefore do not require re-ordering across hardware boundaries.

## Disk Addressing Methodologies

Early operating systems relied heavily on Cylinder-Head-Sector (CHS) addressing schemes utilizing BIOS interrupt `0x13` to navigate the physical spinning platters of early hard disk drives. Modern custom operating system implementations should discard CHS entirely when interfacing with storage media. Contemporary IDE, AHCI (SATA), and NVMe drives universally support Logical Block Addressing (LBA), which abstracts physical disk geometry into a sequential array of logically numbered sectors starting at zero. Developing a driver based purely on LBA arithmetic vastly simplifies file system calculations and ensures compatibility with high-capacity solid-state storage.

## The Boot Sector and BIOS Parameter Block (BPB)

The initial entry point for parsing the file system hierarchy is the Master Boot Record (MBR) or GUID Partition Table (GPT), which points the operating system to the Volume Boot Record (VBR) located at Logical Block Address (LBA) 0 relative to the start of the defined partition. The Boot Sector is a highly structured 512-byte block that contains executable bootstrap code and the comprehensive BIOS Parameter Block (BPB). The BPB serves as the foundational telemetry map, providing the geometric and structural configurations required to successfully mount the volume.

The FAT32 BPB is an evolutionary extension of the traditional FAT16 BPB. The data structures are perfectly identical up to byte offset `0x20` (32 bytes). Beyond this boundary, the structures diverge; FAT32 introduces the Extended BIOS Parameter Block (EBPB) to fulfill its 32-bit cluster addressing requirements.

### Fundamental BPB Layout (Offsets 0x00 to 0x23)

The base parameters common to all FAT architectures are positioned at the very beginning of the boot sector. An OS developer maps a C-style struct directly over these bytes to extract the variables.

| Offset (Hex) | Offset (Dec) | Length (Bytes) | Field Name       | Description and FAT32 Specifics                                                                                                                                                                                                     |
| ------------ | ------------ | -------------- | ---------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `0x00`       | 0            | 3              | BS_jmpBoot       | Executable jump instruction directing the CPU to the bootstrap code. Typically observed as `0xEB 0x58 0x90` (FAT32) or a variant of `0xE9` followed by a 16-bit offset.                                                             |
| `0x03`       | 3            | 8              | BS_OEMName       | OEM Identifier string. Often padded with spaces, such as `"MSWIN4.1"` or `"MSDOS5.0"`. Purely cosmetic, not required for parsing logic.                                                                                             |
| `0x0B`       | 11           | 2              | BPB_BytsPerSec   | The count of bytes per logical sector. Valid values are rigidly restricted to 512, 1024, 2048, or 4096 bytes. Standard implementations predominantly use 512 bytes.                                                                  |
| `0x0D`       | 13           | 1              | BPB_SecPerClus   | Sectors per cluster. The value must be an exact power of 2 (1, 2, 4, 8, 16, 32, 64, 128). The resultant cluster size should not exceed 32,768 bytes (32 KB).                                                                       |
| `0x0E`       | 14           | 2              | BPB_RsvdSecCnt   | Reserved sector count. Dictates the size of the reserved area. For FAT32 volumes, this value is typically set to 32.                                                                                                                |
| `0x10`       | 16           | 1              | BPB_NumFATs      | The number of distinct File Allocation Table arrays stored on the disk. Almost universally set to 2 for redundancy.                                                                                                                 |
| `0x11`       | 17           | 2              | BPB_RootEntCnt   | Maximum 32-byte directory entries in the root directory. **Must strictly be 0 on all FAT32 volumes**, as the root directory is a dynamic cluster chain.                                                                             |
| `0x13`       | 19           | 2              | BPB_TotSec16     | 16-bit total volume sector count. **Must be 0 for all FAT32 volumes.** The actual size is in the 32-bit field.                                                                                                                      |
| `0x15`       | 21           | 1              | BPB_Media        | Media descriptor type. `0xF8` indicates a fixed hard disk. This value is mirrored in FAT[0].                                                                                                                                        |
| `0x16`       | 22           | 2              | BPB_FATSz16      | 16-bit FAT sector count. **Must strictly be 0 for FAT32.** The driver must use the 32-bit equivalent in the EBPB.                                                                                                                   |
| `0x18`       | 24           | 2              | BPB_SecPerTrk    | Sectors per track for legacy INT 13h disk geometry.                                                                                                                                                                                  |
| `0x1A`       | 26           | 2              | BPB_NumHeads     | Number of read/write heads for legacy INT 13h disk geometry.                                                                                                                                                                        |
| `0x1C`       | 28           | 4              | BPB_HiddSec      | Hidden sectors preceding the partition start. Necessary for computing absolute physical LBA offsets on partitioned media.                                                                                                            |
| `0x20`       | 32           | 4              | BPB_TotSec32     | 32-bit total logical sector count defining the absolute capacity of the volume.                                                                                                                                                      |

### FAT32 Extended BIOS Parameter Block (EBPB)

Beginning at offset `0x24`, the parameter block structure permanently diverges from FAT16 standards to accommodate FAT32-specific configuration pointers and flags.

| Offset (Hex) | Offset (Dec) | Length (Bytes) | Field Name       | Description and Implementation Mandates                                                                                                                                                                                                                    |
| ------------ | ------------ | -------------- | ---------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `0x24`       | 36           | 4              | BPB_FATSz32      | 32-bit unsigned integer: exact count of sectors occupied by ONE single FAT data structure.                                                                                                                                                                 |
| `0x28`       | 40           | 2              | BPB_ExtFlags     | Extended flags. Bit 7: if 0, FAT is mirrored to all copies simultaneously; if 1, only active FAT is updated (bits 0–3 = active FAT index).                                                                                                                |
| `0x2A`       | 42           | 2              | BPB_FSVer        | File system version. Must be `0x0000`. Any other value triggers mount rejection.                                                                                                                                                                            |
| `0x2C`       | 44           | 4              | BPB_RootClus     | Cluster number of the FAT32 root directory start. Typically cluster 2, but **must not be assumed -- read this field**.                                                                                                                                       |
| `0x30`       | 48           | 2              | BPB_FSInfo       | Sector number of the FSInfo structure (relative to reserved area). Typically 1.                                                                                                                                                                              |
| `0x32`       | 50           | 2              | BPB_BkBootSec    | Sector number of the backup boot sector (relative to reserved area). Typically 6.                                                                                                                                                                           |
| `0x34`       | 52           | 12             | BPB_Reserved     | Reserved bytes, initialized to zero upon formatting.                                                                                                                                                                                                        |
| `0x40`       | 64           | 1              | BS_DrvNum        | Legacy BIOS INT 13h drive number. `0x00` = floppy, `0x80` = fixed disk.                                                                                                                                                                                    |
| `0x41`       | 65           | 1              | BS_Reserved1     | Reserved flag utilized by Windows NT.                                                                                                                                                                                                                       |
| `0x42`       | 66           | 1              | BS_BootSig       | Extended boot record signature. Must be `0x28` or `0x29`.                                                                                                                                                                                                   |
| `0x43`       | 67           | 4              | BS_VolID         | Randomized 32-bit volume serial number, generated from formatting timestamp.                                                                                                                                                                                |
| `0x47`       | 71           | 11             | BS_VolLab        | Volume label string. Padded with ASCII spaces (`0x20`) if shorter than 11 characters.                                                                                                                                                                       |
| `0x52`       | 82           | 8              | BS_FilSysType    | System identifier: `"FAT32   "` (space-padded). **Drivers must never trust this string for FAT subtype determination.**                                                                                                                                    |

The Boot Sector structure is finalized with executable bootloader code padding up to offset `0x1FD`. The sector terminates at offset `0x1FE` with the mandatory bootable partition signature `0xAA55`. The backup copy at the sector specified by `BPB_BkBootSec` (usually sector 6) allows recovery if sector 0 is corrupted.

## The File System Information (FSInfo) Sector

A notable architectural limitation of FAT16 was the necessity to sequentially scan the entire FAT upon mounting to calculate free disk space -- an O(N) operation unacceptably slow at multi-gigabyte scale.

FAT32 introduces a caching mechanism via the FSInfo sector. Typically located at sector 1 within the Reserved Region, it provides a fast-path for free space queries and cluster allocation hints. The structure is protected by three independent 32-bit signatures.

| Offset (Hex) | Offset (Dec) | Field Description and Functional Purpose                                                                                                                                                                                                         |
| ------------ | ------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `0x00`       | 0            | **Lead Signature:** Must be exactly `0x41615252`.                                                                                                                                                                                                |
| `0x04`       | 4            | **Reserved Space:** 480 bytes, should never be utilized by the driver.                                                                                                                                                                           |
| `0x1E4`      | 484          | **Structural Signature:** Must be exactly `0x61417272`.                                                                                                                                                                                          |
| `0x1E8`      | 488          | **Free Cluster Count:** Last known count of unallocated clusters. `0xFFFFFFFF` = unknown (driver must recalculate by scanning FAT).                                                                                                              |
| `0x1EC`      | 492          | **Next Free Cluster Hint:** Where to begin searching for free clusters. `0xFFFFFFFF` = no hint (start from cluster 2).                                                                                                                          |
| `0x1F0`      | 496          | **Reserved Space:** 12 bytes of zeros.                                                                                                                                                                                                           |
| `0x1FC`      | 508          | **Trail Signature:** Must be exactly `0xAA550000`.                                                                                                                                                                                               |

An embedded systems developer or OS kernel engineer must recognize that these hints are not guaranteed to be synchronized after asynchronous power failure or kernel panic. A robust driver should range-check these values against the total cluster count before executing blind allocations.

## Mathematical Modeling of Volume Boundaries

Before a driver can read or write file payloads, it must compute the absolute physical boundaries of the FAT and Data regions.

### 1. Locating the File Allocation Table

The FAT array begins immediately following the reserved sectors:

```
FatStartSector = BPB_RsvdSecCnt
```

### 2. Total Size of the FAT Arrays

```
FatAreaSectors = BPB_FATSz32 × BPB_NumFATs
```

### 3. Locating the Data Region

Because `BPB_RootEntCnt` is zero in FAT32, the Data Region starts immediately after the FAT Region:

```
DataStartSector = FatStartSector + FatAreaSectors
```

### 4. Determining the Operable Cluster Count

```
DataSectors = BPB_TotSec32 - DataStartSector
CountOfClusters = ⌊DataSectors / BPB_SecPerClus⌋
```

If `CountOfClusters < 65,526`, the driver must reject the volume as invalid FAT32.

## File Allocation Table Mechanics and Cluster Chains

The functional heart of the FAT32 file system is the File Allocation Table itself. The FAT operates as a vast, sequential array of 32-bit (4-byte) integers. Each discrete array index logically corresponds to an equal-sized physical data cluster on the disk. The numerical value stored within that specific index defines the current operational status of the cluster and simultaneously operates as a singly linked list, pointing forward to the next cluster belonging to a file's fragmented chain.

### 28-Bit Addressing and Bitmasking Architecture

Although the FAT is fundamentally composed of 32-bit integer fields, FAT32 actually utilizes only the **lower 28 bits** for resolving cluster addresses. The highest 4 bits (bits 28–31) are strictly reserved.

When reading a FAT entry to determine the next cluster link, the driver must apply a bitwise AND mask:

```c
next_cluster = fat_entry & 0x0FFFFFFF;
```

When writing a new cluster link, the driver must **preserve the upper 4 bits**:

```c
fat_entry = (current_fat_value & 0xF0000000) | (new_cluster_address & 0x0FFFFFFF);
```

### FAT Entry Status Classification

The masked 28-bit value classifies the cluster:

- `0x00000000`: Free cluster, available for allocation.
- `0x00000001`: Internally reserved, not available for file data.
- `0x00000002` to `0x0FFFFFEF`: Allocated cluster. The value is the index of the next cluster in the chain.
- `0x0FFFFFF0` to `0x0FFFFFF6`: Reserved values. Must not be used as cluster pointers.
- `0x0FFFFFF7`: Bad cluster. Permanently quarantined -- physically damaged sectors.
- `0x0FFFFFF8` to `0x0FFFFFFF`: End of File (EOF) marker. Final cluster of the file's data chain.

### Special Reserved Entries: FAT[0] and FAT[1]

The zeroth and first entries do not map to physical data clusters (valid data clusters begin at index 2):

- **FAT[0]:** Duplicate of the media descriptor. The lowest 8 bits match `BPB_Media` (e.g., `0xF8`). Typical full value: `0x0FFFFFF8`.
- **FAT[1]:** Stores dirty flags. Bit 27 (`ClnShutBitMask`, `0x08000000`) = clean shutdown flag (1 = clean, 0 = dirty/improper dismount). Bit 26 (`HrdErrBitMask`, `0x04000000`) = hardware I/O error flag (1 = no errors, 0 = errors detected).

### Cluster-to-LBA Mathematical Resolution

Converting cluster number N to a physical sector on disk:

```
FirstSectorOfCluster = ((N - 2) × BPB_SecPerClus) + DataStartSector
```

For partitioned drives, add hidden sectors for absolute LBA:

```
AbsoluteLBA = BPB_HiddSec + FirstSectorOfCluster
```

Locating cluster N within the FAT array:

```
FATSectorOffset = ⌊(N × 4) / BPB_BytsPerSec⌋
FATByteOffset   = (N × 4) mod BPB_BytsPerSec
```

The physical sector to read: `FatStartSector + FATSectorOffset`. The 32-bit entry is at `FATByteOffset` within that sector buffer. For the second FAT copy: add `BPB_FATSz32` to the sector number.

## Directory Entries and File Metadata Architecture

Within the payload data clusters, directories are formatted as sequential arrays of uniformly sized 32-byte entries. A directory is conceptually identical to a standard file -- it occupies a cluster chain, but its payload is formatted as metadata records rather than raw data.

### Standard Short File Name (SFN) Directory Entry

The traditional 8.3 directory entry consumes exactly 32 bytes:

| Offset (Hex) | Length (Bytes) | Field Purpose   | Strict Specifications                                                                                                                                                                                                                                                                       |
| ------------ | -------------- | --------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `0x00`       | 11             | DIR_Name        | ASCII 8.3 filename. First 8 bytes = name (space-padded), next 3 = extension. First byte `0x00` = unallocated + end-of-directory marker. `0xE5` = deleted file.                                                                                                                              |
| `0x0B`       | 1              | DIR_Attr        | Attribute bitmask. `0x01`: Read-Only, `0x02`: Hidden, `0x04`: System, `0x08`: Volume Label, `0x10`: Directory, `0x20`: Archive. If attribute == `0x0F`, the entry is an LFN component.                                                                                                      |
| `0x0C`       | 1              | DIR_NTRes       | Reserved for Windows NT casing configurations.                                                                                                                                                                                                                                              |
| `0x0D`       | 1              | DIR_CrtTimeTenth| High-resolution creation time (10ms increments).                                                                                                                                                                                                                                            |
| `0x0E`       | 2              | DIR_CrtTime     | Creation time (FAT bitmask format).                                                                                                                                                                                                                                                         |
| `0x10`       | 2              | DIR_CrtDate     | Creation date (FAT bitmask format).                                                                                                                                                                                                                                                         |
| `0x12`       | 2              | DIR_LstAccDate  | Last access date.                                                                                                                                                                                                                                                                           |
| `0x14`       | 2              | DIR_FstClusHI   | **FAT32 Specific:** High 16 bits of starting cluster. Always `0x0000` on FAT16.                                                                                                                                                                                                             |
| `0x16`       | 2              | DIR_WrtTime     | Last modification time.                                                                                                                                                                                                                                                                     |
| `0x18`       | 2              | DIR_WrtDate     | Last modification date.                                                                                                                                                                                                                                                                     |
| `0x1A`       | 2              | DIR_FstClusLO   | Low 16 bits of starting cluster.                                                                                                                                                                                                                                                            |
| `0x1C`       | 4              | DIR_FileSize    | File size in bytes. **Must be 0 for directories.**                                                                                                                                                                                                                                          |

To access file data, concatenate the cluster fields:

```c
starting_cluster = (DIR_FstClusHI << 16) | DIR_FstClusLO;
```

### Timestamp Bitmask Architecture

FAT dates and times are compressed into 16-bit integers:

**FAT Date Structure (16-bit):**

- Bits 9–15 (7 bits): Year -- offset from 1980 (range 0–127, years 1980–2107)
- Bits 5–8 (4 bits): Month (1–12)
- Bits 0–4 (5 bits): Day (1–31)

**FAT Time Structure (16-bit):**

- Bits 11–15 (5 bits): Hour (0–23)
- Bits 5–10 (6 bits): Minute (0–59)
- Bits 0–4 (5 bits): Second ÷ 2 (values 0–29, representing 0–58 seconds in 2-second granularity)

Combined 32-bit FAT Date/Time encoding:

```c
fat_datetime = ((year - 1980) << 25) | (month << 21) | (day << 16) | (hour << 11) | (minute << 5) | (second >> 1);
```

> [!NOTE]
> The 2-second resolution means timestamps like `16:09:53` are truncated to `16:09:52`, creating up to 1994ms synchronization discrepancies when migrating files between NTFS and FAT32.

## Long File Names (LFN) and the Virtual FAT (VFAT) Extension

To support filenames up to 255 Unicode characters without breaking compatibility with legacy MS-DOS 8.3 utilities, Microsoft developed the Virtual FAT (VFAT) LFN extension. VFAT implements Long File Names by hiding them as a contiguous sequence of "invalid" 32-byte directory entries placed immediately **preceding** the valid SFN entry.

### The Attribute Hack and Backward Compatibility

For a 32-byte entry to be interpreted as an LFN string segment, the attribute byte at offset `0x0B` is hardcoded to `0x0F`.

In standard FAT logic, `0x0F` represents the contradictory combination of Volume Label + System + Hidden + Read-Only (`0x08 | 0x04 | 0x02 | 0x01`). Legacy operating systems gracefully ignore entries with this impossible combination, effectively cloaking the LFN components from older software.

### LFN Component Structure and Checksum Linking

Each 32-byte LFN entry stores exactly **13 UTF-16 (UCS-2) characters**, scattered across three separated arrays within the block. Entries are stacked in **reverse sequential order** -- the end of the filename appears first on disk.

| Offset (Hex) | Length (Bytes) | Field Name   | Description                                                                                                                    |
| ------------ | -------------- | ------------ | ------------------------------------------------------------------------------------------------------------------------------ |
| `0x00`       | 1              | Ordinal      | Sequence number (0x01, 0x02, etc.). Bit 6 (`0x40`) is set on the last segment. Bit 7 indicates deleted.                         |
| `0x01`       | 10             | Char Array 1 | 5 UCS-2 Unicode characters (characters 1–5 of the sequence).                                                                   |
| `0x0B`       | 1              | Attribute    | LFN signature: `0x0F`.                                                                                                         |
| `0x0C`       | 1              | Reserved     | Must be `0x00`.                                                                                                                |
| `0x0D`       | 1              | Checksum     | LFN checksum binding this chain to its associated SFN entry.                                                                    |
| `0x0E`       | 12             | Char Array 2 | 6 UCS-2 Unicode characters (characters 6–11).                                                                                   |
| `0x1A`       | 2              | Reserved     | Must be `0x0000`.                                                                                                              |
| `0x1C`       | 4              | Char Array 3 | 2 UCS-2 Unicode characters (characters 12–13).                                                                                  |

### LFN Checksum Algorithm

The checksum is computed by iterating over the 11-byte SFN filename (including space padding) using a bitwise right rotation:

```c
uint8_t lfn_checksum(const uint8_t *sfn_name) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + sfn_name[i];
    }
    return sum;
}
```

The OS must compute this checksum from the SFN and verify it against the checksum stored in each adjacent LFN entry. Mismatches indicate orphaned or corrupt LFN chains.

## Algorithmic Computation of FAT Size for Disk Formatting

When formatting raw storage into a FAT32 volume, the driver must calculate the exact number of sectors for the FAT (`FATSz32`). Because the FAT size determines the Data Region boundary, which determines the cluster count, which determines the required FAT size -- a circular dependency forms.

The Microsoft specification uses a reliable approximation algorithm to break this cycle. The result may overshoot by up to 8 sectors but guarantees sufficient capacity:

```
RootDirSectors = 0                              (always zero for FAT32)
TmpVal1 = DskSize - (BPB_RsvdSecCnt + RootDirSectors)
TmpVal2 = (256 × BPB_SecPerClus) + BPB_NumFATs
TmpVal2 = ⌊TmpVal2 / 2⌋                         (FAT32 adjustment)
FATSz   = ⌊(TmpVal1 + (TmpVal2 - 1)) / TmpVal2⌋
BPB_FATSz32 = FATSz
```

By executing this sizing algorithm in conjunction with rigorous alignment of the Boot Sector structures, robust endianness translations, and mathematically flawless 28-bit cluster chain traversal operations, a custom operating system can achieve deterministic storage performance leveraging the enduring ubiquity of the FAT32 architectural specification.
