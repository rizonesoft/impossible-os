<!-- docs: covers=todo/01-boot-platform/TODO-07-boot-entry-store-menu-policy.md -->
# Boot Policy Audit Schema

> Canonical doc for the policy audit trail feature owned by [TODO-07 boot-entry-store-menu-policy](../../todo/01-boot-platform/TODO-07-boot-entry-store-menu-policy.md). Two streams:
>
> 1. **`X:\Boot\history.jsonl`** -- per-boot policy decision audit, written by the kernel post-mount on every boot.
> 2. **`X:\Boot\mutations.jsonl`** -- per-mutation entry-store change log, written by `bootcfg.py` (offline) and the future live-boot `bootcfg.exe` user-mode binary.
>
> Plus the **`ImpossibleOS-BootSticky`** UEFI variable: a single 256-byte cross-boot trigger record. NVRAM is exceptional-only; per-boot history goes to BlackBox.

## Why disk-first?

`uapi-group.org/specifications/specs/boot_loader_specification` rejects EFI variables for high-frequency state for two reasons:

1. **Wear.** Many UEFI implementations store NVRAM in serial flash. Per-boot writes wear the chip out within a few thousand boots on cheap consumer hardware; NVRAM exhaustion bricks the system at the firmware level.
2. **Quota.** UEFI 2.10 lets firmware impose per-vendor variable quotas. Once exhausted, `SetVariable` returns `EFI_OUT_OF_RESOURCES` and silent data loss begins.

BlackBox JSONL has neither problem. The only NVRAM bits left after this trade-off are **cross-boot triggers** that need to survive a reset before the BlackBox partition is mountable -- a single 256-byte sticky record large enough for the trigger flags, a monotonic boot counter, and per-record CRC.

## `history.jsonl` -- Per-boot policy audit

One JSON object per line, NUL-terminated by `\n`. Every record carries `schema` so consumers can branch on version. Append-only with 4 MiB rotation: when the file size + the next record's bytes would exceed `BOOT_AUDIT_ROTATE_THRESHOLD = 4 MiB`, the kernel renames `history.jsonl` -> `history.1.jsonl` (overwriting any prior rotation) and starts fresh. One generation only; older rotations are dropped to bound disk usage.

> **Known limitation (rotation):** the kernel-side FAT32 driver's rename path does not currently rewrite LFN chains, only the SFN field, and the rename-side SFN derivation differs from the create-side derivation for LFN-required names like `history.jsonl`. As a result, the threshold-triggered rotation falls through to the bounded-spillover branch (append past 4 MiB, never silent loss). The 4 MiB cap is best-effort until the FAT32 LFN rename fix lands; tracked under [Boot Entry Tests](../../todo/01-boot-platform/TODO-07-boot-entry-store-menu-policy.md#17-boot-entry-tests) as a deferred harness. The same limitation affects every existing BlackBox writer (`boot-profile.log`, `postcode.log`, etc.) -- it is a driver-wide concern, not §12-specific.

### Schema version 1

```json
{
  "schema": 1,
  "ts": 1715299200,
  "boot_seq": 42,
  "event": "WATCHDOG_ROLLBACK",
  "event_code": 4,
  "selection_reason": 4,
  "selection_reason_name": "WATCHDOG_ROLLBACK",
  "selected_entry": "slot-a",
  "audit_degraded": 0,
  "sticky": {
    "present": 1,
    "recovery_trigger": 0,
    "watchdog_rollback_request": 1,
    "last_outcome": 0,
    "audit_degraded_last_boot": 0,
    "last_event_code": 1,
    "last_event_name": "NORMAL",
    "last_boot_seq": 41,
    "consumed_trigger_seq": 39
  },
  "rejected": [
    {"id": "slot-b", "reason": 5, "reason_name": "TRIES_EXHAUSTED"}
  ],
  "rejected_overflow": 0,
  "boot_info_version": 20
}
```

### Field reference

| Field | Type | Meaning |
| --- | --- | --- |
| `schema` | u32 | Pinned to `BOOT_AUDIT_JSONL_SCHEMA_VERSION = 1`. Bump only on incompatible wire changes. |
| `ts` | u64 | Wall-clock unix seconds at publish time. `0` = wall clock not available. |
| `boot_seq` | u32 | Monotonic boot counter sourced from the BlackBox-side dual-file counter (`X:\Boot\sequence` + `sequence.new`). Each boot writes `prev + 1` (saturating at `UINT32_MAX`); reader takes max-of-both. `sticky.last_boot_seq` is a previous-record diagnostic / ack-context field, NOT the seq source. |
| `event` | string | Symbolic name from `boot_audit_event_code` (`NORMAL`, `WATCHDOG_ROLLBACK`, `RECOVERY`, etc.). |
| `event_code` | u16 | Numeric value from `boot_audit_event_code`. Pinned for forward compat. |
| `selection_reason` | u32 | Numeric `boot_selection_reason` from the policy ladder. |
| `selection_reason_name` | string | Symbolic name (`STORE_DEFAULT`, `WATCHDOG_ROLLBACK`, `FALLBACK_STORE_INVALID`, ...). |
| `selected_entry` | string | NUL-terminated kebab-case id of the chosen entry. Empty when `selection_reason = FALLBACK_STORE_INVALID`. |
| `audit_degraded` | 0/1 | `1` when the bootloader could not trust the sticky read (missing/wrong attrs/CRC). |
| `sticky.present` | 0/1 | `1` when a valid sticky record was read this boot. |
| `sticky.recovery_trigger` | 0/1 | `1` if recovery boot was requested across the reset. |
| `sticky.watchdog_rollback_request` | 0/1 | `1` if watchdog rollback was requested across the reset. |
| `sticky.last_outcome` | 0/1 | `1` if the previous boot reached mark-good. |
| `sticky.audit_degraded_last_boot` | 0/1 | `1` if the previous boot's NVRAM/BlackBox path was degraded. |
| `sticky.last_event_code` | u16 | `boot_audit_event_code` from previous boot. |
| `sticky.last_event_name` | string | Symbolic name for the same value. |
| `sticky.last_boot_seq` | u32 | Previous boot's `boot_seq`. |
| `sticky.consumed_trigger_seq` | u32 | `boot_seq` of the last successful trigger ack. Detects torn ack windows. |
| `rejected[]` | array | Per-entry policy-layer rejects. Each row: `id`, numeric `reason`, symbolic `reason_name`. |
| `rejected_overflow` | 0/1 | `1` when more than 64 entries were filtered out. |
| `boot_info_version` | u32 | The `BOOT_INFO_VERSION` the producer compiled against. |

### Forward-compat rules

- Consumers MUST tolerate unknown fields (do not crash on additions).
- Consumers MAY treat unknown `event_code` / `reason` integers as the symbolic name `"UNKNOWN"`.
- The line is RFC 8259 JSON; strings are escaped via `jb_str()` (the kernel JSON builder).

### `X:\Boot\sequence` + `X:\Boot\sequence.new` -- monotonic boot counter (dual-file)

Two small ASCII-decimal files (each max 16 bytes including newline) holding the most recent `boot_seq` value. Read at the start of `boot_audit_publish()` as `max(canon, shadow)`; the new value (`prev + 1`, saturating at `UINT32_MAX`) is **written to both files** before the JSONL line is composed. As long as **at least one** of the two writes succeeds, the counter is durably advanced -- the reader's `max` operation recovers the new value even when one file ends up empty or corrupted from a partial write. Sequence persistence failure (BOTH files rejected) is treated as publish failure: the JSONL append is skipped so a duplicate `boot_seq` cannot land. Skipped seqs are tolerable; duplicate seqs would violate the schema's monotonic ordering claim.

The dual-file pattern replaces the more conventional `write-temp + atomic-rename` shape because the kernel-side FAT32 driver does not safely rewrite LFN chains during rename (only the SFN field is updated, leaving the LFN chain encoding the old name). Dual write + max-on-read is uniformly safe: any single-file partial-write at most corrupts one file, and the other still carries the true value.

These files live on the BlackBox partition (NOT NVRAM) because the BlackBox doctrine accepts per-boot disk writes; the sticky NVRAM var stays exceptional-only.

## `mutations.jsonl` -- Per-entry-store mutation audit

One JSON object per line. Written by every mutating subcommand of `bootcfg.py` (and the deferred live-boot `bootcfg.exe`) when `--mutation-log <path>` is supplied. Same 4 MiB rotation contract as `history.jsonl`.

### Schema version 1

```json
{
  "schema": 1,
  "ts": 1715299200,
  "kind": "set-default",
  "target_id": "slot-b",
  "prior_crc": "0xDD4F8DBD",
  "new_crc": "0x12345678",
  "requester": "bootcfg.py user=root host=ci-01",
  "note": "sort_key='00-slot-b'"
}
```

### Field reference

| Field | Type | Meaning |
| --- | --- | --- |
| `schema` | u32 | Pinned to `BOOT_AUDIT_JSONL_SCHEMA_VERSION = 1`. |
| `ts` | u64 | Wall-clock unix seconds at write time. |
| `kind` | string | One of `add`, `remove`, `set-default`, `emit-seed`. New mutating subcommands add new values here. |
| `target_id` | string | The entry id affected (or `"default"` for `emit-seed` -- a v1-stable label for the store-level seed action, NOT an entry id; the 3-entry seed contains slot-a / slot-b / recovery). |
| `prior_crc` | string \| null | Pre-mutation `bootentries.json` CRC, hex `"0xNNNNNNNN"`. `null` on fresh `emit-seed` (no prior file). `"INVALID"` if the prior file failed validation. |
| `new_crc` | string | Post-mutation CRC. |
| `requester` | string | `bootcfg.py user=<user> host=<host>` (offline). Live-boot binary will use a process-id form. |
| `note` | string \| absent | Optional mutation-specific detail (`set-default` records the assigned sort_key). |

### Forward-compat rules

- Same as `history.jsonl` -- consumers tolerate unknown fields.
- `prior_crc` is nullable; `new_crc` is always present.
- Failed mutations do NOT emit a record; the audit trail is post-success only.

## NVRAM sticky record (`ImpossibleOS-BootSticky`)

Single 256-byte UEFI variable, GUID `IMPOSSIBLE_OS_VENDOR_GUID = {6F35D3A4-C0E6-4A82-B5D8-7C9D2E4F8A13}`, attrs = `NV | BS | RT`.

### Two-phase ack

1. Bootloader **reads** the variable and surfaces the bits in `boot_info` (no NVRAM writes).
2. Kernel **acks** consumed triggers (clears the bits) AFTER `boot_audit_publish()` writes the BlackBox JSONL line successfully.

A reset between phases leaves the trigger pending. That is the correct sticky semantic -- a recovery request must persist across crashes until the recovery boot durably records its outcome.

### Byte layout (schema_version=1)

| Offset | Size | Field | Purpose |
| --- | --- | --- | --- |
| 0 | 4 | `magic` | `'BSST'` = `0x54535342` |
| 4 | 2 | `version` | `BOOT_STICKY_RECORD_VERSION = 1` |
| 6 | 2 | `size` | `BOOT_STICKY_VAR_SIZE = 256` |
| 8 | 1 | `recovery_trigger` | 1 if recovery boot requested |
| 9 | 1 | `watchdog_rollback_request` | 1 if watchdog rollback requested |
| 10 | 1 | `last_outcome` | 1 if previous boot reached mark-good |
| 11 | 1 | `audit_degraded_last_boot` | 1 if previous boot's NVRAM/BlackBox path was degraded |
| 12 | 2 | `last_event_code` | `enum boot_audit_event_code` from previous boot |
| 14 | 2 | `_reserved_a` | 0 |
| 16 | 4 | `last_boot_seq` | monotonic boot counter |
| 20 | 4 | `consumed_trigger_seq` | boot_seq of last successful trigger ack |
| 24 | 228 | `_reserved_b[228]` | zero-fill; future fields shrink this |
| 252 | 4 | `crc32` | CRC-32/IEEE-802.3 over bytes [0..251] |

The kernel-side ABI lives at [`include/boot/boot_audit_codes.h`](../../include/boot/boot_audit_codes.h); offsets are pinned with `_Static_assert`s.

### Validation

The bootloader's sticky reader treats any of the following as "absent + audit_degraded=1":

- RuntimeServices unavailable
- `GetVariable` returned an error other than `EFI_NOT_FOUND` (the latter is "first boot", not a degradation)
- Returned `size != 256`
- Returned `attrs != (NV | BS | RT)`
- `magic`, `version`, `size`, or `crc32` fail the validator

Bootloader does NOT delete-then-create on bad data (unlike `boot_history.c` which repairs its 8-entry ring). Leaving a corrupted record in place lets the kernel decide whether to repair after a successful publish, and avoids the failure mode where a corrupt record carrying `recovery_trigger=1` gets silently zeroed before the ladder considers it.

### Failure mode: NVRAM write failed (audit_degraded)

When the kernel ack fails (UEFI `SetVariable` returned an error -- quota exhausted, locked, hardware fault):

1. Boot continues normally; the trigger persists.
2. The **next** boot's bootloader reads the same trigger again.
3. If the next boot's BlackBox publish also fails to ack, the cycle continues -- no data loss, just repeated "trigger consumed" events in `history.jsonl`.
4. The disk-side trail captures the chain of degradation via `audit_degraded` and `audit_degraded_last_boot`, so an operator examining `history.jsonl` can see the NVRAM path stuck.

Boot is never blocked.

## Cross-references

- [`include/boot/boot_audit_codes.h`](../../include/boot/boot_audit_codes.h) -- canonical enums, struct layout, helpers.
- [`src/boot/uefi/boot_sticky.c`](../../src/boot/uefi/boot_sticky.c) -- bootloader-side reader (read-only).
- [`src/kernel/main/boot_audit.c`](../../src/kernel/main/boot_audit.c) -- kernel-side JSONL publisher + ack writer.
- [`tools/bootcfg/bootcfg.py`](../../tools/bootcfg/bootcfg.py) -- offline mutation log producer.
- [`docs/boot/boot-policy.md`](boot-policy.md) -- policy ladder + selection_reason enum.
- [`docs/boot/boot-info-fields.md`](boot-info-fields.md) -- v20 audit surface field reference.
- [Recovery UI](../../todo/01-boot-platform/TODO-22-recovery-partition.md#7-recovery-ui) -- consumes `history.jsonl` for the "what triggered this boot" panel.
- [Boot diagnostics page](../../todo/01-boot-platform/TODO-14-boot-diagnostics.md) -- consumes both JSONL streams for the diagnostics dashboard.
