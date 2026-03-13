# VFS (Virtual Filesystem) API

The VFS provides a unified file operations interface for all filesystem drivers
(IXFS, FAT32, NTFS, ext2/3/4, exFAT, ISO 9660).

## Drive Letters

- `A:\` — EFI System Partition (FAT32)
- `C:\` — System drive (IXFS)
- `D:\`, `E:\`, … — Additional partitions

## Core API

| Function | Description |
|----------|-------------|
| `vfs_open(path, flags)` | Open file/directory, increments `ref_count` |
| `vfs_close(node)` | Close, decrements `ref_count` |
| `vfs_read(node, offset, size, buf)` | Read data from file |
| `vfs_write(node, offset, size, buf)` | Write data to file |
| `vfs_create(path, type)` | Create file (`VFS_FILE`) or directory (`VFS_DIRECTORY`) |
| `vfs_unlink(path)` | Delete file/directory (blocks if `ref_count > 0`) |
| `vfs_rename(old_path, new_path)` | Rename within same drive + directory |
| `vfs_stat(path, &st)` | Get file metadata without opening |
| `vfs_truncate(path, new_size)` | Resize file (free trailing clusters/extents) |
| `vfs_readdir(dir, index)` | Enumerate directory entries |
| `vfs_finddir(dir, name)` | Look up named entry in directory |
| `vfs_mount(letter, driver, root)` | Mount filesystem at drive letter |
| `vfs_unmount(letter)` | Unmount drive |
| `vfs_get_drive_root(letter)` | Get root node of mounted drive |
| `vfs_is_mounted(letter)` | Check if drive letter is in use |

## Open Flags

| Flag | Value | Description |
|------|-------|-------------|
| `VFS_O_READ` | `0x01` | Open for reading |
| `VFS_O_WRITE` | `0x02` | Open for writing |
| `VFS_O_CREATE` | `0x04` | Create if not exists |
| `VFS_O_APPEND` | `0x08` | Append mode |
| `VFS_O_TRUNC` | `0x10` | Truncate on open |

## Open-File Protection

Each `vfs_node` has a `ref_count` field:
- Incremented by `vfs_open()`
- Decremented by `vfs_close()`
- `vfs_unlink()` checks `ref_count > 0` before deletion — returns `-1` if file is still open

## Filesystem Driver Interface (`vfs_ops`)

Each filesystem implements these callbacks:

```c
struct vfs_ops {
    int      (*open)(node, flags);
    int      (*close)(node);
    int      (*read)(node, offset, size, buffer);
    int      (*write)(node, offset, size, buffer);
    dirent  *(*readdir)(node, index);
    node    *(*finddir)(node, name);
    int      (*create)(parent, name, type);
    int      (*unlink)(parent, name);
    int      (*rename)(parent, old_name, new_name);
    int      (*stat)(node, &st);
    int      (*truncate)(node, new_size);
};
```

## File Metadata (`struct vfs_stat`)

| Field | Type | Description |
|-------|------|-------------|
| `size` | `uint64_t` | File size in bytes |
| `type` | `uint8_t` | `VFS_FILE` or `VFS_DIRECTORY` |
| `ctime` | `uint32_t` | Creation time (seconds since boot) |
| `mtime` | `uint32_t` | Modification time |
| `atime` | `uint32_t` | Access time |
| `blocks` | `uint32_t` | Disk blocks used |

## Registry Journaling

The registry hive persistence uses VFS rename for atomic file swaps:

1. **Write** new data to `.hive.log`
2. **Backup** `.hive` → `.hive.bak`
3. **Rename** `.hive.log` → `.hive` (atomic via `vfs_rename`)
4. If rename fails: fallback to copy+overwrite, then `vfs_unlink(.hive.log)`

## Source Files

- `include/kernel/fs/vfs.h` — API declarations and types
- `src/kernel/fs/vfs.c` — VFS dispatch layer
- `src/kernel/fs/ixfs/ixfs_ops.c` — IXFS VFS callbacks
- `src/kernel/fs/fat32.c` — FAT32 VFS callbacks
