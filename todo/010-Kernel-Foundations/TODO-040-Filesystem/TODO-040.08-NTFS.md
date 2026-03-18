# 040.08-NTFS — New Technology File System (Read-Only Driver)

> **Goal:** Implement a production-grade, read-only NTFS 3.1 driver for
> Impossible OS. The driver must parse the BIOS Parameter Block (BPB),
> locate and read the Master File Table (MFT), apply Update Sequence
> Array (fixup) integrity checks, decode resident and non-resident
> attributes, execute data run decoding for file cluster retrieval,
> and traverse B+ tree directory indexes. This enables reading files
> from Windows partitions — critical for dual-boot interoperability.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for MFT record buffers (1 KB each),
> INDX buffers (4 KB each), and data run cluster reads. `kmalloc` is ONLY for
> small kernel structs (≤ 4 KB). See `rules.md` Known Gotchas.

> [!WARNING]
> **Read-Only First.** NTFS write support is extremely complex due to journaling
> (`$LogFile`), MFT Zone protection, B+ tree rebalancing, and Update Sequence
> Array regeneration. This TODO covers **read-only** access. Write support is
> a future P3 extension.

> [!IMPORTANT]
> **Spec Reference:** All offsets, field layouts, algorithms, and data structures
> reference the [NTFS 3.1 Specification](file:///home/derickpayne/impossible-os/specs/ntfs-3.1.md)
> in the repo at `specs/ntfs-3.1.md`.

---

## 1. Volume Boot Record & BPB Parsing

### 1.1 BIOS Parameter Block Extraction

**Prompt:** Read the first sector (LBA 0) of the NTFS partition into a 512-byte buffer. Validate the OEM ID at offset `0x03` as `"NTFS    "` (padded with spaces). Extract all critical BPB fields: Bytes Per Sector, Sectors Per Cluster, Total Sectors (64-bit at `0x28`), LCN of `$MFT` (offset `0x30`), LCN of `$MFTMirr` (offset `0x38`), Clusters Per File Record Segment (offset `0x40`), and Volume Serial (offset `0x48`). Handle the Clusters Per FRS quirk: if the value is negative, interpret as `2^|value|` (e.g., `0xF6` = −10 → 2^10 = 1024 bytes). Validate the boot signature `0x55 0xAA` at offset `0x1FE`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: BPB parsing and MFT location"`. Add notes directly in this TODO section.

- [ ] Read partition first sector (512 bytes or `dev->sector_size`)
- [ ] Validate OEM ID at `0x03`: must match `"NTFS    "` (8 bytes, space-padded)
- [ ] Extract `bytes_per_sector` at `0x0B` (2 bytes LE) — typically 512 or 4096
- [ ] Extract `sectors_per_cluster` at `0x0D` (1 byte) — typically 8
- [ ] Calculate `cluster_size = bytes_per_sector × sectors_per_cluster`
- [ ] Extract `total_sectors` at `0x28` (8 bytes LE, 64-bit)
- [ ] Extract `mft_lcn` at `0x30` (8 bytes LE) — Logical Cluster Number of `$MFT`
- [ ] Extract `mftmirr_lcn` at `0x38` (8 bytes LE) — LCN of `$MFTMirr`
- [ ] Extract `clusters_per_frs` at `0x40` (4 bytes, **signed interpretation**):
  - [ ] If positive: `frs_size = clusters_per_frs × cluster_size`
  - [ ] If negative: `frs_size = 2^|clusters_per_frs|` (e.g., `0xF6` → −10 → 1024 bytes)
- [ ] Extract `clusters_per_index` at `0x44` (same signed interpretation)
- [ ] Extract `volume_serial` at `0x48` (8 bytes LE)
- [ ] Validate boot signature `0x55 0xAA` at offset `0x1FE`
- [ ] Calculate `mft_byte_offset = mft_lcn × cluster_size`
- [ ] Log: `[NTFS] Volume: %llu sectors, cluster=%u bytes, MFT at LCN %llu (byte %llu)`
- [ ] Log: `[NTFS] FRS size=%u bytes, INDX size=%u bytes`
- [ ] Store all values in `struct ntfs_volume` context
- [ ] Commit: `"ntfs: BPB parsing and MFT location"`

---

## 2. MFT Record Reading & Fixup

### 2.1 MFT Record Reader

**Prompt:** Implement reading a single MFT record by its inode number. Calculate the byte offset on disk: `mft_byte_offset + (inode × frs_size)`. Read `frs_size` bytes (typically 1024) from the block device. Parse the record header: magic number (`"FILE"`), USA offset, USA size, sequence number, flags, first attribute offset. Reject records with magic `"BAAD"` or unrecognized magic. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: MFT record reader"`. Add notes directly in this TODO section.

- [ ] Implement `ntfs_read_mft_record(vol, inode, buffer)`:
  - [ ] Calculate disk byte offset: `vol->mft_byte_offset + (inode × vol->frs_size)`
  - [ ] Convert byte offset to LBA: `byte_offset / vol->bytes_per_sector`
  - [ ] Read `frs_size / sector_size` sectors via `blkdev_read()`
  - [ ] Allocate record buffer via `pmm_alloc_contiguous()` (1024 bytes)
- [ ] Parse record header at offsets:
  - [ ] `0x00`: Magic number — must be `"FILE"` (0x454C4946 LE)
  - [ ] `0x04`: USA offset (2 bytes)
  - [ ] `0x06`: USA size in words (2 bytes)
  - [ ] `0x08`: `$LogFile` Sequence Number (8 bytes, store for journaling)
  - [ ] `0x10`: Sequence number (2 bytes — for stale reference detection)
  - [ ] `0x12`: Hard link count (2 bytes)
  - [ ] `0x14`: Offset to first attribute (2 bytes, typically `0x38`)
  - [ ] `0x16`: Flags — bit 0: in-use, bit 1: directory
  - [ ] `0x18`: Used size of record (4 bytes)
  - [ ] `0x1C`: Allocated size (4 bytes, should == `frs_size`)
  - [ ] `0x20`: Base record reference (8 bytes — 0 if this IS the base record)
- [ ] Reject: magic == `"BAAD"` → `NTFS_ERR_BAD_RECORD`
- [ ] Reject: flags bit 0 clear → record is deleted/free
- [ ] Log: `[NTFS] MFT Record %u: flags=0x%04X, attrs_at=0x%X, links=%d`
- [ ] Commit: `"ntfs: MFT record reader"`

### 2.2 Update Sequence Array (Fixup) Verification

**Prompt:** Before any attribute parsing can be trusted, the driver must apply the Update Sequence Array fixup mechanism to detect and repair sector tearing. For a 1024-byte record (2 physical sectors), the USA contains: the Update Sequence Number (USN, 2 bytes) followed by 2 replacement words (one per sector). The last 2 bytes of each 512-byte sector must match the USN — if not, the record is corrupt (sector tear). On match, restore the original bytes from the USA. This applies to both MFT `"FILE"` records and INDX buffers. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: Update Sequence Array fixup"`. Add notes directly in this TODO section.

- [ ] Implement `ntfs_apply_fixup(buffer, record_size, sector_size)`:
  - [ ] Read USA offset from `buffer[0x04]` (2 bytes)
  - [ ] Read USA size from `buffer[0x06]` (2 bytes, in 16-bit words)
  - [ ] Extract USN: first 2 bytes at `buffer[usa_offset]`
  - [ ] Calculate number of sectors: `record_size / sector_size`
  - [ ] For each sector `i` (0-based):
    - [ ] Check last 2 bytes: `buffer[sector_size * (i + 1) - 2]` must match USN
    - [ ] If mismatch → return `NTFS_ERR_FIXUP_FAILED` (sector tear detected)
    - [ ] Replace: copy `usa_array[i + 1]` (original bytes) → `buffer[sector_size * (i + 1) - 2]`
  - [ ] Record is now clean and ready for attribute parsing
- [ ] Handle variable sector sizes (512 and 4096)
- [ ] Apply fixup to both `"FILE"` and `"INDX"` records
- [ ] Log on fixup failure: `[NTFS] FIXUP FAILED: record at byte %llu, sector tear detected`
- [ ] Commit: `"ntfs: Update Sequence Array fixup"`

---

## 3. Attribute Parsing Engine

### 3.1 Attribute Iterator

**Prompt:** Implement the core attribute walking loop. Starting at the first attribute offset (from header field at `0x14`), read each attribute's common header (type ID at `0x00`, total length at `0x04`, non-resident flag at `0x08`). Advance by the total length to reach the next attribute. Stop when type ID == `0xFFFFFFFF` (`$END` marker). Handle named attributes (name length at `0x09`, name offset at `0x0A`, UTF-16LE). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: attribute iterator"`. Add notes directly in this TODO section.

- [ ] Implement `ntfs_attr_first(record)` → pointer to first attribute
- [ ] Implement `ntfs_attr_next(attr)` → advance by `attr->total_length`
- [ ] Implement `ntfs_attr_find(record, type_id)` → scan for specific attribute type
- [ ] Implement `ntfs_attr_find_named(record, type_id, name)` → match type + name
- [ ] Common header parsing (all attributes):
  - [ ] `0x00`: Type ID (4 bytes) — e.g., `0x10`, `0x30`, `0x80`
  - [ ] `0x04`: Total length of attribute including header (4 bytes)
  - [ ] `0x08`: Non-resident flag (1 byte: `0x00` = resident, `0x01` = non-resident)
  - [ ] `0x09`: Name length in chars (1 byte)
  - [ ] `0x0A`: Name offset (2 bytes)
  - [ ] `0x0C`: Flags — compressed (`0x0001`), encrypted (`0x4000`), sparse (`0x8000`)
  - [ ] `0x0E`: Attribute ID (2 bytes)
- [ ] Termination: stop at type ID `0xFFFFFFFF` (`$END`)
- [ ] Safety: stop if attribute offset exceeds record's used size
- [ ] For resident attributes: parse content length at `0x10`, content offset at `0x14`
- [ ] For non-resident attributes: defer to data run decoder (§4)
- [ ] Commit: `"ntfs: attribute iterator"`

### 3.2 `$STANDARD_INFORMATION` Decoder (0x10)

**Prompt:** Parse the always-resident `$STANDARD_INFORMATION` attribute to extract file timestamps and DOS permissions. NTFS timestamps are 64-bit values representing 100-nanosecond intervals since January 1, 1601 (Windows FILETIME). Extract: Creation Time (`0x00`), Modification Time (`0x08`), MFT Change Time (`0x10`), Last Access Time (`0x18`), and DOS Permissions flags (`0x20`). Map permissions: `0x0001` = Read-Only, `0x0002` = Hidden, `0x0004` = System, `0x0020` = Archive, `0x0800` = Compressed, `0x4000` = Encrypted. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: $STANDARD_INFORMATION decoder"`. Add notes directly in this TODO section.

- [ ] Locate `$STANDARD_INFORMATION` (type `0x10`) via attribute iterator
- [ ] Verify it is resident (must always be resident per NTFS spec)
- [ ] Extract timestamps (all 64-bit LE, FILETIME format):
  - [ ] `0x00`: Creation time (C time)
  - [ ] `0x08`: Modification time (A time — content altered)
  - [ ] `0x10`: MFT change time (M time — metadata altered)
  - [ ] `0x18`: Last access time (R time — often disabled)
- [ ] Implement `ntfs_filetime_to_unix(filetime)`:
  - [ ] Subtract Windows epoch delta: `11644473600` seconds (1601-01-01 → 1970-01-01)
  - [ ] Divide by 10,000,000 to convert 100ns intervals to seconds
- [ ] Extract DOS permissions at `0x20` (4 bytes):
  - [ ] `FILE_ATTRIBUTE_READONLY (0x0001)`
  - [ ] `FILE_ATTRIBUTE_HIDDEN (0x0002)`
  - [ ] `FILE_ATTRIBUTE_SYSTEM (0x0004)`
  - [ ] `FILE_ATTRIBUTE_ARCHIVE (0x0020)`
  - [ ] `FILE_ATTRIBUTE_COMPRESSED (0x0800)`
  - [ ] `FILE_ATTRIBUTE_ENCRYPTED (0x4000)`
- [ ] Populate `vfs_node` with converted timestamps and attribute flags
- [ ] Commit: `"ntfs: $STANDARD_INFORMATION decoder"`

### 3.3 `$FILE_NAME` Decoder (0x30)

**Prompt:** Parse the always-resident `$FILE_NAME` attribute to extract filenames, parent directory references, and filename namespaces. The parent reference at `0x00` is 8 bytes: low 6 bytes = MFT inode of parent, high 2 bytes = sequence number. The filename at `0x42` is UTF-16LE, NOT null-terminated — use the length byte at `0x40`. There may be multiple `$FILE_NAME` attributes per record (Win32 + DOS 8.3 names). Prefer the Win32 (`0x01`) or Win32/DOS (`0x03`) namespace name. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: $FILE_NAME decoder"`. Add notes directly in this TODO section.

- [ ] Locate all `$FILE_NAME` (type `0x30`) attributes (may be multiple)
- [ ] Parse parent directory reference at `0x00`:
  - [ ] Low 48 bits (6 bytes): parent MFT inode number
  - [ ] High 16 bits (2 bytes): sequence number
- [ ] Parse duplicated timestamps at `0x08`–`0x28` (creation, modification, MFT change, access)
- [ ] Parse allocated size at `0x28` and real size at `0x30`
- [ ] Parse flags at `0x38` (4 bytes — directory, compressed, hidden, etc.)
- [ ] Parse filename length at `0x40` (1 byte, in UTF-16 characters)
- [ ] Parse namespace at `0x41`:
  - [ ] `0x00` = POSIX (case-sensitive, any chars)
  - [ ] `0x01` = Win32 (case-insensitive, restricted chars)
  - [ ] `0x02` = DOS (8.3 short name)
  - [ ] `0x03` = Win32/DOS (compliant with both)
- [ ] Decode filename at `0x42`: read `name_length × 2` bytes as UTF-16LE → convert to ASCII
- [ ] Selection: prefer namespace `0x01` or `0x03` for display, fall back to `0x00`
- [ ] Ignore namespace `0x02` (DOS 8.3 short name) unless specifically requested
- [ ] Commit: `"ntfs: $FILE_NAME decoder"`

### 3.4 `$ATTRIBUTE_LIST` Handler (0x20)

**Prompt:** When a file's attributes overflow a single 1024-byte MFT record, NTFS creates extension records. The `$ATTRIBUTE_LIST` attribute in the base record maps which attributes live in which extension record. Parse the list entries: each has Type ID (`0x00`, 4B), Entry Length (`0x04`, 2B), name info, Starting VCN (`0x08`, 8B), and MFT Reference (`0x10`, 8B). When looking for an attribute in a record that has an `$ATTRIBUTE_LIST`, follow the references to extension records. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: $ATTRIBUTE_LIST handler"`. Add notes directly in this TODO section.

- [ ] Detect `$ATTRIBUTE_LIST` (type `0x20`) presence in base record
- [ ] Parse list entries (variable-length, walk by entry length at `0x04`):
  - [ ] `0x00`: Attribute Type ID to find (4 bytes)
  - [ ] `0x04`: Record entry length (2 bytes)
  - [ ] `0x06`: Name length (1 byte)
  - [ ] `0x07`: Name offset (1 byte)
  - [ ] `0x08`: Starting VCN (8 bytes) — for split non-resident attributes
  - [ ] `0x10`: MFT Reference (8 bytes) — which record holds the attribute
- [ ] For each entry: if MFT Reference ≠ base record → read extension record
- [ ] Integrate with `ntfs_attr_find()`: if base record has `$ATTRIBUTE_LIST`, search extensions
- [ ] Handle `$ATTRIBUTE_LIST` being non-resident itself (rare but possible)
- [ ] Cache extension records to avoid redundant reads
- [ ] Commit: `"ntfs: $ATTRIBUTE_LIST handler"`

### 3.5 `$SECURITY_DESCRIPTOR` Reader (0x50)

**Prompt:** Read the `$SECURITY_DESCRIPTOR` attribute to extract NTFS file permissions and ownership. In NTFS 3.0+, security descriptors are typically stored centrally in `$Secure` (inode 9) rather than inline, but older volumes and some files still have inline `0x50` attributes. Parse the descriptor to extract: Owner SID, Group SID, DACL (Discretionary Access Control List), and SACL (System Access Control List). For the read-only driver, we only need to READ these — routing them to `GetFileSecurity()` via the VFS compat layer (TODO-040.07 §2.2). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: security descriptor reader"`. Add notes directly in this TODO section.

- [ ] Locate `$SECURITY_DESCRIPTOR` (type `0x50`) in MFT record
- [ ] If not present: check `$STANDARD_INFORMATION` for Security ID → lookup in `$Secure`
- [ ] Parse self-relative security descriptor header:
  - [ ] `0x00`: Revision (1 byte, must be 1)
  - [ ] `0x02`: Control flags (2 bytes)
  - [ ] `0x04`: Owner SID offset (4 bytes)
  - [ ] `0x08`: Group SID offset (4 bytes)
  - [ ] `0x0C`: SACL offset (4 bytes, 0 if absent)
  - [ ] `0x10`: DACL offset (4 bytes, 0 if absent)
- [ ] Parse SID: `S-1-{authority}-{sub1}-{sub2}-...`
- [ ] Parse DACL: ACL header → walk ACEs (Access Control Entries)
  - [ ] Each ACE: type (allow/deny), flags, access mask, SID
- [ ] Expose via `vfs_ops.get_security()` for VFS compat layer
- [ ] Commit: `"ntfs: security descriptor reader"`

### 3.6 `$REPARSE_POINT` Reader (0xC0)

**Prompt:** NTFS reparse points implement symlinks, junctions (directory links), and mount points. The `$REPARSE_POINT` attribute (type `0xC0`) contains a reparse tag identifying the type and a data buffer with the target path. Parse: Reparse Tag (`0x00`, 4 bytes), Data Length (`0x04`, 2 bytes), and the type-specific payload. For symlinks (`IO_REPARSE_TAG_SYMLINK = 0xA000000C`): extract the substitute path (UTF-16LE). For junctions (`IO_REPARSE_TAG_MOUNT_POINT = 0xA0000003`): extract the target directory path. For the read-only driver, follow reparse points during path resolution. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: reparse point reader"`. Add notes directly in this TODO section.

- [ ] Locate `$REPARSE_POINT` (type `0xC0`) in MFT record
- [ ] Parse reparse data header:
  - [ ] `0x00`: Reparse Tag (4 bytes)
  - [ ] `0x04`: Reparse Data Length (2 bytes)
- [ ] Handle `IO_REPARSE_TAG_MOUNT_POINT (0xA0000003)` — junction:
  - [ ] `0x08`: Substitute Name Offset (2 bytes)
  - [ ] `0x0A`: Substitute Name Length (2 bytes)
  - [ ] `0x0C`: Print Name Offset (2 bytes)
  - [ ] `0x0E`: Print Name Length (2 bytes)
  - [ ] `0x10+`: Path buffer (UTF-16LE) — extract substitute name
  - [ ] Strip `\??\` prefix from substitute name → resolve as local path
- [ ] Handle `IO_REPARSE_TAG_SYMLINK (0xA000000C)` — symbolic link:
  - [ ] Same layout as junction but with additional Flags field at `0x10`
  - [ ] Flags `0x01` = relative symlink (resolve relative to containing directory)
- [ ] During path resolution: if directory has reparse point → follow target
- [ ] Set `vfs_node->flags |= VFS_SYMLINK` for reparse nodes
- [ ] Expose target via `ops->readlink()` for VFS compatibility
- [ ] Commit: `"ntfs: reparse point reader"`

---

## 4. Data Run Decoding

### 4.1 Run-List Decoder

**Prompt:** Implement the core data run decoder for non-resident `$DATA` attributes. This is the most algorithmically critical component — it translates Virtual Cluster Numbers (VCNs) to physical Logical Cluster Numbers (LCNs). Each run starts with a header byte: low nibble = length field size (L), high nibble = offset field size (F). Read L bytes as unsigned run length (cluster count), then F bytes as **signed** relative offset. The offset is relative to the PREVIOUS run's LCN (or LCN 0 for the first run). A header byte of `0x00` terminates the list. Handle sparse runs: F=0 means the clusters are all zeros (no disk I/O). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: data run decoder"`. Add notes directly in this TODO section.

- [ ] Implement `ntfs_decode_data_runs(attr, runs[], max_runs)`:
  - [ ] Locate run-list start: for non-resident attrs, at `attr->data_run_offset`
  - [ ] Parse non-resident header fields:
    - [ ] Starting VCN (8 bytes at attr_offset `0x10`)
    - [ ] Last VCN (8 bytes at attr_offset `0x18`)
    - [ ] Data runs offset (2 bytes at attr_offset `0x20`)
    - [ ] Allocated size (8 bytes at `0x28`)
    - [ ] Real (used) size (8 bytes at `0x30`)
    - [ ] Initialized size (8 bytes at `0x38`)
  - [ ] Walk the run-list:
    - [ ] Read header byte. If `0x00` → end of list
    - [ ] `length_size = header & 0x0F` (low nibble)
    - [ ] `offset_size = (header >> 4) & 0x0F` (high nibble)
    - [ ] Read next `length_size` bytes as **unsigned** integer → cluster count
    - [ ] Read next `offset_size` bytes as **signed** integer → relative offset
    - [ ] **Sign-extend** the offset: if high bit set, fill upper bytes with `0xFF`
    - [ ] `absolute_lcn = previous_lcn + relative_offset`
    - [ ] Store run: `{ lcn, length, vcn_start }`
    - [ ] Update `previous_lcn = absolute_lcn`
  - [ ] Handle sparse runs: `offset_size == 0` → LCN = SPARSE (return zeros)
- [ ] Log: `[NTFS] Run: VCN %llu → LCN %llu, %llu clusters`
- [ ] Commit: `"ntfs: data run decoder"`

### 4.2 File Data Reader

**Prompt:** Using the decoded run-list, implement reading file data by VCN range. Given a file offset and length, calculate which runs cover the requested range, translate VCNs to LCNs, and issue `blkdev_read()` for each run's cluster range. Handle sparse runs by filling the output buffer with zeros. Handle reads that span multiple runs. Handle reads within a single cluster (sub-cluster reads). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: file data reader"`. Add notes directly in this TODO section.

- [ ] Implement `ntfs_read_data(vol, runs, file_offset, length, buffer)`:
  - [ ] Convert `file_offset` to VCN: `vcn = file_offset / cluster_size`
  - [ ] Calculate offset within cluster: `cluster_offset = file_offset % cluster_size`
  - [ ] Find the run containing the target VCN
  - [ ] For each run covering the requested range:
    - [ ] If sparse (LCN == SPARSE) → `memset(buffer, 0, run_length × cluster_size)`
    - [ ] Else: `blkdev_read(dev, lcn × sectors_per_cluster, sectors, buffer)`
  - [ ] Handle partial cluster reads (start/end of request not cluster-aligned)
  - [ ] Handle reads spanning multiple runs (stitch runs together)
  - [ ] Cap read at file's `real_size` (actual data), not `allocated_size`
- [ ] Implement `ntfs_read_resident_data(attr, offset, length, buffer)`:
  - [ ] Direct memory copy from attribute's resident content
  - [ ] Content starts at `attr_base + content_offset` (from attr header at `0x14`)
- [ ] Auto-detect: if `$DATA` is resident → `ntfs_read_resident_data()`, else data runs
- [ ] Commit: `"ntfs: file data reader"`

---

## 5. Directory B+ Tree Traversal

### 5.1 `$INDEX_ROOT` Parser (0x90)

**Prompt:** Parse the always-resident `$INDEX_ROOT` attribute which forms the root node of the directory B+ tree. The root contains a small number of index entries sorted alphabetically. Each index entry has: MFT reference (8B at `0x00`), entry length (2B at `0x08`), stream length (2B at `0x0A`), flags (1B at `0x0C` — bit 0: has sub-node, bit 1: last entry), and a `$FILE_NAME` payload at `0x10`. If the entry has a sub-node (flag `0x01`), the last 8 bytes of the entry contain the VCN of the child INDX buffer. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: $INDEX_ROOT parser"`. Add notes directly in this TODO section.

- [ ] Locate `$INDEX_ROOT` (type `0x90`, named `$I30`) in directory's MFT record
- [ ] Parse index root header:
  - [ ] Attribute type being indexed (should be `0x30` = `$FILE_NAME`)
  - [ ] Collation rule (should be `0x01` = filename collation)
  - [ ] Index record size (typically 4096)
  - [ ] Clusters per index record
- [ ] Parse node header:
  - [ ] Offset to first index entry (relative to node header start)
  - [ ] Total size of index entries
  - [ ] Allocated size of index entries
  - [ ] Flags: `0x01` = has children (not a leaf)
- [ ] Walk index entries within the root:
  - [ ] `0x00`: MFT Reference (8 bytes — low 6 = inode, high 2 = sequence)
  - [ ] `0x08`: Entry length (2 bytes) — advance by this
  - [ ] `0x0A`: Stream (filename payload) length (2 bytes)
  - [ ] `0x0C`: Flags — `0x01` = has sub-node, `0x02` = last entry
  - [ ] `0x10`: `$FILE_NAME` payload (decode with §3.3 logic)
  - [ ] If flag `0x01`: read child VCN from last 8 bytes of entry
  - [ ] If flag `0x02`: last entry (sentinel, no filename), stop iteration
- [ ] Commit: `"ntfs: $INDEX_ROOT parser"`

### 5.2 INDX Buffer Reader (0xA0)

**Prompt:** When the B+ tree extends beyond the root, child nodes are stored as 4 KB INDX records pointed to by the `$INDEX_ALLOCATION` attribute's data runs. Read the INDX buffer at the specified VCN, validate its magic (`"INDX"`), apply fixup (§2.2 — INDX buffers also use USAs), and parse index entries. The INDX header has its own node header at offset `0x18` with the same entry layout as `$INDEX_ROOT`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: INDX buffer reader"`. Add notes directly in this TODO section.

- [ ] Locate `$INDEX_ALLOCATION` (type `0xA0`, named `$I30`) in directory record
- [ ] Decode its data runs to map VCN → LCN (reuse §4.1 decoder)
- [ ] Implement `ntfs_read_indx(vol, index_runs, vcn, buffer)`:
  - [ ] Translate VCN to LCN via run-list
  - [ ] Read `index_record_size` bytes (typically 4096) from disk
  - [ ] Validate magic: must be `"INDX"` (0x58444E49 LE)
  - [ ] Apply fixup (§2.2) — INDX uses USA just like FILE records
- [ ] Parse INDX node header at offset `0x18`:
  - [ ] `0x18 + 0x00`: Offset to first entry (4 bytes)
  - [ ] `0x18 + 0x04`: Total size of entries (4 bytes)
  - [ ] `0x18 + 0x08`: Allocated size (4 bytes)
  - [ ] `0x18 + 0x0C`: Flags — `0x01` = has children (not leaf)
- [ ] Walk index entries (same format as §5.1)
- [ ] Commit: `"ntfs: INDX buffer reader"`

### 5.3 Directory Lookup Algorithm

**Prompt:** Implement the full path resolution algorithm for NTFS. Starting at the Root Directory (inode 5), for each path component: search `$INDEX_ROOT` entries alphabetically (case-insensitive using uppercase comparison). If the target precedes an entry with a sub-node, descend to the child INDX buffer. Repeat until a match is found or the entry is definitively absent. Use the `$UpCase` table (inode 10) for case-insensitive comparison if available, or fall back to ASCII `towupper`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: directory lookup algorithm"`. Add notes directly in this TODO section.

- [ ] Implement `ntfs_lookup(vol, parent_inode, name)`:
  - [ ] Read parent's MFT record
  - [ ] Parse `$INDEX_ROOT` (`$I30`) — get sorted index entries
  - [ ] For each entry in root:
    - [ ] Compare `name` against entry's `$FILE_NAME` (case-insensitive)
    - [ ] If match → return entry's MFT reference (inode)
    - [ ] If `name < entry` and entry has sub-node → descend
    - [ ] If `name < entry` and no sub-node → `FILE_NOT_FOUND`
  - [ ] If descending: read child VCN from entry's last 8 bytes
  - [ ] Read INDX buffer at that VCN via `ntfs_read_indx()`
  - [ ] Repeat entry search within INDX buffer
  - [ ] Recurse until leaf node (no more children) → `FILE_NOT_FOUND`
- [ ] Implement `ntfs_resolve_path(vol, path)`:
  - [ ] Split path by `\` (or `/`)
  - [ ] Start at inode 5 (root directory)
  - [ ] For each component: `ntfs_lookup(vol, current_inode, component)`
  - [ ] Return final inode
- [ ] Case-insensitive comparison: uppercase both strings before comparing
- [ ] Handle multiple `$FILE_NAME` attributes per entry (prefer Win32 namespace)
- [ ] Commit: `"ntfs: directory lookup algorithm"`

### 5.4 Directory Enumeration (readdir)

**Prompt:** Implement enumerating all entries in an NTFS directory for `FindFirstFile`/`FindNextFile`. Walk the `$INDEX_ROOT` entries first, then recursively walk all INDX buffers from `$INDEX_ALLOCATION`. Use the `$BITMAP` attribute (type `0xB0`, named `$I30`) to determine which INDX VCNs are in-use. Skip the last sentinel entry (flag `0x02`). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: directory enumeration"`. Add notes directly in this TODO section.

- [ ] Implement `ntfs_readdir(vol, dir_inode, callback)`:
  - [ ] Read directory's MFT record
  - [ ] Walk `$INDEX_ROOT` entries → invoke callback for each (skip sentinel)
  - [ ] If root has children:
    - [ ] Read `$INDEX_ALLOCATION` data runs
    - [ ] Read `$BITMAP` (`$I30`) to find active INDX VCNs
    - [ ] For each active VCN: read INDX buffer, walk entries, invoke callback
    - [ ] Recurse into child nodes if entries have sub-node flag
  - [ ] Callback receives: filename, MFT inode, file size, timestamps, flags
- [ ] Handle directories with thousands of entries (many INDX buffers)
- [ ] Skip DOS 8.3 names (namespace `0x02`) — only enumerate Win32/POSIX names
- [ ] Commit: `"ntfs: directory enumeration"`

---

## 6. VFS Integration

### 6.1 NTFS VFS Driver Registration

**Prompt:** Register NTFS as a VFS filesystem driver. Implement the `vfs_ops` callbacks: `open`, `close`, `read`, `readdir`, `finddir`, `stat`. The `write`, `create`, `unlink`, `rename`, `mkdir`, `rmdir` callbacks return `NTFS_ERR_READ_ONLY` (read-only driver). Detect NTFS volumes during partition scanning by checking the OEM ID `"NTFS    "` in the boot sector. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: VFS driver registration"`. Add notes directly in this TODO section.

- [ ] Create `src/kernel/fs/ntfs.c` and `include/kernel/fs/ntfs.h`
- [ ] Define `struct ntfs_volume` — holds BPB data, MFT location, cluster size, etc.
- [ ] Implement `ntfs_detect(blkdev)` — read first sector, check OEM ID `"NTFS    "`
- [ ] Register with partition scanner: on MBR type `0x07` or GPT GUID `EBD0A0A2-...`
- [ ] Implement `vfs_ops` callbacks:
  - [ ] `ntfs_open(node)` — read MFT record, allocate file context
  - [ ] `ntfs_close(node)` — free file context
  - [ ] `ntfs_read(node, offset, size, buf)` — data run read or resident read
  - [ ] `ntfs_readdir(node, index)` — B+ tree enumeration
  - [ ] `ntfs_finddir(node, name)` — B+ tree lookup
  - [ ] `ntfs_stat(node, stat)` — populate from `$STANDARD_INFORMATION`
  - [ ] Write ops → return `-EROFS` (read-only filesystem)
- [ ] Auto-mount: assign drive letter on detection (e.g., `D:`)
- [ ] Log: `[NTFS] Mounted volume '%s' on drive %c: (%llu bytes)`
- [ ] Commit: `"ntfs: VFS driver registration"`

---

## 7. System File Access

### 7.1 System Metafile Readers

**Prompt:** Implement reading key NTFS system files needed for full operation. `$MFTMirr` (inode 1): read the first 4 mirrored MFT records for backup recovery. `$Volume` (inode 3): extract volume name and dirty flag (check if volume was cleanly unmounted — if dirty, log warning). `$Bitmap` (inode 6): read the cluster allocation bitmap for free space queries. `$UpCase` (inode 10): load the Unicode uppercase mapping table for case-insensitive comparison. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: system metafile readers"`. Add notes directly in this TODO section.

- [ ] Read `$Volume` (inode 3):
  - [ ] Extract `$VOLUME_NAME` attribute (0x60) → volume label
  - [ ] Extract `$VOLUME_INFORMATION` attribute (0x70) → NTFS version, flags
  - [ ] Check dirty flag: if set, log `[NTFS] WARNING: Volume was not cleanly unmounted`
  - [ ] Read-only driver: do NOT clear the dirty flag (Windows chkdsk will handle it)
- [ ] Read `$Bitmap` (inode 6):
  - [ ] Decode `$DATA` attribute (non-resident) → cluster bitmap
  - [ ] Count free/used clusters for `GetDiskFreeSpace()` support
  - [ ] Cache bitmap or compute stats lazily
- [ ] Read `$UpCase` (inode 10):
  - [ ] Load 128 KB uppercase mapping table into kernel memory
  - [ ] Use for case-insensitive filename comparison in B+ tree lookups
  - [ ] Fallback: ASCII-only `towupper` if `$UpCase` loading fails
- [ ] Read `$MFTMirr` (inode 1):
  - [ ] Compare first 4 records against `$MFT` for consistency
  - [ ] Log warning if mismatch detected
- [ ] Commit: `"ntfs: system metafile readers"`

---

## 8. Testing & Validation

### 8.1 NTFS Test Suite

**Prompt:** Create NTFS test disk images using host tools (`mkfs.ntfs` from ntfs-3g, or format from Windows). Test: basic file read, large file (multi-run data), deep directory (multi-level B+ tree), long filenames (≥ 200 chars), file with multiple `$FILE_NAME` attributes (Win32 + DOS), resident small files. Attach images via QEMU and verify the driver reads all files correctly. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"test: NTFS filesystem test suite"`. Add notes directly in this TODO section.

- [ ] Test image: small NTFS volume with files in root directory
  - [ ] Verify: BPB parsing, MFT location, root directory listing
- [ ] Test image: file with known content → read and compare
  - [ ] Create file with known 4 KB pattern, verify byte-exact read
- [ ] Test image: large fragmented file (>4 MB, multiple data runs)
  - [ ] Verify: run-list decoding, multi-run stitching
- [ ] Test image: deep directory tree (`A\B\C\D\E\file.txt`)
  - [ ] Verify: recursive path resolution through B+ tree
- [ ] Test image: directory with >100 files (forces INDX allocation)
  - [ ] Verify: INDX buffer reading, fixup, entry enumeration
- [ ] Test image: resident file (< 700 bytes, fits in MFT record)
  - [ ] Verify: resident data read (no data runs)
- [ ] Test image: long filename (200+ characters)
  - [ ] Verify: UTF-16LE decoding, correct length handling
- [ ] Test image: Windows system files (`C:\Windows\System32\kernel32.dll`)
  - [ ] Verify: real-world NTFS volume reading
- [ ] Test: dirty volume flag detection (unmount without clean shutdown)
  - [ ] Verify: warning logged, no write attempted
- [ ] QEMU flags: `-drive file=ntfs_test.img,format=raw,if=none,id=t0 -device virtio-blk-pci,drive=t0`
- [ ] Commit: `"test: NTFS filesystem test suite"`

---

## 9. Compressed File Reading (LZNT1)

### 9.1 LZNT1 Decompression Engine

**Prompt:** NTFS transparent compression uses LZNT1 (a variant of LZ77), applied to "compression units" of 16 clusters (typically 64 KB). When a file has `$DATA` attribute flag `0x0001` (compressed), the data runs contain a mix of stored (compressed) and sparse (all-zeros) runs. For each 16-cluster compression unit: if the run length on disk is < 16 clusters, the data is LZNT1-compressed — decompress it. If the run length == 16 clusters, the data is stored uncompressed. If the run is sparse (LCN == -1), the entire unit is zeros. Implement the LZNT1 decompression algorithm. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: LZNT1 decompression for compressed files"`. Add notes directly in this TODO section.

> [!NOTE]
> Windows reads compressed NTFS files natively. Linux `ntfs3` supports it in-kernel.
> Linux `ntfs-3g` (FUSE) supports read-only decompression. This is needed for reading
> Windows system files — `C:\Windows\` often contains compressed files.

- [ ] Detect compressed flag in `$DATA` attribute flags (`0x0001`)
- [ ] Read compression unit size: `2^(compression_unit_shift)` clusters (typically 2^4 = 16)
- [ ] For each compression unit in the data runs:
  - [ ] If run length == unit size → uncompressed, read directly
  - [ ] If run length < unit size → LZNT1-compressed, decompress
  - [ ] If run is sparse → fill with zeros
- [ ] Implement `ntfs_lznt1_decompress(src, src_len, dst, dst_len)`:
  - [ ] LZNT1 processes 4096-byte sub-blocks
  - [ ] Each sub-block: 2-byte header (bit 15 = compressed flag, bits 0–11 = size)
  - [ ] If compressed: walk tokens — literal bytes and (offset, length) back-references
  - [ ] Token format: high bit = 1 means back-reference, 0 means literal
  - [ ] Back-reference: variable-length offset and length fields (displacement bits depend on position)
- [ ] Integrate with `ntfs_read_data()`: transparently decompress on read
- [ ] Test: read a compressed file from a Windows NTFS volume, verify contents match
- [ ] Commit: `"ntfs: LZNT1 decompression for compressed files"`

---

## 10. Performance Optimization

### 10.1 MFT Record Cache

**Prompt:** Every path lookup and directory enumeration reads MFT records from disk. Implement an LRU cache for recently-accessed MFT records. Key: MFT inode number. Value: the parsed 1024-byte record buffer (already fixup-verified). This is especially important for directory traversal — looking up `C:\Users\Derickpayne\Documents\file.txt` reads MFT records for inodes 5 (root), `Users`, `Derickpayne`, `Documents`, and `file.txt`. Without caching, reading 100 files in the same directory re-reads the directory's MFT record 100 times. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: MFT record cache"`. Add notes directly in this TODO section.

- [ ] Define MFT cache: array of `{ inode, record_buffer, lru_timestamp }` (default 64 entries)
- [ ] On `ntfs_read_mft_record(vol, inode, buffer)`:
  - [ ] Check cache first — if hit, copy from cache, skip disk read
  - [ ] On miss: read from disk, apply fixup, store in cache (evict LRU if full)
- [ ] Invalidate cache entry if sequence number changes (stale reference)
- [ ] Pin critical records: inode 0 ($MFT), 5 (root) — never evict
- [ ] Telemetry: track hit/miss rate, log on mount: `[NTFS] MFT cache: %u entries, hit rate %.1f%%`
- [ ] Cache size configurable via Registry: `HKLM\SYSTEM\Storage\NTFS\MFTCacheSize`
- [ ] Commit: `"ntfs: MFT record cache"`

---

## 11. NTFS Volume Health Dashboard (🚀 Impossible OS Feature)

### 11.1 Volume Health Aggregation

**Prompt:** Aggregate NTFS volume health metrics into a single dashboard view in Disk Manager. Read: dirty flag from `$Volume`, bad cluster count from `$BadClus`, MFT Mirror consistency (`$MFTMirr` vs `$MFT` first 4 records), MFT fragmentation (number of data runs in `$MFT`'s own `$DATA` attribute — ideally 1 run = contiguous MFT), and free space from `$Bitmap`. Display a health score and per-metric status (✅/⚠️/❌). No OS provides this at-a-glance view. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: volume health dashboard"`. Add notes directly in this TODO section.

> [!TIP]
> **Competitive Edge:** Windows shows NTFS volume info spread across Properties → Tools → chkdsk.
> Linux has `ntfsinfo` but it's CLI-only and doesn't aggregate health. Impossible OS shows
> everything in one GUI panel: dirty flag, bad clusters, MFT fragmentation, mirror consistency,
> all with a computed health score. One-click "Check Disk" runs chkdsk-equivalent.

- [ ] Read dirty flag from `$Volume` (inode 3) → `$VOLUME_INFORMATION` flags
- [ ] Read `$BadClus` (inode 8) → count bad cluster entries in `$Bad` data attribute
- [ ] Compare `$MFTMirr` (inode 1) first 4 records against `$MFT` (inode 0)
  - [ ] Byte-exact comparison of records 0–3
  - [ ] Mismatch → `mirror_status = WARNING`
- [ ] Count `$MFT` data runs → run count > 1 means MFT is fragmented
  - [ ] 1 run = perfect ✅, 2–5 = normal ⚠️, 6+ = fragmented ❌
- [ ] Compute free space percentage from `$Bitmap` cluster bitmap
- [ ] Aggregate health score: all green = "Healthy", any warning = "Needs Attention", any red = "Unhealthy"
- [ ] Wire to Disk Manager: NTFS volume properties panel
- [ ] Commit: `"ntfs: volume health dashboard"`

### 11.2 Deleted File Recovery (Forensics Mode)

**Prompt:** NTFS marks deleted files by clearing the in-use bit (bit 0 of flags at `0x16`) but does NOT overwrite the MFT record. The filename, timestamps, and data runs remain intact until the record is reused. Implement a recovery scanner that walks the MFT for records with the in-use bit cleared that still have valid `$FILE_NAME` and `$DATA` attributes. Display recoverable files in a dedicated panel. Allow recovery by copying the data to a different volume. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ntfs: deleted file recovery"`. Add notes directly in this TODO section.

> [!TIP]
> **Competitive Edge:** Windows requires third-party tools (Recuva, R-Studio) for NTFS file
> recovery. Linux has `ntfsundelete` but it's CLI-only and not well-maintained.
> Impossible OS having built-in, GUI-based deleted file recovery is a major differentiator.

> [!WARNING]
> **Read-only operation.** Recovery copies data to a DIFFERENT volume — never write to the
> NTFS volume being scanned. This preserves forensic integrity.

- [ ] Implement `ntfs_scan_deleted(vol, callback)`:
  - [ ] Walk all MFT records (inode 0 to max based on `$MFT` data size / frs_size)
  - [ ] For each record: check magic == `"FILE"`, in-use bit CLEAR (flags & 0x01 == 0)
  - [ ] Parse `$FILE_NAME` → extract filename, parent, timestamps
  - [ ] Parse `$DATA` → check if data runs are still valid (clusters not reallocated)
  - [ ] Cluster validation: cross-reference against `$Bitmap` — if clusters now in-use, file may be partially overwritten
  - [ ] Callback: `{ filename, size, delete_time, recovery_confidence }`
- [ ] Recovery confidence levels:
  - [ ] **High** — all clusters still free in `$Bitmap`
  - [ ] **Medium** — some clusters reallocated (partial recovery possible)
  - [ ] **Low** — most/all clusters reallocated (likely corrupted)
- [ ] Implement `ntfs_recover_file(vol, deleted_inode, output_path)`:
  - [ ] Read data clusters via data runs (same as §4.2)
  - [ ] Write to output file on a different volume (IXFS, FAT32)
- [ ] Wire to Disk Manager: "Recover Deleted Files" button on NTFS volumes
  - [ ] Show list: filename, size, date deleted, confidence icon (🟢/🟡/🔴)
- [ ] Commit: `"ntfs: deleted file recovery"`

---

## Priority Order

| Priority | Section | Description |
|----------|---------|-------------|
| 🔴 P0 | 1.1 BPB Parsing | Foundation — locate MFT on disk |
| 🔴 P0 | 2.1 MFT Record Reader | Foundation — read any file's metadata |
| 🔴 P0 | 2.2 Fixup Verification | Integrity — must be done before ANY attribute parsing |
| 🔴 P0 | 3.1 Attribute Iterator | Foundation — walk attributes in MFT records |
| 🔴 P0 | 3.3 `$FILE_NAME` Decoder | Foundation — extract filenames |
| 🔴 P0 | 4.1 Run-List Decoder | Foundation — translate VCN → LCN for file reads |
| 🟠 P1 | 3.2 `$STANDARD_INFORMATION` | Metadata — timestamps and permissions |
| 🟠 P1 | 4.2 File Data Reader | Core feature — actually read file contents |
| 🟠 P1 | 5.1 `$INDEX_ROOT` Parser | Directory — root of B+ tree |
| 🟠 P1 | 5.2 INDX Buffer Reader | Directory — child nodes of B+ tree |
| 🟠 P1 | 5.3 Directory Lookup | Directory — path resolution (`C:\path\to\file`) |
| 🟠 P1 | 6.1 VFS Registration | Integration — make NTFS mountable |
| 🟡 P2 | 3.4 `$ATTRIBUTE_LIST` | Robustness — handle fragmented/overflowing MFT records |
| 🟡 P2 | 3.5 `$SECURITY_DESCRIPTOR` | Interop — read NTFS ACLs for GetFileSecurity |
| 🟡 P2 | 3.6 `$REPARSE_POINT` | Feature — follow symlinks and junctions |
| 🟡 P2 | 5.4 Directory Enumeration | Feature — `FindFirstFile`/`FindNextFile` support |
| 🟡 P2 | 7.1 System Metafiles | Feature — volume name, dirty flag, free space, $UpCase |
| 🟡 P2 | 9.1 LZNT1 Decompression | Interop — read compressed Windows system files |
| 🟡 P2 | 10.1 MFT Record Cache | Performance — avoid redundant disk reads |
| 🟢 P3 | 8.1 Test Suite | Quality — automated validation with test images |
| 🟢 P3 | 11.1 Health Dashboard ⭐ | **At-a-glance NTFS health** — no OS does this |
| 🟢 P3 | 11.2 Deleted File Recovery ⭐ | **Built-in forensic recovery** — Windows needs 3rd-party |

> [!NOTE]
> ⭐ = Feature where Impossible OS can be **superior** to both Windows and Linux.

---

## OS Comparison

| Feature | 🪟 Windows 11 (ntfs.sys) | 🐧 Linux (ntfs3 / ntfs-3g) | 🚀 Impossible OS |
| --------------------------------- | ---------------------------------- | ---------------------------------- | --------------------------------------- |
| BPB parsing | ✅ Native | ✅ Full | ⬜ §1.1 P0 |
| MFT record reading | ✅ Native | ✅ Full | ⬜ §2.1 P0 |
| Update Sequence Array (fixup) | ✅ Full | ✅ Full | ⬜ §2.2 P0 |
| Attribute parsing (all types) | ✅ All 14 types | ✅ All types | ⬜ §3.1–3.6 (core + security + reparse) |
| `$STANDARD_INFORMATION` | ✅ Full | ✅ Full | ⬜ §3.2 P1 |
| `$FILE_NAME` (multi-namespace) | ✅ Win32 + DOS + POSIX | ✅ Full | ⬜ §3.3 P0 |
| `$ATTRIBUTE_LIST` (extensions) | ✅ Full | ✅ Full | ⬜ §3.4 P2 |
| `$SECURITY_DESCRIPTOR` / ACLs | ✅ Full DACL/SACL | ✅ ntfs3 full / ntfs-3g limited | ⬜ §3.5 P2 |
| `$REPARSE_POINT` (symlinks)  | ✅ Full (symlinks, junctions) | ✅ ntfs3 full | ⬜ §3.6 P2 |
| Data run decoding | ✅ Full | ✅ Full | ⬜ §4.1 P0 |
| Sparse file support | ✅ Native | ✅ Full | ⬜ §4.1 (sparse runs) |
| File reading (resident + non-res) | ✅ Full | ✅ Full | ⬜ §4.2 P1 |
| B+ tree directory indexing | ✅ Full | ✅ Full | ⬜ §5.1–5.3 P1 |
| Directory enumeration (readdir) | ✅ Full | ✅ Full | ⬜ §5.4 P2 |
| Path resolution | ✅ Full | ✅ Full | ⬜ §5.3 P1 |
| VFS/FUSE integration | ✅ Native (ntfs.sys) | ✅ FUSE (ntfs-3g) / Native (ntfs3) | ⬜ §6.1 P1 |
| Volume label / dirty flag | ✅ Full | ✅ Full | ⬜ §7.1 P2 |
| Free space queries | ✅ Full | ✅ Full | ⬜ §7.1 P2 |
| `$UpCase` case folding | ✅ Full Unicode | ✅ Full Unicode | ⬜ §7.1 P2 (ASCII fallback) |
| LZNT1 compressed file reading | ✅ Native | ✅ ntfs-3g read-only / ntfs3 full | ⬜ §9.1 P2 |
| MFT record caching | ✅ Windows cache manager | ✅ Page cache | ⬜ §10.1 P2 |
| Write support | ✅ Full R/W | ✅ Full R/W (ntfs-3g) | ⬜ Future P3 (read-only first) |
| Journaling recovery ($LogFile) | ✅ Full | ✅ ntfs-3g replays log | ⬜ Future P3 |
| Alternate Data Streams | ✅ Native | ✅ ntfs-3g / ntfs3 | ⬜ Future (routed via VFS §2.1) |
| **Volume health dashboard** ⭐ | ❌ Spread across multiple tools | ❌ CLI `ntfsinfo` only | ⬜ §11.1 P3 — one-panel health |
| **Deleted file recovery** ⭐ | ❌ Requires third-party (Recuva) | ⚠️ CLI `ntfsundelete` only | ⬜ §11.2 P3 — built-in GUI recovery |
| **Full read-only driver** | ✅ | ✅ | ⬜ Requires §1–§6 at minimum |
