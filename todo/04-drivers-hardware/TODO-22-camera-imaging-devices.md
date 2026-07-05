---
schema_version: 1
id: camera-imaging-devices
domain: 04-drivers-hardware
status: active
title: "TODO-22 -- Camera, Video Capture & Imaging Devices"
---

# TODO-22 -- Camera, Video Capture & Imaging Devices

> **Goal:** Add the hardware driver layer for webcams, laptop cameras, capture devices, scanners, privacy LEDs, and imaging controls. User applications and media frameworks live elsewhere; this TODO owns kernel device discovery, streaming buffers, frame formats, privacy enforcement, and scanner/camera class integration.
> **Current state:** There is no UVC, MIPI/IPU, scanner, capture, privacy LED, or camera permission driver path. USB isochronous support is planned in TODO-10 and firmware loading in TODO-06.

## Inputs

- -> XREF: `TODO-10-usb-stack.md §3` -- isochronous transfers for USB Video Class
- -> XREF: `TODO-06-firmware-loader-device-blobs.md` -- camera/IPU firmware and calibration blobs
- -> XREF: `TODO-19-hardware-monitoring-sensors.md` -- privacy switch, lid/posture, ALS interaction
- -> XREF: `02-kernel-core/TODO-15-security-reference-monitor.md` -- camera permissions
- -> XREF: `08-desktop-shell` / media domain -- camera app and capture UX consume this API

## Outcome

- USB UVC cameras stream frames through a safe kernel capture API.
- Laptop camera privacy LEDs and hardware switches are enforced.
- Scanner and still-image devices have a driver-layer owner.
- Camera devices show health, firmware, and permission state in Device Manager.

## Implementation Order

| Priority  | Order | Deliverable                                 | Depends On     | Status |
| --------- | :---: | ------------------------------------------- | -------------- | :----: |
| Parity    |   1   | Camera class API and buffer model           | --             |  [ ]   |
| Parity    |   2   | USB UVC discovery and controls              | §1, TODO-10 §3 |  [ ]   |
| Parity    |   3   | UVC streaming and frame formats             | §2             |  [ ]   |
| Parity    |   4   | Privacy LED/switch enforcement              | §1, TODO-19    |  [ ]   |
| Parity    |   5   | MIPI/IPU laptop camera boundary             | TODO-06        |  [ ]   |
| Parity    |   6   | Capture device and HDMI/USB grabber support | §1             |  [ ]   |
| Parity    |   7   | Scanner/image acquisition class             | §1, TODO-10    |  [ ]   |
| Parity    |   8   | Permissions and audit hooks                 | §1, SRM        |  [ ]   |
| Exclusive |   9   | Camera diagnostics and privacy report       | §1-§8          |  [ ]   |
| Parity    |  10   | Tests and device matrix                     | §1-§9          |  [ ]   |

## 1. Camera Class API and Buffer Model

- [ ] Define `camera_device_t`, frame formats, controls, streaming states, and buffer queues.
- [ ] Use pinned pages or DMA-safe buffers with explicit ownership.
- [ ] Commit: `"drivers/camera: class API and buffers"`

## 2. USB UVC Discovery and Controls

- [ ] Detect UVC interfaces and parse VideoControl descriptors.
- [ ] Expose brightness, contrast, focus, exposure, and privacy controls.
- [ ] Commit: `"drivers/camera: UVC discovery controls"`

## 3. UVC Streaming and Frame Formats

- [ ] Support uncompressed YUY2 and MJPEG baseline.
- [ ] Schedule isochronous transfers through USB core.
- [ ] Commit: `"drivers/camera: UVC streaming"`

## 4. Privacy LED and Switch Enforcement

- [ ] Require LED-on before streaming when hardware exposes LED control.
- [ ] Honor hardware privacy switch and report blocked state.
- [ ] Commit: `"drivers/camera: privacy enforcement"`

## 5. MIPI/IPU Boundary

- [ ] Document unsupported/limited state for IPU3/IPU6 and vendor firmware needs.
- [ ] Add detection stubs and Device Manager messaging.
- [ ] Commit: `"drivers/camera: laptop IPU boundary"`

## 6. Capture Devices

- [ ] Support USB HDMI grabbers that expose UVC.
- [ ] Add frame drop and bandwidth diagnostics.
- [ ] Commit: `"drivers/camera: capture device support"`

## 7. Scanner Class

- [ ] Detect USB scanner class devices where feasible.
- [ ] Define driver-layer scan request and image buffer contract.
- [ ] Commit: `"drivers/imaging: scanner class boundary"`

## 8. Permissions and Audit

- [ ] Gate camera stream start through security policy.
- [ ] Emit audit events for first use and denied access.
- [ ] Commit: `"drivers/camera: permission and audit hooks"`

## 9. Diagnostics

- [ ] Add Device Manager camera details, active client, firmware, format, and privacy state.
- [ ] Add BlackBox privacy violation records.
- [ ] Commit: `"drivers/camera: diagnostics"`

## 10. Tests

- [ ] QEMU/USB fixture for descriptor parsing.
- [ ] Bare-metal USB webcam matrix.
- [ ] Commit: `"test: camera driver matrix"`

## OS Comparison

| Priority  | Feature          | Windows             | Linux         | Impossible OS |
| --------- | ---------------- | ------------------- | ------------- | ------------- |
| Parity    | UVC webcam       | usbvideo.sys        | uvcvideo      | TODO-22 §2-§3 |
| Parity    | Privacy controls | Camera Frame Server | v4l2 controls | TODO-22 §4    |
| Exclusive | Privacy report   | Settings/audit      | portals/logs  | TODO-22 §9    |

