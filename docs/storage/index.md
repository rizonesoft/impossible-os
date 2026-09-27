# Storage

Storage controllers, disk partitioning, and filesystem drivers.

*Implementation docs will be added here as storage TODOs are completed.*

## Subdirectories

| Directory        | Scope                                              | Specs                                                                  |
| ---------------- | -------------------------------------------------- | ---------------------------------------------------------------------- |
| `controllers/`   | AHCI, VirtIO, ATAPI/SCSI drivers                  | [Specs → Storage Controllers](../../specs/storage/controllers/)           |
| `partitioning/`  | GPT, MBR partition table handling                  | [Specs → Partitioning](../../specs/storage/partitioning/)                 |
| `filesystems/`   | FAT32, IXFS, ext4, NTFS, exFAT, Btrfs drivers     | [Specs → Filesystems](../../specs/storage/filesystems/)                   |
