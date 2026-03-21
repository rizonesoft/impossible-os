# HFS Plus (Mac OS Extended) — Technical Specification for OS Implementation

## Overview and Architectural Context

HFS Plus (HFS+), also known as Mac OS Extended, is a proprietary file system developed by Apple
Inc. as the successor to the Hierarchical File System (HFS). Introduced with Mac OS 8.1 in January
1998, HFS+ served as the primary file system for macOS until its replacement by APFS in macOS High
Sierra (2017). Despite APFS adoption, HFS+ remains widely encountered on external media, Time
Machine backups, legacy macOS installations, and cross-platform storage devices.

HFS+ is an extent-based file system that uses B-trees for all metadata storage. It supports volumes
up to 8 EiB (2^63 bytes), files up to 8 EiB, and up to 2^32 allocation blocks per volume. File
names are stored as big-endian UTF-16 strings up to 255 characters, using a frozen variant of
Unicode Normalization Form D (NFD) based on Unicode 3.2.

> [!IMPORTANT]
> All multi-byte integer fields in HFS+ on-disk structures are stored in **big-endian** byte order.
> On x86-64 (little-endian) systems, every multi-byte field must be byte-swapped after reading from
> disk and before writing to disk.

### Position in the Storage Stack

```
Block Device → Partition Table (GPT/MBR/APM) → HFS+ Volume → VFS Layer
```

An OS developer needs HFS+ support for:
- Reading macOS-formatted external drives and USB media
- Accessing Time Machine backup volumes
- Digital forensics and data recovery from Apple hardware
- Cross-platform file exchange with macOS systems

### Version History and Compatibility

| Version    | Date     | Key Additions                                            |
| ---------- | -------- | -------------------------------------------------------- |
| HFS+ 4    | Jan 1998 | Initial release with Mac OS 8.1                          |
| HFS+ 4    | Jan 2000 | CNID reuse, wrapping `nextCatalogID`                     |
| HFS+ 4    | 2002     | Journaling (HFSJ) added in Mac OS X 10.2.2              |
| HFS+ 4    | 2003     | Hard links to directories (Mac OS X 10.3)                |
| HFSX 5     | 2003     | Case-sensitive variant, new signature `0x4858`           |
| HFS+ 4    | 2004     | Hot files B-tree, metadata zone (Mac OS X 10.3+)         |
| HFS+ 4    | 2005     | Inline data attributes (`kHFSPlusAttrInlineData`)        |

> [!NOTE]
> The `version` field in `HFSPlusVolumeHeader` is `4` (`kHFSPlusVersion`) for all standard HFS+
> volumes. HFSX volumes use version `5` (`kHFSXVersion`). There is no "HFS+ version 5" — the
> version change coincides with the signature change to `0x4858`.

---

## Volume Identification and Partition Discovery

### GPT Partition Type

On GUID Partition Table (GPT) disks, HFS+ partitions are identified by the partition type GUID:

```
48465300-0000-11AA-AA11-00306543ECAC
```

### MBR Partition Type

On legacy MBR-partitioned disks, the partition type byte is:

```
0xAF
```

### Apple Partition Map (APM)

On Apple Partition Map disks:
- HFS+ partitions have type string `Apple_HFS`
- HFSX partitions have type string `Apple_HFSX`

### Volume Signature Validation

After locating the partition, the driver reads 512 bytes at offset 1024 from the partition start
and validates the `signature` field:

| Signature  | Value    | Format                                  |
| ---------- | -------- | --------------------------------------- |
| `'H+'`     | `0x482B` | Standard HFS+ (case-insensitive)        |
| `'HX'`     | `0x4858` | HFSX (may be case-sensitive)            |

> [!CAUTION]
> If an HFSX volume has a `version` value the driver does not recognize, it **must not** attempt to
> access or repair the volume. Catastrophic data loss may result. Do NOT change the `version` field.

---

## Physical Volume Layout

An HFS+ volume has the following macroscopic layout:

| Region                   | Location                              | Size       |
| ------------------------ | ------------------------------------- | ---------- |
| Reserved (boot blocks)   | Offset 0                              | 1024 bytes |
| Volume Header            | Offset 1024 bytes from start          | 512 bytes  |
| Allocation File          | Variable (tracked in Volume Header)   | Variable   |
| Catalog File (B-tree)    | Variable (tracked in Volume Header)   | Variable   |
| Extents Overflow (B-tree)| Variable (tracked in Volume Header)   | Variable   |
| Attributes File (B-tree) | Variable (tracked in Volume Header)   | Variable   |
| Startup File             | Variable (tracked in Volume Header)   | Variable   |
| User data                | Remaining allocation blocks           | Variable   |
| Alternate Volume Header  | Offset 1024 bytes before end          | 512 bytes  |
| Reserved                 | Last 512 bytes of volume              | 512 bytes  |

The first 1024 bytes are reserved for boot blocks (legacy Mac OS Finder boot code). The last 512
bytes of the volume are reserved. The allocation blocks containing the first 1536 bytes and the
last allocation block(s) are marked as used in the allocation file.

> [!IMPORTANT]
> The alternate volume header is always at offset 1024 bytes from the **end** of the volume. If the
> volume size is not an even multiple of the allocation block size, this area may lie beyond the
> last allocation block. Implementations should only update the alternate header when the length or
> location of a special file changes.

---

## Volume Header

The volume header is the primary anchor for the entire file system. It is located at byte offset
1024 from the start of the partition and occupies 512 bytes.

### `HFSPlusVolumeHeader` Structure

All fields are big-endian.

```c
struct HFSPlusVolumeHeader {
    uint16_t  signature;          /* 0x0000: 'H+' (0x482B) or 'HX' (0x4858)     */
    uint16_t  version;            /* 0x0002: 4 (kHFSPlusVersion) or 5 (kHFSXVersion) */
    uint32_t  attributes;         /* 0x0004: volume attribute flags               */
    uint32_t  lastMountedVersion; /* 0x0008: implementation signature             */
    uint32_t  journalInfoBlock;   /* 0x000C: alloc block # of JournalInfoBlock    */

    uint32_t  createDate;         /* 0x0010: volume creation date (HFS+ epoch)    */
    uint32_t  modifyDate;         /* 0x0014: last volume modification date        */
    uint32_t  backupDate;         /* 0x0018: last backup date                     */
    uint32_t  checkedDate;        /* 0x001C: last consistency check date          */

    uint32_t  fileCount;          /* 0x0020: total number of files on volume      */
    uint32_t  folderCount;        /* 0x0024: total number of folders on volume    */

    uint32_t  blockSize;          /* 0x0028: allocation block size in bytes       */
    uint32_t  totalBlocks;        /* 0x002C: total allocation blocks on volume    */
    uint32_t  freeBlocks;         /* 0x0030: number of free allocation blocks     */

    uint32_t  nextAllocation;     /* 0x0034: hint for next block allocation       */
    uint32_t  rsrcClumpSize;      /* 0x0038: default clump size for resource forks*/
    uint32_t  dataClumpSize;      /* 0x003C: default clump size for data forks    */
    uint32_t  nextCatalogID;      /* 0x0040: next unused catalog node ID (CNID)   */

    uint32_t  writeCount;         /* 0x0044: volume write count                   */
    uint64_t  encodingsBitmap;    /* 0x0048: bitmap of text encodings used        */

    uint32_t  finderInfo[8];      /* 0x0050: Finder info (bootable dirs, OS IDs)  */

    /* 0x0070: fork data for the five special files (80 bytes each = 400 bytes)   */
    HFSPlusForkData  allocationFile;   /* 0x0070 */
    HFSPlusForkData  extentsFile;      /* 0x00C0 */
    HFSPlusForkData  catalogFile;      /* 0x0110 */
    HFSPlusForkData  attributesFile;   /* 0x0160 */
    HFSPlusForkData  startupFile;      /* 0x01B0 */
};
/* Total size: 512 bytes (0x0200) */
```

### Volume Header Field Details

| Field                | Offset   | Size | Description                                             |
| -------------------- | -------- | ---- | ------------------------------------------------------- |
| `signature`          | `0x0000` | 2    | `0x482B` ('H+') for HFS+, `0x4858` ('HX') for HFSX     |
| `version`            | `0x0002` | 2    | `4` for HFS+, `5` for HFSX                              |
| `attributes`         | `0x0004` | 4    | Volume attribute bit flags                               |
| `lastMountedVersion` | `0x0008` | 4    | Four-char code of last mounting implementation           |
| `journalInfoBlock`   | `0x000C` | 4    | Allocation block number of `JournalInfoBlock`            |
| `createDate`         | `0x0010` | 4    | Seconds since 1904-01-01 00:00:00 GMT                   |
| `modifyDate`         | `0x0014` | 4    | Seconds since 1904-01-01 00:00:00 GMT                   |
| `backupDate`         | `0x0018` | 4    | Seconds since 1904-01-01 00:00:00 GMT                   |
| `checkedDate`        | `0x001C` | 4    | Seconds since 1904-01-01 00:00:00 GMT                   |
| `fileCount`          | `0x0020` | 4    | Total files (excludes special files)                     |
| `folderCount`        | `0x0024` | 4    | Total folders (excludes root folder)                     |
| `blockSize`          | `0x0028` | 4    | Allocation block size in bytes (power of 2, ≥ 512)      |
| `totalBlocks`        | `0x002C` | 4    | Total allocation blocks on volume                        |
| `freeBlocks`         | `0x0030` | 4    | Number of free allocation blocks                         |
| `nextAllocation`     | `0x0034` | 4    | Next allocation search start hint                        |
| `rsrcClumpSize`      | `0x0038` | 4    | Default resource fork clump size (bytes)                 |
| `dataClumpSize`      | `0x003C` | 4    | Default data fork clump size (bytes)                     |
| `nextCatalogID`      | `0x0040` | 4    | Next CNID to assign (monotonically increasing)           |
| `writeCount`         | `0x0044` | 4    | Incremented on each flush                                |
| `encodingsBitmap`    | `0x0048` | 8    | Bit N set if text encoding N is used on volume           |
| `finderInfo`         | `0x0050` | 32   | Eight 32-bit Finder info values                          |
| `allocationFile`     | `0x0070` | 80   | `HFSPlusForkData` for allocation bitmap                  |
| `extentsFile`        | `0x00C0` | 80   | `HFSPlusForkData` for extents overflow B-tree            |
| `catalogFile`        | `0x0110` | 80   | `HFSPlusForkData` for catalog B-tree                     |
| `attributesFile`     | `0x0160` | 80   | `HFSPlusForkData` for attributes B-tree                  |
| `startupFile`        | `0x01B0` | 80   | `HFSPlusForkData` for startup file                       |

> [!NOTE]
> HFS+ dates use a **different epoch** than Unix. The HFS+ epoch is midnight January 1, 1904 GMT.
> The maximum representable date is February 6, 2040 at 06:28:15 GMT. To convert to Unix timestamp:
> `unix_time = hfs_time - 2082844800`.

### `lastMountedVersion` Known Values

| Value    | Meaning                                             |
| -------- | --------------------------------------------------- |
| `'8.10'` | Mac OS 8.1 through 9.x                              |
| `'10.0'` | Mac OS X (non-journaled)                             |
| `'HFSJ'` | Mac OS X (journaled)                                 |
| `'fsck'` | Volume is being repaired by `fsck_hfs`               |

### Volume Attributes (bits 0–31 of `attributes` field)

| Bit | Constant                         | Description                                     |
| --- | -------------------------------- | ----------------------------------------------- |
| 0–6 | —                                | Reserved                                         |
| 7   | `kHFSVolumeHardwareLockBit`      | Hardware write-protect detected                  |
| 8   | `kHFSVolumeUnmountedBit`         | Volume was cleanly unmounted                     |
| 9   | `kHFSVolumeSparedBlocksBit`      | Bad block file contains entries                  |
| 10  | `kHFSVolumeNoCacheRequiredBit`   | No cache required for volume                     |
| 11  | `kHFSBootVolumeInconsistentBit`  | Boot volume consistency check needed             |
| 12  | `kHFSCatalogNodeIDsReusedBit`    | CNIDs have wrapped around and been reused        |
| 13  | `kHFSVolumeJournaledBit`         | Volume has an active journal                     |
| 14  | —                                | Reserved                                         |
| 15  | `kHFSVolumeSoftwareLockBit`      | Software write-protect flag                      |
| 16–31| —                               | Reserved                                         |

> [!CAUTION]
> When `kHFSVolumeJournaledBit` (bit 13) is set, the driver **must** replay the journal before
> modifying any on-disk structures. Writing to a dirty journaled volume without replaying the
> journal will corrupt the file system.

---

## Byte Order and Big-Endian Encoding

All multi-byte on-disk fields in HFS+ are stored in **big-endian** (network) byte order. This is a
direct consequence of HFS+'s origins on Motorola 68k and PowerPC architectures.

On little-endian x86-64 systems, every `uint16_t`, `uint32_t`, and `uint64_t` field must be
byte-swapped immediately after reading from disk, and byte-swapped back to big-endian before
writing.

### Recommended Byte-Swap Helpers

```c
static inline uint16_t hfs_be16(uint16_t val) {
    return __builtin_bswap16(val);  /* or x86 XCHG / MOVBE */
}

static inline uint32_t hfs_be32(uint32_t val) {
    return __builtin_bswap32(val);  /* x86 BSWAP instruction */
}

static inline uint64_t hfs_be64(uint64_t val) {
    return __builtin_bswap64(val);  /* x86 BSWAP */
}
```

> [!CAUTION]
> Failure to byte-swap will cause the driver to interpret block addresses, file sizes, and tree
> pointers incorrectly. On x86-64, reading a big-endian 32-bit block address `0x00001234` without
> swapping yields `0x34120000`, causing reads to garbage disk locations.

---

## Fork Data Structure and Extent Descriptors

HFS+ tracks file contents using the `HFSPlusForkData` structure, which stores the logical size and
the first eight physical extents of a fork. Both data forks and resource forks use this structure.

### `HFSPlusExtentDescriptor`

```c
struct HFSPlusExtentDescriptor {
    uint32_t  startBlock;   /* first allocation block of the extent */
    uint32_t  blockCount;   /* number of allocation blocks          */
};
/* Size: 8 bytes */
```

### `HFSPlusExtentRecord`

An extent record is an array of 8 extent descriptors:

```c
typedef HFSPlusExtentDescriptor HFSPlusExtentRecord[8];
/* Size: 64 bytes (8 × 8) */
```

### `HFSPlusForkData`

```c
struct HFSPlusForkData {
    uint64_t              logicalSize;   /* 0x00: fork logical size in bytes   */
    uint32_t              clumpSize;     /* 0x08: fork-specific clump size     */
    uint32_t              totalBlocks;   /* 0x0C: total alloc blocks in fork   */
    HFSPlusExtentRecord   extents;       /* 0x10: first 8 extents (64 bytes)   */
};
/* Size: 80 bytes (0x50) */
```

Unused extent descriptors have both `startBlock` and `blockCount` set to zero. If a fork requires
more than 8 extents, the additional extents are stored in the Extents Overflow B-tree.

---

## B-Tree Architecture

The Catalog File, Extents Overflow File, and Attributes File are all organized as B-trees. HFS+
B-trees consist of fixed-size nodes arranged sequentially in a file.

### Node Sizes

Node sizes must be a power of 2, ranging from 512 bytes to 32,768 bytes (32 KiB). Default sizes:

| B-tree File          | Default Node Size | Minimum Node Size                    |
| -------------------- | ----------------- | ------------------------------------ |
| Catalog File         | 8 KiB (macOS)     | 4 KiB (`kHFSPlusCatalogMinNodeSize`) |
| Extents Overflow     | 4 KiB             | 1 KiB                               |
| Attributes File      | 4 KiB             | 4 KiB (`kHFSPlusAttrMinNodeSize`)    |
| Hot Files B-tree     | 512 bytes          | 512 bytes                            |

### Node Types

| Kind Value | Constant           | Description                                          |
| ---------- | ------------------ | ---------------------------------------------------- |
| `-1`       | `kBTLeafNode`      | Terminal nodes containing actual data records         |
| `0`        | `kBTIndexNode`     | Internal routing nodes with pointer records           |
| `1`        | `kBTHeaderNode`    | Always node 0; contains B-tree metadata               |
| `2`        | `kBTMapNode`       | Bitmap tracking allocated/free nodes                  |

### `BTNodeDescriptor`

Every node begins with a 14-byte node descriptor:

```c
struct BTNodeDescriptor {
    uint32_t  fLink;        /* 0x00: node number of next node on this level     */
    uint32_t  bLink;        /* 0x04: node number of previous node on this level */
    int8_t    kind;         /* 0x08: node type (-1, 0, 1, or 2)                */
    uint8_t   height;       /* 0x09: level in tree (leaf = 1, root = max)      */
    uint16_t  numRecords;   /* 0x0A: number of records in this node            */
    uint16_t  reserved;     /* 0x0C: must be zero                              */
};
/* Size: 14 bytes */
```

### Node Internal Layout

Each node has three zones:

```
+---------------------------+
| BTNodeDescriptor (14 B)   |  ← offset 0
+---------------------------+
| Record 0                  |  ← offset 14
| Record 1                  |
| ...                       |
| Record N-1                |
| [Free space]              |
+---------------------------+
| Offset[N] (free space ptr)|  ← grows backward from end
| Offset[N-1]               |
| ...                       |
| Offset[0] = 14            |  ← last 2 bytes of node
+---------------------------+
```

The record offset array at the end of the node consists of `numRecords + 1` entries, each a
big-endian `uint16_t`. Offsets are stored in reverse order: the offset for record 0 occupies the
last 2 bytes of the node. The extra entry (`numRecords`) points to the first byte of free space.

> [!IMPORTANT]
> The record offset array always contains **one more entry** than there are records. This extra
> entry points to the first free byte, indicating the end of the last record.

### `BTHeaderRec` (Header Record in Node 0)

The first record in the header node (node 0) of every B-tree:

```c
struct BTHeaderRec {
    uint16_t  treeDepth;       /* 0x00: current depth of tree                  */
    uint32_t  rootNode;        /* 0x02: node number of root node               */
    uint32_t  leafRecords;     /* 0x06: total number of leaf records            */
    uint32_t  firstLeafNode;   /* 0x0A: node number of first leaf node          */
    uint32_t  lastLeafNode;    /* 0x0E: node number of last leaf node           */
    uint16_t  nodeSize;        /* 0x12: size of a node in bytes                 */
    uint16_t  maxKeyLength;    /* 0x14: maximum key length for this B-tree      */
    uint32_t  totalNodes;      /* 0x16: total number of nodes in B-tree file    */
    uint32_t  freeNodes;       /* 0x1A: number of free (unused) nodes           */
    uint16_t  reserved1;       /* 0x1E: reserved                               */
    uint32_t  clumpSize;       /* 0x20: B-tree file clump size (misaligned!)    */
    uint8_t   btreeType;       /* 0x24: B-tree type                            */
    uint8_t   keyCompareType;  /* 0x25: key comparison type (for HFSX)          */
    uint32_t  attributes;      /* 0x26: B-tree attributes (long-aligned again)  */
    uint32_t  reserved3[16];   /* 0x2A: reserved                               */
};
/* Size: 106 bytes */
```

> [!CAUTION]
> The `clumpSize` field at offset `0x20` in `BTHeaderRec` is **misaligned** — it is a 32-bit
> integer at a 2-byte-aligned offset. The driver must use unaligned access or manual byte assembly.

### B-tree Types

| Constant             | Value | Usage                                      |
| -------------------- | ----- | ------------------------------------------ |
| `kHFSBTreeType`      | `0`   | Catalog, Extents, Attributes B-trees       |
| `kUserBTreeType`     | `128` | Hot Files B-tree                           |
| `kReservedBTreeType` | `255` | Reserved                                   |

### Key Comparison Type (HFSX)

| Value | Constant                       | Comparison Method                     |
| ----- | ------------------------------ | ------------------------------------- |
| `0`   | `kHFSCaseFolding`              | Case-insensitive (default for HFS+)   |
| `1`   | `kHFSBinaryCompare`            | Binary (case-sensitive, HFSX only)    |

### Search Algorithm

To find a record in the B-tree:

1. Read the header node (node 0) → extract `rootNode`
2. Read the root node
3. If node `kind` == `kBTIndexNode`:
   - Binary search the node's keyed records to find the child pointer
   - Read the child node, repeat from step 3
4. If node `kind` == `kBTLeafNode`:
   - Binary search the node's keyed records for the target key
   - Return the matching data record, or "not found"

---

## Catalog File

The catalog file is a B-tree that stores the complete file and folder hierarchy of the volume. Its
location is described by `catalogFile` fork data in the volume header.

### Catalog Node IDs (CNIDs)

Every file and folder is assigned a unique 32-bit Catalog Node ID (CNID):

```c
typedef uint32_t HFSCatalogNodeID;
```

#### Reserved CNIDs

The first 16 CNIDs are reserved by Apple:

| CNID | Constant                    | Purpose                                     |
| ---- | --------------------------- | ------------------------------------------- |
| 0    | —                           | Nil value (never used)                       |
| 1    | `kHFSRootParentID`          | Parent of the root folder                    |
| 2    | `kHFSRootFolderID`          | Root folder of the volume                    |
| 3    | `kHFSExtentsFileID`         | Extents overflow file                        |
| 4    | `kHFSCatalogFileID`         | Catalog file                                 |
| 5    | `kHFSBadBlockFileID`        | Bad block file                               |
| 6    | `kHFSAllocationFileID`      | Allocation bitmap file                       |
| 7    | `kHFSStartupFileID`         | Startup file                                 |
| 8    | `kHFSAttributesFileID`      | Attributes file                              |
| 14   | `kHFSRepairCatalogFileID`   | Used by `fsck_hfs` during repairs            |
| 15   | `kHFSBogusExtentFileID`     | Used by `ExchangeFiles`                      |
| 16   | `kHFSFirstUserCatalogNodeID`| First CNID available for user files/folders  |

> [!NOTE]
> CNIDs 9–13 are reserved but have no assigned purpose in the current specification.

### Catalog Key

```c
struct HFSPlusCatalogKey {
    uint16_t          keyLength;   /* length of key (excluding this field)       */
    HFSCatalogNodeID  parentID;    /* CNID of the parent folder                  */
    HFSUniStr255      nodeName;    /* file/folder name in UTF-16 BE              */
};
```

Key length ranges from `kHFSPlusCatalogKeyMinimumLength` (6) to
`kHFSPlusCatalogKeyMaximumLength` (516).

Keys are compared first by `parentID` (unsigned 32-bit comparison), then by `nodeName` using:
- **HFS+ volumes**: Case-insensitive comparison per Apple's frozen Unicode 3.2 tables
- **HFSX case-sensitive**: Binary comparison of unsigned 16-bit character values

### `HFSUniStr255`

```c
struct HFSUniStr255 {
    uint16_t  length;          /* number of Unicode characters    */
    uint16_t  unicode[255];    /* UTF-16 BE characters            */
};
```

### Catalog Record Types

| Value    | Constant                       | Data Type                 |
| -------- | ------------------------------ | ------------------------- |
| `0x0001` | `kHFSPlusFolderRecord`         | `HFSPlusCatalogFolder`    |
| `0x0002` | `kHFSPlusFileRecord`           | `HFSPlusCatalogFile`      |
| `0x0003` | `kHFSPlusFolderThreadRecord`   | `HFSPlusCatalogThread`    |
| `0x0004` | `kHFSPlusFileThreadRecord`     | `HFSPlusCatalogThread`    |

### `HFSPlusCatalogFolder`

```c
struct HFSPlusCatalogFolder {
    int16_t           recordType;         /* 0x0001                              */
    uint16_t          flags;              /* folder flags (reserved)             */
    uint32_t          valence;            /* number of items in this folder      */
    HFSCatalogNodeID  folderID;           /* CNID of this folder                 */
    uint32_t          createDate;         /* creation date                       */
    uint32_t          contentModDate;     /* content modification date           */
    uint32_t          attributeModDate;   /* attribute modification date         */
    uint32_t          accessDate;         /* last access date                    */
    uint32_t          backupDate;         /* last backup date                    */
    HFSPlusBSDInfo    permissions;        /* POSIX permissions                   */
    FolderInfo        userInfo;           /* Finder folder info (16 bytes)       */
    ExtendedFolderInfo finderInfo;        /* Extended Finder info (16 bytes)     */
    uint32_t          textEncoding;       /* text encoding hint                  */
    uint32_t          reserved;           /* reserved                            */
};
```

### `HFSPlusCatalogFile`

```c
struct HFSPlusCatalogFile {
    int16_t           recordType;         /* 0x0002                              */
    uint16_t          flags;              /* file flags                          */
    uint32_t          reserved1;          /* reserved                            */
    HFSCatalogNodeID  fileID;             /* CNID of this file                   */
    uint32_t          createDate;         /* creation date                       */
    uint32_t          contentModDate;     /* content modification date           */
    uint32_t          attributeModDate;   /* attribute modification date         */
    uint32_t          accessDate;         /* last access date                    */
    uint32_t          backupDate;         /* last backup date                    */
    HFSPlusBSDInfo    permissions;        /* POSIX permissions                   */
    FileInfo          userInfo;           /* Finder file info (16 bytes)         */
    ExtendedFileInfo  finderInfo;         /* Extended Finder info (16 bytes)     */
    uint32_t          textEncoding;       /* text encoding hint                  */
    uint32_t          reserved2;          /* reserved                            */
    HFSPlusForkData   dataFork;           /* data fork extents (80 bytes)        */
    HFSPlusForkData   resourceFork;       /* resource fork extents (80 bytes)    */
};
/* Size: 248 bytes */
```

#### File Flags

| Bit | Constant               | Description               |
| --- | ---------------------- | ------------------------- |
| 0   | `kHFSFileLockedBit`    | File is locked            |
| 1   | `kHFSThreadExistsBit`  | Thread record exists      |

### `HFSPlusCatalogThread`

Thread records allow reverse lookups — finding a file/folder record given only its CNID.

```c
struct HFSPlusCatalogThread {
    int16_t           recordType;    /* 0x0003 or 0x0004                        */
    int16_t           reserved;      /* reserved                                */
    HFSCatalogNodeID  parentID;      /* CNID of the parent folder               */
    HFSUniStr255      nodeName;      /* name of the file or folder              */
};
```

> [!IMPORTANT]
> In HFS+, thread records are **required** for both files and folders (unlike HFS, which only
> required them for folders). The catalog key for a thread record uses the CNID as `parentID` and
> an empty string as `nodeName`.

### Catalog Tree Usage

To look up `/System/kernel.bin`:
1. Search for key (`parentID`=2, `nodeName`="System") → get folder CNID
2. Search for key (`parentID`=folder_CNID, `nodeName`="kernel.bin") → get file record

To look up a file by CNID alone:
1. Search for key (`parentID`=target_CNID, `nodeName`="") → get thread record
2. Thread record yields `parentID` and `nodeName`
3. Search for key (`parentID`=thread.parentID, `nodeName`=thread.nodeName) → get file record

---

## POSIX Permissions — `HFSPlusBSDInfo`

```c
struct HFSPlusBSDInfo {
    uint32_t  ownerID;       /* 0x00: user ID                                  */
    uint32_t  groupID;       /* 0x04: group ID                                 */
    uint8_t   adminFlags;    /* 0x08: superuser-changeable flags                */
    uint8_t   ownerFlags;    /* 0x09: owner-changeable flags                    */
    uint16_t  fileMode;      /* 0x0A: file type and permission bits             */
    union {
        uint32_t  iNodeNum;  /* hard links: indirect node number                */
        uint32_t  linkCount; /* hard links: reference count                     */
        uint32_t  rawDevice; /* block/char devices: device number               */
    } special;               /* 0x0C: context-dependent                         */
};
/* Size: 16 bytes */
```

The `fileMode` field uses standard POSIX bit layout:

| Bits   | Mask       | Description                  |
| ------ | ---------- | ---------------------------- |
| 15:12  | `0xF000`   | File type (S_IFREG, etc.)    |
| 11     | `0x0800`   | Set-UID                      |
| 10     | `0x0400`   | Set-GID                      |
| 9      | `0x0200`   | Sticky bit                   |
| 8:6    | `0x01C0`   | Owner rwx                    |
| 5:3    | `0x0038`   | Group rwx                    |
| 2:0    | `0x0007`   | Other rwx                    |

File type constants in `fileMode` bits 15:12:

| Value  | Constant   | Description          |
| ------ | ---------- | -------------------- |
| `0x1`  | `S_IFIFO`  | Named pipe (FIFO)    |
| `0x2`  | `S_IFCHR`  | Character device     |
| `0x4`  | `S_IFDIR`  | Directory            |
| `0x6`  | `S_IFBLK`  | Block device         |
| `0x8`  | `S_IFREG`  | Regular file         |
| `0xA`  | `S_IFLNK`  | Symbolic link        |
| `0xC`  | `S_IFSOCK` | Socket               |
| `0xE`  | `S_IFWHT`  | Whiteout (union FS)  |

> [!NOTE]
> If the upper 4 bits of `fileMode` are zero, the permissions structure is uninitialized (common
> for files created by Mac OS 8/9). The OS should apply default permissions in this case.

---

## Extents Overflow File

When a file's fork has more than 8 extents, the additional extents are stored in the Extents
Overflow B-tree. This B-tree uses a fixed-length key.

### `HFSPlusExtentKey`

```c
struct HFSPlusExtentKey {
    uint16_t          keyLength;    /* always kHFSPlusExtentKeyMaximumLength (10) */
    uint8_t           forkType;     /* 0x00 = data fork, 0xFF = resource fork     */
    uint8_t           pad;          /* padding byte                               */
    HFSCatalogNodeID  fileID;       /* CNID of the file                           */
    uint32_t          startBlock;   /* starting allocation block offset in fork    */
};
/* Size: 12 bytes */
```

Keys are compared in order: `fileID`, `forkType`, `startBlock`.

The data record for each extents overflow entry is an `HFSPlusExtentRecord` (array of 8 extent
descriptors, 64 bytes total).

> [!IMPORTANT]
> The first 8 extents of a fork are stored in the catalog file record. Extents overflow records
> store extents 8–15, 16–23, etc. The `startBlock` in the key is the allocation block offset
> within the fork where these extents begin.

---

## Allocation File

The allocation file is a simple bitmap where each bit represents one allocation block on the
volume. Bit N corresponds to allocation block N.

- Bit value `1` = block is in use
- Bit value `0` = block is free

The allocation file's location and size are described by `allocationFile` fork data in the volume
header. Unlike the other special files, the allocation file is not a B-tree — it is a flat bitmap.

> [!NOTE]
> The allocation file's size in bits equals `totalBlocks` from the volume header. The size in
> bytes is `ceil(totalBlocks / 8)`.

---

## Attributes File

The attributes file is a B-tree that stores extended attributes and named forks. It may not exist
on all volumes — if the first extent in the volume header's `attributesFile` has zero allocation
blocks, no attributes file exists.

### Attribute Record Types

| Value    | Constant                   | Data Type                |
| -------- | -------------------------- | ------------------------ |
| `0x10`   | `kHFSPlusAttrInlineData`   | Inline data attribute    |
| `0x20`   | `kHFSPlusAttrForkData`     | Fork data attribute      |
| `0x30`   | `kHFSPlusAttrExtents`      | Extension attribute      |

### `HFSPlusAttrForkData`

```c
struct HFSPlusAttrForkData {
    uint32_t         recordType;   /* kHFSPlusAttrForkData (0x20)     */
    uint32_t         reserved;
    HFSPlusForkData  theFork;      /* fork data with up to 8 extents  */
};
```

### `HFSPlusAttrExtents`

```c
struct HFSPlusAttrExtents {
    uint32_t             recordType;   /* kHFSPlusAttrExtents (0x30)  */
    uint32_t             reserved;
    HFSPlusExtentRecord  extents;      /* 8 additional extents        */
};
```

---

## Unicode Normalization and String Comparison

### Canonical Decomposition

HFS+ stores file names in a proprietary, frozen variant of Unicode Normalization Form D (NFD)
based on the Unicode 3.2 standard (March 2002). Composite characters must be decomposed into their
base character and combining marks before being stored on disk or used as search keys.

Example: `ü` (U+00FC) → `u` (U+0075) + `¨` (U+0308)

The decomposition tables are **frozen** to the Unicode 3.2 standard. Newer Unicode versions may
define additional decompositions, but HFS+ does not use them.

### Case-Insensitive Comparison (Default HFS+)

For standard HFS+ volumes (not case-sensitive HFSX), catalog key comparison:

1. Decompose both strings to NFD using the frozen Unicode 3.2 tables
2. Apply case folding (convert to lowercase) using Apple's case-folding table
3. Compare the resulting UTF-16 character sequences

> [!CAUTION]
> If the VFS layer accepts user-space path lookups in NFC (composed) form and passes them directly
> to the catalog B-tree without NFD decomposition, lookups for filenames containing diacritical
> marks will fail — the file will appear to not exist even though it is present on disk.

### Case-Sensitive Comparison (HFSX)

For case-sensitive HFSX volumes (identified by `keyCompareType` = `kHFSBinaryCompare` = `1` in the
catalog B-tree header record), characters are compared as raw unsigned 16-bit integers with no
case folding.

---

## Journaling (HFSJ)

HFS+ journaling, introduced in Mac OS X 10.2.2, uses a write-ahead log to protect metadata
integrity during crash or power loss. Journaling is enabled when `kHFSVolumeJournaledBit` (bit 13)
is set in the volume header `attributes` field.

### `JournalInfoBlock`

The `journalInfoBlock` field of the volume header contains the allocation block number of the
`JournalInfoBlock` structure:

```c
struct JournalInfoBlock {
    uint32_t  flags;                 /* 0x00: journal flags                  */
    uint32_t  device_signature[8];   /* 0x04: reserved for external journal  */
    uint64_t  offset;                /* 0x24: byte offset to journal header  */
    uint64_t  size;                  /* 0x2C: size of journal buffer (bytes) */
    uint32_t  reserved[32];          /* 0x34: reserved                       */
};
```

#### JournalInfoBlock Flags

| Bit   | Constant                       | Description                           |
| ----- | ------------------------------ | ------------------------------------- |
| 0     | `kJIJournalInFSMask`           | Journal is within the HFS+ volume     |
| 1     | `kJIJournalOnOtherDeviceMask`  | Journal is on a separate device       |
| 2     | `kJIJournalNeedInitMask`       | Journal needs initialization          |

> [!NOTE]
> Currently, only `kJIJournalInFSMask` is supported. External journals
> (`kJIJournalOnOtherDeviceMask`) are not supported by any known implementation.

### Journal Header (`journal_header`)

The journal begins with a header describing the location of pending transactions:

```c
struct journal_header {
    uint32_t  magic;        /* 0x00: JOURNAL_HEADER_MAGIC (0x4A4E4C78 = "JNLx") */
    uint32_t  endian;       /* 0x04: ENDIAN_MAGIC (0x12345678)                   */
    uint64_t  start;        /* 0x08: byte offset of oldest pending transaction   */
    uint64_t  end;          /* 0x10: byte offset of next (newest) transaction    */
    uint64_t  size;         /* 0x18: total size of journal buffer in bytes       */
    uint32_t  blhdr_size;   /* 0x20: size of block list headers                  */
    uint32_t  checksum;     /* 0x24: checksum of this header                     */
    uint32_t  jhdr_size;    /* 0x28: size of this journal header                 */
};
/* Size: 44 bytes */

#define JOURNAL_HEADER_MAGIC  0x4A4E4C78   /* "JNLx" */
#define ENDIAN_MAGIC          0x12345678
```

The journal is **clean** (empty) when `start == end`. If `start != end`, there are pending
transactions that must be replayed before modifying the volume.

The journal operates as a **circular buffer**. If `end < start`, the buffer has wrapped around.

### Block List Header (`block_list_header`)

Each transaction consists of one or more block lists. Each block list starts with a header:

```c
struct block_list_header {
    uint16_t    max_blocks;     /* max block_info entries in this list           */
    uint16_t    num_blocks;     /* actual number of block_info entries used      */
    uint32_t    bytes_used;     /* total bytes used by this block list           */
    uint32_t    checksum;       /* checksum of all block_info data               */
    uint32_t    pad;            /* reserved                                      */
    block_info  binfo[1];       /* variable-length array of block descriptors    */
};
```

### Block Info (`block_info`)

```c
struct block_info {
    uint64_t  bnum;     /* destination block number on disk (for binfo[1..N])     */
    uint32_t  bsize;    /* size of the block data in bytes                        */
    uint32_t  next;     /* byte offset of next block_list_header (for binfo[0])   */
};
/* Size: 16 bytes */
```

The first element `binfo[0]` is special: its `next` field contains the byte offset of the next
block list header in the transaction chain. The remaining entries (`binfo[1]` through
`binfo[num_blocks-1]`) describe blocks to be replayed.

### Journal Checksum Algorithm

Both the journal header and block list header use the same checksum:

```c
static int calc_checksum(unsigned char *ptr, int len) {
    int i, cksum = 0;
    for (i = 0; i < len; i++, ptr++) {
        cksum = (cksum << 8) ^ (cksum + *ptr);
    }
    return (~cksum);
}
```

To verify: set the `checksum` field to zero, compute `calc_checksum()` over the header, compare
with the original `checksum` value.

### Journal Replay Procedure

1. Read `JournalInfoBlock` from the allocation block specified in the volume header
2. Read `journal_header` from the byte offset in `JournalInfoBlock.offset`
3. Verify `magic` == `0x4A4E4C78` and `endian` == `0x12345678`
4. If `start == end`, journal is clean — no replay needed
5. Otherwise, starting at byte offset `start`:
   a. Read `block_list_header`
   b. Verify its checksum
   c. For each `binfo[1..num_blocks-1]`: read the block data from the journal buffer, write
      it to `binfo[i].bnum` on the volume
   d. If `binfo[0].next != 0`, follow to the next block list header
   e. Handle circular wrapping when offsets exceed `size`
6. After replaying all transactions, set `start = end` and flush the journal header
7. Flush the volume header with updated metadata

> [!CAUTION]
> Replaying the journal is **mandatory** before writing to a dirty journaled volume. Skipping
> replay will corrupt the file system, as B-tree nodes and allocation state may be inconsistent.

---

## Hard Links

HFS+ supports hard links for both files (Mac OS X 10.0+) and directories (Mac OS X 10.3+).

### File Hard Links

When a hard link is created:
1. The original file is moved to a hidden "metadata directory"
   (`\x00\x00\x00\x00HFS+ Private Data`) in the root folder
2. The file is renamed to `iNode<CNID>` within the metadata directory
3. A hard link entry is created in the original location with file type `hlnk` and creator `hfs+`
4. The `special.iNodeNum` field in `HFSPlusBSDInfo` stores the inode number
5. The `special.linkCount` field in the inode's `HFSPlusBSDInfo` tracks the reference count

### Directory Hard Links

Directory hard links use a similar mechanism with the metadata directory
`\x00\x00\x00\x00HFS+ Private Dir Data\x0d` and entries named `dir_<CNID>`.

---

## Startup File

The startup file is a contiguous extent of data intended to assist non-macOS operating systems in
booting from an HFS+ volume. Its location is described by the `startupFile` fork data in the
volume header. Bare-metal OS implementations typically ignore this file.

---

## Hot Files B-Tree

macOS maintains a hidden "Hot Files" B-tree that tracks files with high read access frequency.
During idle periods, the OS defragments these files and moves them to the fastest disk regions
(the "metadata zone" near the beginning of the volume).

The hot files B-tree uses `btreeType` = `kUserBTreeType` (128) and is stored as an extended
attribute named `com.apple.system.hotfiles` on the root folder.

---

## QEMU Testing Configuration

### Mount an HFS+ Disk Image

Create an HFS+ disk image on a macOS host:

```bash
# On macOS: create a 256 MB HFS+ disk image
hdiutil create -size 256m -fs HFS+ -volname TestHFS test_hfsplus.dmg
# Convert to raw format for QEMU
hdiutil convert test_hfsplus.dmg -format UDTO -o test_hfsplus.iso
```

### Basic QEMU Configuration

```bash
# Attach HFS+ image as secondary drive
qemu-system-x86_64 \
    -drive file=build/system-disk.img,format=raw,if=none,id=boot \
    -device ahci,id=ahci0 \
    -device ide-hd,drive=boot,bus=ahci0.0 \
    -drive file=test_hfsplus.raw,format=raw,if=none,id=hfs \
    -device ide-hd,drive=hfs,bus=ahci0.1 \
    -m 256M -serial stdio
```

### VirtIO Configuration

```bash
qemu-system-x86_64 \
    -drive file=build/system-disk.img,format=raw,if=none,id=boot \
    -device virtio-blk-pci,drive=boot \
    -drive file=test_hfsplus.raw,format=raw,if=none,id=hfs \
    -device virtio-blk-pci,drive=hfs \
    -m 256M -serial stdio
```

### Creating Test Images with `mkfs.hfsplus` (Linux)

```bash
# Install hfsprogs on Ubuntu/Debian
sudo apt install hfsprogs

# Create a raw image and format as HFS+
dd if=/dev/zero of=test_hfsplus.raw bs=1M count=256
mkfs.hfsplus -v TestHFS test_hfsplus.raw

# Optionally enable journaling
mkfs.hfsplus -v TestHFS -J test_hfsplus_journaled.raw
```

---

## Implementation Priorities for Impossible OS

| Priority | Feature                              | Description                                   |
| -------- | ------------------------------------ | --------------------------------------------- |
| 🔴 P0    | Volume header parsing                | Read and validate `HFSPlusVolumeHeader`        |
| 🔴 P0    | Big-endian byte swapping             | `hfs_be16/32/64` helper functions              |
| 🔴 P0    | B-tree node reading                  | Parse `BTNodeDescriptor`, extract records      |
| 🔴 P0    | Catalog B-tree traversal             | Search by `(parentID, nodeName)` key           |
| 🔴 P0    | Read-only file access                | Read file data via fork extents                |
| 🟠 P1    | Extents overflow B-tree              | Support fragmented files (> 8 extents)         |
| 🟠 P1    | Journal replay                       | Replay pending transactions on dirty mount     |
| 🟠 P1    | Unicode NFD decomposition            | Frozen Unicode 3.2 tables for name matching    |
| 🟠 P1    | Case-insensitive comparison          | Apple's case-folding tables                    |
| 🟡 P2    | Allocation bitmap management         | Read/write allocation file for block allocation|
| 🟡 P2    | Catalog B-tree insertion/deletion    | Create and delete files and folders             |
| 🟡 P2    | Write support                        | Allocate blocks, update catalog, write data    |
| 🟡 P2    | POSIX permissions enforcement        | Parse `HFSPlusBSDInfo` in VFS `open()` path    |
| 🟢 P3    | Attributes B-tree                    | Extended attributes and named forks            |
| 🟢 P3    | Hard link resolution                 | Follow `hlnk`/`hfs+` entries to inode files   |
| 🟢 P3    | Directory hard links                 | Resolve `dir_<CNID>` entries                   |
| 🔵 P4    | Hot files optimization               | Hot file tracking and defragmentation           |
| 🔵 P4    | HFSX case-sensitive variant          | Binary comparison mode                         |
| 🔵 P4    | VFS write-ahead journal              | Full journal write support (not just replay)   |
| 🔵 P4    | HFS wrapper support                  | Detect HFS wrapper around embedded HFS+ volume |

### Current Codebase State

The Impossible OS VFS layer (`src/kernel/fs/vfs.c`) provides generic file system mounting, inode
management, and POSIX system call routing. The HFS+ driver would register as a new file system
type alongside the existing FAT32 and IXFS drivers, implementing the standard VFS operations
(`mount`, `open`, `read`, `write`, `readdir`, `stat`, `close`).

Key kernel dependencies already available:
- Block device abstraction (`blkdev`)
- GPT/MBR partition discovery
- Memory allocation (`kmalloc`, `pmm_alloc_contiguous`)
- Mutex/spinlock synchronization primitives
