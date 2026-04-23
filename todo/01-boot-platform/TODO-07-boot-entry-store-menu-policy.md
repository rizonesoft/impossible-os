---
schema_version: 1
id: boot-entry-store-menu-policy
domain: 01-boot-platform
status: active
title: "TODO-07 -- Boot Entry Store, Menu & Policy"
---

# TODO-07 -- Boot Entry Store, Menu & Policy

> **Goal:** Provide a complete boot-entry model: multiple OS entries, kernel variants, A/B slots, recovery, safe mode, test mode, BootNext one-shot overrides, timeout policy, and a user-visible boot menu. This complements UEFI BootOrder reading by adding an OS-owned entry store that the bootloader can interpret predictably.
> **Current state:** `boot.conf` is parsed into a single `boot_config`, UEFI BootOrder/BootCurrent/BootNext are read for diagnostics, and TODO-27 has a deferred multi-OS boot menu item. There is no BCD-style object store, no structured boot entries, no menu UI beyond firmware selection, and no policy for merging UEFI BootNext with OS slot/recovery choices.

## Inputs

- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c)
- [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h)
- -> XREF: `TODO-05-boot-device-discovery.md §6` -- UEFI boot variables
- -> XREF: `TODO-21-ab-boot-rollback.md` -- A/B slot state
- -> XREF: `TODO-22-recovery-partition.md` -- recovery entries
- -> XREF: `TODO-27-uefi-advanced.md §1` -- advanced multi-OS menu consumes this store
- -> XREF: `../02-kernel-core/TODO-02-kernel-configuration-policy.md §1, §4, §5` -- runtime policy merge, control-set selection, and safe-mode policy after kernel starts

## Outcome

- Boot entries are structured, validated, and editable by OS tools.
- Boot menu can select normal, safe, recovery, previous kernel, test, installer, and network entries.
- BootNext and A/B rollback compose deterministically.
- Bootloader records the selected entry and reason in boot_info.

## Implementation Order

| ⭐ | Order | Deliverable | Depends On | Status |
| --- | :---: | --- | --- | :---: |
| 💎 | 1 | Boot entry file format | TODO-01 §1 | [ ] |
| 💎 | 2 | Boot entry parser and validator | §1 | [ ] |
| 💎 | 3 | Boot policy merge order | §2, TODO-05 §6 | [ ] |
| 💎 | 4 | Text and graphical boot menu | §2, TODO-15 | [ ] |
| 💎 | 5 | Safe mode, test mode, and diagnostics entries | §3, TODO-10 | [ ] |
| 💎 | 6 | A/B and recovery entry integration | §3, TODO-21, TODO-22 | [ ] |
| 💎 | 7 | Previous-kernel and known-good entries | §2, TODO-06 | [ ] |
| 💎 | 8 | Boot entry editor tooling | §1-§7 | [ ] |
| ⭐ | 9 | Policy audit trail and rollback reason codes | §3, §6 | [ ] |
| 💎 | 10 | Boot entry tests | §1-§9 | [ ] |

## 1. Boot Entry File Format

- [ ] Define `\EFI\ImpossibleOS\bootentries.json` or compact INI equivalent.
- [ ] Support id, title, kernel path, initrd/module list, root selector, flags, timeout, and policy tags.
- [ ] Include schema version and checksum.
- [ ] Define minimal fallback when the store is absent.
- [ ] Commit: `"boot: define boot entry store format"`

## 2. Boot Entry Parser and Validator

- [ ] Parse entries without heap allocation in the UEFI bootloader.
- [ ] Validate paths, flags, duplicate IDs, unsupported schema, and size limits.
- [ ] Reject entries that point outside allowed ESP/recovery paths unless explicitly trusted.
- [ ] Add visible error path with fallback to default entry.
- [ ] Commit: `"boot: parse boot entry store"`

## 3. Boot Policy Merge Order

- [ ] Define priority: firmware BootNext, physical menu hotkey, watchdog rollback, A/B slot state, recovery request, boot entry default.
- [ ] Persist selected entry id and selection reason to boot_info.
- [ ] Prevent loops by recording failed entry attempts.
- [ ] Document all policy precedence in `docs/boot/boot-policy.md`.
- [ ] Commit: `"boot: define boot entry policy precedence"`

## 4. Text and Graphical Boot Menu

- [ ] Add timeout menu using GOP when available and serial/text fallback otherwise.
- [ ] Keyboard navigation via firmware input first; later USB HID handoff can take over.
- [ ] Show Secure Boot, measured boot, slot, recovery, and network indicators.
- [ ] Expose hotkeys for diagnostics, safe mode, and recovery.
- [ ] Commit: `"boot: add boot entry menu"`

## 5. Safe Mode, Test Mode, and Diagnostics Entries

- [ ] Represent safe mode as entry flags rather than ad hoc boot.conf booleans.
- [ ] Add test-mode entries for unit suites.
- [ ] Add VPD/full diagnostics entry.
- [ ] Map entries back into `boot_config` for existing kernel consumers.
- [ ] Commit: `"boot: structured safe/test/diagnostic entries"`

## 6. A/B and Recovery Entry Integration

- [ ] Generate entries for slot A, slot B, recovery, and fallback kernel.
- [ ] Merge A/B success/failure counters before menu display.
- [ ] Auto-select recovery when both slots fail.
- [ ] Display rollback reason in menu and VPD.
- [ ] Commit: `"boot: integrate A/B and recovery entries"`

## 7. Previous-Kernel and Known-Good Entries

- [ ] Keep last-known-good kernel path and manifest digest.
- [ ] Add boot entry for previous kernel after update.
- [ ] Retire previous-kernel entry only after `mark_boot_successful()`.
- [ ] Interlock with code integrity and measured boot.
- [ ] Commit: `"boot: previous-kernel known-good entries"`

## 8. Boot Entry Editor Tooling

- [ ] Add `bootcfg.exe` or host tool to list/add/remove/set-default entries.
- [ ] Add offline image edit mode for CI and release tooling.
- [ ] Validate store before writing.
- [ ] Update install/release scripts.
- [ ] Commit: `"tools: boot entry editor"`

## 9. Policy Audit Trail and Rollback Reason Codes

- [ ] Define reason codes for every automatic selection.
- [ ] Persist selection to NVRAM and BlackBox.
- [ ] Include previous entry, selected entry, reason, and counters.
- [ ] Feed boot diagnostics and recovery UI.
- [ ] Commit: `"boot: audit boot entry policy decisions"`

## 10. Boot Entry Tests

- [ ] Parser fixture tests for valid/invalid stores.
- [ ] QEMU boot menu timeout/default selection test.
- [ ] BootNext one-shot override test.
- [ ] A/B rollback entry selection test.
- [ ] Commit: `"test: boot entry store and menu"`

## OS Comparison

| ⭐ | Feature | Windows | Linux | Impossible OS |
| --- | --- | --- | --- | --- |
| 💎 | Structured boot entries | BCD | systemd-boot/GRUB | TODO-07 |
| 💎 | BootNext merge | Firmware + BCD | efibootmgr | TODO-07 §3 |
| 💎 | Recovery/safe entries | WinRE/safe mode | GRUB entries | TODO-07 §5-§7 |
| ⭐ | BlackBox policy audit | limited logs | scattered | TODO-07 §6 |

## Unit Tests

- [ ] `test_boot_entry_parse_valid`
- [ ] `test_boot_entry_rejects_duplicate_id`
- [ ] `test_boot_policy_bootnext_precedence`
- [ ] `test_boot_policy_ab_rollback_selection`

## Verification

- [ ] QEMU boot menu keyboard selection
- [ ] QEMU BootNext one-shot path
- [ ] A/B rollback auto-selection
- [ ] Bare metal menu over GOP and serial fallback
