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
> **Current state (2026-04-29):** `boot_info.config_table[]` exists, `uefi_find_config_table()` exists at uefi_config.c:47, SMBIOS3+SMBIOS2 parsing at smbios.c:381,429, FPDT timing at boot_timing.c:8-154, `esrt_init()` at uefi_config.c:183, MAT lookup at uefi_config.c:268, Runtime Properties at uefi_runtime.c:199, and EFI_CONFORMANCE_PROFILES_TABLE parsing (UEFI_CONFORM_FULL / EBBR) at uefi_config.c:107-140. **TODO-02 §14 (commit 194e6012) shipped `acpi_enumerate_signatures` / `acpi_get_raw_table` / `smbios_get_raw_table` with hostile-field validation and the Win32 `GetSystemFirmwareTable` / `EnumSystemFirmwareTables` / `SystemFirmwareTableInformation` syscall surface** -- this TODO's §1 catalog API consumes those helpers rather than re-implementing per-provider validation. There is still no unified `firmware_table_entry_t` catalog, no `HKLM\HARDWARE\Firmware\Tables\*` registry mirror, no validation of table physical ranges against the UEFI memory map, no DTB/EBBR handoff path, no FPDT+TSC timeline normalization, and no `firmware-tables.json` host decoder.

## Inputs

- [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h)
- [`include/kernel/uefi_config.h`](../../include/kernel/uefi_config.h)
- [`include/kernel/smbios.h`](../../include/kernel/smbios.h)
- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c)
- -> XREF: `TODO-02-uefi-hardening-secureboot.md §5` -- existing SMBIOS base parser
- -> XREF: `TODO-02-uefi-hardening-secureboot.md §14` -- Win32 GetSystemFirmwareTable + EnumSystemFirmwareTables surface (commit 194e6012); §1 here consumes `acpi_enumerate_signatures` / `acpi_get_raw_table` / `smbios_get_raw_table` from that section rather than re-implementing per-provider validation
- -> XREF: `TODO-11-interrupt-timer-arch.md §1` -- ACPI MADT remains the interrupt owner
- -> XREF: `TODO-27-uefi-advanced.md §2,§6` -- ESRT/capsule and extended SMBIOS consumers
- -> XREF: `02-kernel-core/TODO-14-registry-completion.md §4` -- `HKLM\HARDWARE\Firmware\*` storage via Nt/Zw registry syscalls (consumer for §8)
- -> XREF: `04-drivers-hardware/TODO-04-security-hardware.md §4` -- IOMMU/VT-d/AMD-Vi parses ACPI DMAR/IVRS; consumer of the §1 firmware-table catalog
- -> XREF: `04-drivers-hardware/TODO-08-core-driver-enhancements.md §1` -- PCIe ECAM via ACPI MCFG; consumer of the §1 firmware-table catalog
- -> XREF: `01-boot-platform/TODO-13-tpm-measured-boot-attestation.md` -- TPM TCPA/TPM2 ACPI tables and event-log mirror; this TODO catalogs the table; TODO-13 owns the measured-boot pipeline
- -> XREF: `16-platform-portability/TODO-01-multi-arch-port.md` -- IORT (ARM IO Remapping Table) catalog work belongs to the ARM SBSA port; deferred from §1 catalog source enum until that domain activates

## Outcome

- Every firmware table copied or referenced by the bootloader is range-checked, cataloged, and visible in diagnostics.
- ACPI, SMBIOS, DTB, FPDT, ESRT, memory attributes, runtime properties, and conformance profiles have one inventory API.
- Registry and BlackBox capture enough firmware metadata for support, crash triage, and hardware certification.
- Embedded/EBBR-style DTB systems have a defined path without pretending to be PC ACPI systems.

## Implementation Order

| ⭐ | Order | Deliverable                                             | Depends On     | Status |
| -- | :---: | ------------------------------------------------------- | -------------- | :----: |
| 💎 |   1   | Firmware table catalog API                              | TODO-01 §1     |  [x]   |
| 💎 |   2   | Physical range and checksum validation                  | §1             |  [x]   |
| 💎 |   3   | ACPI/SMBIOS/DTB table arbitration                       | §1, §2         |  [x]   |
| 💎 |   4   | FPDT and boot timing normalization                      | §1             |  [x]   |
| 💎 |   5   | UEFI memory attributes and runtime properties inventory | §1, TODO-27 §3 |  [x]   |
| 💎 |   6   | ESRT firmware inventory mirror                          | §1, TODO-27 §2 |  [/]   |
| 💎 |   7   | UEFI conformance profile and EBBR detection             | §1             |  [x]   |
| 💎 |   8   | Registry and BlackBox firmware report                   | §1-§7          |  [/]   |
| ⭐ |   9   | Firmware quirk database                                 | §8             |  [x]   |
| ⭐ |  10   | Firmware inventory tests and host decoder               | §1-§9          |  [ ]   |
| ⭐ |  11   | Firmware inventory hardening debt                       | §1-§9          |  [ ]   |

---

## 1. Firmware Table Catalog API

> [!NOTE]
> **Foundation already shipped:** `uefi_find_config_table()` (uefi_config.c:47) walks `boot_info.config_table[]` by GUID. `acpi_enumerate_signatures()` + `acpi_get_raw_table()` (acpi.c:972, 1041) and `smbios_get_raw_table()` (smbios.c:679) provide validated raw-byte access. SMBIOS3 + SMBIOS2 entry-point parsing exists at smbios.c:381,429. This section adds the higher-level `firmware_table_entry_t` catalog that aggregates these per-provider helpers behind one inventory API; do NOT re-implement the per-provider validation.

- [x] Define `firmware_table_entry_t` with GUID/name, physical address, size, source (`FW_SOURCE_UEFI_CFG_TABLE` / `FW_SOURCE_ACPI_SDT` / `FW_SOURCE_SMBIOS_RAW` / `FW_SOURCE_FPDT` / `FW_SOURCE_ESRT` / `FW_SOURCE_DTB`), validation status, and owner. Header at `include/kernel/firmware_tables.h`; cap `FIRMWARE_TABLE_MAX = 64` slots.
- [x] Implement `firmware_tables_init()` populating the catalog from `boot_info.config_table[]` plus per-provider accessors. ACPI uses `acpi_for_each_record()` (single validated XSDT/RSDT walk; preserves duplicate SSDTs as distinct entries) and SMBIOS uses `smbios_get_raw_table()`. Wired in `boot_interrupts.c` after `uefi_conformance_init()`.
- [x] Add `firmware_table_lookup_guid()` / `firmware_table_lookup_name()` / `firmware_table_lookup_owner()` plus `firmware_table_count()` / `firmware_table_get()`. NULL/zero-GUID/NULL-name guards in place.
- [x] Emit a compact boot log summary: `[BOOT] firmware tables: <N> cataloged, <M> validated, <K> degraded`. QEMU OVMF reports `17 cataloged, 9 validated, 0 degraded` (validated = entries that either had a per-provider oracle at catalog time OR were promoted by `firmware_table_validate_all` on a clean full-format pass; remaining 8 sit in `FW_STATUS_UNKNOWN_PROFILE` because their cfg-table GUID is unrecognized or has no full-format validator yet -- MAT/RtProps/Conform/DTB and vendor GUIDs are owned by later sections).
- [x] Commit: `"boot: firmware table catalog API"`

**Test checkpoint:** Boot log shows `[BOOT] firmware tables: <N> cataloged` line; `firmware_table_lookup_guid(EFI_ACPI_20_TABLE_GUID)` returns a valid entry on QEMU OVMF + VirtualBox EFI; lookup of an unknown GUID returns NULL. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 12 firmware-table sub-tests / 23 assertions, 0 failures (1 SKIP on QEMU OVMF: <2 SSDTs)
>
> **Notes:**
> - Shipped: `include/kernel/firmware_tables.h` + `src/kernel/firmware_tables.c` (~290 LOC catalog + lookups), unit tests at `src/kernel/test/test_firmware_tables.c`, plus `acpi_for_each_record()` / `acpi_get_record()` accessors on `acpi.c` for duplicate-preserving SDT enumeration.
> - How it runs: `firmware_tables_init()` is called once on the BSP at Phase 1 (after `uefi_conformance_init()`, before `sti`); read-only thereafter so lookups are lock-free.
> - Downstream effects: this catalog is the input for the §2 range/checksum validator and the §8 Registry/BlackBox firmware report; Codex 3-round adversarial review adoptions in commit `57d1aa79`.
> - Canonical doc: `include/kernel/firmware_tables.h` API contract.
> - Scope boundary: §1 records what per-provider helpers already validated. Range checks against the UEFI memory map and per-table checksum re-verification are owned by §2; ESRT/MAT/conformance details remain owned by §5/§6/§7.
>
> **Verified:** 2026-04-30 | commit `57d1aa79` | 5/5 items | build OK | smoke PASS (QEMU OVMF 2.30s)
> **Accepted:** [H] ACPI catalog walk dereferences firmware child pointers before range validation -> XREF: 01-boot-platform/TODO-04 §2 (item: "Validate every table pointer against the UEFI memory map before dereference" at line 69). Reason: pre-existing dereference surface from acpi_init / acpi_enumerate_signatures (commit 194e6012); §1 routes through that path without new pointer chases.
> **Quality reviewed:** 2026-04-30 | Codex 9x (design + adversarial + adversarial-impl + re-adversarial x4 + consistency + perf) | 6H+1M+0L fixed, 1H accepted-XREF | scope: kernel-code-quality

---

## 2. Physical Range and Checksum Validation

- [x] Validate every table pointer against the UEFI memory map before dereference -- `fw_mmap_contains()` in `firmware_tables.c` accepts only RESERVED/LOADER_*/BOOT_SERVICES_*/RUNTIME_*/ACPI_RECLAIM/ACPI_NVS/PERSISTENT; rejects CONVENTIONAL/UNUSABLE/MMIO/MMIO_PORT/PAL_CODE; addr+len overflow guard.
- [x] Verify ACPI RSDP/XSDT checksums, SMBIOS entry-point checksums, FPDT lengths, and ESRT bounds -- `fw_validate_acpi_rsdp` (v1 20-byte sum + v2 ext_checksum over declared length), `fw_validate_acpi_sdt` (header.length must match catalog size + full-table sum), `fw_validate_smbios_ep` (anchor + spec wire-format length 0x18 / [0x1E,0x1F] + sum), `fw_validate_fpdt` (SDT-shaped sum), `fw_validate_esrt` (count <= count_max + count*40 footprint range-checked, multiplication overflow guarded).
- [x] Mark bad tables as degraded, not silently absent -- one-way downgrade via `fw_degrade()`; `e->status = FW_STATUS_DEGRADED` + `degraded_reason` populated; entry remains in catalog with original phys_addr so consumers can still inspect.
- [x] Add `firmware_table_validate_all()` test hooks -- public `firmware_table_validate_all()` declared in `firmware_tables.h`; `KERNEL_TESTS`-gated `firmware_table_validate_one_for_test()` with `bypass_range_check` flag for synthetic-buffer testing.
- [x] Commit: `"boot: validate firmware table ranges and checksums"`

**Test checkpoint:** Synthetic test corrupts an ACPI RSDP checksum byte; `firmware_table_validate_all()` reports the table as `status: degraded` with reason `checksum_fail`; clean OVMF boot reports zero degraded tables. Pointer outside the UEFI memory map -> `status: degraded` with reason `range_unmapped`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 11 validator sub-tests / 19 assertions, 0 failures (TCG: clean SDT VALIDATED, checksum corruption -> CHECKSUM_FAIL, size-below-header / size mismatch / oversized SDT catalog size / oversized FPDT declared length -> LENGTH_BAD, CONVENTIONAL phys_addr -> RANGE_UNMAPPED, NULL phys_addr -> NULL_POINTER, one-way downgrade preserved, UNKNOWN_PROFILE promoted on full pass, validate_all clean firmware no validator degradations)
>
> **Notes:**
> - Shipped: `firmware_table_validate_all()` + per-source helpers (~330 LOC) in `src/kernel/firmware_tables.c`; private wire-format struct views (RSDP v1/v2, SDT header, SMBIOS3/SMBIOS2 entry-point, ESRT header) pinned with `_Static_assert` on size; 8 new test functions / 14 assertions in `src/kernel/test/test_firmware_tables.c`.
> - How it runs: `firmware_tables_init()` calls the validator after the three catalog steps and before the boot summary, so the cataloged/validated/degraded counts in `[BOOT] firmware tables: ...` reflect validator findings; idempotent (one-way downgrade, second call is a no-op).
> - Downstream effects: §8 Registry/BlackBox firmware report consumes `entry->status` + `degraded_reason`; §6 ESRT mirror trusts the validated entry list; Codex 1x adversarial review approved with one residual non-blocking note (test bypass flag global, accepted: test runner is single-threaded by design).
> - Canonical doc: `include/kernel/firmware_tables.h` `firmware_table_validate_all()` contract.
> - Scope boundary: §2 marks status only; consumers act on it. §3 owns full DTB header parsing (validator only range-checks DTB base byte). §6 owns ESRT mirror to Registry. §7 owns conformance-profile flagging. §8 owns Registry + JSON publication. §10 owns SMBIOS wire-format constant consolidation (deferred from this section).

> **Verified:** 2026-04-30 | commit `789b572f` | 5/5 items | build OK | smoke PASS (KVM 2.35s, "17 cataloged, 9 validated, 0 degraded") | tests 710/710 PASS
> **Accepted:** [L] SMBIOS3/2 entry-point wire-format length constants duplicated between `smbios.c` (`SMBIOS3_EP_LEN` / `SMBIOS2_EP_LEN_*`) and `firmware_tables.c` (`FW_SMBIOS3_EP_LEN` / `FW_SMBIOS2_EP_LEN_*`); future-spec drift hazard (reason: scope -- consolidating into a shared internal header touches `src/kernel/smbios.c` private types) -> XREF: 01-boot-platform/TODO-04 §11 (item: "Consolidate SMBIOS entry-point wire-format constants" at line 315)
> **Quality reviewed:** 2026-04-30 | Codex 5x (design + adversarial + re-adversarial x2 + consistency + perf) | 2M+2L fixed, 1L accepted-XREF | scope: kernel-code-quality

---

## 3. ACPI/SMBIOS/DTB Table Arbitration

- [x] Define priority when ACPI 2.0, ACPI 1.0, SMBIOS3, SMBIOS2, and DTB are all present -- `firmware_platform_init()` in `src/kernel/firmware_platform.c` picks ACPI > DTB; HYBRID only when both validated; ACPI 2.0 trumps ACPI 1.0 in `s_acpi_version`. SMBIOS is informational (never decides ACPI vs DTB). PC-class doctrine pins interrupt/timer ownership to ACPI when present; HYBRID logs primary=ACPI, secondary=DTB.
- [x] Add DTB handoff validation and basic `/chosen`/memory/cpu discovery for EBBR systems -- `src/kernel/dtb.c` validates FDT v17 header (magic 0xD00DFEED big-endian, totalsize <= 4 MiB cap, version >= 16, last_comp_version <= 17, off_dt_struct + size_dt_struct in bounds), then walks the structure block once, counting nodes whose names match the unit-address convention "chosen", "memory" / "memory@*", and "cpu@*". Header + body both span-checked against the firmware-bearing UEFI mmap via `firmware_table_mmap_contains` before any dereference (Codex H1 fix). HasDTB is gated on `dtb_memory_count() > 0 && dtb_cpu_count() > 0` so a structurally-valid empty DTB stays Unknown (Codex M1 fix).
- [x] Document that PC-class hardware must use ACPI for interrupt/timer ownership -- doctrine documented in `include/kernel/firmware_platform.h` header docstring + arbitration comment in `firmware_platform_init`. Linux kernel arbitrates the same way on ARM SBSA + DTB systems; HYBRID is a diagnostic, not permission to mix interrupt sources.
- [x] Add registry flags for `FirmwarePlatform=ACPI|DTB|Hybrid` -- `firmware_platform_populate_registry()` writes `HKLM\SYSTEM\Boot\Firmware\FirmwarePlatform` (REG_SZ) plus `HasACPI` / `HasDTB` / `HasSMBIOS` / `AcpiVersion` (REG_DWORD) and `DtbTotalSize` when DTB is present. Sibling of `HKLM\SYSTEM\Boot\Device\*` populated by TODO-05; reserved exclusively from `HKLM\HARDWARE\Firmware\Tables` which the §8 firmware-report owns.
- [x] Commit: `"boot: arbitrate ACPI SMBIOS DTB firmware sources"`

**Test checkpoint:** PC-class boot logs `[BOOT] firmware platform: ACPI` and registry has `FirmwarePlatform=ACPI`. Synthetic DTB-only boot logs `FirmwarePlatform=DTB` with `/chosen` parsed. Hybrid (ACPI + DTB present) logs the priority decision. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 19 sub-tests / 38 assertions, 0 failures (DTB: phys_addr=0 invalid, bad magic invalid, minimal blob valid + 4-node-count assertions, oversized totalsize / unsupported version / truncated struct / outside-firmware-mmap reject, empty-root has zero memory+cpu, memorytest prefix not counted as memory, unbalanced BEGIN/END rejects + accumulator stays 0, multi-root rejects + accumulator stays 0, misnested memory/cpu under bogus parent not counted; FW platform: name table strings, ACPI classification on PC, AcpiVersion consistent; FW catalog promote: UNKNOWN_PROFILE -> VALIDATED + idempotent, unknown name -> 0, NULL name -> 0)
>
> **Notes:**
> - Shipped: `src/kernel/dtb.c` (~210 LOC FDT v17 header + bounded structure-block walker), `src/kernel/firmware_platform.c` (~140 LOC arbitration + Boot\Firmware Registry mirror), `include/kernel/dtb.h` + `include/kernel/firmware_platform.h`, `src/kernel/test/test_firmware_platform.c` (12 tests / 23 assertions). Public `firmware_table_mmap_contains` exposed from firmware_tables.c so DTB consumer reuses the same firmware-region oracle.
> - How it runs: `firmware_platform_init()` runs once on the BSP at Phase 1 immediately after `firmware_tables_init()`; `firmware_platform_populate_registry()` runs from `registry_populate_defaults()` after `boot_device_populate_registry()`. Read-only thereafter.
> - Downstream effects: `HKLM\SYSTEM\Boot\Firmware` is the canonical platform fingerprint for sysinfo / diagnostic tools; §6 ESRT mirror, §7 conformance flagging, and §8 firmware-tables.json all read `firmware_platform_get()` to gate their PC-class vs EBBR behavior. Codex 1x adversarial review adoptions in commit `f6b5c94b`.
> - Canonical doc: `include/kernel/firmware_platform.h` arbitration contract + `include/kernel/dtb.h` FDT validator contract.
> - Scope boundary: §3 owns DTB header validation + node counts only. Full /chosen/bootargs string extraction, /memory@N reg ranges, and /cpu@N compatible strings are owned by a future DTB parser TODO. §6 owns ESRT Registry mirror. §7 owns conformance flagging. §8 owns the firmware-tables.json export and HARDWARE\Firmware\Tables key.

> **Verified:** 2026-04-30 | commit `f6b5c94b` | 5/5 items | build OK | smoke PASS (KVM 2.38s, "firmware platform: ACPI") | tests 735/735 PASS
> **Quality reviewed:** 2026-04-30 | Codex 8x (design + adversarial + re-adversarial x6 + consistency + perf) | 1H+5M fixed, 0 open | scope: kernel-code-quality

---

## 4. FPDT and Boot Timing Normalization

- [x] Normalize FPDT timestamps and bootloader TSC timestamps into one timeline -- `boot_timing_bl_entry_ms_since_reset()` returns the FPDT `os_loader_start_start` ns sample (converted to ms) as the absolute time of the bootloader `bl_entry` TSC sample; `boot_timeline_dump_json()` then offsets every TSC step by that anchor so FPDT firmware phases and TSC kernel steps share one ms-since-reset axis. When FPDT is unreliable the anchor collapses to 0 and TSC entries are ms-since-bl_entry.
- [x] Detect zero/garbage FPDT records and mark as unreliable -- `boot_timing_fpdt_unreliable_eval()` (pure helper for tests) + `boot_timing_fpdt_unreliable()` (live wrapper) reject `fpdt_available==0`, all-zero records (VirtualBox EFI), zero-after-non-zero fields, non-monotonic phase order, and any single field above a 10-minute sanity cap.
- [x] Feed `boot_timing.c` and VPD from the same normalized source -- new `boot_timing_uefi_total_ms()` shared helper computes `(kernel_jump - bl_entry) * 1000 / tsc_freq` once; `boot_timing_init()` and `vpd.c` "UEFI Boot:" cell both call it instead of recomputing the formula inline (drift-proof).
- [x] Export JSON to `X:\Perf\boot-timeline.json` -- `boot_timeline_dump_json()` (`src/kernel/main/boot_progress.c`) prepends 5 FPDT entries (`source:"fpdt"`, `unreliable:bool`) before the existing TSC steps (`source:"tsc"`), now writes to `X:\Perf\` (was `X:\Boot\`); buffer raised to 16 KiB to fit worst-case 64 TSC + 5 FPDT records, trailing-comma stripped on truncation so JSON always closes cleanly. Per-delta math goes through a `safe_tsc_delta_ms()` helper that clamps reverse-ordered TSC samples to 0 instead of wrapping unsigned subtractions.
- [x] Commit: `"boot: normalize FPDT and bootloader timing"`

**Test checkpoint:** `X:\Perf\boot-timeline.json` exists post-boot with monotonically increasing timestamps from FPDT + bootloader TSC unified. Zero-FPDT firmware (VirtualBox) marks FPDT records `unreliable: true`. `boot_timing.c` and VPD report identical phase totals. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 10 FPDT-normalization sub-tests / 13 assertions, 0 failures (unavailable / all-zero / zero-anchor / clean monotonic / non-monotonic / zero-after-nonzero / leading-zero / 10-min cap / fixed schema / cap+null guards)
>
> **Notes:**
> - Shipped: `boot_timing_fpdt_unreliable[_eval]` + `boot_timing_uefi_total_ms` + `boot_timing_tsc_delta_ms` + `boot_timing_bl_entry_ms_since_reset` + `boot_timing_get_fpdt_entries` (~150 LOC) in `src/kernel/boot_timing.c` with a saturating `tsc_to_ms` (no `__udivti3` link); JSON exporter rewrite (~110 LOC delta) in `src/kernel/main/boot_progress.c` with `safe_tsc_delta_ms` + `sat_add_u32` and a 16 KiB buffer; VPD "UEFI Boot:" cell uses the shared helper; 10 unit tests in `src/kernel/test/test_boot_timing.c`.
> - How it runs: helpers are pure reads of `g_boot_info.timing` so they are lock-free and SMP-safe; `boot_timing_uefi_total_ms()` reads `tsc_freq` directly from `g_boot_info` so it works during VPD Phase 0 render before `boot_timing_init()` caches the value; `boot_timeline_dump_json()` runs once on the BSP at end of Phase 3 alongside the existing perf dumps. KVM smoke 2.32s on OVMF (FPDT not exposed; falls through unreliable path correctly).
> - Downstream effects: closes `boot-timeline.json` path drift between TODO-02, TODO-11 §9, TODO-14, TODO-24 (now all point to `X:\Perf\`); enables host decoder work in §10 to consume one schema with FPDT + TSC sources. Codex 6x adversarial + 1x consistency + 1x perf review adoptions in commits `d409f20a` + this review commit -- buffer overflow, TSC wraparound, zero-anchor, fallback-mislabel, ticks*1000 overflow, reverse-order clamp, dur_ms saturation, VPD Phase 0 ordering all fixed.
> - Canonical doc: `include/kernel/boot_timing.h` FPDT + TSC normalization API + `include/kernel/boot_progress.h` JSON contract docstring.
> - Scope boundary: §4 owns FPDT detection + JSON normalization. Per-step name JSON escape and full schema doc are owned by TODO-14 §9. Bare-metal FPDT validation across multiple firmware vendors is the §10 host-decoder + bare-metal verification step.

> **Verified:** 2026-04-30 | commit `d409f20a` | 5/5 items | build OK | smoke PASS (KVM 2.32s) | tests 752/752 PASS
> **Quality reviewed:** 2026-04-30 | Codex 8x (design + adversarial + re-adversarial x5 + consistency + perf) | 1H+9M+1L fixed, 0 open | scope: kernel-code-quality

---

## 5. UEFI Memory Attributes and Runtime Properties Inventory

- [x] Parse and catalog EFI Memory Attributes Table -- `mat_init()` (`src/kernel/uefi_config.c`) caches up to `MAT_MAX_ENTRIES = 128` descriptors into `s_mat_entries[]` with `{phys_addr, num_pages, attribute, cls}`; classification covers `MAT_CLASS_GUARD`/`CODE`/`DATA`/`RODATA`/`WX_VIOLATION` derived from the `EFI_MEMORY_RP/RO/XP` attribute bits. Per-entry `klog INFO` line `MAT[i]: phys=<addr> pages=<N> attr=<RP|RX|RO|RW|WX!>` (capped at 32 entries with `+N more` summary); overflow beyond cap triggers a `LOG_WARN` so the cache cap is auditable.
- [x] Parse Runtime Properties Table and record unavailable services -- `read_rt_properties()` already reads `EFI_RT_PROPERTIES_TABLE`; new `rt_check_property_pointer_mismatch()` cross-checks every named service (14 entries: GetTime, SetTime, GetWakeupTime, SetWakeupTime, GetVariable, GetNextVariableName, SetVariable, SetVirtualAddressMap, ConvertPointer, GetNextHighMonoCount, ResetSystem, UpdateCapsule, QueryCapsuleCapabilities, QueryVariableInfo) against its corresponding `s_rt->method` pointer; mismatches counted via `uefi_rt_property_mismatches()`.
- [x] Expose runtime code/data W^X status to TODO-27 §3 -- new public API `mat_get_count()` / `mat_get_entry(idx, *out)` / `mat_get_code_pages()` / `mat_get_data_pages()` / `mat_get_guard_pages()` / `mat_overflowed()` lets TODO-27 §3 walk regions and call `vmm_set_nx`/`vmm_set_ro` per-region instead of re-walking the firmware table. `mat_class_t` enum + `mat_classify_attr()` pure helper enable test-side synthesis.
- [x] Add warning when runtime service pointers exist but properties table says unsupported -- both directions caught: `[WARN] UEFI: firmware: <Service> property mismatch (pointer=VALID|NULL, supported=0|1)`. Codex H1 fix: when firmware advertises a service but the pointer is NULL, the bit is also CLEARED from `s_supported` so wrappers gating on the bitmask cannot null-deref a phantom service (the reverse direction stays diagnostic-only because wrappers already refuse to call services UEFI 2.10 section 4.6.2 says are unsupported).
- [x] Commit: `"boot: inventory UEFI MAT and RT properties"`

**Test checkpoint:** Boot log lists every MAT entry with attributes (`EFI_MEMORY_RP/RX/XP`); runtime W^X status visible to TODO-27 §3 consumers. Firmware advertising `EFI_RT_SUPPORTED_SET_VARIABLE` but having a NULL `SetVariable` pointer logs `[WARN] firmware: SetVariable property mismatch`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 11 MAT classify+inventory sub-tests + RT property mismatch sub-tests, 0 failures (GUARD/CODE/DATA/RODATA/WX classify, NULL+range guards, count <= cap, mismatch counter range, supported-implies-callable invariant)
>
> **Notes:**
> - Shipped: `mat_class_t` + `mat_entry_t` + 7 public accessors in `include/kernel/uefi_config.h`; ~140 LOC delta in `src/kernel/uefi_config.c` (cache, classify helper, per-entry logger, overflow warn); `int uefi_rt_property_mismatches(void)` in `include/kernel/uefi_runtime.h`; ~70 LOC `rt_check_property_pointer_mismatch()` in `src/kernel/uefi_runtime.c` covering 14 RT services with bidirectional mismatch detection + advertised-but-NULL-bit-clearing; 10 unit tests in `src/kernel/test/test_uefi_boot.c`.
> - How it runs: `mat_init()` and `uefi_runtime_init()` already run once on the BSP at Phase 1 (mat_init from `boot_interrupts.c`, runtime_init from boot path); both stay read-only afterwards so consumer queries are lock-free SMP-safe. KVM smoke 2.32s on OVMF: 27 MAT descriptors logged (4 code + 12 data + 1 WX violation pre-existing in OVMF + 10 guard).
> - Downstream effects: TODO-27 §3 W^X enforcement now has a concrete iteration API (no need to re-walk firmware table); §6 ESRT mirror, §7 conformance flagging, and §8 firmware-tables.json read the catalog without touching firmware again. Codex 1x adversarial review adoptions in commit `c2d12aba` -- H1 advertised-but-NULL pointer cleared from supported bitmask so wrappers cannot null-deref.
> - Canonical doc: `include/kernel/uefi_config.h` MAT inventory contract + `include/kernel/uefi_runtime.h` RT property mismatch contract.
> - Scope boundary: §5 detects + classifies + logs MAT entries and RT property mismatches; the actual W^X *enforcement* (vmm_set_nx / vmm_set_ro per region) is owned by TODO-27 §3. §6 ESRT firmware inventory is owned by §6. RT services per-offset header_size validation in pre-existing `call_set_virtual_address_map()` + 5-pointer init path is tracked in §11 (item: "Validate `s_rt->hdr.header_size` per function-pointer offset" at line 314).

> **Verified:** 2026-04-30 | commit `c2d12aba` | 5/5 items | build OK | smoke PASS (KVM 2.41s) | tests 764/764 PASS
> **Accepted:** [H] RT services per-offset header_size validation gap in pre-existing call_set_virtual_address_map() and 5-pointer init path (reason: scope -- pre-existing init code outside §5 inventory work) -> XREF: 01-boot-platform/TODO-04 §11 (item: "Validate `s_rt->hdr.header_size` per function-pointer offset" at line 314)
> **Quality reviewed:** 2026-04-30 | Codex 12x (design + adversarial-impl x9 + adversarial + consistency + perf) | 1H+9M+1L fixed, 1H accepted-XREF | scope: kernel-code-quality

---

## 6. ESRT Firmware Inventory Mirror

- [x] Parse ESRT entries into kernel-owned structs (already shipped at `src/kernel/uefi_config.c:184` -- `struct esrt_entry` carries all 7 UEFI 2.10 §23.6 entry fields). §6 added header-metadata mirror via `static struct esrt_table_header s_esrt_header` populated at `esrt_init()` so per-table count/count_max/version survive Phase 1.
- [x] Header accessors `esrt_resource_count_max()` + `esrt_resource_version()` (`include/kernel/uefi_config.h`); `esrt_count()` already covered `fw_resource_count`. All three are pure reads of the cached header.
- [x] Decoder helpers `esrt_decode_status()` (8 UEFI 2.10 Table 23-3 mnemonics + `Reserved` fallback) + `esrt_decode_type()` (4 §23.6 categories); both static `const char *` tables, zero-alloc, safe to print directly.
- [x] Capsule-policy primitives -- intentionally narrow per Codex design H1: `esrt_rollback_floor_ok(idx)` (FwVersion >= LowestSupportedFwVersion only) + `esrt_capsule_persists_across_reset(idx)` (CapsuleFlags bit `EFI_CAPSULE_PERSIST_ACROSS_RESET = 0x00010000` only). Out-of-range idx returns 0 cleanly. Capsule-update owner (TODO-27 advanced UEFI work) composes them with FwType + LastAttemptStatus + auth checks; this section is non-authoritative.
- [x] Registry mirror in `src/kernel/firmware_esrt_registry.c` -- `esrt_populate_registry()` writes `HKLM\HARDWARE\Firmware\ESRT\{<FwClass-GUID>}` per entry with all 7 entry fields (Type, FwVersion, LowestSupportedFwVersion, CapsuleFlags, LastAttemptVersion, LastAttemptStatus) + decoded `TypeName` + decoded `LastAttemptStatusName` (REG_SZ), plus a sibling `_Header` subkey carrying ResourceCount + ResourceCountMax + ResourceVersion. **Idempotent per Codex design M1**: `RegDeleteTree` clears the subtree on entry so removed firmware components do not survive across reboots; ESRT-absent boots leave the parent key empty. Wired in `registry_populate_defaults()` after `firmware_platform_populate_registry()`.
- [x] Smoke confirms behavior: `[ OK ] ESRT: Not present (no firmware inventory)` + `[ OK ] ESRT: Registry: ESRT absent or empty; HARDWARE\Firmware\ESRT cleared` on QEMU OVMF (which exposes no ESRT).
- [ ] **Deferred to §8**: ESRT block in `X:\Diag\firmware-tables.json` and BlackBox transcript. §8 owns the JSON schema + writer; this section provides the accessors + decoded labels §8 will consume.
- [ ] **Deferred to TODO-27 §2 (read-only firmware-update advisor)**: advisory composition (rollback-floor + persist-across-reset + FwType + LastAttemptStatus rendered as recommendation text) and the failure-path operator UX that renders `LastAttemptStatusName` to the user. **Note 2026-05-02:** TODO-27 §2 was rescoped from capsule delivery (`UpdateCapsule` + `OsIndications` write + ESP staging) to read-only LVFS-style advisor; Impossible OS does not write firmware. The §6 primitives feed the advisor's rollback warning + persist-capability rendering, not an actual capsule call.
- [x] Commit: `"boot: mirror ESRT firmware inventory"`

**Test checkpoint:** ESRT-bearing firmware (modern bare-metal laptop, OVMF with ESRT) populates `HKLM\HARDWARE\Firmware\ESRT\<FwClass-GUID>\*` with all 7 entry fields + 3 header fields; ESRT-absent firmware (VirtualBox EFI) leaves the registry key empty without errors. TODO-27 §2 capsule policy reads `LowestSupportedFwVersion` to enforce a rollback floor and renders `LastAttemptStatus` as a decoded label (e.g. `INSUFFICIENT_RESOURCES`, `INCORRECT_VERSION`). Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 5 ESRT decoder + capsule-helper sub-tests / 14 assertions, 0 failures (status known mnemonics + reserved fallback, type table, helpers OOR-idx -> 0, persist-bit position constant)
>
> **Notes:**
> - **What shipped** -- 5 public APIs in `include/kernel/uefi_config.h` (`esrt_resource_count_max`, `esrt_resource_version`, `esrt_decode_status`, `esrt_decode_type`, `esrt_rollback_floor_ok`, `esrt_capsule_persists_across_reset`, `esrt_populate_registry`) + 1 new constant `EFI_CAPSULE_PERSIST_ACROSS_RESET = 0x00010000`; ~60 LOC delta in `src/kernel/uefi_config.c` (header cache, decoder tables, two narrow capsule primitives); `src/kernel/firmware_esrt_registry.c` NEW (~150 LOC); 5 unit tests / 14 assertions in `test_uefi_boot.c`.
> - **How it runs** -- `esrt_init()` already runs once on the BSP at Phase 1 (caching all 7 entry fields + 3 header fields); `esrt_populate_registry()` fires from `registry_populate_defaults()` after `firmware_platform_populate_registry()`. Idempotent via `RegDeleteTree("HARDWARE\\Firmware\\ESRT")` on entry, so removed firmware components do not survive across reboots. ESRT-absent boots leave the parent key empty.
> - **Downstream effects** -- TODO-27 advanced UEFI capsule-update work consumes `esrt_rollback_floor_ok` + `esrt_capsule_persists_across_reset` as primitives (composing them with `FwType` + `LastAttemptStatus` + auth checks for authoritative eligibility per Codex design H1 split). §8 firmware-tables.json + BlackBox transcribe consume the same accessors for the JSON ESRT block. Codex 1x design + 1x adversarial in this commit; design adopted H1 (split eligibility primitives) + M1 (idempotent RegDeleteTree) + Q1 (brace GUID) + Q3 (static const char* tables); adversarial M1 fixed (REG_QWORD for ResourceVersion).
> - **Canonical doc** -- [`include/kernel/uefi_config.h`](../../include/kernel/uefi_config.h) ESRT API contract (UEFI 2.10 section 23.6 + section 8.5.3 references inline).
> - **Scope boundary** -- §6 owns kernel-side ESRT inventory + Registry mirror + decoder helpers + capsule-policy primitives. §8 owns the JSON `firmware-tables.json` ESRT block + BlackBox transcript. Authoritative capsule eligibility (composing rollback + persist + FwType + LastAttemptStatus + auth) is owned by TODO-27 advanced UEFI work; this section ships the primitives, not the policy.

> **Verified:** 2026-05-02 | this commit | 7/9 items + 2 deferred-to-owner | build OK | smoke PASS (KVM 2.39s) + ESRT-absent path observed
> **Quality reviewed:** 2026-05-02 | Codex 4x (design + adversarial + consistency + perf) | 1H+1M+1H+1M fixed, 0 open | scope: kernel-code-quality

---

## 7. UEFI Conformance Profile and EBBR Detection

> [!NOTE]
> **Partial foundation shipped:** EFI_CONFORMANCE_PROFILES_TABLE parsing already exists in `uefi_config.c` (the `s_conformance_level` / `UEFI_CONFORM_FULL` / `UEFI_CONFORM_EBBR` path at uefi_config.c:107-140). What remains is (a) feeding the result through the §1 catalog, (b) relaxing/tightening the required-table set, and (c) detecting EBBR-claim + PC-hardware contradictions.

- [x] Replaced the previous 3-enum (`UEFI_CONFORM_FULL/EBBR/UNKNOWN`) collapsed-winner detection with a **table-driven row registry** in `src/kernel/uefi_config.c` (`s_conformance_rows[]`) plus per-profile presence flags (`s_profile_present[UEFI_PROFILE_ID__COUNT]`). Adding a future profile (SBBR, ARM BBR, Microsoft-EBBR variant) is a one-row edit; reserved profiles are commented-only (no zero-GUID placeholders, per Codex design Q3). The legacy `UEFI_CONFORM_*` constants are kept as a backward-compat scalar derived from the presence flags.
- [x] **Pre-existing fabricated GUIDs replaced with spec-correct values**: `UEFI_PROFILE_UEFI_SPEC = 523c91af-a195-4382-818d-295fe4006465` (UEFI 2.10 section 4.6.5) + `UEFI_PROFILE_EBBR = cce33c35-74ac-4087-bce7-8b29b02eeb27` (Arm EBBR 2.1 per U-Boot mailing list). The header had `4b2a-9a5a-d00dd31a2427` / `4b2a-9088-58d50682f149` which never matched real firmware -- this section's audit caught it.
- [x] **Per-profile presence semantics** (Codex design H1): a system claiming BOTH UEFI Spec AND EBBR has both flags set, so `uefi_conformance_pc_contradiction()` and `uefi_conformance_allows_omit_pc_tables()` see EBBR independently of the display winner. New public API: `uefi_conformance_has_profile(id)`, `uefi_conformance_name()`, `uefi_conformance_allows_omit_pc_tables()`, `uefi_conformance_pc_contradiction()` in `include/kernel/uefi_config.h`.
- [x] **Bounded ECPT walk** (Codex design H2): UEFI cfg-table entries carry no length and ECPT itself has no total-length field, so per-GUID reads are bounded via `firmware_table_mmap_contains()` (the firmware-region oracle) plus a defensive `UEFI_CONFORM_PROFILE_MAX = 16` cap on `profile_count`. Header read also gated on the same oracle. Version mismatch (`ct->version != 1` per UEFI 2.10 section 4.6.5) returns UNKNOWN.
- [x] **Unknown-GUID surfacing**: every unmatched profile slot logs `[WARN] UEFI: Conformance: unknown profile GUID=<hex>...` (operator-readable; future profiles can be added via row edit without re-flashing kernel). Out-of-range slots log a separate `profile slot(s) outside firmware mmap; rejected` count.
- [x] **PC contradiction detector** (compile-gated `__x86_64__`): EBBR claim on x86_64 fires `[WARN] UEFI: Conformance: EBBR claim with PC-only architecture; treating as HYBRID` -- structural detection, no runtime port probe. Non-x86 builds return 0 by definition. The HYBRID treatment is policy-light: the WARN is the operator surface; per-profile flags stay truthful so downstream callers see the EBBR claim.
- [x] **Required-table policy primitive**: `uefi_conformance_allows_omit_pc_tables()` returns 1 iff EBBR-class profile present. Default-deny when no profile matches (silent omission is the worse failure mode). Consumed by §1 catalog status promotion + future §8 firmware-tables.json schema.
- [x] **6 unit tests** wired in `src/kernel/test/test_firmware_tables.c`: OOR id returns 0, name non-empty + non-NULL, pc_contradiction arch-gate (no EBBR -> 0; EBBR + x86_64 -> 1), omit-policy default-deny, level-vs-presence single-source-of-truth invariant, no-match name is one of the documented strings.
- [x] Commit: `"boot: detect UEFI conformance profile via registry"`

**Test checkpoint:** PC-class boot logs `[ OK ] UEFI: Conformance: UEFI Spec` (or `Full UEFI (assumed)` when ECPT absent). Synthetic EBBR boot logs `Conformance: EBBR` and the §1 catalog summary flips from WARN to `[ OK ] firmware tables: ... PC-class absent (allowed by profile)`. Hybrid case (EBBR + x86_64) logs `[WARN] UEFI: Conformance: EBBR claim with PC-only architecture; treating as HYBRID`. Unknown profile logs the FULL canonical GUID `8-4-4-4-12` hex. QEMU OVMF (no ECPT, FPDT/MAT absent) observed: `[ OK ] UEFI: Conformance: Full UEFI (table absent, assumed)` + `[WARN] BOOT: firmware tables: 17 cataloged, 9 validated, 0 degraded, 2 PC-class table(s) absent (FPDT/MAT/RtProps required by profile)`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 6 conformance sub-tests / 11 assertions, 0 failures (OOR id, name non-empty, pc_contradiction arch-gate, omit-policy default-deny, level-vs-presence consistency invariant, no-match name documented).
>
> **Notes:**
> - **What shipped** -- table-driven `s_conformance_rows[]` registry + `s_profile_present[]` per-profile flags in `src/kernel/uefi_config.c` (~120 LOC delta); 4 new public APIs in `include/kernel/uefi_config.h` (`has_profile`, `name`, `allows_omit_pc_tables`, `pc_contradiction`); 6 unit tests in `src/kernel/test/test_firmware_tables.c`; **fabricated UEFI Spec + EBBR GUIDs replaced with spec-correct UEFI 2.10 / Arm EBBR 2.1 values** (the previous values never matched real firmware).
> - **How it runs** -- `uefi_conformance_init()` runs once on the BSP at Phase 1 BEFORE `firmware_tables_init()`; reads ECPT with bounded mmap-checked walk (header + per-GUID via `firmware_table_mmap_contains` + `UEFI_CONFORM_PROFILE_MAX = 16` cap + `version != 1` rejection); idempotent (presence flags reset on entry). `firmware_tables_init()` consumes `uefi_conformance_allows_omit_pc_tables()` for the catalog summary log: 3-way message (all present / absent-allowed / absent-required) drives operator UX.
> - **Downstream effects** -- §1 catalog summary now reflects conformance policy (live OVMF emits the WARN form because no profile permits omission and 2 of 3 PC tables are absent). Codex 5x review (design + adversarial + consistency + perf + re-adversarial) caught 2H + 4M + 1M-fpdt-dual-source: GUID drift fixed, presence-flags drive policy independently of display winner, bounded ECPT walk via mmap oracle, full canonical GUID in unknown-profile WARN, init-order doc reconciled, FPDT via ACPI-SDT path now also recognized. Adoption details in commit message.
> - **Canonical doc** -- [`include/kernel/uefi_config.h`](../../include/kernel/uefi_config.h) conformance API contract (UEFI 2.10 section 4.6.5 + Arm EBBR 2.1 section 2.4 inline references).
> - **Scope boundary** -- §7 owns conformance detection + per-profile policy primitives + catalog summary integration. Per-profile fixture blobs (synthetic ECPT with each known GUID + bad-table rejection paths) are owned by §10 host-decoder + fixture inventory. ARM SBSA / SBBR / Microsoft-EBBR profile rows wait for published GUIDs before being added (placeholder-zero rows are foot-guns; commented-only until a real spec lands).

> **Verified:** 2026-05-02 | this commit | 8/8 items | build OK | smoke PASS (KVM 2.31s) + ECPT-absent path observed
> **Accepted:** [H] firmware_table_mmap_contains accepts UEFI_MMAP_BOOT_SERVICES_* which PMM reclaims before firmware_tables_init runs (reason: scope -- pre-existing oracle predates §7, affects §1/§2/§3/§5 equally) -> XREF: 01-boot-platform/TODO-04 §11 (item: "Tighten `firmware_table_mmap_contains` to reject `UEFI_MMAP_BOOT_SERVICES_CODE/DATA`" at line 313)
> **Quality reviewed:** 2026-05-02 | Codex 7x (design + adversarial-impl + adversarial × 2 + consistency + perf + re-adversarial) | 2H+5M fixed, 1H accepted-XREF | scope: kernel-code-quality

---

## 8. Registry and BlackBox Firmware Report

- [x] **Wire-format schema pinned**: `docs/boot/firmware-tables-schema.md` covers `schema_version=1`, top-level keys (`generated_at_utc`, `firmware_platform`, `conformance_profile`, `tables[]`, `degraded[]`, `quirks_active[]`, plus typed sub-blocks for `acpi/smbios/mat/rt_properties/esrt/apei/dbg2/wsmt`), per-entry shape (name, guid, source enum, phys_addr hex, size, checksum_status, validation_reason), ESRT byte-for-byte mirror of the §6 Registry value names (including u64/REG_QWORD `ResourceVersion` constraint and the UEFI 2.10 + UEFI 2.7+ mnemonics for `LastAttemptStatusName`), absent-vs-degraded semantics, RFC 8259 string-escape rules, and bump rules. The schema is now the single source of truth for kernel writer + Registry mirror + future host decoder + sysinfo tool + Win11 GetSystemFirmwareTable consumer.
- [x] **Registry mirror at `HKLM\HARDWARE\Firmware\Tables\<name>\*`** (`src/kernel/firmware_tables_registry.c`, ~135 LOC). One subkey per cataloged entry sanitized to Registry naming rules; values `Address` (REG_QWORD u64), `Size` (REG_DWORD), `Checksum` (REG_DWORD; placeholder 0 until the validators stash a computed checksum), `ValidationStatus` (REG_SZ canonical schema string), `Source` (REG_SZ canonical schema string). Idempotent via `RegDeleteTree` on entry. Wired in `registry_populate_defaults()` after `esrt_populate_registry()`. Smoke confirms `17/17 firmware table(s) mirrored under HARDWARE\Firmware\Tables` on QEMU OVMF.
- [/] **JSON writer at `X:\Diag\firmware-tables.json`** -- builder shipped (`src/kernel/firmware_tables_json.c`, ~420 LOC): manual JSON formatter (no kmalloc, no printf-family) emits `schema_version=1` + `generated_at_utc` (pinned to `g_boot_info.loader_identity.build_unix_time` = git HEAD commit time, offline-deterministic) + `firmware_platform` + full `conformance_profile` block + `tables[]` + `degraded[]` + `quirks_active[]` (populated by §9 from `firmware_quirks_iter_next`) + all 5 required typed sub-blocks (`acpi`, `smbios`, `mat`, `rt_properties`, `esrt`) + `apei` (BERT/HEST/EINJ/ERST) + `dbg2` + `wsmt` generic blocks via `firmware_table_lookup_name`. The `mat` block iterates `mat_get_entry()` summing `num_pages` for `MAT_CLASS_WX_VIOLATION` entries and emits `mat_overflowed()` literal so W^X violations and MAT overflow are reported truthfully. The `esrt` block is byte-for-byte aligned with the §6 Registry mirror: u64 `ResourceVersion`, brace-form GUID per-entry keys, all 8 fields including decoded `TypeName` + `LastAttemptStatusName`. RFC 8259 string escaping. 16 KiB `pmm_alloc_contiguous` buffer; truncation logs WARN but never panics. Wired from `boot_phase3()` after VFS+IXFS mount. **Disk-write currently fails** with `[WARN] FW: JSON: could not open X:\Diag\firmware-tables.json for write` -- same pre-existing Phase-3 VFS failure mode as `boot-profile.log` + `boot-reserved.json`; the writer joins that failure cluster until the shared VFS fix lands. **The serialization code is sound and produces a schema-conforming buffer; only the file-write step needs the VFS-layer fix.** Filed at §10.
- [x] **Validation failures, table addresses, checksums, and quirk matches**: Registry mirror covers Address+Size+ValidationStatus+Source; `computed_checksum` (u32) added to `struct firmware_table_entry` and populated by every validator (RSDP v1 / v2 ext_checksum / SDT / FPDT / SMBIOS3 / SMBIOS2 entry-point) so the Registry `Checksum` value and the JSON `checksum` field carry the firmware-published byte instead of a placeholder zero. Quirks_active is now populated by §9 (canonical lowercase quirk names after `boot.conf firmware_quirk_disable=` suppression; empty when no quirks fired).
- [ ] **`sysinfo.exe firmware` integration** (deferred -- requires new userland tool): `user/sysinfo*` does not exist yet. Owner moves to `01-boot-platform/TODO-04 §10` host-decoder follow-up which already plans a host tool to round-trip the schema; the user-mode `sysinfo.exe firmware` reuses that decoder so kernel writer + host decoder + sysinfo all share one schema. Note: a follow-up filed in §10 once the user-mode test framework + sysinfo skeleton lands.
- [ ] Commit: `"boot: publish firmware inventory report"` (deferred to JSON-writer follow-up; this commit ships schema + Registry mirror under a different commit message)

**Test checkpoint:** Post-boot, `X:\Diag\firmware-tables.json` exists, validates against the schema doc (`docs/boot/firmware-tables-schema.md`) AND against the host decoder (§10). `HKLM\HARDWARE\Firmware\Tables\<GUID>\Address` reads back the same physical address that `firmware_table_lookup_guid()` returns. APEI block shows BERT/HEST signatures when present (operator-visible persistent hardware-error records); DBG2 block lists secondary debug ports beyond SPCR; WSMT block reports the SMM mitigations bitmap so users see firmware SMM posture. `sysinfo.exe firmware` prints the same table set with checksums + quirk matches. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | smoke confirms `17/17 firmware table(s) mirrored under HARDWARE\Firmware\Tables` + JSON-writer WARN observed (VFS gap)
>
> **Notes:**
> - **What shipped** -- `docs/boot/firmware-tables-schema.md` (~250 LOC, schema_version=1 wire format) + `src/kernel/firmware_tables_registry.c` (~135 LOC Registry mirror) + `src/kernel/firmware_tables_json.c` (~420 LOC manual JSON formatter, 16 KiB pmm buffer, RFC 8259 escapes, civil-from-days ISO-8601 timestamp) + `computed_checksum` field on `struct firmware_table_entry` populated by every validator (RSDP v1 / v2 / SDT / FPDT / SMBIOS3 / SMBIOS2) BEFORE the sum-check so CHECKSUM_FAIL entries still carry the firmware byte.
> - **How it runs** -- Registry mirror fires from `registry_populate_defaults()` after `esrt_populate_registry()`; JSON writer fires from `boot_phase3()` BEFORE `boot_history_kernel_mark_phase3()` so file I/O happens on BSP main thread and any diagnostic logs land in the smoke-capture window. JSON writer emits all 5 required typed sub-blocks (`acpi`, `smbios`, `mat`, `rt_properties`, `esrt`) with full population: MAT block iterates `mat_get_entry()` summing `num_pages` for `MAT_CLASS_WX_VIOLATION` entries and emits `mat_overflowed()` literal so violations are reported truthfully; ESRT block walks every entry with brace-form GUID keys + 8 fields per entry including decoded `TypeName` + `LastAttemptStatusName`.
> - **Downstream effects** -- `HKLM\HARDWARE\Firmware\Tables\*` + `firmware-tables.json` (once the VFS-layer fix lands) become the canonical machine-readable firmware fingerprint for §10 host decoder + future `sysinfo.exe firmware` + Win32 `GetSystemFirmwareTable` consumers + diagnostics. Codex 11x review (design + adversarial-impl + adversarial x 2 + consistency + perf + re-adversarial x 4 to convergence) caught: H schema-omission of 5 required typed sub-blocks (fixed with full population from existing accessors); M validators stash computed_checksum AFTER sum-check so CHECKSUM_FAIL entries lose the firmware byte (fixed: stash BEFORE sum-check in all 6 validators); H MAT block hard-coded wx_violation_pages:0 / overflowed:false masking real violations (fixed with iteration); H accessor-success polarity inverted on mat_get_entry which returns 1 on success (fixed: `!= 0`); H tables[] missing per-entry `guid` field (fixed: emit canonical brace-form for nonzero GUID, null for ACPI signature-only entries); M acpi block missing `version` field (fixed: emit 2 / 1 / 0 from RSDP catalog presence); rejected (false positive, prompt declared a wrong canonical enum) checksum_status enum mismatch -- writer + Registry + schema doc all already agree on `validated/degraded/unknown_profile/untested`.
> - **Canonical doc** -- [`docs/boot/firmware-tables-schema.md`](../../docs/boot/firmware-tables-schema.md) wire-format spec + [`include/kernel/firmware_tables.h`](../../include/kernel/firmware_tables.h) Registry/JSON publication API contract.
> - **Scope boundary** -- §8 owns Registry mirror + JSON writer + schema doc + per-validator computed_checksum stash. §9 owns quirk-database population of `quirks_active[]` (now populated, see §9). §10 owns the host decoder, sysinfo userland tool, the kernel-wide JSON-builder truncation contract, and the Phase-3 VFS write failure cluster (this section's JSON writer joins that cluster until the shared fix lands).

> **Verified:** 2026-05-02 | this commit | 4/6 items + 2 deferred-to-owner | build OK | smoke PASS (KVM 2.32s) + Registry 17/17 mirrored
> **Accepted:** [H] Fixed 16 KiB JSON buffer can produce malformed truncated output at FIRMWARE_TABLE_MAX=64 + ESRT_MAX=16 (reason: scope -- the jb_putc permanent-stop pattern is repo-wide; right fix is shared json_builder library with reservation/fail-closed) -> XREF: 01-boot-platform/TODO-04 §11 (item: "Kernel-wide JSON builder truncation contract" at line 317)
> **Deferred:** [N/A] Disk write of `X:\Diag\firmware-tables.json` blocked by Phase-3 VFS write failure cluster -> XREF: 01-boot-platform/TODO-04 §11 (item: "Fix the Phase-3 `vfs_open(X:\Diag\..., VFS_O_WRITE)` failure cluster" at line 312)
> **Quality reviewed:** 2026-05-02 | Codex 11x (design + adversarial-impl + adversarial x2 + consistency + perf + re-adversarial x4) | 4H+2M fixed, 1H accepted-XREF, 1H rejected (false-positive prompt) | scope: kernel-code-quality

---

## 9. Firmware Quirk Database

- [x] **SMBIOS-keyed quirk descriptor table** (`src/kernel/firmware_quirks.c` ~190 LOC + `include/kernel/firmware_quirks.h` ~75 LOC). Static `s_quirks[]` array of `{bit, name, vendor_substr, product_substr, bios_substr}` records; data-driven match predicate (substring search against `smbios_get_info()` fields). 5 quirks defined; `_Static_assert` pins `FW_QUIRK_COUNT == sizeof(s_quirks)`.
- [x] **5 named quirks shipped**: `FW_QUIRK_BROKEN_FPDT` (0x01), `FW_QUIRK_BAD_MADT_CHECKSUM` (0x02), `FW_QUIRK_GOP_PITCH_LIES` (0x04), `FW_QUIRK_BOGUS_MAT` (0x08), `FW_QUIRK_USB_HANDOFF_BLACKLIST` (0x10). Synthetic test predicates use `TestVendor` SMBIOS strings; consumers wire workarounds on `firmware_quirks_is_active(FW_QUIRK_*)`.
- [x] **Single LOG_INFO summary + JSON `quirks_active[]` integration**: `firmware_quirks_init()` emits `[BOOT] FW: quirks: N active[: name1,name2]` once on the BSP at Phase 1 (between `uefi_conformance_init` and `firmware_tables_init` so the JSON catalog publishes the active set on the same boot). `src/kernel/firmware_tables_json.c` `quirks_active` array now iterates `firmware_quirks_iter_next()` emitting names in canonical bit order. BlackBox capture inherits the quirks line via the existing klog-to-`X:\Logs\` transcript path; no separate writer needed.
- [x] **boot.conf override** (`firmware_quirk_disable=broken_fpdt,bogus_mat`): bootloader parser at `src/boot/uefi/bootx64.c:2618` walks comma-separated tokens against a local `qmap[]` and writes the bitmask into `boot_config.firmware_quirk_disable` (new u8 in `_reserved` slot, mirrored in `boot_info_mirror.h` with `_Static_assert` ABI guard). Kernel `firmware_quirks_init()` ANDs `~mask` against the auto-detected set so the override suppresses individual quirks without disabling auto-detection wholesale.
- [x] Commit: `"boot: firmware quirk database"`

**Test checkpoint:** Synthetic SMBIOS vendor=`TestVendor` product=`BrokenFPDT` triggers the broken-FPDT quirk and `firmware-tables.json.quirks_active[]` contains `broken_fpdt`. `boot.conf` line `firmware_quirk_disable=broken_fpdt` overrides the trigger. BlackBox dump includes `quirks_active`. Test on: QEMU WHPX (custom SMBIOS), QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 9 firmware-quirk sub-tests (parse single/multi/unknown/null-empty/whitespace, canonical-name-each-bit, parse round-trip all bits, name-invalid, is_active polarity), 0 failures
>
> **Notes:**
> - **What shipped** -- `firmware_quirks.c/h` (~265 LOC total): static `s_quirks[]` descriptor table, 5 quirks (`broken_fpdt`/`bad_madt_checksum`/`gop_pitch_lies`/`bogus_mat`/`usb_handoff_blacklist`), substring-match predicate, BSP-only init, 6 public APIs (`init`, `active_mask`, `is_active`, `active_count`, `name`, `iter_next`, `parse_disable`), `_Static_assert(FW_QUIRK_COUNT == sizeof table)`.
> - **How it runs** -- `firmware_quirks_init()` runs once on the BSP between `uefi_conformance_init()` and `firmware_tables_init()` in Phase 1 (`boot_interrupts.c:265`); reads `smbios_get_info()`, walks descriptor table, ANDs `~boot_config.firmware_quirk_disable` against the auto-detected mask, emits `[BOOT] FW: quirks: N active[: ...]`. Read-only thereafter -- no locks. Idempotent (s_initialized guard).
> - **Downstream effects** -- `firmware_tables_json.c` `quirks_active[]` array now publishes the active set; BlackBox transcript inherits the LOG_INFO line via existing klog-to-X-drive path. Codex 11x review converged through fix loop: test-coverage caught duplicate {bit,name} table in bootx64.c qmap[] vs kernel s_quirks[] -> fixed by extracting `include/kernel/firmware_quirks_table.inc` shared X-macro consumed by both; re-adversarial caught missing Makefile dep on the .inc file -> added in BOTH src/boot/uefi/Makefile bootx64.o rule AND root Makefile $(UEFI_EFI) prereq list; consistency caught stale §8 quirks_active prose + schema doc empty-only contract -> updated. Adoption details in commit message.
> - **Canonical doc** -- [`include/kernel/firmware_quirks.h`](../../include/kernel/firmware_quirks.h) quirk database API contract.
> - **Scope boundary** -- §9 owns quirk detection + override + JSON publication. Per-quirk workaround consumers (FPDT skip, MADT-checksum bypass, GOP stride probe, MAT degrade, USB handoff blacklist) gate their own logic on `firmware_quirks_is_active(FW_QUIRK_*)` and live in their owning subsystems (TODO-22 timing, TODO-25 ACPI MADT validator, TODO-15 framebuffer, this file's §5 MAT inventory, TODO-16 USB handoff).

> **Verified:** 2026-05-02 | this commit | 5/5 items | build OK | smoke PASS (KVM 2.39s) + `[OK] FW: quirks: 0 active` observed
> **Accepted:** [M] Bootloader `firmware_quirk_disable=` tokenizer is duplicated from `firmware_quirks_parse_disable()` (reason: shared static-inline tokenizer needs new .inc plumbing across kernel/bootloader idiom boundary; X-macro already covers `{name, bit}` drift) -> XREF: 01-boot-platform/TODO-04 §11 (item: "Consolidate `firmware_quirk_disable=` tokenizer between kernel and bootloader" at line 316)
> **Quality reviewed:** 2026-05-02 | Codex 12x (design + adversarial + consistency x3 + perf x3 + re-adversarial x2 + test-coverage x3) | 4H+3M fixed, 1M accepted-XREF | scope: kernel-code-quality

---

## 10. Firmware Inventory Tests and Host Decoder

- [ ] Unit tests for GUID lookup, range rejection, checksum failures, and conformance levels (each known profile GUID + length/count rejection cases per §7).
- [ ] **Positive-case fixture blobs in `src/kernel/test/fixtures/firmware/`**: ACPI RSDP (v1 + v2), SMBIOS3, SMBIOS2, ESRT (8-field full entry), FPDT, DTB, **MAT (positive: code/data/RODATA/guard/WX classes)**, **RT Properties (positive: full 14-service supported bitmask)**, **conformance-profile table (one fixture per registered GUID -- UEFI 2.10, EBBR v2.0, +reserved SBBR/BBR rows from §7)**, **APEI signatures (BERT, HEST, EINJ, ERST)**, **DBG2**, **WSMT**.
- [ ] Add host tool mode to decode `firmware-tables.json`. Host decoder MUST round-trip every §8 schema block (catalog + validation + ESRT + MAT + RT Props + conformance + APEI + DBG2 + WSMT) -- not just table headers. Round-trip means: read JSON -> render human-readable inventory -> re-emit canonical JSON; `cmp` between input and re-emitted output must be byte-identical for any well-formed file.
- [ ] Synthetic-firmware fixtures for malformed MAT and RT properties: zero-entry MAT, oversized count, undersized descriptor_size, unaligned descriptor_size, descriptor with wrapping `number_of_pages`, descriptor with wrapping `physical_start + bytes`, RT properties with declared length below spec minimum, RT services header_size below struct size. Each must prove the parser rejects without dereferencing out-of-extent fields. Owner of these fixtures lives in `src/kernel/test/fixtures/firmware/`.
- [ ] Verify on QEMU OVMF, VirtualBox EFI, and at least two bare-metal machines.
- [ ] Commit: `"test: firmware table inventory coverage"`

**Test checkpoint:** All 4 unit tests in `## Unit Tests` PASS under `SUITE=boot`. Host decoder tool reads a captured `firmware-tables.json` and prints a human-readable table inventory. Fixture blobs in `src/kernel/test/fixtures/firmware/` cover RSDP / SMBIOS3 / ESRT / FPDT / DTB shapes. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 11. Firmware Inventory Hardening Debt

Pre-existing bugs and cross-section parity debt surfaced by §1-§9 review pipelines. Each item names a concrete root-cause fix; none are speculative refactors.

- [ ] **Fix the Phase-3 `vfs_open(X:\Diag\..., VFS_O_WRITE)` failure cluster** (surfaced 2026-05-02 by §8 JSON writer; pre-existing): `boot-profile.log`, `X:\Diag\boot-reserved.json`, and `X:\Diag\firmware-tables.json` all emit `[WARN] cannot open for write` / `vfs_open returned NULL` at Phase 3. **Failing pattern**: open dir READ, call `dir->ops->create`, close, then `vfs_open(file, VFS_O_WRITE)` as a separate call. **Working pattern (smoke evidence)**: `klog_disk` writes `X:\Logs\Serial_*.log` via single-open `vfs_open(path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC)` (`src/kernel/klog_disk.c:357`); smoke log shows `[OK] klog: writing to X:\Logs\Serial_26050201.log` with no WARN. **Same-pattern Phase-0/1 path** at `src/kernel/main/boot_version.c:590-596` (boot_version_blackbox_transcribe) uses dir-then-create-then-WRITE *and* an explicit vfs_truncate workaround (it documents `VFS_O_TRUNC` is ignored by `vfs_open` today; cited TODO is `05-storage-filesystems/TODO-04 VFS_O_TRUNC end-to-end`); only fires on a fault-recovery boot, so smoke does not directly exercise it. Likely root cause: VFS handle-cleanup state between dir-then-create and the separate file-WRITE open, and/or the same `VFS_O_TRUNC`-ignored gap. Audit the three failing Phase-3 call sites; write a synthetic test that round-trips a Phase-3 file create+write+read; consolidate every `X:\` writer onto the single-open `VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC` shape used by `klog_disk`. Once fixed, the §8 JSON publish, `boot-profile.log`, and `X:\Diag\boot-reserved.json` all start writing on the next boot with no per-call-site code change. Scope boundary: this is the dir-then-create-then-WRITE cluster only; klog `X:\Logs\` already works, and Phase-0/1 fault-only paths share the same root cause and should be retrofitted in the same fix.
- [ ] **Tighten `firmware_table_mmap_contains` to reject `UEFI_MMAP_BOOT_SERVICES_CODE/DATA`** (Codex §7 H1, surfaced 2026-05-02): PMM reclaims those types in Phase 0 before `firmware_tables_init` runs in Phase 1, so any firmware table residing in Boot Services memory can be overwritten by the kernel allocator before §1/§2/§3/§5/§7 walks it. The current oracle treats Boot Services as still-firmware-owned and silently dereferences kernel-allocated bytes. Change `fw_mmap_contains()` in `src/kernel/firmware_tables.c` to remove `UEFI_MMAP_BOOT_SERVICES_CODE` and `UEFI_MMAP_BOOT_SERVICES_DATA` from the allow list. Audit fallout in §1 catalog walk + §2 validator + §3 DTB header probe + §5 MAT inventory + §7 ECPT detection. Add a synthetic fixture under `src/kernel/test/fixtures/firmware/` simulating ECPT in Boot Services memory and assert detection treats it as absent rather than dereferencing post-PMM-reclaim bytes. Real-firmware impact: low (modern firmware places ACPI/SMBIOS in RuntimeServices/ACPI_RECLAIM); test fixtures + bare-metal sweep still required.
- [ ] **Validate `s_rt->hdr.header_size` per function-pointer offset** before reading it in `call_set_virtual_address_map()` and the critical-pointer NULL checks in `uefi_runtime_init()` (`src/kernel/uefi_runtime.c`). The §5 mismatch checker already gates on `header_size >= sizeof(struct efi_runtime_services)` for the 14 named slots; this item extends the same per-offset coverage check to the earlier SVAM call and the 5-pointer init validation so a truncated RT header cannot overread before the mismatch guard runs. Reject with WARN + degrade RT services on undersized header.
- [ ] **Consolidate SMBIOS entry-point wire-format constants** into a shared internal header (e.g. `src/kernel/smbios_wire.h`) so `smbios.c` (`SMBIOS3_EP_LEN` / `SMBIOS2_EP_LEN_MIN` / `SMBIOS2_EP_LEN_MAX`) and `firmware_tables.c` (`FW_SMBIOS3_EP_LEN` / `FW_SMBIOS2_EP_LEN_MIN` / `FW_SMBIOS2_EP_LEN_MAX`) reference one definition; either include the same header or add `_Static_assert(FW_* == SMBIOS_*)` linking the two if a shared header is too disruptive. Drift risk is low (DMTF DSP0134 entry-point sizes have not changed since SMBIOS 2.4) but the duplicate is still a future-spec hazard.
- [ ] **Consolidate `firmware_quirk_disable=` tokenizer between kernel and bootloader** (Codex §9 test-coverage M, accept-XREF 2026-05-02): the shared X-macro at `include/kernel/firmware_quirks_table.inc` enforces `{name, bit}` parity, but the comma-separated-list tokenizer is duplicated -- `firmware_quirks_parse_disable()` in `src/kernel/firmware_quirks.c` and the inline parser in `src/boot/uefi/bootx64.c parse_conf_kv()` both walk tokens against the X-macro names, but their tokenization rules (whitespace handling, oversize-token truncation, separator semantics) are implemented twice. A regression in the bootloader half would leave `g_boot_info.config.firmware_quirk_disable` at 0 while the 9 kernel helper tests still pass. The Codex-recommended boot-time integration test (`firmware_quirk_disable=broken_fpdt` + synthetic SMBIOS + assert active mask) is forbidden by the Test Code Policy (tests must not call `firmware_quirks_init()`). Solution: extract the tokenizer into a shared static-inline helper in a new `include/kernel/firmware_quirks_parse.inc` (or expand the X-macro to also generate a tokenizer-table-driven parser); both kernel and bootloader include it so the parsing logic is one source of truth. Verify via the existing 9 kernel parse_disable tests (which then cover the bootloader-side path by construction).
- [ ] **Kernel-wide JSON builder truncation contract** (Codex §8 round-2 M, accept-XREF 2026-05-02): every kernel JSON publisher uses the `jb_putc` / `jb_puts` permanent-stop pattern (`firmware_tables_json.c`, `boot_progress.c` boot-timeline writer, future BlackBox JSON writers). Once `pos >= cap-1` the builder drops every subsequent byte including the closing `}` braces, leaving a non-parseable JSON prefix on truncation. Today the 16 KiB cap is sized for ~3x worst-case OVMF (measured ~7.1 KiB on smoke) so truncation is not a live regression, but a future firmware with hundreds of ESRT entries or richer typed sub-blocks will hit it. Add a shared `jb_*` builder helper that either (a) reserves a fixed tail budget for mandatory closing syntax, or (b) fails closed by skipping the file write entirely on truncation; retrofit every kernel JSON consumer to use it. Owner location: kernel-side json builder library (e.g. `src/kernel/util/json_builder.c`). Add a regression test that forces a tiny buffer cap and asserts the writer either produces parseable JSON or suppresses the write.
- [ ] Commit: `"boot: firmware inventory hardening debt"`

**Test checkpoint:** Phase-3 VFS write to `X:\Diag\firmware-tables.json` succeeds on next boot (smoke confirms file appears with non-zero size, no `[WARN] FW: JSON: could not open` line). `boot_reserved_blackbox_dump` and `boot-profile.log` write paths emit `[OK]` markers. `firmware_table_mmap_contains` rejects synthetic Boot-Services fixture. Quirk-tokenizer round-trip parses both kernel and bootloader paths from one source. JSON builder regression test produces parseable output or suppresses on tiny-cap force. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## OS Comparison

| ⭐ | Feature                               | 🪟 Win11                         | 🐧 Linux                         | 🚀 Impossible OS                  |
| -- | ------------------------------------- | --------------------------------- | -------------------------------- | ---------------------------------- |
| 💎 | Firmware table catalog API            | ✅ ACPI/HAL + WMI                | ✅ acpi_get_table + sysfs        | ✅ firmware_table_entry_t catalog |
| 💎 | Physical range + checksum validation  | ✅ HAL validates pre-use         | ✅ acpi_tb_verify_checksum       | ✅ §2 mmap-bound + checksums      |
| 💎 | ACPI/SMBIOS/DTB arbitration           | ⚠️ ACPI-first; no DTB            | ✅ ACPI on x86, DTB on ARM       | ✅ §3 ACPI > DTB + HYBRID detect  |
| 💎 | FPDT boot-timeline normalization      | ✅ FPDT + ETW timeline           | ⚠️ acpi_fpdt_init read-only      | ✅ §4 FPDT + TSC unified JSON     |
| 💎 | UEFI MAT + Runtime Properties         | ✅ MmGetEfiRuntimeServicesTable  | ✅ efi_memmap_attributes         | ✅ §5 MAT inventory + RT mismatch |
| 💎 | ESRT firmware inventory               | ✅ Windows Update / fwupdd       | ✅ fwupd /sys/firmware/efi/esrt  | ✅ §6 HKLM ESRT mirror + decoders |
| 💎 | UEFI conformance profile + EBBR       | ⚠️ assumes full PC profile       | ✅ EBBR detection in efi-stub    | ✅ §7 ECPT + EBBR + PC contradict |
| 💎 | Registry + BlackBox firmware report   | ✅ msinfo32 + Event Log          | ⚠️ scattered (dmidecode/sysfs)   | ✅ §8 HKLM mirror + JSON writer   |
| ⭐ | Firmware quirk database               | ⚠️ HAL-internal, opaque          | ⚠️ DMI quirks scattered          | ✅ §9 SMBIOS-keyed + JSON publish |
| ⭐ | Host decoder for firmware-tables.json | ❌ N/A                           | ❌ N/A                           | ⬜ §10 host tool decode JSON      |
| 💎 | APEI (BERT/HEST/EINJ/ERST) visibility | ✅ WHEA hardware-error subsystem | ✅ /sys/firmware/acpi/* + ras-mc | ✅ §8 apei block in JSON writer   |
| ⭐ | DBG2 secondary debug ports            | ✅ kernel debugger reads DBG2    | ✅ amba_pl011 + earlycon DBG2    | ✅ §8 dbg2 block in JSON writer   |
| ⭐ | WSMT SMM mitigations posture          | ✅ HAL reads WSMT bitmap         | ❌ Linux ignores WSMT            | ✅ §8 wsmt block in JSON writer   |
| 💎 | firmware-tables.json schema doc       | ⚠️ msinfo32 schema undocumented  | ⚠️ tools differ per distro       | ✅ §8 firmware-tables-schema.md   |

---

## Unit Tests

- [x] `test_firmware_table_guid_lookup` -- §1: ACPI 2.0/1.0 cfg-table entry; non-zero phys_addr; unknown GUID + NULL guid both return NULL.
- [x] `test_firmware_table_range_rejects_unmapped` -- §2: phys_addr in CONVENTIONAL mmap downgrades to RANGE_UNMAPPED; clean firmware leaves no validator degradations.
- [x] `test_firmware_table_checksum_fail` -- §2: corrupted SDT byte downgrades to CHECKSUM_FAIL; size-below-header and header.length-mismatch downgrade to LENGTH_BAD; one-way downgrade preserved.
- [ ] `test_firmware_conformance_profiles` -- owned by §7 (conformance integration).

---

## Verification

- [ ] `bash scripts/build.sh` clean build with TODO-04 sources -> `=== BUILD OK ===`.
- [ ] `bash scripts/test.sh SUITE=boot` -> all 4 `test_firmware_*` cases PASS (per the Unit Tests section).
- [ ] QEMU OVMF (ACPI 2.0 + SMBIOS3 + FPDT present): boot log shows `[BOOT] firmware tables: <N> cataloged, <M> validated, <K> degraded`. `X:\Diag\firmware-tables.json` exists with at least RSDP, SMBIOS3, FPDT entries.
- [ ] VirtualBox EFI (ACPI + SMBIOS, no FPDT/ESRT): boot succeeds; firmware report flags FPDT + ESRT as absent (not degraded).
- [ ] Bare-metal laptop + desktop: every active firmware quirk is logged + included in the BlackBox report; no silent table-validation failures on serial.
- [ ] Commit: `"docs/firmware: TODO-04 verification complete"`

**Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 4 firmware-inventory sub-tests, 0 failures (target on full implementation)
