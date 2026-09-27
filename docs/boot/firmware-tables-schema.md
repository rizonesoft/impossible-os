<!-- docs: covers=todo/01-boot-platform/TODO-04-firmware-table-platform-inventory.md sources=src/kernel/firmware_tables_json.c,src/kernel/firmware_tables_registry.c reviewed=2026-09-28 -->
# `firmware-tables.json` Wire Format (schema_version 1)

> Canonical wire format for `X:\Diag\firmware-tables.json` and the
> `HKLM\HARDWARE\Firmware\Tables\*` registry mirror. Single source of
> truth for the kernel writer (`firmware_tables_publish_json`,
> `firmware_tables_populate_registry`), the host decoder, a future
> `sysinfo.exe firmware` tool, ETW pipelines, and any Win11
> `GetSystemFirmwareTable` consumer that round-trips through this file.

## Versioning

- Top-level `"schema_version"` (integer, current value `1`).
- Bumped when (a) a field is removed, (b) a field's type changes, (c) an
  enum value is renamed, or (d) the meaning of an existing value
  changes. Adding new optional fields is **not** a bump.
- Consumers MUST refuse to interpret a file whose `schema_version`
  exceeds the highest version they understand.

## Top-level keys

| Key                     | Type   | Required | Description                                                                                                     |
| ----------------------- | ------ | :------: | --------------------------------------------------------------------------------------------------------------- |
| `schema_version`        | int    | yes      | Wire-format version (currently `1`).                                                                            |
| `generated_at_utc`      | string | yes      | ISO-8601 timestamp pinned to the git HEAD commit time (matches `coverage.json` pattern; offline-deterministic). |
| `firmware_platform`     | string | yes      | One of `"ACPI"`, `"DTB"`, `"HYBRID"`, `"UNKNOWN"` (`firmware_platform_name`).                                   |
| `conformance_profile`   | object | yes      | See **Conformance profile** below.                                                                              |
| `tables`                | array  | yes      | Per-entry table descriptors (see **Per-entry shape**).                                                          |
| `degraded`              | array  | yes      | Catalog entries whose status is `FW_STATUS_DEGRADED`; subset of `tables[]` indices.                             |
| `quirks_active`         | array  | yes      | Array of canonical firmware quirk names (lowercase tokens) active after `boot.conf firmware_quirk_disable=` suppression. Empty when no quirks fired.                                                            |
| `acpi`                  | object | yes      | Aggregate ACPI summary (see **Typed sub-blocks**).                                                              |
| `smbios`                | object | yes      | Aggregate SMBIOS summary.                                                                                       |
| `mat`                   | object | yes      | UEFI Memory Attributes Table summary.                                                                           |
| `rt_properties`         | object | yes      | UEFI Runtime Properties summary.                                                                                |
| `esrt`                  | object | yes      | ESRT inventory: `_Header` + per-entry data mirroring the Registry layout.                                       |
| `apei`                  | object | yes      | APEI generic listing (BERT/HEST/EINJ/ERST presence + addr/size).                                                |
| `dbg2`                  | object | yes      | Microsoft Debug Port Table 2 generic listing.                                                                   |
| `wsmt`                  | object | yes      | Windows SMM Mitigations Table generic listing.                                                                  |

## Per-entry shape (`tables[]`)

Every cataloged firmware table emits one entry:

```json
{
  "name": "ACPI2.0",
  "guid": "{8868e871-e4f1-11d3-bc22-0080c73c8881}",
  "source": "uefi_cfg_table",
  "phys_addr": "0x000000007fec0014",
  "size": 36,
  "checksum": 0,
  "checksum_status": "validated",
  "validation_reason": null
}
```

| Field               | Type           | Required | Description                                                                                                                                                                                                          |
| ------------------- | -------------- | :------: | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `name`              | string         | yes      | Catalog name (`firmware_table_entry_t.name`). Always populated; primary key for lookups.                                                                                                                            |
| `guid`              | string \| null | no       | Canonical Microsoft brace form `{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}` when source is `uefi_cfg_table`, `esrt`, or any GUID-keyed source. `null` for ACPI SDTs (which carry only a 4-char signature).               |
| `source`            | string enum    | yes      | One of `"uefi_cfg_table"`, `"acpi_sdt"`, `"smbios_raw"`, `"fpdt"`, `"esrt"`, `"dtb"` (matches `FW_SOURCE_*` enum in `firmware_tables.h`).                                                                            |
| `phys_addr`         | string         | yes      | Physical address as `0x` + 16 hex digits (lowercase). `"0x0000000000000000"` is reserved for `name`-only entries (e.g. signature-known but address absent on this firmware).                                         |
| `size`              | int            | yes      | Byte size as cataloged. May be `0` when the source does not carry a size field (UEFI cfg-table entries have GUID + pointer only; the catalog does not probe).                                                        |
| `checksum`          | int            | yes      | Firmware-published checksum byte (`0`-`255`) stashed by the validator before the sum-check, so `checksum_fail` entries still carry the byte. `0` when the validator did not compute one (catalog-only / signature-only entries). |
| `checksum_status`   | string enum    | yes      | One of `"validated"`, `"degraded"`, `"unknown_profile"`, `"untested"`. `"validated"` is `FW_STATUS_VALIDATED`; `"degraded"` is `FW_STATUS_DEGRADED`; `"unknown_profile"` is `FW_STATUS_UNKNOWN_PROFILE`.              |
| `validation_reason` | string \| null | no       | Operator-readable reason when `checksum_status == "degraded"`; one of `"checksum_fail"`, `"length_bad"`, `"range_unmapped"`, `"null_pointer"`, plus future reasons. `null` otherwise.                                |

## Typed sub-blocks

### `conformance_profile`

```json
{
  "name": "UEFI Spec",
  "level": "FULL",
  "has_uefi_spec": true,
  "has_ebbr": false,
  "allows_omit_pc_tables": false,
  "pc_contradiction": false
}
```

`name` is the display name from `uefi_conformance_name()`. `level` is
the backward-compat scalar (`"FULL"` / `"EBBR"` / `"UNKNOWN"`). The
four booleans expose the policy primitives so the host decoder can
render the same warnings the kernel does.

### `acpi`

```json
{
  "version": 2,
  "rsdp_addr": "0x00000000000e0000",
  "xsdt_addr": "0x000000007fea2014",
  "sdt_count": 8
}
```

### `smbios`

```json
{
  "version_major": 3,
  "version_minor": 5,
  "table_addr": "0x000000007fea0000",
  "structure_count": 12,
  "vendor": "Tianocore",
  "product": "OVMF"
}
```

### `mat`

```json
{
  "entry_count": 27,
  "code_pages": 4,
  "data_pages": 12,
  "rodata_pages": 0,
  "guard_pages": 10,
  "wx_violation_pages": 1,
  "overflowed": false
}
```

### `rt_properties`

```json
{
  "supported_bitmask": "0x0000007f",
  "mismatch_count": 0
}
```

### `esrt`

Mirrors the Registry value names byte-for-byte:

```json
{
  "_Header": {
    "ResourceCount": 1,
    "ResourceCountMax": 8,
    "ResourceVersion": 1
  },
  "{12345678-1234-1234-1234-1234567890ab}": {
    "Type": 1,
    "TypeName": "System",
    "FwVersion": 256,
    "LowestSupportedFwVersion": 100,
    "CapsuleFlags": 65536,
    "LastAttemptVersion": 256,
    "LastAttemptStatus": 0,
    "LastAttemptStatusName": "SUCCESS"
  }
}
```

- `_Header.ResourceVersion` is **u64** (REG_QWORD on the registry side).
  The JSON integer value MUST NOT be truncated to 32 bits even though
  firmware always writes `1` in practice. JSON consumers are expected
  to handle u64; decoders that cannot SHOULD emit a 0x-prefixed string
  instead and bump `schema_version`.
- `TypeName` enum spellings: `"System"`, `"Device"`, `"Driver"`,
  `"Unknown"` (UEFI 2.10 §23.6 categories).
- `LastAttemptStatusName` enum spellings (UEFI 2.10 Table 23-3):
  `"SUCCESS"`, `"ERROR_UNSUCCESSFUL"`,
  `"ERROR_INSUFFICIENT_RESOURCES"`, `"ERROR_INCORRECT_VERSION"`,
  `"ERROR_INVALID_FORMAT"`, `"ERROR_AUTH_ERROR"`,
  `"ERROR_PWR_EVT_AC"` (UEFI 2.7+ rename of `AC_NOT_CONNECTED`),
  `"ERROR_PWR_EVT_BATT"` (UEFI 2.7+ rename of `INSUFFICIENT_BATTERY`),
  `"ERROR_UNSATISFIED_DEPENDENCIES"` (added in UEFI 2.7),
  `"Reserved"` (any value outside 0x00..0x08).
- Per-entry key is the canonical Microsoft brace-form `FwClass` GUID.

### `apei`, `dbg2`, `wsmt`

Generic listings -- per-table parsing is future work. Each block shows
the table's catalog presence + address + checksum_status, with no
semantic decode of the body:

```json
{
  "bert": { "present": true, "addr": "0x000000007fea3000", "size": 48, "checksum_status": "validated" },
  "hest": { "present": false, "addr": null, "size": 0, "checksum_status": null },
  "einj": { "present": false, "addr": null, "size": 0, "checksum_status": null },
  "erst": { "present": false, "addr": null, "size": 0, "checksum_status": null }
}
```

`dbg2` and `wsmt` follow the same `present + addr + size + checksum_status`
pattern with one key per table (e.g. `wsmt` has just one key
`"wsmt": {...}`).

## Absent vs degraded semantics

- **Absent** (`"present": false`, `addr: null`): the catalog never saw
  this table. Operator interpretation: firmware does not advertise it.
  Not an error.
- **Degraded** (`"checksum_status": "degraded"`): the catalog saw it
  AND validation rejected it. `validation_reason` carries the cause.
  Operator interpretation: real bug -- bad firmware, corruption, or
  a wrong-type physical address; surfaces in `degraded[]` array.
- **Unknown profile** (`"checksum_status": "unknown_profile"`): the
  catalog saw it but no per-source validator covers it (UEFI cfg-table
  entry whose GUID is not in our recognized list).

## Numeric units

All `size` and `*_pages` fields are byte counts and 4 KiB pages
respectively (matching the kernel C definitions). All `addr` /
`phys_addr` fields are physical addresses, host byte order interpreted
as a 64-bit unsigned integer, rendered as `0x` + 16 lowercase hex
digits.

## String escaping

Vendor / product / name strings are JSON-escaped per RFC 8259 §7:
- `\\` for backslash
- `\"` for double quote
- `\n` `\r` `\t` for the obvious controls
- `\u00XX` for any byte in 0x00..0x1F not covered above
- All other bytes pass through verbatim (UTF-8 expected; the writer
  does not validate UTF-8 well-formedness)

## Bump rules

- Adding an optional field: no bump.
- Adding a new enum value to an existing field: no bump (consumers
  MUST treat unknown enum values as opaque).
- Renaming or removing a field, or changing its type: bump
  `schema_version`.
- Renaming an enum value: bump.

## Registry mirror parity

`HKLM\HARDWARE\Firmware\Tables\<name-or-GUID>\*` writes the per-entry
shape as REG_DWORD/REG_QWORD/REG_SZ values:

| Registry value name | Type      | JSON field          |
| ------------------- | --------- | ------------------- |
| `Address`           | REG_QWORD | `phys_addr`         |
| `Size`              | REG_DWORD | `size`              |
| `Checksum`          | REG_DWORD | `checksum`          |
| `ValidationStatus`  | REG_SZ    | `checksum_status`   |
| `Source`            | REG_SZ    | `source`            |

The Registry mirror is `RegDeleteTree`-cleared on every populate (same
idempotent pattern as `HKLM\HARDWARE\Firmware\ESRT`).
