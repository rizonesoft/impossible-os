<!-- docs: covers=todo/01-boot-platform/TODO-07-boot-entry-store-menu-policy.md sources=src/boot/uefi/bootx64.c,include/boot/boot_entries.h reviewed=2026-09-28 -->
# Boot Menu Renderer + Hotkeys + Indicators

> Pre-EBS user-visible boot selector. Owned by [TODO-07 boot entry store + menu + policy](../../todo/01-boot-platform/TODO-07-boot-entry-store-menu-policy.md): renderer in the menu-renderer section, indicators + hotkeys + `hide_when_alone` in the indicators section.

The menu is rendered by the bootloader before `ExitBootServices` so the operator can pick or override the policy ladder's auto-selection. Both GOP (graphical framebuffer + Selawik AA font) and ConOut (UEFI text console) paths are supported, with a serial mirror that always logs the selection state regardless of which display path the firmware exposes.

## Indicator legend

Indicator badges render to the LEFT of each entry's title. ASCII only because the Selawik AA atlas covers 0x20-0x7E and Windows serial mojibakes multi-byte UTF-8.

| Tag      | Meaning                                                                               |
|----------|---------------------------------------------------------------------------------------|
| `[SB]`   | Secure Boot active. Reads `SecureBoot` + `SetupMode` + `AuditMode` UEFI variables via `bootloader_secureboot_active()`. |
| `[REC]`  | Recovery entry (`kind == RECOVERY`).                                                  |
| `[NET]`  | Network entry (`kind == NETWORK`).                                                    |
| `[FAIL]` | Last-failure recorded for this entry id in `decision->rejected[]` (any non-NONE reason). |
| `[MB]`   | RESERVED for TPM-measured-boot event-log integration. Not yet emitted; producer lives in the measured-boot TODO domain. |

A/B-slot indicators (current/previous slot, try counter, last-failure label) are owned by the [A/B and Recovery integration section of TODO-07](../../todo/01-boot-platform/TODO-07-boot-entry-store-menu-policy.md#9-ab-and-recovery-entry-integration) and will join this legend when that section ships.

## Hotkey table

Recognized in `boot_menu_run()` once the menu is on screen. The pre-menu F11 probe runs before the menu is decided.

| Key       | Action                                                                                              |
|-----------|-----------------------------------------------------------------------------------------------------|
| Up / Down | Move highlight by one row.                                                                          |
| Home      | Jump to first entry.                                                                                |
| End       | Jump to last entry.                                                                                 |
| Enter     | Boot the selected entry.                                                                            |
| Esc       | Boot the currently-highlighted entry without further interaction (alias for Enter).                 |
| F8        | Safe-mode override. Writes `boot_config.boot_mode = 1` directly so kernel-side observers (KUSD `SafeBootMode` population, bare-metal hardening guards) see the operator intent regardless of which envelope kind was highlighted. Consumer wire-up to KUSD `SafeBootMode` is owned by the [kernel configuration policy section on Safe Mode mirroring](../../todo/02-kernel-core/TODO-02-kernel-configuration-policy.md#5-safe-mode-and-recovery-policy-object). |
| F10       | Reboot into firmware setup via the narrow `OsIndications` `BOOT_TO_FW_UI` path. See "F10 narrow path" below. |
| F11       | Force-show menu. Probed BEFORE the should-show gate so the operator can override forced-selection paths (HOTKEY / WATCHDOG / AB_TRY_STATE / RECOVERY_REQUEST / FALLBACK_*). |

Stray non-action keystrokes (printable characters, modifier ghost events, firmware console noise) DO NOT cancel the countdown. Without that gate, an unattended boot could otherwise sit forever once the watchdog refresh started running every tick.

### F10 narrow path

The F10 transition strictly follows the UEFI 2.10 firmware-setup flow:

1. `GetVariable("OsIndicationsSupported", ...)` -- if the variable is absent or does not include `EFI_OS_INDICATIONS_BOOT_TO_FW_UI` (`1ull << 0`), serial logs the unsupported reason and the menu continues. The operator does not get a stranded boot.
2. `GetVariable("OsIndications", ...)` -- `EFI_NOT_FOUND` is treated as "current value 0"; any other error fails closed and stays in the menu.
3. `OsIndications |= BOOT_TO_FW_UI` -- ONLY that single bit. Capsule-trigger bits (`PROCESS_CAPSULES_ON_DISK` etc.) stay banned per the UEFI hardening doctrine; existing capsule bits already in the variable are preserved (we are not authoring them).
4. `SetVariable("OsIndications", ...)` with attributes `NV | BS | RT` (the spec-mandated set for this variable). On `EFI_ERROR`, serial logs the failure and the menu continues; **only** on `EFI_SUCCESS` does the loader call `gRT->ResetSystem(EFI_RESET_COLD, ...)`.

This shape makes F10 fail-safe: a firmware variable-store exhaustion, write protection, auth failure, or RuntimeServices error never produces an unconditional reset that would have looked like F10 was accepted while not entering setup.

## `hide_when_alone` flag

`BOOT_ENTRY_FLAG_HIDE_WHEN_ALONE` (bit 3 in `include/boot/boot_entries.h`) opts a single-entry boot store into "skip the menu, auto-boot the lone entry" behavior. This matches Win11 single-OS and systemd-boot `default-pattern` parity: most users with one OS installed do not want a menu briefly flashing on every boot.

Semantics:

- **`cand_count == 1` AND lone candidate has the flag** -> `boot_menu_run()` returns immediately, no UI render, no countdown.
- **`cand_count == 1` AND lone candidate does NOT have the flag** -> menu renders for the default countdown so the operator gets a chance to hit F8 / F10 / F11.
- **`cand_count >= 2`** -> menu always renders (the flag is meaningless when there is something to choose between).

The flag is in the `BOOT_ENTRY_FLAG_MASK_KNOWN_V1` mask, parsed from boot-entry JSON via the parser flag accept-list, and round-tripped through the host validator.

## Cross-references

- [boot-policy.md](boot-policy.md) -- ladder + counter protocol + selection_reason table.
- [boot-entry-schema.md](boot-entry-schema.md) -- on-disk JSON schema for boot entries (flags, kinds, timeout_override).
- [TODO-07 boot entry store + menu + policy](../../todo/01-boot-platform/TODO-07-boot-entry-store-menu-policy.md) -- master roadmap for the boot entry store + menu + policy.
