---
schema_version: 1
id: hibernation-resume-fast-startup-handoff
domain: 01-boot-platform
status: active
title: "TODO-26 -- Hibernation Resume & Fast Startup Boot Handoff"
---

# TODO-26 -- Hibernation Resume & Fast Startup Boot Handoff

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

> **Goal:** Teach the boot platform how to resume from an S4 hibernation image or fast-startup image before doing a normal cold boot. Power management owns writing the hibernation image, but the boot path owns detecting it, validating it, selecting resume versus cold boot, and handing the image to the kernel safely.
> **Current state:** Power-management TODOs describe S4 and fast startup, but the bootloader has no resume selection path, no hibernation image metadata contract, no resume-failure rollback, and no boot diagnostics for S4.
> [!IMPORTANT] Deferred whole-file (2026-06-17, unattended sequencer + Codex design review): the entire resume boot-path is gated on (1) the kernel hibernation WRITER (02-kernel-core/TODO-26 §4 -- `pm_hibernate_write` + LZ4 + AES-GCM + the resume consumer) which is unimplemented, so no image exists to discover/validate/resume, and (2) bootloader AEAD/HMAC/TPM-seal + an anti-replay TPM-NV monotonic counter, none of which exist (same bootloader-crypto gap that deferred TODO-25 §5). Codex design review: §1 is unsafe as a standalone ABI (would freeze `resume_generation`/AEAD field layout before the cipher/TPM-NV/writer decisions), §5 is dead/false-fail-closed plumbing standalone; no must-ship-now core. Sections stay `[/]` with the shared Deferred stamp until the writer + trust primitives land.

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
- Resume images are fresh (anti-replay generation) and confidential (AEAD-encrypted), not merely integrity-checked.

## Implementation Order

| ⭐  | Order | Deliverable                            | Depends On  | Status |
| --- | :---: | -------------------------------------- | ----------- | :----: |
| 💎  |   1   | Hibernation image metadata format      | D02T26 §4   |  [/]   |
| 💎  |   2   | Bootloader image discovery             | §1          |  [/]   |
| 💎  |   3   | Resume eligibility policy              | §1, T07 §10 |  [/]   |
| 💎  |   4   | Integrity and version validation       | §1, T13 §4  |  [/]   |
| 💎  |   5   | boot_info resume handoff               | T01 §4,§12  |  [/]   |
| 💎  |   6   | Resume failure fallback                | §5, T21 §4  |  [/]   |
| 💎  |   7   | Fast startup mode                      | §1-§6       |  [/]   |
| 💎  |   8   | Diagnostics and BlackBox resume report | §2-§7       |  [/]   |
| 💎  |   9   | Resume tests                           | §1-§8       |  [/]   |

## 1. Hibernation Image Metadata Format

- [ ] Define header with magic, version, kernel build id, boot_info ABI version, root volume id, image size, checksum, flags.
- [ ] Include resume type: full hibernate, fast startup, crash-test image.
- [ ] Include required PCR/Secure Boot state when measured boot is active.
- [ ] Anti-replay `resume_generation`: a monotonic counter in the header written by the D02 T26 writer from a TPM-NV / NVRAM monotonic primitive; the bootloader rejects any image whose generation is not current/highest. -> XREF: T13 §7, T01 §13
- [ ] Encryption metadata: AEAD cipher id + TPM-sealed key id + nonce/tag fields so the bootloader can require an encrypted image and refuse plaintext (debug-only opt-out); the writer owns encryption. -> XREF: D02T26 §4
- [ ] Store metadata in a location readable before normal root mount; this header is the single authoritative on-disk format the D02 T26 §4 writer produces (supersedes the legacy `HIBR_HEADER`). -> XREF: D02T26 §4
- [ ] Commit: `"boot: hibernation image metadata format"`

**Test checkpoint:** A fixture hibernation metadata header parses at known offsets (magic, version, kernel build id, boot_info ABI version, root volume id, image size, checksum, flags, resume type); a bad-magic or truncated header is rejected. Verify on QEMU WHPX/TCG (fixture parse).

> **Test runner:** N/A (deferred -- no code shipped) | validation: deferred until the writer + trust primitives land

> **Deferred:** [H] gated on the kernel hibernation WRITER (no image to resume) + bootloader AEAD/HMAC/TPM-seal + anti-replay TPM-NV (Codex design 2026-06-17: §1 unsafe standalone ABI, §5 dead plumbing) -> XREF: D02T26 §4 (image writer), 01-boot-platform/TODO-25 §5 (bootloader crypto/trust), 01-boot-platform/TODO-13 §7 (TPM-NV anti-replay) + TODO-01 §13 (anti-rollback)

---

## 2. Bootloader Image Discovery

- [ ] Locate hibernation metadata on system, A/B slot, or BlackBox partition.
- [ ] Avoid scanning arbitrary large files before watchdog is armed.
- [ ] Expose discovery failures on VPD and serial.
- [ ] Add boot.conf override `resume=off|auto|force`; `force` only REQUESTS resume and stays subject to §3 eligibility + §4 integrity (never an unconditional bypass).
- [ ] Commit: `"boot: discover hibernation resume images"`

**Test checkpoint:** With a hibernation image on the system / A/B slot / BlackBox partition, the bootloader locates the metadata and logs the source on serial + VPD; with no image it logs `no resume image` and cold-boots; `resume=off` skips discovery, `resume=force` requests resume but §3 eligibility + §4 integrity still gate it. Verify on QEMU WHPX.

> **Test runner:** N/A (deferred -- no code shipped) | validation: deferred until the writer + trust primitives land

> **Deferred:** [H] gated on the kernel hibernation WRITER (no image to resume) + bootloader AEAD/HMAC/TPM-seal + anti-replay TPM-NV (Codex design 2026-06-17: §1 unsafe standalone ABI, §5 dead plumbing) -> XREF: D02T26 §4 (image writer), 01-boot-platform/TODO-25 §5 (bootloader crypto/trust), 01-boot-platform/TODO-13 §7 (TPM-NV anti-replay) + TODO-01 §13 (anti-rollback)

---

## 3. Resume Eligibility Policy

- [ ] Reject resume after kernel update, bootloader ABI mismatch, slot switch, Secure Boot db change, firmware change, or hardware topology change.
- [ ] Allow manual one-time force only from a recovery/diagnostic-menu-issued one-shot NVRAM token (or BootNext provenance); a plain boot.conf `resume=force` cannot override a hard rejection, and force still requires §4 integrity.
- [ ] Integrate with BootNext and boot entry policy.
- [ ] Record selected cold/resume reason.
- [ ] Commit: `"boot: hibernation resume policy"`

**Test checkpoint:** After a simulated kernel update / bootloader ABI mismatch / slot switch / Secure Boot db change / firmware or hardware-topology change, resume is REJECTED with the recorded cold/resume reason and cold boot proceeds; a recovery-menu one-shot-token force overrides exactly once (a plain boot.conf `resume=force` does NOT).  Verify on QEMU WHPX.

> **Test runner:** N/A (deferred -- no code shipped) | validation: deferred until the writer + trust primitives land

> **Deferred:** [H] gated on the kernel hibernation WRITER (no image to resume) + bootloader AEAD/HMAC/TPM-seal + anti-replay TPM-NV (Codex design 2026-06-17: §1 unsafe standalone ABI, §5 dead plumbing) -> XREF: D02T26 §4 (image writer), 01-boot-platform/TODO-25 §5 (bootloader crypto/trust), 01-boot-platform/TODO-13 §7 (TPM-NV anti-replay) + TODO-01 §13 (anti-rollback)

---

## 4. Integrity and Version Validation

- [ ] Verify checksum/HMAC/signature over metadata and image.
- [ ] Validate image physical-memory ranges against current memory map.
- [ ] Validate compressed image algorithm support.
- [ ] Bind to measured boot state where TPM is available.
- [ ] Reject a valid-but-STALE image: compare the header `resume_generation` against the current TPM-NV/NVRAM monotonic value; a lower/non-current generation is rejected even when checksum/HMAC/state all match. -> XREF: T13 §7, T01 §13
- [ ] Require an encrypted image: validate the AEAD metadata (cipher/key-id/nonce) and decrypt-verify with the TPM-sealed key; refuse a plaintext image outside an explicit debug path. -> XREF: D02T26 §4
- [ ] Commit: `"boot: validate hibernation image integrity"`

**Test checkpoint:** A correct checksum/HMAC over metadata + image passes; a single flipped byte is rejected; image physical-memory ranges outside the current memory map are rejected; an unsupported compression algorithm is rejected; TPM-bound state mismatch rejects when measured boot is active. Verify on QEMU WHPX/TCG (fixture).

> **Test runner:** N/A (deferred -- no code shipped) | validation: deferred until the writer + trust primitives land

> **Deferred:** [H] gated on the kernel hibernation WRITER (no image to resume) + bootloader AEAD/HMAC/TPM-seal + anti-replay TPM-NV (Codex design 2026-06-17: §1 unsafe standalone ABI, §5 dead plumbing) -> XREF: D02T26 §4 (image writer), 01-boot-platform/TODO-25 §5 (bootloader crypto/trust), 01-boot-platform/TODO-13 §7 (TPM-NV anti-replay) + TODO-01 §13 (anti-rollback)

---

## 5. boot_info Resume Handoff

- [ ] Add typed payload descriptor for hibernation metadata/image. Use `BOOT_PAYLOAD_HIBERNATION_META` from [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h); set `BOOT_PAYLOAD_FLAG_REQUIRED` on the descriptor so a stale kernel that dropped the type enum fails safe instead of booting past the resume handoff. TODO-01 §4's validator rejects overlap with any retained region before Phase 0 destructive init runs. -> XREF: [`01-boot-platform/TODO-01 §4`](TODO-01-boot-protocol-abi-handoff.md#4-optional-payload-descriptor-array)
- [ ] Add resume flags and shared decision-reason codes to boot_info through TODO-01 §12.
- [ ] Kernel consumes handoff before normal Phase 0 destructive init.
- [ ] PMM reserves image ranges until resume code consumes them.
- [ ] Commit: `"boot: hand off hibernation resume payload"`

**Test checkpoint:** On an eligible resume, boot_info carries the `BOOT_PAYLOAD_HIBERNATION_META` descriptor (`BOOT_PAYLOAD_FLAG_REQUIRED` set, overlap-checked by the TODO-01 §4 validator) plus resume flags + decision-reason codes; the kernel consumes the handoff before Phase 0 destructive init; PMM reserves the image ranges until resume code consumes them. Verify on QEMU WHPX.

> **Test runner:** N/A (deferred -- no code shipped) | validation: deferred until the writer + trust primitives land

> **Deferred:** [H] gated on the kernel hibernation WRITER (no image to resume) + bootloader AEAD/HMAC/TPM-seal + anti-replay TPM-NV (Codex design 2026-06-17: §1 unsafe standalone ABI, §5 dead plumbing) -> XREF: D02T26 §4 (image writer), 01-boot-platform/TODO-25 §5 (bootloader crypto/trust), 01-boot-platform/TODO-13 §7 (TPM-NV anti-replay) + TODO-01 §13 (anti-rollback)

---

## 6. Resume Failure Fallback

- [ ] If resume fails, mark image invalid and cold boot once.
- [ ] Increment failure counter and feed A/B rollback policy if repeated.
- [ ] Preserve failure details in NVRAM and BlackBox.
- [ ] Avoid infinite resume loops.
- [ ] Commit: `"boot: hibernation resume failure fallback"`

**Test checkpoint:** A forced resume failure marks the image invalid, cold-boots exactly once, increments the NVRAM failure counter, and after repeated failures feeds the A/B rollback policy; failure details are preserved in NVRAM + BlackBox; no infinite resume loop occurs. Verify on QEMU WHPX.

> **Test runner:** N/A (deferred -- no code shipped) | validation: deferred until the writer + trust primitives land

> **Deferred:** [H] gated on the kernel hibernation WRITER (no image to resume) + bootloader AEAD/HMAC/TPM-seal + anti-replay TPM-NV (Codex design 2026-06-17: §1 unsafe standalone ABI, §5 dead plumbing) -> XREF: D02T26 §4 (image writer), 01-boot-platform/TODO-25 §5 (bootloader crypto/trust), 01-boot-platform/TODO-13 §7 (TPM-NV anti-replay) + TODO-01 §13 (anti-rollback)

---

## 7. Fast Startup Mode

- [ ] Distinguish kernel-session resume from full user-session hibernate.
- [ ] Allow driver/hardware invalidation list.
- [ ] Integrate with power button and shutdown policy.
- [ ] Expose fast-startup status in boot diagnostics.
- [ ] Commit: `"boot: fast startup handoff"`

**Test checkpoint:** A fast-startup (kernel-session) image resumes the kernel session distinctly from a full user-session hibernate; a driver/hardware on the invalidation list forces cold boot; the power button + shutdown policy select fast startup; fast-startup status appears in boot diagnostics. Verify on QEMU WHPX.

> **Test runner:** N/A (deferred -- no code shipped) | validation: deferred until the writer + trust primitives land

> **Deferred:** [H] gated on the kernel hibernation WRITER (no image to resume) + bootloader AEAD/HMAC/TPM-seal + anti-replay TPM-NV (Codex design 2026-06-17: §1 unsafe standalone ABI, §5 dead plumbing) -> XREF: D02T26 §4 (image writer), 01-boot-platform/TODO-25 §5 (bootloader crypto/trust), 01-boot-platform/TODO-13 §7 (TPM-NV anti-replay) + TODO-01 §13 (anti-rollback)

---

## 8. Diagnostics and BlackBox Resume Report

- [ ] Write `X:\Diag\resume.json`.
- [ ] Include image version, validation status, failure reason, selected path, and duration.
- [ ] Show resume progress in VPD.
- [ ] Add QR failure payload for resume rejection.
- [ ] Commit: `"boot: hibernation resume diagnostics"`

**Test checkpoint:** Post-resume (or post-rejection), `X:\Diag\resume.json` exists with image version, validation status, failure reason, selected path, and duration; VPD shows resume progress; a resume rejection emits a host-decodable QR failure payload. Verify on QEMU WHPX/TCG.

> **Test runner:** N/A (deferred -- no code shipped) | validation: deferred until the writer + trust primitives land

> **Deferred:** [H] gated on the kernel hibernation WRITER (no image to resume) + bootloader AEAD/HMAC/TPM-seal + anti-replay TPM-NV (Codex design 2026-06-17: §1 unsafe standalone ABI, §5 dead plumbing) -> XREF: D02T26 §4 (image writer), 01-boot-platform/TODO-25 §5 (bootloader crypto/trust), 01-boot-platform/TODO-13 §7 (TPM-NV anti-replay) + TODO-01 §13 (anti-rollback)

---

## 9. Resume Tests

- [ ] Fixture valid metadata accepted.
- [ ] Bad checksum rejected.
- [ ] Kernel version mismatch rejected.
- [ ] Resume failure falls back to cold boot.
- [ ] Stale image (lower `resume_generation`) rejected.
- [ ] Plaintext / bad-AEAD-tag image rejected.
- [ ] Plain boot.conf `resume=force` cannot override a hard rejection (kernel-update / ABI / slot / Secure Boot db).
- [ ] Commit: `"test: hibernation boot handoff"`

**Test checkpoint:** The resume test suite passes: valid metadata accepted, bad checksum rejected, kernel-version mismatch rejected, and a resume failure falls back to cold boot. Verify via the `TEST_CAT_BOOT` suite.

> **Test runner:** N/A (deferred -- no code shipped) | validation: deferred until the writer + trust primitives land

> **Deferred:** [H] gated on the kernel hibernation WRITER (no image to resume) + bootloader AEAD/HMAC/TPM-seal + anti-replay TPM-NV (Codex design 2026-06-17: §1 unsafe standalone ABI, §5 dead plumbing) -> XREF: D02T26 §4 (image writer), 01-boot-platform/TODO-25 §5 (bootloader crypto/trust), 01-boot-platform/TODO-13 §7 (TPM-NV anti-replay) + TODO-01 §13 (anti-rollback)

---

## OS Comparison

| ⭐  | Feature                  | 🪟 Win11                   | 🐧 Linux                  | 🚀 Impossible OS              |
| --- | ------------------------ | -------------------------- | ------------------------- | ----------------------------- |
| 💎  | S4 resume selection      | ✅ hiberfil.sys + winload  | ✅ `resume=` kernel param | ⬜ Planned §1-§6              |
| 💎  | Fast startup             | ✅ hybrid boot (hiberboot) | ⚠️ limited distro support  | ⬜ Planned §7                 |
| 💎  | Resume invalidation      | ✅ update/driver policy    | ⚠️ initramfs logic         | ⬜ Planned §3                 |
| ⭐  | BlackBox resume report   | ❌ event logs only         | ❌ journal only           | ⭐ Planned §8 (X:\Diag + QR)  |
| ⭐  | Resume image anti-replay | ❌ none                    | ❌ none                   | ⭐ Planned §4 (TPM-NV gen)    |
| ⭐  | Resume image encryption  | ⚠️ only via BitLocker       | ⚠️ needs encrypted swap    | ⭐ Planned §4 + D02T26 (AEAD) |

---

## Unit Tests

> **Gated:** all cases need an actual hibernation image (the 02-kernel-core/TODO-26 §4 writer, unimplemented) + the bootloader crypto/TPM-seal/anti-replay primitives; they ship with §1-§9, deferred whole-file. See the §1-§9 Deferred stamps.

- [ ] `test_resume_metadata_valid`
- [ ] `test_resume_bad_checksum_rejected`
- [ ] `test_resume_kernel_mismatch_rejected`
- [ ] `test_resume_loop_breaker`
- [ ] `test_resume_stale_generation_rejected`
- [ ] `test_resume_plaintext_rejected`
- [ ] `test_resume_force_cannot_override`

---

## Verification

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | N suites, 0 failures
> **Gated:** the items below need the 02-kernel-core/TODO-26 §4 hibernation writer to produce an image and the bootloader crypto/TPM-seal primitives; they open as §1-§9 ship.

- [ ] QEMU hibernate image fixture
- [ ] Failed resume cold-boot fallback
- [ ] A/B slot switch invalidates resume
- [ ] Bare-metal S4 once power TODO is ready
