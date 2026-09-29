<!-- docs: covers=todo/10-platform-services/TODO-11-installer-iso.md sources=scripts/release/build-image.sh,scripts/release/build-iso.sh,scripts/release/boot-test-iso.sh,scripts/release/verify-esp.sh,include/kernel/fs/gpt.h,src/kernel/fs/gpt.c,include/kernel/fs/fat32.h,include/kernel/fs/ixfs.h,include/kernel/drivers/blkdev.h,include/kernel/uefi_runtime.h,src/installer reviewed=2026-09-29 order=11 -->
# Installer and ISO Build

## What is it?

This roadmap turns Impossible OS from a QEMU disk image into something a user can install: a bootable ISO, an installer that starts from it in a special mode of the same kernel, a graphical wizard, and the steps that partition a disk, format it, copy the system, register the boot entry and hand off to first-boot setup. None of its ten sections has shipped, and `src/installer/` is an empty placeholder. A release-side disk image and hybrid ISO can already be built, and the kernel has most of the disk primitives the installer will call.

## How does it work?

**Today: release images.** Two host scripts, owned by the [Boot Media, Image Pipeline and Installer Handoff](../boot/boot-media-image-pipeline.md) roadmap, produce the media:

- [`build-image.sh`](../../scripts/release/build-image.sh) writes `build/release/disk.img`, a deterministic 512 MiB GPT image with three partitions: a 64 MiB FAT32 ESP holding `BOOTX64.EFI`, `kernel.exe` and `boot.conf`; a 128 MiB FAT32 BlackBox partition for logs and crash records; and an IXFS system partition that fills the rest, formatted and populated from the build's sysroot by `mkfs-ixfs --populate` (the script refuses to run without `cmd.exe` in the sysroot).
- [`build-iso.sh`](../../scripts/release/build-iso.sh) turns it into `build/release/disk.iso`, a UEFI-only ISO whose El Torito boot entry is the ESP image, plus `/IPOS/manifest.json` and empty `/IPOS/installer/` and `/IPOS/recovery/` folders for the payloads this roadmap will supply. [`boot-test-iso.sh`](../../scripts/release/boot-test-iso.sh) boots it in QEMU and passes on `Boot complete in` (the ISO carries only the ESP, so there is no `C:\>` prompt), and [`verify-esp.sh`](../../scripts/release/verify-esp.sh) checks the ESP.

Both are reproducible: timestamps come from `SOURCE_DATE_EPOCH=0` and GUIDs from a seed pinned in the manifest.

**Today: kernel primitives.**

- **GPT.** `gpt_parse()`, `guid_generate()`, `gpt_crc32()` and `gpt_sync_backup()`, which already writes the backup header and entry array, with partition type GUIDs such as `GPT_GUID_EFI_SYSTEM` and `GPT_GUID_IXFS` ([`gpt.h`](../../include/kernel/fs/gpt.h)). There is no call yet to create a new table.
- **Formatting.** `fat32_format(dev, label)` ([`fat32.h`](../../include/kernel/fs/fat32.h)) and `ixfs_format(dev, name)` ([`ixfs.h`](../../include/kernel/fs/ixfs.h)).
- **Disks.** `struct blkdev`, `blkdev_count()` and `blkdev_write()` ([`blkdev.h`](../../include/kernel/drivers/blkdev.h)).
- **Boot entries.** `uefi_get_variable()` and `uefi_set_variable()` ([`uefi_runtime.h`](../../include/kernel/uefi_runtime.h)).

**Planned design.**

1. **ISO build.** `scripts/make-iso.sh` and `make iso` producing an ISO that boots `BOOTX64.EFI` directly, with no GRUB.
2. **Installer mode.** The same kernel boots with an `InstallerMode` flag and starts `installer.exe` instead of the desktop.
3. **Wizard.** Seven screens: welcome, licence, disk selection, partitioning, review, progress and complete.
4. **Partition, format, copy.** A new GPT (ESP, then the system partition), FAT32 and IXFS formatting, and a file copy driven by a build-time install manifest.
5. **Boot entry.** Copy `BOOTX64.EFI` to the new ESP and write a UEFI `Boot####` entry and `BootOrder`.
6. **First boot.** Set the first-boot flag so the [setup wizard](restore-recovery.md) runs, then validate on QEMU, Hyper-V Generation 2 and VirtualBox.

## What are its interfaces?

| Interface | Status |
| --- | --- |
| `build-image.sh`, `build-iso.sh`, `boot-test-iso.sh`, `verify-esp.sh` | Shipped (release side) |
| `gpt_parse()`, `gpt_sync_backup()`, `guid_generate()`, `fat32_format()`, `ixfs_format()` | Shipped |
| `uefi_get_variable()`, `uefi_set_variable()` | Shipped |
| `scripts/make-iso.sh`, `make iso` | Planned |
| `InstallerMode`, `installer.exe`, GPT creation, file copy engine, install manifest | Planned |

## How do I use it?

```bash
bash scripts/build.sh
bash scripts/release/build-image.sh     # build/release/disk.img
bash scripts/release/build-iso.sh       # build/release/disk.iso
bash scripts/release/boot-test-iso.sh   # boot the ISO in QEMU, expect "Boot complete in"
```

The disk image carries the whole system in its populated IXFS partition. The ISO carries only the ESP image, so it boots the kernel but has no system partition and no installer yet.

## What is not implemented yet?

- [ISO Build Script](../../todo/10-platform-services/TODO-11-installer-iso.md#1-iso-build-script-sonnet); Joliet, Rock Ridge and versioned file names are owned by the [release artifacts roadmap](../../todo/15-installer-release/TODO-01-release-artifacts.md)
- [Installer Init Process](../../todo/10-platform-services/TODO-11-installer-iso.md#2-installer-init-process-sonnet) and the [GUI Wizard](../../todo/10-platform-services/TODO-11-installer-iso.md#3-installer-gui-wizard-sonnet)
- [GPT Partition Write](../../todo/10-platform-services/TODO-11-installer-iso.md#4-gpt-partition-write-opus), [Partition Format](../../todo/10-platform-services/TODO-11-installer-iso.md#5-partition-format-esp-fat32--ixfs-system-sonnet) and the [File Copy Engine](../../todo/10-platform-services/TODO-11-installer-iso.md#6-file-copy-engine-sonnet)
- [UEFI Bootloader Install](../../todo/10-platform-services/TODO-11-installer-iso.md#7-uefi-bootloader-install--nvram-boot-entry-opus) and the [first-boot trigger](../../todo/10-platform-services/TODO-11-installer-iso.md#8-post-install-first-boot--oobe-trigger-sonnet)
- [Validation](../../todo/10-platform-services/TODO-11-installer-iso.md#9-validation-qemu--hyper-v-gen2--virtualbox-sonnet) and the [Stability and Performance Pass](../../todo/10-platform-services/TODO-11-installer-iso.md#10-stability--performance-pass-sonnet)

Unattended installs are planned in the [unattended install roadmap](../../todo/15-installer-release/TODO-02-unattended-install.md).

## How does it compare with Windows 11 and Linux?

Windows 11 installs from Windows Setup running in WinPE, a separate minimal Windows, and finishes in OOBE; it boots through Boot Manager. Linux installers such as Anaconda and Calamares run from a live system or initramfs and install GRUB or systemd-boot. The Impossible OS plan uses the exact kernel that will be installed, started in an installer mode, and its own UEFI loader with no GRUB, driven by a manifest produced at build time.

## See also

- [OS Installer and ISO Build roadmap](../../todo/10-platform-services/TODO-11-installer-iso.md)
- [Boot Media, Image Pipeline and Installer Handoff](../boot/boot-media-image-pipeline.md)
- [Recovery Partition](../boot/recovery-partition.md)
- [IXFS Core](../storage/ixfs-core.md)
- [Partition Management and Storage Tools](../storage/partition-tools.md)
- [System Restore, Recovery and Observability](restore-recovery.md)
