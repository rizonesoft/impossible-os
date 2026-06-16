---
schema_version: 1
id: recovery-partition
domain: 01-boot-platform
status: active
title: "TODO-22 -- Recovery Partition & Self-Repair"
---

# TODO-22 -- Recovery Partition & Self-Repair

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

| ⭐  | Order | Deliverable                                   | Depends On       | Status |
| --- | :---: | --------------------------------------------- | ---------------- | :----: |
| 💎  |   1   | Recovery partition in disk layout              | --                |  [/]   |
| 💎  |   2   | Recovery bootloader (minimal UEFI app)         | §1               |  [ ]   |
| 💎  |   3   | Filesystem integrity check (IXFS fsck)         | §2               |  [ ]   |
| 💎  |   4   | Backup kernel restore                          | §2               |  [ ]   |
| 💎  |   5   | Boot metadata reset                            | §2, T21 §1       |  [ ]   |
| 💎  |   6   | NVRAM boot entry reconstruction                | §2               |  [ ]   |
| ⭐  |   7   | Recovery UI with status display                | §2–§6            |  [ ]   |

> 💎 = parity -- Windows WinRE and Chrome OS recovery both provide these.
> ⭐ = exclusive -- clear status display during recovery with step-by-step progress.

---

## 1. Recovery Partition in Disk Layout

Add a read-only recovery partition to the GPT disk layout.

- [x] GPT layout (make-system-disk `--ab`): EFI + BlackBox + ABMeta + Slot A + Slot B + Recovery (34 MiB, FAT32-floor-safe); slots split the space before Recovery (768M default -> ~270 MiB each, >= 96 MiB floor)
- [x] Recovery type GUID `49504F53-7265-636F-7665-727900000001` ("IPOSrecovery") in the make-system-disk GPT writer (distinct from ESP/IXFS/ABMeta); the `bootcfg.py` placeholder + seed are -> XREF: [`TODO-07 §16`](TODO-07-boot-entry-store-menu-policy.md#16-bootstrap-and-first-install-entry-seeding).
- [/] Contents: `kernel.bak` (last known-good kernel) populated now via `mcopy`; `recovery.exe` (the §2 recovery bootloader) + `ixfs-fsck` (§3) deferred to those sections
- [x] Recovery GPT entry marked read-only (attributes field offset 48, bit 60 -- Microsoft basic-data read-only convention)
- [x] Build creates + FAT32-formats the recovery partition from the `.info` `RECOVERY_OFFSET`/`RECOVERY_SIZE` + populates `kernel.bak` (Makefile system-disk recipe)
- [ ] First-boot self-seed (missing store + recovery + known-good slot -> synthesize 3-entry default) -> XREF: [`TODO-07 §16`](TODO-07-boot-entry-store-menu-policy.md#16-bootstrap-and-first-install-entry-seeding).
- [x] Commit: `"build: add recovery partition to GPT disk layout"`

**Test checkpoint:** `fdisk -l` shows the Recovery partition (6-partition A/B layout: EFI + BlackBox + ABMeta + Slot A + Slot B + Recovery); make-system-disk prints "Part 6 (Recovery) ... read-only"; the 6-partition image boots (smoke PASS).

> **Test runner:** N/A (host disk-image tool; no kernel test surface) | validation: `make system-disk` prints the 6-partition layout + "Part 6 (Recovery) ... read-only"; FAT32-floor guard rejects an under-34-MiB recovery; QEMU smoke PASS (the 6-partition image boots, A/B select unregressed); bare metal

> **Notes:**
> - **What shipped:** the Recovery GPT partition (34 MiB read-only) in `tools/make-system-disk.c` `--ab` layout (entry 5, type GUID 49504F53-..., attribute bit 60) + `RECOVERY_OFFSET`/`RECOVERY_SIZE` `.info` sidecar + Makefile FAT32 format + `kernel.bak` populate.
> - **How it runs:** carved at the disk end (Slot A/B split the space before it); the build formats it FAT32 from the `.info` offsets and copies the built kernel as `kernel.bak`; the 6-partition image boots unregressed (smoke).
> - **Design adoptions:** 34 MiB reserve (not 32) so the FAT32 volume clears the 65525 data-cluster floor that UEFI/OVMF requires; a layout-validation guard fails closed if the realized recovery is under that floor (review-caught). Evidence in commit message.
> - **Downstream effects:** the recovery slot is the target of TODO-21's both-slots-bad / ROLLBACK_BLOCKED recovery routing; §2-§7 build the recovery bootloader + fsck + restore + UI on top.
> - **Canonical doc:** `tools/make-system-disk.c` (the `--ab` GPT layout) + the `<img>.info` `RECOVERY_OFFSET`/`SIZE` contract.
> - **Scope boundary:** §1 ships the partition + read-only attr + `kernel.bak`; `recovery.exe` is §2, `ixfs-fsck` is §3, the first-boot self-seed is TODO-07 §16.

---

## 2. Recovery Bootloader

Minimal UEFI application that boots the recovery kernel.

- [ ] Separate UEFI app: `recovery.efi` -- stripped down version of `bootx64.c`
- [ ] Installed at both `\EFI\ImpossibleOS\recovery.efi` AND `\EFI\BOOT\BOOTx64.EFI` (UEFI fallback path)
- [ ] Loads `recovery.exe` from recovery partition
- [ ] Publish recovery-image descriptor and shared `boot_path=recovery` / reason codes through TODO-01 §4 and §12 so the kernel and diagnostics can distinguish recovery boot from a normal cold boot. Use `BOOT_PAYLOAD_RECOVERY_IMAGE` from [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h); the TODO-01 §4 validator retains the region through the overlap + packed-prefix check before recovery.efi's kernel dereferences the image. -> XREF: [`01-boot-platform/TODO-01 §4`](../01-boot-platform/TODO-01-boot-protocol-abi-handoff.md#4-optional-payload-descriptor-array)
- [ ] Shows: `"Impossible OS Recovery Environment"` on screen
- [ ] Does NOT touch A/B slots -- operates only on recovery partition
- [ ] Commit: `"boot: recovery bootloader -- minimal UEFI app at fallback path"`

**Test checkpoint:** Delete normal UEFI boot entry from NVRAM. System falls back to `\EFI\BOOT\BOOTx64.EFI` → recovery environment loads.

---

## 3. Filesystem Integrity Check

Recovery can verify and repair IXFS filesystem on both slots.

- [ ] `ixfs_fsck()` -- verify superblock, inode table, free bitmap, directory tree
- [ ] Report: corrupted inodes, orphan blocks, bad journal entries
- [ ] Auto-repair: fix bitmap inconsistencies, unlink orphan inodes, replay clean journal
- [ ] Log results to serial and display on screen
- [ ] Commit: `"recovery: IXFS filesystem integrity check and auto-repair"`

**Test checkpoint:** Corrupt an IXFS inode bitmap. Recovery fsck detects and repairs it.

---

## 4. Backup Kernel Restore

Restore a known-good kernel from recovery partition to the active slot.

- [ ] Recovery partition contains `kernel.bak` -- copy of the last verified working kernel
- [ ] `restore_kernel(slot)` -- copies `kernel.bak` to `\boot\kernel.exe` on target slot
- [ ] After restore: reset boot metadata for target slot (tries=0, successful=0)
- [ ] Update `kernel.bak` whenever `mark_boot_successful()` fires (→ XREF: TODO-21 §5)
- [ ] Commit: `"recovery: restore backup kernel to slot -- last known-good version"`

**Test checkpoint:** Corrupt Slot A kernel. Enter recovery. Restore backup. Reboot → Slot A boots successfully.

---

## 5. Boot Metadata Reset

Recovery can reset A/B boot metadata to a clean state.

- [ ] Reset active slot to A, tries=0, successful=1
- [ ] Reset inactive slot to tries=0, successful=0
- [ ] Clear any pending update flags
- [ ] Commit: `"recovery: boot metadata reset -- clean A/B state"`

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

---

## 7. Recovery UI

User-visible recovery interface with clear status.

- [ ] Text-mode UI showing: `"Impossible OS Recovery"`, slot status, available actions
- [ ] Actions: `[1] Repair filesystem`, `[2] Restore backup kernel`, `[3] Reset boot metadata`, `[4] Rebuild NVRAM`, `[5] Reboot`
- [ ] Progress display for each action
- [ ] Requires USB keyboard (→ XREF: TODO-18) or serial input
- [ ] Commit: `"recovery: text-mode recovery UI with repair actions"`

**Test checkpoint:** Enter recovery → menu displayed → select "Repair filesystem" → fsck runs with progress → reboot option.

---

## OS Comparison

| ⭐ | Feature                    | 🪟 Win11                   | 🐧 Linux                   | 🚀 Impossible OS             |
|----|----------------------------|-------------------------|-------------------------|---------------------------|
| 💎 | Recovery partition         | ✅ WinRE partition      | ⚠️ Optional initramfs   | 🟦 §1 34M read-only FAT32 |
| 💎 | Filesystem repair          | ✅ chkdsk in WinRE      | ✅ fsck in initramfs    | ⬜ §3                     |
| 💎 | Kernel backup restore      | ✅ System Restore       | ⚠️ Manual from LiveCD   | ⬜ §4                     |
| 💎 | NVRAM reconstruction       | ✅ bootrec /rebuildbcd  | ✅ fallback.efi         | ⬜ §6                     |
| ⭐ | Clear recovery UI          | ⚠️ Blue screen menus   | ❌ CLI only             | ⬜ §7 🚀                  |

---

## Unit Tests

> Recovery environment runs as a separate UEFI app -- use boot-level smoke tests.
> IXFS fsck logic can be validated via kernel unit tests.

- [ ] Create `src/kernel/test/test_ixfs_fsck.c` with:
  - Clean IXFS superblock passes validation (no corruption detected)
  - Corrupted inode bitmap detected: flip 1 bit in bitmap, `ixfs_fsck()` reports bitmap inconsistency
  - Orphan inode detected: mark inode allocated but not in any directory, fsck reports orphan
  - Journal replay: write partial journal entry, fsck replays cleanly
  - Free block count mismatch: set wrong count in superblock, fsck detects and corrects
- [ ] Register in `test_runner_init()`: `test_register_ixfs_fsck()`
- [ ] Create `scripts/test-boot-recovery.sh`:
  - Build disk image with recovery partition (4 partitions in GPT)
  - Assert `fdisk -l` shows EFI + Slot A + Slot B + Recovery partitions
  - Corrupt both slot A and slot B kernels
  - Boot QEMU headless, capture serial
  - Assert serial contains `"Impossible OS Recovery"` (§2 -- recovery bootloader activates)
- [ ] Commit: `"test: add IXFS fsck and recovery partition smoke tests"`

## Verification

- [ ] **Both slots bad**: corrupt both → recovery activates automatically.
- [ ] **NVRAM wiped**: clear all boot entries → fallback.efi rebuilds them → normal boot.
- [ ] **Filesystem corrupt**: damage IXFS bitmap → recovery fsck repairs → boot succeeds.
- [ ] **Normal boot regression**: recovery partition present but not used during normal boot.
- [ ] Commit: `"boot: recovery partition complete -- self-repair without external media"`
