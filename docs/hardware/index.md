# Hardware

Drivers and hardware support: PCI and Plug and Play, interrupt controllers, ACPI and power, security hardware, kernel modules, device firmware, the Device Manager, core built-in drivers, hypervisor guest support, USB, input, touchpads, storage controllers, networking, Wi-Fi, Bluetooth, display, audio, sensors, serial and debug I/O, game controllers, cameras, printers, docks, and driver and bare-metal certification.

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
| [Network Drivers](network-drivers.md) | The built-in RTL8139, the Ethernet layer, planned e1000, VirtIO-net and 2.5 GbE modules |
| [Wi-Fi Drivers](wifi-drivers.md) | Planned Wi-Fi manager, chip drivers, WPA2 supplicant and `netsh wlan` |
| [Bluetooth](bluetooth.md) | Planned in-kernel HCI, L2CAP, profiles, pairing and `btctl` |
| [GPU and Display Drivers](gpu-display-drivers.md) | GOP framebuffer, Bochs page flip, planned display drivers and multi-head |
| [Audio Drivers](audio-drivers.md) | Planned audio interface, AC97, HDA, VirtIO Sound and USB audio |
| [Hardware Monitoring and Sensors](hardware-monitoring-sensors.md) | The embedded controller transport, planned sensor class and events |
| [Serial, Parallel and Debug I/O](serial-parallel-debug-io.md) | The kernel console UART and crash path, planned COM devices, LPT, GPIO and SPI |
| [Game Controllers and Haptics](game-controllers.md) | Why gamepads are ignored today, planned controller class and rumble |
| [Cameras and Imaging Devices](camera-imaging.md) | Planned UVC driver, privacy enforcement and scanner class |
| [Printing and Scanning Device Path](printing-scanning.md) | Planned USB printer class, IPP-over-USB hand-off and multifunction devices |
| [Docking, Thunderbolt and USB4](docking-thunderbolt-usb4.md) | Planned dock topology, Thunderbolt security and external DMA report |
| [Driver Hardware Certification Matrix](driver-certification-matrix.md) | Boot certification today, planned per-driver matrix and release gate |

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
