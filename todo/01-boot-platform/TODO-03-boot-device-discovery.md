# TODO-03 — Boot Device Discovery & Fallback Chain

> **Goal:** The bootloader must correctly identify which device it booted from, load the kernel from that device (not a random filesystem), and support a priority-based fallback chain across SATA, NVMe, USB, and network devices. Current code uses `LocateProtocol()` which returns an arbitrary filesystem — on multi-disk systems this loads the kernel from the wrong device. Windows uses the BCD store + Loaded Image device path; GRUB uses device enumeration + search. This TODO implements proper boot device identification and a fallback chain so the OS boots reliably on any hardware configuration.

> [!IMPORTANT]
> **Current state:** `load_kernel()` calls `LocateProtocol(SIMPLE_FILE_SYSTEM)` which returns the FIRST filesystem protocol handle registered by firmware — not necessarily the boot device. On systems with multiple disks (SATA + USB), the kernel may be loaded from the wrong partition. `parse_boot_conf()` has the same issue. No boot device priority, no fallback if primary device fails.

---

## Inputs

- `src/boot/uefi/bootx64.c` — `load_kernel()` and `parse_boot_conf()` filesystem access
- `include/kernel/boot_info.h` — boot_info struct (needs boot device info)
- → XREF: `TODO-01-uefi-hardening-secureboot.md §8` — multi-OS detection and boot menu
- → XREF: `TODO-02-bootloader-error-recovery.md §3` — fallback kernel search paths
- → XREF: `TODO-10-xhci-usb-boot.md §5` — USB device handover

---

## Outcome

- Bootloader uses `LoadedImage->DeviceHandle` to identify the actual boot device.
- Kernel and boot.conf are always loaded from the boot device's filesystem, not a random one.
- `boot_info` contains the boot device type (SATA, NVMe, USB, network) and device path.
- If boot device fails, fallback chain tries other devices in priority order.
- UEFI boot variables (BootOrder, BootCurrent) are respected and logged.

---

## Implementation Order

| ⭐  | Order | Deliverable                                       | Depends On    | Status |
| --- | :---: | ------------------------------------------------- | ------------- | :----: |
| 💎  |   1   | Boot device identification via LoadedImage         | —             |  [ ]   |
| 💎  |   2   | Filesystem access scoped to boot device            | §1            |  [ ]   |
| 💎  |   3   | Boot device info in boot_info struct               | §1            |  [ ]   |
| 💎  |   4   | Boot device type detection (SATA/NVMe/USB/Net)     | §3            |  [ ]   |
| 💎  |   5   | Device fallback chain (priority-based)             | §2, §4        |  [ ]   |
| ⭐  |   6   | Boot device logging and diagnostics                | §1–§5         |  [ ]   |

> 💎 = parity — Windows (BCD + device path) and Linux (GRUB device search) both do this.
> ⭐ = exclusive — detailed boot device diagnostics logged to serial showing full enumeration.

---

## 1. Boot Device Identification via LoadedImage

Use UEFI `EFI_LOADED_IMAGE_PROTOCOL` to find which device the bootloader was loaded from.

- [ ] After `efi_main()` entry: call `HandleProtocol(ImageHandle, EFI_LOADED_IMAGE_PROTOCOL_GUID, &loaded_image)`
- [ ] Extract `loaded_image->DeviceHandle` — this is the handle of the device the bootloader was loaded from
- [ ] Store `DeviceHandle` for use by `parse_boot_conf()` and `load_kernel()`
- [ ] Log: `"[BOOT] Boot device: handle=0x%x"` on serial
- [ ] Commit: `"boot: identify boot device via EFI_LOADED_IMAGE_PROTOCOL"`

**Test checkpoint:** Serial output shows `"Boot device: handle=0x..."` on all platforms. Handle is non-zero.

---

## 2. Filesystem Access Scoped to Boot Device

Replace `LocateProtocol(SIMPLE_FILE_SYSTEM)` with `HandleProtocol(DeviceHandle, SIMPLE_FILE_SYSTEM)`.

- [ ] In `parse_boot_conf()`: open filesystem from boot `DeviceHandle`, not global `LocateProtocol`
- [ ] In `load_kernel()`: same change — use boot device filesystem
- [ ] If `DeviceHandle` doesn't have `SIMPLE_FILE_SYSTEM_PROTOCOL`: fall back to `LocateProtocol` with warning
- [ ] Log: `"[BOOT] Using boot device filesystem"` or `"[WARN] Boot device has no filesystem, using fallback"`
- [ ] Commit: `"boot: scope filesystem access to boot device — no more random disk"`

**Test checkpoint:** On QEMU with single disk, behavior unchanged. On multi-disk (SATA + NVMe test), kernel loads from correct device.

**Regression risk:** MEDIUM — changes how filesystem is located. If `DeviceHandle` is wrong, falls back to old behavior.

---

## 3. Boot Device Info in boot_info

Pass boot device information to the kernel so it knows which device it booted from.

- [ ] Add to `boot_info`: `uint8_t boot_device_type` (0=unknown, 1=SATA, 2=NVMe, 3=USB, 4=network)
- [ ] Add to `boot_info`: `uint8_t boot_device_path[128]` — UEFI device path as text string
- [ ] Populate from `DevicePathToText()` UEFI protocol (if available)
- [ ] Kernel logs: `"[BOOT] Booted from: %s (type=%u)"` during boot_info parsing
- [ ] Commit: `"boot: pass boot device type and path in boot_info"`

**Test checkpoint:** Kernel serial output shows `"Booted from: PciRoot(0x0)/Pci(0x2,0x0)/Sata(0x0,0xFFFF,0x0)"` or similar.

---

## 4. Boot Device Type Detection

Classify the boot device as SATA, NVMe, USB, or network based on the device path.

- [ ] Parse UEFI device path nodes: `MESSAGING_DEVICE_PATH/MSG_SATA_DP` → SATA, `MSG_NVME_NAMESPACE_DP` → NVMe, `MSG_USB_DP` → USB, `MSG_IPv4_DP/MSG_IPv6_DP` → network
- [ ] If device path parsing unavailable: check PCI class code via `PciIo` protocol on the device handle
- [ ] Store result in `boot_info.boot_device_type`
- [ ] Commit: `"boot: detect boot device type from UEFI device path"`

**Test checkpoint:** USB boot → `boot_device_type=3`, SATA boot → `boot_device_type=1`.

---

## 5. Device Fallback Chain

If the boot device's kernel is missing or corrupt, try other devices in priority order.

- [ ] Enumerate all `SIMPLE_FILE_SYSTEM_PROTOCOL` handles using `LocateHandleBuffer()`
- [ ] For each handle: check if `\boot\kernel.exe` exists (try to open, close immediately)
- [ ] Priority order: boot device first → SATA/NVMe → USB → other
- [ ] If kernel found on non-boot device: `"[WARN] Kernel not on boot device, using %s"` with device path
- [ ] If no device has kernel: trigger boot failure screen (→ XREF: TODO-02 §9)
- [ ] Commit: `"boot: device fallback chain — search all filesystems for kernel"`

**Test checkpoint:** Remove kernel from SATA disk, leave it on USB. Boot from SATA → bootloader finds kernel on USB with warning.

**Regression risk:** MEDIUM — iterates all filesystem handles. If enumeration is slow on firmware, adds boot time.

---

## 6. Boot Device Logging and Diagnostics

Log the full boot device enumeration to serial for debugging.

- [ ] At boot start: enumerate all `SIMPLE_FILE_SYSTEM_PROTOCOL` handles
- [ ] For each: log device path text and whether `\boot\kernel.exe` exists
- [ ] Log UEFI `BootCurrent` variable: which boot entry firmware selected
- [ ] Log `BootOrder` variable: full priority list
- [ ] Commit: `"boot: comprehensive boot device enumeration logging"`

**Test checkpoint:** Serial output shows list of all available boot devices with kernel presence status.

---

## OS Comparison

| ⭐ | Feature                    | 🪟 Win11                   | 🐧 Linux                    | 🚀 Impossible OS             |
|----|----------------------------|-------------------------|--------------------------|---------------------------|
| 💎 | Boot device identification | ✅ BCD + device path    | ✅ GRUB search command   | ⬜ §1–§2                  |
| 💎 | Multi-device fallback      | ✅ BCD boot order       | ✅ GRUB menu entries     | ⬜ §5                     |
| 💎 | Boot device type in kernel | ✅ Registry boot info   | ✅ /proc/cmdline root=   | ⬜ §3–§4                  |
| ⭐ | Full device enumeration log | ❌ Hidden in Event Log | ❌ Not logged            | ⬜ §6 🚀                  |

---

## Unit Tests

> Boot device discovery runs in UEFI bootloader context -- not kernel test framework.
> Use `scripts/test-smoke.sh` serial pattern matching for boot-level validation.
> Kernel-side boot_info fields can be validated via kernel unit tests.

- [ ] Add smoke test patterns to `scripts/test-smoke.sh`:
  - Serial line `"[BOOT] Boot device: handle="` present (§1 — LoadedImage identification)
  - Serial line `"[BOOT] Using boot device filesystem"` present (§2 — scoped filesystem access)
  - Serial line `"[BOOT] Booted from:"` present (§3 — device path in boot_info)
- [ ] Create `src/kernel/test/test_boot_device.c` with:
  - `boot_info.boot_device_type` is a valid enum value (0-4, not out of range)
  - `boot_info.boot_device_path` is non-empty (at least 1 character)
  - `boot_info.boot_device_type != 0` (UNKNOWN) when booted from a real device (QEMU always has SATA)
- [ ] Register in `test_runner_init()`: `test_register_boot_device()`
- [ ] Commit: `"test: add boot device discovery smoke and unit tests"`

## Verification

- [ ] **Multi-disk test**: QEMU with SATA + NVMe — kernel loads from correct device.
- [ ] **USB boot test**: kernel on USB only — bootloader finds it, logs device type.
- [ ] **Fallback test**: kernel missing from boot device — fallback finds it on another device.
- [ ] **Normal boot regression**: all platforms boot cleanly with new diagnostics in serial.
- [ ] Commit: `"boot: boot device discovery complete — correct device, fallback chain, diagnostics"`
