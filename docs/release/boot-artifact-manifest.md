<!-- docs: covers=todo/01-boot-platform/TODO-06-boot-media-image-installer-handoff.md,todo/15-installer-release/TODO-01-release-artifacts.md sources=scripts/release/build-manifest.sh,include/kernel/boot_info.h,tools/boot-info-manifest/dump-kernel.c,scripts/deploy/write-usb.sh,scripts/sign-efi.sh reviewed=2026-09-28 -->
# Boot Artifact Manifest Schema

> Schema owner: [Boot Artifact Matrix and Manifest Format](../../todo/01-boot-platform/TODO-06-boot-media-image-installer-handoff.md#1-boot-artifact-matrix-and-manifest-format). Production pipeline owner: [Disk Image, USB & Release Artifacts](../../todo/15-installer-release/TODO-01-release-artifacts.md). Bootloader-side verification owner: [Artifact Signing and Manifest Verification](../../todo/01-boot-platform/TODO-06-boot-media-image-installer-handoff.md#7-artifact-signing-and-manifest-verification). Offline inspector owner: [Offline Artifact Inspector](../../todo/01-boot-platform/TODO-06-boot-media-image-installer-handoff.md#8-offline-artifact-inspector).

This document is the canonical schema for the **boot artifact manifest** -- a JSON file every Impossible OS release artifact (raw / USB / VHD / VHDX / VDI / ISO / qcow2 / OVA / recovery / installer) ships alongside. The manifest pins the contents the bootloader must verify at load: file hashes, partition map, ABI version, Secure Boot status, media role, and an artifact UUID. The release-pipeline tooling produces it; the bootloader-side verifier consumes it; the offline inspector round-trips it.

## Supported artifact formats

| Format    | Extension     | Notes                                                            |
| --------- | ------------- | ---------------------------------------------------------------- |
| `raw`     | `.img`        | Sector-aligned raw disk image (default development output).       |
| `usb`     | `.img`        | Same as `raw`; written via `scripts/deploy/write-usb.sh`.         |
| `vhd`     | `.vhd`        | Hyper-V legacy fixed-size virtual hard disk.                      |
| `vhdx`    | `.vhdx`       | Hyper-V modern virtual hard disk.                                 |
| `vdi`     | `.vdi`        | VirtualBox virtual disk image.                                    |
| `iso`     | `.iso`        | Hybrid UEFI El Torito ISO (no BIOS boot record). Legacy BIOS boot is **unsupported**: the ISO carries no BIOS El Torito entry, so a BIOS-only host's firmware refuses to boot the medium. Adding BIOS boot support would belong to a future TODO under `01-boot-platform/`. |
| `qcow2`   | `.qcow2`      | QEMU native sparse format.                                        |
| `ova`     | `.ova`        | OVF appliance bundle (tar of OVF descriptor + virtual disks).     |
| `recovery`| `.img`        | Recovery image variant (boot media role marker = `recovery`).     |
| `installer`| `.iso`/`.img` | Installer media variant (boot media role marker = `installer`).   |

## Required fields

The bootloader verifier and packaging gate REJECT manifests missing any of the following top-level fields. Each field maps directly to a check the bootloader performs before kernel jump.

| Field                  | Type                  | Description                                                                                |
| ---------------------- | --------------------- | ------------------------------------------------------------------------------------------ |
| `manifest_version`     | unsigned integer      | Schema version of this manifest (1 today; bumped when this document changes).               |
| `artifact_format`      | string enum           | One of the supported formats above (`raw`, `usb`, `vhdx`, ...).                            |
| `artifact_uuid`        | string (UUID v5)      | Deterministic identifier (UUID v5 of `artifact_format \| bootloader_sha256 \| kernel_sha256 \| boot_info_version`); populated into `boot_info.boot_artifact_id` on load. Per-build randomness lives in a separate `build_id` field outside the byte-reproducible artifact body. |
| `boot_target`          | string                | Architecture + firmware tuple, e.g. `uefi-x86_64`.                                         |
| `boot_info_version`    | unsigned integer      | `BOOT_INFO_VERSION` of the actually-compiled binaries; sourced from `build/boot-info-abi.kernel.json` (generated from the built kernel mirror), NOT from `include/kernel/boot_info.h` directly, so stale-build / compile-time-override drift cannot lie. |
| `secure_boot_status`   | string enum           | `signed`, `unsigned`, or `unknown`. `signed` requires a sign-stamp produced atomically by `scripts/sign-efi.sh` (e.g. `build/.sign-fingerprint`); a missing or stale stamp implies `unsigned`. Bootloader cross-checks against UEFI variable on load. |
| `media_role`           | string enum           | One of `normal`, `installer`, `live`, `recovery`, `manufacturing`, `diagnostics`.          |
| `disk_guid`            | string (GUID, opt.)   | GPT disk identifier. **Optional in `manifest_version=1`** (no GPT parser tooling yet); **promoted to required for GPT-backed formats in v2** when the parser ships. Verified against on-disk GPT by the bootloader at load. |
| `sector_size`          | unsigned integer (opt.)| Physical sector size in bytes (typically 512 or 4096). Optional in v1; required in v2 for GPT-backed formats. |
| `total_sectors`        | unsigned integer (opt.)| Total disk size in sectors. Optional in v1; required in v2 for GPT-backed formats. |
| `partition_map[]`      | array of objects      | One entry per GPT partition; see Partition Map.                                            |
| `entries[]`            | array of objects      | Enumerated artifact entries (bootloader, kernel, boot_entries, blackbox, recovery_payloads); see Entries. |
| `bootloader_sha256`    | string (64-hex)       | Convenience denormalized sha256 of the bootloader entry. MUST match `entries[name=bootloader].sha256`. |
| `kernel_sha256`        | string (64-hex)       | Convenience denormalized sha256 of the kernel entry. MUST match `entries[name=kernel].sha256`.        |
| `toolchain_version`    | string (opt.)         | One-line concatenation of `clang --version | ld.lld --version | nasm -v` first lines. Captures the toolchain identity that produced the binaries. Optional in v1; consumers use it to verify the building host matches an audited toolchain. |
| `source_sha`           | string (40-hex, opt.) | `git rev-parse HEAD` of the source tree at build time, or the literal string `unknown` when git metadata is unavailable. |
| `manifest_seed`        | string (opt.)         | `<source_sha>\|<artifact_format>`. Same string the deterministic-disk-image producer feeds into UUID v5 partition GUID derivation; binding it into the manifest lets a verifier reconstruct the GUID values without re-running the producer. |
| `vm_image_metadata`    | object (opt.)         | VM-container layer description (VHD/VHDX/VDI/qcow2). Subfields: `format` (one of `vhd`, `vhdx`, `vdi`, `qcow2`), `subformat` (`dynamic` or `fixed`), `block_size_bytes` (uint64), `virtual_size_bytes` (uint64). Absent on raw / usb / iso manifests. When `artifact_format` is a VM-container format, `vm_image_metadata.format` MUST match it. |

The `bootloader_sha256` and `kernel_sha256` fields are denormalized so the verifier does not need to walk `entries[]` for the two most-common-checked artifacts. The packaging gate enforces that they match the corresponding `entries[]` row before producing the manifest.

## Partition Map

Each `partition_map[i]` object describes one logical partition the bootloader expects. The map's semantics depend on `artifact_format`:

- **GPT-backed disk formats** (`raw`, `usb`, `vhd`, `vhdx`, `vdi`, `qcow2`, `ova`, `installer`, `recovery`): `partition_map[]` carries the full ESP + BlackBox + IXFS layout (3 entries today). Mismatch against the on-disk GPT triggers `boot_fatal()` under `require_manifest=1`. The bootloader-side verifier owns the cross-check against the on-disk GPT (it is the only consumer with access to the raw disk at load time); the packaging gate validates structural shape only.
- **`iso` format**: `partition_map[]` carries a single `ESP` row describing the El Torito UEFI boot image embedded in the ISO9660 volume (the ISO has no GPT at the medium level). Bootloader-side GPT cross-check is **skipped** for ISO; the verifier instead reads the El Torito boot image's FAT directly. The packaging gate still validates the row's structural shape.

| Field                    | Type                | Description                                                       |
| ------------------------ | ------------------- | ----------------------------------------------------------------- |
| `index`                  | unsigned integer    | 1-based GPT partition index.                                      |
| `name`                   | string              | Free-form label (`ESP`, `BlackBox`, `IXFS-System`, ...).          |
| `type_guid`              | string (GUID)       | GPT partition type GUID per UEFI 2.10 Appendix A.                  |
| `unique_partition_guid`  | string (GUID, opt.) | GPT unique partition GUID (per-instance). Optional in v1; required in v2 for GPT-backed formats. Reproducible-build mode derives it from a manifest-pinned seed. |
| `first_lba`              | unsigned integer (opt.) | First sector of the partition (LBA). Optional in v1; required in v2. |
| `last_lba`               | unsigned integer (opt.) | Last sector of the partition (LBA). Optional in v1; required in v2. |
| `attributes`             | unsigned integer (opt.) | GPT partition attributes (UEFI 2.10 Table 5-3). Default 0.    |
| `size_mib`               | unsigned integer    | Partition size in mebibytes (denormalized convenience; equals `(last_lba - first_lba + 1) * sector_size / 1MiB` when LBA fields present). |
| `filesystem`             | string enum (opt.)  | `fat32`, `ixfs`, `ntfs`, or absent for raw partitions.             |
| `filesystem_uuid`        | string (opt.)       | FAT volume serial / IXFS volume UUID / NTFS volume serial.        |
| `filesystem_label`       | string (opt.)       | FAT volume label / IXFS volume name.                              |

## Entries

Each `entries[i]` object describes one boot-platform-relevant file inside the artifact. Required entries vary by `artifact_format`:

- All formats: `bootloader`, `kernel` (each appears exactly once with `optional: false`).
- ESP-bearing disk formats (`raw`, `usb`, `vhd`, `vhdx`, `vdi`, `qcow2`, `ova`, `installer`, `recovery`): also `boot_config` (the ESP `boot.conf`; both `verify-esp.sh --manifest` and `build-manifest.sh check` fail closed when this row is absent for these formats).
- `iso`: `boot_config` is recommended but not required at the manifest level (the El Torito UEFI image inside the ISO carries its own boot.conf path; cross-check is owned by [`§4 Hybrid ISO / El Torito UEFI Boot`](../../todo/01-boot-platform/TODO-06-boot-media-image-installer-handoff.md#4-hybrid-iso--el-torito-uefi-boot)).

Optional entries (included when the subsystem is shipped): `boot_entries`, `blackbox_skeleton`, `recovery_payloads`.

| Field          | Type                | Description                                                       |
| -------------- | ------------------- | ----------------------------------------------------------------- |
| `name`         | string enum         | One of: `bootloader`, `kernel`, `boot_config`, `boot_entries`, `blackbox_skeleton`, `recovery_payloads`. |
| `path`         | string              | Path of the file inside the artifact (e.g. `\EFI\BOOT\BOOTX64.EFI`). |
| `sha256`       | string (64-hex)     | sha256 of the file contents.                                       |
| `size_bytes`   | unsigned integer    | File size in bytes.                                               |
| `optional`     | boolean             | `true` if the entry is conditionally present (BlackBox, recovery); `false` for required entries. |

## Failure modes

The packaging gate (`scripts/release/build-manifest.sh check <manifest>`) is **structural-only** -- it validates the JSON shape against this schema without access to the build tree. It exits non-zero on any of:

- A required field above is missing from the JSON.
- `bootloader_sha256` or `kernel_sha256` is empty or not 64 hex chars.
- `partition_map[]` is empty.
- `entries[]` is missing the required `bootloader` or `kernel` row, or (for ESP-bearing formats) the required `boot_config` row, or a non-optional row carries an unknown `name`.
- An enum field (`artifact_format`, `secure_boot_status`, `media_role`, `filesystem`) carries a value outside its allowed set.
- An integer field falls outside its documented range, or a UUID/GUID field violates its 8-4-4-4-12 form.
- `artifact_uuid` does not equal the deterministic UUID v5 reconstructed from the on-disk seed (`format|bootloader_sha256|kernel_sha256|boot_info_version`).
- A cross-field invariant is violated (e.g. `artifact_format=installer` with `media_role!=installer`).

Stderr message names the failed field by name, prefixed with `[ERROR]`.

The kernel/bootloader-vs-`boot_info_version` semantic binding is enforced upstream by **build mode**, not by check: `build` refuses to emit a manifest when `build/boot-info-abi.kernel.json` (the binary-derived ABI mirror produced by `tools/boot-info-manifest/dump-kernel`) is older than either `BOOTX64.EFI` or `kernel.exe`. That staleness assertion ties `boot_info_version` to the exact artifact pair being hashed at build time. The bootloader-side verifier closes the loop at load time by re-checking the manifest against the running ABI version in `boot_info`.

## Bootloader-side verification

Owned by [Artifact Signing and Manifest Verification](../../todo/01-boot-platform/TODO-06-boot-media-image-installer-handoff.md#7-artifact-signing-and-manifest-verification). When `boot.conf:require_manifest=1`, the bootloader:

1. Reads `\IPOS\manifest.json` from the ESP (or `/IPOS/manifest.json` from an ISO).
2. Verifies the detached signature `manifest.json.sig` against the release public key.
3. Cross-checks every `entries[]` sha256 against the on-disk file.
4. Cross-checks `partition_map[]` against the GPT (GPT-backed formats only; skipped for `iso`).
5. Populates `boot_info.boot_artifact_id` and `boot_info.boot_manifest_digest` from the manifest before kernel jump.

## Schema version policy

`manifest_version` is bumped when:

- A required field is added, removed, or renamed.
- An enum value is added to `artifact_format`, `secure_boot_status`, or `media_role`.
- The semantic meaning of any field changes (e.g. `boot_info_version` becomes a string).

`manifest_version` is **not** bumped when:

- A new optional `entries[]` row name is added (the verifier skips unknown optional entries).
- A new **optional top-level field** is added with documented ignore semantics (older v1 consumers that don't recognize the field MUST skip it without erroring; the field carries no required-by-default consumer contract). Examples: `toolchain_version`, `source_sha`, `manifest_seed`, `vm_image_metadata`. Adding such a field becomes a `manifest_version` bump only when the new field gains a required-by-default consumer contract.
- Documentation prose is rewritten without changing the on-disk shape.

The bootloader verifier handles `manifest_version > 1` by refusing the boot under `require_manifest=1` (forward-incompatible). Backward-compatible reads of `manifest_version=1` from a `manifest_version=2`-aware bootloader land via explicit migration code at the bump time.

## Example (manifest_version=1, raw image)

```json
{
  "manifest_version": 1,
  "artifact_format": "raw",
  "artifact_uuid": "12345678-1234-4abc-9def-0123456789ab",
  "boot_target": "uefi-x86_64",
  "boot_info_version": 16,
  "secure_boot_status": "unsigned",
  "media_role": "normal",
  "partition_map": [
    { "index": 1, "name": "ESP", "type_guid": "C12A7328-F81F-11D2-BA4B-00A0C93EC93B", "size_mib": 64, "filesystem": "fat32" },
    { "index": 2, "name": "BlackBox", "type_guid": "00000000-0000-0000-0000-000000000000", "size_mib": 128, "filesystem": "fat32" },
    { "index": 3, "name": "IXFS-System", "type_guid": "00000000-0000-0000-0000-000000000000", "size_mib": 4096, "filesystem": "ixfs" }
  ],
  "entries": [
    { "name": "bootloader",  "path": "\\EFI\\BOOT\\BOOTX64.EFI",            "sha256": "<64-hex>", "size_bytes": 124928,  "optional": false },
    { "name": "kernel",      "path": "\\boot\\kernel.exe",                  "sha256": "<64-hex>", "size_bytes": 5242880, "optional": false },
    { "name": "boot_config", "path": "\\EFI\\ImpossibleOS\\boot.conf",      "sha256": "<64-hex>", "size_bytes": 512,     "optional": false }
  ],
  "bootloader_sha256": "<64-hex>",
  "kernel_sha256": "<64-hex>"
}
```
