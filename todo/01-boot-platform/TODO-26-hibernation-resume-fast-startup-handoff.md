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
> **Current state:** The hibernation image metadata contract now EXISTS: `02-kernel-core/TODO-26` §4 shipped the authoritative on-disk format and its LZ4 chunk codec on 2026-09-04 (`include/kernel/pm/hibernate.h`, `src/kernel/pm/hibernate_image.c`), which is what this file was waiting on to define its parser against. Still absent: the writer that produces an image (`02-kernel-core/TODO-26` §28), the bootloader resume selection path, resume-failure rollback, and boot diagnostics for S4.
> [!IMPORTANT] Deferred whole-file (2026-06-17, unattended sequencer + Codex design review): the entire resume boot-path is gated on (1) the kernel hibernation WRITER, now `02-kernel-core/TODO-26` §28 (`pm_hibernate_write` + AES-GCM + the resume consumer), which is unimplemented, so no image exists to discover/validate/resume, and (2) bootloader AEAD/HMAC/TPM-unseal, which does not exist and is owned by `09-desktop-shell/TODO-07` §1 (item: "Bootloader AEAD subset"), not by TODO-25 §5, whose blocker is the unrelated manifest trust model. PARTIALLY UNBLOCKED 2026-09-04: the format half split out as `02-kernel-core/TODO-26` §4 and SHIPPED, so §1 is no longer the unsafe standalone ABI the 2026-06-17 design review described -- the field layout it would have frozen is now defined and pinned by a producer, and the anti-replay TPM-NV monotonic primitive it also named landed as `01-boot-platform/TODO-13` §17. What remains gating is the writer and the bootloader crypto. §5 is still dead plumbing standalone. Sections stay `[/]` with the shared Deferred stamp until the writer + bootloader trust primitives land.

## Inputs

- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c)
- [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h)
- -> XREF: `../02-kernel-core/TODO-26-power-management.md §4,§28,§11` -- image format and codec (shipped), image writer and resume consumer, fast startup
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

- [x] Define header with magic, version, kernel build id, boot_info ABI version, root volume id, image size, checksum, flags.
  - Shipped by `02-kernel-core/TODO-26` §4: `hiber_header_t` in `include/kernel/pm/hibernate.h` carries magic (0), format_version (8), image_bytes (16), boot_info_version (40), flags (44), root_volume_id (48), kernel_id (64), payload and header CRC-32C (132, 136), pinned by `_Static_assert`.
- [x] Include resume type: full hibernate, fast startup, crash-test image.
  - `resume_type` at offset 36 with `HIBER_RESUME_FULL`, `HIBER_RESUME_FAST_STARTUP` and `HIBER_RESUME_CRASH_TEST` (`include/kernel/pm/hibernate.h`).
- [/] Include required PCR/Secure Boot state when measured boot is active.
  - BLOCKED on a design choice the writer owns: `hiber_header_t` has no such field (bytes 152-4095 are reserved), and a TPM-sealed key whose policy binds the PCRs may make one unnecessary. Decide with the sealing policy. -> XREF: `02-kernel-core/TODO-26` §28 (item: "AEAD-encrypt the image")
- [x] Anti-replay `resume_generation`: a monotonic counter in the header written by the D02 T26 writer from a TPM-NV / NVRAM monotonic primitive; the bootloader rejects any image whose generation is not current/highest. -> XREF: T13 §7, T01 §13
  - Field only: `resume_generation` at offset 56. Populating it is `02-kernel-core/TODO-26` §28 (item: "Write the `resume_generation` anti-replay counter"); rejecting a stale value is §4 below.
- [x] Encryption metadata: AEAD cipher id + TPM-sealed key id + nonce/tag fields so the bootloader can require an encrypted image and refuse plaintext (debug-only opt-out); the writer owns encryption. -> XREF: D02T26 §28
  - Fields only: `aead_cipher_id` (96), `aead_key_id` (100), `aead_nonce` (104), `aead_tag` (116), with `HIBER_CIPHER_NONE` stating plaintext explicitly. Encrypting is `02-kernel-core/TODO-26` §28; requiring it is §4 below.
- [/] Store metadata in a location readable before normal root mount; this header is the single authoritative on-disk format (supersedes the legacy `HIBR_HEADER`). -> XREF: D02T26 §4
  - The format half shipped (next line). The location does not exist: the writer opens the partition by GPT type, and no `blkdev_open_by_gpt_type` exists. -> XREF: `02-kernel-core/TODO-26` §28 (item: "Open the hibernation partition by GPT type GUID")
  - DEFINED and shipped 2026-09-04 as `include/kernel/pm/hibernate.h` by D02T26 §4, which also pins the canonical byte stream this parser must agree with; the D02T26 §28 writer produces it.
- [/] Commit: `"boot: hibernation image metadata format"`
  - Waits on §2: a bootloader mirror of `hibernate.h` has no consumer until discovery exists. The kernel-side bad-magic and truncation cases already pass in `test_hiber_header_rejections`.

**Test checkpoint:** A fixture hibernation metadata header parses at known offsets (magic, version, kernel build id, boot_info ABI version, root volume id, image size, checksum, flags, resume type); a bad-magic or truncated header is rejected. Verify on QEMU WHPX/TCG (fixture parse).

> **Test runner:** N/A (deferred -- no code shipped) | validation: deferred until the writer + trust primitives land

> **Deferred:** [H] gated on the kernel hibernation WRITER (no image to resume) + bootloader AEAD/TPM-unseal (Codex design 2026-06-17: §5 dead plumbing) -> XREF: D02T26 §28 (image writer; the on-disk FORMAT shipped as D02T26 §4 on 2026-09-04), 09-desktop-shell/TODO-07 §1 (item: "Bootloader AEAD subset"). The TPM-NV primitives once listed here have shipped (01-boot-platform/TODO-13 §17, TODO-01 §13).

---

## 2. Bootloader Image Discovery

- [/] Locate hibernation metadata on system, A/B slot, or BlackBox partition.
  - BLOCKED: nothing writes an image and the partition is unopened. -> XREF: `02-kernel-core/TODO-26` §28 (item: "`pm_hibernate_write()`")
- [/] Avoid scanning arbitrary large files before watchdog is armed.
  - A constraint on the discovery code above, which does not exist. Waits on the same item.
- [/] Expose discovery failures on VPD and serial.
  - No discovery exists to fail. Waits on the first §2 item.
- [/] Add boot.conf override `resume=off|auto|force`; `force` only REQUESTS resume and stays subject to §3 eligibility + §4 integrity (never an unconditional bypass).
  - No `resume=` key exists in `src/boot/uefi/`. Dead plumbing until the first §2 item lands, because it would select a path nothing implements.
- [/] Commit: `"boot: discover hibernation resume images"`
  - Waits on the §2 items above.

**Test checkpoint:** With a hibernation image on the system / A/B slot / BlackBox partition, the bootloader locates the metadata and logs the source on serial + VPD; with no image it logs `no resume image` and cold-boots; `resume=off` skips discovery, `resume=force` requests resume but §3 eligibility + §4 integrity still gate it. Verify on QEMU WHPX.

> **Test runner:** N/A (deferred -- no code shipped) | validation: deferred until the writer + trust primitives land

> **Deferred:** [H] gated on the kernel hibernation WRITER (no image to resume) + bootloader AEAD/TPM-unseal (Codex design 2026-06-17: §5 dead plumbing) -> XREF: D02T26 §28 (image writer; the on-disk FORMAT shipped as D02T26 §4 on 2026-09-04), 09-desktop-shell/TODO-07 §1 (item: "Bootloader AEAD subset"). The TPM-NV primitives once listed here have shipped (01-boot-platform/TODO-13 §17, TODO-01 §13).

---

## 3. Resume Eligibility Policy

- [/] Reject resume after kernel update, bootloader ABI mismatch, slot switch, Secure Boot db change, firmware change, or hardware topology change.
  - Waits on §2 and a bootloader mirror of the §1 header: the fields to compare exist, but the bootloader reads no image. Firmware and hardware-topology fingerprints are not in the header either, so this item also decides where they live.
- [/] Allow manual one-time force only from a recovery/diagnostic-menu-issued one-shot NVRAM token (or BootNext provenance); a plain boot.conf `resume=force` cannot override a hard rejection, and force still requires §4 integrity.
  - Waits on §2 and the §4 integrity check that a force must still pass.
- [/] Integrate with BootNext and boot entry policy.
  - Waits on the entry-kind handler: `kind: resume` is parsed but only stub-validated. -> XREF: `01-boot-platform/TODO-07` §13
- [/] Record selected cold/resume reason.
  - The codes exist (`BOOT_REASON_RESUME_VALIDATED` and `_INVALIDATED`, `include/kernel/boot_info.h:914`), but `src/boot/uefi/bootx64.c:18925` still reads "not populated yet", so nothing selects them. Waits on the eligibility decision above.
- [/] Commit: `"boot: hibernation resume policy"`
  - Waits on the §3 items above.

**Test checkpoint:** After a simulated kernel update / bootloader ABI mismatch / slot switch / Secure Boot db change / firmware or hardware-topology change, resume is REJECTED with the recorded cold/resume reason and cold boot proceeds; a recovery-menu one-shot-token force overrides exactly once (a plain boot.conf `resume=force` does NOT).  Verify on QEMU WHPX.

> **Test runner:** N/A (deferred -- no code shipped) | validation: deferred until the writer + trust primitives land

> **Deferred:** [H] gated on the kernel hibernation WRITER (no image to resume) + bootloader AEAD/TPM-unseal (Codex design 2026-06-17: §5 dead plumbing) -> XREF: D02T26 §28 (image writer; the on-disk FORMAT shipped as D02T26 §4 on 2026-09-04), 09-desktop-shell/TODO-07 §1 (item: "Bootloader AEAD subset"). The TPM-NV primitives once listed here have shipped (01-boot-platform/TODO-13 §17, TODO-01 §13).

---

## 4. Integrity and Version Validation

- [/] Verify checksum/HMAC/signature over metadata and image.
  - The kernel codec checks both CRC-32Cs (`src/kernel/pm/hibernate_image.c`); those detect corruption and do not authenticate, and the bootloader has no parser or HMAC. -> XREF: `09-desktop-shell/TODO-07` §1 (item: "Bootloader AEAD subset")
- [/] Validate image physical-memory ranges against current memory map.
  - Waits on §2 and a bootloader chunk walker. The inputs exist: strictly ascending `start_pfn` descriptors and `total_ram_pages`.
- [/] Validate compressed image algorithm support.
  - Nothing under `src/boot` or `include/boot` references LZ4 today. Waits on the bootloader parser, and on deciding whether the bootloader decodes at all or only validates the header and hands off. -> XREF: `02-kernel-core/TODO-26` §28
- [/] Bind to measured boot state where TPM is available.
  - Waits on the §1 PCR decision. -> XREF: `02-kernel-core/TODO-26` §28 (item: "AEAD-encrypt the image")
- [/] Reject a valid-but-STALE image: compare the header `resume_generation` against the current TPM-NV/NVRAM monotonic value; a lower/non-current generation is rejected even when checksum/HMAC/state all match. -> XREF: T13 §7, T01 §13
  - No longer waits on the primitive: `01-boot-platform/TODO-13` §17 shipped the monotonic NV index. Waits on an image that carries a real generation, and on a bootloader NV read (`efi.h` declares TCG2 SubmitCommand; no boot code issues NV commands). -> XREF: `02-kernel-core/TODO-26` §28 (item: "Write the `resume_generation` anti-replay counter")
- [/] Require an encrypted image: validate the AEAD metadata (cipher/key-id/nonce) and decrypt-verify with the TPM-sealed key; refuse a plaintext image outside an explicit debug path. -> XREF: D02T26 §28
  - No AEAD and no TPM2 unseal exist under `src/boot`. -> XREF: `09-desktop-shell/TODO-07` §1 (item: "Bootloader AEAD subset")
- [/] Commit: `"boot: validate hibernation image integrity"`
  - Waits on the §4 items above.

**Test checkpoint:** A correct checksum/HMAC over metadata + image passes; a single flipped byte is rejected; image physical-memory ranges outside the current memory map are rejected; an unsupported compression algorithm is rejected; TPM-bound state mismatch rejects when measured boot is active. Verify on QEMU WHPX/TCG (fixture).

> **Test runner:** N/A (deferred -- no code shipped) | validation: deferred until the writer + trust primitives land

> **Deferred:** [H] gated on the kernel hibernation WRITER (no image to resume) + bootloader AEAD/TPM-unseal (Codex design 2026-06-17: §5 dead plumbing) -> XREF: D02T26 §28 (image writer; the on-disk FORMAT shipped as D02T26 §4 on 2026-09-04), 09-desktop-shell/TODO-07 §1 (item: "Bootloader AEAD subset"). The TPM-NV primitives once listed here have shipped (01-boot-platform/TODO-13 §17, TODO-01 §13).

---

## 5. boot_info Resume Handoff

- [/] Add typed payload descriptor for hibernation metadata/image, `BOOT_PAYLOAD_HIBERNATION_META` with `BOOT_PAYLOAD_FLAG_REQUIRED`.
  - Use `BOOT_PAYLOAD_HIBERNATION_META` from [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h); set `BOOT_PAYLOAD_FLAG_REQUIRED` on the descriptor so a stale kernel that dropped the type enum fails safe instead of booting past the resume handoff. TODO-01 §4's validator rejects overlap with any retained region before Phase 0 destructive init runs. -> XREF: [`01-boot-platform/TODO-01 §4`](TODO-01-boot-protocol-abi-handoff.md#4-optional-payload-descriptor-array)
  - The type and validator case exist (`src/kernel/main/boot_payload.c:171`), but the limits row reads NOT RESERVABLE because no producer emits it (`include/boot/boot_payload_limits.h:89`). Waits on §2-§4, which set the bound and produce it.
- [x] Add resume flags and shared decision-reason codes to boot_info through TODO-01 §12.
  - Shipped by `01-boot-platform/TODO-01` §12: `BOOT_PATH_RESUME` and `BOOT_PATH_FAST_STARTUP` (`include/kernel/boot_info.h:881`), `BOOT_REASON_RESUME_*` (:914), `BOOT_SOURCE_FLAG_RESUME_INVALIDATED` (:1011), `RESUME_METADATA` capability (:1134), mirrored in `src/boot/uefi/boot_info_mirror.h`.
- [/] Kernel consumes handoff before normal Phase 0 destructive init.
  - -> XREF: `02-kernel-core/TODO-26` §28 (item: "`pm_hibernate_resume()`")
- [/] PMM reserves image ranges until resume code consumes them.
  - Waits on the descriptor above: the reservation pass only pins types whose limits row has a real bound, and this one is 0.
- [/] Commit: `"boot: hand off hibernation resume payload"`
  - Waits on the §5 items above.

**Test checkpoint:** On an eligible resume, boot_info carries the `BOOT_PAYLOAD_HIBERNATION_META` descriptor (`BOOT_PAYLOAD_FLAG_REQUIRED` set, overlap-checked by the TODO-01 §4 validator) plus resume flags + decision-reason codes; the kernel consumes the handoff before Phase 0 destructive init; PMM reserves the image ranges until resume code consumes them. Verify on QEMU WHPX.

> **Test runner:** N/A (deferred -- no code shipped) | validation: deferred until the writer + trust primitives land

> **Deferred:** [H] gated on the kernel hibernation WRITER (no image to resume) + bootloader AEAD/TPM-unseal (Codex design 2026-06-17: §5 dead plumbing) -> XREF: D02T26 §28 (image writer; the on-disk FORMAT shipped as D02T26 §4 on 2026-09-04), 09-desktop-shell/TODO-07 §1 (item: "Bootloader AEAD subset"). The TPM-NV primitives once listed here have shipped (01-boot-platform/TODO-13 §17, TODO-01 §13).

---

## 6. Resume Failure Fallback

- [/] If resume fails, mark image invalid and cold boot once.
  - No resume path exists to fail. -> XREF: `02-kernel-core/TODO-26` §28 (item: "`pm_hibernate_resume()`")
- [/] Increment failure counter and feed A/B rollback policy if repeated.
  - Waits on the item above. -> XREF: `01-boot-platform/TODO-21` §4 (item: "Accept repeated resume failures as a rollback input")
- [/] Preserve failure details in NVRAM and BlackBox.
  - Waits on the first §6 item. The channel exists (`nvram_write_boot_error`), but no resume failure feeds it.
- [/] Avoid infinite resume loops.
  - Waits on the first §6 item: invalidating the image is what breaks the loop.
- [/] Commit: `"boot: hibernation resume failure fallback"`
  - Waits on the §6 items above.

**Test checkpoint:** A forced resume failure marks the image invalid, cold-boots exactly once, increments the NVRAM failure counter, and after repeated failures feeds the A/B rollback policy; failure details are preserved in NVRAM + BlackBox; no infinite resume loop occurs. Verify on QEMU WHPX.

> **Test runner:** N/A (deferred -- no code shipped) | validation: deferred until the writer + trust primitives land

> **Deferred:** [H] gated on the kernel hibernation WRITER (no image to resume) + bootloader AEAD/TPM-unseal (Codex design 2026-06-17: §5 dead plumbing) -> XREF: D02T26 §28 (image writer; the on-disk FORMAT shipped as D02T26 §4 on 2026-09-04), 09-desktop-shell/TODO-07 §1 (item: "Bootloader AEAD subset"). The TPM-NV primitives once listed here have shipped (01-boot-platform/TODO-13 §17, TODO-01 §13).

---

## 7. Fast Startup Mode

- [/] Distinguish kernel-session resume from full user-session hibernate.
  - `HIBER_RESUME_FAST_STARTUP` and `BOOT_PATH_FAST_STARTUP` exist, but nothing produces such an image. -> XREF: `02-kernel-core/TODO-26` §11
- [/] Allow driver/hardware invalidation list.
  - Waits on the §3 eligibility policy, which it extends, and on `02-kernel-core/TODO-26` §11 naming what may invalidate an image. No list exists today.
- [/] Integrate with power button and shutdown policy.
  - Button dispatch shipped in `02-kernel-core/TODO-26` §7. Fast shutdown does not exist. -> XREF: `02-kernel-core/TODO-26` §11 (item: "`pm_fast_shutdown()`")
- [/] Expose fast-startup status in boot diagnostics.
  - Waits on the first §7 item and §8.
- [/] Commit: `"boot: fast startup handoff"`
  - Waits on the §7 items above.

**Test checkpoint:** A fast-startup (kernel-session) image resumes the kernel session distinctly from a full user-session hibernate; a driver/hardware on the invalidation list forces cold boot; the power button + shutdown policy select fast startup; fast-startup status appears in boot diagnostics. Verify on QEMU WHPX.

> **Test runner:** N/A (deferred -- no code shipped) | validation: deferred until the writer + trust primitives land

> **Deferred:** [H] gated on the kernel hibernation WRITER (no image to resume) + bootloader AEAD/TPM-unseal (Codex design 2026-06-17: §5 dead plumbing) -> XREF: D02T26 §28 (image writer; the on-disk FORMAT shipped as D02T26 §4 on 2026-09-04), 09-desktop-shell/TODO-07 §1 (item: "Bootloader AEAD subset"). The TPM-NV primitives once listed here have shipped (01-boot-platform/TODO-13 §17, TODO-01 §13).

---

## 8. Diagnostics and BlackBox Resume Report

- [/] Write `X:\Diag\resume.json`.
  - No resume decision exists to report. Waits on §3.
- [/] Include image version, validation status, failure reason, selected path, and duration.
  - Waits on the item above and §4, which produces the status.
- [/] Show resume progress in VPD.
  - Waits on §5: no resume path exists to show progress for.
- [/] Add QR failure payload for resume rejection.
  - Waits on §3, which is what rejects.
- [/] Commit: `"boot: hibernation resume diagnostics"`
  - Waits on the §8 items above.

**Test checkpoint:** Post-resume (or post-rejection), `X:\Diag\resume.json` exists with image version, validation status, failure reason, selected path, and duration; VPD shows resume progress; a resume rejection emits a host-decodable QR failure payload. Verify on QEMU WHPX/TCG.

> **Test runner:** N/A (deferred -- no code shipped) | validation: deferred until the writer + trust primitives land

> **Deferred:** [H] gated on the kernel hibernation WRITER (no image to resume) + bootloader AEAD/TPM-unseal (Codex design 2026-06-17: §5 dead plumbing) -> XREF: D02T26 §28 (image writer; the on-disk FORMAT shipped as D02T26 §4 on 2026-09-04), 09-desktop-shell/TODO-07 §1 (item: "Bootloader AEAD subset"). The TPM-NV primitives once listed here have shipped (01-boot-platform/TODO-13 §17, TODO-01 §13).

---

## 9. Resume Tests

- [/] Fixture valid metadata accepted.
  - Codec round trips pass (`test_hiber_roundtrip_compressible`); the boot-path case waits on the §2 and §3 parser and policy.
- [/] Bad checksum rejected.
  - Codec case passes (`test_hiber_payload_corruption`); the boot-path case waits on §4.
- [/] Kernel version mismatch rejected.
  - Codec case passes (`test_hiber_identity_guard`); the boot-path case waits on §3.
- [/] Resume failure falls back to cold boot.
  - Waits on §6.
- [/] Stale image (lower `resume_generation`) rejected.
  - Waits on the §4 stale-image item.
- [/] Plaintext / bad-AEAD-tag image rejected.
  - Waits on the §4 encrypted-image item.
- [/] Plain boot.conf `resume=force` cannot override a hard rejection (kernel-update / ABI / slot / Secure Boot db).
  - Waits on §2 (the key) and §3 (the rejection).
- [/] Commit: `"test: hibernation boot handoff"`
  - Waits on the §9 items above.

**Test checkpoint:** The resume test suite passes: valid metadata accepted, bad checksum rejected, kernel-version mismatch rejected, and a resume failure falls back to cold boot. Verify via the `TEST_CAT_BOOT` suite.

> **Test runner:** N/A (deferred -- no code shipped) | validation: deferred until the writer + trust primitives land

> **Deferred:** [H] gated on the kernel hibernation WRITER (no image to resume) + bootloader AEAD/TPM-unseal (Codex design 2026-06-17: §5 dead plumbing) -> XREF: D02T26 §28 (image writer; the on-disk FORMAT shipped as D02T26 §4 on 2026-09-04), 09-desktop-shell/TODO-07 §1 (item: "Bootloader AEAD subset"). The TPM-NV primitives once listed here have shipped (01-boot-platform/TODO-13 §17, TODO-01 §13).

---

## OS Comparison

| ⭐  | Feature                  | 🪟 Win11                   | 🐧 Linux                  | 🚀 Impossible OS              |
| --- | ------------------------ | -------------------------- | ------------------------- | ----------------------------- |
| 💎  | S4 resume selection      | ✅ hiberfil.sys + winload  | ✅ `resume=` kernel param | ⬜ Planned §1-§6              |
| 💎  | Fast startup             | ✅ hybrid boot (hiberboot) | ⚠️ limited distro support | ⬜ Planned §7                 |
| 💎  | Resume invalidation      | ✅ update/driver policy    | ⚠️ initramfs logic        | ⬜ Planned §3                 |
| ⭐  | BlackBox resume report   | ❌ event logs only         | ❌ journal only           | ⭐ Planned §8 (X:\Diag + QR)  |
| ⭐  | Resume image anti-replay | ❌ none                    | ❌ none                   | ⭐ Planned §4 (TPM-NV gen)    |
| ⭐  | Resume image encryption  | ⚠️ only via BitLocker      | ⚠️ needs encrypted swap   | ⭐ Planned §4 + D02T26 (AEAD) |

---

## Unit Tests

> **Gated:** all cases need an actual hibernation image (the 02-kernel-core/TODO-26 §28 writer, unimplemented; §4 shipped the format these fixtures will be built in) + the bootloader crypto/TPM-seal/anti-replay primitives; they ship with §1-§9, deferred whole-file. See the §1-§9 Deferred stamps.

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
> **Gated:** the items below need the 02-kernel-core/TODO-26 §28 hibernation writer to produce an image and the bootloader crypto/TPM-seal primitives; they open as §1-§9 ship.

- [ ] QEMU hibernate image fixture
- [ ] Failed resume cold-boot fallback
- [ ] A/B slot switch invalidates resume
- [ ] Bare-metal S4 once power TODO is ready
