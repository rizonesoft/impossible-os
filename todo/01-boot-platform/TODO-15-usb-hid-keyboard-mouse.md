# TODO-15 — USB HID Boot-Protocol Keyboard & Mouse

> **Goal:** USB keyboards and mice work during boot and at the desktop on systems without PS/2 hardware. Most modern laptops and desktops (post-2015) have only USB input. Without this, the OS is unusable on real hardware. This TODO implements USB HID boot-protocol drivers for keyboard and mouse, interrupt endpoint polling, and input source coexistence with existing PS/2 drivers. The result: type commands, move the cursor, and click on any system with USB input.

> [!IMPORTANT]
> **Current state:** TODO-07 §6–§9 scoped USB HID but no code exists. PS/2 keyboard and mouse work on hardware that has i8042 (detected via ACPI FADT). Modern laptops without i8042 show `"PS/2 keyboard: skipped (no i8042 in FADT)"` and have zero input. xHCI interrupt endpoint setup, HID report parsing, and input routing are all unimplemented.

---

## Inputs

- `src/kernel/drivers/xhci.c` — xHCI controller (needs interrupt endpoint support)
- `src/kernel/drivers/xhci_ring.c` — TRB ring management
- `src/kernel/drivers/keyboard.c` — PS/2 keyboard driver (input sink interface)
- `src/kernel/drivers/mouse.c` — PS/2 mouse driver (input sink interface)
- `include/kernel/drivers/keyboard.h` — keyboard API (`keyboard_trygetchar()`)
- → XREF: `TODO-07-xhci-usb-boot.md §6–§9` — scoped but unimplemented
- → XREF: `04-drivers-hardware/TODO-05-input-system.md` — unified input system (downstream consumer)

---

## Outcome

- USB keyboards produce keystrokes readable via `keyboard_trygetchar()` — shell works.
- USB mice produce movement/button events readable by the compositor.
- Both coexist with PS/2 input — whatever hardware is present works.
- Boot-protocol mode (no HID report descriptor parsing needed for basic operation).
- Works on QEMU with `-device usb-kbd` and on bare metal USB keyboards.

---

## Implementation Order

| ⭐  | Order | Deliverable                                     | Depends On | Status |
| --- | :---: | ----------------------------------------------- | ---------- | :----: |
| 💎  |   1   | xHCI interrupt endpoint setup                   | —          |  [ ]   |
| 💎  |   2   | Interrupt transfer polling (periodic TRBs)       | §1         |  [ ]   |
| 💎  |   3   | USB HID boot-protocol keyboard driver            | §2         |  [ ]   |
| 💎  |   4   | USB HID boot-protocol mouse driver               | §2         |  [ ]   |
| 💎  |   5   | Input source coexistence (PS/2 + USB)            | §3, §4     |  [ ]   |
| ⭐  |   6   | Hot-plug keyboard/mouse detection                | §5         |  [ ]   |
| ⭐  |   7   | USB input diagnostic logging                     | §1–§6      |  [ ]   |

> 💎 = parity — Windows HID minidriver and Linux usbhid both provide boot-protocol keyboard/mouse.
> ⭐ = exclusive — hot-plug keyboard detection and diagnostic logging.

---

## 1. xHCI Interrupt Endpoint Setup

Add interrupt endpoint support to the xHCI driver (currently only bulk endpoints for MSC).

- [ ] In `xhci_dev.c`: after SET_CONFIGURATION, find interrupt IN endpoints in the interface descriptor
- [ ] HID interfaces: class=0x03, subclass=0x01 (boot), protocol=0x01 (keyboard) or 0x02 (mouse)
- [ ] Configure interrupt endpoint: allocate Transfer Ring, set interval from endpoint descriptor `bInterval`
- [ ] Add endpoint to device's slot context via Configure Endpoint command
- [ ] Log: `"[USB] HID %s found on port %u (EP%u, interval=%ums)"` with keyboard/mouse
- [ ] Commit: `"drivers: xHCI interrupt endpoint setup for USB HID devices"`

**Test checkpoint:** QEMU with `-device usb-kbd` — serial shows `"HID keyboard found on port X"`. No input yet.

---

## 2. Interrupt Transfer Polling

Set up periodic interrupt transfers to receive HID reports from keyboard/mouse.

- [ ] Queue Normal TRBs on the interrupt endpoint's Transfer Ring
- [ ] Poll the interrupt endpoint's completion: check Event Ring for Transfer Event TRBs matching the endpoint
- [ ] Timer-driven polling: check every 10ms from the LAPIC timer tick callback
- [ ] On completion: extract the HID report data from the TRB's data buffer
- [ ] Re-queue a new TRB after processing each report (continuous polling)
- [ ] Commit: `"drivers: xHCI interrupt transfer polling for HID report delivery"`

**Test checkpoint:** Press keys on USB keyboard in QEMU — Transfer Event TRBs arrive, raw report data logged to serial.

---

## 3. USB HID Boot-Protocol Keyboard Driver

Parse boot-protocol keyboard reports (8 bytes) into keystrokes.

- [ ] Boot-protocol keyboard report format: `[modifier, reserved, key1, key2, key3, key4, key5, key6]`
- [ ] Modifier byte: bits for L/R Ctrl, Shift, Alt, GUI
- [ ] Key bytes: USB HID usage IDs (0x04=A, 0x05=B, ..., 0x27=0, 0x28=Enter, 0x2A=Backspace)
- [ ] Convert USB HID usage IDs to ASCII via lookup table (same as PS/2 scancode→ASCII)
- [ ] Feed characters into `keyboard_buffer[]` (same ring buffer as PS/2 keyboard)
- [ ] `keyboard_trygetchar()` returns characters from either PS/2 or USB — transparent to callers
- [ ] SET_PROTOCOL(0) to force boot-protocol mode (some keyboards default to report-protocol)
- [ ] SET_IDLE(0) to suppress duplicate reports when no keys change
- [ ] Commit: `"drivers: USB HID boot-protocol keyboard — type in shell via USB keyboard"`

**Test checkpoint:** QEMU with `-device usb-kbd` — type `dir` at the `C:\>` prompt, output appears. Also test on bare metal USB keyboard.

**Regression risk:** LOW — additive driver. PS/2 keyboard continues to work. If USB keyboard produces garbage, disconnect and use PS/2.

---

## 4. USB HID Boot-Protocol Mouse Driver

Parse boot-protocol mouse reports (3–4 bytes) into cursor movement.

- [ ] Boot-protocol mouse report format: `[buttons, dx, dy]` (3 bytes) or `[buttons, dx, dy, wheel]` (4 bytes)
- [ ] buttons: bit 0=left, bit 1=right, bit 2=middle
- [ ] dx/dy: signed 8-bit relative movement
- [ ] Feed into mouse event system: `mouse_update_relative(dx, dy, buttons)`
- [ ] Compositor picks up events the same way as PS/2 mouse — transparent
- [ ] SET_PROTOCOL(0) to force boot-protocol mode
- [ ] Commit: `"drivers: USB HID boot-protocol mouse — cursor movement via USB mouse"`

**Test checkpoint:** QEMU with `-device usb-mouse` — mouse cursor moves on desktop. Click works on window title bars.

---

## 5. Input Source Coexistence

Both PS/2 and USB input should work simultaneously without conflicts.

- [ ] `keyboard_trygetchar()` checks PS/2 buffer first, then USB buffer — returns first available
- [ ] Mouse events merged: PS/2 and USB mouse both update the same cursor position
- [ ] If PS/2 is absent (no i8042): only USB input is active — no probing of missing hardware
- [ ] If USB HID is absent: only PS/2 input is active — no change from current behavior
- [ ] VirtIO tablet input (QEMU) continues to work alongside USB/PS/2
- [ ] Commit: `"drivers: input source coexistence — PS/2, USB, VirtIO work together"`

**Test checkpoint:** QEMU with VirtIO tablet + USB keyboard — both work simultaneously. Bare metal with PS/2 keyboard + USB mouse — both work.

---

## 6. Hot-Plug Keyboard/Mouse Detection

Detect USB keyboard/mouse plugged in after boot.

- [ ] xHCI Port Status Change Events (already in Event Ring) trigger port scan
- [ ] New device on port: enumerate → if HID, configure interrupt endpoint → start polling
- [ ] Device removed: stop polling, clean up endpoint ring
- [ ] Log: `"[USB] Hot-plug: %s on port %u"` / `"[USB] Removed: port %u"`
- [ ] Commit: `"drivers: USB HID hot-plug detection — keyboard/mouse plug-and-play"`

**Test checkpoint:** Boot without USB keyboard. Plug in USB keyboard after desktop appears — typing works within 1 second.

---

## 7. USB Input Diagnostic Logging

Comprehensive USB input status in serial log.

- [ ] During boot: `"[INPUT] Sources: PS/2=%s USB_KBD=%s USB_MOUSE=%s VIRTIO=%s"` (yes/no for each)
- [ ] Per USB HID device: vendor ID, product ID, protocol (keyboard/mouse), endpoint interval
- [ ] Error statistics: missed reports, endpoint errors, retry counts
- [ ] Commit: `"drivers: USB input diagnostic logging"`

**Test checkpoint:** Serial output shows input source summary at boot.

---

## OS Comparison

| ⭐ | Feature                   | 🪟 Win11                    | 🐧 Linux                     | 🚀 Impossible OS              |
|----|---------------------------|--------------------------|---------------------------|----------------------------|
| 💎 | USB keyboard in boot      | ✅ HID minidriver        | ✅ usbhid + usbkbd       | ⬜ §3                      |
| 💎 | USB mouse in boot         | ✅ HID minidriver        | ✅ usbhid + usbmouse     | ⬜ §4                      |
| 💎 | PS/2 + USB coexistence    | ✅ Automatic             | ✅ Automatic              | ⬜ §5                      |
| 💎 | USB HID hot-plug          | ✅ PnP Manager           | ✅ udev + usbhid          | ⬜ §6                      |
| ⭐ | Input source diagnostics  | ❌ Device Manager only   | ❌ dmesg only             | ⬜ §7 🚀                   |

After §1–§5, USB input is at parity with Windows and Linux. §6–§7 add hot-plug and diagnostics.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_usb_hid()` (XREF: `00-infrastructure/TODO-03-kernel-test-framework.md`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_usb_hid.c` with:
  - Boot-protocol keyboard report parsing: `[0x00, 0x00, 0x04, 0,0,0,0,0]` (modifier=0, key=A) produces ASCII `'a'`
  - Modifier key handling: `[0x02, 0x00, 0x04, 0,0,0,0,0]` (L_SHIFT + A) produces ASCII `'A'`
  - Multiple simultaneous keys: report with key1=0x04, key2=0x05 produces both `'a'` and `'b'`
  - Key release detection: report with all zeroes after keypress produces no new characters
  - Boot-protocol mouse report parsing: `[0x01, 0x0A, 0xF6]` (left button, dx=10, dy=-10) produces correct relative movement
  - HID interface classification: class=0x03, subclass=0x01, protocol=0x01 identified as keyboard
  - HID interface classification: class=0x03, subclass=0x01, protocol=0x02 identified as mouse
  - `keyboard_trygetchar()` returns characters from USB source when PS/2 buffer is empty (§5 coexistence)
- [ ] Register in `test_runner_init()`: `test_register_usb_hid()`
- [ ] Add smoke test for QEMU USB keyboard: extend `scripts/test-smoke.sh` to check serial line `"[INPUT] Sources:"` present (§7 — input diagnostic summary)
- [ ] Commit: `"test: add USB HID keyboard and mouse test suite"`

## Verification

- [ ] **QEMU USB keyboard**: `-device usb-kbd` → type commands in shell.
- [ ] **QEMU USB mouse**: `-device usb-mouse` → move cursor, click windows.
- [ ] **Bare metal USB keyboard**: type commands on real USB keyboard.
- [ ] **PS/2 + USB coexistence**: both input sources work simultaneously.
- [ ] **No PS/2 system**: laptop with no i8042 → USB keyboard is the only input → works.
- [ ] Commit: `"boot: USB HID keyboard and mouse complete — input on any hardware"`
