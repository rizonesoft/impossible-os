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
| 💎  |   1   | Boot metadata structure (GPT/disk wire ABI)     | --          |  [/]   |
| 💎  |   2   | Dual-slot disk layout in build system           | §1         |  [/]   |
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

- [x] **Storage = GPT/disk primary, NVRAM hint-only** (design review): two fixed redundant on-disk blocks (per §7); EFI vars rejected for high-frequency `tries` per repo BootSticky doctrine; §8 floor stays in existing `boot_rollback.c` NVRAM
- [x] Define the FULL v1 wire struct now in shared `include/boot/ab_boot_metadata.h` -- all §1/§7/§8 fields (incl crc32, generation, rollback_index/floor) present + `_Static_assert` size pins, §7/§8 fields zero-init until wired
- [x] `tries` field defined (per-slot, 0..`AB_BOOT_MAX_TRIES`); the bootloader increment-before-boot behavior is §3/§4
- [x] `successful` field defined (per-slot); the kernel set-after-acceptance behavior is §5 (`mark_boot_successful()`, building on `boot_rollback_mark_steady()`)
- [x] `priority` field defined (per-slot, higher = preferred); the both-slots-valid selection is §3
- [/] Shared validator/default/CRC + newest-copy selector shipped in the header; `boot_meta_read()`/`boot_meta_write()` disk adapter (`ab_boot.c`) + kernel adapter land in §2/§5 (no disk blocks until §2's layout)
- [x] Commit: `"boot: A/B boot metadata v1 wire ABI -- shared header + CRC + validator + tests"`

**Test checkpoint:** `make test-boot` -- the 12 `ab_boot:` cases pass (default-valid, 7 reject paths incl. 2 per-slot domain checks, CRC-excludes-field, 3 newest-copy selections). The on-disk round-trip + the `"Slot A: tries=0 ..."` serial line land with §2's disk layout. Test on: TCG `make test-boot`; bare metal exercises the §2 disk path.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 12 `ab_boot` cases, 0 failures

> **Notes:**
> - **What shipped:** `include/boot/ab_boot_metadata.h` -- the A/B slot metadata v1 wire struct (60 bytes) + static-inline validate/default/finalize/compute_crc/select_newest; 12 unit tests in `test_ab_boot.c`.
> - **How it integrates:** shared static-inline so bootloader + kernel use one definition; CRC-32/IEEE-802.3 mirrors `boot_health_handoff.h`; full v1 layout pinned now (`_Static_assert`) so §7/§8 fields ship later without moving bytes.
> - **Downstream effects:** §2 adds the GPT/disk read/write adapter; §3 selection, §4 counting, §5 mark-good, §7 redundancy, §8 floor all consume this struct.
> - **Canonical doc:** `include/boot/ab_boot_metadata.h`.
> - **Scope boundary:** §1 owns the struct + pure logic; disk transport is §2, increment/mark/select behavior is §3/§4/§5, redundancy is §7, the anti-rollback floor reuses `boot_rollback.c` (§8).

> **Verified:** 2026-06-15 | commit `483f8982` | 5/6 items | build OK | tests 2725 PASS
> **Quality reviewed:** 2026-06-15 | Codex 6x (design, adversarial, consistency, perf, re-adversarial) | 2M+1L fixed, 0 open | scope: boot-code-quality

---

## 2. Dual-Slot Disk Layout

Modify the build system to create disk images with two root partitions.

> [!NOTE]
> **Scope (design review):** for §2 BOTH `BOOTX64.EFI` AND `kernel.exe` live on the shared ESP (`load_kernel` is FAT/ESP-only, cannot mount IXFS), so a bad ESP update bricks before slot selection. §2 ships per-slot ROOTS only; true per-slot KERNEL rollback needs a bootloader IXFS reader (deferred). Signed bootloader/ESP self-update safety is recovery-owned -> XREF: [`TODO-22`](TODO-22-recovery-partition.md) + [`TODO-02 §1`](TODO-02-uefi-hardening-secureboot.md).

- [x] `make-system-disk --ab` builds the 5-partition layout (ESP + BlackBox + A/B-metadata + Slot A IXFS + Slot B IXFS) with a post-layout fail-closed invariant (all extents ordered/non-overlapping, both slots >= 96 MiB); verified on 768M
- [ ] Wire `make-system-disk`'s `--ab` mode into the Makefile + consume the `.info` offsets (drop the fixed LBA constants); run `mkfs-ixfs` twice -- Slot A = current root, Slot B identical (grow `SYSTEM_DISK_SIZE` to 768M)
- [/] Dedicated A/B-metadata GPT partition (type GUID `...4D44...`) holding the two §1 blocks shipped + `META_OFFSET`/`IXFS_B_OFFSET` exported in `.info`; the bootloader pre-EBS `EFI_BLOCK_IO` read is pending (atomic write is §7)
- [x] Kernel stays on the SHARED ESP for §2 -- verified: `load_kernel` (`bootx64.c`) is FAT/ESP-only, cannot mount IXFS; slots are per-slot IXFS roots only
- [x] Bootloader loads Slot A until §3 -- verified: `partition_scan_all` mounts the FIRST IXFS-GUID partition (Slot A) at C: and skips Slot B + the metadata partition; no boot change needed
- [ ] **Deferred (per-slot kernel rollback):** kernel-inside-each-slot's-IXFS needs a bootloader IXFS reader + slot binding in `load_kernel` -> §3 or a follow-up; §2 ships shared-ESP kernel + per-slot roots
- [ ] Commit: `"build: dual-slot disk layout -- grow disk, A/B metadata partition, Slot A+B IXFS"`

**Test checkpoint:** Built image has 5 partitions (ESP, BlackBox, A/B-metadata, Slot A, Slot B) in `fdisk -l`; the metadata partition LBAs appear in the `.info`; smoke still boots from Slot A. Test on: host `fdisk -l` + QEMU smoke; bare metal i5-4210U / i5-11600K.

> **Test runner:** N/A (host build tool; verified by running `make-system-disk --ab`: 768M valid, undersized/misaligned rejected) | validation: host run now, QEMU smoke once Makefile-wired

> **Notes:**
> - **What shipped:** `tools/make-system-disk.c` `--ab` now builds the 5-partition layout (ESP + BlackBox + A/B-metadata + Slot A + Slot B) + a post-layout fail-closed invariant + `META_OFFSET`/`IXFS_B_OFFSET` in the `.info`.
> - **How it runs:** `--ab` is opt-in and DORMANT until the Makefile wires it -- the default build is unchanged (still 3-partition single-slot), so build + smoke stay green; verified by running the host tool directly.
> - **Downstream effects:** the §1 metadata record now has an on-disk home; §3 selection reads it; the kernel's `partition_scan_all` mounts Slot A (first IXFS) with no boot change.
> - **Canonical doc:** `tools/make-system-disk.c` + the `<img>.info` offset contract.
> - **Scope boundary:** §2 ships the GPT-writer layout; the Makefile production wiring (use `--ab`, 768M, source `.info`, format both slots) + the bootloader pre-EBS metadata read remain open §2 items.

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
