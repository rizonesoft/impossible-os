---
schema_version: 1
id: firmware-table-platform-inventory
domain: 01-boot-platform
status: active
title: "TODO-04 -- Firmware Table & Platform Inventory"
---

# TODO-04 -- Firmware Table & Platform Inventory

> **Goal:** Make firmware-provided platform data complete, validated, and queryable. The bootloader already copies UEFI configuration-table entries and the kernel has helpers for ACPI, SMBIOS, memory attributes, runtime properties, conformance profiles, ESRT, DTB, and FPDT. This TODO owns the generic firmware inventory layer that discovers, validates, logs, and publishes those tables without mixing policy into each consumer.

> [!IMPORTANT]
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

| ⭐ | Order | Deliverable                                             | Depends On     | Status |
| -- | :---: | ------------------------------------------------------- | -------------- | :----: |
| 💎 |   1   | Firmware table catalog API                              | TODO-01 §1     |  [ ]   |
| 💎 |   2   | Physical range and checksum validation                  | §1             |  [ ]   |
| 💎 |   3   | ACPI/SMBIOS/DTB table arbitration                       | §1, §2         |  [ ]   |
| 💎 |   4   | FPDT and boot timing normalization                      | §1             |  [ ]   |
| 💎 |   5   | UEFI memory attributes and runtime properties inventory | §1, TODO-27 §3 |  [ ]   |
| 💎 |   6   | ESRT firmware inventory mirror                          | §1, TODO-27 §2 |  [ ]   |
| 💎 |   7   | UEFI conformance profile and EBBR detection             | §1             |  [ ]   |
| 💎 |   8   | Registry and BlackBox firmware report                   | §1-§7          |  [ ]   |
| ⭐ |   9   | Firmware quirk database                                 | §8             |  [ ]   |
| ⭐ |  10   | Firmware inventory tests and host decoder               | §1-§9          |  [ ]   |

---

## 1. Firmware Table Catalog API

- [ ] Define `firmware_table_entry_t` with GUID/name, physical address, size, source, validation status, and owner.
- [ ] Implement `firmware_tables_init()` from `boot_info.config_table[]`.
- [ ] Add lookup by GUID, name, and owner subsystem.
- [ ] Emit a compact boot log summary with table count and critical table presence.
- [ ] Commit: `"boot: firmware table catalog API"`

**Test checkpoint:** Boot log shows `[BOOT] firmware tables: <N> cataloged` line; `firmware_table_lookup_guid(EFI_ACPI_20_TABLE_GUID)` returns a valid entry on QEMU OVMF + VirtualBox EFI; lookup of an unknown GUID returns NULL. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 2. Physical Range and Checksum Validation

- [ ] Validate every table pointer against the UEFI memory map before dereference.
- [ ] Verify ACPI RSDP/XSDT checksums, SMBIOS entry-point checksums, FPDT lengths, and ESRT bounds.
- [ ] Mark bad tables as degraded, not silently absent.
- [ ] Add `firmware_table_validate_all()` test hooks.
- [ ] Commit: `"boot: validate firmware table ranges and checksums"`

**Test checkpoint:** Synthetic test corrupts an ACPI RSDP checksum byte; `firmware_table_validate_all()` reports the table as `status: degraded` with reason `checksum_fail`; clean OVMF boot reports zero degraded tables. Pointer outside the UEFI memory map -> `status: degraded` with reason `range_unmapped`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 3. ACPI/SMBIOS/DTB Table Arbitration

- [ ] Define priority when ACPI 2.0, ACPI 1.0, SMBIOS3, SMBIOS2, and DTB are all present.
- [ ] Add DTB handoff validation and basic `/chosen`/memory/cpu discovery for EBBR systems.
- [ ] Document that PC-class hardware must use ACPI for interrupt/timer ownership.
- [ ] Add registry flags for `FirmwarePlatform=ACPI|DTB|Hybrid`.
- [ ] Commit: `"boot: arbitrate ACPI SMBIOS DTB firmware sources"`

**Test checkpoint:** PC-class boot logs `[BOOT] firmware platform: ACPI` and registry has `FirmwarePlatform=ACPI`. Synthetic DTB-only boot logs `FirmwarePlatform=DTB` with `/chosen` parsed. Hybrid (ACPI + DTB present) logs the priority decision. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 4. FPDT and Boot Timing Normalization

- [ ] Normalize FPDT timestamps and bootloader TSC timestamps into one timeline.
- [ ] Detect zero/garbage FPDT records and mark as unreliable.
- [ ] Feed `boot_timing.c` and VPD from the same normalized source.
- [ ] Export JSON to `X:\Perf\boot-timeline.json`.
- [ ] Commit: `"boot: normalize FPDT and bootloader timing"`

**Test checkpoint:** `X:\Perf\boot-timeline.json` exists post-boot with monotonically increasing timestamps from FPDT + bootloader TSC unified. Zero-FPDT firmware (VirtualBox) marks FPDT records `unreliable: true`. `boot_timing.c` and VPD report identical phase totals. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 5. UEFI Memory Attributes and Runtime Properties Inventory

- [ ] Parse and catalog EFI Memory Attributes Table.
- [ ] Parse Runtime Properties Table and record unavailable services.
- [ ] Expose runtime code/data W^X status to TODO-27 §3.
- [ ] Add warning when runtime service pointers exist but properties table says unsupported.
- [ ] Commit: `"boot: inventory UEFI MAT and RT properties"`

**Test checkpoint:** Boot log lists every MAT entry with attributes (`EFI_MEMORY_RP/RX/XP`); runtime W^X status visible to TODO-27 §3 consumers. Firmware advertising `EFI_RT_SUPPORTED_SET_VARIABLE` but having a NULL `SetVariable` pointer logs `[WARN] firmware: SetVariable property mismatch`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 6. ESRT Firmware Inventory Mirror

- [ ] Parse ESRT entries into kernel-owned structs.
- [ ] Mirror firmware device GUID, type, current version, capsule flags, and last attempt status into Registry.
- [ ] Include ESRT in hardware dump and support bundle.
- [ ] Feed TODO-27 capsule update policy.
- [ ] Commit: `"boot: mirror ESRT firmware inventory"`

**Test checkpoint:** ESRT-bearing firmware (modern bare-metal laptop, OVMF with ESRT) populates `HKLM\HARDWARE\Firmware\ESRT\<GUID>\*` with all 5 fields per entry; ESRT-absent firmware (VirtualBox EFI) leaves the registry key empty without errors. TODO-27 §2 capsule policy reads `current_version` correctly. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 7. UEFI Conformance Profile and EBBR Detection

- [ ] Parse conformance profile table.
- [ ] Detect UEFI full profile, EBBR, and unknown profile GUIDs.
- [ ] Relax or tighten required table set based on profile.
- [ ] Add diagnostics when hardware claims EBBR but exposes PC-only assumptions.
- [ ] Commit: `"boot: detect UEFI conformance profile"`

**Test checkpoint:** PC-class boot logs `[BOOT] UEFI conformance: full`. Synthetic EBBR profile boot logs `conformance: EBBR` and skips required-PC-table checks. Hybrid case (EBBR-claimed + PIC/i8042 present) logs `[WARN] firmware: EBBR claim with PC-only hardware`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 8. Registry and BlackBox Firmware Report

- [ ] Create `HKLM\HARDWARE\Firmware\Tables\*`.
- [ ] Write `X:\Diag\firmware-tables.json` on boot.
- [ ] Include validation failures, table addresses, checksums, and quirk matches.
- [ ] Add `sysinfo.exe` integration for firmware inventory.
- [ ] Commit: `"boot: publish firmware inventory report"`

**Test checkpoint:** Post-boot, `X:\Diag\firmware-tables.json` exists and validates against the host decoder (§10). `HKLM\HARDWARE\Firmware\Tables\<GUID>\Address` reads back the same physical address that `firmware_table_lookup_guid()` returns. `sysinfo.exe firmware` prints the same table set with checksums and quirk matches. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 9. Firmware Quirk Database

- [ ] Add a small table keyed by SMBIOS vendor/product/BIOS version.
- [ ] Support quirks: broken FPDT, bad MADT checksum, GOP pitch lies, bogus MAT, USB handoff blacklist.
- [ ] Log all active quirks and include them in BlackBox reports.
- [ ] Add safe override in `boot.conf`.
- [ ] Commit: `"boot: firmware quirk database"`

**Test checkpoint:** Synthetic SMBIOS vendor=`TestVendor` product=`BrokenFPDT` triggers the broken-FPDT quirk and `firmware-tables.json.quirks_active[]` contains `broken_fpdt`. `boot.conf` line `firmware_quirk_disable=broken_fpdt` overrides the trigger. BlackBox dump includes `quirks_active`. Test on: QEMU WHPX (custom SMBIOS), QEMU TCG, VirtualBox, bare metal.

---

## 10. Firmware Inventory Tests and Host Decoder

- [ ] Unit tests for GUID lookup, range rejection, checksum failures, and conformance levels.
- [ ] Add fixture blobs for ACPI RSDP, SMBIOS3, ESRT, FPDT, and DTB.
- [ ] Add host tool mode to decode `firmware-tables.json`.
- [ ] Verify on QEMU OVMF, VirtualBox EFI, and at least two bare-metal machines.
- [ ] Commit: `"test: firmware table inventory coverage"`

**Test checkpoint:** All 4 unit tests in `## Unit Tests` PASS under `SUITE=boot`. Host decoder tool reads a captured `firmware-tables.json` and prints a human-readable table inventory. Fixture blobs in `src/kernel/test/fixtures/firmware/` cover RSDP / SMBIOS3 / ESRT / FPDT / DTB shapes. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## OS Comparison

| ⭐ | Feature                               | 🪟 Win11                         | 🐧 Linux                        | 🚀 Impossible OS                |
| -- | ------------------------------------- | -------------------------------- | ------------------------------- | ------------------------------- |
| 💎 | Firmware table catalog API            | ✅ ACPI/HAL + WMI                | ✅ acpi_get_table + sysfs       | ⬜ §1 firmware_table_entry_t    |
| 💎 | Physical range + checksum validation  | ✅ HAL validates pre-use         | ✅ acpi_tb_verify_checksum      | ⬜ §2 mmap-bound + checksums    |
| 💎 | ACPI/SMBIOS/DTB arbitration           | ⚠️ ACPI-first; no DTB          | ✅ ACPI on x86, DTB on ARM      | ⬜ §3 priority + EBBR detect    |
| 💎 | FPDT boot-timeline normalization      | ✅ FPDT + ETW timeline           | ⚠️ acpi_fpdt_init read-only    | ⬜ §4 FPDT + TSC unified        |
| 💎 | UEFI MAT + Runtime Properties         | ✅ MmGetEfiRuntimeServicesTable  | ✅ efi_memmap_attributes        | ⬜ §5 MAT + RT properties       |
| 💎 | ESRT firmware inventory               | ✅ Windows Update / fwupdd       | ✅ fwupd /sys/firmware/efi/esrt | ⬜ §6 ESRT mirror to Registry   |
| 💎 | UEFI conformance profile + EBBR       | ⚠️ assumes full PC profile     | ✅ EBBR detection in efi-stub   | ⬜ §7 conformance GUID parse    |
| 💎 | Registry + BlackBox firmware report   | ✅ msinfo32 + Event Log          | ⚠️ scattered (dmidecode/sysfs) | ⬜ §8 HKLM\HARDWARE\Firmware  |
| ⭐ | Firmware quirk database               | ⚠️ HAL-internal, opaque        | ⚠️ DMI quirks scattered        | ⬜ §9 SMBIOS-keyed quirks       |
| ⭐ | Host decoder for firmware-tables.json | ❌ N/A                           | ❌ N/A                          | ⬜ §10 host tool decode JSON    |

---

## Unit Tests

- [ ] `test_firmware_table_guid_lookup`
- [ ] `test_firmware_table_range_rejects_unmapped`
- [ ] `test_firmware_table_checksum_fail`
- [ ] `test_firmware_conformance_profiles`

---

## Verification

- [ ] `bash scripts/build.sh` clean build with TODO-04 sources -> `=== BUILD OK ===`.
- [ ] `bash scripts/test.sh SUITE=boot` -> all 4 `test_firmware_*` cases PASS (per the Unit Tests section).
- [ ] QEMU OVMF (ACPI 2.0 + SMBIOS3 + FPDT present): boot log shows `[BOOT] firmware tables: <N> cataloged, <M> validated, <K> degraded`. `X:\Diag\firmware-tables.json` exists with at least RSDP, SMBIOS3, FPDT entries.
- [ ] VirtualBox EFI (ACPI + SMBIOS, no FPDT/ESRT): boot succeeds; firmware report flags FPDT + ESRT as absent (not degraded).
- [ ] Bare-metal laptop + desktop: every active firmware quirk is logged + included in the BlackBox report; no silent table-validation failures on serial.
- [ ] Commit: `"docs/firmware: TODO-04 verification complete"`

**Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 4 firmware-inventory sub-tests, 0 failures (target on full implementation)

