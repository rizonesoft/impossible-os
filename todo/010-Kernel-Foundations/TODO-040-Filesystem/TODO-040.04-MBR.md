# 040.04-MBR — Master Boot Record Partitioning

> **Goal:** Implement a production-grade MBR partition table parser and writer
> for the Impossible OS storage stack. Support primary partitions (4 max),
> Extended Boot Record (EBR) linked list traversal for logical partitions,
> CHS↔LBA translation, 1-MiB alignment enforcement, GPT Protective MBR
> detection, and Disk Signature management. The existing `mbr.c` provides
> basic 4-entry parsing — this TODO enhances it to a complete, robust MBR
> subsystem with write support, EBR traversal, partition creation, and
> the `diskpart` CLI tooling.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for sector buffers (512 bytes+). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). See `rules.md` Known Gotchas.

> [!WARNING]
> **Little-Endian Awareness:** All multi-byte fields in MBR partition entries (Starting LBA, Total Sectors, Disk Signature) are stored in **Little-Endian** format. On x86-64 this is native byte order — no swapping needed. However, CHS fields use a **fractured bit-packed** layout where the 10-bit cylinder is split across two bytes. Always use bitwise extraction, never raw struct overlays for CHS.

> [!IMPORTANT]
> **Spec Reference:** All offsets, field layouts, CHS algorithms, and EBR rules reference the
> [MBR Partitioning Specification](file:///home/derickpayne/impossible-os/specs/mbr.md)
> in the repo at `specs/mbr.md`.

---

## 1. MBR Sector Parsing

### 1.1 Full 512-Byte MBR Layout Parser

**Prompt:** The existing `mbr.c` parses the 4 partition entries but doesn't extract the 32-bit Unique Disk Signature (offsets `0x1B8`–`0x1BB`) or validate the Boot Record Signature (`0xAA55`). Enhance the parser to decode the complete MBR anatomy: bootstrap code region (440 bytes, not interpreted), disk signature (4 bytes LE), reserved bytes (2 bytes), all 4 partition entries (16 bytes each), and the `0xAA55` magic. Reject MBR sectors where the magic is absent. Store the disk signature in `blkdev->disk_id` for persistent volume tracking. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"mbr: full 512-byte layout parser"`. Add notes directly in this TODO section.

- [ ] Validate Boot Record Signature: bytes 510–511 must be `0x55`, `0xAA`
- [ ] Reject sector if signature missing — return `MBR_ERR_NO_SIGNATURE`
- [ ] Extract 32-bit Disk Signature from offsets 440–443 (Little-Endian, native on x86)
- [ ] Store disk signature: `blkdev->disk_id = disk_signature`
- [ ] Log: `[MBR] Disk Signature: 0x%08X`
- [ ] Extract reserved/copy-protection field at offsets 444–445 (log if non-zero)
- [ ] Parse all 4 × 16-byte partition entries at offsets 446, 462, 478, 494
- [ ] For each entry: extract Boot Indicator, CHS start, Type, CHS end, LBA start, Total Sectors
- [ ] Skip empty entries: all 16 bytes zero → `type == 0x00`
- [ ] Validate Boot Indicator: only `0x00` (inactive) or `0x80` (active) are legal
- [ ] Flag if multiple entries have boot indicator `0x80` — log warning
- [ ] Log: `[MBR] Entry %d: type=0x%02X, LBA=%u, sectors=%u, boot=%s`
- [ ] Commit: `"mbr: full 512-byte layout parser"`

### 1.2 Partition Type Recognition

**Prompt:** The Partition Type byte (offset 4 in each 16-byte entry) identifies the filesystem format. Expand recognition beyond the current `0x0C` (FAT32), `0x83` (Linux), `0xDA` (IXFS) to cover the full range needed for robust interoperability. Critically, detect `0xEE` (GPT Protective MBR) — when found, abort MBR parsing and redirect to the GPT parser. Detect `0x05`/`0x0F` (Extended Partition) to trigger EBR chain traversal (§2). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"mbr: expanded partition type recognition"`. Add notes directly in this TODO section.

- [ ] Define `enum mbr_partition_type` with all recognized codes:
  - [ ] `0x00` — Empty / Unallocated
  - [ ] `0x01` — FAT12
  - [ ] `0x04` — FAT16 (≤32 MB)
  - [ ] `0x05` — Extended Partition (CHS) → trigger EBR traversal
  - [ ] `0x06` — FAT16B (>32 MB)
  - [ ] `0x07` — NTFS / HPFS / exFAT
  - [ ] `0x0B` — FAT32 (CHS)
  - [ ] `0x0C` — FAT32 (LBA)
  - [ ] `0x0E` — FAT16 (LBA)
  - [ ] `0x0F` — Extended Partition (LBA) → trigger EBR traversal
  - [ ] `0x27` — Windows Recovery Environment (do NOT auto-mount)
  - [ ] `0x82` — Linux Swap
  - [ ] `0x83` — Linux Native (ext2/3/4)
  - [ ] `0xDA` — IXFS (Impossible OS native)
  - [ ] `0xEE` — GPT Protective MBR → abort, redirect to GPT parser
- [ ] Implement `mbr_type_to_string(type)` → human-readable name for logging
- [ ] On type `0xEE` in any entry: `return MBR_REDIRECT_GPT` and log redirect
- [ ] On type `0x05` or `0x0F`: flag entry as extended container, pass to EBR walker (§2)
- [ ] On type `0x27`: flag as hidden — do not auto-mount
- [ ] Log: `[MBR] Entry %d: %s (0x%02X)`
- [ ] Commit: `"mbr: expanded partition type recognition"`

---

## 2. Extended Boot Record (EBR) Linked List Traversal

### 2.1 EBR Chain Walker

**Prompt:** When an MBR entry has type `0x05` (CHS Extended) or `0x0F` (LBA Extended), it defines a container region subdivided into logical partitions via a linked list of Extended Boot Records. Each EBR is a 512-byte sector with the same layout as the MBR, but only entries 1 and 2 are used. Entry 1 defines the local logical volume (LBA relative to the current EBR). Entry 2 points to the next EBR (LBA relative to the FIRST EBR in the chain). Entry 2 all-zeros terminates the chain. The parser must cache the absolute LBA of the first EBR permanently and apply two distinct relative addressing rules. Cap traversal at 128 logical partitions to prevent infinite loops on corrupted disks. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"mbr: EBR linked list traversal"`. Add notes directly in this TODO section.

- [ ] Detect Extended Partition entry in MBR (type `0x05` or `0x0F`)
- [ ] Read first EBR sector at `extended_entry.start_lba`
- [ ] Cache `first_ebr_lba = extended_entry.start_lba` — persist for entire traversal
- [ ] Validate EBR boot signature `0xAA55` at offsets 510–511
- [ ] Parse EBR Entry 1 (offset `0x1BE`): local logical volume
  - [ ] **Rule 1:** `volume_lba = current_ebr_lba + entry1.start_lba`
  - [ ] Store volume type, absolute LBA, total sectors
  - [ ] Register as logical partition (numbering starts at 5 for MBR)
- [ ] Parse EBR Entry 2 (offset `0x1CE`): next EBR pointer
  - [ ] **Rule 2:** `next_ebr_lba = first_ebr_lba + entry2.start_lba`
  - [ ] If entry 2 is all zeros → end of chain, stop traversal
- [ ] Verify EBR entries 3 and 4 (offsets `0x1DE`, `0x1EE`) are zero — log warning if not
- [ ] Loop: read next EBR, repeat Entry 1 + Entry 2 processing
- [ ] Safety cap: maximum 128 logical partitions per extended container
- [ ] Safety: if `next_ebr_lba` points outside disk bounds → abort with error
- [ ] Safety: if `next_ebr_lba == current_ebr_lba` → circular link, abort
- [ ] Log each logical: `[MBR] Logical %d: type=0x%02X, LBA=%u, sectors=%u`
- [ ] Commit: `"mbr: EBR linked list traversal"`

---

## 3. CHS Addressing & Bit-Packing

### 3.1 CHS Extraction & Encoding

**Prompt:** Implement bidirectional CHS↔LBA translation. The 3-byte CHS tuple uses a fractured bit-packed format: Byte 1 = Head (8 bits), Byte 2 low 6 bits = Sector, Byte 2 high 2 bits = Cylinder bits 8–9, Byte 3 = Cylinder bits 0–7. Extraction requires bitwise AND, shift, and OR operations. The special tuple `FE FF FF` (Head=254, Sector=63, Cylinder=1023) indicates CHS overflow — the field is meaningless and LBA must be used exclusively. For partitions within the 8.4 GB CHS limit (1024×256×63×512 bytes), compute valid CHS from LBA using the standard formulas. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"mbr: CHS extraction and encoding"`. Add notes directly in this TODO section.

- [ ] Implement `chs_extract(bytes[3], *cylinder, *head, *sector)`:
  - [ ] `head = bytes[0]`
  - [ ] `sector = bytes[1] & 0x3F`
  - [ ] `cylinder = ((bytes[1] & 0xC0) << 2) | bytes[2]`
- [ ] Implement `chs_encode(cylinder, head, sector, bytes[3])`:
  - [ ] `bytes[0] = head`
  - [ ] `bytes[1] = (sector & 0x3F) | ((cylinder >> 2) & 0xC0)`
  - [ ] `bytes[2] = cylinder & 0xFF`
- [ ] Implement `chs_to_lba(cylinder, head, sector, heads_per_cyl, sectors_per_track)`:
  - [ ] `LBA = (cylinder × heads_per_cyl + head) × sectors_per_track + (sector - 1)`
- [ ] Implement `lba_to_chs(lba, *cylinder, *head, *sector, heads_per_cyl, sectors_per_track)`:
  - [ ] `cylinder = lba / (heads_per_cyl × sectors_per_track)`
  - [ ] `head = (lba / sectors_per_track) % heads_per_cyl`
  - [ ] `sector = (lba % sectors_per_track) + 1`
- [ ] Detect CHS overflow: if `cylinder > 1023` → write dummy tuple `FE FF FF`
- [ ] Detect CHS overflow on read: if CHS == `FE FF FF` or `FF FF FF` → ignore CHS, use LBA only
- [ ] Default geometry assumption: 255 heads, 63 sectors/track (standard for drives >504 MB)
- [ ] Log CHS overflow: `[MBR] CHS overflow at LBA %u — using LBA addressing only`
- [ ] Commit: `"mbr: CHS extraction and encoding"`

---

## 4. Partition Alignment

### 4.1 Modern 1-MiB Alignment Enforcement

**Prompt:** Legacy partitioning tools start the first partition at LBA 63 (one track offset). Modern partitioning must enforce 1-MiB alignment (LBA 2048) for optimal performance on Advanced Format 4K-sector drives and SSDs. When creating new partitions, always align the starting LBA to 2048 (or the nearest 2048-sector boundary for subsequent partitions). When reading existing partitions, detect legacy 63-sector alignment and log a performance warning without modifying the table. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"mbr: 1-MiB partition alignment"`. Add notes directly in this TODO section.

- [ ] Define `MBR_ALIGNMENT_SECTORS = 2048` (1 MiB / 512 bytes)
- [ ] On partition creation: round starting LBA up to next 2048-sector boundary
  - [ ] First partition: start at LBA 2048 (skip MBR + alignment gap)
  - [ ] Subsequent partitions: `align_up(previous_end, 2048)`
- [ ] Implement `align_lba_up(lba, alignment)`: `((lba + alignment - 1) / alignment) * alignment`
- [ ] On partition read: detect legacy alignment (LBA 63) → log warning:
  - [ ] `[MBR] WARNING: Partition %d at LBA 63 — legacy alignment, poor 4K/SSD performance`
- [ ] On partition read: verify alignment to 2048 boundary → log if misaligned
- [ ] Never auto-modify existing partition alignment (data destructive)
- [ ] Commit: `"mbr: 1-MiB partition alignment"`

---

## 5. MBR Write Support

### 5.1 Partition Table Writer

**Prompt:** Implement MBR write support for creating and managing partitions. Build a complete 512-byte MBR sector in memory: zero the bootstrap code area (440 bytes), write the disk signature, set reserved bytes to `0x0000`, encode the 4 partition entries with correct LBA fields, CHS fields (using §3.1 encoding, with overflow dummy for large offsets), and write `0x55`/`0xAA` to offsets 510/511. Write the sector to LBA 0 via `blkdev_write()`. Preserve the existing bootstrap code if updating an existing MBR (read-modify-write: read sector, update partition table only, write back). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"mbr: partition table writer"`. Add notes directly in this TODO section.

- [ ] Implement `mbr_write(blkdev, partitions[4], disk_signature)`:
  - [ ] Allocate 512-byte sector buffer via `pmm_alloc_contiguous()`
  - [ ] Read existing sector at LBA 0 (preserve bootstrap code)
  - [ ] Keep bytes 0–439 (bootstrap code) intact from existing sector
  - [ ] Write disk signature at offsets 440–443 (LE)
  - [ ] Write `0x0000` at offsets 444–445 (reserved)
  - [ ] For each of 4 entries: encode 16-byte partition entry at offsets 446, 462, 478, 494
  - [ ] Encode partition entry fields:
    - [ ] Boot Indicator: `0x80` or `0x00`
    - [ ] Starting CHS: use `chs_encode()`, write `FE FF FF` if LBA > 8.4 GB
    - [ ] Partition Type: raw byte
    - [ ] Ending CHS: compute from `start_lba + total_sectors - 1`, overflow if needed
    - [ ] Starting LBA: 4 bytes LE
    - [ ] Total Sectors: 4 bytes LE
  - [ ] Write `0x55` at offset 510, `0xAA` at offset 511
  - [ ] Write sector to LBA 0 via `blkdev_write()`
- [ ] Implement `mbr_create_fresh(blkdev, disk_signature)`:
  - [ ] Zero entire 512-byte sector
  - [ ] Write disk signature + reserved + signature bytes
  - [ ] All 4 partition entries zeroed (empty disk)
- [ ] Log: `[MBR] Written: sig=0x%08X, %d partitions`
- [ ] Commit: `"mbr: partition table writer"`

### 5.2 EBR Writer for Logical Partitions

**Prompt:** When creating logical partitions inside an extended container, write EBR sectors to define the linked list. Each EBR is placed immediately before its logical volume (at the alignment boundary). Entry 1 describes the logical volume (LBA relative to current EBR). Entry 2 points to the next EBR (LBA relative to first EBR). The last EBR in the chain has entry 2 all-zeros. Entries 3 and 4 must be zeroed. Always write `0xAA55` signature. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"mbr: EBR writer for logical partitions"`. Add notes directly in this TODO section.

- [ ] Implement `ebr_write(blkdev, ebr_lba, local_vol, next_ebr)`:
  - [ ] Allocate 512-byte sector, zero it
  - [ ] Entry 1 at offset 446: encode local volume (type, relative LBA, total sectors)
  - [ ] Entry 2 at offset 462: encode next EBR pointer (relative to first EBR) or zero
  - [ ] Entries 3, 4 at offsets 478, 494: all zeros
  - [ ] Write `0x55`/`0xAA` at offsets 510/511
  - [ ] Write sector to `ebr_lba` via `blkdev_write()`
- [ ] Implement `ebr_chain_write(blkdev, first_ebr_lba, logical_partitions[])`:
  - [ ] Iterate logical partitions: write EBR for each
  - [ ] Link each EBR to the next via Entry 2
  - [ ] Last EBR: Entry 2 all zeros (terminates chain)
- [ ] Place EBR at 1-MiB aligned boundary before each logical volume
- [ ] Log: `[MBR] EBR written at LBA %u → logical type=0x%02X at LBA %u`
- [ ] Commit: `"mbr: EBR writer for logical partitions"`

---

## 6. Partition Creation & Deletion

### 6.1 Create Primary Partition

**Prompt:** Implement creating a new primary partition: find an empty slot in the 4-entry table, compute aligned start LBA (§4.1), set type and size, write MBR. Enforce the 4-primary-partition limit. If the request would exceed 2 TiB (2^32 sectors), reject with an error. Validate that the new partition doesn't overlap any existing partition. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"mbr: create primary partition"`. Add notes directly in this TODO section.

- [ ] Implement `mbr_create_partition(blkdev, size_sectors, type, bootable)`:
  - [ ] Find first empty slot (type == `0x00`) in the 4-entry table
  - [ ] If no empty slot → return `MBR_ERR_TABLE_FULL`
  - [ ] Calculate start LBA: find largest contiguous gap, align to 2048
  - [ ] Validate: `start_lba + size_sectors` does not exceed 2^32 - 1
  - [ ] Validate: no overlap with any existing partition
  - [ ] Set boot indicator if `bootable == true` (ensure only one `0x80`)
  - [ ] Encode partition entry with `chs_encode()` and LBA fields
  - [ ] Call `mbr_write()` to flush updated table
- [ ] Implement `mbr_find_free_space(blkdev, partitions[4], *start, *max_sectors)`:
  - [ ] Sort partitions by LBA, find gaps between consecutive partitions
  - [ ] Account for MBR at LBA 0 + alignment gap at start (LBA 2048 minimum)
  - [ ] Return largest contiguous gap
- [ ] Overlap validation: for each existing partition, verify no range intersection
- [ ] Log: `[MBR] Created partition %d: type=0x%02X, LBA=%u, sectors=%u`
- [ ] Commit: `"mbr: create primary partition"`

### 6.2 Create Extended & Logical Partitions

**Prompt:** To exceed the 4-partition limit, create an Extended Partition container (type `0x0F` for LBA) consuming a primary slot, then create logical partitions inside it via EBR linked list. The extended container always uses type `0x0F` (LBA Extended) for modern addressing. Only one extended partition is allowed per disk. Logical partitions begin numbering at 5. Each logical partition gets its own EBR in the chain. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"mbr: extended and logical partitions"`. Add notes directly in this TODO section.

- [ ] Implement `mbr_create_extended(blkdev, start_lba, size_sectors)`:
  - [ ] Verify no extended partition already exists (only one allowed)
  - [ ] Create primary entry with type `0x0F` (Extended LBA)
  - [ ] Write initial empty EBR at start of extended region
- [ ] Implement `mbr_create_logical(blkdev, ext_entry, size_sectors, type)`:
  - [ ] Walk existing EBR chain to find the tail (Entry 2 == zeros)
  - [ ] Calculate new logical volume start: aligned, after previous logical + EBR gap
  - [ ] Write new EBR for the new logical volume
  - [ ] Update previous EBR's Entry 2 to point to the new EBR
  - [ ] Register as logical partition (number ≥ 5)
- [ ] Enforce: logical partition must fit within extended container bounds
- [ ] Log: `[MBR] Created logical partition %d inside extended container`
- [ ] Commit: `"mbr: extended and logical partitions"`

### 6.3 Delete Partition

**Prompt:** Delete a partition by zeroing its 16-byte entry in the MBR (for primary) or unlinking its EBR from the chain (for logical). Deleting an extended partition removes ALL logical partitions within it. Prompt with a safety confirmation (when called from CLI). Flush the updated MBR/EBR to disk. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"mbr: delete partition"`. Add notes directly in this TODO section.

- [ ] Implement `mbr_delete_partition(blkdev, partition_number)`:
  - [ ] If primary (1–4): zero the 16-byte entry, write MBR
  - [ ] If logical (≥5): unlink EBR from chain:
    - [ ] Walk EBR chain to find the target
    - [ ] Update previous EBR's Entry 2 to point to the next EBR (skip deleted)
    - [ ] If deleting first logical: update extended container's first EBR
    - [ ] If deleting last logical: set previous Entry 2 to zeros
  - [ ] If deleting the extended partition itself: warn that ALL logicals will be destroyed
- [ ] Unmount any filesystem on the deleted partition first
- [ ] Zero the deleted partition's first sector (optional, for cleanliness)
- [ ] Log: `[MBR] Deleted partition %d`
- [ ] Commit: `"mbr: delete partition"`

---

## 7. GPT Protective MBR

### 7.1 Protective MBR Detection & Generation

**Prompt:** When the first partition entry has type `0xEE`, the disk uses GPT. The parser must detect this immediately and hand off to the GPT parser (`gpt.c`). For GPT disk creation, write a Protective MBR: a single partition entry spanning the entire disk (capped at 2^32 - 1 sectors for the LBA field), type `0xEE`, boot indicator `0x00`, CHS `00 02 00` for start and `FF FF FF` for end. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"mbr: GPT Protective MBR"`. Add notes directly in this TODO section.

- [ ] Implement `mbr_is_protective(partitions[4])`:
  - [ ] Return `true` if entry 0 has type `0xEE`
  - [ ] Redirect caller to `gpt_parse()` from `gpt.c`
- [ ] Implement `mbr_write_protective(blkdev, disk_size_sectors)`:
  - [ ] Entry 1: type=`0xEE`, start_lba=1, total_sectors=`min(disk_size - 1, 0xFFFFFFFF)`
  - [ ] CHS start: `00 02 00` (Head=0, Sector=2, Cylinder=0)
  - [ ] CHS end: `FF FF FF` (overflow for all GPT disks)
  - [ ] Entries 2–4: all zeros
  - [ ] Boot indicator: `0x00` (UEFI ignores this)
  - [ ] Write to LBA 0
- [ ] Existing Protective MBR detection: already in `partition.c` → verify integration
- [ ] Log: `[MBR] GPT Protective MBR detected — redirecting to GPT parser`
- [ ] Commit: `"mbr: GPT Protective MBR"`

---

## 8. Disk Signature Management

### 8.1 Persistent Volume Identification

**Prompt:** The 32-bit Disk Signature at MBR offsets 440–443 uniquely identifies each physical disk, allowing the OS to maintain stable volume-to-drive-letter mappings across reboots even if disk ordering changes. Generate a new random signature when formatting a fresh disk (`mbr_create_fresh()`). Store signatures in the Registry (`HKLM\SYSTEM\Storage\Disks\{signature}\`) with volume mount points. Check for signature collisions on multi-disk systems. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"mbr: disk signature management"`. Add notes directly in this TODO section.

- [ ] On MBR parse: extract disk signature, store in `blkdev->disk_id`
- [ ] On MBR create: generate random 32-bit signature (use RDRAND or PIT-seeded PRNG)
- [ ] Collision detection: check all registered blkdevs for duplicate signatures
  - [ ] If collision: log warning, do not auto-fix (user intervention required)
- [ ] Store in Registry: `HKLM\SYSTEM\Storage\Disks\{0x%08X}\DriveLetter`
- [ ] On boot: match disk signature → previously assigned drive letter
- [ ] Preserve signature on partition table updates (read-modify-write, never overwrite)
- [ ] Log: `[MBR] Disk Signature 0x%08X → drive letter mapping from Registry`
- [ ] Commit: `"mbr: disk signature management"`

---

## 9. CLI Integration

### 9.1 Diskpart MBR Commands

**Prompt:** Wire MBR operations into the `diskpart` shell command for interactive disk management. `diskpart list disks` shows each disk with its MBR/GPT status, signature, and partition count. `diskpart list parts <disk>` shows all partitions (primary + logical) with type, size, LBA, and boot flag. `diskpart create <disk> <size_mb> <type>` creates a primary partition. `diskpart delete <disk> <part>` removes a partition. `diskpart active <disk> <part>` sets the bootable flag. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"shell: diskpart MBR commands"`. Add notes directly in this TODO section.

- [ ] `diskpart list disks` — show all disks:
  - [ ] Table: Disk#, Size, Table Type (MBR/GPT), Signature, Partitions
  - [ ] Read MBR sector to determine table type and signature
- [ ] `diskpart list parts <disk>` — show partitions:
  - [ ] Primary partitions 1–4 from MBR
  - [ ] Logical partitions 5+ from EBR chain
  - [ ] Table: Part#, Type, Label, Size, LBA Start, Boot Flag
- [ ] `diskpart create <disk> <size_mb> <type>` — create primary partition:
  - [ ] Convert size_mb to sectors: `size_mb * 2048`
  - [ ] Call `mbr_create_partition()`
  - [ ] Confirm before writing
- [ ] `diskpart delete <disk> <part>` — delete partition:
  - [ ] Confirmation prompt: "This will destroy all data. Continue? (y/N)"
  - [ ] Call `mbr_delete_partition()`
- [ ] `diskpart active <disk> <part>` — set bootable flag:
  - [ ] Clear `0x80` from all entries, set on target
  - [ ] Write updated MBR
- [ ] `diskpart info <disk>` — detailed disk info:
  - [ ] Disk signature, total size, partition table type, alignment
  - [ ] Each partition: type name, LBA range, CHS (if valid), boot status
- [ ] Commit: `"shell: diskpart MBR commands"`

---

## 10. Testing & Validation

### 10.1 MBR Test Suite

**Prompt:** Create test disk images to validate MBR parsing and writing. Use the host build system to create test images: an MBR disk with 4 primary partitions, an MBR disk with 1 primary + 1 extended (3 logicals), a GPT disk with Protective MBR, a disk with legacy 63-sector alignment, and a corrupted disk (bad signature). Attach via QEMU and verify the parser handles each correctly. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"test: MBR partition test suite"`. Add notes directly in this TODO section.

- [ ] Test image: 4 primary partitions (FAT32, NTFS, Linux, IXFS)
  - [ ] Verify all 4 detected with correct types, LBAs, and sizes
- [ ] Test image: 1 primary + extended with 3 logical partitions
  - [ ] Verify EBR traversal finds partitions 5, 6, 7
  - [ ] Verify correct relative LBA computation for each logical
- [ ] Test image: GPT disk with Protective MBR (type `0xEE`)
  - [ ] Verify parser detects `0xEE` and redirects to GPT
- [ ] Test image: legacy 63-sector alignment
  - [ ] Verify parser reads partitions, logs alignment warning
- [ ] Test image: missing `0xAA55` signature
  - [ ] Verify parser rejects with `MBR_ERR_NO_SIGNATURE`
- [ ] Test: create partition via `diskpart`, reboot, verify persistence
- [ ] Test: create extended + logical, reboot, verify EBR chain persists
- [ ] Test: delete logical partition, verify chain remains valid
- [ ] QEMU flags: `-drive file=mbr_test.img,format=raw,if=none,id=t0 -device virtio-blk-pci,drive=t0`
- [ ] Commit: `"test: MBR partition test suite"`

---

## Priority Order

| Priority | Section                          | Description                                                   |
|----------|----------------------------------|---------------------------------------------------------------|
| 🔴 P0    | 1.1 Full MBR Layout Parser     | Foundation — decode entire 512-byte sector correctly          |
| 🔴 P0    | 1.2 Partition Type Recognition  | Foundation — identify all filesystem types and GPT redirect   |
| 🟠 P1    | 2.1 EBR Chain Walker            | Correctness — read logical partitions beyond the 4-entry limit|
| 🟠 P1    | 3.1 CHS Extraction & Encoding  | Compatibility — legacy BIOS and diagnostic tool interop       |
| 🟠 P1    | 4.1 1-MiB Alignment            | Performance — optimal 4K/SSD sector alignment                 |
| 🟠 P1    | 7.1 Protective MBR             | Integration — GPT detection and Protective MBR generation     |
| 🟡 P2    | 5.1 Partition Table Writer      | Feature — create and modify MBR partition tables              |
| 🟡 P2    | 5.2 EBR Writer                  | Feature — write logical partition chains                      |
| 🟡 P2    | 6.1 Create Primary Partition   | Feature — partition creation with alignment and validation    |
| 🟡 P2    | 6.2 Extended & Logical         | Feature — exceed 4-partition limit                            |
| 🟡 P2    | 6.3 Delete Partition           | Feature — partition removal and chain repair                  |
| 🟡 P2    | 8.1 Disk Signature Management  | Feature — persistent volume identification                    |
| 🟢 P3    | 9.1 Diskpart MBR Commands     | Tooling — CLI partition management                            |
| 🟢 P3    | 10.1 MBR Test Suite            | Quality — automated test coverage for all scenarios           |

---

## OS Comparison

| Feature                          | 🪟 Windows 11 (diskpart)           | 🐧 Linux (fdisk / parted)         | 🚀 Impossible OS                              |
| -------------------------------- | ---------------------------------- | --------------------------------- | ---------------------------------------------- |
| MBR sector parsing               | ✅ Full (disk.sys)                  | ✅ Full (partitions/msdos.c)       | ⬜ §1.1 P0 (basic exists, needs enhancement)   |
| Boot signature `0xAA55` check    | ✅                                  | ✅                                 | ⬜ §1.1 P0                                     |
| 32-bit Disk Signature            | ✅ Critical (volume tracking)       | ✅ Supported                       | ⬜ §1.1 P0 + §8.1 P2                           |
| All partition type IDs           | ✅ Exhaustive                       | ✅ Exhaustive                      | ⬜ §1.2 P0 (partial: 3 types recognized)       |
| GPT Protective MBR (`0xEE`)     | ✅ Redirect to GPT                  | ✅ Redirect to GPT                 | ⬜ §7.1 P1                                     |
| Extended/Logical (EBR)           | ✅ Full chain traversal             | ✅ Full chain traversal            | ⬜ §2.1 P1                                     |
| CHS bit-packing                  | ✅ Full encode/decode               | ✅ Full encode/decode              | ⬜ §3.1 P1                                     |
| CHS overflow dummy (`FE FF FF`) | ✅                                  | ✅                                 | ⬜ §3.1 P1                                     |
| 1-MiB alignment                  | ✅ Default since Vista              | ✅ Default since util-linux 2.17   | ⬜ §4.1 P1                                     |
| Legacy 63-sector awareness       | ✅ Compatible                       | ✅ Compatible                      | ⬜ §4.1 P1 (warn but read)                     |
| MBR write / create partition     | ✅ diskpart / Disk Management       | ✅ fdisk / parted                  | ⬜ §5.1–6.2 P2                                 |
| Delete partition                 | ✅ diskpart                         | ✅ fdisk -d                        | ⬜ §6.3 P2                                     |
| Set active/bootable              | ✅ diskpart active                  | ✅ fdisk -a                        | ⬜ §9.1 P3                                     |
| Drive letter mapping via sig     | ✅ Registry-based                   | ✅ /dev/disk/by-id                 | ⬜ §8.1 P2                                     |
| CLI partition tool               | ✅ diskpart (interactive)           | ✅ fdisk (interactive + scripted)  | ⬜ §9.1 P3                                     |
| Test images / validation         | ✅ Windows PE test suite            | ✅ blktests                        | ⬜ §10.1 P3                                    |
| **2 TiB limit enforcement**     | ✅ Warns in Disk Management         | ✅ parted warns                    | ⬜ §6.1 P2                                     |
| **Full MBR subsystem**           | ✅                                  | ✅                                 | ⬜ Requires §1–§7 at minimum                   |
