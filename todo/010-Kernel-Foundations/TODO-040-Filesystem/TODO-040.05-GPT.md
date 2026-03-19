# 040.05-GPT — GUID Partition Table

> **Goal:** Enhance the existing GPT parser to a production-grade, fault-tolerant
> subsystem with backup header recovery, 4Kn sector support, full attribute
> decoding, comprehensive type GUID registry, GPT write support for partition
> creation/deletion, and Hybrid MBR detection. The current `gpt.c` provides
> validated primary-side read-only parsing — this TODO extends it to a complete
> GPT subsystem with write operations, structural redundancy, and CLI tooling.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for all sector buffers and partition entry arrays (16 KB+). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). See `rules.md` Known Gotchas.

> [!WARNING]
> **Mixed-Endian GUIDs:** GPT uses a mixed-endian GUID format where `TimeLow` (4B), `TimeMid` (2B), `TimeHiAndVersion` (2B) are little-endian, but the trailing 8-byte `ClockSeq+Node` array is stored as raw bytes (big-endian order). The existing `read_guid()` handles this correctly — any new GUID I/O must use the same mixed-endian logic.

> [!IMPORTANT]
> **Spec Reference:** All offsets, field layouts, CRC32 algorithms, and validation rules reference the
> [GPT Specification](file:///home/derickpayne/impossible-os/specs/gpt.md)
> in the repo at `specs/gpt.md`.

---

## 1. GPT Header Parsing (Primary)

<details>
<summary>✅ 1. GPT Header Parsing (Primary) — completed</summary>


### 1.1 Primary Header Validation ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `gpt.c` reads LBA 1, checks `"EFI PART"` signature (`0x5452415020494645`), validates Header CRC32 by zeroing bytes 16–19 before computation, parses all 13 header fields, and validates partition entry array CRC32 over the full `NumberOfPartitionEntries × SizeOfPartitionEntry` range. Run `bash scripts/build.sh clean`. Fix any inconsistencies.

- [x] Read LBA 1 from block device
- [x] Verify 8-byte signature `"EFI PART"` (`GPT_SIGNATURE`)
- [x] Parse all header fields: revision, header_size, my_lba, alt_lba, first/last usable LBA, disk GUID, partition entry LBA, num entries, entry size, array CRC32
- [x] Validate Header CRC32: clone header, zero bytes 16–19, compute CCITT-32, compare
- [x] Use reflected polynomial `0xEDB88320` with init `0xFFFFFFFF` and final XOR `0xFFFFFFFF`
- [x] Validate header size range: ≥92 and ≤512 bytes
- [x] Validate partition entry size ≥128 and num_entries > 0
- [x] Commit: `"fs: GPT partition table parsing"`

### 1.2 Partition Entry Array Parsing ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `gpt.c` reads the partition entry array starting at `part_entry_lba`, computes CRC32 incrementally sector-by-sector over the full array length, parses non-empty entries (type GUID ≠ all-zeros), extracts type GUID, unique GUID, start/end LBA, attributes, and UTF-16LE name. Confirm it correctly skips empty entries without stopping iteration.

- [x] Read partition entries sector-by-sector from `part_entry_lba`
- [x] Compute array CRC32 incrementally over full `num_entries × entry_size` bytes
- [x] Compare computed array CRC32 against header's `part_entry_crc32`
- [x] For each entry: parse type GUID, unique GUID, start LBA, end LBA, attributes, name
- [x] Skip empty entries (`type_guid == GPT_GUID_EMPTY`) without terminating scan
- [x] Decode UTF-16LE name to ASCII (best-effort, null-terminate)
- [x] Cap results at `GPT_MAX_RESULTS` (currently 32)
- [x] Commit: `"fs: GPT partition table parsing"`

### 1.3 PMBR Detection ✅

**Prompt:** This section is marked complete. Verify Protective MBR detection: confirm `gpt_parse()` checks all 4 MBR partition entries at LBA 0 for type `0xEE`, and returns invalid if none found.

- [x] Read MBR sector (LBA 0) passed as `sector0` parameter
- [x] Scan all 4 partition entries for type `0xEE` (`MBR_TYPE_GPT`)
- [x] If no `0xEE` found → return invalid table
- [x] Commit: `"fs: GPT partition table parsing"`


</details>

---
## 2. Backup Header & Structural Redundancy

### 2.1 Backup Header Fallback

**Prompt:** The current parser reads only the primary header at LBA 1 and returns failure if CRC32 validation fails. Implement automatic fallback to the backup GPT header at the last LBA of the disk. When the primary header fails CRC32 validation, calculate the backup location (`blkdev->sector_count - 1`), read that sector, validate its signature and CRC32, and use it to parse the partition entry array from the backup array location (end of disk, before the backup header). Log a critical warning when falling back. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"gpt: backup header fallback recovery"`. Add notes directly in this TODO section.

- [ ] Calculate backup header LBA: `dev->sector_count - 1`
- [ ] On primary header CRC32 failure: attempt reading backup header
- [ ] Validate backup header: signature `"EFI PART"`, CRC32 (same zeroing procedure)
- [ ] Verify backup header's `my_lba` matches the last LBA
- [ ] Verify backup header's `alt_lba == 1` (points back to primary)
- [ ] Parse partition entries from backup array (located before backup header)
- [ ] Log: `[GPT] WARNING: Primary header corrupt — using backup at LBA %llu`
- [ ] Commit: `"gpt: backup header fallback recovery"`

### 2.2 Primary Header Auto-Recovery

**Prompt:** When the backup header is valid but the primary is corrupt, automatically reconstruct the primary header at LBA 1 from the backup. This requires: copying the backup header, swapping `my_lba` and `alt_lba`, recalculating the Header CRC32, and writing the reconstructed header to LBA 1. Also copy the backup partition entry array to the primary location (LBA 2+). This restores full redundancy after corruption. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"gpt: auto-recover primary from backup"`. Add notes directly in this TODO section.

- [ ] Copy backup header into reconstruction buffer
- [ ] Swap `my_lba` ↔ `alt_lba` (backup's values are inverted)
- [ ] Recalculate Header CRC32 (zero CRC field, compute, fill)
- [ ] Write reconstructed primary header to LBA 1
- [ ] Copy backup partition entry array to primary location (LBA 2+)
- [ ] Recompute and verify primary array CRC32 after copy
- [ ] Log: `[GPT] Auto-recovered primary header from backup`
- [ ] Commit: `"gpt: auto-recover primary from backup"`

### 2.3 Backup Header Sync on Write

**Prompt:** Whenever the primary GPT header or partition entry array is modified (partition create/delete/resize), the backup copies at the end of the disk must be synchronously updated. Write the backup partition entry array first (before the backup header LBA), then recalculate and write the backup header with swapped `my_lba`/`alt_lba` and freshly computed CRC32 values. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"gpt: sync backup on write"`. Add notes directly in this TODO section.

- [ ] After any primary GPT modification: mirror partition entry array to backup location
- [ ] Recalculate backup header: swap `my_lba`/`alt_lba`, recompute Header CRC32
- [ ] Write backup partition array → then backup header (order matters for crash safety)
- [ ] Implement `gpt_sync_backup(dev, primary_header, entry_array)` helper
- [ ] Log: `[GPT] Synced backup GPT at LBA %llu`
- [ ] Commit: `"gpt: sync backup on write"`

---

## 3. 4Kn Sector Size Support

### 3.1 Dynamic Sector Size Calculation

**Prompt:** The current parser hardcodes 512-byte sector I/O (e.g., `hdr_sect[512]`, `entry_buf[512]`, `entries_per_sector = 512 / entry_size`). GPT offsets are defined relative to the logical block size — on a 4Kn drive (4096-byte sectors), LBA 1 is at byte offset 4096, and the 16 KB entry array spans only 4 LBAs instead of 32. Refactor the parser to use `dev->sector_size` for all buffer allocations and LBA calculations. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"gpt: dynamic sector size support"`. Add notes directly in this TODO section.

- [ ] Read `dev->sector_size` instead of assuming 512
- [ ] Allocate sector buffers dynamically: `pmm_alloc_contiguous()` for buffers > 512 bytes
- [ ] Recalculate `entries_per_sector = sector_size / part_entry_size`
- [ ] Recalculate `sectors_needed = (total_entry_bytes + sector_size - 1) / sector_size`
- [ ] Handle 4Kn: entry array spans LBA 2–5 (4 sectors × 4096 = 16,384 bytes)
- [ ] Handle 512b: entry array spans LBA 2–33 (32 sectors × 512 = 16,384 bytes)
- [ ] Validate `first_usable_lba` matches calculated boundary for the sector size
- [ ] Test: QEMU with `logical_block_size=4096` flag
- [ ] Commit: `"gpt: dynamic sector size support"`

---

## 4. Mixed-Endian GUID Operations

### 4.1 GUID Encoding (Write Path) ✅ (Read Path)

**Prompt:** The read path (`read_guid()`) correctly handles mixed-endian parsing. Implement the write path: `write_guid(guid, buffer)` that serializes a `gpt_guid` struct back to the 16-byte on-disk mixed-endian format. `TimeLow` → 4 bytes LE, `TimeMid` → 2 bytes LE, `TimeHiAndVersion` → 2 bytes LE, trailing 8 bytes → direct copy. Also implement `guid_to_string(guid, buf)` for human-readable output (`xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx`) and `guid_from_string(str, guid)` for parsing user input. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"gpt: GUID encode + string conversion"`. Add notes directly in this TODO section.

- [x] `read_guid(bytes, guid)` — mixed-endian parse (existing, working)
- [x] `gpt_guid_equal(a, b)` — field-by-field comparison (existing, working)
- [ ] Implement `write_guid(guid, bytes[16])`:
  - [ ] Write `data1` as 4 bytes LE
  - [ ] Write `data2` as 2 bytes LE
  - [ ] Write `data3` as 2 bytes LE
  - [ ] Copy `data4[8]` directly
- [ ] Implement `guid_to_string(guid, buf[37])` → `"xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"`
- [ ] Implement `guid_from_string(str, guid)` → parse canonical text format
- [ ] Implement `guid_generate()` → random v4 GUID (RDRAND or PIT-seeded PRNG)
- [ ] Commit: `"gpt: GUID encode + string conversion"`

---

## 5. Type GUID Registry Expansion

### 5.1 Comprehensive Type GUID Registry

**Prompt:** The current `gpt.c` recognizes only 4 partition types: EFI System, MS Basic Data, Linux Filesystem, and IXFS. Expand the registry to cover all GUIDs from the spec (§9): BIOS Boot, MS Reserved, MS LDM, MS Recovery, Linux Swap, Linux Root (x86-64), Apple HFS+, Apple APFS, FreeBSD ZFS, ChromeOS Kernel. Add `GPT_GUID_*` constants and update `gpt_type_name()` to return meaningful strings for all types. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"gpt: expanded type GUID registry"`. Add notes directly in this TODO section.

- [x] `GPT_GUID_EMPTY` — `00000000-0000-0000-0000-000000000000`
- [x] `GPT_GUID_EFI_SYSTEM` — `C12A7328-F81F-11D2-BA4B-00A0C93EC93B`
- [x] `GPT_GUID_MS_BASIC_DATA` — `EBD0A0A2-B9E5-4433-87C0-68B6B72699C7`
- [x] `GPT_GUID_LINUX_FS` — `0FC63DAF-8483-4772-8E79-3D69D8477DE4`
- [x] `GPT_GUID_IXFS` — `DA000000-0000-4978-4653-000000000001`
- [ ] Add `GPT_GUID_BIOS_BOOT` — `21686148-6449-6E6F-744E-656564454649`
- [ ] Add `GPT_GUID_MS_RESERVED` — `E3C9E316-0B5C-4DB8-817D-F92DF00215AE`
- [ ] Add `GPT_GUID_MS_LDM_META` — `5808C8AA-7E8F-42E0-85D2-E1E90434CFB3`
- [ ] Add `GPT_GUID_MS_LDM_DATA` — `AF9B60A0-1431-4F62-BC68-3311714A69AD`
- [ ] Add `GPT_GUID_MS_RECOVERY` — `DE94BBA4-06D1-4D40-A16A-BFD50179D6AC`
- [ ] Add `GPT_GUID_MS_STORAGE_SPACES` — `E75CAF8F-F680-4CEE-AFA3-B001E56EFC2D`
- [ ] Add `GPT_GUID_LINUX_SWAP` — `0657FD6D-A4AB-43C4-84E5-0933C84B4F4F`
- [ ] Add `GPT_GUID_LINUX_ROOT_X64` — `4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709`
- [ ] Add `GPT_GUID_LINUX_HOME` — `933AC7E1-2EB4-4F13-B844-0E14E2AEF915`
- [ ] Add `GPT_GUID_LINUX_SRV` — `3B8F8425-20E0-4F3B-907F-1A25A76F98E8`
- [ ] Add `GPT_GUID_LINUX_LVM` — `E6D6D379-F507-44C2-A23C-238F2A3DF928`
- [ ] Add `GPT_GUID_LINUX_RAID` — `A19D880F-05FC-4D3B-A006-743F0F84911E`
- [ ] Add `GPT_GUID_APPLE_HFS` — `48465300-0000-11AA-AA11-00306543ECAC`
- [ ] Add `GPT_GUID_APPLE_APFS` — `7C3457EF-0000-11AA-AA11-00306543ECAC`
- [ ] Add `GPT_GUID_FREEBSD_ZFS` — `516E7CB5-6ECF-11D6-8FF8-00022D09712B`
- [ ] Add `GPT_GUID_SOLARIS_ROOT` — `6A85CF4D-1DD2-11B2-99A6-080020736631`
- [ ] Add `GPT_GUID_VMWARE_VMFS` — `AA31E02A-400F-11DB-9590-000C2911D1B8`
- [ ] Add `GPT_GUID_CHROMEOS_KERNEL` — `FE3A2A5D-4F32-41A7-B725-ACCC3285A309`
- [ ] Add `GPT_GUID_CEPH_OSD` — `4FBD7E29-9D25-41B8-AFD0-062C0CEFF05D`
- [ ] For unknown types: display as `"Unknown ({GUID string})"` — never crash
- [ ] Update `gpt_type_name()` to return human-readable strings for all types
- [ ] Commit: `"gpt: expanded type GUID registry"`

---

## 6. Partition Attributes Decoding

### 6.1 UEFI Global Attributes

**Prompt:** Parse the 64-bit Attributes bitmask from each partition entry. Bits 0–2 are UEFI-defined: bit 0 = Required Partition (OS must not delete), bit 1 = No Block IO Protocol (hidden from UEFI), bit 2 = Legacy BIOS Bootable (GPT "Active" flag). When a partition has bit 0 set, prevent deletion in `diskpart` and partition manager. When bit 2 is set, log it as "BIOS Bootable". After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"gpt: partition attribute decoding"`. Add notes directly in this TODO section.

- [ ] Define `GPT_ATTR_REQUIRED (1 << 0)` — system partition, prevent deletion
- [ ] Define `GPT_ATTR_NO_BLOCKIO (1 << 1)` — hide from firmware
- [ ] Define `GPT_ATTR_LEGACY_BIOS_BOOT (1 << 2)` — Legacy BIOS bootable
- [ ] Parse attributes from entry offset `0x30` (existing field, already read)
- [ ] Log attribute flags: `[GPT] Partition %d: Required=%d, BIOSBoot=%d`
- [ ] Expose `gpt_entry.attributes` to partition scanner and VFS
- [ ] Commit: `"gpt: partition attribute decoding"`

### 6.2 Microsoft Type-Specific Attributes

**Prompt:** For partitions with type GUID `EBD0A0A2-B9E5-4433-87C0-68B6B72699C7` (MS Basic Data), decode bits 60–63: bit 60 = Read-Only, bit 61 = Shadow Copy, bit 62 = Hidden, bit 63 = No Automount. When read-only is set, mount the filesystem as write-protected. When hidden or no-automount is set, skip auto-mount drive letter assignment. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"gpt: Microsoft-specific attributes"`. Add notes directly in this TODO section.

- [ ] Define `GPT_MS_ATTR_READONLY (1ULL << 60)`
- [ ] Define `GPT_MS_ATTR_SHADOW (1ULL << 61)`
- [ ] Define `GPT_MS_ATTR_HIDDEN (1ULL << 62)`
- [ ] Define `GPT_MS_ATTR_NO_AUTOMOUNT (1ULL << 63)`
- [ ] Only apply MS attribute logic when type GUID == `GPT_GUID_MS_BASIC_DATA`
- [ ] Read-only flag → set `blkdev->readonly = true`, filesystem mounts write-protected
- [ ] Hidden / No-Automount → skip auto-mount in partition scanner
- [ ] Log: `[GPT] MS Basic Data partition: readonly=%d, hidden=%d, no_automount=%d`
- [ ] Commit: `"gpt: Microsoft-specific attributes"`

---

## 7. Hybrid MBR Detection

### 7.1 Hybrid MBR Warning

**Prompt:** Detect the Hybrid MBR anomaly: when LBA 0 contains a `0xEE` partition entry whose size does NOT span the entire disk AND other non-zero partition entries exist in slots 2–4. This indicates a legacy dual-boot layout (typically Apple BootCamp). Log a critical warning and always prefer the GPT structures over the MBR mappings. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"gpt: Hybrid MBR detection"`. Add notes directly in this TODO section.

- [ ] After detecting `0xEE` at LBA 0: check if its `Size in LBA` covers the full disk
  - [ ] Full disk: `size_lba >= disk_sectors - 1` → standard PMBR ✅
  - [ ] Partial: `size_lba < disk_sectors - 1` AND other entries non-zero → Hybrid MBR ⚠️
- [ ] Check MBR entries 2–4 for non-zero type codes
- [ ] If Hybrid MBR: log `[GPT] WARNING: Hybrid MBR detected — ignoring legacy MBR entries`
- [ ] Always prefer GPT structures over Hybrid MBR entries
- [ ] Set `blkdev->hybrid_mbr = true` flag for informational purposes
- [ ] Commit: `"gpt: Hybrid MBR detection"`

---

## 8. GPT Write Support

### 8.1 Protective MBR Writer

**Prompt:** Implement writing a standard Protective MBR to LBA 0 for GPT disk initialization. Entry 1: boot=`0x00`, CHS start=`0x00 0x02 0x00`, type=`0xEE`, CHS end=`0xFF 0xFF 0xFF`, LBA start=1, size=`min(disk_sectors - 1, 0xFFFFFFFF)`. Entries 2–4 all zeros. Signature `0x55 0xAA` at bytes 510–511. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"gpt: Protective MBR writer"`. Add notes directly in this TODO section.

- [ ] Implement `gpt_write_pmbr(dev)`:
  - [ ] Allocate 512-byte buffer, zero it
  - [ ] Entry 1 at offset 446: boot=`0x00`, CHS start=`{0x00, 0x02, 0x00}`, type=`0xEE`
  - [ ] CHS end=`{0xFF, 0xFF, 0xFF}`, LBA start=`1`
  - [ ] Size = `min(dev->sector_count - 1, 0xFFFFFFFF)`
  - [ ] Entries 2–4: zeroed (offsets 462, 478, 494)
  - [ ] Write `0x55` at byte 510, `0xAA` at byte 511
  - [ ] Write to LBA 0 via `blkdev_write()`
- [ ] Log: `[GPT] Written Protective MBR`
- [ ] Commit: `"gpt: Protective MBR writer"`

### 8.2 GPT Header Writer

**Prompt:** Implement writing a GPT header to a specified LBA (1 for primary, last LBA for backup). Build the 92-byte structure: signature, revision `0x00010000`, header size 92, reserved=0, my_lba, alt_lba, first/last usable LBA, disk GUID, partition entry LBA, entry count (128), entry size (128), array CRC32. Compute Header CRC32 last (zero field, hash, fill). Pad remainder of sector with zeros. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"gpt: header writer"`. Add notes directly in this TODO section.

- [ ] Implement `gpt_write_header(dev, header, lba)`:
  - [ ] Allocate sector buffer, zero it
  - [ ] Write signature `"EFI PART"` at offset 0
  - [ ] Write revision `0x00010000` at offset 8
  - [ ] Write header size `92` at offset 12
  - [ ] Zero CRC32 field at offset 16 (compute last)
  - [ ] Write reserved `0` at offset 20
  - [ ] Write `my_lba`, `alt_lba`, `first_usable_lba`, `last_usable_lba`
  - [ ] Write disk GUID via `write_guid()` at offset 56
  - [ ] Write `part_entry_lba`, `num_entries`, `entry_size`, `array_crc32`
  - [ ] Compute Header CRC32 over 92 bytes (with CRC field zeroed), fill at offset 16
  - [ ] Write sector to `lba` via `blkdev_write()`
- [ ] Commit: `"gpt: header writer"`

### 8.3 Partition Entry Writer

**Prompt:** Implement writing partition entries to the entry array. Serialize each `gpt_entry` to its 128-byte on-disk format: type GUID (mixed-endian via `write_guid()`), unique GUID, start/end LBA (LE64), attributes (LE64), name (UTF-16LE). Compute the array CRC32 over the full `128 × 128 = 16,384` byte range (including empty entries). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"gpt: partition entry writer"`. Add notes directly in this TODO section.

- [ ] Implement `gpt_write_entries(dev, entries[], count, entry_lba)`:
  - [ ] Allocate 16,384-byte buffer for 128 entries via `pmm_alloc_contiguous()`
  - [ ] Zero entire buffer (empty entries = all-zero GUIDs)
  - [ ] For each active entry: serialize type GUID, unique GUID, LBA range, attributes, name
  - [ ] Encode name as UTF-16LE (ASCII → UTF-16LE: zero-extend each byte)
  - [ ] Compute array CRC32 over full 16,384 bytes
  - [ ] Write entry array to disk starting at `entry_lba` (32 sectors for 512b)
  - [ ] Return computed array CRC32 (needed by header writer)
- [ ] Always write 128 entries regardless of active count (Windows compatibility — spec §5.1)
- [ ] Commit: `"gpt: partition entry writer"`

---

## 9. Partition Management

### 9.1 Initialize GPT Disk

**Prompt:** Create a fresh GPT layout on a raw disk. Write Protective MBR (§8.1), generate a random Disk GUID, compute first/last usable LBA based on sector size, write an empty partition entry array (all-zeros, 128 entries), write primary header at LBA 1 and backup header at last LBA. This is the equivalent of `gdisk` creating a new partition table. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"gpt: initialize fresh GPT disk"`. Add notes directly in this TODO section.

- [ ] Implement `gpt_init_disk(dev)`:
  - [ ] Generate random Disk GUID via `guid_generate()`
  - [ ] Calculate `first_usable_lba`: depends on sector size
    - [ ] 512b sectors: LBA 34 (1 PMBR + 1 header + 32 array sectors)
    - [ ] 4096b sectors: LBA 6 (1 PMBR + 1 header + 4 array sectors)
  - [ ] Calculate `last_usable_lba`: `disk_sectors - 34` (512b) or `disk_sectors - 6` (4Kn)
  - [ ] Write Protective MBR to LBA 0
  - [ ] Write empty entry array to primary (LBA 2+) and backup locations
  - [ ] Compute array CRC32 over zeroed 16,384 bytes
  - [ ] Write primary header at LBA 1
  - [ ] Write backup header at last LBA
- [ ] Log: `[GPT] Initialized GPT: disk_guid=%s, usable=%llu–%llu`
- [ ] Commit: `"gpt: initialize fresh GPT disk"`

### 9.2 Create Partition

**Prompt:** Add a partition to the GPT: find the first empty entry slot (type GUID all-zeros), fill in the type GUID, generate a unique partition GUID, set start/end LBA (with 1-MiB alignment), set attributes and name. Recompute array CRC32, recompute header CRC32, write both primary and backup. Enforce: start/end must be within the usable LBA range, no overlap with existing partitions. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"gpt: create partition"`. Add notes directly in this TODO section.

- [ ] Implement `gpt_create_partition(dev, type_guid, size_sectors, name)`:
  - [ ] Read current partition entry array from disk
  - [ ] Find first empty slot (type GUID == `GPT_GUID_EMPTY`)
  - [ ] If no empty slot → return `GPT_ERR_TABLE_FULL`
  - [ ] Calculate start LBA: find largest contiguous gap in usable range, align to 2048
  - [ ] Validate: no overlap with any existing partition
  - [ ] Validate: `start_lba >= first_usable_lba` and `end_lba <= last_usable_lba`
  - [ ] Generate unique partition GUID via `guid_generate()`
  - [ ] Set attributes to 0 (default)
  - [ ] Encode partition name as UTF-16LE (max 36 chars)
  - [ ] Recompute array CRC32, update both headers, write primary + backup
- [ ] Log: `[GPT] Created partition: type=%s, LBA=%llu–%llu, name=%s`
- [ ] Commit: `"gpt: create partition"`

### 9.3 Delete Partition

**Prompt:** Delete a partition by zeroing its 128-byte entry in the array. Zero only the target slot — do not shift remaining entries. Recompute array CRC32, update both primary and backup headers. Unmount any filesystem on the partition first. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"gpt: delete partition"`. Add notes directly in this TODO section.

- [ ] Implement `gpt_delete_partition(dev, entry_index)`:
  - [ ] Validate entry_index is within `0..num_entries-1`
  - [ ] Validate entry is not empty (already deleted)
  - [ ] Check `GPT_ATTR_REQUIRED` → refuse deletion if set
  - [ ] Unmount filesystem on this partition if mounted
  - [ ] Zero the 128-byte entry slot (type GUID becomes all-zeros)
  - [ ] Recompute array CRC32
  - [ ] Update and write primary header + backup header
  - [ ] Write updated entry array to primary + backup locations
- [ ] Log: `[GPT] Deleted partition entry %d`
- [ ] Commit: `"gpt: delete partition"`

### 9.4 Modify Partition Attributes

**Prompt:** Allow setting/clearing individual attribute bits on a partition: set bootable (bit 2), set read-only (bit 60), set hidden (bit 62), set no-automount (bit 63). Recompute CRC32 values after modification. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"gpt: modify partition attributes"`. Add notes directly in this TODO section.

- [ ] Implement `gpt_set_attribute(dev, entry_index, bit, value)`:
  - [ ] Read entry, set or clear the specified bit
  - [ ] For MS-specific bits (60–63): only allow on `GPT_GUID_MS_BASIC_DATA` type
  - [ ] Recompute array CRC32, update headers, write primary + backup
- [ ] Implement `gpt_set_partition_name(dev, entry_index, name)`:
  - [ ] Encode new name as UTF-16LE, update entry
  - [ ] Recompute CRC32s, write primary + backup
- [ ] Log: `[GPT] Set attribute bit %d=%d on partition %d`
- [ ] Commit: `"gpt: modify partition attributes"`

### 9.5 Partition Resize (Non-Destructive)

**Prompt:** Implement growing and shrinking GPT partitions in-place by modifying the End LBA in the partition entry. Growing: extend End LBA into adjacent free space (verify no collision with next partition). Shrinking: reduce End LBA (the filesystem on top must be shrunk FIRST or data loss occurs). Never move Start LBA — that would destroy data. After entry modification, recompute array CRC32, update both headers. When growing, also relocate the backup GPT if the partition was the last one (backup GPT lives at disk end). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"gpt: non-destructive partition resize"`. Add notes directly in this TODO section.

> [!TIP]
> **Competitive Edge:** Windows Disk Management can only extend the LAST partition or
> one with adjacent right-side free space. It cannot shrink GPT partitions in many scenarios.
> Linux requires CLI tools (`parted resizepart`). Impossible OS doing this from the GUI
> Disk Manager with a drag handle is a significant usability advantage.

> [!WARNING]
> **Shrink is dangerous.** The filesystem MUST be shrunk first (e.g., `ntfs_resize`, `resize2fs`).
> The partition resize only changes the GPT entry — it does NOT touch filesystem structures.

- [ ] Implement `gpt_resize_partition(dev, entry_index, new_end_lba)`:
  - [ ] Validate `new_end_lba >= entry.start_lba` (can't make partition empty)
  - [ ] Validate `new_end_lba <= last_usable_lba`
  - [ ] If growing: verify no overlap with next partition's start LBA
  - [ ] If shrinking: warn — filesystem must be shrunk first
  - [ ] Update `entry.end_lba = new_end_lba`
  - [ ] Recompute array CRC32, update both headers, write primary + backup
- [ ] Implement `gpt_get_max_resize(dev, entry_index)` → returns max possible end LBA
  - [ ] Scan for next partition's start LBA or `last_usable_lba`, whichever is smaller
- [ ] Wire to Disk Manager: drag handle to resize partitions visually
- [ ] Log: `[GPT] Resized partition %d: LBA %llu–%llu → %llu–%llu`
- [ ] Commit: `"gpt: non-destructive partition resize"`

---

## 10. CLI Integration

### 10.1 Diskpart GPT Commands

**Prompt:** Wire GPT operations into the `diskpart` shell command. `diskpart list disks` shows GPT/MBR status and Disk GUID. `diskpart list parts <disk>` shows all GPT entries with type name, GUID, LBA range, size, name, and attributes. `diskpart create <disk> <size_mb> <type>` creates a partition. `diskpart delete <disk> <entry>` removes a partition. `diskpart info <disk>` shows the full GPT header details. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"shell: diskpart GPT commands"`. Add notes directly in this TODO section.

- [ ] `diskpart list disks` — show GPT/MBR status per disk:
  - [ ] For GPT disks: show Disk GUID, revision, usable LBA range
  - [ ] For MBR disks: show Disk Signature (from MBR TODO)
- [ ] `diskpart list parts <disk>` — show GPT partition entries:
  - [ ] Table: Entry#, Type Name, Unique GUID, Start LBA, End LBA, Size, Name, Flags
  - [ ] Show attribute flags: Required, BIOSBoot, ReadOnly, Hidden, NoMount
- [ ] `diskpart create <disk> <size_mb> <type_name>` — create GPT partition:
  - [ ] Map type name to GUID (e.g., "ixfs" → `GPT_GUID_IXFS`, "fat32" → `GPT_GUID_MS_BASIC_DATA`)
  - [ ] Convert size_mb to sectors, call `gpt_create_partition()`
  - [ ] Confirm before writing
- [ ] `diskpart delete <disk> <entry>` — delete GPT partition:
  - [ ] Confirmation prompt: "This will destroy all data. Continue? (y/N)"
  - [ ] Call `gpt_delete_partition()`
- [ ] `diskpart gptinit <disk>` — initialize fresh GPT layout:
  - [ ] Warning: "This will erase all partition data. Continue?"
  - [ ] Call `gpt_init_disk()`
- [ ] `diskpart info <disk>` — full GPT header dump:
  - [ ] Signature, revision, disk GUID, first/last usable LBA, entry count
  - [ ] Primary CRC32, array CRC32, backup status
- [ ] Commit: `"shell: diskpart GPT commands"`

---

## 11. Disk GUID & Volume Tracking

### 11.1 Persistent Volume Identification

**Prompt:** Use the Disk GUID (from GPT header) and Unique Partition GUIDs (from entries) for persistent volume identification across reboots. Store GUID-to-drive-letter mappings in the Registry so that drive letters are stable even if disk ordering changes. The Disk GUID replaces the MBR 32-bit Disk Signature for GPT disks. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"gpt: persistent volume identification"`. Add notes directly in this TODO section.

- [ ] Store Disk GUID in `blkdev->disk_guid` on GPT parse
- [ ] Store Unique Partition GUID in `sub_blkdev->partition_guid` on partition mount
- [ ] Registry: `HKLM\SYSTEM\Storage\Volumes\{unique_guid}\DriveLetter`
- [ ] On boot: match partition GUID → previously assigned drive letter
- [ ] Prefer GUID-based mapping over discovery-order for drive letters
- [ ] Log: `[GPT] Partition GUID %s → drive %c:`
- [ ] Commit: `"gpt: persistent volume identification"`

---

## 13. GPT CRC Scrubbing & Self-Healing (🚀 Impossible OS Feature)

### 13.1 Periodic Integrity Validation

**Prompt:** GPT has built-in CRC32 redundancy but no OS proactively validates it after boot. Implement periodic CRC scrubbing: on a configurable interval (default: once per boot + every 24 hours), re-read the primary and backup GPT headers and entry arrays, recompute both CRC32 values, and verify they match. If either header has a CRC mismatch, automatically repair it from the other (same as §2.2 but triggered proactively, not on failure). Log all scrub results. This is equivalent to ZFS scrubbing for partition tables — no OS does this. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"gpt: periodic CRC scrubbing and self-healing"`. Add notes directly in this TODO section.

> [!TIP]
> **Competitive Edge:** No OS proactively validates GPT integrity after boot. Windows and Linux
> only check CRC32 on mount. Impossible OS can detect and repair silent corruption before it
> causes data loss — similar to ZFS scrubbing but for the partition table itself.

- [ ] Implement `gpt_scrub(dev)` — full integrity check:
  - [ ] Read primary header, recompute CRC32, compare
  - [ ] Read backup header, recompute CRC32, compare
  - [ ] Read primary entry array, recompute CRC32, compare to primary header's array CRC
  - [ ] Read backup entry array, recompute CRC32, compare to backup header's array CRC
  - [ ] Cross-validate: primary array CRC should match backup array CRC
- [ ] Auto-repair on mismatch:
  - [ ] Primary corrupt, backup valid → reconstruct primary from backup (§2.2)
  - [ ] Backup corrupt, primary valid → reconstruct backup from primary (§2.3)
  - [ ] Both corrupt → log critical error, do NOT attempt repair
- [ ] Schedule: run on boot + configurable interval
  - [ ] Registry: `HKLM\SYSTEM\Storage\GPT\ScrubIntervalHours` (default 24)
- [ ] Log: `[GPT] Scrub complete: primary=%s, backup=%s, repairs=%d`
- [ ] Expose via Disk Manager: "Validate Partition Table" with last-scrub timestamp
- [ ] Commit: `"gpt: periodic CRC scrubbing and self-healing"`

---

## 14. GUID Collision Detection (🚀 Impossible OS Feature)

### 14.1 Disk & Partition GUID Duplicate Detection

**Prompt:** When a disk is cloned (e.g., `dd`, Clonezilla), the clone has identical Disk GUID and Partition GUIDs. If both the original and clone are connected simultaneously, the duplicate GUIDs cause volume tracking confusion — the wrong partition can get the wrong drive letter or mount point. Windows silently regenerates GUIDs in some cases; Linux does nothing. Implement explicit detection: on disk enumeration, compare every Disk GUID and Partition GUID against all other known disks. On collision: log a warning, display in Disk Manager, and offer to regenerate GUIDs for the duplicate disk. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"gpt: GUID collision detection and resolution"`. Add notes directly in this TODO section.

> [!TIP]
> **Competitive Edge:** Windows handles collision silently (and sometimes incorrectly).
> Linux ignores it entirely. Impossible OS can alert the user and offer a one-click
> "Regenerate GUIDs" option in Disk Manager — far more transparent and safe.

- [ ] On disk enumeration: build global GUID registry (`disk_guid_table[]`)
- [ ] For each new disk: check `disk.disk_guid` against all registered disks
  - [ ] Collision found → log: `[GPT] WARNING: Disk GUID collision with disk %d`
  - [ ] Flag disk as `blkdev->guid_collision = true`
- [ ] For each partition: check `partition.unique_guid` against all registered partitions
  - [ ] Collision → log: `[GPT] WARNING: Partition GUID collision on %s`
- [ ] Implement `gpt_regenerate_guids(dev)`:
  - [ ] Generate new Disk GUID via `guid_generate()`
  - [ ] Generate new Unique GUID for each partition via `guid_generate()`
  - [ ] Update primary + backup headers and entry arrays
  - [ ] Update Registry volume mappings with new GUIDs
- [ ] Disk Manager: show warning icon on disks with GUID collision
  - [ ] Right-click → "Regenerate GUIDs" action
- [ ] Commit: `"gpt: GUID collision detection and resolution"`

---

## 15. GPT Backup & Restore (🚀 Impossible OS Feature)

### 15.1 Partition Table Backup & Restore

**Prompt:** Back up the entire GPT structure (PMBR + primary header + entry array + backup header + backup array) to a file for disaster recovery. Unlike MBR (512 bytes), GPT backup requires capturing ~33 KB of data (PMBR + header + 32 entry sectors). Restore from backup to recover a completely wiped partition table without affecting data. Auto-backup before every partition table modification. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"gpt: partition table backup and restore"`. Add notes directly in this TODO section.

> [!TIP]
> **Competitive Edge:** Windows has NO built-in GPT backup. Linux requires manual
> `sgdisk --backup`. Impossible OS auto-backing up before every partition change
> and offering one-click restore is a clear safety advantage.

- [ ] Implement `gpt_backup(dev, output_path)`:
  - [ ] Read PMBR (LBA 0, 512 bytes)
  - [ ] Read primary header (LBA 1)
  - [ ] Read primary entry array (LBAs 2–33 for 512b sectors)
  - [ ] Write to backup file: magic + version + disk_size + PMBR + header + entries
  - [ ] File format: `"IGPT" (4 bytes) + version (4 bytes) + disk_sectors (8 bytes) + sector_size (4 bytes) + PMBR (sector_size) + header (sector_size) + entries (16384 bytes)`
- [ ] Implement `gpt_restore(dev, input_path)`:
  - [ ] Validate backup magic and version
  - [ ] Verify disk size matches (warn if different)
  - [ ] Write PMBR to LBA 0
  - [ ] Write primary header to LBA 1
  - [ ] Write primary entry array to LBA 2+
  - [ ] Reconstruct and write backup header + array at disk end
  - [ ] Re-scan partition table after restore
- [ ] Auto-backup: save GPT to Registry before any write operation
  - [ ] `HKLM\SYSTEM\Storage\Disks\{guid}\GPTBackup` (binary blob)
- [ ] Disk Manager GUI: "Backup Partition Table" / "Restore Partition Table" buttons
- [ ] Commit: `"gpt: partition table backup and restore"`

---

## Priority Order

| Priority | Section                              | Description                                                 |
|----------|--------------------------------------|-------------------------------------------------------------|
| ✅ Done   | 1.1 Primary Header Validation       | Foundation — signature + CRC32 validation                   |
| ✅ Done   | 1.2 Partition Entry Array Parsing    | Foundation — entry parsing with array CRC32                 |
| ✅ Done   | 1.3 PMBR Detection                   | Foundation — Protective MBR `0xEE` check                    |
| 🔴 P0    | **2.1 Backup Header Fallback**       | **Reliability — don't fail on single-sector corruption**    |
| 🔴 P0    | **5.1 Type GUID Registry**           | **Correctness — identify all partition types encountered**  |
| 🟠 P1    | 2.2 Primary Header Auto-Recovery    | Redundancy — restore primary from valid backup              |
| 🟠 P1    | 3.1 Dynamic Sector Size (4Kn)       | Compatibility — modern NVMe + AF drives                     |
| 🟠 P1    | 4.1 GUID Encode + String Conversion | Write path — needed by §8–§9 write operations               |
| 🟠 P1    | 6.1 UEFI Global Attributes          | Correctness — Required, BIOSBoot, hidden partition handling |
| 🟠 P1    | 7.1 Hybrid MBR Detection            | Safety — warn and prefer GPT over conflicting MBR           |
| 🟡 P2    | 2.3 Backup Sync on Write            | Redundancy — keep backup in sync after modifications        |
| 🟡 P2    | 6.2 Microsoft Attributes            | Interop — read-only, hidden, no-automount on Basic Data     |
| 🟡 P2    | 8.1 PMBR Writer                     | Write support — create Protective MBR                       |
| 🟡 P2    | 8.2 GPT Header Writer               | Write support — serialize and write headers                 |
| 🟡 P2    | 8.3 Partition Entry Writer           | Write support — serialize entries + array CRC32             |
| 🟡 P2    | 9.1 Initialize GPT Disk            | Feature — create fresh GPT layout                           |
| 🟡 P2    | 9.2 Create Partition                | Feature — add partitions with alignment + validation        |
| 🟡 P2    | 9.3 Delete Partition                | Feature — remove partitions safely                          |
| 🟡 P2    | 9.4 Modify Attributes              | Feature — set bootable, read-only, hidden flags             |
| 🟡 P2    | 11.1 Volume Identification         | Feature — GUID-based persistent drive letter mapping        |
| 🟡 P2    | 13.1 CRC Scrubbing ⭐              | **Proactive integrity validation** — no OS does this       |
| 🟡 P2    | 14.1 GUID Collision ⭐             | **Clone disk safety** — transparent collision resolution   |
| 🟢 P3    | 9.5 Partition Resize ⭐            | **GUI drag-to-resize** — Windows severely limited          |
| 🟢 P3    | 10.1 Diskpart GPT Commands         | Tooling — CLI partition management                          |
| 🟢 P3    | 12.1 GPT Test Suite                 | Quality — automated test coverage                           |
| 🟢 P3    | 15.1 GPT Backup & Restore ⭐       | **Auto-backup partition table** — Windows has nothing      |

> [!NOTE]
> ⭐ = Feature where Impossible OS can be **superior** to both Windows and Linux.

---

## OS Comparison

| Feature                          | 🪟 Windows 11                       | 🐧 Linux (gdisk / parted)          | 🚀 Impossible OS                          |
| -------------------------------- | ---------------------------------- | ---------------------------------- | ----------------------------------------- |
| GPT header parsing               | ✅ Full (partmgr.sys)               | ✅ Full (part/efi.c)                | ✅ Done §1.1 (primary only)               |
| `"EFI PART"` signature check    | ✅                                   | ✅                                  | ✅ Done §1.1                               |
| Header CRC32 validation          | ✅                                   | ✅                                  | ✅ Done §1.1                               |
| Array CRC32 validation           | ✅ (assumes 128 entries)            | ✅ (uses actual NumberOfEntries)    | ✅ Done §1.2                               |
| Protective MBR detection         | ✅                                   | ✅                                  | ✅ Done §1.3                               |
| Backup header fallback           | ✅ Automatic                         | ✅ Automatic                        | ⬜ §2.1 P0                                |
| Primary auto-recovery            | ✅ Silent repair                     | ✅ gdisk repair                     | ⬜ §2.2 P1                                |
| Backup sync on write             | ✅                                   | ✅                                  | ⬜ §2.3 P2                                |
| 4Kn sector support               | ✅ Native                            | ✅ Native                           | ⬜ §3.1 P1                                |
| Mixed-endian GUID (read)         | ✅                                   | ✅                                  | ✅ Done §4.1                               |
| Mixed-endian GUID (write)        | ✅                                   | ✅                                  | ⬜ §4.1 P1                                |
| Full type GUID registry          | ✅ Exhaustive                        | ✅ Exhaustive                       | ⬜ §5.1 P0 (expanded: 25+ types)          |
| UEFI global attributes           | ✅ Required, BIOSBoot               | ✅ Full                             | ⬜ §6.1 P1                                |
| MS type-specific attributes      | ✅ ReadOnly, Hidden, NoMount        | ✅ Recognized                       | ⬜ §6.2 P2                                |
| Hybrid MBR detection             | ⚠️ Partial                         | ✅ gdisk warns                      | ⬜ §7.1 P1                                |
| GPT write / create partition     | ✅ Disk Management                   | ✅ gdisk / parted / sgdisk          | ⬜ §8–9 P2                                |
| Delete partition                 | ✅                                   | ✅                                  | ⬜ §9.3 P2                                |
| **Partition resize** ⭐          | ⚠️ Extend-only, last partition     | ⚠️ CLI parted/gdisk only           | ⬜ §9.5 P3 — GUI drag-to-resize           |
| GUID-based volume tracking       | ✅ mountvol                          | ✅ /dev/disk/by-partuuid            | ⬜ §11.1 P2                               |
| CLI partition tool               | ✅ diskpart                          | ✅ gdisk (interactive + scripted)   | ⬜ §10.1 P3                               |
| **CRC scrubbing** ⭐             | ❌ Only checks on mount             | ❌ Only checks on mount             | ⬜ §13.1 P2 — proactive periodic scrub     |
| **GUID collision detection** ⭐  | ⚠️ Silent, sometimes wrong         | ❌ No detection                     | ⬜ §14.1 P2 — GUI alert + one-click fix    |
| **GPT backup & restore** ⭐     | ❌ No built-in backup               | ⚠️ `sgdisk --backup` CLI only      | ⬜ §15.1 P3 — auto-backup on every change  |
| **128-entry interop**            | ✅ (enforces 128)                    | ✅ (flexible)                       | ✅ Uses `NumberOfEntries` from header      |
| **CRC32 polynomial correctness** | ✅ CCITT `0xEDB88320`               | ✅ CCITT `0xEDB88320`              | ✅ Correct polynomial implemented          |
