<!-- docs: covers=todo/01-boot-platform/TODO-29-boot-perf-health-observability.md -->
# `boot-trend.json` Wire Format (schema_version 1)

> Canonical wire format for `X:\Perf\boot-trend.json`. Single source of
> truth for the kernel writer (`boot_trend_publish_json` in
> `src/kernel/main/boot_trend.c`), the future CI regression gate, and any
> trend-analysis tooling that reads the rolling 16-boot ring.

## Versioning

- Top-level `"schema_version"` (integer, current value `1`).
- Bumped when (a) a field is removed, (b) a field's type changes, (c) the
  meaning of a field changes. Adding new optional per-entry fields is
  **not** a bump.
- Schema validation is **top-level only**. `schema_version != 1` or `boots`
  not an array -> the kernel renames the file to
  `boot-trend.json.corrupt-<seq>` and starts a fresh v1 file. Per-entry
  fields are read tolerantly: missing or non-numeric values are skipped,
  not promoted to file-level corruption.

## Atomicity

The publisher writes the new document to `boot-trend.json.tmp`, then calls
`vfs_rename_ex(... VFS_RENAME_REPLACE_EXISTING)` (the [VFS Rename
Replace-Existing](../../todo/05-storage-filesystems/TODO-04-fat32-hardening-vfs-semantics.md#16-vfs-rename-replace-existing----atomic-temp-file-durable-write-primitive)
primitive) to swap it into place. Mid-write failure leaves the prior
`boot-trend.json` untouched. Short-write or rename failure leaves `.tmp`
on disk for the next boot to overwrite or for offline triage.

## Top-level keys

| Key              | Type   | Required | Description                                                  |
| ---------------- | ------ | :------: | ------------------------------------------------------------ |
| `schema_version` | int    | yes      | Wire-format version (currently `1`).                         |
| `boots`          | array  | yes      | Up to 16 per-boot entries, newest at index 0, oldest at 15.  |

## Per-entry shape: `boots[]`

```json
{
  "boot_seq": 7,
  "unix_time": 1714771200,
  "phase_durations_ms": {
    "PMM": 8,
    "SMBIOS": 80,
    "MOUSE": 1119
  }
}
```

| Field                | Type   | Description                                                                                  |
| -------------------- | ------ | -------------------------------------------------------------------------------------------- |
| `boot_seq`           | int    | Monotonic boot sequence committed by `boot_history_kernel_mark_phase3()`. `0` if unavailable. |
| `unix_time`          | int    | Unix epoch seconds for that boot. `0` if RTC was unreadable (same convention as `boot-health.json`). |
| `phase_durations_ms` | object | Map of step name -> elapsed ms for that phase. Keys match the live kernel's `boot_timing` step labels. |

Phases present in the running kernel but absent from an older boot entry
are simply absent from that entry's `phase_durations_ms` object. Phases
dropped from the kernel are not re-emitted on the next rewrite (the trend
file tracks the running binary's phase namespace).

## Regression detection

Once the ring carries at least 6 entries, the publisher computes per-phase
3-run medians:

- **Newest window**: median over `boots[0..2].phase_durations_ms[name]` --
  includes the current boot just prepended.
- **Prior window**: median over `boots[3..5].phase_durations_ms[name]` --
  the immediately-prior 3 boots.

Growth percentage is `(newest - prior) * 100 / prior` (integer truncation,
clamped to 0 when newest <= prior or prior == 0). When growth strictly
exceeds **15%**, the kernel emits:

```
[WARN] BOOT-TREND: <phase> 3-run median grew <pct>% (<old>ms -> <new>ms)
```

A phase that exists in only one of the two windows (newer step name,
renamed phase) is silently skipped -- no false-positive warns from
namespace evolution.

## Failure modes

- **Existing file too large** (>= 16 KiB minus a NUL): publisher logs
  `LOG_WARN` and treats as empty (writes a fresh single-entry file).
- **Existing file unparseable**: quarantined to `.corrupt-<seq>`, fresh
  write proceeds.
- **PMM allocation failure** for either of the two 16 KiB buffers
  (separate input + output allocations): `LOG_WARN`, no write; any
  successfully-allocated buffer is freed.
- **Buffer overflow during emission**: `jb_truncated()` fail-closed;
  `LOG_WARN`, no write.
- **Short write to `.tmp`**: `.tmp` deleted, `LOG_WARN`, no rename
  attempted; prior `boot-trend.json` remains untouched.
- **Rename failure**: `.tmp` remains on disk; `LOG_WARN`. Next boot
  overwrites `.tmp` and retries.

## Consumers

- CI regression gate (planned, owned by the boot-validation certification
  matrix): reads the trend file, fails the release pipeline when any
  phase shows the WARN-class growth.
- Local triage: `cJSON_Parse` round-trip from `X:\Perf\boot-trend.json`;
  the `boots[0..2]` entries are the most actionable for "did this last
  commit slow something down."
- `sysinfo.exe boot-trend` (planned): pretty-prints the per-phase trend.
