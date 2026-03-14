# P2501 — Installer & ISO

> **Goal:** Build the ISO installer, automate ISO creation, and validate the
> full install-to-desktop cycle on Hyper-V and other hypervisors. This phase is
> **deferred** until all core OS features are complete.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.

> [!NOTE]
> **Consolidated from:** TODO-Phase-98.md (Installer Program + ISO Creation)
> and TODO-Phase-99.md (Hyper-V Validation).

---

## 1. Bootable ISO Creation

**Prompt:** Automate ISO creation with `scripts/make-iso.sh` using `grub-mkrescue` or `xorriso`. Verify ISO boots in QEMU. Sign with `sha256sum os-build.iso > os-build.iso.sha256`. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"release: ISO build script"`.


- [ ] Write `scripts/make-iso.sh` — automates ISO creation
- [ ] Use `grub-mkrescue` or `xorriso` to produce `os-build.iso`
- [ ] Verify ISO boots in QEMU
- [ ] Sign the ISO with a checksum (`sha256sum os-build.iso > os-build.iso.sha256`)
- [ ] Commit: `"release: ISO build script"`

---

## 2. Installer Program

**Prompt:** The ISO installer is the capstone feature: boot from ISO, partition a target disk (GPT with EFI System Partition + IXFS root), format both partitions, copy kernel/initrd/OS files, install GRUB for UEFI, and display completion. Test the full cycle in QEMU: boot ISO → install to virtual disk → reboot from disk → OS loads → ✅. After completing all items, create `docs/architecture/installer.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"installer: full OS installer"`.


- [ ] Write `src/installer/installer.c` — runs as a special init process from the ISO
- [ ] Display a **welcome screen** (GUI or text-mode)
- [ ] **Disk selection** — list available drives (ATA/AHCI enumeration)
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

---

## 3. Hyper-V VM Setup

**Prompt:** Set up a Generation 2 Hyper-V VM on the Windows host with 2+ GB RAM, 1+ vCPU, 20+ GB VHDX disk, Secure Boot disabled. Attach the ISO from `build/os-build.iso` via the WSL path. This tests the OS on real Microsoft hardware virtualization (different from QEMU's KVM). After completing all items, mark every item as `[x]` and document results.


- [ ] Enable Hyper-V on Windows host (if not already)
- [ ] Open Hyper-V Manager → **New → Virtual Machine**
- [ ] Choose **Generation 2** VM
- [ ] Allocate ≥ 2 GB RAM, ≥ 1 vCPU
- [ ] Create a virtual hard disk (≥ 20 GB, VHDX)
- [ ] **Disable Secure Boot** (VM Settings → Security → uncheck)
- [ ] Attach ISO: `\\wsl.localhost\Ubuntu\home\<user>\impossible-os\build\os-build.iso`

---

## 4. Installer Validation Tests

**Prompt:** Execute 8 validation tests in sequence: ISO boots to installer, installer partitions and formats, installer copies files and installs bootloader, VM reboots from disk → kernel loads → desktop, keyboard/mouse work, filesystem CRUD works, window manager renders correctly at Hyper-V resolution, graceful ACPI shutdown/reboot. Document any Hyper-V-specific issues. After all tests, mark every item as `[x]`.


- [ ] **Test 1:** ISO boots to installer without errors
- [ ] **Test 2:** Installer partitions and formats the virtual disk
- [ ] **Test 3:** Installer copies OS files and installs bootloader
- [ ] **Test 4:** VM reboots from disk → kernel loads → shell or desktop appears
- [ ] **Test 5:** Keyboard and mouse work inside Hyper-V
- [ ] **Test 6:** Filesystem operations work (create, read, delete files)
- [ ] **Test 7:** Window manager renders correctly at Hyper-V's resolution
- [ ] **Test 8:** Graceful shutdown/reboot via ACPI (see TODO-100-Power-Management.md §9)
- [ ] Document any Hyper-V-specific issues and fixes

---

## 5. Performance & Stability

**Prompt:** Run the OS for 30+ minutes without crash. Stress-test memory allocator (alloc/free loops), stress-test process creation (fork-bomb protection), verify no memory leaks via serial log. Document results. After all items, mark every item as `[x]`, and commit as `"test: Hyper-V validation pass"`.


- [ ] Run the OS for 30+ minutes without crash
- [ ] Stress-test memory allocator (allocate/free in a loop)
- [ ] Stress-test process creation (fork-bomb protection)
- [ ] Verify no memory leaks via serial log inspection
- [ ] Commit: `"test: Hyper-V validation pass"`

---

## 6. VirtualBox Validation *(Stretch)*

- [ ] Create VirtualBox VM (64-bit, EFI, 2+ GB RAM)
- [ ] Attach ISO → boot → verify installer works
- [ ] Test graphics, keyboard, mouse, filesystem
- [ ] Document any VirtualBox-specific issues

---

## Priority Order

| Priority | Section | Reason |
|----------|---------|--------|
| 🔴 P0 | 1. Bootable ISO Creation | Build pipeline |
| 🔴 P0 | 2. Installer Program | Capstone feature |
| 🟠 P1 | 3. Hyper-V VM Setup | Test environment |
| 🟠 P1 | 4. Installer Validation Tests | End-to-end validation |
| 🟡 P2 | 5. Performance & Stability | Soak test |
| 🔵 P4 | 6. VirtualBox Validation | Cross-hypervisor |
