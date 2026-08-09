---
schema_version: 1
id: input-system-drivers
domain: 04-drivers-hardware
status: active
title: "TODO-11 -- Input System Enhancement"
---

# TODO-11 -- Input System Enhancement

> **Goal:** Complete the input stack beyond the working PS/2 basic, VirtualBox absolute, and VirtIO tablet drivers: Intellimouse scroll and 5-button extension, packet resync, mouse acceleration, raw input grab API, keyboard layout system with dead keys and Unicode, layout switching with system-tray indicator, and sticky keys + typematic configuration.

> [!IMPORTANT]
> **Already complete:** PS/2 byte-stream reader, IRQ handler, basic 3-button mouse, absolute mouse (VBox/VirtIO), keyboard scan-code → keycode mapping, basic keydown/keyup events. This TODO builds the full-featured input layer on top. Mouse acceleration and layout switching must not affect absolute input sources (VirtIO tablet, VBox absolute) -- always check `mouse_source_t` before applying transformations.

## Inputs

- [`src/kernel/drivers/mouse.c`](../../src/kernel/drivers/mouse.c), [`include/kernel/drivers/mouse.h`](../../include/kernel/drivers/mouse.h)
- [`src/kernel/drivers/keyboard.c`](../../src/kernel/drivers/keyboard.c), [`include/kernel/drivers/keyboard.h`](../../include/kernel/drivers/keyboard.h)
- → XREF: `08-graphics-ui` domain -- `WM_SCROLL`, `WM_INPUT`, cursor hide/show, and layout indicator popup are message types and UI elements handled by the compositor/window manager
- → XREF: `09-desktop-shell` domain -- system-tray layout indicator (`EN`/`FR`/`DE`) and layout picker popup are shell components consuming `kbd_get_layout()` / `kbd_set_layout()`
- → XREF: `04-drivers-hardware/TODO-11-input-system.md §9` -- `SYS_MOUSE_GRAB`/`SYS_MOUSE_RELEASE` syscalls registered in the native API dispatch table (→ XREF `02-kernel-core/TODO-12-native-api-ssdt.md`)

## Outcome

- Intellimouse (ID 3) and Explorer 5-button (ID 4) mice are detected via magic init sequences; 4-byte packets deliver scroll and side-button events to focused windows via `WM_SCROLL`.
- Packet boundary is validated every byte; sync loss increments `sync_loss_count` and triggers re-scan rather than silent corruption.
- Mouse delta acceleration uses a 16.16 fixed-point polynomial; skipped for absolute sources; tunable via Registry `MouseSensitivity`.
- `mouse_raw_grab(task)` / `mouse_raw_release()` hide the cursor and route `{dx, dy, buttons}` in `WM_INPUT` messages directly to the grabbing task -- enabling first-person games and 3D viewport control.
- Keyboard output is fully Unicode/UTF-8: layout tables cover normal, shift, and AltGr planes; dead keys compose accented characters; unmapped dead+base pairs emit the dead character then the base character.
- Six built-in layouts (en-US, en-GB, de-DE, fr-FR, es-ES, Dvorak); Win+Space cycles them; a 2-letter system-tray indicator reflects the active layout.
- Sticky Keys activates after 5 consecutive rapid Shift presses; typematic delay and rate are configurable via PS/2 command `0xF3` and Registry.

## Implementation Order

| ⭐  | Order | Deliverable                                                      | Depends On                             | Status |
| --- | :---: | ---------------------------------------------------------------- | -------------------------------------- | :----: |
| 💎  |   1   | §1 Keyboard layout system (`kbd_layout_t`, `kbd_set_layout`)     | keyboard.c baseline                    |  [ ]   |
| 💎  |   2   | §2 Built-in layouts -- en-US, en-GB, de-DE, fr-FR, es-ES, Dvorak | §6                                     |  [ ]   |
| 💎  |   3   | §3 UTF-8 / Unicode codepoint output                              | §6 (layout tables emit codepoints)     |  [ ]   |
| 💎  |   4   | §4 Dead key compose                                              | §6, §5                                 |  [ ]   |
| 💎  |   5   | §5 PS/2 packet resync -- sync-bit validation                     | mouse.c baseline                       |  [ ]   |
| 💎  |   6   | §6 Intellimouse scroll wheel (ID 3)                              | §3 (stable packet parser)              |  [ ]   |
| 💎  |   7   | §7 Explorer 5-button extension (ID 4)                            | §6 (scroll already done)               |  [ ]   |
| 💎  |   8   | §8 Mouse acceleration + sensitivity                              | §3, §6, §7 (final packet layout known) |  [ ]   |
| 💎  |   9   | §9 Raw input grab API (`WM_INPUT`, syscalls)                     | §8 (acceleration skip for raw mode)    |  [ ]   |
| 💎  |  10   | §10 Layout switching -- Win+Space, tray indicator                | §7 (layouts exist), shell tray         |  [ ]   |
| 💎  |  11   | §11 Sticky keys + typematic rate                                 | §6 (modifier key events)               |  [ ]   |

> All eleven rows are 💎 parity: Windows 11 and Linux both support Intellimouse, raw input, Unicode keyboard, layout switching, and accessibility features. Closing these gaps brings the Impossible OS input stack to desktop-OS standard.

---

## 1. Keyboard Layout System `[Sonnet]`

Define `kbd_layout_t` -- a fixed structure of 128-entry arrays for normal, shift, and AltGr planes, plus up to 32 dead-key entries. `kbd_set_layout(code)` hot-swaps the active layout and persists to Registry.

**Files:** `include/kernel/drivers/kbd_layout.h` (new), `src/kernel/drivers/keyboard.c`, `src/kernel/drivers/kbd_layouts/` (new directory)

- [ ] Define `dead_key_entry_t { uint32_t dead_cp; uint32_t base_cp; uint32_t composed_cp; }` and `kbd_layout_t { char name[32]; char code[8]; uint32_t normal[128]; uint32_t shift[128]; uint32_t altgr[128]; dead_key_entry_t deadkeys[32]; uint8_t deadkey_count; }`
- [ ] `kbd_set_layout(code)` -- find layout by `code` in registered list; set `g_active_layout`; write `HKLM\SYSTEM\Input\KeyboardLayout` (`REG_SZ`)
- [ ] `kbd_get_layout()` -- return `g_active_layout->code`
- [ ] `kbd_register_layout(layout)` -- add to global layout list; called by each layout module at init
- [ ] Load active layout from Registry at boot; fall back to en-US
- [ ] `kbd_translate(scancode, modifiers)` -- look up in `g_active_layout->normal/shift/altgr` based on current modifier state; return Unicode codepoint
- [ ] Commit: `"drivers: keyboard layout system -- kbd_layout_t, kbd_set_layout, codepoint translation"`

## 2. Built-in Layouts -- en-US, en-GB, de-DE, fr-FR, es-ES, Dvorak `[Sonnet]`

Populate six `kbd_layout_t` instances with correct normal/shift/AltGr codepoints and dead-key entries. All registered at kernel init via `kbd_register_layout()`.

**Files:** `src/kernel/drivers/kbd_layouts/en_us.c`, `en_gb.c`, `de_de.c`, `fr_fr.c`, `es_es.c`, `dvorak.c` (all new)

- [ ] `en_us` -- standard US QWERTY; no AltGr; no dead keys; register as `"en-US"`
- [ ] `en_gb` -- UK QWERTY; `#` at position 3 shifted to `£`; AltGr `€` on E; no dead keys; register as `"en-GB"`
- [ ] `de_de` -- German QWERTZ (Y↔Z swap); AltGr plane: `@`, `€`, `µ`, `{`, `}`, `[`, `]`, `\`, `~`, `|`; dead keys: `` ` ``→`grave`, `^`→`circ`, `~`→`tilde`; umlauts `ä`, `ö`, `ü`, `Ä`, `Ö`, `Ü`, `ß` in normal/shift; register as `"de-DE"`
- [ ] `fr_fr` -- French AZERTY; AltGr: `€`, `#`, `{`, `[`, `|`, `` ` ``, `\`, `^`, `@`, `]`, `}`; dead keys: `^`→`circ`, `¨`→`dieresis`; accented base chars; register as `"fr-FR"`
- [ ] `es_es` -- Spanish; AltGr: `€`, `@`, `#`, `[`, `]`, `{`, `}`, `\`, `|`; dead keys: `` ` ``→`grave`, `´`→`acute`, `¨`→`dieresis`; `ñ`, `Ñ`, `¿`, `¡`; register as `"es-ES"`
- [ ] `dvorak` -- Programmer Dvorak; rearranged codepoints per standard Dvorak map; register as `"Dvorak"`
- [ ] All six registered via `kbd_register_layout()` calls in `keyboard_init()`
- [ ] Commit: `"drivers: built-in keyboard layouts -- en-US, en-GB, de-DE, fr-FR, es-ES, Dvorak"`

## 3. UTF-8 / Unicode Codepoint Output `[Sonnet]`

Add `utf8_encode(cp, buf)` / `utf8_decode(buf, &cp)` helper functions. All keyboard output flows through `utf8_encode`; font rendering (`stb_truetype`) already takes codepoints -- wire the translation chain together.

**Files:** `include/kernel/unicode.h` (new), `src/kernel/unicode.c` (new), `src/kernel/drivers/keyboard.c`

- [ ] `utf8_encode(uint32_t cp, uint8_t buf[4])` → number of bytes written (1–4); standard UTF-8 encoding
- [ ] `utf8_decode(const uint8_t *buf, uint32_t *cp)` → bytes consumed; handles multi-byte sequences
- [ ] `utf8_strlen(const uint8_t *s)` → number of codepoints (not bytes)
- [ ] `utf8_next(const uint8_t **ptr)` → decode and advance pointer (iterator pattern)
- [ ] Keyboard event struct: replace `char ascii` with `uint32_t codepoint` + `uint8_t utf8[4]` (pre-encoded); ASCII keys map to codepoints U+0020–U+007E naturally
- [ ] `stb_truetype` codepoint pass-through: `gfx_draw_text_utf8(surface, x, y, str, len, color)` iterates codepoints via `utf8_next`, renders each glyph
- [ ] Commit: `"kernel: UTF-8/Unicode -- utf8_encode/decode, keyboard codepoint events, stb_truetype wire-up"`

## 4. Dead Key Compose `[Sonnet]`

Track a `pending_dead_cp` codepoint in keyboard state. When a dead key is struck, store its codepoint. On the next non-modifier key, look up `(dead_cp, base_cp)` in the active layout's `deadkeys[]` table; emit the composed codepoint or -- on miss -- emit the dead character followed by the base character.

**Files:** `src/kernel/drivers/keyboard.c`, `include/kernel/drivers/keyboard.h`

- [ ] Add `uint32_t pending_dead_cp` (0 = none) to `kbd_state_t`
- [ ] `kbd_translate()` extended: if translated codepoint is a dead key (look up `is_dead_key(cp)` flag in layout), set `pending_dead_cp = cp`; return `KBD_PENDING_DEAD` sentinel (no character emitted yet)
- [ ] On next non-modifier translation with `pending_dead_cp != 0`: search `g_active_layout->deadkeys[]` for `(pending_dead_cp, base_cp)`; if found, emit `composed_cp`; if not found, emit `pending_dead_cp` then `base_cp` as two separate codepoints
- [ ] Reset `pending_dead_cp = 0` after emission (composed or double-emit)
- [ ] Pressing a dead key twice emits the spacing version of the dead character (e.g., `^^` → `^` literal)
- [ ] Commit: `"drivers: dead key compose -- pending_dead_cp, (dead, base, composed) triple lookup, double-emit fallback"`

## 5. PS/2 Packet Resync `[Sonnet]`

Every PS/2 mouse packet byte 0 has bit 3 always set. Validate this on each packet boundary. On failure, discard bytes one-by-one (re-scan mode) until sync is re-established. Track sync loss count as a diagnostic counter.

**Files:** `src/kernel/drivers/mouse.c`

> [!NOTE]
> The re-scan approach: when the expected sync byte (index 0) fails the bit-3 check, do not discard the entire 3/4-byte window -- instead advance the ring buffer by one byte and retest. This converges to sync within at most 3/4 bytes regardless of where data corruption began.

- [ ] Add `uint32_t sync_loss_count` to `struct mouse_state`
- [ ] In packet accumulator: before storing byte 0, check `(byte & 0x08) != 0`; if not set, increment `sync_loss_count`, discard byte, stay at index 0 (re-scan mode)
- [ ] Once sync established (valid byte 0), accumulate bytes 1–2 (or 1–3 for 4-byte mode) normally
- [ ] Expose `sync_loss_count` via Registry read-only `HKLM\HARDWARE\Mouse\SyncLossCount` (updated on each loss)
- [ ] Boot log only on first loss per boot: `[MOUSE] Packet sync lost (total: %u)` at `LOG_DEBUG`
- [ ] Commit: `"drivers: PS/2 packet resync -- sync-bit validation, re-scan on loss, sync_loss_count"`

## 6. PS/2 Intellimouse Scroll Wheel (ID 3) `[Sonnet]`

Send the magic rate sequence (200→100→80 Hz) to the PS/2 mouse to unlock Intellimouse extension (ID 3). Switch to 4-byte packet parsing. Deliver the scroll delta as `WM_SCROLL` to the focused window.

**Files:** `src/kernel/drivers/mouse.c`, `include/kernel/drivers/mouse.h`

- [ ] `ps2_mouse_init()` extended: after basic reset/enable, send rate sequence `{ 200, 100, 80 }` via `0xF3`; read ID with `0xF2`; if ID=3 set `mouse_mode = MOUSE_INTELLIMOUSE`
- [ ] Switch packet parser to 4-byte mode when `mouse_mode == MOUSE_INTELLIMOUSE`; byte 3: bits 7:4 reserved, bits 3:0 = `int8_t scroll` (signed 4-bit, sign-extend)
- [ ] Add `int8_t scroll` to `struct mouse_state`
- [ ] Dispatch `WM_SCROLL(dx=0, dy=scroll * SCROLL_LINES_PER_NOTCH)` to focused window when `scroll != 0`; default `SCROLL_LINES_PER_NOTCH=3`, configurable via `HKLM\SYSTEM\Input\ScrollLinesPerNotch`
- [ ] Boot log: `[MOUSE] Intellimouse scroll wheel detected (ID=3)`
- [ ] Commit: `"drivers: PS/2 Intellimouse scroll wheel -- magic init, 4-byte packet, WM_SCROLL"`

## 7. Explorer 5-Button Extension (ID 4) `[Sonnet]`

A second magic sequence (200→200→80 Hz) after the ID-3 sequence upgrades to the Explorer extension (ID 4), adding two side buttons in the upper nibble of packet byte 3.

**Files:** `src/kernel/drivers/mouse.c`, `include/kernel/drivers/mouse.h`

- [ ] After ID-3 detection, send second rate sequence `{ 200, 200, 80 }` and re-read ID; if ID=4 set `mouse_mode = MOUSE_EXPLORER`
- [ ] In 4-byte parser (Explorer): byte 3 bits 4–5 = `BTN_SIDE` / `BTN_EXTRA`; bits 3:0 = scroll (same as ID 3)
- [ ] Define `MOUSE_BTN_SIDE (1<<3)` and `MOUSE_BTN_EXTRA (1<<4)` in `mouse.h`; include in `mouse_state.buttons`
- [ ] Dispatch `WM_MOUSEBUTTON(button=SIDE/EXTRA, action=DOWN/UP)` to focused window
- [ ] Boot log: `[MOUSE] Explorer 5-button mouse detected (ID=4)`
- [ ] Commit: `"drivers: PS/2 Explorer 5-button -- second magic sequence, side/extra buttons"`

## 8. Mouse Acceleration + Sensitivity `[Sonnet]`

Apply a configurable 16.16 fixed-point polynomial acceleration curve to relative delta inputs. Skip for absolute sources (VirtIO tablet, VBox absolute). Expose `MouseSensitivity` (0–200, default 100) in Registry; 100 = 1× multiplier, 200 = 2×.

**Files:** `src/kernel/drivers/mouse_accel.c` (new), `include/kernel/drivers/mouse_accel.h` (new), `src/kernel/drivers/mouse.c`

- [ ] Define `mouse_accel_t { uint32_t sensitivity_fp; uint32_t threshold_fp; uint32_t factor_fp; }` (all 16.16 fixed-point)
- [ ] `mouse_accel_apply(dx, dy, speed, accel)`: `speed = sqrt(dx*dx + dy*dy)` (integer approx); `accel = 1.0 + max(0, speed - threshold) * factor`; apply `sensitivity * accel` to both components
- [ ] Load from Registry at init: `sensitivity = HKLM\SYSTEM\Input\MouseSensitivity / 100` (default 1.0); `EnhancePointerPrecision` boolean enables/disables acceleration curve
- [ ] Skip in `mouse_process_event()` if `mouse_source == MOUSE_SRC_ABSOLUTE` (VBox/VirtIO) or if raw-grab is active (§9)
- [ ] `SYS_SET_MOUSE_SENSITIVITY(val)` syscall writes the Registry key and hot-reloads `mouse_accel`
- [ ] Commit: `"drivers: mouse acceleration -- 16.16 fixed-point curve, sensitivity Registry, absolute bypass"`

## 9. Raw Input Grab API `[Sonnet]`

`mouse_raw_grab(task)` hides the system cursor and routes all mouse deltas directly to the grabbing task as `WM_INPUT` messages. Required for first-person games and 3D viewport control. Release is triggered by the task or by ESC/Alt+F4 policy.

**Files:** `src/kernel/drivers/mouse.c`, `include/kernel/drivers/mouse.h`, `src/kernel/sched/syscall.c`

> [!IMPORTANT]
> Only one task may hold the raw grab at a time. A second `SYS_MOUSE_GRAB` call from a different task returns `STATUS_DEVICE_BUSY`. The kernel must automatically release the grab if the grabbing task exits (`task_exit()` calls `mouse_raw_release()`).

- [ ] Add `task_t *raw_grab_owner` to mouse driver state; protected by `spinlock_t raw_grab_lock`
- [ ] `mouse_raw_grab(task)`: if `raw_grab_owner != NULL && raw_grab_owner != task`, return `STATUS_DEVICE_BUSY`; set `raw_grab_owner = task`; hide cursor via `cursor_hide()`; warp pointer to screen centre
- [ ] `mouse_raw_release()`: clear `raw_grab_owner`; restore cursor visibility; re-centre pointer
- [ ] In `mouse_process_event()`: if `raw_grab_owner != NULL`, post `WM_INPUT { dx, dy, buttons, timestamp }` directly to `raw_grab_owner`'s message queue; skip normal pointer movement and click dispatch
- [ ] Register `SYS_MOUSE_GRAB` / `SYS_MOUSE_RELEASE` syscalls in native API dispatch
- [ ] `task_exit()` hook: if exiting task == `raw_grab_owner`, call `mouse_raw_release()`
- [ ] Commit: `"drivers: raw input grab -- WM_INPUT, cursor hide, task ownership, SYS_MOUSE_GRAB"`

## 10. Layout Switching -- Win+Space, Tray Indicator `[Sonnet]`

Win+Space cycles through installed layouts. A 2-letter indicator in the system tray (`EN`, `FR`, `DE`) updates immediately. Clicking the indicator opens a layout picker popup.

**Files:** `src/kernel/drivers/keyboard.c`, `src/desktop/` (tray indicator component)

> [!NOTE]
> → XREF: `09-desktop-shell` domain -- the system-tray indicator and picker popup are shell UI components; this section defines the kernel API (`kbd_get_layout()` / `kbd_set_layout()`) and the hotkey dispatch; the shell consumes the API.

- [ ] Win+Space hotkey: in global key handler, detect `VK_SPACE` with `MOD_WIN`; call `kbd_cycle_layout()` → advance index in registered layout list; wrap around
- [ ] `kbd_cycle_layout()` calls `kbd_set_layout(next_code)` and posts `WM_INPUT_LAYOUT_CHANGED(new_code)` to the compositor message queue
- [ ] Compositor handler: update system-tray 2-letter label to `g_active_layout->code[0..1]`; trigger tray repaint
- [ ] Tray indicator click: post `WM_SHOW_LAYOUT_PICKER` to shell; shell renders a popup list of `kbd_list_layouts()` entries with radio-button selection
- [ ] `kbd_list_layouts(buf, max)` -- fill array of `{ code, name }` structs from registered layout list
- [ ] Boot log: `[KBD] Active layout: %s (%s)`
- [ ] Commit: `"drivers: keyboard layout switching -- Win+Space cycle, WM_INPUT_LAYOUT_CHANGED, tray indicator"`

## 11. Sticky Keys + Typematic Rate `[Sonnet]`

Sticky Keys activates after 5 rapid consecutive Shift presses (< 500 ms each); modifier keys are latched until the next non-modifier key. PS/2 typematic delay and rate are programmed via command `0xF3` and configurable via Registry.

**Files:** `src/kernel/drivers/keyboard.c`, `include/kernel/drivers/keyboard.h`

- [ ] `sticky_keys_t { bool enabled; uint8_t shift_tap_count; uint64_t last_shift_tap_ns; uint32_t latched_mods; }` in `kbd_state_t`
- [ ] On each Shift key down: if `now - last_shift_tap_ns < 500_000_000` (500 ms), increment `shift_tap_count`; if `shift_tap_count >= 5`: `sticky_keys.enabled = true`; play notification sound stub; reset count
- [ ] In sticky mode: on modifier key down, latch modifier into `latched_mods`; on next non-modifier key, apply `latched_mods`, then clear `latched_mods`
- [ ] Deactivate sticky mode if two keys are held simultaneously (non-sticky key + modifier = intentional chord)
- [ ] Typematic delay/rate: at PS/2 init, send `0xF3` (Set Rate/Delay) + encoding byte; delay from `HKLM\SYSTEM\Input\TypematicDelayMs` (default 500 ms, valid: 250/500/750/1000); rate from `HKLM\SYSTEM\Input\TypematicRateHz` (default 10, valid 2–30)
- [ ] `typematic_encode(delay_ms, rate_hz)` → PS/2 byte `(delay_bits << 5) | rate_bits`
- [ ] Expose `SYS_SET_TYPEMATIC(delay_ms, rate_hz)` syscall; validates range before `0xF3` write
- [ ] Registry write from syscall: hot-apply without reboot
- [ ] Commit: `"drivers: sticky keys + typematic -- 5-tap Shift activation, latch, PS/2 0xF3 rate/delay"`

---

## OS Comparison


| ⭐  | Feature                                     | 🪟 Win11                                                 | 🐧 Linux                                                  | 🚀 Impossible OS                                                    |
| --- | ------------------------------------------- | -------------------------------------------------------- | --------------------------------------------------------- | ------------------------------------------------------------------- |
| 💎  | Intellimouse scroll wheel                   | ✅ `mouhid.sys`; `WM_MOUSEWHEEL`                         | ✅ `psmouse`; `INPUT_EV_REL` `REL_WHEEL`                  | ⬜ §6 -- magic init, 4-byte parser, `WM_SCROLL`                     |
| 💎  | Explorer 5-button (ID 4) side/extra buttons | ✅ `mouhid.sys`; `WM_XBUTTONDOWN`; `XBUTTON1`/`XBUTTON2` | ✅ `psmouse` Explorer; `BTN_SIDE`/`BTN_EXTRA`             | ⬜ §7 -- second magic sequence, `MOUSE_BTN_SIDE`/`EXTRA`            |
| 💎  | PS/2 packet resync on sync-bit failure      | ✅ `i8042prt.sys` sync recovery                          | ✅ `psmouse` resync logic; `psmouse_resync()`             | ⬜ §5 -- bit-3 validation, single-byte re-scan, `sync_loss_count`   |
| 💎  | Mouse acceleration + sensitivity curve      | ✅ Enhanced pointer precision; sensitivity slider        | ✅ `libinput` accel profiles (`adaptive`, `flat`)         | ⬜ §8 -- 16.16 fixed-point polynomial, `MouseSensitivity` Registry  |
| 💎  | Raw input grab                              | ✅ `WM_INPUT` + `SetCapture`; DirectInput raw            | ✅ `evdev` grab (`EVIOCGRAB`); `libinput` grab            | ⬜ §9 -- `mouse_raw_grab()`, `WM_INPUT`, `SYS_MOUSE_GRAB` syscall   |
| 💎  | Multi-plane keyboard layout                 | ✅ KTT layout files; `ToUnicodeEx`; full                 | ✅ `xkb` layouts; `evdev` key translation                 | ⬜ §1 -- `kbd_layout_t`, 3 planes + dead                            |
| 💎  | Built-in layouts -- 6 locales + Dvorak      | ✅ 100+ layouts via Windows Update                       | ✅ `xkb` symbols ships 200+ layouts                       | ⬜ §2 -- en-US, en-GB, de-DE, fr-FR, es-ES,                         |
| 💎  | Dead key compose -- accented characters     | ✅ `ToUnicodeEx` dead key state machine                  | ✅ `xkb` dead keys; `compose` table                       | ⬜ §4 -- `pending_dead_cp`, triple lookup, double-emit fallback     |
| 💎  | UTF-8 / Unicode codepoint keyboard output   | ✅ `WM_CHAR` sends UTF-16 codepoint; `wchar_t`           | ✅ `evdev` `EV_KEY` + `KEY_*`; `libinput`                 | ⬜ §3 -- `utf8_encode/decode`, `codepoint` in key event,            |
| 💎  | Layout switching -- hotkey + tray indicator | ✅ Win+Space / Win+Shift+Space; language bar             | ✅ `setxkbmap`; GNOME/KDE layout indicator in             | ⬜ §10 -- Win+Space cycle, `WM_INPUT_LAYOUT_CHANGED`, 2-letter tray |
| 💎  | Sticky keys + typematic rate / delay        | ✅ Accessibility Settings → Sticky Keys;                 | ✅ `xkb` `StickyKeys`; `typematic_rate` via `setkeycodes` | ⬜ §11 -- 5-tap Shift, latch modifier, PS/2                         |

> **After §1–11:** Impossible OS matches Windows 11 and Linux on the complete desktop input stack. No exclusive differentiators are claimed here -- correctness and parity are the goal. The notable design decision: the keyboard outputs Unicode codepoints natively (not scan codes or VK codes) from the driver layer up, matching how modern compositors expect to receive text input and eliminating the legacy ASCII transformation layer that Windows and Linux carry for backward compatibility.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] QEMU `-device ps2-mouse,id=mouse0 -usb` with Intellimouse emulation: boot log shows `[MOUSE] Intellimouse scroll wheel detected (ID=3)`; scrolling in a text window advances scroll position
- [ ] Packet resync: inject a corrupted byte via test path; `sync_loss_count` increments; next packets decode correctly
- [ ] Mouse acceleration: move mouse fast vs. slow; fast motion covers more screen pixels per physical unit
- [ ] Raw grab: `SYS_MOUSE_GRAB` → cursor hidden → mouse movements arrive as `WM_INPUT` → `SYS_MOUSE_RELEASE` restores cursor
- [ ] de-DE layout: `kbd_set_layout("de-DE")`; key Y produces `z`, key AltGr+E produces `€`; `` ` ``+`a` produces `à`
- [ ] Dead key compose: on de-DE, press `^` then `e` → `ê`; press `^` twice → literal `^`
- [ ] UTF-8 output: type `ü` on de-DE layout; `WM_CHAR` codepoint = `0x00FC`; UTF-8 bytes `0xC3 0xBC`
- [ ] Win+Space cycles from en-US → en-GB → de-DE → … → en-US; tray indicator updates to `"DE"` etc.
- [ ] Sticky Keys: tap Shift 5 times rapidly → Caps indicator shows latch; next letter is uppercase; then clears
- [ ] Typematic: `SYS_SET_TYPEMATIC(250, 30)` → PS/2 `0xF3` byte sent; held key repeats at ~30 Hz
- [ ] Commit: `"drivers: input system -- scroll, 5-btn, resync, accel, raw grab, layout, dead keys, UTF-8, sticky"`
