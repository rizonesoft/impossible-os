---
schema_version: 1
id: ab-boot-rollback
domain: 01-boot-platform
status: active
title: "TODO-21 -- A/B Dual-Slot Boot & Automatic Rollback"
---

# TODO-21 -- A/B Dual-Slot Boot & Automatic Rollback

> **Goal:** The system is never unbootable. Implement A/B dual-slot boot partitioning with automatic rollback on failed updates. If a kernel update breaks boot, the system automatically reverts to the previous working version on the next reboot -- no user intervention, no recovery USB, no expertise needed. This is the pattern used by Android, Chrome OS, and modern embedded systems. Windows achieves similar via Automatic Repair; Linux via systemd-boot auto-assessment.

> [!IMPORTANT]
> **Current state:** Single root partition, no rollback. A corrupted kernel update bricks the system. A/B slot selection is **not** in TODO-02 (UEFI §8 is serial klog). This TODO implements the full pipeline: partition layout, boot metadata, failure counting, automatic rollback, and update engine integration.

---

## Inputs

- `src/boot/uefi/bootx64.c` -- bootloader (needs slot selection logic)
- `scripts/build.sh` -- disk image creation (needs dual-slot layout)
- `include/kernel/boot_info.h` -- boot_info (needs slot metadata)
- → XREF: `TODO-02-uefi-hardening-secureboot.md §1` -- UEFI runtime handoff / shared bootloader file; A/B slot policy is owned only here
- → XREF: `TODO-03-bootloader-error-recovery.md §2` -- boot failure screen integration
- → XREF: `10-platform-services/TODO-03-updates-packages.md` -- update engine (downstream consumer)
- → XREF: `../02-kernel-core/TODO-02-kernel-configuration-policy.md §4, §5, §10` -- LastKnownGood, Safe Mode recovery, and boot-status acceptance feed rollback decisions
- → XREF: `TODO-07-boot-entry-store-menu-policy.md §9` -- A/B slot state from §1 + §3 + §4 here feeds the boot menu's slot/recovery entry generation + counter merge + auto-select-recovery (§9 deferred until those §§ land)
- → XREF: `TODO-13-tpm-measured-boot-attestation.md §7` -- `tpm_nv_*` write-lock/monotonic-counter primitive backs the §8 anti-rollback floor
- → XREF: `../02-kernel-core/TODO-19-code-integrity-trust-policy.md` + `TODO-02-uefi-hardening-secureboot.md §1` -- verified slot identity gates §5 mark-good + §8 floor advance

---

## Outcome

- Disk layout has two root partitions: Slot A and Slot B.
- Boot metadata tracks: active slot, try count, successful flag per slot.
- Bootloader reads metadata, selects active slot, boots from it.
- If boot fails (kernel panic before `mark_boot_successful()`), try count increments.
- After 3 failed attempts, bootloader automatically rolls back to the other slot.
- Update engine writes to inactive slot, marks it as `pending`, reboots.
- Boot metadata is CRC-protected with redundant copies (no torn-write brick); anti-rollback blocks downgrade to a vulnerable slot.
- User never sees a brick -- worst case is "previous version boots".

---

## Implementation Order

| ⭐  | Order | Deliverable                                    | Depends On | Status |
| --- | :---: | ---------------------------------------------- | ---------- | :----: |
| 💎  |   1   | Boot metadata structure (UEFI NVRAM or GPT)     | --          |  [ ]   |
| 💎  |   2   | Dual-slot disk layout in build system           | §1         |  [ ]   |
| 💎  |   3   | Bootloader slot selection logic                 | §1, §2     |  [ ]   |
| 💎  |   4   | Boot failure counting and rollback              | §3         |  [ ]   |
| 💎  |   5   | Kernel `mark_boot_successful()` syscall         | §4         |  [ ]   |
| ⭐  |   6   | Slot status in boot diagnostics                 | §1-§5      |  [ ]   |
| 💎  |   7   | Boot metadata integrity + atomic writes         | §1         |  [ ]   |
| 💎  |   8   | Per-slot anti-rollback version floor            | §1, §7     |  [ ]   |

> 💎 = parity -- Android/Chrome OS A/B and systemd-boot auto-assessment both provide this.
> ⭐ = exclusive -- slot status integrated into VPD boot diagnostics.

---

## 1. Boot Metadata Structure

Define where boot slot metadata is stored and the v1 wire format. Design review (Codex, 2H+2M adopted) resolved storage to GPT/disk-primary with a full ABI-stable v1 struct.

> [!NOTE]
> **Existing infra (do not duplicate):** `src/kernel/main/boot_rollback.c` already implements the security-version anti-rollback FLOOR -- `IPOSRequiredSecVersion` NVRAM var + `boot_rollback_mark_steady()`/`raise_if_steady()` (advance the floor only after the boot reaches steady state) + `boot_info.{os_loader_security_version, required_security_version}`. §8 INTEGRATES with this floor; §5 builds on `mark_steady`. The genuinely NEW work is the per-SLOT A/B metadata (tries/successful/priority per slot), which `boot_rollback.c` does not cover.

- [ ] **Storage = GPT/disk primary, NVRAM hint-only** (design review): two fixed redundant on-disk blocks (per §7); EFI vars rejected for high-frequency `tries` per repo BootSticky doctrine; §8 floor stays in existing `boot_rollback.c` NVRAM
- [ ] Define the FULL v1 wire struct now in shared `include/boot/ab_boot_metadata.h` -- all §1/§7/§8 fields (incl crc32, generation, rollback_index/floor) present + `_Static_assert` size pins, §7/§8 fields zero-init until wired
- [ ] `tries`: incremented by bootloader before each boot attempt (0-3)
- [ ] `successful`: set by kernel after the acceptance stage (via `mark_boot_successful()`, building on existing `boot_rollback_mark_steady()`)
- [ ] `priority`: which slot is preferred when both are valid (higher = preferred)
- [ ] `boot_meta_read()`/`boot_meta_write()` as thin wrappers over the shared header's validator + default-builder + CRC; pre-EBS disk adapter `src/boot/uefi/ab_boot.c` + kernel adapter for §5
- [ ] Commit: `"boot: A/B boot metadata structure and NVRAM storage"`

**Test checkpoint:** Bootloader reads/writes metadata. Serial shows `"Slot A: tries=0 successful=1 priority=1"`. Test on: QEMU smoke + bare metal -- UEFI NVRAM (SetVariable/GetVariable) persistence differs between QEMU OVMF and real firmware; a green QEMU result can mask a bare-metal NVRAM failure.

---

## 2. Dual-Slot Disk Layout

Modify the build system to create disk images with two root partitions.

> [!NOTE]
> **Scope:** the shared `BOOTX64.EFI` on the ESP is a single point of brick -- a bad bootloader update bricks before slot selection runs. TODO-21 covers root/kernel-slot rollback only; signed bootloader/ESP self-update safety is recovery-owned -> XREF: [`TODO-22`](TODO-22-recovery-partition.md) + [`TODO-02 §1`](TODO-02-uefi-hardening-secureboot.md).

- [ ] GPT layout: EFI System Partition (64 MiB) + Slot A IXFS (223 MiB) + Slot B IXFS (223 MiB)
- [ ] Both slots contain identical initial OS image
- [ ] Kernel is stored per-slot: `\boot\kernel.exe` in each slot's filesystem
- [ ] Bootloader remains on EFI System Partition (shared, not duplicated)
- [ ] `scripts/build.sh` updated to create dual-slot images
- [ ] Commit: `"build: dual-slot disk layout -- EFI + Slot A + Slot B"`

**Test checkpoint:** Built disk image has 3 partitions visible in `fdisk -l`.

---

## 3. Bootloader Slot Selection

Bootloader reads metadata and mounts the correct slot's filesystem.

- [ ] At boot: read boot metadata → determine active slot
- [ ] Select the highest-`priority` slot that is valid (passes §7 CRC + the §8 floor) and not `unbootable`; tie-break by `successful` then lower `tries`
- [ ] Slot state = `successful` / `pending` (update written, unproven) / `unbootable` (tries exhausted); roll back when the active slot exhausts `tries` EVEN IF it was previously `successful`, as long as another viable slot exists
- [ ] Increment `tries` for active slot before booting
- [ ] Mount the active slot's partition (not EFI partition) for kernel loading
- [ ] Pass `boot_info.active_slot = 'A'/'B'` to kernel
- [ ] Log: `"[BOOT] Booting Slot %c (tries=%u, successful=%u)"` with slot info
- [ ] Honor SPLIT `payload.root` in `load_kernel()` so the seeded slot-b entry resolves to its partition -> XREF: [`TODO-07 §16`](TODO-07-boot-entry-store-menu-policy.md#16-bootstrap-and-first-install-entry-seeding).
- [ ] Flip seeded `slot-b` from inactive to active in `bootcfg.py` `_seed_store()` once root-aware lookup ships -> XREF: [`TODO-07 §16`](TODO-07-boot-entry-store-menu-policy.md#16-bootstrap-and-first-install-entry-seeding).
- [ ] Commit: `"boot: slot selection -- boot from active slot, rollback on failure"`

**Test checkpoint:** Normal boot shows `"Booting Slot A (tries=0, successful=1)"`. Manual metadata corruption triggers rollback to Slot B.

---

## 4. Boot Failure Counting and Rollback

Automatic rollback after 3 consecutive boot failures.

- [ ] Bootloader increments `tries` BEFORE attempting boot (so crash = tries counted)
- [ ] If boot succeeds (kernel calls `mark_boot_successful()`), `tries` resets to 0
- [ ] If 3 boots without `mark_boot_successful()`: switch active slot on next boot
- [ ] On rollback: `"[BOOT] Slot %c failed 3 times -- rolling back to Slot %c"` on serial
- [ ] On rollback: render brief notification on boot splash: `"Update failed -- reverting to previous version"`
- [ ] If BOTH slots have `tries >= 3`: enter recovery mode (→ XREF: TODO-22)
- [ ] Commit: `"boot: automatic rollback after 3 failed boot attempts"`

**Test checkpoint:** Intentionally corrupt Slot A kernel. Boot 3 times → 4th boot automatically switches to Slot B.

**Regression risk:** MEDIUM -- if try counter logic has off-by-one, healthy boots get rolled back. Test thoroughly.

---

## 5. Kernel `mark_boot_successful()`

Kernel-side API to tell the bootloader "this boot worked".

- [ ] Add syscall or kernel function: `mark_boot_successful()` -- writes `successful=1, tries=0` to boot metadata only after TODO-02 §10 marks the boot accepted
- [ ] Called from `boot_phase3()` after the configured acceptance stage is satisfied, not a hard-coded "desktop visible" heuristic
- [ ] Before marking good, require the slot kernel to have reported a verified identity (signature / code-integrity pass); refuse mark-good on an unverified slot -> XREF: `02-kernel-core/TODO-19` + `TODO-02 §1`
- [ ] On mark-good, advance the §8 `rollback_floor` to the active slot's `rollback_index` (never lower it)
- [ ] On success: `"[BOOT] Boot marked successful (Slot %c)"`
- [ ] If metadata write fails: `"[WARN] Cannot mark boot successful -- rollback may trigger on next reboot"`
- [ ] For NVRAM storage: use UEFI `SetVariable()` via runtime services
- [ ] Commit: `"kernel: mark_boot_successful -- reset try counter after successful boot"`

**Test checkpoint:** Normal boot → serial shows `"Boot marked successful (Slot A)"` after the configured acceptance stage is reached. Test on: QEMU smoke + bare metal -- `SetVariable()` at runtime via UEFI runtime services must persist across reboot on real firmware, not just OVMF.

---

## 6. Slot Status in Boot Diagnostics

Integrate A/B slot status into the VPD and boot timing display.

- [ ] VPD shows: `"Slot A [active] tries=0 ok"` / `"Slot B [standby] tries=0 ok"`
- [ ] If rollback occurred: VPD shows `"⚠ Rolled back from Slot X (3 failures)"`
- [ ] boot_timing log includes slot selection time
- [ ] Commit: `"boot: A/B slot status in VPD diagnostics"`

**Test checkpoint:** Boot with `postbars=on` -- slot status visible in VPD display.

---

## 7. Boot Metadata Integrity and Atomic Writes

Boot metadata is the single source of truth for slot selection; a torn write must never brick the box. Follow the Android libavb_ab pattern: magic + CRC-32 + redundant copies with a monotonic generation.

- [ ] Add a `crc32` field over the metadata record + a monotonic `generation` counter; `boot_meta_read()` rejects any record with bad magic or CRC
- [ ] Store TWO redundant copies (primary + backup); `boot_meta_read()` returns the valid copy with the highest `generation`
- [ ] `boot_meta_write()` is power-fail-atomic: update the older (lower-generation) copy first, flush, then leave the other intact -- a crash always leaves one valid copy
- [ ] If BOTH copies fail CRC: load compiled-in factory defaults (active=A, tries=0, successful=0) and log `"[BOOT] metadata corrupt -- factory defaults"`
- [ ] NVRAM storage: two UEFI variables (`BootMetaA`/`BootMetaB`); GPT storage: two metadata blocks at fixed LBAs
- [ ] Validate the `version` field; on an unknown future version fail safe to factory defaults rather than misparsing
- [ ] Commit: `"boot: A/B metadata integrity -- CRC-32, generation counter, redundant copies"`

**Test checkpoint:** Flip a byte in the primary copy -- bootloader reads the backup, logs the CRC recovery, boots normally. Corrupt both -- factory defaults load. Test on: QEMU smoke + bare metal (NVRAM torn-write behavior differs).

---

## 8. Per-Slot Anti-Rollback Version Floor

Prevent a security update from being rolled back to an older, vulnerable slot. Follow the Android `stored_rollback_index` model: a monotonic version floor that advances only after a slot is verified and marked good.

> [!NOTE]
> **Reuses `boot_rollback.c`:** the monotonic floor + advance-after-steady-state mechanism already exists (`IPOSRequiredSecVersion` NVRAM, `boot_rollback_mark_steady()`/`raise_if_steady()`, `boot_rollback_validate()`). §8's NEW work is the PER-SLOT `rollback_index` in the §1 metadata + the §3 below-floor rejection; the floor STORAGE + advance logic is the existing mechanism, not a parallel one.

- [ ] Add a `rollback_index` (monotonic OS/security version) per slot to the metadata + a single stored `rollback_floor`
- [ ] §3 selection rejects any slot whose `rollback_index < rollback_floor` even if otherwise bootable (treat as invalid, try the other slot)
- [ ] Advance `rollback_floor` to the active slot's `rollback_index` ONLY after `mark_boot_successful()` AND a verified slot identity -> XREF: `02-kernel-core/TODO-19` + `TODO-02 §1`
- [ ] Store `rollback_floor` in TPM NV (write-lock / monotonic counter) where available, else authenticated metadata -> XREF: [`TODO-13 §7`](TODO-13-tpm-measured-boot-attestation.md)
- [ ] On rollback to an older slot NEVER lower `rollback_floor`; a slot below the floor is unbootable and routes to recovery -> XREF: [`TODO-22`](TODO-22-recovery-partition.md)
- [ ] Commit: `"boot: A/B anti-rollback -- per-slot version floor, monotonic, TPM-NV backed"`

**Test checkpoint:** `rollback_floor=5`, slot B `rollback_index=4`: selection skips B. Mark slot A good at index 6: floor advances to 6. Test on: QEMU smoke + bare metal.

---

## OS Comparison

| ⭐ | Feature                   | 🪟 Win11                      | 🐧 Linux                    | 🚀 Impossible OS             |
|----|---------------------------|----------------------------|--------------------------|---------------------------|
| 💎 | Dual-slot boot            | ⚠️ Automatic Repair only  | ⚠️ systemd-boot assess   | ⬜ §1-§3                  |
| 💎 | Boot failure counting     | ✅ 2-attempt detection     | ✅ systemd tries counter | ⬜ §4                     |
| 💎 | Automatic rollback        | ⚠️ Manual repair needed   | ⚠️ Manual or auto        | ⬜ §4                     |
| 💎 | Mark boot successful      | ✅ Implicit (desktop OK)   | ✅ systemd boot-complete | ⬜ §5                     |
| 💎 | Metadata integrity (CRC)  | ⚠️ BCD, no A/B redundancy | ✅ Android CRC-32 dual    | ⬜ §7                     |
| 💎 | Anti-rollback floor       | ⚠️ WU rollback window      | ✅ Android stored index   | ⬜ §8                     |
| ⭐ | Slot status in boot UI    | ❌ Hidden                  | ❌ journalctl only       | ⬜ §6 🚀                  |

After §1-§5, Impossible OS has stronger rollback than Windows (which requires manual Automatic Repair) and matches Chrome OS/Android A/B. §6 makes slot status visible during boot.

---

## Unit Tests

> Boot metadata and slot selection run in UEFI bootloader context -- use smoke tests.
> Kernel-side `mark_boot_successful()` logic can be validated via kernel unit tests.

- [ ] Create `src/kernel/test/test_ab_boot.c` with:
  - Boot metadata struct round-trip: write `{active_slot=A, tries=0, successful=1}`, read back, fields match
  - Slot selection logic: `slot_a.tries=3, slot_a.successful=0` causes rollback to slot B
  - Slot selection logic: `slot_a.tries=2, slot_a.successful=0` still boots slot A (under threshold)
  - Slot selection logic: both slots `tries>=3` triggers recovery mode (returns error code)
  - Try counter increment: before boot, `tries` increments by 1
  - `mark_boot_successful()` resets `tries=0` and sets `successful=1` for active slot
  - `boot_info.active_slot` is valid (`'A'` or `'B'`)
  - Metadata CRC (§7): corrupt primary copy → read returns the valid backup; corrupt both → factory defaults
  - Metadata generation (§7): higher-`generation` copy wins on read
  - Anti-rollback (§8): a slot with `rollback_index < rollback_floor` is skipped by selection
  - Anti-rollback (§8): `mark_boot_successful()` advances `rollback_floor`, never lowers it
  - State machine (§3): a `successful==1` slot with `tries` exhausted still rolls back when the other slot is viable
- [ ] Register in `test_runner_init()`: `test_register_ab_boot()`
- [ ] Create `scripts/test-boot-rollback.sh`:
  - Build dual-slot disk image
  - Corrupt slot A kernel (truncate to 0 bytes)
  - Boot QEMU 4 times in sequence, capture serial each time
  - Assert 4th boot serial contains `"rolling back to Slot B"` (§4 -- automatic rollback)
- [ ] Commit: `"test: add A/B boot rollback test suite"`

---

## Verification

- [ ] **Normal boot**: Slot A boots, try counter resets, `mark_boot_successful` logged.
- [ ] **Rollback test**: corrupt Slot A kernel, boot 3 times → Slot B activates automatically.
- [ ] **Both slots bad**: corrupt both → enters recovery mode (or shows error screen).
- [ ] **Update simulation**: write new kernel to Slot B, mark pending, reboot → boots Slot B.
- [ ] Commit: `"boot: A/B dual-slot boot complete -- automatic rollback, never unbootable"`

**Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) for `test_ab_boot.c` + `scripts/test-boot-rollback.sh` (rollback smoke) | suites TBD until §1-§5 land
