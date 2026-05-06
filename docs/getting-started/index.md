# Getting Started

Setup guides and emulator configuration for running Impossible OS.

## Documents

| Document                     | Topics                                                   |
| ---------------------------- | -------------------------------------------------------- |
| [QEMU](qemu.md)             | QEMU setup, OVMF firmware, AHCI disk, serial output     |
| [VirtualBox](virtualbox.md)  | VirtualBox VM creation, EFI settings, VDI format         |

## Picking the right artifact

Once you have built the OS (`bash scripts/build.sh`), choose the artifact format that matches your platform:

| You want to ...                                    | Use this format     | Recipe                                                                  |
| -------------------------------------------------- | ------------------- | ----------------------------------------------------------------------- |
| Boot in QEMU on Linux / WSL2                       | raw `.img`          | [Raw disk image](../release/boot-artifacts.md#raw-disk-image-buildreleasediskimg) |
| Write to a USB stick and boot bare metal           | usb (raw `.img`)    | [USB-bootable image](../release/boot-artifacts.md#usb-bootable-image-same-artifact-different-medium) |
| Boot in Hyper-V                                    | vhdx                | [VHD / VHDX](../release/boot-artifacts.md#vhd--vhdx-hyper-v)           |
| Boot in VirtualBox                                 | vdi                 | [VDI](../release/boot-artifacts.md#vdi-virtualbox)                      |
| Burn / image-mount as a bootable CD                | iso                 | [Hybrid ISO](../release/boot-artifacts.md#hybrid-iso-uefi-el-torito)   |

Full per-format recipes, verification flows (`bootimg inspect`), Secure Boot setup, and troubleshooting are in [Boot Artifacts: Build, Verify, Write](../release/boot-artifacts.md).

## See Also

- [Boot Artifacts: Build, Verify, Write](../release/boot-artifacts.md) -- per-format build, write, verify, troubleshoot
- [Infrastructure → Development Tooling](../infrastructure/development-tooling.md) -- build system and `build.sh run`
- [TODO-008 Hyper-V Runner](../../todo/000-Infrastructure/TODO-008-Hyper-V-Runner.md) -- Hyper-V Gen 2 support (planned)
