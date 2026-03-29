# TODO-09 — USB HID Input (Boot-Critical)

> **Goal:** Enable USB keyboard and mouse input on systems without PS/2 controllers. Most modern laptops and desktops use USB-connected keyboards and mice — without this driver, the OS has no input on those systems.

> [!IMPORTANT]
> This TODO extracts the **boot-critical** USB HID sections from `04-drivers-hardware/TODO-09-usb-stack.md §1-§2`. Advanced USB HID features (report descriptor parsing, multi-device, hot-plug) remain in TODO-09. After this TODO, USB keyboards and mice work in boot-protocol mode.

> [!NOTE]
> USB HID boot protocol is deliberately simple: 8-byte keyboard reports, 3-byte mouse reports. No report descriptor parsing needed. This covers ~95% of USB keyboards and mice. Complex HID devices (gaming keyboards, multi-button mice) need the full report descriptor parser in TODO-09.

## Inputs

- [`src/kernel/drivers/xhci.c`](../../src/kernel/drivers/xhci.c) — xHCI controller
- [`src/kernel/drivers/xhci_dev.c`](../../src/kernel/drivers/xhci_dev.c) — device enumeration
- [`src/kernel/drivers/keyboard.c`](../../src/kernel/drivers/keyboard.c) — PS/2 keyboard (injection target)
- [`src/kernel/drivers/mouse.c`](../../src/kernel/drivers/mouse.c) — PS/2 mouse (injection target)
- → XREF: `01-boot-platform/TODO-07-xhci-usb-boot.md §1-§2` — xHCI controller must be operational first
- → XREF: `04-drivers-hardware/TODO-09-usb-stack.md §1` — interrupt endpoint setup
- → XREF: `04-drivers-hardware/TODO-09-usb-stack.md §2` — full USB HID class driver
- → XREF: `04-drivers-hardware/TODO-09-usb-stack.md §7` — PS/2 ↔ USB input fallback

## Outcome

- USB keyboards produce key events in the input subsystem
- USB mice produce mouse events (movement, buttons)
- PS/2 and USB input coexist — both active if both present
- Systems without PS/2 (USB-only) have working keyboard and mouse

## Implementation Order

| ⭐  | Order | Deliverable                                    | Depends On | Status |
| --- | :---: | ---------------------------------------------- | ---------- | :----: |
| 💎  |   1   | xHCI interrupt endpoint setup for HID          | TODO-07 §2 |  [ ]   |
| 💎  |   2   | USB HID boot-protocol keyboard driver          | §1         |  [ ]   |
| 💎  |   3   | USB HID boot-protocol mouse driver             | §1         |  [ ]   |
| 💎  |   4   | Input source priority and coexistence          | §2, §3     |  [ ]   |

---

## 1. xHCI Interrupt Endpoint Setup for HID
Configure interrupt-IN endpoints for HID devices so the xHCI controller polls them periodically.

**Files:** `src/kernel/drivers/xhci_dev.c`

- [ ] Detect HID interface (class 0x03) during USB enumeration
- [ ] `SET_PROTOCOL(0)` — switch to boot protocol (simpler 8-byte reports)
- [ ] `SET_IDLE(0)` — report only on change
- [ ] Configure interrupt-IN endpoint via Configure Endpoint command
- [ ] Set up Transfer Ring with TRBs for periodic interrupt-IN transfers
- [ ] Event ring callback: when interrupt-IN completes, deliver report to HID driver
- [ ] Commit: `"drivers: xHCI interrupt endpoint setup for USB HID devices"`

**Test checkpoint:** USB keyboard/mouse detected, interrupt endpoint configured. POST code 0xD900.

## 2. USB HID Boot-Protocol Keyboard Driver
Parse 8-byte boot-protocol keyboard reports and inject key events into the input subsystem.

**Files:** `src/kernel/drivers/usb_hid_kbd.c` (new)

- [ ] Parse boot keyboard report: byte 0 = modifiers (Ctrl/Shift/Alt/GUI), bytes 2-7 = keycodes
- [ ] Convert USB HID usage codes to PS/2 scancodes (lookup table, ~104 entries)
- [ ] Inject into `keyboard_handle_scancode()` — same path as PS/2
- [ ] Handle key-up detection: compare current report with previous, detect released keys
- [ ] Log: `input: USB keyboard initialized (boot protocol)`
- [ ] Commit: `"drivers: USB HID boot-protocol keyboard — key events via interrupt-IN"`

**Test checkpoint:** Plug USB keyboard, type characters, see them in terminal. POST code 0xD901.

## 3. USB HID Boot-Protocol Mouse Driver
Parse 3-byte boot-protocol mouse reports and inject mouse events.

**Files:** `src/kernel/drivers/usb_hid_mouse.c` (new)

- [ ] Parse boot mouse report: byte 0 = buttons, byte 1 = X delta (signed), byte 2 = Y delta (signed)
- [ ] Inject into mouse subsystem via `mouse_handle_event(dx, dy, buttons)`
- [ ] Log: `input: USB mouse initialized (boot protocol)`
- [ ] Commit: `"drivers: USB HID boot-protocol mouse — movement + buttons"`

**Test checkpoint:** Plug USB mouse, cursor moves on screen. POST code 0xD902.

## 4. Input Source Priority and Coexistence
Ensure PS/2 and USB input sources coexist without conflict.

**Files:** `src/kernel/drivers/keyboard.c`, `src/kernel/drivers/mouse.c`

- [ ] Both PS/2 and USB keyboard active simultaneously (no conflict — both inject scancodes)
- [ ] Both PS/2 and USB mouse active simultaneously
- [ ] If PS/2 not detected (FADT `acpi_has_8042()` = 0): USB is sole input source
- [ ] Log active input sources at boot: `input: sources: PS/2 keyboard, USB mouse`
- [ ] Commit: `"drivers: input source coexistence — PS/2 + USB active simultaneously"`

**Test checkpoint:** System with both PS/2 and USB keyboard — both work. System with only USB — keyboard works. POST code 0xD903.

---

## OS Comparison

| ⭐ | Feature                 | Win11                       | Linux                        | Impossible OS                    |
|----|-------------------------|-----------------------------|------------------------------|----------------------------------|
| 💎 | USB HID keyboard       | ✅ hidusb.sys + kbdhid.sys   | ✅ usbhid + hid-generic      | ⬜ §2 — boot protocol            |
| 💎 | USB HID mouse          | ✅ hidusb.sys + mouhid.sys   | ✅ usbhid + hid-generic      | ⬜ §3 — boot protocol            |
| 💎 | PS/2 + USB coexist     | ✅ Automatic                 | ✅ Automatic                  | ⬜ §4 — priority chain           |

## Verification

- [ ] USB keyboard: type in terminal on bare metal
- [ ] USB mouse: cursor moves on bare metal
- [ ] PS/2-only system: still works (no regression)
