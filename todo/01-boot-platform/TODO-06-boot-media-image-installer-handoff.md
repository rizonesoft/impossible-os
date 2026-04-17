# TODO-06 -- Boot Media, Image Pipeline & Installer Handoff

> **Goal:** Make every bootable artifact reproducible, validated, and understood by the boot platform: raw disk, USB image, VHD/VHDX, hybrid ISO, recovery image, installer image, and signed release media. The bootloader should know when it is running from installer/recovery media and hand that state to the kernel cleanly.
> **Current state:** There are scripts for QEMU, USB writing, Secure Boot signing, and disk images. The domain does not own a complete media matrix, artifact manifest, ISO/El Torito path, VHD/VHDX generation, installer handoff, or release verification flow.

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

| ⭐ | Order | Deliverable | Depends On | Status |
| --- | :---: | --- | --- | :---: |
| 💎 | 1 | Boot artifact matrix and manifest format | TODO-24 | [ ] |
| 💎 | 2 | Reproducible raw/USB image build | §1 | [ ] |
| 💎 | 3 | VHD/VHDX/VDI conversion and validation | §1, §2 | [ ] |
| 💎 | 4 | Hybrid ISO / El Torito UEFI boot | §1 | [ ] |
| 💎 | 5 | Installer/live/recovery media detection | §1, TODO-07 | [ ] |
| 💎 | 6 | Bootloader handoff of media role | §5, TODO-01 | [ ] |
| 💎 | 7 | Artifact signing and manifest verification | §1, TODO-02 | [ ] |
| ⭐ | 8 | Offline artifact inspector | §1-§7 | [ ] |
| 💎 | 9 | CI boot matrix for every artifact | §2-§7 | [ ] |
| 💎 | 10 | Release checklist and documentation | §1-§9 | [ ] |

## 1. Boot Artifact Matrix and Manifest Format

- [ ] Define supported artifacts: raw, USB, VHD, VHDX, VDI, ISO, recovery, installer.
- [ ] Add `build/artifacts/manifest.json` with partition map, file hashes, Secure Boot signature status, and boot target.
- [ ] Include bootloader, kernel, boot entries, BlackBox skeleton, and recovery payloads.
- [ ] Fail release packaging if any artifact lacks a manifest.
- [ ] Commit: `"release: boot artifact manifest"`

## 2. Reproducible Raw/USB Image Build

- [ ] Make disk image generation deterministic: timestamps, GUID policy, partition ordering, FAT labels.
- [ ] Verify ESP contains expected `BOOTX64.EFI`, kernel, boot.conf, boot entries, and signatures.
- [ ] Add USB write verification by rereading partition table and hashes.
- [ ] Record image build inputs in manifest.
- [ ] Commit: `"release: reproducible raw USB images"`

## 3. VHD/VHDX/VDI Conversion and Validation

- [ ] Add conversion scripts for Hyper-V VHDX and VirtualBox VDI.
- [ ] Verify converted images preserve GPT, ESP, BlackBox, and IXFS partitions.
- [ ] Add sparse/dynamic image metadata to manifest.
- [ ] Boot-test each virtual disk format.
- [ ] Commit: `"release: validate virtual disk artifacts"`

## 4. Hybrid ISO / El Torito UEFI Boot

- [ ] Build ISO with FAT ESP image as El Torito UEFI boot image.
- [ ] Include installer/recovery payloads and artifact manifest.
- [ ] Verify QEMU and VirtualBox boot from ISO.
- [ ] Document that legacy BIOS boot is unsupported unless TODO-08 enables it.
- [ ] Commit: `"release: hybrid UEFI ISO artifact"`

## 5. Installer/Live/Recovery Media Detection

- [ ] Define media role markers on ESP and BlackBox.
- [ ] Bootloader reads role marker before loading kernel.
- [ ] Roles: normal, installer, live, recovery, manufacturing, diagnostics.
- [ ] Role feeds boot entry policy and boot_config.
- [ ] Commit: `"boot: detect boot media role"`

## 6. Bootloader Handoff of Media Role

- [ ] Extend boot_info via TODO-01 with media role, artifact id, manifest digest, and source path.
- [ ] Kernel exposes role via Registry and platform APIs.
- [ ] Installer mode starts installer shell instead of normal desktop.
- [ ] Recovery mode starts recovery environment when local system is broken.
- [ ] Commit: `"boot: hand off boot media role"`

## 7. Artifact Signing and Manifest Verification

- [ ] Sign artifact manifests with release key.
- [ ] Bootloader verifies manifest signature when Secure Boot is active or `require_manifest=1`.
- [ ] Reject modified installer/recovery media unless explicitly allowed.
- [ ] Include SBAT and dbx status in verification.
- [ ] Commit: `"boot: verify signed artifact manifests"`

## 8. Offline Artifact Inspector

- [ ] Add host tool `bootimg inspect <image>`.
- [ ] Print partition map, labels, manifest, signatures, boot entries, and artifact role.
- [ ] Validate hashes and bootloader/kernel ABI version.
- [ ] Support raw, VHD/VHDX, VDI, and ISO where possible.
- [ ] Commit: `"tools: boot artifact inspector"`

## 9. CI Boot Matrix for Every Artifact

- [ ] QEMU raw disk boot.
- [ ] QEMU ISO boot.
- [ ] Hyper-V VHDX boot.
- [ ] VirtualBox VDI boot.
- [ ] USB image smoke test in loopback plus bare-metal manual gate.
- [ ] Commit: `"ci: boot every release artifact"`

## 10. Release Checklist and Documentation

- [ ] Add `docs/release/boot-artifacts.md`.
- [ ] Document image writing, Secure Boot setup, verification, and troubleshooting.
- [ ] Add release checklist entries for artifact hashes, boot tests, and rollback tests.
- [ ] Link from getting-started guides.
- [ ] Commit: `"docs: boot media release checklist"`

## OS Comparison

| ⭐ | Feature | Windows | Linux | Impossible OS |
| --- | --- | --- | --- | --- |
| 💎 | USB/disk image | Media Creation Tool | distro images | TODO-06 |
| 💎 | ISO UEFI boot | Windows ISO | distro ISO | TODO-06 §4 |
| 💎 | VHD boot artifact | Hyper-V | cloud images | TODO-06 §3 |
| ⭐ | signed artifact manifest at boot | limited | distro-specific | TODO-06 §7 |

## Unit Tests

- [ ] `test_artifact_manifest_parse`
- [ ] `test_media_role_marker`
- [ ] `test_manifest_hash_rejects_modified_kernel`
- [ ] `test_bootimg_inspector_fixture`

## Verification

- [ ] QEMU raw/ISO boot
- [ ] Hyper-V VHDX boot
- [ ] VirtualBox VDI boot
- [ ] Bare-metal USB boot

