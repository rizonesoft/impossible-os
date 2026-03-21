# 040.19-Joliet-UDF — Joliet & UDF Read-Only Filesystem Drivers

> **Goal:** Implement read-only drivers for Joliet (ISO 9660 extension) and UDF
> (Universal Disk Format) filesystems. Joliet adds Unicode filename support to
> ISO 9660 via a Supplementary Volume Descriptor with UCS-2 encoding. UDF is a
> complete ECMA-167/ISO 13346 filesystem used on DVD-Video (1.02), DVD-RW (1.50),
> DVD+RW (2.00/2.01), and Blu-Ray (2.50/2.60) media. Both build on the existing
> ATAPI/SCSI block device layer and ISO 9660 infrastructure from §2.1. This
> enables reading files from commercial DVDs, Blu-Ray data discs, and any
> UCS-2-named CD-ROMs — essential for media compatibility and data interchange.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL sector buffers, descriptor
> reads, FID arrays, and extent data blocks. `kmalloc` is ONLY for small kernel
> structs (≤ 4 KB). See `rules.md` Known Gotchas.

> [!WARNING]
> **Read-Only First.** UDF write support requires implementing the full VAT/Sparing
> Table update cycle and metadata partition commits. This TODO covers **read-only**
> access only. Write support is a future P4 extension.

> [!IMPORTANT]
> **Byte Order:** Joliet text fields are **Big Endian** UCS-2 (byte-swap on x86-64).
> UDF on-disk integers are **Little Endian** — do NOT confuse with SCSI transport
> (Big Endian). See spec §Hardware Transport.
>
> **Spec Reference:** All offsets, field layouts, and escape sequences reference the
> [Joliet & UDF Specification](file:///home/derickpayne/impossible-os/specs/storage/filesystems/joliet-udf.md).
>
> **ISO 9660 Prerequisite:** Joliet extends ISO 9660 — the base PVD parser and
> directory record reader from `TODO-040-Filesystem.md §2.1` must be complete first.

---

## TODO Completion Roadmap

> [!IMPORTANT]
> **Five TODO files and two specs** feed into the Joliet/UDF drivers.

### Dependency Graph

```mermaid
graph TD
    SPEC["specs/storage/filesystems/joliet-udf.md<br/>Joliet & UDF Specification"]
    ISO9660_SPEC["specs/storage/filesystems/iso9660.md<br/>ISO 9660 (ECMA-119) Spec"]
    ATAPI["TODO-040.03-ATAPI-SCSI-MMC<br/>ATAPI/SCSI Block Device"]
    ISO9660["TODO-040-Filesystem §2.1<br/>ISO 9660 Read Support"]
    VFS["TODO-040.07-VFS.md<br/>VFS Core"]

    A["§1.1 Joliet SVD Detection"]
    B["§1.2 Joliet Directory Hierarchy"]
    C["§2.1 UCS-2 to UTF-8 Converter"]
    D["§2.2 Joliet Path Resolution"]
    E["§3.1 UDF Volume Recognition"]
    F["§3.2 Anchor Volume Descriptor Pointer"]
    G["§4.1 UDF Volume Descriptor Sequence"]
    H["§4.2 Partition & Logical Volume"]
    I["§5.1 UDF Tag Validation"]
    J["§5.2 File Identifier Descriptors"]
    K["§5.3 File Entry & Allocation"]
    L["§5.4 UDF File Data Reader"]
    M["§6.1 Joliet VFS Registration"]
    N["§6.2 UDF VFS Registration"]
    O["§7.1 Test Suite"]
    P["§8.1 Optical Media Inspector"]
    Q["§8.2 UDF Revision Dashboard"]
    R["§8.3 Unicode Filename Analyzer"]

    SPEC --> A
    ISO9660_SPEC --> A
    ATAPI --> ISO9660
    ISO9660 --> A
    A --> B
    B --> C
    C --> D
    D --> M
    VFS --> M

    SPEC --> E
    ATAPI --> E
    E --> F
    F --> G
    G --> H
    H --> I
    I --> J
    J --> K
    K --> L
    L --> N
    VFS --> N

    M --> O
    N --> O
    J --> P
    G --> Q
    C --> R
```

### Phase-by-Phase Implementation Order

| ⭐ | Phase  | Sections                                    | Depends On               | Status |
| -- | :----: | ------------------------------------------- | ------------------------ | :----: |
| 💎 | **0**  | Prerequisites (spec, ATAPI, ISO 9660)       | —                        |   ⬜   |
| 💎 | **1**  | §1.1 Joliet SVD Detection                   | Phase 0 (ISO 9660)       |   ⬜   |
| 💎 | **1**  | §1.2 Joliet Directory Hierarchy             | Phase 1 (§1.1)           |   ⬜   |
| 💎 | **1**  | §2.1 UCS-2 to UTF-8 Converter              | Phase 1 (§1.2)           |   ⬜   |
| 💎 | **1**  | §2.2 Joliet Path Resolution                 | Phase 1 (§2.1)           |   ⬜   |
| 💎 | **2**  | §3.1 UDF Volume Recognition Sequence        | Phase 0 (ATAPI)          |   ⬜   |
| 💎 | **2**  | §3.2 Anchor Volume Descriptor Pointer       | Phase 2 (§3.1)           |   ⬜   |
| 💎 | **3**  | §4.1 UDF Volume Descriptor Sequence         | Phase 2 (§3.2)           |   ⬜   |
| 💎 | **3**  | §4.2 Partition & Logical Volume Mapping     | Phase 3 (§4.1)           |   ⬜   |
| 💎 | **4**  | §5.1 UDF Descriptor Tag Validation          | Phase 3 (§4.2)           |   ⬜   |
| 💎 | **4**  | §5.2 File Identifier Descriptors            | Phase 4 (§5.1)           |   ⬜   |
| 💎 | **4**  | §5.3 File Entry & Allocation Descriptors    | Phase 4 (§5.1)           |   ⬜   |
| 💎 | **5**  | §5.4 UDF File Data Reader                   | Phase 4 (§5.3)           |   ⬜   |
| 💎 | **5**  | §6.1 Joliet VFS Registration               | Phase 1 + VFS            |   ⬜   |
| 💎 | **5**  | §6.2 UDF VFS Registration                  | Phase 5 (§5.4) + VFS     |   ⬜   |
| 💎 | **6**  | §7.1 Test Suite                             | Phase 5                  |   ⬜   |
| ⭐ | **7**  | §8.1 Optical Media Inspector                | Phase 4 (§5.2)           |   ⬜   |
| ⭐ | **7**  | §8.2 UDF Revision Dashboard                 | Phase 3 (§4.1)           |   ⬜   |
| ⭐ | **7**  | §8.3 Unicode Filename Analyzer              | Phase 1 (§2.1)           |   ⬜   |

> [!NOTE]
> **Phase 0** requires ISO 9660 base (§2.1) and ATAPI driver (040.03).
>
> **Phase 1** adds Joliet: detect the SVD, parse UCS-2 directories, convert to
> UTF-8 for VFS, and resolve paths through the Joliet hierarchy.
>
> **Phases 2–3** bootstrap UDF: volume recognition sequence (BEA01/NSR02/NSR03/TEA01),
> AVDP at sector 256, then the full Volume Descriptor Sequence.
>
> **Phases 4–5** implement UDF file I/O: tag validation, FID parsing for directories,
> File Entries with allocation descriptors, and file data reading.
>
> **Phase 5** also wires both Joliet and UDF into VFS for drive-letter access.
>
> **Phase 6** validates everything with test disc images.
>
> **Phase 7** delivers exclusive features (⭐): optical media inspector, UDF
> revision dashboard, and Unicode filename analyzer.

> [!TIP]
> **Joliet is a quick win** — it reuses the ISO 9660 directory record format.
> The main work is SVD detection (3 escape sequences) and UCS-2→UTF-8 conversion.
>
> **UDF is the heavy lift** — it's a completely separate filesystem with its own
> volume structures, tag-based descriptors, and allocation mechanisms.
>
> **QEMU testing:**
> ```
> qemu-system-x86_64 -m 512M -bios /usr/share/ovmf/OVMF.fd \
>     -drive file=system-disk.img,format=raw,if=none,id=boot \
>     -device virtio-blk-pci,drive=boot \
>     -cdrom test-joliet.iso
> ```
> Create test images: `genisoimage -J -o test-joliet.iso ./testdir` (Joliet)
> `mkudffs --media-type=dvd test-udf.img 2048000` (UDF)
>
> **Memory rule reminder:** Each 2048-byte sector read needs `pmm_alloc_contiguous()`
> if reading multiple sectors. Single-sector descriptor reads fit in `kmalloc`.

---

## 1. Joliet Extension (ISO 9660 SVD)

### 1.1 Joliet SVD Detection

**Prompt:** Extend the ISO 9660 volume descriptor scanner to detect the Joliet
Supplementary Volume Descriptor (SVD). Iterate descriptors from sector 16 until
type 255 terminator. For each type 2 (SVD), check the Escape Sequences field at
offset `0x58` for UCS-2 level indicators: `%/@` (`25 2F 40`), `%/C` (`25 2F 43`),
or `%/E` (`25 2F 45`). Also verify Volume Flags bit 0 at offset `0x07` is zero.
Extract the Joliet root directory record (offset `0x9E`, 34 bytes), path table
locations, and volume identifiers — all in UCS-2 Big Endian. Per Joliet/UDF spec
§Joliet Volume Recognition. After completing all items, mark every item as `[x]`,
update this prompt to a verification prompt, run `bash scripts/build.sh clean`,
and commit as `"fs: joliet SVD detection"`. After implementation, save any gotchas
to MCP memory.

- [ ] Extend `iso9660_scan_descriptors()` to handle type 2 (SVD)
- [ ] For each SVD, check Escape Sequences at offset `0x58` (32 bytes):
  - [ ] Scan for `(25)(2F)(40)` → UCS-2 Level 1
  - [ ] Scan for `(25)(2F)(43)` → UCS-2 Level 2
  - [ ] Scan for `(25)(2F)(45)` → UCS-2 Level 3
- [ ] Verify Volume Flags (offset `0x07`) bit 0 == 0
- [ ] Extract Joliet SVD fields:
  - [ ] Volume Descriptor Type (`0x00`, 1B) — must be `0x02`
  - [ ] Standard Identifier (`0x01`, 5B) — must be `"CD001"`
  - [ ] Volume Flags (`0x07`, 1B) — bit 0 must be 0
  - [ ] Root Directory Record (`0x9E`, 34B) — Joliet root extent LBA + size
  - [ ] Path Table Size (`0x8C`, 4B LE) — Joliet path table size
  - [ ] L Path Table Location (`0x94`, 4B LE) — Type L path table LBA
- [ ] Store Joliet SVD alongside PVD in mount context
- [ ] Prefer Joliet hierarchy over PVD when Joliet SVD is detected
- [ ] Log: `[joliet] SVD detected: UCS-2 Level %u, root at LBA %u`
- [ ] Commit: `"fs: joliet SVD detection"`

### 1.2 Joliet Directory Hierarchy

**Prompt:** Parse directory records from the Joliet root directory extent. Records
use the same ISO 9660 directory record format but filenames are UCS-2 Big Endian
(2 bytes per character). The current directory (`0x00`) and parent directory
(`0x01`) identifiers remain 8-bit single bytes. SEPARATOR 1 (`.`) becomes
`(00)(2E)`, SEPARATOR 2 (`;`) becomes `(00)(3B)`. Maximum identifier length is
128 bytes (64 UCS-2 characters). Sort pad byte is `(00)` instead of `(20)`.
Per Joliet/UDF spec §Joliet Naming Rules. After completing all items, mark every
item as `[x]`, update this prompt to a verification prompt, run
`bash scripts/build.sh clean`, and commit as `"fs: joliet directory parser"`.
After implementation, save any gotchas to MCP memory.

- [ ] Read Joliet root directory extent (LBA + size from SVD Root Directory Record)
- [ ] Parse directory records with UCS-2 filenames:
  - [ ] Record length byte at offset 0 — skip if 0 (padding to sector boundary)
  - [ ] Extent LBA (offset 2, 4B LE) — file/directory data location
  - [ ] Data length (offset 10, 4B LE) — file/directory size
  - [ ] Flags (offset 25, 1B) — bit 1 = directory
  - [ ] File identifier length (offset 32, 1B) — in bytes (UCS-2: always even)
  - [ ] File identifier (offset 33, variable) — UCS-2 Big Endian
- [ ] Handle special identifiers:
  - [ ] `0x00` (1 byte) → current directory `.`
  - [ ] `0x01` (1 byte) → parent directory `..`
- [ ] Handle UCS-2 separators:
  - [ ] `(00)(2E)` → `.` (file extension separator)
  - [ ] `(00)(3B)` → `;` (version separator — strip version number)
- [ ] Validate: identifier length ≤ 128 bytes (64 UCS-2 chars)
- [ ] Validate: no forbidden UCS-2 code points (`*`, `/`, `:`, `;`, `?`, `\`)
- [ ] Implement `joliet_read_dir(extent_lba, size, callback)` — enumerate entries
- [ ] Commit: `"fs: joliet directory parser"`

---

## 2. UCS-2 Text Processing

### 2.1 UCS-2 to UTF-8 Converter

**Prompt:** Implement UCS-2 Big Endian to UTF-8 conversion for Joliet filenames.
On x86-64 (Little Endian), each UCS-2 word must be byte-swapped before conversion.
UCS-2 code points map to UTF-8 as: `0x0000–0x007F` → 1 byte, `0x0080–0x07FF` →
2 bytes, `0x0800–0xFFFF` → 3 bytes. The VFS uses UTF-8 internally, so all Joliet
filenames must be converted before creating VFS nodes. Per Joliet/UDF spec §UCS-2
Character Encoding. After completing all items, mark every item as `[x]`, update
this prompt to a verification prompt, run `bash scripts/build.sh clean`, and
commit as `"fs: ucs2 to utf8 converter"`. After implementation, save any gotchas
to MCP memory.

- [ ] Implement `ucs2be_to_utf8(src, src_bytes, dst, dst_size)`:
  - [ ] Byte-swap each 16-bit word: `ch = (src[i] << 8) | src[i+1]`
  - [ ] UTF-8 encode: 1-byte for ASCII, 2-byte for `0x80–0x7FF`, 3-byte for `0x800–0xFFFF`
  - [ ] NUL-terminate output
  - [ ] Return number of UTF-8 bytes written
- [ ] Implement `utf8_to_ucs2be(src, dst, dst_words)` — reverse for future write support
- [ ] Handle edge cases:
  - [ ] Zero-length input → empty string
  - [ ] Odd byte count → ignore trailing byte
  - [ ] Output buffer too small → truncate cleanly at character boundary
- [ ] Strip trailing UCS-2 `;1` version suffix from filenames
- [ ] Unit test: `"T\0E\0S\0T\0"` (UCS-2 BE) → `"TEST"` (UTF-8)
- [ ] Unit test: `"\x00\xE9"` (UCS-2 BE for é) → `"\xC3\xA9"` (UTF-8)
- [ ] Commit: `"fs: ucs2 to utf8 converter"`

### 2.2 Joliet Path Resolution

**Prompt:** Resolve full paths through the Joliet directory hierarchy. Split the
path by `\` (Windows-style), convert each component from VFS UTF-8 to UCS-2 BE
for comparison, then search the Joliet directory for a match. Joliet abolishes
the 8-level depth limit but enforces a 240-byte total path length. Per Joliet/UDF
spec §Path Length Constraint. After completing all items, mark every item as `[x]`,
update this prompt to a verification prompt, run `bash scripts/build.sh clean`,
and commit as `"fs: joliet path resolution"`. After implementation, save any
gotchas to MCP memory.

- [ ] Implement `joliet_resolve_path(mount, path)`:
  - [ ] Start at Joliet root directory (from SVD Root Directory Record)
  - [ ] Split path by `\` separator
  - [ ] For each component:
    - [ ] Convert UTF-8 component to UCS-2 BE for comparison
    - [ ] Search directory entries for matching identifier
    - [ ] If directory flag set → recurse into subdirectory extent
    - [ ] If not found → return `STATUS_OBJECT_NAME_NOT_FOUND`
- [ ] Case-insensitive comparison (simple ASCII fold for Latin chars)
- [ ] Validate total path length ≤ 240 bytes
- [ ] Implement `joliet_find(mount, dir_lba, dir_size, name)`:
  - [ ] Linear scan of directory records
  - [ ] Compare UCS-2 identifiers (byte-by-byte after normalization)
  - [ ] Return extent LBA + size + flags on match
- [ ] Commit: `"fs: joliet path resolution"`

---

## 3. UDF Volume Bootstrap

### 3.1 UDF Volume Recognition Sequence

**Prompt:** Scan the Volume Recognition Sequence (VRS) starting at sector 16.
Each VRS descriptor is 2048 bytes with a 1-byte Structure Type, 5-byte Identifier,
and 1-byte Version. Scan for `"BEA01"` (beginning), `"NSR02"` (ECMA-167 2nd ed,
UDF ≤2.00) or `"NSR03"` (ECMA-167 3rd ed, UDF ≥2.01), and `"TEA01"` (terminator).
The presence of NSR02 or NSR03 confirms a valid UDF filesystem. Note: Joliet SVDs
may coexist in the same VRS region — a disc can be both ISO 9660+Joliet AND UDF
(bridge format). Per Joliet/UDF spec §UDF Volume Recognition. After completing all
items, mark every item as `[x]`, update this prompt to a verification prompt, run
`bash scripts/build.sh clean`, and commit as `"fs: udf volume recognition"`. After
implementation, save any gotchas to MCP memory.

- [ ] Scan sectors starting at sector 16 (byte offset `0x8000`):
  - [ ] Read each 2048-byte descriptor
  - [ ] Parse Structure Type (offset `0x00`, 1B) — expect `0x00`
  - [ ] Parse Identifier (offset `0x01`, 5B) — check for magic strings
  - [ ] Parse Version (offset `0x06`, 1B) — expect `0x01`
- [ ] Track VRS state machine:
  - [ ] `"BEA01"` → mark BEA found (start of extended area)
  - [ ] `"NSR02"` → UDF detected, ECMA-167 2nd edition (revisions ≤ 2.00)
  - [ ] `"NSR03"` → UDF detected, ECMA-167 3rd edition (revisions ≥ 2.01)
  - [ ] `"TEA01"` → end of VRS scan
  - [ ] `"CD001"` → ISO 9660 descriptor (skip, handled by ISO driver)
- [ ] Stop scanning at type 255 terminator or after 16 sectors with no match
- [ ] Store detected ECMA edition (2 or 3) for revision compatibility checks
- [ ] Log: `[udf] VRS: NSR0%u detected (ECMA-167 %s edition)`
- [ ] Commit: `"fs: udf volume recognition"`

### 3.2 Anchor Volume Descriptor Pointer (AVDP)

**Prompt:** Read the AVDP from logical sector 256 (`0x100`). The AVDP is 32 bytes:
16-byte Descriptor Tag + two 8-byte Extent structures (Main VDS extent + Reserve
VDS extent). Each extent has a 4-byte Length and 4-byte Location (both LE). If
sector 256 is unreadable, try backup locations: last recorded sector and 256
sectors from end. Use READ CAPACITY to determine the last sector. Per Joliet/UDF
spec §AVDP Structure. After completing all items, mark every item as `[x]`, update
this prompt to a verification prompt, run `bash scripts/build.sh clean`, and
commit as `"fs: udf AVDP parser"`. After implementation, save any gotchas to MCP
memory.

- [ ] Read sector 256 (2048 bytes)
- [ ] Parse Descriptor Tag (16 bytes) at offset `0x00`:
  - [ ] Tag Identifier (2B LE) — must be `0x0002` (AVDP)
  - [ ] Descriptor Version (2B LE)
  - [ ] Tag Checksum (1B) — sum of bytes 0–3 and 5–15 mod 256
  - [ ] Tag Serial Number (2B LE)
  - [ ] Descriptor CRC (2B LE) + CRC Length (2B LE)
  - [ ] Tag Location (4B LE) — must match sector number (256)
- [ ] Parse Main VDS Extent (offset `0x10`, 8B):
  - [ ] Length (4B LE) — total VDS size in bytes
  - [ ] Location (4B LE) — starting sector of Main VDS
- [ ] Parse Reserve VDS Extent (offset `0x18`, 8B):
  - [ ] Length (4B LE) + Location (4B LE) — backup VDS
- [ ] Fallback if sector 256 fails:
  - [ ] Issue READ CAPACITY → get last LBA
  - [ ] Try last sector
  - [ ] Try sector (last_lba - 256)
- [ ] Log: `[udf] AVDP: Main VDS at sector %u (%u bytes), Reserve at sector %u`
- [ ] Commit: `"fs: udf AVDP parser"`

---

## 4. UDF Volume Descriptor Sequence

### 4.1 Volume Descriptor Sequence Parser

**Prompt:** Read the Main Volume Descriptor Sequence (VDS) extent pointed to by the
AVDP. The VDS contains multiple descriptors identified by Tag Identifier: Primary
Volume Descriptor (0x0001), Implementation Use VD (0x0004), Partition Descriptor
(0x0005), Logical Volume Descriptor (0x0006), Unallocated Space Descriptor (0x0007),
and Terminating Descriptor (0x0008). Read sectors sequentially until the Terminating
Descriptor. Extract volume name, partition start/length, and logical volume map.
After completing all items, mark every item as `[x]`, update this prompt to a
verification prompt, run `bash scripts/build.sh clean`, and commit as
`"fs: udf VDS parser"`. After implementation, save any gotchas to MCP memory.

- [ ] Read sectors from Main VDS Location through Location + Length:
  - [ ] Parse Descriptor Tag at offset 0 of each sector
  - [ ] Dispatch by Tag Identifier:
    - [ ] `0x0001` → Primary Volume Descriptor
    - [ ] `0x0004` → Implementation Use Volume Descriptor
    - [ ] `0x0005` → Partition Descriptor
    - [ ] `0x0006` → Logical Volume Descriptor
    - [ ] `0x0007` → Unallocated Space Descriptor
    - [ ] `0x0008` → Terminating Descriptor (stop)
- [ ] Parse Primary Volume Descriptor:
  - [ ] Volume Identifier (32B, d-string / CS0) — volume name
  - [ ] Volume Set Identifier (128B)
  - [ ] Recording Date and Time (12B, UDF timestamp)
- [ ] Parse Partition Descriptor:
  - [ ] Partition Number (2B LE)
  - [ ] Partition Contents — `"+NSR02"` or `"+NSR03"`
  - [ ] Access Type (4B LE) — 1=read-only, 2=write-once, 3=rewritable
  - [ ] Partition Starting Location (4B LE) — sector offset
  - [ ] Partition Length (4B LE) — in sectors
- [ ] Parse Logical Volume Descriptor:
  - [ ] Logical Block Size (4B LE) — must be 2048
  - [ ] Logical Volume Contents Use (16B) — FSD extent (LBA + length)
  - [ ] Partition Map(s) — Type 1: partition number mapping
- [ ] If Main VDS is corrupt, fall back to Reserve VDS from AVDP
- [ ] Log: `[udf] Volume: '%s', partition at sector %u, %u sectors`
- [ ] Commit: `"fs: udf VDS parser"`

### 4.2 Partition & Logical Volume Mapping

**Prompt:** Build the partition-to-physical translation layer. UDF uses logical
block addresses within a partition. The physical sector = Partition Starting
Location + logical block address. Parse the Partition Map from the Logical Volume
Descriptor to map partition numbers to physical partitions. Then locate the File
Set Descriptor (FSD) using the Logical Volume Contents Use extent. The FSD points
to the Root Directory ICB (Information Control Block). After completing all items,
mark every item as `[x]`, update this prompt to a verification prompt, run
`bash scripts/build.sh clean`, and commit as `"fs: udf partition mapping"`. After
implementation, save any gotchas to MCP memory.

- [ ] Implement `udf_logical_to_physical(partition_num, logical_block)`:
  - [ ] Look up partition by number → get Partition Starting Location
  - [ ] Return `partition_start + logical_block`
- [ ] Parse Type 1 Partition Map (6 bytes):
  - [ ] Map Type (1B) — `0x01` for Type 1
  - [ ] Map Length (1B) — `0x06`
  - [ ] Volume Sequence Number (2B LE)
  - [ ] Partition Number (2B LE)
- [ ] Locate File Set Descriptor:
  - [ ] Read FSD extent from Logical Volume Contents Use (LBA + partition)
  - [ ] Translate via `udf_logical_to_physical()`
  - [ ] Verify Tag Identifier == `0x0100` (FSD)
- [ ] Parse File Set Descriptor:
  - [ ] Root Directory ICB (16B long_ad): LBA + partition + length
  - [ ] Domain Identifier — `"*OSTA UDF Compliant"`
  - [ ] File Set Descriptor character set
- [ ] Store Root Directory ICB location for directory traversal
- [ ] Log: `[udf] FSD: root ICB at partition %u, block %u`
- [ ] Commit: `"fs: udf partition mapping"`

---

## 5. UDF File I/O

### 5.1 UDF Descriptor Tag Validation

**Prompt:** Implement UDF Descriptor Tag validation. Every UDF descriptor starts
with a 16-byte tag containing: Tag Identifier (2B), Descriptor Version (2B), Tag
Checksum (1B), Tag Serial Number (2B), Descriptor CRC (2B), CRC Length (2B), and
Tag Location (4B). The checksum covers bytes 0–3 and 5–15 (skipping the checksum
byte at offset 4). The CRC is a CRC-CCITT (ISO 3309) over the descriptor data
following the tag, for CRC Length bytes. Validate every descriptor before use.
After completing all items, mark every item as `[x]`, update this prompt to a
verification prompt, run `bash scripts/build.sh clean`, and commit as
`"fs: udf tag validation"`. After implementation, save any gotchas to MCP memory.

- [ ] Implement `udf_validate_tag(sector_data, expected_location)`:
  - [ ] Parse Tag Identifier (offset `0x00`, 2B LE)
  - [ ] Parse Descriptor Version (offset `0x02`, 2B LE)
  - [ ] Compute tag checksum: sum bytes 0–3, 5–15 (skip byte 4), mod 256
  - [ ] Compare against Tag Checksum (offset `0x04`, 1B)
  - [ ] Parse CRC Length (offset `0x0A`, 2B LE)
  - [ ] Compute CRC-CCITT over bytes 16..(16+CRC_Length-1)
  - [ ] Compare against Descriptor CRC (offset `0x08`, 2B LE)
  - [ ] Verify Tag Location (offset `0x0C`, 4B LE) matches `expected_location`
- [ ] Implement CRC-CCITT (polynomial `0x11021`, init `0x0000`)
- [ ] Return tag identifier on success, error on mismatch
- [ ] Log on failure: `[udf] TAG INVALID: sector %u, expected id=%u, got id=%u`
- [ ] Commit: `"fs: udf tag validation"`

### 5.2 File Identifier Descriptors (Directory Entries)

**Prompt:** Parse UDF File Identifier Descriptors (FIDs) for directory traversal.
FIDs are variable-length records within a directory's data extent. Each FID
contains: a 16-byte Descriptor Tag (Tag ID = `0x0101`), File Version Number,
File Characteristics (bit 1 = directory, bit 3 = parent), ICB field pointing to
the file's File Entry, and the filename in UDF d-characters (CS0 encoding, first
byte = compression ID: `0x08` = UTF-8, `0x10` = UTF-16). FIDs are padded to
4-byte boundaries. After completing all items, mark every item as `[x]`, update
this prompt to a verification prompt, run `bash scripts/build.sh clean`, and
commit as `"fs: udf FID parser"`. After implementation, save any gotchas to MCP
memory.

- [ ] Implement `udf_read_dir(mount, dir_icb, callback)`:
  - [ ] Read directory data extent (from File Entry allocation descriptors)
  - [ ] Parse FIDs sequentially within the extent:
    - [ ] Validate Descriptor Tag (Tag ID `0x0101`)
    - [ ] File Version Number (2B LE)
    - [ ] File Characteristics (1B):
      - [ ] Bit 0: hidden
      - [ ] Bit 1: directory
      - [ ] Bit 2: deleted
      - [ ] Bit 3: parent entry (`..\`)
    - [ ] L_FI — Length of File Identifier (1B)
    - [ ] ICB (16B long_ad) — points to file's File Entry
    - [ ] L_IU — Length of Implementation Use (2B LE)
    - [ ] Implementation Use (L_IU bytes) — skip
    - [ ] File Identifier (L_FI bytes) — CS0 encoded filename
  - [ ] Decode filename from CS0:
    - [ ] Byte 0 = Compression ID: `0x08` → UTF-8, `0x10` → UTF-16LE
    - [ ] Remaining bytes = filename characters
  - [ ] Pad to 4-byte boundary: `next_fid = current + 38 + L_FI + L_IU`, aligned up
  - [ ] Skip deleted entries (File Characteristics bit 2)
  - [ ] Invoke callback with: filename, ICB location, is_directory flag
- [ ] Handle parent entry (bit 3): map to `..`
- [ ] Handle root entry (L_FI == 0): map to `.`
- [ ] Log: `[udf] Dir entry: '%s' → ICB at partition %u, block %u`
- [ ] Commit: `"fs: udf FID parser"`

### 5.3 File Entry & Allocation Descriptors

**Prompt:** Parse UDF File Entries (Tag ID `0x0105`) and Extended File Entries
(Tag ID `0x010A`). Each File Entry contains: ICB Tag (type, strategy, flags),
permissions, file link count, record format, UID/GID, timestamps, data length,
number of allocation descriptors, and the allocation descriptor array. Allocation
descriptors come in three forms: Short (8B: length + position), Long (16B:
length + location with partition), and Extended (20B: adds implementation use).
ICB Tag flags bits 0–2 select the form. After completing all items, mark every
item as `[x]`, update this prompt to a verification prompt, run
`bash scripts/build.sh clean`, and commit as `"fs: udf file entry parser"`.
After implementation, save any gotchas to MCP memory.

- [ ] Implement `udf_read_file_entry(mount, icb_location)`:
  - [ ] Translate ICB location to physical sector
  - [ ] Read sector, validate Descriptor Tag (ID `0x0105` or `0x010A`)
  - [ ] Parse ICB Tag (20B):
    - [ ] Prior Recorded Number of Direct Entries (4B LE)
    - [ ] Strategy Type (2B LE) — typically 4 (normal) or 4096 (VAT)
    - [ ] File Type (1B): 4=directory, 5=regular file, 12=symlink
    - [ ] Flags (2B LE) — bits 0–2: allocation descriptor type
  - [ ] Parse file metadata:
    - [ ] Permissions (4B LE)
    - [ ] File Link Count (2B LE)
    - [ ] Information Length (8B LE) — logical file size
    - [ ] Logical Blocks Recorded (8B LE)
    - [ ] Access/Modification/Attribute timestamps (12B each, UDF timestamp)
  - [ ] Parse Extended Attributes length (4B LE)
  - [ ] Parse Allocation Descriptors length (4B LE)
- [ ] Parse allocation descriptors based on ICB Tag flags bits 0–2:
  - [ ] Type 0 — Short Allocation Descriptor (8B each):
    - [ ] Extent Length (4B LE) — upper 2 bits = type (0=recorded, 1=unrecorded)
    - [ ] Extent Position (4B LE) — logical block within partition
  - [ ] Type 1 — Long Allocation Descriptor (16B each):
    - [ ] Extent Length (4B LE)
    - [ ] Extent Location (6B) — partition number + logical block
  - [ ] Type 3 — Embedded data (data within the File Entry itself)
- [ ] Build extent list from allocation descriptors (sorted by offset)
- [ ] Log: `[udf] File Entry: type=%u, size=%llu, %u alloc descriptors`
- [ ] Commit: `"fs: udf file entry parser"`

### 5.4 UDF File Data Reader

**Prompt:** Implement reading file data using the allocation descriptor list from
the File Entry. Convert logical block addresses to physical sectors, read from
disk, and assemble into the output buffer. Handle reads spanning multiple extents,
partial block reads, embedded data (Type 3 alloc), and unrecorded extents (return
zeros). After completing all items, mark every item as `[x]`, update this prompt
to a verification prompt, run `bash scripts/build.sh clean`, and commit as
`"fs: udf file data reader"`. After implementation, save any gotchas to MCP memory.

- [ ] Implement `udf_read_data(mount, file_entry, offset, length, buffer)`:
  - [ ] If embedded data (alloc type 3): copy directly from File Entry body
  - [ ] Otherwise iterate allocation descriptors:
    - [ ] Find extent covering current offset
    - [ ] Translate logical block → physical sector
    - [ ] Read sectors from disk
    - [ ] Handle extent type 1 (unrecorded) → fill with zeros
  - [ ] Handle reads spanning multiple extents
  - [ ] Cap at Information Length (file size)
- [ ] Handle partial sector reads at start/end of range
- [ ] Implement `udf_resolve_path(mount, path)`:
  - [ ] Start at root directory ICB (from FSD)
  - [ ] Split path, lookup each component via `udf_read_dir` + FID search
  - [ ] Follow ICB → File Entry → check if directory → recurse
- [ ] Commit: `"fs: udf file data reader"`

---

## 6. VFS Integration

### 6.1 Joliet VFS Registration

**Prompt:** Register Joliet as a VFS filesystem overlay on ISO 9660.  When an
ISO 9660 mount detects a Joliet SVD, the VFS should use the Joliet directory
hierarchy for filename resolution (Unicode names) while falling back to PVD for
metadata. Implement `vfs_ops` callbacks: `open`, `close`, `read`, `readdir`,
`finddir`, `stat`. Write callbacks return `-EROFS`. After completing all items,
mark every item as `[x]`, update this prompt to a verification prompt, run
`bash scripts/build.sh clean`, and commit as `"fs: joliet VFS registration"`.
After implementation, save any gotchas to MCP memory.

- [ ] Extend `iso9660_mount()` to check for Joliet SVD
- [ ] If Joliet SVD detected: use Joliet root directory extent for VFS
- [ ] Implement `joliet_open(node, flags)` — resolve via Joliet hierarchy
- [ ] Implement `joliet_close(node)` — free cached UCS-2 data
- [ ] Implement `joliet_read(node, offset, size, buf)` — read from ISO extent
- [ ] Implement `joliet_readdir(node, index)` — enumerate Joliet entries (UCS-2→UTF-8)
- [ ] Implement `joliet_finddir(node, name)` — search Joliet directory (UTF-8→UCS-2)
- [ ] Implement `joliet_stat(node, stat)` — populate from directory record
- [ ] Write ops → return `-EROFS`
- [ ] Log: `[joliet] Mounted Joliet volume on drive %c:`
- [ ] Commit: `"fs: joliet VFS registration"`

### 6.2 UDF VFS Registration

**Prompt:** Register UDF as a VFS filesystem driver. Detect UDF via the Volume
Recognition Sequence (NSR02/NSR03). On mount: run full bootstrap (VRS → AVDP →
VDS → Partition Map → FSD → Root ICB). Implement `vfs_ops` callbacks. Write
callbacks return `-EROFS`. Support UDF revisions 1.02 through 2.60 for full
optical media compatibility. After completing all items, mark every item as `[x]`,
update this prompt to a verification prompt, run `bash scripts/build.sh clean`,
and commit as `"fs: udf VFS registration"`. After implementation, save any
gotchas to MCP memory.

- [ ] Create `src/kernel/fs/udf.c` and `include/kernel/fs/udf.h`
- [ ] Define `struct udf_mount` — AVDP, VDS, partition map, FSD, root ICB
- [ ] Implement `udf_detect(blkdev)` — scan VRS for NSR02/NSR03
- [ ] Implement `udf_mount(blkdev)`:
  - [ ] Run bootstrap: VRS → AVDP → VDS → partition map → FSD → root ICB
  - [ ] Create VFS mount from root directory
- [ ] Implement `vfs_ops` callbacks:
  - [ ] `udf_open(node, flags)` — resolve File Entry via ICB
  - [ ] `udf_close(node)` — free cached allocations
  - [ ] `udf_read(node, offset, size, buf)` — allocation-based read
  - [ ] `udf_readdir(node, index)` — FID enumeration
  - [ ] `udf_finddir(node, name)` — FID search
  - [ ] `udf_stat(node, stat)` — from File Entry metadata
  - [ ] Write ops → return `-EROFS`
- [ ] Support UDF revisions: 1.02, 1.50, 2.00, 2.01, 2.50, 2.60
- [ ] Log: `[udf] Mounted UDF rev %u.%02u volume '%s' on drive %c:`
- [ ] Commit: `"fs: udf VFS registration"`

---

## 7. Testing & Validation

### 7.1 Joliet & UDF Test Suite

**Prompt:** Create test disc images for both Joliet and UDF. Test Joliet: Unicode
filenames, deep directories, mixed ISO+Joliet fallback. Test UDF: 1.02 (DVD-Video
structure), 2.01 (standard data), 2.50+ (Blu-Ray-style metadata partition). After
completing all items, mark every item as `[x]`, update this prompt to a verification
prompt, run `bash scripts/build.sh clean`, and commit as `"test: joliet and udf
test suite"`. After implementation, save any gotchas to MCP memory.

- [ ] Joliet test images (via `genisoimage -J`):
  - [ ] ASCII filenames → verify Joliet preference over PVD
  - [ ] Unicode filenames (accented chars, CJK) → verify UCS-2→UTF-8
  - [ ] Deep directory (>8 levels) → verify Joliet abolishes depth limit
  - [ ] Long filenames (64 UCS-2 chars) → verify 128-byte limit
  - [ ] Mixed ISO+Joliet disc → verify Joliet used when available
- [ ] UDF test images (via `mkudffs`):
  - [ ] UDF 1.02: basic directory + files → verify FID parsing
  - [ ] UDF 2.01: files with embedded data (small files in File Entry)
  - [ ] Large file (>4 MB, multiple allocation descriptors)
  - [ ] Deep directory path
  - [ ] Bridge disc: ISO 9660 + Joliet + UDF on same image
- [ ] Tag validation: deliberately corrupt a tag → verify rejection
- [ ] QEMU: `-cdrom test.iso` for optical, `-drive file=test.img` for block
- [ ] Commit: `"test: joliet and udf test suite"`

---

## 8. Optical Media Intelligence (🚀 Impossible OS Exclusive)

### 8.1 Optical Media Inspector

**Prompt:** Build an Optical Media Inspector panel for Disk Manager. When an
optical disc is inserted, display: disc type (CD/DVD/BD), session info, filesystem
type (ISO 9660/Joliet/UDF/bridge), UDF revision, volume name, total capacity,
used space, track count, and disc status (finalized/open). Aggregate info from
READ TOC, READ DISC INFORMATION, and filesystem detection results. Neither Windows
nor Linux surfaces all this info in a single GUI panel. After completing all items,
mark every item as `[x]`, update this prompt to a verification prompt, run
`bash scripts/build.sh clean`, and commit as `"fs: optical media inspector"`.
After implementation, save any gotchas to MCP memory.

> [!TIP]
> **Competitive Edge:** Windows shows basic disc properties. Linux requires CLI
> tools (`isoinfo`, `udfinfo`, `dvd+rw-mediainfo`). Impossible OS combining all
> optical disc metadata in a single GUI panel is unique.

- [ ] Aggregate disc metadata:
  - [ ] Media type: CD-ROM, DVD-ROM, DVD±R/RW, BD-ROM, BD-R/RE
  - [ ] Filesystem(s): ISO 9660 only / ISO+Joliet / UDF only / bridge
  - [ ] UDF revision: 1.02 / 1.50 / 2.00 / 2.01 / 2.50 / 2.60
  - [ ] Volume name (from PVD, Joliet SVD, or UDF PVD)
  - [ ] Total capacity / used space
  - [ ] Session/track info from READ TOC
  - [ ] Disc status: finalized / appendable / blank
- [ ] Wire to Disk Manager: "Optical Media" properties panel
- [ ] Commit: `"fs: optical media inspector"`

### 8.2 UDF Revision Dashboard (🚀 Impossible OS Exclusive)

**Prompt:** Display UDF revision compatibility info. Show which UDF revision is
present, what media types it was designed for (1.02=DVD-Video, 1.50=CD-R, 2.50=BD),
whether the partition has VAT (write-once) or Sparing Table (rewritable), and
a compatibility matrix showing which OSes can read this specific revision. Neither
Windows nor Linux shows UDF revision context in a GUI. After completing all items,
mark every item as `[x]`, update this prompt to a verification prompt, run
`bash scripts/build.sh clean`, and commit as `"fs: udf revision dashboard"`.
After implementation, save any gotchas to MCP memory.

> [!TIP]
> **Competitive Edge:** Windows `udfs.sys` silently handles revisions. Linux
> `udftools` requires CLI. Impossible OS explaining which UDF revision is present
> and its implications is a unique educational feature.

- [ ] Parse UDF revision from Implementation Use VD or domain identifiers
- [ ] Display revision-specific info:
  - [ ] 1.02: "DVD-Video format, plain build, no VAT"
  - [ ] 1.50: "CD-R/DVD-R support, VAT for sequential write"
  - [ ] 2.00: "Stream files, ACLs, real-time files"
  - [ ] 2.01: "Bugfix for 2.00"
  - [ ] 2.50: "Metadata Partition for crash recovery"
  - [ ] 2.60: "Pseudo OverWrite for BD-R"
- [ ] Show compatibility matrix (which OSes support each revision)
- [ ] Wire to Disk Manager: "UDF Details" sub-panel
- [ ] Commit: `"fs: udf revision dashboard"`

### 8.3 Unicode Filename Analyzer (🚀 Impossible OS Exclusive)

**Prompt:** Analyze Unicode filenames on Joliet/UDF volumes. Show: filename
encoding (UCS-2/UTF-8/UTF-16), character script distribution (Latin, CJK, Cyrillic,
Arabic), longest filename, total unique characters used, and any filenames that
would be truncated by ISO 9660 8.3 naming. Useful for diagnosing cross-platform
filename issues. Neither Windows nor Linux provides filename analytics. After
completing all items, mark every item as `[x]`, update this prompt to a verification
prompt, run `bash scripts/build.sh clean`, and commit as
`"fs: unicode filename analyzer"`. After implementation, save any gotchas to MCP
memory.

> [!TIP]
> **Competitive Edge:** No OS provides disc-wide filename analysis. Impossible OS
> showing Unicode script distribution and 8.3 compatibility is unique for forensics
> and cross-platform troubleshooting.

- [ ] Walk all directory entries on Joliet/UDF volume
- [ ] Collect statistics:
  - [ ] Total files / total directories
  - [ ] Filename encoding: UCS-2 (Joliet) vs CS0 UTF-8/UTF-16 (UDF)
  - [ ] Character script histogram (Latin, CJK, Cyrillic, Arabic, etc.)
  - [ ] Longest filename (characters and bytes)
  - [ ] Files incompatible with ISO 9660 Level 1 (8.3)
  - [ ] Files with forbidden Win32 characters
- [ ] Wire to Disk Manager: "Filename Analysis" tab
- [ ] Commit: `"fs: unicode filename analyzer"`

---

## Priority Order

| ⭐ | Priority | Section                            | Description                                                          |
| -- | -------- | ---------------------------------- | -------------------------------------------------------------------- |
| 💎 | 🔴 P0   | 1.1 Joliet SVD Detection           | Foundation — detect Joliet on ISO 9660 discs                         |
| 💎 | 🔴 P0   | 1.2 Joliet Directory Hierarchy     | Foundation — parse UCS-2 directory records                           |
| 💎 | 🔴 P0   | 2.1 UCS-2 to UTF-8 Converter      | Foundation — text conversion for all Joliet operations               |
| 💎 | 🔴 P0   | 3.1 UDF Volume Recognition        | Foundation — detect UDF on optical media                             |
| 💎 | 🔴 P0   | 3.2 AVDP Parser                   | Foundation — locate Main Volume Descriptor Sequence                  |
| 💎 | 🟠 P1   | 2.2 Joliet Path Resolution        | Core — resolve paths through Joliet hierarchy                        |
| 💎 | 🟠 P1   | 4.1 UDF VDS Parser                | Core — parse volume, partition, and logical volume descriptors       |
| 💎 | 🟠 P1   | 4.2 Partition & Logical Mapping   | Core — translate logical blocks to physical sectors                  |
| 💎 | 🟠 P1   | 5.1 UDF Tag Validation            | Integrity — CRC-CCITT + checksum for every descriptor                |
| 💎 | 🟠 P1   | 5.2 FID Parser                    | Core — UDF directory entry parsing                                   |
| �� | 🟠 P1   | 5.3 File Entry & Allocation       | Core — UDF file metadata + extent mapping                            |
| 💎 | 🟠 P1   | 5.4 UDF File Data Reader          | Core — actually read UDF file contents                               |
| 💎 | 🟠 P1   | 6.1 Joliet VFS Registration       | Integration — Joliet mountable as drive letter                       |
| 💎 | 🟠 P1   | 6.2 UDF VFS Registration          | Integration — UDF mountable as drive letter                          |
| 💎 | 🟡 P2   | 7.1 Test Suite                    | Quality — automated validation with test images                      |
| ⭐ | 🟢 P3   | 8.1 Optical Media Inspector       | **All disc metadata in one panel** — no OS does this 🚀              |
| ⭐ | 🟢 P3   | 8.2 UDF Revision Dashboard        | **Revision context + compat matrix** — unique 🚀                     |
| ⭐ | 🟢 P3   | 8.3 Unicode Filename Analyzer     | **Disc-wide filename analytics** — unique forensic feature 🚀        |
| 🔵 | 🔵 P4   | UDF Write (VAT/Sparing Table)     | Full R/W — future stretch goal                                       |
| 🔵 | 🔵 P4   | UDF 2.50+ Metadata Partition      | Blu-Ray crash recovery — future stretch goal                         |
| 🔵 | 🔵 P4   | Multi-session CD support          | Incremental CD-R writes — future stretch goal                        |

> [!NOTE]
> ⭐ = Feature where Impossible OS can be **superior** to Windows 11 and Linux.

---

## OS Comparison

| Feature                            | 🪟 Windows 11                      | 🐧 Linux                            | 🚀 Impossible OS                                |
| ---------------------------------- | ---------------------------------- | ------------------------------------ | ------------------------------------------------ |
| ISO 9660 read                      | ✅ Built-in `cdfs.sys`             | ✅ Built-in `isofs`                  | ⬜ §2.1 P0 (master TODO)                         |
| Joliet (UCS-2 filenames)           | ✅ Built-in                        | ✅ Built-in                          | ⬜ §1.1–§2.2 P0                                  |
| UDF 1.02 (DVD-Video)              | ✅ `udfs.sys`                      | ✅ `udf` module                      | ⬜ §3.1–§6.2 P1                                  |
| UDF 1.50 (VAT for CD-R)           | ✅ `udfs.sys`                      | ✅ `udf` module                      | ⬜ §6.2 P1                                       |
| UDF 2.00/2.01 (standard)          | ✅ `udfs.sys`                      | ✅ `udf` module                      | ⬜ §6.2 P1                                       |
| UDF 2.50/2.60 (Blu-Ray)           | ✅ `udfs.sys`                      | ✅ `udf` module                      | ⬜ §6.2 P1 (metadata partition P4)               |
| Bridge disc (ISO+Joliet+UDF)      | ✅ Prefers UDF                     | ✅ Configurable                      | ⬜ §7.1 P2                                       |
| UDF write support                  | ✅ R/W for CD/DVD-RW               | ✅ R/W via `pktcdvd`                 | ⬜ Future P4                                      |
| Tag CRC validation                 | ✅ `udfs.sys`                      | ✅ `udf` module                      | ⬜ §5.1 P1                                       |
| **Optical Media Inspector**        | ⚠️ Basic disc properties only      | ⬜ CLI tools only                    | ⬜ §8.1 P3 — **unified GUI panel** 🚀            |
| **UDF Revision Dashboard**        | ⬜ No revision context             | ⬜ `udfinfo` CLI only               | ⬜ §8.2 P3 — **revision + compat matrix** 🚀     |
| **Unicode Filename Analyzer**      | ⬜ Not available                   | ⬜ Not available                     | ⬜ §8.3 P3 — **disc-wide analytics** 🚀          |

> **After P0+P1 items:** Impossible OS reads Joliet and UDF discs — matching
> Windows and Linux for standard optical media access.
> **After P2–P3 exclusive features:** Exceeds both — optical media inspector,
> UDF revision dashboard, and Unicode filename analyzer are unique to Impossible OS.
> **After P4 items:** Full spec parity including UDF write, metadata partitions,
> and multi-session support.

---

## Key Files

| File                                   | Purpose                                                         |
| -------------------------------------- | --------------------------------------------------------------- |
| `src/kernel/fs/iso9660.c`             | [MODIFY] Add Joliet SVD detection + UCS-2 directory parsing     |
| `include/kernel/fs/iso9660.h`          | [MODIFY] Add Joliet structs + UCS-2 conversion prototypes       |
| `src/kernel/fs/udf.c`                 | [NEW] UDF filesystem driver                                     |
| `include/kernel/fs/udf.h`             | [NEW] UDF structures, tag IDs, allocation descriptor types      |
| `src/kernel/fs/partition.c`           | [MODIFY] Add UDF detection via VRS scan                         |
| `specs/storage/filesystems/joliet-udf.md` | Joliet & UDF on-disk specification (299 lines)              |
| `specs/storage/filesystems/iso9660.md` | ISO 9660 (ECMA-119) specification                              |
