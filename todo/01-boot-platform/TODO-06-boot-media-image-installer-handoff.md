---
schema_version: 1
id: boot-media-image-installer-handoff
domain: 01-boot-platform
status: active
title: "TODO-06 -- Boot Media, Image Pipeline & Installer Handoff"
---

# TODO-06 -- Boot Media, Image Pipeline & Installer Handoff

> **Goal:** Make every bootable artifact reproducible, validated, and understood by the boot platform: raw disk, USB image, VHD/VHDX, hybrid ISO, recovery image, installer image, and signed release media. The bootloader should know when it is running from installer/recovery media and hand that state to the kernel cleanly.

> [!IMPORTANT]
> **Current state:** There are scripts for QEMU, USB writing, Secure Boot signing, and disk images. The domain does not own a complete media matrix, artifact manifest, ISO/El Torito path, VHD/VHDX generation, installer handoff, or release verification flow. Win11 ships a Media Creation Tool + Hyper-V VHDX path; Linux ships per-distro hybrid ISOs + cloud VHD/VDI images via `dracut` / `grub2-mkrescue`. Neither verifies a release-key-signed artifact manifest at the bootloader stage -- §7 is the competitive edge.

> [!WARNING]
> **Scope overlap with [`15-installer-release/TODO-01`](../15-installer-release/TODO-01-release-artifacts.md) and [`10-platform-services/TODO-11`](../10-platform-services/TODO-11-installer-iso.md).** The release-pipeline build steps (raw/USB/ISO/VHD/VHDX/VDI generation, artifact manifest production, code signing, VM image variants, release docs) are owned by **D15 T01**; the ISO build script (`make-iso.sh`, El Torito+EFI hybrid) is owned by **D10 T11 §6**; PE+kernel code signing primitives are owned by **D09 T07 §9** (`cng-crypto`). This TODO's authoritative scope is the boot-platform-native surface: §5 media role detection (bootloader reads `/IPOS/role.txt`), §6 `boot_info` handoff of role+artifact UUID+manifest digest, §7 bootloader-side manifest signature verification at load time, and §8 offline inspector. §1-§4 + §9-§10 are kept as scope-boundary anchors that XREF the owning release-pipeline TODOs; do not implement here without the owning TODO's prerequisites. See gap-audit report for the full conflict matrix.

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

| ⭐  | Order | Deliverable                                        | Depends On    | Status |
| --- | :---: | -------------------------------------------------- | ------------- | :----: |
| 💎  |   1   | Boot artifact matrix and manifest format           | T24           |  [x]   |
| 💎  |   2   | Reproducible raw/USB image build                   | §1            |  [x]   |
| 💎  |   3   | VHD/VHDX/VDI conversion and validation             | §1, §2        |  [x]   |
| 💎  |   4   | Hybrid ISO / El Torito UEFI boot                   | §1            |  [x]   |
| 💎  |   5   | Installer/live/recovery media detection            | §1, T07       |  [x]   |
| 💎  |   6   | Bootloader handoff of media role                   | §5, T01 §12   |  [x]   |
| 💎  |   7   | Artifact signing and manifest verification         | §1, T02       |  [ ]   |
| ⭐  |   8   | Offline artifact inspector                         | §1--§7        |  [ ]   |
| 💎  |   9   | CI boot matrix for every artifact                  | §2--§7        |  [ ]   |
| 💎  |  10   | Release checklist and documentation                | §1--§9        |  [ ]   |
| ⭐  |  11   | UKI + network-boot artifact role + manifest path   | §5, §6, §7    |  [ ]   |
| 💎  |  12   | Windows host parity for build + test tooling       | §1--§4, §8    |  [ ]   |

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
> **Accepted:** [M] Bootloader smoke fixtures for installer/recovery/diagnostics + ESP/BlackBox mismatch + oversize role.txt -> XREF: 15-installer-release/TODO-04 §2 (item: "Media-role boot matrix" at line 121)
> **Quality reviewed:** 2026-05-06 | Codex 5x (design, adversarial, re-adversarial, consistency, perf) | 3H+4M+0L fixed, 1M accepted-XREF | scope: kernel-code-quality + boot-code-quality

---

## 6. Bootloader Handoff of Media Role

- [x] `boot_info.boot_media_role` + mismatch field at struct tail; `BOOT_INFO_VERSION` bumped to 17; offset asserts at 23968 + 23972; `dump-fields.inc` updated. Shipped in §5.
- [/] `boot_info.boot_artifact_id[16]` + `boot_manifest_digest[32]` -- deferred to artifact signing (owns the producer for these fields).
- [x] Boot-path decision record reused: `BOOT_REASON_MEDIA_ROLE_MARKER` + Rule 8 path/role consistency in `boot_decision_validate()`. Shipped in §5.
- [x] HKLM `Boot\Device\MediaRole` (REG_SZ) + `MediaRoleEnum`/`MediaRoleMismatch` DWORDs + `boot_media_role_name()` platform API in `boot_hw.c` + `boot_decision.c`.
- [/] Installer/recovery shell launch -- kernel emits non-normal-role serial line pointing at owning TODO; actual shell binaries owned by separate TODOs.
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

> **Scope boundary:** [`09-desktop-shell/TODO-07 §9`](../09-desktop-shell/TODO-07-cng-crypto.md) owns Ed25519 PE32+ COSI-trailer signing primitives (`codesign_sign` / `codesign_verify`). [`15-installer-release/TODO-01 §5`](../15-installer-release/TODO-01-release-artifacts.md) owns the host-side release-signing script. This section owns the bootloader-side verification path -- the path that runs at every boot and refuses load on mismatch -- which is the genuine ⭐ competitive edge.

- [ ] Sign artifact manifests with the release key produced by TODO-02; embed the detached signature next to the manifest (`manifest.json.sig`).
- [ ] Bootloader verifies the manifest signature when Secure Boot is active OR `boot.conf` sets `require_manifest=1`; refuses load on mismatch.
- [ ] Reject modified installer/recovery media unless `boot.conf` sets `allow_unsigned_media=1` (default 0).
- [ ] Include SBAT level + dbx blacklist status in the verification report; surface to `boot_info` so the kernel can warn on degraded trust.
- [ ] Commit: `"boot: verify signed artifact manifests"`

**Test checkpoint:** Tampering with one byte of `kernel.exe` on a signed image makes the bootloader emit `[FAIL] Manifest signature mismatch` and `boot_fatal()` halt under `require_manifest=1`. With `require_manifest=0`, same tamper produces `[WARN] Manifest unverified` and continues. SBAT level below dbx threshold produces `[WARN] SBAT below baseline` with degraded-trust flag set in `boot_info`.

---

## 8. Offline Artifact Inspector

- [ ] Add host tool `tools/bootimg/bootimg.py` (or compiled `bootimg`) supporting `bootimg inspect <image>`.
- [ ] Print partition map, FAT/IXFS labels, manifest content, signature verification result, boot entries, and artifact role.
- [ ] Validate hashes (manifest vs. on-disk) and bootloader/kernel ABI version pin.
- [ ] Support raw, VHD, VHDX, VDI, and ISO image formats (use `qemu-img info` for virtual formats).
- [ ] Commit: `"tools: boot artifact inspector"`

**Test checkpoint:** `python3 tools/bootimg/bootimg.py inspect build/release/disk.img` prints a parseable report listing both partitions, the manifest sha256, signature OK/FAIL, the 4+ boot entries, and `MediaRole=normal`. Same command on `disk.vhdx` and `disk.iso` produces equivalent reports. Tampered image inspection prints `Signature: FAIL` and exit code != 0.

---

## 9. CI Boot Matrix for Every Artifact

> **Scope boundary:** [`01-boot-platform/TODO-28`](TODO-28-boot-validation-certification-matrix.md) owns the broader boot certification matrix (QEMU/VBox/Hyper-V/USB/NVMe/Secure Boot/TPM/network/A-B/recovery/watchdog/hibernation). This section's matrix is artifact-format-only and feeds into TODO-28 as a per-artifact gate. [`15-installer-release/TODO-04`](../15-installer-release/TODO-04-release-qa.md) owns release-QA unattended-install testing. Reciprocal XREF in TODO-28 §(artifact matrix) already exists.

- [ ] QEMU raw disk boot (KVM + TCG fallback per CLAUDE.md smoke-test platform table).
- [ ] QEMU ISO boot.
- [ ] Hyper-V VHDX boot under WHPX.
- [ ] VirtualBox VDI boot.
- [ ] USB image smoke test in loopback (`losetup` + qemu) plus a manual bare-metal gate documented in the release checklist.
- [ ] Commit: `"ci: boot every release artifact"`

**Test checkpoint:** `bash scripts/ci/boot-matrix.sh` runs all five boot configurations and reports a single PASS/FAIL summary. Each configuration pattern-matches `Boot complete in` + `C:\>` on serial within its platform timeout; any failure produces a per-configuration stripped serial log under `build/ci/<config>.log`.

---

## 10. Release Checklist and Documentation

- [ ] Add `docs/release/boot-artifacts.md` with per-format build commands, expected artifact sizes, and verification recipes.
- [ ] Document image writing (`write-usb.sh`), Secure Boot setup (key enrollment, dbx), verification (`bootimg inspect`), and troubleshooting flows.
- [ ] Add release checklist entries for artifact hashes, the §9 boot matrix, and rollback tests.
- [ ] Link from the getting-started guide so first-time users land on the right artifact for their platform.
- [ ] Commit: `"docs: boot media release checklist"`

**Test checkpoint:** `docs/release/boot-artifacts.md` exists and contains a section for each artifact format from §1; every documented command (`build-image.sh`, `to-vhdx.sh`, `bootimg inspect`) runs to completion when copy-pasted. Release checklist contains explicit entries that map 1:1 to the §9 matrix configurations.

---

## 11. Alternate Artifact Formats: UKI and Network Boot

UKI (Unified Kernel Image, single signed PE containing kernel + cmdline + `.initrd` + `.recovery` + `.modules`) and network boot (PXE/HTTP-served kernel) are artifact paths that differ structurally from the raw/USB/VHD/ISO matrix in §1. They must obey the same media-role + manifest-verification contract but read from different sources.

> **Scope boundary:** [`01-boot-platform/TODO-02 §16`](TODO-02-uefi-hardening-secureboot.md) owns UKI section structure + signed-payload extraction; [`01-boot-platform/TODO-25`](TODO-25-network-pxe-http-boot.md) owns PXE/HTTP/TFTP transport + DHCP provenance + network `boot_info` fields. This section pins the cross-format contract -- both artifact paths must publish the media role, artifact UUID, and manifest digest fields from §6 so kernel-side consumers see one shape regardless of source.

- [ ] UKI artifact path: when `BOOT_FLAG_INVOKED_VIA_UKI`, read `boot_info.boot_media_role` from the UKI's `.cmdline` (e.g., `media_role=installer`) instead of `/IPOS/role.txt`; reciprocal XREF in TODO-02 §16.
- [ ] Network-boot artifact path: PXE/HTTP-served kernel publishes `boot_media_role = network` and verifies the DHCP-served `manifest.json.sig` before kernel jump; reciprocal XREF in TODO-25 §6.
- [ ] Document 3-source media-role precedence in §5: UKI `.cmdline` -> DHCP option -> ESP `/IPOS/role.txt` -> default `normal`.
- [ ] Tests: `test_media_role_uki_cmdline` (mock UKI with `media_role=recovery`) and `test_media_role_network_dhcp_option` (mock DHCP).
- [ ] Commit: `"boot: cross-format media role for UKI + network artifacts"`

**Test checkpoint:** UKI artifact boots with `media_role=recovery` parsed from `.cmdline`; PXE/HTTP boot reports `boot_media_role = network` and rejects unsigned manifest. `test_media_role_uki_cmdline` and `test_media_role_network_dhcp_option` both PASS.

---

## 12. Windows Host Parity for Build + Test Tooling

> **Scope boundary:** Bash on Linux / WSL is the canonical host path; this section adds native Windows PowerShell scripts + `.bat` shims so a developer on a Windows test laptop can build, write, and verify release artifacts without WSL. Each PS1 is a peer of the existing `.sh`, not a wrapper around WSL. Detailed recipes per script live in `docs/release/windows-host-tooling.md`.

- [ ] `scripts/release/build-manifest.ps1` -- peer of `build-manifest.sh build|check`; same arg surface, exit codes, stderr grammar; UUID v5 + JSON byte-identical to bash output.
- [ ] `scripts/release/test-build-manifest.ps1` -- peer of `test-build-manifest.sh` (23 assertions); honors the same `BOOT_INFO_ABI_FILE` / `SIGN_FINGERPRINT_FILE` / `SIGN_STAMP_FILE` env contract.
- [ ] `scripts/deploy/write-usb.ps1` -- USB writer via `Get-Disk` / `Initialize-Disk` / `New-Partition`; refuses any disk where `BusType -ne 'USB'`; post-write per-file sha256 cross-check.
- [ ] `scripts/release/to-vhdx.ps1` -- VHDX conversion via `New-VHD` (Hyper-V module) with `qemu-img.exe` fallback; preserves GPT layout + per-partition sha256.
- [ ] `scripts/release/to-iso.ps1` -- hybrid UEFI ISO via `oscdimg.exe` (Windows ADK) with `xorriso.exe` fallback; matches §4 bash output byte-for-byte.
- [ ] `tools/bootimg/bootimg.bat` -- Windows-native shim wrapping `python bootimg.py` (tries `py -3` first, then `python.exe`); enables `bootimg inspect` on a clean WSL-less host.
- [ ] `scripts/debug/release/run-build-manifest-tests.bat` -- new `release/` subdir aggregate runner for §1 harness on Windows; mirrors `scripts/debug/kernel/run-*.bat` shape.
- [ ] `scripts/debug/release/run-write-usb.bat`, `run-to-vhdx.bat`, `run-to-iso.bat` -- one-line per-artifact bat shims invoking the matching PS1.
- [ ] `scripts/debug/release/run-all-release-tests.bat` -- aggregate that chains every per-artifact `run-*.bat` for a one-button Windows release-tooling smoke; mirrors `run-all-kernel-tests.bat`.
- [ ] Cross-host parity test under `tools/bootimg/tests/cross_host/`: PS1 + bash pair produce byte-identical manifests + identical exit codes for identical inputs; runs on both hosts in CI (§9).
- [ ] `docs/release/windows-host-tooling.md` -- per-script Windows-host recipe (Hyper-V module, ADK, qemu-tools, ExecutionPolicy, UAC elevation, signtool path) and the no-WSL guarantee.
- [ ] Commit: `"release: windows-host parity for build + test tooling"`

**Test checkpoint:** On a clean Windows host (PowerShell 5.1+, no WSL), `scripts\debug\release\run-build-manifest-tests.bat` exits 0 with `23 pass, 0 fail`. `build-manifest.ps1 build` and `build-manifest.sh build` produce byte-identical `manifest.json`. `write-usb.ps1` refuses non-USB disks with `[ERROR] disk N is BusType=<x>; only BusType=USB is allowed`.

> **Planned runner:** `scripts\debug\release\run-build-manifest-tests.bat` (Windows host) | 23 assertions, 0 failures (cross-host parity with bash harness). When §12 ships, `scripts/todo-graph/validate.py` `VALID_TEST_LAYERS` extends to `("kernel", "usermode", "desktop", "release")` and the bat-alignment check accepts the new subdir.

**Test checkpoint:** UKI built with `media_role=installer` in `.cmdline` produces `boot_info.boot_media_role == 1` without ESP `role.txt`. PXE-booted kernel with matching DHCP option produces `boot_media_role == 5` (network) and runs manifest verification against HTTP-served `manifest.json.sig`. Conflicting markers across sources produce `[WARN] Media role conflict: <src1>=<v1> vs <src2>=<v2>` and the highest-precedence source wins per the §5 documentation.

---

## OS Comparison

| ⭐  | Feature                              | 🪟 Win11                       | 🐧 Linux                       | 🚀 Impossible OS                 |
| --- | ------------------------------------ | ------------------------------ | ------------------------------ | -------------------------------- |
| 💎  | USB / raw disk image                 | ✅ Media Creation Tool         | ✅ distro raw images           | ✅ §2 build-image.sh + verify    |
| 💎  | ISO UEFI boot (El Torito)            | ✅ Windows ISO                 | ✅ distro hybrid ISO           | ✅ §4 build-iso.sh + boot-test   |
| 💎  | VHD / VHDX virtual disk artifact     | ✅ Hyper-V VHDX                | ⚠️ cloud images per distro     | ✅ §3 to-vhdx.sh + boot-test     |
| 💎  | VDI virtual disk artifact            | ❌ no first-class VDI          | ⚠️ cloud images per distro     | ✅ §3 to-vdi.sh + VBox boot-test |
| 💎  | Installer / recovery media detection | ✅ WinPE / Windows RE          | ✅ live ISO + dracut rescue    | ✅ §5 role.txt marker + bootloader |
| 💎  | Reproducible image build             | ⚠️ partial via WIM tooling     | ⚠️ per-distro reproducibility  | ✅ §2 byte-identical disk.img    |
| ⭐  | Versioned boot artifact schema       | ❌ no unified schema           | ❌ no unified schema           | ✅ §1 v1 schema + check tool     |
| ⭐  | Signed artifact manifest at boot     | ❌ SBAT/dbx only (coarser)     | ❌ SBAT/dbx only (coarser)     | ⬜ planned -- §7                 |
| ⭐  | Offline artifact inspector (1 tool)  | ❌ separate tools per format   | ❌ separate tools per format   | ⬜ planned -- §8                 |
| ⭐  | UKI as a release artifact format     | ❌ no UKI ecosystem            | ✅ systemd-boot UKI            | ⬜ planned -- §11                |
| 💎  | Network-boot kernel + manifest       | ⚠️ WDS / iPXE chainload        | ✅ PXE + HTTP boot + dracut    | ⬜ planned -- §11 + T25          |
| ⭐  | Native Windows + Linux build hosts   | ✅ MSBuild / WDK / ADK only    | ✅ shell tooling only          | ⬜ planned -- §12 PS1 + bat parity |

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
