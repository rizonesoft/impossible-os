# Architecture

OS internals — kernel subsystems, drivers, filesystem, boot chain, desktop, and networking.

*No architecture docs yet — these will be generated as kernel, driver, and filesystem TODOs are completed.*

## Subdirectories

| Directory      | Scope                                    |
| -------------- | ---------------------------------------- |
| `boot/`        | Boot chain, UEFI bootloader              |
| `drivers/`     | Hardware drivers (AHCI, VirtIO, NIC)     |
| `desktop/`     | Window manager, compositor, shell        |
| `filesystem/`  | VFS, FAT32, IXFS, ext4                   |
| `kernel/`      | PMM, VMM, scheduler, heap, syscalls      |
| `net/`         | Ethernet, ARP, IPv4, ICMP, UDP, DHCP     |

## See Also

- [Infrastructure](../infrastructure/Index.md) — build system and CI/CD
- [Specs](../specs/Index.md) — external reference specifications
