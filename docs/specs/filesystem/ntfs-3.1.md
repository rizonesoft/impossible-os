# Advanced NTFS Specification and Implementation Guide for Custom Operating Systems

---

## 1. Architectural Philosophy and File System Overview

The New Technology File System (NTFS) is a highly robust, proprietary, journaling file system initially developed by Microsoft in the early 1990s, making its first appearance with Windows NT 3.1 in July 1993. For an operating system developer tasked with implementing a custom file system driver, NTFS presents a fundamental paradigm shift from simpler, legacy structures such as the File Allocation Table (FAT) family or the Second Extended Filesystem (ext2). The defining architectural philosophy of NTFS is encapsulated in a single, overarching principle: **absolutely everything on the volume is a file**. Unlike legacy file systems that utilize rigid, specialized disk structures for root directories, boot sectors, or bad block maps, NTFS abstracts every component into discrete files managed by a central relational database known as the **Master File Table (MFT)**. This means that the volume metadata, the root directory, the cluster allocation bitmap, and even the boot sector itself are treated as standard file objects with their own metadata records.

This abstraction provides extraordinary scalability and flexibility. The file system uses 64-bit addressing for its logical cluster numbers (LCNs), allowing for theoretical maximum volume sizes that far exceed modern storage capacities. Depending on the cluster size and the specific implementation version of the operating system formatting the drive, the maximum volume size can range from 256 Terabytes (with 64 KB clusters in Windows 10 version 1703 and Windows Server 2016 or earlier) up to a staggering 8 Petabytes (with 2 MB clusters in Windows 10 version 1709 and Windows Server 2019 or later). Individual files are permitted to grow up to 16 Exabytes minus 1 KB natively, though practical implementations limit this to 16 Terabytes to 8 Petabytes depending on the operating system version. Furthermore, filenames are natively encoded in UTF-16, supporting both case-insensitive and case-sensitive namespaces (such as POSIX, Win32, and DOS environments) with a maximum filename length of 255 code units.

To build a functional NTFS driver, the implementation must be meticulously phased and integrated into the operating system's Virtual File System (VFS) layer. Developers often look to established open-source projects, such as the GPLv2-licensed NTFS-3G project (a FUSE-based user-space implementation) or the native ntfs3 kernel driver introduced in Linux kernel version 5.15, for structural reference and algorithmic validation. The custom driver must first locate and parse the BIOS Parameter Block (BPB) within the boot sector to understand the underlying volume geometry. Next, it must calculate the physical disk location of the Master File Table. Once the MFT is located, the driver must parse the complex internal structure of individual MFT records, handling integrity checks via update sequence arrays (fixups), parsing dynamic attribute headers, and executing complex decoding algorithms for non-resident data runs. Finally, directory traversal requires a deep understanding of complex B+ tree data structures used for indexing.

---

## 2. The Partition Boot Sector and BIOS Parameter Block (BPB)

The absolute first step for any NTFS file system driver is to mount the volume and determine its fundamental geometric properties. Regardless of whether the host system utilizes a legacy Master Boot Record (MBR) partitioning scheme or a modern GUID Partition Table (GPT), the first sector of the NTFS partition—designated as Logical Sector 0—contains the **Volume Boot Record (VBR)** and the **BIOS Parameter Block (BPB)**. Even on modern Unified Extensible Firmware Interface (UEFI) systems where legacy BIOS boot code is entirely unnecessary for system initialization, this first sector is strictly reserved and formatted by the NTFS format utility to maintain the file system's metadata anchors and backward compatibility. Reserving this space ensures that the file system can operate within logical partitions and provides the necessary offsets for the file system driver to orient itself.

The Boot Sector is exactly 512 bytes in length (or the size of one physical disk sector) and contains the foundational constants required to calculate all subsequent physical and logical disk offsets.

### 2.1 BPB Layout and Critical Offsets

The implementation must read the first sector into a memory buffer and parse the following byte offsets with extreme precision. It is critical to note that all multi-byte values within the NTFS structures are stored in **little-endian format**, requiring appropriate byte-swapping macros on big-endian hardware architectures.

| Offset (Hex) | Length | Field Name | Description and Kernel Usage |
|---|---|---|---|
| `0x00` | 3 Bytes | Jump Instruction | Typically `EB 52 90` or similar. Used by legacy BIOS systems to jump over the BPB to the executable boot code. Ignored by modern OS drivers. |
| `0x03` | 8 Bytes | OEM ID | Identifies the formatting system, usually containing the string `"NTFS    "` (padded with spaces). |
| `0x0B` | 2 Bytes | Bytes Per Sector | The fundamental unit of disk geometry. Historically 512, but often 4096 on Advanced Format drives. |
| `0x0D` | 1 Byte | Sectors Per Cluster | Multiplier to determine the cluster size. For example, 8 sectors per cluster on a 512-byte sector drive equals a 4096-byte (4 KB) cluster. |
| `0x0E` | 2 Bytes | Reserved Sectors | Always set to `0x0000` in NTFS. In legacy FAT systems, this indicated reserved space, but in NTFS, the bootstrap code uses this as a temporary variable. |
| `0x10` | 3 Bytes | Always Zero | Unused by the NTFS file system specification. |
| `0x13` | 2 Bytes | Not Used | Historically used in FAT, but entirely unused by NTFS. |
| `0x15` | 1 Byte | Media Descriptor | A hex value indicating the media type. `0xF8` strictly indicates a non-removable hard disk. |
| `0x16` | 2 Bytes | Always Zero | Unused by the NTFS file system specification. |
| `0x18` | 2 Bytes | Sectors Per Track | Used for legacy Cylinder-Head-Sector (CHS) disk geometry calculations. |
| `0x1A` | 2 Bytes | Number Of Heads | Used for legacy Cylinder-Head-Sector (CHS) disk geometry calculations. |
| `0x1C` | 4 Bytes | Hidden Sectors | The Logical Block Addressing (LBA) offset of the partition start relative to the physical disk. |
| `0x20` | 4 Bytes | Not Used | Unused by the NTFS file system specification. |
| `0x24` | 4 Bytes | Not Used | Unused by the NTFS file system specification. |
| `0x28` | 8 Bytes | Total Sectors | A 64-bit integer representing the total size of the volume in sectors. |
| `0x30` | 8 Bytes | LCN of `$MFT` | **CRITICAL:** The Logical Cluster Number denoting the absolute starting position of the Master File Table. |
| `0x38` | 8 Bytes | LCN of `$MFTMirr` | The Logical Cluster Number denoting the position of the backup Master File Table Mirror. |
| `0x40` | 4 Bytes | Clusters Per FRS | Defines the size of a File Record Segment (MFT Record). |
| `0x44` | 4 Bytes | Clusters Per Index | Defines the size of an INDX block used in directory B+ trees. |
| `0x48` | 8 Bytes | Volume Serial | A unique 64-bit identifier assigned to the volume during formatting. |
| `0x50` | 4 Bytes | Checksum | A simple additive checksum of all the 32-bit values that precede the checksum field itself. |

The remainder of the 512-byte sector contains bootstrap code (which may display messages such as "A disk read error occurred" or "NTLDR is missing" if the system attempts to boot an unbootable volume) and concludes with the standard boot signature `0x55 0xAA` at offset `0x1FE`.

### 2.2 Algorithmic Calculation of the MFT Location

The primary objective of parsing the BIOS Parameter Block is to locate the `$MFT` file on the physical disk. The OS driver must perform the following absolute physical byte offset calculations using the variables extracted from the BPB:

1. **Determine Cluster Size in Bytes:** The driver must multiply the `Bytes Per Sector` (extracted from offset `0x0B`) by the `Sectors Per Cluster` (extracted from offset `0x0D`). For example, a standard configuration yields `512 * 8 = 4096` bytes per cluster.

2. **Determine the Exact MFT Byte Offset:** The driver must multiply the Logical Cluster Number (LCN) of `$MFT` (extracted from offset `0x30` as a 64-bit integer) by the calculated Cluster Size. If the `$MFT` is located at LCN 175,376, the byte offset from the start of the partition is `175,376 * 4096 = 718,340,096` bytes. This is the exact location where the file system must issue its first read command.

3. **Calculate Record Sizes:** The driver must read the `Clusters Per FRS` field at offset `0x40`. The interpretation of this field involves a specific NTFS quirk:
   - If this value is **positive**, it represents the literal number of clusters per record.
   - If this value is **negative** (which is highly common in modern implementations), it represents the size in bytes as a shift value. Specifically, the size is calculated as `2^(absolute value)`.
   - For example, an unsigned byte value of `0xF6` is equivalent to the signed integer `-10` in two's complement. This indicates a File Record Segment size of `2^10 = 1024` bytes. This 1024-byte dimension is the standard size for almost all modern NTFS implementations, regardless of the underlying cluster size.

With the absolute byte offset and the exact record size calculated, the operating system driver can issue a low-level I/O read request to the disk controller to load the first record of the Master File Table into kernel memory for parsing.

---

## 3. The Master File Table (MFT) Architecture and System Files

The Master File Table is the undisputed heart of the NTFS file system. It is structured as an array of **File Record Segments (FRS)**, which, as calculated above, are conventionally 1024 bytes each. There is exactly one entry in the MFT for every single file and directory that exists on the volume. If a file is extremely small, its entire data payload may fit within the 1024-byte record itself; this is known as a **"resident" file**. If the file's data payload is larger than the available space within the record, the record instead contains pointers—known as **Data Runs**—to the physical clusters on the disk where the actual data resides; this is known as a **"non-resident" file**.

As new files are created, the NTFS file system appends new entries to the MFT, causing the table to grow in size. Conversely, when files are deleted, their corresponding MFT entries are flagged as free and made available for immediate reuse. However, it is a crucial characteristic of NTFS that the disk space allocated for the MFT itself is never truncated or reallocated; **the physical size of the MFT never decreases**, even if millions of files are deleted.

### 3.1 Reserved System Files (Inodes 0 to 15)

The first 16 records of the MFT (Inodes 0 through 15) are strictly reserved by the NTFS architecture for its own metadata. These are commonly referred to as **System Files**. An operating system driver must be explicitly aware of these files, as they govern the structural rules, allocation maps, and integrity mechanisms of the entire file system. These files are hidden from normal user-space view.

| Inode / Record | File Name | Description and OS Driver Responsibility |
|---|---|---|
| 0 | `$MFT` | The Master File Table itself. Its data runs describe the exact physical disk locations of the entire MFT array. It is self-referential. |
| 1 | `$MFTMirr` | A partial backup duplicate of the MFT (typically mirroring only the first four records) designed to guarantee access to the core volume metadata in the event of a localized single-sector failure on the physical disk. |
| 2 | `$LogFile` | The transaction journal file. It contains a circular log of transactional steps used to restore consistency to the file system after a sudden system failure or power loss. |
| 3 | `$Volume` | Contains volume-level information, including the volume serial number, the volume creation time, the NTFS version, and a dirty flag indicating if the volume was cleanly unmounted. |
| 4 | `$AttrDef` | The Attribute Definition Table. This file maps numeric attribute IDs (e.g., `0x10`) to their textual names (e.g., `$STANDARD_INFORMATION`) and defines their minimum and maximum size constraints. |
| 5 | `.` (Root Directory) | The root directory index of the volume. All hierarchical path resolution begins by traversing from this specific node. |
| 6 | `$Bitmap` | The master volume allocation bitmap. Each individual bit represents one cluster on the physical disk; a bit value of `1` indicates the cluster is allocated, and a `0` indicates the cluster is free space. |
| 7 | `$Boot` | Contains the Volume Boot Sector and the BPB. Mapping the boot sector as a file ensures that this critical sector is tracked as an allocated cluster and is not accidentally overwritten. |
| 8 | `$BadClus` | Contains a `$DATA` attribute named `$BAD` that maps all known defective or physically damaged clusters on the disk to prevent the file system from attempting to allocate them to user files. |
| 9 | `$Secure` | Stores unique security descriptors, Access Control Lists (ACLs), and Security Identifiers (SIDs) in a centralized location to prevent massive redundant storage of permission data across millions of individual files. |
| 10 | `$UpCase` | A massive mapping table used by the file system to convert lowercase Unicode characters to uppercase. This is required to enforce case-insensitive collation rules during directory index sorting and filename comparisons. |
| 11 | `$Extend` | A system directory containing optional file system extensions, such as quota management files, object IDs, and reparse points. |

Records 12 through 15 are marked as in-use during formatting but remain empty, serving as a protective buffer. Standard user files and directories begin their allocation at **Inode 16**.

### 3.2 The MFT Zone and Fragmentation Mechanics

To prevent the Master File Table from becoming severely fragmented as it grows, the file system proactively reserves a contiguous block of physical disk space specifically for MFT expansion. This reserved area is known as the **MFT Zone**. Standard user files and directories are strictly prohibited from allocating clusters inside the MFT Zone unless all of the volume's unreserved free space has been completely exhausted.

The size of the MFT Zone is dynamically calculated depending on average file sizes; volumes formatted with a large number of very small files will consume the MFT Zone rapidly, while volumes with massive files will fill the unreserved space first. When designing the write and allocation logic for a custom OS driver, the driver must parse the `$BITMAP` attribute of the `$MFT` file to determine which records are currently free, while simultaneously querying the global volume `$Bitmap` (Inode 6) to ensure new user files do not encroach upon the physical clusters reserved for the MFT Zone.

---

## 4. MFT Record Segment Structure

Every 1024-byte File Record Segment (FRS) in the Master File Table follows a rigid, highly structured layout. The record begins with a standard header, followed immediately by an Update Sequence Array (USA) designed for hardware error detection, and then a contiguous sequence of variable-length Attributes that contain the actual metadata and data.

### 4.1 Record Header Layout

Upon reading a 1024-byte sector into memory, the OS driver must cast the raw byte array into a structured header defined by the following precise offsets:

| Offset | Size | Data Type | Description and Driver Handling |
|---|---|---|---|
| `0x00` | 4 Bytes | Magic Number | Typically the ASCII string `"FILE"`. If the file system detects a defective record, it may mark it as `"BAAD"`. Index nodes in directories read `"INDX"`, and log file records read `"RSTR"` or `"RCRD"`. The driver must abort parsing if the magic number is unrecognized. |
| `0x04` | 2 Bytes | `uint16` | The byte offset from the start of the record to the Update Sequence Array (Fixup Array). |
| `0x06` | 2 Bytes | `uint16` | The size of the Update Sequence Array measured in 16-bit words (Fixup Number). |
| `0x08` | 8 Bytes | `uint64` | The `$LogFile` Sequence Number (LSN). This is utilized heavily during volume mount for journaling and crash recovery. |
| `0x10` | 2 Bytes | `uint16` | The Sequence Number of this specific record. This value is incremented by one every time the record is deleted and subsequently reallocated to a new file. This prevents stale file references from pointing to newly allocated data. |
| `0x12` | 2 Bytes | `uint16` | The Hard Link Count. This represents the number of directories that reference this MFT record. It is only utilized in base records. |
| `0x14` | 2 Bytes | `uint16` | The exact byte offset to the first Attribute in the record (usually `0x30` or `0x38`). |
| `0x16` | 2 Bytes | Bitfield | Record Flags. The low-order bit (`0x0001`) indicates the record is in use (a live file). The next bit (`0x0002`) indicates the record is a directory index. A value of `0x0000` indicates a deleted file, allowing forensic recovery. |
| `0x18` | 4 Bytes | `uint32` | The Real (used) size of the MFT record in bytes, indicating where the attribute sequence terminates. |
| `0x1C` | 4 Bytes | `uint32` | The Allocated size of the MFT record. This is almost universally `0x0400` (1024 bytes). |
| `0x20` | 8 Bytes | `uint64` | File reference to the Base FILE Record. If this value is 0, the current record is the base record. If this record is merely an extension holding overflowing attributes, this field contains the address of the base record. |
| `0x28` | 2 Bytes | `uint16` | The Next Attribute ID to be assigned if a new attribute is created within this record. |
| `0x2A` | 2 Bytes | `uint16` | The High part of the MFT record or Padding. |

---

## 5. System Integrity: The Update Sequence Array (Fixup Mechanism)

A profound engineering challenge in file system design is handling incomplete sector writes caused by sudden power failures or system crashes during disk I/O. Because standard disk sectors are 512 bytes, but an MFT record spans 1024 bytes (two physical sectors), a sudden crash could result in the first 512-byte sector being successfully written to the platter while the second sector is not. This phenomenon is known as **"sector tearing."**

To detect and mitigate this, NTFS employs an **Update Sequence Array (USA)**, commonly referred to as the "Fixup" mechanism. The OS driver must implement this fixup logic before it can trust the contents of the MFT record. Attempting to parse attributes without verifying and applying the fixup array will inevitably result in corrupted data extraction.

### 5.1 Fixup Verification and Restoration Algorithm

When the OS driver reads an MFT record (or an INDX directory buffer) from the disk into memory, it must execute the following algorithmic steps:

1. **Validate Magic Number:** Check the first 4 bytes at `0x00`. Ensure it matches expected signatures like `"FILE"` or `"INDX"`.

2. **Locate the USA:** Read the USA Offset at `0x04` and the USA Size in words at `0x06`.

3. **Extract the Update Sequence Number (USN):** Jump to the offset specified by the USA Offset. The first 2 bytes at this location form the Update Sequence Number (USN).

4. **Identify Original Data:** The remaining words in the Update Sequence Array are the original bytes that naturally belong at the very end of each physical sector that makes up the record.

5. **Perform Integrity Check:** For a standard 1024-byte record, check the last two bytes of the first sector (offset `0x1FE`) and the last two bytes of the second sector (offset `0x3FE`).

6. **Verify Against USN:** Compare the values found at `0x1FE` and `0x3FE` against the USN extracted in Step 3. If they do not match perfectly, a sector tear has occurred, the record is corrupt, and the driver should ideally flag an error or trigger a volume check (`chkdsk`).

7. **Restore Original Bytes:** If the values match, the OS driver must copy the original bytes stored in the remainder of the Update Sequence Array back over the USN placeholders at offsets `0x1FE` and `0x3FE`.

The record is now mathematically clean, reconstructed in memory, and ready for attribute parsing. When writing data back to the disk, the driver must meticulously reverse this process: it must increment the USN by one (skipping `0x0000`), copy the actual data present at `0x1FE` and `0x3FE` into the USA buffer, overwrite the data at `0x1FE` and `0x3FE` with the new USN, and finally issue the write command to the hardware controller.

---

## 6. Attribute Architecture and Headers

Immediately following the MFT Record Header and the Update Sequence Array, the memory space is populated by **Attributes**. In NTFS, an attribute is any discrete unit of information associated with a file. The file's timestamps, its permissions, its filename, and its actual raw data payload are all classified simply as attributes.

Attributes are identified by a 32-bit integer type code. An OS driver parsing an MFT record will continuously read attributes sequentially until it encounters an attribute ID of `0xFFFFFFFF`, which serves as the `$END` marker for the record.

| Type ID | Textual Name | Core Functionality and Description |
|---|---|---|
| `0x10` | `$STANDARD_INFORMATION` | Contains primary timestamps (MAC times), traditional DOS permissions, and quota tracking metrics. |
| `0x20` | `$ATTRIBUTE_LIST` | Utilized only when attributes overflow a single MFT record. It contains pointers linking to extension MFT records. |
| `0x30` | `$FILE_NAME` | Contains the UTF-16 encoded filename, the parent directory reference, namespace flags, and duplicated timestamps. |
| `0x40` | `$OBJECT_ID` | Stores a unique GUID assigned to the file, used primarily for distributed link tracking across domains. |
| `0x50` | `$SECURITY_DESCRIPTOR` | Stores Access Control Lists (ACLs) and Security Identifiers (SIDs) governing user access rights. |
| `0x60` | `$VOLUME_NAME` | Contains the human-readable label of the volume, used exclusively in the `$Volume` system file. |
| `0x70` | `$VOLUME_INFORMATION` | Contains the volume version and state flags. |
| `0x80` | `$DATA` | Contains the actual file contents. A file can possess multiple named data streams (Alternate Data Streams). |
| `0x90` | `$INDEX_ROOT` | The resident root node of a B+ tree, utilized extensively to implement directory structures. |
| `0xA0` | `$INDEX_ALLOCATION` | Non-resident data runs pointing to physical cluster blocks that hold massive tree sub-nodes (INDX buffers). |
| `0xB0` | `$BITMAP` | A bitmap array illustrating which index records or clusters within an allocation are currently in use. |
| `0xC0` | `$REPARSE_POINT` | Used to implement symbolic links, directory junction points, and volume mount points. |
| `0x100` | `$LOGGED_UTILITY_STREAM` | Used heavily by the Encrypting File System (EFS) to manage encrypted data streams. |

### 6.1 The Principle of Attribute Residency Constraints

A critical concept for an OS driver developer is understanding **Residency**. Because the MFT record is strictly limited to 1024 bytes, not all data can fit within it.

- **Resident Attributes:** The entire content and payload of the attribute fits perfectly within the physical 1024-byte MFT record.
- **Non-Resident Attributes:** The content is too large. The attribute header inside the MFT record instead contains a mapping structure—known as **Data Runs**—that points to standard physical disk clusters located elsewhere on the volume where the actual payload is stored.

Certain attributes, such as `$STANDARD_INFORMATION` and `$INDEX_ROOT`, are strictly required by the NTFS specification to be resident at all times. Conversely, attributes like `$INDEX_ALLOCATION` are inherently designed to be strictly non-resident. The `$DATA` attribute is flexible and can be either resident or non-resident depending on the file size.

### 6.2 Attribute Header Layout Structure

Every attribute, regardless of its type, begins with a standard attribute header. The OS driver must read the first 16 bytes of this header to determine the attribute's type, its total size, and its residency status.

**Common Header (Applicable to both Resident and Non-Resident):**

| Offset | Size | Description and Function |
|---|---|---|
| `0x00` | 4 Bytes | Attribute Type ID (e.g., `0x10` for standard info, `0x80` for data). |
| `0x04` | 4 Bytes | The Total Length of this attribute, including this header. Used to jump to the next attribute. |
| `0x08` | 1 Byte | Non-Resident Flag: A value of `0x00` indicates Resident, while `0x01` indicates Non-Resident. |
| `0x09` | 1 Byte | Name length (N) in characters. If this is 0, the attribute is unnamed (which is standard for the primary `$DATA` stream). |
| `0x0A` | 2 Bytes | Offset to the Name (typically `0x18` if a name is present). |
| `0x0C` | 2 Bytes | Attribute Flags (e.g., compressed, encrypted, sparse file). |
| `0x0E` | 2 Bytes | Attribute ID, a unique sequence number for this specific attribute within the record. |

If the driver determines the attribute is **Resident** (Non-Resident Flag at `0x08 == 0x00`), the following structural layout applies immediately after offset `0x0F`:

| Offset | Size | Description and Function |
|---|---|---|
| `0x10` | 4 Bytes | Length of the actual Attribute Content Payload (L). |
| `0x14` | 2 Bytes | Byte Offset to the beginning of the Attribute Content Payload. |
| `0x16` | 1 Byte | Indexed flag. |
| `0x17` | 1 Byte | Padding for byte alignment. |

The OS driver will utilize the offset value found at `0x14` to jump directly to the raw payload of the resident attribute.

---

## 7. Core Metadata Attributes

To provide basic file read access and directory traversal, the custom OS driver must feature robust decoders for the `$STANDARD_INFORMATION`, `$FILE_NAME`, `$ATTRIBUTE_LIST`, and `$DATA` attributes.

### 7.1 `$STANDARD_INFORMATION` (0x10)

This attribute is always resident and contains the core metadata governing file permissions, timestamps, and ownership information. NTFS timestamps (often referred to in digital forensics as MAC times) are 64-bit values representing the number of **100-nanosecond intervals** that have elapsed since **January 1, 1601**.

| Offset | Size | Description and Operational Metrics |
|---|---|---|
| `0x00` | 8 Bytes | **C Time** - File Creation Time. |
| `0x08` | 8 Bytes | **A Time** - File Altered Time. Refers to when the file content was last modified. |
| `0x10` | 8 Bytes | **M Time** - MFT Changed Time. Refers to when the metadata within the MFT record itself was last updated. |
| `0x18` | 8 Bytes | **R Time** - File Read. Refers to the last access time (often disabled in modern Windows to improve performance). |
| `0x20` | 4 Bytes | DOS File Permissions. |
| `0x24` | 4 Bytes | Maximum Number of Versions. |
| `0x28` | 4 Bytes | Version Number. |
| `0x2C` | 4 Bytes | Class ID. |
| `0x30` | 4 Bytes | Owner ID (Introduced in Windows 2000). |
| `0x34` | 4 Bytes | Security ID (Introduced in Windows 2000). |
| `0x38` | 8 Bytes | Quota Charged (Introduced in Windows 2000). |
| `0x40` | 8 Bytes | Update Sequence Number (USN) for the volume journal. |

The DOS File Permissions flag at offset `0x20` is a bitmask determining basic access rights. The OS driver should recognize `0x0001` (Read-Only), `0x0002` (Hidden), `0x0004` (System), `0x0020` (Archive), `0x0080` (Normal), `0x0800` (Compressed), and `0x4000` (Encrypted).

### 7.2 `$FILE_NAME` (0x30)

This attribute is strictly resident and stores the human-readable name of the file alongside its parent hierarchy. A fascinating and unique insight into NTFS architectural design is that the MAC timestamps and file sizes are deliberately **duplicated** within the `$FILE_NAME` attribute, mirroring the data found in `$STANDARD_INFORMATION` and `$DATA`.

This duplication is a profound optimization for directory listings. When an operating system lists the contents of a directory (via an `ls` or `dir` command), it scans the directory's B-tree index, which contains copies of these `$FILE_NAME` attributes. By embedding the file sizes and timestamps directly into the filename attribute, the operating system can instantly display the file metadata to the user without having to perform costly disk I/O operations to open the individual MFT record for every single file in that directory. However, this means the OS driver must ensure it updates both attributes when a file is modified to prevent forensic discrepancies.

| Offset | Size | Description and Operational Metrics |
|---|---|---|
| `0x00` | 8 Bytes | File Reference to the Parent Directory. The first 6 bytes represent the MFT Entry (Inode) of the parent folder, and the remaining 2 bytes represent the sequence number. |
| `0x08` | 8 Bytes | C Time (Duplicated Creation Time). |
| `0x10` | 8 Bytes | A Time (Duplicated Altered Time). |
| `0x18` | 8 Bytes | M Time (Duplicated MFT Changed Time). |
| `0x20` | 8 Bytes | R Time (Duplicated Read Time). |
| `0x28` | 8 Bytes | Allocated Size of the File (multiple of cluster size). |
| `0x30` | 8 Bytes | Real (Used) Size of the File (actual byte count). |
| `0x38` | 4 Bytes | Flags (Directory, Compressed, Hidden, etc., matching the standard info flags). |
| `0x3C` | 4 Bytes | Used by Extended Attributes (EA) and Reparse points. |
| `0x40` | 1 Byte | Length of the filename in Unicode characters (L). Maximum value is `0xFF` (255). |
| `0x41` | 1 Byte | Filename Namespace. |
| `0x42` | 2*L B | The Filename encoded in UTF-16. Notably, this string is **not null-terminated**. |

**Filename Namespaces:** The single byte at offset `0x41` dictates the parsing rules and allowable character set:

- `0x00` — **POSIX** namespace (case-sensitive, maximum 255 characters, allowing any character except NULL and `/`).
- `0x01` — **Win32** namespace (case-insensitive, restricting characters like `*`, `?`, `<`, `>`).
- `0x02` — **DOS** namespace (strictly enforcing the legacy 8.3 short filename format).
- `0x03` — **Win32/DOS** namespace, indicating that the filename is naturally compliant with both and does not require a secondary short name to be generated.

Furthermore, hard links in NTFS are elegantly implemented simply by assigning **multiple `$FILE_NAME` attributes** to a single MFT record, with each attribute potentially pointing to a different parent directory reference.

### 7.3 `$ATTRIBUTE_LIST` (0x20)

When a file experiences heavy disk fragmentation, its non-resident `$DATA` attribute requires an increasingly extensive list of data runs. If these runs exceed the ~1024-byte spatial limit of the base MFT record, NTFS dynamically allocates a new, separate "extension" MFT record from the MFT Zone. To logically link these disparate records together, a `$ATTRIBUTE_LIST` attribute is instantiated in the base record.

This list contains an array of entries that provide the OS driver with the MFT Reference (Inode) identifying exactly where specific attributes have been moved. It is also utilized when a file possesses an excessive number of hard links or named alternate data streams.

The structural layout of each entry within the attribute list is as follows:

| Offset | Size | Description |
|---|---|---|
| `0x00` | 4 Bytes | The Attribute Type ID (e.g., `0x80` for `$DATA`). |
| `0x04` | 2 Bytes | The total length of this list entry. |
| `0x06` | 1 Byte | Name length (N). |
| `0x07` | 1 Byte | Offset to the name. |
| `0x08` | 8 Bytes | Starting VCN (Virtual Cluster Number). This is critical if a highly fragmented `$DATA` attribute is split linearly across multiple extension MFT records. |
| `0x10` | 8 Bytes | The Base File Reference (Inode) of the specific MFT extension record currently holding the attribute. |

---

## 8. Data Retrieval: Non-Resident Data Runs and Run-Lists

When the `$DATA` attribute is marked as Non-Resident (Non-Resident Flag at `0x08 == 0x01`), the attribute header is immediately followed by an array of **Data Runs** (also referred to as Run-lists). The implementation of a highly robust, mathematically precise Data Run decoder is absolutely mandatory for any OS driver, as this mechanism dictates precisely how the physical clusters of a file are reassembled from the disparate sectors of the hard disk.

A Data Run translates **Virtual Cluster Numbers (VCNs)**—the logical, sequential layout of data inside the file as the user perceives it—into **Logical Cluster Numbers (LCNs)**—the actual, scattered physical clusters on the disk platter. To conserve precious bytes within the MFT record, NTFS utilizes a tightly packed, variable-length, relative-offset encoding structure.

### 8.1 Data Run Decoding Algorithm

The run-list consists of sequential elements. Each element begins with a single **Header Byte**. To decode the list, the OS driver must execute the following logic:

1. **Parse the Header Byte:** The driver must execute bitwise operations to split the header byte into two distinct nibbles.
   - **Low Nibble (L):** Represents the size, in bytes, of the subsequent Length field.
   - **High Nibble (F):** Represents the size, in bytes, of the subsequent Offset field.

2. **Extract the Run Length:** The driver must read the next L bytes from the stream. This value is an unsigned integer representing the number of contiguous physical clusters contained in this specific run.

3. **Extract the Relative Offset:** The driver must read the next F bytes. This is a **signed integer** representing the starting cluster offset.

> [!IMPORTANT]
> **Crucial Decoding Detail:** This offset is explicitly **relative** to the starting LCN of the
> previous run in the sequence. For the very first run in the list, the offset is relative
> to LCN 0 (the absolute beginning of the volume). Because this integer is signed, a
> negative offset indicates physical disk fragmentation where the next piece of file data
> is located closer to the start of the disk than the previous piece.

4. **Calculate the Absolute LCN:** The driver determines the physical disk location via the formula:
   ```
   Current Absolute LCN = Previous Absolute LCN + Relative Offset
   ```

5. **Termination Condition:** The driver continues decoding sequential runs until a header byte of `0x00` is encountered, which signals the definitive end of the run-list.

### 8.2 Handling Sparse Files and Compressed Units

The OS driver must be programmed to handle highly specific edge cases where the **F value** (the offset size) in the header byte equals 0. If F=0, the offset field is entirely omitted from the byte stream. This explicitly defines a **Sparse Run**—a block of Virtual Clusters that are entirely filled with zeros and occupy absolutely no physical space on the hard disk. When the driver encounters a sparse run, it must not attempt disk I/O; instead, it should simply return a memory buffer populated with zeros for the length of the run.

Similarly, for files where the transparent compression flag is set (typically utilizing the LZ77 algorithm introduced in Windows NT 3.51), the data is divided into logical **compression units** consisting of exactly 16 clusters. If a chunk of data successfully compresses down to N clusters (where N is less than 16), the run-list will first contain a standard run of length N pointing to the physically compressed clusters on the disk. This is immediately followed by a sparse run (F=0) with a length of `16 - N` to mathematically pad the unit back to the required 16 clusters. The custom OS driver's decompression logic must identify this highly specific **N followed by 16-N sparse** pattern to buffer and properly decompress the LZ77 payload.

---

## 9. Directory Structure and B+ Tree Indexing

In early iterations of the FAT file system, or in basic Unix file systems, a directory is essentially a simple, linear flat file containing an unsorted list of filenames. NTFS comprehensively rejects this inefficient model in favor of highly optimized **B+ trees**. While significantly more complex to implement at the driver level, B+ trees allow the file system to perform search, insertion, and deletion of file entries in logarithmic time (`O(log n)`)—a critical architectural advantage that prevents massive performance degradation in directories containing hundreds of thousands of files.

From the perspective of NTFS, a directory is a specific kind of file that serves as an index of file names, but crucially, it contains **no `$DATA` attribute**. Instead, it utilizes three distinct attributes to build and manage the B+ tree:

- **`$INDEX_ROOT` (0x90):** This attribute is always resident within the MFT record. It contains the primary root node of the B+ tree.
- **`$INDEX_ALLOCATION` (0xA0):** This attribute is always non-resident. It consists of a standard Data Run list that points to logical physical cluster blocks (known as INDX records or buffers) which contain the child sub-nodes of the tree.
- **`$BITMAP` (0xB0):** A bitfield attribute that actively tracks which Virtual Cluster Numbers (VCNs) within the index allocation are currently active and in use.

For directories, these indexing attributes are universally named `$I30`. It is worth noting that B-tree indexing is not limited solely to directories; NTFS uses similar index structures (`$SDH`, `$SII`, `$O`, `$Q`, `$R`) within system files like `$Secure`, `$ObjId`, and `$Quota` to manage security descriptors and quotas.

---

## 10. Index Records and Algorithmic Directory Traversal

When a directory grows beyond the handful of files that can fit perfectly within the resident `$INDEX_ROOT` attribute, the `$INDEX_ALLOCATION` data runs point to external 4 KB clusters formatted specifically as **INDX Records**. The OS driver must read these clusters from the disk platter, strictly apply the Update Sequence Array (Fixup) verification just as it does for standard MFT records, and then parse the node header.

### 10.1 INDX Record Header Layout

The INDX buffer operates as an independent node within the B+ tree.

| Offset | Size | Data Type | Description and Function |
|---|---|---|---|
| `0x00` | 4 Bytes | Magic Number | Must be `"INDX"`. |
| `0x04` | 2 Bytes | `uint16` | Offset to the Update Sequence Array. |
| `0x06` | 2 Bytes | `uint16` | Size of the Update Sequence Array in words. |
| `0x08` | 8 Bytes | `uint64` | The Log Sequence Number (LSN) for journaling. |
| `0x10` | 8 Bytes | `uint64` | The Virtual Cluster Number (VCN) of this specific INDX buffer within the tree hierarchy. |
| `0x18` | 4 Bytes | `uint32` | The exact offset to the sequence of Index Entries (relative to `0x18`). |
| `0x1C` | 4 Bytes | `uint32` | The exact size of the Index Entries. |
| `0x20` | 4 Bytes | `uint32` | The allocated size of the Index Entries. |
| `0x24` | 1 Byte | Flag | A critical flag. If `0x01` is set, this record is not a leaf node and has further children (sub-nodes) located deeper in the `$INDEX_ALLOCATION`. |
| `0x25` | 3 Bytes | Padding | Always zero. |
| `0x28` | 2 Bytes | `uint16` | The Update Sequence Number (USN) used for the fixup integrity check. |

### 10.2 Index Entry Structure and Target Resolution

Immediately following the INDX header is a contiguous, sorted list of **Index Entries**. Within each node, these entries are strictly sorted in increasing alphabetical order (utilizing the `$UpCase` system file to guarantee case-insensitivity).

Each individual Index Entry essentially contains a direct copy of the target file's `$FILE_NAME` attribute, preceded by an entry header that governs the tree traversal logic:

| Offset | Size | Description and Traversal Logic |
|---|---|---|
| `0x00` | 8 Bytes | The MFT Reference (Inode) of the target file being indexed. |
| `0x08` | 2 Bytes | The total length of this specific index entry (L). |
| `0x0A` | 2 Bytes | The length of the stream (the payload size of the `$FILE_NAME` attribute). |
| `0x0C` | 1 Byte | Index Flags. `0x01` indicates the entry points to a sub-node. `0x02` indicates this is the final, terminating entry in the node. |
| `0x0E` | 2 Bytes | Padding to align the structure to 8 bytes. |
| `0x10` | Payload | The duplicated `$FILE_NAME` attribute data, providing immediate access to the filename string and MAC times. |

Crucially, if the Flag at offset `0x0C` has the `0x01` bit active, the final 8 bytes of this specific entry (located at `offset L - 8`) will contain the **VCN of the child sub-node** in the `$INDEX_ALLOCATION`.

From a digital forensics perspective, the mechanics of B-tree rebalancing present a unique scenario. When files are deleted, the B-tree nodes are actively shuffled to keep the tree balanced. However, the old index entries within the INDX blocks are merely marked as deleted using the corresponding `$BITMAP` attribute, rather than being wiped. The size of index nodes can vary, particularly for long filenames, providing a type of slack space within the 4 KB INDX buffer. Custom OS drivers, particularly those built for forensic analysis, can parse this slack space to identify remnants of deleted `$I30` entries, recovering filenames and timestamps of files that no longer exist in the active MFT.

### 10.3 OS Directory Traversal Algorithm

To resolve a file path (e.g., `C:\Windows\System32\kernel32.dll`), the OS driver must execute the following algorithmic steps:

1. Read the MFT record for the **Root Directory** (Inode 5).
2. Extract and parse the resident `$INDEX_ROOT` attribute to obtain its list of Index Entries.
3. Perform a string comparison of the requested path segment (e.g., `"Windows"`) against the UTF-16 names in the sorted entries.
4. If an exact match is found, extract the MFT Reference (at offset `0x00`) to immediately open the target file or subdirectory.
5. If the requested filename alphabetically precedes an entry, and that entry has the sub-node flag (`0x01`) active, extract the 64-bit VCN located at the end of the entry.
6. Translate this VCN to a physical LCN using the decoding algorithms against the data runs found in the `$INDEX_ALLOCATION` attribute.
7. Issue a disk read to load the 4 KB INDX block at that physical LCN, apply the fixup array to verify integrity, and repeat the alphabetical search within the new node.

---

## 11. Concluding Architectural Considerations

Implementing a custom NTFS driver from scratch demands meticulous adherence to the data structures, nested pointer logic, and byte-level constraints defined in this specification. By approaching the volume hierarchically—first extracting the BIOS Parameter Block for geometric dimensions, mathematically mapping the Master File Table, rigorously applying the Update Sequence Array for sector integrity, and subsequently developing bulletproof bitwise decoding logic for Data Runs and B+ Tree traversals—an operating system developer can accurately and safely read NTFS files.

While advanced features such as EFS transparent encryption, transactional `$LogFile` journaling, and `$REPARSE_POINT` resolution require additional layers of cryptographic and logical complexity, the core mechanics of LCN calculation, attribute parsing, and cluster extraction outlined in this document form the absolute, uncompromising baseline required to construct a highly robust, enterprise-grade read-access driver.
