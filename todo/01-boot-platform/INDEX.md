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

- [TODO-01 — UEFI Bootloader Hardening & Secure Boot](TODO-01-uefi-hardening-secureboot.md) — Secure Boot shim, UEFI runtime, GOP resolution, SMBIOS, A/B slots, boot menu, capsule update, W^X
- [TODO-02 — Boot Diagnostics, Heartbeat & Spinner](TODO-02-boot-diagnostics.md) — POST codes, named-stage API, debug waterfall, panic forensics, QR code, vital signs, multi-instance spinner
- [TODO-03 — Interrupt Architecture & Unified Timer Subsystem](TODO-03-interrupt-timer-arch.md) — MADT, LAPIC/IOAPIC order, full IDT, dynamic IRQ API, UTS HPET/LAPIC/PIT HAL, LAPIC calibration
- [TODO-04 — CPU Boot Sequencing & AP Hardening](TODO-04-cpu-boot-sequencing.md) — Phase 0 activation order (EFER→CR4 before VMM), hypervisor detection before UTS, AP hardening replication

## Completed / Doc-converted

- None yet.

## Local Naming

- Use `TODO-01-uefi-handoff.md` as the filename style for new leaf TODOs in this folder.
- Create a parent TODO only when one topic needs multiple leaf files or shared verification.
