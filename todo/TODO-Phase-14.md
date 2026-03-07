# ISO Installer

> **Deferred** — implement after all core OS features are complete.

## 14.1 Installer Program

- [ ] Write `src/installer/installer.c` — runs as a special init process from the ISO
- [ ] Display a **welcome screen** (GUI or text-mode)
- [ ] **Disk selection** — list available drives (ATA enumeration)
- [ ] **Partitioning** — create a **GPT** partition table on the target disk (UEFI requires GPT)
  - [ ] Create an **EFI System Partition** (FAT32, ~512 MiB, type `EF00`) → `A:\`
  - [ ] Create a root partition (remainder of disk, IXFS) → `C:\`
- [ ] **Format** partitions:
  - [ ] Write FAT32 BPB + empty FAT for ESP
  - [ ] Run `ixfs_format()` on the root partition
- [ ] **Copy files** — copy kernel ELF, initrd, and OS files to `C:\`
- [ ] **Install GRUB for UEFI** — `grub-install --target=x86_64-efi` to the ESP
- [ ] **Finalize** — display "Installation Complete, Reboot" message
- [ ] Test in QEMU: boot ISO → install to a virtual disk → reboot from disk → OS loads → ✅
- [ ] Commit: `"installer: full OS installer"`

## 14.2 Bootable ISO Creation

- [ ] Write `scripts/make-iso.sh` — automates ISO creation
- [ ] Use `grub-mkrescue` or `xorriso` to produce `os-build.iso`
- [ ] Verify ISO boots in QEMU
- [ ] Sign the ISO with a checksum (`sha256sum os-build.iso > os-build.iso.sha256`)
- [ ] Commit: `"release: ISO build script"`
