# 04 Drivers Hardware

This domain covers hardware-facing work: bus discovery, interrupts, controller drivers, device support, and physical platform integration.

## Belongs Here

- PCI, PCIe, ACPI, APIC, timers, SMBIOS, TPM, and similar platform-facing work.
- Storage controller drivers such as AHCI, VirtIO, NVMe, ATA, and USB controller or transport work.
- Input, display, NIC, power, guest additions, and other hardware-specific device integration.

## Does Not Belong Here

- Partition, VFS, and filesystem semantics. Put that in [05 Storage Filesystems](../05-storage-filesystems/INDEX.md).
- High-level network protocol work or internet applications. Put that in [07 Networking](../07-networking/INDEX.md) or [11 Apps](../11-apps/INDEX.md).

## Likely Source Areas

- [src/kernel/drivers](../../src/kernel/drivers/)
- [src/kernel/acpi.c](../../src/kernel/acpi.c)
- [src/kernel/timer.c](../../src/kernel/timer.c)
- [src/kernel/smbios.c](../../src/kernel/smbios.c)
- [src/kernel/tpm.c](../../src/kernel/tpm.c)

## Epics

- [Gap Analysis](./GAP-ANALYSIS.md) -- 2026-04-16 full-domain audit adding missing PCI/PNP, firmware, storage-controller breadth, sensors, serial/debug I/O, camera/imaging, controllers, printing/scanning, docking/Thunderbolt, and certification lanes.

## Active TODOs

- [TODO-01 PCI/PCIe, PnP & Resource Manager](./TODO-01-pci-pcie-pnp-resource-manager.md) -- canonical device tree, PCIe bridge enumeration, BAR sizing, resource windows, ACPI `_ADR`/`_PRT`, IRQ/MSI routing, driver bind/unbind, hot-plug, power states, conflict diagnostics.
- [TODO-02 APIC Architecture & Advanced Interrupt Routing](./TODO-02-apic-interrupt-routing.md) -- x2APIC MSR mode, LAPIC timer calibration via HPET, TLB shootdown IPI (`invlpg` handler + ack counter), NMI watchdog via PMI overflow, MSI vector bitmap allocator, per-vector interrupt profiler (`/sys/interrupts`).
- [TODO-03 ACPI Full Subsystem & Power Management](./TODO-03-acpi-power-management.md) -- ACPICA AML integration, S3 suspend/resume (trampoline), battery `_BST`/`_BIF`, CPU DVFS `_PSS`/`IA32_PERF_CTL`, power button SCI, per-core thermal monitoring, C-states `_CST`/`mwait`, S4 hibernate (hiberfil.sys), clean shutdown orchestrator, power profiles.
- [TODO-04 Security Hardware & DMA Safety](./TODO-04-security-hardware.md) -- `hwrng_read()` RDRAND/RDSEED + ChaCha20 CSPRNG fallback, IOMMU/VT-d + AMD-Vi 4-level page tables + default-deny DMA, DMA bounce buffer manager (PMM low zone), TPM 2.0 command driver (CRB/FIFO, STARTUP/PCR/GetRandom), Secure Boot UEFI variable -> Registry, Intel CET shadow stacks (`CR4.CET`, per-task SSP, `INCSSPQ`/`RSTORSSP`), SMEP+SMAP on all APs, kernel integrity SHA-256 + TPM PCR[10] baseline.
- [TODO-05 Kernel Module System](./TODO-05-kernel-module-system.md) -- kernel symbol table (`EXPORT_SYMBOL`/`.ksymtab`), ELF relocatable module loader, module build system (`src/modules/`), driver model + HAL vtables (`blk_ops`/`net_ops`/`input_ops`), boot-time auto-load from `C:\Impossible\System\Drivers\`, RTL8139 as first loadable module.
- [TODO-06 Firmware Loader & Device Blob Policy](./TODO-06-firmware-loader-device-blobs.md) -- `request_firmware()` API, firmware manifests, integrity/signature policy, version fallback, rollback, license audit, Device Manager/BlackBox missing-firmware diagnostics.
- [TODO-07 Device Manager & Driver Diagnostics](./TODO-07-device-manager.md) -- persistent `pci_device_db[]` registry, embedded 2000-entry PCI ID database, live `irq_count[]`/`irq_rate()` counters, driver health registry (`OK/WARN/ERROR` badges), `/sys/devices` + `/sys/interrupts` VFS files, native Device Manager GUI (class tree + live IRQ rate + 1 s auto-refresh), USB hub tree integration, `SetupDiGetClassDevs`/`EnumDeviceInfo` Win32 API, `lspci`/`lsusb` shell commands, driver hot-unload/reload.
- [TODO-08 Core Built-in Driver Enhancements](./TODO-08-core-driver-enhancements.md) -- NVMe storage driver (Admin+I/O queues, `blkdev_register`), HPET timer (`hpet_read_ns()`, LAPIC calibration), PCIe ECAM via ACPI MCFG, capability chain scanner (`pci_find_capability`/`pcie_find_extended_capability`), MSI/MSI-X, PCIe hot-plug.
- [TODO-09 Hypervisor Abstraction Layer](./TODO-09-hypervisor-abstraction.md) -- CPUID `0x40000000` hypervisor detection, unified `hv_ops_t` dispatch table, VBox display auto-resize + HGCM + shared folders + clipboard, VirtIO GPU (page flip, hardware cursor), VirtIO 9P shared folders, Hyper-V synthetic HID (keyboard/mouse VMBus VSP), Hyper-V synthetic video (VRAM GPA mapping), bare-metal null backend.
- [TODO-10 USB Stack Completion](./TODO-10-usb-stack.md) -- USB core abstraction layer (`usb_device_t`, `usb_hcd_ops_t` vtable), USB string descriptor retrieval, isochronous endpoint support, interrupt-IN endpoint setup for HID, USB HID boot-protocol keyboard/mouse, USB MSC BOT SCSI transport + multi-LUN (`blkdev_register`), hot-plug Port Status Change TRB handling + desktop toasts, USB hub class driver, EHCI fallback (USB 2.0), PS/2↔USB input fallback, Bluetooth HCI via USB, CDC-ECM Ethernet, CDC-ACM serial.
- [TODO-11 Input System Enhancement](./TODO-11-input-system.md) -- PS/2 Intellimouse scroll (ID 3), Explorer 5-button (ID 4), packet resync, mouse acceleration + sensitivity, raw input grab (`WM_INPUT`/`SYS_MOUSE_GRAB`), keyboard layout system (`kbd_layout_t`, 6 built-in locales + Dvorak), dead key compose, UTF-8 codepoint output, Win+Space layout switching + tray indicator, sticky keys + typematic rate/delay.
- [TODO-12 I2C/SMBus Bus & Precision Touchpad](./TODO-12-i2c-touchpad.md) -- Intel PCH + AMD FCH I2C/SMBus host controller, ACPI DSDT `I2CSerialBusV2` device enumeration, HID-over-I2C transport (ACPI `PNP0C50`), HID report descriptor parser (`hid_field_t[]`), Microsoft PTP 10-touch multi-contact, kernel-resident gesture engine (scroll/pinch/swipe/tap/palm reject), Synaptics PS/2 fallback, ELAN/Goodix quirks, `mouse.cpl` Touchpad tab, `xinput list`/`touchpad-info` shell commands.
- [TODO-13 Storage Controller & Removable Media Drivers](./TODO-13-storage-controller-device-drivers.md) -- AHCI/SATA parity, ATA/ATAPI, VirtIO-blk integration, SDHCI/eMMC, USB card readers, storage identity/health, surprise removal, storage-driver diagnostics.
- [TODO-14 Network Drivers](./TODO-14-network-drivers.md) -- Intel e1000, VirtIO-net, RTL8169/8111, Intel igc (I225/I226 2.5 GbE), RTL8125 2.5 GbE as loadable `.kmod` modules; WiFi 802.11 MAC stub (`wifi_mac_t`, station state machine, WPA2-PSK hooks); iwlwifi + rtw89 PCI stubs (P4); `LICENSES/` + `NOTICE.md` per-file attribution table.
- [TODO-15 WiFi Hardware Drivers](./TODO-15-wifi-drivers.md) -- `wifi_device_t` vtable + `wifi_manager.c` singleton + `SYS_WIFI_*` syscalls, RTL8188/8192 USB WiFi (embedded firmware), RTL8821CE/8822BE PCIe (VFS firmware, DMA rings, MSI), Intel AX200/AX210 iwlwifi (BSD ucode, clean-room transport), MediaTek MT7921/MT7922 (WFDMA rings, MIT firmware), WPA2-PSK in-kernel (PBKDF2+HMAC-SHA1+AES-CCM, no daemon), 802.11 frame layer, scan/association SM, PS-Poll power management, `ncpa.cpl` WiFi tab + `netsh wlan` commands.
- [TODO-16 Bluetooth Full Stack](./TODO-16-bluetooth.md) -- HCI transport layer (commands/events/ACL/`hci_conn[]`), Intel AX200-BT + Realtek RTL8761B firmware loading, L2CAP channel multiplexing, SDP service discovery, BT HID profile (wireless keyboard/mouse), A2DP+SBC encoder+AVRCP (headphones), RFCOMM/SPP (`/dev/rfcomm0`), BLE LE scan + ATT/GATT Battery+DevInfo, in-kernel `bt_manager.c` (SSP pairing, Registry link key, auto-reconnect, no daemon), `bluetooth.cpl`, `btctl` shell.
- [TODO-17 GPU & Display Drivers](./TODO-17-gpu-display-drivers.md) -- `display_device_t` vtable (7-function, priority-sorted), VMSVGA 2D FIFO + hardware cursor, VirtIO-GPU scanout pipeline, Bochs/BGA Y_OFFSET page-flip, VBE/GOP fallback registration, Intel iGPU modesetting stub (Gen9–12 PRM, P3), AMD APU Vega/RDNA DCN stub (GPUOpen, P4), multi-head compositor span + `NtQueryDisplayConfig` stub.
- [TODO-18 Audio Drivers](./TODO-18-audio-drivers.md) -- `audio_device_t` HAL vtable (PCM passthrough, no in-kernel mixer), AC97 BDL DMA ring, Intel HDA CORB/RIRB + widget tree + DMA stream, VirtIO Sound controlq/txq/rxq, USB Audio UAC1 isochronous OUT + `SET_CUR` volume, loadable `.kmod` conversion, audio device hot-plug + Registry `DefaultDevice`.
- [TODO-19 Hardware Monitoring, Sensors & Environmental Devices](./TODO-19-hardware-monitoring-sensors.md) -- sensor class API, ACPI thermal/lid/tablet/ALS, SMBus hwmon, fan/voltage/chassis sensors, accelerometers, audio jack sensing, event notifications, sensor diagnostics.
- [TODO-20 Serial, Parallel, GPIO/SPI & Debug I/O Devices](./TODO-20-serial-parallel-debug-io.md) -- runtime COM device model, 16550 cleanup, PCI/USB serial, KD/console arbitration, LPT, GPIO, SPI, industrial I/O diagnostics.
- [TODO-21 Game Controllers, HID Force Feedback & Haptics](./TODO-21-game-controller-haptics.md) -- controller class API, HID gamepad parsing, XInput profiles, Bluetooth controllers, rumble, battery/LED/player index, calibration, low-latency routing.
- [TODO-22 Camera, Video Capture & Imaging Devices](./TODO-22-camera-imaging-devices.md) -- camera class API, USB UVC controls/streaming, privacy LED/switch enforcement, laptop IPU boundary, capture devices, scanner class, permissions/audit, camera diagnostics.
- [TODO-23 Printing, Scanning & Imaging Peripheral Device Path](./TODO-23-printing-scanning-device-path.md) -- USB printer class, IPP-over-USB handoff, LPT printer transport, multifunction composition, scanner handoff, status reporting, device permissions.
- [TODO-24 Docking, Thunderbolt, USB4 & External Expansion](./TODO-24-docking-thunderbolt-usb4-expansion.md) -- dock topology, USB-C hubs, Thunderbolt/USB4 security, external PCIe authorization, display alt-mode/eGPU boundary, hot-plug storm resilience, DMA risk reporting.
- [TODO-25 Driver Hardware Certification Matrix](./TODO-25-driver-hardware-certification-matrix.md) -- VM and bare-metal driver certification, USB/peripheral matrix, wireless/Bluetooth matrix, suspend/hot-plug tests, firmware/license matrix, support bundle, release gate.

## Completed / Doc-converted

- None yet.

## Local Naming

- Use `TODO-01-pci-bus-basics.md` as the filename style for new leaf TODOs in this folder.
- Create a parent TODO only when one topic needs multiple leaf files or shared verification.
