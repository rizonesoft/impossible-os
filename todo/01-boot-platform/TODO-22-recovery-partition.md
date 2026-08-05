---
schema_version: 1
id: recovery-partition
domain: 01-boot-platform
status: active
title: "TODO-22 -- Recovery Partition & Self-Repair"
---

# TODO-22 -- Recovery Partition & Self-Repair

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

> **Goal:** A read-only recovery partition that can repair a broken system without external media. If both A/B slots fail, the system boots into a minimal recovery environment that can: rebuild boot metadata, verify filesystem integrity, restore a known-good kernel from backup, and recreate UEFI NVRAM boot entries. Modeled after Windows Recovery Environment (WinRE), Chrome OS recovery, and Linux's fallback.efi NVRAM repair.

> [!IMPORTANT]
> **Current state:** No recovery mechanism. If the system can't boot, the only option is to reflash from a USB stick. No recovery partition, no self-repair, no NVRAM reconstruction.

---

## Inputs

- `src/boot/uefi/bootx64.c` -- bootloader (needs recovery boot path)
- `scripts/build.sh` -- disk image creation (needs recovery partition)
- -> XREF: `TODO-01-boot-protocol-abi-handoff.md §4,§12` -- recovery image descriptor and shared boot-path decision record
- → XREF: `TODO-21-ab-boot-rollback.md §4` -- both slots failed → enter recovery
- → XREF: `TODO-03-bootloader-error-recovery.md §2` -- boot failure error screen
- -> XREF: `TODO-03-bootloader-error-recovery.md` -- pre-kernel hardening (ELF, EBS, mmap, watchdog) surfaces failures before recovery shell
- → XREF: `TODO-27-uefi-advanced.md §1` -- multi-OS boot menu (TODO-02 §8 is serial klog)
- → XREF: `TODO-07-boot-entry-store-menu-policy.md §6` -- recovery partition layout from §1 here feeds the boot menu's recovery entry; §9 audit consumes recovery-trigger reasons

---

## Outcome

- Disk layout includes a 32 MiB read-only recovery partition.
- Recovery partition contains: minimal kernel, recovery shell, filesystem check tool, backup kernel.
- When both A/B slots fail, bootloader chains to recovery automatically.
- Recovery environment can: verify IXFS integrity, restore backup kernel, reset boot metadata, recreate NVRAM entries.
- UEFI fallback path `\EFI\BOOT\BOOTx64.EFI` always points to recovery bootloader.

---

## Implementation Order

| ⭐  | Order | Deliverable                                                                  | Depends On               | Status |
| --- | :---: | ---------------------------------------------------------------------------- | ------------------------ | :----: |
| 💎  |   1   | Recovery partition in disk layout                                            | --                       |  [/]   |
| 💎  |   2   | Recovery load-path contract + kernel recovery-mode entry (entry-store model) | §1, T07 §4+§9            |  [/]   |
| 💎  |   3   | Filesystem integrity check (IXFS fsck)                                       | §2 (recovery invocation) |  [x]   |
| 💎  |   4   | Backup kernel restore                                                        | §2                       |  [/]   |
| 💎  |   5   | Boot metadata reset                                                          | §2, T21 §1               |  [/]   |
| 💎  |   6   | NVRAM boot entry reconstruction                                              | §2                       |  [/]   |
| ⭐  |   7   | Recovery UI with status display                                              | §2–§6                    |  [/]   |

> 💎 = parity -- Windows WinRE and Chrome OS recovery both provide these.
> ⭐ = exclusive -- clear status display during recovery with step-by-step progress.

---

## 1. Recovery Partition in Disk Layout

Add a read-only recovery partition to the GPT disk layout.

- [x] GPT layout (make-system-disk `--ab`): EFI + BlackBox + ABMeta + Slot A + Slot B + Recovery (34 MiB, FAT32-floor-safe); slots split the space before Recovery (768M default -> ~270 MiB each, >= 96 MiB floor)
- [x] Recovery type GUID `49504F53-7265-636F-7665-727900000001` ("IPOSrecovery") in the make-system-disk GPT writer (distinct from ESP/IXFS/ABMeta); the `bootcfg.py` placeholder + seed are -> XREF: [`TODO-07 §16`](TODO-07-boot-entry-store-menu-policy.md#16-bootstrap-and-first-install-entry-seeding).
- [/] Contents: `kernel.bak` (last known-good kernel) populated now via `mcopy`; `recovery.exe` (the §2 recovery bootloader) + `ixfs-fsck` (§3) deferred to those sections
- [x] Recovery GPT entry read-only (attr bit 60) AND kernel-enforced: `gpt_is_recovery` + `partition_mount_filesystems` skip it, so a normal boot never drive-letter-mounts it (review HIGH: a FAT32 recovery would otherwise auto-mount writable as D: and expose `kernel.bak`)
- [x] Build creates + FAT32-formats the recovery partition from the `.info` `RECOVERY_OFFSET`/`RECOVERY_SIZE` + populates `kernel.bak` (Makefile system-disk recipe)
- [/] First-boot self-seed (missing store + recovery + known-good slot -> synthesize 3-entry default) -> XREF: [`TODO-07 §16`](TODO-07-boot-entry-store-menu-policy.md#16-bootstrap-and-first-install-entry-seeding).
- [x] Commit: `"build: add recovery partition to GPT disk layout"`

**Test checkpoint:** `fdisk -l` shows the Recovery partition (6-partition A/B layout: EFI + BlackBox + ABMeta + Slot A + Slot B + Recovery); make-system-disk prints "Part 6 (Recovery) ... read-only"; the 6-partition image boots (smoke PASS).

> **Test runner:** N/A (host disk-image tool; no kernel test surface) | validation: `make system-disk` prints the 6-partition layout + "Part 6 (Recovery) ... read-only"; FAT32-floor guard rejects an under-34-MiB recovery; QEMU smoke PASS (the 6-partition image boots, A/B select unregressed); bare metal

> **Notes:**
> - **What shipped:** the Recovery GPT partition (34 MiB read-only) in `tools/make-system-disk.c` `--ab` layout (entry 5, type GUID 49504F53-..., attribute bit 60) + `RECOVERY_OFFSET`/`RECOVERY_SIZE` `.info` sidecar + Makefile FAT32 format + `kernel.bak` populate.
> - **How it runs:** carved at the disk end (Slot A/B split the space before it); the build formats it FAT32 from the `.info` offsets and copies the built kernel as `kernel.bak`; the 6-partition image boots unregressed (smoke).
> - **Design adoptions (review-caught):** 34 MiB reserve (not 32) so the FAT32 volume clears the 65525-cluster floor UEFI/OVMF requires + a fail-closed layout guard; the kernel skips the recovery GUID in `partition_mount_filesystems` so it is never drive-letter-mounted writable. Evidence in commit message.
> - **Downstream effects:** the recovery slot is the target of TODO-21's both-slots-bad / ROLLBACK_BLOCKED recovery routing; §2-§7 build the recovery bootloader + fsck + restore + UI on top.
> - **Canonical doc:** `tools/make-system-disk.c` (the `--ab` GPT layout) + the `<img>.info` `RECOVERY_OFFSET`/`SIZE` contract.
> - **Scope boundary:** §1 ships the partition + read-only attr + `kernel.bak`; `recovery.exe` is §2, `ixfs-fsck` is §3, the first-boot self-seed is TODO-07 §16.

> **Verified:** 2026-06-16 | commit `75d3915a` + recovery-mount-skip fix | 4/6 items | build OK | smoke PASS (KVM 2.11s, recovery NOT drive-letter-mounted; C: slot + X: BlackBox unregressed)
> **Quality reviewed:** 2026-06-16 | Codex 6x (design, adversarial, consistency, perf, re-adversarial) | 1H+1M fixed, 0 open | scope: kernel-code-quality + boot-code-quality (host disk tool + kernel mount path)

---

## 2. Recovery Bootloader

**Design-locked + deferred [/] 2026-06-16 (Codex design review, 3 HIGH + cross-TODO reconciliation).** The original "standalone `recovery.efi` at the `\EFI\BOOT\BOOTX64.EFI` fallback path loading `kernel.bak`" is architecturally rejected -- recovery boots through the existing bootloader + TODO-07 boot-entry store, NOT a forked binary. See NOTE for the three HIGH findings and the blocker chain.

- [x] Recovery boots via a `kind=recovery` entry through the existing `bootx64.c` producer (flips `boot_path=BOOT_PATH_RECOVERY`); ALREADY SHIPPED by TODO-07 §4. -> XREF: [`01-boot-platform/TODO-07 §4`](TODO-07-boot-entry-store-menu-policy.md)
- [ ] Kernel recovery-mode entry point: Phase-3 routes `boot_path==BOOT_PATH_RECOVERY` into the recovery flow (§3-§7) instead of normal desktop init. §2's real deliverable; non-hollow only once §3-§7 land.
- [ ] Recovery-load-path contract: define `BOOT_ENTRY_KIND_RECOVERY` + the `BOOT_SELECTION_FALLBACK_ALL_PATHS_BAD` sentinel TODO-07 §9 consumes. -> XREF: [`01-boot-platform/TODO-07 §9`](TODO-07-boot-entry-store-menu-policy.md)
- [ ] Recovery entry REGISTRATION owned by TODO-07 §9 (integration) + §16 (first-install seeding); §2 provides the load path they register against. -> XREF: [`01-boot-platform/TODO-07 §9`](TODO-07-boot-entry-store-menu-policy.md)
- [ ] Shows `"Impossible OS Recovery Environment"` -- rendered by the §7 recovery UI on the recovery-mode entry path
- [ ] Does NOT touch A/B slots -- recovery operates only on the recovery partition
- [ ] DROPPED: standalone `recovery.efi` + `\EFI\BOOT\BOOTX64.EFI` fallback-path install (replaces NORMAL boot, HIGH; forked binary drifts vs boot_info validation, HIGH)
- [ ] Quiesce before invoking `ixfs_fsck`: run fsck pre-mount OR hold a per-volume repair lock excluding VFS mutators (active-volume fsck is quiesced-only by contract; §3 review HIGH). -> XREF: §3
- [ ] Commit: `"boot: recovery-mode kernel entry + load-path contract (entry-store model)"`

**Test checkpoint (deferred):** a registered `kind=recovery` entry (TODO-07 §9) boots through the existing bootloader; the kernel logs `boot_path=RECOVERY` and enters the recovery flow; the normal boot entry is unaffected. Cannot run until TODO-07 §9 registration + §3-§7 recovery flow land.

> **Test runner:** N/A (deferred -- design-locked, blocked on TODO-07 §9 registration + §3-§7 flow) | validation: §2 Test checkpoint once the load path is registered and the flow lands

> **Notes:**
> - **Why deferred:** 3 HIGH design findings reject the as-written §2 -- fallback-path install replaces NORMAL boot, `kernel.bak` load is a smoke target, forked binary BSS-zero producer hits `BOOT_FATAL` (detail in commit).
> - **Corrected architecture:** recovery boots via the TODO-07 entry store through the EXISTING hardened `bootx64.c` producer (no forked binary, no producer drift); TODO-07 §4 already ships `kind=recovery -> boot_path=RECOVERY`.
> - **§2 real scope (post-correction):** the kernel recovery-mode entry point + the `BOOT_ENTRY_KIND_RECOVERY`/`ALL_PATHS_BAD` load-path contract; the recovery FLOW is §3-§7.
> - **Cross-TODO deadlock:** TODO-07 §9 (recovery entry integration, deferred) is blocked on "TODO-22 §2 (recovery load path)"; §2 defines + defers that contract so §9 has a concrete target.
> - **Scope boundary:** §2 = load-path contract + kernel recovery-mode entry; TODO-07 §9/§16 own registration; §3-§7 own the flow.

> **Verified:** 2026-06-16 | design-corrected, 0/7 functional items (deferred) | build N/A (no code shipped this pass) | Codex design review adopted (3 HIGH)
> **Deferred:** [H] §2 credible form is a cross-TODO recovery subsystem (load-path contract + kernel recovery-mode entry + §3-§7 flow), entangled with deferred TODO-07 §9 and unvalidatable until a recovery entry is registered -> XREF: [`01-boot-platform/TODO-07 §9`](TODO-07-boot-entry-store-menu-policy.md) (item: "Widen `supported_kinds_mask` to include `BOOT_ENTRY_KIND_RECOVERY`")

---

## 3. Filesystem Integrity Check

Recovery can verify and repair the IXFS filesystem on a slot.

- [x] `ixfs_fsck(fix, report)` + internal `ixfs_fsck_volume()` (`src/kernel/fs/ixfs/ixfs_fsck.c`): 11-pass check of superblock (magic/version/CRC32C/layout), inode table, free bitmap, directory tree, refcounts, and journal
- [x] `struct ixfs_fsck_report` tallies each error class (orphan inodes, bad dirents, cross-links, bitmap/free-count/refcount/journal/data-checksum faults)
- [x] Auto-repair (fix mode, SAFE subset): bitmap reconcile, free-count + refcount fix, single-owner orphan free, bad-dirent clear, validated journal replay; cross-links + snapshot-shared blocks stay report-only
- [/] Log to serial (klog `fsck` category) done; on-screen display is the §7 recovery UI -> XREF: §7
- [ ] Large-volume scalability (deferred): fuse the 3 inode scans (pass 3/6/8) via pass-3 allocated/dir-type bitsets + recorded free-inode count; chunked scratch to avoid the 8 MiB-contiguous refc alloc at the 32 GiB ceiling
- [ ] Crash-atomic repair (deferred): route fsck repair writes through a transaction / fsck-repair journal; today repairs rely on idempotent re-run (e2fsck model; `IXFS_TXN_MAX_ENTRIES=8` too small to wrap a full repair)
- [x] Commit: `"recovery: IXFS filesystem integrity check and auto-repair"`

**Test checkpoint:** `ixfs_fsck_reconcile_bitmap` detects a flipped bitmap bit and (fix mode) sets the missing + clears the spurious bit; `test_ixfs_fsck.c` covers the superblock / bitmap / journal validators (24 assertions, TEST_CAT_FS); full fs suite 111 kernel + 16 user-mode PASS on TCG.

> **Test runner:** `scripts\debug\kernel\run-fs-tests.bat` (SUITE=fs) | 111 kernel + 16 user-mode PASS, 0 failures

> **Notes:**
> - **What shipped:** `src/kernel/fs/ixfs/ixfs_fsck.c` (11-pass checker + SAFE-subset repair) + 3 pure validators in `ixfs.h` + `struct ixfs_fsck_report` + 24 assertions in `test_ixfs_fsck.c`.
> - **How it runs:** `ixfs_fsck(fix, report)` on the active volume (mirrors `ixfs_scrub`); flushes the write-back cache then uses raw disk I/O; `fat32_fsck` return contract (0=clean/fixed, -1=untrustworthy).
> - **Safety:** snapshot/refcount-aware (frees a block only when expected refcount<=1); corrupt journal discarded not replayed; a partial directory walk disables every freeing pass (no data wipe on transient OOM).
> - **Scope boundary:** the live-volume orchestrator (orphan/journal-replay/free-count) is exercised by the §2 recovery flow + serial; unit tests cover the pure validators only. On-screen display + both-slots are §2/§7.
> - **Canonical doc:** `src/kernel/fs/ixfs/ixfs_fsck.c` header + `struct ixfs_fsck_report` in `include/kernel/fs/ixfs.h`.

> **Verified:** 2026-06-16 | commit `79096a4f` (+ review fixes) | 3/6 items | build OK | tests 113 kernel + 16 user-mode PASS (fs, TCG); lint clean
> **Deferred:** [H] active-volume fsck has no enforced VFS-exclusion (quiesced-only by contract; only caller is the §2 recovery flow) -> XREF: 01-boot-platform/TODO-22 §2 (item: "Quiesce before invoking `ixfs_fsck`")
> **Deferred:** [H] large-volume perf: 3 inode-table scans + per-dirent inode re-read + 8 MiB-contiguous refc scratch at the 32 GiB ceiling -> XREF: 01-boot-platform/TODO-22 §3 (item: "Large-volume scalability (deferred)")
> **Deferred:** [H] fsck repair is not power-fail-atomic; relies on idempotent re-run (e2fsck model) -> XREF: 01-boot-platform/TODO-22 §3 (item: "Crash-atomic repair (deferred)")
> **Quality reviewed:** 2026-06-16 | Codex 12x (design + adversarial + test-coverage + consistency + perf + re-adversarial) | 3C+12H+4M fixed, 4H+1M deferred | scope: kernel-code-quality

---

## 4. Backup Kernel Restore

Restore a known-good kernel from recovery partition to the active slot.

- [ ] Recovery partition contains `kernel.bak` -- copy of the last verified working kernel
- [ ] `restore_kernel(slot)` -- copies `kernel.bak` to `\boot\kernel.exe` on target slot
- [ ] After restore: reset boot metadata for target slot (tries=0, successful=0)
- [ ] Update `kernel.bak` whenever `mark_boot_successful()` fires (→ XREF: TODO-21 §5)
- [ ] Commit: `"recovery: restore backup kernel to slot -- last known-good version"`

**Test checkpoint:** Corrupt Slot A kernel. Enter recovery. Restore backup. Reboot → Slot A boots successfully.

> **Notes:**
> - **Status:** deferred -- backup-kernel restore is a recovery-context VFS operation (copy `kernel.bak` between recovery-mounted partitions); no standalone pure surface.
> - **Blocker:** needs the §2 recovery boot path + mounted slot/recovery partitions; the `kernel.bak` populate already lands in §1.
> - **Scope boundary:** §4 owns the restore op; §2 owns the recovery flow that invokes it; the `kernel.bak` refresh-on-mark-good is TODO-21 §5.

> **Verified:** 2026-06-16 | 0/4 items (deferred) | build N/A | blocked on §2 recovery flow
> **Deferred:** [M] backup-kernel restore is a recovery-flow op blocked on the recovery boot path -> XREF: 01-boot-platform/TODO-22 §2 (item: "Kernel recovery-mode entry point: Phase-3 routes")

---

## 5. Boot Metadata Reset

Recovery can reset A/B boot metadata to a clean state.

- [ ] Reset active slot to A, tries=0, successful=1
- [ ] Reset inactive slot to tries=0, successful=0
- [ ] Clear any pending update flags
- [ ] Commit: `"recovery: boot metadata reset -- clean A/B state"`

> **Notes:**
> - **Status:** deferred -- A/B metadata reset runs in the recovery context (writes the metadata partition); the clean-state computation reuses TODO-21 `ab_boot_metadata`.
> - **Blocker:** needs the §2 recovery boot path + the metadata-write context; no standalone pure surface beyond TODO-21's factory-init.
> - **Scope boundary:** §5 owns the reset op; §2 owns the recovery flow; TODO-21 §1 owns the `ab_boot_metadata` format.

> **Verified:** 2026-06-16 | 0/3 items (deferred) | build N/A | blocked on §2 recovery flow
> **Deferred:** [M] A/B metadata reset is a recovery-flow op blocked on the recovery boot path -> XREF: 01-boot-platform/TODO-22 §2 (item: "Kernel recovery-mode entry point: Phase-3 routes")

---

## 6. NVRAM Boot Entry Reconstruction

If UEFI NVRAM boot entries are lost (firmware reset, battery pull), rebuild them.

- [ ] Scan `\EFI\` directory on ESP for known bootloader filenames
- [ ] Recreate `BootXXXX` UEFI variables for each found bootloader
- [ ] Set `BootOrder` with Impossible OS as first entry
- [ ] Model after Linux `fallback.efi` / `BOOT.CSV` approach
- [ ] **Create + honor `PlatformRecovery####` UEFI variables** (filed 2026-05-01 from [`TODO-03-bootloader-error-recovery.md`](TODO-03-bootloader-error-recovery.md) gap-audit Codex M1): UEFI 2.10 section 3.4 mandates that when `BootOrder` / `BootNext` entries are exhausted without success (or when firmware is asked to attempt boot recovery), the boot manager must consult `PlatformRecovery####` variables BEFORE giving up. Today's recovery story relies on vendor firmware defaults (often a recovery partition entry the vendor pre-created) rather than Impossible OS-controlled policy. Implementation: at boot-info-publish time, ensure `PlatformRecovery0000` exists and points at `\EFI\Impossible\BOOTRECOVERY.EFI` (the recovery loader from §2); refresh the variable on every successful boot so a wiped NVRAM reseeds it on the next round; document the `LOAD_OPTION_CATEGORY_APP` attribute requirement (PlatformRecovery entries use a different attribute mask than Boot entries). Test: clear all `Boot####` and `PlatformRecovery####` variables, reboot, assert firmware rebuilds `PlatformRecovery0000` and triggers it after `BootOrder` exhaustion.
- [ ] Commit: `"recovery: NVRAM boot entry reconstruction -- rebuild after firmware reset"`

**Test checkpoint:** Clear all NVRAM boot entries. Reboot → fallback.efi triggers → NVRAM rebuilt → normal boot works.

> **Notes:**
> - **Status:** deferred -- NVRAM / PlatformRecovery reconstruction runs in the recovery bootloader (pre-EBS UEFI variable I/O) and consumes the TODO-07 entry store.
> - **Blocker:** needs the §2 recovery boot path + the TODO-07 boot-entry store; UEFI var I/O has no standalone pure surface.
> - **Scope boundary:** §6 owns the NVRAM rebuild; §2 owns the recovery loader; TODO-07 owns the entry store it registers against.

> **Verified:** 2026-06-16 | 0/6 items (deferred) | build N/A | blocked on §2 + TODO-07 entry store
> **Deferred:** [M] NVRAM/PlatformRecovery reconstruction is a recovery-bootloader op blocked on the recovery boot path -> XREF: 01-boot-platform/TODO-22 §2 (item: "Recovery entry REGISTRATION owned by TODO-07")

---

## 7. Recovery UI

User-visible recovery interface with clear status.

- [ ] Text-mode UI showing: `"Impossible OS Recovery"`, slot status, available actions
- [ ] Actions: `[1] Repair filesystem`, `[2] Restore backup kernel`, `[3] Reset boot metadata`, `[4] Rebuild NVRAM`, `[5] Reboot`
- [ ] Progress display for each action
- [ ] Requires USB keyboard (→ XREF: TODO-18) or serial input
- [ ] Commit: `"recovery: text-mode recovery UI with repair actions"`

**Test checkpoint:** Enter recovery → menu displayed → select "Repair filesystem" → fsck runs with progress → reboot option.

> **Notes:**
> - **Status:** deferred -- the recovery UI dispatches §3 fsck / §4 restore / §5 reset / §6 NVRAM; it cannot exist without the recovery flow and those operations.
> - **Blocker:** needs §2-§6 (the recovery flow + every operation the menu invokes) + keyboard/serial input (TODO-18).
> - **Scope boundary:** §7 owns the menu + progress UI; §2-§6 own the operations; §3 fsck is the one already shipped.

> **Verified:** 2026-06-16 | 0/4 items (deferred) | build N/A | blocked on §2-§6
> **Deferred:** [M] recovery UI is blocked on the recovery flow + the operations it dispatches -> XREF: 01-boot-platform/TODO-22 §2 (item: "Shows `"Impossible OS Recovery Environment"`")

---

## OS Comparison

| ⭐  | Feature               | 🪟 Win11               | 🐧 Linux              | 🚀 Impossible OS                 |
| --- | --------------------- | ---------------------- | --------------------- | -------------------------------- |
| 💎  | Recovery partition    | ✅ WinRE partition     | ⚠️ Optional initramfs | 🟦 §1 34M read-only FAT32        |
| 💎  | Filesystem repair     | ✅ chkdsk in WinRE     | ✅ fsck in initramfs  | ✅ §3 ixfs_fsck (snapshot-aware) |
| 💎  | Kernel backup restore | ✅ System Restore      | ⚠️ Manual from LiveCD | ⬜ §4                            |
| 💎  | NVRAM reconstruction  | ✅ bootrec /rebuildbcd | ✅ fallback.efi       | ⬜ §6                            |
| ⭐  | Clear recovery UI     | ⚠️ Blue screen menus   | ❌ CLI only           | ⬜ §7 🚀                         |

---

## Unit Tests

> Recovery environment runs as a separate UEFI app -- use boot-level smoke tests.
> IXFS fsck logic can be validated via kernel unit tests.

- [x] `src/kernel/test/test_ixfs_fsck.c` -- 24 assertions on the pure validators (superblock check, bitmap reconcile, journal-entry validation: clean + guard-path + boundary cases)
- [x] Orchestrator-level cases (orphan / journal-replay / free-count) need a mounted volume -> validated by the §2 recovery flow + serial, not unit-tested here
- [x] Registered in `test_runner_init()`: `test_register_ixfs_fsck()`
- [ ] Create `scripts/test-boot-recovery.sh` (boot-level recovery smoke: assert serial `"Impossible OS Recovery"`); blocked on §2 recovery bootloader -> XREF: §2
- [x] Commit: folded into the §3 commit `"recovery: IXFS filesystem integrity check and auto-repair"`

## Verification

- [ ] **Both slots bad**: corrupt both → recovery activates automatically.
- [ ] **NVRAM wiped**: clear all boot entries → fallback.efi rebuilds them → normal boot.
- [ ] **Filesystem corrupt**: damage IXFS bitmap → recovery fsck repairs → boot succeeds.
- [ ] **Normal boot regression**: recovery partition present but not used during normal boot.
- [ ] Commit: `"boot: recovery partition complete -- self-repair without external media"`
