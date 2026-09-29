---
schema_version: 1
id: input-system
domain: 06-desktop-foundation
status: active
title: "TODO-03 -- Input System"
---

# TODO-03 -- Input System

> **Goal:** Build a proper input routing system: modifier key tracking (Shift, Ctrl, Alt, Win), keyboard focus with Tab navigation between controls, and a global hotkey dispatch table. Currently input is ad-hoc -- mouse clicks route through `wm_handle_mouse()` and keyboard goes directly to the terminal.

## Inputs

- [`src/desktop/wm.c`](../../src/desktop/wm.c) -- `wm_handle_mouse()`, basic focus
- [`src/desktop/controls.c`](../../src/desktop/controls.c) -- `ctrl_handle_key()`, `ctrl_handle_mouse()`
- [`src/kernel/drivers/keyboard.c`](../../src/kernel/drivers/keyboard.c) -- PS/2 keyboard driver, scancode to ASCII
- -> XREF: `D08 T07 §4` -- IME and composition work builds on the key-event and focus-routing pipeline owned here

## Outcome

- Modifier keys tracked globally: Shift, Ctrl, Alt, Win (left + right variants)
- Keyboard events include modifier state: `key_event_t { scancode, ascii, modifiers }`
- Tab key cycles focus between controls in a window
- Ctrl+C / Ctrl+V wire to clipboard (future)
- Global hotkey table dispatches Win+D, Alt+Tab, etc. before window routing

## Implementation Order

| ⭐  | Order | Deliverable                           | Depends On | Status |
| --- | :---: | ------------------------------------- | ---------- | :----: |
| 💎  |   1   | Modifier key tracking                 | --         |  [ ]   |
| 💎  |   2   | Key event struct + dispatch pipeline  | §1         |  [ ]   |
| 💎  |   3   | Focus model and Tab navigation        | §2         |  [ ]   |
| 💎  |   4   | Global hotkey dispatch table          | §2         |  [ ]   |
| 💎  |   5   | Bare-metal input bugs (focus + mouse) | §2, §3     |  [ ]   |

---

## 1. Modifier Key Tracking
Track press/release state of Shift, Ctrl, Alt, Win keys.

**Files:** `src/kernel/drivers/keyboard.c`, new `include/kernel/input.h`

- [ ] `g_modifiers` bitmap: `MOD_LSHIFT`, `MOD_RSHIFT`, `MOD_LCTRL`, `MOD_RCTRL`, `MOD_LALT`, `MOD_RALT`, `MOD_LWIN`, `MOD_RWIN`
- [ ] Update on key press (scancode make) and release (scancode break)
- [ ] `input_get_modifiers()` -- query current modifier state
- [ ] Handle key repeat: modifier keys don't repeat
- [ ] Commit

**Test checkpoint:** Hold Shift, type 'a' -- terminal shows 'A'. Release Shift -- lowercase again.

## 2. Key Event Struct + Dispatch Pipeline
Route keyboard events through a unified pipeline: global hotkeys → focused window → focused control.

**Files:** `include/kernel/input.h` (new), `src/desktop/wm.c`

- [ ] `struct key_event { uint8_t scancode; char ascii; uint16_t modifiers; uint8_t pressed; }`
- [ ] Keyboard ISR builds `key_event` and pushes to event queue
- [ ] `input_dispatch(key_event)`: global hotkeys → WM → focused window → focused control → terminal
- [ ] If no consumer handles the event, drop it
- [ ] Commit

**Test checkpoint:** Keyboard input reaches terminal via dispatch pipeline (same behavior as before, but through new path).

## 3. Focus Model and Tab Navigation
Tab key cycles focus between controls within the focused window.

**Files:** `src/desktop/controls.c`

- [ ] Controls have a `tab_order` field (auto-assigned on creation, or explicit)
- [ ] Tab: focus next control in tab order; Shift+Tab: focus previous
- [ ] Focused control gets visual indicator (accent border or highlight)
- [ ] Enter key on focused button → trigger click callback
- [ ] Escape key → close dialog / cancel
- [ ] Commit

**Test checkpoint:** Open gallery → Tab cycles between button, textbox, scrollbar. Enter on button triggers click.

## 4. Global Hotkey Dispatch Table
Register system-wide hotkeys that intercept before window routing.

**Files:** `src/desktop/wm.c`, `include/desktop/wm.h`

- [ ] `struct hotkey { uint8_t scancode; uint16_t modifiers; void (*handler)(void); }`
- [ ] `wm_register_hotkey(scancode, modifiers, handler)` -- add to table
- [ ] Table checked first in `input_dispatch()` -- if match, call handler and consume event
- [ ] Register defaults: Alt+Tab, Alt+F4, Win+D, Win+L, PrintScreen
- [ ] Reconcile with `08-graphics-ui/TODO-08` §5, which also plans a hotkey table (`hotkeys.c`, `uint8_t` modifiers): keep one table and one modifier contract, with the other roadmap registering through it
  - `06-desktop-foundation/TODO-01` §4 already names TODO-08 §5 as the hotkey owner; this section's table would be a second one
  - Found by the `00-infrastructure/TODO-10-documentation-site.md` §16 review; verified at source
- [ ] Commit

**Test checkpoint:** Alt+F4 closes window. PrintScreen captures to file (placeholder log).

## 5. Bare-Metal Input Bugs (Focus + Mouse)
Fix two input issues observed on bare metal (i5-4210U laptop, 2026-03-31):

**Files:** `src/desktop/wm.c`, `src/desktop/terminal.c`, `src/kernel/drivers/mouse.c`

### Bug A: Mouse pointer jumps when crossing window boundaries
The cursor visibly jumps or moves irregularly when it moves over a window. Likely cause: compositor hit-test or coordinate translation has an off-by-one or stale-rect issue at window edges. The WM may be switching between "window drag" and "desktop" coordinate spaces incorrectly.

- [ ] Audit `wm_handle_mouse()` -- check hit-test at window boundary transitions
- [ ] Check if mouse delta is being applied twice (raw + compositor) during window crossings
- [ ] Verify cursor coordinates are clamped to screen bounds during fast movement
- [ ] Test: move mouse smoothly across window edges -- no jumps

### Bug B: Keyboard input delayed or requires click on Command Prompt
When the terminal window is visually focused, typing either appears after a 1-2 second delay, or doesn't appear until the user clicks on the terminal. Likely cause: keyboard events are not being routed to the terminal because the WM focus state doesn't match the visual state, or the terminal's key handler isn't being called on every key event.

- [ ] Audit keyboard event routing: today the ISR sends every key to `terminal_key_input()` whenever `terminal_is_open()` (`keyboard.c`), with no WM focus check; route through the §2 pipeline
- [ ] Check if `wm_get_focused_window()` returns the terminal after it's created (may lose focus to gallery)
- [ ] Check if terminal has a key event handler registered with the WM
- [ ] Verify no event queue overflow or stale event coalescing drops keystrokes
- [ ] Test: type immediately after boot without clicking -- text appears instantly

- [ ] Clear the terminal's handle when its window is destroyed by the caption close button or Alt+F4: only `terminal_close()` resets `term_handle` (`terminal.c:128-133`)
  - Afterwards `terminal_is_open()` stays 1, keys still route to the dead terminal (`keyboard.c:286-292`), and Start > Terminal cannot reopen it (`terminal.c:88-89`); a candidate cause of Bug B
  - Found while writing the docs pages (`00-infrastructure/TODO-10-documentation-site.md` §16); verified at source, not reproduced at runtime
- [ ] Commit: `"desktop: fix bare-metal input bugs -- mouse jump + keyboard focus"`

**Test checkpoint:** Mouse moves smoothly across window edges on bare metal. Typing in terminal works immediately without clicking. Test on: bare metal i5-4210U, QEMU.

**Regression risk:** LOW -- input routing changes. If focus breaks, windows stop receiving events. Rollback: revert to current ad-hoc routing.

---

## OS Comparison

| ⭐  | Feature           | 🪟 Win11          | 🐧 Linux (Wayland) | 🚀 Impossible OS |
| --- | ----------------- | ----------------- | ------------------ | ---------------- |
| 💎  | Modifier tracking | ✅ Full           | ✅ Full            | ⬜ §1            |
| 💎  | Tab navigation    | ✅ Built-in       | ✅ Built-in        | ⬜ §3            |
| 💎  | Global hotkeys    | ✅ RegisterHotKey | ✅ XGrabKey        | ⬜ §4            |
