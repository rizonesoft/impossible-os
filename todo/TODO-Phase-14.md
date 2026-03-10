# ISO Installer

> **Deferred** — implement after all core OS features are complete.
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.


## 14.1 Installer Program

**Prompt:** The ISO installer is the capstone feature: boot from ISO, partition a target disk (GPT with EFI System Partition + IXFS root), format both partitions, copy kernel/initrd/OS files, install GRUB for UEFI, and display completion. Test the full cycle in QEMU: boot ISO → install to virtual disk → reboot from disk → OS loads → ✅. After completing all items, create `docs/architecture/installer.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"installer: full OS installer"`.


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

**Prompt:** Automate ISO creation with `scripts/make-iso.sh` using `grub-mkrescue` or `xorriso`. Verify ISO boots in QEMU. Sign with `sha256sum os-build.iso > os-build.iso.sha256`. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"release: ISO build script"`.


- [ ] Write `scripts/make-iso.sh` — automates ISO creation
- [ ] Use `grub-mkrescue` or `xorriso` to produce `os-build.iso`
- [ ] Verify ISO boots in QEMU
- [ ] Sign the ISO with a checksum (`sha256sum os-build.iso > os-build.iso.sha256`)
- [ ] Commit: `"release: ISO build script"`
