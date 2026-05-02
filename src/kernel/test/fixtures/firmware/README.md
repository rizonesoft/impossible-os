# Firmware Inventory Test Fixtures

Static firmware-data fixtures consumed by `src/kernel/test/test_firmware_tables.c` (kernel-side, via `firmware_table_validate_one_for_test`) and by `tools/firmware-tables-decode` (host-side, via `scripts/test-firmware-decode.sh`).

## Files

| File | Purpose | Consumer |
|------|---------|----------|
| `firmware-tables-sample.json` | A schema_version=1 firmware-tables.json snapshot covering every typed sub-block (acpi/smbios/mat/rt_properties/esrt/apei/dbg2/wsmt). Used by the host-decoder regression test for byte-identical round-trip + pretty-print + schema-version reject. | `scripts/test-firmware-decode.sh` |

## In-tree synthetic fixtures (not in this directory)

The kernel-side validator tests in `test_firmware_tables.c` build their fixture blobs **inline** as `static uint8_t buf[N]` arrays inside each test function. This pattern keeps the test self-contained, avoids a separate fixture-loading mechanism, and dodges the WSL test-runtime constraint that forbids `vfs_open` calls from `test_*.c`. Existing inline fixtures cover:

- ACPI SDT (clean / checksum-fail / length below header / length mismatch / range unmapped / null phys_addr / oversized length)
- ACPI RSDP (v1 + v2)
- SMBIOS3 + SMBIOS2 entry points
- FPDT (oversized declared length capped)
- ESRT (8-field decoded helpers + brace-form GUID)
- DTB (minimal blob + bad magic + truncated structure block + unbalanced begin/end + multi-root + misnested memory/cpu)
- MAT (classify all 5 classes, NULL guard, range guard, count cap)
- RT Properties (mismatch counter range, supported-bit invariant)
- Conformance profile (OOR id, name non-empty, pc_contradiction arch-gate, omit-policy default-deny, level-vs-presence consistency)

If a future test needs a fixture that does not fit the "build inline" pattern (e.g. binary blobs over 4 KB, real-firmware captures from bare-metal labs), drop the binary into this directory and load it from a host-side helper in `scripts/`.

## Adding a new fixture

1. Drop the file into `src/kernel/test/fixtures/firmware/<name>.<ext>`.
2. Document its purpose + consumer in this README.
3. Wire the consumer (kernel test, host script, or sysinfo tool) to read the file via a path constant.
4. Add the fixture path to any `clean:` / `dist:` Makefile targets that scrub the build tree.
