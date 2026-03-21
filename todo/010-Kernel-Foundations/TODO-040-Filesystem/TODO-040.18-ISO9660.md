# 040.18-ISO9660 — ISO 9660 / ECMA-119 Read-Only Filesystem Driver

> **Goal:** Implement a read-only ISO 9660 (ECMA-119) filesystem driver for Impossible OS.
> The driver must parse the Volume Descriptor Set (PVD, SVD, Boot Record), navigate
> directory records with variable-length entries and sector-boundary padding, resolve
> file paths via both directory traversal and the Path Table, read contiguous file
> extents, and handle Level 3 multi-extent files exceeding 4 GiB. Extensions include
> Joliet (Unicode filenames via SVD), Rock Ridge (POSIX metadata via SUSP), and
> El Torito (boot catalog parsing). ISO 9660 is essential for mounting installation
> media, reading driver packages from optical discs, and supporting `.iso` images
> attached via QEMU `-cdrom`. No existing ISO 9660 code exists in the codebase.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for path table buffers (typically
> 1–32 KiB), directory extent caches, and any allocation > 4 KB. `kmalloc` is ONLY
> for small kernel structs (≤ 4 KB). See `rules.md` Known Gotchas.

> [!WARNING]
> **Read-Only Only.** ISO 9660 is an inherently read-only filesystem — there are no
> free-space management structures, no journaling, and no write paths. All VFS write
> callbacks must return `-EROFS`.

> [!IMPORTANT]
> **Spec Reference:** All byte offsets, field layouts, and algorithms reference the
> [ISO 9660 / ECMA-119 Specification](file:///home/derickpayne/impossible-os/specs/storage/filesystems/iso9660.md).
>
> **Endianness:** ISO 9660 uses both-endian (733/723) encoding for multi-byte integers.
> On x86-64, read only the little-endian half. See spec §Encoding Methods.

---

## TODO Completion Roadmap

> [!IMPORTANT]
> **Four TODO files and two specs** feed into the ISO 9660 driver. The ATAPI/SCSI
> driver provides the transport layer for physical optical drives, while the block
> device layer handles virtual `.iso` images. Joliet and UDF extensions build on
> the base ISO 9660 infrastructure.

### Dependency Graph

```mermaid
graph TD
    SPEC["specs/storage/filesystems/iso9660.md<br/>ISO 9660 / ECMA-119 Specification"]
    JSPEC["specs/storage/filesystems/joliet-udf.md<br/>Joliet + UDF Specification"]
    ATAPI["TODO-040.03-ATAPI-SCSI-MMC.md<br/>ATAPI Optical Drive + SCSI Layer"]
    BLK["TODO-040.01-VirtIO / 040.02-AHCI<br/>Block Device Layer"]
    VFS["TODO-040.07-VFS.md<br/>VFS Core"]

    A["§1.1 Volume Descriptor Scanner"]
    B["§1.2 PVD Parser"]
    C["§2.1 Directory Record Parser"]
    D["§2.2 File Flags & Special Entries"]
    E["§3.1 Path Table Loader"]
    F["§3.2 Path Table Lookup"]
    G["§4.1 File Extent Reader"]
    H["§4.2 Multi-Extent Files (Level 3)"]
    I["§5.1 Joliet SVD Parser"]
    J["§5.2 Rock Ridge SUSP Parser"]
    K["§6.1 El Torito Boot Catalog"]
    L["§7.1 VFS Registration"]
    M["§8.1 Test Suite"]
    N["§9.1 ISO Browser GUI"]
    O["§9.2 El Torito Inspector"]
    P["§9.3 Disc Health Analyzer"]
    Q["§9.4 Auto-Mount & Eject"]

    SPEC --> A
    BLK --> A
    ATAPI --> A
    A --> B
    B --> C
    C --> D
    B --> E
    E --> F
    C --> G
    G --> H
    B --> I
    JSPEC --> I
    C --> J
    B --> K
    G --> L
    F --> L
    VFS --> L
    L --> M
    I --> L
    J --> L
    L --> N
    K --> O
    L --> P
    ATAPI --> Q
    L --> Q
```

### Phase-by-Phase Implementation Order

| ⭐ | Phase  | Sections                                          | What It Delivers                                              | Depends On                     | Status |
| -- | :----: | ------------------------------------------------- | ------------------------------------------------------------- | ------------------------------ | :----: |
| 💎 | **0**  | Block device + ATAPI + spec                       | `blkdev_read()`, ATAPI SCSI, spec knowledge                  | —                              |   ✅   |
| 💎 | **1**  | §1.1 Volume Descriptor Scanner                    | `CD001` detection, PVD/SVD/Boot Record location               | Phase 0                        |   ⬜   |
| 💎 | **1**  | §1.2 PVD Parser                                   | Root directory record, path table, volume size                | Phase 1 (§1.1)                 |   ⬜   |
| 💎 | **2**  | §2.1 Directory Record Parser                      | Variable-length record parsing, sector-boundary handling      | Phase 1 (§1.2)                 |   ⬜   |
| 💎 | **2**  | §2.2 File Flags & Special Entries                  | Hidden files, `.`/`..` entries, directory vs file detection   | Phase 2 (§2.1)                 |   ⬜   |
| 💎 | **2**  | §3.1 Path Table Loader                             | Cached path table in kernel memory                            | Phase 1 (§1.2)                 |   ⬜   |
| 💎 | **3**  | §3.2 Path Table Lookup                             | O(n) fast directory lookup without disk I/O                   | Phase 2 (§3.1)                 |   ⬜   |
| 💎 | **3**  | §4.1 File Extent Reader                            | Read contiguous file data by LBA + length                     | Phase 2 (§2.1)                 |   ⬜   |
| 💎 | **3**  | §4.2 Multi-Extent Files (Level 3)                  | Files > 4 GiB via concatenated extents                        | Phase 3 (§4.1)                 |   ⬜   |
| 💎 | **4**  | §5.1 Joliet SVD Parser                             | Unicode filenames up to 64 chars via UCS-2 decoding           | Phase 1 (§1.2)                 |   ⬜   |
| 💎 | **4**  | §5.2 Rock Ridge SUSP Parser                        | Long filenames, POSIX permissions, symlinks                   | Phase 2 (§2.1)                 |   ⬜   |
| 💎 | **4**  | §6.1 El Torito Boot Catalog                        | Boot image enumeration for installation media                 | Phase 1 (§1.2)                 |   ⬜   |
| 💎 | **5**  | §7.1 VFS Registration                              | Mount ISO volumes, `vfs_ops` callbacks, drive letter          | Phase 3 + Phase 4 + VFS       |   ⬜   |
| 💎 | **6**  | §8.1 Test Suite                                    | Automated validation with ISO test images                     | Phase 5 (§7.1)                 |   ⬜   |
| ⭐ | **6**  | §9.1 ISO Browser GUI                               | Visual ISO contents explorer in File Manager                  | Phase 5 (§7.1)                 |   ⬜   |
| ⭐ | **6**  | §9.2 El Torito Inspector                           | Boot catalog viewer with platform/emulation details           | Phase 4 (§6.1)                 |   ⬜   |
| ⭐ | **6**  | §9.3 Disc Health Analyzer                          | Media integrity verification with read-error mapping          | Phase 5 (§7.1)                 |   ⬜   |
| ⭐ | **6**  | §9.4 Auto-Mount & Eject                            | Hot-insert notification, auto-mount, safe eject               | Phase 5 (§7.1) + ATAPI        |   ⬜   |

> [!NOTE]
> **Phase 0** is already done — block devices, ATAPI/SCSI, and VFS core are in place.
>
> **Phase 1** parses the Volume Descriptor Set — the entry point to all ISO 9660 data.
> The PVD contains the root directory record and path table locations.
>
> **Phases 2–3** implement the two navigation strategies: directory record traversal
> (recursive) and path table lookup (flat index). Both are needed for robustness.
>
> **Phase 4** adds extensions: Joliet (Unicode), Rock Ridge (POSIX), El Torito (boot).
>
> **Phase 5** wires everything to VFS for mountable volumes.
>
> **Phase 6** adds competitive features (⭐) and the test suite.

> [!TIP]
> **ISO 9660 is simple compared to other filesystems.** No free-space bitmap, no
> journaling, no cluster chains — files are contiguous extents addressed by LBA.
> A basic read-only driver can be implemented in ~800 lines of C.
>
> **QEMU testing flags:**
> ```
> qemu ... -cdrom test.iso
> qemu ... -drive file=test.iso,media=cdrom,if=none,id=cd0 -device ide-cd,drive=cd0
> ```
>
> **Create test ISOs on the host:**
> ```
> mkdir -p iso_root/BOOT && echo "hello" > iso_root/TEST.TXT
> mkisofs -o test.iso -R -J -b BOOT/bootimg -no-emul-boot iso_root
> ```
>
> **Memory rule reminder:** Path table buffers (1–32 KiB) and directory extent
> caches (2–64 KiB) must use `pmm_alloc_contiguous()`. Volume descriptor parsing
> uses a 2048-byte stack buffer.

---

## 1. Volume Descriptor Parsing

### 1.1 Volume Descriptor Scanner

**Prompt:** ISO 9660 volumes begin with a System Area (LBA 0–15, 32 KiB) followed by the Volume Descriptor Set starting at LBA 16. Each descriptor is exactly 2048 bytes. Scan sequentially: validate the magic string `"CD001"` at bytes 1–5, read the type code at byte 0, and dispatch to the appropriate parser. Continue until type code 255 (Terminator) is encountered. Per spec §Volume Descriptors and §Scanning Algorithm. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"fs: ISO 9660 volume descriptor scanner"`. After implementation, save any gotchas to MCP memory.

- [ ] Create `src/kernel/fs/iso9660.c` and `include/kernel/fs/iso9660.h`
- [ ] Define `struct iso9660_volume` — PVD data, root dir record, path table, block device
- [ ] Implement `iso9660_detect(blkdev)`:
  - [ ] Read 2048 bytes from LBA 16
  - [ ] Validate bytes 1–5 == `"CD001"` (ASCII)
  - [ ] Return `true` if magic matches
- [ ] Implement `iso9660_scan_descriptors(blkdev)`:
  - [ ] Start at LBA 16, read one sector (2048 bytes) per iteration
  - [ ] Check byte 0 (type code):
    - [ ] Type 0 → store Boot Record (El Torito catalog LBA at bytes 71–74 LE)
    - [ ] Type 1 → parse as PVD (§1.2)
    - [ ] Type 2 → check Joliet escape sequences at bytes 88–120 (§5.1)
    - [ ] Type 255 → stop scanning
    - [ ] Other → skip (log unknown type)
  - [ ] Validate byte 6 (version) == `0x01`
  - [ ] Advance LBA and repeat
- [ ] Safety: limit scan to 32 descriptors max (prevent infinite loop on corrupt media)
- [ ] Log: `[iso9660] Found %s at LBA %u` for each descriptor type
- [ ] Commit: `"fs: ISO 9660 volume descriptor scanner"`

### 1.2 Primary Volume Descriptor Parser

**Prompt:** The PVD (type 1) is the central metadata record, occupying a full 2048-byte logical sector. Extract all critical fields per spec §PVD Field Layout: Volume Space Size (offset 80, uint32_bb), Logical Block Size (offset 128, uint16_bb — must be 2048), Root Directory Record (offset 156, 34 bytes inline), Path Table Size and Location (offsets 132–143), Volume Identifier (offset 40, 32 bytes strD), and date/time fields. The root directory record contains the LBA and data length of the root directory extent. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"fs: ISO 9660 PVD parser"`. After implementation, save any gotchas to MCP memory.

- [ ] Implement both-endian accessor macros per spec §Implementation Strategy:
  - [ ] `iso_read_733(p)` — read LE half of both-endian uint32 (8 bytes total, read first 4)
  - [ ] `iso_read_723(p)` — read LE half of both-endian uint16 (4 bytes total, read first 2)
- [ ] Parse PVD fields from 2048-byte buffer:
  - [ ] Volume Identifier at offset 40 (32 bytes, strD) — strip trailing spaces
  - [ ] Volume Space Size at offset 80 (uint32_bb) — total logical blocks
  - [ ] Logical Block Size at offset 128 (uint16_bb) — validate == 2048, reject otherwise
  - [ ] Path Table Size at offset 132 (uint32_bb) — bytes
  - [ ] Type L Path Table LBA at offset 140 (uint32_le) — mandatory LE path table
  - [ ] Root Directory Record at offset 156 (34 bytes) — inline directory record
- [ ] Parse inline root directory record (34 bytes):
  - [ ] Location of Extent at offset 2 (uint32_bb) — LBA of root directory
  - [ ] Data Length at offset 10 (uint32_bb) — size of root directory extent in bytes
  - [ ] File Flags at offset 25 — must have bit 1 set (directory)
- [ ] Parse date/time fields (dec-datetime, 17 bytes each):
  - [ ] Volume Creation Date at offset 813
  - [ ] Volume Modification Date at offset 830
  - [ ] Volume Expiration Date at offset 847 (all ASCII '0' = never expires)
  - [ ] Volume Effective Date at offset 864
- [ ] Store parsed PVD in `struct iso9660_volume`
- [ ] Log: `[iso9660] Volume: "%s", %llu blocks, block_size=%u`
- [ ] Log: `[iso9660] Root dir: LBA=%u, size=%u bytes`
- [ ] Commit: `"fs: ISO 9660 PVD parser"`

---

## 2. Directory Record Navigation

### 2.1 Directory Record Parser

**Prompt:** Directory records are variable-length (33–255 bytes) packed sequentially within directory extents. Each record starts with a length byte at offset 0. If the length byte is 0, it signals sector-boundary padding — skip to the next 2048-byte sector boundary. Records must NOT span sector boundaries. Parse each record to extract: extent LBA (offset 2, uint32_bb), data length (offset 10, uint32_bb), recording date/time (offset 18, 7-byte dir-datetime), file flags (offset 25), and file identifier (offset 33, variable length with LEN_FI at offset 32). Per spec §Directory Records and §Parsing Algorithm. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"fs: ISO 9660 directory record parser"`. After implementation, save any gotchas to MCP memory.

- [ ] Define `struct iso9660_dir_record`:
  - [ ] `uint8_t length` — total record length
  - [ ] `uint8_t ext_attr_len` — extended attribute record length
  - [ ] `uint32_t extent_lba` — LBA of file/dir data
  - [ ] `uint32_t data_length` — file/dir size in bytes
  - [ ] `uint8_t datetime[7]` — compact 7-byte date/time
  - [ ] `uint8_t flags` — file flags bitfield
  - [ ] `uint8_t name_len` — length of file identifier
  - [ ] `char name[222]` — file identifier (max 255 - 33 = 222)
- [ ] Implement `iso9660_parse_dir_record(raw_bytes, out_record)`:
  - [ ] Read length byte at offset 0 — if 0, return `ISO_PADDING`
  - [ ] Validate length >= 33 (minimum fixed fields)
  - [ ] Extract extent LBA via `iso_read_733(raw + 2)`
  - [ ] Extract data length via `iso_read_733(raw + 10)`
  - [ ] Copy 7-byte datetime from offset 18
  - [ ] Read flags from offset 25
  - [ ] Read LEN_FI from offset 32, copy name from offset 33
  - [ ] Strip version suffix (`;1`) from filename for user display
- [ ] Implement `iso9660_read_directory(vol, extent_lba, extent_size)`:
  - [ ] Allocate buffer for entire directory extent via `pmm_alloc_contiguous()`
  - [ ] Read all sectors from `extent_lba` to `extent_lba + ceil(extent_size / 2048)`
  - [ ] Walk records per spec §Parsing Algorithm:
    - [ ] `offset = 0; while offset < extent_size`
    - [ ] If `data[offset] == 0` → `offset = ALIGN_UP(offset + 1, 2048); continue`
    - [ ] If `offset + rec_len > extent_size` → break (truncated)
    - [ ] Parse record, advance `offset += rec_len`
- [ ] Safety: validate `extent_lba + data_length / 2048 <= volume_space_size`
- [ ] Commit: `"fs: ISO 9660 directory record parser"`

### 2.2 File Flags & Special Entries

**Prompt:** Each directory record has a File Flags byte at offset 25 with 8 bit flags per spec §File Flags Bitfield. Every directory extent starts with exactly two mandatory entries: `.` (identifier byte `0x00`) pointing to self, and `..` (identifier byte `0x01`) pointing to parent. Handle the hidden file flag (bit 0), directory flag (bit 1), associated file flag (bit 2), and multi-extent flag (bit 7). Parse the 7-byte binary date/time (dir-datetime) with GMT offset. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"fs: ISO 9660 file flags and special entries"`. After implementation, save any gotchas to MCP memory.

- [ ] Implement file flags decoder:
  - [ ] Bit 0 — Hidden: exclude from default directory listings
  - [ ] Bit 1 — Directory: record describes a subdirectory
  - [ ] Bit 2 — Associated File: resource fork / metadata (report but skip)
  - [ ] Bit 7 — Multi-Extent: not the final record for this file (Level 3)
- [ ] Detect `.` and `..` entries:
  - [ ] LEN_FI == 1 && identifier[0] == `0x00` → current directory
  - [ ] LEN_FI == 1 && identifier[0] == `0x01` → parent directory
  - [ ] Skip these in `readdir` output (or map to `.`/`..` names)
- [ ] Implement `iso9660_parse_dir_datetime(raw_7bytes)`:
  - [ ] Year = `raw[0] + 1900` (unsigned 8-bit, range 1900–2155)
  - [ ] Month = `raw[1]` (1–12)
  - [ ] Day = `raw[2]` (1–31)
  - [ ] Hour = `raw[3]`, Minute = `raw[4]`, Second = `raw[5]`
  - [ ] GMT offset = `(int8_t)raw[6] * 15` minutes
  - [ ] Convert to kernel timestamp format
- [ ] Filename normalization for user display:
  - [ ] Strip trailing `;1` version suffix
  - [ ] Strip trailing `.` if no extension
  - [ ] Convert to lowercase for display (ISO 9660 stores uppercase only)
  - [ ] Accept non-compliant lowercase/extended char filenames (robustness)
- [ ] Commit: `"fs: ISO 9660 file flags and special entries"`

---

## 3. Path Table

### 3.1 Path Table Loader

**Prompt:** The Path Table is a flat, sequential index of every directory on the volume. It enables efficient directory lookup without recursive traversal — critical on optical media with high seek latency. Load the entire Type L (little-endian) path table into kernel memory at mount time. Each record is variable-length: 1-byte LEN_DI, 1-byte ext attr length, 4-byte LBA (uint32_le), 2-byte parent directory number (uint16_le), then the directory name (LEN_DI bytes + optional padding byte if LEN_DI is odd). Per spec §Path Table. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"fs: ISO 9660 path table loader"`. After implementation, save any gotchas to MCP memory.

- [ ] Read path table location and size from PVD:
  - [ ] Type L Path Table LBA at PVD offset 140 (uint32_le)
  - [ ] Path Table Size at PVD offset 132 (uint32_bb) — in bytes
- [ ] Allocate buffer: `pmm_alloc_contiguous()` if size > 4 KiB, stack otherwise
- [ ] Read path table sectors from disk (may span multiple 2048-byte sectors)
- [ ] Define `struct iso9660_path_entry`:
  - [ ] `uint8_t name_len` — directory name length
  - [ ] `uint32_t extent_lba` — LBA of the directory extent
  - [ ] `uint16_t parent_num` — 1-based parent record number
  - [ ] `char name[32]` — directory name
- [ ] Parse path table records sequentially:
  - [ ] Records may span sector boundaries (unlike directory records)
  - [ ] Padding byte after name if `name_len` is odd
  - [ ] Record 1 is always root (parent = 1, self-referential)
  - [ ] Records sorted by hierarchy level, then alphabetically within level
- [ ] Store parsed entries in `struct iso9660_volume` as array
- [ ] Safety: cap at 65,535 entries (16-bit parent directory number limit)
- [ ] Log: `[iso9660] Path table: %u directories, %u bytes`
- [ ] Commit: `"fs: ISO 9660 path table loader"`

### 3.2 Path Table Lookup

**Prompt:** Implement fast directory lookup via the path table. Given a path like `D:\BOOT\MYLOADER`, split on `\`, start at record 1 (root), search children (records whose parent number matches current), compare directory names. This avoids reading intermediate directory extents from disk. Fall back to directory traversal if path table is absent or exceeds 65,535 entries. Per spec §Path Table Lookup Algorithm. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"fs: ISO 9660 path table lookup"`. After implementation, save any gotchas to MCP memory.

- [ ] Implement `iso9660_path_table_lookup(vol, path)`:
  - [ ] Split path by `\` (Windows-style) or `/`
  - [ ] Start at record 1 (root directory)
  - [ ] For each path component:
    - [ ] Search all records where `parent_num == current_record_number`
    - [ ] Compare directory identifier (case-insensitive) against component
    - [ ] If found → update current record number, continue
    - [ ] If not found → path does not exist, return error
  - [ ] Final component may be a file → use returned LBA to read directory extent
    and search directory records for the filename
- [ ] Case-insensitive comparison (ISO 9660 stores uppercase, user may type mixed case)
- [ ] Fall back to recursive directory traversal if path table is unavailable
- [ ] Log: `[iso9660] Path table lookup: "%s" → LBA %u`
- [ ] Commit: `"fs: ISO 9660 path table lookup"`

---

## 4. File Data Reading

### 4.1 File Extent Reader

**Prompt:** ISO 9660 files (Level 1 and 2) are stored as single contiguous extents on the media. The directory record provides the extent LBA and data length. Reading a file is a direct LBA-based block read — no cluster chains, no fragmentation. Calculate the starting LBA from the file offset, issue READ(10) or `blkdev_read()` commands, and copy data to the caller's buffer. Support partial reads (offset + length within file bounds). Per spec §VFS Integration / File Operations. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"fs: ISO 9660 file extent reader"`. After implementation, save any gotchas to MCP memory.

- [ ] Implement `iso9660_read_file(vol, extent_lba, file_size, offset, length, buffer)`:
  - [ ] Validate: `offset + length <= file_size`, clamp if needed
  - [ ] Calculate starting LBA: `extent_lba + offset / 2048`
  - [ ] Calculate byte offset within first sector: `offset % 2048`
  - [ ] Calculate number of sectors to read: `ceil((offset % 2048 + length) / 2048)`
  - [ ] Read sectors via `blkdev_read()`
  - [ ] Copy from sector-aligned buffer to output buffer (handling partial first/last sectors)
- [ ] Read-ahead: for sequential reads, request 32–64 sectors (64–128 KiB) to amortize
  ATAPI/AHCI command overhead
- [ ] Safety: validate LBA bounds — `extent_lba + sectors_needed <= volume_space_size`
- [ ] Use 64-bit arithmetic for all LBA/offset calculations to prevent overflow
- [ ] Commit: `"fs: ISO 9660 file extent reader"`

### 4.2 Multi-Extent Files (Level 3)

**Prompt:** Level 3 permits files larger than 4 GiB by splitting them across multiple consecutive directory records with the same file identifier. Bit 7 (Multi-Extent) of File Flags is set on all non-final records. The reader must detect multi-extent sequences, collect all extent descriptors (LBA + length), and concatenate them for the full file. Per spec §Multi-Extent Files (Level 3). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"fs: ISO 9660 multi-extent files"`. After implementation, save any gotchas to MCP memory.

- [ ] Detect multi-extent flag: `flags & 0x80` (bit 7 set)
- [ ] When opening a file with multi-extent flag:
  - [ ] Collect consecutive directory records with the same file identifier
  - [ ] Each record provides one extent (LBA + data_length)
  - [ ] Final record has bit 7 clear
  - [ ] Store extent list in file handle: array of `{ lba, length }`
- [ ] Update `iso9660_read_file()` for multi-extent:
  - [ ] Map `(offset, length)` to one or more extents
  - [ ] Handle reads spanning extent boundaries
  - [ ] Total file size = sum of all extent data_length fields
- [ ] Safety: limit extent count (e.g., 4096 extents max)
- [ ] Log: `[iso9660] Multi-extent file: %u extents, total %llu bytes`
- [ ] Commit: `"fs: ISO 9660 multi-extent files"`

---

## 5. Extensions

### 5.1 Joliet SVD Parser

**Prompt:** Joliet, defined by Microsoft, provides Unicode filenames via a Supplementary Volume Descriptor (SVD, type 2). A Joliet SVD is identified by escape sequences `%/@`, `%/C`, or `%/E` at bytes 88–120. Joliet provides a completely independent directory tree with UCS-2 encoded filenames up to 64 characters (128 bytes). If a Joliet SVD is present, preferentially use its directory tree for richer filenames, falling back to the PVD's tree for maximum compatibility. Per spec §Joliet Extension. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"fs: Joliet SVD parser"`. After implementation, save any gotchas to MCP memory.

- [ ] Detect Joliet SVD during volume descriptor scan (type 2):
  - [ ] Check bytes 88–120 for escape sequences:
    - [ ] `%/@` (0x25 0x2F 0x40) — UCS-2 Level 1
    - [ ] `%/C` (0x25 0x2F 0x43) — UCS-2 Level 2
    - [ ] `%/E` (0x25 0x2F 0x45) — UCS-2 Level 3 (full BMP)
- [ ] Parse Joliet SVD (same layout as PVD, but with UCS-2 strings):
  - [ ] Volume Identifier at offset 40 (32 bytes, UCS-2 BE)
  - [ ] Root Directory Record at offset 156 (34 bytes) — independent root
  - [ ] Path Table and Volume Space Size — independent from PVD
- [ ] Implement `iso9660_ucs2_to_utf8(ucs2_be, len, utf8_out)`:
  - [ ] Byte-swap UCS-2 BE to native LE
  - [ ] Convert to UTF-8 (BMP only — no surrogate pair handling needed)
  - [ ] Strip trailing spaces and version suffix
- [ ] If Joliet SVD present: use its directory tree for filenames, PVD for fallback
- [ ] Directory records in Joliet tree: filenames are UCS-2 BE encoded
- [ ] Log: `[iso9660] Joliet SVD: UCS-2 Level %u, volume="%s"`
- [ ] Commit: `"fs: Joliet SVD parser"`

### 5.2 Rock Ridge SUSP Parser

**Prompt:** Rock Ridge uses the System Use area at the end of each directory record (bytes after the file identifier + padding) to store POSIX-compatible metadata via the System Use Sharing Protocol (SUSP). Each SUSP entry has a 2-byte signature, 1-byte length, 1-byte version, then payload. Key entries: `NM` (alternate/long filename), `PX` (POSIX attributes: mode, nlink, uid, gid), `SL` (symbolic link target), `TF` (timestamps), `CL`/`PL` (deep directory nesting beyond 8 levels). A reader that doesn't support Rock Ridge simply ignores System Use data — ISO 9660 base structures remain fully functional. Per spec §Rock Ridge. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"fs: Rock Ridge SUSP parser"`. After implementation, save any gotchas to MCP memory.

- [ ] Detect SUSP: look for `SP` (Sharing Protocol) entry in root directory's `.` record
  - [ ] `SP` entry at System Use offset: signature `0x53 0x50`, check byte = `0xBE`
- [ ] Implement `iso9660_parse_susp(system_use_area, su_length)`:
  - [ ] Walk entries: each has signature (2B), length (1B), version (1B)
  - [ ] `NM` (0x4E 0x4D) — Alternate Name: long filename (may span multiple entries)
  - [ ] `PX` (0x50 0x58) — POSIX File Attributes:
    - [ ] `st_mode` (uint32_bb) — file type + permissions
    - [ ] `st_nlink` (uint32_bb) — hard link count
    - [ ] `st_uid` (uint32_bb), `st_gid` (uint32_bb)
  - [ ] `SL` (0x53 0x4C) — Symbolic Link: component records for link target
  - [ ] `TF` (0x54 0x46) — Time Stamps: creation, modify, access, attributes
  - [ ] `CL` (0x43 0x4C) — Child Link: redirect to real directory (deep nesting)
  - [ ] `PL` (0x50 0x4C) — Parent Link: true parent for relocated directories
  - [ ] `CE` (0x43 0x45) — Continuation Entry: SUSP data overflows to another sector
- [ ] `NM` entries may be continued (flag bit 0): concatenate fragments
- [ ] If Rock Ridge detected, prefer `NM` names over ISO 9660 8.3 names
- [ ] Map `PX` mode to Impossible OS file attributes (uid/gid → SID mapping)
- [ ] Commit: `"fs: Rock Ridge SUSP parser"`
