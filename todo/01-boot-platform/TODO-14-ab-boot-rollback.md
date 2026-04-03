# TODO-14 -- A/B Dual-Slot Boot & Automatic Rollback

> **Goal:** The system is never unbootable. Implement A/B dual-slot boot partitioning with automatic rollback on failed updates. If a kernel update breaks boot, the system automatically reverts to the previous working version on the next reboot -- no user intervention, no recovery USB, no expertise needed. This is the pattern used by Android, Chrome OS, and modern embedded systems. Windows achieves similar via Automatic Repair; Linux via systemd-boot auto-assessment.

> [!IMPORTANT]
> **Current state:** Single root partition, no rollback. A corrupted kernel update bricks the system. TODO-01 §8 scoped A/B boot but deferred it. This TODO implements the full pipeline: partition layout, boot metadata, failure counting, automatic rollback, and update engine integration.

---

## Inputs

- `src/boot/uefi/bootx64.c` -- bootloader (needs slot selection logic)
- `scripts/build.sh` -- disk image creation (needs dual-slot layout)
- `include/kernel/boot_info.h` -- boot_info (needs slot metadata)
- → XREF: `TODO-01-uefi-hardening-secureboot.md` -- UEFI bootloader (§8 A/B dual-slot was removed; this TODO is the sole owner)
- → XREF: `TODO-02-bootloader-error-recovery.md §9` -- boot failure screen integration
- → XREF: `10-services-security/TODO-03-updates-packages.md` -- update engine (downstream consumer)

---

## Outcome

- Disk layout has two root partitions: Slot A and Slot B.
- Boot metadata tracks: active slot, try count, successful flag per slot.
- Bootloader reads metadata, selects active slot, boots from it.
- If boot fails (kernel panic before `mark_boot_successful()`), try count increments.
- After 3 failed attempts, bootloader automatically rolls back to the other slot.
- Update engine writes to inactive slot, marks it as `pending`, reboots.
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
| ⭐  |   6   | Slot status in boot diagnostics                 | §1–§5      |  [ ]   |

> 💎 = parity -- Android/Chrome OS A/B and systemd-boot auto-assessment both provide this.
> ⭐ = exclusive -- slot status integrated into VPD boot diagnostics.

---

## 1. Boot Metadata Structure

Define where boot slot metadata is stored. Two options: UEFI NVRAM variables or a dedicated GPT partition.

- [ ] Choose storage: UEFI NVRAM (simpler, survives partition changes) vs GPT metadata partition (survives NVRAM reset)
- [ ] Define metadata format: `{ magic, version, active_slot (A/B), slot_a { tries, successful, priority }, slot_b { same } }`
- [ ] `tries`: incremented by bootloader before each boot attempt (0–3)
- [ ] `successful`: set by kernel after boot reaches desktop (via `mark_boot_successful()`)
- [ ] `priority`: which slot is preferred when both are valid (higher = preferred)
- [ ] Implement read/write functions: `boot_meta_read()` / `boot_meta_write()` in bootloader
- [ ] Commit: `"boot: A/B boot metadata structure and NVRAM storage"`

**Test checkpoint:** Bootloader reads/writes metadata. Serial shows `"Slot A: tries=0 successful=1 priority=1"`.

---

## 2. Dual-Slot Disk Layout

Modify the build system to create disk images with two root partitions.

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
- [ ] If active slot `tries >= 3` and `successful == 0`: switch to other slot (rollback)
- [ ] Increment `tries` for active slot before booting
- [ ] Mount the active slot's partition (not EFI partition) for kernel loading
- [ ] Pass `boot_info.active_slot = 'A'/'B'` to kernel
- [ ] Log: `"[BOOT] Booting Slot %c (tries=%u, successful=%u)"` with slot info
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
- [ ] If BOTH slots have `tries >= 3`: enter recovery mode (→ XREF: TODO-15)
- [ ] Commit: `"boot: automatic rollback after 3 failed boot attempts"`

**Test checkpoint:** Intentionally corrupt Slot A kernel. Boot 3 times → 4th boot automatically switches to Slot B.

**Regression risk:** MEDIUM -- if try counter logic has off-by-one, healthy boots get rolled back. Test thoroughly.

---

## 5. Kernel `mark_boot_successful()`

Kernel-side API to tell the bootloader "this boot worked".

- [ ] Add syscall or kernel function: `mark_boot_successful()` -- writes `successful=1, tries=0` to boot metadata
- [ ] Called from `boot_phase3()` after desktop is visible and cmd.exe starts (proof of successful boot)
- [ ] On success: `"[BOOT] Boot marked successful (Slot %c)"`
- [ ] If metadata write fails: `"[WARN] Cannot mark boot successful -- rollback may trigger on next reboot"`
- [ ] For NVRAM storage: use UEFI `SetVariable()` via runtime services
- [ ] Commit: `"kernel: mark_boot_successful -- reset try counter after successful boot"`

**Test checkpoint:** Normal boot → serial shows `"Boot marked successful (Slot A)"` after desktop appears.

---

## 6. Slot Status in Boot Diagnostics

Integrate A/B slot status into the VPD and boot timing display.

- [ ] VPD shows: `"Slot A [active] tries=0 ok"` / `"Slot B [standby] tries=0 ok"`
- [ ] If rollback occurred: VPD shows `"⚠ Rolled back from Slot X (3 failures)"`
- [ ] boot_timing log includes slot selection time
- [ ] Commit: `"boot: A/B slot status in VPD diagnostics"`

**Test checkpoint:** Boot with `postbars=on` -- slot status visible in VPD display.

---

## OS Comparison

| ⭐ | Feature                   | 🪟 Win11                      | 🐧 Linux                    | 🚀 Impossible OS             |
|----|---------------------------|----------------------------|--------------------------|---------------------------|
| 💎 | Dual-slot boot            | ⚠️ Automatic Repair only  | ⚠️ systemd-boot assess   | ⬜ §1–§3                  |
| 💎 | Boot failure counting     | ✅ 2-attempt detection     | ✅ systemd tries counter | ⬜ §4                     |
| 💎 | Automatic rollback        | ⚠️ Manual repair needed   | ⚠️ Manual or auto        | ⬜ §4                     |
| 💎 | Mark boot successful      | ✅ Implicit (desktop OK)   | ✅ systemd boot-complete | ⬜ §5                     |
| ⭐ | Slot status in boot UI    | ❌ Hidden                  | ❌ journalctl only       | ⬜ §6 🚀                  |

After §1–§5, Impossible OS has stronger rollback than Windows (which requires manual Automatic Repair) and matches Chrome OS/Android A/B. §6 makes slot status visible during boot.

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
- [ ] Register in `test_runner_init()`: `test_register_ab_boot()`
- [ ] Create `scripts/test-boot-rollback.sh`:
  - Build dual-slot disk image
  - Corrupt slot A kernel (truncate to 0 bytes)
  - Boot QEMU 4 times in sequence, capture serial each time
  - Assert 4th boot serial contains `"rolling back to Slot B"` (§4 -- automatic rollback)
- [ ] Commit: `"test: add A/B boot rollback test suite"`

## Verification

- [ ] **Normal boot**: Slot A boots, try counter resets, `mark_boot_successful` logged.
- [ ] **Rollback test**: corrupt Slot A kernel, boot 3 times → Slot B activates automatically.
- [ ] **Both slots bad**: corrupt both → enters recovery mode (or shows error screen).
- [ ] **Update simulation**: write new kernel to Slot B, mark pending, reboot → boots Slot B.
- [ ] Commit: `"boot: A/B dual-slot boot complete -- automatic rollback, never unbootable"`
