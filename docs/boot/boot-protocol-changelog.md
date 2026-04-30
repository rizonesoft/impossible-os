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

### v12 (current) -- Boot-decision UNSET sentinel + per-reason policy table

- **Commit**: pending (re-review of [Common Boot-Path Provenance and Decision Record](../../todo/01-boot-platform/TODO-01-boot-protocol-abi-handoff.md#12-common-boot-path-provenance-and-decision-record))
- **Fields added**: none (semantic-only ABI change).
- **Why**: Codex 2026-04-30 H1 caught a phantom-NORMAL-boot bug -- the v8 layout had `BOOT_PATH_NORMAL = BOOT_REASON_NORMAL = 0`, so any producer that did not populate the four boot-decision fields (e.g. `multiboot2_parse.c` pre-adapter wiring) passed validation as if the firmware reported a real cold boot. Fixed by introducing `BOOT_PATH_UNSET = 0` and `BOOT_REASON_UNSET = 0` as sentinels; `NORMAL` shifts to value 1. Other enum values shift up by 1. Validator Rule 1/2 now hard-rejects `UNSET` so a BSS-zero record halts instead of certifying.
- **Why (consistency review)**: Rule 5/6/7 switches default-allowed any future enum value to bypass classification, and Rule 7 was unidirectional (reason -> flag enforced; flag -> reason not). Replaced with a per-reason policy table (`reason_policy[]` in `boot_decision.c`) indexed by `enum boot_reason_code`, with `_Static_assert` pinning length to `BOOT_REASON_CODE_MAX + 1`. Rule 7b now also enforces the inverse: any trigger flag set in `boot_source_flags` MUST match the current reason (a producer setting `RECOVERY_TRIGGERED` while reporting `reason=NORMAL` is rejected as a provenance contradiction).
- **Producers / consumers**: `bootx64.c` already uses enum names so the value shift is implicit; `multiboot2_parse.c` now explicitly populates the four fields with `BOOT_PATH_NORMAL` / `BOOT_REASON_NORMAL` / `0u` / `0u` before completing parse.

### v11 -- ESP integrity fields

Adds three boot_info fields populated by the UEFI bootloader's pre-load ESP sanity gate so the kernel knows the partition was structurally validated before any code was loaded.

- **Commit**: [`fb0c6520`](https://github.com/rizonetech/impossible-os/commit/fb0c6520) "boot: ESP integrity check (GPT type GUID + FAT32 BPB + required-files batch)"
- **TODO**: [`02-uefi-hardening-secureboot.md` §13](../../todo/01-boot-platform/TODO-02-uefi-hardening-secureboot.md#13-esp-integrity-check)
- **Fields added**: `esp_size_mb` (uint32), `esp_filesystem_type` (uint8 -- FAT32 / FAT16 / unknown), `esp_type_guid_valid` (uint8 -- 1 if the ESP partition's GPT type GUID matched the expected `C12A7328-F81F-11D2-BA4B-00A0C93EC93B`).
- **Producer contract**: bootloader runs `esp_integrity_check()` before `load_kernel()`; halts via `boot_fatal()` on GPT type GUID mismatch, FAT32 BPB validation failure, or any required-file (BOOTX64.EFI, kernel.exe, boot.conf) missing. On success the three fields are stamped into boot_info as a structural-validity attestation the kernel can echo into BlackBox.
- **Consumer contract**: `boot_phase0` reads the fields after header validation and surfaces them through the `[BOOT-INTEGRITY]` klog line. A future attestation consumer can chain them into the SRTM PCR replay alongside the v10 UKI flag.
- **Why**: ESP corruption / partition-type drift used to surface as a confusing kernel crash mid-init when a stale BOOTX64.EFI loaded the wrong kernel.exe. Validating at the partition layer + propagating the attestation forward gives operators a definitive "the partition was structurally valid at boot time" signal in BlackBox without re-walking the FAT.

### v10 -- Unified Kernel Image (UKI) provenance flag

Adds a single new flag bit to the existing `boot_info.flags` field. No
struct layout changes; the bump signals that consumers may now observe
`BOOT_FLAG_INVOKED_VIA_UKI` (`1u << 3`) in the flag word.

- **Owning TODO**: [Unified Signed Boot Artifact (UKI-style)](../../todo/01-boot-platform/TODO-02-uefi-hardening-secureboot.md#11-unified-signed-boot-artifact-uki-style)
- **Mask update**: `BOOT_FLAG_MASK_KNOWN` extends to include the new bit, so the validator does not reject UKI-marked boot info as unknown-flag drift.
- **Producer contract**: bootloader sets the bit when `detect_uki_sections()` finds a `.linux` PE section in its own LoadedImage and `load_kernel()` uses the embedded buffer instead of opening `\\kernel.exe` from the ESP. Implies the kernel + cmdline + osrel were covered by the firmware-Secure-Boot-verified PE signature as a single signed unit.
- **Consumer contract**: kernel attestation surfaces (`HKLM\SYSTEM\Boot\Decision`, future PCR replay) read the bit to report whole-chain signature coverage vs the per-file split path.

### v9 -- Anti-rollback binding + warm-kernel-update handoff ABI

Two independent ABI extensions land under v9:

- **Commits**: [`11be3e13`](https://github.com/rizonetech/impossible-os/commit/11be3e13) "boot: anti-rollback security-version binding via UEFI NVRAM" + [`975ef6e5`](https://github.com/rizonetech/impossible-os/commit/975ef6e5) "boot: warm-kernel-update handoff ABI fields and descriptor"
- **TODOs**: [§13 Anti-Rollback and Security-Version Binding](../../todo/01-boot-platform/TODO-01-boot-protocol-abi-handoff.md#13-anti-rollback-and-security-version-binding) + [§14 Warm-Kernel-Update Handoff ABI](../../todo/01-boot-platform/TODO-01-boot-protocol-abi-handoff.md#14-warm-kernel-update-handoff-abi)
- **Anti-rollback fields added**: `flags`, `os_loader_security_version`, `required_security_version`, `_rollback_pad` (four `uint32_t` fields appended to `struct boot_info` after `boot_fallback_depth`). Also `anti_rollback_raise` (1 byte taken from `boot_config._reserved[10]` -> `_reserved[9]`; boot_config total size unchanged).
- **Warm-update extensions**: new enum value `BOOT_PAYLOAD_WARM_UPDATE_STATE=9` for `enum boot_payload_type`, new flag bit `BOOT_FLAG_WARM_UPDATE` in `boot_info.flags`, new mmap discriminator `BOOT_MMAP_WARM_UPDATE=15`, and a 6-bit continuation flag family (`BOOT_WARM_UPDATE_CONT_*`) carried in `boot_payload_desc.flags` bits 8..13 alongside the standard `BOOT_PAYLOAD_FLAG_*` family. No struct field changes for the warm-update half -- ABI extension via new enum + flag values.
- **Anti-rollback why**: bind a monotonic counter to UEFI NVRAM variable `IPOSRequiredSecVersion` so a stored-but-older signed `kernel.exe` cannot boot on a machine whose policy has advanced past that version. Windows 11 ships `OsLoaderSecurityVersion` in LPB; Linux uses shim+SBAT revocation. Validator: [`boot_rollback_validate`](../../src/kernel/main/boot_rollback.c) wired from `boot_hw.c` after `boot_decision_validate`. Phase-3 raise hook in `boot_desktop.c` after `POST16_BOOT_OK`, gated on `boot_config.anti_rollback_raise` opt-in, uses `uefi_set_variable` to advance the NVRAM counter. Never decreases. Never writes a redundant same-value. Refusal path is pre-jump bootloader halt with a UEFI fatal screen + serial diagnostic.
- **Warm-update why**: matches the Linux 6.16 Kexec Handover ABI surface so kernel replacement on cloud / server hosts can preserve memory state across kexec. Validator: [`boot_warm_update_consume`](../../src/kernel/main/boot_warm_update.c) returns `ACCEPTED` or `COLD_FALLBACK` (fail-closed on any rejection; partial reattach would leave unreconciled subsystem state). Actual runtime live-update machinery is out-of-scope here; tracked in [03-memory-concurrency/TODO-11 Warm-Kernel-Update Runtime](../../todo/03-memory-concurrency/TODO-11-warm-kernel-update-runtime.md).

### v8 -- Boot-path provenance and decision record

- **Commit**: [`12f39619`](https://github.com/rizonetech/impossible-os/commit/12f39619) "boot: add common boot decision record"
- **TODO**: [Common Boot-Path Provenance and Decision Record](../../todo/01-boot-platform/TODO-01-boot-protocol-abi-handoff.md#12-common-boot-path-provenance-and-decision-record)
- **Fields added**: `boot_path`, `boot_reason`, `boot_source_flags`, `boot_fallback_depth` (four `uint32_t` fields appended to `struct boot_info` after `caps_degraded`).
- **Manifest**: 232 fields, `struct_size = 23736` bytes.
- **Kernel canonical SHA-256**: `ec89e4588783...` (produced by `tools/boot-info-manifest/dump-kernel.c`; kernel and mirror must agree).
- **Why**: Give consumers (Registry / BlackBox / recovery orchestrator / attestation) one authoritative answer to "what path did this boot take and why" BEFORE they inspect per-path descriptors. Stable enums + a closed flag mask mean a consumer can trust the record rather than re-deriving it from a dozen provenance fields scattered across `boot_info`. Unknown `boot_path` / `boot_reason` halt (stale-kernel-on-newer-loader gate). Unknown bits in `boot_source_flags` also halt -- the forward-compat tolerance that applies to capability bits does NOT apply here because every flag maps to a kernel-side policy. Validator: [`boot_decision_validate`](../../src/kernel/main/boot_decision.c) wired from `boot_hw.c` after `boot_caps_validate`.

### v7 -- Capability negotiation

- **Commit**: [`bed68e53`](https://github.com/rizonetech/impossible-os/commit/bed68e53) "boot: add boot_info capability negotiation"
- **TODO**: [Capability Negotiation and Degraded-Feature Flags](../../todo/01-boot-platform/TODO-01-boot-protocol-abi-handoff.md#11-capability-negotiation-and-degraded-feature-flags)
- **Fields added**: `caps_required`, `caps_present`, `caps_degraded` (three `uint64_t` bitmasks appended to `struct boot_info` after `payload_total_bytes`).
- **Manifest**: 228 fields, `struct_size = 23720` bytes.
- **Kernel canonical SHA-256**: `a44cc0c61ce7...` (produced by `tools/boot-info-manifest/dump-kernel.c`; kernel and mirror must agree).
- **Why**: establish a forward-compatible handshake between loader and kernel so the nine optional capabilities (`PAYLOAD_DESCRIPTORS`, `RUNTIME_SERVICES`, `SECURE_BOOT_STATE`, `TPM_EVENT_LOG`, `USB_HANDOVER`, `MEDIA_ROLE`, `NETWORK_PROVENANCE`, `RESUME_METADATA`, `ALT_PROTOCOL_ADAPTER`) can be independently published OR marked degraded with a safe fallback, without requiring a version bump for every future capability bit. Unknown `caps_required` bits are rejected (stale-kernel-on-newer-loader gate); unknown bits in `caps_present` / `caps_degraded` are tolerated so older kernels can boot against newer loaders that opportunistically populate reserved bits. Validator: [`boot_caps_validate`](../../src/kernel/main/boot_caps.c) wired from `boot_hw.c` after `boot_payload_validate`. Full bit catalog: [boot-info-fields.md](boot-info-fields.md) "Capability negotiation" -> "BOOT_CAP_* bit catalog (v7)".

### v6 -- Typed payload descriptor array

- **Commit**: [`01991083`](https://github.com/rizonetech/impossible-os/commit/01991083) "boot: add typed payload descriptors"
- **TODO**: [Optional Payload Descriptor Array](../../todo/01-boot-platform/TODO-01-boot-protocol-abi-handoff.md#4-optional-payload-descriptor-array)
- **Fields added**: `payload_descriptors[32]`, `payload_count`, `payload_overflow`, `payload_total_bytes`.
- **Manifest**: 225 fields, `struct_size = 23696` bytes.
- **Kernel canonical SHA-256**: `eb5f035e21f5...` (produced by `tools/boot-info-manifest/dump-kernel.c`; kernel and mirror must agree).
- **Why**: establish the typed payload ABI that the [Module and initrd Handoff Contract](../../todo/01-boot-platform/TODO-01-boot-protocol-abi-handoff.md#5-module-and-initrd-handoff-contract) fills in (module, initrd, recovery_image) and that the [Handoff Memory Ownership section](../../todo/01-boot-platform/TODO-01-boot-protocol-abi-handoff.md#6-handoff-memory-ownership-and-pmm-reservation-table) reserves from PMM reclaim. Post-v6 hardening (2026-04-23) tightened the NONE-empty-slot invariant so the validator rejects descriptors with `type=BOOT_PAYLOAD_NONE` that carry non-zero fields; this is a validator tightening, not a layout change, so no version bump.

### v5 -- Removable media detection

- **Commit**: [`e4bd76be`](https://github.com/rizonetech/impossible-os/commit/e4bd76be) "boot: detect removable media via EFI_BLOCK_IO_PROTOCOL"
- **TODO**: [UEFI Hardening + Secure Boot](../../todo/01-boot-platform/TODO-02-uefi-hardening-secureboot.md) (boot media classification work).
- **Fields added**: removable-media / boot partition classification flags in the boot_device area.
- **Why**: downstream consumers (recovery path, default drive selection) need to distinguish removable from fixed boot media before mounting.

### v4 -- Boot partition GUID

- **Commit**: [`d125a47c`](https://github.com/rizonetech/impossible-os/commit/d125a47c) "boot: extract and validate boot partition GUID from device path"
- **Fields added**: `boot_partition_guid[16]`, `boot_partition_style` (GPT vs MBR), plus related metadata.
- **Why**: GPT-style boot media needs explicit partition identification so the kernel can locate diagnostic scratch (X:\Diag\) on the correct partition and so recovery can cross-check the partition identity vs `BootOrder`.

### v3 -- UEFI boot variables

- **Commit**: [`77a4823c`](https://github.com/rizonetech/impossible-os/commit/77a4823c) "boot: read UEFI boot variables -- BootOrder, BootCurrent, BootNext"
- **Fields added**: `uefi_boot_current`, `uefi_boot_next`, `uefi_boot_next_valid`, `uefi_boot_order[16]`, `uefi_boot_order_count`.
- **Why**: the bootloader needs to read `BootOrder` + `BootCurrent` + `BootNext` before ExitBootServices (they come from UEFI variable storage); surfacing them to the kernel lets higher-level boot-device debug code explain the boot entry that fired.

### v2 -- Boot device type and path

- **Commit**: [`6c3e723f`](https://github.com/rizonetech/impossible-os/commit/6c3e723f) "boot: pass boot device type and path in boot_info"
- **Fields added**: `boot_device_type` (enum: unknown / SATA / NVMe / USB / network), `boot_device_path[128]` (UEFI device path in text form).
- **Why**: the kernel needs to log which device it booted from for diagnostics and to drive the recovery-partition search.

### v1 -- Initial ABI

- **Commit**: [`24d7baa2`](https://github.com/rizonetech/impossible-os/commit/24d7baa2) "boot: boot_info ABI header fields and bootloader populate"
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
