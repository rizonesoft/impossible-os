# TODO-01 -- Boot Protocol ABI & Handoff Contract

> **Goal:** Make the bootloader-to-kernel contract explicit, versioned, testable, and complete. `struct boot_info` has become the central ABI for memory maps, framebuffer, config tables, runtime services, TPM logs, USB handoff, timing, boot device identity, serial, and future modules. This TODO owns the full handoff schema so no field is added without versioning, ownership, validation, and cross-build drift protection.
> [!IMPORTANT]
> **Current state:** `include/kernel/boot_info.h` and `src/boot/uefi/bootx64.c` now ship the core ABI safety foundation: offset-0 header, pre-copy kernel validation, mirrored offset asserts, reserved handoff pages, and full-range overlap guards. The remaining gap is no longer "can the kernel trust the blob at all?" but "is the contract fully owned, versioned, extensible, and auditable across every payload and consumer?"

## Inputs

- [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h)
- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c)
- [`src/kernel/main/boot_info.c`](../../src/kernel/main/boot_info.c)
- [`src/kernel/test/test_boot_info.c`](../../src/kernel/test/test_boot_info.c)
- -> XREF: `T03 §2,§13,§14,§18` -- stale-loader failure UX, persisted pre-kernel error codes, QR/error screen paths, and bootloader fatal handling that complement ABI failures here
- -> XREF: `T06 §6` -- boot media role, artifact id, manifest digest, and source-path metadata added to `boot_info`
- -> XREF: `T12 §7` -- random-seed payload descriptor and early CSPRNG handoff
- -> XREF: `T13 §2` -- TPM event-log ownership, PCR metadata, and measured-boot consumers
- -> XREF: `T20 §4` -- USB controller/device DMA state in `boot_info`
- -> XREF: `T22 §2` -- recovery environment payload consumer and recovery boot path
- -> XREF: `T25 §6` -- network provenance and config payload descriptors
- -> XREF: `T26 §5` -- typed hibernation/resume payload descriptor

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

| ⭐  | Order | Deliverable                                        | Depends On                         | Status |
| --- | :---: | -------------------------------------------------- | ---------------------------------- | :----: |
| 💎  |   1   | Canonical boot_info field ownership table          | --                                 |  [ ]   |
| 💎  |   2   | Generated ABI manifest and offset fingerprint      | §1                                 |  [ ]   |
| 💎  |   3   | Full bootloader/kernel mirror drift checker        | §2                                 |  [ ]   |
| 💎  |   4   | Optional payload descriptor array                  | §1                                 |  [ ]   |
| 💎  |   5   | Module and initrd handoff contract                 | §4                                 |  [ ]   |
| 💎  |   6   | Handoff memory ownership and PMM reservation table | §4                                 |  [ ]   |
| 💎  |   7   | Version negotiation and stale-loader error path    | §2, T03 §2                         |  [ ]   |
| 💎  |   8   | Boot protocol documentation and schema changelog   | §1, §2, §3, §4, §5, §6, §7         |  [ ]   |
| ⭐  |   9   | ABI fuzz and compatibility tests                   | §2, §7                             |  [ ]   |
| ⭐  |  10   | Cross-domain owner audit for every boot_info field | §1, §2, §3, §4, §5, §6, §7, §8, §9 |  [ ]   |

## 1. Canonical boot_info Field Ownership Table

- [ ] Add `docs/boot/boot-info-fields.md` with columns: field, producer, first valid phase, consumer(s), lifetime, owning TODO, validation.
- [ ] Mark kernel-populated fields (`degraded_mask`, `hv_flags`, `secure_boot_enabled`) separately from bootloader-populated fields.
- [ ] Identify fields that are stale, legacy, or only used by Multiboot2 and decide retain/deprecate.
- [ ] Add comments in `boot_info.h` that point to the ownership document rather than duplicating all policy inline.
- [ ] Commit: `"docs: boot_info field ownership matrix"`

**Test checkpoint:** `docs/boot/boot-info-fields.md` exists and every `struct boot_info` field is listed with producer, first valid phase, consumer, lifetime, owning TODO, and validation rule. Spot-check `header`, `fb`, `usb_controller`, and TPM-related fields from `boot_info.h`, then confirm QEMU WHPX, QEMU TCG, VirtualBox, and bare-metal boot behavior is unchanged because this section is documentation-only.

## 2. Generated ABI Manifest and Offset Fingerprint

- [ ] Add a build step that emits `build/boot-info-abi.json`: struct size, version, every field offset/size, and SHA-256 fingerprint.
- [ ] Generate the same manifest for the UEFI mirror compilation unit.
- [ ] Fail the build if kernel and bootloader manifests disagree.
- [ ] Keep the existing static asserts for early compile failures.
- [ ] Commit: `"boot: generate boot_info ABI manifest"`

**Test checkpoint:** `bash scripts/build.sh` emits `build/boot-info-abi.json` for both kernel and bootloader views, and a forced field reorder or offset change fails the build with the first mismatching field named. After restoring the valid layout, QEMU WHPX, QEMU TCG, VirtualBox, and bare-metal boots remain unchanged.

## 3. Full Bootloader/Kernel Mirror Drift Checker

- [ ] Replace the five-field static fingerprint with complete table validation in host tooling.
- [ ] Check nested structs: `boot_config`, `boot_usb_device`, `boot_usb_controller`, runtime memory entries, GOP modes.
- [ ] Add CI output that prints the first differing field on mismatch.
- [ ] Add negative tests with intentionally reordered fixture structs.
- [ ] Commit: `"test: boot_info mirror drift checker"`

**Test checkpoint:** The host drift checker passes for the real kernel/bootloader pair, fails for an intentionally reordered fixture, and prints the first differing nested field rather than only a hash mismatch. QEMU WHPX, QEMU TCG, VirtualBox, and bare-metal boot behavior stays unchanged once the real structs match again.

## 4. Optional Payload Descriptor Array

- [ ] Add `boot_payload_desc[]` for typed physical payloads: module, initrd, recovery image, hibernation metadata, TPM log copy, network config blob.
- [ ] Include type, flags, physical start, length, alignment, checksum, and producer.
- [ ] Keep legacy single `module_start/module_end` as compatibility aliases until consumers migrate.
- [ ] Record the owner for each typed descriptor: USB handover (`T20 §4`), random seed (`T12 §7`), TPM log (`T13 §2`), network provenance (`T25 §6`), and hibernation/resume metadata (`T26 §5`). If a module, initrd, or recovery-image consumer still has no owning section, create that owner before marking this section done.
- [ ] Validate no payload overlaps boot_info, runtime memory, USB DMA, framebuffer, or reserved crash regions.
- [ ] Commit: `"boot: add typed payload descriptors"`

**Test checkpoint:** Kernel boot logs show typed payload descriptors with stable type, start, and length values; overlap attempts against `boot_info`, runtime memory, USB DMA, framebuffer, or crash-reserved regions are rejected with a clear serial diagnostic. Verify descriptor ingestion on QEMU WHPX, QEMU TCG, VirtualBox, and bare metal.

## 5. Module and initrd Handoff Contract

- [ ] Define kernel module payload type for early `.kmod` or recovery helpers.
- [ ] Define initrd/recovery payload type for recovery shell, driver bundles, and installer assets.
- [ ] Add bootloader loading syntax in `boot.conf`: `module=`, `initrd=`, `recovery_image=`.
- [ ] Add kernel-side enumeration API: `boot_payload_find(type, index)`.
- [ ] Commit: `"boot: module and initrd handoff contract"`

**Test checkpoint:** A boot image with `module=`, `initrd=`, and `recovery_image=` entries exposes the expected descriptor types, `boot_payload_find(type, index)` returns the correct start/length pair, and missing or malformed entries fail with a specific bootloader error. Verify on QEMU WHPX, QEMU TCG, VirtualBox, and bare metal.

## 6. Handoff Memory Ownership and PMM Reservation Table

- [ ] Create a single `boot_reserved_region` table populated from boot_info before PMM frees memory.
- [ ] Include boot_info itself, USB DMA pages, TPM log copy, payload descriptors, runtime services regions, framebuffer, survival logs.
- [ ] Teach PMM to log every retained region and fail on overlap.
- [ ] Add a boot diagnostic dump to BlackBox.
- [ ] Commit: `"boot: centralize handoff memory reservations"`

**Test checkpoint:** Early PMM logs enumerate every retained boot region exactly once, overlap attempts fail before free-memory handoff, and BlackBox captures the reservation dump for post-boot inspection. Verify the reservation table on QEMU WHPX, QEMU TCG, VirtualBox, and bare metal.

## 7. Version Negotiation and Stale-Loader Error Path

- [ ] Define compatibility policy: exact version required for boot, or explicit downgrade adapter.
- [ ] Add bootloader-side display for kernel ABI mismatch before jump when possible.
- [ ] Add kernel-side fatal screen with observed/expected version, size, and manifest hash.
- [ ] Persist mismatch reason in UEFI NVRAM and BlackBox.
- [ ] Commit: `"boot: hard fail stale boot protocol versions"`

**Test checkpoint:** A stale `BOOTX64.EFI` paired with a current kernel, and a stale kernel paired with a current bootloader, both stop with the expected observed/expected version, size, and manifest-hash diagnostics instead of hanging. Verify the friendly failure path on QEMU WHPX, QEMU TCG, VirtualBox, and at least one bare-metal system using removable-media recovery workflow.

## 8. Boot Protocol Documentation and Schema Changelog

- [ ] Add `docs/boot/boot-protocol.md` with struct lifecycle, phases, pointer validity, and examples.
- [ ] Add `docs/boot/boot-protocol-changelog.md` keyed by `BOOT_INFO_VERSION`.
- [ ] Link every version bump to a TODO and commit.
- [ ] Document how third-party bootloaders can populate a minimal supported handoff.
- [ ] Commit: `"docs: boot protocol ABI reference"`

**Test checkpoint:** `docs/boot/boot-protocol.md` and `docs/boot/boot-protocol-changelog.md` exist, each `BOOT_INFO_VERSION` bump points to the owning TODO and commit, and the minimal third-party bootloader contract lists the exact fields and validation steps required for a supported handoff. Confirm QEMU WHPX, QEMU TCG, VirtualBox, and bare-metal boots are unchanged because this section is documentation-only.

## 9. ABI Fuzz and Compatibility Tests

- [ ] Fuzz `boot_info_validate_addr()` and `boot_info_validate_header()` with malformed sizes, addresses, versions, and overlap cases.
- [ ] Boot QEMU with intentionally stale `BOOTX64.EFI` and assert friendly error output.
- [ ] Boot QEMU with intentionally stale `kernel.exe` and assert friendly error output.
- [ ] Add tests for optional payload overlap rejection.
- [ ] Commit: `"test: boot protocol ABI fuzz coverage"`

**Test checkpoint:** `bash scripts/test.sh SUITE=boot` covers malformed address, size, version, and overlap cases, and the stale-image QEMU fixtures assert the exact mismatch strings rather than a generic boot halt. Re-run the positive boot path on QEMU WHPX, QEMU TCG, VirtualBox, and bare metal after the negative fixtures pass.

## 10. Cross-Domain Owner Audit for Every boot_info Field

- [ ] Audit every field in `boot_info.h` and add an XREF to the owning TODO.
- [ ] Remove or deprecate fields with no owner and no consumer.
- [ ] Update `GAP-ANALYSIS.md` closure table when all fields have owners.
- [ ] Commit: `"docs: close boot_info ownership audit"`

**Test checkpoint:** Every `boot_info` field in `include/kernel/boot_info.h` resolves to one owner section or deprecation note, `todo/01-boot-platform/GAP-ANALYSIS.md` reflects the closure state, and no field remains ownerless in docs or comments. Confirm QEMU WHPX, QEMU TCG, VirtualBox, and bare-metal boots still consume the audited fields without behavior changes.

## OS Comparison

| ⭐ | Feature                        | 🪟 Win11                          | 🐧 Linux                                  | 🚀 Impossible OS                              |
| --- | ---                            | ---                               | ---                                        | ---                                           |
| 💎 | Versioned loader/kernel ABI    | ✅ winload and BCD-owned contract | ✅ EFI stub and boot-params contract       | ⬜ Planned in §1, §7, and §8                  |
| 💎 | Typed initrd and module handoff | ✅ Boot drivers and ramdisk paths | ✅ initrd/initramfs plus loader metadata   | ⬜ Planned in §4 and §5                       |
| ⭐ | Generated ABI manifest         | ⚪ Internal tooling, not exposed   | ⚠️ Partial header checks, no shared digest | ⬜ Planned in §2 and §3                       |
| ⭐ | Field-level ownership map      | ⚪ Internal ownership only         | ⚠️ Scattered comments and docs             | ⬜ Planned in §1 and §10                      |
| ⭐ | Friendly stale-loader mismatch | ⚠️ Usually opaque to developers    | ⚠️ Usually debug-log driven                | ⬜ Planned in §7 with manifest-hash evidence  |

## Unit Tests

> Wire into `test_runner_init()` through the existing `test_register_boot_info()` boot suite in `src/kernel/test/test_runner.c`, and extend that registration when new `TEST_CAT_BOOT` cases land in `src/kernel/test/test_boot_info.c` or the follow-on boot-protocol test file.
> Host-side manifest/drift tooling should also get fixture-based negative tests, but the kernel-facing ABI validation stays owned by the boot suite.

- [ ] Keep the existing validator coverage in `src/kernel/test/test_boot_info.c`: `boot_info_validate_addr((const void *)0x10000, sizeof(struct boot_info), BOOT_INFO_EARLY_MAP_END) == BOOT_OK`, bad magic/version/size headers return `BOOT_FATAL`, and the combined validator short-circuits misaligned pointers before header reads.
- [ ] Add `test_boot_info_manifest_kernel_bootloader_match`: generated kernel and bootloader ABI manifests contain the same struct size, version, per-field offsets, and fingerprint.
- [ ] Add `test_boot_info_payload_overlap_rejected`: a payload descriptor that overlaps `boot_info`, framebuffer, USB DMA, runtime memory, or crash-reserved regions is rejected with the expected failure code.
- [ ] Add `test_boot_info_payload_find_second_entry`: `boot_payload_find(type, index)` returns the second descriptor of a repeated type with the expected `phys_start` and `length`.
- [ ] Add `test_boot_info_stale_version_error_fields`: an ABI mismatch report preserves observed version, expected version, struct size, and manifest hash for the fatal screen and persisted diagnostics.
- [ ] Commit: `"test: extend boot_info ABI coverage"`

**Test checkpoint:** `bash scripts/test.sh SUITE=boot` exercises the validator, payload, and mismatch-report cases through `TEST_CAT_BOOT`, and the host manifest fixtures fail only when the layout actually drifts.

## Verification

- [ ] `bash scripts/build.sh` completes and the ABI manifest/drift tooling reports no differences for the real kernel and bootloader pair.
- [ ] `bash scripts/test.sh SUITE=boot` passes with `test_register_boot_info()` and the new boot-protocol cases enabled.
- [ ] QEMU WHPX and QEMU TCG both reject a stale `BOOTX64.EFI` with observed/expected version, size, and manifest-hash diagnostics.
- [ ] QEMU WHPX and QEMU TCG both reject a stale `kernel.exe` with the same friendly mismatch path.
- [ ] VirtualBox boots a matching image and logs the retained boot reservations plus typed payload descriptors without overlap warnings.
- [ ] Bare metal boots a matching image with USB handoff and TPM log payloads present, and PMM retains those regions exactly once.

**Test runner:** `scripts\debug\run-boot-tests.bat` (SUITE=boot)
