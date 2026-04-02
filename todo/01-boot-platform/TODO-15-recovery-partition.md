# TODO-15 — Recovery Partition & Self-Repair

> **Goal:** A read-only recovery partition that can repair a broken system without external media. If both A/B slots fail, the system boots into a minimal recovery environment that can: rebuild boot metadata, verify filesystem integrity, restore a known-good kernel from backup, and recreate UEFI NVRAM boot entries. Modeled after Windows Recovery Environment (WinRE), Chrome OS recovery, and Linux's fallback.efi NVRAM repair.

> [!IMPORTANT]
> **Current state:** No recovery mechanism. If the system can't boot, the only option is to reflash from a USB stick. No recovery partition, no self-repair, no NVRAM reconstruction.

---

## Inputs

- `src/boot/uefi/bootx64.c` — bootloader (needs recovery boot path)
- `scripts/build.sh` — disk image creation (needs recovery partition)
- → XREF: `TODO-14-ab-boot-rollback.md §4` — both slots failed → enter recovery
- → XREF: `TODO-02-bootloader-error-recovery.md §9` — boot failure error screen
- → XREF: `TODO-01-uefi-hardening-secureboot.md §8` — multi-OS detection

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
| 💎  |   1   | Recovery partition in disk layout              | —                |  [ ]   |
| 💎  |   2   | Recovery bootloader (minimal UEFI app)         | §1               |  [ ]   |
| 💎  |   3   | Filesystem integrity check (IXFS fsck)         | §2               |  [ ]   |
| 💎  |   4   | Backup kernel restore                          | §2               |  [ ]   |
| 💎  |   5   | Boot metadata reset                            | §2, T14 §1       |  [ ]   |
| 💎  |   6   | NVRAM boot entry reconstruction                | §2               |  [ ]   |
| ⭐  |   7   | Recovery UI with status display                | §2–§6            |  [ ]   |

> 💎 = parity — Windows WinRE and Chrome OS recovery both provide these.
> ⭐ = exclusive — clear status display during recovery with step-by-step progress.

---

## 1. Recovery Partition in Disk Layout

Add a read-only recovery partition to the GPT disk layout.

- [ ] GPT layout: EFI (64 MiB) + Slot A (200 MiB) + Slot B (200 MiB) + Recovery (32 MiB)
- [ ] Recovery partition type GUID: custom `{IMPOSSIBLE-RECOVERY-PART}`
- [ ] Contents: `recovery.exe` (minimal kernel), `kernel.bak` (last known-good kernel), `ixfs-fsck` tool
- [ ] Partition marked read-only in GPT attributes
- [ ] `scripts/build.sh` creates recovery partition with contents
- [ ] Commit: `"build: add recovery partition to GPT disk layout"`

**Test checkpoint:** `fdisk -l` shows 4 partitions including Recovery.

---

## 2. Recovery Bootloader

Minimal UEFI application that boots the recovery kernel.

- [ ] Separate UEFI app: `recovery.efi` — stripped down version of `bootx64.c`
- [ ] Installed at both `\EFI\ImpossibleOS\recovery.efi` AND `\EFI\BOOT\BOOTx64.EFI` (UEFI fallback path)
- [ ] Loads `recovery.exe` from recovery partition
- [ ] Shows: `"Impossible OS Recovery Environment"` on screen
- [ ] Does NOT touch A/B slots — operates only on recovery partition
- [ ] Commit: `"boot: recovery bootloader — minimal UEFI app at fallback path"`

**Test checkpoint:** Delete normal UEFI boot entry from NVRAM. System falls back to `\EFI\BOOT\BOOTx64.EFI` → recovery environment loads.

---

## 3. Filesystem Integrity Check

Recovery can verify and repair IXFS filesystem on both slots.

- [ ] `ixfs_fsck()` — verify superblock, inode table, free bitmap, directory tree
- [ ] Report: corrupted inodes, orphan blocks, bad journal entries
- [ ] Auto-repair: fix bitmap inconsistencies, unlink orphan inodes, replay clean journal
- [ ] Log results to serial and display on screen
- [ ] Commit: `"recovery: IXFS filesystem integrity check and auto-repair"`

**Test checkpoint:** Corrupt an IXFS inode bitmap. Recovery fsck detects and repairs it.

---

## 4. Backup Kernel Restore

Restore a known-good kernel from recovery partition to the active slot.

- [ ] Recovery partition contains `kernel.bak` — copy of the last verified working kernel
- [ ] `restore_kernel(slot)` — copies `kernel.bak` to `\boot\kernel.exe` on target slot
- [ ] After restore: reset boot metadata for target slot (tries=0, successful=0)
- [ ] Update `kernel.bak` whenever `mark_boot_successful()` fires (→ XREF: TODO-14 §5)
- [ ] Commit: `"recovery: restore backup kernel to slot — last known-good version"`

**Test checkpoint:** Corrupt Slot A kernel. Enter recovery. Restore backup. Reboot → Slot A boots successfully.

---

## 5. Boot Metadata Reset

Recovery can reset A/B boot metadata to a clean state.

- [ ] Reset active slot to A, tries=0, successful=1
- [ ] Reset inactive slot to tries=0, successful=0
- [ ] Clear any pending update flags
- [ ] Commit: `"recovery: boot metadata reset — clean A/B state"`

---

## 6. NVRAM Boot Entry Reconstruction

If UEFI NVRAM boot entries are lost (firmware reset, battery pull), rebuild them.

- [ ] Scan `\EFI\` directory on ESP for known bootloader filenames
- [ ] Recreate `BootXXXX` UEFI variables for each found bootloader
- [ ] Set `BootOrder` with Impossible OS as first entry
- [ ] Model after Linux `fallback.efi` / `BOOT.CSV` approach
- [ ] Commit: `"recovery: NVRAM boot entry reconstruction — rebuild after firmware reset"`

**Test checkpoint:** Clear all NVRAM boot entries. Reboot → fallback.efi triggers → NVRAM rebuilt → normal boot works.

---

## 7. Recovery UI

User-visible recovery interface with clear status.

- [ ] Text-mode UI showing: `"Impossible OS Recovery"`, slot status, available actions
- [ ] Actions: `[1] Repair filesystem`, `[2] Restore backup kernel`, `[3] Reset boot metadata`, `[4] Rebuild NVRAM`, `[5] Reboot`
- [ ] Progress display for each action
- [ ] Requires USB keyboard (→ XREF: TODO-12) or serial input
- [ ] Commit: `"recovery: text-mode recovery UI with repair actions"`

**Test checkpoint:** Enter recovery → menu displayed → select "Repair filesystem" → fsck runs with progress → reboot option.

---

## OS Comparison

| ⭐ | Feature                    | 🪟 Win11                   | 🐧 Linux                   | 🚀 Impossible OS             |
|----|----------------------------|-------------------------|-------------------------|---------------------------|
| 💎 | Recovery partition         | ✅ WinRE partition      | ⚠️ Optional initramfs   | ⬜ §1                     |
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
  - Assert serial contains `"Impossible OS Recovery"` (§2 — recovery bootloader activates)
- [ ] Commit: `"test: add IXFS fsck and recovery partition smoke tests"`

## Verification

- [ ] **Both slots bad**: corrupt both → recovery activates automatically.
- [ ] **NVRAM wiped**: clear all boot entries → fallback.efi rebuilds them → normal boot.
- [ ] **Filesystem corrupt**: damage IXFS bitmap → recovery fsck repairs → boot succeeds.
- [ ] **Normal boot regression**: recovery partition present but not used during normal boot.
- [ ] Commit: `"boot: recovery partition complete — self-repair without external media"`
