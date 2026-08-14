---
schema_version: 1
id: ixfs-core-win32-compat
domain: 05-storage-filesystems
status: active
title: "TODO-06 -- IXFS Core Foundation & Win32 Compatibility"
---

# TODO-06 -- IXFS Core Foundation & Win32 Compatibility

> **Goal:** First verify every IXFS subsystem is correct against its spec (and fix two known doc bugs), then upgrade inodes from 128-byte v2 to 256-byte v3, and finally implement the complete Win32 compatibility layer -- case-insensitive paths, ADS, security descriptors, hard links, symlinks, EAs, object IDs, and a v3 format tool. **This is the P0 blocker for all Win32 apps that touch the filesystem.**

> [!IMPORTANT]
> IXFS is on v2 inodes (128 bytes, 32 per block). Two known doc bugs in `ixfs.h`: (1) line ~14 stale comment "indirect block pointers" -- IXFS uses extent trees, not indirect blocks; (2) line ~163 dir-entry comment "64 bytes each" -- actual struct is `d_inode(4) + d_name[252] = 256 bytes, 16 per block`. Fix both in §1 before any structural work. §2 (v3 inode) is a **breaking on-disk format change** -- bump `IXFS_VERSION → 3`, update `IXFS_INODES_PER_BLOCK` from 32 to 16, and make v2 volumes mount read-only. All subsequent sections (§3–§10) depend on v3 inodes being in place. Do **not** delete `todo-old/010-Kernel-Foundations/TODO-040-Filesystem/TODO-040.11-IXFS.md` yet -- TODO-07 still consumes its remaining sections.

## Inputs

- `include/kernel/fs/ixfs.h` -- `struct ixfs_inode` (128B v2); `struct ixfs_dir_entry` (actually 256B but comment says 64B); `IXFS_VERSION=2`; snapshot/scrub API stubs; §1–2 extend it
- `src/kernel/fs/ixfs/ixfs_internal.h` -- `ixfs_strcmp()` declared (line 121); §3 adds `ixfs_name_cmp()`
- `src/kernel/fs/ixfs/ixfs_core.c`, `ixfs_alloc.c`, `ixfs_inode.c`, `ixfs_extent.c`, `ixfs_journal.c`, `ixfs_cow.c`, `ixfs_format.c`, `ixfs_ops.c`, `ixfs_test.c` -- working v2 implementation; §1 verifies each; §2–10 extend them
- → XREF: `05-storage-filesystems/TODO-05-win32-file-io-api.md §2` -- `FILE_OBJECT`/`HANDLE` table must exist before §3 (ADS `CreateFile` path) and §9 (`OpenFileById`) can wire up
- → XREF: `05-storage-filesystems/TODO-04-fat32-hardening-vfs-semantics.md §11` -- Win32 feature stubs that return empty ADS, default ACL are superseded here by real IXFS implementations
- → XREF: `10-platform-services` Object Manager + Security TODO -- §5 security descriptors wire into `SeAccessCheck`; coordinate on `SECURITY_DESCRIPTOR` binary format and well-known SID constants
- Legacy reference (not an XREF target): `todo-old/010-Kernel-Foundations/TODO-040-Filesystem/TODO-040.11-IXFS.md` -- **do not delete**; advanced storage features (compression, encryption, dedup, tiering) remain there for TODO-07

## Outcome

- All eight IXFS subsystems verified correct; two header doc bugs fixed.
- v3 256-byte inodes on disk; v2 volumes mount read-only; `IXFS_INODES_PER_BLOCK = 16`; `IXFS_VERSION = 3`.
- Case-insensitive path resolution via `ixfs_name_cmp()` in all directory lookups.
- ADS via `i_ads_first` inode chain; `filename:streamname` path syntax; `FindFirstStreamW` enumeration.
- Security descriptors: hash-deduped table in reserved inodes; `GetFileSecurity`/`SetFileSecurity`; parent DACL inheritance.
- Hard links: `CreateHardLink`, `i_links` refcount, cascade-free on last unlink.
- Symlinks: `IXFS_S_SYMLINK` inode type; path resolution with max-8-hop loop guard; `CreateSymbolicLink`.
- Extended attributes: EA block referenced by `i_ea_block`; `NtQueryEaFile`/`NtSetEaFile`; WSL interop EAs.
- Object IDs: `i_file_id` 16-byte UUID; `FSCTL_CREATE_OR_GET_OBJECT_ID`; `OpenFileById`; persists across rename.
- v3 format tool: 256-byte inodes, security descriptor table, USN journal region, quota table region.
- Full `ixfs_test.c` suite passes; `bash scripts/build.sh clean run` succeeds.

## Implementation Order

| ⭐  | Order | Deliverable                                                                            | Depends On                                                     | Status |
| --- | :---: | -------------------------------------------------------------------------------------- | -------------------------------------------------------------- | :----: |
| 💎  |   1   | §1 Subsystem verification -- 8 passes, fix header doc bugs, each committed separately   | Working v2 foundation                                          |  [ ]   |
| ⭐  |   2   | §2 Extended inode v3 -- 256B, 10 new fields, v2 read-only mount, `IXFS_VERSION=3`      | §1 (subsystems verified before on-disk format change)          |  [ ]   |
| ⭐  |   3   | §3 Case-insensitive paths -- `ixfs_name_cmp()`, FNV1a fold-to-lower, per-mount override | §2 (v3 inode; no new inode fields needed but must be stable)  |  [ ]   |
| 💎  |   4   | §4 Alternate Data Streams -- `i_ads_first` chain, `filename:stream` syntax, `FindFirstStreamW` | §2 (`i_ads_first` in v3 inode)                          |  [ ]   |
| 💎  |   5   | §5 Security descriptors -- hash-deduped table, `i_security_id`, `GetFileSecurity`, DACL inherit | §2 (`i_security_id` in v3 inode)                       |  [ ]   |
| 💎  |   6   | §6 Hard links -- `CreateHardLink`, `i_links`, cascade-free on last unlink               | §2 (`i_links` already in v2; §6 wires Win32 API)              |  [ ]   |
| 💎  |   7   | §7 Symbolic links + reparse points -- `IXFS_S_SYMLINK`, path follow, `CreateSymbolicLink` | §2 (new inode type in `i_mode`)                             |  [ ]   |
| 💎  |   8   | §8 Extended attributes -- EA block, `NtQueryEaFile`/`NtSetEaFile`, WSL interop EAs     | §2 (`i_ea_block` in v3 inode)                                  |  [ ]   |
| ⭐  |   9   | §9 Object IDs -- `i_file_id` UUID, `FSCTL_CREATE_OR_GET_OBJECT_ID`, `OpenFileById`     | §2 (`i_file_id` in v3 inode)                                   |  [ ]   |
| ⭐  |  10   | §10 Format tool v3 -- `ixfs_format()` v3: 256B inodes, SD table, USN region, quota region | §2–9 all stable                                             |  [ ]   |
| 💎  |  11   | §11 Parity gate -- `ixfs_test.c` full suite, `bash scripts/build.sh clean run` passes  | §1–10 all complete                                             |  [ ]   |

> §2 (v3 inode), §3 (case-insensitive paths in-filesystem), §9 (object IDs), and §10 (v3 format tool) are `⭐` exclusive: no Linux native filesystem offers an in-kernel, casefold-at-lookup, Win32-compatible ADS+SD+EA+ObjectID-aware filesystem that is also the system volume. NTFS on Windows comes closest, but is closed-source and not redesignable. IXFS v3 is purpose-built for the Win32 semantic surface.

---

## 1. Subsystem Verification `[Sonnet]`

Eight independent verification passes -- one per subsystem. Fix known header doc bugs first. Each pass ends with a dedicated commit.

**Files:** `include/kernel/fs/ixfs.h` (fix doc bugs), then all `src/kernel/fs/ixfs/*.c` files audited per pass.

> [!NOTE]
> Two confirmed doc bugs to fix before any other changes: (1) `ixfs.h` line ~14 comment says "indirect block pointers" -- IXFS uses extent trees, not indirect blocks; correct to "extent tree". (2) `ixfs.h` line ~163 comment says "64 bytes each" -- actual struct is `d_inode(4) + d_name[252] = 256 bytes`, 16 per block; correct both the comment and `IXFS_DIRENTS_PER_BLOCK` if it resolves incorrectly. Run `bash scripts/build.sh clean` after each fix.

- [ ] **Doc fixes**: fix comment at line ~14 ("indirect block pointers" → "extent tree"); fix comment at line ~163 ("64 bytes each" → "256 bytes each, 16 per block"); verify `IXFS_DIRENTS_PER_BLOCK = 4096 / 256 = 16`; commit `"ixfs: fix stale header comments -- extent tree, dir-entry 256 bytes"`
- [ ] **Superblock**: verify `s_magic == IXFS_MAGIC (0x49584653)`, `s_version == 2`, CRC32C field, layout fields (`s_inode_table_block`, `s_bitmap_block`, `s_root_inode`); commit `"ixfs: verify superblock -- magic, version, CRC32C, layout"`
- [ ] **Block allocator**: bitmap ops (set/clear/find-free), group hints, locality-aware alloc, flush on unmount; commit `"ixfs: verify block allocator -- bitmap, group hints, locality, flush"`
- [ ] **Inode table**: 128B packed, 4 inline extents, overflow block, timestamps, `i_links`; clarify `i_ctime` is *creation* time (not POSIX "change" time) in comment; commit `"ixfs: verify inode table -- 128B, extents, timestamps, i_ctime=creation"`
- [ ] **Extent engine**: block mapping, overflow extents, inline data ≤48 B, extent merging; commit `"ixfs: verify extent engine -- block map, overflow, inline data, merge"`
- [ ] **WAL journal**: header, txn API (`ixfs_journal_begin`/`commit`/`abort`), wraparound, crash recovery replay; commit `"ixfs: verify WAL journal -- txn API, wraparound, crash recovery"`
- [ ] **CoW + snapshots**: per-block refcount table (`ixfs_refcount_init`), `ixfs_cow_block`, snapshot create/restore/delete, max 8 snapshots; commit `"ixfs: verify CoW/snapshot -- refcount, cow_block, 8-snapshot limit"`
- [ ] **Checksums**: per-block CRC32C, `ixfs_scrub()` full-volume scan, mismatch handling (log + mark block bad); commit `"ixfs: verify per-block CRC32C and ixfs_scrub()"`
    - **Known mismatch bug (2026-04-18, diagnose-serial-log):** persistent checksum failure on data-region blocks 131 (x7 repeats) and 2307 (x2 repeats) during desktop asset read (C:\Impossible\Web\Wallpaper\default.jpg + fonts + icons). Pattern: same block number reports mismatch on each read; boot continues and assets load visibly. Investigate: (1) `ixfs_crc32c()` in `src/kernel/fs/ixfs/ixfs_core.c` vs `crc32c()` in `tools/mkfs-ixfs.c:139` -- diff polynomial/byte-order; (2) mkfs-ixfs.c:570-574 checksum-write loop runs AFTER populate_dir, but verify no post-populate writes (journal pre-image, refcount table) invalidate already-computed checksums; (3) is journal replay running on mount and bypassing `ixfs_checksum_update` via `ixfs_disk_write()` in `src/kernel/fs/ixfs/ixfs_journal.c:121`? Log didn't show "replayed N entries", so likely not the cause here. Reproduce: boot with test=1 and watch `X:\Logs\Serial_*.log` for `[WARN] ixfs: checksum mismatch on block 131`. Fix shape: either (a) mkfs writes correct checksums and kernel reads them correctly -- then checksums should match; diff the two crc32c implementations bit-for-bit, or (b) some runtime write path bypasses `ixfs_checksum_update()` -- audit all `ixfs_disk_write()` callers outside the cache-eviction path.
- [ ] **VFS callbacks**: `ixfs_ops.c` -- all 14 ops, dir entries, mount sequence; verify cross-directory rename works; commit `"ixfs: verify VFS callbacks -- all 14 ops, cross-dir rename"`
- [ ] **Flush must reach the DEVICE**: ixfs flush writes fs caches without `blkdev_sync` and drops lower errors, so `vfs_flush() == 0` is not proof of durability -> XREF: `01-boot-platform/TODO-10` §28 (item: "PARKED: consume is left non-durable").
  - Callers already treat a zero return as a durability predicate and act irreversibly on it. The cross-boot crash path is the sharp case: it wants to retire the evidence page once `last-panic.txt` is written, and cannot, because a reset after a cache-only flush would lose the file AND the record. That retirement is parked on this item.
  - The fix is `blkdev_sync` on the underlying device plus error propagation, so a caller that needs stable storage can distinguish "cached" from "on the platter". Compare `src/kernel/fs/partition.c:103`, which already does the device-level call.
- [ ] **Reject over-length names**: `ixfs_ops.c` rename/create silently truncate names > IXFS_MAX_NAME (252), so a 252-259 char op resolves to a different entry. Fail with a name-too-long error instead of truncating. XREF: TODO-12 §13.

## 2. Extended Inode v3 `[Opus]`

Grow inode from 128 → 256 bytes. Add 10 new Win32-compatibility fields. Bump `IXFS_VERSION → 3`. Mount v2 volumes read-only with a clear error log.

**Files:** `include/kernel/fs/ixfs.h` (extend inode struct + version), `src/kernel/fs/ixfs/ixfs_inode.c`, `ixfs_format.c`, `ixfs_core.c` (version check at mount)

> [!NOTE]
> v3 inode must remain 256 bytes and packed. New fields (filling the 128-byte gap from 128→256): `uint32_t i_win32_attrs` (Win32 attribute flags), `uint64_t i_crtime` + `uint32_t i_crtime_ns` (creation time, separate from `i_ctime` change time), `uint32_t i_mtime_ns` (ns precision for `i_mtime`), `uint32_t i_security_id` (security descriptor ref), `uint32_t i_ads_first` (first ADS stream inode), `uint8_t i_compress_type`, `uint8_t i_encrypt_key_id`, `uint8_t i_flags` (sparse, immutable, append-only), `uint8_t i_reserved1`, `uint8_t i_file_id[16]` (UUID), `uint32_t i_ea_block`. Total new bytes: 4+8+4+4+4+4+1+1+1+1+16+4 = 52 bytes + 76 bytes padding/reserved = 128 bytes added. Verify `sizeof(struct ixfs_inode) == 256` at compile time with `_Static_assert`.

- [ ] Add all 10 new fields to `struct ixfs_inode`; add 76 bytes `i_reserved[76]` pad; add `_Static_assert(sizeof(struct ixfs_inode) == 256, "ixfs_inode must be 256 bytes")`
- [ ] Update `#define IXFS_VERSION 3`; update `IXFS_INODES_PER_BLOCK` → will now compute as `4096 / 256 = 16`
- [ ] Mount check: in `ixfs_init()`, if `sb.s_version == 2`: log `[IXFS] v2 volume mounted read-only (upgrade with ixfs_upgrade_v3)`; set `vol->read_only = 1`; return handle. If `sb.s_version < 2` or `> 3`: return error.
- [ ] Zero-init all new fields in `ixfs_inode_create()` (no garbage in new slots)
- [ ] Update `ixfs_format()` to write v3 superblock with `s_version = 3`
- [ ] `_Static_assert(sizeof(struct ixfs_inode) == 256, ...)` -- compile-time guard
- [ ] Update `vfs_init()` to return `boot_result_t` instead of `void` -- moved from TODO-01 §8
- [ ] Commit: `"ixfs: v3 inode -- 256B, 10 Win32 fields, IXFS_VERSION=3, v2 read-only mount"`

## 3. Case-Insensitive Paths `[Sonnet]`

Replace `ixfs_strcmp()` with `ixfs_name_cmp()` in all directory lookups. Fold lowercase before FNV1a hash. Add `IXFS_MOUNT_CASE_SENSITIVE` per-mount override.

**Files:** `src/kernel/fs/ixfs/ixfs_internal.h` (extend), all `ixfs_*.c` files that call `ixfs_strcmp`

> [!NOTE]
> ASCII fold table: characters `A–Z (0x41–0x5A)` map to `a–z (0x61–0x7A)`; all other bytes unchanged. Apply fold *only* in comparison and hash functions -- do not modify the stored filename on disk; preserve the original case in `d_name`. `IXFS_MOUNT_CASE_SENSITIVE` flag in `ixfs_volume.flags`: when set, use byte-exact compare (for POSIX compatibility). Future: Unicode NFC normalization via `ixfs_unicode.c` (separate TODO; document as planned here).

- [ ] `ixfs_name_cmp(const char *a, const char *b)`: ASCII fold both; return 0 if equal (case-insensitive by default)
- [ ] `ixfs_name_hash(const char *name)`: fold to lowercase before FNV1a hash; same hash for `"Foo"` and `"foo"`
- [ ] Replace all `ixfs_strcmp()` calls in directory lookup paths (`ixfs_dir_lookup`, `ixfs_dir_find_entry`, etc.) with `ixfs_name_cmp()`
- [ ] Add `IXFS_MOUNT_CASE_SENSITIVE (1 << 0)` to `ixfs_volume.mount_flags`; if set: use `ixfs_strcmp()` instead
- [ ] `ixfs_name_cmp` is also used by `vfs_probe()` (TODO-03 §1) IXFS identification -- verify probe still works after change
- [ ] Add to `ixfs_test.c`: create file `"Hello.txt"`, open `"hello.txt"` → same inode; open `"HELLO.TXT"` → same inode; commit `"ixfs: case-insensitive dir lookup -- ixfs_name_cmp, FNV1a fold, per-mount override"`

## 4. Alternate Data Streams (ADS) `[Opus]`

Implement ADS via the `i_ads_first` inode chain. Support `filename:streamname` path syntax in `vfs_open()`. Implement `FindFirstStreamW`/`FindNextStreamW` enumeration.

**Files:** `src/kernel/fs/ixfs/ixfs_ads.c` (new), `include/kernel/fs/ixfs.h` (extend), `src/kernel/fs/vfs.c` (path parser)

> [!NOTE]
> Stream inode: same `struct ixfs_inode` with `i_mode` type bits set to `IXFS_S_STREAM (0xA000)`. The primary file inode's `i_ads_first` points to the first stream inode; subsequent streams chain via `i_ads_first` (used as `i_ads_next` for stream inodes). Well-known streams: `"Zone.Identifier"` (Internet Zone mark), `"$OBJECT_ID"` (object ID stream mirror). `DeleteFile` on the primary inode must cascade-delete all stream inodes. Path syntax: `"C:\foo.txt:Zone.Identifier"` -- VFS parser splits on `:` (second occurrence; first is drive letter).

- [ ] `IXFS_S_STREAM 0xA000` -- add to `i_mode` type constants; `ixfs_is_stream(inode)` helper
- [ ] `ixfs_ads_create(vol, parent_inode_num, stream_name)` → create stream inode; set `i_mode = IXFS_S_STREAM`; append to `i_ads_first` chain
- [ ] `ixfs_ads_open(vol, parent_inode_num, stream_name)` → walk `i_ads_first` chain; match `stream_name` against stream inode's `d_name` (stored in inline data or extent); return stream inode number
- [ ] VFS path parser: in `vfs_open()`, after drive-letter split, check for second `:` in path component; if found: open parent, call `ixfs_ads_open()`; if stream not found and create flag: `ixfs_ads_create()`
- [ ] Cascade delete: in `ixfs_unlink()`, before freeing the primary inode, walk `i_ads_first` chain; free each stream inode and its data
- [ ] `FindFirstStreamW(path, infoLevel, &data)`: open inode; return first entry `"::$DATA"` (primary stream); then walk `i_ads_first` chain returning `":stream_name:$DATA"` entries; return `FIND_STREAM_DATA`
- [ ] `FindNextStreamW(hFind, &data)`: continue stream chain walk
- [ ] Add to `ixfs_test.c`: write `"foo.txt:Zone.Identifier"`, read back; `FindFirstStreamW` enumerates both; delete primary → streams gone
- [ ] Commit: `"ixfs: ADS -- i_ads_first chain, filename:stream path, cascade-delete, FindFirstStreamW"`

## 5. Security Descriptors `[Opus]`

Implement a hash-deduped security descriptor table in IXFS reserved inodes. Map each file to a table entry via `i_security_id`. Implement `GetFileSecurity`/`SetFileSecurity`. Inherit parent DACL on create.

**Files:** `src/kernel/fs/ixfs/ixfs_security.c` (new), `include/kernel/fs/ixfs.h` (extend)

> [!NOTE]
> SD table: reserved inode range (e.g., inodes 8–15 reserved for metadata). SD table inode stores an array of `{ uint32_t sd_hash; uint32_t sd_size; uint8_t sd_data[...]; }` entries appended to a data block chain. `i_security_id` in the v3 inode is the index into this table. Default SD at format time: Everyone:Full-Control (same binary format as NTFS `$Secure`). Parent DACL inheritance: when creating a file, if parent dir has a DACL with `CONTAINER_INHERIT_ACE` or `OBJECT_INHERIT_ACE` flags, propagate those ACEs to the child's DACL. Wire into `SeAccessCheck` (→ XREF: `10-platform-services`).

- [ ] `ixfs_sd_table_init(vol)`: at mount, find/create SD table inode; load existing entries into in-memory cache `vol->sd_cache[]` (up to 256 entries)
- [ ] `ixfs_sd_write(vol, sd_buf, sd_len, &sd_id)`: CRC32C hash SD; scan cache for dup; if not found: append to table inode data; assign new `sd_id`; return `sd_id`
- [ ] `ixfs_sd_read(vol, sd_id, buf, max_len)` → copy from cache; return length
- [ ] Default SD: Everyone:Full-Control -- written to table at format time; `sd_id = 0`; used for all newly created files unless parent inheritance applies
- [ ] Parent DACL inheritance: in `ixfs_create()`, read parent `i_security_id` → get parent SD; if DACL contains inheritable ACEs → build child SD; call `ixfs_sd_write()`; set `child->i_security_id`
- [ ] `GetFileSecurity(path/handle, SECURITY_INFORMATION, buf, len, &needed)` → `ixfs_sd_read(vol, fo->VfsNode->i_security_id, buf, len)`
- [ ] `SetFileSecurity(path/handle, SECURITY_INFORMATION, sd_buf)` → `ixfs_sd_write(vol, sd_buf, len, &new_id)` → update `i_security_id` in inode on disk
- [ ] Commit: `"ixfs: security descriptors -- hash-deduped SD table, i_security_id, GetFileSecurity, DACL inherit"`

## 6. Hard Links `[Sonnet]`

Implement `CreateHardLink`: add a directory entry to the same inode, increment `i_links`. On last unlink, cascade-free ADS chain and security descriptor reference.

**Files:** `src/kernel/fs/ixfs/ixfs_inode.c` (extend), `src/kernel/win32/hardlink.c` (new)

> [!NOTE]
> `i_links` already exists in the v2 inode and is incremented/decremented by `ixfs_link`/`ixfs_unlink`. This section wires the Win32 API and verifies the cascade-free path: on `i_links` reaching 0, the inode free sequence must be: (1) free all ADS stream inodes (§4); (2) decrement security descriptor refcount (§5) -- if refcount → 0, free SD table entry; (3) free data extents; (4) free inode itself. `GetFileInformationByHandle.nNumberOfLinks` must return `i_links`.

- [ ] Verify `ixfs_link(vol, inode_num, new_dir, new_name)`: creates dir entry pointing to existing inode, increments `i_links`, journals both changes
- [ ] Verify `ixfs_unlink(vol, dir, name)`: decrements `i_links`; if `i_links > 0`: only remove dir entry; if `i_links == 0`: cascade-free ADS → SD ref → extents → inode
- [ ] `CreateHardLinkW(lpNewFileName, lpExistingFileName, lpSecAttr)` → resolve both paths to drive/dir; call `ixfs_link()`; return TRUE/FALSE
- [ ] `GetFileInformationByHandle(hFile, &info)`: set `info.nNumberOfLinks = inode->i_links`
- [ ] Add to `ixfs_test.c`: create file; hard-link; verify `i_links == 2`; delete one → `i_links == 1`, data intact; delete other → inode freed, ADS chains gone
- [ ] Commit: `"ixfs: hard links -- CreateHardLink, i_links refcount, cascade-free on last unlink"`

## 7. Symbolic Links + Reparse Points `[Opus]`

Implement `IXFS_S_SYMLINK` inode type. Store target path in inline data (≤48 B) or extents. Resolve symlinks in VFS path traversal with max-8-hop guard. Implement `CreateSymbolicLink`.

**Files:** `src/kernel/fs/ixfs/ixfs_symlink.c` (new), `src/kernel/fs/vfs.c` (path traversal extension)

> [!NOTE]
> Symlink inode: `i_mode` type = `IXFS_S_SYMLINK (0xA001)`. Target path stored in inode inline data (up to 48 bytes without extents) or in a single extent block for longer paths (up to 255 bytes). `CreateSymbolicLink` with `SYMBOLIC_LINK_FLAG_DIRECTORY (0x1)` creates a symlink that points to a directory target. In VFS path traversal: after `finddir()` returns a node with `S_ISLNK` mode, read the target path, substitute it in the remaining traversal path, increment hop count; if hop count > 8: return `STATUS_REPARSE_LOOP` (maps to `WSAELOOP`).

- [ ] `IXFS_S_SYMLINK 0xA001` in `i_mode` type constants; `ixfs_is_symlink(inode)` helper
- [ ] `ixfs_symlink_create(vol, dir, link_name, target_path)`: create symlink inode; store target in inline data if `len ≤ 48` else in extent
- [ ] `ixfs_symlink_read(vol, inode_num, buf, max)`: read target path from inline or extent
- [ ] VFS path traversal: after resolving a path component to a symlink inode, read target; if absolute: restart from drive root; if relative: prepend current dir; increment `hop_count`; if `hop_count > 8`: `STATUS_REPARSE_LOOP`
- [ ] `CreateSymbolicLinkW(lpSymlink, lpTarget, dwFlags)`: call `ixfs_symlink_create()`; set `IXFS_S_SYMLINK`; if `SYMBOLIC_LINK_FLAG_DIRECTORY`: verify target is a dir
- [ ] `GetFileAttributesW` on symlink: follow by default; `FILE_FLAG_OPEN_REPARSE_POINT` in `CreateFile` → open the symlink inode itself without following
- [ ] `ReadLink(path)` → `ixfs_symlink_read()`; exposed as `FSCTL_GET_REPARSE_POINT` ioctl (returns REPARSE_DATA_BUFFER with `SYMLINK_REPARSE_TAG`)
- [ ] Commit: `"ixfs: symbolic links -- IXFS_S_SYMLINK, inline/extent target, 8-hop guard, CreateSymbolicLink"`

## 8. Extended Attributes `[Sonnet]`

Implement EA block referenced by `i_ea_block`. Pack EA entries (name+value). Implement `NtQueryEaFile`/`NtSetEaFile`. Support WSL interop EAs (`LXATTRB`, `LXUID`, `LXGID`).

**Files:** `src/kernel/fs/ixfs/ixfs_ea.c` (new), `include/kernel/fs/ixfs.h` (extend), `src/kernel/sched/syscall.c` (extend)

> [!NOTE]
> EA entry format (packed): `uint8_t NextEntryOffset (4)`; `uint8_t Flags`; `uint8_t EaNameLength`; `uint16_t EaValueLength`; `char EaName[EaNameLength+1]`; `uint8_t EaValue[EaValueLength]`. EA block is a single 4 KiB block at `i_ea_block`; total EA size ≤ 64 KiB (overflow: chain multiple blocks, same extent mechanism). WSL EAs: `"LXATTRB"` (struct containing `st_mode`, `st_uid`, `st_gid`, etc.); `"LXUID"` (UID); `"LXGID"` (GID) -- written by WSL when accessing IXFS-native files.

- [ ] `ixfs_ea_read(vol, inode, ea_name, buf, max_len)`: load `i_ea_block`; walk packed entries; find by name (case-insensitive); copy value to buf
- [ ] `ixfs_ea_write(vol, inode, ea_name, value, value_len, flags)`: load EA block (or alloc if `i_ea_block == 0`); find existing entry → overwrite; if not found: append; if block full: error `STATUS_BUFFER_OVERFLOW` (multi-block EA deferred to TODO-07)
- [ ] `ixfs_ea_delete(vol, inode, ea_name)`: zero out entry (set `EaNameLength = 0`); pack remaining entries
- [ ] `NtQueryEaFile(handle, IoStatus, buf, len, ReturnSingleEntry, EaList, EaListLen, EaIndex, RestartScan)` → `ixfs_ea_read()`; fill `FILE_FULL_EA_INFORMATION` records
- [ ] `NtSetEaFile(handle, IoStatus, buf, len)` → parse `FILE_FULL_EA_INFORMATION` records; call `ixfs_ea_write()` for each
- [ ] WSL interop: `"LXATTRB"` read → translate to Unix `stat` mode bits; write → update `i_mode` permission bits to match
- [ ] Commit: `"ixfs: extended attributes -- EA block, NtQueryEaFile/NtSetEaFile, LXATTRB/LXUID/LXGID"`

## 9. Object IDs `[Sonnet]`

Assign a 16-byte UUID to each file in `i_file_id`. Maintain an in-memory `objid → inode` hash table rebuilt on mount. Implement `FSCTL_CREATE_OR_GET_OBJECT_ID`, `FSCTL_DELETE_OBJECT_ID`, and `OpenFileById`.

**Files:** `src/kernel/fs/ixfs/ixfs_objid.c` (new), `src/kernel/fs/vfs.c` (extend `vfs_ioctl`)

> [!NOTE]
> UUID generation: use `hwrng_read()` (→ XREF: `04-drivers-hardware/TODO-04-security-hardware.md §2`) for 16 random bytes. Fall back to hash of (`volume_serial`, `inode_number`, `creation_time`) if hwrng not available. Hash table: `objid_ht[256]` keyed by `objid[0] ^ objid[1]` (simple XOR bucket); entries are `{ uint8_t id[16]; uint32_t inode; }`. Rebuild on mount by scanning all inodes with non-zero `i_file_id`. Object IDs persist across rename/move on the same volume; they are invalidated (zeroed) on cross-volume copy.

- [ ] `ixfs_objid_assign(vol, inode)`: if `i_file_id` is all-zero: generate UUID; write to inode; insert into hash table
- [ ] `ixfs_objid_lookup(vol, id[16], &inode_num)`: XOR-bucket hash lookup; compare 16-byte id; return inode number
- [ ] Rebuild on mount: `ixfs_objid_rebuild(vol)`: iterate all allocated inodes; for each with non-zero `i_file_id`: insert into hash table
- [ ] `FSCTL_CREATE_OR_GET_OBJECT_ID`: if `i_file_id` zero → call `ixfs_objid_assign()`; return `FILE_OBJECTID_BUFFER { ObjectId[16], BirthVolumeId[16], BirthObjectId[16], DomainId[16] }`
- [ ] `FSCTL_DELETE_OBJECT_ID`: zero out `i_file_id` in inode; remove from hash table
- [ ] `FSCTL_SET_OBJECT_ID`: set custom ID (must not conflict with existing)
- [ ] `OpenFileById(hVol, FILE_ID_DESCRIPTOR, access, share, ...)`: extract 16-byte ID; `ixfs_objid_lookup()`; open inode directly; return file handle
- [ ] Commit: `"ixfs: object IDs -- i_file_id UUID, objid hash table, FSCTL_CREATE_OR_GET_OBJECT_ID, OpenFileById"`

## 10. Format Tool v3 `[Opus]`

Upgrade `ixfs_format()` to emit a v3 superblock: 256-byte inodes, security descriptor table region, encryption key table region, backup superblock, USN journal region, quota table region.

**Files:** `src/kernel/fs/ixfs/ixfs_format.c` (extend), `include/kernel/fs/ixfs.h` (extend superblock)

> [!NOTE]
> v3 volume layout: sector 0–7 (VBR/reserved); block 0: superblock v3; block 1: inode allocation bitmap; block 2–N: inode table (256B inodes, `IXFS_INODES_PER_BLOCK = 16`); block N+1–M: data block bitmap; block M+1: SD table inode (inode 9); block M+2: encryption key table (inode 10, empty); block M+3: USN journal region (inode 11, 1 MiB default); block M+4: quota table (inode 12, empty); last block: backup superblock. Configurable format options: `inode_count`, `journal_size_mib`, `volume_label[64]`, `feature_flags` (compression, dedup, encryption -- bitmask in superblock, features not yet implemented).

- [ ] Add to superblock: `uint64_t s_sd_table_inode`, `uint64_t s_key_table_inode`, `uint64_t s_usn_journal_inode`, `uint64_t s_quota_table_inode`, `uint32_t s_feature_flags`, `uint8_t s_volume_label[64]`, `uint64_t s_backup_sb_block`; bump `s_version = 3`
- [ ] `ixfs_format_v3(blkdev, label, inode_count, journal_mib, feature_flags)`: compute layout; write superblock v3; create reserved inodes 0–15 (0=root, 8=SD table, 9=key table, 10=USN journal, 11=quota, 12=object ID table); write default SD (Everyone:Full-Control) to SD table inode; write backup superblock to last block
- [ ] Wire to `diskpart format fs=ixfs label=<str> quick`: calls `ixfs_format_v3(dev, label, default_inodes, 4, 0)`
- [ ] Wire to Disk Manager GUI "Format → IXFS" dialog: same call with user-specified label and optional flags
- [ ] `ixfs_format()` (old v2 entry point): call `ixfs_format_v3()` with defaults; maintain backward-compatible signature
- [ ] Commit: `"ixfs: format v3 -- 256B inodes, SD/key/USN/quota regions, backup superblock, feature flags"`

## 11. Parity Gate `[Sonnet]`

Verify that after §1–10, IXFS matches NTFS on all Win32 compatibility features. Run the full `ixfs_test.c` suite and `bash scripts/build.sh clean run`.

**Files:** `src/kernel/fs/ixfs/ixfs_test.c` (extend), `bash scripts/build.sh`

- [ ] Extend `ixfs_test.c` with Win32-compat tests: ADS create/read/delete/enumerate; hard link create/verify/cascade-free; symlink create/follow/loop-guard; EA write/read/delete; object ID assign/lookup/persist-across-rename; case-insensitive dir lookup
- [ ] `ixfs_format_v3` test: format 128 MiB ramdisk; verify layout (inode size=256, SD table inode exists, backup SB at last block)
- [ ] Security descriptor test: create file; `GetFileSecurity` returns Everyone:Full-Control; `SetFileSecurity` with custom SD; read back matches
- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Boot QEMU → C: (IXFS v3): all kernel subsystems mount correctly; `ixfs_test.c` suite runs and logs `[IXFS-TEST] All tests PASS`
- [ ] Commit: `"ixfs: Win32 compatibility complete -- ADS, SD, hard links, symlinks, EAs, ObjectIDs, v3 format"`

---

## OS Comparison


| ⭐  | Feature                                                                      | 🪟 Win11                                                              | 🐧 Linux                                            | 🚀 Impossible OS                                                              |
| --- | ---------------------------------------------------------------------------- | --------------------------------------------------------------------- | --------------------------------------------------- | ----------------------------------------------------------------------------- |
| 💎  | In-kernel CRC32C per-block checksums + `scrub` scan                          | ❌ NTFS has no per-block checksum;                                    | ✅ Btrfs per-block CRC32C; `btrfs scrub`;           | ⚠️ §1 -- Partial -- `ixfs_scrub()` exists; verifies                           |
| ⭐  | v3 256-byte inode with all Win32+Linux fields in one struct                  | ❌ NTFS `$STANDARD_INFORMATION` + `$DATA` +                           | ❌ `ext4_inode` has separate inode +                | ⬜ §2 -- single 256B struct: `i_win32_attrs`, `i_crtime`,                     |
| ⭐  | Case-insensitive lookup in-filesystem (not VFS fold) with per-mount override | ✅ NTFS `$UpCase` table; case-insensitive in-FS;                      | ⚠️ `ext4` dir-level casefold (optional); `btrfs`    | ⬜ §3 -- `ixfs_name_cmp()` ASCII fold, FNV1a fold-to-lower,                   |
| 💎  | Alternate Data Streams                                                       | ✅ NTFS ADS; `filename:streamname`; `FindFirstStreamW`                | ❌ `xattr` as partial ADS equivalent;               | ⬜ §4 -- `i_ads_first` inode chain, `filename:stream` path,                   |
| 💎  | On-disk security descriptors                                                 | ✅ NTFS `$Secure`; SD hash dedup;                                     | ❌ Linux ACL (`posix_acl`); no SD,                  | ⬜ §5 -- reserved-inode SD table, CRC32C dedup,                               |
| 💎  | Hard links                                                                   | ✅ NTFS hard links; `CreateHardLink`; `$FILE_NAME`                    | ✅ All Linux FS; `link()`/`unlink()`; `nlink`       | ⚠️ §6 -- Partial -- `i_links` and `ixfs_link/unlink`                          |
| 💎  | Symbolic links + reparse points, 8-hop guard, `CreateSymbolicLink`           | ✅ NTFS reparse points; symlinks; 63-hop                              | ✅ All Linux FS; `symlink()`; 40-hop                | ⬜ §7 -- `IXFS_S_SYMLINK`, inline/extent target, 8-hop guard,                 |
| 💎  | Extended attributes                                                          | ✅ NTFS EAs via `NtQueryEaFile`; `FILE_NEED_EA`;                      | ✅ All major FS; `xattr`; `getxattr`/`setxattr`;    | ⬜ §8 -- EA block, `NtQueryEaFile/NtSetEaFile`, `LXATTRB`/`LXUID`/`LXGID` EAs |
| ⭐  | Object IDs                                                                   | ✅ NTFS `$OBJECT_ID`; `OpenFileById`; `FSCTL_CREATE_OR_GET_OBJECT_ID` | ❌ No equivalent in ext4/btrfs; `inotify`           | ⬜ §9 -- `i_file_id[16]` in v3 inode, hash                                    |
| ⭐  | v3 format tool                                                               | ✅ `format.exe`; in-kernel `NTFS.sys`; regions hidden                 | ✅ `mkfs.ext4`, `mkfs.btrfs`; configurable features | ⬜ §10 -- `ixfs_format_v3()`, all regions pre-allocated, `diskpart            |

> **After §1–11:** IXFS v3 is the first from-scratch filesystem with native Win32 ADS, security descriptor dedup, object IDs, and WSL interop EAs built into the on-disk format from day one. NTFS achieves the same feature set but through decades of extensions to a 1990s design; `btrfs`/`ext4` are POSIX-native and require adaptation layers for Win32 semantics. IXFS v3 has no adaptation layer -- every Win32 file API call maps directly to an on-disk IXFS concept.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Doc bugs fixed: `ixfs.h` line ~14 shows "extent tree"; line ~163 shows "256 bytes each, 16 per block"; `_Static_assert(IXFS_INODES_PER_BLOCK == 16)` passes
- [ ] v3 inode: `_Static_assert(sizeof(struct ixfs_inode) == 256)` passes at compile time
- [ ] v2 mount: format v2 volume, attempt mount → serial log shows `[IXFS] v2 volume mounted read-only`; write attempt returns error
- [ ] Case-insensitive: `vfs_open("C:\\Impossible\\Fonts\\INTER.TTF")` returns same node as `vfs_open("C:\\impossible\\fonts\\inter.ttf")`
- [ ] ADS: create `test.txt:Zone.Identifier`; read back correct; `FindFirstStreamW` returns both `::$DATA` and `:Zone.Identifier:$DATA`; delete `test.txt` → stream inode freed
- [ ] Security: new file has `sd_id = 0` (Everyone:Full-Control); `SetFileSecurity` with custom SD → `sd_id` updated; `GetFileSecurity` reads back correctly
- [ ] Hard links: `CreateHardLink("C:\\link.txt", "C:\\orig.txt")` → `i_links == 2`; delete `orig.txt` → `i_links == 1`, data intact; delete `link.txt` → `i_links == 0`, inode freed
- [ ] Symlink: `CreateSymbolicLinkW("C:\\link", "C:\\Impossible\\System32")` → `vfs_open("C:\\link\\cmd.exe")` resolves correctly; circular symlink (A→B→A) → `STATUS_REPARSE_LOOP`
- [ ] EAs: `NtSetEaFile` writes `"LXUID"` value 1000; `NtQueryEaFile` reads it back; `ixfs_ea_delete` removes it
- [ ] Object IDs: `FSCTL_CREATE_OR_GET_OBJECT_ID` returns 16-byte UUID; `rename` same volume → UUID unchanged; `OpenFileById` opens correct file
- [ ] Format v3: `ixfs_format_v3(ramdisk, "TEST", ...)` → mount succeeds; SD table inode at inode 8; backup superblock at last block; `inode_size == 256`
- [ ] `ixfs_test.c` suite: all Win32-compat tests log `[IXFS-TEST] PASS`; no failures
- [ ] Commit: `"ixfs: Win32 compatibility complete -- ADS, SD, hard links, symlinks, EAs, ObjectIDs, v3 format"`
