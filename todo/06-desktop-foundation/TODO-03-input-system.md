# TODO-03 — Input System

> **Goal:** Build a proper input routing system: modifier key tracking (Shift, Ctrl, Alt, Win), keyboard focus with Tab navigation between controls, and a global hotkey dispatch table. Currently input is ad-hoc — mouse clicks route through `wm_handle_mouse()` and keyboard goes directly to the terminal.

## Inputs

- [`src/desktop/wm.c`](../../src/desktop/wm.c) — `wm_handle_mouse()`, basic focus
- [`src/desktop/controls.c`](../../src/desktop/controls.c) — `ctrl_handle_key()`, `ctrl_handle_mouse()`
- [`src/kernel/drivers/keyboard.c`](../../src/kernel/drivers/keyboard.c) — PS/2 keyboard driver, scancode to ASCII

## Outcome

- Modifier keys tracked globally: Shift, Ctrl, Alt, Win (left + right variants)
- Keyboard events include modifier state: `key_event_t { scancode, ascii, modifiers }`
- Tab key cycles focus between controls in a window
- Ctrl+C / Ctrl+V wire to clipboard (future)
- Global hotkey table dispatches Win+D, Alt+Tab, etc. before window routing

## Implementation Order

| ⭐  | Order | Deliverable                               | Depends On | Status |
| --- | :---: | ----------------------------------------- | ---------- | :----: |
| 💎  |   1   | Modifier key tracking                     | —          |  [ ]   |
| 💎  |   2   | Key event struct + dispatch pipeline      | §1         |  [ ]   |
| 💎  |   3   | Focus model and Tab navigation            | §2         |  [ ]   |
| 💎  |   4   | Global hotkey dispatch table              | §2         |  [ ]   |

---

## 1. Modifier Key Tracking
Track press/release state of Shift, Ctrl, Alt, Win keys.

**Files:** `src/kernel/drivers/keyboard.c`, new `include/kernel/input.h`

- [ ] `g_modifiers` bitmap: `MOD_LSHIFT`, `MOD_RSHIFT`, `MOD_LCTRL`, `MOD_RCTRL`, `MOD_LALT`, `MOD_RALT`, `MOD_LWIN`, `MOD_RWIN`
- [ ] Update on key press (scancode make) and release (scancode break)
- [ ] `input_get_modifiers()` — query current modifier state
- [ ] Handle key repeat: modifier keys don't repeat
- [ ] Commit

**Test checkpoint:** Hold Shift, type 'a' — terminal shows 'A'. Release Shift — lowercase again.

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
- [ ] `wm_register_hotkey(scancode, modifiers, handler)` — add to table
- [ ] Table checked first in `input_dispatch()` — if match, call handler and consume event
- [ ] Register defaults: Alt+Tab, Alt+F4, Win+D, Win+L, PrintScreen
- [ ] Commit

**Test checkpoint:** Alt+F4 closes window. PrintScreen captures to file (placeholder log).

---

## OS Comparison

| ⭐ | Feature          | Win11            | Linux (Wayland)  | Impossible OS        |
|----|------------------|------------------|------------------|----------------------|
| 💎 | Modifier tracking | ✅ Full          | ✅ Full          | ⬜ §1                |
| 💎 | Tab navigation   | ✅ Built-in      | ✅ Built-in      | ⬜ §3                |
| 💎 | Global hotkeys   | ✅ RegisterHotKey | ✅ XGrabKey     | ⬜ §4                |
