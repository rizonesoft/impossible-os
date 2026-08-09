---
schema_version: 1
id: device-manager
domain: 04-drivers-hardware
status: active
title: "TODO-07 -- Device Manager & Driver Diagnostics"
---

# TODO-07 -- Device Manager & Driver Diagnostics

> **Goal:** Build the live Device Manager GUI and associated diagnostics APIs that give Impossible OS a native hardware-visibility advantage over both Windows (static tree, no live counters) and Linux (CLI-only via `lspci`/`lsusb`) -- covering a PCI device registry, embedded PCI ID database, live interrupt counters, driver health reporting, `/sys/devices` and `/sys/interrupts` VFS files, a composited Device Manager window with auto-refreshing rates, the `SetupDi` Win32 device enumeration API, USB tree integration, `lspci`/`lsusb` shell commands, and driver hot-unload/reload.

> [!IMPORTANT]
> **Partial foundation exists:** `src/kernel/drivers/pci.c` implements `pci_scan()` and `pci_find_device(vendor, device)` but does **not** maintain a persistent device registry -- it scans on demand and returns a value, not a pointer to stored state. This TODO builds a permanent `pci_device_db[]` populated once at scan time and queried by all later infrastructure. The existing `pci_find_device()` should be kept for backward-compatibility but reimplemented to search the new registry.

## Inputs

- [`src/kernel/drivers/pci.c`](../../src/kernel/drivers/pci.c) -- existing `pci_scan()` and `pci_find_device()`; extend to populate `pci_device_db[]`
- → XREF: `04-drivers-hardware/TODO-05-kernel-module-system.md §3` -- driver model HAL vtables (`net_ops`, `blk_ops`, `input_ops`) define the `driver_name` field used by §3 health registry; module loader must register names
- → XREF: `04-drivers-hardware/TODO-02-apic-interrupt-routing.md §6` -- per-vector `irq_ns` timing profiler lives in the APIC TODO; §4 here adds `irq_count[]` / `irq_rate()` complementary stats; coordinate to avoid double-tracking
- → XREF: `08-graphics-ui` domain -- Device Manager window (§7) is a compositor-managed GUI window using the same tree-view and panel widgets as other system tools; widget library must support `tree_view_t` with expand/collapse
- → XREF: `04-drivers-hardware/TODO-10-usb-stack.md` -- USB device tree (§8) walks the `usb_device_t` list populated by the xHCI/EHCI drivers (via §1 usb_core API); USB hot-plug events (§8) must trigger a Device Manager tree refresh; string descriptors (§2) provide device names for `lsusb`

## Outcome

- `pci_device_db[]` is the single source of truth for enumerated hardware; all diagnostic tools query it rather than re-scanning PCI.
- Embedded PCI ID database resolves vendor and device names for the top-2000 entries (~60 KiB) without network access.
- Live per-vector interrupt counters (`irq_count[]`, `irq_rate()`) expose real-time IRQ pressure.
- Driver health registry (`driver_health_map`) lets any driver report OK/WARN/ERROR; Device Manager shows a status badge per device.
- `/sys/devices` and `/sys/interrupts` VFS files provide machine-readable diagnostics for scripts and the shell.
- Device Manager GUI: auto-refreshing tree + detail panel; live interrupt rate; driver unload/reload from context menu.
- `SetupDiGetClassDevs` / `SetupDiEnumDeviceInfo` Win32 API surface for application-level device enumeration.
- `lspci` and `lsusb` shell commands output Linux-compatible tabular format.

## Implementation Order

| ⭐  | Order | Deliverable                                                                    | Depends On                                  | Status |
| --- | :---: | ------------------------------------------------------------------------------ | ------------------------------------------- | :----: |
| 💎  |   1   | §1 PCI device registry -- `pci_device_db[]`, BDF/vendor/driver storage         | `pci_scan()` (existing)                     |  [ ]   |
| 💎  |   2   | §2 Embedded PCI ID database -- 2000-entry vendor/device name lookup            | §1 (IDs known at scan time)                 |  [ ]   |
| 💎  |   3   | §3 Live interrupt counter -- `irq_count[]`, `irq_rate()`, 1 s sliding window   | IDT stubs (existing)                        |  [ ]   |
| 💎  |   4   | §4 Driver health registry -- `DRIVER_HEALTH_*` map, probe/error updates        | §1 (driver name linked to PCI entry)        |  [ ]   |
| 💎  |   5   | §5 `/sys/devices` + `/sys/interrupts` VFS files                                | §1–4 (data sources ready)                   |  [ ]   |
| 💎  |   6   | §6 `lspci` / `lsusb` shell commands                                            | §1–2 (registry + names), §8 (USB list)      |  [ ]   |
| ⭐  |   7   | §7 Device Manager GUI -- tree view + detail panel + auto-refresh               | §1–5 (all data sources), compositor widgets |  [ ]   |
| 💎  |   8   | §8 USB device tree integration                                                 | §7 (tree widget), TODO-10 USB device list   |  [ ]   |
| 💎  |   9   | §9 `SetupDiGetClassDevs` / `SetupDiEnumDeviceInfo` Win32 API                   | §1 (PCI registry), §4 (health/driver name)  |  [ ]   |
| 💎  |  10   | §10 Driver unload/reload -- context menu → `module_unload`, `driver_probe_all` | §7 (GUI), TODO-05 module system             |  [ ]   |

> §7 Device Manager with live interrupt-rate auto-refresh is `⭐` exclusive: Windows Device Manager is a static MMC snap-in with no live counters; Linux has no built-in GUI Device Manager (only `lspci`/`lsusb` CLI + third-party tools like `hardinfo`). Impossible OS ships a first-class native Device Manager with live IRQ rates and driver health badges in the base OS.

---

## 1. PCI Device Registry `[Sonnet]`

During `pci_scan()`, populate a static `pci_device_db[]` array with one entry per found device: BDF, vendor/device/class IDs, BAR addresses, IRQ vector, and bound driver name. Provide iteration and lookup APIs. Retrofit `pci_find_device()` to search the registry.

**Files:** `src/kernel/drivers/pci.c` (extend), `include/kernel/drivers/pci.h` (extend)

- [ ] Define `pci_device_entry_t { uint8_t bus,dev,fn; uint16_t vendor_id,device_id; uint8_t class,subclass,prog_if,rev; uint32_t bar[6]; uint8_t irq; char driver_name[32]; uint8_t health; }` in `pci.h`
- [ ] `static pci_device_entry_t pci_device_db[MAX_PCI_DEVICES]` (256 entries); `uint32_t pci_device_count = 0` in `pci.c`
- [ ] Extend `pci_scan()`: for each found device, fill a `pci_device_entry_t` and append to `pci_device_db[]`; set `driver_name[0] = '\0'` (unbound) and `health = DRIVER_HEALTH_UNKNOWN`
- [ ] `pci_find_device(vendor, device)` retrofitted: iterate `pci_device_db[]` and return pointer to matching entry (or `NULL`); keep the old value-return wrapper for ABI compat
- [ ] `pci_find_device_by_bdf(bus, dev, fn)` → pointer to entry
- [ ] `pci_enumerate_devices(callback)` → call `callback(entry)` for each entry; used by Device Manager, SetupDi, lspci
- [ ] `pci_bind_driver(entry, driver_name)` → copy `driver_name` into entry; called from each driver's probe function
- [ ] Commit: `"drivers: PCI device registry -- pci_device_db[], pci_bind_driver, pci_enumerate_devices"`

## 2. Embedded PCI ID Database `[Sonnet]`

Embed a compact binary lookup table of the top-2000 PCI vendor/device IDs by market share (~60 KiB). Provide `pci_vendor_name(vid)` and `pci_device_name(vid, did)` returning static strings.

**Files:** `src/kernel/drivers/pci_ids.c` (new), `include/kernel/drivers/pci_ids.h` (new), `tools/gen_pci_ids/gen_pci_ids.py` (new build-time generator)

> [!NOTE]
> Source data: [https://pci-ids.ucw.cz/v2.2/pci.ids](https://pci-ids.ucw.cz/v2.2/pci.ids) (public domain). The generator script filters to the top-2000 entries by common hardware market share (Intel, AMD, NVIDIA, Realtek, Broadcom, Marvell, etc.) and emits a C array of `{ uint16_t vid, uint16_t did, const char *name }` structs, sorted by `(vid << 16) | did` for binary search.

- [ ] `tools/gen_pci_ids/gen_pci_ids.py`: parse `pci.ids`; filter top-2000 entries; emit `src/kernel/drivers/pci_ids_generated.c` with `const pci_id_entry_t pci_id_table[] = { ... }` and `const uint32_t pci_id_count`
- [ ] `pci_id_entry_t { uint16_t vendor_id; uint16_t device_id; const char *name; }` -- device name for `vid:did` pairs; separate `pci_vendor_entry_t { uint16_t vendor_id; const char *name; }` for vendor-only lookup
- [ ] `pci_vendor_name(vid)` → binary search vendor table; return name or `"Unknown Vendor"`
- [ ] `pci_device_name(vid, did)` → binary search `(vid<<16)|did` in combined table; return device name or `"Unknown Device"`
- [ ] Add generator to build system: `make pci_ids` target runs the Python script before kernel compilation; generated file committed to repo (not regenerated on every build)
- [ ] Boot log: `[PCI] ID database: %u entries loaded`
- [ ] Commit: `"drivers: embedded PCI ID database -- gen_pci_ids.py, 2000-entry binary-search table"`

## 3. Live Interrupt Counter `[Sonnet]`

Maintain a per-IRQ-vector `irq_count[]` (u64) array incremented in each IDT stub. Track `irq_last_ns[]` timestamps. `irq_rate(vec)` computes interrupts-per-second over a 1-second sliding window. Match `NtQuerySystemInformation(SystemInterruptInformation)` semantics.

**Files:** `src/kernel/irq_stats.c` (new), `include/kernel/irq_stats.h` (new); IDT stubs in `src/kernel/idt.c` or equivalent (extend)

> [!NOTE]
> → XREF: `04-drivers-hardware/TODO-02-apic-interrupt-routing.md §6` -- per-vector `irq_ns` timing (latency profiler) lives there. This section tracks `irq_count` (cumulative) and `irq_rate` (current Hz). The two are complementary; if the APIC interrupt profiler already stores `irq_count`, use that array here rather than duplicating.

- [ ] `extern uint64_t irq_count[256]` -- one counter per IDT vector; zeroed at init; incremented with `__atomic_fetch_add` (or locked INC for SMP correctness)
- [ ] `extern uint64_t irq_last_ns[256]` -- timestamp of last occurrence; written in IDT stub via `hpet_read_ns()`
- [ ] Add `irq_count[vector]++` to the common IDT C dispatch path (after IRQ acknowledgment, before EOI); keep fast path -- one atomic increment
- [ ] `irq_rate(vec)` -- sample `irq_count[vec]`, sleep 1 s (or use a pre-sampled snapshot), compute delta; return Hz as `uint32_t`; for Device Manager auto-refresh, use a background sampler that updates `irq_rate_cache[256]` every 1 s via a kernel timer
- [ ] `irq_stats_snapshot(buf, max)` → fill array of `{ vector, count, last_ns, rate_hz }` structs; used by `/sys/interrupts` and `NtQuerySystemInformation`
- [ ] `NtQuerySystemInformation(SystemInterruptInformation, buf, size, NULL)`: fill `SYSTEM_INTERRUPT_INFORMATION[256]` with `{ Count, Time, Rate }` per vector
- [ ] Commit: `"kernel: irq_stats -- irq_count[] per-vector, irq_rate() 1s window, NtQuerySystemInformation"`

## 4. Driver Health Registry `[Sonnet]`

Provide a global `driver_health_map` where each driver reports `DRIVER_HEALTH_OK`, `DRIVER_HEALTH_WARN`, or `DRIVER_HEALTH_ERROR`. Updated in probe and error-handling paths. Device Manager reads health per device to display status badges.

**Files:** `src/kernel/driver_health.c` (new), `include/kernel/driver_health.h` (new)

- [ ] Define `typedef enum { DRIVER_HEALTH_UNKNOWN=0, DRIVER_HEALTH_OK=1, DRIVER_HEALTH_WARN=2, DRIVER_HEALTH_ERROR=3 } driver_health_t`
- [ ] `driver_health_map_t { char name[32]; driver_health_t health; char msg[128]; }` -- 64-entry static table
- [ ] `driver_health_set(name, health, msg)` -- update entry; create if absent; protected by `spinlock_t health_lock`; called from driver probe on success (`OK`) and error handlers (`WARN`/`ERROR`)
- [ ] `driver_health_get(name)` → return `driver_health_t` for named driver; `UNKNOWN` if not registered
- [ ] `driver_health_iterate(callback)` → call `callback(name, health, msg)` for all entries; used by Device Manager and `/sys/devices`
- [ ] Each existing driver (`rtl8139`, `ahci`, `virtio_blk`, etc.): add `driver_health_set(name, OK, "")` at end of successful probe; add `driver_health_set(name, ERROR, strerror)` on fatal init failure
- [ ] Commit: `"kernel: driver health registry -- driver_health_set/get, OK/WARN/ERROR badges"`

## 5. `/sys/devices` + `/sys/interrupts` VFS Files `[Sonnet]`

Register two read-only VFS files under `/sys/` that expose `pci_device_db[]` and `irq_stats_snapshot()` in human-readable tabular format.

**Files:** `src/kernel/sys_vfs.c` (new or extend), `include/kernel/sys_vfs.h`

> [!NOTE]
> `/sys/` is a virtual in-memory filesystem backed by read handlers -- no on-disk storage. Each registered VFS file has a `read_handler(buf, offset, len)` that generates text on demand. This matches the Linux `sysfs`/`procfs` pattern but implemented as VFS read callbacks rather than a full synthetic filesystem.

- [ ] `/sys/devices`: one line per PCI device: `BDF  VendorID:DeviceID  VendorName  DeviceName  Driver  Health  IRQ  Rate_Hz`; e.g., `00:02.0  8086:1050  Intel  VirtIO GPU  virtio_gpu  OK  11  423`
- [ ] `/sys/interrupts`: one line per active IRQ vector (rate > 0 or count > 0): `Vec  Count  LastNs  RateHz  Driver`; matches Linux `/proc/interrupts` column style
- [ ] `sys_register_file(path, read_handler)` -- register a VFS read-only file at `path` under the `/sys/` mount point; mount `/sys/` in VFS init
- [ ] Call `sys_register_file("/sys/devices", sys_devices_read)` and `sys_register_file("/sys/interrupts", sys_interrupts_read)` during kernel init
- [ ] `sys_devices_read(buf, offset, len)`: call `pci_enumerate_devices()`, `pci_vendor_name()`, `pci_device_name()`, `driver_health_get()`, `irq_rate_cache[]`; write formatted lines
- [ ] `sys_interrupts_read(buf, offset, len)`: call `irq_stats_snapshot()`; write one line per active vector
- [ ] Commit: `"kernel: /sys/devices + /sys/interrupts VFS files -- tabular PCI + IRQ stats"`

## 6. `lspci` / `lsusb` Shell Commands `[Sonnet]`

Implement `lspci` and `lsusb` as built-in shell commands outputting Linux-compatible tabular format. Suitable for diagnostic scripts and headless environments.

**Files:** `src/shell/cmd_lspci.c` (new), `src/shell/cmd_lsusb.c` (new)

- [ ] `lspci`: iterate `pci_device_db[]`; output one line per device: `BB:DD.F CLASS: VENDOR DEVICE [DRIVER]`; example: `00:02.0 0300: Intel VirtIO GPU [virtio_gpu]`; `-v` flag adds BAR addresses and IRQ; `-n` flag shows numeric IDs only
- [ ] `lspci -k`: include `Kernel driver in use: driver_name` and `Kernel modules: driver_name` lines per device (Linux `lspci -k` format)
- [ ] `lsusb`: iterate `usb_device_list`; output: `Bus BBB Device DDD: ID VVVV:PPPP MANUFACTURER PRODUCT`; hub ports shown with correct Bus/Device numbering; `-t` flag shows tree topology
- [ ] Register both commands in `src/shell/shell.c` command dispatch table
- [ ] `lspci --version` → `lspci 1.0 -- Impossible OS`; `lsusb --version` → same
- [ ] Commit: `"shell: lspci + lsusb -- PCI device registry + USB topology, Linux-compatible format"`

## 7. Device Manager GUI `[Opus]`

Build the Device Manager window: a tree view of PCI devices grouped by class, with a detail panel showing BDF, vendor/device name, driver health badge, IRQ number, and live interrupt rate (auto-refreshed every 1 second). Accessible from the Start menu and system tray context menu.

**Files:** `src/desktop/devmgr.c` (new), `include/desktop/devmgr.h` (new)

> [!NOTE]
> → XREF: `08-graphics-ui` domain -- tree-view widget (`tree_view_t`), icon badges, and auto-refresh timer are compositor-level widget primitives. Device Manager is a first-class system window; it should not be modal and should coexist with other windows. The 1 s refresh uses a `wm_set_timer(1000, devmgr_refresh_cb)` compositor timer.

- [ ] Tree structure by PCI class: `Storage (0x01)`, `Network (0x02)`, `Display (0x03)`, `Multimedia (0x04)`, `Bridge (0x06)`, `Input (0x09)`, `USB Controller (0x0C)`, `Other`; use class code from `pci_device_entry_t.class`
- [ ] Each leaf node: icon (class-specific), `DeviceName (VendorName)`, health badge: `✅ OK` / `⚠️ Warning` / `❌ Error` / `❔ Unknown`
- [ ] Detail panel (right-click → Properties, or single-click): `BDF`, `Vendor ID : Device ID`, `Vendor Name`, `Device Name`, `Driver`, `Health`, `IRQ vector`, `IRQ rate (Hz)`, `BAR[0..5]` addresses; update rate fields every 1 s
- [ ] Auto-refresh: `wm_set_timer(window, DEVMGR_TIMER_ID, 1000)`; in `WM_TIMER` handler, re-read `irq_rate_cache[]` for all displayed devices; repaint rate column only (dirty-region optimization)
- [ ] Right-click context menu per device: `Properties`, `Disable` (stub), `Unload Driver` (§10), `Reload Driver` (§10)
- [ ] Header bar: `File > Save Report` → writes `/sys/devices` snapshot to `C:\Users\%username%\Documents\devmgr-report.txt`
- [ ] Accessible via `Start Menu > System Tools > Device Manager` and system tray right-click
- [ ] Commit: `"desktop: Device Manager -- class tree, health badges, live IRQ rate, 1s auto-refresh"`

## 8. USB Device Tree Integration `[Sonnet]`

Walk the `usb_device_t` list (populated by TODO-10 xHCI/EHCI drivers) and display the hub topology under a "Universal Serial Bus Controllers" tree node in Device Manager. Show class driver name for each USB device.

**Files:** `src/desktop/devmgr.c` (extend §7), `src/kernel/drivers/usb_hub.c` (extend -- expose `usb_device_list`)

> [!NOTE]
> → XREF: `04-drivers-hardware/TODO-10-usb-stack.md` -- USB device enumeration and hub topology are maintained by the xHCI/hub drivers. This section reads the existing `usb_device_t` list (populated via §1 usb_core API); it does not re-implement enumeration. Device names come from §2 string descriptors. USB hot-plug events from §8 of TODO-10 should post `WM_DEVMGR_REFRESH` to the Device Manager window to trigger a tree rebuild.

- [ ] `usb_device_iterate(callback)` → expose from `src/kernel/drivers/xhci_dev.c`; call `callback(usb_device_t*)` for each known USB device (root-hub ports + downstream hub ports)
- [ ] `usb_device_t` must expose: `slot`, `port`, `parent_slot` (0=root), `speed`, `class`, `subclass`, `protocol`, `class_driver_name[32]` (set by HID/MSC/HUB probe)
- [ ] Device Manager "Universal Serial Bus Controllers" node: root is the xHCI controller; each port below it is a device or hub; hub ports show their downstream devices as children
- [ ] Per USB device: icon (keyboard/mouse/storage/hub/generic), `class_driver_name`, speed badge (`SS`/`HS`/`FS`/`LS`), port number
- [ ] USB hot-plug: `xhci_handle_port_event()` (TODO-10 §8) posts `WM_DEVMGR_REFRESH` after attach/detach; Device Manager receives the message and calls `devmgr_rebuild_usb_subtree()`
- [ ] Commit: `"desktop: Device Manager USB tree -- hub topology, class driver name, hot-plug refresh"`

## 9. `SetupDiGetClassDevs` / `SetupDiEnumDeviceInfo` Win32 API `[Sonnet]`

Implement the Win32 `SetupDi` device enumeration API surface so applications can query installed devices by class GUID, enumerate `SP_DEVINFO_DATA` records, and retrieve device properties (driver name, hardware ID).

**Files:** `src/kernel/win32/setupdi.c` (new), `include/kernel/win32/setupapi.h` (new)

> [!NOTE]
> Windows `SetupDi` API: `SetupDiGetClassDevs(ClassGuid, Enumerator, hwndParent, Flags)` returns `HDEVINFO` handle. `SetupDiEnumDeviceInfo(DeviceInfoSet, MemberIndex, DeviceInfoData)` fills `SP_DEVINFO_DATA { cbSize, ClassGuid, DevInst, Reserved }`. `SetupDiGetDeviceProperty(Set, Data, PropertyKey, PropertyType, Buffer, BufferSize, RequiredSize, Flags)` returns properties. Minimum for compatibility: `DEVPKEY_Device_DriverDesc`, `DEVPKEY_Device_HardwareIds`, `DEVPKEY_Device_Class`.

- [ ] `SetupDiGetClassDevs(ClassGuid, NULL, NULL, DIGCF_PRESENT)` → iterate `pci_device_db[]`; filter by PCI class GUID mapping; return opaque `HDEVINFO` handle (pointer to internal list)
- [ ] `SetupDiEnumDeviceInfo(devinfo, index, &data)` → fill `SP_DEVINFO_DATA` for device at `index`; `DevInst = (bus<<8)|(dev<<3)|fn` (device instance ID); return `FALSE` when index out of range
- [ ] `SetupDiGetDeviceProperty(set, data, key, &type, buf, bufsize, &reqsize, 0)`:
  - `DEVPKEY_Device_HardwareIds` → `"PCI\\VEN_%04X&DEV_%04X&SUBSYS_00000000&REV_%02X"` string
  - `DEVPKEY_Device_DriverDesc` → `pci_device_name(vid, did)`
  - `DEVPKEY_Device_Class` → class name string (`"Net"`, `"DiskDrive"`, `"Display"`, etc.)
- [ ] `SetupDiDestroyDeviceInfoList(devinfo)` → free the handle
- [ ] Register syscall: `NtSetupDi*` dispatch in native API table; `kernel32.dll` stub wrapper (or direct syscall for now)
- [ ] Commit: `"win32: SetupDiGetClassDevs/EnumDeviceInfo -- PCI registry wrapper, DEVPKEY properties"`

## 10. Driver Unload / Reload `[Sonnet]`

Expose "Unload Driver" and "Reload Driver" actions in the Device Manager context menu. Calls `module_unload(name)` → driver `remove()` → frees module memory. Reload calls `driver_probe_all()` to re-bind any unbound device.

**Files:** `src/desktop/devmgr.c` (extend §7), `src/kernel/drivers/driver_model.c` (extend)

> [!NOTE]
> → XREF: `04-drivers-hardware/TODO-05-kernel-module-system.md` -- `module_unload()` and `driver_probe_all()` are defined in the module system TODO. This section wires those calls into the Device Manager UI; the implementation of `module_unload` lives in TODO-05. This section is blocked on TODO-05 completion.

- [ ] `driver_unload_by_device(pci_device_entry_t *dev)`: look up module name from `dev->driver_name`; call `module_unload(name)` (TODO-05 API); clear `dev->driver_name` and set `dev->health = DRIVER_HEALTH_UNKNOWN`; post `WM_DEVMGR_REFRESH`
- [ ] `driver_reload_by_device(pci_device_entry_t *dev)`: call `module_load(driver_name)` then `driver_probe_all()` (TODO-05 API) which rescans unbound devices and binds matching modules; post `WM_DEVMGR_REFRESH`
- [ ] Device Manager context menu handler: on `Unload Driver` → confirm dialog → call `driver_unload_by_device()`; on `Reload Driver` → call `driver_reload_by_device()`
- [ ] Guard: if device has no bound driver (`driver_name[0]=='\0'`), gray out `Unload Driver`; show `Install Driver` (stub, links to future driver store)
- [ ] Health update: after reload, if `driver_health_get(name) == OK`, show `✅`; update tree node badge
- [ ] Commit: `"desktop: driver unload/reload -- module_unload/load, driver_probe_all, DevMgr context menu"`

---

## OS Comparison


| ⭐  | Feature                                                   | 🪟 Win11                                                                    | 🐧 Linux                                                          | 🚀 Impossible OS                                                                |
| --- | --------------------------------------------------------- | --------------------------------------------------------------------------- | ----------------------------------------------------------------- | ------------------------------------------------------------------------------- |
| 💎  | PCI device registry                                       | ✅ PnP manager device tree; `HKLM\SYSTEM\CurrentControlSet\Enum\PCI\`       | ✅ `struct pci_dev` in driver model;                              | ⬜ §1 -- `pci_device_db[]`, `pci_bind_driver()`, `pci_enumerate_devices()`      |
| 💎  | Embedded PCI ID name database                             | ✅ `pci.ids` via Windows Update; `SetupAPI`                                 | ✅ `pci.ids` file; `libpci` or kernel                             | ⬜ §2 -- 2000-entry binary-search C array, `gen_pci_ids.py`                     |
| 💎  | Live per-IRQ interrupt counter + rate                     | ✅ `NtQuerySystemInformation(SystemInterruptInformation)`; Perfmon counters | ✅ `/proc/interrupts`; per-CPU vector counts; `perf               | ⬜ §3 -- `irq_count[]`, `irq_rate()` 1 s window,                                |
| 💎  | Driver health status reporting                            | ✅ Device Manager yellow `!` /                                              | ✅ `driver_probe()` return codes; `dmesg` errors;                 | ⬜ §4 -- `driver_health_map`, `DRIVER_HEALTH_OK/WARN/ERROR`, probe-path updates |
| 💎  | `/sys/devices` + `/sys/interrupts` machine-readable VFS   | ✅ WMI `Win32_PnPEntity`; registry; no simple                               | ✅ `/sys/bus/pci/devices/`, `/proc/interrupts` -- canonical Linux | ⬜ §5 -- `sys_register_file()`, tabular PCI + IRQ                               |
| 💎  | `lspci` / `lsusb` CLI commands                            | ❌ No inbox `lspci`/`lsusb`; only WMI                                       | ✅ `lspci`/`lsusb`; Linux standard diagnostic tools               | ⬜ §6 -- built-in shell commands, Linux-compatible column                       |
| ⭐  | Native GUI Device Manager with live IRQ-rate auto-refresh | ⚠️ `devmgmt.msc` static MMC snap-in; no                                     | ❌ No built-in GUI Device Manager;                                | ⬜ §7 -- tree + detail panel, `✅⚠️❌`                                          |
| 💎  | USB hub topology in Device Manager tree                   | ✅ Device Manager shows USB hub                                             | ❌ No built-in GUI; `lsusb -t`                                    | ⬜ §8 -- hub parent/child tree in DevMgr,                                       |
| 💎  | `SetupDiGetClassDevs` / `SetupDiEnumDeviceInfo` Win32 API | ✅ Full `SetupAPI.dll` -- `GetClassDevs`, `EnumDeviceInfo`,                 | ✅ `libudev` equivalent; `udev_enumerate_*` API                   | ⬜ §9 -- `HDEVINFO`, `SP_DEVINFO_DATA`, `DEVPKEY_Device_*` property keys        |
| 💎  | Driver hot-unload / reload from UI                        | ✅ Device Manager → Disable/Enable; driver                                  | ✅ `modprobe -r` / `modprobe`; `udevadm                           | ⬜ §10 -- context menu → `module_unload` +                                      |

> **After §1–10:** Impossible OS ships a Device Manager that Windows 11 and Linux cannot match out-of-the-box. The live IRQ-rate auto-refresh (§7, `⭐`) turns Device Manager from a static hardware list into a real-time diagnostic tool -- useful for identifying IRQ storms, misbehaving drivers, and latency bottlenecks without needing `perf` or Perfmon. Linux users rely on CLI tools and optional third-party GUIs; Windows users get a static snap-in. Impossible OS integrates driver health, live counters, USB topology, and hot-unload in a single native window.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] `pci_enumerate_devices(cb)` in kernel: callback called once per scanned device; `driver_name` field shows `"rtl8139"` for RTL8139 device after probe
- [ ] `pci_vendor_name(0x8086)` → `"Intel Corporation"`; `pci_device_name(0x10EC, 0x8139)` → `"RTL-8100C/8139D"`
- [ ] `irq_count[11]` increments on each NIC interrupt; `irq_rate(11)` returns ~100 Hz under `ping` flood
- [ ] `driver_health_set("rtl8139", DRIVER_HEALTH_OK, "")` called at probe; `driver_health_get("rtl8139")` → `OK`
- [ ] `cat /sys/devices` in shell: one line per PCI device with BDF, names, driver, health, IRQ, rate
- [ ] `cat /sys/interrupts` in shell: active vectors listed with counts and rates
- [ ] `lspci` output: correct BDF, class, vendor, device per enumerated device; matches QEMU device list
- [ ] `lsusb -t` output: hub topology for USB keyboard connected via `usb-xhci`
- [ ] Device Manager window opens: tree shows Storage/Network/Display/USB nodes; RTL8139 shows `✅ OK`; IRQ rate column updates every 1 s
- [ ] Unload Driver: context menu on RTL8139 → confirm → module unloaded; tree entry shows `❔ Unknown`; network stops; Reload Driver → module reloaded; `✅ OK` returns
- [ ] `SetupDiGetClassDevs(NET_GUID)` returns handle; `EnumDeviceInfo(0, &data)` fills `SP_DEVINFO_DATA` for RTL8139
- [ ] Commit: `"drivers+desktop: Device Manager -- PCI registry, PCI IDs, IRQ stats, health, GUI, lspci/lsusb, SetupDi"`
