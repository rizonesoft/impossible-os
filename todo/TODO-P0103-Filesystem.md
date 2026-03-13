# Phase 06 — Storage & Persistent Filesystem

> **Goal:** Move from a RAM-only filesystem to real persistent storage. Implement
> disk drivers (virtio-blk, AHCI), a widely-compatible filesystem (FAT32), persist
> the custom IXFS format to disk, add partition table support (GPT/MBR), and build
> a graphical disk management tool.
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.


---

## 1. Disk Drivers

### 1.1 VirtIO Block Device Driver ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `src/kernel/drivers/virtio_blk.c` exists with `virtio_blk_read`, `virtio_blk_write`, `virtio_blk_capacity`, VirtIO 1.0 PCI capability detection, and 3-descriptor chain I/O. Check it registers as a blkdev ("virtio0"). Verify `docs/architecture/storage-drivers.md` exists and covers VirtIO — create or update if missing. Run `make clean && make all && make run`. Fix any inconsistencies in the TODO items below.


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

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `src/kernel/drivers/ahci.c` exists with `ahci_read`, `ahci_write`, `ahci_identify`, ABAR mapping, port enumeration, and command list/FIS allocation. Check it registers as a blkdev ("sata0"). Verify `docs/architecture/storage-drivers.md` covers AHCI — update if not. Run `make clean && make all && make run`. Fix any inconsistencies in the TODO items below.


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

### 1.4 ATAPI (IDE/SATA Optical) Driver

**Prompt:** ATAPI devices appear on AHCI ports with device signature `0xEB140101` (vs `0x00000101` for disk). Reuse the existing AHCI infrastructure from `ahci.c` but send SCSI PACKET commands instead of ATA commands. Key SCSI commands: INQUIRY (0x12, device identification), READ(10) (0x28, read sectors), GET CAPACITY (0x25, disc size). ATAPI uses 2048-byte sectors (not 512). Register as a blkdev (e.g., "cdrom0") with `sector_size = 2048`. Test with `qemu -cdrom test.iso`. After completing all items, update `docs/architecture/storage-drivers.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"drivers: ATAPI optical disc driver"`.


> Required for CD/DVD/Blu-ray reading. ATAPI devices appear on AHCI ports
> with a different device signature (0xEB140101 vs 0x00000101 for disk).

- [ ] Detect ATAPI device signature on AHCI ports
- [ ] Implement SCSI INQUIRY command via AHCI ATAPI command format
- [ ] Implement SCSI READ(10) / READ(12) — read sectors from optical media
- [ ] Implement SCSI GET CAPACITY — determine disc size
- [ ] Register as blkdev (e.g., "cdrom0") with 2048-byte sector size
- [ ] QEMU flag: `-cdrom build/test-disks/optical/iso9660.iso`
- [ ] Test: `make run-test DISK=optical/iso9660`
- [ ] Commit: `"drivers: ATAPI optical disc driver"`

### 1.3 Block Device Abstraction Layer ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `struct blkdev` (name, sector_size, sector_count, read_fn, write_fn, driver_data), `blkdev_register`, `blkdev_get`, `blkdev_read`, `blkdev_write`, `blkdev_list` exist. Verify both VirtIO and AHCI are registered via adapters. Check `docs/architecture/storage-drivers.md` covers the block device layer — update if not. Run `make clean && make all && make run`. Fix any inconsistencies in the TODO items below.


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

### 2.1 MBR Partition Table ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `src/kernel/fs/mbr.c` parses MBR at LBA 0 with 4 partition entries, recognizes types 0x0C (FAT32), 0x83 (Linux), 0xDA (IXFS). Run `make clean && make all && make run`. Fix any inconsistencies in the TODO items below.


- [x] Create `src/kernel/fs/mbr.c`
- [x] Parse MBR at LBA 0: 4 partition entries at offset 446
- [x] Define `struct mbr_entry` (status, type, start_lba, sector_count)
- [x] Recognize partition types: `0x0C` (FAT32 LBA), `0x83` (Linux), `0xDA` (IXFS custom)
- [x] Return partition list (start LBA, size, type) for each active entry
- [x] Commit: `"fs: MBR partition table parsing"`

### 2.2 GPT Partition Table

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `src/kernel/fs/gpt.c` detects protective MBR type 0xEE, parses GPT header at LBA 1 (signature, CRC32), parses 128-byte partition entries, and recognizes EFI SP, Basic Data, and IXFS GUIDs. Run `make clean && make all && make run`. Fix any inconsistencies in the TODO items below.


- [x] Create `src/kernel/fs/gpt.c`
- [x] Detect GPT: check protective MBR at LBA 0 (type `0xEE`)
- [x] Parse GPT header at LBA 1: verify signature "EFI PART", validate CRC32
- [x] Parse partition entry array (128-byte entries, starting at LBA 2)
- [x] Define `struct gpt_entry` (type_guid, unique_guid, start_lba, end_lba, name)
- [x] Recognize GUIDs: EFI System Partition, Microsoft Basic Data, custom IXFS GUID
- [x] CRC32 validation of header + partition array
- [x] Commit: `"fs: GPT partition table parsing"`

### 2.3 Partition Scanner

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `src/kernel/fs/partition.c` scans each blkdev trying GPT first then MBR, creates sub-blkdevs offset by partition start LBA, auto-detects filesystem via magic bytes, and logs partitions to serial. Run `make clean && make all && make run`. Fix any inconsistencies in the TODO items below.


- [x] Create `src/kernel/fs/partition.c`
- [x] On each registered blkdev: try GPT first, fall back to MBR
- [x] For each discovered partition: create a sub-blkdev (offset reads/writes by partition start LBA)
- [x] Auto-detect filesystem on each partition (probe FAT32, ext2, IXFS magic bytes)
- [x] Log discovered partitions via serial: "Disk 0, Partition 1: FAT32, 2.0 GB"
- [x] Call at boot after disk drivers initialize
- [x] Commit: `"fs: partition scanner & auto-detect"`

---

## 3. FAT32 Filesystem

### 3.1 FAT32 Read Support

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `src/kernel/fs/fat32.c` parses BPB, calculates FAT/data region offsets, implements `fat32_read_cluster_chain`, `fat32_read_dir` (handling both 8.3 and LFN entries), `fat32_read_file`, and `fat32_stat`. Check `docs/architecture/fat32.md` exists — create or update if missing. Run `make clean && make all && make run`. Fix any inconsistencies in the TODO items below.


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

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `fat32_write_file`, `fat32_create_file`, `fat32_create_dir`, `fat32_delete_file`, `fat32_rename`, free cluster search, `fat32_format`, and dual FAT flush all exist and work. Run `make clean && make all && make run`. Fix any inconsistencies in the TODO items below.


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

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm FAT32 is registered as a VFS filesystem type with callbacks for open, close, read, write, readdir, stat, create, delete, rename. Verify it mounts to a drive letter (e.g., D:\). Test file persistence across reboots. Run `make clean && make all && make run`. Fix any inconsistencies in the TODO items below.


- [x] Register FAT32 as a VFS filesystem type
- [x] Implement VFS callbacks: open, close, read, write, readdir, stat, create, delete, rename
- [x] Mount FAT32 partition to a drive letter (e.g., `D:\`)
- [x] Test: create file on FAT32, reboot, verify file persists
- [x] Commit: `"fs: FAT32 VFS integration"`

---

### 3.5 VFS File Operations

> **Goal:** Extend the VFS layer with missing file operations required by the registry
> hive journaling system and future shell utilities. Currently, VFS supports `open`,
> `read`, `write`, `close`, and `create` — but lacks `rename`, `delete`, `stat`, and
> `truncate`. These are needed for atomic file swaps, cleanup, and general POSIX-like
> file management.

#### 3.5.1 File Rename ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `vfs_rename(old_path, new_path)` exists in `vfs.h` and `vfs.c`, dispatches to `vfs_ops.rename` callback. Confirm `ixfs_rename` is wired into `ixfs_dir_ops` and `fat32_vfs_rename` into `fat32_dir_ops`. Confirm `registry.c` `hive_save()` uses `vfs_rename(log_path, filepath)` for atomic journal swap with fallback to copy+overwrite. Run `bash scripts/build.sh clean`. Fix any inconsistencies in the TODO items below.

- [x] Add `vfs_rename(old_path, new_path)` to VFS interface
- [x] Implement FAT32 rename (update directory entry)
- [x] Handle cross-directory rename within same filesystem
- [x] Update registry journaling to use atomic rename
- [x] Commit: `"vfs: file rename"`

#### 3.5.2 File Delete ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `vfs_unlink(path)` exists in `vfs.h` and `vfs.c`, dispatches to `vfs_ops.unlink` callback. Confirm `ref_count` field in `vfs_node` is incremented on `vfs_open` and decremented on `vfs_close`. Confirm `vfs_unlink` checks `ref_count > 0` via `finddir` before deletion and returns -1 if file is open. Confirm `ixfs_unlink` checks for non-empty directories. Confirm `fat32_vfs_unlink` marks entry 0xE5 and frees cluster chain. Confirm `registry.c` calls `vfs_unlink(log_path)` to clean up journal in the fallback path. Run `bash scripts/build.sh clean`. Fix any inconsistencies in the TODO items below.

- [x] Add `vfs_delete(path)` to VFS interface
- [x] Implement FAT32 delete (mark entry 0xE5, free clusters)
- [x] Prevent deletion of open files
- [x] Update registry journaling to delete old `.hive.log` after successful save
- [x] Commit: `"vfs: file delete"`

#### 3.5.3 File Stat ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `struct vfs_stat` is defined in `vfs.h` with `size`, `type`, `ctime`, `mtime`, `atime`, `blocks`. Confirm `stat` callback exists in `vfs_ops`. Confirm `vfs_stat(path, &st)` in `vfs.c` walks the path and dispatches to `ops->stat` with a fallback to node fields. Confirm `ixfs_vfs_stat` populates from inode fields (`i_size`, `i_ctime`, `i_mtime`, `i_atime`, `i_blocks`). Confirm `fat32_vfs_stat` populates from `fat32_file` fields. Run `bash scripts/build.sh clean`. Fix any inconsistencies.

- [x] Define `vfs_stat_t` struct (size, type, timestamps)
- [x] Add `vfs_stat(path, &st)` to VFS interface
- [x] Implement FAT32 stat (read directory entry metadata)
- [x] Commit: `"vfs: file stat"`

#### 3.5.4 File Truncate

**Prompt:** Implement `vfs_truncate(const char *path, uint32_t new_size)` to resize a file. If `new_size` is 0, the file becomes empty (free all clusters). If smaller than current, free trailing clusters. This is needed to properly reset journal files and for future text editors. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"vfs: file truncate"`. Create or update documentation in `docs/` covering the truncate API.

- [ ] Add `vfs_truncate(path, new_size)` to VFS interface
- [ ] Implement FAT32 truncate (free/allocate clusters)
- [ ] Handle truncate-to-zero (free entire cluster chain)
- [ ] Commit: `"vfs: file truncate"`

---

## 4. External Filesystem Drivers
> *Read/write drivers for NTFS, ext2/ext3/ext4, and exFAT*

### 4.1 NTFS Read Support

**Prompt:** NTFS is the most complex filesystem to implement. The Master File Table (MFT) is the core structure — every file and directory is an MFT entry (1024 bytes). Each entry contains attributes: `$STANDARD_INFORMATION` (timestamps), `$FILE_NAME` (name, parent ref), `$DATA` (file contents as "data runs" — compressed offset/length pairs mapping logical clusters to physical clusters). Directories use `$INDEX_ROOT` and `$INDEX_ALLOCATION` B+ trees for name lookup. Parse data runs carefully — they use variable-length encoding with relative offsets. Probe via OEM ID "NTFS    " at boot sector bytes 3-10. After completing all items, create `docs/architecture/ntfs.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"fs: NTFS read support"`.


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
- [ ] Test: `make run-test DISK=ntfs`
- [ ] Commit: `"fs: NTFS read support"`

### 4.2 NTFS Write Support

**Prompt:** NTFS write is significantly harder than read. Writing to an existing file means finding free clusters via `$Bitmap`, extending data runs (which may require splitting/merging run entries), and updating the MFT entry. Creating a file requires allocating a new MFT entry from `$MFT`, initializing attributes, adding the filename to the parent directory's B+ tree index. Always update `$STANDARD_INFORMATION` timestamps. The `$Bitmap` tracks free clusters as a bit array. Be extremely careful with endianness and NTFS's 64-bit cluster addressing. After completing all items, update `docs/architecture/ntfs.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"fs: NTFS write support"`.


- [ ] Implement `ntfs_write_file(mft_entry, data, size)` — write to existing data runs
- [ ] Implement `ntfs_extend_data_run(mft_entry, clusters)` — allocate new clusters
- [ ] Implement `ntfs_create_file(dir_mft, name)` — allocate MFT entry, add to index
- [ ] Implement `ntfs_delete_file(dir_mft, name)` — mark MFT entry as free, free clusters
- [ ] Implement free cluster bitmap (`$Bitmap`) read/write
- [ ] Update `$STANDARD_INFORMATION` timestamps on file operations
- [ ] Commit: `"fs: NTFS write support"`

### 4.3 NTFS VFS Integration

**Prompt:** Wire the NTFS read/write functions into VFS callbacks. Register NTFS as a filesystem type. When the partition scanner detects an NTFS partition (OEM ID match), create a VFS mount point at the next available drive letter. Test by creating a small NTFS partition image with `mkfs.ntfs` on the build host, attaching it as a QEMU drive, and verifying files created on Linux are readable in the OS. After completing all items, update `docs/architecture/ntfs.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"fs: NTFS VFS integration"`.


- [ ] Register NTFS as a VFS filesystem type
- [ ] Implement VFS callbacks: open, close, read, write, readdir, finddir, stat, create, delete, rename, truncate
- [ ] Mount NTFS partition to a drive letter (e.g., `D:\`)
- [ ] Test: read files from a Windows-formatted NTFS partition
- [ ] Commit: `"fs: NTFS VFS integration"`

### 4.4 ext2/ext3/ext4 Read Support

**Prompt:** ext2/3/4 share the same on-disk layout (magic `0xEF53` at superblock offset 56). The superblock at byte offset 1024 contains block_size (1024 << s_log_block_size), inode_size, total blocks, and total inodes. Inodes are organized in block groups — each group has a block bitmap, inode bitmap, and inode table. Block pointers: 12 direct + single indirect + double indirect + triple indirect. ext4 adds extents (check `EXT4_EXTENTS_FL` flag in inode) — parse the extent tree header and entries instead of block pointers. Detect ext3 vs ext4 by feature flags (`s_feature_incompat`). After completing all items, create `docs/architecture/ext-filesystem.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"fs: ext2/ext3/ext4 read support"`.


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
- [ ] Test: `make run-test DISK=ext2` / `ext3` / `ext4`
- [ ] Commit: `"fs: ext2/ext3/ext4 read support"`

### 4.5 ext2/ext3/ext4 Write Support

**Prompt:** ext write support requires managing two bitmaps: block bitmap (tracks free blocks per group) and inode bitmap (tracks free inodes per group). Allocating a new file: find free inode from bitmap, initialize it, find free blocks for data, add directory entry to parent. The `ext2_dir_entry_2` format is a linked list of variable-length entries within directory blocks. Update the block group descriptor's free-block and free-inode counts, and the superblock's global counts. Wire all write operations into VFS callbacks. After completing all items, update `docs/architecture/ext-filesystem.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"fs: ext2/ext3/ext4 write support"`.


- [ ] Implement block bitmap read/write for allocation
- [ ] Implement inode bitmap read/write for inode allocation
- [ ] Implement `ext2_write_file(inode, offset, data, size)` — allocate blocks, write data
- [ ] Implement `ext2_create(dir_inode, name, type)` — allocate inode + dir entry
- [ ] Implement `ext2_delete(dir_inode, name)` — free blocks/inode, unlink dir entry
- [ ] Implement `ext2_rename(dir, old_name, new_name)`
- [ ] Implement `ext2_truncate(inode, new_size)` — free/allocate blocks
- [ ] Update block group descriptor free counts + superblock free counts
- [ ] Wire write ops into VFS callbacks: open, close, read, write, readdir, finddir, stat, create, delete, rename, truncate
- [ ] Commit: `"fs: ext2/ext3/ext4 write support"`

### 4.6 exFAT Read Support

**Prompt:** exFAT is simpler than NTFS but more complex than FAT32. The boot sector uses shift-based sizes (bytes_per_sector_shift, sectors_per_cluster_shift). The FAT is 32-bit entries (cluster chain, similar to FAT32). Directories use a unique entry-set format: File Directory Entry (type 0x85) + Stream Extension Entry (0xC0) + one or more File Name Entries (0xC1, each holding 15 UTF-16LE characters). Assembly of long filenames requires chaining File Name Entries. Probe via OEM name "EXFAT   " at boot sector. After completing all items, create `docs/architecture/exfat.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"fs: exFAT read support"`.


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
- [ ] Test: `make run-test DISK=exfat`
- [ ] Commit: `"fs: exFAT read support"`

### 4.7 exFAT Write Support

**Prompt:** exFAT uses an allocation bitmap instead of scanning the FAT for free clusters (more efficient). The allocation bitmap is a contiguous file whose first cluster is specified in the boot sector. Writing a new file: create a directory entry set (File + Stream + Name entries), allocate clusters from the bitmap, update the FAT chain. Deleting: mark directory entries as deleted (type & 0x80 cleared), free clusters in both FAT and bitmap. The UpCase table (a Unicode case-folding table embedded in the volume) is needed for case-insensitive filename comparisons. After completing all items, update `docs/architecture/exfat.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"fs: exFAT write support"`.


- [ ] Implement allocation bitmap read/write (replaces FAT-based free scan)
- [ ] Implement `exfat_alloc_cluster()` — scan allocation bitmap
- [ ] Implement `exfat_write_file(dir_cluster, name, data, size)` — allocate clusters, write data
- [ ] Implement `exfat_create_file(dir, name)` — create directory entry set (File + Stream + Name entries)
- [ ] Implement `exfat_create_dir(parent, name)` — create directory with dot entries
- [ ] Implement `exfat_delete_file(dir, name)` — free clusters, mark entries as deleted (0x05/0x40/0x41)
- [ ] Implement `exfat_rename(dir, old, new)` — update File Name entries
- [ ] Implement `exfat_truncate(dir, name, new_size)` — free/allocate clusters
- [ ] UpCase table validation for case-insensitive comparisons
- [ ] Wire write ops into VFS callbacks: open, close, read, write, readdir, finddir, stat, create, delete, rename, truncate
- [ ] Commit: `"fs: exFAT write support"`

### 4.8 ISO 9660 Read Support

**Prompt:** ISO 9660 is the standard CD/DVD filesystem. It uses 2048-byte sectors. The Primary Volume Descriptor (PVD) is at sector 16, identified by signature "CD001". The root directory record in the PVD gives the LBA and size of the root directory. Directory records are variable-length with a length byte, extent LBA, data length, flags (bit 1 = directory), and 8.3 filename. Files are stored as contiguous extents (no fragmentation). Rock Ridge extensions add POSIX metadata (long names, permissions, symlinks) via System Use Entries appended to each directory record. This is a read-only filesystem. Requires ATAPI driver (§1.4) for real optical media, or can read `.iso` files attached as raw block devices. After completing all items, create `docs/architecture/iso9660.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"fs: ISO 9660 read support"`.


> ISO 9660 is the standard filesystem for CD/DVD media. Read-only by design.
> Requires the ATAPI driver (§1.4) for real optical media, or raw block
> access for `.iso` files attached via QEMU `-cdrom`.

- [ ] Create `src/kernel/fs/iso9660.c` and `include/kernel/fs/iso9660.h`
- [ ] Parse Primary Volume Descriptor at sector 16 (2048-byte sectors)
- [ ] Define `struct iso9660_dir_record` (length, extent LBA, data length, flags, name)
- [ ] Implement `iso9660_read_dir(extent_lba, size)` — parse directory records
- [ ] Implement `iso9660_find(path)` — traverse directory tree from root
- [ ] Implement `iso9660_read_file(extent_lba, size, buf)` — read contiguous extent
- [ ] Handle both 8.3 names and Rock Ridge (POSIX extensions) if present
- [ ] VFS integration: register iso9660 driver (read-only), mount to drive letter
  - [ ] Implement VFS callbacks: open, close, read, readdir, finddir, stat (no write/create/delete/rename/truncate — read-only FS)
- [ ] Add ISO 9660 probe to partition scanner (check PVD signature `"CD001"`)
- [ ] Test: `make run-test DISK=optical/iso9660`
- [ ] Commit: `"fs: ISO 9660 read support"`

### 4.9 Joliet / UDF Read Support

**Prompt:** Joliet extends ISO 9660 with Unicode filenames via a Supplementary Volume Descriptor (detected by escape sequences in the SVD). Filenames are UTF-16BE encoded. UDF (Universal Disk Format) is used on DVDs and Blu-ray discs. UDF parsing starts from the Anchor Volume Descriptor Pointer at sector 256, which points to the Main Volume Descriptor Sequence. Files are located via File Identifier Descriptors and allocation descriptors (short/long/extended). Both Joliet and UDF build on the ISO 9660 infrastructure and the ATAPI driver. After completing all items, update `docs/architecture/iso9660.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"fs: Joliet + UDF read support"`.


> Joliet extends ISO 9660 with long Unicode filenames. UDF is the standard
> for DVD and Blu-ray media. Both build on top of ISO 9660 infrastructure.

- [ ] Joliet: parse Supplementary Volume Descriptor, decode UTF-16BE filenames
- [ ] UDF: parse Anchor Volume Descriptor Pointer (sector 256)
- [ ] UDF: parse Partition Descriptor, Logical Volume Descriptor
- [ ] UDF: implement `udf_read_dir()` — parse File Identifier Descriptors
- [ ] UDF: implement `udf_read_file()` — follow allocation descriptors
- [ ] VFS integration: register UDF driver (read-only), implement open, close, read, readdir, finddir, stat
- [ ] Test: `make run-test DISK=optical/joliet` / `optical/udf`
- [ ] Commit: `"fs: Joliet + UDF read support"`

---

## 5. Persistent IXFS (IXFS-on-Disk)

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
- [x] Divide volume into block groups (~128 MiB each) for locality
- [x] Track per-group free block count + "next free" hint in group descriptor
- [x] `ixfs_alloc_block()` starts from hint, wraps around on full group
- [x] Prefer allocating in same group as file's inode (reduces seek)
- [x] Update superblock `s_free_blocks` only on group exhaustion (lazy)
- [x] Commit: `"fs: IXFS block group allocator"`

#### 5.5.2 Buffer Cache / Write-Back
- [x] Implement dirty buffer cache (LRU, configurable size)
- [x] Cache recently-read blocks in memory (avoid re-reading bitmaps/inodes)
- [x] Batch dirty blocks and flush to disk periodically or on `sync()`
- [x] Mark buffers dirty on write; actual disk I/O deferred
- [x] Flush all dirty buffers on unmount
- [x] Commit: `"fs: IXFS buffer cache + write-back"`

#### 5.5.3 Directory Hash Index (B-tree)
- [x] For directories with > 64 entries: build hash index in an extra block
- [x] Use FNV-1a hash of filename → bucket → chain of entry offsets
- [x] `finddir()` goes from O(n) → O(1) average case
- [x] Falls back to linear scan for small directories (< 64 entries)
- [x] Rebuild index on create/delete
- [x] Commit: `"fs: IXFS directory hash index"`

#### 5.5.4 Multi-Volume Support
- [x] Remove static globals: wrap all state in `struct ixfs_volume`
- [x] Each mounted IXFS partition gets its own `ixfs_volume` instance
- [x] Pass volume pointer through VFS `fs_data` / `priv_data`
- [x] Support mounting multiple IXFS partitions simultaneously (C:\, E:\, etc.)
- [x] Commit: `"fs: IXFS multi-volume support"`

### 5.6 Extent-Based Allocation

> **Priority: P1** — Replaces the old block-pointer system entirely

- [x] Define `struct ixfs_extent { uint64_t start_block; uint32_t block_count; }` (12 bytes)
- [x] Replace `i_direct[12]` + `i_indirect` + `i_dindirect` with extent array in inode
- [x] Fit 4 inline extents directly in inode (replaces block pointers, same 52 bytes)
- [x] If > 4 extents needed: store extent tree in a data block (B-tree of extents)
- [x] Merge adjacent extents on allocation (contiguous writes = 1 extent)
- [x] Update `ixfs_alloc_block()` to allocate contiguous runs preferentially
- [x] Update `ixfs_read_file` / `ixfs_write_file` to use extent lookup
- [x] Max file size with 64-bit block numbers: **64 TiB** (with 4K blocks)
- [x] Commit: `"fs: IXFS extent-based allocation"`

### 5.7 Write-Ahead Log (Journal)

> **Priority: P1** — Crash safety is essential for a system partition

- [x] Reserve journal area: configurable size (default 32 MiB), stored after superblock
- [x] Journal record: `{ txn_id, block_number, old_data, new_data, checksum }`
- [x] **Metadata journaling** (default): journal only inode/bitmap/directory changes
- [x] Optional **full journaling**: journal data blocks too (slower but safer)
- [x] Transaction API: `ixfs_txn_begin()`, `ixfs_txn_write()`, `ixfs_txn_commit()`
- [x] On commit: write all records to journal → flush → write to final location → flush → mark txn complete
- [x] On mount after crash: replay incomplete transactions from journal
- [x] Circular journal with head/tail pointers in superblock
- [x] Commit: `"fs: IXFS write-ahead log (journal)"`

### 5.8 Copy-on-Write (CoW) & Snapshots

> **Priority: P2** — IXFS's signature feature, differentiating it from ext4/NTFS

- [x] **CoW block writes**: never overwrite a block in-place; always write to a new location
- [x] Update parent pointers (extent tree / inode) to point to new block
- [x] Old blocks remain valid until explicitly freed → enables snapshots
- [x] **Snapshot API**: `ixfs_snapshot_create(name)` — freeze current state by incrementing refcounts
- [x] **Refcounted blocks**: each block has a reference count (shared between snapshots)
- [x] `ixfs_free_block()` decrements refcount; only frees when refcount reaches 0
- [x] **Snapshot listing**: `ixfs_snapshot_list()` — enumerate available snapshots
- [x] **Snapshot rollback**: `ixfs_snapshot_restore(name)` — swap active tree with snapshot tree
- [x] **Snapshot delete**: `ixfs_snapshot_delete(name)` — decrement refcounts, free unreferenced blocks
- [x] Shell commands: `snapshot create <name>`, `snapshot list`, `snapshot restore <name>`
- [x] Commit: `"fs: IXFS copy-on-write + snapshots"`

### 5.9 Advanced Features

#### 5.9.1 Sparse File Support
- [x] Allow holes in files: extents with `start_block = 0` represent unallocated regions
- [x] `read()` on a hole returns zeroes without allocating blocks
- [x] `write()` into a hole allocates only the needed blocks
- [x] `ixfs_stat()` reports both logical size and actual blocks used
- [x] Commit: `"fs: IXFS sparse file support"`

#### 5.9.2 Inline Small Files
- [x] Files ≤ 48 bytes: store data directly in inode's extent/pointer area (no data block needed)
- [x] Flag in `i_extent_flags`: `IXFS_INLINE` (0x02) indicates inline data
- [x] Transparently promote to extent-based when file grows beyond 48 bytes
- [x] Significant speedup for small config files, symlinks, etc.
- [x] Commit: `"fs: IXFS inline small files"`

#### 5.9.3 Per-Block Checksums
- [x] CRC32C checksum computed on every block write, stored in checksum table
- [x] Checksum table: dedicated blocks between bitmap and inode table
- [x] Verify checksum on every block read; log corruption if mismatch
- [x] Superblock checksum field for self-verification
- [x] `ixfs_scrub()` — full-volume integrity scan (background or on-demand)
- [x] Commit: `"fs: IXFS per-block checksums"`

#### 5.9.4 64-Bit Block Addressing
- [x] Upgrade `s_total_blocks`, extent `start_block`, and inode pointers to `uint64_t`
- [x] Superblock version bump to v2
- [x] Maximum volume size: **64 TiB** (4K blocks × 2^44 blocks)
- [x] Backward-compatible: v1 volumes mountable read-only, auto-upgrade on format
- [x] Commit: `"fs: IXFS 64-bit block addressing"`

### 5.10 IXFS Unique Selling Points

> See the full feature comparison table in [ixfs-specification.md](../docs/architecture/ixfs-specification.md#feature-comparison).

### 5.11 Host-Side mkfs-ixfs Tool (Option B)

> **Priority: P3** — Needed for the installer / build-time disk images

- [x] Create `tools/mkfs-ixfs.c` — standalone host tool (runs on Linux/macOS)
- [x] Accept: output image path, volume size, label
- [x] Write IXFS superblock, bitmaps, inode table (same layout as kernel `ixfs_format()`)
- [x] `--populate <dir>` flag: recursively copy a host directory into the IXFS image
- [ ] Makefile target: `build/system-disk.img` with pre-populated IXFS from `build/sysroot/`
- [ ] Once working: remove firstboot.c/firstboot.h and the `firstboot_setup()` call from main.c
- [x] Commit: `"tools: mkfs-ixfs host formatter"`

---

## 6. Drive Letter Mounting

### 6.1 Auto-Mount System

**Prompt:** After the partition scanner discovers all partitions, assign drive letters automatically: C:\ is always the first IXFS partition (the system drive), then D:\, E:\, etc. for additional partitions in discovery order. The existing `vfs_mount()` needs to support real disk-backed partitions (not just the initrd). Store mount configuration in Registry: `HKLM\SYSTEM\Storage\Drive\{letter}\Device` and `Filesystem`. Log each mount to serial: "Mounted C:\ (IXFS, 2.0 GB) on sata0-part1". After completing all items, update `docs/architecture/vfs.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"fs: auto-mount drive letters"`.


- [ ] After partition scanning: auto-assign drive letters
  - [ ] `C:\` — first IXFS partition (system drive)
  - [ ] `D:\`, `E:\`, etc. — additional partitions in order
- [ ] Update existing `vfs_mount()` to support real disk partitions (not just initrd)
- [ ] Store mount configuration in Registry: `HKLM\SYSTEM\Storage\Drive\{letter}\Device`, `Filesystem`
- [ ] Log mounts: "Mounted C:\ (IXFS, 2.0 GB) on disk0-part1"
- [ ] Commit: `"fs: auto-mount drive letters"`

### 6.2 Manual Mount/Unmount

**Prompt:** Shell commands `mount` and `umount` give the user manual control. `mount D: /dev/disk1p1 fat32` maps a partition to a drive letter. `umount D:` flushes all pending writes (dirty buffers, journal), then removes the VFS mount point. Prevent unmounting C:\ while the system is running (return error). The `mount` command with no arguments lists all current mounts. After completing all items, update `docs/user/shell-commands.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"shell: mount/umount commands"`.


- [ ] Shell command: `mount D: /dev/disk1p1 fat32` — mount a partition
- [ ] Shell command: `umount D:` — unmount a drive
- [ ] Flush pending writes before unmount
- [ ] Prevent unmount of `C:\` while running
- [ ] Commit: `"shell: mount/umount commands"`

---

## 7. Disk Management GUI

### 7.1 Disk Management App

**Prompt:** The Disk Management app is a two-panel window. The upper panel is a table listing mounted drives: Drive letter, Total Size, Used, Free, Filesystem type. The lower panel shows a graphical representation of each physical disk: colored bars proportional to partition sizes (IXFS = blue, FAT32 = green, NTFS = orange, unallocated = gray) with labels showing drive letter, filesystem, and size. Data comes from `blkdev_list()` for physical disks, the partition scanner for partition info, and VFS `stat()` for usage stats. After completing all items, create `docs/user/disk-management.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"apps: Disk Management layout"`.


- [ ] Create `src/apps/diskmgr/diskmgr.c`
- [ ] Upper panel: table view of mounted drives (Drive, Size, Used, Free, Filesystem)
- [ ] Lower panel: graphical partition layout per physical disk
  - [ ] Colored bars proportional to partition size
  - [ ] Labels: drive letter, filesystem, size
  - [ ] Unallocated space shown as empty bar
- [ ] Read from: blkdev list + partition table + filesystem stats
- [ ] Commit: `"apps: Disk Management layout"`

### 7.2 Disk Operations

**Prompt:** Disk operations are high-risk and need confirmation dialogs. Create partition: select unallocated space, specify size and filesystem type (FAT32 or IXFS), write a new GPT/MBR entry, then format. Delete partition: remove the partition table entry (WARNING: destroys all data). Format: rewrite the filesystem structures (`fat32_format()` or `ixfs_format()`). Change drive letter: update the VFS mount point and Registry entry. View usage: show a pie chart or bar of used vs. free space. Stretch goals: partition resize (complex — requires filesystem-aware shrink/grow) and SMART status for AHCI drives. After completing all items, update `docs/user/disk-management.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Disk Management operations"`.


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

**Prompt:** The block-level disk cache sits between the filesystem drivers and the blkdev layer. It caches recently-read sectors in memory using an LRU eviction policy. Write-back mode: mark cached blocks as dirty on write, batch dirty blocks and flush to disk periodically (every 5 seconds) or on explicit `sync()`. The cache should be configurable (1-8 MB). `cache_flush()` writes all dirty blocks (called on shutdown). `cache_invalidate(dev)` clears all cached blocks for a device (called on unmount). Note: IXFS already has its own buffer cache (§5.5.2) — this is a lower-level, device-agnostic cache that benefits all filesystems. After completing all items, create `docs/architecture/disk-cache.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"fs: block-level disk cache"`.


- [ ] Implement block-level read cache (LRU, configurable size: 1–8 MB)
- [ ] Cache recently read sectors to avoid repeated disk I/O
- [ ] Write-back cache: batch writes, flush periodically or on sync
- [ ] `cache_flush()` — force write all dirty blocks (called on shutdown)
- [ ] `cache_invalidate(dev)` — clear cache for a device (on unmount)
- [ ] Commit: `"fs: block-level disk cache"`

### 8.2 Filesystem Integrity / CheckDisk

**Prompt:** CheckDisk validates filesystem consistency after crashes or corruption. For IXFS: validate superblock magic/version/CRC, verify free block bitmap matches actual block references, check inode reference counts against directory entries, detect orphan inodes, verify CRC32C checksums, validate journal state, and check extent tree consistency. For FAT32: validate BPB, compare FAT copies, check chain consistency (no cross-links or loops), detect lost clusters. The CLI `chkdsk C:` runs all checks; `/fix` auto-repairs issues; `/scan` is read-only. Auto-run on mount if the filesystem's "dirty" flag is set. The GUI version wraps the same logic with a progress bar, drive selector, and results panel. After completing all items, create `docs/user/chkdsk.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"tools: chkdsk command"` and `"apps: CheckDisk GUI"`.


> **Priority: P2** — Critical for data integrity after crashes

#### CLI: `chkdsk`
- [ ] Shell command: `chkdsk C:` — run filesystem check on a volume
- [ ] `chkdsk C: /fix` — auto-repair detected issues
- [ ] `chkdsk C: /scan` — scan-only mode (no modifications)
- [ ] IXFS checks:
  - [ ] Validate superblock magic, version, and self-checksum
  - [ ] Verify free block bitmap consistency (allocated vs. referenced)
  - [ ] Check inode reference counts against directory entries
  - [ ] Detect orphan inodes (allocated but unreferenced)
  - [ ] Verify checksum table integrity (CRC32C mismatches)
  - [ ] Validate journal state (replay or discard incomplete txn)
  - [ ] Check extent tree consistency (overlapping, out-of-bounds)
- [ ] FAT32 checks:
  - [ ] Validate BPB fields and FAT copies
  - [ ] Check FAT chain consistency (no cross-links, no loops)
  - [ ] Detect lost clusters (allocated but not in any chain)
- [ ] Auto-check on mount if "dirty" flag is set (unclean shutdown)
- [ ] Commit: `"tools: chkdsk command"`

#### GUI: CheckDisk Utility
- [ ] Create `src/apps/chkdsk/chkdsk_gui.c`
- [ ] Drive selector dropdown (list mounted volumes)
- [ ] Options: Scan Only / Scan & Fix / Surface Scan
- [ ] Real-time progress bar with phase descriptions
- [ ] Results panel: errors found, fixed, remaining
- [ ] Log output scrollable text area
- [ ] "Schedule on next boot" option for system drive
- [ ] Commit: `"apps: CheckDisk GUI"`

### 8.3 Partition Manager

**Prompt:** The partition manager provides both CLI and GUI interfaces for managing disk partition tables. The CLI `diskpart` operates interactively: `list disks` shows physical disks, `list parts <disk>` shows partitions, `create` writes a new GPT/MBR entry and formats, `delete` removes an entry, `format` reinitializes a filesystem, `assign` changes drive letters. Reads/writes GPT and MBR structures from §2.1/§2.2. The GUI version shows a graphical bar per disk with color-coded partition segments. Right-click context menus for create, delete, format, change letter, properties. All destructive operations require confirmation dialogs. After completing all items, create `docs/user/partition-manager.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"tools: diskpart command"` and `"apps: Partition Manager GUI"`.


> **Priority: P2** — Essential for disk management

#### CLI: `diskpart`
- [ ] Shell command: `diskpart` — interactive partition manager
- [ ] `diskpart list disks` — list all physical disks with sizes
- [ ] `diskpart list parts <disk>` — list partitions on a disk
- [ ] `diskpart create <disk> <size_mb> <type>` — create partition (FAT32/IXFS)
- [ ] `diskpart delete <disk> <part_num>` — delete a partition
- [ ] `diskpart format <drive> <fs_type> [label]` — format a partition
- [ ] `diskpart assign <part> <letter>` — assign drive letter
- [ ] `diskpart active <disk> <part>` — set active/boot partition
- [ ] `diskpart info <drive>` — show detailed partition/volume info
- [ ] Read/write GPT and MBR partition tables
- [ ] Safety: confirm before destructive operations
- [ ] Commit: `"tools: diskpart command"`

#### GUI: Partition Manager
- [ ] Create `src/apps/partmgr/partmgr.c`
- [ ] Upper panel: list view of all disks and partitions (tabular)
- [ ] Lower panel: graphical disk bar with proportional partition segments
  - [ ] Color-coded by filesystem type (IXFS=blue, FAT32=green, NTFS=orange, unalloc=gray)
  - [ ] Labels: drive letter, filesystem, size, % used
- [ ] Right-click context menu: Create, Delete, Format, Change Letter, Properties
- [ ] Create Partition dialog: size slider, filesystem picker, label
- [ ] Format dialog: filesystem type, quick vs. full, label
- [ ] Warning dialogs for all destructive operations
- [ ] Disk properties panel: model, serial, total size, SMART status
- [ ] Commit: `"apps: Partition Manager GUI"`

### 8.4 Defragmentation & TRIM

**Prompt:** The defrag tool analyzes IXFS volumes for fragmentation (files with multiple non-contiguous extents). `defrag C: /analyze` reports the fragmentation percentage without modifying anything. `defrag C:` relocates blocks to consolidate extents — use the IXFS journal for crash safety during block relocation, skip metadata blocks, and prioritize large files. TRIM support sends ATA TRIM commands to SSDs for free block ranges (requires AHCI driver support for DATA SET MANAGEMENT command). The GUI shows a visual block map (grid of colored squares: used=blue, free=gray, fragmented=red) with real-time updates during defrag. After completing all items, create `docs/user/defrag.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"tools: defrag/trim command"` and `"apps: Disk Defragmenter GUI"`.


> **Priority: P3** — Performance optimization for fragmented volumes

#### CLI: `defrag`
- [ ] Shell command: `defrag C:` — defragment an IXFS volume
- [ ] `defrag C: /analyze` — report fragmentation level without modifying
- [ ] `defrag C: /trim` — send TRIM commands to SSD (AHCI/NVMe)
- [ ] `defrag C: /optimize` — auto-choose defrag (HDD) or TRIM (SSD)
- [ ] Fragmentation analysis: scan all inodes, count non-contiguous extents
- [ ] Defrag engine: relocate blocks to consolidate extents
  - [ ] Use journal for crash safety during block relocation
  - [ ] Skip metadata blocks and system files
  - [ ] Priority: large files first (biggest fragmentation impact)
- [ ] TRIM support: iterate free-block bitmap, send TRIM for free ranges
- [ ] Progress reporting: % complete, files processed, extents consolidated
- [ ] Commit: `"tools: defrag/trim command"`

#### GUI: Disk Defragmenter
- [ ] Create `src/apps/defrag/defrag_gui.c`
- [ ] Volume selector with fragmentation percentage per drive
- [ ] Visual block map: grid showing used/free/fragmented blocks (color-coded)
- [ ] Analyze button: populate fragmentation report without modifying
- [ ] Defragment / Optimize button: run with real-time block map updates
- [ ] Progress bar with ETA
- [ ] Results: fragments before/after, time elapsed
- [ ] Schedule option: auto-defrag weekly/monthly
- [ ] Commit: `"apps: Disk Defragmenter GUI"`

### 8.5 File & Data Recovery

**Prompt:** Recovery scans for deleted files that haven't been overwritten. For IXFS: scan the inode table for inodes with `i_links == 0` whose data blocks are still unallocated (block bitmap shows them free). Recover filenames from directory entry scans (entries with zeroed inode pointers). Assign a confidence score: high (all blocks intact), medium (some blocks overwritten), low (mostly gone). For FAT32: scan for directory entries with the 0xE5 deletion marker, follow the FAT chain if the first cluster is still intact. Data carving: scan raw blocks for magic bytes (JPEG: FFD8FF, PNG: 89504E47, PDF: 25504D46, ZIP: 504B0304). The GUI provides a wizard: select drive → scan → results table → select files → choose destination. After completing all items, create `docs/user/recovery.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"tools: recover command"` and `"apps: Recovery Wizard GUI"`.


> **Priority: P3** — Essential safety net for accidental deletion

#### CLI: `recover`
- [ ] Shell command: `recover C:` — scan for recoverable deleted files
- [ ] `recover C: /list` — list recoverable files with sizes and confidence
- [ ] `recover C: /restore <filename> <dest>` — restore a specific file
- [ ] `recover C: /all <dest_dir>` — restore all recoverable files
- [ ] IXFS recovery:
  - [ ] Scan inode table for deleted inodes (i_links == 0, data intact)
  - [ ] Check if data blocks are still unallocated (not overwritten)
  - [ ] Recover filename from directory entry scan (d_inode == 0 entries)
  - [ ] Confidence score: high (blocks untouched), medium (partial), low (reused)
- [ ] FAT32 recovery:
  - [ ] Scan for directory entries with 0xE5 (deleted) marker
  - [ ] Follow FAT chain if first cluster still intact
- [ ] Data carving: scan raw blocks for file signatures (JPEG, PNG, PDF, ZIP, etc.)
- [ ] Commit: `"tools: recover command"`

#### GUI: Recovery Wizard
- [ ] Create `src/apps/recover/recover_gui.c`
- [ ] Step 1: Select drive to scan
- [ ] Step 2: Scanning progress with file count
- [ ] Step 3: Results table — filename, size, type, confidence (High/Med/Low)
- [ ] Step 4: Select files to recover → choose destination
- [ ] Preview panel for text/image files before recovery
- [ ] Filter by file type, date range, size range
- [ ] Commit: `"apps: Recovery Wizard GUI"`

### 8.6 System File Checker

**Prompt:** SFC validates OS integrity by comparing installed system files against a known-good manifest. The manifest `C:\Impossible\System\manifest.dat` is generated at build time: for each system file, store the relative path, CRC32C checksum, size, and version. `sfc /scannow` iterates the manifest, computes CRC32C of each file on disk, and reports/repairs mismatches. Use the CRC32C function from IXFS checksums (§5.9.3). Repair copies correct files from a recovery source (initrd or backup). The GUI wraps this with a "Scan Now" button, progress bar, and results table showing verified/corrupted/repaired files. After completing all items, create `docs/user/sfc.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"tools: sfc command"` and `"apps: System File Checker GUI"`.


> **Priority: P2** — Validates OS integrity

#### CLI: `sfc`
- [ ] Shell command: `sfc /scannow` — verify all system files
- [ ] `sfc /verifyonly` — scan without repairing
- [ ] `sfc /scanfile <path>` — check a specific file
- [ ] Maintain `C:\Impossible\System\manifest.dat` — hash table of known-good system files
  - [ ] Each entry: relative path, expected CRC32C, expected size, version
  - [ ] Generated at build time by Makefile from system binaries
- [ ] Scan: iterate manifest, compute CRC32C of each file, compare
- [ ] Repair: restore corrupted files from C:\ / recovery image
- [ ] Report: files scanned, verified, corrupted, repaired
- [ ] Commit: `"tools: sfc command"`

#### GUI: System File Checker
- [ ] Create `src/apps/sfc/sfc_gui.c`
- [ ] "Scan Now" button — runs full system file verification
- [ ] Progress bar with current file being checked
- [ ] Results: list of verified/corrupted/repaired files
- [ ] Detail view: click a file to see expected vs. actual hash
- [ ] "Repair" button for corrupted files (requires recovery source)
- [ ] Log export: save results to `C:\Temp\sfc_log.txt`
- [ ] Commit: `"apps: System File Checker GUI"`

### 8.7 Disk Benchmark

**Prompt:** The disk benchmark measures I/O performance. Sequential tests: read/write 1 MiB blocks for 32 MiB total, calculate throughput (MB/s). Random tests: 4 KiB blocks at random LBAs for 1000 operations, calculate IOPS and latency (avg/min/max). Use `blkdev_read/write` directly to bypass filesystem overhead. Measure time with PIT ticks or TSC. The CLI `diskbench C:` runs all tests and prints results. The GUI shows real-time bar charts filling in as each test completes, with a results table and history for comparison. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"tools: diskbench command"` and `"apps: Disk Benchmark GUI"`.


> **Priority: P3** — Performance testing and diagnostics

#### CLI: `diskbench`
- [ ] Shell command: `diskbench C:` — run sequential + random I/O benchmark
- [ ] Sequential read/write: 1 MiB block, 32 MiB total
- [ ] Random read/write: 4 KiB block, 1000 operations
- [ ] Report: throughput (MB/s), IOPS, latency (avg/min/max)
- [ ] Commit: `"tools: diskbench command"`

#### GUI: Disk Benchmark
- [ ] Create `src/apps/diskbench/diskbench_gui.c`
- [ ] Drive selector + Start button
- [ ] Real-time bar chart: seq read, seq write, rand read, rand write
- [ ] Results table: MB/s, IOPS, latency per test
- [ ] History: save past results for comparison
- [ ] Commit: `"apps: Disk Benchmark GUI"`

### 8.8 Disk Wipe / Secure Erase

**Prompt:** Disk wipe overwrites data for privacy. Quick wipe: zero all free space (write 0x00 to unallocated blocks). Full wipe: overwrite entire partition with zeros. DoD 5220.22-M: 3-pass pattern (zeros, ones, random). Secure erase: send ATA SECURITY ERASE command via AHCI (for SSDs). Only allow wiping unmounted or non-system drives. Mandatory confirmation prompt showing the volume label and size. The GUI version adds a wipe method picker, big warning dialog, progress bar with ETA, and completion report. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"tools: diskwipe command"` and `"apps: Disk Wipe GUI"`.


> **Priority: P4** — Privacy and drive decommissioning

#### CLI: `diskwipe`
- [ ] Shell command: `diskwipe <drive> /quick` — zero all free space
- [ ] `diskwipe <drive> /full` — overwrite entire partition (1-pass zeros)
- [ ] `diskwipe <drive> /dod` — 3-pass DoD 5220.22-M wipe
- [ ] `diskwipe <drive> /trim` — secure erase via TRIM (SSD only)
- [ ] Mandatory confirmation prompt with volume label + size
- [ ] Progress bar with % complete and throughput
- [ ] Commit: `"tools: diskwipe command"`

#### GUI: Disk Wipe Utility
- [ ] Create `src/apps/diskwipe/diskwipe_gui.c`
- [ ] Drive selector (only unmounted or non-system drives)
- [ ] Wipe method picker: Quick / Full / DoD / Secure Erase
- [ ] Warning dialog with drive info and point-of-no-return confirmation
- [ ] Progress with ETA
- [ ] Completion certificate: report of wipe method, time, verification
- [ ] Commit: `"apps: Disk Wipe GUI"`

### 8.9 Disk Usage Analyzer

**Prompt:** The disk usage analyzer recursively walks directory trees and calculates space consumption. The CLI `diskuse C:` shows top-level directory sizes. `diskuse C:\Users /depth 3` recurses to 3 levels. `diskuse C: /top 20` shows the 20 largest files. Output is a tree with size, percentage of parent, and an ASCII bar. The GUI shows a treemap visualization (rectangles proportional to size, colored by file type), a expandable directory tree, and a "Largest Files" tab. Right-click options: Open location, Delete, Properties. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"tools: diskuse command"` and `"apps: Disk Usage Analyzer GUI"`.


> **Priority: P3** — Visual space consumption analysis

#### CLI: `diskuse`
- [ ] Shell command: `diskuse C:` — show top-level directory sizes
- [ ] `diskuse C:\Users /depth 3` — recursive to N levels
- [ ] `diskuse C: /top 20` — show 20 largest files
- [ ] Output: tree with size, percentage, bar chart in terminal
- [ ] Commit: `"tools: diskuse command"`

#### GUI: Disk Usage Analyzer
- [ ] Create `src/apps/diskuse/diskuse_gui.c`
- [ ] Treemap visualization: rectangles sized by space consumption
- [ ] Hierarchical directory list with expandable nodes
- [ ] Color by file type (documents, images, executables, etc.)
- [ ] "Largest Files" tab: sorted list of biggest space consumers
- [ ] Right-click: Open location, Delete, Properties
- [ ] Commit: `"apps: Disk Usage Analyzer GUI"`

### 8.10 Volume Shadow Copy / Backup

**Prompt:** This builds on the existing IXFS snapshot infrastructure (§5.8). The CLI `snapshot` commands wrap `ixfs_snapshot_create/list/restore/delete`. `snapshot diff` compares two snapshots to show files added/modified/deleted (walk both extent trees, compare inodes). The GUI shows a timeline of snapshots sorted chronologically, with Create/Restore/Delete buttons and confirmation dialogs. A diff viewer shows changed files. Auto-snapshot scheduling (daily/weekly) stores the schedule in Registry `HKLM\SYSTEM\Storage\SnapshotSchedule`. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"tools: snapshot command"` and `"apps: Snapshot Manager GUI"`.


> **Priority: P4** — Leverages existing IXFS snapshot infrastructure

#### CLI: `snapshot`
- [ ] Shell command: `snapshot create C: "backup_name"` — create named snapshot
- [ ] `snapshot list C:` — list all snapshots with dates and sizes
- [ ] `snapshot restore C: "backup_name"` — restore from snapshot
- [ ] `snapshot delete C: "backup_name"` — remove a snapshot
- [ ] `snapshot diff C: "snap_name"` — show files changed since snapshot
- [ ] Built on existing `ixfs_snapshot_create/list/restore/delete` APIs
- [ ] Commit: `"tools: snapshot command"`

#### GUI: Snapshot Manager
- [ ] Create `src/apps/snapshot/snapshot_gui.c`
- [ ] Timeline view: snapshots shown chronologically
- [ ] Create / Restore / Delete buttons with confirmation
- [ ] Diff viewer: files added/modified/deleted since a snapshot
- [ ] Auto-snapshot: schedule daily/weekly system snapshots
- [ ] Commit: `"apps: Snapshot Manager GUI"`

### 8.11 NVMe Driver (Future)

**Prompt:** NVMe is the modern SSD interface. Detect via PCI class 0x01, subclass 0x08. Map BAR0 for the NVMe registers. Initialize admin submission and completion queues, then create I/O queue pairs. Use the Identify command to discover namespaces and capacity. Read/Write commands use Physical Region Pages (PRP) for scatter-gather DMA. This is a stretch goal — prioritize AHCI first since it covers all current testing scenarios. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"drivers: NVMe SSD driver"`.


- [ ] *(Stretch)* Create `src/kernel/drivers/nvme.c`
- [ ] *(Stretch)* Detect NVMe controller via PCI (class `0x01`, subclass `0x08`)
- [ ] *(Stretch)* Map BAR0, initialize admin + I/O submission/completion queues
- [ ] *(Stretch)* Implement read/write via NVMe commands (Identify, Read, Write)
- [ ] *(Stretch)* QEMU flag: `-drive file=disk.img,format=raw,if=none,id=d0 -device nvme,drive=d0,serial=1234`

### 8.12 USB Mass Storage (Future)

**Prompt:** USB mass storage uses the Bulk-Only Transport (BOT) protocol over USB bulk endpoints. SCSI commands (INQUIRY, READ10, WRITE10, READ CAPACITY) are wrapped in Command Block Wrappers (CBW). This requires a USB host controller driver (xHCI or EHCI) as a prerequisite. Hot-plug detection: when a new USB device is attached, show a notification toast and auto-mount. Safe removal: flush all dirty buffers, unmount, then notify the user. This is a long-term stretch goal. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"drivers: USB mass storage"`.


- [ ] *(Stretch)* USB mass storage class driver (bulk-only transport)
- [ ] *(Stretch)* SCSI command layer (INQUIRY, READ10, WRITE10)
- [ ] *(Stretch)* Hot-plug notification: show toast "USB drive detected", auto-mount
- [ ] *(Stretch)* Safe removal: flush + unmount + notification

### 8.13 Disk I/O Metrics

**Prompt:** Track per-device I/O statistics in the blkdev layer: `bytes_read`, `bytes_written`, `read_ops`, `write_ops`, accumulated since boot. Expose via `blkdev_stats(dev)` returning a stats struct. The Task Manager's Performance tab (Phase 05 §8.1) reads these for disk activity graphs. The `iostat` shell command prints a table: Device, Reads/s, Writes/s, Read MB/s, Write MB/s. Update counters atomically in `blkdev_read/write` dispatch functions. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"drivers: disk I/O metrics"`.


- [ ] Track read/write byte counts per block device
- [ ] Track I/O operations per second
- [ ] Expose via `blkdev_stats(dev)` — used by Task Manager performance tab
- [ ] Shell command: `iostat` — show disk I/O statistics
- [ ] Commit: `"drivers: disk I/O metrics"`

### 8.14 IXFS Directory Structure (First Boot)

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm the Makefile sysroot target creates the standard directory tree (C:\Impossible\System\, C:\Impossible\Bin\, C:\Impossible\Fonts\, C:\Users\Default\, C:\Temp\, C:\Programs\, etc.) and that all paths exist after boot. Run `make clean && make all && make run` and list directories in the shell. Fix any inconsistencies in the TODO items below.


- [x] On first boot (fresh IXFS format), create standard directory tree:
  - [x] `C:\Impossible\` — system root
  - [x] `C:\Impossible\System\` — system files
  - [x] `C:\Impossible\System\Config\Registry\` — Registry hive files
  - [x] `C:\Impossible\Bin\` — system executables
  - [x] `C:\Impossible\Fonts\` — system fonts
  - [x] `C:\Impossible\Icons\` — system icons
  - [x] `C:\Impossible\Wallpapers\` — wallpapers
  - [x] `C:\Users\Default\` — default user home
  - [x] `C:\Users\Default\Desktop\` — desktop shortcuts
  - [x] `C:\Users\Default\Documents\`
  - [x] `C:\Users\Default\Downloads\`
  - [x] `C:\Users\Default\Pictures\`
  - [x] `C:\Temp\` — temp files
  - [x] `C:\Recycle\` — recycle bin
  - [x] `C:\Programs\` — installed applications
- [x] ~~Copy initrd contents into IXFS directories~~ Created at build time via Makefile sysroot target
- [x] Commit: `"fs: IXFS first-boot directory structure"`

---

## Priority Order

| Priority | Section | Reason |
|----------|---------|--------|
| ✅ Done | 1.1 VirtIO-blk Driver | Modern VirtIO 1.0 MMIO — implemented and tested |
| ✅ Done | 1.3 Block Device Layer | Unified interface over VirtIO/AHCI |
| ✅ Done | 2. Partition Tables | MBR + GPT + partition scanner |
| 🔴 P0 | 3.1 FAT32 Read | Read USB drives, boot media |
| 🟠 P1 | 3.5 VFS File Operations | rename, delete, stat, truncate — needed by registry + shell |
| 🟠 P1 | 4.1 NTFS Read | Read Windows-formatted partitions |
| 🟠 P1 | 5.1–5.4 IXFS on Disk | Persistence — files survive reboot |
| 🟠 P1 | 6.1 Auto-Mount | Drive letters from real disks |
| 🟠 P1 | 3.2 FAT32 Write | Full read/write for removable media |
| 🟠 P1 | 8.14 IXFS Directory Structure | Standard paths on first boot |
| ✅ Done | 1.2 AHCI Driver | SATA HDD/SSD — implemented and tested |
| 🟡 P2 | 4.2 NTFS Write | Write to Windows partitions |
| 🟡 P2 | 8.1 Disk Cache | Performance — reduce disk I/O |
| 🟡 P2 | 6.2 Mount/Unmount Commands | Manual storage management |
| 🟡 P2 | 7. Disk Management GUI | Visual partition management |
| 🟡 P2 | 8.2 CheckDisk (CLI + GUI) | Filesystem integrity after crashes |
| 🟡 P2 | 8.3 Partition Manager (CLI + GUI) | Disk partitioning |
| 🟡 P2 | 8.6 System File Checker (CLI + GUI) | OS integrity validation |
| 🟢 P3 | 3.3 FAT32 VFS + Format | Complete FAT32 integration |
| 🟢 P3 | 4.3 NTFS VFS | Complete NTFS integration |
| 🟢 P3 | 8.4 Defrag/TRIM (CLI + GUI) | Performance optimization |
| 🟢 P3 | 8.5 File/Data Recovery (CLI + GUI) | Accidental deletion safety net |
| 🟢 P3 | 8.7 Disk Benchmark (CLI + GUI) | Performance testing |
| 🟢 P3 | 8.9 Disk Usage Analyzer (CLI + GUI) | Space consumption analysis |
| 🟢 P3 | 8.13 Disk I/O Metrics | Performance monitoring |
| 🔵 P4 | 8.8 Disk Wipe (CLI + GUI) | Secure erase / privacy |
| 🔵 P4 | 8.10 Snapshot Manager (CLI + GUI) | Volume shadow copy / backup |
| 🔵 P4 | 8.11 NVMe Driver | Modern SSD support (future) |
| 🔵 P4 | 8.12 USB Mass Storage | Hot-plug USB drives (future) |
| 🟢 P3 | 1.4 ATAPI Driver | Optical disc reading |
| 🟢 P3 | 4.8 ISO 9660 Read | CD/DVD filesystem |
| 🟢 P3 | 4.9 Joliet/UDF Read | DVD/Blu-ray extensions |
