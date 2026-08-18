---
schema_version: 1
id: ab-boot-rollback
domain: 01-boot-platform
status: active
title: "TODO-21 -- A/B Dual-Slot Boot & Automatic Rollback"
---

# TODO-21 -- A/B Dual-Slot Boot & Automatic Rollback

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

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
- → XREF: `TODO-13-tpm-measured-boot-attestation.md §17`/`§21`/`§22` -- NV write-lock + counter primitives (§17), the authenticated floor record (§21) and the loader-side floor read (§22) back the §8 anti-rollback floor
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

| ⭐  | Order | Deliverable                                 | Depends On | Status |
| --- | :---: | ------------------------------------------- | ---------- | :----: |
| 💎  |   1   | Boot metadata structure (GPT/disk wire ABI) | --         |  [/]   |
| 💎  |   2   | Dual-slot disk layout in build system       | §1         |  [/]   |
| 💎  |   3   | Bootloader slot selection logic             | §1, §2     |  [/]   |
| 💎  |   4   | Boot failure counting and rollback          | §3         |  [/]   |
| 💎  |   5   | Kernel `mark_boot_successful()` syscall     | §4         |  [/]   |
| ⭐  |   6   | Slot status in boot diagnostics             | §1-§5      |  [x]   |
| 💎  |   7   | Boot metadata integrity + atomic writes     | §1         |  [x]   |
| 💎  |   8   | Per-slot anti-rollback version floor        | §1, §7     |  [/]   |

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
- [/] **Deferred (per-slot kernel rollback):** kernel-inside-each-slot's-IXFS needs a bootloader IXFS reader + slot binding in `load_kernel` -> §3 or a follow-up; §2 ships shared-ESP kernel + per-slot roots
- [x] Commit: `"build: dual-slot disk layout -- grow disk, A/B metadata partition, Slot A+B IXFS"` **Verified 2026-07-31:** work landed as `498cabfb` + review `698f19d1`; message drifted from the plan

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
- [x] Increment `tries` for active slot before booting -- shipped in §4 (`ab_bl_increment_tries`, pre-EBS atomic `WriteBlocks` RMW via §7 helpers); §3 read stays read-only
- [x] Mount the active slot's partition (not EFI) for kernel loading -- kernel `partition_mount_filesystems(active_slot)` mounts the selected slot's IXFS as C: (`gpt_ixfs_slot` tells A from B), recording `ab_boot_mounted_slot`/`ab_boot_slot_mismatch`
- [x] Pass `boot_info.active_slot` to kernel -- `uint8_t active_slot` (0=A, 1=B; logs render 'A'/'B') in header + mirror + manifest + doc, carved from the reserved tail (no `BOOT_INFO_VERSION` bump)
- [x] Log: `"[BOOT] Booting Slot %c (tries=%u, successful=%u)"` -- emitted by `select_active_slot` on the metadata-valid path
- [/] Honor SPLIT `payload.root` in `load_kernel()` so the seeded slot-b entry resolves to its partition -> XREF: [`TODO-07 §16`](TODO-07-boot-entry-store-menu-policy.md#16-bootstrap-and-first-install-entry-seeding).
- [/] Flip seeded `slot-b` from inactive to active in `bootcfg.py` `_seed_store()` once root-aware lookup ships -> XREF: [`TODO-07 §16`](TODO-07-boot-entry-store-menu-policy.md#16-bootstrap-and-first-install-entry-seeding).
- [/] Kernel GPT split-brain hardening: reconcile primary/backup slot-root entries in `gpt_parse()`, or bind the kernel mount to a bootloader-passed authoritative root LBA. Blocked: not reachable single-disk
  - Owner: [`05-storage-filesystems/TODO-13 §1`](../05-storage-filesystems/TODO-13-partition-tools-storage-suite.md) (item: "Reconcile a primary/backup GPT divergence on the READ path"), which already owns the identical-entry-array invariant `gpt_write_all` maintains. `src/kernel/fs/gpt.c` is the single edit site for both sides
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

> **Verified:** 2026-06-16 | commit `46726a8a` (+review fixes; tries-write since shipped in §4) | 8/11 items (3 deferred: 2x TODO-07 §16, kernel-GPT split-brain) | build OK | smoke PASS (KVM 1.96s) | 5925 kernel + 16 user PASS
> **Deferred:** [L] §3 -> TODO-07 §16 (cross-TODO): SPLIT `payload.root` root-aware `load_kernel` + flipping the seeded `slot-b` active block on TODO-07 §16's root-aware lookup -> XREF: 01-boot-platform/TODO-07 §16 (item: "Root-aware `load_kernel()`: honor a SPLIT entry's `payload.root`" at §16).
> **Deferred:** [L] §3 -> §3 follow-up (hardening): kernel-side GPT primary/backup slot-root reconciliation (split-brain vs the bootloader's reconciled view); not reachable single-disk -> XREF: 01-boot-platform/TODO-21 §3 (item: "Kernel GPT split-brain hardening" at §3).
> **Quality reviewed:** 2026-06-16 | Codex 15x (design + adversarial + consistency + perf + re-adversarial) | 16H+1M fixed, 1H rejected (evidence), 1H deferred-XREF | scope: boot-code-quality + kernel-code-quality

---

## 4. Boot Failure Counting and Rollback

Automatic rollback after 3 consecutive boot failures.

> [!IMPORTANT]
> **DESIGN LOCKED (Codex design review 2H+1M adopted).** §4 must NOT ship alone: (1) **Co-ship with §5's reset.** If `tries` is incremented (§4) without `mark_boot_successful()` resetting it (§5), every NORMAL successful reboot consumes a try and after `AB_BOOT_MAX_TRIES` §3 rolls a HEALTHY slot away. The minimal mark-good/reset path lands WITH the first tries-increment, or try-based rollback stays DISABLED until reset is implemented + covered by a normal-reboot test. (2) **Classify which failures count.** On the current shared-ESP layout (kernel.exe on the shared ESP, no per-slot kernel), a shared-ESP / kernel-load / firmware failure is NOT a slot failure and rolling roots cannot fix it -- only count failures attributable to the selected slot; pin the exact pre-EBS write point (after non-slot bootloader work, before EBS while EFI_BLOCK_IO is still live -- NOT after `jump_to_kernel`). (3) **Both-exhausted stopgap:** TODO-22 recovery is not built; define a concrete stopgap (boot the least-bad previously-successful slot with a loud diagnostic) rather than an undefined terminal path. The bootloader write uses §7's pure helpers (`ab_boot_meta_write_target`/`next_generation`) + a WriteBlocks IoAlign full-block RMW adapter + `boot_info.ab_meta_lba`.

- [x] Bootloader increments `tries` before boot -- `ab_bl_increment_tries` (`bootx64.c`), pre-EBS after `load_kernel` OK, IoAlign-compliant `WriteBlocks` single-block RMW (§7 helpers), `FlushBlocks`-checked; no-op on non-A/B
- [x] Boot success resets `tries` to 0 -- §5 `ab_boot_mark_slot_successful` at first-frame (smoke round-trip: increment gen1 -> mark-good gen2 tries=0)
- [x] 3 boots without mark-good -> roll back on next boot -- §3 `ab_boot_meta_choose_slot` skips a slot at `tries >= AB_BOOT_MAX_TRIES`; the increment now drives it
- [x] On rollback: serial `"Slot X failed N times -- rolling back to Slot Y"` -- `ab_boot_meta_decide` (shared header) classifies NORMAL / ROLLBACK / BOTH_EXHAUSTED; `select_active_slot` emits the rollback line with the failed slot + its try count
- [x] On rollback: on-screen notice "Reverting to previous version (Slot Y)" -- `ab_draw_rollback_banner` paints a self-contained banner (own dark background so the AA text is legible over any firmware/splash backdrop) via `bsod_fill_rect` + `bsod_aa_string`; no-op headless
- [x] Both slots `tries >= MAX` -> stopgap implemented: `decide` returns BOTH_EXHAUSTED + `ab_boot_meta_least_bad_slot` (prefer once-successful, then priority, then fewer tries) with a loud serial + on-screen diagnostic; the full recovery UI is still owned by -> XREF: [`TODO-22`](TODO-22-recovery-partition.md)
- [x] Codebase-wide bootloader `ReadBlocks` IoAlign hardening -- shipped in TODO-02 §19 (`bl_read_blocks_aligned` helper + GPT/FAT/A-B reader retrofits; `IoAlign <= 1` fast path smoke-validated, `IoAlign > 8` bounce path bare-metal-pending)
- [x] Commit: `"boot: automatic rollback after 3 failed boot attempts"`

**Test checkpoint:** A fresh boot logs `"A/B: tries incremented"` then `"A/B: boot marked successful (gen 2)"` (smoke-validated round-trip). A slot that never reaches mark-good `AB_BOOT_MAX_TRIES` times rolls back (`choose_slot`). Test on: QEMU smoke + bare metal.

**Regression risk:** MEDIUM -- co-shipped with §5's reset so a normal boot's increment is reset at acceptance (no healthy-slot rollback); smoke confirms the 0->1->0 round-trip.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | ab_boot pure-logic cases pass (7 new: all_exhausted, least_bad, decide normal/rollback/priority/both-exhausted) | the bootloader `ab_bl_increment_tries` + `ab_draw_rollback_banner` (EFI_BLOCK_IO / GOP) have no kernel unit surface -- validated via QEMU smoke (tries incremented + reset round-trip) + bare metal

> **Notes:**
> - **What shipped:** `ab_bl_increment_tries()` (pre-EBS tries-increment, IoAlign `WriteBlocks` RMW, co-shipped with §5's reset) + `ab_boot_meta_decide`/`ab_boot_meta_least_bad_slot` (shared header) + `select_active_slot` rollback/both-exhausted serial diagnostics + `ab_draw_rollback_banner` on-screen notice.
> - **How it runs:** increment fires after `load_kernel` OK + before ExitBootServices; the kernel mark-good resets it at first-frame acceptance -- smoke shows the 0->1->0 round-trip (gen1 copy0 -> gen2 copy1).
> - **Fail posture:** durable (`FlushBlocks` return-checked, no success on flush fail); rejects non-block-aligned geometry + generation exhaustion + unreadable copies; no-op on non-A/B.
> - **Rollback UX:** `decide` classifies NORMAL/ROLLBACK/BOTH_EXHAUSTED; rollback + both-exhausted emit a serial line and a self-contained framebuffer banner; both-exhausted boots the least-bad once-successful slot rather than an undefined terminal path.
> - **Canonical doc:** `include/boot/ab_boot_metadata.h`.
> - **Scope boundary:** the codebase-wide read-`ReadBlocks`-IoAlign hardening shipped in TODO-02 §19 (`bl_read_blocks_aligned`); the A/B write path was already aligned.

> **Verified:** 2026-06-16 | commit `91234a8e` + diagnostics | 6/7 items | build OK | smoke PASS (KVM 2.0s, healthy-boot round-trip, no false rollback) | boot suite 2772 PASS (7 new ab_boot)
> **Deferred:** [Low] Codebase-wide bootloader `ReadBlocks` IoAlign hardening (pre-existing idiom; A/B write path already aligned) -> XREF: 01-boot-platform/TODO-02 §19 (item: "Add a shared `bl_read_blocks_aligned()` helper") (RESOLVED 2026-06-16 by TODO-02 §19: helper + GPT/FAT/A-B retrofits shipped; IoAlign>8 path bare-metal-pending)
> **Quality reviewed:** 2026-06-16 | Codex 3x (adversarial, consistency, perf) | 0H+0M+0L fixed, 0 open | scope: boot-code-quality (re-adversarial n/a: all 3 approved, zero fix diff)

---

## 5. Kernel `mark_boot_successful()`

Kernel-side API to tell the bootloader "this boot worked".

- [x] `ab_boot_mark_slot_successful(active_slot)` (`partition.c`): power-fail-atomic single-copy blkdev RMW writing `successful=1, tries=0` (§7 helpers); refuses unless mounted slot == selected; durable (`blkdev_sync`-checked)
- [/] Called at boot acceptance from the compositor first-frame steady-state; configured-acceptance-stage upgrade -> XREF: [`02-kernel-core/TODO-02 §10`](../02-kernel-core/TODO-02-kernel-configuration-policy.md)
- [/] Verified slot identity gate before mark-good (refuse unsigned/unverified slot); blocked on code-integrity infra -> XREF: [`02-kernel-core/TODO-19 §7`](../02-kernel-core/TODO-19-code-integrity-trust-policy.md) (slot-boot-verification query)
- [/] On mark-good, advance the §8 `rollback_floor` to the active slot's `rollback_index` (never lower it) -- lands with §8, itself blocked on the authenticated floor store -> XREF: [`TODO-13 §22`](TODO-13-tpm-measured-boot-attestation.md)
- [x] On success: `"A/B: boot marked successful (slot %d, copy %u, gen %u)"`; write/flush failure logs + returns -1 (boot proceeds)
- [x] Storage = GPT/disk metadata blocks (NVRAM `SetVariable` rejected per BootSticky doctrine, §1) -- writes the MD partition, not a UEFI variable
- [x] Commit: `"kernel: mark_boot_successful -- reset try counter after successful boot"`

**Test checkpoint:** Normal boot -> serial shows `"A/B: boot marked successful (slot 0, copy 0, gen 1)"` after the desktop first frame (smoke-validated). Refused when mounted slot != selected, or on flush failure. Test on: QEMU smoke + bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | ab_boot pure write-logic cases pass | the kernel write adapter (`ab_boot_mark_slot_successful`, blkdev I/O) has no RAM-blkdev unit fixture -- validated via QEMU smoke (mark-good fires + writes gen 1) + bare metal

> **Notes:**
> - **What shipped:** `ab_boot_mark_slot_successful()` + `ab_find_meta_blkdev()` + `ab_read_meta_copy_k()` in `partition.c` (kernel A/B metadata write path) + the compositor first-frame hook.
> - **How it runs:** fires once at the desktop first-frame steady-state; locates the MD sub-blkdev by `start_lba == boot_info.ab_meta_lba`; power-fail-atomic single-copy RMW; durable (`blkdev_sync`-checked).
> - **Fail posture:** refuses unless mounted slot == selected slot; refuses on generation exhaustion or flush failure; no-op on non-A/B disks; rollback stays dormant until §4 increments tries (no brick).
> - **Downstream effects:** initializes the on-disk metadata (gen 1) on first successful boot; §4 tries-increment + rollback builds on this reset.
> - **Canonical doc:** `include/boot/ab_boot_metadata.h` + `docs/boot/boot-info-fields.md` (`ab_meta_lba`).
> - **Scope boundary:** verified-identity gate (TODO-19 §7) + configured-acceptance-stage upgrade (TODO-02 §10) + §8 floor-advance are tracked `[ ]` items here; §4 owns the tries-increment that activates rollback.

> **Verified:** 2026-06-16 | commit `b4370256` | 3/6 items | build OK | smoke PASS (mark-good gen1 round-trip, slot-mismatch refusal)
> **Deferred:** [M] Verified slot-identity gate before mark-good (refuse unsigned/unverified slot) -> XREF: 02-kernel-core/TODO-19 §7 (item: "Expose a slot-boot-verification query ... so A/B mark-good + the anti-rollback floor refuse to bless an unverified slot")
> **Deferred:** [M] Configured-acceptance-stage upgrade (mark-good fires at compositor first-frame; gate it on the typed acceptance ledger instead) -> XREF: 02-kernel-core/TODO-02 §10 (item: "Gate mark_boot_successful() ... on the configured acceptance stage instead of a hard-coded Phase 3 heuristic")
> **Deferred:** [L] On mark-good, advance the rollback_floor to the active slot rollback_index -> XREF: 01-boot-platform/TODO-21 §8 (anti-rollback floor advance item)
> **Quality reviewed:** 2026-06-16 | Codex adversarial (impl): 2H fixed (mark-good mounted==selected equality gate; durability blkdev_sync return-checked), 0 open | scope: kernel-code-quality

---

## 6. Slot Status in Boot Diagnostics

Integrate A/B slot status into the VPD and boot timing display.

- [x] VPD per-slot status `"Slot A [active] tries=N bootable/EXHAUSTED verified/pending"` -- `vpd_render_ab_slot`; health from `tries` vs `AB_BOOT_MAX_TRIES`, `successful` shown separately as verified/pending (not conflated)
- [x] Rollback/both-exhausted VPD warning -- distinct `"Rolled back from Slot X failures=N"` vs `"Both slots exhausted -- booting least-bad"`, so a terminal state never reads as a simple rollback
- [x] boot_timing logs the A/B resolution point -- `ab-slot-handoff` step in `boot_hw.c`, labeled a kernel handoff observation (real selection is pre-EBS, different TSC domain)
- [x] Snapshot wiring: A/B status fields in `boot_info` carved from `_loader_vars_pad` (no version bump); `select_active_slot` publishes the as-selected `ab_boot_meta_decide` result + mirror/manifest/doc/`_Static_assert`
- [x] Commit: `"boot: A/B slot status in VPD diagnostics"`

**Test checkpoint:** Boot with `postbars=on` -- the VPD info-header shows two slot lines (active/standby, tries, bootable/EXHAUSTED, verified/pending); a rolled-back boot shows the rollback warning. The boot timeline includes `ab-slot-handoff` (smoke: `PHASE1 +62ms ab-slot-handoff`). Test on: QEMU smoke + bare metal.

> **Test runner:** N/A (VPD rendering draws to the framebuffer + bootloader publish is pre-EBS -- no kernel runtime test surface) | the boot_info A/B snapshot layout is pinned by compile-time `_Static_assert` (kernel + mirror) + the manifest compare gate; validated via QEMU smoke (`ab-slot-handoff` in the timeline, no boot regression) + bare-metal visual

> **Notes:**
> - **What shipped:** `vpd_render_ab_slot` + the A/B block in `vpd_render_info_header` (`vpd.c`); a 5-byte `boot_info` A/B snapshot carved from `_loader_vars_pad`; bootloader publish in `select_active_slot`; `ab-slot-handoff` boot_timing step.
> - **How it runs:** the bootloader snapshots the `ab_boot_meta_decide` result at selection time (authoritative -- the on-disk record mutates as tries increment/reset) + sets `ab_status_valid`; the kernel VPD renders only when that marker is set (an older same-version loader -> "snapshot unavailable", never a false zero-state).
> - **Design adoptions:** health from `tries` vs `AB_BOOT_MAX_TRIES` (not the `successful` flag); both-exhausted as its own terminal message; the boot_timing step labeled a handoff observation, not pre-EBS latency.
> - **Downstream effects:** complements the §4 pre-EBS on-screen banner with the persistent kernel diagnostic surface; non-A/B disks render "A/B Boot: single-slot".
> - **Canonical doc:** `docs/boot/boot-info-fields.md` (the `ab_select_reason`/`ab_from_slot`/`ab_slot_tries`/`ab_slot_flags` rows).
> - **Scope boundary:** §6 owns the kernel VPD + boot_timing surface; the pre-EBS banner is §4; the anti-rollback floor is §8.

> **Verified:** 2026-06-16 | commit `f5dfae76` + review fixes | 4/4 items | build OK | smoke PASS (KVM 2.33s) + boot 2777 PASS
> **Quality reviewed:** 2026-06-16 | Codex 7x (design, adversarial, consistency, perf, re-adversarial x2) | 3M fixed, 0 open | scope: kernel-code-quality + boot-code-quality

---

## 7. Boot Metadata Integrity and Atomic Writes

Boot metadata is the single source of truth for slot selection; a torn write must never brick the box. Follow the Android libavb_ab pattern: magic + CRC-32 + redundant copies with a monotonic generation.

> [!NOTE]
> **DESIGN LOCKED (Codex 2H+1M adopted).** The READ side shipped in §3; the remaining work is the power-fail-atomic WRITE primitive. Adopted: (1) initialized-corrupt both-invalid metadata FAILS CLOSED (does NOT factory-default to Slot A -- preserves the §3 rollback-safety invariant); only an all-zero uninitialized partition defaults to Slot A. (2) The bootloader passes its reconciled MD-partition LBA range + boot-disk identity via `boot_info`; the kernel writes those LBAs directly and MUST NOT re-derive the MD partition via the kernel `gpt_parse` (which lacks the bootloader's primary+backup reconciliation -- a stale/split-brain primary would diverge). (3) The write adapter does a full-media-block, IoAlign-compliant, read-modify-write of exactly one block (`WriteBlocks`/`FlushBlocks` errors = write failure), never serializing a bare 60-byte struct.

- [x] `crc32` field + monotonic `generation`; reader rejects bad magic/CRC -- shipped §3 (`ab_boot_meta_is_valid` + `ab_boot_meta_compute_crc`)
- [x] TWO redundant copies; reader returns the valid copy with the highest `generation` -- shipped §3 (`ab_boot_meta_select_newest` + `ab_read_meta_copy` read both copies at byte offsets 0 + 4096)
- [x] Power-fail-atomic write primitive shipped: `ab_boot_meta_write_target` + `ab_boot_meta_next_generation` (header); thin one-copy adapters -- §4 `ab_bl_increment_tries` + §5 `ab_boot_mark_slot_successful`, IoAlign/blkdev RMW, flush-checked
- [x] Both copies invalid: all-zero -> Slot A (first boot); initialized-corrupt -> fail closed (not factory-default) -- shipped §3 (`select_active_slot`); preserves the rollback-safety invariant
- [x] GPT storage: two metadata blocks at fixed byte offsets in the A/B-metadata partition (NVRAM rejected per BootSticky doctrine) -- shipped §3 (`AB_META_COPY0/1_BYTE_OFF` 0 + 4096)
- [x] Validate `version`; unknown future version fails safe (rejected, not misparsed) -- shipped §3 (`ab_boot_meta_is_valid`)
- [x] Bootloader-reconciled MD-partition LBA range via `boot_info.ab_meta_lba`/`ab_meta_block_count` (§3); the kernel write path targets those LBAs directly without re-deriving GPT state
- [x] Commit: `"boot: A/B metadata atomic write -- power-fail-atomic boot_meta_write + boot_info MD range"`

**Test checkpoint:** Flip a byte in one copy -- the reader returns the other (shipped §3). `ab_boot_meta_prepare_write` picks the lower-generation target + bumps generation (unit test). Write then read-back round-trips on QEMU; a simulated crash after writing one copy still boots from the untouched prior-good copy. Test on: QEMU smoke + bare metal (media write-cache behavior differs).

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | `ab_boot` write_target + next_generation (incl. ceiling) cases pass | the disk adapters (`ab_bl_increment_tries` EFI_BLOCK_IO + `ab_boot_mark_slot_successful` blkdev) have no unit fixture -- validated via QEMU smoke (atomic gen1 copy0 -> gen2 copy1 alternation round-trip) + bare metal

> **Notes:**
> - **What shipped:** the power-fail-atomic write contract -- shared pure `ab_boot_meta_write_target` + `ab_boot_meta_next_generation` (header) + the boot_info MD-range handoff; the READ side + redundancy + storage + version-gate shipped in §3.
> - **How it runs:** the write helpers pick the overwrite-invalid-first-else-lower-generation copy + a strictly-greater generation, so a crash mid-write always leaves the other copy valid; the thin §4 bootloader + §5 kernel adapters do a single-block IoAlign/blkdev RMW of exactly that copy, flush-checked.
> - **Downstream effects:** §4 tries-increment + §5 mark-good both write through this primitive; the smoke round-trip (gen1 copy0 -> gen2 copy1) proves the dual-copy alternation is atomic.
> - **Canonical doc:** `include/boot/ab_boot_metadata.h` (write-support helpers) + `docs/boot/boot-info-fields.md` (`ab_meta_lba`).
> - **Scope boundary:** §7 owns the atomic-write contract + redundancy; the increment/mark behavior is §4/§5, slot selection is §3, the anti-rollback floor is §8.

> **Verified:** 2026-06-16 | helpers §1 `483f8982` + adapters §4 `91234a8e` + §5 `b4370256` | 6/6 items | build OK | smoke PASS (atomic gen1->gen2 round-trip)
> **Quality reviewed:** 2026-06-16 | Codex: write helpers under §1 (generation-ceiling fix) + §4 3x re-adversarial (durability/IoAlign/FlushBlocks) + §5 (mark-good gate) | 0 open | scope: boot-code-quality (reconciliation -- no new §7 code)

---

## 8. Per-Slot Anti-Rollback Version Floor

Prevent a security update from being rolled back to an older, vulnerable slot. Follow the Android `stored_rollback_index` model: a monotonic version floor that advances only after a slot is verified and marked good.

> [!IMPORTANT]
> **DESIGN LOCKED (Codex design review 2026-06-16, 1C+1H+1M adopted) -- TWO INDEPENDENT FLOORS; enforcement BLOCKED on an authenticated store:**
> 1. **Two independent floors.** The GLOBAL OS-security-version floor stays owned by `boot_rollback.c` (`IPOSRequiredSecVersion` NVRAM, `boot_rollback_raise_if_steady`). The PER-SLOT A/B `rollback_floor` (metadata) is a SEPARATE value read only by A/B slot selection and MUST NOT advance through `boot_rollback_raise_if_steady` (that global path has no verified-active-slot input). Corrects the earlier "reuse boot_rollback.c floor" framing.
> 2. **The floor store MUST be authenticated.** `ab_boot_meta_is_valid` checks only CRC-32 = corruption detection, NOT authenticity: anyone who can write the metadata partition can lower `rollback_floor` + recompute the CRC. CRC-metadata is a CACHED HINT, never the floor AUTHORITY. Real anti-rollback REQUIRES a monotonic / write-locked store (TODO-13 §27, on §21's index lifecycle and §17's primitives, read pre-selection by §22); selection enforcement ships WITH that store -- enforcing against an unauthenticated floor is false security.
> 3. **Below-floor refusal is DISTINCT from retry exhaustion.** A `tries >= AB_BOOT_MAX_TRIES` slot may boot as a least-bad availability stopgap; a below-floor slot must NOT (known-vulnerable). Selection filters below-floor slots; if NO at-or-above-floor slot exists -> a `ROLLBACK_BLOCKED` refusal that routes to recovery (TODO-22), NEVER `least_bad`.

- [/] Per-slot `rollback_index` + a single `rollback_floor` -- **fields present** (§1: `ab_boot_slot.rollback_index`, `ab_boot_metadata.rollback_floor`); the comparison semantics ship with the authenticated store
  - Blocked: the floor has no authenticated home to compare against until the anti-rollback anchors are provisioned -> XREF: [`TODO-13 §22`](TODO-13-tpm-measured-boot-attestation.md) (item: "Provision the authorized anti-rollback anchors")
- [/] §3 selection rejects `rollback_index < rollback_floor` as a DISTINCT `ROLLBACK_BLOCKED` state -> XREF: [`TODO-13 §22`](TODO-13-tpm-measured-boot-attestation.md) (item: "Read the floor BEFORE `select_active_slot()` decides")
  - Filter below-floor slots from the normal choice; all-below-floor refuses and routes to recovery, NEVER `least_bad`.
- [/] Advance `rollback_floor` to the active slot's `rollback_index` ONLY after `mark_boot_successful()` AND verified slot identity -> XREF: [`02-kernel-core/TODO-19 §7`](../02-kernel-core/TODO-19-code-integrity-trust-policy.md)
- [/] Store `rollback_floor` in an authenticated monotonic / write-locked TPM-NV index; CRC metadata is a cached hint. The record shape shipped in §27; the anchors are not provisioned -> XREF: [`TODO-13 §22`](TODO-13-tpm-measured-boot-attestation.md)
- [/] On rollback NEVER lower `rollback_floor` (monotonic raise only); a below-floor slot is unbootable and routes to recovery -> XREF: [`TODO-22`](TODO-22-recovery-partition.md)
- [/] Commit: `"boot: A/B anti-rollback -- per-slot version floor, monotonic, TPM-NV backed"` -- blocked with the rest of §8 on the authenticated floor store -> XREF: [`TODO-13 §22`](TODO-13-tpm-measured-boot-attestation.md)

**Test checkpoint:** (when the authenticated store lands) `rollback_floor=5`, slot B `rollback_index=4`: selection refuses B as below-floor; both below floor -> `ROLLBACK_BLOCKED` + recovery (NOT least-bad). Mark slot A good at index 6 with verified identity: floor advances to 6. A test asserts `boot_rollback_raise_if_steady` does NOT mutate the A/B `rollback_floor` (independent floors). Test on: QEMU smoke + bare metal.

> **Test runner:** N/A (enforcement deferred -- blocked on the authenticated floor store) | the per-slot `rollback_index`/`rollback_floor` fields are pinned by §1 `_Static_assert`; the selection-logic + tests land with TODO-13 §27 + §22

> **Notes:**
> - **What shipped:** the corrected §8 DESIGN only (two independent floors; authenticated-store-required; below-floor = distinct `ROLLBACK_BLOCKED` refusal, not least-bad). No enforcement code; the per-slot `rollback_index`/`rollback_floor` fields exist from §1.
> - **Why blocked:** real anti-rollback needs an AUTHENTICATED monotonic floor store (CRC-metadata is corruption-detection, not authenticity) -- TODO-13 §27 on §21's index lifecycle, read pre-selection by §22; the floor advance additionally needs a verified slot identity -- TODO-19 §7. Shipping selection enforcement against an unauthenticated floor is false security + adds risk to the never-brick selection path for zero current benefit.
> - **Downstream effects:** when TODO-13 §21 (NV index lifecycle) + §27 (authenticated NV floor record) + §22 (loader-side floor read) + TODO-19 §7 (slot-verify) land, the `ROLLBACK_BLOCKED` selection enforcement + the verified advance ship here.
> - **Canonical doc:** `include/boot/ab_boot_metadata.h` (the `rollback_index`/`rollback_floor` fields) + this section's design-lock note.
> - **Scope boundary:** the GLOBAL OS-security-version floor is owned by `boot_rollback.c` (independent); §8 owns only the per-slot A/B floor enforcement; the floor STORE is TODO-13 §27 (record) on §21 (index lifecycle) + §22 (loader read); the verified-identity advance is TODO-19 §7; recovery routing is TODO-22.

> **Verified:** 2026-06-16 | design-only (no code) | 0/5 items | design corrected via Codex design review (1C+1H+1M adopted) | enforcement blocked on authenticated store
> **Deferred:** [Critical] Below-floor selection enforcement (distinct `ROLLBACK_BLOCKED` refusal, never least-bad) + the authenticated monotonic floor store it requires -> XREF: 01-boot-platform/TODO-13 §22 (item: "Read the floor BEFORE `select_active_slot()` decides") (RETARGETED 2026-08-17: the owning item moved TODO-13 §7 -> §17 when that cohort was split, then §17 -> §21/§22 when §17 was split again. §17 now ships only the NV wire primitives; the authenticated floor RECORD is §21 and the pre-selection loader READ this item actually waits on is §22)
> **Deferred:** [M] Floor advance gated on verified slot identity -> XREF: 02-kernel-core/TODO-19 §7 (item: "Expose a slot-boot-verification query ... so A/B mark-good + the anti-rollback floor refuse to bless an unverified slot")

---

## OS Comparison

| ⭐  | Feature                  | 🪟 Win11                  | 🐧 Linux                 | 🚀 Impossible OS            |
| --- | ------------------------ | ------------------------- | ------------------------ | --------------------------- |
| 💎  | Dual-slot boot           | ⚠️ Automatic Repair only  | ⚠️ systemd-boot assess   | 🟦 §1-§3 select+mount       |
| 💎  | Boot failure counting    | ✅ 2-attempt detection    | ✅ systemd tries counter | 🟦 §4 pre-EBS tries++       |
| 💎  | Automatic rollback       | ⚠️ Manual repair needed   | ⚠️ Manual or auto        | 🟦 §3+§4+§5 round-trip      |
| 💎  | Mark boot successful     | ✅ Implicit (desktop OK)  | ✅ systemd boot-complete | 🟦 §5 first-frame mark-good |
| 💎  | Metadata integrity (CRC) | ⚠️ BCD, no A/B redundancy | ✅ Android CRC-32 dual   | ✅ CRC-32 dual-copy atomic  |
| 💎  | Anti-rollback floor      | ⚠️ WU rollback window     | ✅ Android stored index  | 🟦 §8 design-lock (TPM-NV)  |
| ⭐  | Slot status in boot UI   | ❌ Hidden                 | ❌ journalctl only       | ✅ VPD per-slot + rollback  |

After §1-§5, Impossible OS has stronger rollback than Windows (which requires manual Automatic Repair) and matches Chrome OS/Android A/B. §6 makes slot status visible during boot.

---

## Unit Tests

> Boot metadata and slot selection run in UEFI bootloader context -- use smoke tests.
> Kernel-side `mark_boot_successful()` logic can be validated via kernel unit tests.

- [x] `src/kernel/test/test_ab_boot.c` shipped (38 `ab_boot:` cases across §1/§3/§6) -- struct validate/default/CRC, generation select-newest, choose_slot (fresh/rollback/once-successful-exhausted/priority/tie-breaks/both-exhausted), GPT reconcile, gpt_ixfs_slot, write_target/next_generation ceiling, decide NORMAL/ROLLBACK/priority/both-exhausted. Bootloader `tries`-increment + kernel `mark_boot_successful` have no RAM-blkdev fixture (smoke-validated round-trip). The §8 below-floor/advance cases ship with §8's deferred enforcement.
- [x] Registered in `test_runner_init()`: `test_register_ab_boot()` (`test_runner.c:378`/`:471`)
- [ ] E2E `scripts/test-boot-rollback.sh` (build dual-slot, corrupt slot A, boot 4x, assert `"rolling back to Slot B"`) -- DEFERRED follow-up test infra: the rollback LOGIC is unit-tested (`decide`/`choose_slot` rollback cases) + the increment->mark-good round-trip is smoke-validated; the multi-boot-with-corruption harness is a standalone E2E task.
- [x] Commit: `"test: add A/B boot rollback test suite"` -- suite shipped incrementally across the §1/§3/§6 section commits (38 cases)

---

## Verification

- [x] **Normal boot**: Slot A boots, `mark_boot_successful` logged -- smoke-validated (`A/B: boot marked successful (slot 0, copy 1, gen 2)`; tries 0->1->0 round-trip).
- [ ] **Rollback test**: corrupt Slot A, boot 3x -> Slot B (manual/E2E -- needs the deferred `test-boot-rollback.sh` harness; the `decide`/`choose_slot` rollback path is unit-tested).
- [ ] **Both slots bad**: corrupt both -> recovery (blocked: recovery partition -> TODO-22; the both-exhausted least-bad stopgap + §8 `ROLLBACK_BLOCKED` refusal are unit-tested/design-locked).
- [ ] **Update simulation**: write Slot B, mark pending, reboot -> Slot B (blocked: update engine -> 10-platform-services/TODO-03; the slot-select + mount path is shipped).
- [x] Commit: `"boot: A/B dual-slot boot complete -- automatic rollback, never unbootable"`

**Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) for `test_ab_boot.c` + `scripts/test-boot-rollback.sh` (rollback smoke) | suites TBD until §1-§5 land
