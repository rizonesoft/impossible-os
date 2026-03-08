# Phase 00 — Boot Consolidation (Single Disk Image)

> **Goal:** Replace the current three-artifact boot chain (`os-build.iso` + `initrd.img` + `gpt-test.img`)
> with a **single bootable GPT disk image** (`system-disk.img`). Eliminates ISO creation,
> initrd packing, and first-boot file copying.

---

## 1. Standalone GRUB EFI Binary ✅

- [x] Use `grub-mkimage` to produce `BOOTX64.EFI` with modules: `part_gpt fat normal multiboot2 boot all_video efi_gop gfxterm configfile echo search test reboot halt font loadenv`
- [x] Target: `x86_64-efi`, prefix `/boot/grub`
- [x] Store output in `build/tools/BOOTX64.EFI` (790 KiB PE32+)
- [x] Add Makefile target: `grub-efi` (wired into `all`)
- [x] Commit: `9bc69c7` — `"boot: standalone GRUB EFI binary"`

---

## 2. Unified System Disk Builder

> Upgrade `make-gpt.c` (or create `tools/make-system-disk.c`) to produce a single
> bootable GPT disk with 3 partitions.

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

- [ ] `/EFI/BOOT/BOOTX64.EFI` — GRUB standalone binary
- [ ] `/boot/grub/grub.cfg` — GRUB config (load kernel via multiboot2)
- [ ] `/boot/kernel.exe` — kernel binary
- [ ] Write FAT32 using `mtools` (`mcopy`) or raw FAT32 writer in the tool

> ⚠️ Only boot-critical files on the EFI partition. All assets (wallpaper,
> icons, backgrounds) live on C:\ — loaded after IXFS mounts.

### 2.3 IXFS System Partition (C:\)

- [ ] Use `mkfs-ixfs` logic to create IXFS v2 filesystem
- [ ] Pre-populate directory hierarchy:
  - [ ] `C:\Impossible\System\` — system files + assets
  - [ ] `C:\Impossible\Commands\` — command-line tools
  - [ ] `C:\Users\Default\` — default user profile
  - [ ] `C:\Programs\` — installed applications
  - [ ] `C:\Documents\backgrounds\` — wallpaper copies
- [ ] Pre-populate system files: `hello.exe`, `shell.exe`, `hello.txt`, `readme.txt`
- [ ] Pre-populate assets: `wallpaper.raw`, `bg.raw`, `start_icon.raw` → `C:\Impossible\System\`
- [ ] Label: `"Impossible OS"`

### 2.4 Tool Implementation

- [ ] Accept CLI args: `-o output.img`, `-s total_size`, `--kernel path`, `--grub path`, `--populate dir`
- [ ] Write GPT header + partition entries
- [ ] Write FAT32 EFI partition with boot files
- [ ] Write IXFS system partition with pre-populated hierarchy
- [ ] Write empty IXFS user data partition
- [ ] Compute protective MBR + GPT checksums
- [ ] Commit: `"tools: unified system disk builder"`

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

- [ ] Add `grub-efi` target: `grub-mkimage` → `build/tools/BOOTX64.EFI`
- [ ] Add `assets` target: `jpg2raw` conversions
- [ ] Update `system-disk` target: invoke unified disk builder with all inputs
- [ ] Remove `iso` target (`grub-mkrescue`, `make-initrd`)
- [ ] Remove `initrd.img` build steps
- [ ] Commit: `"build: Makefile refactor for single disk image"`

### 3.2 QEMU Configuration Update

- [ ] Remove `-cdrom $(ISO_FILE)` from QEMU flags
- [ ] Boot from disk: `-drive file=build/system-disk.img,format=raw,if=none,id=disk0`
- [ ] Keep OVMF UEFI firmware (boots from EFI partition on GPT disk)
- [ ] Keep secondary SATA test disk for AHCI testing
- [ ] Update `run`, `run-debug`, `run-log` targets
- [ ] Commit: `"build: QEMU direct-disk boot"`

---

## 4. Kernel Boot Path Cleanup

### 4.1 Remove initrd Dependency

- [ ] Remove `initrd_init()` and `vfs_mount('B', ...)` from `main.c`
- [ ] Update VFS boot test: read `hello.txt` from `C:\` instead of `B:\`
- [ ] Load wallpaper/icons from `D:\boot\` (FAT32 EFI partition) instead of initrd
- [ ] Move wallpaper load after `partition_mount_filesystems()` (FAT32 available)
- [ ] Commit: `"kernel: boot from disk, remove initrd dependency"`

### 4.2 Remove firstboot.c

- [ ] Delete `src/kernel/fs/firstboot.c`
- [ ] Delete `include/kernel/fs/firstboot.h`
- [ ] Remove `firstboot_setup()` call from `main.c`
- [ ] Remove `#include "kernel/fs/firstboot.h"` from `main.c`
- [ ] Commit: `"kernel: remove firstboot (pre-populated disk)"`

### 4.3 Remove initrd.c (Optional / Deferred)

- [ ] Delete `src/kernel/fs/initrd.c` and `include/kernel/fs/initrd.h`
- [ ] Remove initrd parsing from multiboot2 module handler
- [ ] Remove `make-initrd.c` from `tools/`
- [ ] ⚠️ Only if no other subsystem depends on initrd
- [ ] Commit: `"kernel: remove initrd subsystem"`

---

## 5. Consolidate Disk Images

> All test scenarios currently spread across 4 disk images are handled by
> `system-disk.img` alone. Attach it to both VirtIO **and** AHCI in QEMU
> to exercise both drivers.

### 5.1 Remove Legacy Disk Images

- [ ] Remove `disk.img` creation (FAT32 test) — EFI partition covers FAT32 testing
- [ ] Remove `gpt-test.img` creation (`make-gpt`) — `system-disk.img` is the GPT disk
- [ ] Remove `sata.img` creation — attach `system-disk.img` via AHCI instead
- [ ] Remove `mbr-test.img` creation (`make-mbr`) — see 5.2
- [ ] Update QEMU flags: single disk attached to **both** VirtIO and AHCI
- [ ] Commit: `"build: remove legacy disk images"`

### 5.2 Test Disk Library (`build/test-disks/`)

> Small pre-formatted disk images for filesystem driver testing.
> Attached as secondary QEMU drives. Each image is 4–16 MiB.

- [ ] Create `tools/make-test-disks.sh` — script to generate all test images
- [ ] `fat32.img` — FAT32 with sample files (via `mkfs.fat` + `mcopy`)
- [ ] `exfat.img` — exFAT with sample files (via `mkfs.exfat`)
- [ ] `ext2.img` — ext2 with sample files (via `mkfs.ext2`)
- [ ] `ext3.img` — ext3 with journal (via `mkfs.ext3`)
- [ ] `ext4.img` — ext4 with extents + journal (via `mkfs.ext4`)
- [ ] `ntfs.img` — NTFS with sample files (via `mkntfs` from ntfs-3g)
- [ ] `ixfs.img` — standalone IXFS v2 (via `mkfs-ixfs`)
- [ ] `mbr.img` — MBR partition table with FAT32 + Linux partitions (via `make-mbr`)
- [ ] `gpt.img` — GPT partition table with multiple FS types
- [ ] Each image populated with: `test.txt`, `subdir/nested.txt`, empty file, large file
- [ ] Makefile target: `test-disks` (generates all, skips if already exist)
- [ ] Commit: `"tools: test disk library for filesystem drivers"`

### 5.3 Optical Media Test Images (`build/test-disks/optical/`)

> CD/DVD/Blu-ray test images for ATAPI driver and optical filesystem support.
> Attached via `-cdrom` in QEMU.

- [ ] `iso9660.iso` — ISO 9660 (classic CD-ROM filesystem, via `genisoimage`)
- [ ] `joliet.iso` — ISO 9660 + Joliet extensions (long Unicode filenames, via `genisoimage -J`)
- [ ] `udf.iso` — UDF 1.02 (DVD data disc, via `mkudffs` + `genisoimage -udf`)
- [ ] `udf250.iso` — UDF 2.50 (Blu-ray compatible, via `mkudffs -r 2.50`)
- [ ] `mixed.iso` — ISO 9660 + UDF bridge (readable by both drivers)
- [ ] Each image populated with: `readme.txt`, `media/sample.dat`, nested directories
- [ ] QEMU attachment: `-cdrom build/test-disks/optical/iso9660.iso`
- [ ] Commit: `"tools: optical media test images"`

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
| VirtIO-blk | `virtio_blk.c` | ✅ Implemented | `system-disk.img` |
| AHCI/SATA | `ahci.c` | ✅ Implemented | `test-disks/*.img` |
| NVMe | `nvme.c` | 🔲 Future | `test-disks/*.img` |

- [ ] Update `make run` QEMU flags to include all three controllers
- [ ] Add `make run-sata` variant (system disk on AHCI, no VirtIO)
- [ ] Add `make run-nvme` variant (system disk on NVMe, no VirtIO)
- [ ] Commit: `"build: multi-controller QEMU configurations"`

---

## 6. Cleanup Obsolete Files

- [ ] Delete `tools/make-initrd.c`
- [ ] Delete `tools/make-gpt.c`
- [ ] Delete `tools/make-mbr.c` (if MBR support dropped)
- [ ] Delete `src/kernel/fs/firstboot.c` and `include/kernel/fs/firstboot.h`
- [ ] Delete `src/boot/grub.cfg` (moves into disk builder)
- [ ] Update `.gitignore` to reflect new build artifacts
- [ ] Commit: `"build: remove obsolete tools and configs"`

---

## Priority Order

| Priority | Section | Reason |
|----------|---------|--------|
| 🔴 P0 | 1. GRUB EFI binary | Prerequisite for disk boot |
| 🔴 P0 | 2. System disk builder | Core deliverable |
| 🔴 P0 | 3. Makefile refactor | Must update build to match |
| 🟠 P1 | 4.1 Remove initrd dep | Kernel must boot from disk |
| 🟠 P1 | 4.2 Remove firstboot | Dead code with pre-populated disk |
| 🟡 P2 | 4.3 Remove initrd.c | Full cleanup, may defer |
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
