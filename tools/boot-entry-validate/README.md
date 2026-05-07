# tools/boot-entry-validate

Host-side validator for the boot entry store (`\EFI\ImpossibleOS\bootentries.json`).

Schema spec: [`docs/boot/boot-entry-schema.md`](../../docs/boot/boot-entry-schema.md).
C constants: [`include/boot/boot_entries.h`](../../include/boot/boot_entries.h).

## Usage

```bash
# Validate a store
python3 tools/boot-entry-validate/validate.py resources/boot/bootentries-example.json

# Recompute the crc32 field in-place (useful when editing the sample)
python3 tools/boot-entry-validate/validate.py --emit-crc resources/boot/bootentries-example.json

# Run the 78-case mutator harness (canonical sample + 77 negative mutators)
python3 tools/boot-entry-validate/test_validate.py
```

Exit codes:

- `0` -- store is valid (or `--emit-crc` succeeded).
- `1` -- store fails validation. Reason is on stderr with `[FAIL]` prefix.
- `2` -- usage error (missing argument, file not readable).

## Checks performed

1. Top-level shape (`schema_version`, `crc32`, `entries`).
2. `schema_version == 1`.
3. `crc32` field formatted as `"0x"` + 8 hex digits.
4. File size <=16 KiB.
5. Entry count 1..64.
6. Per-entry envelope (`id`, `title`, `kind`, `flags`, `sort_key`, `machine_id`, `policy_tags`, `payload`).
7. `id` uniqueness within store.
8. `kind` is a known stable name OR (numeric in vendor range 100..199 / reserved >=200) which is
   skip-with-warn per the forward-compat policy. Numeric in stable range 0..99 but not a known
   value is rejected.
9. Per-kind required payload fields.
10. `kind: chainload` requires `flags` includes `trusted_chainload`.
11. `kind: network` requires `uri_scheme` in {http, https, tftp} and `asset_digest` is 64 lowercase hex.
12. `kind: safe` requires `safe_mode_subset` in {minimal, network, cmd}.
13. `kind: test` requires `test_suite` to be a known TEST_CAT_* category.
14. CRC-32 of canonical-form `entries` payload matches `crc32` field.

Unknown flag names produce a `[WARN]` line and are dropped (forward-compat). Unknown vendor-range
kinds skip the entry with a `[WARN]` line; the store as a whole remains valid.

## Sync with C header

This validator's constants mirror [`include/boot/boot_entries.h`](../../include/boot/boot_entries.h).
A drift detector (CI test) is filed as a follow-up; until then, edits to the C header MUST be
mirrored here in the same commit.

## Future integration

This tool will become the producer for idempotent first-install entry seeding (a later TODO
section). For now, it's read-only validation.
