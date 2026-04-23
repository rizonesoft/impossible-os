---
schema_version: 1
id: firmware-table-platform-inventory
domain: 01-boot-platform
status: active
title: "TODO-04 -- Firmware Table & Platform Inventory"
---

# TODO-04 -- Firmware Table & Platform Inventory

> **Goal:** Make firmware-provided platform data complete, validated, and queryable. The bootloader already copies UEFI configuration-table entries and the kernel has helpers for ACPI, SMBIOS, memory attributes, runtime properties, conformance profiles, ESRT, DTB, and FPDT. This TODO owns the generic firmware inventory layer that discovers, validates, logs, and publishes those tables without mixing policy into each consumer.
> **Current state:** `boot_info.config_table[]` exists, `uefi_find_config_table()` exists, SMBIOS base parsing exists, FPDT timing is partially copied, and ESRT/conformance declarations exist. There is no complete table catalog, no registry mirror, no validation of table physical ranges against the memory map, no DTB/EBBR path, and no unified user-visible inventory.

## Inputs

- [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h)
- [`include/kernel/uefi_config.h`](../../include/kernel/uefi_config.h)
- [`include/kernel/smbios.h`](../../include/kernel/smbios.h)
- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c)
- -> XREF: `TODO-02-uefi-hardening-secureboot.md §5` -- existing SMBIOS base parser
- -> XREF: `TODO-11-interrupt-timer-arch.md §6` -- ACPI MADT remains the interrupt owner
- -> XREF: `TODO-27-uefi-advanced.md §2,§6` -- ESRT/capsule and extended SMBIOS consumers

## Outcome

- Every firmware table copied or referenced by the bootloader is range-checked, cataloged, and visible in diagnostics.
- ACPI, SMBIOS, DTB, FPDT, ESRT, memory attributes, runtime properties, and conformance profiles have one inventory API.
- Registry and BlackBox capture enough firmware metadata for support, crash triage, and hardware certification.
- Embedded/EBBR-style DTB systems have a defined path without pretending to be PC ACPI systems.

## Implementation Order

| ⭐ | Order | Deliverable | Depends On | Status |
| --- | :---: | --- | --- | :---: |
| 💎 | 1 | Firmware table catalog API | TODO-01 §1 | [ ] |
| 💎 | 2 | Physical range and checksum validation | §1 | [ ] |
| 💎 | 3 | ACPI/SMBIOS/DTB table arbitration | §1, §2 | [ ] |
| 💎 | 4 | FPDT and boot timing normalization | §1 | [ ] |
| 💎 | 5 | UEFI memory attributes and runtime properties inventory | §1, TODO-27 §3 | [ ] |
| 💎 | 6 | ESRT firmware inventory mirror | §1, TODO-27 §2 | [ ] |
| 💎 | 7 | UEFI conformance profile and EBBR detection | §1 | [ ] |
| 💎 | 8 | Registry and BlackBox firmware report | §1-§7 | [ ] |
| ⭐ | 9 | Firmware quirk database | §8 | [ ] |
| ⭐ | 10 | Firmware inventory tests and host decoder | §1-§9 | [ ] |

## 1. Firmware Table Catalog API

- [ ] Define `firmware_table_entry_t` with GUID/name, physical address, size, source, validation status, and owner.
- [ ] Implement `firmware_tables_init()` from `boot_info.config_table[]`.
- [ ] Add lookup by GUID, name, and owner subsystem.
- [ ] Emit a compact boot log summary with table count and critical table presence.
- [ ] Commit: `"boot: firmware table catalog API"`

## 2. Physical Range and Checksum Validation

- [ ] Validate every table pointer against the UEFI memory map before dereference.
- [ ] Verify ACPI RSDP/XSDT checksums, SMBIOS entry-point checksums, FPDT lengths, and ESRT bounds.
- [ ] Mark bad tables as degraded, not silently absent.
- [ ] Add `firmware_table_validate_all()` test hooks.
- [ ] Commit: `"boot: validate firmware table ranges and checksums"`

## 3. ACPI/SMBIOS/DTB Table Arbitration

- [ ] Define priority when ACPI 2.0, ACPI 1.0, SMBIOS3, SMBIOS2, and DTB are all present.
- [ ] Add DTB handoff validation and basic `/chosen`/memory/cpu discovery for EBBR systems.
- [ ] Document that PC-class hardware must use ACPI for interrupt/timer ownership.
- [ ] Add registry flags for `FirmwarePlatform=ACPI|DTB|Hybrid`.
- [ ] Commit: `"boot: arbitrate ACPI SMBIOS DTB firmware sources"`

## 4. FPDT and Boot Timing Normalization

- [ ] Normalize FPDT timestamps and bootloader TSC timestamps into one timeline.
- [ ] Detect zero/garbage FPDT records and mark as unreliable.
- [ ] Feed `boot_timing.c` and VPD from the same normalized source.
- [ ] Export JSON to `X:\Perf\boot-timeline.json`.
- [ ] Commit: `"boot: normalize FPDT and bootloader timing"`

## 5. UEFI Memory Attributes and Runtime Properties Inventory

- [ ] Parse and catalog EFI Memory Attributes Table.
- [ ] Parse Runtime Properties Table and record unavailable services.
- [ ] Expose runtime code/data W^X status to TODO-27 §3.
- [ ] Add warning when runtime service pointers exist but properties table says unsupported.
- [ ] Commit: `"boot: inventory UEFI MAT and RT properties"`

## 6. ESRT Firmware Inventory Mirror

- [ ] Parse ESRT entries into kernel-owned structs.
- [ ] Mirror firmware device GUID, type, current version, capsule flags, and last attempt status into Registry.
- [ ] Include ESRT in hardware dump and support bundle.
- [ ] Feed TODO-27 capsule update policy.
- [ ] Commit: `"boot: mirror ESRT firmware inventory"`

## 7. UEFI Conformance Profile and EBBR Detection

- [ ] Parse conformance profile table.
- [ ] Detect UEFI full profile, EBBR, and unknown profile GUIDs.
- [ ] Relax or tighten required table set based on profile.
- [ ] Add diagnostics when hardware claims EBBR but exposes PC-only assumptions.
- [ ] Commit: `"boot: detect UEFI conformance profile"`

## 8. Registry and BlackBox Firmware Report

- [ ] Create `HKLM\HARDWARE\Firmware\Tables\*`.
- [ ] Write `X:\Diag\firmware-tables.json` on boot.
- [ ] Include validation failures, table addresses, checksums, and quirk matches.
- [ ] Add `sysinfo.exe` integration for firmware inventory.
- [ ] Commit: `"boot: publish firmware inventory report"`

## 9. Firmware Quirk Database

- [ ] Add a small table keyed by SMBIOS vendor/product/BIOS version.
- [ ] Support quirks: broken FPDT, bad MADT checksum, GOP pitch lies, bogus MAT, USB handoff blacklist.
- [ ] Log all active quirks and include them in BlackBox reports.
- [ ] Add safe override in `boot.conf`.
- [ ] Commit: `"boot: firmware quirk database"`

## 10. Firmware Inventory Tests and Host Decoder

- [ ] Unit tests for GUID lookup, range rejection, checksum failures, and conformance levels.
- [ ] Add fixture blobs for ACPI RSDP, SMBIOS3, ESRT, FPDT, and DTB.
- [ ] Add host tool mode to decode `firmware-tables.json`.
- [ ] Verify on QEMU OVMF, VirtualBox EFI, and at least two bare-metal machines.
- [ ] Commit: `"test: firmware table inventory coverage"`

## OS Comparison

| ⭐ | Feature | Windows | Linux | Impossible OS |
| --- | --- | --- | --- | --- |
| 💎 | ACPI/SMBIOS inventory | WMI / ACPI HAL | sysfs, dmidecode | TODO-04 |
| 💎 | ESRT exposure | Windows Update firmware | fwupd | TODO-04 §8 |
| 💎 | DTB/EBBR path | ARM-specific | device tree | TODO-04 §3 |
| ⭐ | BlackBox firmware report | ETW/internal | scattered logs | TODO-04 §7 |

## Unit Tests

- [ ] `test_firmware_table_guid_lookup`
- [ ] `test_firmware_table_range_rejects_unmapped`
- [ ] `test_firmware_table_checksum_fail`
- [ ] `test_firmware_conformance_profiles`

## Verification

- [ ] `scripts/debug/kernel/run-boot-tests.bat`
- [ ] QEMU OVMF with ACPI + SMBIOS + FPDT
- [ ] VirtualBox EFI
- [ ] Bare metal laptop and desktop

