# 01 Boot Platform

This domain covers the path from firmware entry through kernel handoff and early boot platform setup.

## Belongs Here

- UEFI bootloader work, boot contracts, memory map handoff, and GOP setup.
- Early platform initialization that must exist before normal kernel subsystems can run.
- Boot diagnostics, boot timing, and other first-stage bring-up tasks.

## Does Not Belong Here

- General kernel facilities after bring-up. Put that in [02 Kernel Core](../02-kernel-core/INDEX.md) or [03 Memory Concurrency](../03-memory-concurrency/INDEX.md).
- Long-lived device driver work. Put that in [04 Drivers Hardware](../04-drivers-hardware/INDEX.md).

## Likely Source Areas

- [src/boot](../src/boot/)
- [src/kernel/main](../src/kernel/main/)
- [docs](../docs/)

## Epics

- None yet.

## Active TODOs

- [TODO-01 — UEFI Bootloader Hardening & Secure Boot](TODO-01-uefi-hardening-secureboot.md) — Secure Boot shim, UEFI runtime, GOP resolution, SMBIOS, boot menu, capsule update, W^X
- [TODO-02 — Bootloader Error Recovery & ELF Hardening](TODO-02-bootloader-error-recovery.md) — Eliminate silent failures: ELF bounds checking, ExitBootServices retry, fallback kernel search, boot failure error screen
- [TODO-03 — Boot Device Discovery & Fallback Chain](TODO-03-boot-device-discovery.md) — Boot device identification via LoadedImage, multi-device fallback, device type detection
- [TODO-04 — CPU Boot Sequencing & AP Hardening](TODO-04-cpu-boot-sequencing.md) — Phase 0 activation order (EFER→CR4 before VMM), hypervisor detection before UTS, AP hardening replication
- [TODO-05 — Bare Metal Boot Hardening](TODO-05-bare-metal-hardening.md) — IST stacks, ACPI-gated hardware access, graceful degradation, hw interrupt fix, UC MMIO, CPU feature verification
- [TODO-06 — Interrupt Architecture & Unified Timer Subsystem](TODO-06-interrupt-timer-arch.md) — MADT, LAPIC/IOAPIC order, full IDT, dynamic IRQ API, UTS HPET/LAPIC/PIT HAL, LAPIC calibration
- [TODO-07 — Boot Diagnostics, Heartbeat & Spinner](TODO-07-boot-diagnostics.md) — POST codes, named-stage API, panic forensics, QR code, vital signs, multi-instance spinner
- [TODO-08 — Visual POST Display (VPD)](TODO-08-visual-post-display.md) — Two-tier boot progress visualization: pre-splash micro-font bars + splash-integrated stages, NVRAM crash persistence, configurable via `postbars`
- [TODO-09 — NVMe Storage Driver](TODO-09-nvme-storage.md) — NVMe controller, Admin+I/O queues, sector read/write — access internal NVMe storage
- [TODO-10 — xHCI, USB Storage & USB HID](TODO-10-xhci-usb-boot.md) — xHCI controller, USB MSC BOT, boot handover, hardware compatibility (EHCI/hub); USB HID → [TODO-12](TODO-12-usb-hid-keyboard-mouse.md)
- [TODO-11 — USB Boot Hardening & Fail-Safe Pipeline](TODO-11-usb-boot-hardening.md) — SCSI retry, sleep hack removal, EHCI fallback, bounded klog flush, media speed detection, single-pass log routing, slow-media IXFS tests
- [TODO-12 — USB HID Boot-Protocol Keyboard & Mouse](TODO-12-usb-hid-keyboard-mouse.md) — xHCI interrupt endpoints, HID boot-protocol keyboard/mouse, input coexistence
- [TODO-13 — Zero-Delay USB Boot](TODO-13-usb-zero-delay-handover.md) — Pre-ExitBootServices xHCI driver loading with persistent DMA — true Windows-style zero-delay USB handover
- [TODO-14 — A/B Dual-Slot Boot & Automatic Rollback](TODO-14-ab-boot-rollback.md) — Never unbootable: dual root partitions, failure counting, automatic rollback
- [TODO-15 — Recovery Partition & Self-Repair](TODO-15-recovery-partition.md) — Recovery environment: filesystem repair, kernel restore, NVRAM reconstruction
- [TODO-16 — Boot Watchdog & Hang Detection](TODO-16-boot-watchdog.md) — LAPIC NMI + ACPI TCO watchdog: detect hung boot, auto-reboot, integrate with A/B rollback

## Completed / Doc-converted

- None yet.

## Local Naming

- Use `TODO-01-uefi-handoff.md` as the filename style for new leaf TODOs in this folder.
- Create a parent TODO only when one topic needs multiple leaf files or shared verification.
