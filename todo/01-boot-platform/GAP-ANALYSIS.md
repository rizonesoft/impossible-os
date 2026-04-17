# 01 Boot Platform Gap Analysis

Date: 2026-04-16

## Scope

This analysis covers the path from firmware entry to the first reliable kernel services: bootloader execution, boot device selection, boot assets, firmware table handoff, memory-map ownership, boot diagnostics, boot security, recovery, and boot validation. Long-lived kernel subsystems stay in later domains, but any contract that must be established before normal kernel execution belongs here.

## Existing Coverage

| Area | Existing TODOs | Coverage |
| --- | --- | --- |
| Core UEFI boot path | TODO-01, TODO-02, TODO-03, TODO-27 | Boot ABI, runtime services, variables, GOP, Secure Boot, error handling, ELF hardening, advanced UEFI backlog |
| Boot device and storage path | TODO-05, TODO-06, TODO-16, TODO-17, TODO-19, TODO-20, TODO-25 | Boot device identity, image artifacts, fallback chain, NVMe, USB MSC, USB robustness, zero-delay handoff, network boot |
| Early CPU/platform bring-up | TODO-04, TODO-09, TODO-10, TODO-11 | Firmware inventory, CPU sequencing, bare-metal hardening, interrupt/timer architecture |
| Boot visibility and diagnostics | TODO-14, TODO-15, TODO-24 | POST codes, boot timeline, VPD, BlackBox service partition |
| Recovery and rollback | TODO-21, TODO-22, TODO-23, TODO-26 | A/B rollback, recovery partition, boot watchdog, hibernation resume |
| Boot input | TODO-18 | USB HID boot keyboard/mouse |

## Gaps Added

| Gap | New TODO | Why It Is Needed |
| --- | --- | --- |
| Boot handoff ABI as a single owned contract | TODO-01 | `struct boot_info` now carries many independent features; versioning, ownership, modules/initrd, and layout drift need one authoritative owner. |
| Firmware table and platform inventory hub | TODO-04 | Configuration-table entries exist for ACPI, SMBIOS, DTB, FPDT, memory attributes, ESRT, and conformance, but no TODO owns the generic discovery/registry contract. |
| TPM measured boot and attestation | TODO-13 | TPM event-log parsing exists, but PCR replay, baseline enrollment, TPM2 commands, sealing, and attestation are still only stubbed. |
| Network boot | TODO-25 | `boot_device_type` reserves network and fallback-chain prose mentions network, but there is no PXE/HTTP/TFTP/NFS/iSCSI owner. |
| Boot entry store and user-selectable boot policy | TODO-07 | Boot variables are read, but BCD-style entries, boot menu policy, one-shot overrides, and safe-mode entry selection lack a boot-platform owner. |
| Boot media/image artifact pipeline | TODO-06 | Scripts exist for QEMU/USB, but hybrid ISO, VHD/VHDX, installer handoff, artifact manifests, and image verification are not covered in this domain. |
| Hibernation resume and fast-startup boot handoff | TODO-26 | Power management can create S4 images later, but the bootloader/kernel early path needs an owner for resume detection and image handoff. |
| Early entropy handoff | TODO-12 | Secure Boot/TPM exist, and later crypto needs entropy, but firmware RNG, RDRAND, TPM RNG, seed-file carryover, and early CSPRNG seeding are not owned. |
| Alternate boot protocol compatibility | TODO-08 | Multiboot2 code exists beside UEFI, but policy, feature parity, and deprecation/compatibility tests are not captured. |
| Boot validation and certification matrix | TODO-28 | The domain has many platform-specific boot promises but no single certification gate spanning VM, USB, NVMe, Secure Boot, recovery, and bare metal. |

## Complete Domain Closure Rule

The `01-boot-platform` domain is complete when TODO-01 through TODO-28 are implemented or explicitly doc-converted, and every field in `include/kernel/boot_info.h`, every firmware handoff path in `src/boot/uefi/bootx64.c`, every early boot script artifact, and every boot-recovery path has exactly one owning TODO with unit, VM, and bare-metal verification.
