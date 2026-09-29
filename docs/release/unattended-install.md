<!-- docs: covers=todo/15-installer-release/TODO-02-unattended-install.md sources=src/installer/.gitkeep,scripts/release/build-iso.sh,scripts/machines/run-qemu-kvm.sh,scripts/machines/run-vbox.sh,include/kernel/boot_info.h reviewed=2026-09-29 order=4 -->
# Unattended Installation and Deployment

## What is it?

This roadmap plans hands-off installation for fleets and virtual machines: an `answer.ini` file that answers every installer question, an unattended mode that runs the installer without a screen, `sysprep.exe` to generalize a machine before imaging, an OEM customization folder, `imagex.exe` to capture and apply system images, a PXE and TFTP network boot server, VM provisioning scripts and an enterprise deployment guide. None of its eight sections has shipped, and it depends on an installer that does not exist yet either.

## How does it work?

**Today.** There is no installer to automate. `src/installer/` holds only a placeholder, and the release ISO built by [`build-iso.sh`](../../scripts/release/build-iso.sh) carries the boot partition only, so it boots the kernel but installs nothing (see [Installer and ISO Build](../services/installer-iso.md)). Developers start virtual machines with the launchers in `scripts/machines/`, such as [`run-qemu-kvm.sh`](../../scripts/machines/run-qemu-kvm.sh) and [`run-vbox.sh`](../../scripts/machines/run-vbox.sh), which boot a prebuilt disk image rather than provisioning a fresh install. The boot handoff already has a `cmdline` field in [`boot_info.h`](../../include/kernel/boot_info.h), which section 1 plans to use for an `answer=<path>` token.

**Planned design.**

1. **Answer file.** An INI file found from the kernel command line, the install media root or the Registry, covering disk, partitions, locale, user, network, OEM and packages.
2. **Unattended mode.** When an answer file is present, the installer skips every dialog, runs the same disk, format, copy and first-boot steps, and writes `setup.log`.
3. **sysprep.** `sysprep.exe /generalize` gives the machine a new identity (a 96-bit machine SID from the CSPRNG), clears host name, credentials and logs, and seals it for first-boot setup.
4. **OEM and images.** A `$OEM$` folder adds drivers, Registry entries, branding and packages; `imagex.exe` captures a system into a `.iim` image with single-instance file storage and applies it back.
5. **Network boot and VMs.** A TFTP server with a DHCP proxy for PXE clients, and scripts that provision QEMU, Hyper-V and CI machines from an answer file.

```mermaid
flowchart LR
    A[answer.ini] --> U[unattended installer]
    U --> F[first boot]
    F --> S[sysprep /generalize]
    S --> C[imagex /capture]
    C --> P[PXE or VM deploy]
```

## What are its interfaces?

| Interface | Status |
| --- | --- |
| `boot_info->cmdline` | Shipped (boot handoff) |
| Developer VM launchers in `scripts/machines/` | Shipped (not provisioning) |
| `answer_parse()` and `answer.ini` | Planned in section 1 |
| Unattended installer mode, `setup.log` | Planned in section 2 |
| `sysprep.exe` | Planned in section 3 |
| `$OEM$` layout | Planned in section 4 |
| `imagex.exe` and the `.iim` format | Planned in section 5 |
| `pxesrv` TFTP server | Planned in section 6 |
| `provision-qemu.sh`, `provision-hyperv.ps1`, `ci-provision.sh` | Planned in section 7 |

## How do I use it?

Nothing in this roadmap can be used yet. To boot the current image in a virtual machine, follow [Boot Artifacts: Build, Verify, Write](boot-artifacts.md) or the launcher table in the [Machine Launcher and Debug Profile Matrix](../infrastructure/machine-matrix.md).

## Who owns what?

The installer pipeline itself (disk selection, partitioning, formatting, file copy, boot entry) belongs to the [Installer and ISO Build](../services/installer-iso.md) roadmap (`10-platform-services/TODO-11`); this file only drives it without a UI and must not re-specify it. First-boot setup belongs to the [System Restore, Recovery and Observability](../services/restore-recovery.md) roadmap's section 5. The machine matrix page names section 7 here as the owner of provisioning templates, and the release QA roadmap reuses a CI answer file from that section for its virtual machine certification runs.

## What is not implemented yet?

- [Answer File Format and INI Parser](../../todo/15-installer-release/TODO-02-unattended-install.md#1-answer-file-format--ini-parser-sonnet)
- [Unattended Install Mode](../../todo/15-installer-release/TODO-02-unattended-install.md#2-unattended-install-mode-sonnet), which needs the installer first
- [`sysprep.exe`](../../todo/15-installer-release/TODO-02-unattended-install.md#3-sysprepexe-generalize--oobe-seal-opus)
- [OEM Customization](../../todo/15-installer-release/TODO-02-unattended-install.md#4-oem-customization-sonnet)
- [WIM and Image Capture](../../todo/15-installer-release/TODO-02-unattended-install.md#5-wim--image-capture-imagexexe-opus)
- [Network Boot (PXE and TFTP Server)](../../todo/15-installer-release/TODO-02-unattended-install.md#6-network-boot-pxe--tftp-server-opus)
- [VM Provisioning Templates](../../todo/15-installer-release/TODO-02-unattended-install.md#7-vm-provisioning-templates-sonnet)
- [Enterprise Deployment Guide](../../todo/15-installer-release/TODO-02-unattended-install.md#8-enterprise-deployment-guide-sonnet)

## How does it compare with Windows 11 and Linux?

Windows uses `unattend.xml`, `sysprep`, `$OEM$` folders, WIM images managed with DISM and network deployment through Windows Deployment Services. Linux distributions use Kickstart, preseed or AutoYaST answer files, `virt-sysprep` or cloud-init to generalize, disk images or Clonezilla to capture, and dnsmasq with iPXE to boot over the network. The Impossible OS plan keeps the Windows shape with a simpler INI answer file and a native image format, and ships the provisioning scripts for QEMU and Hyper-V alongside it.

## See also

- [Unattended Installation and Deployment roadmap](../../todo/15-installer-release/TODO-02-unattended-install.md)
- [Installer and ISO Build](../services/installer-iso.md)
- [Boot Artifacts: Build, Verify, Write](boot-artifacts.md)
- [Machine Launcher and Debug Profile Matrix](../infrastructure/machine-matrix.md)
