---
schema_version: 1
id: hibernation-resume-fast-startup-handoff
domain: 01-boot-platform
status: active
title: "TODO-26 -- Hibernation Resume & Fast Startup Boot Handoff"
---

# TODO-26 -- Hibernation Resume & Fast Startup Boot Handoff

> **Goal:** Teach the boot platform how to resume from an S4 hibernation image or fast-startup image before doing a normal cold boot. Power management owns writing the hibernation image, but the boot path owns detecting it, validating it, selecting resume versus cold boot, and handing the image to the kernel safely.
> **Current state:** Power-management TODOs describe S4 and fast startup, but the bootloader has no resume selection path, no hibernation image metadata contract, no resume-failure rollback, and no boot diagnostics for S4.

## Inputs

- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c)
- [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h)
- -> XREF: `../02-kernel-core/TODO-26-power-management.md §4,§11` -- hibernation image writer and fast startup
- -> XREF: `TODO-01-boot-protocol-abi-handoff.md §4` -- typed hibernation payload descriptor
- -> XREF: `TODO-21-ab-boot-rollback.md` -- resume failure participates in rollback
- -> XREF: `TODO-07-boot-entry-store-menu-policy.md §10` -- resume targets are a first-class `kind: resume` entry kind

## Outcome

- Bootloader detects valid hibernation/fast-startup images and selects resume only when safe.
- Kernel receives validated image metadata and can resume without rediscovering policy.
- Resume failures fall back to cold boot and preserve diagnostics.
- A/B updates and Secure Boot invalidate stale hibernation images.

## Implementation Order

| ⭐ | Order | Deliverable | Depends On | Status |
| --- | :---: | --- | --- | :---: |
| 💎 | 1 | Hibernation image metadata format | D02T26 §4 | [ ] |
| 💎 | 2 | Bootloader image discovery | §1 | [ ] |
| 💎 | 3 | Resume eligibility policy | §1, T07 §10 | [ ] |
| 💎 | 4 | Integrity and version validation | §1, T13 §4 | [ ] |
| 💎 | 5 | boot_info resume handoff | T01 §4,§12 | [ ] |
| 💎 | 6 | Resume failure fallback | §5, T21 §4 | [ ] |
| 💎 | 7 | Fast startup mode | §1-§6 | [ ] |
| 💎 | 8 | Diagnostics and BlackBox resume report | §2-§7 | [ ] |
| 💎 | 9 | Resume tests | §1-§8 | [ ] |

## 1. Hibernation Image Metadata Format

- [ ] Define header with magic, version, kernel build id, boot_info ABI version, root volume id, image size, checksum, flags.
- [ ] Include resume type: full hibernate, fast startup, crash-test image.
- [ ] Include required PCR/Secure Boot state when measured boot is active.
- [ ] Store metadata in a location readable before normal root mount.
- [ ] Commit: `"boot: hibernation image metadata format"`

**Test checkpoint:** A fixture hibernation metadata header parses at known offsets (magic, version, kernel build id, boot_info ABI version, root volume id, image size, checksum, flags, resume type); a bad-magic or truncated header is rejected. Verify on QEMU WHPX/TCG (fixture parse).

---

## 2. Bootloader Image Discovery

- [ ] Locate hibernation metadata on system, A/B slot, or BlackBox partition.
- [ ] Avoid scanning arbitrary large files before watchdog is armed.
- [ ] Expose discovery failures on VPD and serial.
- [ ] Add boot.conf override `resume=off|auto|force`.
- [ ] Commit: `"boot: discover hibernation resume images"`

**Test checkpoint:** With a hibernation image on the system / A/B slot / BlackBox partition, the bootloader locates the metadata and logs the source on serial + VPD; with no image it logs `no resume image` and cold-boots; `resume=off` skips discovery, `resume=force` selects it. Verify on QEMU WHPX.

---

## 3. Resume Eligibility Policy

- [ ] Reject resume after kernel update, bootloader ABI mismatch, slot switch, Secure Boot db change, firmware change, or hardware topology change.
- [ ] Allow manual one-time force only from recovery/diagnostic menu.
- [ ] Integrate with BootNext and boot entry policy.
- [ ] Record selected cold/resume reason.
- [ ] Commit: `"boot: hibernation resume policy"`

**Test checkpoint:** After a simulated kernel update / bootloader ABI mismatch / slot switch / Secure Boot db change / firmware or hardware-topology change, resume is REJECTED with the recorded cold/resume reason and cold boot proceeds; a recovery-menu one-time force overrides exactly once. Verify on QEMU WHPX.

---

## 4. Integrity and Version Validation

- [ ] Verify checksum/HMAC/signature over metadata and image.
- [ ] Validate image physical-memory ranges against current memory map.
- [ ] Validate compressed image algorithm support.
- [ ] Bind to measured boot state where TPM is available.
- [ ] Commit: `"boot: validate hibernation image integrity"`

**Test checkpoint:** A correct checksum/HMAC over metadata + image passes; a single flipped byte is rejected; image physical-memory ranges outside the current memory map are rejected; an unsupported compression algorithm is rejected; TPM-bound state mismatch rejects when measured boot is active. Verify on QEMU WHPX/TCG (fixture).

---

## 5. boot_info Resume Handoff

- [ ] Add typed payload descriptor for hibernation metadata/image. Use `BOOT_PAYLOAD_HIBERNATION_META` from [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h); set `BOOT_PAYLOAD_FLAG_REQUIRED` on the descriptor so a stale kernel that dropped the type enum fails safe instead of booting past the resume handoff. TODO-01 §4's validator rejects overlap with any retained region before Phase 0 destructive init runs. -> XREF: [`01-boot-platform/TODO-01 §4`](TODO-01-boot-protocol-abi-handoff.md#4-optional-payload-descriptor-array)
- [ ] Add resume flags and shared decision-reason codes to boot_info through TODO-01 §12.
- [ ] Kernel consumes handoff before normal Phase 0 destructive init.
- [ ] PMM reserves image ranges until resume code consumes them.
- [ ] Commit: `"boot: hand off hibernation resume payload"`

**Test checkpoint:** On an eligible resume, boot_info carries the `BOOT_PAYLOAD_HIBERNATION_META` descriptor (`BOOT_PAYLOAD_FLAG_REQUIRED` set, overlap-checked by the TODO-01 §4 validator) plus resume flags + decision-reason codes; the kernel consumes the handoff before Phase 0 destructive init; PMM reserves the image ranges until resume code consumes them. Verify on QEMU WHPX.

---

## 6. Resume Failure Fallback

- [ ] If resume fails, mark image invalid and cold boot once.
- [ ] Increment failure counter and feed A/B rollback policy if repeated.
- [ ] Preserve failure details in NVRAM and BlackBox.
- [ ] Avoid infinite resume loops.
- [ ] Commit: `"boot: hibernation resume failure fallback"`

**Test checkpoint:** A forced resume failure marks the image invalid, cold-boots exactly once, increments the NVRAM failure counter, and after repeated failures feeds the A/B rollback policy; failure details are preserved in NVRAM + BlackBox; no infinite resume loop occurs. Verify on QEMU WHPX.

---

## 7. Fast Startup Mode

- [ ] Distinguish kernel-session resume from full user-session hibernate.
- [ ] Allow driver/hardware invalidation list.
- [ ] Integrate with power button and shutdown policy.
- [ ] Expose fast-startup status in boot diagnostics.
- [ ] Commit: `"boot: fast startup handoff"`

**Test checkpoint:** A fast-startup (kernel-session) image resumes the kernel session distinctly from a full user-session hibernate; a driver/hardware on the invalidation list forces cold boot; the power button + shutdown policy select fast startup; fast-startup status appears in boot diagnostics. Verify on QEMU WHPX.

---

## 8. Diagnostics and BlackBox Resume Report

- [ ] Write `X:\Diag\resume.json`.
- [ ] Include image version, validation status, failure reason, selected path, and duration.
- [ ] Show resume progress in VPD.
- [ ] Add QR failure payload for resume rejection.
- [ ] Commit: `"boot: hibernation resume diagnostics"`

**Test checkpoint:** Post-resume (or post-rejection), `X:\Diag\resume.json` exists with image version, validation status, failure reason, selected path, and duration; VPD shows resume progress; a resume rejection emits a host-decodable QR failure payload. Verify on QEMU WHPX/TCG.

---

## 9. Resume Tests

- [ ] Fixture valid metadata accepted.
- [ ] Bad checksum rejected.
- [ ] Kernel version mismatch rejected.
- [ ] Resume failure falls back to cold boot.
- [ ] Commit: `"test: hibernation boot handoff"`

**Test checkpoint:** The resume test suite passes: valid metadata accepted, bad checksum rejected, kernel-version mismatch rejected, and a resume failure falls back to cold boot. Verify via the `TEST_CAT_BOOT` suite.

---

## OS Comparison

| ⭐  | Feature                | 🪟 Win11                  | 🐧 Linux                  | 🚀 Impossible OS                |
| --- | ---------------------- | ------------------------- | ------------------------- | ------------------------------- |
| 💎  | S4 resume selection    | ✅ hiberfil.sys + winload | ✅ `resume=` kernel param | ⬜ Planned §1-§6                |
| 💎  | Fast startup           | ✅ hybrid boot (hiberboot)| ⚠️ limited distro support | ⬜ Planned §7                   |
| 💎  | Resume invalidation    | ✅ update/driver policy   | ⚠️ initramfs logic        | ⬜ Planned §3                   |
| ⭐  | BlackBox resume report | ❌ event logs only        | ❌ journal only           | ⭐ Planned §8 (X:\Diag + QR)   |

---

## Unit Tests

- [ ] `test_resume_metadata_valid`
- [ ] `test_resume_bad_checksum_rejected`
- [ ] `test_resume_kernel_mismatch_rejected`
- [ ] `test_resume_loop_breaker`

---

## Verification

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | N suites, 0 failures

- [ ] QEMU hibernate image fixture
- [ ] Failed resume cold-boot fallback
- [ ] A/B slot switch invalidates resume
- [ ] Bare-metal S4 once power TODO is ready
