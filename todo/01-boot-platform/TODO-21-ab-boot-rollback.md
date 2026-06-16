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
| 💎  |   3   | Bootloader slot selection logic                 | §1, §2     |  [/]   |
| 💎  |   4   | Boot failure counting and rollback              | §3         |  [ ]   |
| 💎  |   5   | Kernel `mark_boot_successful()` syscall         | §4         |  [ ]   |
| ⭐  |   6   | Slot status in boot diagnostics                 | §1-§5      |  [ ]   |
| 💎  |   7   | Boot metadata integrity + atomic writes         | §1         |  [/]   |
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
- [x] Wired `--ab` into the Makefile (768M; ESP FAT bounded to 64M via `-s 1`); Step 3 sources the `.info` offsets + runs `mkfs-ixfs` twice (Slot A = current root, Slot B identical). Smoke PASS -- kernel mounts Slot A
- [x] Dedicated A/B-metadata GPT partition (type GUID `...4D44...`) holding the two §1 blocks + `META_OFFSET`/`IXFS_B_OFFSET` in `.info`; the bootloader pre-EBS `EFI_BLOCK_IO` read landed in §3 (atomic write stays §7)
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

> **Verified:** 2026-06-16 | commit `de98d105` + Makefile wiring | 4/6 items | build OK | smoke PASS (KVM 1.95s -- dual-slot image boots Slot A, 29/29 POST16)
> **Quality reviewed:** 2026-06-16 | Codex 13x (design, adversarial, consistency, perf, re-adversarial) | 1H+7M fixed, 0 open | scope: boot-code-quality (Makefile + GPT writer)

---

## 3. Bootloader Slot Selection

Bootloader reads metadata and mounts the correct slot's filesystem.

> [!NOTE]
> **Scope (design review):** §3 is READ + SELECT + PASS + the kernel-side MOUNT. The bootloader is the SOLE slot-selection authority; the read is read-only. The `tries` increment-WRITE belongs to §4 (logic) + §7 (power-fail-atomic write) -- a non-atomic write here would defeat §7's torn-write-brick protection. The two `load_kernel`/seed items below stay blocked on TODO-07 §16's root-aware lookup.

- [x] At boot: read boot metadata -> determine active slot -- `select_active_slot()` (`bootx64.c`) reads the on-disk record from the A/B-metadata partition pre-EBS (`ab_find_meta_partition` + `ab_read_meta_copy`, EFI_BLOCK_IO)
- [x] Select the highest-`priority` valid, not-`unbootable` slot; tie-break by `successful` then lower `tries` -- `ab_boot_meta_choose_slot()` in the shared header (passes the §7 CRC validator; the §8 floor is wired by §8)
- [x] Slot state = `successful` / `pending` / `unbootable` (derived: `!successful && tries >= AB_BOOT_MAX_TRIES`); rolls back off an exhausted slot even if once-successful when another slot is viable -- unit-tested
- [ ] Increment `tries` for active slot before booting -- **owned by §4 + §7** (atomic `boot_meta_write`; §3 read is read-only) -> XREF: §4 increment item + §7 atomic write
- [x] Mount the active slot's partition (not EFI) for kernel loading -- kernel `partition_mount_filesystems(active_slot)` mounts the selected slot's IXFS as C: (`gpt_ixfs_slot` tells A from B), recording `ab_boot_mounted_slot`/`ab_boot_slot_mismatch`
- [x] Pass `boot_info.active_slot` to kernel -- `uint8_t active_slot` (0=A, 1=B; logs render 'A'/'B') in header + mirror + manifest + doc, carved from the reserved tail (no `BOOT_INFO_VERSION` bump)
- [x] Log: `"[BOOT] Booting Slot %c (tries=%u, successful=%u)"` -- emitted by `select_active_slot` on the metadata-valid path
- [ ] Honor SPLIT `payload.root` in `load_kernel()` so the seeded slot-b entry resolves to its partition -> XREF: [`TODO-07 §16`](TODO-07-boot-entry-store-menu-policy.md#16-bootstrap-and-first-install-entry-seeding).
- [ ] Flip seeded `slot-b` from inactive to active in `bootcfg.py` `_seed_store()` once root-aware lookup ships -> XREF: [`TODO-07 §16`](TODO-07-boot-entry-store-menu-policy.md#16-bootstrap-and-first-install-entry-seeding).
- [ ] Kernel GPT split-brain hardening: reconcile primary/backup slot-root entries in `gpt_parse()` (`src/kernel/fs/gpt.c`), or bind the kernel mount to a bootloader-passed authoritative root LBA. Not reachable single-disk.
- [x] Commit: `"boot: slot selection -- boot from active slot, rollback on failure"`

**Test checkpoint:** Normal boot shows `"Booting Slot A (tries=0, successful=0)"` on a fresh disk (uninitialized metadata -> factory defaults -> Slot A) or `"... successful=1"` once mark-good lands; the kernel logs `A/B: mounted slot 0 as C:`. `make test-boot` covers the selection state machine (6 cases incl. rollback + tie-breaks) + `gpt_ixfs_slot` (A/B/non-IXFS). Manual metadata corruption / unreadable blocks fail closed (`boot_fatal`).

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 26 ab_boot cases, 0 failures (14 new in §3) | bootloader `select_active_slot` + kernel boot-disk binding validated via QEMU smoke + bare metal (no kernel test surface for UEFI BlockIO / boot-time globals)

> **Notes:**
> - **What shipped:** bootloader pre-EBS `select_active_slot()`, shared-header `ab_boot_meta_choose_slot()`, `boot_info.active_slot` (0/1), kernel slot-aware mount (`gpt_ixfs_slot`, `partition_mount_filesystems(active_slot)`).
> - **How it runs:** pre-EBS in `efi_main` between `boot_policy_invoke()` and `load_kernel` (POST16 `0xB0B9`/`0xB0BA`); kernel mounts EXACTLY the selected slot's IXFS as C: bound to the boot disk (no cross-slot/cross-disk fallback) + records `ab_boot_mounted_slot`/`ab_boot_slot_mismatch`.
> - **Fail posture:** both GPT copies are CRC-validated (header + entry-array) and reconciled (primary authoritative, backup recovers, split-brain/corrupt -> `boot_fatal`); non-A/B / non-GPT / uninitialized -> Slot A; selected slot unavailable or multi-disk-ambiguous -> C: unmounted (rollback next boot).
> - **Downstream effects:** closes §2's open pre-EBS `EFI_BLOCK_IO` read item; `active_slot` + mismatch flag feed §4 (rollback), §5 (mark-good gating), §6 (VPD slot status).
> - **Canonical doc:** `docs/boot/boot-info-fields.md` (`active_slot` row) + `include/boot/ab_boot_metadata.h`.
> - **Scope boundary:** §4 owns the `tries` write + rollback notice, §7 the atomic metadata write, §8 the floor check, TODO-07 §16 the boot-menu slot-b entry.

> **Verified:** 2026-06-16 | commit `46726a8a` (+review fixes) | 7/11 items (4 deferred: §4/§7 tries-write, 2x TODO-07 §16, kernel-GPT split-brain) | build OK | smoke PASS (KVM 1.96s) | 5925 kernel + 16 user PASS
> **Deferred:** [M] §3 -> §4 + §7: the `tries` increment-before-boot WRITE is owned by §4's failure-counting atop §7's power-fail-atomic `boot_meta_write`; §3 ships the read-only selection -> XREF: 01-boot-platform/TODO-21 §4 (item: "Bootloader increments `tries` BEFORE attempting boot" at §4) + §7 (item: "`boot_meta_write()` is power-fail-atomic" at §7).
> **Deferred:** [L] §3 -> TODO-07 §16 (cross-TODO): SPLIT `payload.root` root-aware `load_kernel` + flipping the seeded `slot-b` active block on TODO-07 §16's root-aware lookup -> XREF: 01-boot-platform/TODO-07 §16 (item: "Root-aware `load_kernel()`: honor a SPLIT entry's `payload.root`" at §16).
> **Deferred:** [L] §3 -> §3 follow-up (hardening): kernel-side GPT primary/backup slot-root reconciliation (split-brain vs the bootloader's reconciled view); not reachable single-disk -> XREF: 01-boot-platform/TODO-21 §3 (item: "Kernel GPT split-brain hardening" at §3).
> **Quality reviewed:** 2026-06-16 | Codex 15x (design + adversarial + consistency + perf + re-adversarial) | 16H+1M fixed, 1H rejected (evidence), 1H deferred-XREF | scope: boot-code-quality + kernel-code-quality

---

## 4. Boot Failure Counting and Rollback

Automatic rollback after 3 consecutive boot failures.

> [!IMPORTANT]
> **DESIGN LOCKED (Codex design review 2H+1M adopted).** §4 must NOT ship alone: (1) **Co-ship with §5's reset.** If `tries` is incremented (§4) without `mark_boot_successful()` resetting it (§5), every NORMAL successful reboot consumes a try and after `AB_BOOT_MAX_TRIES` §3 rolls a HEALTHY slot away. The minimal mark-good/reset path lands WITH the first tries-increment, or try-based rollback stays DISABLED until reset is implemented + covered by a normal-reboot test. (2) **Classify which failures count.** On the current shared-ESP layout (kernel.exe on the shared ESP, no per-slot kernel), a shared-ESP / kernel-load / firmware failure is NOT a slot failure and rolling roots cannot fix it -- only count failures attributable to the selected slot; pin the exact pre-EBS write point (after non-slot bootloader work, before EBS while EFI_BLOCK_IO is still live -- NOT after `jump_to_kernel`). (3) **Both-exhausted stopgap:** TODO-22 recovery is not built; define a concrete stopgap (boot the least-bad previously-successful slot with a loud diagnostic) rather than an undefined terminal path. The bootloader write uses §7's pure helpers (`ab_boot_meta_write_target`/`next_generation`) + a WriteBlocks IoAlign full-block RMW adapter + `boot_info.ab_meta_lba`.

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

> [!NOTE]
> **DESIGN LOCKED (Codex 2H+1M adopted).** The READ side shipped in §3; the remaining work is the power-fail-atomic WRITE primitive. Adopted: (1) initialized-corrupt both-invalid metadata FAILS CLOSED (does NOT factory-default to Slot A -- preserves the §3 rollback-safety invariant); only an all-zero uninitialized partition defaults to Slot A. (2) The bootloader passes its reconciled MD-partition LBA range + boot-disk identity via `boot_info`; the kernel writes those LBAs directly and MUST NOT re-derive the MD partition via the kernel `gpt_parse` (which lacks the bootloader's primary+backup reconciliation -- a stale/split-brain primary would diverge). (3) The write adapter does a full-media-block, IoAlign-compliant, read-modify-write of exactly one block (`WriteBlocks`/`FlushBlocks` errors = write failure), never serializing a bare 60-byte struct.

- [x] `crc32` field + monotonic `generation`; reader rejects bad magic/CRC -- shipped §3 (`ab_boot_meta_is_valid` + `ab_boot_meta_compute_crc`)
- [x] TWO redundant copies; reader returns the valid copy with the highest `generation` -- shipped §3 (`ab_boot_meta_select_newest` + `ab_read_meta_copy` read both copies at byte offsets 0 + 4096)
- [ ] `boot_meta_write()` power-fail-atomic write primitive: shared pure `ab_boot_meta_prepare_write` (lower-generation target + gen bump + CRC) + thin bootloader/kernel disk adapters writing only that copy -- DESIGN LOCKED (see note)
- [x] Both copies invalid: all-zero -> Slot A (first boot); initialized-corrupt -> fail closed (not factory-default) -- shipped §3 (`select_active_slot`); preserves the rollback-safety invariant
- [x] GPT storage: two metadata blocks at fixed byte offsets in the A/B-metadata partition (NVRAM rejected per BootSticky doctrine) -- shipped §3 (`AB_META_COPY0/1_BYTE_OFF` 0 + 4096)
- [x] Validate `version`; unknown future version fails safe (rejected, not misparsed) -- shipped §3 (`ab_boot_meta_is_valid`)
- [ ] Pass the bootloader-reconciled MD-partition LBA range + boot-disk identity via `boot_info` so the kernel write path (§5) targets the correct partition without re-deriving GPT state -- DESIGN LOCKED, new `boot_info` fields
- [ ] Commit: `"boot: A/B metadata atomic write -- power-fail-atomic boot_meta_write + boot_info MD range"`

**Test checkpoint:** Flip a byte in one copy -- the reader returns the other (shipped §3). `ab_boot_meta_prepare_write` picks the lower-generation target + bumps generation (unit test). Write then read-back round-trips on QEMU; a simulated crash after writing one copy still boots from the untouched prior-good copy. Test on: QEMU smoke + bare metal (media write-cache behavior differs).

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
| 💎 | Dual-slot boot            | ⚠️ Automatic Repair only  | ⚠️ systemd-boot assess   | 🟦 §1-§3 select+mount     |
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
