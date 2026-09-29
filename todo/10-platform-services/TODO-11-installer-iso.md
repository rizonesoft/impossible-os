---
schema_version: 1
id: installer-iso
domain: 10-platform-services
status: active
title: "TODO-11 -- OS Installer & ISO Build"
---

# TODO-11 -- OS Installer & ISO Build

**Domain:** `10-platform-services`
**Goal:** Deliver a bootable ISO and graphical installer that make Impossible OS distributable and installable on real hardware -- the capstone that transforms the OS from a QEMU-only raw disk image into a product that ships.

> [!IMPORTANT]
> **Depends on:** `TODO-07 §9` (ring-3 PE execution) and `TODO-08 §10–13` (IxUI windows, user32, gdi32) for the installer GUI. GPT + FAT32 + IXFS format APIs must be available: `fat32_format()` (`include/kernel/fs/fat32.h`), `ixfs_format()` (`include/kernel/fs/ixfs.h`), `gpt_parse()` (`include/kernel/fs/gpt.h`), `blkdev_count()` (`include/kernel/drivers/blkdev.h`).
> **Overlap:** §8 (first-boot trigger) XREFs `TODO-04-restore-recovery §5` (OOBE wizard in this domain); do not duplicate that wizard; trigger it from here.

---

## Important Notes

- `fat32_format(dev, label)` exists in `include/kernel/fs/fat32.h`. `ixfs_format(dev, volume_name)` exists in `include/kernel/fs/ixfs.h`. `gpt_parse()` exists but **GPT write** (`gpt_create`/`gpt_commit`) does not appear to exist -- §4 must implement it.
- `blkdev_count()` and `blkdev_list()` exist in `include/kernel/drivers/blkdev.h` for device enumeration.
- `uefi_set_variable()` / `uefi_get_variable()` exist in `include/kernel/uefi_runtime.h` -- use these to write UEFI `BootNNNN` + `BootOrder` NVRAM entries on real hardware.
- The ISO does **not** use GRUB to boot -- it boots via the existing `BOOTX64.EFI` (our custom UEFI bootloader). `xorriso` creates the El Torito + EFI-bootable hybrid image; `BOOTX64.EFI` goes into `EFI/BOOT/` on the ISO filesystem.
- The installer runs as a **user-mode PE process** (`installer.exe`) via the ring-3 execution path. The kernel detects `HKLM\SYSTEM\InstallerMode=1` in the registry (seeded on the ISO disk image) and launches `installer.exe` instead of `explorer.exe`.
- File copy (§5) copies from the **ISO's read-only VFS** (mounted at boot) to the **target IXFS partition**; the copy list is generated at build time from the Makefile.
- NTFS is not the initial target for the system partition -- IXFS is the native filesystem. NTFS format support (`ntfs_format()`) is not yet available in the kernel; use `ixfs_format()`.

---

## Inputs

| Path                                        | Purpose                                                      |
| ------------------------------------------- | ------------------------------------------------------------ |
| `include/kernel/fs/fat32.h`                 | `fat32_format(dev, label)` for ESP formatting                |
| `include/kernel/fs/ixfs.h`                  | `ixfs_format(dev, volume_name)` for system partition         |
| `include/kernel/fs/gpt.h`                   | `gpt_parse()` -- extend with GPT write                       |
| `include/kernel/drivers/blkdev.h`           | `blkdev_count()`, `blkdev_list()`, `blkdev_write()`          |
| `include/kernel/uefi_runtime.h`             | `uefi_set_variable()` for NVRAM boot entry                   |
| `include/registry.h`                        | `registry_set()` for `InstallerMode`, `FirstBoot` flags      |
| `src/boot/uefi/bootx64.c`                   | Existing bootloader -- copy to ISO `EFI/BOOT/BOOTX64.EFI`    |
| `scripts/build.sh`                          | Add `make iso` target here                                   |
| → XREF: `10-platform-services/TODO-04 §5`   | OOBE first-boot wizard (do not re-implement -- trigger only) |
| → XREF: `05-storage-filesystems/TODO-08 §5` | GPT partition table write (reuse if available)               |
| → XREF: `TODO-08 §10–13`                    | IxUI window/message/GDI stack for installer GUI              |

---

## Outcome

- `scripts/make-iso.sh` produces `build/os-build.iso` that boots in QEMU, Hyper-V Gen2, and VirtualBox EFI.
- The installer GUI walks through Welcome → License → Disk Selection → Partitioning → Review → Progress → Complete.
- Target disk is partitioned (GPT, 512 MiB FAT32 ESP + remainder IXFS), formatted, and populated with all OS files.
- `BOOTX64.EFI` is installed to the ESP; UEFI NVRAM boot entry is written on real hardware.
- On reboot from installed disk, the kernel loads and the OOBE first-boot wizard runs.
- OS passes 8 validation tests on QEMU and Hyper-V Gen2.

---

## Implementation Order

| #   | Section                                              | Tag        | Dep                | Mark |
| --- | ---------------------------------------------------- | ---------- | ------------------ | ---- |
| 1   | ISO build script (`make-iso.sh`, `make iso`)         | `[Sonnet]` | existing build     | ⭐   |
| 2   | Installer init process (InstallerMode registry flag) | `[Sonnet]` | TODO-08 §4         | ⭐   |
| 3   | Installer GUI wizard (IxUI screens A–G)              | `[Sonnet]` | §2, TODO-08 §10–13 | 💎   |
| 4   | GPT partition write (`gpt_create`/`gpt_commit`)      | `[Opus]`   | §3                 | 💎   |
| 5   | Partition format (ESP FAT32 + IXFS system)           | `[Sonnet]` | §4                 | 💎   |
| 6   | File copy engine (ISO VFS → target IXFS)             | `[Sonnet]` | §5                 | 💎   |
| 7   | UEFI bootloader install + NVRAM boot entry           | `[Opus]`   | §5–6               | 💎   |
| 8   | Post-install first boot + OOBE trigger               | `[Sonnet]` | §7                 | 💎   |
| 9   | Validation: QEMU + Hyper-V Gen2 + VirtualBox         | `[Sonnet]` | §1–8               | 💎   |
| 10  | Stability + performance pass                         | `[Sonnet]` | §9                 | ⭐   |

---

## 1. ISO Build Script `[Sonnet]`

Create `scripts/make-iso.sh` to produce a hybrid El Torito + EFI-bootable ISO from the existing build output. Does not require GRUB -- uses `BOOTX64.EFI` directly.

- [ ] ISO directory structure assembled at `build/iso-root/`:
  - `EFI/BOOT/BOOTX64.EFI` -- copy from `build/bootx64.efi`
  - `boot/kernel.exe` -- copy from `build/kernel.elf`
  - `boot/fonts/`, `boot/icons/`, `boot/wallpaper/` -- from `resources/`
  - `Impossible/System32/` -- system DLLs and apps (from `build/user/`)
  - `Impossible/Fonts/` -- TTF fonts from `resources/fonts/`
  - `installer.exe` -- installer PE (from `build/user/installer.exe`)
  - `EFI/BOOT/startup.nsh` -- optional UEFI shell script: `\EFI\BOOT\BOOTX64.EFI`
- [ ] `xorriso -as mkisofs` command:
  - `-R -J -joliet-long` (Rock Ridge + Joliet for long filenames)
  - `-e EFI/BOOT/BOOTX64.EFI -no-emul-boot` (EFI El Torito boot entry)
  - `-efi-boot-part --efi-boot-image` (create EFI system partition image)
  - Output: `build/os-build.iso`
- [ ] `sha256sum build/os-build.iso > build/os-build.iso.sha256`
- [ ] Add `make iso` target to `Makefile` / `scripts/build.sh`: `bash scripts/make-iso.sh`
- [ ] Verify: `bash scripts/build.sh && make iso` → `build/os-build.iso` exists, size > 10 MiB
- [ ] Test: `qemu-system-x86_64 -cdrom build/os-build.iso -bios /usr/share/ovmf/OVMF.fd -boot d` → kernel loads from ISO
- [ ] Commit: `"release: ISO build script (xorriso, EFI El Torito)"`

---

## 2. Installer Init Process `[Sonnet]`

The installer runs as a user-mode PE process rather than a special kernel mode. The kernel detects installer mode via a registry flag baked into the ISO disk image. Network-boot entry into installer mode (fetch assets over TFTP/HTTP, verify, then set `InstallerMode`) is owned by `01-boot-platform/TODO-25 §8` -- the same-kernel installer, no separate WinPE/initramfs.

- [ ] Pre-bake `HKLM\SYSTEM\InstallerMode = 1` (REG_DWORD) into the registry hive on the ISO image (set during `make-iso.sh` via a host-side registry tool or by including a pre-built hive)
- [ ] In `src/kernel/main/kernel_main.c` (or desktop init path): after mounting root VFS, read `HKLM\SYSTEM\InstallerMode`; if `1` → launch `installer.exe` via `pe_exec("C:\\installer.exe")` instead of `explorer.exe`
- [ ] Create `src/installer/installer.c` -- PE `main()` entry point; initializes IxUI message loop; allocates installer state struct; calls §3 wizard
- [ ] Installer state struct: `installer_state_t { int screen; char target_disk[64]; uint64_t target_lba_start; uint64_t target_size_sectors; int partition_mode; /* AUTO=0, MANUAL=1 */ }`
- [ ] Serial log on entry: `"installer: started, mode=%d"` (InstallerMode value)
- [ ] Commit: `"installer: init process and InstallerMode flag"`

---

## 3. Installer GUI Wizard `[Sonnet]`

**Design:** [`shell.md#first-run-setup`](../../docs/design/shell.md#first-run-setup)

Seven IxUI wizard screens laid out per `docs/design/shell.md#first-run-setup`: full-screen dark silk wallpaper (`silk-dark.jpg`) under acrylic, a centred 800 x 600 card (radius 8, `window_bg`, `THEME_ELEV_START_*`) with an illustration or icon on the left third and the step on the right (title style 28/36, body style, controls per `docs/design/controls.md`). Navigation: accent `[Next]` / `[Install]` / `[Reboot]` bottom right, standard `[Back]` beside it.

**Screen A -- Welcome:**
- [ ] Title: "Install Impossible OS v1.0" (large `ttf_draw_string`)
- [ ] Subtitle: "This wizard will guide you through installing Impossible OS on your computer."
- [ ] Language selector: `CTRL_LISTBOX` with "English (US)" only (future: add more); selection stored in `installer_state.lang`
- [ ] `[Next →]` button advances to Screen B

**Screen B -- License Agreement:**
- [ ] Full license text in a `CTRL_SCROLLBAR`-wrapped `CTRL_LABEL` (read from `C:\Impossible\license.txt`)
- [ ] `CTRL_CHECKBOX` "I accept the terms of the license agreement"
- [ ] `[Next →]` disabled until checkbox checked

**Screen C -- Disk Selection:**
- [ ] Call `blkdev_count()` + enumerate block devices; show `CTRL_LISTBOX` with: drive index, model string (from AHCI/VirtIO identity), size in GiB
- [ ] Warn if selected disk < 2 GiB: "Disk too small -- minimum 2 GiB required"
- [ ] Store selection in `installer_state.target_disk`

**Screen D -- Partitioning:**
- [ ] Two radio buttons: "Automatic (recommended)" and "Manual"
- [ ] Automatic: preview layout -- "ESP: 512 MiB (FAT32) + System: N GiB (IXFS)"
- [ ] Manual: launches diskpart-style modal dialog: list partitions, `[New]` / `[Delete]` / `[Format]` buttons; user must manually create ESP (type `EF00`) and system partition before confirming
- [ ] Store `partition_mode` in installer state

**Screen E -- Review:**
- [ ] Summary table: target disk name + size, partitioning plan, filesystem types
- [ ] Warning: "All data on the selected disk will be erased"
- [ ] `[Install]` accent button → triggers §4–7 in sequence; the destructive nature is stated in the step text and a confirmation dialog (`docs/design/controls.md#dialog`)
  - Destructive actions are standard buttons behind a confirmation dialog whose default is the safe choice (`docs/design/controls.md#status-colours`); do not use `caption_close_hover` red for buttons

**Screen F -- Progress:**
- [ ] `CTRL_LABEL` status line: "Partitioning disk…" / "Formatting ESP…" / "Copying files…" / "Installing bootloader…"
- [ ] `CTRL_PROGRESSBAR` (or custom filled rect) updated per-file during copy (§6)
- [ ] Errors displayed inline: "Error: disk write failed" with `[Abort]` button
- [ ] No `[Back]` during progress (installation in flight)

**Screen G -- Complete:**
- [ ] "Installation successful! Remove the installation media and reboot."
- [ ] `[Reboot Now]` button → `ExitProcess()` with a special code that triggers kernel reboot via ACPI; alternatively write `HKLM\SYSTEM\RebootPending=1` and exit

- [ ] Commit: `"installer: GUI wizard screens A–G"`

---

## 4. GPT Partition Write `[Opus]`

Implement `gpt_create()` and `gpt_commit()` in `src/kernel/fs/gpt.c` (header: `include/kernel/fs/gpt.h`). This is a security-critical disk write path -- incorrect GPT CRC or LBA placement corrupts the target disk.

- [ ] `gpt_new(dev)` → allocate and zero `struct gpt_table`; generate random disk GUID (`getrandom()` or kernel RNG); set `disk_guid`
- [ ] `gpt_add_partition(table, name, type_guid, lba_start, lba_end, attributes)` → append entry to `table->entries[]`; validate no overlaps; return partition index or error
- [ ] GPT GUID constants: reuse `GPT_GUID_EFI_SYSTEM` and `GPT_GUID_IXFS` from `include/kernel/fs/gpt.h` (ESP first, IXFS system second)
- [ ] `gpt_commit(table, dev)`:
  - Write MBR protective partition (LBA 0): type `0xEE`, full disk span
  - Write primary GPT header at LBA 1: signature `"EFI PART"`, version 1.0, header CRC32 (zeroed field during CRC calc), partition table LBA = 2, entry size = 128, entry count, first/last usable LBAs
  - Write primary partition entries at LBA 2–33 (128 bytes × 128 entries = 32 sectors)
  - Compute partition entry array CRC32; store in primary header
  - Recompute and store header CRC32
  - Write backup partition entries at (last_lba - 32) to (last_lba - 1)
  - Write backup GPT header at last_lba (mirror of primary with swapped MyLBA/AlternateLBA)
  - Use `blkdev_write()` for all sector writes; verify write return code
- [ ] `gpt_calculate_partition_layout(dev, esp_size_sectors, *esp_start, *esp_end, *sys_start, *sys_end)`:
  - LBA 0 = MBR, LBA 1 = primary GPT header, LBA 2–33 = entries, LBA 34 = first usable
  - ESP starts at LBA 34; ends at LBA 34 + `esp_size_sectors - 1`
  - System partition: LBA = ESP end + 1 through last usable (last_lba - 33)
- [ ] Expose `gpt_create`, `gpt_add_partition`, `gpt_commit`, `gpt_calculate_partition_layout` in `include/kernel/fs/gpt.h`
- [ ] Commit: `"fs: GPT partition table write (create + commit)"`

---

## 5. Partition Format (ESP FAT32 + IXFS System) `[Sonnet]`

- [ ] After `gpt_commit()`, open each partition as a sub-device (offset `blkdev`): `blkdev_subdev(parent_dev, lba_start, lba_count)` → new `struct blkdev` scoped to the partition
- [ ] Format ESP: `fat32_format(esp_dev, "EFI")` → returns 0 on success; log result to serial + update Screen F status
- [ ] Format system partition: `ixfs_format(sys_dev, "ImpossibleOS")` → returns 0 on success
- [ ] Verify formats: mount ESP via `fat32_mount()` and IXFS via `ixfs_mount()`; read back volume label; unmount
- [ ] Create required directory tree on IXFS immediately after format: `C:\Impossible\`, `C:\Impossible\System32\`, `C:\Impossible\Fonts\`, `C:\Impossible\Icons\`, `C:\Impossible\Bin\`, `C:\Users\Default\`, `C:\Temp\`, `C:\Program Files\`
- [ ] Implement `blkdev_subdev(parent, lba_start, count)` if not already present (thin wrapper that offsets all `read`/`write` calls by `lba_start`)
- [ ] Commit: `"installer: partition format (FAT32 ESP + IXFS system)"`

---

## 6. File Copy Engine `[Sonnet]`

Copy all OS files from the ISO's read-only VFS to the installed IXFS partition. A build-time generated copy manifest drives the copy loop.

- [ ] Generate `build/install-manifest.txt` at build time (added to `Makefile`): one `src_path|dst_path` entry per file to install; examples:
  - `\boot\kernel.exe | C:\Impossible\kernel.exe`
  - `\Impossible\System32\cmd.exe | C:\Impossible\System32\cmd.exe`
  - `\Impossible\Fonts\*.ttf | C:\Impossible\Fonts\`
  - `\Impossible\Icons\icons.ires | C:\Impossible\Icons\icons.ires`
  - `\installer\installer.reg | C:\Windows\System32\config\SYSTEM` (registry hive seed)
- [ ] `installer_copy_files(manifest_path, progress_cb)`:
  - Open manifest via VFS; parse line by line
  - For each entry: `vfs_open(src)` → `vfs_stat()` for size → `vfs_read()` chunks of 64 KiB → `vfs_write()` to destination; call `progress_cb(files_done, files_total, current_filename)` after each file
  - Handle wildcard `*` entries: `FindFirstFile` on source dir, iterate matches
  - On any write error: log to serial + display error on Screen F + return failure code
- [ ] `progress_cb` updates Screen F: moves progress bar + updates status label
- [ ] Total file count shown in Screen F: "Copying file N of M: filename"
- [ ] After copy: write `HKLM\SYSTEM\InstallerMode = 0` to the installed registry hive (so the installed OS boots normally)
- [ ] Commit: `"installer: file copy engine with progress callback"`

---

## 7. UEFI Bootloader Install + NVRAM Boot Entry `[Opus]`

Install `BOOTX64.EFI` to the ESP and register a UEFI boot entry so the firmware boots Impossible OS first.

- [ ] Copy `BOOTX64.EFI` to ESP: `EFI\BOOT\BOOTX64.EFI` (fallback path -- all UEFI firmware boots this automatically, no NVRAM required for the fallback case)
- [ ] Copy `BOOTX64.EFI` to ESP: `EFI\ImpossibleOS\BOOTX64.EFI` (named path for NVRAM entry)
- [ ] Write UEFI NVRAM boot entry via `uefi_set_variable()`:
  - Scan existing `BootNNNN` variables to find a free slot (try `Boot0001` through `Boot00FF`)
  - Construct `EFI_LOAD_OPTION` struct: `Attributes = LOAD_OPTION_ACTIVE (0x1)`, `FilePathList` = `MEDIA_FILEPATH_DP` pointing to `\EFI\ImpossibleOS\BOOTX64.EFI` on the ESP partition (identified by partition GUID from GPT), description = `"Impossible OS"`
  - `uefi_set_variable(EFI_GLOBAL_GUID, "BootNNNN", EFI_VARIABLE_NV | EFI_VARIABLE_BS | EFI_VARIABLE_RT, entry_data, entry_size)`
  - Update `BootOrder` variable: prepend our `BootNNNN` number to existing order array
- [ ] QEMU/VM fallback: if `uefi_set_variable()` returns error (firmware read-only in some VMs), log warning and rely on fallback path `EFI/BOOT/BOOTX64.EFI` -- this is sufficient for QEMU + Hyper-V + VirtualBox
- [ ] Verify: after install, `BOOTX64.EFI` file exists on mounted ESP at correct path; serial log shows NVRAM write result
- [ ] Commit: `"installer: UEFI bootloader install + NVRAM boot entry"`

---

## 8. Post-Install First Boot + OOBE Trigger `[Sonnet]`

**Design:** [`shell.md#first-run-setup`](../../docs/design/shell.md#first-run-setup)

- [ ] After §7 completes: write `HKLM\SYSTEM\FirstBoot = 1` to installed registry hive (signals OOBE wizard on first boot)
- [ ] Write `HKLM\SYSTEM\InstallerMode = 0` to installed registry hive (ensures normal boot path next time)
- [ ] Write `HKLM\SYSTEM\InstallDate` = current timestamp (for system info display)
- [ ] Screen G `[Reboot Now]` → eject ISO media (clear QEMU `-cdrom` equivalent) + ACPI reset via UEFI runtime `ResetSystem(EfiResetCold)`
- [ ] On first boot from installed disk: kernel reads `FirstBoot=1` → launches OOBE wizard from `TODO-04 §5` (timezone, keyboard, user account, wallpaper, updates); OOBE wizard writes `FirstBoot=0` when complete
- [ ] Commit: `"installer: post-install first boot and OOBE trigger"`

---

## 9. Validation: QEMU + Hyper-V Gen2 + VirtualBox `[Sonnet]`

**QEMU (primary):**
- [ ] T1: `qemu-system-x86_64 -cdrom build/os-build.iso -boot d` → installer Welcome screen appears
- [ ] T2: Complete wizard → installer partitions + formats virtual disk (no crash, serial shows GPT commit + format success)
- [ ] T3: File copy completes (Screen F reaches 100%); Screen G appears
- [ ] T4: `[Reboot Now]` → QEMU boots from virtual disk → kernel loads → shell or desktop

**Hyper-V Gen2 (Windows host):**
- [ ] Create Gen2 VM: 2+ GiB RAM, 1+ vCPU, 20+ GiB VHDX, Secure Boot disabled, attach `build/os-build.iso`
- [ ] T5: Installer runs; keyboard + mouse work in Hyper-V (test CTRL_LISTBOX navigation)
- [ ] T6: After install and reboot: filesystem CRUD works (create/read/delete files from shell)
- [ ] T7: Window manager renders at Hyper-V's GOP resolution (no garbled framebuffer)
- [ ] T8: `shutdown -s` in shell → ACPI `ResetSystem(EfiResetShutdown)` → VM powers off gracefully

**VirtualBox (stretch):**
- [ ] Create VM: 64-bit EFI, Secure Boot off, 2+ GiB RAM; attach ISO
- [ ] T9: Installer boots and completes; reboot from disk loads OS

- [ ] Document any hypervisor-specific issues found during testing (add to `docs/guides/hypervisor-compat.md`)
- [ ] Commit: `"test: installer validation pass (QEMU + Hyper-V + VirtualBox)"`

---

## 10. Stability + Performance Pass `[Sonnet]`

- [ ] Boot installed OS and run for 30+ minutes without crash; serial log must show no panics or assertion failures
- [ ] Memory stress: allocate/free 1 MiB blocks in a loop for 5 minutes via test PE; verify PMM free list remains consistent
- [ ] Process stress: spawn 50 processes sequentially; verify scheduler returns to idle cleanly
- [ ] Disk stress: write 1000 files of 4 KiB each to `C:\Temp\`; read each back and checksum; delete all
- [ ] Verify installed OS cold-boot time (from power-on to desktop/shell prompt) < 10 seconds in QEMU
- [ ] Commit: `"test: 30-minute stability pass + performance baseline"`

---

## OS Comparison


| ⭐  | Feature                             | 🪟 Win11          | 🐧 Linux                | 🚀 Impossible OS                      |
| --- | ----------------------------------- | ----------------- | ----------------------- | ------------------------------------- |
| 💎  | Bootable ISO image                  | ✅ Windows ISO    | ✅ distro ISO           | ⬜ `xorriso` EFI El Torito            |
| 💎  | Graphical installer wizard          | ✅ Windows Setup  | ✅ Anaconda/Calamares   | ⬜ IxUI wizard (A–G screens)          |
| 💎  | GPT partitioning during install     | ✅ Windows Setup  | ✅ distro installers    | ⬜ `gpt_create`/`gpt_commit`          |
| 💎  | FAT32 ESP + native FS formatting    | ✅ Windows Setup  | ✅ mkfs.fat + mkfs.ext4 | ⬜ `fat32_format` + `ixfs_format`     |
| 💎  | UEFI NVRAM boot entry registration  | ✅ Windows Setup  | ✅ grub-install         | ⬜ `uefi_set_variable()`              |
| 💎  | File copy progress bar              | ✅ Windows Setup  | ✅ distro installers    | ⬜ per-file `progress_cb`             |
| 💎  | Post-install OOBE first-boot wizard | ✅ Windows OOBE   | ✅ distro firstboot     | ⬜ `HKLM\SYSTEM\FirstBoot=1`          |
| 💎  | Hyper-V + VirtualBox + QEMU compat  | ✅ Windows        | ✅ Linux                | ⬜ §9 -- validation suite             |
| ⭐  | Custom UEFI bootloader              | ❌ Bootmgr only   | ❌ Requires GRUB        | ⬜ `BOOTX64.EFI` direct boot from ISO |
| ⭐  | Build-time install manifest         | ❌ Black-box WIM  | ❌ Varies per distro    | ⬜ transparent, diff-able file list   |
| ⭐  | `InstallerMode` registry flag       | ❌ Separate WinPE | ❌ Separate initramfs   | ⬜ same kernel, flag-switched path    |

**Impossible OS advantage:** The installer uses the **exact same kernel** as the installed OS -- there is no separate WinPE or initramfs environment. A single registry flag (`InstallerMode=1`) switches the boot into installer mode. The bootloader is our own `BOOTX64.EFI` with no GRUB dependency, and the install manifest is a human-readable build artifact that makes the file copy process fully transparent.

---

## Verification

**ISO build:**
- `bash scripts/build.sh && make iso` exits 0; `build/os-build.iso` > 10 MiB; `os-build.iso.sha256` present
- `qemu-system-x86_64 -cdrom build/os-build.iso -bios OVMF.fd -boot d` → installer Welcome screen on serial + framebuffer

**Partitioning:**
- After Screen E `[Install]` → serial shows: `"gpt_commit: wrote primary + backup GPT"`, `"fat32_format: ESP formatted"`, `"ixfs_format: system partition formatted"`
- Post-install: `gpt_parse()` on target disk returns 2 valid partitions (ESP type `EF00`, IXFS basic data)

**File copy:**
- Screen F progress bar advances per file; serial logs each copied filename
- After copy: `dir C:\Impossible\System32\` on installed OS shows expected binaries

**First boot:**
- After `[Reboot Now]` and removing ISO: kernel boots from IXFS; `HKLM\SYSTEM\InstallerMode = 0`; OOBE wizard appears
- OOBE completes: `HKLM\SYSTEM\FirstBoot = 0`; normal desktop/shell launches

**Stability:**
- 30-minute run: zero panics in serial log
- Cold boot time in QEMU < 10 seconds from UEFI handoff to shell prompt
