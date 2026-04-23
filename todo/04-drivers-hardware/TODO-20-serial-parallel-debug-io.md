---
schema_version: 1
id: serial-parallel-debug-io
domain: 04-drivers-hardware
status: active
title: "TODO-20 -- Serial, Parallel, GPIO/SPI & Debug I/O Devices"
---

# TODO-20 -- Serial, Parallel, GPIO/SPI & Debug I/O Devices

> **Goal:** Turn early serial logging into a production I/O device stack for UARTs, PCI/USB serial adapters, parallel/LPT ports, GPIO/SPI controllers, debug consoles, and industrial/embedded devices. Early boot COM output remains in boot code; this TODO owns runtime character-device drivers and debug-console arbitration.
> **Current state:** `src/kernel/drivers/serial.c` provides early COM logging and `serial_trygetchar()`. USB CDC-ACM is planned in TODO-10. There is no runtime TTY/COM namespace, no PCI serial enumeration, no LPT, no GPIO/SPI framework, and no policy for choosing KD/debug/console ownership of a serial port.

## Inputs

- [`src/kernel/drivers/serial.c`](../../src/kernel/drivers/serial.c)
- [`include/kernel/drivers/serial.h`](../../include/kernel/drivers/serial.h)
- -> XREF: `TODO-10-usb-stack.md §12` -- USB CDC-ACM serial transport
- -> XREF: `TODO-01-pci-pcie-pnp-resource-manager.md` -- PCI serial and resource ownership
- -> XREF: `02-kernel-core/TODO-29-kernel-debugger-kd-protocol.md` -- KD serial transport
- -> XREF: `12-user-platform-sdk` -- user-mode COM APIs consume runtime serial devices

## Outcome

- Runtime serial ports are named, discoverable, exclusive-open devices.
- Debugger, kernel console, and user processes cannot fight over the same port.
- Parallel, GPIO, and SPI devices have enough infrastructure for printers, sensors, and embedded boards.

## Implementation Order

| Priority | Order | Deliverable | Depends On | Status |
| --- | :---: | --- | --- | :---: |
| Parity | 1 | Runtime COM device model | existing serial | [ ] |
| Parity | 2 | UART 16550A driver cleanup | §1 | [ ] |
| Parity | 3 | PCI/PCIe multiport serial adapters | §1, TODO-01 | [ ] |
| Parity | 4 | USB CDC-ACM integration | §1, TODO-10 §12 | [ ] |
| Parity | 5 | KD/console/user ownership arbitration | §1, KD | [ ] |
| Parity | 6 | Parallel/LPT port driver | §1 | [ ] |
| Parity | 7 | GPIO controller framework | TODO-01 | [ ] |
| Parity | 8 | SPI controller framework | §7 | [ ] |
| Exclusive | 9 | Industrial I/O diagnostics | §1-§8 | [ ] |
| Parity | 10 | Tests and loopback fixtures | §1-§9 | [ ] |

## 1. Runtime COM Device Model

- [ ] Define `serial_port_t` with name, transport, IRQ, baud, line status, owner, and capabilities.
- [ ] Expose `COM1`/`COM2` style aliases and stable device paths.
- [ ] Commit: `"drivers/serial: runtime COM device model"`

## 2. UART 16550A Cleanup

- [ ] Separate early boot polled serial from runtime interrupt-driven UART mode.
- [ ] Add FIFOs, line settings, modem status, and error counters.
- [ ] Commit: `"drivers/serial: 16550 runtime mode"`

## 3. PCI Multiport Serial

- [ ] Detect common PCI serial adapters and map BARs.
- [ ] Register each port as an independent serial device.
- [ ] Commit: `"drivers/serial: PCI multiport adapters"`

## 4. USB CDC-ACM Integration

- [ ] Consume USB CDC-ACM devices from TODO-10 through the same serial API.
- [ ] Handle disconnect and reconnect cleanly.
- [ ] Commit: `"drivers/serial: USB CDC ACM integration"`

## 5. Ownership Arbitration

- [ ] Reserve debugger-owned ports while KD is active.
- [ ] Allow emergency console takeover only through explicit policy.
- [ ] Commit: `"drivers/serial: debug console ownership"`

## 6. Parallel/LPT Driver

- [ ] Support basic SPP/ECP/EPP detection where hardware exists.
- [ ] Expose LPT devices for printer-class work.
- [ ] Commit: `"drivers: parallel port driver"`

## 7. GPIO Framework

- [ ] Define GPIO controller ops, pin descriptors, direction, value, interrupt, and debounce APIs.
- [ ] Support ACPI GPIO resource discovery.
- [ ] Commit: `"drivers: GPIO controller framework"`

## 8. SPI Framework

- [ ] Define SPI controller and device ops with mode, speed, chip select, and transfer lists.
- [ ] Support ACPI-enumerated SPI peripherals.
- [ ] Commit: `"drivers: SPI controller framework"`

## 9. Industrial I/O Diagnostics

- [ ] Add `serial list`, `gpio list`, and `spi list` shell diagnostics.
- [ ] Add Device Manager nodes and health counters.
- [ ] Commit: `"drivers: industrial IO diagnostics"`

## 10. Tests

- [ ] QEMU serial loopback and USB CDC fixture.
- [ ] Bare-metal COM adapter smoke tests.
- [ ] Commit: `"test: serial and debug IO drivers"`

## OS Comparison

| Priority | Feature | Windows | Linux | Impossible OS |
| --- | --- | --- | --- | --- |
| Parity | COM devices | serial.sys | tty/serial core | TODO-20 |
| Parity | USB serial | usbser.sys | cdc_acm | TODO-20 §4 |
| Parity | GPIO/SPI | SPB/GPIO framework | gpiolib/spi | TODO-20 §7-§8 |
| Exclusive | KD ownership report | limited | console params | TODO-20 §5 |

