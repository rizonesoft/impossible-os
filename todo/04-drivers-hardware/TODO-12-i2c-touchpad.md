---
schema_version: 1
id: i2c-touchpad
domain: 04-drivers-hardware
status: active
title: "TODO-12 -- I2C/SMBus Bus & Precision Touchpad"
---

# TODO-12 -- I2C/SMBus Bus & Precision Touchpad

> **Goal:** Add an I2C/SMBus host controller driver, ACPI I2C device enumeration, HID-over-I2C transport, HID report descriptor parser, Microsoft Precision Touchpad (PTP) multi-touch support, a gesture engine, Synaptics/ELAN PS/2 touchpad driver, vendor quirks, a touchpad control panel, and diagnostic shell commands -- making Impossible OS a first-class laptop OS.
>
> → **Loadable module** -- touchpads are not boot-critical. The kernel boots with PS/2 keyboard plus the boot-platform USB HID path (`01-boot-platform/TODO-18-usb-hid-keyboard-mouse.md`). This driver loads from the filesystem after C:\ is mounted.

> [!IMPORTANT]
> **No I2C, HID-over-I2C, or touchpad infrastructure exists.** This is a greenfield driver stack. The correct build order is: I2C bus (§1) → Synaptics PS/2 fallback (§2) → ACPI device enumeration (§3) → HoI2C transport (§4) → HID report parser (§5) → PTP multi-touch (§6) → vendor quirks (§7) → gesture engine (§8). All paths feed into the same `mouse_driver_handle_event()` injection point. The PS/2 ↔ USB ↔ I2C priority fallback chain must coordinate with `04-drivers-hardware/TODO-10-usb-stack.md §6`.

## Inputs

- [`src/kernel/drivers/mouse.c`](../../src/kernel/drivers/mouse.c), [`src/kernel/drivers/keyboard.c`](../../src/kernel/drivers/keyboard.c) -- event injection targets (`mouse_driver_handle_event()`, `keyboard_driver_handle_event()`)
- → XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md §1` -- ACPICA AML interpreter required by §3 (ACPI I2C device enumeration from DSDT); §3 is blocked until ACPICA is initialised
- → XREF: `04-drivers-hardware/TODO-10-usb-stack.md §6` -- PS/2 ↔ USB fallback flags (`usb_keyboard_active`, `usb_mouse_active`); §2 here adds a third tier (`i2c_touchpad_active`); all three must be checked in `mouse_process_event()`
- → XREF: `08-desktop-shell` domain -- `mouse.cpl` touchpad tab (§9) is a control-panel applet; settings written to Registry are hot-reloaded by the gesture engine (§8)
- → XREF: `07-graphics-ui` domain -- `WM_GESTURE_ZOOM`, `WM_GESTURE_SWIPE`, `MOUSE_WHEEL` messages are compositor-level message types consumed by the window manager

## Outcome

- Intel PCH / AMD FCH SMBus host controllers initialised; `i2c_transfer()` provides reliable combined write+read transactions.
- ACPI DSDT walked for I2C slave children; each device's address, HID string, and IRQ recorded in `i2c_device_db[]`.
- HID-over-I2C transport initialised; HID descriptor fetched; reports dispatched from GPIO interrupt.
- HID report descriptor parsed into `hid_field[]` -- axis ranges, contact count, button layout known before first report.
- Microsoft PTP touchpad delivers up to 10 simultaneous contacts; tip switch, X/Y, contact ID all correct.
- Gesture engine synthesises two-finger scroll, pinch-zoom, three-finger swipe, tap-to-click, and two-finger right-click; palm rejection suppresses accidental input.
- Synaptics PS/2 6-byte absolute packet fallback covers legacy hardware and VMs.
- ELAN and Goodix I2C quirk sequences ensure correct initialisation without ACPI-provided firmware.
- Touchpad settings tab in `mouse.cpl`; `touchpad-info` shell command.

## Implementation Order

| ⭐  | Order | Deliverable                              | Depends On                               | Status |
| --- | :---: | ---------------------------------------- | ---------------------------------------- | :----: |
| 💎  |   1   | §1 I2C/SMBus host controller driver      | PCI scan (existing), ACPI base           |  [ ]   |
| 💎  |   2   | §2 Synaptics PS/2 touchpad fallback      | §1 independent -- PS/2 path; `mouse.c` baseline |  [ ]   |
| 💎  |   3   | §3 ACPI I2C device enumeration -- `i2c_device_db[]` | §1, ACPICA (TODO-03 §1)                  |  [ ]   |
| 💎  |   4   | §4 HID-over-I2C transport -- descriptor fetch, `hoi2c_reset`, report read | §1, §5 (ACPI address + IRQ known)        |  [ ]   |
| 💎  |   5   | §5 HID report descriptor parser -- `hid_field[]`, usage codes | §6 (transport delivers raw descriptor bytes) |  [ ]   |
| 💎  |   6   | §6 Microsoft Precision Touchpad (PTP) multi-touch | §8 (fields parsed), §6 (reports arriving) |  [ ]   |
| 💎  |   7   | §7 ELAN and Goodix I2C touchpad quirks   | §5 (ACPI HID strings), §1 (I2C bus)      |  [ ]   |
| ⭐  |   8   | §8 PTP gesture engine -- scroll, pinch, swipe, tap, palm rejection | §7 (contact data flowing)                |  [ ]   |
| 💎  |   9   | §9 Touchpad control panel (`mouse.cpl` Touchpad tab) | §2 (gestures configurable), Registry     |  [ ]   |
| 💎  |  10   | §10 `xinput list` / `touchpad-info` shell command | §7 (PTP active), §3 (Synaptics active)   |  [ ]   |

> §8 gesture engine is `⭐` exclusive: Windows PTP gestures run in `HIDCLASS.sys` + `precision touchpad.dll` (closed, user-mode); Linux `libinput` runs entirely in user space. Impossible OS implements the gesture engine in the kernel driver where it can fire `WM_GESTURE_*` messages directly into the compositor without a user-space daemon round-trip -- lower latency, no race between gesture recognition and window focus change.

---

## 1. I2C/SMBus Host Controller Driver `[Opus]`

Detect Intel PCH and AMD FCH SMBus controllers via PCI class `0x0C/0x05` and via ACPI `_HID` `PNP0C50`. Map MMIO or I/O base. Implement `i2c_transfer(addr, wbuf, wlen, rbuf, rlen)` for combined write+read transactions. Register multiple host adapters (PCH, embedded controller) as separate `bus_controller_t` instances.

**Files:** `src/kernel/drivers/i2c.c` (new), `include/kernel/drivers/i2c.h` (new)

> [!NOTE]
> Intel SMBus MMIO map: `SMBALERT_STATUS (0x00)`, `SMBHSTSTS (0x00)`, `SMBHSTCNT (0x02)`, `SMBHSTCMD (0x03)`, `SMBHSTADD (0x04)`, `SMBHSTDAT0 (0x05)`, `SMBHSTDAT1 (0x06)`, `SMBBLKDAT (0x07)`. Combined read/write (I2C_BLOCK_DATA, cmd `0x05`): write to slave, then restart, then read. For AMD FCH: `FCH_SMB_BASE` from ACPI `SSDT` or PCI BAR2. Transactions use polling on `SMBHSTSTS.HOST_BUSY` bit with a 25 ms timeout.

- [ ] PCI match: `{ 0x8086, 0x9C22 }` (Lynx Point), `{ 0x8086, 0xA323 }` (Cannon Point), `{ 0x8086, 0x02A3 }` (Ice Lake), `{ 0x1022, 0x790B }` (AMD FCH Promontory); class `0x0C`, subclass `0x05`
- [ ] ACPI fallback: scan ACPI namespace for `_HID == "PNP0C50"` (HID-over-I2C host); extract `_CRS` Memory32Fixed resource → MMIO base
- [ ] `bus_controller_t { uint8_t id; void *mmio; int (*transfer)(bus_controller_t*, uint8_t addr, const uint8_t *wbuf, uint8_t wlen, uint8_t *rbuf, uint8_t rlen); }` -- registered in `i2c_controller_db[MAX_I2C_CONTROLLERS]`
- [ ] Intel transfer: write slave address + W to `SMBHSTADD`; write register to `SMBHSTCMD`; write data bytes to `SMBBLKDAT`; set `SMBHSTCNT.START`; poll `SMBHSTSTS.INTR` (done) or `FAILED`; clear status bits; repeated-START for read phase
- [ ] AMD FCH transfer: similar register layout at different offsets; use `FCH_SMB_CNTL` instead of `SMBHSTCNT`
- [ ] `i2c_transfer(ctrl_id, addr, wbuf, wlen, rbuf, rlen)` -- select controller by `ctrl_id`; delegate to `bus_controller_t.transfer`
- [ ] `smbus_read_byte(ctrl_id, addr, cmd)` / `smbus_write_byte(ctrl_id, addr, cmd, val)` convenience wrappers
- [ ] `i2c_register_controller(ctrl)` -- append to `i2c_controller_db[]`; call from PCI probe and ACPI device probe
- [ ] Boot log: `[I2C] Controller %u: Intel PCH/AMD FCH MMIO 0x%lx`
- [ ] Commit: `"drivers: I2C/SMBus host controller -- Intel PCH + AMD FCH, i2c_transfer, bus_controller_t"`

## 2. Synaptics PS/2 Touchpad Fallback `[Sonnet]`

Send the Synaptics PS/2 magic identify sequence to detect Synaptics capability bits. Switch to 6-byte extended absolute packet mode. Map absolute X/Y to relative delta via a low-pass filter. Detect multi-finger from pressure + width heuristics. Implement scroll zone.

**Files:** `src/kernel/drivers/mouse.c` (extend -- Synaptics probe + packet parser)

> [!NOTE]
> Synaptics magic identify sequence: `0xF3 0xC8` then `0xF3 0x64` then `0xF3 0x50` then `0xE9` (Status Request) → byte 1 bits 6+2 both set → Synaptics present. Extended absolute mode: `Set Resolution (0xE8) + 0x00` four times → `0xF0` then `0x18` enables extended mode. 6-byte absolute packet: bytes `{b0,b1,b2,b3,b4,b5}` where absolute X = `((b3 & 0x10)<<8) | ((b1 & 0x0F)<<8) | b4`, absolute Y = `((b3 & 0x20)<<7) | ((b1 & 0x30)<<4) | b5`, pressure = `b1 & 0xC0) | (b0 & 0x04)<<4 | b2>>4`, width = `(b0 >> 4) & 0x0F`.

- [ ] `synaptics_probe()`: send identify sequence; check byte 1 bits 6+2; if present enable extended absolute mode; set `synaptics_active = true`; set `i2c_touchpad_active = false` (PS/2 takes lower priority than I2C)
- [ ] `synaptics_parse_6byte(buf)`: extract absolute X (0–6143), Y (0–6143), pressure (0–255), width; convert to relative delta using exponential moving average: `delta = (abs_pos - prev_pos) * alpha` where `alpha` loaded from Registry `SynapticsSensitivity`
- [ ] Two-finger detection: `pressure > 40 && width > 4` heuristic → active two fingers; while active: `Δy` → `MOUSE_WHEEL`; `Δx` → `MOUSE_HWHEEL` (horizontal scroll)
- [ ] Scroll zone: if absolute X > `(6143 * 90 / 100)` (right 10% of pad) → vertical scroll instead of pointer movement; if Y < `(6143 * 10 / 100)` (bottom 10%) → horizontal scroll
- [ ] `i2c_touchpad_active` flag: set by PTP probe (§6); `synaptics_parse_6byte()` checks this flag and returns early if set (PS/2 yields to I2C)
- [ ] Boot log: `[PS2] Synaptics touchpad detected (capability byte = 0x%02x)`
- [ ] Commit: `"drivers: Synaptics PS/2 touchpad -- 6-byte absolute, delta filter, scroll zone, I2C yield"`

## 3. ACPI I2C Device Enumeration `[Sonnet]`

Walk the ACPI namespace for I2C slave children under each I2C controller node. Extract `_CRS` for I2C address, IRQ, and GPIO interrupt pin. Build `i2c_device_db[]` so touchpad and sensor probes have a list of devices to bind against.

**Files:** `src/kernel/drivers/i2c.c` (extend), `include/kernel/drivers/i2c.h` (extend)

> [!NOTE]
> → XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md §1` -- ACPICA `AcpiGetHandle` / `AcpiWalkNamespace` / `AcpiGetCurrentResources` must be available. I2C slave devices appear as ACPI namespace objects directly under the I2C controller's ACPI node, with `_CRS` containing a `I2CSerialBusV2` resource descriptor (descriptor type `0x8E`) holding the slave address and bus speed.

- [ ] After ACPICA init: for each registered I2C controller, call `AcpiWalkNamespace(ACPI_TYPE_DEVICE, controller_handle, 1, i2c_enum_callback, ...)` to find direct children
- [ ] `i2c_enum_callback`: call `AcpiGetObjectInfo(handle, &info)` for `_HID` and `_CID` strings; call `AcpiGetCurrentResources` → parse `I2CSerialBusV2` descriptor for slave address; parse `Interrupt` resource for IRQ; parse `GpioInt` resource for GPIO pin + controller
- [ ] `i2c_device_entry_t { uint8_t ctrl_id; uint16_t addr; char hid[16]; char cid[16]; uint8_t irq; uint8_t gpio_pin; }` -- stored in `i2c_device_db[MAX_I2C_DEVICES]`
- [ ] After enumeration, iterate `i2c_device_db[]` and call `i2c_device_probe(entry)` which dispatches to registered probe functions (HoI2C, ELAN, Goodix) matching by HID string
- [ ] Boot log: `[I2C] Enumerated %u devices from ACPI (DSDT)`; per-device: `[I2C] %s @ 0x%02x IRQ %u`
- [ ] Commit: `"drivers: ACPI I2C device enumeration -- I2CSerialBusV2 CRS, i2c_device_db[], probe dispatch"`

## 4. HID-over-I2C (HoI2C) Transport `[Opus]`

Detect the ACPI `PNP0C50` HID-over-I2C host. Fetch the HID descriptor at `wHIDDescRegister` (from `_DSM`). Issue `RESET`, wait for GPIO interrupt, then read reports via `wInputRegister` on each interrupt. Dispatch raw report bytes to the HID report parser (§5).

**Files:** `src/kernel/drivers/hid_i2c.c` (new), `include/kernel/drivers/hid_i2c.h` (new)

> [!NOTE]
> HID Descriptor layout (28 bytes): `wHIDDescLength (2)`, `bcdVersion (2)`, `wReportDescLength (2)`, `wReportDescRegister (2)`, `wInputRegister (2)`, `wMaxInputLength (2)`, `wOutputRegister (2)`, `wMaxOutputLength (2)`, `wCommandRegister (2)`, `wDataRegister (2)`, `wVendorID (2)`, `wProductID (2)`, `wVersionID (2)`, `RESERVED (4)`. To read the descriptor: `i2c_transfer(addr, {wHIDDescRegister_lo, wHIDDescRegister_hi}, 2, desc_buf, 28)`.

- [ ] `hoi2c_probe(entry)`: call `_DSM(PNP0C50_GUID, 1, 0x01, {})` via ACPICA to retrieve `wHIDDescRegister` address; read 28-byte HID descriptor; validate `bcdVersion == 0x0100`
- [ ] Fetch report descriptor: `i2c_transfer(addr, {wReportDescRegister_lo, hi}, 2, rdesc_buf, wReportDescLength)`; pass to `hid_parse_report_descriptor(rdesc_buf, len)` (§5)
- [ ] `hoi2c_reset()`: write `{ wCommandRegister_lo, wCommandRegister_hi, 0x00, 0x00, RESET_CMD=0x01 }` over I2C; wait for GPIO interrupt (timeout 5 s); read and discard reset-completion report
- [ ] GPIO interrupt setup: `gpio_request_irq(entry->gpio_pin, hoi2c_irq_handler, IRQF_EDGE_FALLING)`; in handler: read `wInputRegister` -- 2-byte length prefix + report data; dispatch to `hid_dispatch_report(dev, report_buf, report_len)`
- [ ] `hoi2c_write_output(report_id, buf, len)`: write to `wOutputRegister` (used for feature requests, e.g., set power mode)
- [ ] Boot log: `[HoI2C] Descriptor OK: reportDesc=%u bytes, maxInput=%u, VID=%04x PID=%04x`
- [ ] Commit: `"drivers: HID-over-I2C -- HID descriptor fetch, RESET, GPIO interrupt, report dispatch"`

## 5. HID Report Descriptor Parser `[Opus]`

Parse binary HID report descriptors into a `hid_field[]` array mapping byte+bit offsets to usage codes, logical/physical ranges, and report size/count. Support `Generic Desktop`, `Digitizer`, and `Button` usage pages. Used by PTP (§5) and USB HID (TODO-10).

**Files:** `src/kernel/drivers/hid_parser.c` (new), `include/kernel/drivers/hid_parser.h` (new)

> [!NOTE]
> HID report descriptor items: short items are 1–5 bytes (`bTag[7:4] | bType[3:2] | bSize[1:0]`); long items start with `0xFE`. Main items: `Input (0x80)`, `Output (0x90)`, `Feature (0xB0)`, `Collection (0xA0)`, `End Collection (0xC0)`. Global items: `Usage Page (0x04)`, `Logical Min (0x14)`, `Logical Max (0x24)`, `Physical Min (0x34)`, `Physical Max (0x44)`, `Unit Exponent (0x54)`, `Unit (0x64)`, `Report Size (0x74)`, `Report ID (0x84)`, `Report Count (0x94)`. Local items: `Usage (0x08)`, `Usage Min (0x18)`, `Usage Max (0x28)`.

- [ ] `hid_parse_report_descriptor(buf, len, fields_out, max_fields)` → returns count of parsed `hid_field_t`
- [ ] `hid_field_t { uint8_t report_id; uint32_t usage_page; uint32_t usage; int32_t logical_min, logical_max; int32_t physical_min, physical_max; uint8_t report_size; uint8_t report_count; uint16_t bit_offset; uint8_t flags; }` (flags: Data/Const, Array/Variable, Absolute/Relative)
- [ ] Parser state machine: maintain global/local item state stack; on `Input` main item, emit `report_count` × `hid_field_t` entries with sequential `bit_offset` tracking
- [ ] Supported usage pages: `Generic Desktop (0x01)`: X=0x30, Y=0x31, Z=0x32, Wheel=0x38; `Digitizer (0x0D)`: Tip Switch=0x42, Contact Id=0x51, Contact Count=0x54, Confidence=0x47, Azimuth=0x3F, Width=0x48, Height=0x49; `Button (0x09)`: Button 1–8 mapped by index
- [ ] `hid_get_field_value(report_buf, field)` → extract signed integer from report buffer at `field->bit_offset` with `field->report_size` bits; apply logical min/max clamping
- [ ] `hid_find_field(fields, count, usage_page, usage)` → find first matching field; used by PTP to locate contact array
- [ ] Commit: `"drivers: HID report descriptor parser -- hid_field[], hid_get_field_value, Digitizer+Desktop usage"`

## 6. Microsoft Precision Touchpad (PTP) Multi-Touch `[Sonnet]`

Detect PTP via HID usage `Digitizer/TouchPad (0x0D/0x05)` in the parsed descriptor. Parse multi-touch contact reports (up to 10 contacts × contact_id, tip_switch, x, y, confidence, azimuth, width, height). Emit `touch_contact[]` array for the gesture engine (§8).

**Files:** `src/kernel/drivers/touchpad_ptp.c` (new), `include/kernel/drivers/touchpad_ptp.h` (new)

> [!NOTE]
> PTP reports contain a `Contact Count` field (usage `0x0D/0x54`) indicating the number of valid contacts in the current report. Each contact is a separate logical collection (`Collection(Physical)` or via indexed `Report Count`). The gesture engine (§8) receives the full `touch_contact[]` array on each report cycle, not individual contact events.

- [ ] Detect PTP: after `hid_parse_report_descriptor()`, check if any field has `usage_page=0x0D, usage=0x05` (TouchPad collection); if present, probe as PTP
- [ ] Per-contact field lookup: `hid_find_field(ContactId)`, `hid_find_field(TipSwitch)`, `hid_find_field(X)`, `hid_find_field(Y)`, `hid_find_field(Confidence)`, `hid_find_field(Azimuth)`, `hid_find_field(Width)`, `hid_find_field(Height)`; store field pointers in `ptp_device_t`
- [ ] `ptp_parse_report(dev, buf, len)`: read `Contact Count`; for each contact (0..count-1): extract all per-contact fields via `hid_get_field_value()`; populate `touch_contact_t { id, tip, x, y, confidence, azimuth, width_mm, height_mm }`
- [ ] `ptp_device_t { touch_contact_t contacts[10]; uint8_t contact_count; bool btn_left, btn_right, btn_middle; uint64_t timestamp_ns; }` -- updated on each report
- [ ] After parsing: call `touchpad_gesture_process(&ptp_dev)` (§8) and `mouse_driver_handle_ptp(&ptp_dev)` (absolute position tracking)
- [ ] Read `HKLM\SYSTEM\Input\TouchpadEnabled` at probe; if false, disable IRQ and return early from report handler
- [ ] Boot log: `[PTP] %u contacts max, logical X [%d..%d] Y [%d..%d]`
- [ ] Commit: `"drivers: PTP multi-touch -- contact parsing, touch_contact_t[], TouchpadEnabled Registry"`

## 7. ELAN and Goodix I2C Touchpad Quirks `[Sonnet]`

Register device-specific probe functions for ELAN (`ELAN1000`–`ELAN9008`) and Goodix (`GT9110`/`GT9271`) touchpads. ELAN requires a wake-up I2C sequence before HoI2C. Goodix uses a direct I2C register protocol (not HID-over-I2C) and may require firmware update on first boot.

**Files:** `src/kernel/drivers/touchpad_elan.c` (new), `src/kernel/drivers/touchpad_goodix.c` (new)

> [!NOTE]
> ELAN wake-up: `i2c_write(addr, {0x00, 0x0B})` (wake command); wait 10 ms; then proceed with standard HoI2C init. ELAN ACPI HIDs: `ELAN1000`, `ELAN1200`, `ELAN1300`, `ELAN2000`, `ELAN9008` (non-exhaustive). Goodix GT9x: I2C address `0x14` (INT active low) or `0x5D` (INT active high, toggled by GPIO at boot); product ID at register `0x8140`; config checksum at `0x813C`.

- [ ] ELAN probe: match ACPI HID prefix `"ELAN"` in `i2c_device_db[]`; issue wake-up `{0x00, 0x0B}`; wait 10 ms; then call `hoi2c_probe()` as normal HoI2C device; register in `i2c_device_probe_table[]`
- [ ] Goodix probe: match ACPI HID `"GDIX"` or PCI equivalent; determine I2C address by toggling GPIO `INT` line high/low during probe; read product ID from register `0x8140`; read config length from `0x8143`; read full config from `0x8047..0x8047+len`
- [ ] Goodix firmware check: compute XOR checksum of config bytes; compare to `config[len-1]`; if mismatch, write corrected config + checksum; trigger soft reset (`0x8040 = 0x02`)
- [ ] Goodix report read: interrupt-driven; read from register `0x814E` (status); if `status & 0x80` (buffer ready): read `status & 0x0F` contacts from `0x8150` (6 bytes per contact: x_lo, x_hi, y_lo, y_hi, size, reserved); clear status register
- [ ] Goodix contacts → `touch_contact_t[]` → `touchpad_gesture_process()` (§8); Goodix does not produce a HID report descriptor -- hard-code field layout
- [ ] Boot log: `[ELAN] Touchpad woken: %s` / `[Goodix] GT%s firmware checksum OK/corrected`
- [ ] Commit: `"drivers: ELAN + Goodix I2C touchpad quirks -- wake-up, firmware check, direct register read"`

## 8. PTP Gesture Engine `[Opus]`

Recognise two-finger scroll, pinch-to-zoom, three-finger swipe, tap-to-click, two-finger right-click, and palm rejection from the `touch_contact[]` array. Synthesise `MOUSE_WHEEL`, `WM_GESTURE_ZOOM`, `WM_GESTURE_SWIPE`, and click events. All thresholds configurable via Registry.

**Files:** `src/kernel/drivers/touchpad_gestures.c` (new), `include/kernel/drivers/touchpad_gestures.h` (new)

> [!NOTE]
> All gesture parameters are loaded from Registry at probe and hot-reloaded on `WM_SETTINGS_CHANGED`. Default values in parentheses. Gesture decisions run entirely within the kernel on each report interrupt -- no user-space daemon latency.

- [ ] `gesture_state_t { uint8_t n_contacts; uint64_t contact_start_ns[10]; float start_x[10], start_y[10]; gesture_phase_t phase; }` -- maintained across reports
- [ ] **Two-finger scroll**: 2 contacts moving with `|Δy| > |Δx|` (vertical) or vice versa → `MOUSE_WHEEL(delta)` or `MOUSE_HWHEEL(delta)`; speed = distance / time; natural scroll direction from `HKLM\...\NaturalScroll` (default true); scroll speed multiplier from `ScrollSpeed` (1–10, default 5)
- [ ] **Pinch-to-zoom**: 2 contacts, inter-contact distance changing > 20 px threshold → `WM_GESTURE_ZOOM(scale_factor)` where `scale = current_dist / start_dist`; enabled via `PinchZoom` (default true)
- [ ] **Three-finger swipe**: 3 contacts all moving > 30 px in same direction within 300 ms → `WM_GESTURE_SWIPE(direction)`: LEFT=back, RIGHT=forward, UP=task view, DOWN=show desktop; enabled via `ThreeFingerSwipe` (default true)
- [ ] **Tap-to-click**: 1 contact lifts within 100 ms + position delta < 5 px → synthesise left click (`WM_LBUTTONDOWN`/`UP`); two-finger tap within 100 ms → right click; enabled via `TapToClick` (default true)
- [ ] **Palm rejection**: any contact with `width_mm > PalmRejectWidth` (default 25 mm) → set `contact.confidence = 0`; exclude from all gesture and pointer processing; log `[PTP] Palm rejected`
- [ ] **Contact tracking**: assign contacts to persistent touch IDs across reports using proximity matching (nearest previous contact within 15 px); required for pinch and swipe direction consistency
- [ ] Registry hot-reload: on report enter, call `gesture_reload_config()` if `g_gesture_config_dirty` flag is set (set by `mouse.cpl` on settings change)
- [ ] Commit: `"drivers: PTP gesture engine -- scroll, pinch, 3-finger swipe, tap-click, palm reject, Registry cfg"`

## 9. Touchpad Control Panel (`mouse.cpl` Touchpad Tab) `[Sonnet]`

Extend `mouse.cpl` with a "Touchpad" settings tab: enable/disable toggle, tap-to-click, scroll direction, scroll speed slider, gesture toggles, palm rejection sensitivity. All settings → Registry `HKLM\SYSTEM\Input\Touchpad\*`; gesture engine hot-reloads on change.

**Files:** `src/desktop/mouse_cpl.c` (extend or new), `include/desktop/mouse_cpl.h`

> [!NOTE]
> → XREF: `08-desktop-shell` domain -- `mouse.cpl` is a control-panel applet opened from the Settings app or Start Menu; it is a compositor-managed window. The "Touchpad" tab appears only if `ptp_device_t` or `synaptics_active` is true at runtime; hidden on desktops with no touchpad detected.

- [ ] Touchpad tab added to `mouse.cpl` window (tab control, only shown if touchpad detected)
- [ ] Controls: enable/disable toggle (`TouchpadEnabled`), tap-to-click checkbox (`TapToClick`), scroll direction radio (`NaturalScroll`), scroll speed slider 1–10 (`ScrollSpeed`), two-finger scroll checkbox (`TwoFingerScroll`), three-finger gestures checkbox (`ThreeFingerSwipe`), pinch-zoom checkbox (`PinchZoom`), palm rejection slider 10–40 mm (`PalmRejectWidth`)
- [ ] On any control change: write to `HKLM\SYSTEM\Input\Touchpad\<key>` (`REG_DWORD`); set `g_gesture_config_dirty = 1` so gesture engine hot-reloads on next report interrupt
- [ ] Live preview: a touchpad diagram shows active contact positions updating in real-time (reads `ptp_device_t.contacts[]`); refreshed every 33 ms (30 Hz)
- [ ] "Reset to defaults" button: restore all Registry keys to documented defaults
- [ ] Commit: `"desktop: mouse.cpl Touchpad tab -- enable/tap/scroll/gestures/palm, Registry hot-reload"`

## 10. `xinput list` / `touchpad-info` Shell Commands `[Sonnet]`

Print connected input devices and detailed touchpad diagnostics from the shell. For compatibility with scripts that expect Linux `xinput` output format.

**Files:** `src/shell/cmd_xinput.c` (new), `src/shell/cmd_touchpad_info.c` (new)

- [ ] `xinput list`: print one line per input device: `⎜ ↳ Precision Touchpad  id=2 [slave pointer (3)]` (Linux `xinput list` format); includes keyboard, mouse, touchpad; device IDs from `input_device_db[]`
- [ ] `xinput list-props <id>`: print all input device properties (e.g., `libinput Tapping Enabled (280): 1`); map Impossible OS Registry keys to libinput-style property names for script compatibility
- [ ] `touchpad-info`: print: model name/ACPI HID string, firmware version (from Goodix `0x8140` or ELAN version register), max contacts, physical dimensions (mm), current Registry settings (all `HKLM\SYSTEM\Input\Touchpad\*`), Synaptics vs I2C active path
- [ ] `touchpad-info --contacts`: enter live contact-display mode (ncurses-style); refresh at 30 Hz; print `Contact 0: x=%d y=%d tip=%d width=%.1fmm` per active contact; Ctrl+C to exit
- [ ] Register both commands in `src/shell/shell.c` dispatch table
- [ ] Commit: `"shell: xinput list + touchpad-info -- device list, properties, live contact display"`

---

## OS Comparison


| ⭐  | Feature                               | 🪟 Win11                                 | 🐧 Linux                                 | 🚀 Impossible OS                         |
| --- | ------------------------------------- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- |
| 💎  | I2C/SMBus host controller driver      | ✅ `smbus.sys`; inbox ACPI-enumerated SMBus | ✅ `i2c-i801.c`; `i2c-piix4.c` (AMD); ACPI `_CRS` | ⬜ §1 -- PCI + ACPI `PNP0C50`, `bus_controller_t`, |
| 💎  | ACPI I2C device enumeration from DSDT | ✅ ACPI PnP manager; `I2CSerialBusV2` resource | ✅ `i2c-acpi.c`; `acpi_i2c_register_devices()` | ⬜ §3 -- ACPICA walk, `I2CSerialBusV2` CRS parse, |
| 💎  | HID-over-I2C transport                | ✅ `hidi2c.sys`; HID descriptor fetch; GPIO | ✅ `i2c-hid.c`; `_DSM` wHIDDescRegister; GPIO IRQ | ⬜ §4 -- HID descriptor read, `hoi2c_reset`, GPIO |
| 💎  | HID report descriptor parser          | ✅ `HIDCLASS.sys`; full HID 1.11 parser; | ✅ `hid-core.c`; full HID parser; `hid_field` | ⬜ §5 -- `hid_field_t[]`, `hid_get_field_value`, Digitizer + Desktop |
| 💎  | Microsoft Precision Touchpad          | ✅ PTP inbox driver; all 10              | ✅ `libinput` PTP; `MT_TOOL_FINGER`; `ABS_MT_POSITION_X/Y` | ⬜ §6 -- `touch_contact_t[10]`, contact_id, tip, x/y, confidence, |
| ⭐  | PTP gesture engine in kernel          | ⚠️ `precision touchpad.dll` in user space; | ❌ `libinput` entirely in user space;    | ⬜ §8 -- kernel gesture engine → `WM_GESTURE_*` |
| 💎  | Synaptics PS/2 fallback               | ✅ `SynTP.sys` Synaptics driver; absolute mode, | ✅ `psmouse`; Synaptics protocol; `evdev` absolute | ⬜ §2 -- 6-byte absolute, EMA delta filter, |
| 💎  | ELAN I2C touchpad quirks              | ✅ `ETD2003.sys` / `ETDTouchScreen.sys` inbox Elantech | ✅ `elan_i2c.c`; `elan_i2c_initialize()`; wake-up command | ⬜ §7 -- ACPI HID prefix `"ELAN"`, `{0x00, |
| 💎  | Goodix I2C touchpad                   | ✅ Goodix inbox via generic HoI2C        | ✅ `goodix.c`; direct I2C registers; config | ⬜ §7 -- `0x8140` product ID, config checksum |
| 💎  | Touchpad settings GUI                 | ✅ Settings → Bluetooth & Devices        | ✅ GNOME Settings → Mouse &              | ⬜ §9 -- `mouse.cpl` Touchpad tab, live contact |
| 💎  | `xinput list` / `touchpad-info` CLI   | ❌ No inbox `xinput`; PowerShell `Get-PnpDevice` | ✅ `xinput`; `libinput debug-events`; standard diagnostic | ⬜ §10 -- `xinput list/list-props`, `touchpad-info --contacts` live |

> **After §1–10:** Impossible OS supports the full modern laptop touchpad stack -- I2C bus enumeration from ACPI DSDT, HID-over-I2C, PTP 10-touch, and a gesture engine that is architecturally superior to both Windows and Linux. The kernel-resident gesture engine (§8, `⭐`) fires `WM_GESTURE_*` messages directly into the compositor message queue without a user-space daemon round-trip -- eliminating the latency introduced by `precision touchpad.dll` (Windows) and `libinput` (Linux) when a gesture needs to change window focus or trigger a system-level action.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] QEMU (no I2C touchpad -- tests PS/2 Synaptics path): Synaptics probe skipped gracefully; PS/2 mouse works normally
- [ ] Real laptop with Intel PCH I2C: `[I2C] Controller 0: Intel PCH` boot log; `[I2C] Enumerated N devices from ACPI`; touchpad model name logged
- [ ] HoI2C: `[HoI2C] Descriptor OK: reportDesc=%u bytes`; PTP detected; `[PTP] N contacts max`
- [ ] Two-finger scroll: two-finger drag on touchpad → `MOUSE_WHEEL` events; content scrolls
- [ ] Tap-to-click: single-finger tap → left click; two-finger tap → right click
- [ ] Three-finger swipe right → `WM_GESTURE_SWIPE(RIGHT)` → compositor triggers back action
- [ ] Palm rejection: wide contact (palm) → no pointer movement; removed and narrow contact accepted normally
- [ ] Synaptics PS/2 (VM): enable Synaptics PS/2 in QEMU; `[PS2] Synaptics touchpad detected`; absolute-to-relative works; right-edge scroll zone scrolls
- [ ] ELAN laptop: `[ELAN] Touchpad woken` then HoI2C normal init
- [ ] `touchpad-info` output: model, firmware, max contacts, current settings
- [ ] `mouse.cpl` Touchpad tab: disable touchpad → touch events stop; re-enable → resume; change scroll direction → natural/traditional swap confirmed
- [ ] Commit: `"drivers: I2C/HoI2C/PTP -- bus controller, ACPI enum, HID parser, PTP, gestures, Synaptics, ELAN/Goodix"`
