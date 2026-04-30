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
- -> XREF: `TODO-11-interrupt-timer-arch.md §6` -- ACPI MADT remains the interrupt owner
- -> XREF: `TODO-27-uefi-advanced.md §2,§6` -- ESRT/capsule and extended SMBIOS consumers
- -> XREF: `02-kernel-core/TODO-14-registry-completion.md §5` -- `HKLM\HARDWARE\Firmware\*` storage via registry syscalls (consumer for §8)

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
| 💎 |   6   | ESRT firmware inventory mirror                          | §1, TODO-27 §2 |  [ ]   |
| 💎 |   7   | UEFI conformance profile and EBBR detection             | §1             |  [ ]   |
| 💎 |   8   | Registry and BlackBox firmware report                   | §1-§7          |  [ ]   |
| ⭐ |   9   | Firmware quirk database                                 | §8             |  [ ]   |
| ⭐ |  10   | Firmware inventory tests and host decoder               | §1-§9          |  [ ]   |

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
> - Downstream effects: this catalog is the input for the §2 range/checksum validator and the §8 Registry/BlackBox firmware report; Codex 3-round adversarial review adoptions in commit `<hash>`.
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
> **Accepted:** [L] SMBIOS3/2 entry-point wire-format length constants duplicated between `smbios.c` (`SMBIOS3_EP_LEN` / `SMBIOS2_EP_LEN_*`) and `firmware_tables.c` (`FW_SMBIOS3_EP_LEN` / `FW_SMBIOS2_EP_LEN_*`); future-spec drift hazard (reason: scope -- consolidating into a shared internal header touches `src/kernel/smbios.c` private types) -> XREF: 01-boot-platform/TODO-04 §10 (item: "Consolidate SMBIOS entry-point wire-format constants into a shared internal header" at line 188)
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
> - Downstream effects: `HKLM\SYSTEM\Boot\Firmware` is the canonical platform fingerprint for sysinfo / diagnostic tools; §6 ESRT mirror, §7 conformance flagging, and §8 firmware-tables.json all read `firmware_platform_get()` to gate their PC-class vs EBBR behavior. Codex 1x adversarial review adoptions in commit `<hash>`.
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

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 10 MAT classify+inventory sub-tests + RT property mismatch sub-tests, 0 failures (GUARD/CODE/DATA/RODATA/WX classify, NULL+range guards, count <= cap, mismatch counter range, supported-implies-callable invariant)
>
> **Notes:**
> - Shipped: `mat_class_t` + `mat_entry_t` + 7 public accessors in `include/kernel/uefi_config.h`; ~140 LOC delta in `src/kernel/uefi_config.c` (cache, classify helper, per-entry logger, overflow warn); `int uefi_rt_property_mismatches(void)` in `include/kernel/uefi_runtime.h`; ~70 LOC `rt_check_property_pointer_mismatch()` in `src/kernel/uefi_runtime.c` covering 14 RT services with bidirectional mismatch detection + advertised-but-NULL-bit-clearing; 10 unit tests in `src/kernel/test/test_uefi_boot.c`.
> - How it runs: `mat_init()` and `uefi_runtime_init()` already run once on the BSP at Phase 1 (mat_init from `boot_interrupts.c`, runtime_init from boot path); both stay read-only afterwards so consumer queries are lock-free SMP-safe. KVM smoke 2.32s on OVMF: 27 MAT descriptors logged (4 code + 12 data + 1 WX violation pre-existing in OVMF + 10 guard).
> - Downstream effects: TODO-27 §3 W^X enforcement now has a concrete iteration API (no need to re-walk firmware table); §6 ESRT mirror, §7 conformance flagging, and §8 firmware-tables.json read the catalog without touching firmware again. Codex 1x adversarial review adoptions in commit `<hash>` -- H1 advertised-but-NULL pointer cleared from supported bitmask so wrappers cannot null-deref.
> - Canonical doc: `include/kernel/uefi_config.h` MAT inventory contract + `include/kernel/uefi_runtime.h` RT property mismatch contract.
> - Scope boundary: §5 detects + classifies + logs MAT entries and RT property mismatches; the actual W^X *enforcement* (vmm_set_nx / vmm_set_ro per region) is owned by TODO-27 §3. §6 ESRT firmware inventory is owned by §6. RT services per-offset header_size validation in pre-existing `call_set_virtual_address_map()` + 5-pointer init path is tracked in §10 (item: "Validate s_rt->hdr.header_size covers each function-pointer offset" at line 232).

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

> [!NOTE]
> **Partial foundation shipped:** EFI_CONFORMANCE_PROFILES_TABLE parsing already exists in `uefi_config.c` (the `s_conformance_level` / `UEFI_CONFORM_FULL` / `UEFI_CONFORM_EBBR` path at uefi_config.c:107-140). What remains is (a) feeding the result through the §1 catalog, (b) relaxing/tightening the required-table set, and (c) detecting EBBR-claim + PC-hardware contradictions.

- [ ] Wire existing `s_conformance_level` (uefi_config.c) into the §1 firmware table catalog as a query helper rather than a static.
- [ ] Detect UEFI full profile, EBBR, and unknown profile GUIDs (existing) -- promote unknown-GUID logging from DEBUG to a `firmware_table_entry_t.status = unknown_profile` flag.
- [ ] Relax or tighten required table set based on profile (e.g. EBBR allows missing FPDT/MAT; full UEFI requires them).
- [ ] Add diagnostics when hardware claims EBBR but exposes PC-only assumptions (PIC, i8042, RTC port 0x70).
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
- [ ] Consolidate SMBIOS entry-point wire-format constants into a shared internal header (e.g. `src/kernel/smbios_wire.h`) so `smbios.c` (`SMBIOS3_EP_LEN` / `SMBIOS2_EP_LEN_MIN` / `SMBIOS2_EP_LEN_MAX`) and `firmware_tables.c` (`FW_SMBIOS3_EP_LEN` / `FW_SMBIOS2_EP_LEN_MIN` / `FW_SMBIOS2_EP_LEN_MAX`) reference one definition; either include the same header or add `_Static_assert(FW_* == SMBIOS_*)` linking the two if a shared header is too disruptive. Drift risk is low (DMTF DSP0134 entry-point sizes have not changed since SMBIOS 2.4) but the duplicate is still a future-spec hazard.
- [ ] Validate `s_rt->hdr.header_size` covers each function-pointer offset before reading it in `call_set_virtual_address_map()` and the critical-pointer NULL checks in `uefi_runtime_init()` (`src/kernel/uefi_runtime.c`). The §5 mismatch checker already gates on `header_size >= sizeof(struct efi_runtime_services)` for the 14 named slots; this item extends the same per-offset coverage check to the earlier SVAM call and the 5-pointer init validation so a truncated RT header cannot overread before the mismatch guard runs. Reject with WARN + degrade RT services on undersized header.
- [ ] Synthetic-firmware fixtures for malformed MAT and RT properties: zero-entry MAT, oversized count, undersized descriptor_size, unaligned descriptor_size, descriptor with wrapping `number_of_pages`, descriptor with wrapping `physical_start + bytes`, RT properties with declared length below spec minimum, RT services header_size below struct size. Each must prove the parser rejects without dereferencing out-of-extent fields. Owner of these fixtures lives in `src/kernel/test/fixtures/firmware/` per §10 fixture inventory.
- [ ] Verify on QEMU OVMF, VirtualBox EFI, and at least two bare-metal machines.
- [ ] Commit: `"test: firmware table inventory coverage"`

**Test checkpoint:** All 4 unit tests in `## Unit Tests` PASS under `SUITE=boot`. Host decoder tool reads a captured `firmware-tables.json` and prints a human-readable table inventory. Fixture blobs in `src/kernel/test/fixtures/firmware/` cover RSDP / SMBIOS3 / ESRT / FPDT / DTB shapes. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## OS Comparison

| ⭐ | Feature                               | 🪟 Win11                         | 🐧 Linux                        | 🚀 Impossible OS                |
| -- | ------------------------------------- | --------------------------------- | ------------------------------- | -------------------------------- |
| 💎 | Firmware table catalog API            | ✅ ACPI/HAL + WMI                | ✅ acpi_get_table + sysfs       | ✅ firmware_table_entry_t catalog |
| 💎 | Physical range + checksum validation  | ✅ HAL validates pre-use         | ✅ acpi_tb_verify_checksum      | ✅ §2 mmap-bound + checksums    |
| 💎 | ACPI/SMBIOS/DTB arbitration           | ⚠️ ACPI-first; no DTB            | ✅ ACPI on x86, DTB on ARM      | ✅ §3 ACPI > DTB + HYBRID detect |
| 💎 | FPDT boot-timeline normalization      | ✅ FPDT + ETW timeline           | ⚠️ acpi_fpdt_init read-only     | ✅ §4 FPDT + TSC unified JSON   |
| 💎 | UEFI MAT + Runtime Properties         | ✅ MmGetEfiRuntimeServicesTable  | ✅ efi_memmap_attributes        | ✅ §5 MAT inventory + RT mismatch |
| 💎 | ESRT firmware inventory               | ✅ Windows Update / fwupdd       | ✅ fwupd /sys/firmware/efi/esrt | ⬜ §6 ESRT mirror to Registry   |
| 💎 | UEFI conformance profile + EBBR       | ⚠️ assumes full PC profile       | ✅ EBBR detection in efi-stub   | ⬜ §7 conformance GUID parse    |
| 💎 | Registry + BlackBox firmware report   | ✅ msinfo32 + Event Log          | ⚠️ scattered (dmidecode/sysfs)  | ⬜ §8 HKLM\HARDWARE\Firmware    |
| ⭐ | Firmware quirk database               | ⚠️ HAL-internal, opaque          | ⚠️ DMI quirks scattered         | ⬜ §9 SMBIOS-keyed quirks       |
| ⭐ | Host decoder for firmware-tables.json | ❌ N/A                           | ❌ N/A                          | ⬜ §10 host tool decode JSON    |

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

