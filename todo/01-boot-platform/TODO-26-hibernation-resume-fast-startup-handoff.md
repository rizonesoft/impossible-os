# TODO-26 -- Hibernation Resume & Fast Startup Boot Handoff

> **Goal:** Teach the boot platform how to resume from an S4 hibernation image or fast-startup image before doing a normal cold boot. Power management owns writing the hibernation image, but the boot path owns detecting it, validating it, selecting resume versus cold boot, and handing the image to the kernel safely.
> **Current state:** Power-management TODOs describe S4 and fast startup, but the bootloader has no resume selection path, no hibernation image metadata contract, no resume-failure rollback, and no boot diagnostics for S4.

## Inputs

- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c)
- [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h)
- -> XREF: `../02-kernel-core/TODO-26-power-management.md §4,§11` -- hibernation image writer and fast startup
- -> XREF: `TODO-01-boot-protocol-abi-handoff.md §4` -- typed hibernation payload descriptor
- -> XREF: `TODO-21-ab-boot-rollback.md` -- resume failure participates in rollback

## Outcome

- Bootloader detects valid hibernation/fast-startup images and selects resume only when safe.
- Kernel receives validated image metadata and can resume without rediscovering policy.
- Resume failures fall back to cold boot and preserve diagnostics.
- A/B updates and Secure Boot invalidate stale hibernation images.

## Implementation Order

| ⭐ | Order | Deliverable | Depends On | Status |
| --- | :---: | --- | --- | :---: |
| 💎 | 1 | Hibernation image metadata format | ../02-kernel-core/TODO-26 §4 | [ ] |
| 💎 | 2 | Bootloader image discovery | §1 | [ ] |
| 💎 | 3 | Resume eligibility policy | §1, TODO-07 | [ ] |
| 💎 | 4 | Integrity and version validation | §1, TODO-13 | [ ] |
| 💎 | 5 | boot_info resume handoff | TODO-01 §4, §12 | [ ] |
| 💎 | 6 | Resume failure fallback | §5, TODO-21 | [ ] |
| 💎 | 7 | Fast startup mode | §1-§6 | [ ] |
| 💎 | 8 | Diagnostics and BlackBox resume report | §2-§7 | [ ] |
| 💎 | 9 | Resume tests | §1-§8 | [ ] |

## 1. Hibernation Image Metadata Format

- [ ] Define header with magic, version, kernel build id, boot_info ABI version, root volume id, image size, checksum, flags.
- [ ] Include resume type: full hibernate, fast startup, crash-test image.
- [ ] Include required PCR/Secure Boot state when measured boot is active.
- [ ] Store metadata in a location readable before normal root mount.
- [ ] Commit: `"boot: hibernation image metadata format"`

## 2. Bootloader Image Discovery

- [ ] Locate hibernation metadata on system, A/B slot, or BlackBox partition.
- [ ] Avoid scanning arbitrary large files before watchdog is armed.
- [ ] Expose discovery failures on VPD and serial.
- [ ] Add boot.conf override `resume=off|auto|force`.
- [ ] Commit: `"boot: discover hibernation resume images"`

## 3. Resume Eligibility Policy

- [ ] Reject resume after kernel update, bootloader ABI mismatch, slot switch, Secure Boot db change, firmware change, or hardware topology change.
- [ ] Allow manual one-time force only from recovery/diagnostic menu.
- [ ] Integrate with BootNext and boot entry policy.
- [ ] Record selected cold/resume reason.
- [ ] Commit: `"boot: hibernation resume policy"`

## 4. Integrity and Version Validation

- [ ] Verify checksum/HMAC/signature over metadata and image.
- [ ] Validate image physical-memory ranges against current memory map.
- [ ] Validate compressed image algorithm support.
- [ ] Bind to measured boot state where TPM is available.
- [ ] Commit: `"boot: validate hibernation image integrity"`

## 5. boot_info Resume Handoff

- [ ] Add typed payload descriptor for hibernation metadata/image.
- [ ] Add resume flags and shared decision-reason codes to boot_info through TODO-01 §12.
- [ ] Kernel consumes handoff before normal Phase 0 destructive init.
- [ ] PMM reserves image ranges until resume code consumes them.
- [ ] Commit: `"boot: hand off hibernation resume payload"`

## 6. Resume Failure Fallback

- [ ] If resume fails, mark image invalid and cold boot once.
- [ ] Increment failure counter and feed A/B rollback policy if repeated.
- [ ] Preserve failure details in NVRAM and BlackBox.
- [ ] Avoid infinite resume loops.
- [ ] Commit: `"boot: hibernation resume failure fallback"`

## 7. Fast Startup Mode

- [ ] Distinguish kernel-session resume from full user-session hibernate.
- [ ] Allow driver/hardware invalidation list.
- [ ] Integrate with power button and shutdown policy.
- [ ] Expose fast-startup status in boot diagnostics.
- [ ] Commit: `"boot: fast startup handoff"`

## 8. Diagnostics and BlackBox Resume Report

- [ ] Write `X:\Diag\resume.json`.
- [ ] Include image version, validation status, failure reason, selected path, and duration.
- [ ] Show resume progress in VPD.
- [ ] Add QR failure payload for resume rejection.
- [ ] Commit: `"boot: hibernation resume diagnostics"`

## 9. Resume Tests

- [ ] Fixture valid metadata accepted.
- [ ] Bad checksum rejected.
- [ ] Kernel version mismatch rejected.
- [ ] Resume failure falls back to cold boot.
- [ ] Commit: `"test: hibernation boot handoff"`

## OS Comparison

| ⭐ | Feature | Windows | Linux | Impossible OS |
| --- | --- | --- | --- | --- |
| 💎 | S4 resume selection | hiberfil.sys/winload | resume= kernel param | TODO-26 |
| 💎 | Fast startup | hybrid boot | limited distro support | TODO-26 §7 |
| 💎 | Resume invalidation | update/driver policy | initramfs logic | TODO-26 §3 |
| ⭐ | BlackBox resume report | event logs | journal | TODO-26 §8 |

## Unit Tests

- [ ] `test_resume_metadata_valid`
- [ ] `test_resume_bad_checksum_rejected`
- [ ] `test_resume_kernel_mismatch_rejected`
- [ ] `test_resume_loop_breaker`

## Verification

- [ ] QEMU hibernate image fixture
- [ ] Failed resume cold-boot fallback
- [ ] A/B slot switch invalidates resume
- [ ] Bare-metal S4 once power TODO is ready
