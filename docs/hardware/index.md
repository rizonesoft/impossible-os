# Hardware

Drivers and hardware support: PCI and Plug and Play, interrupt controllers, ACPI and power, security hardware, kernel modules, device firmware, the Device Manager, core built-in drivers, hypervisor guest support, USB, input, touchpads, storage controllers and bare-metal validation.

## Roadmap Overviews

One page per drivers and hardware roadmap file, each following the [page contract](../contributing/docs-page-contract.md).

| Document | Topics |
| --- | --- |
| [PCI, Plug and Play and Resource Manager](pci-pnp-resource-manager.md) | Locked config access, boot scan, D-state transitions, planned device tree and allocator |
| [APIC and Interrupt Routing](apic-interrupt-routing.md) | Local and I/O APIC, vector allocator, timer calibration, planned x2APIC and shootdown |
| [ACPI and Power Management](acpi-power-management.md) | Table parsing, dormant ACPICA, power button and shutdown, refused S3 and S4 |
| [Security Hardware and DMA Safety](security-hardware.md) | SMEP and SMAP status, random generator, TPM transport, Secure Boot, planned IOMMU and CET |
| [Kernel Module System](kernel-modules.md) | Built-in drivers today, `.kmod` format tag, planned loader and driver model |
| [Firmware Loader and Device Blobs](firmware-loader.md) | Planned `request_firmware()`, manifests, hashing and licence audit |
| [Device Manager and Driver Diagnostics](device-manager.md) | Where device facts live today, planned registry, `lspci` and window |
| [Core Built-in Drivers](core-drivers.md) | HPET clock, polled NVMe, per-driver MSI, planned ECAM and hot-plug |
| [Hypervisor Abstraction and Guest Support](hypervisor-abstraction.md) | Hypervisor detection, VirtualBox and VirtIO pointers, planned guest additions |
| [USB Stack](usb-stack.md) | xHCI host, boot HID and mass storage, planned core layer, hubs and class drivers |
| [Input System](input-system.md) | PS/2 keyboard and mouse, merged input sources, planned layouts and wheel |
| [I2C and Precision Touchpad](i2c-touchpad.md) | Why I2C touchpads do not work yet and the planned stack |
| [Storage Controllers and Removable Media](storage-controllers.md) | Block-device table, AHCI, ATAPI, VirtIO-blk, USB and NVMe disks |

## Validation

| Document | Topics |
| --- | --- |
| [Bare-Metal Boot Lab Inventory](boot-lab.md) | Certification classes and manual evidence for real hardware |

## Specifications

Reference specifications live outside the docs tree, grouped by area:

| Area | Scope |
| --- | --- |
| [CPU](../../specs/hardware/cpu/) | x86-64 Long Mode, AMD64, Intel SDM |
| [Bus](../../specs/hardware/bus/) | PCI and PCIe enumeration, BAR mapping |
| [Firmware](../../specs/hardware/firmware/) | ACPI tables, UEFI boot services |
| [Interrupts](../../specs/hardware/interrupts/) | APIC, I/O APIC, interrupt routing |
