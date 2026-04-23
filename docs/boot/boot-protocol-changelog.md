# Boot Protocol Changelog

> History of every `BOOT_INFO_VERSION` bump. Each row links to the
> owning TODO section and the commit that landed the bump, so anyone
> diffing `struct boot_info` across versions can jump straight to the
> design rationale.
>
> Invariant: **every version bump MUST land with a changelog row in
> this file and a TODO section that names the new fields**. Adding a
> field without bumping the version is an ABI-drift bug; bumping
> without documenting it leaves the next maintainer to guess.
>
> See also:
>
> - [`boot-protocol.md`](boot-protocol.md): living contract (layout, lifecycle, pointer validity, minimum handoff).
> - [`boot-info-fields.md`](boot-info-fields.md): per-field ownership matrix.

## Bump policy

Bump `BOOT_INFO_VERSION` whenever any of the following happens:

- A field is **added**, **removed**, or **reordered** inside `struct boot_info` or any nested struct.
- A field **changes type or size** (`uint32_t` to `uint64_t`, `UINT8[16]` to `UINT8[32]`, etc.).
- A field's **semantics change** in a way that a consumer would read the same bytes differently (e.g., reinterpreting a pad as a new flag).

Do NOT bump for:

- Pure documentation edits (comment rewrites, new examples in `boot-protocol.md`).
- Renaming a field without touching layout (though this is strongly discouraged because it breaks grep).
- Filling a previously-reserved pad slot whose offset and size were already pinned (if the pad was advertised as reserved-for-future, consumers know not to rely on zero).

Both halves of the ABI (the kernel header at `include/kernel/boot_info.h` and the bootloader mirror at `src/boot/uefi/boot_info_mirror.h`) update the version macro in the SAME commit as the struct change. The [Generated ABI Manifest](../../todo/01-boot-platform/TODO-01-boot-protocol-abi-handoff.md#2-generated-abi-manifest-and-offset-fingerprint) build step catches mismatches before the image boots.

## Versions

### v6 (current) -- Typed payload descriptor array

- **Commit**: [`01991083`](https://github.com/rizonesoft/impossible-os/commit/01991083) "boot: add typed payload descriptors"
- **TODO**: [Optional Payload Descriptor Array](../../todo/01-boot-platform/TODO-01-boot-protocol-abi-handoff.md#4-optional-payload-descriptor-array)
- **Fields added**: `payload_descriptors[32]`, `payload_count`, `payload_overflow`, `payload_total_bytes`.
- **Manifest**: 225 fields, `struct_size = 23696` bytes.
- **Kernel canonical SHA-256**: `eb5f035e21f5...` (produced by `tools/boot-info-manifest/dump-kernel.c`; kernel and mirror must agree).
- **Why**: establish the typed payload ABI that the [Module and initrd Handoff Contract](../../todo/01-boot-platform/TODO-01-boot-protocol-abi-handoff.md#5-module-and-initrd-handoff-contract) fills in (module, initrd, recovery_image) and that the [Handoff Memory Ownership section](../../todo/01-boot-platform/TODO-01-boot-protocol-abi-handoff.md#6-handoff-memory-ownership-and-pmm-reservation-table) reserves from PMM reclaim. Post-v6 hardening (2026-04-23) tightened the NONE-empty-slot invariant so the validator rejects descriptors with `type=BOOT_PAYLOAD_NONE` that carry non-zero fields; this is a validator tightening, not a layout change, so no version bump.

### v5 -- Removable media detection

- **Commit**: [`e4bd76be`](https://github.com/rizonesoft/impossible-os/commit/e4bd76be) "boot: detect removable media via EFI_BLOCK_IO_PROTOCOL"
- **TODO**: [UEFI Hardening + Secure Boot](../../todo/01-boot-platform/TODO-02-uefi-hardening-secureboot.md) (boot media classification work).
- **Fields added**: removable-media / boot partition classification flags in the boot_device area.
- **Why**: downstream consumers (recovery path, default drive selection) need to distinguish removable from fixed boot media before mounting.

### v4 -- Boot partition GUID

- **Commit**: [`d125a47c`](https://github.com/rizonesoft/impossible-os/commit/d125a47c) "boot: extract and validate boot partition GUID from device path"
- **Fields added**: `boot_partition_guid[16]`, `boot_partition_style` (GPT vs MBR), plus related metadata.
- **Why**: GPT-style boot media needs explicit partition identification so the kernel can locate diagnostic scratch (X:\Diag\) on the correct partition and so recovery can cross-check the partition identity vs `BootOrder`.

### v3 -- UEFI boot variables

- **Commit**: [`77a4823c`](https://github.com/rizonesoft/impossible-os/commit/77a4823c) "boot: read UEFI boot variables -- BootOrder, BootCurrent, BootNext"
- **Fields added**: `uefi_boot_current`, `uefi_boot_next`, `uefi_boot_next_valid`, `uefi_boot_order[16]`, `uefi_boot_order_count`.
- **Why**: the bootloader needs to read `BootOrder` + `BootCurrent` + `BootNext` before ExitBootServices (they come from UEFI variable storage); surfacing them to the kernel lets higher-level boot-device debug code explain the boot entry that fired.

### v2 -- Boot device type and path

- **Commit**: [`6c3e723f`](https://github.com/rizonesoft/impossible-os/commit/6c3e723f) "boot: pass boot device type and path in boot_info"
- **Fields added**: `boot_device_type` (enum: unknown / SATA / NVMe / USB / network), `boot_device_path[128]` (UEFI device path in text form).
- **Why**: the kernel needs to log which device it booted from for diagnostics and to drive the recovery-partition search.

### v1 -- Initial ABI

- **Commit**: [`24d7baa2`](https://github.com/rizonesoft/impossible-os/commit/24d7baa2) "boot: boot_info ABI header fields and bootloader populate"
- **Baseline** of `struct boot_info` with `header{magic, version, size}`, memory map, framebuffer, UEFI config table, runtime services pointer, TPM event log pointer, USB enumeration state, timing TSC timestamps, serial port metadata, last-boot error code, and the parsed `boot_config` subset.
- **Why**: first version of the ABI. Everything since is accretion.

## Rollback and compatibility

The compatibility policy is **strict exact match** documented in [`include/kernel/boot_version.h`](../../include/kernel/boot_version.h) and enforced by [`boot_version_classify()`](../../src/kernel/main/boot_version.c) in the kernel's Phase 0 validator. There is no downgrade adapter: a v5 bootloader paired with a v6 kernel halts at `boot_phase0` with a `boot_version_fault` record carrying `observed_version=5`, `expected_version=6`. The fatal screen + serial log show every observed/expected field; late-boot [`boot_version_blackbox_transcribe()`](../../src/kernel/main/boot_version.c) transcribes a persisted NVRAM record (when present) to `X:\Diag\boot-proto-fault.txt`.

When the ABI eventually reaches a stable version branch, a compatibility window (min/max version pair) can be added to `boot_version_classify` without disrupting the fatal-rendering or transcription paths. Until then, rebuilding both halves with `bash scripts/build.sh` is the canonical response to any version mismatch.

## How to add a new version

1. Pick the next integer after the current `BOOT_INFO_VERSION`.
2. Update BOTH `include/kernel/boot_info.h` AND `src/boot/uefi/boot_info_mirror.h` in the same commit. Both must carry the same new value.
3. Add or modify fields. Update every `_Static_assert` that pins an offset. Re-run `make boot-info-abi` to regenerate the manifest.
4. Open the owning TODO section and list the new fields under a **Notes** block with the committing hash.
5. Add a new `### v<N>` heading at the top of this file with the row shape above. Keep the list **newest first**.
6. Commit. The next push lands both the code change and the changelog row together; reviewers see one atomic unit of ABI evolution.
