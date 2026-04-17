# TODO-01 -- Boot Protocol ABI & Handoff Contract

> **Goal:** Make the bootloader-to-kernel contract explicit, versioned, testable, and complete. `struct boot_info` has become the central ABI for memory maps, framebuffer, config tables, runtime services, TPM logs, USB handoff, timing, boot device identity, serial, and future modules. This TODO owns the full handoff schema so no field is added without versioning, ownership, validation, and cross-build drift protection.
> [!IMPORTANT]
> **Current state:** `include/kernel/boot_info.h` and `src/boot/uefi/bootx64.c` now ship the core ABI safety foundation: offset-0 header, pre-copy kernel validation, mirrored offset asserts, reserved handoff pages, and full-range overlap guards. The remaining gap is no longer "can the kernel trust the blob at all?" but "is the contract fully owned, versioned, extensible, and auditable across every payload and consumer?"

---

## Inputs

- [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h)
- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c)
- [`src/kernel/main/boot_info.c`](../../src/kernel/main/boot_info.c)
- [`src/kernel/main/boot_hw.c`](../../src/kernel/main/boot_hw.c)
- [`src/kernel/multiboot2_parse.c`](../../src/kernel/multiboot2_parse.c)
- [`src/kernel/tpm.c`](../../src/kernel/tpm.c)
- [`src/kernel/drivers/xhci.c`](../../src/kernel/drivers/xhci.c)
- [`src/kernel/test/test_boot_info.c`](../../src/kernel/test/test_boot_info.c)
- -> XREF: `T03 §2,§13,§14,§18` -- stale-loader failure UX, persisted pre-kernel error codes, QR/error screen paths, and bootloader fatal handling that complement ABI failures here
- -> XREF: `T05 §3,§6,§9,§12` -- shipped boot-device, Boot####, and Registry handoff fields already living inside `boot_info`
- -> XREF: `T06 §6` -- boot media role, artifact id, manifest digest, and source-path metadata added to `boot_info`
- -> XREF: `T12 §7` -- random-seed payload descriptor and early CSPRNG handoff
- -> XREF: `T13 §2` -- TPM event-log ownership, PCR metadata, and measured-boot consumers
- -> XREF: `T20 §4` -- USB controller/device DMA state in `boot_info`
- -> XREF: `T22 §2` -- recovery environment payload consumer and recovery boot path
- -> XREF: `T25 §6` -- network provenance and config payload descriptors
- -> XREF: `T26 §5` -- typed hibernation/resume payload descriptor
- -> XREF: `T08 §3,§4` -- alternate boot protocols must adapt into the canonical `boot_info` contract and explicit degraded-capability flags

## Outcome

- Every `boot_info` field has a documented owner, producer, consumer, lifetime, and validation rule.
- `BOOT_INFO_VERSION` changes are intentional, testable, and reflected in both bootloader and kernel builds.
- Optional payloads such as modules, initrd, recovery image, hibernation image metadata, and network-boot provenance have stable descriptors.
- The bootloader never passes pointers the kernel cannot safely dereference in Phase 0.
- A generated ABI report catches same-size field reorder drift, not just size mismatches.
- Compatibility negotiation is explicit: optional versus required handoff features can evolve without relying on struct-size drift alone.
- Recovery, installer, network, and resume flows share one boot-path decision record instead of inventing per-feature reason codes.

## Consolidated Shipped Foundations

The boot-protocol foundations that were previously documented under `TODO-03` are now owned here so the full handoff story stays in one roadmap:

- `boot_info` header at offset 0 with `magic`, `version`, and `size`, populated by the bootloader as the last handoff step.
- Pre-copy kernel validation split into address-phase and header-phase checks, with pure unit tests and mirrored offset assertions across bootloader and kernel.
- Full-range `boot_info` reservation and overlap protection so ELF segments, DMA pages, and later `AllocatePages` calls cannot silently clobber the handoff buffer.
- Memory-map normalization and handoff-shape hardening as prerequisite context for the remaining ownership, payload, and versioning work in this TODO.

---

## Implementation Order

| ⭐  | Order | Deliverable                                        | Depends On                         | Status |
| --- | :---: | -------------------------------------------------- | ---------------------------------- | :----: |
| 💎  |   1   | Canonical boot_info field ownership table          | --                                 |  [x]   |
| 💎  |   2   | Generated ABI manifest and offset fingerprint      | §1                                 |  [ ]   |
| 💎  |   3   | Full bootloader/kernel mirror drift checker        | §2                                 |  [ ]   |
| 💎  |   4   | Optional payload descriptor array                  | §1                                 |  [ ]   |
| 💎  |   5   | Module and initrd handoff contract                 | §4                                 |  [ ]   |
| 💎  |   6   | Handoff memory ownership and PMM reservation table | §4                                 |  [ ]   |
| 💎  |   7   | Version negotiation and stale-loader error path    | §2, T03 §2                         |  [ ]   |
| 💎  |   8   | Boot protocol documentation and schema changelog   | §1, §2, §3, §4, §5, §6, §7         |  [ ]   |
| ⭐  |   9   | ABI fuzz and compatibility tests                   | §2, §7                             |  [ ]   |
| ⭐  |  10   | Cross-domain owner audit for every boot_info field | §1, §2, §3, §4, §5, §6, §7, §8, §9 |  [ ]   |
| 💎  |  11   | Capability negotiation and degraded-feature flags  | §2, §3, §4                         |  [ ]   |
| 💎  |  12   | Common boot-path provenance and decision record    | §1, §4, §7, §11                    |  [ ]   |

---

## 1. Canonical boot_info Field Ownership Table

- [x] Add `docs/boot/boot-info-fields.md` with columns: field, producer, first valid phase, consumer(s), lifetime, owning TODO, validation.
- [x] Mark kernel-populated fields (`degraded_mask`, `hv_flags`, `secure_boot_enabled`) separately from bootloader-populated fields.
- [x] Identify fields that are stale, legacy, or only used by Multiboot2 and decide retain/deprecate.
- [x] Add comments in `boot_info.h` that point to the ownership document rather than duplicating all policy inline.
- [x] Commit: `"docs: boot_info field ownership matrix"`

**Test checkpoint:** `docs/boot/boot-info-fields.md` exists and every `struct boot_info` field is listed with producer, first valid phase, consumer, lifetime, owning TODO, and validation rule. Spot-check `header`, `fb`, `usb_controller`, and TPM-related fields from `boot_info.h`, then confirm QEMU WHPX, QEMU TCG, VirtualBox, and bare-metal boot behavior is unchanged because this section is documentation-only.

> **Notes:**
> - Canonical doc: [`docs/boot/boot-info-fields.md`](../../docs/boot/boot-info-fields.md). Covers every field in `struct boot_info` and its nested structs (`boot_info_header`, `boot_mmap_entry`, `boot_framebuffer`, `boot_gop_mode`, `boot_config`, `boot_uefi_config_entry`, `boot_rt_mem_entry`, `boot_uefi_runtime`, `boot_usb_device`, `boot_usb_endpoint`, `boot_usb_controller`, `timing`) with producer / first-valid-phase / consumer / lifetime / owning-roadmap / validation columns.
> - Kernel-populated vs bootloader-populated split called out explicitly: `secure_boot_enabled`, `degraded_mask`, `hv_flags`, `hv_vendor[]` are the four top-level fields kernel code fills after handoff; readers must gate on the producer having run.
> - Legacy / Multiboot2-only retain/deprecate decisions codified: `mem_lower_kb` / `mem_upper_kb` -> **deprecate** (no kernel consumer today; UEFI mmap supersedes them; flagged for removal in a future `BOOT_INFO_VERSION` bump). `module_start` / `module_end` / `module_available` -> **retain as compatibility aliases** per §4's explicit direction ("Keep legacy single `module_start/module_end` as compatibility aliases until consumers migrate"). Multiboot2 retire/retain itself remains owned by [alternate boot protocols roadmap](../../todo/01-boot-platform/TODO-08-alternate-boot-protocols.md).
> - [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h) file-top comment rewritten to point at the new doc as single source of truth. Per-field policy prose is NOT duplicated inline; the header keeps only ABI-critical static asserts (header offset 0, `uint16_t header.size` fit, six pinned offset mirrors) + the `boot_config` offset table that is load-bearing for the UEFI-kernel bootloader ABI contract.
> - §2 (Generated ABI Manifest and Offset Fingerprint) builds on this matrix next: the producer/consumer columns here feed the manifest diff.

> **Test runner:** docs + one header comment, no kernel test surface. Validation is structural: the file exists at `docs/boot/boot-info-fields.md`; every top-level field in `struct boot_info` (spot-checked `header`, `fb`, `usb_controller`, TPM block) has a row; no Unicode dashes; no numeric-TODO shorthand outside `todo/` (all cross-references use anchor links with named text). The runtime check is inherited: build clean (`=== BUILD OK ===`) proves the ABI static asserts in the header still pass after the comment rewrite. Structural drift protection between this doc and the header lands with the §7 Tooling Doctor / `scripts/test-tooling.sh` regression pack.

> **Verified:** 2026-04-17 -- 5/5 checklist items [x] with diff in commit `8aae7a1b`: [`docs/boot/boot-info-fields.md`](../../docs/boot/boot-info-fields.md) lists every top-level `struct boot_info` field (ABI header, memory map, basic memory, framebuffer, GOP mode list, ACPI, module legacy, boot config, UEFI config table, UEFI runtime services, TPM, USB discovery, timing, serial port, last-boot error, boot device identity, UEFI boot variables, boot partition, removable media) + every nested struct (`boot_info_header`, `boot_mmap_entry`, `boot_uefi_guid`, `boot_uefi_config_entry`, `boot_rt_mem_entry`, `boot_uefi_runtime`, `boot_framebuffer`, `boot_gop_mode`, `boot_config` 23 fields, `boot_usb_endpoint`, `boot_usb_device` 16 fields, `boot_usb_controller` 26 fields, `timing` 18 fields) with the 6-column matrix (producer / first valid phase / consumer / lifetime / owning roadmap / validation). Kernel-populated fields (`secure_boot_enabled`, `degraded_mask`, `hv_flags`, `hv_vendor[]`) called out in a separate table. Legacy / Multiboot2-only retain/deprecate block records concrete decisions. [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h) file-top comment rewritten to name the doc as single source of truth; policy prose removed. Build clean: `=== BUILD OK ===`. Lint delta 0 (no Unicode dashes, no `§`-pattern shorthand outside `todo/`).
> **Accepted:** USB scratchpad count field is unclamped against `BOOT_USB_MAX_SCRATCHPADS` -- producer writes raw xHCI HCSParams2 value, allocator trusts it; controllers reporting >16 scratchpads would allocate beyond the documented ABI surface. -> XREF: [`01-boot-platform/TODO-20 §1` (item: "USB scratchpad clamp" -- clamp in `src/boot/uefi/bootx64.c` near the HCSParams2 read, log serial warning if clamp trips, update canonical matrix `max_scratchpads` row](../../todo/01-boot-platform/TODO-20-usb-zero-delay-handover.md#1-bootloader-allocates-xhci-dma-structures)). Not a functional defect today (all target hardware reports <= 16); filed to fully close the §1 completion radar.
> **Quality reviewed:** 2026-04-17 -- two Codex dispatches (adversarial at step 13 + consistency at step 20). Step 13 caught 2 findings, both fixed before commit: nested structs collapsed into prose (rewrote all nested sections as uniform 7-column per-field tables so every member has a row, explicitly adding `scratchpad_array_phys`, `dma_pages[]`, `alloc_fail_status`, `alloc_fail_page`, `_reserved[12]`, `_pad[223]`); top-level Module block vs Legacy block contradicted on module_* retain/deprecate (aligned both to "retain as compatibility alias per §4"). Step 20 caught 3 more, all fixed this review: `_mmap_pad[2]` row missing from Memory map table (added); `gop_mode_count` validation inconsistent (was `<` in one row, `<=` in another; aligned to `<=` matching the producer's `i < BOOT_GOP_MODE_MAX` loop semantics); `max_scratchpads` validation falsely claimed `<= BOOT_USB_MAX_SCRATCHPADS` bound that the producer does not enforce (rewrote row to describe real producer behavior + filed the clamp as a concrete `[ ]` item in TODO-20 §1). No domain code-quality skill applies (docs + one header comment; no src/kernel, src/boot, src/desktop, or user/ touched). Perf/dead-code N/A for this scope.

---

## 2. Generated ABI Manifest and Offset Fingerprint

- [ ] Add a build step that emits `build/boot-info-abi.json`: struct size, version, every field offset/size, and SHA-256 fingerprint.
- [ ] Generate the same manifest for the UEFI mirror compilation unit.
- [ ] Fail the build if kernel and bootloader manifests disagree.
- [ ] Keep the existing static asserts for early compile failures.
- [ ] Commit: `"boot: generate boot_info ABI manifest"`

**Test checkpoint:** `bash scripts/build.sh` emits `build/boot-info-abi.json` for both kernel and bootloader views, and a forced field reorder or offset change fails the build with the first mismatching field named. After restoring the valid layout, QEMU WHPX, QEMU TCG, VirtualBox, and bare-metal boots remain unchanged.

---

## 3. Full Bootloader/Kernel Mirror Drift Checker

- [ ] Replace the five-field static fingerprint with complete table validation in host tooling.
- [ ] Check nested structs: `boot_config`, `boot_usb_device`, `boot_usb_controller`, runtime memory entries, GOP modes.
- [ ] Add CI output that prints the first differing field on mismatch.
- [ ] Add negative tests with intentionally reordered fixture structs.
- [ ] Commit: `"test: boot_info mirror drift checker"`

**Test checkpoint:** The host drift checker passes for the real kernel/bootloader pair, fails for an intentionally reordered fixture, and prints the first differing nested field rather than only a hash mismatch. QEMU WHPX, QEMU TCG, VirtualBox, and bare-metal boot behavior stays unchanged once the real structs match again.

---

## 4. Optional Payload Descriptor Array

- [ ] Add `boot_payload_desc[]` for typed physical payloads: module, initrd, recovery image, hibernation metadata, TPM log copy, network config blob.
- [ ] Include type, flags, physical start, length, alignment, checksum, and producer.
- [ ] Add descriptor count, total bytes, and unknown-type skip rules so older kernels can ignore future optional payloads without misparsing the array.
- [ ] Keep legacy single `module_start/module_end` as compatibility aliases until consumers migrate.
- [ ] Record the owner for each typed descriptor: USB handover (`T20 §4`), random seed (`T12 §7`), TPM log (`T13 §2`), network provenance (`T25 §6`), and hibernation/resume metadata (`T26 §5`). If a module, initrd, or recovery-image consumer still has no owning section, create that owner before marking this section done.
- [ ] Validate no payload overlaps boot_info, runtime memory, USB DMA, framebuffer, or reserved crash regions.
- [ ] Commit: `"boot: add typed payload descriptors"`

**Test checkpoint:** Kernel boot logs show typed payload descriptors with stable type, start, and length values; overlap attempts against `boot_info`, runtime memory, USB DMA, framebuffer, or crash-reserved regions are rejected with a clear serial diagnostic. Verify descriptor ingestion on QEMU WHPX, QEMU TCG, VirtualBox, and bare metal.

---

## 5. Module and initrd Handoff Contract

- [ ] Define kernel module payload type for early `.kmod` or recovery helpers.
- [ ] Define initrd/recovery payload type for recovery shell, driver bundles, and installer assets.
- [ ] Add bootloader loading syntax in `boot.conf`: `module=`, `initrd=`, `recovery_image=`.
- [ ] Add kernel-side enumeration API: `boot_payload_find(type, index)`.
- [ ] Commit: `"boot: module and initrd handoff contract"`

**Test checkpoint:** A boot image with `module=`, `initrd=`, and `recovery_image=` entries exposes the expected descriptor types, `boot_payload_find(type, index)` returns the correct start/length pair, and missing or malformed entries fail with a specific bootloader error. Verify on QEMU WHPX, QEMU TCG, VirtualBox, and bare metal.

---

## 6. Handoff Memory Ownership and PMM Reservation Table

- [ ] Create a single `boot_reserved_region` table populated from boot_info before PMM frees memory.
- [ ] Include boot_info itself, USB DMA pages, TPM log copy, payload descriptors, runtime services regions, framebuffer, survival logs.
- [ ] Teach PMM to log every retained region and fail on overlap.
- [ ] Add a boot diagnostic dump to BlackBox.
- [ ] Commit: `"boot: centralize handoff memory reservations"`

**Test checkpoint:** Early PMM logs enumerate every retained boot region exactly once, overlap attempts fail before free-memory handoff, and BlackBox captures the reservation dump for post-boot inspection. Verify the reservation table on QEMU WHPX, QEMU TCG, VirtualBox, and bare metal.

---

## 7. Version Negotiation and Stale-Loader Error Path

- [ ] Define compatibility policy: exact version required for boot, or explicit downgrade adapter.
- [ ] Add bootloader-side display for kernel ABI mismatch before jump when possible.
- [ ] Add kernel-side fatal screen with observed/expected version, size, and manifest hash.
- [ ] Persist mismatch reason in UEFI NVRAM and BlackBox.
- [ ] Commit: `"boot: hard fail stale boot protocol versions"`

**Test checkpoint:** A stale `BOOTX64.EFI` paired with a current kernel, and a stale kernel paired with a current bootloader, both stop with the expected observed/expected version, size, and manifest-hash diagnostics instead of hanging. Verify the friendly failure path on QEMU WHPX, QEMU TCG, VirtualBox, and at least one bare-metal system using removable-media recovery workflow.

---

## 8. Boot Protocol Documentation and Schema Changelog

- [ ] Add `docs/boot/boot-protocol.md` with struct lifecycle, phases, pointer validity, and examples.
- [ ] Add `docs/boot/boot-protocol-changelog.md` keyed by `BOOT_INFO_VERSION`.
- [ ] Link every version bump to a TODO and commit.
- [ ] Document how third-party bootloaders can populate a minimal supported handoff.
- [ ] Publish a compatibility matrix that points alternate boot adapters at `T08 §1-§4` and names which payloads, capability flags, and provenance fields are required, optional, or unsupported outside the native UEFI path.
- [ ] Commit: `"docs: boot protocol ABI reference"`

**Test checkpoint:** `docs/boot/boot-protocol.md` and `docs/boot/boot-protocol-changelog.md` exist, each `BOOT_INFO_VERSION` bump points to the owning TODO and commit, and the minimal third-party bootloader contract lists the exact fields and validation steps required for a supported handoff. Confirm QEMU WHPX, QEMU TCG, VirtualBox, and bare-metal boots are unchanged because this section is documentation-only.

---

## 9. ABI Fuzz and Compatibility Tests

- [ ] Fuzz `boot_info_validate_addr()` and `boot_info_validate_header()` with malformed sizes, addresses, versions, and overlap cases.
- [ ] Boot QEMU with intentionally stale `BOOTX64.EFI` and assert friendly error output.
- [ ] Boot QEMU with intentionally stale `kernel.exe` and assert friendly error output.
- [ ] Add tests for optional payload overlap rejection.
- [ ] Commit: `"test: boot protocol ABI fuzz coverage"`

**Test checkpoint:** `bash scripts/test.sh SUITE=boot` covers malformed address, size, version, and overlap cases, and the stale-image QEMU fixtures assert the exact mismatch strings rather than a generic boot halt. Re-run the positive boot path on QEMU WHPX, QEMU TCG, VirtualBox, and bare metal after the negative fixtures pass.

---

## 10. Cross-Domain Owner Audit for Every boot_info Field

- [ ] Audit every field in `boot_info.h` and add an XREF to the owning TODO.
- [ ] Trace legacy scalar aliases such as `module_start/module_end` to their current Multiboot2-only producer in `src/kernel/multiboot2_parse.c` and either mark them compatibility-only (`T08 §3`) or retire them once typed payloads land.
- [ ] Remove or deprecate fields with no owner and no consumer.
- [ ] Update `GAP-ANALYSIS.md` closure table when all fields have owners.
- [ ] Commit: `"docs: close boot_info ownership audit"`

**Test checkpoint:** Every `boot_info` field in `include/kernel/boot_info.h` resolves to one owner section or deprecation note, `todo/01-boot-platform/GAP-ANALYSIS.md` reflects the closure state, and no field remains ownerless in docs or comments. Confirm QEMU WHPX, QEMU TCG, VirtualBox, and bare-metal boots still consume the audited fields without behavior changes.

---

## 11. Capability Negotiation and Degraded-Feature Flags

The handoff needs more than a version number. The loader and kernel must distinguish optional data from required data, and alternate boot adapters must declare what they could not provide instead of silently zeroing fields.

> [!WARNING]
> This is boot-path ABI work. Unknown required bits must hard-fail before Phase 0 consumes unsupported data, while unknown optional bits must be safely ignored so native UEFI, alternate-protocol adapters, and stale images can evolve without paper compatibility.
> **Regression risk:** HIGH. Shared capability bits cross bootloader, kernel, and alternate loaders. A mismatched required/optional policy can turn a recoverable omission into a false boot halt or, worse, silent misinterpretation. Reserve `POST16(0xB096)` / `POST16(0xB097)` for capability negotiation entry/success if this lands in the native bootloader path.

- [ ] Define `boot_info` capability words for required, present, and degraded features: payload descriptors, runtime services, Secure Boot state, TPM log, USB handover, media role, network provenance, resume metadata, and alternate-protocol adapter mode.
- [ ] Emit capability words, required-version range, and reserved-bit policy in the generated ABI manifest so compatibility decisions do not rely on struct-size checks alone.
- [ ] Require alternate boot adapters (-> XREF: `T08 §3,§4`) to set capability bits explicitly and mark missing UEFI-only data as degraded instead of silently leaving fields zero.
- [ ] Add bootloader and kernel compatibility handling for unknown required bits, unknown optional bits, and manifest capability mismatches before Phase 0 starts consuming dependent fields.
- [ ] Add ABI tests for ignore-unknown-optional, reject-unknown-required, and stale-manifest capability mismatch cases.
- [ ] Commit: `"boot: add boot_info capability negotiation"`

**Test checkpoint:** A current kernel plus current bootloader negotiates all required capabilities successfully, an older kernel ignores newly added optional bits, and an image with an unknown required bit fails with a specific compatibility diagnostic before Phase 0 dereferences unsupported data. Verify on QEMU WHPX, QEMU TCG, VirtualBox, and bare metal, plus a GRUB/Multiboot2 adapter fixture from `T08 §3`.

---

## 12. Common Boot-Path Provenance and Decision Record

Media role, recovery, network boot, and resume each carry their own details, but the ABI still needs one shared record that answers what path was selected and why.

> [!NOTE]
> **Regression risk:** MEDIUM. This stitches together several existing producers. Schema drift here will not usually crash boot, but it can misclassify recovery or resume paths and break diagnostics, rollback policy, or attestation narratives. Reserve `POST16(0xB098)` / `POST16(0xB099)` for decision-record entry/success if the native bootloader writes this schema.

- [ ] Add common `boot_path`, `boot_reason`, `boot_source_flags`, and `boot_fallback_depth` fields to `boot_info` covering cold boot, installer, recovery, network, resume, fast startup, and diagnostic flows.
- [ ] Define one shared provenance record that `T06 §6`, `T22 §2`, `T25 §6`, and `T26 §5` populate alongside their payload-specific descriptors, with stable enums for selected path and policy reason.
- [ ] Include the inputs that drove the decision: `BootCurrent`/`BootNext`, media role, rollback or recovery trigger, resume invalidation reason, network insecure flag, and manifest or measured-boot status.
- [ ] Export the common decision record to `HKLM\SYSTEM\Boot\Decision` and BlackBox so recovery, attestation, and rollback logic can explain why the current path was chosen.
- [ ] Add round-trip tests for cold boot, recovery boot, network boot, and resume fixtures so the shared reason codes stay synchronized across bootloader and kernel consumers.
- [ ] Commit: `"boot: add common boot decision record"`

**Test checkpoint:** Kernel logs and Registry/BlackBox output agree on the selected boot path and reason for a normal cold boot, a forced recovery boot, a network boot, and a rejected resume that falls back to cold boot. Verify on QEMU WHPX, QEMU TCG, VirtualBox, and bare metal for the native path; use fixtures where the platform-specific producer is not implemented yet.

---

## OS Comparison

| ⭐ | Feature                         | 🪟 Win11                  | 🐧 Linux                     | 🚀 Impossible OS |
| --- | ------------------------------- | ------------------------- | ----------------------------- | ---------------- |
| 💎 | Versioned loader/kernel ABI     | ✅ LPB + extensions       | ✅ boot_params + kernel_info | ⬜ §1 §7 §8      |
| 💎 | Typed initrd and module handoff | ✅ ramdisk + boot drivers | ✅ initrd + initramfs        | ⬜ §4 §5         |
| ⭐ | Generated ABI manifest          | ⚠️ internal only          | ⚠️ docs + CI                 | ⬜ §2 §3         |
| ⭐ | Field-level ownership map       | ⚠️ internal ownership     | ⚠️ scattered docs            | ⬜ §1 §10        |
| 💎 | Capability negotiation          | ✅ loader extensions      | ✅ version + flags           | ⬜ §11           |
| 💎 | Boot provenance decision record | ✅ boot status + resume   | ⚠️ cmdline + logs            | ⬜ §12           |
| ⭐ | Friendly stale-loader mismatch  | ✅ recovery codes         | ⚠️ log-driven failures       | ⬜ §7            |

> Parity now covers the contract itself and the decisions made around it. Adding explicit capability negotiation and a shared boot decision record would make this handoff easier to debug and safer to evolve than either Windows' mostly internal loader state or Linux's split between versioned structs and scattered provenance channels.

## Unit Tests

> Wire into `test_runner_init()` through the existing `test_register_boot_info()` boot suite in `src/kernel/test/test_runner.c`, and extend that registration when new `TEST_CAT_BOOT` cases land in `src/kernel/test/test_boot_info.c` or the follow-on boot-protocol test file.
> Host-side manifest/drift tooling should also get fixture-based negative tests, but the kernel-facing ABI validation stays owned by the boot suite.

- [ ] Keep the existing validator coverage in `src/kernel/test/test_boot_info.c`: `boot_info_validate_addr((const void *)0x10000, sizeof(struct boot_info), BOOT_INFO_EARLY_MAP_END) == BOOT_OK`, bad magic/version/size headers return `BOOT_FATAL`, and the combined validator short-circuits misaligned pointers before header reads.
- [ ] Add `test_boot_info_manifest_kernel_bootloader_match`: generated kernel and bootloader ABI manifests contain the same struct size, version, per-field offsets, and fingerprint.
- [ ] Add `test_boot_info_payload_overlap_rejected`: a payload descriptor that overlaps `boot_info`, framebuffer, USB DMA, runtime memory, or crash-reserved regions is rejected with the expected failure code.
- [ ] Add `test_boot_info_payload_find_second_entry`: `boot_payload_find(type, index)` returns the second descriptor of a repeated type with the expected `phys_start` and `length`.
- [ ] Add `test_boot_info_stale_version_error_fields`: an ABI mismatch report preserves observed version, expected version, struct size, and manifest hash for the fatal screen and persisted diagnostics.
- [ ] Add `test_boot_info_capability_unknown_optional_ignored`: unknown optional capability bits do not reject a newer handoff when all required bits are understood.
- [ ] Add `test_boot_info_capability_unknown_required_rejected`: unknown required capability bits fail before dependent fields are consumed.
- [ ] Add `test_boot_info_boot_decision_round_trip`: shared `boot_path` and `boot_reason` values survive bootloader-to-kernel handoff and Registry export without enum drift.
- [ ] Commit: `"test: extend boot_info ABI coverage"`

**Test checkpoint:** `bash scripts/test.sh SUITE=boot` exercises the validator, payload, and mismatch-report cases through `TEST_CAT_BOOT`, and the host manifest fixtures fail only when the layout actually drifts.

## Verification

- [ ] `bash scripts/build.sh` completes and the ABI manifest/drift tooling reports no differences for the real kernel and bootloader pair.
- [ ] `bash scripts/test.sh SUITE=boot` passes with `test_register_boot_info()` and the new boot-protocol cases enabled.
- [ ] QEMU WHPX and QEMU TCG both reject a stale `BOOTX64.EFI` with observed/expected version, size, and manifest-hash diagnostics.
- [ ] QEMU WHPX and QEMU TCG both reject a stale `kernel.exe` with the same friendly mismatch path.
- [ ] QEMU GRUB/Multiboot2 adapter path either populates capability bits and degraded flags correctly or fails with an explicit unsupported-protocol message.
- [ ] Recovery, network, installer, and resume fixtures all produce the same `HKLM\SYSTEM\Boot\Decision` / BlackBox schema for selected path and reason.
- [ ] VirtualBox boots a matching image and logs the retained boot reservations plus typed payload descriptors without overlap warnings.
- [ ] Bare metal boots a matching image with USB handoff and TPM log payloads present, and PMM retains those regions exactly once.

**Test runner:** `scripts\debug\run-boot-tests.bat` (SUITE=boot)
