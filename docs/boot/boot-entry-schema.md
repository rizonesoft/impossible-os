# Boot Entry Store Schema

> Canonical specification for `\EFI\ImpossibleOS\bootentries.json`. Authoritative for envelope
> layout, kind enum, flag bits, CRC-32 algorithm, schema-version policy, forward-compat rules,
> and the in-firmware fallback contract. Owned by the
> [Boot Entry File Format](../../todo/01-boot-platform/TODO-07-boot-entry-store-menu-policy.md#1-boot-entry-file-format)
> section of the boot entry store TODO.
>
> C header (single source of truth for kind values + flag bits + size caps):
> [`include/boot/boot_entries.h`](../../include/boot/boot_entries.h).
>
> Host validator: [`tools/boot-entry-validate/validate.py`](../../tools/boot-entry-validate/validate.py).
> Sample store: [`resources/boot/bootentries-example.json`](../../resources/boot/bootentries-example.json).

## 1. Overview

The boot entry store is the OS-owned counterpart to UEFI `Boot####` variables. Where firmware
`Boot####` controls "which Impossible OS boot loader runs", this store controls "which internal
entry the loader picks once running" -- a separate layer documented in the
[Boot Policy Merge Order](../../todo/01-boot-platform/TODO-07-boot-entry-store-menu-policy.md#3-boot-policy-merge-order)
section.

The store is a single JSON file at `\EFI\ImpossibleOS\bootentries.json` on the EFI System
Partition. The on-disk format is JSON; there is no INI alternative (dropped during design
review for YAGNI / parser-surface reasons).

## 2. Trust Model (two-tier)

The store's authority varies by boot mode:

| Boot mode                            | Store role          | Trust anchor                                  |
| ------------------------------------ | ------------------- | --------------------------------------------- |
| UKI + Secure Boot (UKI flag set)     | **Advisory only**   | Signed UKI image (kind / path / cmdline embedded) |
| Split-path with Secure Boot          | **Load-bearing**    | `BOOTX64.EFI` Secure Boot signature           |
| Split-path without Secure Boot       | **Load-bearing**    | None (existing project reality)               |

Under UKI mode, the bootloader uses the store for menu labels, ordering, and `hide_when_alone`,
but **never** for kind / path / cmdline selection -- those come from the signed UKI's embedded
sections. Disk-side path overrides under UKI are already rejected by
[`include/boot/uki_cmdline_check.h`](../../include/boot/uki_cmdline_check.h); this schema does
not relax that contract.

Under split-path, the CRC-32 in the envelope detects **corruption**, not adversary substitution.
An attacker with ESP write access can produce a valid-CRC store. Authentication of the store
proper (Ed25519 from the
[CNG crypto TODO](../../todo/09-desktop-shell/TODO-07-cng-crypto.md)) is tracked as a Branch B
follow-up; until then, the split-path threat model accepts the store as configuration, not
authority.

## 3. Envelope Structure

The store is a JSON object with three top-level keys:

```json
{
  "schema_version": 1,
  "crc32": "0xXXXXXXXX",
  "entries": [ /* array of entry objects */ ]
}
```

| Field            | Type    | Required | Notes                                                   |
| ---------------- | ------- | -------- | ------------------------------------------------------- |
| `schema_version` | integer | yes      | Must equal `1` for v1 readers; otherwise hard reject.   |
| `crc32`          | string  | yes      | `"0x"` + exactly 8 hex digits; covers `entries` payload |
| `entries`        | array   | yes      | 1..64 entry objects; total file <=16 KiB                |

**Key names are unique within an object, and spelled literally.** Both rules are hard rejects, enforced by the firmware parser (`BOOT_ENTRIES_REJECT_DUPLICATE_KEY`, `BOOT_ENTRIES_REJECT_ESCAPED_KEY`) and by the host validator's `strict_loads()`.

- **Unique.** RFC 8259 permits a repeated name and leaves the choice of which one wins to the implementation; `json.loads` keeps the last. This store cannot afford that latitude: the firmware parser's `entries` cap counter is per-occurrence while its output index spans occurrences, so a second `entries` array wrote past the fixed 64-slot output array, and the reset also disarmed the cross-occurrence duplicate-id check. The rule applies to every key at every level the firmware parses (root and entry objects), not only to the three known root keys, so that host and firmware accept the same set of stores.
- **Literal.** A key name must not use a JSON escape sequence: `"entries"` is not `entries`. The firmware compares key bytes raw, and it locates the CRC field by scanning for the literal bytes `"crc32"`, so an escaped spelling was never readable there while `json.loads` decoded it into the real key.
- Distinct unknown keys remain forward-compatible and are skipped, with **no limit on how many** an object may carry. Only a REPEAT is rejected.
- The firmware enforces uniqueness by rescanning the current object's already-parsed prefix, so there is no seen-key table and no key-count ceiling. A rescan that cannot complete (a store nested deeper than the scan budget) is a hard reject, not a silent pass: "the check could not run" must never read as "the check passed".
- **Value nesting is capped at 8 containers, counted from each value the firmware SKIPS wholesale** (an unrecognised top-level key, an unrecognised entry key, and `payload`), not from the root. `validate_value_depths()` in the host validator mirrors this exactly, and both sides carry a depth-8-accept / depth-9-reject test pair so they cannot drift apart. Values the firmware interprets itself (`flags`, `policy_tags`, `health_check_subset`) are shape-checked instead and never reach that budget.
- Both host writers emit key names literally (UTF-8, never `\uXXXX`) and re-parse their own serialized bytes before persisting: `bootcfg`'s canonical writer and the validator's `--emit-crc`. Validating the in-memory object is not enough, because these rules live in the serialized form. Both build the complete file in memory and replace the destination atomically through one shared implementation, so a store that fails validation -- or one that cannot be encoded at all -- leaves the existing store untouched rather than truncated. The temporary file is exclusively owned, so a concurrent run or a planted symlink cannot defeat that.
- The host validator applies both rules inside `payload` objects too, which the firmware envelope parser skips wholesale. That asymmetry is deliberate producer-side strictness (the host already validates payload contents), not a claim that the two parsers accept identical input.

Each entry object carries an envelope plus a per-kind payload:

```json
{
  "id":        "slot-a-normal",
  "title":     "Impossible OS (Slot A)",
  "kind":      "split",
  "flags":     ["active"],
  "timeout_override": 5,
  "sort_key":  "00-impossible-os-a",
  "machine_id": "11111111-2222-3333-4444-555555555555",
  "policy_tags": [],
  "payload":   { /* kind-specific fields */ }
}
```

### 3.1 Envelope fields

| Field              | Type            | Required | Constraints                                                 |
| ------------------ | --------------- | -------- | ----------------------------------------------------------- |
| `id`               | string          | yes      | kebab-case, 1..47 chars, unique within store                |
| `title`            | string          | yes      | 1..63 chars, UTF-8, ASCII recommended                       |
| `kind`             | string          | yes      | one of the kind names below; unknown -> skip-with-warn      |
| `flags`            | array of string | yes      | subset of {active, hidden, trusted_chainload, hide_when_alone, allow_editor} |
| `timeout_override` | integer         | no       | 0..600; if absent, use loader default                       |
| `sort_key`         | string          | yes      | sort string (BLS-style); shorter sorts earlier              |
| `machine_id`       | string          | yes      | RFC 4122 UUID textual form, OR empty string for the "match any machine" wildcard |
| `policy_tags`      | array of string | yes      | reserved for future policy filtering; may be empty          |
| `health_check_subset` | array of string | no   | optional; up to 8 names, each 1..23 printable-ASCII chars; restricts the post-boot health gate to the named checks (intersection with kernel registry). Absent / empty -> kernel runs the full default set. See [`boot-health.md`](boot-health.md). |
| `payload`          | object          | yes      | per-kind fields (see "Per-Kind Fields" below)               |

The `flags` array uses string names mapped to the bit values defined in
[`include/boot/boot_entries.h`](../../include/boot/boot_entries.h). Unknown flag names produce a
parse warning and are dropped (forward-compat -- newer flag names land without breaking older
loaders). Reserved bits 5..31 must be zero in v1.

### 3.2 Kind names and numeric values

| Kind name     | Numeric | Stable | Description                                              |
| ------------- | ------: | ------ | -------------------------------------------------------- |
| `split`       |       0 | yes    | kernel + initrd[] + cmdline + root                       |
| `uki`         |       1 | yes    | unified PE under `\EFI\Linux` or `\EFI\ImpossibleOS`     |
| `chainload`   |       2 | yes    | non-IPOS UEFI app (`LoadImage` / `StartImage`)           |
| `network`     |       3 | yes    | HTTP / TFTP target with sha256 digest                    |
| `resume`      |       4 | yes    | hibernation snapshot                                     |
| `recovery`    |       5 | yes    | recovery partition target                                |
| `installer`   |       6 | yes    | installer media role                                     |
| `safe`        |       7 | yes    | safe-mode entry                                          |
| `diagnostics` |       8 | yes    | verbose POST + extended boot logging                     |
| `test`        |       9 | yes    | `TEST_CAT_*` runner                                      |
| reserved      |  10..99 | future | reserved for future stable kinds                         |
| vendor        | 100..199| no     | vendor / experimental; skip-with-warn for unknown values |
| reserved      |   >=200 | no     | reserved future use; skip-with-warn                      |

Both string and numeric forms are accepted; the parser canonicalizes to the numeric value before
hashing into the per-kind handler table. For maximum forward-compat, producers SHOULD emit string
form (a value the producer doesn't know cannot be a name the parser does know).

## 4. Per-Kind Fields

The `payload` object's required + optional fields by kind:

### 4.1 `kind: split`

| Field      | Type            | Required | Notes                                                  |
| ---------- | --------------- | -------- | ------------------------------------------------------ |
| `kernel`   | string          | yes      | ESP-relative path, ASCII, <=255 chars; under one of the allowed prefixes; no `..` traversal |
| `initrd`   | array of string | no       | 0..8 ESP-relative paths; same prefix + traversal rules as `kernel` |
| `cmdline`  | string          | no       | ASCII; <=255 chars (`BOOT_ENTRIES_MAX_PATH_LEN`); empty string accepted |
| `root`     | string          | no       | slot id (`A` / `B`) or partition GUID; ASCII; no `..` |

**Allowed path prefixes** (kernel + initrd entries): `\EFI\ImpossibleOS\`, `\EFI\Linux\`, `\boot\`, or a single-segment `\<file>` (e.g. `\kernel.exe`). Anything else is rejected with `REJ_FIELD_VALUE`.

**Validator behavior:** the per-kind validator (`include/boot/boot_entry_kind.h`) requires `kernel` only; `cmdline` / `root` are optional but get ASCII / traversal checks when present. Unknown payload keys are tolerated for forward compatibility (vendor extension fields).

### 4.2 `kind: uki`

| Field       | Type    | Required | Notes                                                  |
| ----------- | ------- | -------- | ------------------------------------------------------ |
| `uki_path`  | string  | yes      | ESP-relative PE path under `\EFI\Linux` or `\EFI\ImpossibleOS` |
| `profile`   | integer | no       | 0..15 multi-profile UKI selector (per UAPI UKI spec)   |

NOTE: under UKI mode the store is advisory; selecting a `kind: uki` entry simply updates menu
labeling. The UKI image's embedded `.cmdline` / `.linux` / `.initrd` are the load-bearing
inputs. The validator (`validate_uki()`) accepts payload-absence (firmware launched
BOOTX64.UKI.efi which already located the kernel via PE sections) but enforces the schema
contract whenever a payload object IS present: `uki_path` is required, must be ASCII, must
start with `\EFI\Linux\` or `\EFI\ImpossibleOS\`, must not contain `..`; `profile` is parsed
as a non-negative integer 0..15 (decimals, signs, and out-of-range values reject). Disk-side
`kernel` / `cmdline` / `initrd` / `root` keys are forbidden in UKI payloads -- those would
be smuggled overrides on a Secure-Boot-signed image.

### 4.3 `kind: chainload`

| Field          | Type    | Required | Notes                                                  |
| -------------- | ------- | -------- | ------------------------------------------------------ |
| `efi_path`     | string  | yes      | path on the named device (target firmware syntax)      |
| `device_guid`  | string  | yes      | partition GUID hosting `efi_path`                      |

REQUIRED: entry's `flags` MUST include `trusted_chainload` when firmware Secure Boot is on. The parser does NOT enforce this -- the boot-policy filter does (see [`boot-policy.md`](boot-policy.md) section 3 and `BOOT_REJECT_REASON_PATH_ESCAPE`). Untrusted chainload entries under Secure Boot are demoted per-entry (other viable entries still get to boot) rather than failing the whole store.

### 4.4 `kind: network`

| Field          | Type    | Required | Notes                                                  |
| -------------- | ------- | -------- | ------------------------------------------------------ |
| `url`          | string  | yes      | http(s)://... or tftp://...                            |
| `uri_scheme`   | string  | yes      | `http` / `https` / `tftp`                              |
| `asset_digest` | string  | yes      | sha256 hex (64 chars) of the fetched payload           |

### 4.5 `kind: resume`

| Field            | Type   | Required | Notes                                                  |
| ---------------- | ------ | -------- | ------------------------------------------------------ |
| `snapshot_path`  | string | yes      | ESP-relative or partition-anchored path                |
| `snapshot_digest`| string | yes      | sha256 hex; must match hibernation metadata            |

### 4.6 `kind: recovery`

| Field                  | Type   | Required | Notes                                              |
| ---------------------- | ------ | -------- | -------------------------------------------------- |
| `recovery_partition_guid` | string | yes  | partition GUID hosting recovery image              |

### 4.7 `kind: installer`

| Field                  | Type   | Required | Notes                                              |
| ---------------------- | ------ | -------- | -------------------------------------------------- |
| `installer_image_guid` | string | yes      | partition GUID hosting installer image             |
| `media_role`           | string | yes      | `live` / `install` / `recovery-install`            |

### 4.8 `kind: safe`

| Field             | Type   | Required | Notes                                              |
| ----------------- | ------ | -------- | -------------------------------------------------- |
| `safe_mode_subset` | string | yes     | `minimal` / `network` / `cmd`                      |
| `kernel`           | string | yes     | ESP-relative path (typically same as default split)|

### 4.9 `kind: diagnostics`

| Field             | Type   | Required | Notes                                              |
| ----------------- | ------ | -------- | -------------------------------------------------- |
| `kernel`          | string | yes      | ESP-relative path                                  |
| `verbose_log`     | bool   | yes      | enable extended boot logging                       |

### 4.10 `kind: test`

| Field        | Type   | Required | Notes                                                   |
| ------------ | ------ | -------- | ------------------------------------------------------- |
| `kernel`     | string | yes      | ESP-relative path                                       |
| `test_suite` | string | yes      | `mm` / `fs` / `boot` / `ob` / `security` / `ipc` / `sched` / `abi` / `storage` / `exec` |

## 5. CRC-32 Algorithm

The header's `crc32` field MUST equal the IEEE 802.3 CRC-32 (polynomial `0xEDB88320`, initial
`0xFFFFFFFF`, final XOR `0xFFFFFFFF`) of the file bytes with the 8 hex digits inside the
`crc32` value zeroed.

### Algorithm

1. Locate the `crc32` field. The producer MUST emit it with the exact shape
   `"crc32": "0xHHHHHHHH"` where `HHHHHHHH` is 8 lowercase or uppercase hex digits.
   Whitespace between `"crc32"`, `:`, and the value is permitted (matches the regex
   `"crc32"\s*:\s*"0x[0-9a-fA-F]{8}"`).
2. Construct a temporary buffer equal to the file bytes, with the 8 hex digits replaced by
   `00000000` (no `0x` prefix change; only the 8 hex chars change).
3. Compute IEEE 802.3 CRC-32 over the temporary buffer.
4. Compare to the saved 8 hex digits, parsed as a hex-encoded uint32.

### Producer flow (validate.py --emit-crc, bootcfg)

**Every step happens in memory. The destination is touched exactly once, at the end, and only
after the bytes have passed every check.** The earlier recipe wrote the placeholder to the
destination and patched it in place; that overwrites a valid boot store before validation has
run, and an interruption or an encoding failure leaves the store truncated or holding a
placeholder with `crc32` of `0x00000000`.

1. Serialize the store to text with key names spelled literally (`ensure_ascii=False`) and
   `"crc32": "0x00000000"` as the placeholder (no whitespace difference between placeholder and
   final).
2. Encode to UTF-8. A store that cannot be encoded is rejected here, with the destination
   untouched.
3. Re-parse those exact bytes under the strict loader, and validate the store. The key rules
   live in the serialized form, so validating the in-memory object is not sufficient.
4. Compute CRC per the algorithm above and patch the 8 hex digits in the buffer, MSB-first
   uppercase hex. The buffer already contains `00000000`, so the patch makes the stored CRC
   match the CRC of the file with that field zeroed -- the file is self-verifying.
5. Replace the destination with the finished buffer through the shared atomic writer
   (exclusively owned same-directory temporary, fsync, `os.replace`, parent fsync). Atomic
   replacement is guaranteed; crash durability additionally depends on the directory flush,
   which is skipped only where the platform genuinely does not support it.

### Reference (Python)

```python
import re, zlib

_CRC_FIELD_RE = re.compile(rb'"crc32"\s*:\s*"0x([0-9a-fA-F]{8})"')


def compute_crc_from_file(raw: bytes) -> int:
    m = _CRC_FIELD_RE.search(raw)
    if m is None:
        raise ValueError('crc32 field not found in expected shape')
    offset = m.start(1)  # offset of the 8 hex chars
    zeroed = raw[:offset] + b"00000000" + raw[offset + 8:]
    return zlib.crc32(zeroed) & 0xFFFFFFFF
```

`zlib.crc32` uses IEEE 802.3 / 0xEDB88320 by default; the bootloader's parser ships its own
copy at [`src/kernel/fs/gpt.c`](../../src/kernel/fs/gpt.c) (`gpt_crc32`).

### Why byte-level, not canonical-form

This scheme is corruption-only integrity (per the two-tier trust model in Section 2: under
UKI Secure Boot the store is advisory only; under split-path the bootloader signature is the
trust anchor + CRC for corruption detection). Byte-level CRC is ~6x simpler in freestanding
C than canonical-form re-emission (no JSON re-emitter, no key sort, no string-escape
canonicalization). Producers (`validate.py --emit-crc`, future `bootcfg.exe`, future
bootloader-side writer if any) emit deterministic bytes; tampering or reformatting changes
the CRC. Authenticated stores (Ed25519 over file bytes) are tracked as a Branch B follow-up.

## 6. Schema Version Policy

`schema_version` is a hard-reject ONLY on **envelope-incompatible** bumps:

- Adding / removing / reordering envelope fields.
- Changing a field's type or semantics.
- Changing the CRC algorithm or its canonicalization.

The following changes do **not** bump `schema_version`:

- Adding a new stable kind in the `0..99` range.
- Adding a new optional payload field.
- Adding a new flag bit name.

When a parser sees `schema_version > BOOT_ENTRIES_SCHEMA_VERSION`, it rejects the store and
synthesizes the in-firmware fallback (see "Fallback Contract"). When it sees
`schema_version < BOOT_ENTRIES_SCHEMA_VERSION`, behavior is reserved for future minor-version
work; v1 readers reject `< 1` as malformed and accept `== 1`.

## 7. Forward-Compat Policy

The kind enum reserves three ranges:

| Range    | Class                  | Numeric-form unknown behavior  | String-form unknown behavior |
| -------- | ---------------------- | ------------------------------ | ---------------------------- |
| `0..99`  | stable named           | reject as malformed            | skip-with-warn               |
| `100..199` | vendor / experimental | skip-with-warn                | skip-with-warn               |
| `>=200`  | reserved future        | skip-with-warn                 | skip-with-warn               |

### Asymmetry rationale

The numeric and string forms intentionally differ for unknown values in the stable `0..99`
range. The numeric form `kind: 10` says **"this is a stable kind I expected you to know"** --
if the reader does not, that is a producer-side mistake (compile-time mismatch between writer
and reader, or a corrupted store) and the entry is rejected. The string form `kind: "future-stable"`
says **"this is a name I might know"** -- the reader cannot tell from the syntax whether it
refers to a future stable kind (>=10 in the canonical numbering) or a vendor extension whose
producer chose a string label, so the safe forward-compat behavior is to skip-with-warn.

This asymmetry is the reason producers SHOULD emit string form (paired with a pinned numeric
mapping in the C header for known values). New stable kinds that ship as a name in old tooling
become "skipped on old, recognized on new"; emitting them as numbers would brick old tooling.

Skip-with-warn produces a serial log line `boot-entries: skipped entry <id> (kind=<value> unknown, vendor/reserved range)` and continues parsing; the store as a whole remains valid.

If the **selected** entry (by precedence) is a skipped entry, the parser falls through to the next
priority entry per the policy merge precedence rules.

## 8. Fallback Contract

When the store is missing, oversize, CRC-mismatched, has unrecognized envelope schema_version,
or fails parser validation entirely, the bootloader synthesizes ONE in-firmware fallback entry
matching whatever path it currently loads:

| Bootloader state      | Synthesized entry                                                  |
| --------------------- | ------------------------------------------------------------------ |
| UKI fast path active  | `kind: uki`, `uki_path` = loaded UKI image path                    |
| Split-path active     | `kind: split`, `kernel` = `\EFI\ImpossibleOS\kernel.exe`, `cmdline` from `boot.conf` |

The synthesized entry has `id = "fallback"`, `title = "Impossible OS (fallback)"`,
`flags = ["active"]`, no per-entry `tries_left` counter (no rollback semantics in fallback mode).

`boot_info.selection_reason` is set to `ENTRY_STORE_INVALID` (or a more specific code per the
audit trail TODO) so user-mode tooling can surface the fallback condition.

No A/B reference today -- the slot-metadata TODO is not yet shipped. Once it lands, the A/B
integration TODO will widen the fallback to honor the active slot.

## 9. Sample Store

See [`resources/boot/bootentries-example.json`](../../resources/boot/bootentries-example.json).

## 10. Validator Usage

```bash
python3 tools/boot-entry-validate/validate.py resources/boot/bootentries-example.json
```

Exits 0 on success; non-zero on validation failure with a `[FAIL]` line on stderr naming the
failed check. See [`tools/boot-entry-validate/README.md`](../../tools/boot-entry-validate/README.md)
for the full list of checks.

## 11. Producer / Consumer Reference

| Producer / Consumer              | Role                                                       |
| -------------------------------- | ---------------------------------------------------------- |
| `tools/boot-entry-validate/`     | Host-side validator (Python), CI gate                      |
| `tools/boot-entry-validate/`     | Idempotent offline seed for installer + image build (later) |
| `bootcfg.exe`                    | Online + offline editor; consumes validator (later)        |
| `src/boot/uefi/bootx64.c`        | Bootloader parser; CRC verify; per-kind dispatch (later)   |
| `src/kernel/...`                 | Boot policy merge consumer of `boot_info.selected_entry_id` (later) |

> Menu UX consumers of these flags + kinds (indicator legend, hotkey behavior, `hide_when_alone` single-entry skip) are documented in [boot-menu.md](boot-menu.md).
