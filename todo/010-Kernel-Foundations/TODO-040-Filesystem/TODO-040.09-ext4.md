# 040.09-ext4 — Fourth Extended Filesystem (Read-Only Driver)

> **Goal:** Implement a read-only ext4 driver for Impossible OS. The driver must
> parse the Superblock, navigate Block Group Descriptors, read inodes from the
> Inode Table, traverse extent trees for data block mapping, enumerate directories
> (linear entries and HTree indexed), and handle the feature flag matrices for
> safe backward compatibility with ext2/ext3 volumes. This enables reading files
> from Linux partitions — essential for dual-boot interoperability and data recovery.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for inode table reads, block group
> descriptor tables, extent tree blocks, and directory data blocks. `kmalloc` is
> ONLY for small kernel structs (≤ 4 KB). See `rules.md` Known Gotchas.

> [!WARNING]
> **Read-Only First.** ext4 write support requires a fully functional JBD2
> journaling layer — writing without it causes irrecoverable corruption on crash.
> This TODO covers **read-only** access only. Write support is a future P3 extension.

> [!IMPORTANT]
> **Byte Order:** All ext4 on-disk structures are **little-endian**, EXCEPT the
> JBD2 journal which is **big-endian**. The driver must handle this dichotomy.
>
> **Spec Reference:** All offsets, field layouts, and algorithms reference the
> [ext4 Specification](file:///home/derickpayne/impossible-os/specs/ext4.md)
> in the repo at `specs/ext4.md`.

---

## 1. Superblock Parsing & Validation

### 1.1 Superblock Reader

**Prompt:** Read the Superblock from absolute byte offset 1024 of the partition (NOT block 0 — the first 1024 bytes are reserved boot padding). Validate the magic number `0xEF53` at offset `0x38`. Extract all critical fields: inode count, block count, block size (`1024 << s_log_block_size`), blocks per group, inodes per group, inode size, feature flags, UUID, and volume name. Handle 64-bit block counts by combining `s_blocks_count_lo` + `s_blocks_count_hi`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ext4: superblock parsing and validation"`. Add notes directly in this TODO section.

- [ ] Read 1024 bytes from partition byte offset 1024 (skip boot padding)
- [ ] Validate magic number at `0x38`: must be `0xEF53` (LE stored as `0x53 0xEF`)
- [ ] Extract fundamental fields:
  - [ ] `s_inodes_count` at `0x00` (4B) — total inodes on volume
  - [ ] `s_blocks_count_lo` at `0x04` (4B) — total blocks (lower 32 bits)
  - [ ] `s_free_blocks_count_lo` at `0x0C` (4B) — free blocks (lower)
  - [ ] `s_free_inodes_count` at `0x10` (4B) — free inodes
  - [ ] `s_first_data_block` at `0x14` (4B) — 0 for 4K blocks, 1 for 1K blocks
  - [ ] `s_log_block_size` at `0x18` (4B) — block_size = `1024 << value`
  - [ ] `s_blocks_per_group` at `0x20` (4B) — blocks per block group
  - [ ] `s_inodes_per_group` at `0x28` (4B) — inodes per block group
  - [ ] `s_magic` at `0x38` (2B) — must be `0xEF53`
  - [ ] `s_state` at `0x3A` (2B) — 1=clean, 2=errors, 4=orphan recovery
  - [ ] `s_rev_level` at `0x4C` (4B) — 0=original, 1=dynamic inodes
- [ ] Extract extended fields (revision 1+):
  - [ ] `s_first_ino` at `0x54` (4B) — first non-reserved inode (usually 11)
  - [ ] `s_inode_size` at `0x58` (2B) — 128 (ext2/ext3) or 256 (ext4)
  - [ ] `s_feature_compat` at `0x5C` (4B) — compatible feature flags
  - [ ] `s_feature_incompat` at `0x60` (4B) — **incompatible** feature flags
  - [ ] `s_feature_ro_compat` at `0x64` (4B) — read-only compatible flags
  - [ ] `s_uuid` at `0x68` (16B) — filesystem UUID
  - [ ] `s_volume_name` at `0x78` (16B) — volume label (null-terminated)
  - [ ] `s_desc_size` at `0xFE` (2B) — group descriptor size (32 or 64)
- [ ] 64-bit block counts: combine `s_blocks_count_lo` + `s_blocks_count_hi` (at `0x150`)
- [ ] Calculate: `block_size = 1024 << s_log_block_size`
- [ ] Calculate: `num_block_groups = (total_blocks + blocks_per_group - 1) / blocks_per_group`
- [ ] Log: `[ext4] Volume: %llu blocks, block_size=%u, %u groups, inode_size=%u`
- [ ] Log: `[ext4] UUID=%s, label='%s', state=%s`
- [ ] Commit: `"ext4: superblock parsing and validation"`

### 1.2 Feature Flag Gating

**Prompt:** Implement the three-tier feature flag system. If ANY bit in `s_feature_incompat` is set that the driver does not support, the mount MUST be rejected — proceeding causes data corruption. If unrecognized `s_feature_ro_compat` bits are set, mount as read-only (which is our default anyway). Compatible features can be safely ignored. Log all detected features. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ext4: feature flag gating"`. Add notes directly in this TODO section.

- [ ] Parse `s_feature_incompat` — must support these to mount:
  - [ ] `INCOMPAT_FILETYPE` (`0x0002`) — dir entries have file type byte
  - [ ] `INCOMPAT_EXTENTS` (`0x0040`) — extent-based allocation (mandatory for ext4)
  - [ ] `INCOMPAT_64BIT` (`0x0080`) — 64-bit block numbers
  - [ ] `INCOMPAT_FLEX_BG` (`0x0200`) — flexible block groups
- [ ] Reject mount if any UNSUPPORTED incompat bits are set:
  - [ ] `INCOMPAT_RECOVER` (`0x0004`) — needs journal replay (we don't support)
  - [ ] `INCOMPAT_ENCRYPT` (`0x10000`) — encrypted inodes
  - [ ] Any other unknown bits → reject
  - [ ] Log: `[ext4] FATAL: Unsupported incompat features: 0x%08X — refusing mount`
- [ ] Parse `s_feature_ro_compat` — mount read-only if unrecognized bits present:
  - [ ] `RO_COMPAT_SPARSE_SUPER` (`0x0001`) — sparse superblock backups
  - [ ] `RO_COMPAT_LARGE_FILE` (`0x0002`) — files > 2 GiB
  - [ ] `RO_COMPAT_HUGE_FILE` (`0x0008`) — block counts in FS units
  - [ ] `RO_COMPAT_EXTRA_ISIZE` (`0x0040`) — extended inode fields
  - [ ] `RO_COMPAT_METADATA_CSUM` (`0x0400`) — CRC32C checksumming
- [ ] Parse `s_feature_compat` — log for informational purposes:
  - [ ] `COMPAT_HAS_JOURNAL` (`0x0004`) — has JBD2 journal
  - [ ] `COMPAT_DIR_INDEX` (`0x0020`) — HTree directory indexing
- [ ] Log all detected features: `[ext4] Features: incompat=0x%X, ro_compat=0x%X, compat=0x%X`
- [ ] Commit: `"ext4: feature flag gating"`

---

## 2. Block Group Descriptors

### 2.1 Group Descriptor Table Reader

**Prompt:** Read the Group Descriptor Table (GDT) from the block(s) immediately following the Superblock. Each descriptor is 32 bytes (ext2/ext3) or 64 bytes (ext4 with `INCOMPAT_64BIT`, size in `s_desc_size`). The GDT contains one entry per block group, providing the locations of the block bitmap, inode bitmap, and inode table for each group. Handle 64-bit mode by combining `_lo` and `_hi` fields. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ext4: block group descriptor table"`. Add notes directly in this TODO section.

- [ ] Calculate GDT location: block after Superblock
  - [ ] 1K block size: GDT starts at block 2
  - [ ] 4K block size: GDT starts at block 1 (Superblock is at byte 1024 within block 0)
- [ ] Calculate GDT size: `num_block_groups × s_desc_size` bytes
- [ ] Allocate buffer via `pmm_alloc_contiguous()` for entire GDT
- [ ] Read all GDT blocks from disk
- [ ] Parse each group descriptor:
  - [ ] `bg_block_bitmap_lo` at `0x00` (4B) + `bg_block_bitmap_hi` at `0x20` (4B)
  - [ ] `bg_inode_bitmap_lo` at `0x04` (4B) + `bg_inode_bitmap_hi` at `0x24` (4B)
  - [ ] `bg_inode_table_lo` at `0x08` (4B) + `bg_inode_table_hi` at `0x28` (4B)
  - [ ] `bg_free_blocks_count_lo` at `0x0C` (2B) + `bg_free_blocks_count_hi` at `0x2C` (2B)
  - [ ] `bg_free_inodes_count_lo` at `0x0E` (2B) + `bg_free_inodes_count_hi` at `0x2E` (2B)
  - [ ] `bg_checksum` at `0x1E` (2B) — validate if `METADATA_CSUM` enabled
- [ ] Combine `_lo` + `_hi` into 64-bit values when `INCOMPAT_64BIT` is set
- [ ] Handle `flex_bg`: metadata relocated to first group in flex sequence
- [ ] Cache parsed descriptors in `struct ext4_volume`
- [ ] Log: `[ext4] Group %u: bitmap=%llu, inode_table=%llu, free=%u blocks, %u inodes`
- [ ] Commit: `"ext4: block group descriptor table"`

---

## 3. Inode Reading & Metadata

### 3.1 Inode Table Reader

**Prompt:** Implement reading a single inode by its inode number. ext4 inodes are numbered starting at 1 (not 0). Calculate the block group: `(inode - 1) / s_inodes_per_group`. Calculate the local index: `(inode - 1) % s_inodes_per_group`. Read the inode from the inode table at byte offset `local_index × s_inode_size` from the `bg_inode_table` block. Parse the core 128-byte inode fields and the extended 256-byte fields if the inode size supports them. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ext4: inode table reader"`. Add notes directly in this TODO section.

- [ ] Implement `ext4_read_inode(vol, inode_number, inode_out)`:
  - [ ] Calculate `block_group = (inode_number - 1) / vol->inodes_per_group`
  - [ ] Calculate `local_index = (inode_number - 1) % vol->inodes_per_group`
  - [ ] Look up `inode_table_block` from group descriptor `block_group`
  - [ ] Calculate byte offset within table: `local_index × vol->inode_size`
  - [ ] Read the inode from disk (may need to read surrounding block)
- [ ] Parse core inode fields (128 bytes):
  - [ ] `i_mode` at `0x00` (2B) — file type (upper 4 bits) + permissions (lower 12)
  - [ ] `i_uid` at `0x02` (2B) — owner UID (lower 16)
  - [ ] `i_size_lo` at `0x04` (4B) — file size (lower 32)
  - [ ] `i_atime` at `0x08` (4B) — last access time (POSIX seconds)
  - [ ] `i_ctime` at `0x0C` (4B) — inode change time
  - [ ] `i_mtime` at `0x10` (4B) — last modification time
  - [ ] `i_gid` at `0x18` (2B) — group ID (lower 16)
  - [ ] `i_links_count` at `0x1A` (2B) — hard link count
  - [ ] `i_blocks_lo` at `0x1C` (4B) — block count (in 512B units, or FS units if `HUGE_FILE`)
  - [ ] `i_flags` at `0x20` (4B) — inode flags (extents, inline, etc.)
  - [ ] `i_block[15]` at `0x28` (60B) — extent tree root OR indirect blocks
  - [ ] `i_size_high` at `0x6C` (4B) — file size (upper 32, for 64-bit size)
- [ ] Parse extended fields (256-byte inode, offset `0x80`+):
  - [ ] `i_extra_isize` at `0x80` (2B)
  - [ ] `i_crtime` at `0x90` (4B) — file creation time
  - [ ] Nanosecond extensions at `0x84`–`0x94`
- [ ] File type extraction from `i_mode` upper 4 bits:
  - [ ] `0x4` = directory, `0x8` = regular file, `0xA` = symlink, `0x6` = block dev, etc.
- [ ] Combine `i_size_lo` + `i_size_high` for 64-bit file size
- [ ] Log: `[ext4] Inode %u: mode=0x%04X, size=%llu, links=%u, flags=0x%08X`
- [ ] Commit: `"ext4: inode table reader"`

### 3.2 Special Inode Handling

**Prompt:** Define and handle the reserved inodes. Inode 2 is the root directory — all path resolution starts here. Inode 8 is the JBD2 journal file (read its inode but don't replay — we're read-only). Inodes 1–10 are reserved system inodes. The first user inode is `s_first_ino` (typically 11, traditionally `lost+found`). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ext4: special inode handling"`. Add notes directly in this TODO section.

- [ ] Define reserved inode constants:
  - [ ] `EXT4_ROOT_INO = 2` — root directory
  - [ ] `EXT4_JOURNAL_INO = 8` — JBD2 journal
  - [ ] `EXT4_FIRST_INO = s_first_ino` — first non-reserved (typically 11)
- [ ] On mount: read inode 2 (root directory) to verify it's a directory (`i_mode & 0xF000 == 0x4000`)
- [ ] Detect journal: if `COMPAT_HAS_JOURNAL` flag set, note inode 8 exists
  - [ ] Log: `[ext4] Journal at inode 8 (read-only driver — not replaying)`
  - [ ] If `INCOMPAT_RECOVER` set → refuse mount (journal replay required)
- [ ] Commit: `"ext4: special inode handling"`

---

## 4. Extent Tree Traversal

### 4.1 Extent Tree Reader

**Prompt:** Implement the extent tree decoder for data block mapping. The extent tree root lives in the inode's `i_block[0..14]` (60 bytes). It starts with a 12-byte header: magic `0xF30A`, entry count, max entries, depth. At depth 0 (leaf): entries are `ext4_extent` structs mapping logical blocks to physical blocks. At depth > 0 (internal): entries are `ext4_extent_idx` structs pointing to child tree blocks. Handle uninitialized extents (bit 15 of `ee_len` set — preallocated, reads return zeros). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ext4: extent tree reader"`. Add notes directly in this TODO section.

- [ ] Parse extent header from inode's `i_block[0..11]`:
  - [ ] `eh_magic` (2B) — must be `0xF30A`
  - [ ] `eh_entries` (2B) — number of valid entries
  - [ ] `eh_max` (2B) — maximum capacity
  - [ ] `eh_depth` (2B) — 0=leaf, >0=internal node
  - [ ] `eh_generation` (4B) — for checksumming
- [ ] If `eh_depth == 0` (leaf node) — parse `ext4_extent` entries (12 bytes each):
  - [ ] `ee_block` (4B) — first logical block this extent covers
  - [ ] `ee_len` (2B) — length in blocks (max 32768)
    - [ ] If bit 15 set → uninitialized extent, length = `ee_len - 32768`, reads return zeros
  - [ ] `ee_start_hi` (2B) + `ee_start_lo` (4B) → 48-bit physical block number
- [ ] If `eh_depth > 0` (internal node) — parse `ext4_extent_idx` entries (12 bytes each):
  - [ ] `ei_block` (4B) — first logical block this index covers
  - [ ] `ei_leaf_lo` (4B) + `ei_leaf_hi` (2B) → 48-bit physical block of child node
  - [ ] Read child block from disk, parse its extent header, recurse
- [ ] Implement `ext4_extent_lookup(vol, inode, logical_block)`:
  - [ ] Binary search entries for the extent/index covering `logical_block`
  - [ ] If leaf → return `physical = ee_start + (logical_block - ee_block)`
  - [ ] If index → read child block, recurse
- [ ] Cache all leaf extents on file open for O(1) lookups
- [ ] Handle extent tail checksum (`ext4_extent_tail`) if `METADATA_CSUM` enabled
- [ ] Log: `[ext4] Extent: logical=%u → physical=%llu, len=%u`
- [ ] Commit: `"ext4: extent tree reader"`

### 4.2 File Data Reader

**Prompt:** Using the extent tree, implement reading file data by logical block range. Given a file offset and read length, convert to logical blocks, look up extents, translate to physical blocks, read from disk. Handle reads spanning multiple extents. Handle uninitialized extents by returning zero-filled buffers. Cap reads at `i_size` (actual file length). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ext4: file data reader"`. Add notes directly in this TODO section.

- [ ] Implement `ext4_read_data(vol, inode, offset, length, buffer)`:
  - [ ] Calculate starting logical block: `offset / block_size`
  - [ ] Calculate offset within block: `offset % block_size`
  - [ ] For each logical block in the read range:
    - [ ] Look up extent via `ext4_extent_lookup()`
    - [ ] If uninitialized → fill buffer with zeros
    - [ ] If hole (no extent covering this block) → fill with zeros
    - [ ] Else → read physical block from disk
  - [ ] Handle partial block reads at start/end
  - [ ] Cap total read at `i_size` (don't read beyond file end)
- [ ] Handle inline data: if `EXT4_INLINE_DATA_FL` set, data is in `i_block` (60 bytes)
  - [ ] Copy directly from `i_block` array, cap at 60 bytes
- [ ] Commit: `"ext4: file data reader"`

---

## 5. Legacy Indirect Block Map (ext2/ext3 Compatibility)

### 5.1 Indirect Block Reader

**Prompt:** For ext2/ext3 volumes (or ext4 inodes without `EXT4_EXTENTS_FL`), the `i_block[15]` array uses the classic indirect block scheme: indices 0–11 are direct block pointers, index 12 is a single indirect, index 13 is double indirect, index 14 is triple indirect. Each indirect block is a full filesystem block containing `block_size / 4` 32-bit block pointers. Implement this for backward compatibility so the driver can mount ext2 and ext3 volumes. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ext4: legacy indirect block reader"`. Add notes directly in this TODO section.

- [ ] Detect: if `EXT4_EXTENTS_FL` NOT set in `i_flags` → use indirect blocks
- [ ] Direct blocks: `i_block[0]` through `i_block[11]` → physical block numbers
- [ ] Single indirect: `i_block[12]` → read block, it contains `block_size/4` direct pointers
- [ ] Double indirect: `i_block[13]` → read block of single-indirect pointers
- [ ] Triple indirect: `i_block[14]` → read block of double-indirect pointers
- [ ] Block number 0 means "hole" — return zero-filled buffer
- [ ] Implement `ext4_indirect_lookup(vol, inode, logical_block)`:
  - [ ] If `logical_block < 12` → return `i_block[logical_block]`
  - [ ] Else calculate indirect depth and navigate pointer chain
- [ ] Cache indirect blocks to avoid redundant reads
- [ ] Commit: `"ext4: legacy indirect block reader"`

---

## 6. Directory Reading

### 6.1 Linear Directory Entry Parser

**Prompt:** ext4 directories store entries as `ext4_dir_entry_2` records in their data blocks. Each entry: inode number (4B), record length (2B), name length (1B), file type (1B), name (variable, NOT null-terminated). Entries are 4-byte aligned. The final entry's `rec_len` extends to the end of the block. Deleted entries have `inode == 0`. Walk entries by advancing `rec_len` bytes. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ext4: linear directory entry parser"`. Add notes directly in this TODO section.

- [ ] Implement `ext4_readdir_block(vol, block_data, block_size, callback)`:
  - [ ] Start at offset 0
  - [ ] While offset < block_size:
    - [ ] Read `inode` (4B), `rec_len` (2B), `name_len` (1B), `file_type` (1B)
    - [ ] If `inode != 0` → valid entry, invoke callback with name + inode + type
    - [ ] If `inode == 0` → deleted entry, skip
    - [ ] Advance by `rec_len`
    - [ ] Safety: if `rec_len == 0` or `rec_len < 8` → break (corrupt)
- [ ] File type codes (from `INCOMPAT_FILETYPE`):
  - [ ] 1 = regular, 2 = directory, 3 = chardev, 4 = blockdev, 5 = FIFO, 6 = socket, 7 = symlink
- [ ] Name is NOT null-terminated — use `name_len` to determine length
- [ ] Handle: `"."` (self) and `".."` (parent) entries (always first two entries)
- [ ] Commit: `"ext4: linear directory entry parser"`

### 6.2 HTree Directory Index

**Prompt:** For large directories with `EXT4_INDEX_FL` set, ext4 uses HTree (Hash Tree) indexing. The first data block contains a fake `dx_root` header (for ext2 backward compat), followed by the hash version, tree depth, and an array of `dx_entry` pairs (hash, block). To look up a filename: hash the name using the specified algorithm (typically Half MD4), binary search the `dx_entry` array, read the referenced leaf block, then linear-scan `ext4_dir_entry_2` entries. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ext4: HTree directory index"`. Add notes directly in this TODO section.

- [ ] Detect HTree: check `EXT4_INDEX_FL` (`0x1000`) in directory inode's `i_flags`
- [ ] Parse `dx_root` from first data block:
  - [ ] Skip fake `.` and `..` entries at `0x00`–`0x17`
  - [ ] Hash version at `0x18` (1B): 0=legacy, 1=half_md4, 2=tea, 3=half_md4_unsigned, 4=tea_unsigned
  - [ ] Tree depth at `0x1A` (1B): number of indirect levels
  - [ ] Limit at `0x1C` (2B): max `dx_entry` count
  - [ ] Count at `0x1E` (2B): active `dx_entry` count
  - [ ] `dx_entry` array starting at `0x20`: each is 8 bytes (hash 4B + block 4B)
- [ ] Implement Half MD4 hash function (unsigned variant for ext4):
  - [ ] Hash the target filename to a 32-bit value
- [ ] Directory lookup:
  - [ ] Hash the filename
  - [ ] Binary search `dx_entry` array for the bounding hash range
  - [ ] Read the referenced leaf data block
  - [ ] Linear scan `ext4_dir_entry_2` entries for exact filename match
- [ ] Fallback: if directory does NOT have `EXT4_INDEX_FL` → use linear scan only
- [ ] Commit: `"ext4: HTree directory index"`

### 6.3 Path Resolution

**Prompt:** Implement full path resolution by splitting the path on `/`, starting at inode 2 (root directory), and recursively looking up each component. For each component, search the directory's data blocks (HTree or linear). Handle symlinks: read `i_block` directly for short symlinks (target fits in 60 bytes), or read data blocks for long symlinks. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ext4: path resolution"`. Add notes directly in this TODO section.

- [ ] Implement `ext4_resolve_path(vol, path)`:
  - [ ] Start at inode 2 (root directory)
  - [ ] Split path by `/`
  - [ ] For each component:
    - [ ] Read directory data for current inode
    - [ ] Search for component name (HTree if indexed, linear otherwise)
    - [ ] If found → current inode = entry's inode number, continue
    - [ ] If not found → return `EXT4_ERR_NOT_FOUND`
  - [ ] Return final inode
- [ ] Handle symlinks (`i_mode & 0xF000 == 0xA000`):
  - [ ] Short symlink: target stored directly in `i_block` (if `i_size < 60`)
  - [ ] Long symlink: read data blocks for target path
  - [ ] Recursion limit: max 8 symlink follows (prevent loops)
- [ ] Case sensitivity: ext4 is case-sensitive by default (unlike NTFS/FAT32)
  - [ ] If `INCOMPAT_CASEFOLD` set on directory → case-insensitive comparison
- [ ] Commit: `"ext4: path resolution"`

---

## 7. Metadata Checksumming (CRC32C)

### 7.1 CRC32C Validation

**Prompt:** When `RO_COMPAT_METADATA_CSUM` is enabled, all metadata structures carry CRC32C checksums. The seed is derived from the filesystem UUID (or `s_checksum_seed` if `INCOMPAT_CSUM_SEED` is set). Validate checksums on: Superblock, group descriptors, inodes, directory blocks (dx_tail), extent blocks (extent_tail). On checksum failure, log error and flag the structure as corrupt — do not silently proceed. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ext4: CRC32C metadata checksumming"`. Add notes directly in this TODO section.

- [ ] Implement `ext4_crc32c(seed, data, length)` — CRC32C (Castagnoli polynomial `0x1EDC6F41`)
  - [ ] Use reflected table approach (similar to `gpt_crc32` but different polynomial)
  - [ ] Or use hardware `CRC32C` instruction if available (`__builtin_ia32_crc32`)
- [ ] Calculate checksum seed:
  - [ ] If `INCOMPAT_CSUM_SEED` → seed = `s_checksum_seed` at offset `0x190`
  - [ ] Else → seed = `crc32c(~0, s_uuid, 16)`
- [ ] Validate checksummed structures:
  - [ ] Superblock: `s_checksum` at offset `0xFC` — zero field, compute, compare
  - [ ] Group descriptor: `bg_checksum` — uses UUID + group number + descriptor data
  - [ ] Inode: `i_checksum_lo` (at core offset) + `i_checksum_hi` (extended)
  - [ ] Extent block: `ext4_extent_tail` — last 4 bytes of block
  - [ ] Directory block: `dx_tail` — if HTree indexed
- [ ] On checksum failure: log `[ext4] CHECKSUM FAILED: %s at block %llu`
- [ ] Optional: make checksum verification configurable (skip for performance during bulk reads)
- [ ] Commit: `"ext4: CRC32C metadata checksumming"`

---

## 8. VFS Integration

### 8.1 ext4 VFS Driver Registration

**Prompt:** Register ext4 as a VFS filesystem driver. Implement the `vfs_ops` callbacks: `open`, `close`, `read`, `readdir`, `finddir`, `stat`. Write callbacks return `-EROFS`. Detect ext4/ext3/ext2 volumes during partition scanning by checking the magic `0xEF53` in the boot sector at byte offset 1080 from partition start. Support mounting ext2 and ext3 volumes (they use the same magic) — handle based on feature flags. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ext4: VFS driver registration"`. Add notes directly in this TODO section.

- [ ] Create `src/kernel/fs/ext4.c` and `include/kernel/fs/ext4.h`
- [ ] Define `struct ext4_volume` — superblock data, GDT, config
- [ ] Implement `ext4_detect(blkdev)`:
  - [ ] Read 2 sectors from partition start
  - [ ] Check magic `0xEF53` at byte offset 1080 (partition offset 1024 + superblock offset 0x38)
- [ ] Register with partition scanner:
  - [ ] MBR type `0x83` (Linux native)
  - [ ] GPT GUID `0FC63DAF-8483-4772-8E79-3D69D8477DE4` (Linux filesystem)
- [ ] Implement `vfs_ops` callbacks:
  - [ ] `ext4_open(node)` — read inode, cache extent tree
  - [ ] `ext4_close(node)` — free cached extents
  - [ ] `ext4_read(node, offset, size, buf)` — extent/indirect read
  - [ ] `ext4_readdir(node, index)` — directory enumeration
  - [ ] `ext4_finddir(node, name)` — directory lookup (HTree or linear)
  - [ ] `ext4_stat(node, stat)` — populate from inode metadata
  - [ ] Write ops → return `-EROFS`
- [ ] Handle ext2/ext3 detection:
  - [ ] No `INCOMPAT_EXTENTS` → assume ext2/ext3, use indirect blocks
  - [ ] No `COMPAT_HAS_JOURNAL` → ext2 (no journaling concern)
  - [ ] Has journal but no extents → ext3
- [ ] Log: `[ext4] Mounted %s volume on drive %c: (%llu bytes, %s)`
- [ ] Commit: `"ext4: VFS driver registration"`

---

## 9. Testing & Validation

### 9.1 ext4 Test Suite

**Prompt:** Create ext4 test disk images using host tools (`mkfs.ext4`, `mkfs.ext3`, `mkfs.ext2`). Test: basic file read, large file spanning multiple extents, deep directories, inline data (small file), HTree-indexed directory (>500 files), symlinks, and ext2 backward compatibility. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"test: ext4 filesystem test suite"`. Add notes directly in this TODO section.

- [ ] Test image: ext4 volume with files in root directory
  - [ ] Verify: superblock parsing, root inode read, directory listing
- [ ] Test image: file with known content → read and compare byte-exact
- [ ] Test image: large file (>4 MB, multiple extents)
  - [ ] Verify: extent tree traversal, multi-extent stitching
- [ ] Test image: deep directory path (`a/b/c/d/e/file.txt`)
  - [ ] Verify: recursive path resolution
- [ ] Test image: directory with 500+ files (forces HTree indexing)
  - [ ] Verify: HTree hash lookup, `dx_entry` binary search
- [ ] Test image: inline data file (< 60 bytes, `INLINE_DATA_FL`)
  - [ ] Verify: data read from `i_block` directly
- [ ] Test image: symbolic link (short + long targets)
  - [ ] Verify: symlink resolution
- [ ] Test image: **ext3** volume (journal + indirect blocks, no extents)
  - [ ] Verify: indirect block reader, journal detection (no replay)
- [ ] Test image: **ext2** volume (no journal, no extents)
  - [ ] Verify: pure indirect block reading, no feature flag rejection
- [ ] Test: checksum validation with corrupt metadata block
  - [ ] Verify: CRC32C mismatch produces error, doesn't corrupt
- [ ] QEMU: `-drive file=ext4_test.img,format=raw,if=none,id=t0 -device virtio-blk-pci,drive=t0`
- [ ] Commit: `"test: ext4 filesystem test suite"`

---

## Priority Order

| Priority | Section | Description |
|----------|---------|-------------|
| 🔴 P0 | 1.1 Superblock Parsing | Foundation — locate block groups and inodes |
| 🔴 P0 | 1.2 Feature Flag Gating | Safety — reject unsafe mounts, handle ext2/ext3 |
| 🔴 P0 | 2.1 Group Descriptor Table | Foundation — locate bitmaps and inode tables |
| 🔴 P0 | 3.1 Inode Reader | Foundation — read any file's metadata |
| 🔴 P0 | 4.1 Extent Tree Reader | Foundation — map logical to physical blocks |
| 🟠 P1 | 3.2 Special Inode Handling | Metadata — root dir, journal detection |
| 🟠 P1 | 4.2 File Data Reader | Core feature — actually read file contents |
| 🟠 P1 | 6.1 Linear Directory Parser | Directory — read dir entries |
| 🟠 P1 | 6.3 Path Resolution | Directory — resolve full paths |
| 🟠 P1 | 8.1 VFS Registration | Integration — make ext4 mountable |
| 🟡 P2 | 5.1 Indirect Block Reader | Compat — mount ext2/ext3 volumes |
| 🟡 P2 | 6.2 HTree Directory Index | Performance — fast lookup in large dirs |
| 🟡 P2 | 7.1 CRC32C Checksumming | Integrity — detect metadata corruption |
| 🟢 P3 | 9.1 Test Suite | Quality — automated validation |

---

## OS Comparison

| Feature | 🪟 Windows 11 | 🐧 Linux (native ext4) | 🚀 Impossible OS |
| ------------------------------- | -------------------- | --------------------------------- | --------------------------------- |
| Superblock parsing | ❌ No ext4 support | ✅ Full | ⬜ §1.1 P0 |
| Feature flag gating | ❌ | ✅ Full 3-tier (compat/incompat/ro) | ⬜ §1.2 P0 |
| Block group descriptors | ❌ | ✅ Full (32/64-byte) | ⬜ §2.1 P0 |
| Inode reading | ❌ | ✅ Full (128/256-byte) | ⬜ §3.1 P0 |
| Extent tree traversal | ❌ | ✅ Full (extent cache + preread) | ⬜ §4.1 P0 |
| Indirect block map (ext2/ext3) | ❌ | ✅ Full (triple indirect) | ⬜ §5.1 P2 |
| Linear directory entries | ❌ | ✅ Full | ⬜ §6.1 P1 |
| HTree directory index | ❌ | ✅ Full (Half MD4/TEA) | ⬜ §6.2 P2 |
| Path resolution | ❌ | ✅ Full (symlinks, case-fold) | ⬜ §6.3 P1 |
| CRC32C metadata checksums | ❌ | ✅ Full | ⬜ §7.1 P2 |
| JBD2 journal replay | ❌ | ✅ Full recovery | ⬜ Future P3 (reject RECOVER) |
| VFS integration | ❌ | ✅ Native | ⬜ §8.1 P1 |
| Inline data (i_block payload) | ❌ | ✅ Full | ⬜ §4.2 P1 |
| flex_bg support | ❌ | ✅ Full | ⬜ §2.1 (handled in GDT read) |
| ext2/ext3 backward compat | ❌ | ✅ Full | ⬜ §5.1 + §8.1 P2 |
| Uninitialized extents | ❌ | ✅ Full (return zeros) | ⬜ §4.1 (bit 15 handling) |
| Write support | ❌ | ✅ Full R/W | ⬜ Future P3 |
| **Read-only driver (minimum)** | ❌ None | ✅ | ⬜ Requires §1–§4, §6, §8 |

---

## Key Files

| File | Purpose |
|------|---------|
| `src/kernel/fs/ext4.c` | [NEW] ext4/ext3/ext2 driver implementation |
| `include/kernel/fs/ext4.h` | [NEW] ext4 structures, constants, feature flags |
| `src/kernel/fs/partition.c` | Register ext4 detection on MBR `0x83` / GPT Linux GUID |
| `specs/ext4.md` | Full on-disk specification reference |
