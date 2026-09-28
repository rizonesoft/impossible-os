---
schema_version: 1
id: hardware-monitoring-sensors
domain: 04-drivers-hardware
status: active
title: "TODO-19 -- Hardware Monitoring, Sensors & Environmental Devices"
---

# TODO-19 -- Hardware Monitoring, Sensors & Environmental Devices

> **Goal:** Add a hardware-monitoring and sensor class stack for temperature, fan, voltage, battery-side sensors, ambient light, accelerometers, tablet/lid posture sensors, chassis intrusion, jack detection, and platform-specific embedded-controller readings. This makes laptops, desktops, tablets, and workstations observable without each driver inventing private APIs.
> **Current state:** ACPI power work (TODO-03) plans battery and thermal policy, but today only an ECDT-found EC transport exists and it stays disabled (`src/kernel/drivers/acpi_ec.c`); there is no generic sensor class, no `hwmon`-style API, no user-visible sensor inventory, and no consistent event path for environmental devices.

## Inputs

- [`src/kernel/acpi.c`](../../src/kernel/acpi.c)
- [`include/kernel/acpi.h`](../../include/kernel/acpi.h)
- -> XREF: `TODO-03-acpi-power-management.md` -- ACPI thermal, battery, power and EC foundations
- -> XREF: `TODO-12-i2c-touchpad.md §1-§3` -- I2C/SMBus bus and ACPI enumeration
- -> XREF: `TODO-07-device-manager.md` -- sensor inventory and health display
- -> XREF: `02-kernel-core/TODO-16-kernel-notification-facility.md` -- event publication

## Outcome

- Sensors register with a common class API and expose typed readings with units, scale, accuracy, and update rate.
- ACPI, SMBus/I2C, PCI, and embedded-controller sensors share one event and query model.
- Thermal, power, display, and shell components consume sensor data without knowing the bus.

## Implementation Order

| Priority  | Order | Deliverable                                     | Depends On                | Status |
| --------- | :---: | ----------------------------------------------- | ------------------------- | :----: |
| Parity    |   1   | Sensor class API and unit model                 | --                        |  [ ]   |
| Parity    |   2   | ACPI thermal, lid, tablet, and ALS sensors      | §1, TODO-03               |  [ ]   |
| Parity    |   3   | SMBus/I2C hwmon sensor transport                | §1, TODO-12               |  [ ]   |
| Parity    |   4   | Fan, voltage, and chassis sensors               | §2, §3                    |  [ ]   |
| Parity    |   5   | Accelerometer and orientation sensors           | §2, TODO-12               |  [ ]   |
| Parity    |   6   | Audio jack and device-presence sensors          | §1, TODO-18               |  [ ]   |
| Parity    |   7   | Sensor event notifications                      | §1, notification facility |  [ ]   |
| Parity    |   8   | Registry, Device Manager, and shell diagnostics | §1-§7, TODO-07            |  [ ]   |
| Exclusive |   9   | BlackBox environmental timeline                 | §7                        |  [ ]   |
| Parity    |  10   | Sensor test matrix                              | §1-§9                     |  [ ]   |

## 1. Sensor Class API

- [ ] Define `sensor_device_t`, `sensor_reading_t`, typed units, capabilities, and polling/event flags.
- [ ] Implement `sensor_register()`, `sensor_read()`, `sensor_subscribe()`.
- [ ] Commit: `"drivers/sensors: sensor class API"`

## 2. ACPI Sensors

- [ ] Register ACPI thermal zones, lid switch, tablet mode, and ambient light devices.
- [ ] Normalize ACPI units and debounce switch events.
- [ ] Commit: `"drivers/sensors: ACPI sensor backend"`

## 3. SMBus/I2C hwmon Transport

- [ ] Add SMBus read helpers for common monitor chips.
- [ ] Support ACPI-enumerated sensor devices and board-specific quirks.
- [ ] Commit: `"drivers/sensors: SMBus hwmon backend"`

## 4. Fan, Voltage, and Chassis Sensors

- [ ] Expose fan RPM, voltage rails, chassis intrusion, and temperature chips.
- [ ] Define unavailable/invalid reading semantics.
- [ ] Commit: `"drivers/sensors: fan voltage chassis readings"`

## 5. Accelerometer and Orientation Sensors

- [ ] Support ACPI and I2C accelerometers with orientation matrix metadata.
- [ ] Publish orientation changes for shell rotation policy.
- [ ] Commit: `"drivers/sensors: accelerometer orientation"`

## 6. Audio Jack and Presence Sensors

- [ ] Route HDA/AC97 jack detect through sensor or notification path.
- [ ] Notify audio policy of headphone/mic insert and remove.
- [ ] Commit: `"drivers/sensors: audio jack detection"`

## 7. Event Notifications

- [ ] Publish sensor threshold and state-change events.
- [ ] Rate-limit noisy sensors and coalesce orientation events.
- [ ] Commit: `"drivers/sensors: event notifications"`

## 8. Diagnostics

- [ ] Add Device Manager sensor tab and `sensors` shell command.
- [ ] Mirror critical readings under Registry for diagnostics.
- [ ] Commit: `"drivers/sensors: diagnostics surfaces"`

## 9. BlackBox Timeline

- [ ] Persist thermal throttling, fan faults, lid events, and orientation changes to BlackBox.
- [ ] Commit: `"drivers/sensors: BlackBox environmental timeline"`

## 10. Tests

- [ ] Unit-test unit conversion, debouncing, and event coalescing.
- [ ] QEMU ACPI fixtures and bare-metal laptop checks.
- [ ] Commit: `"test: sensor driver matrix"`

## OS Comparison

| Priority  | Feature                  | Windows                | Linux           | Impossible OS |
| --------- | ------------------------ | ---------------------- | --------------- | ------------- |
| Parity    | Sensor class             | Sensor Class Extension | hwmon/iio/input | TODO-19       |
| Parity    | Thermal/fan              | ACPI/WMI               | thermal/hwmon   | TODO-19 §2-§4 |
| Exclusive | BlackBox sensor timeline | Event logs             | journald/dmesg  | TODO-19 §9    |

