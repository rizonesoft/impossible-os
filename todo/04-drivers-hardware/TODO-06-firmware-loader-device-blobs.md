---
schema_version: 1
id: firmware-loader-device-blobs
domain: 04-drivers-hardware
status: active
title: "TODO-06 -- Firmware Loader & Device Blob Policy"
---

# TODO-06 -- Firmware Loader & Device Blob Policy

> **Goal:** Provide a safe, auditable firmware-loading subsystem for drivers that require device microcode or configuration blobs: WiFi, Bluetooth, GPU, touchpad, NICs, storage controllers, cameras, and future DSP/NPU devices. The loader must handle licensing metadata, version selection, integrity, rollback, and diagnostics without embedding opaque blobs in kernel code.
> **Current state:** Several TODOs mention firmware needs, but there is no `request_firmware()` equivalent, no on-disk layout, no firmware license manifest, no version policy, and no user-visible failure path when a device is present but its firmware is missing.

## Inputs

- `src/kernel/drivers/`
- `include/kernel/drivers/`
- -> XREF: `TODO-05-kernel-module-system.md` -- modules and driver probe paths call firmware APIs
- -> XREF: `TODO-04-security-hardware.md` -- integrity and TPM measurement policy
- -> XREF: `TODO-07-device-manager.md` -- missing firmware health display
- -> XREF: `TODO-15-wifi-drivers.md` and `TODO-16-bluetooth.md` -- first heavy consumers

## Outcome

- Drivers call `request_firmware(name, constraints, out)` and receive pinned, immutable bytes.
- Firmware lives under a documented tree with license and source metadata.
- Missing or rejected firmware is visible in logs, Device Manager, and BlackBox reports.
- Firmware updates are atomic and rollback-safe.

## Implementation Order

| Priority  | Order | Deliverable                                | Depends On  | Status |
| --------- | :---: | ------------------------------------------ | ----------- | :----: |
| Parity    |   1   | Firmware directory and manifest format     | VFS         |  [ ]   |
| Parity    |   2   | `request_firmware()` kernel API            | §1          |  [ ]   |
| Parity    |   3   | Pinning, lifetime, and DMA-safe copy rules | §2          |  [ ]   |
| Parity    |   4   | Integrity, signature, and hash policy      | §1, TODO-04 |  [ ]   |
| Parity    |   5   | Version matching and fallback order        | §1          |  [ ]   |
| Parity    |   6   | Firmware update and rollback path          | §4          |  [ ]   |
| Parity    |   7   | Device Manager and log integration         | §2, TODO-07 |  [ ]   |
| Exclusive |   8   | License/NOTICE audit report                | §1          |  [ ]   |
| Parity    |   9   | Driver conversion pass                     | §2          |  [ ]   |
| Parity    |  10   | Tests and fixture blobs                    | §1-§9       |  [ ]   |

## 1. Firmware Directory and Manifest Format

- [ ] Define `C:\Impossible\System\Firmware\vendor\device\version\blob.bin`.
- [ ] Add `firmware.toml` or compact INI with device ids, version, license, source URL, hash, and redistribution flag.
- [ ] Define policy for blobs that cannot be redistributed.
- [ ] Commit: `"drivers: firmware manifest and directory policy"`

## 2. `request_firmware()` Kernel API

- [ ] Implement `request_firmware(const char *name, const firmware_constraints_t *, firmware_image_t *)`.
- [ ] Return precise NTSTATUS values for missing, denied, corrupt, wrong version, and unsupported license.
- [ ] Add `release_firmware()` lifetime rules.
- [ ] Commit: `"drivers: request_firmware API"`

## 3. Pinning and DMA-safe Copies

- [ ] Pin firmware pages while a driver owns them.
- [ ] Provide `firmware_copy_to_dma()` for devices that need physically contiguous buffers.
- [ ] Wipe temporary buffers that contain key material or calibration secrets.
- [ ] Commit: `"drivers: firmware lifetime and DMA copy rules"`

## 4. Integrity and Signature Policy

- [ ] Verify SHA-256 before exposing bytes to drivers.
- [ ] Optionally require signatures for security-sensitive firmware.
- [ ] Measure loaded firmware into TPM policy when TODO-04/TODO-12 boot attestation requires it.
- [ ] Commit: `"drivers: firmware integrity enforcement"`

## 5. Version Matching

- [ ] Match by PCI/USB/I2C ids, subsystem id, revision, ACPI HID/CID, and driver-declared minimum version.
- [ ] Support fallback to older known-good firmware when a new blob fails device init.
- [ ] Commit: `"drivers: firmware version selection"`

## 6. Firmware Update and Rollback

- [ ] Stage updates atomically with `.new` and manifest hash verification.
- [ ] Keep previous known-good version until the driver reports success.
- [ ] Commit: `"drivers: firmware update rollback"`

## 7. Device Manager and Log Integration

- [ ] Surface missing firmware as `DRIVER_HEALTH_WARN` with exact blob name.
- [ ] Add `fw list` shell command or Device Manager details tab.
- [ ] Commit: `"drivers: firmware diagnostics"`

## 8. License Audit Report

- [ ] Generate `NOTICE.md` rows from firmware manifests.
- [ ] Refuse non-redistributable blobs in release images unless explicitly marked external.
- [ ] Commit: `"build: firmware license audit report"`

## 9. Driver Conversion Pass

- [ ] Convert WiFi, Bluetooth, touchpad, GPU, camera, and storage firmware references to `request_firmware()`.
- [ ] Add per-driver fallback messages.
- [ ] Commit: `"drivers: convert firmware consumers"`

## 10. Tests

- [ ] Fixture blobs: valid, hash mismatch, missing, revoked, rollback.
- [ ] QEMU tests for missing firmware and success path.
- [ ] Commit: `"test: firmware loader policy"`

## OS Comparison

| Priority  | Feature                    | Windows                | Linux                   | Impossible OS |
| --------- | -------------------------- | ---------------------- | ----------------------- | ------------- |
| Parity    | Firmware API               | WDF/PNP package        | request_firmware        | TODO-06       |
| Parity    | License metadata           | driver package catalog | linux-firmware licenses | TODO-06 §8    |
| Exclusive | BlackBox firmware failures | Event Viewer           | dmesg                   | TODO-06 §7    |

