<!-- docs: covers=todo/01-boot-platform/TODO-29-boot-perf-health-observability.md sources=src/kernel/main/boot_health.c,src/kernel/main/boot_trend.c reviewed=2026-09-28 -->
# `boot-health.json` Wire Format (schema_version 1)

> Canonical wire format for `X:\Diag\boot-health.json`. Single source of
> truth for the kernel writer (`boot_health_publish_json` in
> `src/kernel/main/boot_health.c`), the host-side `sysinfo.exe` decoder,
> the future boot-trend rolling regression file, the CI regression gate,
> and any bare-metal triage path where serial is unavailable.

## Versioning

- Top-level `"schema_version"` (integer, current value `1`).
- Bumped when (a) a field is removed, (b) a field's type changes, (c) an
  enum string is renamed, or (d) the meaning of an existing value
  changes. Adding new optional fields is **not** a bump.
- Consumers MUST refuse to interpret a file whose `schema_version`
  exceeds the highest version they understand.
- The publisher writes once per boot at Phase 3 (after
  `boot_history_kernel_mark_phase3`), via `vfs_open(VFS_O_TRUNC)`.
  On short write the publisher re-truncates the file to 0 bytes so a
  consumer never sees a malformed JSON prefix; "no data this run" is
  treated as a recoverable signal, "partial JSON" is not.

## Time encoding

All time fields in this schema are emitted as **unsigned 32-bit Unix
epoch seconds** (UTC), unquoted JSON numbers. This deviates from
`firmware-tables.json`'s ISO-8601 string convention; the rationale:

- `boot-health.json` is consumed by the boot-trend diff/regression path
  which subtracts timestamps in arithmetic. ISO-8601 strings would force
  every consumer to parse before comparing.
- `sysinfo.exe` formats epoch seconds for human display at print time.
- The valid range (1970..2106) covers every realistic boot of this OS.

The unit is appended to most time-key names (`*_unix`) so downstream
readers cannot misread the type. One exception: `generated_at_utc`
retains its canonical name (matches the firmware-tables.json convention
for "when this file was generated"), with the unit documented in its
row of the table below.

## Top-level keys

| Key                        | Type   | Required | Description                                                                                            |
| -------------------------- | ------ | :------: | ------------------------------------------------------------------------------------------------------ |
| `schema_version`           | int    | yes      | Wire-format version (currently `1`).                                                                   |
| `generated_at_utc`         | int    | yes      | Unix epoch seconds (UTC) for THIS boot. Anchored to the seq successfully committed by `boot_history_kernel_mark_phase3()` (NOT max-of-ring); the corresponding ring entry's `unix_time` is emitted. May be `0` when this boot's history timestamp is unavailable -- root causes: firmware `GetTime` failed (RTC unreadable), the Phase-3 mark could not persist to NVRAM (`SetVariable` failed), or the committed seq is missing from the ring snapshot. Consumers MUST treat `0` as "current boot timestamp unavailable," NOT as "epoch 1970-01-01." |
| `boot_seq`                 | int    | yes      | Phase-3 committed sequence number for THIS boot. `0` when the mark could not persist (same root causes as `generated_at_utc: 0`). NEVER falls back to a stale higher-seq entry from a prior boot's ring write. |
| `secureboot_state`         | string | yes      | One of `"UNKNOWN"`, `"DISABLED"`, `"ENABLED"`, `"SETUP"`. `UNKNOWN` is emitted when the firmware Secure Boot state was unreadable; it is NEVER aliased to `DISABLED`. Classifier priority: `UNKNOWN > SETUP > ENABLED > DISABLED`. |
| `degraded_caps`            | array  | yes      | String names of every `BOOT_CAP_*` bit set in `boot_info.caps_degraded`. Empty array when no caps are degraded. |
| `degraded_subsystems`      | array  | yes      | String names of every kernel subsystem currently in degraded state (per the `kernel_subsys_t` enum). Empty when all subsystems are healthy. |
| `missing_capabilities`     | array  | yes      | Hardware capabilities the kernel expected but did not find on this boot: subset of `["TPM", "USB", "NVME", "STORAGE"]` (uppercase canonical names). Empty when all expected hardware was enumerated. |
| `perf_breaches`            | array  | yes      | Per-step boot-perf budget breaches; see **Per-entry shape: perf_breaches**. |
| `mat_wx_violations`        | array  | yes      | UEFI Memory Attributes Table entries classified as `MAT_CLASS_WX_VIOLATION` (writable + executable). Capped at 16 entries to bound the JSON; a sibling `mat_overflowed` boolean signals truncation. |
| `mat_overflowed`           | bool   | yes      | `true` when the MAT inventory contains more than 16 W^X violations and the array was capped. |
| `firmware_quirks_active`   | array  | yes      | String names of every firmware quirk active after `boot.conf firmware_quirk_disable=` suppression. Empty when no quirks fired. |
| `recent_boot_times_unix`   | array  | yes      | Up to 3 most-recent boots' Unix epoch timestamps, descending by `boot_seq`. Includes the current boot only when its Phase-3 mark was successfully committed AND is visible in the ring snapshot; otherwise the array reflects only prior boots. Each entry is `0` when that boot's RTC was unreadable (same `0` convention as `generated_at_utc`). Empty when the ring has no committed entries (first boot). |

## Per-entry shape: `perf_breaches[]`

Each entry is one boot-step that exceeded its budget classification:

```json
{
  "step": "smp_init",
  "elapsed_ms": 612,
  "budget_ms": 500,
  "severity": "SOFT"
}
```

| Field        | Type   | Description                                                            |
| ------------ | ------ | ---------------------------------------------------------------------- |
| `step`       | string | Step name (matches `boot_timing` step labels).                         |
| `elapsed_ms` | int    | Measured elapsed milliseconds for this step.                           |
| `budget_ms`  | int    | Soft / hard budget threshold this step was compared against.           |
| `severity`   | string | `"SOFT"` or `"HARD"` per `boot_perf_budget_classify`.                  |

## Per-entry shape: `mat_wx_violations[]`

```json
{
  "phys_base": "0x000000007f000000",
  "page_count": 16,
  "type": "WX_VIOLATION",
  "attr_hex": "0x000000000000000f"
}
```

| Field        | Type   | Description                                                            |
| ------------ | ------ | ---------------------------------------------------------------------- |
| `phys_base`  | string | Hex-encoded 64-bit physical base address (16 hex digits, lowercase, `0x` prefix). |
| `page_count` | int    | Number of 4 KiB pages covered by this descriptor (u64; never truncated). |
| `type`       | string | Derived MAT classification name: always `"WX_VIOLATION"` for entries in this array. Reserved for future expansion when other violation classes are added. |
| `attr_hex`   | string | Raw `EFI_MEMORY_*` attribute bitmask (hex-encoded u64). Diagnostic value; consumers that only care about the W^X violation can ignore it. |

## Failure modes

- **Buffer overflow during emission:** publisher logs `LOG_WARN` and
  skips the disk write entirely. The previous boot's file (if any)
  remains; consumers see stale data, not partial data.
- **Short write:** publisher re-opens the file with `VFS_O_TRUNC` and
  closes immediately so the on-disk file becomes 0 bytes. Consumers
  treat 0-byte / missing as "no boot health data this run."
- **PMM allocation failure** (8 KiB buffer): publisher logs `LOG_WARN`
  and returns; no file is written.

## Consumers

- `sysinfo.exe boot-health` (planned): pretty-prints the JSON for
  on-device triage when serial is unavailable.
- Boot-trend rolling regression file ([Boot Perf Trend File +
  Regression Detection](../../todo/01-boot-platform/TODO-29-boot-perf-health-observability.md#3-boot-perf-trend-file--regression-detection),
  SHIPPED): `X:\Perf\boot-trend.json`, written by
  `boot_trend_publish_json()` (`src/kernel/main/boot_trend.c`). It rolls
  a per-boot `{boot_seq, unix_time, phase_durations_ms}` entry and does
  NOT carry `recent_boot_times_unix` or `perf_breaches`; wire format:
  [`boot-trend-schema.md`](boot-trend-schema.md).
- CI regression gate (planned, owned by
  [TODO-28 boot validation and certification matrix](../../todo/01-boot-platform/TODO-28-boot-validation-certification-matrix.md)
  section 9, NOT by the trend section above): refuses a green build if
  any HARD perf breach or MAT W^X violation appears in the JSON.
- Bare-metal triage: read directly from `X:\Diag\boot-health.json` over
  the storage path when serial output is not accessible.
