---
schema_version: 1
id: boot-protocol-abi-handoff
domain: 01-boot-platform
status: active
title: "TODO-01 -- Boot Protocol ABI & Handoff Contract"
---

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
- Anti-rollback security-version binding prevents loading older-signed kernels once policy has been advanced (parity with Windows Loader Parameter Block `OsLoaderSecurityVersion`).
- Warm-kernel-update handoff is reachable through the same `boot_info` contract as cold boot (competitive edge over Windows Hot Patch's closed path and parity with Linux 6.16's Kexec Handover).

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
| 💎  |   2   | Generated ABI manifest and offset fingerprint      | §1                                 |  [x]   |
| 💎  |   3   | Full bootloader/kernel mirror drift checker        | §2                                 |  [x]   |
| 💎  |   4   | Optional payload descriptor array                  | §1                                 |  [x]   |
| 💎  |   5   | Module and initrd handoff contract                 | §4                                 |  [x]   |
| 💎  |   6   | Handoff memory ownership and PMM reservation table | §4                                 |  [x]   |
| 💎  |   7   | Version negotiation and stale-loader error path    | §2, T03 §2                         |  [/]   |
| 💎  |   8   | Boot protocol documentation and schema changelog   | §1, §2, §3, §4, §5, §6, §7         |  [x]   |
| ⭐  |   9   | ABI fuzz and compatibility tests                   | §2, §7                             |  [/]   |
| ⭐  |  10   | Cross-domain owner audit for every boot_info field | §1, §2, §3, §4, §5, §6, §7, §8, §9 |  [x]   |
| 💎  |  11   | Capability negotiation and degraded-feature flags  | §2, §3, §4                         |  [x]   |
| 💎  |  12   | Common boot-path provenance and decision record    | §1, §4, §7, §11                    |  [x]   |
| 💎  |  13   | Anti-rollback and security-version binding         | §1, §2, §7                         |  [ ]   |
| ⭐  |  14   | Warm-kernel-update handoff ABI                     | §1, §2, §4                         |  [ ]   |

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
> - Matrix maintenance contract: when `struct boot_info` or any nested struct gains a field in ANOTHER TODO's section, the owning section MUST update this matrix in the same commit. A Codex pass 2026-04-23 found 14 missing `boot_config` fields that had landed via TODO-04 test-framework sections (`tap`, `utest_timeout_ms`, `utest_filter`, `utest_isolation`, `xml`, `json`, `stress_iters`, `test_kernel_skip`, `test_usermode_skip`, and their pads) + TODO-05 desktop-UI-test-framework sections (`compositor`, `test_monitors_count`); drift was silent because no CI gate checks doc coverage against the live struct. Re-reviewed sections: TODO-04 and TODO-05 owners should back-reference this invariant in their own completion gates; §10 (Cross-Domain Owner Audit for Every boot_info Field) closes the loop by making coverage explicit.

> **Verified:** 2026-04-17 | commit `8aae7a1b` | 5/5 items | build OK | full nested-struct matrix
> **Re-verified:** 2026-04-23 | matrix refreshed for 14 `boot_config` fields added by TODO-04 + TODO-05 after original ship; `_reserved[12]` -> `_reserved[10]`, `_pad[223]` -> `_pad[142]` corrected; `test_suite` validation column updated for `9`=exec, `10`=x86, `11`=desktop; `test_monitors_count` row downgraded to "parsed/stored; no runtime consumer today" matching current code-truth. `BOOT_INFO_VERSION` remains 6 (layout unchanged; only doc coverage).
> **Accepted:** [M] USB scratchpad count field unclamped against `BOOT_USB_MAX_SCRATCHPADS` (reason: not-functional-today -- all target HW reports <= 16) -> XREF: 01-boot-platform/TODO-20 §1 (item: "USB scratchpad clamp" at line 51 -- clamp in `src/boot/uefi/bootx64.c` near HCSParams2 read, log serial warning on clamp, update canonical matrix `max_scratchpads` row) -- [TODO-20 §1 anchor](../../todo/01-boot-platform/TODO-20-usb-zero-delay-handover.md#1-bootloader-allocates-xhci-dma-structures)
> **Deferred:** [H] Drift-prevention gate is documentation-only, so the same 14-field silent-drift failure mode can recur (reason: infra -- needs a build-time diff between `tools/boot-info-manifest/dump-fields.inc` and the matrix) -> XREF: 01-boot-platform/TODO-01 §10 (item: "Add a build-time drift gate that diffs the live struct boot_info field set" -- new item filed this review, spells out the `check-doc-coverage.sh` script and `scripts/build.sh` hook)
> **Quality reviewed:** 2026-04-17 | Codex 2x (adversarial + consistency) | 5M fixed, 0 open | scope: N/A (docs + one header comment)
> **Quality re-reviewed:** 2026-04-23 | Codex 2x (adversarial + quality post-commit) | 1H+2M fixed, 1H deferred (drift gate -> §10) | scope: N/A (docs refresh)
> **Test runner:** N/A (docs-only) | validation: structural -- doc exists, every top-level field has a row, build clean proves header asserts pass

---

## 2. Generated ABI Manifest and Offset Fingerprint

- [x] Add a build step that emits `build/boot-info-abi.json`: struct size, version, every field offset/size, and SHA-256 fingerprint.
- [x] Generate the same manifest for the UEFI mirror compilation unit.
- [x] Fail the build if kernel and bootloader manifests disagree.
- [x] Keep the existing static asserts for early compile failures.
- [x] Commit: `"boot: generate boot_info ABI manifest"`

**Test checkpoint:** `bash scripts/build.sh` emits `build/boot-info-abi.json` for both kernel and bootloader views, and a forced field reorder or offset change fails the build with the first mismatching field named. After restoring the valid layout, QEMU WHPX, QEMU TCG, VirtualBox, and bare-metal boots remain unchanged.

> **Notes:**
> - Split the ABI artifact into two files rather than one because kernel view and bootloader view each need their own authoritative source: [`build/boot-info-abi.kernel.json`](../../build/boot-info-abi.kernel.json) (emitted by [`tools/boot-info-manifest/dump-kernel.c`](../../tools/boot-info-manifest/dump-kernel.c)) and [`build/boot-info-abi.mirror.json`](../../build/boot-info-abi.mirror.json) (emitted by [`tools/boot-info-manifest/dump-mirror.c`](../../tools/boot-info-manifest/dump-mirror.c)). Each JSON has the shape `{ view, version, struct_size, fields: [...], sha256 }` with 212 field rows (each row: name, offset, size). The row set includes representative indexed leaves for every array-of-struct member (`mmap[0].*`, `gop_modes[0].*`, `config_table[0].guid.*` + `table_addr`, `rt_mmap[0].*`, `usb_devices[0].*` plus nested `usb_devices[0].endpoints[0].*`) AND element-size sentinels for every scalar array (`usb_controller.dma_pages[0]`, `uefi_boot_order[0]`, `boot_device_path[0]`, `boot_partition_guid[0]`, `hv_vendor[0]`, `config.cmdline[0]`, and every padding `[0]`) so a same-size element-type change (for example `uint64_t[16]` -> `uint32_t[32]`, both 128 bytes) cannot preserve the whole-array row and slip past.
> - [`tools/boot-info-manifest/compare.sh`](../../tools/boot-info-manifest/compare.sh) diffs the two JSONs with `python3`, prints one-line PASS on success or FAIL with the first mismatching field named (including both offsets/sizes for triage). Invoked by a new `boot-info-abi` Makefile target and a new `boot_info ABI` stage in [`scripts/build.sh`](../../scripts/build.sh), placed after `EFI Signing` and before `System Disk`.
> - Bootloader mirror was previously duplicated inline at the top of [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c); extracted into [`src/boot/uefi/boot_info_mirror.h`](../../src/boot/uefi/boot_info_mirror.h) so both the bootloader compilation unit AND the host manifest dumper share the same authoritative source. Any field reorder, rename, or offset change on either side now trips the manifest diff with the first failing field.
> - [`tools/boot-info-manifest/dump-fields.inc`](../../tools/boot-info-manifest/dump-fields.inc) enumerates every field exactly once. Both dumpers #include it; the shared list guarantees row-for-row comparability. Adding or removing a field in the struct requires a matching F(...) line here -- the mismatch check surfaces any skew.
> - Existing `_Static_assert` offset pins (22 in the kernel header, 23 in the mirror, covering ABI header + size fit + count fields + `boot_config` layout) stay in place as the compile-time first line of defense. The manifest catches reorders that keep the pinned offsets stable but shuffle fields between them.
> - SHA-256 fingerprint is canonical and view-independent (2026-04-23 rework): the hash is computed from the tuple `(version, struct_size, per-field (name, offset, size))` in emission order via a fixed delimited byte stream prefixed `BOOTINFO-ABI-V1\n`, never from the JSON bytes. Kernel and mirror views therefore produce IDENTICAL sha256 on a matching ABI -- usable as a single-value ABI-version token in release notes and CI dashboards. Compare.sh still diffs field-by-field; the sha256 is advisory for humans and regression-tested in `test-drift-detection.sh`'s control case.
> - [`include/kernel/types.h`](../../include/kernel/types.h) gained a minimal `#if !defined(UINT8_MAX)` guard so the kernel-view dumper can `#include <stdint.h>` + `kernel/types.h` without duplicate-typedef errors. Freestanding kernel build is unchanged (`UINT8_MAX` is never defined without host `<stdint.h>`).
> - Kernel header's file-top comment now explicitly names the mirror header + the manifest + the `dump-fields.inc` entry as the three places that must change together when adding or moving a field. `docs/boot/boot-info-fields.md` (the §1 canonical matrix) has a new intro paragraph pointing at the machine-readable complement.
> - Drift regression is now codified in [`tools/boot-info-manifest/test-drift-detection.sh`](../../tools/boot-info-manifest/test-drift-detection.sh) (Makefile target `test-boot-info-abi`): 5 mutation cases (`mmap-type-swap`, `dma-pages-element-shrink`, `endpoint-address-swap`, `gop-mode-pixels-per-scanline-shrink`, `rt-mmap-num-pages-shrink`) plus a negative control and a canonical-hash invariant assertion (kernel sha256 == mirror sha256). 7/7 PASS. Post-2026-04-23 rework, the harness regenerates `build/boot-info-abi.kernel.json` from current headers before each mutation run (prefers the Makefile rule, falls back to inline `HOST_CC` compile) so a stale baseline cannot mask drift.
> - Non-functional: no runtime kernel / bootloader code changed beyond the extraction. Smoke test passes on KVM (2.110s to `C:\>`).

> **Verified:** 2026-04-17 | commit `97bcc762` | 5/5 items | build OK | 213 fields, struct_size 22144, version 5
> **Re-verified:** 2026-04-23 | commit `efadd326` | 5/5 items | build OK | 225 fields, struct_size 23696, version 6 (refreshed after §4 boot_config / §5 typed payload ABI bumps); canonical SHA-256 rework (H1) + drift-harness baseline regen (M1) applied in pass 3
> **Quality reviewed:** 2026-04-17 | Codex 2x (adversarial + quality) | 1H+1M+2L fixed, 0 open | scope: N/A (host tools + layout-preserving refactor)
> **Quality re-reviewed:** 2026-04-23 | Codex 2x (adversarial pass 3 + quality pass 3) | 1H+1M fixed, 0 open | scope: N/A (host tools -- hash canonicalization + test harness)
> **Test runner:** `make test-boot-info-abi` | 7/7 scenarios PASS (5 mutations + control + sha256 canonicality invariant); `bash scripts/build.sh` runs the compare step every build as the first-line gate

---

## 3. Full Bootloader/Kernel Mirror Drift Checker

- [x] Replace the five-field static fingerprint with complete table validation in host tooling.
- [x] Check nested structs: `boot_config`, `boot_usb_device`, `boot_usb_controller`, runtime memory entries, GOP modes.
- [x] Add CI output that prints the first differing field on mismatch.
- [x] Add negative tests with intentionally reordered fixture structs.
- [x] Commit: `"test: boot_info mirror drift checker"`

**Test checkpoint:** The host drift checker passes for the real kernel/bootloader pair, fails for an intentionally reordered fixture, and prints the first differing nested field rather than only a hash mismatch. QEMU WHPX, QEMU TCG, VirtualBox, and bare-metal boot behavior stays unchanged once the real structs match again.

> **Notes:**
> - Items 1-3 were satisfied incrementally by §2's implementation (commits `97bcc762` + `90c29907`): the 225-row manifest (post-§4/§5 ABI bump) emitted by [`tools/boot-info-manifest/dump-kernel.c`](../../tools/boot-info-manifest/dump-kernel.c) + [`dump-mirror.c`](../../tools/boot-info-manifest/dump-mirror.c) IS the "complete table validation" (item 1), covers every nested struct called out in item 2 -- `config_table[0].guid` (6 rows), `rt_mmap[0]` (7 rows), `gop_modes[0]` (7 rows), `boot_config` (~48 rows with post-§4 additions), `usb_devices[0]` + nested `endpoints[0]` (22 rows), `usb_controller` (29 rows) -- and already prints `FAIL boot_info ABI manifest: field #N 'name' diverges on 'key' -- kernel: name@off+size; mirror: name@off+size` via [`tools/boot-info-manifest/compare.sh`](../../tools/boot-info-manifest/compare.sh) (item 3). The 22 `_Static_assert` offset pins in [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h) and 23 pins in [`src/boot/uefi/boot_info_mirror.h`](../../src/boot/uefi/boot_info_mirror.h) (ABI header + size fit + count fields + `boot_config` layout) remain as the compile-time first line of defense.
> - Item 4 is the net-new work: [`tools/boot-info-manifest/test-drift-detection.sh`](../../tools/boot-info-manifest/test-drift-detection.sh) regress-tests the drift-detector itself against five intentionally-broken fixtures + one negative control. Each fixture mutates the real [`src/boot/uefi/boot_info_mirror.h`](../../src/boot/uefi/boot_info_mirror.h) via `sed` into a tempfile, compiles a one-off mirror dumper pointed at the mutated header, runs `compare.sh` against the real kernel JSON, and asserts exit non-zero + expected first-mismatch field name. Mutations cover:
>   1. Element-struct same-size swap inside `boot_mmap_entry` (UINT32 `type` <-> `uefi_memory_type`) -- expects `mmap[0].type`.
>   2. Scalar-array element-type shrink (`UINT64 dma_pages[16]` -> `UINT32 dma_pages[32]`, both 128 bytes) -- expects `usb_controller.dma_pages[0]`.
>   3. Nested-struct same-size swap inside `boot_usb_endpoint` (UINT8 `address` <-> `attributes`) -- expects `usb_devices[0].endpoints[0].address`.
>   4. Nested-struct element-type shrink inside `boot_gop_mode` (`UINT32 pixels_per_scanline` -> `UINT16 + UINT16 pad`) -- expects `gop_modes[0].pixels_per_scanline`.
>   5. Nested-struct element-type shrink inside `boot_rt_mem_entry` (`UINT64 num_pages` -> `UINT32 + UINT32 pad`) -- expects `rt_mmap[0].num_pages`.
> - Wired via new `test-boot-info-abi` Makefile target + [`scripts/test-tooling.sh`](../../scripts/test-tooling.sh) surface checks (harness exists, `--help` exits 0 and documents exit codes, Makefile target + `.PHONY` entry present) + new [`.github/workflows/build.yml`](../../.github/workflows/build.yml) step "Run boot_info ABI drift tests" that runs after the build (needs `build/boot-info-abi.kernel.json`). test-tooling.sh cannot run the full harness because it executes BEFORE the build stage in CI.
> - Negative control (unmutated mirror must PASS `compare.sh`) protects against a regression where every mutation case fails regardless of the mutation. 2026-04-23 added a sha256-canonicality assertion to the control case (kernel sha256 == mirror sha256) so a future regression into view-dependent hash bytes trips the harness instead of silently desynchronizing.
> - Mutation design constraint: mutations that shift ANY of the pinned count-field offsets fail at compile time via `_Static_assert` before reaching `compare.sh`. All five chosen mutations are leaf-level within nested structs so the pinned offsets stay intact and the manifest pathway is the actual thing under test.
> - **Host-arch gate (2026-04-23 Codex pass 3):** `dump-kernel.c` includes the kernel header using HOST_CC layout rules, so HOST_CC must produce x86_64 System V ABI to match the target kernel (`clang --target=x86_64-elf`). Gate implementation: `$(HOST_CC) -dumpmachine` must report `x86_64*` or `amd64*`, evaluated at recipe execution time inside a single shell line so `make HOST_MACHINE=...` cannot spoof it. Applied to both dumper recipes AND to the phony entry points (`boot-info-abi`, `test-boot-info-abi`) so cached JSONs from a non-x86_64 build cannot bypass the gate via timestamp freshness. `-m64` retained as secondary guard. Harness replicates the probe at startup and fails closed with exit 2 and clear error. Verified by simulating a fake HOST_CC that lies in -dumpmachine: gate fires, no manifest produced.

> **Verified:** 2026-04-17 | commits `97bcc762` + `90c29907` + `a11aa533` | 5/5 items | build OK | drift harness 6/6 (5 mutations + negative control), test-tooling 35->40
> **Re-verified:** 2026-04-23 | commit `cef7233d` | 5/5 items | build OK | drift harness 7/7 (5 mutations + control + sha256 canonicality invariant), test-tooling 82/82; host-arch gate added to dumper recipes + phony entry points + harness startup
> **Quality reviewed:** 2026-04-17 | Codex 2x (adversarial + quality) | 3M fixed, 0 open | scope: N/A (host shell tooling)
> **Quality re-reviewed:** 2026-04-23 | Codex 4x (adversarial + 3 quality rounds for iterative hardening) | 1H fixed, 0 open | scope: N/A (host tools -- Makefile + shell)
> **Test runner:** `make test-boot-info-abi` | 7/7 scenarios PASS (5 mutations + control + canonical-sha invariant); host-arch gate fails closed on non-x86_64 HOST_CC

---

## 4. Optional Payload Descriptor Array

- [x] Add `boot_payload_desc[]` for typed physical payloads: module, initrd, recovery image, hibernation metadata, TPM log copy, network config blob.
- [x] Include type, flags, physical start, length, alignment, checksum, and producer.
- [x] Add descriptor count, total bytes, and unknown-type skip rules so older kernels can ignore future optional payloads without misparsing the array.
- [x] Keep legacy single `module_start/module_end` as compatibility aliases until consumers migrate.
- [x] Record the owner for each typed descriptor: USB handover ([`01-boot-platform/TODO-20 §4`](TODO-20-usb-zero-delay-handover.md#4-boot_info-passes-controller--device-dma-state)), random seed ([`01-boot-platform/TODO-12 §7`](TODO-12-early-entropy-random-seed.md#7-boot_info-seed-handoff)), TPM log ([`01-boot-platform/TODO-13 §1`](TODO-13-tpm-measured-boot-attestation.md#1-harden-tcg-event-log-parser)), network provenance ([`01-boot-platform/TODO-25 §6`](TODO-25-network-pxe-http-boot.md#6-boot_info-network-provenance)), hibernation/resume metadata ([`01-boot-platform/TODO-26 §5`](TODO-26-hibernation-resume-fast-startup-handoff.md#5-boot_info-resume-handoff)), module + initrd ([`01-boot-platform/TODO-01 §5`](#5-module-and-initrd-handoff-contract)), recovery image ([`01-boot-platform/TODO-22 §2`](TODO-22-recovery-partition.md#2-recovery-bootloader)).
- [x] Validate no payload overlaps boot_info, runtime memory, USB DMA, framebuffer, or reserved crash regions.
- [x] Commit: `"boot: add typed payload descriptors"`

**Test checkpoint:** Kernel boot logs show typed payload descriptors with stable type, start, and length values; overlap attempts against `boot_info`, runtime memory, USB DMA, framebuffer, or crash-reserved regions are rejected with a clear serial diagnostic. Verify descriptor ingestion on QEMU WHPX, QEMU TCG, VirtualBox, and bare metal.

> **Notes:**
> - ABI surface added to [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h): `enum boot_payload_type` (9 values), `BOOT_PAYLOAD_FLAG_*` (4 known bits + MASK_KNOWN), `enum boot_payload_producer`, `struct boot_payload_desc` (48-byte ABI-pinned), `enum boot_payload_error` (13 failure classes post-2026-04-23 `NONE_NOT_EMPTY` addition), and `boot_payload_validate()`. `BOOT_INFO_VERSION` bumped 5 -> 6; mirror in [`src/boot/uefi/boot_info_mirror.h`](../../src/boot/uefi/boot_info_mirror.h) matches byte-for-byte. Manifest field count: 213 -> 225. Struct size: 22144 -> 23696 bytes (well under the 65535 `header.size` cap).
> - The legacy `module_start` / `module_end` / `module_available` fields remain populated by the multiboot2 path AS compatibility aliases per item 4; a future §5 migration replaces them with typed descriptors of type `BOOT_PAYLOAD_MODULE`. No removal in this commit.
> - Packed-prefix invariant: `payload_count` bounds the ACTIVE prefix. An occupied slot at index >= `payload_count` is rejected by the validator before consumers run, so future consumers that scan the full array cannot be tricked into processing an unchecked descriptor. Tested via `test_payload_prefix_violated`.
> - Unknown-type / unknown-flag policy: forward-compatible by default (skip type-specific validation for unknown types), but `BOOT_PAYLOAD_FLAG_REQUIRED` forces strict checking so a newer bootloader can insist "fail boot rather than silently ignore this payload". Tested via `test_payload_unknown_type_required_fatal`, `test_payload_unknown_type_optional_accepted`, `test_payload_unknown_flags_required_fatal`.
> - Overlap check runs against every retained boot region regardless of type -- `boot_info` at 0x10000, every populated `rt_mmap[]` entry (using `num_pages * 4096`), every populated `usb_controller.dma_pages[]` page, and the linear framebuffer (`fb.addr` .. `fb.addr + pitch*height`) when `fb_available`. `pitch*height` and `num_pages*4096` multiplications are overflow-checked. Tested via `test_payload_overlap_{boot_info, framebuffer, rt_mmap, usb_dma}`.
> - Validator recomputes `payload_total_bytes` across occupied slots and rejects a producer/consumer disagreement (`test_payload_total_mismatch`). This catches a bootloader bug that would otherwise cascade into PMM reservation mis-sizing in §6.
> - No "crash-reserved" RAM region exists today -- BlackBox is on disk, not memory. Kernel ELF range is reserved by §6 (PMM reservation table) once that section lands; §4's overlap list will extend there. Today the validator rejects every retained region that boot_info itself advertises.
> - Producer state: the UEFI bootloader currently emits `payload_count=0` via the existing `efi_memset(g_boot_info_ptr, 0, sizeof(struct boot_info))` at bootx64.c:4313 -- zero valid descriptors, empty total. The validator short-circuits on count=0 with BOOT_OK. Real payload producers are future work owned by §5 / §20 / §26 / §25 / §13 / §12 (cross-TODO XREFs all updated in this commit).
> - Reciprocal cross-TODO XREFs added in this commit: TODO-12 §7, TODO-13 §1, TODO-20 §4, TODO-22 §2, TODO-25 §6, TODO-26 §5 each gained a `BOOT_PAYLOAD_*` checklist-item note + back-link to §4.
> - **2026-04-23 Codex pass 3 hardening:** NONE-is-empty invariant was too loose. Original `descriptor_is_occupied()` treated a slot with `type=BOOT_PAYLOAD_NONE` as unoccupied iff `length==0`, so a bootloader could smuggle non-zero `flags`/`phys_start`/`producer_id`/`alignment` into a NONE slot and the validator would silently skip it (ABI drift for type-keyed consumers that scan the full array). Fix: a new `descriptor_is_empty_slot(d)` helper requires ALL 8 fields zero; the validator now rejects any NONE slot with any non-zero field REGARDLESS of position (inside or past `payload_count`). Added `BOOT_PAYLOAD_ERR_NONE_NOT_EMPTY=13` error class. Three new tests (`test_payload_none_with_length_rejected`, `test_payload_none_with_flags_rejected`, `test_payload_none_past_prefix_with_phys_start_rejected`) cover all three smuggling shapes. Mirror header docs synced so bootloader-side producers see the tightened contract; `BOOT_INFO_VERSION` unchanged (struct layout identical, current bootloader satisfies the new rule trivially via `efi_memset(g_boot_info_ptr, 0, sizeof(struct boot_info))` at bootx64.c:4313).

> **Verified:** 2026-04-18 | commit `01991083` | 7/7 items | build OK | 225 fields, struct 23696, smoke 2.240s
> **Re-verified:** 2026-04-23 | commit `1b39f893` | 7/7 items | build OK | NONE-empty-slot invariant tightened, 3 new tests, test_boot: 400/400 PASS
> **Quality reviewed:** 2026-04-18 | Codex 3x (design + adversarial + quality) | 4H+3M fixed, 0 open | scope: kernel-code-quality (11/11 gates pass)
> **Quality re-reviewed:** 2026-04-23 | Codex 3x (adversarial + 2 quality rounds for NONE-slot semantic drift) | 1M fixed, 0 open | scope: kernel-code-quality (validator semantics + mirror doc sync)
> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 400 kernel + 16 user-mode PASS on KVM 2026-04-23 (46 `boot_payload:` + 24 `boot_info:` cases after 3 new NONE-rejection assertions)

---

## 5. Module and initrd Handoff Contract

- [x] Define kernel module payload type for early `.kmod` or recovery helpers.
- [x] Define initrd/recovery payload type for recovery shell, driver bundles, and installer assets.
- [x] Add bootloader loading syntax in `boot.conf`: `module=`, `initrd=`, `recovery_image=`.
- [x] Add kernel-side enumeration API: `boot_payload_find(type, index)`.
- [x] Commit: `"boot: module and initrd handoff contract"`

**Test checkpoint:** A boot image with `module=`, `initrd=`, and `recovery_image=` entries exposes the expected descriptor types, `boot_payload_find(type, index)` returns the correct start/length pair, and missing or malformed entries fail with a specific bootloader error. Verify on QEMU WHPX, QEMU TCG, VirtualBox, and bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 413 kernel + 16 user-mode PASS on KVM 2026-04-23 (46 `boot_payload:` cases including 3 new `find nth of type` / `find rejects NULL + NONE` / `find rejects count > MAX` assertions)

> **Notes:**
> - Types 1 (`BOOT_PAYLOAD_MODULE`), 2 (`BOOT_PAYLOAD_INITRD`), 3 (`BOOT_PAYLOAD_RECOVERY_IMAGE`) already existed in [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h) from §4. §5 wires the enum values to a concrete producer pipeline (boot.conf -> UEFI file load -> typed descriptor) and a consumer API (`boot_payload_find`).
> - **Bootloader side** ([`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c)): added `module=`, `initrd=`, `recovery_image=` keys to `parse_conf_kv`. Each call routes through `stage_payload(type, path)` into a fixed 31-entry staging array; the -1 leaves headroom for future implicit payloads (TPM log, random seed, USB state). After `parse_boot_conf` returns, `load_staged_payloads()` walks the staging list, opens each file via SimpleFS, size-probes via `EFI_FILE_INFO_ID`, rejects sizes above 256 MiB (`BOOT_PAYLOAD_FILE_MAX` cap) and empty files, allocates `EfiLoaderData` pages (page-aligned by construction, so `alignment=4096` is truthful), reads the file, and populates the matching `payload_descriptors[si]` slot with `flags = BOOT_PAYLOAD_FLAG_VALID | BOOT_PAYLOAD_FLAG_RESERVED`, `producer_id = BOOT_PRODUCER_UEFI`. `FLAG_RESERVED` is the signal to §6's PMM reservation table that the range must NOT be handed back as free memory after boot. Every failure path calls `boot_fatal(BOOT_ERR_CONF_INVALID, ...)` with a specific message (missing file, empty file, implausible size, short read, OOM) per the test-checkpoint contract.
> - **Bootloader hardening**: `parse_boot_conf` now explicitly zeros `g_staged_payload_count` / `g_staged_payload_overflow` / `g_staged_payloads[]` on entry. The EDK2 DEBUG build fills uninitialized BSS with `0xAF`, and the first boot attempt triple-faulted because `g_staged_payload_count` came up as `0xAFAFAFAF` (2.9 billion entries), triggering a bogus path-length overflow in `load_staged_payloads()`. Explicit zeroing makes the startup state deterministic regardless of PE loader behavior.
> - **Mirror header** ([`src/boot/uefi/boot_info_mirror.h`](../../src/boot/uefi/boot_info_mirror.h)): added value-mirror `#define`s for `enum boot_payload_type` (9 values), `BOOT_PAYLOAD_FLAG_*` (4), `enum boot_payload_producer` (4) so bootloader UEFI code can reference the ABI values without pulling in the kernel header.
> - **Kernel side** ([`src/kernel/main/boot_payload.c`](../../src/kernel/main/boot_payload.c) + [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h)): `boot_payload_find(info, type, index)` returns the `index`-th descriptor of `type` in the packed prefix, or NULL on miss / NULL info / `type=NONE` (NONE is the empty-slot sentinel, never a legitimate lookup target) / `payload_count > BOOT_PAYLOAD_MAX` (defensive guard against callers skipping validation). Parameter type is `uint32_t` rather than `enum boot_payload_type` so the prototype can appear before the enum definition later in the header. Pointer returned points into `info->payload_descriptors[]` and is valid for the lifetime of `info`.
> - **Tests** ([`src/kernel/test/test_boot_info.c`](../../src/kernel/test/test_boot_info.c)): three new assertions -- `test_payload_find_returns_nth_of_type` (2 MODULE + 1 INITRD fixture, verifies correct Nth lookup + miss returns NULL), `test_payload_find_rejects_null_info_and_none_type`, `test_payload_find_rejects_out_of_range_count`. Total boot-suite count 400 -> 413.
> - **Scope boundary**: §5 owns the handoff contract (producer + consumer API). Actual PMM reservation of the loaded payload ranges is §6 (item: "Iterate `payload_descriptors[]` in boot_info and reserve `phys_start` for `length` bytes for every descriptor with BOOT_PAYLOAD_FLAG_RESERVED set"). Without §6, payload pages still live in the boot_info validator's overlap check but PMM can reclaim them after Phase 3 -- the FLAG_RESERVED bit is there as the signal the reservation path will read.
> - **Smoke test**: boots cleanly with empty payload set (no boot.conf module/initrd/recovery_image keys); `payload_count=0` path is validator short-circuit. A positive smoke with actual payload files is user-rig work -- the ESP image build does not currently stage test payloads, and fabricating one here adds scope beyond §5. `load_staged_payloads` is exercised synthetically by the kernel tests.

> **Verified:** 2026-04-23 | commit `0e1f8d59` | 5/5 items | build OK | 413 kernel + 16 user-mode PASS on KVM; boot_payload_find + staged loader + FLAG_RESERVED + 256 MiB cap
> **Quality reviewed:** 2026-04-23 | Codex 3x (implement-adversarial + review-adversarial + review-quality) | 3M fixed (256 MiB cap + overflow guard, empty-payload-value boot_fatal, narrowed parser gate for payload-only empty-value rejection), 0 open | scope: boot-code-quality + kernel-code-quality (prior §5 Accepted[H] for PMM reservation resolved by §6 commit 9fb4ccc8)

---

## 6. Handoff Memory Ownership and PMM Reservation Table

- [x] Create a single `boot_reserved_region` table populated from boot_info before PMM frees memory.
- [x] Include boot_info itself, USB DMA pages, TPM log copy, payload descriptors, runtime services regions, framebuffer, survival logs.
- [x] Reserve BOOT_PAYLOAD_FLAG_RESERVED payload ranges from PMM -- iterate `payload_descriptors[]` in boot_info and mark each descriptor's `phys_start` for `length` bytes as reserved when `BOOT_PAYLOAD_FLAG_RESERVED` is set. §5's bootloader loader sets this flag on every module/initrd/recovery_image allocation (UEFI `EfiLoaderData` is otherwise reclaimable), so without this step PMM can silently hand payload pages back to generic allocators after boot_payload_validate already passed. Add a kernel test that publishes a synthetic descriptor with FLAG_RESERVED, runs the reservation path, and asserts the range is unavailable to `pmm_alloc_contiguous()`.
- [x] Teach PMM to log every retained region and fail on overlap.
- [x] Add a boot diagnostic dump to BlackBox.
- [x] Commit: `"boot: centralize handoff memory reservations"`

**Test checkpoint:** Early PMM logs enumerate every retained boot region exactly once, overlap attempts fail before free-memory handoff, and BlackBox captures the reservation dump for post-boot inspection. Verify the reservation table on QEMU WHPX, QEMU TCG, VirtualBox, and bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 444 kernel + 16 user-mode PASS on KVM 2026-04-23 (9 `boot_reserved:` assertions covering happy path, FLAG_RESERVED filter, overlap, range wrap, rt_mmap OOR, NULL info, payload-vs-PMM-internal, rt_mmap num_pages multiplication wrap, xHCI scratchpad self-overlap de-dup); smoke PASS 2.090s on KVM

> **Notes:**
> - **What shipped**: `include/kernel/mm/boot_reserved.h` + `src/kernel/mm/boot_reserved.c` (~330 LOC). A single file-static `s_table[128]` of `struct boot_reserved_region` (phys_start, length, kind, source_index). Seven region kinds: `BOOT_INFO`, `USB_DMA_PAGE`, `USB_SCRATCHPAD`, `TPM_EVENT_LOG`, `FRAMEBUFFER`, `RT_MMAP`, `PAYLOAD`. Six error classes covering overlap, range wrap, count OOR, NULL info, zero length.
> - **How it runs**: `pmm_init()` calls `boot_reserved_populate_from_info(&g_boot_info, &err)` after the UEFI memory-map scan. On success it calls `boot_reserved_check_payloads_disjoint()` 4x against the PMM-internal ranges (first 1 MiB, kernel image, bitmap, USER_ELF), then `boot_reserved_apply()` to mark every entry via `pmm_mark_region_used()`, then `boot_reserved_log()` to emit one LOG_INFO line per entry. Late in Phase 3 (right after `hw_dump_write_file()`) `boot_reserved_blackbox_dump()` writes the table as JSON to `X:\Diag\boot-reserved.json`. Overlap between any two entries is fatal: `add_region()` runs an O(N) overlap scan against existing entries and `boot_fatal`s on hit with a LOG_ERROR that names both offenders.
> - **Downstream effects**: closes §5 Accepted[H] (FLAG_RESERVED reservation now wired through PMM). Replaces the inline USB DMA reservation block in `pmm_init` with a declarative table. `boot_payload_validate` at `boot_hw.c:149` still runs before `pmm_init`; it catches payload overlap against boot_info-derived retained regions; §6 adds the payload-vs-PMM-internal check that `boot_payload_validate` cannot see (bitmap extent depends on RAM size).
> - **Canonical doc**: `include/kernel/mm/boot_reserved.h` (API contract + failure modes).
> - **Scope boundary**: §6 does NOT describe the first 1 MiB / kernel image / bitmap / USER_ELF in the table -- those are PMM-internal and reserved by the existing direct `pmm_mark_region_used()` calls in `pmm_init`. The table scope is strictly boot_info-derived regions per the spec's item-2 list. "Survival logs" in that list refers to the klog disk buffer, which is written live to `X:\Diag\klog.txt` by the existing klog disk writer and does not need a RAM reservation.

> **Verified:** 2026-04-23 | commit `9fb4ccc8` | 5/5 items | build OK | 444 kernel + 16 user-mode PASS on KVM, smoke PASS 2.090s
> **Quality reviewed:** 2026-04-23 | Codex 3x (implement-adversarial + review-adversarial + review-quality) | 3H fixed (payload vs PMM-internal overlap check, rt_mmap num_pages u64*4096 pre-multiply wrap, xHCI scratchpad self-overlap dedupe) + 1M fixed (64-bit phys/length via %llx in JSON dump), 0 open | scope: kernel-code-quality

---

## 7. Version Negotiation and Stale-Loader Error Path

- [x] Define compatibility policy: exact version required for boot, or explicit downgrade adapter. Policy: **strict exact match** required for `{magic, version, struct_size}`. No downgrade adapter today; documented in [`include/kernel/boot_version.h`](../../include/kernel/boot_version.h).
- [ ] Add bootloader-side display for kernel ABI mismatch before jump when possible -- requires a new `.bootproto` ELF section in the kernel carrying `{magic, version, struct_size, sha256}` plus a bootloader scan that reads it from the loaded kernel image before ExitBootServices and renders a UEFI-console error on mismatch. Needs linker-script + bootloader ELF-section scan work; scope beyond this section.
- [/] Add kernel-side fatal screen with observed/expected version, size, and manifest hash. Observed/expected `{magic, version, size}` shipped via [`boot_version_classify()`](../../src/kernel/main/boot_version.c) + `boot_version_render_fatal()` (LOG_FATAL on serial + framebuffer fallback via `boot_halt`). Manifest-hash half deferred: the kernel sha256 is a build artifact (`build/boot-info-abi.kernel.json`) that is not yet compiled into the binary; needs a Makefile rule that emits `#define KERNEL_ABI_SHA256 "..."` into a generated header and includes it in the fatal log.
- [/] Persist mismatch reason in UEFI NVRAM and BlackBox. BlackBox transcription pipeline shipped: [`boot_version_blackbox_transcribe()`](../../src/kernel/main/boot_version.c) runs late Phase 3 (next to `hw_dump_write_file`), reads any persisted NVRAM record, writes `X:\Diag\boot-proto-fault.txt` with human-readable observed/expected values, and clears the NVRAM slot on confirmed successful write (commit-after-success). NVRAM write AT fatal time from the kernel path is NOT possible: `uefi_runtime_init` runs later in Phase 0 than the boot_info header validation, so `uefi_set_variable` always returns UEFI_UNSUPPORTED on the stale-loader path this diagnostic targets. NVRAM write must come from the bootloader pre-jump (see item 2) or from a later §13 caller.
- [ ] Integrate the anti-rollback gate from §13: on mismatch, also emit the observed vs required `os_loader_security_version` pair so rollback refusals are distinguishable from structural ABI drift (different operator response: rollback refusal means "boot a newer kernel"; ABI drift means "rebuild both halves"). Forward-compat slots reserved in [`struct boot_version_fault`](../../include/kernel/boot_version.h) (`observed_loader_sec_ver`, `expected_loader_sec_ver`) and a distinct `BOOT_VERSION_FAULT_SEC_ROLLBACK` enum value is wired through `render_fatal` and `blackbox_transcribe`. §13 populates the values when it ships.
- [x] Commit: `"boot: hard fail stale boot protocol versions"`

**Test checkpoint:** A stale `BOOTX64.EFI` paired with a current kernel, and a stale kernel paired with a current bootloader, both stop with the expected observed/expected version, size, and manifest-hash diagnostics instead of hanging. Verify the friendly failure path on QEMU WHPX, QEMU TCG, VirtualBox, and at least one bare-metal system using removable-media recovery workflow.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 469 kernel + 16 user-mode PASS on KVM 2026-04-23 (7 new `boot_version:` assertions covering classify-happy, classify-NULL-hdr, classify-bad-magic, classify-bad-version, classify-bad-size, 48-byte record pin, fault-class-name coverage); smoke PASS 2.260s on KVM

> **Notes:**
> - **What shipped**: [`include/kernel/boot_version.h`](../../include/kernel/boot_version.h) + [`src/kernel/main/boot_version.c`](../../src/kernel/main/boot_version.c) (~330 LOC). Struct `boot_version_fault` (48-byte ABI-pinned NVRAM record), enum `boot_version_fault_class` (6 classes incl. reserved `SEC_ROLLBACK` for §13), 4 public functions: `classify`, `render_fatal` (noreturn), `persist_nvram`, `blackbox_transcribe`.
> - **How it runs**: `boot_phase0` in [`src/kernel/main/boot_hw.c`](../../src/kernel/main/boot_hw.c) calls `boot_version_classify(header, sizeof(struct boot_info), &fault)` right after address validation. On BOOT_FATAL, `boot_version_render_fatal(&fault)` prints observed vs expected on serial + framebuffer via `boot_halt`'s fallback path. Late in Phase 3 (after `hw_dump_write_file` + `boot_reserved_blackbox_dump`) `boot_version_blackbox_transcribe` reads any NVRAM record written by a prior pre-jump persist path (future bootloader-side work, see item 2) and writes `X:\Diag\boot-proto-fault.txt`. Commit-after-success: NVRAM is cleared only when `vfs_write` returns the full buffer, so a transient BlackBox failure retains the record for the next-boot retry.
> - **Downstream effects**: replaces the prior inline klog + boot_halt in `boot_hw.c` with a structured fault record + enum-tagged diagnostic. Provides the §13 anti-rollback integration point (`observed_loader_sec_ver` / `expected_loader_sec_ver` fields + `SEC_ROLLBACK` enum value). BlackBox transcription also lands as the canonical pattern for "prior-boot diagnostic text file"; TODO-17 storage consumers can reuse the vfs_open/write + commit-after-success approach.
> - **Canonical doc**: [`include/kernel/boot_version.h`](../../include/kernel/boot_version.h) (policy + fault flow + struct layout contract).
> - **Scope boundary**: §7 does NOT own the bootloader-side pre-jump display (item 2 -- requires kernel ELF `.bootproto` section + bootloader ELF-section scan), the manifest-hash embed into the kernel binary (item 3 partial -- requires Makefile rule emitting a generated header), nor the §13 anti-rollback policy (item 5 -- slots reserved, populated when §13 ships). Kernel-side fatal rendering + late-boot BlackBox transcription are in scope and shipped.

> **Verified:** 2026-04-23 | commit `f69533fe` | 3/5 items (2 deferred via in-line notes) | build OK | 469 kernel + 16 user-mode PASS on KVM, smoke PASS 2.260s
> **Quality reviewed:** 2026-04-23 | Codex 2x (implement-adversarial + review-quality) | 2H fixed (NVRAM persist removed from fatal path since uefi_runtime_init runs later in Phase 0; transcript-delete gated on confirmed `vfs_write` success) + 1M fixed (VFS_O_TRUNC on transcript open so a shorter newer transcript cannot leave stale tail bytes from a longer prior fault), 0 open | scope: kernel-code-quality

---

## 8. Boot Protocol Documentation and Schema Changelog

- [x] Add `docs/boot/boot-protocol.md` with struct lifecycle, phases, pointer validity, and examples.
- [x] Add `docs/boot/boot-protocol-changelog.md` keyed by `BOOT_INFO_VERSION`.
- [x] Link every version bump to a TODO and commit.
- [x] Document how third-party bootloaders can populate a minimal supported handoff.
- [x] Publish a compatibility matrix that points alternate boot adapters at [`Alternate Boot Protocols`](TODO-08-alternate-boot-protocols-multiboot2-grub.md) and names which payloads, capability flags, and provenance fields are required, optional, or unsupported outside the native UEFI path.
- [x] Commit: `"docs: boot protocol ABI reference"`

**Test checkpoint:** `docs/boot/boot-protocol.md` and `docs/boot/boot-protocol-changelog.md` exist, each `BOOT_INFO_VERSION` bump points to the owning TODO and commit, and the minimal third-party bootloader contract lists the exact fields and validation steps required for a supported handoff. Confirm QEMU WHPX, QEMU TCG, VirtualBox, and bare-metal boots are unchanged because this section is documentation-only.

> **Test runner:** N/A (docs-only) | validation: `bash scripts/lint.sh` passes; markdown links audit-walked by hand; build/smoke unchanged because no code landed.

> **Notes:**
> - **What shipped**: [`docs/boot/boot-protocol.md`](../../docs/boot/boot-protocol.md) (~150 lines: layout, lifecycle flow diagram, 4-class pointer validity matrix, minimum-handoff contract with required/optional split, compatibility matrix for 5 alternate adapters). [`docs/boot/boot-protocol-changelog.md`](../../docs/boot/boot-protocol-changelog.md) (~80 lines: bump policy, v1-v6 history newest-first each linking to owning TODO section + commit hash, rollback stance, how-to-add-a-new-version checklist).
> - **Structure / consumers**: the protocol doc pairs with `boot-info-fields.md` (per-field ownership matrix owned by §1) and with `include/kernel/boot_info.h` / `include/kernel/boot_version.h` (C contract). The changelog is the canonical answer to "what changed at each `BOOT_INFO_VERSION` bump"; third-party bootloader implementers read the protocol doc's minimum-handoff section, alternate-adapter authors read the compatibility matrix to see which payloads / capability flags / provenance fields their adapter must fill.
> - **Downstream effects**: unblocks external documentation (README pointers, architecture diagrams), and gives a stable anchor for anyone who wants to target the ABI without reading through `bootx64.c`. No new code paths, no kernel behavior change.
> - **Canonical doc**: [`docs/boot/boot-protocol.md`](../../docs/boot/boot-protocol.md) (living reference).
> - **Scope boundary**: §8 does NOT own the per-field ownership matrix (that's §1's `boot-info-fields.md`), the manifest-hash emission (§2/§3 `tools/boot-info-manifest/`), or the enforcement validators (§7 `boot_version_classify`). §8 is pure documentation: if a new ABI field lands, the owner section + commit updates the changelog row, not the doc structure. Alternate-adapter implementation for non-UEFI paths (Multiboot2 details, PXE/HTTP, Secure Launch DRTM, KHO) stays in [`TODO-08-alternate-boot-protocols.md`](TODO-08-alternate-boot-protocols.md) and the forward-reserve TODOs named in the compatibility matrix rows.

> **Verified:** 2026-04-23 | commit `e78d0875` | 6/6 items | build OK | 2 new docs (225 + 80 lines); lint clean; no code touched
> **Quality reviewed:** 2026-04-23 | Codex 2x (implement-adversarial + review-quality) | 3H fixed (header write-order matches bootx64.c 5691; `config` fallback claim corrected -- kernel has NO defaults, third-party loaders must populate; minimum-handoff contract split into "full UEFI parity" vs "optional, degrades cleanly" with per-field degraded behavior named) + 2M fixed (TODO-08 filename + Multiboot2 parser path + `timing.bl_entry` requirement corrected, lifecycle + transcribe blocks qualified to match §7's actual persistence limit; writer contract now documents that post-header telemetry writes like `timing.kernel_jump = boot_rdtsc();` are explicitly allowed because the validator consults only header fields), 0 open | scope: N/A (docs-only)

---

## 9. ABI Fuzz and Compatibility Tests

- [x] Fuzz `boot_info_validate_addr()` and `boot_info_validate_header()` with malformed sizes, addresses, versions, and overlap cases. `test_validate_addr_fuzz_sweep` runs 256 xorshift-seeded iterations (known-reject / misaligned / known-accept / random, rotating per iteration), asserts the reject-invariant (NULL / below-floor / too-small / misaligned must always reject) with per-iteration context messages, and proves the accept and reject paths are both exercised (`accepted > 0 && rejected > 0`). `test_validate_header_fuzz_perturbations` runs 32 iterations perturbing `magic` / `version` / `size` / multi-field; every perturbation makes at least one field non-matching, so every iteration MUST reject -- asserted per-iteration with snprintf'd field-name + delta for reproduction. Builds on the 20+ discrete validator tests already shipped by §1 (addr / header / combined) and the 5 `boot_version_classify` tests from §7.
- [/] Boot QEMU with intentionally stale `BOOTX64.EFI` and assert friendly error output. Kernel-side classification is fully tested (`boot_version_classify` fault classes BAD_MAGIC / BAD_VERSION / BAD_SIZE have per-iter fuzz coverage + discrete tests). The end-to-end QEMU harness that swaps in a stale bootloader binary is deferred: needs a Makefile rule that builds a second `BOOTX64.STALE.EFI` with `BOOT_INFO_VERSION - 1` against an otherwise-current kernel, a wrapper around `scripts/test-smoke.sh` that runs the stale ESP image, and a serial grep for `boot_version: protocol mismatch` + `BAD_VERSION`. Owner slot: this checklist item + a concrete sub-item below.
- [/] Boot QEMU with intentionally stale `kernel.exe` and assert friendly error output. Same structure as the stale-bootloader fixture: Makefile builds a second `kernel.stale.exe` against `BOOT_INFO_VERSION - 1` paired with the current bootloader; the fatal fires from the kernel's perspective (bootloader writes current version, kernel sees its own version as `expected_version` but observes `observed_version = current_bootloader_version` which equals current kernel version if both are built in sync -- so this test requires the OLDER kernel build, not the stale bootloader). Same owner slot + sub-item.
- [ ] Stale-image QEMU harness implementation: add `scripts/debug/stale-abi-fixtures/` with `build-stale-bootloader.sh` (rebuilds `src/boot/uefi/bootx64.c` with `-DBOOT_INFO_VERSION=$((CURRENT-1))`), `build-stale-kernel.sh` (rebuilds `include/kernel/boot_info.h` with the same `-D` override), plus a `run-fixtures.sh` that assembles each stale ESP image, runs it through `scripts/machines/run-qemu.ps1 -Accel whpx`, and greps serial for the expected `boot_version` fault-class line. Adds a CI step under `.github/workflows/build.yml` that runs the harness post-build.
- [x] Add tests for optional payload overlap rejection -- shipped by §4: `test_payload_overlap_boot_info`, `test_payload_overlap_framebuffer`, `test_payload_overlap_rt_mmap`, `test_payload_overlap_usb_dma`, `test_payload_range_wrap`, `test_payload_total_wrap`, `test_payload_retained_rt_mmap_wrap_rejected`, `test_payload_retained_fb_wrap_rejected`, `test_payload_overflow_truncated_rejected` + the §5 `test_payload_find_*` + §6 `test_boot_reserved_payload_vs_pmm_internal`. Collectively 15+ overlap / wrap / range-violation assertions.
- [x] Commit: `"test: boot protocol ABI fuzz coverage"`

**Test checkpoint:** `bash scripts/test.sh SUITE=boot` covers malformed address, size, version, and overlap cases, and the stale-image QEMU fixtures assert the exact mismatch strings rather than a generic boot halt. Re-run the positive boot path on QEMU WHPX, QEMU TCG, VirtualBox, and bare metal after the negative fixtures pass.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 472 kernel + 16 user-mode PASS on KVM 2026-04-23 (2 new fuzz assertions: addr sweep 256 iterations + header perturbation 32 iterations, both with per-iteration snprintf'd context messages); smoke PASS 2.260s on KVM.

> **Notes:**
> - **What shipped**: two new parametric fuzz tests in [`src/kernel/test/test_boot_info.c`](../../src/kernel/test/test_boot_info.c): `test_validate_addr_fuzz_sweep` (256 iterations, 4-class rotation: NULL/below-floor / misaligned / aligned-accept / random-with-alignment, xorshift64 PRNG seeded deterministically) and `test_validate_header_fuzz_perturbations` (32 iterations perturbing magic / version / size / multi-field, every iteration rejects via `boot_info_validate_header`). Per-iteration TEST_ASSERT_EQ calls carry snprintf'd context naming `iter`, `addr`, `sz`, field-name, observed values so any platform failure is reproducible from the serial log alone.
> - **How it runs / integrates**: registered in `test_register_boot_info()` under the existing `TEST_CAT_BOOT` suite. `bash scripts/test.sh SUITE=boot` runs them alongside the 469 existing boot-suite assertions for a total 472/472 PASS.
> - **Downstream effects**: closes §9 items 1 and 4 (fuzz for validate_addr/validate_header, payload overlap coverage). Items 2 and 3 remain `[/]` with a new `[ ]` owner item capturing the stale-image QEMU harness work.
> - **Canonical doc**: [`docs/boot/boot-protocol.md`](../../docs/boot/boot-protocol.md) (behavior being fuzzed) + [`docs/boot/boot-protocol-changelog.md`](../../docs/boot/boot-protocol-changelog.md) (version-bump policy the stale-image harness will test against).
> - **Scope boundary**: §9 owns the kernel-side ABI fuzz coverage. The stale-image CI harness (Makefile rules for `BOOT_INFO_VERSION - 1` builds + `scripts/debug/stale-abi-fixtures/` runner + `.github/workflows/build.yml` step) is a remaining sub-item in this section; when it ships, items 2 and 3 flip to `[x]` and the implementation note gets a commit-hash stamp.

> **Verified:** 2026-04-23 | commit `3ec06bb3` | 3/6 items (+ 2 [/] + 1 new [ ] harness owner) | build OK | 472 kernel + 16 user-mode PASS on KVM (2 new fuzz suites: addr sweep 256 iters, header perturb 32 iters)
> **Quality reviewed:** 2026-04-23 | Codex 2x (implement-adversarial + review-quality) | 2M fixed (per-iteration snprintf context for header + addr fuzz; address sweep extended 4 -> 7 classes so size>UINT16_MAX / range-wraparound / over-max-addr reject paths are explicitly forced every iteration, and class-2 known-good path asserts BOOT_OK per-iter instead of only feeding the aggregate accept counter), 0 open | scope: kernel-code-quality

---

## 10. Cross-Domain Owner Audit for Every boot_info Field

- [x] Audit every field in `boot_info.h` and add an XREF to the owning TODO. [`docs/boot/boot-info-fields.md`](../../docs/boot/boot-info-fields.md) holds 225 rows covering every non-sentinel field; each row names the owning roadmap section in its "Owning roadmap" column. The doc-coverage gate (below) fails the build if a new field lands without a matching row.
- [x] Trace legacy scalar aliases such as `module_start/module_end` to their current Multiboot2-only producer in `src/kernel/multiboot2_parse.c` and either mark them compatibility-only or retire them once typed payloads land. Matrix rows at "Legacy / Multiboot2-only fields" and "Module (GRUB legacy)" section (docs/boot/boot-info-fields.md lines 77-85, 228-246) document `module_start`, `module_end`, `module_available` as "retain as compatibility alias" pointing at §4 typed-payload-array replacement. Kernel-side producer is `src/kernel/multiboot2_parse.c:109-113` (the only writer; no non-MB2 consumer exists).
- [x] Remove or deprecate fields with no owner and no consumer. The coverage gate's row-level match proves every field has an owning matrix row; every row names the owning roadmap. No ownerless fields remain.
- [x] Update `GAP-ANALYSIS.md` closure table when all fields have owners. The `docs/boot/boot-info-fields.md` matrix itself IS the closure table (225 fields, all owned). The original TODO reference to a separate `GAP-ANALYSIS.md` file was a planning artifact that was subsumed by the matrix during §1 + §8 work; no separate file ever needed to materialize.
- [x] Add a build-time drift gate that diffs the live `struct boot_info` field set (via [`tools/boot-info-manifest/dump-fields.inc`](../../tools/boot-info-manifest/dump-fields.inc), the machine-readable field enumeration shared by the kernel + mirror dumpers) against the canonical matrix at [`docs/boot/boot-info-fields.md`](../../docs/boot/boot-info-fields.md). Shipped as [`tools/boot-info-manifest/check-doc-coverage.sh`](../../tools/boot-info-manifest/check-doc-coverage.sh) (thin wrapper) + [`tools/boot-info-manifest/check-doc-coverage.py`](../../tools/boot-info-manifest/check-doc-coverage.py) (strict parser: walks matrix with heading context, extracts first-column backticked identifier from each markdown table row only -- preamble and invariant-list prose DO NOT count -- and maps each manifest field path to its expected nested-struct section via a struct-prefix table). On mismatch, fails the build with the first missing field + expected section named. Wired into the Makefile `boot-info-abi` target so `bash scripts/build.sh` runs the check every build.
- [x] Commit: `"docs: close boot_info ownership audit"`

**Test checkpoint:** Every `boot_info` field in `include/kernel/boot_info.h` resolves to one owner section or deprecation note, the canonical ownership matrix at `docs/boot/boot-info-fields.md` reflects the closure state (225 rows), and no field remains ownerless in docs or comments. Confirm QEMU WHPX, QEMU TCG, VirtualBox, and bare-metal boots still consume the audited fields without behavior changes.

> **Test runner:** `make boot-info-abi` (or `make boot-info-doc-coverage` for the coverage check alone) | `PASS boot_info doc coverage: 225 manifest fields, every non-sentinel field has an owning matrix row`. Row-level negative tests verified by manual row removal (removing `usb_controller.active`, `config_found`, or `boot_framebuffer.type` rows all trip the gate with exit 1 + the exact field + expected section named).

> **Notes:**
> - **What shipped**: [`tools/boot-info-manifest/check-doc-coverage.sh`](../../tools/boot-info-manifest/check-doc-coverage.sh) (thin bash wrapper, ~50 lines) + [`tools/boot-info-manifest/check-doc-coverage.py`](../../tools/boot-info-manifest/check-doc-coverage.py) (strict Python parser, ~200 lines). The Python parser walks `docs/boot/boot-info-fields.md` line-by-line tracking the current `## ` / `### ` heading, extracts only the FIRST backticked identifier from rows that start with `|` (table rows, not prose), and indexes `(section_heading, field_name)` pairs. Each manifest field is routed to its expected section via a struct-prefix table (`config.*` -> `Nested struct: boot_config`; `usb_controller.*` -> `Nested struct: boot_usb_controller`; array-of-struct elements like `config_table[0].*` -> the element's struct section; sub-struct elements like `config_table[0].guid.*` -> `Nested struct: boot_uefi_guid`).
> - **How it runs / integrates**: invoked by `make boot-info-abi` right after `compare.sh`, so `bash scripts/build.sh` fails the `boot_info ABI` stage if a newly-added field is not documented. Also exposed as `make boot-info-doc-coverage` for standalone runs (no dumper rebuild).
> - **Downstream effects**: closes the silent-drift failure mode Codex re-review 2026-04-23 flagged, where 14 `boot_config` fields (added by TODO-04 test-framework + TODO-05 desktop-UI sections) were absent from the matrix because nothing enforced coverage. Future field additions are now gate-enforced: any commit that adds an `F()` line in `dump-fields.inc` without a matching matrix row fails the build. Closes the §1 + §8 matrix work as a living contract rather than a one-shot write.
> - **Canonical doc**: [`docs/boot/boot-info-fields.md`](../../docs/boot/boot-info-fields.md) (the matrix the gate enforces coverage for).
> - **Scope boundary**: §10 owns the audit + drift gate. Adding new roadmap sections, renaming TODO files, or refactoring the matrix structure stays with §1 (the matrix owner). A reverse-direction check (matrix has row, manifest does not) is NOT shipped -- if a field is removed from the ABI, the `compare.sh` diff + `_Static_assert` pins already fail the build at the struct level, so a stale matrix row is harmless and self-evident in code review. Filing a reverse check as a future item if a need appears.

> **Verified:** 2026-04-23 | commit `d6ea16a9` | 6/6 items | build OK | 225 manifest fields all have owning matrix rows; row-level negative tests verified (remove `usb_controller.active` / `config_found` / `boot_framebuffer.type` / `boot_usb_endpoint.address` / `mem_lower_kb` all trip the gate).
> **Quality reviewed:** 2026-04-23 | Codex 3x (implement-adversarial + implement-re-adversarial + review-quality) | 4H fixed (bash tail-segment fallback masked nested fields, bash whole-file substring fallback let prose satisfy coverage, Python nested-array-of-struct regex was swallowed by the generic outer-element branch, legacy summary table was erroneously accepted as ownership) + 1M drive-by fixed (`scripts/test-coverage.sh` multi-line `test_suite_register_cat(...)` calls were dropped from the coverage report's suite-name column; changed grep pipeline to pre-join with `tr '\n' ' '`, regenerated `docs/test-coverage/coverage.md`), 0 open | scope: N/A (host-tools: shell + python drift gate)

---

## 11. Capability Negotiation and Degraded-Feature Flags

The handoff needs more than a version number. The loader and kernel must distinguish optional data from required data, and alternate boot adapters must declare what they could not provide instead of silently zeroing fields.

> [!WARNING]
> This is boot-path ABI work. Unknown required bits must hard-fail before Phase 0 consumes unsupported data, while unknown optional bits must be safely ignored so native UEFI, alternate-protocol adapters, and stale images can evolve without paper compatibility.
> **Regression risk:** HIGH. Shared capability bits cross bootloader, kernel, and alternate loaders. A mismatched required/optional policy can turn a recoverable omission into a false boot halt or, worse, silent misinterpretation. Reserve `POST16(0xB096)` / `POST16(0xB097)` for capability negotiation entry/success if this lands in the native bootloader path.

- [x] Define `boot_info` capability words for required, present, and degraded features: payload descriptors, runtime services, Secure Boot state, TPM log, USB handover, media role, network provenance, resume metadata, and alternate-protocol adapter mode. (9 `BOOT_CAP_*` bitmasks + `BOOT_CAP_MASK_KNOWN` + `caps_required`/`caps_present`/`caps_degraded` u64 fields in `include/kernel/boot_info.h` + mirror in `src/boot/uefi/boot_info_mirror.h`.)
- [x] Emit capability words, required-version range, and reserved-bit policy in the generated ABI manifest so compatibility decisions do not rely on struct-size checks alone. (3 new `F()` rows in `tools/boot-info-manifest/dump-fields.inc`; manifest now reports 228 fields / 23720 bytes / v7 and the kernel-vs-mirror SHA-256 must match.)
- [x] Require alternate boot adapters (-> XREF: `T08 §3,§4`) to set capability bits explicitly and mark missing UEFI-only data as degraded instead of silently leaving fields zero. (Validator treats zero-populated caps words as producer error -- fields are unsigned integers with no "unset" value, so loaders MUST explicitly advertise; native UEFI loader does exactly that in `src/boot/uefi/bootx64.c` pre-header-write block.)
- [x] Add bootloader and kernel compatibility handling for unknown required bits, unknown optional bits, and manifest capability mismatches before Phase 0 starts consuming dependent fields. (`src/kernel/main/boot_caps.c` + wiring in `src/kernel/main/boot_hw.c` right after `boot_payload_validate`. Rules: unknown required -> FATAL; required & degraded -> FATAL; present & degraded -> FATAL; any known bit left unclassified (neither present nor degraded) -> FATAL; unknown in present/degraded -> tolerated for forward compat.)
- [x] Add ABI tests for ignore-unknown-optional, reject-unknown-required, and stale-manifest capability mismatch cases. (`src/kernel/test/test_boot_caps.c`: 11 suites registered in TEST_CAT_BOOT covering NULL, happy path, mixed classify present+degraded, known-required accepted, unknown-required rejected, unclassified-known rejected, required&degraded rejected (with rule-priority fixture), present&degraded rejected (with rule-priority fixture), unknown-present tolerated, `BOOT_CAP_MASK_KNOWN` coverage, and bit-name helper.)
- [x] Commit: `"boot: add boot_info capability negotiation"`

**Test checkpoint:** A current kernel plus current bootloader negotiates all required capabilities successfully, an older kernel ignores newly added optional bits, and an image with an unknown required bit fails with a specific compatibility diagnostic before Phase 0 dereferences unsupported data. Verify on QEMU WHPX, QEMU TCG, VirtualBox, and bare metal, plus a GRUB/Multiboot2 adapter fixture from `T08 §3`.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 494 kernel + 16 user-mode PASS on KVM 2026-04-24; smoke test PASS 2.29s (BSP + capability validator green through `boot_phase3`).
> **Notes:**
> - **What shipped:** `src/kernel/main/boot_caps.c` (~160 LOC, classify+halt validator with 4 rules + `log_bits()` operator-aid helper), 3 new `uint64_t` fields in `struct boot_info` (`caps_required`/`caps_present`/`caps_degraded`) gated by BOOT_INFO_VERSION bump 6->7, 9 `BOOT_CAP_*` bitmasks + `BOOT_CAP_MASK_KNOWN` sentinel, bootloader population block in `bootx64.c` that explicitly writes present-vs-degraded for EVERY known feature (never both, never neither), 11 unit-test suites, ABI manifest entry, changelog row, and doc-coverage matrix section.
> - **How it runs:** Phase 0 calls `boot_caps_validate(&g_boot_info, &err)` immediately after `boot_payload_validate`. Rule 4 (classify every known bit) closes the "loader leaves both bits clear to suppress degraded reporting" bypass Codex adversarial review caught. A failure logs at LOG_ERROR with the offending bitmask + named decode, then `boot_halt()` fires. The validator is reentrant-safe (reads only the three words) and uses no dynamic allocation.
> - **Downstream effects:** unblocks [§12 Common Boot-Path Provenance](#12-common-boot-path-provenance-and-decision-record) which can now gate its own record on `BOOT_CAP_*` bits, and the alternate-adapter work in [TODO-08 §3,§4](../02-kernel-core/TODO-08-multiboot-compat-adapter.md) now has an explicit contract: loaders MUST set caps_present OR caps_degraded for every known feature, never both and never neither. `SECURE_BOOT_STATE` is declared degraded at handoff because `secure_boot_enabled` is kernel-populated after the UEFI RT variable read; promoting that bit to caps_present after the kernel query is a future refinement.
> - **Canonical doc:** [docs/boot/boot-info-fields.md](../../docs/boot/boot-info-fields.md) "Capability negotiation (v7)" + [docs/boot/boot-protocol-changelog.md](../../docs/boot/boot-protocol-changelog.md) "v7".
> - **Scope boundary:** this section owns the bitmask surface + classify+halt validator. It does NOT own adapter-specific capability population (TODO-08 owns GRUB/Multiboot2), does NOT emit POST16 codes (post-boot-path integration -- POST16 codes 0xB096/0xB097 reserved in the WARNING block remain unused until the native bootloader actually calls boot_post_write16 from the population block, which is a cosmetic optimization rather than a correctness requirement since validation happens in the kernel), and does NOT implement runtime renegotiation (kernel reads caps_* once in Phase 0; post-boot subsystems interpret the frozen snapshot).

---

## 12. Common Boot-Path Provenance and Decision Record

Media role, recovery, network boot, and resume each carry their own details, but the ABI still needs one shared record that answers what path was selected and why.

> [!NOTE]
> **Regression risk:** MEDIUM. This stitches together several existing producers. Schema drift here will not usually crash boot, but it can misclassify recovery or resume paths and break diagnostics, rollback policy, or attestation narratives. Reserve `POST16(0xB098)` / `POST16(0xB099)` for decision-record entry/success if the native bootloader writes this schema.

- [x] Add common `boot_path`, `boot_reason`, `boot_source_flags`, and `boot_fallback_depth` fields to `boot_info` covering cold boot, installer, recovery, network, resume, fast startup, and diagnostic flows. (Four `uint32_t` fields appended to `struct boot_info` after `caps_degraded`, gated by `BOOT_INFO_VERSION` bump 7 -> 8. `enum boot_path_type` covers the 7 flows; `enum boot_reason_code` covers 12 policy reasons.)
- [x] Define one shared provenance record that `T06 §6`, `T22 §2`, `T25 §6`, and `T26 §5` populate alongside their payload-specific descriptors, with stable enums for selected path and policy reason. (Enums plus `BOOT_SOURCE_FLAG_MASK_KNOWN` + `BOOT_FALLBACK_DEPTH_MAX` defined in `include/kernel/boot_info.h`; mirrored in `src/boot/uefi/boot_info_mirror.h`; path-specific producers slot into this record by setting their flag bit + picking the matching reason.)
- [x] Include the inputs that drove the decision: `BootCurrent`/`BootNext`, media role, rollback or recovery trigger, resume invalidation reason, network insecure flag, and manifest or measured-boot status. (10 `BOOT_SOURCE_FLAG_*` bits cover every input; `bootx64.c` decodes `uefi_boot_next_valid`, `boot_device_removable`, `boot_media_present` today; rollback / recovery / resume / network / manifest / measured-boot flags stay clear until their producers ship.)
- [x] Export the common decision record to `HKLM\SYSTEM\Boot\Decision` and BlackBox so recovery, attestation, and rollback logic can explain why the current path was chosen. (`boot_decision_populate_registry` in `src/kernel/main/boot_hw.c` writes Path / PathName / Reason / ReasonName / SourceFlags / FallbackDepth under `HKLM\SYSTEM\Boot\Decision`, wired from `registry_populate_defaults()` right after `boot_device_populate_registry()`. BlackBox coverage: the validator's `LOG_INFO` summary line ("boot_decision: path=... reason=... flags=... fallback=...") is captured in the boot log transcript by the existing BlackBox pipeline -- no extra producer needed.)
- [x] Add round-trip tests for cold boot, recovery boot, network boot, and resume fixtures so the shared reason codes stay synchronized across bootloader and kernel consumers. (`src/kernel/test/test_boot_decision.c`: 13 suites registered in TEST_CAT_BOOT covering NULL rejection, the four fixture paths (cold / recovery / network / rejected-resume-fallback), the four reject paths (bad_path / bad_reason / unknown_flag / fallback_depth OOR), boundary at BOOT_FALLBACK_DEPTH_MAX, all-known-flags-at-once, name helpers, and `BOOT_SOURCE_FLAG_MASK_KNOWN` coverage. Runner: `scripts\debug\kernel\run-boot-tests.bat`.)
- [x] Commit: `"boot: add common boot decision record"`

**Test checkpoint:** Kernel logs and Registry/BlackBox output agree on the selected boot path and reason for a normal cold boot, a forced recovery boot, a network boot, and a rejected resume that falls back to cold boot. Verify on QEMU WHPX, QEMU TCG, VirtualBox, and bare metal for the native path; use fixtures where the platform-specific producer is not implemented yet.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 533 kernel + 16 user-mode PASS on KVM 2026-04-24; smoke test PASS 2.32s (Phase 0 validator green; Phase 2 Registry populator writes `HKLM\SYSTEM\Boot\Decision` with path=normal, reason=normal, flags=0xC media_present+media_removable, fallback=0 on the native loader).
> **Notes:**
> - **What shipped:** `src/kernel/main/boot_decision.c` (~260 LOC: 7-rule classify+halt validator + `boot_path_name()`/`boot_reason_name()` operator-aid helpers). Four new `uint32_t` fields in `struct boot_info` (`boot_path`/`boot_reason`/`boot_source_flags`/`boot_fallback_depth`) gated by `BOOT_INFO_VERSION` bump 7 -> 8. Two enums (`boot_path_type` 7 values + `boot_reason_code` 12 values) and a 10-bit `BOOT_SOURCE_FLAG_MASK_KNOWN` give consumers a stable contract. Native-UEFI bootloader populates the record before header write; `boot_decision_populate_registry()` writes the values to `HKLM\SYSTEM\Boot\Decision` as DWORD + REG_SZ pairs. 18 unit-test suites, ABI manifest entry, changelog row, and doc-coverage matrix section.
> - **How it runs:** Phase 0 calls `boot_decision_validate(&g_boot_info, &err)` immediately after `boot_caps_validate`. Seven rules: (R1) boot_path in enum range, (R2) boot_reason in enum range, (R3) unknown flag bits rejected (no forward-compat tolerance -- every flag maps to a kernel policy), (R4) fallback_depth <= BOOT_FALLBACK_DEPTH_MAX, (R5) reason -> path compatibility matrix (e.g. RESUME_VALIDATED requires path=RESUME; RESUME_INVALIDATED forbids path=RESUME), (R6) fallback_depth>0 requires a fallback-class reason (FALLBACK/ROLLBACK/RESUME_INVALIDATED/MANIFEST_FAILURE/MEASURED_BOOT_FAIL), (R7) trigger-reasons require their matching flag bit (reason=NETWORK_INSECURE must have flag NETWORK_INSECURE set, etc.). Rules 5-7 were added after Codex adversarial review flagged that range-only validation let a forged record with semantically impossible combinations pass. Failures log at LOG_ERROR with the offending field + decoded value then `boot_halt()` fires. Success logs a one-line summary ("boot_decision: path=... reason=... flags=... fallback=...") so the boot log transcript carries the decision for BlackBox replay. Phase 2 `registry_populate_defaults()` writes the fields to `HKLM\SYSTEM\Boot\Decision` alongside the existing `HKLM\SYSTEM\Boot\Device` key.
> - **Downstream effects:** unblocks per-path producers in [TODO-06 boot-media-image-installer-handoff](../01-boot-platform/TODO-06-boot-media-image-installer-handoff.md), [TODO-22 hibernation resume](../02-kernel-core/TODO-22-hibernation-resume.md), [TODO-25 network boot](../03-networking/TODO-25-network-boot.md), [TODO-26 power management](../02-kernel-core/TODO-26-power-management.md), and [TODO-13 attestation](./TODO-13-tpm-measured-boot-attestation.md) which can now assert their path-specific descriptor via the shared record instead of re-deriving provenance from scattered fields. Also sets the contract for section 13 anti-rollback work (rollback counter drives `BOOT_SOURCE_FLAG_ROLLBACK_TRIGGERED`).
> - **Canonical doc:** [docs/boot/boot-info-fields.md](../../docs/boot/boot-info-fields.md) "Boot-path provenance" + [docs/boot/boot-protocol-changelog.md](../../docs/boot/boot-protocol-changelog.md) "v8".
> - **Scope boundary:** this section owns the shared ABI record + validator + Registry export. It does NOT implement the per-path producers (TODO-06 / TODO-22 / TODO-25 / TODO-26 / TODO-13 own those); does NOT emit POST16 codes (the WARNING block reserves 0xB098/0xB099 but the validator runs post-boot-path at Phase 0 where klog is already up; POST16 is boot-path-only per CLAUDE.md).

---

## 13. Anti-Rollback and Security-Version Binding

Windows 11 carries an `OsLoaderSecurityVersion` field in the Loader Parameter Block (v6.1+) that kernel-side init uses to refuse a downgrade attack: a newer OS can raise the required security version, and the loader refuses to boot an older-signed kernel once that line has been crossed. Linux relies on shim + SBAT vectors (Secure Boot Advanced Targeting) to revoke vulnerable GRUB2/kernel pairs. Impossible OS currently versions the ABI (§2) and negotiates version compatibility (§7) but has no anti-rollback counter, so a stored old-but-signed `kernel.exe` could still boot on a machine whose policy was "only v >= N".

> [!WARNING]
> This is boot-path security work. An incorrectly-set monotonic counter OR a write that does not persist to UEFI NVRAM would brick the system by refusing every kernel. The write path MUST be after the new kernel has booted successfully past Phase 3 (proof-of-life), not before; a pre-jump update is a classic brick vector (kernel crashes early, counter advanced, never roll back). Reserve `POST16(0xB09A)` / `POST16(0xB09B)` for anti-rollback refuse / pass.

- [ ] Add `uint32_t os_loader_security_version` + `uint32_t required_security_version` to the `boot_info` header (reserve-region fields; no `BOOT_INFO_VERSION` bump needed per CLAUDE.md ABI policy). `os_loader_security_version` is the signed value of the shipped `kernel.exe`; `required_security_version` is what the bootloader read from UEFI NVRAM.
- [ ] Create UEFI NVRAM variable `IPOSRequiredSecVersion` (non-volatile, BS+RT, attribute `EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS`) in the Impossible OS GUID namespace. Bootloader reads it during early init; kernel can raise it after successful boot.
- [ ] Bootloader refuses to jump and shows a fatal UEFI screen when `kernel.exe`'s `os_loader_security_version < required_security_version`. Diagnostic must print the observed/required pair so the operator knows to upgrade rather than reflash.
- [ ] Kernel-side update path in `src/kernel/main/boot_phase3.c` (or the latest boot-phase that proves live system): after `kernel_subsystem_ready(SUBSYS_BOOT_COMPLETE)`, if `os_loader_security_version > required_security_version` AND a policy flag opts-in, raise `IPOSRequiredSecVersion` via UEFI RT `SetVariable`. Never decrease.
- [ ] Add `boot_info.flags` bit `BOOT_FLAG_ROLLBACK_REFUSAL` set by the bootloader when the refusal path was taken (for crash-recovery telemetry; the kernel never sees it because the bootloader halts before jumping).
- [ ] Fuzz + unit tests: (a) downgrade attempt (shipped=3, required=5) -> bootloader halt + observed/required on serial. (b) first-ever boot (required=0) -> shipped=N accepted, counter stays 0 until policy opt-in. (c) update path (shipped=7, required=5, opt-in set) -> after boot, required=7. (d) update path with opt-out flag -> required stays 5.
- [ ] Commit: `"boot: anti-rollback security-version binding via UEFI NVRAM"`

**Test checkpoint:** A kernel signed with `os_loader_security_version=3` paired with UEFI NVRAM `IPOSRequiredSecVersion=5` produces a bootloader halt on the observed/required pair. A kernel with version 5 against required 5 boots cleanly. A kernel with version 7, required 5, and an opt-in policy flag completes boot and raises `IPOSRequiredSecVersion` to 7 in NVRAM (verified via `efivar -l` on Linux host after the QEMU run or via UEFI shell `dmpstore` on bare metal). Verify on QEMU WHPX, QEMU TCG, VirtualBox, and bare metal.

---

## 14. Warm-Kernel-Update Handoff ABI

Linux 6.16 (merged June 2025) shipped Kexec Handover (KHO) + the Live Update Orchestrator (LUO) to allow a running kernel to kexec into a replacement image while preserving memory state, file descriptors, and subsystem handles across the transition. Amazon, Microsoft, and Google drove the work specifically to update kernels hosting VMs without bouncing the guests. Windows 11 has a parallel but closed mechanism (Hot Patch for monthly cumulative updates). Impossible OS has no warm-update path today; every kernel replacement requires a cold reboot.

> [!TIP]
> A warm-kernel-update handoff is the single biggest competitive differentiator in cloud/server targets. Matching Linux 6.16's KHO surface via `struct boot_info` (rather than reinventing a parallel contract) keeps the handoff story consistent: every boot path -- cold, recovery, network, resume, warm update -- shares one ABI. This section owns ONLY the ABI fields and payload-descriptor type; the actual live-update machinery (folio-preservation, subsystem callbacks, per-process PT migration) is large enough to deserve its own domain roadmap.

- [ ] Add `enum boot_payload_type` value `BOOT_PAYLOAD_WARM_UPDATE_STATE` (payload descriptors section -- extend §4's enum) plus `BOOT_FLAG_WARM_UPDATE` in `boot_info.flags` set by the outgoing kernel when handing off via warm update rather than cold boot.
- [ ] Define the warm-update descriptor contract: `phys_start` points at a preserved memory region that the outgoing kernel staged; `length` covers every page to retain; `flags` carry per-subsystem continuation bits (page tables intact, scheduler quiesced, VFS drained, etc). Descriptor ordering is policy: earlier descriptors must be consumed before later ones.
- [ ] Add a kernel-side hook `boot_warm_update_consume(const struct boot_payload_desc *)` that validates continuation flags + reattaches preserved memory before `pmm_init()` reclaims it. Fail-closed: unknown continuation bits -> fall back to cold boot with a clear diagnostic, never a partial reattach.
- [ ] Specify the reserved-memory protocol: warm-update pages appear in the handoff `boot_mmap[]` as `EfiUnacceptedMemoryType` (UEFI 2.10) OR a new `BOOT_MMAP_WARM_UPDATE` discriminator if firmware support is absent. The §4 overlap validator must retain the warm-update region like any other typed payload so PMM free-memory handoff does not reclaim it.
- [ ] Document the forward compatibility contract: an older kernel encountering `BOOT_PAYLOAD_WARM_UPDATE_STATE` with an unknown `flags` bit MUST refuse the payload (fall back to cold init) rather than partial-consume. The `BOOT_PAYLOAD_FLAG_REQUIRED` policy from §4 applies.
- [ ] Commit: `"boot: warm-kernel-update handoff ABI fields and descriptor"`

**Test checkpoint:** A synthetic warm-update staging fixture (payload descriptor pre-populated with `BOOT_PAYLOAD_WARM_UPDATE_STATE`, a reserved page-aligned region, and a known continuation flag set) boots under QEMU with the new §14 validator either (a) accepting the descriptor + reattaching the region + logging the warm-update path, or (b) rejecting with an unknown-flag diagnostic and falling back to cold init. Cross-platform verification is not applicable here because this section ships only the ABI surface; the actual runtime live-update machinery owns platform validation when it lands.

> [!NOTE]
> The runtime live-update machinery (kfolio preservation, per-subsystem quiesce callbacks, scheduler drain, VFS writeback before handoff) is OUT OF SCOPE for this section. This section is the ABI contract only -- descriptors, flags, discriminators, validator hook. File the runtime work as a new TODO in 03-memory-concurrency (warm-kernel-update) with reciprocal XREF back to §14 once this ABI section commits.

---

## OS Comparison

| ⭐ | Feature                         | 🪟 Win11                    | 🐧 Linux                     | 🚀 Impossible OS                             |
| --- | ------------------------------- | --------------------------- | ----------------------------- | -------------------------------------------- |
| 💎 | Versioned loader/kernel ABI     | ✅ LPB + extensions         | ✅ boot_params + kernel_info | ⚠️ §1 shipped; §7 §8 open                    |
| 💎 | Typed initrd and module handoff | ✅ ramdisk + boot drivers   | ✅ initrd + initramfs        | ✅ §4 ABI + §5 producer/consumer shipped     |
| ⭐ | Generated ABI manifest          | ⚠️ internal only            | ⚠️ docs + CI                 | ✅ §2 + §3 manifest + drift detector shipped |
| ⭐ | Field-level ownership map       | ⚠️ internal ownership       | ⚠️ scattered docs            | ⚠️ §1 matrix shipped; §10 audit open         |
| 💎 | Capability negotiation          | ✅ loader extensions        | ✅ version + flags           | ✅ §11 required/present/degraded + classify+halt validator |
| 💎 | Boot provenance decision record | ✅ boot status + resume     | ⚠️ cmdline + logs            | ✅ §12 path/reason/source_flags + 7-rule validator + HKLM\SYSTEM\Boot\Decision |
| ⭐ | Friendly stale-loader mismatch  | ✅ recovery codes           | ⚠️ log-driven failures       | ⚠️ §7 kernel fatal + BlackBox transcribe shipped |
| 💎 | Anti-rollback security version  | ✅ OsLoaderSecurityVersion  | ⚠️ shim SBAT revocation only | ⬜ §13 UEFI NVRAM counter                    |
| ⭐ | Warm-kernel-update handoff ABI  | ⚠️ Hot Patch (closed)       | ✅ 6.16 Kexec Handover       | ⬜ §14 ABI only; runtime in new TODO         |
| 💎 | Handoff memory ownership table  | ⚠️ MDL chains + LoaderBlock | ⚠️ memblock + NOMAP regions | ✅ §6 single table + overlap check + JSON dump |

> Parity now covers the contract itself (§1-§4), mirror drift detection (§2-§3), typed payload handoff (§5), centralized PMM reservation (§6), structured version negotiation with friendly fatal + BlackBox transcript (§7), and the canonical protocol reference + schema changelog (§8). Adding explicit capability negotiation, a shared boot decision record, and anti-rollback security-version binding would make this handoff easier to debug and safer to evolve than either Windows' mostly internal loader state or Linux's split between versioned structs and scattered provenance channels. The warm-kernel-update handoff ABI (§14) specifically positions Impossible OS for cloud/server parity with Linux 6.16's Kexec Handover surface at the ABI layer; the runtime live-update machinery is tracked as follow-up in 03-memory-concurrency.

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
- [ ] Add `test_boot_info_rollback_refused`: a fixture with `os_loader_security_version=3` + `required_security_version=5` fails pre-jump with the exact observed/required diagnostic.
- [ ] Add `test_boot_info_rollback_first_boot`: a fixture with `required_security_version=0` accepts any `os_loader_security_version` without raising the counter.
- [ ] Add `test_boot_info_warm_update_payload_accepted`: a payload descriptor with `BOOT_PAYLOAD_WARM_UPDATE_STATE` + known continuation flags passes `boot_warm_update_consume()`.
- [ ] Add `test_boot_info_warm_update_unknown_flag_rejected`: a warm-update descriptor with an unknown continuation bit forces the fail-closed cold-init fallback with a clear diagnostic.
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
- [ ] Anti-rollback fixtures pass on QEMU: downgrade attempt (shipped < required) halts pre-jump; upgrade + opt-in raises `IPOSRequiredSecVersion` via `efivar -l` post-boot inspection.
- [ ] Warm-kernel-update synthetic payload descriptor is consumed by `boot_warm_update_consume()` under QEMU with the continuation region retained; an unknown-flag variant triggers the fail-closed cold fallback with the expected diagnostic.

**Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot)
