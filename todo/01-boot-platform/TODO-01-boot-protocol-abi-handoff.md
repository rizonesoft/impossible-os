# TODO-01 -- Boot Protocol ABI & Handoff Contract

> **Goal:** Make the bootloader-to-kernel contract explicit, versioned, testable, and complete. `struct boot_info` has become the central ABI for memory maps, framebuffer, config tables, runtime services, TPM logs, USB handoff, timing, boot device identity, serial, and future modules. This TODO owns the full handoff schema so no field is added without versioning, ownership, validation, and cross-build drift protection.
> **Current state:** `include/kernel/boot_info.h` and `src/boot/uefi/bootx64.c` now ship the core ABI safety foundation: offset-0 header, pre-copy kernel validation, mirrored offset asserts, reserved handoff pages, and full-range overlap guards. The remaining gap is no longer "can the kernel trust the blob at all?" but "is the contract fully owned, versioned, extensible, and auditable across every payload and consumer?"

## Inputs

- [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h)
- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c)
- [`src/kernel/main/boot_info.c`](../../src/kernel/main/boot_info.c)
- [`src/kernel/test/test_boot_info.c`](../../src/kernel/test/test_boot_info.c)
- -> XREF: `TODO-03-bootloader-error-recovery.md §2,§13,§14,§18` -- stale-loader failure UX, persisted pre-kernel error codes, QR/error screen paths, and bootloader fatal handling that complement ABI failures here
- -> XREF: `TODO-20-usb-zero-delay-handover.md §4` -- USB controller/device DMA state in boot_info
- -> XREF: `TODO-13-tpm-measured-boot-attestation.md §2` -- TPM event-log ownership and PCR metadata

## Outcome

- Every `boot_info` field has a documented owner, producer, consumer, lifetime, and validation rule.
- `BOOT_INFO_VERSION` changes are intentional, testable, and reflected in both bootloader and kernel builds.
- Optional payloads such as modules, initrd, recovery image, hibernation image metadata, and network-boot provenance have stable descriptors.
- The bootloader never passes pointers the kernel cannot safely dereference in Phase 0.
- A generated ABI report catches same-size field reorder drift, not just size mismatches.

## Consolidated Shipped Foundations

The boot-protocol foundations that were previously documented under `TODO-03` are now owned here so the full handoff story stays in one roadmap:

- `boot_info` header at offset 0 with `magic`, `version`, and `size`, populated by the bootloader as the last handoff step.
- Pre-copy kernel validation split into address-phase and header-phase checks, with pure unit tests and mirrored offset assertions across bootloader and kernel.
- Full-range `boot_info` reservation and overlap protection so ELF segments, DMA pages, and later `AllocatePages` calls cannot silently clobber the handoff buffer.
- Memory-map normalization and handoff-shape hardening as prerequisite context for the remaining ownership, payload, and versioning work in this TODO.

## Implementation Order

| ⭐ | Order | Deliverable | Depends On | Status |
| --- | :---: | --- | --- | :---: |
| 💎 | 1 | Canonical boot_info field ownership table | -- | [ ] |
| 💎 | 2 | Generated ABI manifest and offset fingerprint | §1 | [ ] |
| 💎 | 3 | Full bootloader/kernel mirror drift checker | §2 | [ ] |
| 💎 | 4 | Optional payload descriptor array | §1 | [ ] |
| 💎 | 5 | Module and initrd handoff contract | §4 | [ ] |
| 💎 | 6 | Handoff memory ownership and PMM reservation table | §4 | [ ] |
| 💎 | 7 | Version negotiation and stale-loader error path | §2, TODO-03 §2 | [ ] |
| 💎 | 8 | Boot protocol documentation and schema changelog | §1-§7 | [ ] |
| ⭐ | 9 | ABI fuzz and compatibility tests | §2, §7 | [ ] |
| ⭐ | 10 | Cross-domain owner audit for every boot_info field | §1-§9 | [ ] |

## 1. Canonical boot_info Field Ownership Table

- [ ] Add `docs/boot/boot-info-fields.md` with columns: field, producer, first valid phase, consumer(s), lifetime, owning TODO, validation.
- [ ] Mark kernel-populated fields (`degraded_mask`, `hv_flags`, `secure_boot_enabled`) separately from bootloader-populated fields.
- [ ] Identify fields that are stale, legacy, or only used by Multiboot2 and decide retain/deprecate.
- [ ] Add comments in `boot_info.h` that point to the ownership document rather than duplicating all policy inline.
- [ ] Commit: `"docs: boot_info field ownership matrix"`

## 2. Generated ABI Manifest and Offset Fingerprint

- [ ] Add a build step that emits `build/boot-info-abi.json`: struct size, version, every field offset/size, and SHA-256 fingerprint.
- [ ] Generate the same manifest for the UEFI mirror compilation unit.
- [ ] Fail the build if kernel and bootloader manifests disagree.
- [ ] Keep the existing static asserts for early compile failures.
- [ ] Commit: `"boot: generate boot_info ABI manifest"`

## 3. Full Bootloader/Kernel Mirror Drift Checker

- [ ] Replace the five-field static fingerprint with complete table validation in host tooling.
- [ ] Check nested structs: `boot_config`, `boot_usb_device`, `boot_usb_controller`, runtime memory entries, GOP modes.
- [ ] Add CI output that prints the first differing field on mismatch.
- [ ] Add negative tests with intentionally reordered fixture structs.
- [ ] Commit: `"test: boot_info mirror drift checker"`

## 4. Optional Payload Descriptor Array

- [ ] Add `boot_payload_desc[]` for typed physical payloads: module, initrd, recovery image, hibernation metadata, TPM log copy, network config blob.
- [ ] Include type, flags, physical start, length, alignment, checksum, and producer.
- [ ] Keep legacy single `module_start/module_end` as compatibility aliases until consumers migrate.
- [ ] Validate no payload overlaps boot_info, runtime memory, USB DMA, framebuffer, or reserved crash regions.
- [ ] Commit: `"boot: add typed payload descriptors"`

## 5. Module and initrd Handoff Contract

- [ ] Define kernel module payload type for early `.kmod` or recovery helpers.
- [ ] Define initrd/recovery payload type for recovery shell, driver bundles, and installer assets.
- [ ] Add bootloader loading syntax in `boot.conf`: `module=`, `initrd=`, `recovery_image=`.
- [ ] Add kernel-side enumeration API: `boot_payload_find(type, index)`.
- [ ] Commit: `"boot: module and initrd handoff contract"`

## 6. Handoff Memory Ownership and PMM Reservation Table

- [ ] Create a single `boot_reserved_region` table populated from boot_info before PMM frees memory.
- [ ] Include boot_info itself, USB DMA pages, TPM log copy, payload descriptors, runtime services regions, framebuffer, survival logs.
- [ ] Teach PMM to log every retained region and fail on overlap.
- [ ] Add a boot diagnostic dump to BlackBox.
- [ ] Commit: `"boot: centralize handoff memory reservations"`

## 7. Version Negotiation and Stale-Loader Error Path

- [ ] Define compatibility policy: exact version required for boot, or explicit downgrade adapter.
- [ ] Add bootloader-side display for kernel ABI mismatch before jump when possible.
- [ ] Add kernel-side fatal screen with observed/expected version, size, and manifest hash.
- [ ] Persist mismatch reason in UEFI NVRAM and BlackBox.
- [ ] Commit: `"boot: hard fail stale boot protocol versions"`

## 8. Boot Protocol Documentation and Schema Changelog

- [ ] Add `docs/boot/boot-protocol.md` with struct lifecycle, phases, pointer validity, and examples.
- [ ] Add `docs/boot/boot-protocol-changelog.md` keyed by `BOOT_INFO_VERSION`.
- [ ] Link every version bump to a TODO and commit.
- [ ] Document how third-party bootloaders can populate a minimal supported handoff.
- [ ] Commit: `"docs: boot protocol ABI reference"`

## 9. ABI Fuzz and Compatibility Tests

- [ ] Fuzz `boot_info_validate_addr()` and `boot_info_validate_header()` with malformed sizes, addresses, versions, and overlap cases.
- [ ] Boot QEMU with intentionally stale `BOOTX64.EFI` and assert friendly error output.
- [ ] Boot QEMU with intentionally stale `kernel.exe` and assert friendly error output.
- [ ] Add tests for optional payload overlap rejection.
- [ ] Commit: `"test: boot protocol ABI fuzz coverage"`

## 10. Cross-Domain Owner Audit for Every boot_info Field

- [ ] Audit every field in `boot_info.h` and add an XREF to the owning TODO.
- [ ] Remove or deprecate fields with no owner and no consumer.
- [ ] Update `GAP-ANALYSIS.md` closure table when all fields have owners.
- [ ] Commit: `"docs: close boot_info ownership audit"`

## OS Comparison

| ⭐ | Feature | Windows | Linux | Impossible OS |
| --- | --- | --- | --- | --- |
| 💎 | Versioned loader/kernel ABI | BCD/winload contracts | boot_params, EFI stub | TODO-01 |
| 💎 | initrd/module handoff | boot drivers / ramdisk | initrd/initramfs | TODO-01 §5 |
| ⭐ | Generated ABI manifest | internal only | partial headers | TODO-01 §2 |
| ⭐ | Field-level owner map | internal only | scattered docs | TODO-01 §1 |

## Unit Tests

- [ ] `test_boot_info_abi_manifest_matches`
- [ ] `test_boot_info_payload_overlap_rejected`
- [ ] `test_boot_info_payload_find`
- [ ] `test_boot_info_stale_version_error`

## Verification

- [ ] `scripts/debug/run-boot-tests.bat`
- [ ] QEMU stale loader image
- [ ] QEMU stale kernel image
- [ ] Bare metal boot with USB handoff and TPM log present
