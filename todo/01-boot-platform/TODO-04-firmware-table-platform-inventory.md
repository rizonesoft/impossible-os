---
schema_version: 1
id: firmware-table-platform-inventory
domain: 01-boot-platform
status: active
title: "TODO-04 -- Firmware Table & Platform Inventory"
---

# TODO-04 -- Firmware Table & Platform Inventory

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

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
- -> XREF: `16-architecture-ports/TODO-01-multi-arch-port.md` -- IORT (ARM IO Remapping Table) catalog work belongs to the ARM SBSA port; deferred from §1 catalog source enum until that domain activates

## Outcome

- Every firmware table copied or referenced by the bootloader is range-checked, cataloged, and visible in diagnostics.
- ACPI, SMBIOS, DTB, FPDT, ESRT, memory attributes, runtime properties, and conformance profiles have one inventory API.
- Registry and BlackBox capture enough firmware metadata for support, crash triage, and hardware certification.
- Embedded/EBBR-style DTB systems have a defined path without pretending to be PC ACPI systems.

## Implementation Order

| ⭐  | Order | Deliverable                                             | Depends On     | Status |
| --- | :---: | ------------------------------------------------------- | -------------- | :----: |
| 💎  |   1   | Firmware table catalog API                              | TODO-01 §1     |  [x]   |
| 💎  |   2   | Physical range and checksum validation                  | §1             |  [x]   |
| 💎  |   3   | ACPI/SMBIOS/DTB table arbitration                       | §1, §2         |  [x]   |
| 💎  |   4   | FPDT and boot timing normalization                      | §1             |  [x]   |
| 💎  |   5   | UEFI memory attributes and runtime properties inventory | §1, TODO-27 §3 |  [x]   |
| 💎  |   6   | ESRT firmware inventory mirror                          | §1, TODO-27 §2 |  [x]   |
| 💎  |   7   | UEFI conformance profile and EBBR detection             | §1             |  [x]   |
| 💎  |   8   | Registry and BlackBox firmware report                   | §1-§7          |  [x]   |
| ⭐  |   9   | Firmware quirk database                                 | §8             |  [x]   |
| ⭐  |  10   | Firmware inventory tests and host decoder               | §1-§9          |  [x]   |
| ⭐  |  11   | Firmware inventory bug-fix debt                         | §1-§9          |  [x]   |
| ⭐  |  12   | Firmware inventory refactor debt                        | §1-§9          |  [x]   |

---

## 1. Firmware Table Catalog API

> [!NOTE]
> **Foundation already shipped:** `uefi_find_config_table()` (uefi_config.c:47) walks `boot_info.config_table[]` by GUID. `acpi_enumerate_signatures()` + `acpi_get_raw_table()` (acpi.c:972, 1041) and `smbios_get_raw_table()` (smbios.c:679) provide validated raw-byte access. SMBIOS3 + SMBIOS2 entry-point parsing exists at smbios.c:381,429. This section adds the higher-level `firmware_table_entry_t` catalog that aggregates these per-provider helpers behind one inventory API; do NOT re-implement the per-provider validation.

- [x] Define `firmware_table_entry_t` (GUID/name, phys_addr, size, source enum, status, owner) with `FIRMWARE_TABLE_MAX = 64` cap.
- [x] Implement `firmware_tables_init()` populating the catalog from `boot_info.config_table[]` + per-provider accessors (ACPI via `acpi_for_each_record()`, SMBIOS via `smbios_get_raw_table()`). Wired after `uefi_conformance_init()`.
- [x] Add lookup API: `firmware_table_lookup_{guid,name,owner}` + `firmware_table_count` / `firmware_table_get` with NULL/zero-GUID guards.
- [x] Emit boot log summary `[BOOT] firmware tables: <N> cataloged, <M> validated, <K> degraded`. OVMF: 17/9/0.
- [x] Commit: `"boot: firmware table catalog API"`

**Test checkpoint:** Boot log shows `[BOOT] firmware tables: <N> cataloged` line; `firmware_table_lookup_guid(EFI_ACPI_20_TABLE_GUID)` returns a valid entry on QEMU OVMF + VirtualBox EFI; lookup of an unknown GUID returns NULL. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 12 firmware-table sub-tests / 23 assertions, 0 failures (1 SKIP on QEMU OVMF: <2 SSDTs)
>
> **Notes:**
> - **What shipped** -- `include/kernel/firmware_tables.h` + `src/kernel/firmware_tables.c` catalog + lookups; `acpi_for_each_record()` accessor for duplicate-preserving SDT enumeration.
> - **How it runs** -- `firmware_tables_init()` runs BSP-only at Phase 1; read-only thereafter, lookups lock-free.
> - **Downstream effects** -- input for §2 range/checksum validator and §8 Registry/BlackBox report.
> - **Canonical doc** -- [`include/kernel/firmware_tables.h`](../../include/kernel/firmware_tables.h).
> - **Scope boundary** -- §1 records per-provider validation only; range/checksum re-verify owned by §2; ESRT/MAT/conformance details by §5-§7.
>
> **Verified:** 2026-04-30 | commit `57d1aa79` | 5/5 items | build OK | smoke PASS (QEMU OVMF 2.30s)
> **Accepted:** [H] ACPI catalog walk dereferences firmware child pointers before range validation -> XREF: 01-boot-platform/TODO-04 §2 (item: "Validate every table pointer against the UEFI memory map before dereference" at line 69). Reason: pre-existing dereference surface from acpi_init / acpi_enumerate_signatures (commit 194e6012); §1 routes through that path without new pointer chases.
> **Quality reviewed:** 2026-04-30 | Codex 9x (design + adversarial + adversarial-impl + re-adversarial x4 + consistency + perf) | 6H+1M+0L fixed, 1H accepted-XREF | scope: kernel-code-quality

---

## 2. Physical Range and Checksum Validation

- [x] Validate every table pointer against the UEFI memory map before dereference (`fw_mmap_contains()`; firmware-bearing types only; addr+len overflow guard).
- [x] Verify ACPI RSDP/XSDT checksums, SMBIOS EP checksums, FPDT lengths, and ESRT bounds via per-source `fw_validate_*` helpers with overflow guards.
- [x] Mark bad tables `FW_STATUS_DEGRADED` + `degraded_reason` (one-way downgrade); entry remains in catalog so consumers can inspect.
- [x] Public `firmware_table_validate_all()` + `KERNEL_TESTS`-gated `firmware_table_validate_one_for_test()` with `bypass_range_check` flag.
- [x] Commit: `"boot: validate firmware table ranges and checksums"`

**Test checkpoint:** Synthetic test corrupts an ACPI RSDP checksum byte; `firmware_table_validate_all()` reports the table as `status: degraded` with reason `checksum_fail`; clean OVMF boot reports zero degraded tables. Pointer outside the UEFI memory map -> `status: degraded` with reason `range_unmapped`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 11 validator sub-tests / 19 assertions, 0 failures (TCG: clean SDT VALIDATED, checksum corruption -> CHECKSUM_FAIL, size-below-header / size mismatch / oversized SDT catalog size / oversized FPDT declared length -> LENGTH_BAD, CONVENTIONAL phys_addr -> RANGE_UNMAPPED, NULL phys_addr -> NULL_POINTER, one-way downgrade preserved, UNKNOWN_PROFILE promoted on full pass, validate_all clean firmware no validator degradations)
>
> **Notes:**
> - **What shipped** -- `firmware_table_validate_all()` + per-source helpers; wire-format struct views pinned via `_Static_assert`; 8 new test functions.
> - **How it runs** -- runs after the catalog walk; counts in `[BOOT] firmware tables:` reflect validator findings; idempotent.
> - **Downstream effects** -- §8 Registry/BlackBox consume `entry->status`; §6 ESRT mirror trusts the validated set.
> - **Canonical doc** -- [`include/kernel/firmware_tables.h`](../../include/kernel/firmware_tables.h) `firmware_table_validate_all()` contract.
> - **Scope boundary** -- §2 marks status only; full DTB header parsing in §3; ESRT mirror in §6; Registry/JSON in §8; SMBIOS constant consolidation closed by §12.

> **Verified:** 2026-04-30 | commit `789b572f` | 5/5 items | build OK | smoke PASS (KVM 2.35s, "17 cataloged, 9 validated, 0 degraded") | tests 710/710 PASS
> **Accepted:** [L] SMBIOS3/2 entry-point wire-format length constants duplicated between `smbios.c` (`SMBIOS3_EP_LEN` / `SMBIOS2_EP_LEN_*`) and `firmware_tables.c` (`FW_SMBIOS3_EP_LEN` / `FW_SMBIOS2_EP_LEN_*`); future-spec drift hazard (reason: scope -- consolidating into a shared internal header touches `src/kernel/smbios.c` private types) -> XREF: 01-boot-platform/TODO-04 §12 (item: "Consolidate SMBIOS entry-point wire-format constants" at line 109)
> **Quality reviewed:** 2026-04-30 | Codex 5x (design + adversarial + re-adversarial x2 + consistency + perf) | 2M+2L fixed, 1L accepted-XREF | scope: kernel-code-quality

---

## 3. ACPI/SMBIOS/DTB Table Arbitration

- [x] Define priority -- `firmware_platform_init()` picks ACPI > DTB; HYBRID only when both validated; ACPI 2.0 trumps ACPI 1.0; SMBIOS informational only.
- [x] DTB handoff validation + `/chosen`/memory/cpu discovery -- `src/kernel/dtb.c` validates FDT v17 header (magic, totalsize cap, version) + spans against firmware mmap; HasDTB gated on `memory_count > 0 && cpu_count > 0`.
- [x] Document PC-class doctrine: interrupt/timer ownership belongs to ACPI when present; HYBRID is diagnostic only.
- [x] Registry mirror at `HKLM\SYSTEM\Boot\Firmware\*` -- `FirmwarePlatform` REG_SZ + `HasACPI` / `HasDTB` / `HasSMBIOS` / `AcpiVersion` / `DtbTotalSize` REG_DWORD.
- [x] Commit: `"boot: arbitrate ACPI SMBIOS DTB firmware sources"`

**Test checkpoint:** PC-class boot logs `[BOOT] firmware platform: ACPI` and registry has `FirmwarePlatform=ACPI`. Synthetic DTB-only boot logs `FirmwarePlatform=DTB` with `/chosen` parsed. Hybrid (ACPI + DTB present) logs the priority decision. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 19 sub-tests / 38 assertions, 0 failures (DTB: phys_addr=0 invalid, bad magic invalid, minimal blob valid + 4-node-count assertions, oversized totalsize / unsupported version / truncated struct / outside-firmware-mmap reject, empty-root has zero memory+cpu, memorytest prefix not counted as memory, unbalanced BEGIN/END rejects + accumulator stays 0, multi-root rejects + accumulator stays 0, misnested memory/cpu under bogus parent not counted; FW platform: name table strings, ACPI classification on PC, AcpiVersion consistent; FW catalog promote: UNKNOWN_PROFILE -> VALIDATED + idempotent, unknown name -> 0, NULL name -> 0)
>
> **Notes:**
> - **What shipped** -- `src/kernel/dtb.c` FDT v17 validator + walker; `src/kernel/firmware_platform.c` arbitration + Registry mirror; 12 unit tests.
> - **How it runs** -- BSP-only at Phase 1 right after `firmware_tables_init()`; Registry mirror from `registry_populate_defaults()`. Read-only thereafter.
> - **Downstream effects** -- `HKLM\SYSTEM\Boot\Firmware` is canonical platform fingerprint; §6/§7/§8 read `firmware_platform_get()` for PC-class vs EBBR behavior.
> - **Canonical doc** -- [`include/kernel/firmware_platform.h`](../../include/kernel/firmware_platform.h) + [`include/kernel/dtb.h`](../../include/kernel/dtb.h).
> - **Scope boundary** -- §3 owns DTB header + node counts; full /chosen/memory@N/cpu@N parsing belongs to a future DTB parser TODO.

> **Verified:** 2026-04-30 | commit `f6b5c94b` | 5/5 items | build OK | smoke PASS (KVM 2.38s, "firmware platform: ACPI") | tests 735/735 PASS
> **Quality reviewed:** 2026-04-30 | Codex 8x (design + adversarial + re-adversarial x6 + consistency + perf) | 1H+5M fixed, 0 open | scope: kernel-code-quality

---

## 4. FPDT and Boot Timing Normalization

- [x] Normalize FPDT + TSC timestamps into one ms-since-reset timeline (`boot_timing_bl_entry_ms_since_reset()` anchors TSC steps to FPDT `os_loader_start_start`).
- [x] Detect zero/garbage FPDT records as unreliable (`boot_timing_fpdt_unreliable_eval` rejects all-zero, non-monotonic, >10-min field cap).
- [x] Shared `boot_timing_uefi_total_ms()` -- `boot_timing.c` + VPD "UEFI Boot:" call it (drift-proof).
- [x] Export `X:\Perf\boot-timeline.json` -- `boot_timeline_dump_json()` prepends 5 FPDT entries before TSC steps; 16 KiB buffer, `safe_tsc_delta_ms` clamps reverse order.
- [x] Commit: `"boot: normalize FPDT and bootloader timing"`

**Test checkpoint:** `X:\Perf\boot-timeline.json` exists post-boot with monotonically increasing timestamps from FPDT + bootloader TSC unified. Zero-FPDT firmware (VirtualBox) marks FPDT records `unreliable: true`. `boot_timing.c` and VPD report identical phase totals. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 10 FPDT-normalization sub-tests / 13 assertions, 0 failures (unavailable / all-zero / zero-anchor / clean monotonic / non-monotonic / zero-after-nonzero / leading-zero / 10-min cap / fixed schema / cap+null guards)
>
> **Notes:**
> - **What shipped** -- 5 boot_timing helpers + JSON exporter rewrite (saturating math, 16 KiB buffer); VPD shares the same total-ms helper; 10 unit tests.
> - **How it runs** -- pure reads of `g_boot_info.timing`, lock-free SMP-safe; JSON dump from `boot_phase3()` end.
> - **Downstream effects** -- one `X:\Perf\boot-timeline.json` schema feeds §10 host decoder + future sysinfo; closes path drift across TODO-02 / TODO-11 / TODO-14 / TODO-24.
> - **Canonical doc** -- [`include/kernel/boot_timing.h`](../../include/kernel/boot_timing.h) + JSON shape in `boot_progress.h`.
> - **Scope boundary** -- §4 owns detection + normalization; per-step name JSON escape and full schema doc in TODO-14 §9; multi-vendor FPDT verification by §10 host decoder.

> **Verified:** 2026-04-30 | commit `d409f20a` | 5/5 items | build OK | smoke PASS (KVM 2.32s) | tests 752/752 PASS
> **Quality reviewed:** 2026-04-30 | Codex 8x (design + adversarial + re-adversarial x5 + consistency + perf) | 1H+9M+1L fixed, 0 open | scope: kernel-code-quality

---

## 5. UEFI Memory Attributes and Runtime Properties Inventory

- [x] Parse + catalog EFI MAT -- `mat_init()` caches up to 128 descriptors with `MAT_CLASS_GUARD`/`CODE`/`DATA`/`RODATA`/`WX_VIOLATION` classification; per-entry log + overflow WARN.
- [x] Parse RT Properties Table + cross-check pointers -- `rt_check_property_pointer_mismatch()` covers all 14 named services; mismatches counted via `uefi_rt_property_mismatches()`.
- [x] Expose W^X iteration API for TODO-27 §3 -- `mat_get_count` / `mat_get_entry` / `mat_get_{code,data,guard}_pages` / `mat_overflowed` + `mat_classify_attr` pure helper.
- [x] WARN on advertised-but-NULL RT pointer + clear the supported bit so wrappers cannot null-deref a phantom service.
- [x] Commit: `"boot: inventory UEFI MAT and RT properties"`

**Test checkpoint:** Boot log lists every MAT entry with attributes (`EFI_MEMORY_RP/RX/XP`); runtime W^X status visible to TODO-27 §3 consumers. Firmware advertising `EFI_RT_SUPPORTED_SET_VARIABLE` but having a NULL `SetVariable` pointer logs `[WARN] firmware: SetVariable property mismatch`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 11 MAT classify+inventory sub-tests + RT property mismatch sub-tests, 0 failures (GUARD/CODE/DATA/RODATA/WX classify, NULL+range guards, count <= cap, mismatch counter range, supported-implies-callable invariant)
>
> **Notes:**
> - **What shipped** -- `mat_class_t` + 7 public accessors; `rt_check_property_pointer_mismatch()` covering 14 RT services with bidirectional detection + bit-clearing; 10 unit tests.
> - **How it runs** -- BSP-only at Phase 1; read-only thereafter, queries lock-free SMP-safe.
> - **Downstream effects** -- TODO-27 §3 W^X enforcement consumes the iteration API; §6/§7/§8 read the catalog without touching firmware again.
> - **Canonical doc** -- [`include/kernel/uefi_config.h`](../../include/kernel/uefi_config.h) MAT contract + [`include/kernel/uefi_runtime.h`](../../include/kernel/uefi_runtime.h) RT mismatch contract.
> - **Scope boundary** -- §5 detects/classifies/logs only; W^X enforcement in TODO-27 §3; RT header_size validation closed by §11.

> **Verified:** 2026-04-30 | commit `c2d12aba` | 5/5 items | build OK | smoke PASS (KVM 2.41s) | tests 764/764 PASS
> **Accepted:** [H] RT services per-offset header_size validation gap in pre-existing call_set_virtual_address_map() and 5-pointer init path (reason: scope -- pre-existing init code outside §5 inventory work) -> XREF: 01-boot-platform/TODO-04 §11 (item: "Validate `s_rt->hdr.header_size` per function-pointer offset" at line 182)
> **Quality reviewed:** 2026-04-30 | Codex 12x (design + adversarial-impl x9 + adversarial + consistency + perf) | 1H+9M+1L fixed, 1H accepted-XREF | scope: kernel-code-quality

---

## 6. ESRT Firmware Inventory Mirror

- [x] Parse ESRT entries (`struct esrt_entry`, all 7 UEFI 2.10 entry fields) + cache header metadata (`s_esrt_header`).
- [x] Header accessors `esrt_resource_count_max()` + `esrt_resource_version()`; `esrt_count()` pre-existing.
- [x] Decoder helpers `esrt_decode_status()` (8 mnemonics + reserved) + `esrt_decode_type()` (4 categories); zero-alloc.
- [x] Narrow capsule-policy primitives -- `esrt_rollback_floor_ok(idx)` + `esrt_capsule_persists_across_reset(idx)`; OOR-idx returns 0.
- [x] Registry mirror `HKLM\HARDWARE\Firmware\ESRT\{<FwClass-GUID>}` -- 7 entry fields + decoded `TypeName` / `LastAttemptStatusName` + `_Header` sibling subkey. Idempotent via `RegDeleteTree`.
- [x] Smoke OVMF confirms ESRT-absent path leaves parent key empty without errors.
- [x] ESRT block in `firmware-tables.json` consumes these accessors -- shipped in §8 (`firmware_tables_json.c:423-432`).
- [x] **Advisory composition + `LastAttemptStatusName` operator UX** -- shipped at TODO-27 §2 (commit `3f15268a`). `firmware_advisor_init` consumes `esrt_decode_status` + `esrt_rollback_floor_ok`; `sysinfo firmware-updates` renders the decoded label.
- [x] Commit: `"boot: mirror ESRT firmware inventory"`

**Test checkpoint:** ESRT-bearing firmware (modern bare-metal laptop, OVMF with ESRT) populates `HKLM\HARDWARE\Firmware\ESRT\<FwClass-GUID>\*` with all 7 entry fields + 3 header fields; ESRT-absent firmware (VirtualBox EFI) leaves the registry key empty without errors. TODO-27 §2 capsule policy reads `LowestSupportedFwVersion` to enforce a rollback floor and renders `LastAttemptStatus` as a decoded label (e.g. `INSUFFICIENT_RESOURCES`, `INCORRECT_VERSION`). Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 5 ESRT decoder + capsule-helper sub-tests / 14 assertions, 0 failures (status known mnemonics + reserved fallback, type table, helpers OOR-idx -> 0, persist-bit position constant)
>
> **Notes:**
> - **What shipped** -- 7 public APIs + `EFI_CAPSULE_PERSIST_ACROSS_RESET` constant; `firmware_esrt_registry.c`; 5 unit tests.
> - **How it runs** -- `esrt_init()` BSP-only Phase 1; `esrt_populate_registry()` from `registry_populate_defaults()`. Idempotent.
> - **Downstream effects** -- TODO-27 §2 advisor composes the primitives; §8 JSON consumes the same accessors.
> - **Canonical doc** -- [`include/kernel/uefi_config.h`](../../include/kernel/uefi_config.h) ESRT API contract.
> - **Scope boundary** -- §6 owns kernel-side inventory + Registry + decoders + primitives; authoritative capsule eligibility owned by TODO-27 advanced UEFI work.

> **Verified:** 2026-05-02 | this commit | 9/9 items | build OK | smoke PASS (KVM 2.39s) + ESRT-absent path observed; advisor consumer landed 2026-05-03 at TODO-27 §2 (`3f15268a`)
> **Quality reviewed:** 2026-05-02 | Codex 4x (design + adversarial + consistency + perf) | 2H+2M fixed | scope: kernel-code-quality

---

## 7. UEFI Conformance Profile and EBBR Detection

> [!NOTE]
> **Partial foundation shipped:** EFI_CONFORMANCE_PROFILES_TABLE parsing already exists in `uefi_config.c` (the `s_conformance_level` / `UEFI_CONFORM_FULL` / `UEFI_CONFORM_EBBR` path at uefi_config.c:107-140). What remains is (a) feeding the result through the §1 catalog, (b) relaxing/tightening the required-table set, and (c) detecting EBBR-claim + PC-hardware contradictions.

- [x] Table-driven row registry `s_conformance_rows[]` + per-profile presence flags `s_profile_present[]`; legacy `UEFI_CONFORM_*` scalar kept as backward-compat derivative.
- [x] Spec-correct GUIDs: `UEFI_PROFILE_UEFI_SPEC = 523c91af-...` + `UEFI_PROFILE_EBBR = cce33c35-...` (header had fabricated values that never matched real firmware).
- [x] Per-profile presence semantics so a system claiming BOTH profiles has both flags set; new API `has_profile`, `name`, `allows_omit_pc_tables`, `pc_contradiction`.
- [x] Bounded ECPT walk via `firmware_table_mmap_contains()` + `UEFI_CONFORM_PROFILE_MAX = 16` cap; version mismatch returns UNKNOWN.
- [x] WARN on unknown profile GUID with full canonical hex; out-of-range slots logged separately.
- [x] PC contradiction detector (`__x86_64__`-gated) emits HYBRID WARN on EBBR-claim + x86_64.
- [x] Required-table policy primitive `uefi_conformance_allows_omit_pc_tables()` -- default-deny.
- [x] 6 unit tests covering OOR id, arch-gate, omit-policy, presence-vs-level invariant.
- [x] Commit: `"boot: detect UEFI conformance profile via registry"`

**Test checkpoint:** PC-class boot logs `[ OK ] UEFI: Conformance: UEFI Spec` (or `Full UEFI (assumed)` when ECPT absent). Synthetic EBBR boot logs `Conformance: EBBR` and the §1 catalog summary flips from WARN to `[ OK ] firmware tables: ... PC-class absent (allowed by profile)`. Hybrid case (EBBR + x86_64) logs `[WARN] UEFI: Conformance: EBBR claim with PC-only architecture; treating as HYBRID`. Unknown profile logs the FULL canonical GUID `8-4-4-4-12` hex. QEMU OVMF (no ECPT, FPDT/MAT absent) observed: `[ OK ] UEFI: Conformance: Full UEFI (table absent, assumed)` + `[WARN] BOOT: firmware tables: 17 cataloged, 9 validated, 0 degraded, 2 PC-class table(s) absent (FPDT/MAT/RtProps required by profile)`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 6 conformance sub-tests / 11 assertions, 0 failures (OOR id, name non-empty, pc_contradiction arch-gate, omit-policy default-deny, level-vs-presence consistency invariant, no-match name documented).
>
> **Notes:**
> - **What shipped** -- table-driven row registry + presence flags + 4 public APIs + spec-correct GUIDs replacing fabricated values; 6 unit tests.
> - **How it runs** -- `uefi_conformance_init()` BSP-only Phase 1 before `firmware_tables_init()`; bounded mmap-checked walk; idempotent.
> - **Downstream effects** -- catalog summary 3-way message (all present / absent-allowed / absent-required); §8 JSON consumes the same accessors.
> - **Canonical doc** -- [`include/kernel/uefi_config.h`](../../include/kernel/uefi_config.h) conformance API.
> - **Scope boundary** -- §7 owns detection + primitives + catalog integration; per-profile fixture blobs in §10; ARM/Microsoft profiles wait for published GUIDs.

> **Verified:** 2026-05-02 | this commit | 8/8 items | build OK | smoke PASS (KVM 2.31s) + ECPT-absent path observed
> **Accepted:** [H] firmware_table_mmap_contains accepts UEFI_MMAP_BOOT_SERVICES_* which PMM reclaims before firmware_tables_init runs (reason: scope -- pre-existing oracle predates §7, affects §1/§2/§3/§5 equally) -> XREF: 01-boot-platform/TODO-04 §11 (item: "Tighten `firmware_table_mmap_contains` to reject `UEFI_MMAP_BOOT_SERVICES_CODE/DATA`" at line 242)
> **Quality reviewed:** 2026-05-02 | Codex 7x (design + adversarial-impl + adversarial × 2 + consistency + perf + re-adversarial) | 2H+5M fixed, 1H accepted-XREF | scope: kernel-code-quality

---

## 8. Registry and BlackBox Firmware Report

- [x] Wire-format schema pinned at `docs/boot/firmware-tables-schema.md` (schema_version=1; tables[] / degraded[] / quirks_active[] + typed acpi/smbios/mat/rt_properties/esrt/apei/dbg2/wsmt sub-blocks; RFC 8259 escapes).
- [x] Registry mirror `HKLM\HARDWARE\Firmware\Tables\<name>\*` -- Address (REG_QWORD) + Size + Checksum + ValidationStatus + Source per cataloged entry. Idempotent via `RegDeleteTree`.
- [x] JSON writer `X:\Diag\firmware-tables.json` -- 16 KiB pmm buffer, all typed sub-blocks, fail-closed on truncation. Disk write live since D05 T04 §15 shipped (smoke confirms `wrote X:\Diag\firmware-tables.json (4528 bytes)`).
- [x] Validation failures + addresses + checksums + quirk matches surfaced -- `computed_checksum` populated BEFORE sum-check by every validator; `quirks_active[]` populated by §9.
- [x] Commit: `"boot: publish firmware inventory report"` (shipped under `boot: persist firmware-tables.json schema + registry mirror`).

**Test checkpoint:** Post-boot, `X:\Diag\firmware-tables.json` exists, validates against the schema doc (`docs/boot/firmware-tables-schema.md`) AND against the host decoder (§10). `HKLM\HARDWARE\Firmware\Tables\<GUID>\Address` reads back the same physical address that `firmware_table_lookup_guid()` returns. APEI block shows BERT/HEST signatures when present (operator-visible persistent hardware-error records); DBG2 block lists secondary debug ports beyond SPCR; WSMT block reports the SMM mitigations bitmap so users see firmware SMM posture. `sysinfo.exe firmware` prints the same table set with checksums + quirk matches. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | smoke confirms `17/17 firmware table(s) mirrored under HARDWARE\Firmware\Tables` + JSON-writer WARN observed (VFS gap)
>
> **Notes:**
> - **What shipped** -- schema doc + Registry mirror + JSON formatter + `computed_checksum` field populated by every validator before sum-check.
> - **How it runs** -- Registry mirror from `registry_populate_defaults()`; JSON writer from `boot_phase3()` BSP-only; emits all 5 required typed sub-blocks.
> - **Downstream effects** -- `HKLM\HARDWARE\Firmware\Tables\*` + `firmware-tables.json` are the canonical machine-readable firmware fingerprint for §10 + Win32 GetSystemFirmwareTable consumers.
> - **Canonical doc** -- [`docs/boot/firmware-tables-schema.md`](../../docs/boot/firmware-tables-schema.md) + [`include/kernel/firmware_tables.h`](../../include/kernel/firmware_tables.h).
> - **Scope boundary** -- §8 owns Registry + JSON + schema; quirks_active[] populated by §9; host decoder by §10; JSON-builder library + VFS-cluster fix by §12 / D05 T04 §15. Userland `sysinfo.exe firmware` is out-of-scope (future userland TODO consumes the schema doc when it lands).

> **Verified:** 2026-05-03 | this commit | 5/5 items | build OK | smoke PASS (KVM 2.51s) + Registry 17/17 mirrored + firmware-tables.json (4528 bytes) written
> **Quality reviewed:** 2026-05-02 | Codex 11x (design + adversarial-impl + adversarial x2 + consistency + perf + re-adversarial x4) | 4H+2M fixed, 1H rejected (false-positive prompt) | scope: kernel-code-quality

---

## 9. Firmware Quirk Database

- [x] SMBIOS-keyed quirk descriptor table (`s_quirks[]` of `{bit, name, vendor_substr, product_substr, bios_substr}`); X-macro pins `{name,bit}` parity (closed by §12).
- [x] 5 named quirks: `BROKEN_FPDT` / `BAD_MADT_CHECKSUM` / `GOP_PITCH_LIES` / `BOGUS_MAT` / `USB_HANDOFF_BLACKLIST`; consumers gate on `firmware_quirks_is_active()`.
- [x] LOG_INFO summary at BSP Phase 1 + JSON `quirks_active[]` populated by §8 writer via `firmware_quirks_iter_next()`.
- [x] `boot.conf firmware_quirk_disable=name1,name2` override -- shared static-inline tokenizer used by both kernel + bootloader (closed by §12).
- [x] Commit: `"boot: firmware quirk database"`

**Test checkpoint:** Synthetic SMBIOS vendor=`TestVendor` product=`BrokenFPDT` triggers the broken-FPDT quirk and `firmware-tables.json.quirks_active[]` contains `broken_fpdt`. `boot.conf` line `firmware_quirk_disable=broken_fpdt` overrides the trigger. BlackBox dump includes `quirks_active`. Test on: QEMU WHPX (custom SMBIOS), QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 9 firmware-quirk sub-tests (parse single/multi/unknown/null-empty/whitespace, canonical-name-each-bit, parse round-trip all bits, name-invalid, is_active polarity), 0 failures
>
> **Notes:**
> - **What shipped** -- `firmware_quirks.[ch]` + 5 named quirks + 6 public APIs + 9 unit tests; X-macro at `firmware_quirks_table.inc` enforces {name,bit} parity.
> - **How it runs** -- BSP-only at Phase 1 between `uefi_conformance_init()` and `firmware_tables_init()`; ANDs `~firmware_quirk_disable` against auto-detected mask. Idempotent.
> - **Downstream effects** -- `quirks_active[]` published by §8 JSON; BlackBox transcript inherits the LOG_INFO line.
> - **Canonical doc** -- [`include/kernel/firmware_quirks.h`](../../include/kernel/firmware_quirks.h).
> - **Scope boundary** -- §9 owns detection + override + publication; per-quirk workaround consumers live in TODO-22 / TODO-25 / TODO-15 / §5 / TODO-16. Tokenizer dedup closed by §12.

> **Verified:** 2026-05-02 | this commit | 5/5 items | build OK | smoke PASS (KVM 2.39s) + `[OK] FW: quirks: 0 active` observed
> **Accepted:** [M] Bootloader `firmware_quirk_disable=` tokenizer is duplicated from `firmware_quirks_parse_disable()` (reason: shared static-inline tokenizer needs new .inc plumbing across kernel/bootloader idiom boundary; X-macro already covers `{name, bit}` drift) -> XREF: 01-boot-platform/TODO-04 §12 (item: "Consolidate `firmware_quirk_disable=` tokenizer between kernel and bootloader" at line 291)
> **Quality reviewed:** 2026-05-02 | Codex 12x (design + adversarial + consistency x3 + perf x3 + re-adversarial x2 + test-coverage x3) | 4H+3M fixed, 1M accepted-XREF | scope: kernel-code-quality

---

## 10. Firmware Inventory Tests and Host Decoder

- [x] Unit tests in `test_firmware_tables.c` cover GUID lookup, range rejection, checksum failures, conformance levels (~957 assertions across §1-§7).
- [x] Positive-case fixture directory `src/kernel/test/fixtures/firmware/` with schema_version=1 snapshot + README.
- [x] Host tool `tools/firmware-tables-decode.c` -- self-contained parser + canonical re-emitter (pretty-print / `--canonical` / `--round-trip`); `host-tools` aggregate; regression via `scripts/test-firmware-decode.sh`.
- [x] Synthetic firmware fixtures for malformed shapes -- inline in `test_firmware_tables.c` (DTB bad magic / oversized totalsize / unsupported version / truncated block / unbalanced begin-end / multi-root / misnested memory-cpu).
- [x] **BlackBox read-back round-trip tests** -- `test_fileio.exe` opens `X:\Diag\blackbox-marker.txt` (11-byte "BlackBox-v1" mcopied into the FAT32 image), asserts byte-exact readback. Proves the FAT32 stack end-to-end from ring 3.
- [x] Verify on QEMU OVMF (host decoder + 5 reject paths PASS, live `firmware-tables.json` write confirmed via smoke); VirtualBox + bare-metal deferred to hardware.
- [x] Decoder hardening: full v1 required-field validation -- `validate_v1_required()` walks 15 documented keys + types; missing-required / wrong-type rejected with exit 5/9. Negative fixtures shipped in `scripts/test-firmware-decode.sh`.
- [x] Decoder hardening: length-aware string handling -- `jp_string` rejects embedded ` ` with a clear error (full `{data,len}` refactor unnecessary; kernel writer never emits NULs and the reject closes the truncation hazard).
- [x] Commit: `"test: firmware table inventory coverage"`

**Test checkpoint:** All 4 unit tests in `## Unit Tests` PASS under `SUITE=boot`. Host decoder tool reads a captured `firmware-tables.json` and prints a human-readable table inventory. Fixture blobs in `src/kernel/test/fixtures/firmware/` cover RSDP / SMBIOS3 / ESRT / FPDT / DTB shapes. BlackBox read-back round-trip via `test_fileio.exe` opens `X:\Diag\blackbox-marker.txt`, asserts 11-byte length + byte-exact "BlackBox-v1" payload + clean close (catches BPB/fsck/walk_path/open/read regressions on the FAT32 stack). Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `bash scripts/test-firmware-decode.sh` | host-decoder pretty-print + round-trip + schema-version reject; PASS on QEMU OVMF this commit
>
> **Notes:**
> - **What shipped** -- `tools/firmware-tables-decode.c` host decoder + `host-tools` recipe + `scripts/test-firmware-decode.sh` + fixture dir with sample snapshot.
> - **How it runs** -- `make build/tools/firmware-tables-decode` (no external deps); regression script exercises pretty-print + round-trip + schema-version reject. Idempotent.
> - **Downstream effects** -- TODO-29 §2 boot-health.json consumer can reuse the pattern; future sysinfo userland tool consumes the schema doc.
> - **Canonical doc** -- [`docs/boot/firmware-tables-schema.md`](../../docs/boot/firmware-tables-schema.md).
> - **Scope boundary** -- §10 owns kernel tests + decoder + fixtures; §11 / D05 T04 §15 own the VFS write fix BlackBox read-back depends on; bare-metal verification gated on hardware.

> **Verified:** 2026-05-03 | this commit | 9/9 items | build OK | tests 2743/2743 PASS + 16/16 user-mode (test_fileio asserts marker readback) | smoke confirms `wrote X:\Diag\firmware-tables.json (4528 bytes)` + 11-byte marker round-trip
> **Deferred:** [L] VirtualBox + bare-metal verification (reason: hardware availability) -> XREF: 04-drivers-hardware (no concrete owner item yet; revisit when test laptop available)
> **Quality reviewed:** 2026-05-02 | Codex 7x (design + adversarial-impl + adversarial + consistency x2 + perf x2) | 3H+2M fixed (round-trip schema-version-gate + trailing-garbage reject + strict number parser + fixture rebuild to match kernel writer's actual conformance_profile keys + NUL-byte stripped from TODO prose), 2H accepted-XREF, 1L rejected (obj_push partial-realloc OOM not realistic) | scope: userland-code-quality (host-side tool, no kernel impact)

---

## 11. Firmware Inventory Bug-Fix Debt

Pre-existing safety / regression fixes surfaced by §1-§9 review pipelines. Each item names a concrete root-cause fix that resolves an active bug or a known-bad path on real hardware. Refactor / dedup hygiene is owned by §12.

- [x] **Phase-3 `vfs_open(X:\Diag\..., VFS_O_WRITE)` failure cluster** -- closed by D05 T04 §15 (VFS_O_TRUNC + walk_path) + fat32_fsck sector-0 re-read in commit `d96b76fc`. Smoke confirms all 3 writers persist on X:\.
- [x] Tightened `firmware_table_mmap_contains` to reject `UEFI_MMAP_BOOT_SERVICES_CODE/DATA` + `LOADER_CODE/DATA` (PMM reclaims both in Phase 0); pre-reclaim variant exposed for Phase-0 callers. OVMF: 6 newly-degraded catalog entries flagged `range_unmapped`.
- [x] Validated `s_rt->hdr.header_size` per function-pointer offset -- `rt_field_within_header()` + `CHECK_RT_PTR` macro gate SVAM read + every callable RT pointer (extended to all 14 services post-review); fail-closed latched effective header_size after extent validation.
- [x] Commit: `"boot: firmware inventory bug-fix debt"`

**Test checkpoint:** Phase-3 VFS writes to `X:\Diag\*` succeed (smoke confirms `wrote X:\Diag\firmware-tables.json (4528 bytes)` after the D05 T04 §15 + fat32_fsck sector-0 fix). `firmware_table_mmap_contains` rejects synthetic Boot-Services fixture (smoke confirms 6 catalog entries flagged degraded as `range_unmapped`). Truncated-RT-header fixture triggers WARN + RT-services degrade. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | smoke confirms RT services pass all 5 critical-pointer checks; firmware tables: 17 cataloged, 9 validated, 6 degraded (Boot-Services rejection working correctly)
>
> **Notes:**
> - **What shipped** -- 2 safety fixes: `fw_mmap_contains` lifecycle split (post-reclaim default rejects BootServices+Loader; pre-reclaim variant for Phase-0 callers); per-offset RT header_size gate covering all 14 callable RT services with deferred pointer loads + latched effective_hdr_size.
> - **How it runs** -- both BSP-only at Phase 1; oracle read-only after init; RT gate runs once per `uefi_runtime_init()` + `call_set_virtual_address_map()`. Idempotent.
> - **Downstream effects** -- `firmware-tables.json` publishes 6 newly-degraded entries with `checksum_status: range_unmapped` on OVMF; malformed firmware would degrade RT services cleanly instead of overreading.
> - **Canonical doc** -- [`include/kernel/firmware_tables.h`](../../include/kernel/firmware_tables.h) oracle contract + RT header gate in `src/kernel/uefi_runtime.c`.
> - **Scope boundary** -- §11 owns the 3 safety fixes (mmap_contains lifecycle, RT header gate, fat32_fsck sector-0 re-read in commit `d96b76fc`); refactor / dedup in §12.

> **Verified:** 2026-05-03 | this commit | 3/3 items | build OK | tests 2743/2743 PASS | smoke PASS (KVM 2.57s, X:\ mounted, all 3 Phase-3 writers persist, 17 firmware tables / 9 validated / 6 correctly degraded)
> **Quality reviewed:** 2026-05-03 | Codex 10x (design + adversarial-impl + adversarial x2 + consistency x2 + perf x2 + re-adversarial x3) | 8H+2M fixed, 1H cross-filed to D05 T04 §15, 1M accept-as-is | scope: kernel-code-quality

---

## 12. Firmware Inventory Refactor Debt

Cross-section dedup / future-spec drift prevention. None of these items is an active regression on any currently-tested platform; each removes a duplicated source of truth or a class of latent failure that will become a regression on richer firmware.

- [x] SMBIOS EP-length constants in shared `include/kernel/smbios_wire.h`; both `smbios.c` and `firmware_tables.c` include it; local `FW_SMBIOS*` aliases dropped.
- [x] `firmware_quirk_disable=` tokenizer extracted to `include/kernel/firmware_quirks_parse.inc` static-inline; both kernel and bootloader call it. X-macro `_Static_assert` enforces UINT8 fit. 9 existing kernel tests cover the bootloader path by construction.
- [x] Kernel-wide JSON builder library at `include/kernel/util/json_builder.[ch]` with fail-closed truncation; `firmware_tables_json.c` retrofitted to skip the write on overflow. 10 unit tests cover putc/puts/str/escapes/hex64/u32_dec/truncation. `boot_progress.c` left on snprintf (already fail-closed via `goto close`).
- [x] Commit: `"boot: firmware inventory refactor debt"`

**Test checkpoint:** SMBIOS entry-point constants compile-time linked between `smbios.c` and `firmware_tables.c` via shared header. Quirk-tokenizer parses both kernel and bootloader paths from one source. JSON builder regression test (10 cases) covers normal writes + truncation + control-byte escapes. No runtime behavior change on QEMU OVMF (smoke confirms identical 17/9/6 catalog counts). Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 10 json_builder tests + existing 9 firmware_quirks tests | 0 failures
>
> **Notes:**
> - **What shipped** -- `include/kernel/smbios_wire.h` (3 EP-length constants), `include/kernel/firmware_quirks_parse.inc` (static-inline tokenizer + X-macro UINT8 fit guard), `include/kernel/util/json_builder.[ch]` (10-function fail-closed JSON library), `src/kernel/test/test_json_builder.c` (10 unit tests).
> - **How it runs** -- Refactor-only; byte-equivalent on the live boot path (verified: identical 17/9/6 catalog counts on smoke). Compile-time guards fire in every TU that includes the .inc files; bootloader Makefile now lists both `firmware_quirks_table.inc` + `firmware_quirks_parse.inc` + `uki_cmdline_check.h` as bootx64.o prerequisites.
> - **Downstream effects** -- Future quirks with bit > 0xFF fail the build instead of silently truncating at the bootloader's UINT8 narrowing. Future kernel JSON publishers (BlackBox, boot-health) reuse the json_builder library + fail-closed gate pattern instead of re-rolling jb_*.
> - **Canonical doc** -- [`include/kernel/util/json_builder.h`](../../include/kernel/util/json_builder.h) builder API contract; [`include/kernel/firmware_quirks_parse.inc`](../../include/kernel/firmware_quirks_parse.inc) tokenizer contract.
> - **Scope boundary** -- `boot_progress.c` boot-timeline writer kept on snprintf-bounded writes; it is already fail-closed via `goto close` and migrating to jb_* would be churn for no semantic gain.

> **Verified:** 2026-05-03 | this commit | 4/4 items | build OK | tests 2639/2639 PASS | smoke PASS (KVM, identical 17/9/6 catalog vs pre-refactor)
> **Quality reviewed:** 2026-05-03 | Codex 7x (design + adversarial x2 + consistency x2 + perf x2) | 3H+1M fixed, 0 open | scope: kernel-code-quality

---

## OS Comparison

| ⭐  | Feature                               | 🪟 Win11                         | 🐧 Linux                         | 🚀 Impossible OS                  |
| --- | ------------------------------------- | -------------------------------- | -------------------------------- | --------------------------------- |
| 💎  | Firmware table catalog API            | ✅ ACPI/HAL + WMI                | ✅ acpi_get_table + sysfs        | ✅ firmware_table_entry_t catalog |
| 💎  | Physical range + checksum validation  | ✅ HAL validates pre-use         | ✅ acpi_tb_verify_checksum       | ✅ §2 mmap-bound + checksums      |
| 💎  | ACPI/SMBIOS/DTB arbitration           | ⚠️ ACPI-first; no DTB            | ✅ ACPI on x86, DTB on ARM       | ✅ §3 ACPI > DTB + HYBRID detect  |
| 💎  | FPDT boot-timeline normalization      | ✅ FPDT + ETW timeline           | ⚠️ acpi_fpdt_init read-only      | ✅ §4 FPDT + TSC unified JSON     |
| 💎  | UEFI MAT + Runtime Properties         | ✅ MmGetEfiRuntimeServicesTable  | ✅ efi_memmap_attributes         | ✅ §5 MAT inventory + RT mismatch |
| 💎  | ESRT firmware inventory               | ✅ Windows Update / fwupdd       | ✅ fwupd /sys/firmware/efi/esrt  | ✅ §6 HKLM ESRT mirror + decoders |
| 💎  | UEFI conformance profile + EBBR       | ⚠️ assumes full PC profile       | ✅ EBBR detection in efi-stub    | ✅ §7 ECPT + EBBR + PC contradict |
| 💎  | Registry + BlackBox firmware report   | ✅ msinfo32 + Event Log          | ⚠️ scattered (dmidecode/sysfs)   | ✅ §8 HKLM mirror + JSON writer   |
| ⭐  | Firmware quirk database               | ⚠️ HAL-internal, opaque          | ⚠️ DMI quirks scattered          | ✅ §9 SMBIOS-keyed + JSON publish |
| ⭐  | Host decoder for firmware-tables.json | ❌ N/A                           | ❌ N/A                           | ✅ §10 firmware-tables-decode     |
| 💎  | APEI (BERT/HEST/EINJ/ERST) visibility | ✅ WHEA hardware-error subsystem | ✅ /sys/firmware/acpi/* + ras-mc | ✅ §8 apei block in JSON writer   |
| ⭐  | DBG2 secondary debug ports            | ✅ kernel debugger reads DBG2    | ✅ amba_pl011 + earlycon DBG2    | ✅ §8 dbg2 block in JSON writer   |
| ⭐  | WSMT SMM mitigations posture          | ✅ HAL reads WSMT bitmap         | ❌ Linux ignores WSMT            | ✅ §8 wsmt block in JSON writer   |
| 💎  | firmware-tables.json schema doc       | ⚠️ msinfo32 schema undocumented  | ⚠️ tools differ per distro       | ✅ §8 firmware-tables-schema.md   |
| ⭐  | Single-source SMBIOS + quirk parsers  | ❌ N/A (no public refactor)      | ⚠️ DMI const drift across files  | ✅ §12 shared header + .inc dedup |

---

## Unit Tests

- [x] `test_firmware_table_guid_lookup` -- §1: ACPI 2.0/1.0 cfg-table entry; non-zero phys_addr; unknown GUID + NULL guid both return NULL.
- [x] `test_firmware_table_range_rejects_unmapped` -- §2: phys_addr in CONVENTIONAL mmap downgrades to RANGE_UNMAPPED; clean firmware leaves no validator degradations.
- [x] `test_firmware_table_checksum_fail` -- §2: corrupted SDT byte downgrades to CHECKSUM_FAIL; size-below-header and header.length-mismatch downgrade to LENGTH_BAD; one-way downgrade preserved.
- [x] `test_firmware_conformance_profiles` -- 6 conformance tests shipped in §7 (`test_firmware_tables.c:653-723`).

---

## Verification

- [x] `bash scripts/build.sh` clean build with TODO-04 sources -> `=== BUILD OK ===`.
- [x] `bash scripts/test.sh SUITE=boot` -> all `test_firmware_*` cases PASS (2639 kernel + 16 user-mode total this session).
- [/] QEMU OVMF: `[BOOT] firmware tables: 17 cataloged, 9 validated, 6 degraded` confirmed via smoke. `firmware-tables.json` disk-write blocked by D05 T04 §15 VFS cluster (in-memory builder verified equivalent on the round-trip fixture).
- [ ] VirtualBox EFI (ACPI + SMBIOS, no FPDT/ESRT): boot succeeds; firmware report flags FPDT + ESRT as absent (not degraded).
- [ ] Bare-metal laptop + desktop: every active firmware quirk is logged + included in the BlackBox report; no silent table-validation failures on serial.
- [ ] Commit: `"docs/firmware: TODO-04 verification complete"`

**Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 4 firmware-inventory sub-tests, 0 failures (target on full implementation)
