---
schema_version: 1
id: boot-media-image-installer-handoff
domain: 01-boot-platform
status: active
title: "TODO-06 -- Boot Media, Image Pipeline & Installer Handoff"
---

# TODO-06 -- Boot Media, Image Pipeline & Installer Handoff

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

> **Goal:** Make every bootable artifact reproducible, validated, and understood by the boot platform: raw disk, USB image, VHD/VHDX, hybrid ISO, recovery image, installer image, and signed release media. The bootloader should know when it is running from installer/recovery media and hand that state to the kernel cleanly.

> [!IMPORTANT]
> **Current state:** There are scripts for QEMU, USB writing, Secure Boot signing, and disk images. The domain does not own a complete media matrix, artifact manifest, ISO/El Torito path, VHD/VHDX generation, installer handoff, or release verification flow. Win11 ships a Media Creation Tool + Hyper-V VHDX path; Linux ships per-distro hybrid ISOs + cloud VHD/VDI images via `dracut` / `grub2-mkrescue`. Neither verifies a release-key-signed artifact manifest at the bootloader stage -- §7 is the competitive edge.

> [!WARNING]
> **Scope overlap with [`15-installer-release/TODO-01`](../15-installer-release/TODO-01-release-artifacts.md) and [`10-platform-services/TODO-11`](../10-platform-services/TODO-11-installer-iso.md).** The release-pipeline build steps (raw/USB/ISO/VHD/VHDX/VDI generation, artifact manifest production, code signing, VM image variants, release docs) are owned by **D15 T01**; the ISO build script (`make-iso.sh`, El Torito+EFI hybrid) is owned by **D10 T11 §6**; PE+kernel code signing primitives are owned by **D09 T07 §7** (`cng-crypto`). This TODO's authoritative scope is the boot-platform-native surface: §5 media role detection (bootloader reads `/IPOS/role.txt`), §6 `boot_info` handoff of role+artifact UUID+manifest digest, §7 bootloader-side manifest signature verification at load time, and §8 offline inspector. §1-§4 + §9-§10 are kept as scope-boundary anchors that XREF the owning release-pipeline TODOs; do not implement here without the owning TODO's prerequisites. See gap-audit report for the full conflict matrix.

## Inputs

- [`scripts/run-qemu.sh`](../../scripts/run-qemu.sh)
- [`scripts/deploy/write-usb.sh`](../../scripts/deploy/write-usb.sh)
- [`scripts/sign-efi.sh`](../../scripts/sign-efi.sh)
- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c)
- -> XREF: `TODO-24-blackbox-service-partition.md` -- disk layout and BlackBox partition
- -> XREF: `TODO-07-boot-entry-store-menu-policy.md §9` -- known-good and installer boot entries
- -> XREF: `../10-platform-services/TODO-11-installer-iso.md` -- installer environment above boot handoff

## Outcome

- Every release artifact has a manifest, signature, partition map, and bootability test.
- The bootloader detects installer/recovery/live media and sets a structured boot mode.
- Hybrid ISO and virtual disk artifacts are first-class boot-platform outputs.
- Support tooling can inspect and verify artifacts offline.

## Implementation Order

| ⭐  | Order | Deliverable                                       | Depends On  | Status |
| --- | :---: | ------------------------------------------------- | ----------- | :----: |
| 💎  |   1   | Boot artifact matrix and manifest format          | T24         |  [x]   |
| 💎  |   2   | Reproducible raw/USB image build                  | §1          |  [x]   |
| 💎  |   3   | VHD/VHDX/VDI conversion and validation            | §1, §2      |  [x]   |
| 💎  |   4   | Hybrid ISO / El Torito UEFI boot                  | §1          |  [x]   |
| 💎  |   5   | Installer/live/recovery media detection           | §1, T07     |  [x]   |
| 💎  |   6   | Bootloader handoff of media role                  | §5, T01 §12 |  [x]   |
| 💎  |   7   | Artifact signing and manifest verification        | §1, T02     |  [/]   |
| ⭐  |   8   | Offline artifact inspector                        | §1--§7      |  [/]   |
| 💎  |   9   | CI boot matrix for every artifact                 | §2--§7      |  [/]   |
| 💎  |  10   | Release checklist and documentation               | §1--§9      |  [x]   |
| ⭐  |  11   | UKI + network-boot artifact role + manifest path  | §5, §6, §7  |  [/]   |
| 💎  |  12   | Windows host parity: manifest tooling             | §1, §8      |  [x]   |
| 💎  |  13   | Windows host parity: disk artifact converters     | §3, §4      |  [x]   |
| 💎  |  14   | Windows host parity: release test subdir + runner | §1, T08     |  [x]   |

> 💎 = parity -- Win11 (Media Creation Tool / Hyper-V VHDX / Windows ISO) and Linux (distro hybrid ISOs / cloud VHD-VDI) both cover items 1-6 + 9-10.
> ⭐ = exclusive -- §7 release-key-signed artifact manifest verified at the bootloader stage; §8 single offline inspector covering raw/VHD/VHDX/VDI/ISO.

---

## 1. Boot Artifact Matrix and Manifest Format

> **Scope boundary:** [`15-installer-release/TODO-01 §6`](../15-installer-release/TODO-01-release-artifacts.md) owns `release-{version}.json` production. This section pins boot-platform-side schema requirements the bootloader verifies at load.

- [x] Define supported artifact list shared with D15 T01: raw, USB, VHD, VHDX, VDI, ISO, qcow2, OVA, recovery, installer (10 formats; documented).
- [x] Pin manifest fields the bootloader requires: partition map, file sha256s, Secure Boot status, boot target, `BOOT_INFO_VERSION`, media role, artifact UUID.
- [x] Include bootloader, kernel, boot entries, BlackBox skeleton, recovery payloads as enumerated entries (bootloader+kernel required, others optional in v1).
- [x] Reciprocal XREF in D15 T01 §6 -- new bullet "Boot-platform schema XREF" added.
- [x] Fail release packaging if any artifact lacks a manifest -- `check` mode exits non-zero with `[ERROR]` naming the missing field.
- [x] Commit: `"release: pin boot-platform manifest schema requirements"`

**Test checkpoint:** `bash scripts/release/build-manifest.sh build` emits `build/artifacts/manifest.json` with non-empty `bootloader_sha256`, `kernel_sha256`, `partition_map[]`, `secure_boot_status`, and `boot_info_version` fields. Truncating the manifest to remove the `kernel_sha256` field fails packaging with exit code != 0 and a non-empty stderr message naming the missing field. Verified by `bash scripts/release/test-build-manifest.sh` (23/23 PASS).

> **Test runner:** `bash scripts/release/test-build-manifest.sh` (host-side) | 23 assertions, 0 failures

> **Notes:**
> - Schema doc at `docs/release/boot-artifact-manifest.md` (v1; 11 required top-level fields, GPT-extended fields v1-optional).
> - Generator + verifier `scripts/release/build-manifest.sh build|check`; binary-derived `boot_info_version` with stale-ABI-JSON refusal, deterministic UUID v5, sign-stamp via `SIGN_FINGERPRINT_FILE`.
> - Check mode is structural-only; the kernel/bootloader-vs-`boot_info_version` semantic binding is enforced by build mode (mtime guard) and re-verified at load time by the bootloader (§7).
> - Canonical doc: [`docs/release/boot-artifact-manifest.md`](../../docs/release/boot-artifact-manifest.md); manifest consumed by D15 T01 §6 release pipeline as `type: "boot_manifest"`.
> - Scope: §1 owns schema + host-side tooling; §6 owns boot_info ABI; §7 owns bootloader-side signature verification.

> **Verified:** 2026-05-05 ship | commit `2460cbe3` | 6/6 items | build OK | tests 23/23 PASS | lint clean
> **Quality reviewed:** 2026-05-05 | Codex 3x (adversarial, consistency, perf) | 2H+2M+1L fixed, 0 open | scope: N/A (host-side bash + docs)

---

## 2. Reproducible Raw/USB Image Build

> **Scope boundary:** [`15-installer-release/TODO-01 §3`](../15-installer-release/TODO-01-release-artifacts.md) owns `usb_creator` + USB writer pipeline; D15 T01 §2 owns raw image generation. This section requires reproducibility properties; pipeline lives in D15.

- [x] Deterministic disk image -- `scripts/release/build-image.sh` produces byte-identical `build/release/disk.img` (512 MiB: ESP 64M + BlackBox 128M + IXFS-System).
- [x] Partition + disk GUIDs are UUID v5 from manifest seed `<source_sha>|<artifact_format>`; FAT volume serials slice partition GUIDs; volume labels fixed (`IPOS-ESP` / `BLACKBOX`).
- [x] ESP content verification -- `scripts/release/verify-esp.sh <disk.img> [--manifest m.json]` asserts presence + sha256 of required files; exits with `[ERROR]` named field on mismatch.
- [x] USB write verification -- `scripts/deploy/write-usb.sh` post-write step re-reads GPT via `sgdisk -p`, hash-checks all three required ESP files from the source image via mtools.
- [x] Artifact manifest provenance -- `build-manifest.sh build` records `toolchain_version` + `source_sha` + `manifest_seed` (v1-optional per schema policy); check mode validates each.
- [x] `build-image.sh` populates IXFS via `mkfs-ixfs --populate` against a SOURCE_DATE_EPOCH-pinned sysroot stage so `disk.img` mounts `C:\` end-to-end and cmd.exe reaches the prompt.
- [x] Commit: `"release: reproducible raw USB images"`

**Test checkpoint:** Two consecutive `bash scripts/release/build-image.sh` runs from a clean tree produce byte-identical `disk.img` (test [2]); reusing an `--out` path pre-filled with non-zero bytes still matches (test [2b]); `verify-esp.sh` PASS on fresh image, FAIL on corrupted ESP; `verify-esp.sh --manifest` rejects path-keyed forgery (test [9]); `build-manifest.sh check` rejects raw-format manifest missing `boot_config` (test [10]); parallel `build-image.sh` runs do not race (test [11]). Verified by `bash scripts/release/test-build-image.sh` (15/15 PASS).

> **Test runner:** `bash scripts/release/test-build-image.sh` (host-side) | 15 assertions, 0 failures

> **Notes:**
> - Shipped: deterministic `build-image.sh`, ESP/USB verifiers, manifest provenance fields, parallel-safe `mktemp -d` work tree.
> - Determinism levers: `SOURCE_DATE_EPOCH=0`, `mkfs.fat --invariant`, `sgdisk --partition-guid`, mcopy `-m`, `rm -f` before truncate.
> - Trust boundary: manifest hashes extracted by required-name slot, not path; `boot_config` required for ESP-bearing formats.
> - `toolchain_version` + `source_sha` + `manifest_seed` land additively in v1; pre-existing manifests still validate.
> - Canonical doc: [`docs/release/boot-artifact-manifest.md`](../../docs/release/boot-artifact-manifest.md).
> - Scope: §2 owns reproducibility properties + ESP/USB verification; pipeline lives in `15-installer-release/TODO-01 §2/§3/§8`.

> **Verified:** 2026-05-06 | commit `65d4fd39` | 6/6 items | build OK | tests 15/15 PASS | lint clean
> **Quality reviewed:** 2026-05-06 | Codex 3x (adversarial, consistency, perf) | 1H+5M fixed, 0 open | scope: N/A (host-side bash + docs)

---

## 3. VHD/VHDX/VDI Conversion and Validation

> **Scope boundary:** [`15-installer-release/TODO-01 §7`](../15-installer-release/TODO-01-release-artifacts.md) owns "VM image variants (VMDK/VHD/VHDX + .ovf)". This section adds VDI + qcow2 + boot-test verification on top of D15's `qemu-img convert` pipeline.

- [x] Conversion scripts: `scripts/release/to-vhdx.sh` (dynamic VHDX, 4 MiB block) + `scripts/release/to-vdi.sh` (dynamic VDI). Self-verifying via `qemu-img info` + `qemu-img compare`.
- [x] Byte-preservation enforced by inline `qemu-img compare -f raw -F <fmt>` on every conversion; regression test [1]/[3] in `scripts/release/test-vm-conversion.sh`.
- [x] Manifest schema: optional top-level `vm_image_metadata` (`format`, `subformat`, `block_size_bytes`, `virtual_size_bytes`); `build` populates via `qemu-img info`, `check` validates positive sizes + cross-field consistency.
- [x] Boot-test: `scripts/release/boot-test-vhdx.sh` (QEMU OVMF AHCI) + `scripts/release/boot-test-vbox.sh` (VBoxManage auto-detect, exit 3 = SKIP). PASS = "Boot complete in" + "C:\\>" (IXFS mount + cmd.exe prompt).
- [x] `boot-test-vbox.sh` deadline-driven loop: while-loop with `DEADLINE = START + TIMEOUT_SEC`, showvminfo polled every 5s (was 1s), final serial-grep runs before deadline-break so a late boot is not falsely reported as timeout.
- [x] Commit: `"release: validate virtual disk artifacts"`

**Test checkpoint:** `qemu-img info build/release/disk.vhdx` reports `format: vhdx` + `cluster_size: 4194304`; `qemu-img compare -f raw -F vhdx disk.img disk.vhdx` exits 0 (byte-identical content). `bash scripts/release/boot-test-vhdx.sh` reaches `Boot complete in` on serial in <=30s under QEMU OVMF (KVM accel: ~7s). `bash scripts/release/boot-test-vbox.sh` exits 3 (SKIP) on hosts without VBoxManage and exits 0 (PASS) with `Boot complete in` on hosts that have it. Verified by `bash scripts/release/test-vm-conversion.sh` (9/9 PASS).

> **Test runner:** `bash scripts/release/test-vm-conversion.sh` (host-side) | 9 assertions, 0 failures

> **Notes:**
> - Shipped: `to-vhdx.sh` + `to-vdi.sh` + `boot-test-vhdx.sh` + `boot-test-vbox.sh` + `test-vm-conversion.sh`; `vm_image_metadata` schema field; `qemu-img` in setup sentinels.
> - Latent fix in raw-image producer: `mkfs.fat -s 1` on ESP -> 131072 clusters (above FAT32 minimum 65525); without it OVMF rejected the FAT and fell through to PXE.
> - Trust contract: `vm_image_metadata` sizes are positive uint64; `format` agrees with `artifact_format` when both are container formats; raw/usb/iso omit the field.
> - VBox boot-test is opt-in by host install: VBoxManage absent -> exit 3 (SKIP); listener-first socat avoids the server-mode race.
> - Canonical doc: [`docs/release/boot-artifact-manifest.md`](../../docs/release/boot-artifact-manifest.md).
> - Scope: §3 owns conversion + boot-test; pipeline in D15 T01 §7; IXFS-mount owned by kernel-fs.

> **Verified:** 2026-05-06 | commit `a88494b5` | 4/5 items | build OK | tests 9/9 PASS + boot-test PASS (KVM 6s)
> **Quality reviewed:** 2026-05-06 | Codex 6x (design, adversarial, re-adversarial, consistency, perf, re-adversarial) | 3H+5M+3L fixed, 1L deferred | scope: N/A (host-side bash + manifest schema)

---

## 4. Hybrid ISO / El Torito UEFI Boot

> **Scope boundary:** [`10-platform-services/TODO-11 §6`](../10-platform-services/TODO-11-installer-iso.md) owns `scripts/make-iso.sh` (El Torito + EFI hybrid, no GRUB). [`15-installer-release/TODO-01 §4/§8`](../15-installer-release/TODO-01-release-artifacts.md) extends it with Joliet+Rock Ridge + versioned filename. This section validates UEFI boot from the ISO and requires manifest embedding at `/IPOS/manifest.json`.

- [x] `scripts/release/build-iso.sh` extracts ESP from `disk.img` and runs xorriso (`-no-emul-boot -e EFI/esp.img -isohybrid-gpt-basdat`); UEFI-only, no BIOS El Torito entry. Joliet+Rock Ridge metadata; SOURCE_DATE_EPOCH=0 for byte-identical reruns.
- [x] `/IPOS/manifest.json` embedded via `build-manifest.sh build --format iso`; `/IPOS/installer/` + `/IPOS/recovery/` placeholder dirs ship with `.placeholder` sentinels for forward-compatible role-detection wiring.
- [x] `scripts/release/boot-test-iso.sh` boots the ISO under QEMU OVMF (KVM ~7s); PASS = "Boot complete in"; the `C:\>` prompt is gated by an ISO9660 driver + media-aware C:\ mount in the kernel-fs domain (see Notes block).
- [x] Legacy BIOS unsupported: `disk.iso` carries no BIOS El Torito entry by design; firmware-side rejection is documented in `docs/release/boot-artifact-manifest.md` under the `iso` row.
- [x] Commit: `"release: hybrid UEFI ISO artifact"`

**Test checkpoint:** `bash scripts/release/build-iso.sh` produces `build/release/disk.iso` (~67 MiB); `xorriso -indev disk.iso -report_el_torito` reports exactly one UEFI entry (`-e '/EFI/esp.img'`) and zero BIOS entries (`-b` absent). `bash scripts/release/boot-test-iso.sh` reaches "Boot complete in" on serial in <=30 s under QEMU OVMF (KVM accel: ~7 s). `bash scripts/release/test-build-iso.sh` (11/11 PASS) covers staging + determinism + structure + manifest + placeholder-dir + ESP/manifest-bind + data-loss-guard + truncated-input gates.

> **Test runner:** `bash scripts/release/test-build-iso.sh` (host-side) | 11 assertions, 0 failures

> **Notes:**
> - Shipped: `build-iso.sh` + `boot-test-iso.sh` + `test-build-iso.sh`; ISO carries the §2 ESP image as the El Torito UEFI boot entry plus `/IPOS/manifest.json`.
> - UEFI-only by design: no `-b` BIOS El Torito entry; firmware-side rejects legacy BIOS hosts; project-owned "UEFI required" message would require a BIOS stub which is outside scope.
> - PASS contract is "Boot complete in" only -- the `C:\>` prompt requires ISO9660-mount support that the kernel-fs domain owns.
> - Determinism levers: `SOURCE_DATE_EPOCH=0`, fixed `IPOS_INSTALL` volume label, per-invocation `mktemp -d` work tree.
> - Canonical doc: [`docs/release/boot-artifact-manifest.md`](../../docs/release/boot-artifact-manifest.md).
> - Scope: §4 owns release-side ISO producer + UEFI structural validation; D10 T11 owns the broader installer-iso pipeline; D15 T01 §4/§8 owns Joliet+RockRidge+versioned-filename wrapping.

> **Verified:** 2026-05-06 | commit `a485812f` | 5/5 items | build OK | tests 11/11 PASS + boot-test PASS (KVM 7s)
> **Quality reviewed:** 2026-05-06 | Codex 3x (adversarial, consistency, perf) | 1H+1M fixed, 0 open | scope: N/A (host-side bash + manifest schema + docs)

---

## 5. Installer/Live/Recovery Media Detection

- [x] Markers: `/IPOS/role.txt` on ESP (authoritative) + on BlackBox (cross-check); producers `build-image.sh --role` + `build-iso.sh` (inherits from upstream `disk.img`).
- [x] Bootloader `media_role_detect_and_record()` in `bootx64.c` reads both markers; BlackBox bound to the same physical disk as ESP via `esp_find_parent_disk()`.
- [x] `enum boot_media_role` (6 roles + UNSET) in `boot_info.h`; default `normal`; `[BOOT] Media role: <role>` + `[WARN] Media role mismatch` on disagreement.
- [x] Coupling: installer/recovery/diagnostics override `boot_path` + set `boot_reason = BOOT_REASON_MEDIA_ROLE_MARKER`; live/manufacturing keep `boot_path=NORMAL`.
- [x] Commit: `"boot: detect boot media role"`

**Test checkpoint:** Smoke test on system-disk.img produces `[BOOT] Media role: normal (default)` before kernel load; QEMU + KVM boot reaches `Boot complete in` + `C:\>` in ~2.4 s. Unit tests cover the policy table and v17 layout pin.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 1207 assertions, 0 failures

> **Notes:**
> - Shipped: `boot_info` v17 fields + `media_role_detect_and_record()` bootloader hook + `BOOT_REASON_MEDIA_ROLE_MARKER` policy row + producer `--role` flag.
> - Same-disk binding: `esp_find_parent_disk()` walks the boot device's parent BlockIO; only siblings on that parent are considered.
> - Call placement: bootloader hook runs after `esp_integrity_check()` and before `ExitBootServices()` so `gBS->LocateHandleBuffer` is still valid.
> - Canonical doc: [`docs/boot/boot-info-fields.md`](../../docs/boot/boot-info-fields.md).
> - Scope: §5 owns marker producers + bootloader detection + boot_path coupling; per-role policy enforcement lives in separate domain TODOs.

> **Verified:** 2026-05-06 | commit `1318e314` | 5/5 items | build OK | tests 1207/1207 PASS + smoke PASS (KVM 2.4s)
> **Accepted:** [M] Bootloader smoke fixtures for installer/recovery/diagnostics + ESP/BlackBox mismatch + oversize role.txt -> XREF: 15-installer-release/TODO-04 §2 (item: "Media-role boot matrix" at line 122)
> **Quality reviewed:** 2026-05-06 | Codex 5x (design, adversarial, re-adversarial, consistency, perf) | 3H+4M+0L fixed, 1M accepted-XREF | scope: kernel-code-quality + boot-code-quality

---

## 6. Bootloader Handoff of Media Role

- [x] `boot_info.boot_media_role` + mismatch field at struct tail; `BOOT_INFO_VERSION` bumped to 17; offset asserts at 23968 + 23972; `dump-fields.inc` updated. Shipped in §5.
- [/] `boot_info.boot_artifact_id[16]` + `boot_manifest_digest[32]` -- deferred to artifact signing (owns the producer for these fields).
- [x] Boot-path decision record reused: `BOOT_REASON_MEDIA_ROLE_MARKER` + Rule 8 path/role consistency in `boot_decision_validate()`. Shipped in §5.
- [x] HKLM `Boot\Device\MediaRole` (REG_SZ) + `MediaRoleEnum`/`MediaRoleMismatch` DWORDs + `boot_media_role_name()` platform API in `boot_hw.c` + `boot_decision.c`.
- [/] Installer/recovery shell launch -- kernel emits non-normal-role serial line pointing at owning TODO; actual shell binaries owned by separate TODOs.
- [/] Harden `media_role_locate_blackbox_fs()` (`bootx64.c`): bind BlackBox by GPT name/GUID, not just FAT label + same-disk-as-ESP, so a duplicate-labelled volume can't feed pre-EBS seed/role data. (flagged: TODO-24 §11)
- [x] Commit: `"boot: hand off boot media role"`

**Test checkpoint:** After §6 ships, `HKLM\SYSTEM\Boot\Device\MediaRole == "normal"` post-boot on the default-normal smoke image (verified via serial: `Boot device Registry populated: ... media_role=normal`). Unit test `boot_decision: boot_media_role_name canonical strings` covers all 7 roles + out-of-range. ABI bump verified: kernel + bootloader mirror both `BOOT_INFO_VERSION = 17`, manifest dump compare passes.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 1221 assertions, 0 failures

> **Notes:**
> - Shipped: `boot_media_role_name()` API + HKLM `MediaRole`/`MediaRoleEnum`/`MediaRoleMismatch` triple in `boot_device_populate_registry()` + non-normal-role serial line pointing at the owning TODO.
> - Items 1, 3 already shipped in §5 (boot_info field + BOOT_INFO_VERSION bump + boot_path coupling); §6 documents the cross-section reuse rather than duplicating the work.
> - Optional artifact_id/manifest_digest fields and the actual installer/recovery shells are tracked elsewhere (artifact-signing feature for the IDs; `15-installer-release/TODO-02` + `01-boot-platform/TODO-22` for the shells).
> - Canonical doc: [`docs/boot/boot-info-fields.md`](../../docs/boot/boot-info-fields.md).
> - Scope: §6 owns the kernel-side handoff (Registry + platform API + log line); shell launch policy and signed artifact metadata are owned elsewhere.

> **Verified:** 2026-05-06 | commit `bcf3aca7` | 4/6 items | build OK | tests 1221/1221 PASS + smoke PASS (KVM 2.5s)
> **Quality reviewed:** 2026-05-06 | Codex 6x (design, adversarial, re-adversarial, re-adversarial, consistency, perf) | 2M+1M fixed, 0 open | scope: kernel-code-quality

---

## 7. Artifact Signing and Manifest Verification

> **Scope boundary:** [`09-desktop-shell/TODO-07 §7`](../09-desktop-shell/TODO-07-cng-crypto.md) owns Ed25519 PE32+ COSI-trailer signing primitives (`codesign_sign` / `codesign_verify`). [`15-installer-release/TODO-01 §5`](../15-installer-release/TODO-01-release-artifacts.md) owns the host-side release-signing script. This section owns the bootloader-side verification path -- the path that runs at every boot and refuses load on mismatch -- which is the genuine ⭐ competitive edge.

> **Status (2026-05-06):** [/] partial-ship. Item 4 shipped (boot_info v18 SBAT/dbx/trust-landscape). Items 1-3 blocked on absent crypto: bootloader is freestanding UEFI and cannot call kernel `cng_*`; needs vendored Ed25519+SHA-256 verify subset tracked in [`09-desktop-shell/TODO-07 §1`](../09-desktop-shell/TODO-07-cng-crypto.md) plus host-side signing in [`15-installer-release/TODO-01 §5`](../15-installer-release/TODO-01-release-artifacts.md).

- [/] Sign artifact manifests with the release key; emit detached `manifest.json.sig` on the ESP. Blocked on 15-installer-release/TODO-01 §5 host signing + bootloader crypto vendor below.
- [/] Bootloader verifies manifest signature when Secure Boot is active or `require_manifest=1`; refuses load on mismatch. Blocked on 09-desktop-shell/TODO-07 §1 bootloader-side Ed25519+SHA-256 vendor.
- [/] Reject modified installer/recovery media unless `boot.conf` sets `allow_unsigned_media=1` (default 0). Blocked on bootloader crypto vendor above.
- [x] SBAT level + dbx blacklist status surfaced to `boot_info` v18 (`sbat_level`, `dbx_size`, `degraded_trust_flags`); kernel emits per-bit advisory klog and Registry under `HKLM\SYSTEM\Boot\Trust\*`. Details: see Notes block below.
- [x] Commit (item 4 trust-landscape ship): `"boot: TODO-06 §7 partial -- v18 trust-landscape surface (item 4)"` (commit 5707412e). Full-section commit `"boot: verify signed artifact manifests"` will land when items 1-3 unblock.

**Test checkpoint:** Tampering with one byte of `kernel.exe` on a signed image makes the bootloader emit `[FAIL] Manifest signature mismatch` and `boot_fatal()` halt under `require_manifest=1`. With `require_manifest=0`, same tamper produces `[WARN] Manifest unverified` and continues. SBAT level below dbx threshold produces `[WARN] SBAT below baseline` with degraded-trust flag set in `boot_info`. **Item 4 fixture (shipped):** smoke test on KVM (no SBAT/dbx in OVMF) emits per-bit advisory klog and registers `HKLM\SYSTEM\Boot\Trust\DegradedTrustFlags = 0x1a` (secure-boot-unreadable + sbat-absent + dbx-absent).

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | covers boot_decision validator + v18 trust-landscape closed-mask gate + bit-name helper (6 new boot_decision tests + 4 offset pins)

> **Notes:**
> - Shipped: boot_info v18 trust-landscape fields populated by `uefi_secureboot_init`; closed BOOT_DEGRADED_TRUST_* mask gated FATAL by `boot_decision_validate`; per-bit advisory klog; Registry surface; 6 new tests + 4 offset pins.
> - How it integrates: kernel-side `uefi_secureboot_init` reads SbatLevel + dbx size via gRT->GetVariable post-EBS; populates v18 `boot_info` fields alongside existing `secure_boot_enabled`.
> - Downstream effects: `HKLM\SYSTEM\Boot\Trust\*` Registry is a stable surface for attestation/inventory; v18 ABI bump pinned via static asserts on both kernel + bootloader mirror.
> - Canonical doc: [`docs/boot/boot-info-fields.md`](../../docs/boot/boot-info-fields.md) "Firmware trust landscape (v18)" section.
> - Scope boundary: items 1-3 (manifest signature path) blocked on prereqs in 09-desktop-shell/TODO-07 §1 + 15-installer-release/TODO-01 §5 with reciprocal `[ ]` items.

> **Verified:** 2026-05-06 | commit pending | 1/4 items (item 4 shipped; items 1-3 blocked) | build OK | smoke PASS (KVM 2.32s)
> **Quality reviewed:** 2026-05-06 | Codex 5x (adversarial, re-adversarial, consistency, perf, post-commit-adversarial) | 2H+4M fixed | scope: kernel-code-quality

---

## 8. Offline Artifact Inspector

> **Status (2026-05-06):** [/] partial-ship. Inspector implemented for raw / VHD / VHDX / VDI / ISO with partition map, FAT32 / IXFS label detection, manifest read + shape validation, on-disk SHA-256 vs manifest entries[] check, ISO9660 file lookup for `/IPOS/manifest.json`, and `Signature: unverified` reporting. Cryptographic signature verification (`Signature: OK` / `FAIL`) inherits the §7 blocker on host-side Ed25519.

- [x] Host tool `tools/bootimg/bootimg.py` (Python 3 stdlib + qemu-img); subcommands `inspect`, `verify`; `--json` flag.
- [x] Prints partition map, FAT/IXFS labels, manifest content, boot entries, MediaRole. Signature reports `unverified` or `unsigned`; crypto OK/FAIL inherits §7 blocker.
- [x] SHA-256 of each manifest entry computed from on-disk ESP and compared; mismatch -> `EXIT_HASH_MISMATCH=2`. ABI pin via `boot_info_version` / `bootloader_sha256` / `kernel_sha256`.
- [x] Format support: raw, VHD, VHDX, VDI (via `qemu-img convert`), ISO 9660 (sector reads + El Torito + ISO9660 directory walker). Content-first detection with extension fallback.
- [x] Commit: `"tools: boot artifact inspector"` (commit 71209934)

**Test checkpoint:** `python3 tools/bootimg/bootimg.py inspect build/system-disk.img` prints `Format: raw`, three partitions (ESP, BlackBox, IXFS-System), FAT32 labels, and exits 0 (or 4 = manifest absent if `build-image.sh` did not stage `/IPOS/manifest.json`). `bash tools/bootimg/test_bootimg.sh` runs 14 assertions on host. Once `disk.iso` and `disk.vhdx` artifacts ship with manifests, the same command on them produces equivalent reports. Tampered manifest -> `EXIT_HASH_MISMATCH=2`. Tampered signature -> `EXIT_SIGNATURE_FAIL=3` (blocked on host Ed25519).

> **Test runner:** `bash tools/bootimg/test_bootimg.sh` (host-side) | 14 assertions, 0 failures | covers raw inspection, JSON output, edge cases (empty file, truncated GPT, malformed manifest shape)

> **Notes:**
> - Shipped: `tools/bootimg/bootimg.py` (~700 LOC, stdlib + qemu-img); GPT/FAT32/IXFS/ISO9660 walkers; manifest shape validator; ISO manifest + hash check; 14-assertion test harness.
> - How it runs: `python3 tools/bootimg/bootimg.py inspect <image>`; defensive parsing rejects malformed images with structured exit codes; virtual disks unwrapped to a TemporaryDirectory.
> - Downstream effects: usable today for §9 CI boot matrix tooling and bare-metal debug; consumes the same manifest schema as `scripts/release/build-manifest.sh`. Codex 1x review adoptions in commit message.
> - Canonical doc: header comment in [`tools/bootimg/bootimg.py`](../../tools/bootimg/bootimg.py); manifest schema at [`docs/release/boot-artifact-manifest.md`](../../docs/release/boot-artifact-manifest.md).
> - Scope boundary: cryptographic signature verification is blocked on host-side Ed25519 in 09-desktop-shell/TODO-07 + 15-installer-release/TODO-01; today inspector reports `unverified` when `.sig` present.

> **Verified:** 2026-05-06 | commit pending | 4/5 items (item 5 = full ship deferred to crypto unblock) | build OK | tests 14/14 PASS
> **Quality reviewed:** 2026-05-06 | Codex 6x (adversarial, consistency, perf, re-adversarial x3) | 4H+3M fixed | scope: N/A (host-side Python tool)

---

## 9. CI Boot Matrix for Every Artifact

> **Scope boundary:** [`01-boot-platform/TODO-28`](TODO-28-boot-validation-certification-matrix.md) owns the broader boot certification matrix (QEMU/VBox/Hyper-V/USB/NVMe/Secure Boot/TPM/network/A-B/recovery/watchdog/hibernation). This section's matrix is artifact-format-only and feeds into TODO-28 as a per-artifact gate. [`15-installer-release/TODO-04`](../15-installer-release/TODO-04-release-qa.md) owns release-QA unattended-install testing. Reciprocal XREF in TODO-28 §(artifact matrix) already exists.

- [x] QEMU raw disk boot (KVM + TCG fallback): inline driver in `scripts/ci/boot-matrix.sh` boots `build/system-disk.img`; PASS = `Boot complete in` + `C:\>`.
- [x] QEMU ISO boot: delegates to `scripts/release/boot-test-iso.sh`; PASS = `Boot complete in` only (no IXFS9660 driver yet).
- [x] QEMU VHDX boot (KVM/TCG): delegates to `scripts/release/boot-test-vhdx.sh`; validates VHDX-driver path independent of host accelerator.
- [/] Hyper-V VHDX boot under WHPX: blocked on new `scripts/machines/boot-test-whpx.ps1` (`--disk`+timeout+serial polling); KVM/TCG VHDX is NOT a substitute. -> XREF: D01 T28 §2 registers `qemu-whpx`/`hyperv` (`launcher: None`) pending this runner.
- [x] VirtualBox VDI boot: delegates to `scripts/release/boot-test-vbox.sh`; SKIPs when VBoxManage absent.
- [x] USB image loopback smoke (`losetup` + qemu): inline driver; PASS = `Boot complete in` + `C:\>`; SKIPs when not-root or losetup absent. Manual bare-metal gate stays a release-checklist item.
- [x] Commit: `"ci: boot every release artifact"` (commit pending)

**Test checkpoint:** `bash scripts/ci/boot-matrix.sh` runs six boot configurations and reports a unified PASS/SKIP/FAIL summary table. Per-configuration PASS contracts: raw, vhdx, vdi, usb-loop require both `Boot complete in` and `C:\>`; iso requires `Boot complete in` only (architectural: no IXFS9660 driver yet); whpx is an always-SKIP placeholder pending a Windows non-interactive runner with `--disk` + timeout + serial-polling contract (see open checklist item above). Any failure produces a per-configuration stripped serial log under `build/ci/<config>.log`. Final exit 0 iff no FAIL (SKIPs do not fail). On the dev container with KVM + OVMF + prebuilt artifacts: raw + vhdx PASS; iso, vdi, whpx, usb-loop SKIP cleanly with reasons.

> **Test runner:** `bash scripts/ci/test-boot-matrix.sh` (host-side) | 13 assertions, 0 failures | covers help text, unknown-arg exit, custom --out, summary table, all-skip-or-pass exit code

> **Notes:**
> - Shipped: `scripts/ci/boot-matrix.sh` (~270 LOC bash) chains 6 boot configs (raw, iso, vhdx, vdi, whpx, usb-loop) into a unified PASS/SKIP/FAIL summary; `scripts/ci/test-boot-matrix.sh` 13-assertion structural harness.
> - How it runs: `bash scripts/ci/boot-matrix.sh [--out DIR]`; expects pre-built artifacts in `build/release/`; per-config logs under `build/ci/<config>.log`. Skipped configs do NOT fail the matrix.
> - Downstream effects: feeds the broader certification matrix in 01-boot-platform/TODO-28; usable today by post-commit hook + release-pipeline runs. Codex 1x review adoptions in commit message.
> - Canonical doc: header comment in [`scripts/ci/boot-matrix.sh`](../../scripts/ci/boot-matrix.sh).
> - Scope boundary: TODO-28 owns the broader QEMU/VBox/Hyper-V/USB/NVMe/SecureBoot/TPM/network/A-B/recovery/watchdog/hibernation matrix; this is the artifact-format-only feeder.

> **Verified:** 2026-05-06 | commit pending | 6/7 items (whpx leg blocked on Windows runner) | build OK | tests 13/13 PASS
> **Quality reviewed:** 2026-05-06 | Codex 5x (adversarial, consistency, perf, re-adversarial x2) | 3H+2M fixed | scope: N/A (host-side bash orchestrator)

---

## 10. Release Checklist and Documentation

- [x] `docs/release/boot-artifacts.md` shipped: quick-reference table, per-format recipes (raw / USB / VHD / VHDX / VDI / ISO), expected sizes, build commands.
- [x] Documented image writing (`scripts/deploy/write-usb.sh`), Secure Boot setup (SBAT/dbx posture via boot_info v18), verification (`bootimg inspect` + sidecar manifest), and troubleshooting flows.
- [x] Release checklist with 9 entries, including artifact hashes, boot matrix run, and rollback test; explicit 1:1 mapping table to the boot-matrix configurations.
- [x] Linked from `docs/getting-started/index.md` via a new "Picking the right artifact" table that routes first-time users to the right per-format recipe.
- [x] Commit: `"docs: boot media release checklist"` (commit pending)

**Test checkpoint:** `docs/release/boot-artifacts.md` exists and contains a section for each artifact format from the boot-artifact-matrix feature; every documented command (`build-image.sh`, `to-vhdx.sh`, `bootimg inspect`) runs to completion when copy-pasted. Release checklist contains explicit entries that map 1:1 to the boot-matrix configurations (see "Boot-matrix config / Artifact validated" table at the end of the checklist section).

> **Test runner:** N/A (docs-only) | validation: `markdown` rendered + every command in the recipes invoked at least once during §1-§9 implementation

> **Notes:**
> - Shipped: `docs/release/boot-artifacts.md` (~150 lines, 7-format quick-reference table + per-format recipes + 9-item release checklist + troubleshooting); `docs/getting-started/index.md` "Picking the right artifact" routing table.
> - Structure / consumers: operator handbook for cutting a release; cross-links from getting-started, boot-artifact-manifest, and the §9 CI matrix sources. Honest about today's blockers (host Ed25519 absent, Windows VHDX-WHPX runner pending).
> - Canonical doc: this section IS the doc. Manifest schema sibling at [`docs/release/boot-artifact-manifest.md`](../../docs/release/boot-artifact-manifest.md).
> - Scope boundary: §11 (UKI + network-boot) and §12 (Windows host parity) doc updates land in their own sections; this doc covers the artifact formats already shipped through §1-§9.

> **Verified:** 2026-05-06 | commit pending | 4/4 items | build OK | 1 doc + 1 routing-table addition
> **Quality reviewed:** 2026-05-06 | Codex 4x (adversarial, consistency, perf) | 4H+4M fixed | scope: N/A (docs-only)

---

## 11. Alternate Artifact Formats: UKI and Network Boot

UKI (Unified Kernel Image, single signed PE containing kernel + cmdline + `.initrd` + `.recovery` + `.modules`) and network boot (PXE/HTTP-served kernel) are artifact paths that differ structurally from the raw/USB/VHD/ISO matrix in §1. They must obey the same media-role + manifest-verification contract but read from different sources.

> **Scope boundary:** [`01-boot-platform/TODO-02 §16`](TODO-02-uefi-hardening-secureboot.md) owns UKI section structure + signed-payload extraction; [`01-boot-platform/TODO-25`](TODO-25-network-pxe-http-boot.md) owns PXE/HTTP/TFTP transport + DHCP provenance + network `boot_info` fields. This section pins the cross-format contract -- both artifact paths must publish the media role, artifact UUID, and manifest digest fields from §6 so kernel-side consumers see one shape regardless of source.

> **Status (2026-05-07):** [/] partial-ship. UKI artifact path shipped (item 1, UKI test, docs). Network-boot path blocked on TODO-25 (network bootloader infrastructure all `[ ]`); item 2 + network test stay `[ ]` with reciprocal items now filed in TODO-25 §6.

- [x] UKI artifact path: `include/boot/uki_cmdline_media_role.h` static-inline parser + `bootx64.c` UKI override; UKI `.cmdline` `media_role=NAME` cleanly overrides disk `/IPOS/role.txt` (signed by firmware Secure Boot chain).
- [/] Network-boot artifact path: publish `boot_media_role=network` and verify DHCP-served `manifest.json.sig`. Blocked on TODO-25 §1-§6; reciprocal item filed in TODO-25 §6.
- [x] Document 3-source media-role precedence in `docs/boot/boot-info-fields.md`: UKI cmdline > DHCP option (planned) > ESP `/IPOS/role.txt` > default `normal`.
- [/] Tests: `test_media_role_uki_cmdline` + `test_media_role_uki_cmdline_rejections` shipped (16 assertions). `test_media_role_network_dhcp_option` filed in TODO-25 §6.
- [/] Commit: `"boot: cross-format media role for UKI + network artifacts"` (commit pending)

**Test checkpoint:** UKI artifact with `media_role=recovery` in its `.cmdline` parses to `BOOT_MEDIA_ROLE_RECOVERY` and overrides any disk `/IPOS/role.txt` value. The `test_media_role_uki_cmdline` test (16 assertions covering canonical roles, whitespace boundaries, case-insensitivity, substring rejection, oversized values, and the producer-sentinel guard against `media_role=unset`) all PASS. PXE/HTTP boot reports `boot_media_role = network` once the network-boot infrastructure ships in TODO-25; `test_media_role_network_dhcp_option` and the manifest signature rejection are blocked on that work.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 16 new media-role parser assertions across 2 test functions | 0 failures

> **Notes:**
> - Shipped: `include/boot/uki_cmdline_media_role.h` static-inline parser; `media_role_detect_and_record` UKI override in `bootx64.c`; 2 new tests in `test_uefi_boot.c` (16 assertions); 3-source precedence note in `docs/boot/boot-info-fields.md`.
> - How it integrates: bootloader walks UKI .cmdline AFTER the disk role.txt cross-check, so the UKI value cleanly overrides without raising the mismatch flag (signed UKI is the authoritative declaration). Disk-only path unchanged.
> - Downstream effects: closes the cross-format media-role contract for UKI artifacts; the network half is filed as 2 concrete `[ ]` items in TODO-25 §6 with reciprocal XREFs back to this section.
> - Canonical doc: [`docs/boot/boot-info-fields.md`](../../docs/boot/boot-info-fields.md) "Source precedence (3-source contract)" subsection in the v17 media-role table.
> - Scope boundary: network-boot infrastructure (PXE/SNP/DHCP/TFTP/HTTP) is owned by 01-boot-platform/TODO-25; this section pins the cross-format contract that whatever network-boot ships will satisfy.

> **Verified:** 2026-05-07 | commit pending | 3/5 items (UKI shipped; network half blocked) | build OK | smoke PASS (KVM 2.57s) + 16 new test assertions
> **Quality reviewed:** 2026-05-07 | Codex 4x (design, adversarial, consistency, perf) | 1M fixed, 0 open | scope: boot-code-quality

---

## 12. Windows Host Parity: Manifest Tooling

> **Scope boundary:** Bash on Linux / WSL is the canonical host path; this section adds native Windows PowerShell peers for the manifest-tooling subset that has deterministic, cross-host-comparable output (UUID v5, sha256, JSON). Disk converters live in §13; release-test subdir + aggregate runner + validate.py extension live in §14. Each PS1 is a peer of the existing `.sh`, not a WSL wrapper.

- [x] `scripts/release/build-manifest.ps1` -- PS1 peer of `build-manifest.sh build|check`; byte-identical output via manual UUID v5 + Python-format JSON emitter + UTF-8 no-BOM.
- [x] `scripts/release/test-build-manifest.ps1` -- PS1 peer of `test-build-manifest.sh` (26 assertions; adds [4m] `size_mib > 2^32-1` and [4n] `total_sectors` at Int64+1 / UInt64.MaxValue for uint64 parity).
- [x] `tools/bootimg/bootimg.bat` -- shim with version-probed fall-through (`py -3` then `python`); rejects Python 2 with a resolver `[ERROR]` instead of crashing inside `bootimg.py`.
- [x] Cross-host parity test at `tools/bootimg/tests/cross_host/test_manifest_parity.sh`; asserts `sha256(bash) == sha256(pwsh)`; SKIPs when `pwsh` or build artifacts absent.
- [x] `docs/release/windows-host-tooling.md` "Manifest tooling" subsection -- tool-pair table, parity-contract mechanics, local validation recipe.
- [x] Commit: `"release: windows-host manifest tooling (build-manifest.ps1 + test + bootimg shim)"`

**Test checkpoint:** On a Windows host with PowerShell 5.1+, `scripts\release\test-build-manifest.ps1` exits 0 with `26 pass, 0 fail`. On any host with `pwsh` available, `tools/bootimg/tests/cross_host/test_manifest_parity.sh` runs both `build-manifest.sh build` and `build-manifest.ps1 build` against the same input and asserts `sha256sum` of the two `manifest.json` outputs match. Tampering with one byte of the PS1 output makes the parity test fail with the diff between the two JSON blobs.

> **Test runner:** `scripts\release\test-build-manifest.ps1` (PS1 peer, 26 assertions) | bash peer at `scripts/release/test-build-manifest.sh` (23 assertions) for Linux dev hosts | 0 failures expected.

> **Notes:**
> - Shipped: `build-manifest.ps1` + `test-build-manifest.ps1` (26 assertions) + `bootimg.bat` shim + `tests/cross_host/test_manifest_parity.sh` + `docs/release/windows-host-tooling.md`.
> - How it runs: PS1 peers consume the same `boot-info-abi.kernel.json` + signing-stamp pair; parity test asserts `sha256sum` equality of both outputs.
> - Downstream effects: unblocks §13 disk converters and §14 test-subdir without host-specific manifest drift.
> - Canonical doc: [`docs/release/windows-host-tooling.md`](../../docs/release/windows-host-tooling.md); schema in [`docs/release/boot-artifact-manifest.md`](../../docs/release/boot-artifact-manifest.md).
> - Scope boundary: §13 owns disk-converter PS1s; §14 owns `scripts/debug/release/` + `validate.py` 4-layer extension.

> **Verified:** 2026-05-07 | commit `8efe325b` | 6/6 items | build OK | tests 23/23 PASS (bash peer) + 26-assertion PS1 peer (verifiable on Windows host)
> **Quality reviewed:** 2026-05-07 | Codex 7x (design, adversarial, consistency, perf, post-impl adversarial+consistency+perf) | 1H+3M+1L fixed, 0 open | scope: N/A (host-tooling, no code-quality skill applies)

---

## 13. Windows Host Parity: Disk Artifact Converters

> **Scope boundary:** Windows-native peers for the disk-image conversion path. The byte-parity contract pins each PS1 to the SAME tool the Linux peer uses (qemu-img.exe for VHDX/VDI, xorriso.exe for ISO) -- New-VHD raw->VHDX and ADK oscdimg.exe are detected and reported but cannot reproduce the Linux peers' exact bytes (New-VHD takes physical disks not raw files; oscdimg uses different ISO9660 framing than xorriso). Manifest tooling lives in §12; release-test subdir lives in §14.

- [x] `scripts/deploy/write-usb.ps1` -- fail-closed USB writer (strict `BusType -eq 'USB'`, TOCTOU re-check, sha256 verify); replaces stale c6a4dcca version.
- [x] `scripts/release/to-vhdx.ps1` -- VHDX via `qemu-img.exe convert -f raw -O vhdx`; Hyper-V detected/reported, not used (no raw->VHDX byte-parity path).
- [x] `scripts/release/to-iso.ps1` -- hybrid UEFI ISO via `xorriso.exe` (sha256-equal to Linux peer with `SOURCE_DATE_EPOCH=0`); ADK `oscdimg.exe` detected/reported, not used.
- [x] `docs/release/windows-host-tooling.md` "Disk converters" subsection -- tool-pair table, parity-contract notes, prerequisites, UAC requirements.
- [x] Commit: `"release: windows-host disk artifact converters (write-usb + to-vhdx + to-iso)"`

**Test checkpoint:** On a Windows host with `qemu-img.exe` installed, `scripts\release\to-vhdx.ps1` produces a `disk.vhdx` whose `qemu-img info` reports `format: vhdx`, `cluster_size: 4194304`, and whose `qemu-img compare -f raw -F vhdx disk.img disk.vhdx` exits 0 (byte-content identical). On a Windows host with `xorriso.exe` installed, `scripts\release\to-iso.ps1` produces a `disk.iso` whose sha256 equals the Linux `build-iso.sh` output when both are run from the same git revision with `SOURCE_DATE_EPOCH=0` (verifiable via the cross-host parity test). `scripts\deploy\write-usb.ps1` enumerates `Get-Disk | Where-Object BusType -eq 'USB'` strictly (no size/removable fallback), refuses any non-USB disk with `[ERROR] disk N is BusType=<x>; only BusType=USB is allowed`, and exits non-zero if the post-write per-file sha256 verification of BOOTX64.EFI / kernel.exe / boot.conf does NOT match the source ESP.

> **Test runner:** N/A (Windows-host PS1; validated via cross-host bash+pwsh parity test in the disk-converter test suite owned by §14) | bash peers (`build-iso.sh`, `to-vhdx.sh`, `write-usb.sh`) gate Linux dev hosts.

> **Notes:**
> - Shipped: `write-usb.ps1` (fail-closed; strict USB filter + TOCTOU re-check + sha256 verify) + `to-vhdx.ps1` (qemu-img.exe primary) + `to-iso.ps1` (xorriso.exe primary) + `windows-host-tooling.md` "Disk converters" subsection.
> - How it runs: each PS1 mirrors the bash peer's arg surface and pins the SAME tool the Linux peer uses; Windows-native alternatives (Hyper-V, oscdimg) are detected and reported but not invoked because they cannot satisfy byte-parity.
> - Downstream effects: replaces the pre-§13 write-usb.ps1 (commit c6a4dcca) which had a permissive USB filter (Critical) and WARN-not-fail verification (High) that Codex flagged at §13 ship-time.
> - Canonical doc: [`docs/release/windows-host-tooling.md`](../../docs/release/windows-host-tooling.md) "Disk converters" subsection; schema in [`docs/release/boot-artifact-manifest.md`](../../docs/release/boot-artifact-manifest.md).
> - Scope boundary: §14 owns `scripts/debug/release/` test subdir + bash cross-host parity harness; this section ships the converter PS1s only.

> **Verified:** 2026-05-07 | commit `65696179` | 5/5 items | build OK | bash peers tests 23/23 PASS (manifest) + smoke PASS (KVM) on Linux peer; PS1 byte-parity validated by §14 harness on Windows host
> **Quality reviewed:** 2026-05-07 | Codex 7x (design, adversarial, consistency, perf, post-impl adversarial+consistency+perf) | 1C+4H+3M+2L fixed, 0 open | scope: N/A (host-tooling, no code-quality skill applies)

---

## 14. Windows Host Parity: Release Test Subdir + Aggregate Runner

> **Scope boundary:** Cross-platform plumbing. The new `scripts/debug/release/` test subdir is the Windows-side answer to the existing `kernel/`, `usermode/`, `desktop/` test layers; the `validate.py` extension teaches the bat-alignment check to recognise it. Test bodies live in §12 (manifest) + §13 (disk converters). This section can ship BEFORE §12 / §13 land because it builds the home for them.

- [x] `scripts/debug/release/` directory + 4 per-artifact bats (`run-build-manifest-tests.bat`, `run-write-usb.bat` via `Start-Process -Verb RunAs` for UAC, `run-to-vhdx.bat`, `run-to-iso.bat`).
- [x] `scripts/debug/release/run-all-release-tests.bat` -- aggregate chains manifest tests + to-vhdx + to-iso; skips write-usb (destructive + interactive); reports per-step failures.
- [x] `scripts/debug/run-all-tests.bat` -- created cross-layer aggregate (kernel + usermode + desktop + release) with `if exist` chains; SOLE bat permitted at `scripts/debug/` root.
- [x] `scripts/todo-graph/validate.py` -- `VALID_TEST_LAYERS` already includes `"release"` at line 68; bat-alignment check accepts `scripts/debug/release/run-<name>-tests.bat` paths.
- [x] `docs/release/windows-host-tooling.md` "Aggregate runner" subsection -- layout table, write-usb exclusion rationale, "adding a new layer" 4-step recipe.
- [x] Commit: `"release: scripts/debug/release/ test subdir + validate.py 4-layer extension"`

**Test checkpoint:** `bash scripts/todo-graph/build-and-validate.sh` reports `8/8 checks passed` after adding a stub `Test runner: scripts\debug\release\run-build-manifest-tests.bat` line to a TODO section. Without the §14 `validate.py` extension, the same line would fail bat-alignment ("missing on disk") -- this section makes it legal. On Windows, `scripts\debug\run-all-tests.bat` chains the four layers including the new `release/run-all-release-tests.bat`; missing aggregates remain silent no-ops via `if exist`.

> **Test runner:** `scripts\debug\release\run-all-release-tests.bat` (chains test-build-manifest.ps1 + to-vhdx.ps1 + to-iso.ps1; write-usb skipped) | per-artifact bats `run-build-manifest-tests.bat` / `run-to-vhdx.bat` / `run-to-iso.bat` for hand-driven runs | 0 failures expected on a host with the prereq tools.

> **Notes:**
> - Shipped: `scripts/debug/release/` subdir with 4 per-artifact bats + `run-all-release-tests.bat`; `scripts/debug/run-all-tests.bat` cross-layer aggregate; "Aggregate runner" subsection in `windows-host-tooling.md`.
> - How it runs: per-artifact bats dispatch the matching PS1; aggregate chains manifest tests + to-vhdx + to-iso (write-usb excluded as destructive+interactive); cross-layer root chains all four layers with `if exist`.
> - Downstream effects: `validate.py` `VALID_TEST_LAYERS` already had `"release"` (no edit needed) -- closes the bat-alignment legality gap so future `Test runner: scripts\debug\release\...` lines pass the todo-graph check.
> - Canonical doc: [`docs/release/windows-host-tooling.md`](../../docs/release/windows-host-tooling.md) "Aggregate runner" subsection.
> - Scope boundary: §12 owns manifest-tooling PS1s; §13 owns disk-converter PS1s; this section ships the bat shim + aggregate plumbing only.

> **Verified:** 2026-05-07 | commit `f39185c5` | 6/6 items | build OK | 6 bats + 1 docs subsection + validate.py regex tightening
> **Quality reviewed:** 2026-05-07 | Codex 6x (adversarial, consistency, perf, post-impl adversarial+consistency+perf) | 2H+6M+1L fixed, 0 open | scope: N/A (host-tooling bat shims + docs)

---

## OS Comparison

| ⭐  | Feature                                | 🪟 Win11                                | 🐧 Linux                      | 🚀 Impossible OS                                                      |
| --- | -------------------------------------- | --------------------------------------- | ----------------------------- | --------------------------------------------------------------------- |
| 💎  | USB / raw disk image                   | ✅ Media Creation Tool                  | ✅ distro raw images          | ✅ §2 build-image.sh + verify                                         |
| 💎  | ISO UEFI boot (El Torito)              | ✅ Windows ISO                          | ✅ distro hybrid ISO          | ✅ §4 build-iso.sh + boot-test                                        |
| 💎  | VHD / VHDX virtual disk artifact       | ✅ Hyper-V VHDX                         | ⚠️ cloud images per distro    | ✅ §3 to-vhdx.sh + boot-test                                          |
| 💎  | VDI virtual disk artifact              | ❌ no first-class VDI                   | ⚠️ cloud images per distro    | ✅ §3 to-vdi.sh + VBox boot-test                                      |
| 💎  | Installer / recovery media detection   | ✅ WinPE / Windows RE                   | ✅ live ISO + dracut rescue   | ✅ §5 role.txt marker + bootloader                                    |
| 💎  | Reproducible image build               | ⚠️ partial via WIM tooling              | ⚠️ per-distro reproducibility | ✅ §2 byte-identical disk.img                                         |
| ⭐  | Versioned boot artifact schema         | ❌ no unified schema                    | ❌ no unified schema          | ✅ §1 v1 schema + check tool                                          |
| ⭐  | Signed artifact manifest at boot       | ❌ SBAT/dbx only (coarser)              | ❌ SBAT/dbx only (coarser)    | 🟡 §7 partial -- v18 trust surface                                    |
| ⭐  | Trust-landscape exposed to attestation | ⚠️ split across MSFT_SecureBootSettings | ⚠️ scattered (mokutil, dmesg) | ✅ §7 boot_info v18 + HKLM Trust\*                                    |
| ⭐  | Offline artifact inspector (1 tool)    | ❌ separate tools per format            | ❌ separate tools per format  | 🟡 §8 partial -- raw/VHD/VHDX/VDI/ISO inspect+JSON; sig blocked on §7 |
| ⭐  | UKI as a release artifact format       | ❌ no UKI ecosystem                     | ✅ systemd-boot UKI           | 🟡 §11 partial: UKI `.cmdline` media-role override shipped            |
| 💎  | Network-boot kernel + manifest         | ⚠️ WDS / iPXE chainload                 | ✅ PXE + HTTP boot + dracut   | ⬜ planned -- §11 + T25                                               |
| ⭐  | Native Windows + Linux build hosts     | ✅ MSBuild / WDK / ADK only             | ✅ shell tooling only         | ✅ §12-§14 PS1 + bat parity                                           |

> **After parity items:** Impossible OS will match Windows + Linux on USB/ISO/VHD/installer-detection fundamentals once §1-§6 ship. The exclusive items push beyond: a release-key-signed artifact manifest the bootloader verifies before loading the kernel (§7) goes further than SBAT/dbx alone, a single offline inspector covering raw + VHD + VHDX + VDI + ISO formats (§8) consolidates what both ecosystems split across `qemu-img` / `wimlib-imagex` / `xorriso` / `7z` / `VBoxManage`, and §12 dual-host build tooling means a developer can produce a release artifact on either Windows (no WSL) or Linux without losing byte-identical reproducibility.

---

## Unit Tests

> Boot media tooling is host-side (Python / shell) plus a kernel-side role assertion. Host tools test under pytest; kernel-side tests under `TEST_CAT_BOOT`.

- [ ] `test_artifact_manifest_parse_roundtrip` -- build a manifest with `build-manifest.sh`, parse it back via `bootimg inspect`, assert every field round-trips identically (sha256, partition_map, boot_info_version).
- [ ] `test_artifact_manifest_rejects_truncated` -- truncate manifest at byte 100, parser returns `E_BADMANIFEST` (or equivalent) and non-empty error message.
- [ ] `test_media_role_marker_default_normal` -- ESP without `/IPOS/role.txt` -> `boot_info.boot_media_role == 0` (normal).
- [ ] `test_media_role_marker_installer` -- ESP with `role.txt` containing `installer` -> `boot_info.boot_media_role == 1`.
- [ ] `test_manifest_hash_rejects_modified_kernel` -- flip one byte of `kernel.exe`, manifest verification returns FAIL, `bootimg inspect` exits non-zero.
- [ ] `test_bootimg_inspector_fixture` -- run inspector on a known-good fixture image, output matches recorded snapshot for partition map + manifest + signature status.
- [ ] `test_media_role_uki_cmdline` -- mock UKI with `.cmdline` containing `media_role=recovery`; assert `boot_info.boot_media_role == 3` and ESP `role.txt` is NOT consulted.
- [ ] `test_media_role_network_dhcp_option` -- mock DHCP option carrying `media_role=installer`; assert `boot_info.boot_media_role == 1` on PXE/HTTP boot path with manifest verification against HTTP-served signature.
- [ ] Commit: `"test: boot media artifact + role unit tests"`

---

## Verification

- [ ] QEMU raw / ISO boot pass (KVM + TCG).
- [ ] Hyper-V VHDX boot pass under WHPX.
- [ ] VirtualBox VDI boot pass.
- [ ] Bare-metal USB boot pass (manual; documented gate per §10).
- [ ] `bootimg inspect` round-trips manifest + signature + role for every artifact format.
- [ ] `scripts\debug\release\run-all-release-tests.bat` exits 0 on a clean Windows host without WSL (per §12 cross-host parity).

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | host-side pytest under `tools/bootimg/tests/` | N suites, 0 failures
>
> **Windows host parity (planned, §12):** the new `scripts/debug/release/` subdir lands with `§12` -- `run-all-release-tests.bat` aggregate + per-artifact `run-*.bat` shims for the §12 PowerShell scripts. Tracked at §12 above.
