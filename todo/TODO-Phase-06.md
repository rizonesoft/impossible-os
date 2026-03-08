# Phase 06 — Storage & Persistent Filesystem

> **Goal:** Move from a RAM-only filesystem to real persistent storage. Implement
> disk drivers (virtio-blk, AHCI), a widely-compatible filesystem (FAT32), persist
> the custom IXFS format to disk, add partition table support (GPT/MBR), and build
> a graphical disk management tool.

---

## 1. Disk Drivers
> *Research: [01_disk_persistent_fs.md](research/phase_06_storage/01_disk_persistent_fs.md)*

### 1.1 VirtIO Block Device Driver ✅

- [x] Create `src/kernel/drivers/virtio_blk.c` and `include/virtio_blk.h`
- [x] Detect virtio-blk device via PCI enumeration (vendor `0x1AF4`, device `0x1001` or `0x1042`)
- [x] Map MMIO BAR registers via modern VirtIO 1.0 PCI capabilities
- [x] Initialize virtio device:
  - [x] Acknowledge → DRIVER → negotiate features → FEATURES_OK → DRIVER_OK
  - [x] Allocate virtqueue (descriptor table, available ring, used ring) via `virtq_init()`
- [x] Implement `virtio_blk_read(lba, count, buffer)` — 3-descriptor chain (header, data, status)
- [x] Implement `virtio_blk_write(lba, count, buffer)` — write sectors
- [x] Implement `virtio_blk_capacity()` — query disk size via device_cfg MMIO
- [x] Register with VFS block device layer *(done in §1.3 as "virtio0")*
- [x] Test: read sector 0 from QEMU virtio disk (verified UEFI + BIOS boot)
- [x] QEMU flag: `-drive file=disk.img,format=raw,if=none,id=disk0 -device virtio-blk-pci,drive=disk0`
- [x] Commit: `"drivers: virtio-blk modern VirtIO 1.0 transport"`

> **Implementation notes:**
> - Uses modern VirtIO 1.0 MMIO transport from `virtio.c` (shared with virtio-input)
> - Legacy PIO transport abandoned due to device reset issues on both QEMU 8.2 and 10.2
> - Negotiates `VIRTIO_F_VERSION_1` for modern interface
> - QEMU upgraded from 8.2.2 → 10.2.1 (built from source with GTK display)

### 1.2 AHCI (SATA) Driver ✅

- [x] Create `src/kernel/drivers/ahci.c` and `include/kernel/drivers/ahci.h`
- [x] Detect AHCI controller via PCI (class `0x01`, subclass `0x06`)
- [x] Map ABAR (AHCI Base Address Register) from PCI BAR5
- [x] Enumerate ports: scan for attached SATA drives (device signature)
- [x] Initialize HBA: enable AHCI mode, clear interrupts, allocate command lists + FIS buffers
- [x] Implement `ahci_read(port, lba, count, buffer)` — READ DMA EXT + PRDT
- [x] Implement `ahci_write(port, lba, count, buffer)` — WRITE DMA EXT command
- [x] Implement `ahci_identify(port)` — IDENTIFY DEVICE (model, serial, LBA48 capacity)
- [x] Handle AHCI IRQ (polling-based, interrupt enable per port)
- [x] Register with VFS block device layer *(done in §1.3 as "sata0")*
- [x] Test: read drive identity and sector 0 from QEMU SATA disk
- [x] QEMU flags: `-drive file=sata.img,format=raw,if=none,id=disk1 -device ahci,id=ahci0 -device ide-hd,drive=disk1,bus=ahci0.0`
- [x] Commit: `"drivers: AHCI SATA disk driver"`

> **Implementation notes:**
> - Intel ICH9 AHCI controller detected at PCI 0:3.0
> - ABAR at 0x81043000, AHCI v1.0, 6 ports / 32 command slots
> - Per-port: 1 KiB command list (32 headers), 256-byte FIS receive, 32 command tables
> - Uses command slot 0 with single PRDT entry for I/O
> - Tested: "QEMU HARDDISK" 32 MiB (65536 sectors), sector 0 read OK

### 1.3 Block Device Abstraction Layer ✅

- [x] Create `include/kernel/drivers/blkdev.h` and `src/kernel/drivers/blkdev.c`
- [x] Define `struct blkdev` (name, sector_size, sector_count, read_fn, write_fn, driver_data)
- [x] Implement `blkdev_register(dev)` — add to global device list
- [x] Implement `blkdev_get(name)` — look up by name ("virtio0", "sata0")
- [x] Implement `blkdev_read(dev, lba, count, buf)` — dispatch to driver
- [x] Implement `blkdev_write(dev, lba, count, buf)` — dispatch to driver
- [x] Implement `blkdev_list()` — enumerate all registered devices
- [x] Register existing ATA/IDE driver as a blkdev (adapter wraps `uint8_t count`)
- [x] Register virtio-blk and AHCI as blkdevs (adapter wrappers in `main.c`)
- [x] Commit: `"drivers: block device abstraction layer"`

> **Implementation notes:**
> - Registry holds up to 16 devices, function-pointer dispatch
> - Adapter wrappers bridge driver APIs to uniform `(lba, count, buf, driver_data)` signature
> - AHCI uses `driver_data` as port index; VirtIO ignores it
> - Tested: 2 devices registered (virtio0 + sata0)

---

## 2. Partition Table Support
> *Research: [01_disk_persistent_fs.md](research/phase_06_storage/01_disk_persistent_fs.md)*

### 2.1 MBR Partition Table ✅

- [x] Create `src/kernel/fs/mbr.c`
- [x] Parse MBR at LBA 0: 4 partition entries at offset 446
- [x] Define `struct mbr_entry` (status, type, start_lba, sector_count)
- [x] Recognize partition types: `0x0C` (FAT32 LBA), `0x83` (Linux), `0xDA` (IXFS custom)
- [x] Return partition list (start LBA, size, type) for each active entry
- [x] Commit: `"fs: MBR partition table parsing"`

### 2.2 GPT Partition Table

- [x] Create `src/kernel/fs/gpt.c`
- [x] Detect GPT: check protective MBR at LBA 0 (type `0xEE`)
- [x] Parse GPT header at LBA 1: verify signature "EFI PART", validate CRC32
- [x] Parse partition entry array (128-byte entries, starting at LBA 2)
- [x] Define `struct gpt_entry` (type_guid, unique_guid, start_lba, end_lba, name)
- [x] Recognize GUIDs: EFI System Partition, Microsoft Basic Data, custom IXFS GUID
- [x] CRC32 validation of header + partition array
- [x] Commit: `"fs: GPT partition table parsing"`

### 2.3 Partition Scanner

- [x] Create `src/kernel/fs/partition.c`
- [x] On each registered blkdev: try GPT first, fall back to MBR
- [x] For each discovered partition: create a sub-blkdev (offset reads/writes by partition start LBA)
- [x] Auto-detect filesystem on each partition (probe FAT32, ext2, IXFS magic bytes)
- [x] Log discovered partitions via serial: "Disk 0, Partition 1: FAT32, 2.0 GB"
- [x] Call at boot after disk drivers initialize
- [x] Commit: `"fs: partition scanner & auto-detect"`

---

## 3. FAT32 Filesystem
> *Research: [01_disk_persistent_fs.md](research/phase_06_storage/01_disk_persistent_fs.md)*

### 3.1 FAT32 Read Support

- [x] Create `src/kernel/fs/fat32.c` and `include/fat32.h`
- [x] Parse BPB (BIOS Parameter Block) at partition start:
  - [x] bytes_per_sector, sectors_per_cluster, reserved_sectors, fat_count, root_cluster
- [x] Calculate: FAT region start, data region start, cluster→LBA conversion
- [x] Implement `fat32_read_cluster_chain(start_cluster)` — follow FAT entries
- [x] Implement `fat32_read_dir(cluster)` — parse 32-byte directory entries
  - [x] Handle 8.3 short names
  - [x] Handle LFN (Long File Name) entries
- [x] Implement `fat32_read_file(path, buffer, max_size)` — traverse directory tree, read cluster chain
- [x] Implement `fat32_stat(path)` — return file size, attributes, timestamps
- [x] Commit: `"fs: FAT32 read support"`

### 3.2 FAT32 Write Support

- [x] Implement `fat32_write_file(path, data, size)` — allocate clusters, write data, update directory entry
- [x] Implement `fat32_create_file(dir, name)` — add directory entry, allocate first cluster
- [x] Implement `fat32_create_dir(dir, name)` — create directory with `.` and `..` entries
- [x] Implement `fat32_delete_file(path)` — mark clusters as free in FAT, clear directory entry
- [x] Implement `fat32_rename(old_path, new_path)` — update directory entry name
- [x] Implement free cluster search (scan FAT for `0x00000000` entries)
- [x] Implement `fat32_format(dev, label)` — write BPB, FATs, root directory
- [x] Flush FAT to disk after modifications (write both FAT copies)
- [x] Commit: `"fs: FAT32 write support"`

### 3.3 FAT32 VFS Integration

- [x] Register FAT32 as a VFS filesystem type
- [x] Implement VFS callbacks: open, close, read, write, readdir, stat, create, delete, rename
- [x] Mount FAT32 partition to a drive letter (e.g., `D:\`)
- [x] Test: create file on FAT32, reboot, verify file persists
- [x] Commit: `"fs: FAT32 VFS integration"`

---

## 4. External Filesystem Drivers
> *Read/write drivers for NTFS, ext2/ext3/ext4, and exFAT*

### 4.1 NTFS Read Support

- [ ] Create `src/kernel/fs/ntfs.c` and `include/kernel/fs/ntfs.h`
- [ ] Parse NTFS boot sector: bytes_per_sector, sectors_per_cluster, MFT start cluster
- [ ] Define `struct ntfs_mft_entry` (signature "FILE", update sequence, attribute list)
- [ ] Parse MFT entry attributes: `$STANDARD_INFORMATION`, `$FILE_NAME`, `$DATA`
- [ ] Implement data run decoding: parse compressed (offset, length) pairs → LBA list
- [ ] Implement `ntfs_read_mft_entry(mft_number)` — read 1024-byte MFT record
- [ ] Implement `ntfs_read_file(mft_entry, buffer, size)` — follow data runs, read clusters
- [ ] Implement `ntfs_readdir(mft_entry)` — parse `$INDEX_ROOT` / `$INDEX_ALLOCATION` B+ tree
- [ ] Recognize `$MFT`, `$MFTMirr`, `$Root` (MFT entry 5) system files
- [ ] Add NTFS probe to partition scanner (boot sector OEM ID = "NTFS    ")
- [ ] Commit: `"fs: NTFS read support"`

### 4.2 NTFS Write Support

- [ ] Implement `ntfs_write_file(mft_entry, data, size)` — write to existing data runs
- [ ] Implement `ntfs_extend_data_run(mft_entry, clusters)` — allocate new clusters
- [ ] Implement `ntfs_create_file(dir_mft, name)` — allocate MFT entry, add to index
- [ ] Implement `ntfs_delete_file(dir_mft, name)` — mark MFT entry as free, free clusters
- [ ] Implement free cluster bitmap (`$Bitmap`) read/write
- [ ] Update `$STANDARD_INFORMATION` timestamps on file operations
- [ ] Commit: `"fs: NTFS write support"`

### 4.3 NTFS VFS Integration

- [ ] Register NTFS as a VFS filesystem type
- [ ] Implement VFS callbacks: open, close, read, write, readdir, stat, create, delete
- [ ] Mount NTFS partition to a drive letter (e.g., `D:\`)
- [ ] Test: read files from a Windows-formatted NTFS partition
- [ ] Commit: `"fs: NTFS VFS integration"`

### 4.4 ext2/ext3/ext4 Read Support

> ext2/ext3/ext4 share the same on-disk layout (magic `0xEF53`). A single
> driver handles all three — ext3 adds journaling, ext4 adds extents.

- [ ] Create `src/kernel/fs/ext2.c` and `include/kernel/fs/ext2.h`
- [ ] Parse superblock at byte 1024: magic, block_size, inode_size, blocks_count, inodes_count
- [ ] Parse block group descriptor table (follows superblock)
- [ ] Implement `ext2_read_inode(ino)` — locate inode in correct block group
- [ ] Implement block pointer traversal: 12 direct + indirect + dindirect + tindirect
- [ ] Detect ext4 extents: check `EXT4_EXTENTS_FL` flag, parse extent tree header + entries
- [ ] Implement `ext2_read_file(inode, offset, buf, size)` — follow block pointers or extents
- [ ] Implement `ext2_readdir(inode)` — parse `ext2_dir_entry_2` linked list
- [ ] Implement `ext2_stat(path)` — return file metadata from inode
- [ ] VFS integration: register ext2 driver, mount to drive letter
- [ ] Update partition scanner to log "ext2", "ext3", or "ext4" based on feature flags
- [ ] Commit: `"fs: ext2/ext3/ext4 read support"`

### 4.5 ext2/ext3/ext4 Write Support

- [ ] Implement block bitmap read/write for allocation
- [ ] Implement inode bitmap read/write for inode allocation
- [ ] Implement `ext2_write_file(inode, offset, data, size)` — allocate blocks, write data
- [ ] Implement `ext2_create(dir_inode, name, type)` — allocate inode + dir entry
- [ ] Implement `ext2_delete(dir_inode, name)` — free blocks/inode, unlink dir entry
- [ ] Implement `ext2_rename(dir, old_name, new_name)`
- [ ] Update block group descriptor free counts + superblock free counts
- [ ] Wire write ops into VFS callbacks
- [ ] Commit: `"fs: ext2/ext3/ext4 write support"`

### 4.6 exFAT Read Support

> exFAT is the standard for large USB drives (>32 GB) and SDXC cards.
> Simpler than NTFS, designed for flash media.

- [ ] Create `src/kernel/fs/exfat.c` and `include/kernel/fs/exfat.h`
- [ ] Parse exFAT boot sector: bytes_per_sector_shift, sectors_per_cluster_shift, cluster_count, FAT_offset, data_offset, root_dir_cluster
- [ ] Verify OEM name `"EXFAT   "` and boot signature `0xAA55`
- [ ] Implement FAT traversal (32-bit entries, similar to FAT32 but no 0x0FFFFFFF masking)
- [ ] Implement `exfat_read_dir(cluster)` — parse directory entry set:
  - [ ] File Directory Entry (0x85): attributes, timestamps, data_length
  - [ ] Stream Extension Entry (0xC0): name_length, first_cluster, data_length
  - [ ] File Name Entry (0xC1): 15 UTF-16LE characters per entry
- [ ] Assemble full filename from chained File Name Entries
- [ ] Implement `exfat_read_file(path, buf, size)` — follow cluster chain
- [ ] Implement `exfat_stat(path)` — return file metadata
- [ ] Add exFAT probe to partition scanner (OEM "EXFAT   " at boot sector)
- [ ] VFS integration: register exfat driver, mount to drive letter
- [ ] Commit: `"fs: exFAT read support"`

### 4.7 exFAT Write Support

- [ ] Implement allocation bitmap read/write (replaces FAT-based free scan)
- [ ] Implement `exfat_alloc_cluster()` — scan allocation bitmap
- [ ] Implement `exfat_write_file(dir_cluster, name, data, size)` — allocate clusters, write data
- [ ] Implement `exfat_create_file(dir, name)` — create directory entry set (File + Stream + Name entries)
- [ ] Implement `exfat_create_dir(parent, name)` — create directory with dot entries
- [ ] Implement `exfat_delete_file(dir, name)` — free clusters, mark entries as deleted (0x05/0x40/0x41)
- [ ] Implement `exfat_rename(dir, old, new)` — update File Name entries
- [ ] UpCase table validation for case-insensitive comparisons
- [ ] Wire write ops into VFS callbacks
- [ ] Commit: `"fs: exFAT write support"`

---

## 5. Persistent IXFS (IXFS-on-Disk)
> *Research: [01_disk_persistent_fs.md](research/phase_06_storage/01_disk_persistent_fs.md)*

### 5.1 IXFS On-Disk Format

- [x] Define IXFS superblock (magic "IXFS", version, block_size, block_count, inode_count, free_block_bitmap_lba)
- [x] Define inode table: fixed-size inodes (name, type, size, timestamps, block pointers)
- [x] Direct block pointers (12) + single indirect + double indirect
- [x] Free block bitmap for allocation
- [x] Free inode bitmap for inode allocation
- [x] Write IXFS format specification document
- [x] Commit: `"fs: IXFS on-disk format specification"`

### 5.2 IXFS Disk Operations

- [x] Create `src/kernel/fs/ixfs_disk.c`
- [x] Implement `ixfs_format(dev, label)` — write superblock, bitmaps, root directory inode
- [x] Implement `ixfs_mount(dev)` — read superblock, load bitmaps into memory
- [x] Implement `ixfs_alloc_block()` — find free block from bitmap
- [x] Implement `ixfs_free_block(block)` — mark block as free
- [x] Implement `ixfs_alloc_inode()` — find free inode slot
- [x] Implement `ixfs_read_inode(ino)` — read inode from disk
- [x] Implement `ixfs_write_inode(ino, inode)` — write inode to disk
- [x] Commit: `"fs: IXFS disk operations"`

### 5.3 IXFS File Operations

- [x] Implement `ixfs_read_file(inode, offset, buf, size)` — follow block pointers, read data
- [x] Implement `ixfs_write_file(inode, offset, data, size)` — allocate blocks, write data
- [x] Implement `ixfs_create(dir_inode, name, type)` — add directory entry + allocate inode
- [x] Implement `ixfs_delete(dir_inode, name)` — free blocks + inode, remove directory entry
- [x] Implement `ixfs_rename(dir, old_name, new_name)` — update directory entry
- [x] Implement `ixfs_readdir(dir_inode)` — list directory entries
- [x] Commit: `"fs: IXFS file operations"`

### 5.4 IXFS VFS Integration

- [x] Register IXFS as a VFS filesystem type
- [x] Implement VFS callbacks: open, close, read, write, readdir, stat, create, delete, rename
- [x] Mount IXFS partition as `C:\` (system drive)
- [x] Migrate boot: copy initrd contents → IXFS `C:\` on first boot
- [x] Test: write file, reboot, verify persistence
- [x] Commit: `"fs: IXFS VFS integration + C:\\ mount"`

#### First-Boot Setup (temporary — remove when installer exists)
- [x] Create `src/kernel/fs/firstboot.c` + `include/kernel/fs/firstboot.h`
- [x] Detect empty C:\ on boot (readdir returns no entries)
- [x] Create default hierarchy: `Impossible\System`, `Impossible\Commands`, `Users\Default`, `Programs`
- [x] Copy all initrd files (B:\) to `C:\Impossible\System\`
- [x] Call `firstboot_setup()` from `main.c` after `partition_mount_filesystems()`
- [x] Commit: `"fs: first-boot setup module"`

### 5.5 Performance Optimizations

> **Priority: P1** — These fix fundamental performance issues in the current driver

#### 5.5.1 Block Group Allocator
- [ ] Divide volume into block groups (~128 MiB each) for locality
- [ ] Track per-group free block count + "next free" hint in group descriptor
- [ ] `ixfs_alloc_block()` starts from hint, wraps around on full group
- [ ] Prefer allocating in same group as file's inode (reduces seek)
- [ ] Update superblock `s_free_blocks` only on group exhaustion (lazy)
- [ ] Commit: `"fs: IXFS block group allocator"`

#### 5.5.2 Buffer Cache / Write-Back
- [ ] Implement dirty buffer cache (LRU, configurable size)
- [ ] Cache recently-read blocks in memory (avoid re-reading bitmaps/inodes)
- [ ] Batch dirty blocks and flush to disk periodically or on `sync()`
- [ ] Mark buffers dirty on write; actual disk I/O deferred
- [ ] Flush all dirty buffers on unmount
- [ ] Commit: `"fs: IXFS buffer cache + write-back"`

#### 5.5.3 Directory Hash Index (B-tree)
- [ ] For directories with > 64 entries: build hash index in an extra block
- [ ] Use FNV-1a hash of filename → bucket → chain of entry offsets
- [ ] `finddir()` goes from O(n) → O(1) average case
- [ ] Falls back to linear scan for small directories (< 64 entries)
- [ ] Rebuild index on create/delete
- [ ] Commit: `"fs: IXFS directory hash index"`

#### 5.5.4 Multi-Volume Support
- [ ] Remove static globals: wrap all state in `struct ixfs_volume`
- [ ] Each mounted IXFS partition gets its own `ixfs_volume` instance
- [ ] Pass volume pointer through VFS `fs_data` / `priv_data`
- [ ] Support mounting multiple IXFS partitions simultaneously (C:\, E:\, etc.)
- [ ] Commit: `"fs: IXFS multi-volume support"`

### 5.6 Extent-Based Allocation

> **Priority: P1** — Replaces the old block-pointer system entirely

- [ ] Define `struct ixfs_extent { uint64_t start_block; uint32_t block_count; }` (12 bytes)
- [ ] Replace `i_direct[12]` + `i_indirect` + `i_dindirect` with extent array in inode
- [ ] Fit 4 inline extents directly in inode (replaces block pointers, same 52 bytes)
- [ ] If > 4 extents needed: store extent tree in a data block (B-tree of extents)
- [ ] Merge adjacent extents on allocation (contiguous writes = 1 extent)
- [ ] Update `ixfs_alloc_block()` to allocate contiguous runs preferentially
- [ ] Update `ixfs_read_file` / `ixfs_write_file` to use extent lookup
- [ ] Max file size with 64-bit block numbers: **64 TiB** (with 4K blocks)
- [ ] Commit: `"fs: IXFS extent-based allocation"`

### 5.7 Write-Ahead Log (Journal)

> **Priority: P1** — Crash safety is essential for a system partition

- [ ] Reserve journal area: configurable size (default 32 MiB), stored after superblock
- [ ] Journal record: `{ txn_id, block_number, old_data, new_data, checksum }`
- [ ] **Metadata journaling** (default): journal only inode/bitmap/directory changes
- [ ] Optional **full journaling**: journal data blocks too (slower but safer)
- [ ] Transaction API: `ixfs_txn_begin()`, `ixfs_txn_write()`, `ixfs_txn_commit()`
- [ ] On commit: write all records to journal → flush → write to final location → flush → mark txn complete
- [ ] On mount after crash: replay incomplete transactions from journal
- [ ] Circular journal with head/tail pointers in superblock
- [ ] Commit: `"fs: IXFS write-ahead log (journal)"`

### 5.8 Copy-on-Write (CoW) & Snapshots

> **Priority: P2** — IXFS's signature feature, differentiating it from ext4/NTFS

- [ ] **CoW block writes**: never overwrite a block in-place; always write to a new location
- [ ] Update parent pointers (extent tree / inode) to point to new block
- [ ] Old blocks remain valid until explicitly freed → enables snapshots
- [ ] **Snapshot API**: `ixfs_snapshot_create(name)` — freeze current state by incrementing refcounts
- [ ] **Refcounted blocks**: each block has a reference count (shared between snapshots)
- [ ] `ixfs_free_block()` decrements refcount; only frees when refcount reaches 0
- [ ] **Snapshot listing**: `ixfs_snapshot_list()` — enumerate available snapshots
- [ ] **Snapshot rollback**: `ixfs_snapshot_restore(name)` — swap active tree with snapshot tree
- [ ] **Snapshot delete**: `ixfs_snapshot_delete(name)` — decrement refcounts, free unreferenced blocks
- [ ] Shell commands: `snapshot create <name>`, `snapshot list`, `snapshot restore <name>`
- [ ] Commit: `"fs: IXFS copy-on-write + snapshots"`

### 5.9 Advanced Features

#### 5.9.1 Sparse File Support
- [ ] Allow holes in files: extents with `start_block = 0` represent unallocated regions
- [ ] `read()` on a hole returns zeroes without allocating blocks
- [ ] `write()` into a hole allocates only the needed blocks
- [ ] `ixfs_stat()` reports both logical size and actual blocks used
- [ ] Commit: `"fs: IXFS sparse file support"`

#### 5.9.2 Inline Small Files
- [ ] Files ≤ 48 bytes: store data directly in inode's extent/pointer area (no data block needed)
- [ ] Flag in `i_mode` or `i_flags`: `IXFS_INLINE` indicates inline data
- [ ] Transparently promote to extent-based when file grows beyond 48 bytes
- [ ] Significant speedup for small config files, symlinks, etc.
- [ ] Commit: `"fs: IXFS inline small files"`

#### 5.9.3 Per-Block Checksums
- [ ] CRC32C checksum computed on every block write, stored in checksum table
- [ ] Checksum table: dedicated blocks between bitmap and inode table
- [ ] Verify checksum on every block read; log corruption if mismatch
- [ ] Superblock checksum field for self-verification
- [ ] `ixfs_scrub()` — full-volume integrity scan (background or on-demand)
- [ ] Commit: `"fs: IXFS per-block checksums"`

#### 5.9.4 64-Bit Block Addressing
- [ ] Upgrade `s_total_blocks`, extent `start_block`, and inode pointers to `uint64_t`
- [ ] Superblock version bump to v2
- [ ] Maximum volume size: **64 TiB** (4K blocks × 2^44 blocks)
- [ ] Backward-compatible: v1 volumes mountable read-only, auto-upgrade on format
- [ ] Commit: `"fs: IXFS 64-bit block addressing"`

### 5.10 IXFS Unique Selling Points

> These features, taken together, make IXFS genuinely competitive:
>
> | Feature | ext4 | NTFS | Btrfs | **IXFS** |
> |---------|------|------|-------|----------|
> | Journaling | ✅ | ✅ | N/A (CoW) | ✅ WAL + CoW |
> | Copy-on-Write | ❌ | ❌ | ✅ | ✅ |
> | Snapshots | ❌ | VSS (userspace) | ✅ | ✅ (kernel-native) |
> | Inline small files | ❌ | ✅ (MFT) | ✅ | ✅ |
> | Extent-based | ✅ | ✅ | ✅ | ✅ |
> | Per-block checksum | ❌ (metadata only) | ❌ | ✅ | ✅ |
> | Sparse files | ✅ | ✅ | ✅ | ✅ |
> | Block groups | ✅ | ❌ | ❌ | ✅ |
> | Max file/vol size | 16 TiB | 16 TiB | 16 EiB | **64 TiB** |
>
> **IXFS's unique identity**: A hybrid of ext4's block-group locality with
> Btrfs-style CoW snapshots, plus mandatory per-block checksums. It is
> designed from scratch for Impossible OS with zero legacy baggage.

### 5.11 Host-Side mkfs-ixfs Tool (Option B)

> **Priority: P3** — Needed for the installer / build-time disk images

- [ ] Create `tools/mkfs-ixfs.c` — standalone host tool (runs on Linux/macOS)
- [ ] Accept: output image path, volume size, label
- [ ] Write IXFS superblock, bitmaps, inode table (same layout as kernel `ixfs_format()`)
- [ ] `--populate <dir>` flag: recursively copy a host directory into the IXFS image
- [ ] Makefile target: `build/system-disk.img` with pre-populated IXFS from `build/initrd_files/`
- [ ] Once working: remove firstboot.c/firstboot.h and the `firstboot_setup()` call from main.c
- [ ] Commit: `"tools: mkfs-ixfs host formatter"`

---

## 6. Drive Letter Mounting

### 6.1 Auto-Mount System

- [ ] After partition scanning: auto-assign drive letters
  - [ ] `C:\` — first IXFS partition (system drive)
  - [ ] `D:\`, `E:\`, etc. — additional partitions in order
- [ ] Update existing `vfs_mount()` to support real disk partitions (not just initrd)
- [ ] Store mount configuration in Codex: `System\Storage\Drive\{letter}\Device`, `Filesystem`
- [ ] Log mounts: "Mounted C:\\ (IXFS, 2.0 GB) on disk0-part1"
- [ ] Commit: `"fs: auto-mount drive letters"`

### 6.2 Manual Mount/Unmount

- [ ] Shell command: `mount D: /dev/disk1p1 fat32` — mount a partition
- [ ] Shell command: `umount D:` — unmount a drive
- [ ] Flush pending writes before unmount
- [ ] Prevent unmount of `C:\` while running
- [ ] Commit: `"shell: mount/umount commands"`

---

## 7. Disk Management GUI
> *Research: [02_disk_management.md](research/phase_06_storage/02_disk_management.md)*

### 7.1 Disk Management App

- [ ] Create `src/apps/diskmgr/diskmgr.c`
- [ ] Upper panel: table view of mounted drives (Drive, Size, Used, Free, Filesystem)
- [ ] Lower panel: graphical partition layout per physical disk
  - [ ] Colored bars proportional to partition size
  - [ ] Labels: drive letter, filesystem, size
  - [ ] Unallocated space shown as empty bar
- [ ] Read from: blkdev list + partition table + filesystem stats
- [ ] Commit: `"apps: Disk Management layout"`

### 7.2 Disk Operations

- [ ] Create partition: select unallocated space → set size + filesystem type
- [ ] Delete partition: select partition → confirm → delete (removes data!)
- [ ] Format partition: select partition → choose filesystem (FAT32, IXFS)
- [ ] Assign/change drive letter
- [ ] View usage: pie chart or bar showing used vs. free
- [ ] *(Stretch)* Resize partition (requires filesystem support)
- [ ] *(Stretch)* SMART status for AHCI/NVMe drives
- [ ] Commit: `"apps: Disk Management operations"`

---

## 8. Agent-Recommended Additions

> Items not in the research files but critical for a complete storage subsystem.

### 8.1 Disk Cache (Buffer Cache)

- [ ] Implement block-level read cache (LRU, configurable size: 1–8 MB)
- [ ] Cache recently read sectors to avoid repeated disk I/O
- [ ] Write-back cache: batch writes, flush periodically or on sync
- [ ] `cache_flush()` — force write all dirty blocks (called on shutdown)
- [ ] `cache_invalidate(dev)` — clear cache for a device (on unmount)
- [ ] Commit: `"fs: block-level disk cache"`

### 8.2 Filesystem Integrity

- [ ] `fsck_ixfs()` — basic filesystem check on IXFS
  - [ ] Validate superblock magic and version
  - [ ] Check free block bitmap consistency
  - [ ] Verify inode reference counts
  - [ ] Detect orphan inodes (allocated but unreferenced)
- [ ] `fsck_fat32()` — basic FAT32 check
  - [ ] Validate BPB fields
  - [ ] Check FAT chain consistency (no cross-links)
  - [ ] Detect lost clusters (allocated but not in any chain)
- [ ] Auto-check on mount if "dirty" flag is set (unclean shutdown)
- [ ] Commit: `"fs: filesystem integrity checks (fsck)"`

### 8.3 NVMe Driver (Future)

- [ ] *(Stretch)* Create `src/kernel/drivers/nvme.c`
- [ ] *(Stretch)* Detect NVMe controller via PCI (class `0x01`, subclass `0x08`)
- [ ] *(Stretch)* Map BAR0, initialize admin + I/O submission/completion queues
- [ ] *(Stretch)* Implement read/write via NVMe commands (Identify, Read, Write)
- [ ] *(Stretch)* QEMU flag: `-drive file=disk.img,format=raw,if=none,id=d0 -device nvme,drive=d0,serial=1234`

### 8.4 USB Mass Storage (Future)

- [ ] *(Stretch)* USB mass storage class driver (bulk-only transport)
- [ ] *(Stretch)* SCSI command layer (INQUIRY, READ10, WRITE10)
- [ ] *(Stretch)* Hot-plug notification: show toast "USB drive detected", auto-mount
- [ ] *(Stretch)* Safe removal: flush + unmount + notification

### 8.5 Disk I/O Metrics

- [ ] Track read/write byte counts per block device
- [ ] Track I/O operations per second
- [ ] Expose via `blkdev_stats(dev)` — used by Task Manager performance tab
- [ ] Shell command: `iostat` — show disk I/O statistics
- [ ] Commit: `"drivers: disk I/O metrics"`

### 8.6 IXFS Directory Structure (First Boot)

- [ ] On first boot (fresh IXFS format), create standard directory tree:
  - [ ] `C:\Impossible\` — system root
  - [ ] `C:\Impossible\System\` — system files
  - [ ] `C:\Impossible\System\Config\Codex\` — Codex registry files
  - [ ] `C:\Impossible\Bin\` — system executables
  - [ ] `C:\Impossible\Fonts\` — system fonts
  - [ ] `C:\Impossible\Icons\` — system icons
  - [ ] `C:\Impossible\Wallpapers\` — wallpapers
  - [ ] `C:\Users\Default\` — default user home
  - [ ] `C:\Users\Default\Desktop\` — desktop shortcuts
  - [ ] `C:\Users\Default\Documents\`
  - [ ] `C:\Users\Default\Downloads\`
  - [ ] `C:\Users\Default\Pictures\`
  - [ ] `C:\Temp\` — temp files
  - [ ] `C:\Recycle\` — recycle bin
  - [ ] `C:\Programs\` — installed applications
- [ ] Copy initrd contents into IXFS directories
- [ ] Commit: `"fs: IXFS first-boot directory structure"`

---

## Priority Order

| Priority | Section | Reason |
|----------|---------|--------|
| ✅ Done | 1.1 VirtIO-blk Driver | Modern VirtIO 1.0 MMIO — implemented and tested |
| ✅ Done | 1.3 Block Device Layer | Unified interface over VirtIO/AHCI |
| ✅ Done | 2. Partition Tables | MBR + GPT + partition scanner |
| 🔴 P0 | 3.1 FAT32 Read | Read USB drives, boot media |
| 🟠 P1 | 4.1 NTFS Read | Read Windows-formatted partitions |
| 🟠 P1 | 5.1–5.4 IXFS on Disk | Persistence — files survive reboot |
| 🟠 P1 | 6.1 Auto-Mount | Drive letters from real disks |
| 🟠 P1 | 3.2 FAT32 Write | Full read/write for removable media |
| 🟠 P1 | 8.6 IXFS Directory Structure | Standard paths on first boot |
| ✅ Done | 1.2 AHCI Driver | SATA HDD/SSD — implemented and tested |
| 🟡 P2 | 4.2 NTFS Write | Write to Windows partitions |
| 🟡 P2 | 8.1 Disk Cache | Performance — reduce disk I/O |
| 🟡 P2 | 6.2 Mount/Unmount Commands | Manual storage management |
| 🟡 P2 | 7. Disk Management GUI | Visual partition management |
| 🟢 P3 | 3.3 FAT32 VFS + Format | Complete FAT32 integration |
| 🟢 P3 | 4.3 NTFS VFS | Complete NTFS integration |
| 🟢 P3 | 8.2 Filesystem Integrity | Recovery from unclean shutdown |
| 🟢 P3 | 8.5 Disk I/O Metrics | Performance monitoring |
| 🔵 P4 | 8.3 NVMe Driver | Modern SSD support (future) |
| 🔵 P4 | 8.4 USB Mass Storage | Hot-plug USB drives (future) |
