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

- [src/boot](../../src/boot/)
- [src/kernel/main](../../src/kernel/main/)
- [docs](../../docs/)

## Epics

- [Gap Analysis](./GAP-ANALYSIS.md) -- 2026-04-16 full-domain audit adding the missing boot protocol, firmware inventory, measured boot, network boot, media pipeline, resume, entropy, alternate protocol, and certification lanes.

## Active TODOs

- [TODO-01 -- Boot Protocol ABI & Handoff Contract](TODO-01-boot-protocol-abi-handoff.md) -- Authoritative `boot_info` owner: shipped ABI header and pre-copy validation foundations, plus remaining ownership, manifest, optional payloads, memory reservation, stale-loader handling, and fuzz coverage.
- [TODO-02 -- UEFI Bootloader Hardening & Secure Boot](TODO-02-uefi-hardening-secureboot.md) -- ✅ 20 sections; runtime services, GOP, SMBIOS, Secure Boot state + drift monitor, UKI signed payloads, SBAT revocation and ESP integrity all shipped, `test_uefi_boot` at 42 suites. Remaining items are parked with named owners (S3 refresh, UKI smoke harness, shim re-pin -- operator-gated). Advanced UEFI in [TODO-27](TODO-27-uefi-advanced.md)
- [TODO-03 -- Bootloader Error Recovery & ELF Hardening](TODO-03-bootloader-error-recovery.md) -- Eliminate silent failures: ELF bounds checking, ExitBootServices retry, fallback kernel search, boot failure error screen
- [TODO-04 -- Firmware Table & Platform Inventory](TODO-04-firmware-table-platform-inventory.md) -- Unified catalog, validation, Registry/BlackBox reports, ESRT, FPDT, MAT, RT properties, DTB/EBBR, and firmware quirk database.
- [TODO-05 -- Boot Device Discovery & Fallback Chain](TODO-05-boot-device-discovery.md) -- Boot device identification via LoadedImage, local-device fallback, UEFI boot variables, partition GUID validation, removable media, Registry population, disk health check
- [TODO-06 -- Boot Media, Image Pipeline & Installer Handoff](TODO-06-boot-media-image-installer-handoff.md) -- Reproducible raw/USB/VHD/VHDX/VDI/ISO artifacts, signed manifests, installer/live/recovery media roles, artifact inspector, and CI boot matrix.
- [TODO-07 -- Boot Entry Store, Menu & Policy](TODO-07-boot-entry-store-menu-policy.md) -- BCD-style boot entries, menu UI, BootNext/policy merge, safe/test/recovery entries, A/B integration, known-good kernel entries, and bootcfg tooling.
- [TODO-08 -- Alternate Boot Protocols & Compatibility Boundary](TODO-08-alternate-boot-protocols.md) -- **CLOSED**. Policy = `unsupported`. UEFI/GPT/ESP is the only supported boot path; Multiboot2 / GRUB / Limine / legacy BIOS / EFI stub / kexec are explicit non-goals per [`docs/boot/alt-boot.md`](../../docs/boot/alt-boot.md).
- [TODO-09 -- CPU Boot Sequencing & AP Hardening](TODO-09-cpu-boot-sequencing.md) -- Phase 0 activation order, hypervisor detection, AP hardening replication, AP feature consistency validation, CR4 bit pinning, MTRR/PAT AP sync, CPU register audit trail
- [TODO-10 -- Bare Metal Boot Hardening](TODO-10-bare-metal-hardening.md) -- IST stacks, ACPI-gated hardware access, graceful degradation, hw interrupt fix, UC MMIO, CPU feature verification, and bare-metal validation discipline
- [TODO-11 -- Interrupt Architecture & Unified Timer Subsystem](TODO-11-interrupt-timer-arch.md) -- MADT, LAPIC/IOAPIC order, full IDT, dynamic IRQ API, UTS HPET/LAPIC/PIT HAL, LAPIC calibration
- [TODO-12 -- Early Entropy & Random Seed Handoff](TODO-12-early-entropy-random-seed.md) -- EFI RNG, RDRAND/RDSEED, TPM RNG, jitter, seed-file carryover, boot_info seed payload, early CSPRNG seeding, and entropy policy reporting.
- [TODO-13 -- TPM Measured Boot, PCR Replay & Attestation](TODO-13-tpm-measured-boot-attestation.md) -- TPM2 transport, PCR reads, event-log replay, baseline enrollment, NV storage, sealed secrets, attestation export, and mismatch recovery UX.
- [TODO-14 -- Boot Diagnostics, Heartbeat & Spinner](TODO-14-boot-diagnostics.md) -- POST codes, named-stage API, panic forensics, QR code, vital signs, multi-instance spinner
- [TODO-15 -- Visual POST Display (VPD)](TODO-15-visual-post-display.md) -- Two-tier boot progress visualization: pre-splash micro-font bars + splash-integrated stages, NVRAM crash persistence, configurable via `postbars`
- [TODO-16 -- NVMe Storage Driver](TODO-16-nvme-storage.md) -- NVMe controller, Admin+I/O queues, sector read/write -- access internal NVMe storage
- [TODO-17 -- xHCI, USB Storage & USB HID](TODO-17-xhci-usb-boot.md) -- Baseline xHCI controller, USB MSC BOT, block-device registration, post-boot hot-plug, and hardware compatibility (EHCI/hub); zero-delay handover -> [TODO-20](TODO-20-usb-zero-delay-handover.md), USB HID -> [TODO-18](TODO-18-usb-hid-keyboard-mouse.md)
- [TODO-18 -- USB HID Boot-Protocol Keyboard & Mouse](TODO-18-usb-hid-keyboard-mouse.md) -- xHCI interrupt endpoints, HID boot-protocol keyboard/mouse, input coexistence
- [TODO-19 -- USB Boot Hardening & Fail-Safe Pipeline](TODO-19-usb-boot-hardening.md) -- SCSI retry, USB transport stall recovery, bulk transfer timeouts, sleep hack removal, EHCI fallback, bounded klog flush, media speed detection, single-pass log routing, slow-media IXFS tests
- [TODO-20 -- Zero-Delay USB Boot](TODO-20-usb-zero-delay-handover.md) -- Pre-ExitBootServices xHCI takeover, persistent DMA state, and kernel inherit path for true Windows-style zero-delay USB boot
- [TODO-21 -- A/B Dual-Slot Boot & Automatic Rollback](TODO-21-ab-boot-rollback.md) -- Never unbootable: dual root partitions, failure counting, automatic rollback
- [TODO-22 -- Recovery Partition & Self-Repair](TODO-22-recovery-partition.md) -- Recovery environment: filesystem repair, kernel restore, NVRAM reconstruction
- [TODO-23 -- Boot Watchdog & Hang Detection](TODO-23-boot-watchdog.md) -- LAPIC NMI + ACPI TCO watchdog: detect hung boot, auto-reboot, integrate with A/B rollback
- [TODO-24 -- BlackBox Service Partition](TODO-24-blackbox-service-partition.md) -- ✅ Complete: 128 MiB FAT32 "BlackBox" partition (X:\) for logs, crash dumps, diagnostics, tools
- [TODO-25 -- Network / PXE / HTTP Boot](TODO-25-network-pxe-http-boot.md) -- UEFI network protocol discovery, PXE/TFTP/HTTP asset loading, signed network manifests, network provenance handoff, recovery over network, and test harnesses.
- [TODO-26 -- Hibernation Resume & Fast Startup Boot Handoff](TODO-26-hibernation-resume-fast-startup-handoff.md) -- S4/fast-startup metadata, resume eligibility, integrity validation, boot_info resume payload, failure fallback, and resume diagnostics.
- [TODO-27 -- UEFI Advanced Features](TODO-27-uefi-advanced.md) -- Deferred UEFI-specific features: multi-OS discovery/chainload entries for [TODO-07](TODO-07-boot-entry-store-menu-policy.md), capsule updates, W^X, multi-GPU GOP, extended SB, SMBIOS ext, DBX sync
- [TODO-28 -- Boot Validation & Hardware Certification Matrix](TODO-28-boot-validation-certification-matrix.md) -- Unified VM, media, security, rollback, network, and bare-metal boot certification matrix with support bundle and release gate.
- [TODO-29 -- Boot Performance & Health Observability](TODO-29-boot-perf-health-observability.md) -- Per-phase perf budgets + threshold alarms, consolidated `boot-health.json` dashboard, rolling `boot-trend.json` regression detection, plus targeted optimization on the slowest observed phases (SMBIOS, PS/2 mouse, font/icon load).

## Completed / Doc-converted

- None yet.

## Local Naming

- Use `TODO-01-uefi-handoff.md` as the filename style for new leaf TODOs in this folder.
- Create a parent TODO only when one topic needs multiple leaf files or shared verification.
