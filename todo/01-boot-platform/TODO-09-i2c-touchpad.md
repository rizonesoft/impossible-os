# TODO-09 — Laptop Touchpad Driver (Boot-Critical)

> **Goal:** Enable touchpad input on laptops — covering all common touchpad buses: I2C precision touchpads (most laptops since ~2015), Synaptics/ELAN PS/2 touchpads (older laptops), and USB HID touchpads. Without this, the only pointing device on laptops is an external USB mouse.

> [!IMPORTANT]
> This TODO extracts the **boot-critical** touchpad sections from `04-drivers-hardware/TODO-13-i2c-touchpad.md`. Advanced features (gesture engine, multi-touch, vendor quirks, control panel) remain in TODO-13. After this TODO, the touchpad moves the cursor and generates clicks on any laptop.

> [!NOTE]
> **Discovery chain:** ACPI DSDT → `I2CSerialBusV2` resource → I2C controller (Intel PCH or AMD FCH) → HID-over-I2C device → touchpad reports. This is a full driver stack from bus to input — no shortcuts.

## Inputs

- [`src/kernel/acpi.c`](../../src/kernel/acpi.c) — ACPI table parsing
- [`src/kernel/drivers/pci.c`](../../src/kernel/drivers/pci.c) — PCI discovery for I2C controller
- → XREF: `04-drivers-hardware/TODO-13-i2c-touchpad.md §1-§5` — full I2C + touchpad stack
- → XREF: `04-drivers-hardware/TODO-04-acpi-power-management.md §1` — ACPICA AML interpreter (may be needed for DSDT parsing)
- → XREF: `01-boot-platform/TODO-06-bare-metal-hardening.md §4` — ACPI FADT flags

## Outcome

- I2C host controller discovered and initialized (Intel PCH LPSS or AMD FCH)
- HID-over-I2C touchpad device detected via ACPI
- Single-touch cursor movement and left/right click working
- Cursor moves on bare metal laptop without external mouse

## Implementation Order

| ⭐  | Order | Deliverable                                    | Depends On | Status |
| --- | :---: | ---------------------------------------------- | ---------- | :----: |
| 💎  |   1   | I2C host controller driver (Intel PCH LPSS)    | —          |  [ ]   |
| 💎  |   2   | ACPI I2C device enumeration                    | §1         |  [ ]   |
| 💎  |   3   | HID-over-I2C transport                         | §2         |  [ ]   |
| 💎  |   4   | Basic touchpad input (single-touch + clicks)   | §3         |  [ ]   |
| 💎  |   5   | Synaptics/ELAN PS/2 touchpad driver            | —          |  [ ]   |

---

## 1. I2C Host Controller Driver (Intel PCH + AMD FCH)
Implement the I2C controller register interface. Both Intel PCH LPSS and AMD FCH use the Synopsys DesignWare I2C IP — same register layout, different PCI device IDs.

**Files:** `src/kernel/drivers/i2c.c` (new), `include/kernel/drivers/i2c.h` (new)

- [ ] PCI discovery: find DesignWare I2C controllers (Intel: vendor 0x8086, various IDs per PCH gen; AMD: vendor 0x1022, FCH I2C IDs)
- [ ] Map BAR0 via `vmm_map_mmio_uc()` for I2C register access
- [ ] I2C controller reset and initialization (IC_ENABLE, IC_CON, timing registers)
- [ ] `i2c_read(bus, addr, reg, buf, len)` — single or burst read from I2C slave
- [ ] `i2c_write(bus, addr, reg, buf, len)` — single or burst write to I2C slave
- [ ] Polled transfer (TX/RX FIFO status polling) — interrupt-based deferred
- [ ] Log: `i2c: DesignWare I2C bus N at PCI B:D.F (Intel/AMD)`
- [ ] Commit: `"drivers: I2C host controller driver for Intel PCH + AMD FCH"`

**Test checkpoint:** I2C controller found, initialized, test read from known address. POST code 0xDA00. Test on: bare metal.

## 2. ACPI I2C Device Enumeration
Walk the ACPI DSDT to find I2C-attached devices (touchpad, sensors, etc.) and their I2C bus/address.

**Files:** `src/kernel/drivers/i2c.c`, `src/kernel/acpi.c`

> [!NOTE]
> Full AML interpreter (ACPICA) is in `TODO-04-acpi-power-management.md`. For this section, use a minimal DSDT walker that finds `_HID` and `I2CSerialBusV2` resource descriptors by pattern matching, without full AML evaluation. This works for ~90% of touchpads.

- [ ] Minimal DSDT walker: scan device nodes for `_HID` string matching known touchpad HIDs (`PNP0C50`, `MSFT0001`, `ELAN*`, `SYNA*`, `GDIX*`)
- [ ] Extract `I2CSerialBusV2` connection descriptor: slave address, bus number, speed
- [ ] Build device list: `i2c_device_t { bus, address, hid, name }`
- [ ] Log: `i2c: Found touchpad "ELAN1200" on bus 0 addr 0x15`
- [ ] Commit: `"drivers: ACPI I2C device enumeration — touchpad discovery"`

**Test checkpoint:** Touchpad device found with correct bus/address. POST code 0xDA01. Test on: bare metal.

## 3. HID-over-I2C Transport
Implement the HID-over-I2C (HoI2C) protocol to communicate with the touchpad: reset, get HID descriptor, read input reports.

**Files:** `src/kernel/drivers/hid_i2c.c` (new), `include/kernel/drivers/hid_i2c.h` (new)

- [ ] Read HID descriptor (register 0x0001): wReportDescLength, wInputRegister, wMaxInputLength, wOutputRegister, etc.
- [ ] `hoi2c_reset()` — write 0x0100 to reset register
- [ ] `hoi2c_read_report(buf, len)` — read from input register, parse 2-byte length prefix + report data
- [ ] Set up interrupt (GPIO-based or ACPI GSI) for touchpad report-ready signal
- [ ] If no interrupt available: poll at 125 Hz from timer callback
- [ ] Commit: `"drivers: HID-over-I2C transport — reset, descriptor, input reports"`

**Test checkpoint:** HID descriptor read, input reports received when touching the touchpad. POST code 0xDA02.

## 4. Basic Touchpad Input (Single-Touch + Clicks)
Parse touchpad input reports for single-finger movement and button clicks, inject into the mouse subsystem.

**Files:** `src/kernel/drivers/touchpad.c` (new)

- [ ] Parse HID input report: X position, Y position, tip switch (finger down), button states
- [ ] Convert absolute touchpad coordinates to relative cursor movement (delta calculation)
- [ ] Inject into `mouse_handle_event(dx, dy, buttons)` — same path as PS/2/USB mouse
- [ ] Tap-to-click: short touch-and-release = left click
- [ ] Two-finger tap = right click (if report supports contact count)
- [ ] Log: `input: I2C touchpad initialized (single-touch + click)`
- [ ] Commit: `"drivers: I2C touchpad — single-touch cursor movement + tap-to-click"`

**Test checkpoint:** Bare metal laptop: cursor moves with touchpad, tap = click, two-finger tap = right click. POST code 0xDA03.

## 5. Synaptics/ELAN PS/2 Touchpad Driver
Independent driver for laptops with PS/2-connected touchpads. Runs alongside the I2C driver (§1–§4) — both are probed at boot, each activates only if its hardware is present. All touchpad drivers inject into the same `mouse_handle_event()` input path.

**Files:** `src/kernel/drivers/ps2_touchpad.c` (new)

> [!NOTE]
> This is independent from §1–§4 (I2C). Both drivers probe at boot. A system may have both (rare) or neither (desktop). Full Synaptics multi-touch and quirk handling is in `04-drivers-hardware/TODO-13-i2c-touchpad.md §7-§8`.

- [ ] Detect Synaptics touchpad: send Identify command (0xE8 sequence + 0xE9), check for magic response bytes
- [ ] Detect ELAN touchpad: similar identification sequence with ELAN-specific response
- [ ] If detected: enable absolute mode (Synaptics: set mode byte via 0xE8 sequence)
- [ ] Parse absolute packets: X/Y position, pressure, finger count
- [ ] Convert to relative cursor movement + tap-to-click (same as §4)
- [ ] If neither detected: driver doesn't activate (standard PS/2 mouse protocol still works independently)
- [ ] Commit: `"drivers: Synaptics/ELAN PS/2 touchpad — independent driver"`

**Test checkpoint:** Older laptop with PS/2 touchpad: cursor moves, tap = click. Modern laptop with I2C: PS/2 touchpad driver doesn't activate (no conflict). POST code 0xDA05.

---

## OS Comparison

| ⭐ | Feature                 | Win11                       | Linux                        | Impossible OS                    |
|----|-------------------------|-----------------------------|------------------------------|----------------------------------|
| 💎 | I2C host controller    | ✅ DesignWare (Intel+AMD)    | ✅ i2c-designware-pci         | ⬜ §1 — Intel PCH + AMD FCH     |
| 💎 | HID-over-I2C           | ✅ hidi2c.sys                | ✅ i2c-hid-acpi               | ⬜ §3 — basic transport          |
| 💎 | Precision touchpad     | ✅ PTP class driver          | ✅ hid-multitouch             | ⬜ §4 — single-touch + click     |
| 💎 | PS/2 touchpad          | ✅ Synaptics/ELAN drivers    | ✅ psmouse + synaptics        | ⬜ §5 — independent PS/2 driver  |
| ⭐ | Kernel gesture engine  | ❌ User-mode only            | ❌ libinput (user-mode)       | ⬜ TODO-13 §6 — kernel-resident  |

> **After §1–§5:** Touchpad works on any laptop — I2C (modern) and PS/2 (older) are independent drivers, both probed at boot. Multi-touch gestures deferred to `TODO-13-i2c-touchpad.md §5-§6`.

## Verification

- [ ] Bare metal (i5-11600K): touchpad moves cursor, tap = click
- [ ] Older laptop with PS/2 touchpad: cursor moves, tap = click
- [ ] QEMU: graceful skip (no I2C or Synaptics on virtual hardware)
- [ ] No PS/2 mouse regression on systems with standard PS/2 mouse
