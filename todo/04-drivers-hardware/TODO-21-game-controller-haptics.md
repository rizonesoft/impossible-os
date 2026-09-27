---
schema_version: 1
id: game-controller-haptics
domain: 04-drivers-hardware
status: active
title: "TODO-21 -- Game Controllers, HID Force Feedback & Haptics"
---

# TODO-21 -- Game Controllers, HID Force Feedback & Haptics

> **Goal:** Support modern game controllers and haptic devices: USB HID gamepads, XInput-compatible pads, Bluetooth controllers, force feedback, LEDs, battery reporting, low-latency event routing, calibration, and per-device profiles. This complements keyboard/mouse input without forcing controllers through ad hoc HID paths.
> **Current state:** Input TODOs cover keyboard/mouse, raw input, USB HID boot devices, touchpads, and Bluetooth HID. There is no game-controller abstraction, no haptics/force-feedback output path, no controller battery/profile model, and no compatibility layer for XInput-style games.

## Inputs

- -> XREF: `TODO-11-input-system.md` -- input dispatch and raw-grab policy
- -> XREF: `TODO-10-usb-stack.md` -- USB HID and interrupt endpoints
- -> XREF: `TODO-16-bluetooth.md §5` -- Bluetooth HID profile
- -> XREF: `12-user-platform-sdk` -- XInput/DirectInput-style user APIs

## Outcome

- Game controllers register as first-class devices with axes, buttons, hats, triggers, battery, LEDs, and rumble.
- USB and Bluetooth controllers share one mapping/profile layer.
- Games get stable XInput-compatible ordering and hot-plug events.

## Implementation Order

| Priority  | Order | Deliverable                            | Depends On  | Status |
| --------- | :---: | -------------------------------------- | ----------- | :----: |
| Parity    |   1   | Controller class API                   | TODO-11     |  [ ]   |
| Parity    |   2   | HID gamepad parser and mapper          | §1, TODO-10 |  [ ]   |
| Parity    |   3   | XInput-compatible profile layer        | §1, SDK     |  [ ]   |
| Parity    |   4   | Bluetooth controller support           | §1, TODO-16 |  [ ]   |
| Parity    |   5   | Force feedback and rumble output       | §1-§4       |  [ ]   |
| Parity    |   6   | Battery, LED, and player index support | §1          |  [ ]   |
| Parity    |   7   | Calibration and dead-zone profiles     | §1          |  [ ]   |
| Parity    |   8   | Hot-plug and low-latency event routing | §1, TODO-11 |  [ ]   |
| Exclusive |   9   | Controller diagnostics panel           | §1-§8       |  [ ]   |
| Parity    |  10   | Controller test matrix                 | §1-§9       |  [ ]   |

## 1. Controller Class API

- [ ] Define `controller_device_t`, axes/buttons/hats/triggers, and event packet ABI.
- [ ] Register controller devices separately from mouse/keyboard.
- [ ] Commit: `"drivers/input: controller class API"`

## 2. HID Gamepad Parser

- [ ] Parse Generic Desktop Game Pad/Joystick usages.
- [ ] Normalize axes to signed 16-bit and triggers to unsigned 8/16-bit.
- [ ] Commit: `"drivers/input: HID gamepad parser"`

## 3. XInput-Compatible Profiles

- [ ] Map common Xbox-compatible layouts to stable player slots.
- [ ] Expose profile ids for user-mode APIs.
- [ ] Commit: `"drivers/input: XInput controller profiles"`

## 4. Bluetooth Controllers

- [ ] Accept Bluetooth HID gamepad reports from TODO-16.
- [ ] Preserve reconnect identity and battery state.
- [ ] Commit: `"drivers/input: Bluetooth controller path"`

## 5. Force Feedback and Rumble

- [ ] Add output-report path for rumble and simple haptics.
- [ ] Fail gracefully for unsupported devices.
- [ ] Commit: `"drivers/input: controller rumble output"`

## 6. Battery, LED and Player Index

**Design:** n/a -- no desktop UI surface (developer tooling, data export, CLI or device plumbing)

- [ ] Query battery where supported.
- [ ] Drive player LEDs and lightbar policy where supported.
- [ ] Commit: `"drivers/input: controller battery and LEDs"`

## 7. Calibration and Profiles

- [ ] Store dead zones, axis inversion, and calibration in Registry.
- [ ] Add per-device defaults.
- [ ] Commit: `"drivers/input: controller calibration profiles"`

## 8. Hot-Plug and Latency

- [ ] Publish controller add/remove events.
- [ ] Keep event path allocation-free after open.
- [ ] Commit: `"drivers/input: low latency controller events"`

## 9. Diagnostics

- [ ] Add `joy.cpl`/diagnostic shell stub ownership and Device Manager details.
- [ ] Show live axes, buttons, battery, LEDs, and rumble test.
- [ ] Commit: `"drivers/input: controller diagnostics"`

## 10. Tests

- [ ] HID descriptor fixtures for Xbox, DualShock/DualSense-compatible, generic gamepads.
- [ ] Bare-metal USB and Bluetooth controller matrix.
- [ ] Commit: `"test: game controller drivers"`

## OS Comparison

| Priority  | Feature             | Windows         | Linux           | Impossible OS |
| --------- | ------------------- | --------------- | --------------- | ------------- |
| Parity    | Gamepad HID         | HIDClass/XInput | hid/input/evdev | TODO-21       |
| Parity    | Rumble              | XInput FF       | ff-memless      | TODO-21 §5    |
| Exclusive | Unified live tester | joy.cpl partial | evtest/jstest   | TODO-21 §9    |

