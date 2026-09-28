<!-- docs: covers=todo/01-boot-platform/TODO-21-ab-boot-rollback.md sources=include/boot/ab_boot_metadata.h,src/boot/uefi/bootx64.c,src/boot/uefi/boot_info_mirror.h,include/kernel/boot_info.h,src/kernel/fs/partition.c,src/kernel/fs/gpt.c,src/kernel/vpd.c,src/kernel/main/boot_status.c,src/kernel/main/compositor.c,src/kernel/main/boot_hw.c,tools/make-system-disk.c,src/kernel/test/test_ab_boot.c reviewed=2026-09-28 order=21 -->
# A/B Dual-Slot Boot and Automatic Rollback

## What is it?

This is the mechanism that keeps the system bootable after a bad update: two root partitions (Slot A and Slot B), a CRC-protected on-disk metadata record tracking which slot is active, how many boot attempts it has consumed and whether it has ever booted successfully, and a bootloader that switches to the other slot after three failed attempts. It follows the A/B pattern Android and ChromeOS use, so a corrupted root filesystem no longer leaves the machine unbootable.

The core pipeline ships and runs on every boot: slot selection, mount, the increment-before-boot try counter, rollback on exhaustion, mark-good on the first composited desktop frame, dual-copy atomic metadata writes, and VPD status reporting. What is not built is per-slot kernel isolation (the kernel binary still lives on the shared ESP), enforcement of the per-slot anti-rollback version floor, and an automated end-to-end rollback test.

## How does it work?

The bootloader owns slot selection. Before `ExitBootServices`, `select_active_slot()` locates the dedicated A/B-metadata GPT partition and reads both redundant metadata copies. Each copy carries a CRC-32, a monotonic generation counter and a version; `ab_boot_meta_select_newest()` returns whichever valid copy has the higher generation. A fully zeroed pair (first boot) defaults to Slot A. A pair that is initialized but fails validation on both copies stops with a fatal boot error rather than silently defaulting to Slot A, because a corrupted record could otherwise hide a real prior rollback.

`ab_boot_meta_decide()` classifies the outcome as NORMAL, ROLLBACK or BOTH_EXHAUSTED. A slot counts as exhausted once its `tries` counter reaches `AB_BOOT_MAX_TRIES` (3), even if it once booted successfully. `ab_boot_meta_choose_slot()` picks the highest-priority slot that is not exhausted; if both are, `ab_boot_meta_least_bad_slot()` picks a stopgap, preferring a slot that once succeeded. The choice is published in `boot_info.active_slot`, together with an as-selected snapshot (reason, source slot, per-slot tries and flags) so the kernel reports what was actually selected rather than re-reading state that later writes have changed.

Before handing off, `ab_bl_increment_tries()` writes the incremented try count for the slot about to boot. The write targets the older-generation copy and bumps the generation (`ab_boot_meta_write_target()`, `ab_boot_meta_next_generation()`), so a power loss mid-write always leaves the other copy intact. This is the only place `tries` is incremented.

On the kernel side, `partition_mount_filesystems()` mounts the slot `boot_info.active_slot` names as C:, using `gpt_ixfs_slot()` to tell Slot A from Slot B, and records whether the mounted slot matches the selected one. When the compositor draws its first frame, `boot_status_accept_advance(BOOT_ACCEPT_UI_READY)` calls `ab_boot_mark_slot_successful()`. That function refuses to run unless the mounted slot equals the selected slot, then uses the same dual-copy write to set `tries=0, successful=1`. A slot that never reaches this point keeps accumulating tries until the bootloader rolls away from it.

A rollback or both-exhausted decision also draws an on-screen banner (`ab_draw_rollback_banner()`) alongside the serial line, and the boot-timing log records an `ab-slot-handoff` step.

The disk layout comes from `make-system-disk --ab`, which the default `system-disk` Makefile target uses: six GPT partitions (ESP, BlackBox, Slot A, Slot B, the A/B-metadata partition, and the read-only [Recovery partition](recovery-partition.md)), checked after layout for ordering, overlap and a 96 MiB minimum per slot. Only the root filesystems are duplicated per slot, because the bootloader's `load_kernel()` reads the kernel from the FAT ESP and cannot read IXFS.

## What are its interfaces?

| Interface | Purpose |
| --------- | ------- |
| `struct ab_boot_metadata`, `struct ab_boot_slot` | The v1 on-disk record: per-slot `tries`, `successful`, `priority`, `rollback_index`, plus `crc32`, `generation`, `version`, `rollback_floor` ([`ab_boot_metadata.h`](../../include/boot/ab_boot_metadata.h)) |
| `ab_boot_meta_is_valid()`, `ab_boot_meta_select_newest()` | Validation and dual-copy selection, shared by bootloader, kernel and unit tests ([`ab_boot_metadata.h`](../../include/boot/ab_boot_metadata.h)) |
| `ab_boot_meta_choose_slot()`, `ab_boot_meta_decide()`, `ab_boot_meta_least_bad_slot()` | Slot selection: normal pick, rollback, both-exhausted stopgap ([`ab_boot_metadata.h`](../../include/boot/ab_boot_metadata.h)) |
| `select_active_slot()`, `ab_bl_increment_tries()`, `ab_draw_rollback_banner()` | Bootloader: read and decide, pre-boot try increment, rollback banner ([`bootx64.c`](../../src/boot/uefi/bootx64.c)) |
| `boot_info.active_slot` and the `ab_*` status fields | Handoff of the selected slot and the as-selected snapshot ([`boot_info.h`](../../include/kernel/boot_info.h), mirrored in [`boot_info_mirror.h`](../../src/boot/uefi/boot_info_mirror.h)) |
| `gpt_ixfs_slot()` | Kernel: identifies a partition's A/B slot ([`gpt.c`](../../src/kernel/fs/gpt.c)) |
| `ab_boot_mark_slot_successful()` | Kernel: mark-good write, gated on mounted slot equals selected slot ([`partition.c`](../../src/kernel/fs/partition.c)) |
| `boot_status_accept_advance()` | Boot-acceptance ledger that fires mark-good at `BOOT_ACCEPT_UI_READY` ([`boot_status.c`](../../src/kernel/main/boot_status.c)), called from the first-frame path in [`compositor.c`](../../src/kernel/main/compositor.c) |
| `vpd_render_ab_slot()` | Per-slot VPD status lines and the rollback warning ([`vpd.c`](../../src/kernel/vpd.c)) |
| `make-system-disk --ab` | Builds the A/B disk layout ([`make-system-disk.c`](../../tools/make-system-disk.c)) |

## How do I use it?

There is no opt-in flag: the default build already produces an A/B disk.

```bash
bash scripts/build.sh
bash scripts/test.sh SUITE=boot
```

On a fresh disk, serial shows only `[BOOT] A/B select: metadata uninitialized (first boot) -- Slot A`. Once metadata exists, later boots show a `[BOOT] Booting Slot A (tries=...` line instead. The kernel logs `A/B: mounted slot N as C:` at mount and `A/B: boot marked successful (slot N, copy N, gen N)` once the desktop draws its first frame.

When a slot exhausts its tries, the bootloader logs a `[BOOT] A/B: Slot X failed N times -- rolling back to Slot Y` line and draws the banner. When both slots are exhausted, it logs `[BOOT] A/B: WARNING both slots exhausted (no verified slot) -- booting least-bad Slot X`. With `postbars=on`, the VPD screen shows one line per slot (active or standby, tries, bootable or exhausted, verified or pending), plus a "Rolled back from" or "Both slots exhausted" line when relevant.

The `test_ab_boot.c` cases (record validation, CRC, generation selection, the selection state machine, slot reconciliation and the write helpers) run in the `boot` suite (`scripts\debug\kernel\run-boot-tests.bat` on Windows).

## What is not implemented yet?

- The kernel binary stays on the shared ESP, so a bad ESP update can still fail before slot selection runs; per-slot kernels need a bootloader IXFS reader: [Dual-Slot Disk Layout](../../todo/01-boot-platform/TODO-21-ab-boot-rollback.md#2-dual-slot-disk-layout).
- The bootloader does not yet honor a split boot entry's `payload.root`, so a seeded `slot-b` entry cannot resolve to its own partition: [Bootloader Slot Selection](../../todo/01-boot-platform/TODO-21-ab-boot-rollback.md#3-bootloader-slot-selection).
- Mark-good has no verified-slot-identity gate, so it cannot refuse to mark an unverified slot good: [Kernel mark_boot_successful()](../../todo/01-boot-platform/TODO-21-ab-boot-rollback.md#5-kernel-mark_boot_successful).
- `rollback_index` and `rollback_floor` exist on disk only; nothing refuses a slot below the floor, because the floor has no authenticated store yet (the CRC detects corruption, not an attacker): [Per-Slot Anti-Rollback Version Floor](../../todo/01-boot-platform/TODO-21-ab-boot-rollback.md#8-per-slot-anti-rollback-version-floor).
- No automated harness corrupts a slot, boots it repeatedly and asserts the rollback; the state machine is unit-tested but the multi-boot scenario is not: [Unit Tests](../../todo/01-boot-platform/TODO-21-ab-boot-rollback.md#unit-tests).
- Both-exhausted boots a least-bad slot rather than routing to a recovery environment, which does not exist yet: [Boot Failure Counting and Rollback](../../todo/01-boot-platform/TODO-21-ab-boot-rollback.md#4-boot-failure-counting-and-rollback).

## How does it compare with Windows 11 and Linux?

Windows 11 relies on Automatic Repair, which detects a failed boot and offers recovery rather than switching to an alternate slot. On Linux, systemd-boot's boot counting provides a similar try-and-assess model but is optional and not widely deployed. Impossible OS already rolls back automatically, with CRC-protected dual-copy metadata writes, which matches the core of the Android and ChromeOS model. It falls short of Android in one place: the anti-rollback floor that blocks downgrading to a vulnerable slot is designed but not enforced.

## See also

- [A/B Dual-Slot Boot and Rollback roadmap](../../todo/01-boot-platform/TODO-21-ab-boot-rollback.md)
- [Boot Health Gate](boot-health.md)
- [Boot Entries, Menu and Policy](boot-entries-menu-policy.md)
- [Boot Entry Store Schema](boot-entry-schema.md)
- [Recovery Partition](recovery-partition.md)
- [boot_info Field Ownership](boot-info-fields.md)
