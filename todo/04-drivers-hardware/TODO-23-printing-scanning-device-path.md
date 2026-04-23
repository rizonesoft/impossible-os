---
schema_version: 1
id: printing-scanning-device-path
domain: 04-drivers-hardware
status: active
title: "TODO-23 -- Printing, Scanning & Imaging Peripheral Device Path"
---

# TODO-23 -- Printing, Scanning & Imaging Peripheral Device Path

> **Goal:** Own the hardware-facing path for printers, scanners, and multifunction peripherals: USB printer class, IPP-over-USB discovery, legacy parallel printers, scanner class boundaries, device permissions, status reporting, and handoff to the user-mode spooler/image stack. Rendering, print queues, and applications belong outside this domain.
> **Current state:** There is no USB printer class driver, no LPT ownership, no scanner peripheral path, and no status/permission model for multifunction devices. TODO-20 owns LPT transport, TODO-22 owns camera/scanner imaging buffers, and this TODO ties printer/scanner peripherals into the driver model.

## Inputs

- -> XREF: `TODO-10-usb-stack.md` -- USB class device enumeration and bulk transfers
- -> XREF: `TODO-20-serial-parallel-debug-io.md §6` -- LPT transport
- -> XREF: `TODO-22-camera-imaging-devices.md §7` -- scanner image buffer boundary
- -> XREF: `10-platform-services` -- print spooler and UI consume this hardware path

## Outcome

- USB printers and multifunction devices are discovered and exposed to user-mode services.
- Printer status, errors, and permissions are visible through driver diagnostics.
- Scanning devices have a clear handoff to imaging APIs.

## Implementation Order

| Priority | Order | Deliverable | Depends On | Status |
| --- | :---: | --- | --- | :---: |
| Parity | 1 | Printer/scanner device class model | TODO-01 | [ ] |
| Parity | 2 | USB printer class transport | §1, TODO-10 | [ ] |
| Parity | 3 | IPP-over-USB discovery boundary | §2 | [ ] |
| Parity | 4 | Parallel/LPT printer transport | §1, TODO-20 §6 | [ ] |
| Parity | 5 | Multifunction device composition | §1-§4 | [ ] |
| Parity | 6 | Scanner transport handoff | §1, TODO-22 §7 | [ ] |
| Parity | 7 | Status, ink/toner, and paper errors | §2-§6 | [ ] |
| Parity | 8 | Permissions and sandbox handoff | §1, SRM | [ ] |
| Exclusive | 9 | Device Manager print/imaging diagnostics | §1-§8 | [ ] |
| Parity | 10 | Tests and hardware matrix | §1-§9 | [ ] |

## 1. Device Class Model

- [ ] Define printer/scanner class devices and multifunction parent-child relationships.
- [ ] Register devices from USB, LPT, and future network discovery handoffs.
- [ ] Commit: `"drivers/print: device class model"`

## 2. USB Printer Class Transport

- [ ] Detect USB class 7 printer interfaces.
- [ ] Implement bulk OUT/IN transport and IEEE 1284 id query.
- [ ] Commit: `"drivers/print: USB printer class"`

## 3. IPP-over-USB Boundary

- [ ] Detect IPP-over-USB descriptors and expose an endpoint to user-mode service.
- [ ] Keep HTTP/IPP protocol parsing out of kernel.
- [ ] Commit: `"drivers/print: IPP over USB handoff"`

## 4. Parallel/LPT Transport

- [ ] Consume LPT devices from TODO-20 and expose printer writes/status reads.
- [ ] Commit: `"drivers/print: LPT printer transport"`

## 5. Multifunction Composition

- [ ] Group print, scan, storage-card, and fax-like interfaces under one physical device.
- [ ] Commit: `"drivers/print: multifunction composition"`

## 6. Scanner Transport Handoff

- [ ] Route scanner endpoints to imaging class without inventing print-specific buffers.
- [ ] Commit: `"drivers/imaging: scanner transport handoff"`

## 7. Status Reporting

- [ ] Surface paper, cover, offline, jam, ink/toner, and transport errors where available.
- [ ] Commit: `"drivers/print: status reporting"`

## 8. Permissions

- [ ] Gate raw printer and scanner access by device ACLs.
- [ ] Audit scanner use and raw printer access.
- [ ] Commit: `"drivers/print: permissions and audit"`

## 9. Diagnostics

- [ ] Add Device Manager details for printers/scanners and `print-devices` shell output.
- [ ] Commit: `"drivers/print: diagnostics"`

## 10. Tests

- [ ] USB descriptor fixtures and QEMU/USB pass-through smoke tests.
- [ ] Bare-metal USB printer and multifunction scanner checklist.
- [ ] Commit: `"test: printing and scanning device path"`

## OS Comparison

| Priority | Feature | Windows | Linux | Impossible OS |
| --- | --- | --- | --- | --- |
| Parity | USB printer | usbprint.sys | usblp/IPP | TODO-23 §2 |
| Parity | IPP-over-USB | print stack | ipp-usb | TODO-23 §3 |
| Parity | Scanner path | WIA driver | sane backends | TODO-23 §6 |

