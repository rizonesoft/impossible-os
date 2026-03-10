# Phase 00 — Boot Consolidation (Single Disk Image)

> **Goal:** Replace the current three-artifact boot chain (`os-build.iso` + `initrd.img` + `gpt-test.img`)
> with a **single bootable GPT disk image** (`system-disk.img`). Eliminates ISO creation,
> initrd packing, and first-boot file copying.
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.


---

## 1. Standalone GRUB EFI Binary ✅

- [x] Use `grub-mkimage` to produce `BOOTX64.EFI` with modules: `part_gpt fat normal multiboot2 boot all_video efi_gop gfxterm configfile echo search test reboot halt font loadenv`
- [x] Target: `x86_64-efi`, prefix `/boot/grub`
- [x] Store output in `build/tools/BOOTX64.EFI` (790 KiB PE32+)
- [x] Add Makefile target: `grub-efi` (wired into `all`)
- [x] Commit: `9bc69c7` — `"boot: standalone GRUB EFI binary"`

---

## 2. Unified System Disk Builder ✅

> Created `tools/make-system-disk.c` — produces a bootable GPT disk
> with 2 partitions. Makefile orchestrates formatting via mkfs.fat/mcopy + mkfs-ixfs.

### 2.1 Partition Layout

| # | Type | GUID | Size | Mount |
|---|------|------|------|-------|
| 1 | EFI System Partition (FAT32) | `C12A7328-...` | 256 MiB | D:\ |
| 2 | IXFS (System) | Custom GUID | remainder | C:\ |

> Windows-style: **everything** lives on C:\ — system files, user profiles
> (`C:\Users\`), programs (`C:\Programs\`). No separate user partition.
>
> The test disk library (`build/test-disks/`) provides secondary drives
> for filesystem driver testing — mounted as D:\, E:\, etc.

### 2.2 EFI Partition (FAT32) Contents — Boot Only

- [x] `/EFI/BOOT/BOOTX64.EFI` — GRUB standalone binary
- [x] `/boot/grub/grub.cfg` — GRUB config (load kernel via multiboot2)
- [x] `/boot/kernel.exe` — kernel binary
- [x] Write FAT32 using `mkfs.fat --offset` + `mcopy` in Makefile

> ⚠️ Only boot-critical files on the EFI partition. All assets (wallpaper,
> icons, backgrounds) live on C:\ — loaded after IXFS mounts.

### 2.3 IXFS System Partition (C:\)

- [x] Use `mkfs-ixfs` logic to create IXFS v2 filesystem
- [x] Pre-populate directory hierarchy:
  - [x] `C:\Impossible\System\` — system files + assets
  - [x] `C:\Impossible\Commands\` — command-line tools
  - [x] `C:\Users\Default\` — default user profile
  - [x] `C:\Programs\` — installed applications
  - [x] `C:\Documents\backgrounds\` — wallpaper copies
- [x] Pre-populate system files: `hello.exe`, `shell.exe`, `hello.txt`, `readme.txt`
- [x] Pre-populate assets: `wallpaper.raw`, `bg.raw`, `start_icon.raw` → `C:\Impossible\System\`
- [x] Label: `"Impossible OS"`

### 2.4 Tool Implementation

- [x] Accept CLI args: `-o output.img`, `-s total_size`, `--efi-size SIZE`
- [x] Write GPT header + partition entries
- [x] Write backup GPT at end of disk
- [x] Generate `.info` file with partition offsets
- [x] Compute protective MBR + GPT checksums
- [x] Commit: `3251de9` — `"tools: unified system disk builder"`

---

## 3. Makefile Refactor

### 3.1 New Build Flow

```
make all
  → kernel (compile kernel.exe)
  → userland (compile hello.exe, shell.exe)
  → assets (convert wallpaper.raw, bg.raw, start_icon.raw)
  → grub-efi (build BOOTX64.EFI)
  → system-disk (assemble build/system-disk.img)
```

- [x] Add `grub-efi` target: `grub-mkimage` → `build/tools/BOOTX64.EFI`
- [x] Add `assets` target: `jpg2raw` conversions
- [x] Update `system-disk` target: 3-step pipeline (GPT → FAT32 → IXFS)
- [x] Remove `iso` target (`grub-mkrescue`, `make-initrd`) — *keeping ISO for fallback*
- [x] Remove `initrd.img` build steps — removed in `56c2a12`
- [x] Commit: `3251de9` — Makefile rewrite

### 3.2 QEMU Configuration Update

- [x] Boot from disk: `-drive file=build/system-disk.img,format=raw,if=none,id=sysdisk`
- [x] Keep OVMF UEFI firmware (boots from EFI partition on GPT disk)
- [x] Update `run`, `run-debug`, `run-log` targets
- [x] Commit: `3251de9`

---

## 4. Kernel Boot Path Cleanup

### 4.1 Remove initrd Dependency ✅

- [x] Remove `initrd_init()` and `vfs_mount('B', ...)` from `main.c`
- [x] Update VFS boot test: read `hello.txt` from `C:\` instead of `B:\`
- [x] Load wallpaper/icons from `C:\` (IXFS) via VFS heap-allocated buffers
- [x] Desktop loads after `partition_mount_filesystems()` (IXFS available)
- [x] Commit: `ba277b0` — `"kernel: boot from disk, remove initrd dependency"`

### 4.2 Remove firstboot.c ✅

- [x] Delete `src/kernel/fs/firstboot.c`
- [x] Delete `include/kernel/fs/firstboot.h`
- [x] Remove `firstboot_setup()` call from `main.c` *(done in `ba277b0`)*
- [x] Remove `#include "kernel/fs/firstboot.h"` from `main.c` *(done in `ba277b0`)*
- [x] Commit: `e542907` — `"kernel: remove firstboot (pre-populated disk)"`

### 4.3 Remove initrd.c ✅

- [x] Delete `src/kernel/fs/initrd.c` and `include/kernel/fs/initrd.h`
- [x] Remove initrd parsing from multiboot2 module handler
- [x] Remove `make-initrd.c` from `tools/`
- [x] ⚠️ Verified: no other subsystem depends on initrd
- [x] Migrated `syscall.c` (SYS_READFILE, SYS_READDIR, SYS_EXEC) to `vfs_get_drive_root('C')`
- [x] Renamed `build/initrd_files/` → `build/sysroot/` in Makefile
- [x] Updated comments across 8 files + `user/shell.c`
- [x] Commit: `56c2a12` — `"kernel: remove initrd subsystem"`

---

## 5. Consolidate Disk Images

> All test scenarios currently spread across 4 disk images are handled by
> `system-disk.img` alone. Attach it to both VirtIO **and** AHCI in QEMU
> to exercise both drivers.

### 5.1 Remove Legacy Disk Images ✅

- [x] Remove `disk.img` creation (FAT32 test) — EFI partition covers FAT32 testing
- [x] Remove `gpt-test.img` creation (`make-gpt`) — `system-disk.img` is the GPT disk
- [x] Remove `sata.img` creation — attach `system-disk.img` via AHCI instead
- [x] Remove `mbr-test.img` creation (`make-mbr`) — see 5.2
- [x] Update QEMU flags: single disk attached via AHCI
- [x] Commit: consolidated in system-disk build pipeline

### 5.2 Test Disk Library (`build/test-disks/`) ✅

> Small pre-formatted disk images for filesystem driver testing.
> Attached as secondary QEMU drives. Each image is 4–16 MiB.

- [x] Create `tools/make-test-disks.sh` — script to generate all test images
- [x] `fat32.img` — FAT32 with sample files (via `mkfs.fat` + `mcopy`)
- [x] `exfat.img` — exFAT with sample files (via `mkfs.exfat`)
- [x] `ext2.img` — ext2 with sample files (via `mkfs.ext2` + `debugfs`)
- [x] `ext3.img` — ext3 with journal (via `mkfs.ext3` + `debugfs`)
- [x] `ext4.img` — ext4 with extents + journal (via `mkfs.ext4` + `debugfs`)
- [x] `ntfs.img` — NTFS with sample files (via `mkntfs` + `ntfscp`)
- [x] `ixfs.img` — standalone IXFS v2 (via `mkfs-ixfs`)
- [x] `mbr.img` — MBR partition table (fallback: minimal MBR if `make-mbr` missing)
- [x] `gpt.img` — GPT partition table (fallback: minimal GPT if `make-gpt` missing)
- [x] Each image populated with: `test.txt`, `subdir/nested.txt`, empty file, large file
- [x] Makefile target: `test-disks` (generates all, skips if already exist)
- [x] Commit: `"tools: test disk library for filesystem drivers"`

### 5.3 Optical Media Test Images (`build/test-disks/optical/`) ✅

> CD/DVD/Blu-ray test images for ATAPI driver and optical filesystem support.
> Attached via `-cdrom` in QEMU.

- [x] `iso9660.iso` — ISO 9660 (via `xorriso` or `genisoimage`)
- [x] `joliet.iso` — ISO 9660 + Joliet extensions (via `xorriso -J`)
- [x] `udf.iso` — UDF 1.02 (via `genisoimage -udf` or `mkudffs`, skips if not installed)
- [x] `udf250.iso` — UDF 2.50 (via `mkudffs --udfrev=0x0250`, skips if not installed)
- [x] `mixed.iso` — ISO 9660 + UDF bridge (via `xorriso -J -udf`)
- [x] Each image populated with: `readme.txt`, `media/sample.dat`, nested directories
- [x] QEMU attachment: `-cdrom build/test-disks/optical/iso9660.iso`
- [x] Commit: `"tools: optical media test images"`

### 5.4 QEMU Disk Attachment

```
# Primary system disk (VirtIO — fast, paravirtualized):
-drive file=build/system-disk.img,format=raw,if=none,id=sysdisk
-device virtio-blk-pci,drive=sysdisk

# Secondary test disk (AHCI/SATA — real hardware emulation):
-drive file=build/test-disks/ntfs.img,format=raw,if=none,id=satadisk
-device ahci,id=ahci0
-device ide-hd,drive=satadisk,bus=ahci0.0

# Tertiary test disk (NVMe — modern storage):
-drive file=build/test-disks/ext4.img,format=raw,if=none,id=nvmedisk
-device nvme,serial=impossible01,drive=nvmedisk
```

> Swap test disk images to exercise different filesystems.
> The kernel's partition scanner discovers all three controllers at boot.

### 5.5 Storage Driver Test Matrix

| Controller | Driver | Status | Test Disk |
|------------|--------|--------|-----------|
| AHCI/SATA | `ahci.c` | ✅ Implemented | `system-disk.img` (port 0) + `test-disks/*.img` (port 1) |
| VirtIO-blk | `virtio_blk.c` | ✅ Implemented | (available, not default) |
| NVMe | `nvme.c` | 🔲 Future | `test-disks/*.img` |

- [x] Add `make run-test DISK=<name>` — attaches secondary test disk on AHCI port 1
- [x] Optical support: `make run-test DISK=optical/iso9660` (attaches via `-cdrom`)
- [ ] ~~Add `make run-nvme`~~ — deferred until NVMe driver exists
- [x] Commit: `"build: add run-test target for filesystem driver testing"`

---

## 6. Cleanup Obsolete Files ✅

- [x] Delete `tools/make-initrd.c` — removed in `56c2a12`
- [x] Delete `tools/make-gpt.c` — removed (test-disks uses minimal dd-based GPT)
- [x] Delete `tools/make-mbr.c` — removed (test-disks uses minimal dd-based MBR)
- [x] Delete `src/kernel/fs/firstboot.c` and `include/kernel/fs/firstboot.h` — already deleted in `e542907`
- [x] ~~Delete `src/boot/grub.cfg`~~ — **kept** (still used by both ISO and system-disk Makefile targets)
- [x] Update `.gitignore` to reflect new build artifacts
- [x] Commit: `"build: remove obsolete tools and configs"`

---

## Priority Order

| Priority | Section | Reason |
|----------|---------|--------|
| 🔴 P0 | 1. GRUB EFI binary | Prerequisite for disk boot |
| 🔴 P0 | 2. System disk builder | Core deliverable |
| 🔴 P0 | 3. Makefile refactor | Must update build to match |
| 🟠 P1 | 4.1 Remove initrd dep | Kernel must boot from disk |
| 🟠 P1 | 4.2 Remove firstboot | Dead code with pre-populated disk |
| ✅ Done | 4.3 Remove initrd.c | Full cleanup, completed |
| 🟡 P2 | 5. Cleanup obsolete | Housekeeping |

---

## Architecture Diagram

```
┌─────────────────────────────────────────────────────────────┐
│              system-disk.img (GPT)              │
├──────────────┬───────────────────────────────────┤
│ Part 1       │ Part 2                            │
│ FAT32 (EFI)  │ IXFS v2 (C:\)                     │
│ 256 MiB      │ remainder                         │
│              │                                   │
│ /EFI/BOOT/   │ C:\Impossible\System\             │
│  BOOTX64.EFI │   wallpaper.raw, bg.raw           │
│ /boot/       │   start_icon.raw                  │
│  kernel.exe  │   hello.exe, shell.exe            │
│  grub.cfg    │ C:\Impossible\Commands\           │
│              │ C:\Users\Default\                 │
│              │ C:\Programs\                      │
│              │ C:\Documents\backgrounds\         │
└─────────────┴───────────────────────┴───────────────────────┘
         │
         ▼
    OVMF (UEFI) → GRUB → kernel.exe → boot
```
