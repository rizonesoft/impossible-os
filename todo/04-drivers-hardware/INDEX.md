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

- [TODO-01 Kernel Module System](./TODO-01-kernel-module-system.md) -- kernel symbol table (`EXPORT_SYMBOL`/`.ksymtab`), ELF relocatable module loader, module build system (`src/modules/`), driver model + HAL vtables (`blk_ops`/`net_ops`/`input_ops`), boot-time auto-load from `C:\Impossible\System\Drivers\`, RTL8139 as first loadable module.
- [TODO-02 Core Built-in Driver Enhancements](./TODO-02-core-driver-enhancements.md) -- NVMe storage driver (Admin+I/O queues, `blkdev_register`), HPET timer (`hpet_read_ns()`, LAPIC calibration), PCIe ECAM via ACPI MCFG, capability chain scanner (`pci_find_capability`/`pcie_find_extended_capability`), MSI/MSI-X, PCIe hot-plug.
- [TODO-03 APIC Architecture & Advanced Interrupt Routing](./TODO-03-apic-interrupt-routing.md) -- x2APIC MSR mode, LAPIC timer calibration via HPET, TLB shootdown IPI (`invlpg` handler + ack counter), NMI watchdog via PMI overflow, MSI vector bitmap allocator, per-vector interrupt profiler (`/sys/interrupts`).
- [TODO-04 ACPI Full Subsystem & Power Management](./TODO-04-acpi-power-management.md) -- ACPICA AML integration, S3 suspend/resume (trampoline), battery `_BST`/`_BIF`, CPU DVFS `_PSS`/`IA32_PERF_CTL`, power button SCI, per-core thermal monitoring, C-states `_CST`/`mwait`, S4 hibernate (hiberfil.sys), clean shutdown orchestrator, power profiles.
- [TODO-05 Input System Enhancement](./TODO-05-input-system.md) -- PS/2 Intellimouse scroll (ID 3), Explorer 5-button (ID 4), packet resync, mouse acceleration + sensitivity, raw input grab (`WM_INPUT`/`SYS_MOUSE_GRAB`), keyboard layout system (`kbd_layout_t`, 6 built-in locales + Dvorak), dead key compose, UTF-8 codepoint output, Win+Space layout switching + tray indicator, sticky keys + typematic rate/delay.
- [TODO-06 Hypervisor Abstraction Layer](./TODO-06-hypervisor-abstraction.md) -- CPUID `0x40000000` hypervisor detection, unified `hv_ops_t` dispatch table, VBox display auto-resize + HGCM + shared folders + clipboard, VirtIO GPU (page flip, hardware cursor), VirtIO 9P shared folders, Hyper-V synthetic HID (keyboard/mouse VMBus VSP), Hyper-V synthetic video (VRAM GPA mapping), bare-metal null backend.
- [TODO-07 Network Drivers](./TODO-07-network-drivers.md) -- Intel e1000, VirtIO-net, RTL8169/8111, Intel igc (I225/I226 2.5 GbE), RTL8125 2.5 GbE as loadable `.kmod` modules; WiFi 802.11 MAC stub (`wifi_mac_t`, station state machine, WPA2-PSK hooks); iwlwifi + rtw89 PCI stubs (P4); `LICENSES/` + `NOTICE.md` per-file attribution table.
- [TODO-08 GPU & Display Drivers](./TODO-08-gpu-display-drivers.md) -- `display_device_t` vtable (7-function, priority-sorted), VMSVGA 2D FIFO + hardware cursor, VirtIO-GPU scanout pipeline, Bochs/BGA Y_OFFSET page-flip, VBE/GOP fallback registration, Intel iGPU modesetting stub (Gen9–12 PRM, P3), AMD APU Vega/RDNA DCN stub (GPUOpen, P4), multi-head compositor span + `NtQueryDisplayConfig` stub.
- [TODO-09 USB Stack Completion](./TODO-09-usb-stack.md) -- interrupt-IN endpoint setup for HID, USB HID boot-protocol keyboard/mouse, USB MSC BOT SCSI transport (`blkdev_register`), hot-plug Port Status Change TRB handling + desktop toasts, USB hub class driver, EHCI fallback (USB 2.0), PS/2↔USB input fallback, Bluetooth HCI via USB, CDC-ECM Ethernet, CDC-ACM serial.
- [TODO-10 Audio Drivers](./TODO-10-audio-drivers.md) -- `audio_device_t` HAL vtable (PCM passthrough, no in-kernel mixer), AC97 BDL DMA ring, Intel HDA CORB/RIRB + widget tree + DMA stream, VirtIO Sound controlq/txq/rxq, USB Audio UAC1 isochronous OUT + `SET_CUR` volume, loadable `.kmod` conversion, audio device hot-plug + Registry `DefaultDevice`.
- [TODO-11 Security Hardware & DMA Safety](./TODO-11-security-hardware.md) -- `hwrng_read()` RDRAND/RDSEED + ChaCha20 CSPRNG fallback, IOMMU/VT-d + AMD-Vi 4-level page tables + default-deny DMA, DMA bounce buffer manager (PMM low zone), TPM 2.0 command driver (CRB/FIFO, STARTUP/PCR/GetRandom), Secure Boot UEFI variable → Registry, Intel CET shadow stacks (`CR4.CET`, per-task SSP, `INCSSPQ`/`RSTORSSP`), SMEP+SMAP on all APs, kernel integrity SHA-256 + TPM PCR[10] baseline.
- [TODO-12 Device Manager & Driver Diagnostics](./TODO-12-device-manager.md) -- persistent `pci_device_db[]` registry, embedded 2000-entry PCI ID database, live `irq_count[]`/`irq_rate()` counters, driver health registry (`OK/WARN/ERROR` badges), `/sys/devices` + `/sys/interrupts` VFS files, native Device Manager GUI (class tree + live IRQ rate + 1 s auto-refresh), USB hub tree integration, `SetupDiGetClassDevs`/`EnumDeviceInfo` Win32 API, `lspci`/`lsusb` shell commands, driver hot-unload/reload.
- [TODO-13 I2C/SMBus Bus & Precision Touchpad](./TODO-13-i2c-touchpad.md) -- Intel PCH + AMD FCH I2C/SMBus host controller, ACPI DSDT `I2CSerialBusV2` device enumeration, HID-over-I2C transport (ACPI `PNP0C50`), HID report descriptor parser (`hid_field_t[]`), Microsoft PTP 10-touch multi-contact, kernel-resident gesture engine (scroll/pinch/swipe/tap/palm reject), Synaptics PS/2 fallback, ELAN/Goodix quirks, `mouse.cpl` Touchpad tab, `xinput list`/`touchpad-info` shell commands.
- [TODO-14 WiFi Hardware Drivers](./TODO-14-wifi-drivers.md) -- `wifi_device_t` vtable + `wifi_manager.c` singleton + `SYS_WIFI_*` syscalls, RTL8188/8192 USB WiFi (embedded firmware), RTL8821CE/8822BE PCIe (VFS firmware, DMA rings, MSI), Intel AX200/AX210 iwlwifi (BSD ucode, clean-room transport), MediaTek MT7921/MT7922 (WFDMA rings, MIT firmware), WPA2-PSK in-kernel (PBKDF2+HMAC-SHA1+AES-CCM, no daemon), 802.11 frame layer, scan/association SM, PS-Poll power management, `ncpa.cpl` WiFi tab + `netsh wlan` commands.
- [TODO-15 Bluetooth Full Stack](./TODO-15-bluetooth.md) -- HCI transport layer (commands/events/ACL/`hci_conn[]`), Intel AX200-BT + Realtek RTL8761B firmware loading, L2CAP channel multiplexing, SDP service discovery, BT HID profile (wireless keyboard/mouse), A2DP+SBC encoder+AVRCP (headphones), RFCOMM/SPP (`/dev/rfcomm0`), BLE LE scan + ATT/GATT Battery+DevInfo, in-kernel `bt_manager.c` (SSP pairing, Registry link key, auto-reconnect, no daemon), `bluetooth.cpl`, `btctl` shell.

## Completed / Doc-converted

- None yet.

## Local Naming

- Use `TODO-01-pci-bus-basics.md` as the filename style for new leaf TODOs in this folder.
- Create a parent TODO only when one topic needs multiple leaf files or shared verification.
