# 047-NTFS — New Technology File System Driver

> **Goal:** Implement an NTFS 3.1 read/write driver for Impossible OS, enabling
> native access to Windows NTFS volumes — the dominant filesystem on internal
> hard drives. Starting with read-only MFT parsing and directory traversal,
> then extending to write support, journaling, compression, and security
> descriptors. This is critical for Win32 API compatibility (`CreateFile`,
> `ReadFile`, `WriteFile` on `C:\` volumes).

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (MFT records, INDX buffers, data run clusters, $Bitmap cache). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). See `rules.md` Known Gotchas.

> [!IMPORTANT]
> **Spec Reference:** All field offsets, data structures, and algorithms reference the
> [NTFS 3.1 Specification](file:///home/derickpayne/impossible-os/specs/ntfs-3.1.md)
> in `specs/ntfs-3.1.md`. See also Microsoft's open-source ntfs3 driver (Linux 5.15+)
> and the NTFS-3G FUSE project for reference implementations.

> [!NOTE]
> **Cross-references:**
> - [TODO-040-Filesystem.md](TODO-040-Filesystem.md) — VFS layer that NTFS plugs into
> - [TODO-045-Win32-VFS-Compat.md](TODO-045-Win32-VFS-Compat.md) — Win32 path translation (`C:\` → mount point)
> - [TODO-041-AHCI.md](TODO-041-AHCI.md) — Block device layer (AHCI reads/writes)
> - [TODO-060-PCI.md](TODO-060-PCI.md) — AHCI controller discovery via PCI

---

## 1. Volume Mounting & Boot Sector

### 1.1 Boot Sector / BPB Parsing

**Prompt:** The NTFS Volume Boot Record (VBR) at Sector 0 contains the BIOS Parameter Block (BPB) with all constants needed to locate the MFT. Parse the boot sector to extract sector size, cluster size, total sectors, MFT location, and MFT record size. Validate the OEM ID (`"NTFS    "`), media descriptor (`0xF8`), and boot signature (`0x55AA`). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: boot sector BPB parsing"`. Add notes directly in this TODO section.

- [ ] Read Sector 0 of NTFS partition into buffer
- [ ] Validate OEM ID at offset `0x03`: must be `"NTFS    "` (8 bytes, space-padded)
- [ ] Parse BPB fields:
  - [ ] `BytesPerSector` (0x0B, 2 bytes) — typically 512 or 4096
  - [ ] `SectorsPerCluster` (0x0D, 1 byte) — multiplier for cluster size
  - [ ] `TotalSectors` (0x28, 8 bytes) — volume size in sectors
  - [ ] `LCN of $MFT` (0x30, 8 bytes) — **critical**: starting cluster of Master File Table
  - [ ] `LCN of $MFTMirr` (0x38, 8 bytes) — backup MFT mirror location
  - [ ] `ClustersPerFRS` (0x40, 4 bytes) — MFT record size (see negative-value trick)
  - [ ] `ClustersPerIndex` (0x44, 4 bytes) — INDX buffer size
  - [ ] `VolumeSerial` (0x48, 8 bytes) — unique volume identifier
- [ ] Compute derived values:
  - [ ] `ClusterSize = BytesPerSector × SectorsPerCluster` (e.g., 512 × 8 = 4096)
  - [ ] `MFT_ByteOffset = LCN_of_MFT × ClusterSize`
  - [ ] `RecordSize`: if ClustersPerFRS < 0, then `2^abs(ClustersPerFRS)` (typically 1024)
- [ ] Validate boot signature `0x55AA` at offset `0x1FE`
- [ ] Validate checksum at offset `0x50` (additive checksum of preceding 32-bit words)
- [ ] Store all values in `ntfs_volume_t` structure
- [ ] Log: `[NTFS] Volume: 512B sectors, 4KB clusters, MFT@LCN 175376, 1024B records`
- [ ] Commit: `"ntfs: boot sector BPB parsing"`

### 1.2 $MFTMirr Validation

**Prompt:** The MFT Mirror (`$MFTMirr`) at the LCN specified in the BPB contains backup copies of the first 4 MFT records (`$MFT`, `$MFTMirr`, `$LogFile`, `$Volume`). On mount, compare the mirror against the primary MFT to detect corruption. If the primary MFT is damaged, fall back to the mirror. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: MFT mirror validation"`. Add notes directly in this TODO section.

- [ ] Read first 4 records from `$MFTMirr` location
- [ ] Compare byte-for-byte against first 4 records of `$MFT`
- [ ] If mismatch detected: log warning, use mirror if primary is corrupt
- [ ] If both corrupt: fail mount with `STATUS_DISK_CORRUPT_ERROR`
- [ ] Log: `[NTFS] MFT Mirror verified: 4 records match`
- [ ] Commit: `"ntfs: MFT mirror validation"`

---

## 2. MFT Record Parsing

### 2.1 Record Header & Update Sequence Array (Fixup)

**Prompt:** Every MFT File Record Segment (FRS) is 1024 bytes, starting with a `"FILE"` magic number. Before trusting *any* data in the record, the Update Sequence Array (USA / fixup) must be verified and applied. This prevents sector-tearing corruption where only half the record was written to disk. The same fixup logic applies to INDX buffers (`"INDX"` magic). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: MFT record header and fixup"`. Add notes directly in this TODO section.

- [ ] Read 1024-byte MFT record into buffer
- [ ] Validate magic number at offset 0x00:
  - [ ] `"FILE"` — valid MFT record
  - [ ] `"BAAD"` — corrupted record (skip)
  - [ ] `"INDX"` — index buffer (separate path)
- [ ] Parse record header:
  - [ ] USA offset (0x04, 2 bytes) and USA size in words (0x06, 2 bytes)
  - [ ] $LogFile sequence number (0x08, 8 bytes)
  - [ ] Sequence number (0x10, 2 bytes) — increments on delete/realloc
  - [ ] Hard link count (0x12, 2 bytes)
  - [ ] First attribute offset (0x14, 2 bytes) — typically 0x38
  - [ ] Record flags (0x16, 2 bytes): bit 0 = in-use, bit 1 = directory
  - [ ] Used size (0x18, 4 bytes) and allocated size (0x1C, 4 bytes)
  - [ ] Base record reference (0x20, 8 bytes) — 0 if this IS the base record
- [ ] Implement `ntfs_apply_fixup(buffer, record_size)`:
  - [ ] Extract USN (Update Sequence Number) = first 2 bytes at USA offset
  - [ ] For each sector in the record (record_size / sector_size):
    - [ ] Check last 2 bytes of sector match USN — if not, sector tear detected → error
    - [ ] Replace last 2 bytes with original data from USA array
  - [ ] Record is now safe to parse
- [ ] Implement reverse fixup for writes:
  - [ ] Increment USN (skip 0x0000)
  - [ ] Save original sector-end bytes into USA
  - [ ] Stamp USN at sector boundaries
- [ ] Commit: `"ntfs: MFT record header and fixup"`

### 2.2 Attribute Walking

**Prompt:** After fixup, the MFT record contains a sequence of variable-length attributes starting at the first attribute offset. Walk the attribute chain until the `0xFFFFFFFF` end marker. For each attribute, parse the common header (type, length, resident flag, name) and dispatch to type-specific parsers. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: attribute walker"`. Add notes directly in this TODO section.

- [ ] Start at first attribute offset from record header (typically 0x38)
- [ ] For each attribute, parse common header (16 bytes):
  - [ ] Type ID (0x00, 4 bytes): 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xA0, 0xB0, 0xC0, 0x100
  - [ ] Total length (0x04, 4 bytes) — used to advance to next attribute
  - [ ] Non-resident flag (0x08, 1 byte): 0 = resident, 1 = non-resident
  - [ ] Name length (0x09, 1 byte) and name offset (0x0A, 2 bytes)
  - [ ] Attribute flags (0x0C, 2 bytes): compressed, encrypted, sparse
  - [ ] Attribute ID (0x0E, 2 bytes)
- [ ] If resident: parse content offset (0x14, 2 bytes) and content length (0x10, 4 bytes)
- [ ] If non-resident: parse data run mapping (see §3)
- [ ] Stop when type == `0xFFFFFFFF` (end marker)
- [ ] Handle named attributes (name length > 0): extract UTF-16 name for ADS support
- [ ] Commit: `"ntfs: attribute walker"`

---

## 3. Data Run Decoding (Non-Resident Files)

### 3.1 Run-List Decoder

**Prompt:** Non-resident attributes store a compact run-list that maps Virtual Cluster Numbers (VCNs) to physical Logical Cluster Numbers (LCNs). Each run is encoded as: a header byte (low nibble = length field size, high nibble = offset field size), followed by the length (unsigned) and offset (signed, relative to previous LCN). A header of `0x00` terminates the list. This decoder is the most critical piece of the NTFS driver — every file read depends on it. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: data run decoder"`. Add notes directly in this TODO section.

- [ ] Implement `ntfs_decode_runlist(data, &runs, &num_runs)`:
  - [ ] Initialize `current_lcn = 0` (first offset is absolute from volume start)
  - [ ] Loop until header byte == `0x00`:
    - [ ] Split header byte: `len_size = header & 0x0F`, `off_size = (header >> 4) & 0x0F`
    - [ ] Read `len_size` bytes as unsigned integer → run length (in clusters)
    - [ ] Read `off_size` bytes as **signed** integer → relative LCN offset
    - [ ] `current_lcn += relative_offset` → absolute LCN for this run
    - [ ] Store: `{ vcn_start, lcn_start, length }`
  - [ ] Handle **sparse runs**: `off_size == 0` means no physical allocation (return zeros)
- [ ] Implement `ntfs_vcn_to_lcn(runs, vcn)` → physical LCN lookup
- [ ] Implement `ntfs_read_nonresident(attr, offset, length, buffer)`:
  - [ ] Convert byte offset to VCN
  - [ ] Look up LCN via run-list
  - [ ] Issue disk read at `LCN × ClusterSize`
  - [ ] Handle reads spanning multiple runs
- [ ] Handle compressed files (stretch):
  - [ ] 16-cluster compression units
  - [ ] Pattern: N physical clusters + (16−N) sparse → decompress with LZ77
- [ ] Commit: `"ntfs: data run decoder"`

---

## 4. Core Attribute Parsers

### 4.1 $STANDARD_INFORMATION (0x10)

**Prompt:** Always resident. Contains NTFS timestamps (100-nanosecond intervals since Jan 1, 1601), DOS file permission flags, and ownership info. Parse the 4 MAC timestamps and permission flags. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: $STANDARD_INFORMATION parser"`. Add notes directly in this TODO section.

- [ ] Parse always-resident attribute:
  - [ ] Creation time (0x00, 8 bytes) — NTFS timestamp (100ns since 1601-01-01)
  - [ ] Modification time (0x08, 8 bytes)
  - [ ] MFT changed time (0x10, 8 bytes)
  - [ ] Access time (0x18, 8 bytes)
  - [ ] DOS permission flags (0x20, 4 bytes):
    - [ ] `0x0001` Read-Only, `0x0002` Hidden, `0x0004` System
    - [ ] `0x0020` Archive, `0x0800` Compressed, `0x4000` Encrypted
  - [ ] Owner ID (0x30, 4 bytes) — Windows 2000+
  - [ ] Security ID (0x34, 4 bytes) — index into `$Secure`
- [ ] Implement `ntfs_timestamp_to_unix(ntfs_ts)`:
  - [ ] Subtract epoch offset (11644473600 seconds between 1601 and 1970)
  - [ ] Divide by 10,000,000 to convert 100ns intervals to seconds
- [ ] Map DOS flags to VFS `stat` mode bits
- [ ] Commit: `"ntfs: $STANDARD_INFORMATION parser"`

### 4.2 $FILE_NAME (0x30)

**Prompt:** Always resident. Contains the UTF-16 filename, parent directory MFT reference, duplicated timestamps, file sizes, and namespace flag. A single file may have multiple `$FILE_NAME` attributes (Win32 + DOS 8.3 short name, or hard links). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: $FILE_NAME parser"`. Add notes directly in this TODO section.

- [ ] Parse always-resident attribute:
  - [ ] Parent directory MFT reference (0x00, 8 bytes):
    - [ ] Lower 6 bytes = parent inode number
    - [ ] Upper 2 bytes = sequence number (for stale-reference detection)
  - [ ] Duplicated timestamps (0x08–0x27, 4×8 bytes) — same format as $STANDARD_INFORMATION
  - [ ] Allocated size (0x28, 8 bytes) and real size (0x30, 8 bytes)
  - [ ] Flags (0x38, 4 bytes) — mirrors DOS permissions
  - [ ] Name length in characters (0x40, 1 byte)
  - [ ] Namespace (0x41, 1 byte):
    - [ ] `0x00` POSIX (case-sensitive)
    - [ ] `0x01` Win32 (case-insensitive)
    - [ ] `0x02` DOS (8.3 short name)
    - [ ] `0x03` Win32+DOS (naturally compliant)
  - [ ] Filename (0x42, 2×N bytes) — UTF-16LE, NOT null-terminated
- [ ] Prefer Win32 (0x01) or Win32+DOS (0x03) namespace for display
- [ ] Convert UTF-16LE filename to kernel string format
- [ ] Handle multiple $FILE_NAME attributes (hard links)
- [ ] Commit: `"ntfs: $FILE_NAME parser"`

### 4.3 $DATA (0x80) — File Reading

**Prompt:** The `$DATA` attribute holds the actual file contents. If resident, the data is inline in the MFT record (tiny files < ~700 bytes). If non-resident, decode the run-list (§3.1) to read physical clusters. Files can have multiple named `$DATA` attributes (Alternate Data Streams). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: $DATA file reading"`. Add notes directly in this TODO section.

- [ ] If resident (Non-Resident Flag == 0):
  - [ ] Read inline data from content offset, content length bytes
  - [ ] Return directly — no disk I/O needed
- [ ] If non-resident (Non-Resident Flag == 1):
  - [ ] Parse non-resident header:
    - [ ] Starting VCN (0x10, 8 bytes) and last VCN (0x18, 8 bytes)
    - [ ] Data runs offset (0x20, 2 bytes) — where run-list starts
    - [ ] Allocated size (0x28, 8 bytes) and real size (0x30, 8 bytes)
    - [ ] Initialized size (0x38, 8 bytes) — data beyond this is zeros
  - [ ] Decode run-list using §3.1 decoder
  - [ ] For read(offset, length): translate to VCN → LCN → disk read
- [ ] Handle Alternate Data Streams (ADS):
  - [ ] Named $DATA attributes: name identifies the stream (e.g., `:Zone.Identifier`)
  - [ ] Enumerate streams for Win32 `FindFirstStreamW` compatibility
- [ ] Commit: `"ntfs: $DATA file reading"`

### 4.4 $ATTRIBUTE_LIST (0x20) — Extension Records

**Prompt:** When a file's attributes overflow a single 1024-byte MFT record, NTFS creates extension records and links them via a `$ATTRIBUTE_LIST`. This list maps attribute types to MFT references of the extension records holding them. Required for highly fragmented files or files with many named streams. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: $ATTRIBUTE_LIST parser"`. Add notes directly in this TODO section.

- [ ] Detect presence of $ATTRIBUTE_LIST (type 0x20) in base record
- [ ] Parse list entries (variable length):
  - [ ] Attribute type (0x00, 4 bytes)
  - [ ] Entry length (0x04, 2 bytes)
  - [ ] Name length (0x06, 1 byte) and name offset (0x07, 1 byte)
  - [ ] Starting VCN (0x08, 8 bytes) — for split data runs
  - [ ] MFT reference (0x10, 8 bytes) — inode of extension record
- [ ] When looking up an attribute: check $ATTRIBUTE_LIST first
- [ ] If attribute is in extension record: read that MFT record and parse
- [ ] Handle $ATTRIBUTE_LIST itself being non-resident (extreme fragmentation)
- [ ] Commit: `"ntfs: $ATTRIBUTE_LIST parser"`

---

## 5. Directory Traversal (B+ Tree Indexing)

### 5.1 $INDEX_ROOT (0x90) — Small Directories

**Prompt:** Directories in NTFS use B+ tree indexing for O(log n) file lookup. The `$INDEX_ROOT` attribute (always resident, named `$I30`) contains the root node with sorted index entries. Each index entry embeds a copy of the file's `$FILE_NAME` attribute and the target MFT reference. For small directories (few files), all entries fit in $INDEX_ROOT. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: B+ tree INDEX_ROOT"`. Add notes directly in this TODO section.

- [ ] Parse $INDEX_ROOT (type 0x90, name `$I30`):
  - [ ] Attribute type of indexed content (always 0x30 = $FILE_NAME for directories)
  - [ ] Collation rule (0x04, 4 bytes) — `COLLATION_FILE_NAME` for case-insensitive sort
  - [ ] Index entry size (0x08, 4 bytes) — typically 4096
  - [ ] Clusters per INDX buffer (0x0C, 4 bytes)
- [ ] Parse node header:
  - [ ] Offset to first entry (relative to node header)
  - [ ] Total size of entries
  - [ ] Allocated size
  - [ ] Flags: bit 0 = has children (non-leaf)
- [ ] Walk index entries:
  - [ ] MFT reference (0x00, 8 bytes) — target file inode
  - [ ] Entry length (0x08, 2 bytes)
  - [ ] Content length (0x0A, 2 bytes) — size of embedded $FILE_NAME
  - [ ] Flags (0x0C, 1 byte): `0x01` = has sub-node, `0x02` = last entry
  - [ ] Embedded $FILE_NAME data (0x10+) — filename, timestamps, sizes
  - [ ] If has sub-node: VCN of child at `entry + entry_length - 8`
- [ ] Implement case-insensitive comparison using `$UpCase` table (inode 10)
- [ ] Commit: `"ntfs: B+ tree INDEX_ROOT"`

### 5.2 $INDEX_ALLOCATION (0xA0) — Large Directories

**Prompt:** When directories grow beyond what fits in $INDEX_ROOT, child B+ tree nodes are stored in INDX buffers (4 KB blocks) pointed to by the non-resident `$INDEX_ALLOCATION` attribute. Each INDX buffer has its own `"INDX"` magic, USA fixup, and sorted index entries. The `$BITMAP` (0xB0) tracks which INDX records are in use. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: B+ tree INDEX_ALLOCATION"`. Add notes directly in this TODO section.

- [ ] Parse $INDEX_ALLOCATION (type 0xA0, non-resident):
  - [ ] Decode run-list to locate INDX buffers on disk
- [ ] Read INDX buffer (typically 4096 bytes):
  - [ ] Validate magic `"INDX"` at offset 0x00
  - [ ] Apply USA fixup (same algorithm as MFT records)
  - [ ] Parse INDX header:
    - [ ] VCN of this buffer (0x10, 8 bytes)
    - [ ] Index entries offset (0x18, 4 bytes, relative to 0x18)
    - [ ] Index entries size (0x1C, 4 bytes)
    - [ ] Has-children flag (0x24, 1 byte)
  - [ ] Walk index entries (same format as §5.1)
- [ ] Parse $BITMAP (type 0xB0) — bitfield of active INDX records
- [ ] Implement full B+ tree traversal:
  - [ ] Alphabetical comparison against sorted entries
  - [ ] If target < entry and entry has sub-node → descend to child VCN
  - [ ] If target == entry → return MFT reference (file found)
  - [ ] If last entry (flag 0x02) with sub-node → descend
- [ ] Commit: `"ntfs: B+ tree INDEX_ALLOCATION"`

### 5.3 Path Resolution

**Prompt:** Resolve a full path like `C:\Windows\System32\kernel32.dll` by walking the directory tree from the root directory (inode 5) through each path component. This is the entry point for VFS `open()` / `lookup()` operations. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: path resolution"`. Add notes directly in this TODO section.

- [ ] Start at root directory (MFT inode 5)
- [ ] Split path into components: `["Windows", "System32", "kernel32.dll"]`
- [ ] For each component:
  - [ ] Read MFT record for current directory
  - [ ] Search $INDEX_ROOT for matching filename (case-insensitive via $UpCase)
  - [ ] If not found in root: descend to $INDEX_ALLOCATION child nodes
  - [ ] Extract MFT reference of matched entry
  - [ ] Verify sequence number matches (stale reference detection)
- [ ] Return final MFT inode for the target file
- [ ] Wire to VFS `lookup` operation
- [ ] Commit: `"ntfs: path resolution"`

---

## 6. System File Access

### 6.1 $Volume (Inode 3) — Volume Metadata

**Prompt:** Parse the `$Volume` system file to read volume name, NTFS version, and dirty flag. The dirty flag indicates whether the volume was cleanly unmounted — if dirty, the driver should mount read-only until `chkdsk` runs. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: $Volume metadata"`. Add notes directly in this TODO section.

- [ ] Read MFT record for inode 3 ($Volume)
- [ ] Parse $VOLUME_NAME attribute (0x60) — UTF-16 volume label
- [ ] Parse $VOLUME_INFORMATION attribute (0x70):
  - [ ] Major version (1 byte) and minor version (1 byte)
  - [ ] Volume flags: dirty bit, resize in progress, upgrade in progress
- [ ] NTFS version check: must be 3.1 (Windows XP+) for full feature support
- [ ] If dirty flag set: mount read-only, log warning
- [ ] Log: `[NTFS] Volume "Windows" v3.1, clean`
- [ ] Commit: `"ntfs: $Volume metadata"`

### 6.2 $Bitmap (Inode 6) — Cluster Allocation Map

**Prompt:** The master volume bitmap (`$Bitmap`, inode 6) tracks cluster allocation — each bit represents one cluster (1 = allocated, 0 = free). Required for write operations and free-space reporting. Cache the bitmap in memory for fast allocation lookups. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: cluster allocation bitmap"`. Add notes directly in this TODO section.

- [ ] Read $Bitmap (inode 6) $DATA attribute — non-resident, may be large
- [ ] Cache bitmap in memory (use `pmm_alloc_contiguous()` — can be several MB)
- [ ] Implement `ntfs_cluster_is_free(lcn)` — check bit at position lcn
- [ ] Implement `ntfs_find_free_clusters(count)` — find contiguous free run
- [ ] Implement `ntfs_alloc_clusters(lcn, count)` — set bits to 1
- [ ] Implement `ntfs_free_clusters(lcn, count)` — clear bits to 0
- [ ] Calculate and report: total clusters, free clusters, used percentage
- [ ] Log: `[NTFS] Bitmap: 262144 clusters, 45231 free (17%)`
- [ ] Commit: `"ntfs: cluster allocation bitmap"`

### 6.3 $UpCase (Inode 10) — Case Folding Table

**Prompt:** The `$UpCase` table (inode 10) is a 128 KB mapping of all UTF-16 code points to their uppercase equivalents. Required for case-insensitive filename comparisons in B+ tree traversal. Load on mount and keep in memory. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: $UpCase table"`. Add notes directly in this TODO section.

- [ ] Read $UpCase (inode 10) $DATA attribute — 128 KB (65536 × 2 bytes)
- [ ] Allocate 128 KB via `pmm_alloc_contiguous()`
- [ ] Load full table into memory on mount
- [ ] Implement `ntfs_upcase(uint16_t ch)` → uppercase equivalent
- [ ] Implement `ntfs_name_compare(name1, len1, name2, len2)`:
  - [ ] Up-case both names using loaded table
  - [ ] Compare character by character
  - [ ] Return <0, 0, >0 for sort ordering
- [ ] Used by B+ tree traversal for case-insensitive lookup
- [ ] Commit: `"ntfs: $UpCase table"`

---

## 7. VFS Integration

### 7.1 NTFS VFS Operations

**Prompt:** Register NTFS as a filesystem type with the VFS layer. Implement the standard VFS operations: mount, unmount, open, read, readdir, stat, close. Map NTFS concepts to VFS abstractions: MFT inodes → VFS inodes, $FILE_NAME → VFS dentry, $DATA → VFS file data. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: VFS integration"`. Add notes directly in this TODO section.

- [ ] Register `ntfs_fs_type` with VFS
- [ ] Implement `ntfs_mount(device)`:
  - [ ] Parse boot sector (§1.1)
  - [ ] Load MFT, $UpCase, $Bitmap
  - [ ] Check volume dirty flag
  - [ ] Return superblock
- [ ] Implement `ntfs_unmount()`:
  - [ ] Flush caches
  - [ ] Clear dirty flag (if read-write)
  - [ ] Free cached $UpCase and $Bitmap
- [ ] Implement `ntfs_lookup(dir, name)` → inode:
  - [ ] B+ tree search (§5)
- [ ] Implement `ntfs_read(file, offset, size, buffer)`:
  - [ ] Resident: copy from MFT inline data
  - [ ] Non-resident: run-list → disk read (§3)
- [ ] Implement `ntfs_readdir(dir, &entries)`:
  - [ ] Walk $INDEX_ROOT + $INDEX_ALLOCATION
  - [ ] Extract filename, size, timestamps from embedded $FILE_NAME
- [ ] Implement `ntfs_stat(inode, &stat)`:
  - [ ] Timestamps from $STANDARD_INFORMATION
  - [ ] Size from $DATA attribute
  - [ ] Permissions from DOS flags
- [ ] Commit: `"ntfs: VFS integration"`

---

## 8. Write Support *(Stretch)*

### 8.1 File Write Operations *(Stretch)*

**Prompt:** Implement NTFS write support: modifying existing file data, extending files (appending data runs), truncating files, and creating new files (allocating MFT records + clusters). Updates must maintain both $STANDARD_INFORMATION and $FILE_NAME timestamps, and update the $Bitmap. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: file write operations"`. Add notes directly in this TODO section.

- [ ] *(Stretch)* Implement `ntfs_write(file, offset, size, data)`:
  - [ ] Overwrite existing clusters (in-place for non-fragmented writes)
  - [ ] Extend file: allocate new clusters from $Bitmap, append data run
  - [ ] Update $DATA real size and initialized size
  - [ ] Update timestamps in both $STANDARD_INFORMATION and $FILE_NAME
- [ ] *(Stretch)* Implement `ntfs_create(dir, name, attrs)`:
  - [ ] Allocate free MFT record (scan $MFT $Bitmap)
  - [ ] Initialize record header, increment sequence number
  - [ ] Create $STANDARD_INFORMATION, $FILE_NAME, $DATA attributes
  - [ ] Insert filename into parent directory B+ tree index
  - [ ] Update parent directory timestamps
- [ ] *(Stretch)* Implement `ntfs_truncate(file, new_size)`:
  - [ ] Free excess clusters via $Bitmap
  - [ ] Shorten or remove data runs
  - [ ] Update sizes
- [ ] *(Stretch)* Implement `ntfs_delete(dir, name)`:
  - [ ] Mark MFT record flags = 0 (not in-use)
  - [ ] Free clusters via $Bitmap
  - [ ] Remove entry from parent B+ tree index
  - [ ] Decrement hard link count
- [ ] *(Stretch)* Apply USA fixup before every MFT write
- [ ] Commit: `"ntfs: file write operations"`

### 8.2 $LogFile Journaling *(Future)*

**Prompt:** NTFS uses `$LogFile` (inode 2) for transaction journaling — recording metadata operations before committing them, enabling recovery after crashes. The log contains redo/undo records for each transaction. On mount, if the volume is dirty, replay the log to restore consistency. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: $LogFile journaling"`. Add notes directly in this TODO section.

- [ ] *(Future)* Parse $LogFile structure:
  - [ ] Restart pages (contain latest checkpoint LSN)
  - [ ] Log record pages (circular buffer of redo/undo entries)
- [ ] *(Future)* On dirty mount: replay journal
  - [ ] Scan from last checkpoint LSN forward
  - [ ] Apply redo records for committed transactions
  - [ ] Apply undo records for uncommitted transactions
- [ ] *(Future)* For write operations: log before commit
  - [ ] Write intention to $LogFile
  - [ ] Commit actual metadata change
  - [ ] Mark transaction complete
- [ ] *(Future)* Clear dirty flag and update restart page on clean unmount
- [ ] Commit: `"ntfs: $LogFile journaling"`

---

## 9. Advanced Features *(Future)*

### 9.1 $Secure & ACL Support *(Future)*

**Prompt:** The `$Secure` system file (inode 9) stores all unique security descriptors centrally. Each file references its security descriptor via a Security ID in $STANDARD_INFORMATION. Parse ACLs (Access Control Lists) and SIDs (Security Identifiers) for Win32 permission checking. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: security descriptors"`. Add notes directly in this TODO section.

- [ ] *(Future)* Parse $Secure file indexes ($SII and $SDH)
- [ ] *(Future)* Look up security descriptor by Security ID from $STANDARD_INFORMATION
- [ ] *(Future)* Parse security descriptor: owner SID, group SID, DACL, SACL
- [ ] *(Future)* Parse ACE (Access Control Entry) within DACL:
  - [ ] Allow/Deny entries
  - [ ] Access mask: read, write, execute, delete, etc.
- [ ] *(Future)* Wire to Win32 `GetFileSecurity()` / `SetFileSecurity()`
- [ ] Commit: `"ntfs: security descriptors"`

### 9.2 Reparse Points & Symlinks *(Future)*

**Prompt:** The `$REPARSE_POINT` attribute (0xC0) implements symbolic links, directory junctions, and volume mount points. Each reparse point has a tag identifying its type and a data buffer containing the target path. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: reparse points"`. Add notes directly in this TODO section.

- [ ] *(Future)* Parse $REPARSE_POINT attribute (type 0xC0):
  - [ ] Reparse tag: `IO_REPARSE_TAG_SYMLINK`, `IO_REPARSE_TAG_MOUNT_POINT`
  - [ ] Data length and data buffer
- [ ] *(Future)* Symlinks: extract substitute name and print name (UTF-16)
- [ ] *(Future)* Junction points: extract target directory path
- [ ] *(Future)* Wire to VFS symlink resolution
- [ ] Commit: `"ntfs: reparse points"`

### 9.3 Compression & Encryption *(Future)*

**Prompt:** NTFS supports transparent LZ77 compression (16-cluster units) and EFS encryption. Compressed files have special data run patterns (N clusters + sparse padding to 16). Encrypted files use $LOGGED_UTILITY_STREAM (0x100) for key management. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: compression and encryption"`. Add notes directly in this TODO section.

- [ ] *(Future)* Detect compressed flag in attribute flags (0x0001)
- [ ] *(Future)* Identify 16-cluster compression units in run-list
- [ ] *(Future)* Implement LZNT1 decompression (NTFS variant of LZ77)
- [ ] *(Future)* Detect encrypted flag (0x4000 in $STANDARD_INFORMATION)
- [ ] *(Future)* Parse $LOGGED_UTILITY_STREAM for EFS key material
- [ ] Commit: `"ntfs: compression and encryption"`

---

## Priority Order

| Priority | Section                                | Description                                         |
| -------- | -------------------------------------- | --------------------------------------------------- |
| 🔴 P0    | 1.1 Boot Sector / BPB Parsing          | Foundation — locates MFT on disk                    |
| 🔴 P0    | 2.1 Record Header & Fixup (USA)        | Must verify before trusting ANY MFT data            |
| 🔴 P0    | 2.2 Attribute Walking                  | Parse the attribute chain inside MFT records        |
| 🔴 P0    | 3.1 Run-List Decoder                   | Read non-resident file data from physical clusters  |
| 🟠 P1    | 4.1 $STANDARD_INFORMATION             | Timestamps and permissions                          |
| 🟠 P1    | 4.2 $FILE_NAME                        | Filename, parent reference, namespace               |
| 🟠 P1    | 4.3 $DATA (File Reading)              | Actual file content access                          |
| 🟠 P1    | 5.1 $INDEX_ROOT (Small Dirs)          | B+ tree root for directory listing                  |
| 🟠 P1    | 5.2 $INDEX_ALLOCATION (Large Dirs)    | INDX buffers for big directories                    |
| 🟠 P1    | 5.3 Path Resolution                   | Full path → inode lookup                            |
| 🟠 P1    | 6.1 $Volume Metadata                  | Version check and dirty flag                        |
| 🟠 P1    | 6.3 $UpCase Table                     | Case-insensitive comparison for B+ tree             |
| 🟡 P2    | 1.2 $MFTMirr Validation               | Corruption recovery                                 |
| 🟡 P2    | 4.4 $ATTRIBUTE_LIST                   | Extension records for fragmented files              |
| 🟡 P2    | 6.2 Cluster Allocation Bitmap         | Free space tracking (needed for writes)             |
| 🟡 P2    | 7.1 VFS Integration                   | Plug into kernel VFS layer                          |
| 🟢 P3    | 8.1 File Write Operations             | Create, write, truncate, delete                     |
| 🟢 P3    | 9.1 $Secure & ACLs                    | Windows security descriptors                        |
| 🟢 P3    | 9.2 Reparse Points & Symlinks         | Symbolic links and junctions                        |
| 🔵 P4    | 8.2 $LogFile Journaling               | Crash recovery via transaction log                  |
| 🔵 P4    | 9.3 Compression & Encryption          | LZNT1 and EFS support                               |

---

## OS Comparison

| Feature                          | 🪟 Windows 11                           | 🐧 Linux 6.x                        | 🚀 Impossible OS                          |
| -------------------------------- | --------------------------------------- | ------------------------------------ | ----------------------------------------- |
| NTFS read support                | ✅ Native (ntfs.sys)                     | ✅ ntfs3 (5.15+)                     | ⬜ §1–5 — not yet implemented             |
| NTFS write support               | ✅ Full                                  | ✅ ntfs3 full r/w                    | ⬜ §8.1 P3 Stretch                        |
| MFT parsing                      | ✅ Full                                  | ✅ Full                              | ⬜ §2.1–2.2 P0                            |
| Data run decoding                | ✅ Full                                  | ✅ Full                              | ⬜ §3.1 P0                                |
| B+ tree directory indexing       | ✅ Full                                  | ✅ Full                              | ⬜ §5.1–5.2 P1                            |
| Update Sequence Array (fixup)    | ✅ Full                                  | ✅ Full                              | ⬜ §2.1 P0                                |
| $LogFile journaling              | ✅ Full                                  | ✅ ntfs3 replay                      | ⬜ §8.2 P4 Future                         |
| Case-insensitive via $UpCase     | ✅ Full                                  | ✅ Full                              | ⬜ §6.3 P1                                |
| ACLs / Security descriptors     | ✅ Full (NTFS permissions)               | ✅ ntfs3 ACL support                 | ⬜ §9.1 P3 Future                         |
| Alternate Data Streams           | ✅ Native                                | ✅ ntfs3 xattr                       | ⬜ §4.3 P1                                |
| Reparse points / symlinks        | ✅ Full                                  | ✅ ntfs3                             | ⬜ §9.2 P3 Future                         |
| LZNT1 compression                | ✅ Transparent                           | ✅ ntfs3                             | ⬜ §9.3 P4 Future                         |
| EFS encryption                   | ✅ Full                                  | ❌ No kernel EFS                     | ⬜ §9.3 P4 Future                         |
| Sparse files                     | ✅ Full                                  | ✅ Full                              | ⬜ §3.1 (sparse runs)                     |
| Hard links                       | ✅ Full                                  | ✅ Full                              | ⬜ §4.2 (multi $FILE_NAME)                |
| $MFTMirr recovery                | ✅ chkdsk                                | ✅ ntfsfix                           | ⬜ §1.2 P2                                |
| Cluster allocation bitmap        | ✅ Full                                  | ✅ Full                              | ⬜ §6.2 P2                                |
