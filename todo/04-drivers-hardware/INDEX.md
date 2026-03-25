# 04 Drivers Hardware

This domain covers hardware-facing work: bus discovery, interrupts, controller drivers, device support, and physical platform integration.

## Belongs Here

- PCI, PCIe, ACPI, APIC, timers, SMBIOS, TPM, and similar platform-facing work.
- Storage controller drivers such as AHCI, VirtIO, NVMe, ATA, and USB controller or transport work.
- Input, display, NIC, power, guest additions, and other hardware-specific device integration.

## Does Not Belong Here

- Partition, VFS, and filesystem semantics. Put that in [05 Storage Filesystems](../05-storage-filesystems/INDEX.md).
- High-level network protocol work or internet applications. Put that in [06 Networking](../06-networking/INDEX.md) or [10 Apps](../10-apps/INDEX.md).

## Likely Source Areas

- [src/kernel/drivers](../src/kernel/drivers/)
- [src/kernel/acpi.c](../src/kernel/acpi.c)
- [src/kernel/timer.c](../src/kernel/timer.c)
- [src/kernel/smbios.c](../src/kernel/smbios.c)
- [src/kernel/tpm.c](../src/kernel/tpm.c)

## Epics

- None yet.

## Active TODOs

- None yet.

## Completed / Doc-converted

- None yet.

## Local Naming

- Use `TODO-01-pci-bus-basics.md` as the filename style for new leaf TODOs in this folder.
- Create a parent TODO only when one topic needs multiple leaf files or shared verification.
