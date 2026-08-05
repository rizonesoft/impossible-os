---
schema_version: 1
id: usb-stack
domain: 04-drivers-hardware
status: active
title: "TODO-10 -- USB Stack Completion"
---

# TODO-10 -- USB Stack Completion

> **Goal:** Complete the USB stack -- adding a transport-agnostic USB core abstraction layer, isochronous and string descriptor support, hot-plug event handling, hub class driver, EHCI fallback, Bluetooth HCI via USB, CDC-ECM Ethernet, and CDC-ACM serial -- reaching parity with Windows 11's `USBXHCI.sys`/`HIDCLASS.sys`/`USBSTOR.sys` and Linux's `xhci_hcd`/`usbhid`/`usb-storage`.
>
> → **Boot-critical USB sections extracted to `01-boot-platform/TODO-17-xhci-usb-boot.md`:** xHCI bring-up, USB MSC BOT, and USB HID boot-protocol keyboard/mouse. This TODO covers the USB core layer and advanced/non-boot USB features.

> [!IMPORTANT]
> **Partial implementation exists.** `src/kernel/drivers/xhci.c` (381 lines), `xhci_dev.c` (912 lines), `xhci_ring.c` (269 lines) implement: controller halt/reset, DCBAA, scratchpad, TRB command/event rings, port scanning, slot enable, Address Device, GET_DESCRIPTOR (device + configuration), SET_CONFIGURATION, Configure Endpoint, and bulk-endpoint setup for MSC. **Do not rewrite** these files -- complete them. What is missing: a transport-agnostic USB core API (`usb_device_t`, `usb_submit_control/bulk/interrupt()`), isochronous endpoint support, USB string descriptor retrieval, interrupt-endpoint setup for HID, HID boot-protocol report parsers, multi-LUN MSC support, hot-plug interrupt handling, hub class driver, and EHCI fallback.

## Inputs

- [`src/kernel/drivers/xhci.c`](../../src/kernel/drivers/xhci.c) -- xHCI controller init, event ring, port scanning
- [`src/kernel/drivers/xhci_dev.c`](../../src/kernel/drivers/xhci_dev.c) -- slot enable, Address Device, GET_DESCRIPTOR, Configure Endpoint, bulk-endpoint setup
- [`src/kernel/drivers/xhci_ring.c`](../../src/kernel/drivers/xhci_ring.c) -- TRB ring allocation, enqueue/dequeue, doorbell
- [`src/kernel/drivers/usb_msc.c`](../../src/kernel/drivers/usb_msc.c) -- existing BOT SCSI transport (to be refactored behind usb_core API)
- [`src/kernel/drivers/keyboard.c`](../../src/kernel/drivers/keyboard.c), [`src/kernel/drivers/mouse.c`](../../src/kernel/drivers/mouse.c) -- injection targets for HID events
- → XREF: `01-boot-platform/TODO-19-usb-boot-hardening.md §4–§5` -- USB transport error recovery (stall/halt) and bulk transfer timeouts; usb_core API (§1) must expose these recovery primitives to class drivers
- → XREF: `04-drivers-hardware/TODO-14-network-drivers.md §3` -- CDC-ECM (§11) calls `net_register_nic()` into the same network stack as VirtIO-net; NIC registration API must be compatible
- → XREF: `04-drivers-hardware/TODO-05-kernel-module-system.md` -- EHCI fallback (§10) and Bluetooth HCI (§13) are good candidates for loadable `.kmod`; module loader should be available
- → XREF: `04-drivers-hardware/TODO-11-input-system.md §9` -- PS/2↔USB fallback (§10) coordinates with raw-grab ownership; if a raw-grab task owns the mouse, USB HID events must also be routed through `mouse_raw_grab()` path
- → XREF: `04-drivers-hardware/TODO-18-audio-drivers.md §5` -- USB Audio UAC1 depends on §3 (isochronous endpoint support) from this TODO
- → XREF: `02-kernel-core/TODO-26-power-management.md §9` -- USB selective suspend and Link Power Management (U1/U2) deferred to power management; see OS Comparison note

## Outcome

- Transport-agnostic USB core API (`usb_device_t`, `usb_submit_control/bulk/interrupt/isoch()`) -- class drivers work identically on xHCI and EHCI.
- USB string descriptors retrieved for device names (manufacturer, product, serial number) -- used by Device Manager, safe-remove toasts, and CDC-ECM MAC address.
- Isochronous endpoint support for USB audio and video class devices.
- USB HID keyboards and mice work via xHCI interrupt endpoints; boot-protocol reports decoded; events injected into existing keyboard/mouse subsystem.
- USB mass storage drives mount as new drive letters (E:\, F:\, …) via BOT SCSI transport; multi-LUN devices (card readers) expose each LUN as a separate block device; hot-plug attach/detach with desktop toast notifications.
- USB hubs enumerate downstream devices; cascaded hot-plug events work.
- EHCI fallback for USB 2.0-only systems shares the same HID and MSC class drivers via usb_core API.
- PS/2 input gracefully yields to USB HID when a USB keyboard/mouse is connected; re-activates on disconnect.
- Bluetooth HCI via USB provides the foundation for the Bluetooth stack (→ future TODO).
- CDC-ECM USB-to-Ethernet adapters register as NICs and reach the DHCP path.
- CDC-ACM serial adapters expose a virtual serial port for debugging and device communication.

## Implementation Order

| ⭐  | Order | Deliverable                              | Depends On                               | Status |
| --- | :---: | ---------------------------------------- | ---------------------------------------- | :----: |
| 💎  |   1   | §1 USB core abstraction layer (`usb_device_t`, HCD vtable) | `xhci_dev.c` existing transport          |  [ ]   |
| 💎  |   2   | §2 USB string descriptor retrieval       | §1 (usb_submit_control)                  |  [ ]   |
| 💎  |   3   | §3 Isochronous endpoint support          | §1 (usb_device_t, HCD vtable)            |  [ ]   |
| 💎  |   4   | §4 Interrupt endpoint setup for HID (`xhci_configure_interrupt_ep`) | §1 (usb_device_t)                        |  [ ]   |
| 💎  |   5   | §5 USB HID class driver -- keyboard + mouse boot protocol | §4 (interrupt-IN endpoint)               |  [ ]   |
| 💎  |   6   | §6 PS/2 ↔ USB input fallback             | §5 (USB HID active flag)                 |  [ ]   |
| 💎  |   7   | §7 USB MSC BOT completion -- SCSI CBW/CSW, multi-LUN, blkdev | §1 (usb_submit_bulk), §2 (string descriptors) |  [ ]   |
| 💎  |   8   | §8 Hot-plug interrupt handling -- port status change TRB | §5 + §9 (HID + MSC attach/detach paths needed) |  [ ]   |
| 💎  |   9   | §9 USB hub class driver                  | §10 (hot-plug path established)          |  [ ]   |
| 💎  |  10   | §10 EHCI fallback (USB 2.0)              | §1 (HCD vtable), §5, §9 (class drivers to reuse) |  [ ]   |
| 💎  |  11   | §11 USB CDC-ECM Ethernet                 | §1 (usb_submit_bulk/interrupt), §2 (iMACAddress) |  [ ]   |
| 💎  |  12   | §12 USB CDC-ACM serial                   | §1 (usb_submit_bulk, interrupt-IN)       |  [ ]   |
| 💎  |  13   | §13 Bluetooth HCI via USB                | §1 (usb_submit_control/interrupt/bulk)   |  [ ]   |

> All rows are 💎 parity: Windows 11 and Linux ship all listed USB infrastructure and class drivers in their inbox/mainline driver sets. §1–§3 establish the USB core layer that all class drivers (§4–§12) build on.

---

## 1. USB Core Abstraction Layer `[Opus]`

Define a transport-agnostic USB core API so class drivers (HID, MSC, hub, CDC-ECM, CDC-ACM, Bluetooth HCI, USB Audio) never call xHCI or EHCI functions directly. Each host controller driver registers a `usb_hcd_ops_t` vtable; class drivers call `usb_submit_control()` / `usb_submit_bulk()` / `usb_submit_interrupt()` / `usb_submit_isoch()` which dispatch through the vtable. This is the layer that makes EHCI fallback (§10) possible without `if (xhci) ... else if (ehci) ...` branches in every class driver.

**Files:** `src/kernel/drivers/usb_core.c` (new), `include/kernel/drivers/usb_core.h` (new)

> [!NOTE]
> `01-boot-platform/TODO-19 §11` defers its EHCI/UHCI boot-storage fallback here: the "share `usb_msc.c` BOT layer" + "HCD-agnostic recovery" items cannot land until this section's `usb_hcd_ops_t` vtable + `usb_submit_*` error contract exist (today `usb_msc.c` is hardwired to `struct xhci_device`). TODO-19 §11 ships boot-time legacy-controller detection + graceful no-xHCI skip + the multi-controller MSC keying fix; the active HCD is owned here + §10.

> [!NOTE]
> Windows has USBD (USB Driver Interface) between class drivers and HCI miniport drivers. Linux has `struct usb_device` + `usb_submit_urb()` + `struct usb_hcd`. Both achieve the same goal: class drivers are HCI-agnostic. Our `usb_device_t` is simpler -- no URB queuing or async completion callbacks in v1, just synchronous wrappers that return success/error.

- [ ] Define `usb_device_t`: `slot_id`, `port`, `speed` (LS/FS/HS/SS), `vid`, `pid`, `dev_class`, `dev_subclass`, `dev_protocol`, `max_packet_ep0`, `num_interfaces`, `hcd` pointer, `hcd_private` (opaque per-HCD data), `string_manufacturer[64]`, `string_product[64]`, `string_serial[64]`
- [ ] Define `usb_hcd_ops_t` vtable: `submit_control(dev, setup_packet, data, len, timeout_ms)`, `submit_bulk(dev, ep_addr, data, len, timeout_ms)`, `submit_interrupt(dev, ep_addr, data, len, timeout_ms)`, `submit_isoch(dev, ep_addr, data, len, frame_id)`, `configure_endpoint(dev, ep_addr, type, max_pkt, interval)`, `reset_endpoint(dev, ep_id)`, `clear_halt(dev, ep_addr)`
- [ ] Define `usb_hcd_t`: `name` (e.g., "xhci", "ehci"), `ops` (`usb_hcd_ops_t`), `devices[]`, `num_devices`, `private` (opaque controller data)
- [ ] Convert the 271 KiB `devices[XHCI_MAX_DEVICES]` static (`xhci_dev.c:31`) to a frame-backed table during the `usb_hcd_t.devices[]` restructure; `xhci_device_index()` needs a NULL guard + explicit extent -> XREF: `02-kernel-core/TODO-33 §10`
- [ ] `usb_hcd_register(hcd)`: register a host controller; called from `xhci_init()` and future `ehci_init()`
- [ ] `usb_submit_control(dev, setup, data, len)` / `usb_submit_bulk(dev, ep, data, len)` / `usb_submit_interrupt(dev, ep, data, len)`: dispatch through `dev->hcd->ops->submit_*`; return `USB_OK`, `USB_ERR_STALL`, `USB_ERR_TIMEOUT`, `USB_ERR_TRANSPORT`, or `USB_ERR_DEVICE_GONE`
- [ ] `usb_control_msg(dev, request_type, request, value, index, data, len)`: convenience wrapper that builds the 8-byte setup packet and calls `usb_submit_control()`
- [ ] Refactor existing `xhci_bulk_transfer()` and `xhci_control_transfer()` to implement `usb_hcd_ops_t.submit_bulk` and `usb_hcd_ops_t.submit_control`; existing callers (`usb_msc.c`) migrate to `usb_submit_bulk()` / `usb_control_msg()`
- [ ] Commit: `"drivers: USB core abstraction layer -- usb_device_t, usb_hcd_ops_t, transport-agnostic API"`

## 2. USB String Descriptor Retrieval `[Sonnet]`

Implement `GET_DESCRIPTOR(STRING)` to read manufacturer, product, and serial number strings from USB devices. These strings are needed by Device Manager (`lsusb`), safe-remove toast messages, CDC-ECM MAC address retrieval, and general USB diagnostics.

**Files:** `src/kernel/drivers/usb_core.c` (extend), `include/kernel/drivers/usb_core.h` (extend)

> [!NOTE]
> USB string descriptors are UTF-16LE encoded. The first 2 bytes are `bLength` and `bDescriptorType (3)`. String index 0 returns the list of supported LANGIDs; index 1+ returns the actual string. To read a string: (1) `GET_DESCRIPTOR(STRING, index=N, langid=0x0409)` with `wLength=255`; (2) parse: skip 2-byte header, convert UTF-16LE to ASCII by taking every other byte (sufficient for Latin characters).

- [ ] `usb_get_string_descriptor(dev, index, langid, buf, max_len)`: issue `GET_DESCRIPTOR` control transfer (bmRequestType=`0x80`, bRequest=`0x06`, wValue=`0x0300|index`, wIndex=langid, wLength=max_len); return actual bytes read
- [ ] `usb_get_string_ascii(dev, index, buf, max_len)`: call `usb_get_string_descriptor()` with langid=`0x0409` (English); convert UTF-16LE to ASCII; null-terminate; return string length
- [ ] During enumeration: after `GET_DESCRIPTOR(DEVICE)`, read `iManufacturer`, `iProduct`, `iSerialNumber` string indices; if non-zero, call `usb_get_string_ascii()` to populate `usb_device_t.string_manufacturer/product/serial`
- [ ] Log: `"[USB] Device: %s %s (S/N: %s)"` with retrieved strings (or "unknown" if index is 0)
- [ ] Commit: `"drivers: USB string descriptor retrieval -- manufacturer, product, serial number strings"`

## 3. Isochronous Endpoint Support `[Opus]`

Add isochronous endpoint configuration and transfer submission to the USB core and xHCI backend. Isochronous transfers are the fourth fundamental USB transfer type (alongside control, bulk, interrupt) and are required for USB Audio (→ XREF `04-drivers-hardware/TODO-18-audio-drivers.md §5`) and USB Video class devices.

**Files:** `src/kernel/drivers/xhci_dev.c` (extend), `include/kernel/drivers/usb_core.h` (extend)

> [!NOTE]
> xHCI isochronous endpoints use endpoint type `0x1` (ISOCH_OUT) or `0x5` (ISOCH_IN) in the endpoint context. The transfer ring uses Isoch TRBs (type `0x5`) rather than Normal TRBs. Each Isoch TRB carries one USB microframe of data. The `Max ESIT Payload` field in the endpoint context specifies the maximum bytes per Endpoint Service Interval Time. For USB 2.0 (HS): max 3072 bytes/microframe. For USB 3.x (SS): max 48 KiB/ESIT via burst+mult.

- [ ] `xhci_configure_isoch_ep(dev, ep_addr, max_pkt, interval, mult, burst, esit_payload)`: allocate endpoint context; set `EP Type = ISOCH_OUT (0x1)` or `ISOCH_IN (0x5)`; set `Max Packet Size`, `Interval`, `Mult`, `Max Burst Size`, `Max ESIT Payload`; submit Configure Endpoint command
- [ ] `xhci_submit_isoch_transfer(dev, ep_addr, buf, len, frame_id)`: enqueue Isoch TRB with `Frame ID` (or `SIA=1` for start ASAP); `IOC=1`; ring doorbell
- [ ] Register as `usb_hcd_ops_t.submit_isoch` in the xHCI HCD vtable
- [ ] Error handling: isochronous transfers do not retry on failure (real-time data is time-sensitive); log dropped frames; return actual bytes transferred
- [ ] Commit: `"drivers: xHCI isochronous endpoint support -- configure + Isoch TRB submission"`

## 4. Interrupt Endpoint Setup for HID `[Opus]`

Extend `xhci_dev.c` to configure interrupt-IN endpoints. HID devices have a single interrupt-IN endpoint on their first interface; call `xhci_configure_interrupt_ep()` from the enumeration path when interface class == `0x03`.

**Files:** `src/kernel/drivers/xhci_dev.c` (extend), `include/kernel/drivers/xhci_dev.h` (extend)

> [!NOTE]
> The bulk-endpoint configuration in `xhci_dev.c` sets endpoint context type `0x2` (BULK_IN) or `0x6` (BULK_OUT). Interrupt endpoint type is `0x3` (INTERRUPT_IN) or `0x7` (INTERRUPT_OUT). The Configure Endpoint command submission path is identical; only the endpoint context type and `Interval` field differ. `Interval` = log2 of the polling period in 125 µs microframes (e.g., `Interval=3` → 1 ms for FS/LS, `Interval=8` → 1 ms for HS).

- [ ] `xhci_configure_interrupt_ep(dev, ep_addr, max_pkt, interval)`: allocate endpoint context in the input context; set `EP Type = INTERRUPT_IN (0x3)`; set `Max Packet Size = max_pkt`; set `Interval = interval`; set `Average TRB Length = max_pkt`; set `Max Burst Size = 0` for FS/LS; submit Configure Endpoint command; wait for Command Completion Event with `CC == SUCCESS`
- [ ] `xhci_submit_interrupt_transfer(dev, ep_addr, buf, len, callback)`: enqueue Normal TRB on the endpoint's transfer ring with `IOC=1`; ring doorbell; on Transfer Event received on event ring with matching TRB pointer, call `callback(buf, actual_len, status)`
- [ ] Update `xhci_enumerate_device()`: after SET_CONFIGURATION, scan parsed interface descriptors; if `bInterfaceClass == 0x03` (HID), call `xhci_configure_interrupt_ep()` then `usb_hid_probe(dev, ep_addr, max_pkt, interval)`
- [ ] Error path: if Configure Endpoint fails (CC != SUCCESS), log `[xHCI] EP config failed slot=%u ep=%u cc=%u`; mark device as non-functional
- [ ] Commit: `"drivers: xhci -- interrupt-IN endpoint setup, INTERRUPT_IN context type, HID probe hook"`

## 5. USB HID Class Driver `[Sonnet]`

Implement `usb_hid.c` with boot-protocol keyboard and mouse parsers. `usb_hid_probe()` issues `SET_PROTOCOL(0)` to request boot-protocol reports. Completions decode reports and inject into the existing keyboard/mouse event pipeline.

**Files:** `src/kernel/drivers/usb_hid.c` (new), `include/kernel/drivers/usb_hid.h` (new)

> [!NOTE]
> Boot-protocol keyboard report (8 bytes): `[modifier][reserved][keycode0][keycode1][keycode2][keycode3][keycode4][keycode5]`. Modifier byte bits: `b0=LCtrl, b1=LShift, b2=LAlt, b3=LMeta, b4=RCtrl, b5=RShift, b6=RAlt, b7=RMeta`. Boot-protocol mouse report (3 bytes): `[buttons][Δx][Δy]` -- `buttons` bits `b0=L, b1=R, b2=M`.

- [ ] `usb_hid_probe(dev, ep_addr, max_pkt, interval)`: determine HID subclass (`bInterfaceSubClass==1` = boot device) and protocol (`1`=keyboard, `2`=mouse); issue `SET_PROTOCOL(0)` control transfer (request type `0x21`, request `0x0B`, value `0`, index = interface, length `0`); submit first interrupt transfer
- [ ] Keyboard completion: on 8-byte report received, decode `modifier` byte (bits 0–7 → modifier flags); iterate `keycode[0..5]`; compare with previous report to detect key-down (new code not in previous) and key-up (old code not in current); call `keyboard_driver_handle_event(keycode, modifier, DOWN/UP)`
- [ ] Mouse completion: decode 3-byte report; call `mouse_driver_handle_event(Δx, Δy, buttons)` -- same path as PS/2 mouse events
- [ ] Re-submit interrupt transfer in completion handler for continuous polling
- [ ] `usb_hid_disconnect(dev)`: cancel pending interrupt transfer; clear `usb_keyboard_active` / `usb_mouse_active` flags (§6)
- [ ] Boot log: `[USB-HID] Keyboard slot=%u` / `[USB-HID] Mouse slot=%u`
- [ ] Commit: `"drivers: USB HID -- boot-protocol keyboard/mouse, SET_PROTOCOL(0), event injection"`

## 6. PS/2 ↔ USB Input Fallback `[Sonnet]`

When USB HID claims a keyboard or mouse, the PS/2 driver must yield and stop injecting duplicate events. When the USB HID device disconnects, PS/2 must automatically re-activate. No application-layer changes required.

**Files:** `src/kernel/drivers/keyboard.c`, `src/kernel/drivers/mouse.c`, `src/kernel/drivers/usb_hid.c`

- [ ] Add `bool usb_keyboard_active` and `bool usb_mouse_active` to the keyboard/mouse driver state (or a shared `input_sources.h` flags header)
- [ ] `usb_hid_probe()` (§5): after successful HID probe, set `usb_keyboard_active = true` / `usb_mouse_active = true`
- [ ] `keyboard_process_event()` (PS/2 path): check `usb_keyboard_active`; if true, return early (discard PS/2 scan code)
- [ ] `mouse_process_event()` (PS/2 path): check `usb_mouse_active`; if true, return early
- [ ] `usb_hid_disconnect()` (§5 hot-plug path, §8): clear `usb_keyboard_active` / `usb_mouse_active`; PS/2 re-activates on next scan code automatically
- [ ] Boot log: `[INPUT] USB keyboard claimed -- PS/2 keyboard suspended` / `PS/2 keyboard resumed`
- [ ] Commit: `"drivers: PS/2↔USB fallback -- usb_keyboard/mouse_active flag, yield/resume on attach/detach"`

## 7. USB MSC BOT Completion `[Opus]`

Complete the USB Mass Storage Class Bulk-Only Transport layer. Bulk endpoints are already set up in `xhci_dev.c`; add `usb_msc_scsi.c` implementing SCSI CBW/CSW framing and `INQUIRY`, `READ CAPACITY`, `READ(10)`, `WRITE(10)` commands. Support multi-LUN devices (card readers). Register via `blkdev_register()`.

**Files:** `src/kernel/drivers/usb_msc_scsi.c` (new), `include/kernel/drivers/usb_msc.h` (new)

> [!NOTE]
> BOT Command Block Wrapper (CBW): 31-byte structure -- `dCBWSignature=0x43425355 ("USBC")`, `dCBWTag` (unique per command), `dCBWDataTransferLength`, `bmCBWFlags` (bit7=IN), `bCBWLUN`, `bCBWCBLength`, `CBWCB[16]` (SCSI CDB). Command Status Wrapper (CSW): 13-byte -- `dCSWSignature=0x53425355 ("USBS")`, `dCSWTag`, `dCSWDataResidue`, `bCSWStatus` (0=pass, 1=fail, 2=phase error).

- [ ] `usb_msc_send_cbw(dev, tag, len, flags, lun, cdb, cdb_len)`: build 31-byte CBW; transmit over bulk-OUT endpoint; wait for Transfer Event
- [ ] `usb_msc_recv_csw(dev, tag)`: read 13-byte CSW from bulk-IN; verify signature and matching tag; return `bCSWStatus`
- [ ] `usb_msc_inquiry(dev)`: CDB `0x12` (INQUIRY), allocation length 36; parse `Peripheral Device Type` and `Product Identification`; log vendor/product string
- [ ] `usb_msc_read_capacity(dev, &sectors, &block_size)`: CDB `0x25` (READ CAPACITY(10)); parse 8-byte response
- [ ] `usb_msc_read(dev, lba, count, buf)`: CDB `0x28` (READ(10)); CBW flags=`0x80` (IN); receive `count * block_size` bytes from bulk-IN; verify CSW
- [ ] `usb_msc_write(dev, lba, count, buf)`: CDB `0x2A` (WRITE(10)); CBW flags=`0x00` (OUT); transmit data to bulk-OUT; verify CSW
- [ ] `usb_msc_get_max_lun(dev)`: issue `GET_MAX_LUN` class request (bmRequestType=`0xA1`, bRequest=`0xFE`, wValue=0, wIndex=interface, wLength=1); returns max LUN index (0 = single LUN, N = LUNs 0..N); if request STALLs, assume max_lun=0 (single LUN, per BOT spec)
- [ ] For each LUN 0..max_lun: issue `INQUIRY`, `READ CAPACITY`; register as separate block device (`usb0`, `usb0.1`, `usb0.2`, etc.); set `bCBWLUN` field in CBW for all SCSI commands to that LUN
- [ ] Register `blkdev_t usb_msc_blkdev { .read=usb_msc_read, .write=usb_msc_write, .sectors, .block_size }` via `blkdev_register()`; VFS auto-mounts next available drive letter (E:\, F:\, …)
- [ ] Boot log: `[USB-MSC] slot=%u LUN=%u vendor="%s" product="%s" %llu sectors mounted as %c:\\`
- [ ] Commit: `"drivers: USB MSC BOT -- SCSI INQUIRY/READ_CAPACITY/READ/WRITE CBW/CSW, multi-LUN, blkdev_register"`

## 8. Hot-Plug Interrupt Handling `[Opus]`

Process xHCI Port Status Change Events (TRB type `0x22`) from the event ring. On connect: enumerate device. On disconnect: tear down class driver, unregister blkdev, notify desktop with toast.

**Files:** `src/kernel/drivers/xhci.c` (extend event loop), `src/kernel/drivers/usb_hid.c`, `src/kernel/drivers/usb_msc_scsi.c`, `src/kernel/drivers/usb_core.c`

> [!NOTE]
> Port Status Change Event TRB: `Port ID` field (bits 31:24) identifies the port. After receiving the event, read `PORTSC[Port ID - 1]` to determine current state: `CCS (bit 0)` = device present, `PED (bit 1)` = port enabled. A CCS=1 transition means a new device connected; CCS=0 means disconnect.

- [ ] Event-ring ownership: ISR only acks + records the port-change, defers enumeration to a serialized worker (no ISR-context blocking); replaces the interim in-ISR enumeration in `01-boot-platform/TODO-17 §5`
- [ ] In `xhci_event_loop()`: handle TRB type `0x22` (Port Status Change); extract port number; read `PORTSC`
- [ ] CCS=1 path (connect): reset port via `PR` bit in PORTSC; wait for `PRC` (port reset complete); call `xhci_enumerate_device(hc, port)` which triggers HID or MSC probe
- [ ] CCS=0 path (disconnect): find device slot by port number in `hc->slot_map[]`; call `usb_device_detach(slot)`
  - If MSC: call `blkdev_unregister(blkdev)`, trigger VFS unmount, send desktop toast: `"USB drive %c:\\ safely removed"`
  - If HID: call `usb_hid_disconnect(dev)`; PS/2 re-activates via §6
- [ ] `usb_device_detach(slot)`: cancel all pending TRBs on all endpoints (Stop Endpoint command); Disable Slot command; free slot context
- [ ] Boot log: `[xHCI] Port %u: device connected` / `Port %u: device disconnected`
- [ ] Publish connect/disconnect via `knf_publish` on the `Device/*` catalog states (in addition to the desktop toast), so system subscribers observe USB hot-plug through the kernel notification facility (-> XREF: D02 T16 §5).
- [ ] Commit: `"drivers: xHCI hot-plug -- Port Status Change TRB, attach/detach, MSC unmount, HID disconnect"`

## 9. USB Hub Class Driver `[Opus]`

Implement the USB hub class driver (interface class `0x09`). Issue GET_DESCRIPTOR(HUB) to read port count. Power each port. Poll via interrupt-IN for port-change bitmask. Enumerate or detach downstream devices on change events. Cascade hot-plug to xHCI.

**Files:** `src/kernel/drivers/usb_hub.c` (new), `include/kernel/drivers/usb_hub.h` (new)

> [!NOTE]
> Hub descriptor (type `0x29`): `bNbrPorts` at offset 2; `wHubCharacteristics` at offset 3; `bPwrOn2PwrGood` at offset 5. Port power-on: `SET_FEATURE(PORT_POWER)` = `SetPortFeature(port, PORT_POWER=8)` via control endpoint. Port status: `GET_STATUS(PORT_N)` returns 4 bytes -- bits `b0=PORT_CONNECTION, b1=PORT_ENABLE, b4=PORT_RESET`.

- [ ] `usb_hub_probe(dev)`: issue GET_DESCRIPTOR(HUB); parse `bNbrPorts`; store in `usb_hub_t`
- [ ] Power sequence: for each port 1..N, issue `SET_FEATURE(PORT_POWER)`; wait `bPwrOn2PwrGood * 2` ms
- [ ] Set up interrupt-IN endpoint (§4 path); submit interrupt transfer for port-change bitmap (1 + ⌈N/8⌉ bytes)
- [ ] Port-change completion: for each set bit `p` in bitmap, issue `GET_STATUS(PORT_p)`; if `PORT_CONNECTION` changed:
  - Connect: `SET_FEATURE(PORT_RESET)` → wait `PORT_RESET` clear → call `xhci_enumerate_device()` for downstream slot
  - Disconnect: `usb_device_detach(downstream_slot)`
- [ ] Re-submit interrupt transfer after each change notification
- [ ] Boot log: `[USB-HUB] slot=%u ports=%u`
- [ ] Commit: `"drivers: USB hub class -- GET_DESCRIPTOR(HUB), port power, interrupt-IN, downstream enumerate"`

## 10. EHCI Fallback (USB 2.0) `[Opus]`

Implement an EHCI host controller driver for USB 2.0-only systems. PCI prog-if `0x20`. Async schedule (QH→QTD chain) for control/bulk; periodic schedule (256-entry frame list) for interrupt transfers. Register as a `usb_hcd_t` via §1's `usb_hcd_register()`; all class drivers (§5–§9, §13–§12) work automatically via the USB core abstraction layer.

**Files:** `src/kernel/drivers/ehci.c` (new), `include/kernel/drivers/ehci.h` (new)

> [!NOTE]
> `01-boot-platform/TODO-19 §11` (USB boot hardening) defers its "minimal EHCI driver" + "EHCI/UHCI recovery at §4/§5 parity" items to this section; it already ships the boot-time detection (`usb_legacy_scan`) + graceful no-xHCI skip that this HCD plugs into. Boot-storage integration (mount the EHCI-backed device, run the §2-§5 MSC hardening through the shared BOT layer) is owned by TODO-19 §11 once this driver + §1 land.

> [!NOTE]
> EHCI MMIO: BAR0 offset 0 = capability registers (`CAPLENGTH`, `HCIVERSION`, `HCSPARAMS`, `HCCPARAMS`). Operational registers start at BAR0 + `CAPLENGTH`: `USBCMD`, `USBSTS`, `USBINTR`, `FRINDEX`, `CTRLDSSEGMENT`, `PERIODICLISTBASE`, `ASYNCLISTADDR`. Async schedule: QH (Queue Head) linked list; each QH has a QTD (Queue Transfer Descriptor) chain. Companion controllers (OHCI/UHCI) handle FS/LS devices via port release.

- [ ] PCI match: PCI class `0x0C`, subclass `0x03`, prog-if `0x20` (EHCI)
- [ ] Map BAR0; read `CAPLENGTH`; operational base = BAR0 + CAPLENGTH
- [ ] Controller init: halt (`USBCMD.RS=0`); reset (`USBCMD.HCRESET=1`); wait; configure `PERIODICLISTBASE` (256-entry frame list, 1 ms frames); configure `ASYNCLISTADDR` (dummy QH head, circular)
- [ ] Port scan: read `PORTSC[0..N-1]`; for connected FS/HS ports: reset port, determine speed from `PORTSC.PSPD`, enumerate with control transfers (same GET_DESCRIPTOR / SET_CONFIGURATION sequence as xHCI)
- [ ] Async schedule: control/bulk transfers via QH→QTD chain; `ehci_submit_control(dev, setup, data, len)` / `ehci_submit_bulk(dev, ep, buf, len)`
- [ ] Periodic schedule: interrupt transfers -- insert QH into frame list at appropriate interval slot; poll QTD `HALTED+ACTIVE` bits in interrupt handler
- [ ] Register `usb_hcd_t ehci_hcd` with `usb_hcd_register()` -- all class drivers work via `usb_submit_control/bulk/interrupt()` without EHCI-specific code
- [ ] On enumeration success: call `usb_hid_probe()` or `usb_msc_probe()` (same class drivers as xHCI path via usb_core API)
- [ ] QEMU test: `-device usb-ehci,id=ehci -device usb-kbd,bus=ehci.0` → HID keyboard works
- [ ] Boot log: `[EHCI] USB 2.0 controller %04x:%04x, %u ports`
- [ ] Commit: `"drivers: EHCI -- async/periodic schedule, QH/QTD, USB 2.0 enumeration, reuse HID/MSC"`

## 11. USB CDC-ECM Ethernet `[Sonnet]`

Implement CDC-ECM (Communication Device Class, Ethernet Control Model) for USB-to-Ethernet adapters and USB-tethered devices. Interface class `0x02` subclass `0x06`. Bulk IN/OUT for data; interrupt-IN for network notifications. Register as a NIC via `net_register_nic()`.

**Files:** `src/kernel/drivers/usb_cdc_ecm.c` (new), `include/kernel/drivers/usb_cdc_ecm.h` (new)

> [!NOTE]
> CDC-ECM uses two interfaces: a control interface (class `0x02`, subclass `0x06`) with an interrupt-IN endpoint, and a data interface (class `0x0A`) with bulk-IN and bulk-OUT endpoints. The MAC address is retrieved via a `GET_DESCRIPTOR(CS_INTERFACE, Ethernet Networking Functional Descriptor)` which contains an iMACAddress string index -- read via GET_DESCRIPTOR(STRING, iMACAddress).

- [ ] Detect control interface (`class=0x02, subclass=0x06`); find paired data interface (`class=0x0A`); both must be present
- [ ] Parse Ethernet Functional Descriptor (bDescriptorSubType=`0x0F`) for `iMACAddress` and `wMaxSegmentSize`
- [ ] Retrieve MAC: `usb_get_string_ascii(dev, iMACAddress_index, mac_str, 13)` (§2 string descriptor API) → parse 12 hex chars → 6-byte MAC
- [ ] `SET_ETHERNET_PACKET_FILTER(0x0F)` control transfer: enable directed, broadcast, multicast, promiscuous
- [ ] Set up bulk-IN endpoint for RX: submit large (1514-byte) interrupt transfer; on completion: `ethernet_receive(buf, len)` then re-submit
- [ ] `cdc_ecm_send(buf, len)`: write frame to bulk-OUT endpoint
- [ ] `net_register_nic(&cdc_ecm_nic_ops, mac)` → DHCP path activates for this adapter
- [ ] Hot-plug aware: on disconnect, `net_unregister_nic()`
- [ ] Boot log: `[USB-ECM] MAC %02x:%02x:... registered as NIC`
- [ ] Commit: `"drivers: USB CDC-ECM -- iMACAddress, bulk RX/TX, SET_PACKET_FILTER, net_register_nic"`

## 12. USB CDC-ACM Serial `[Sonnet]`

Implement CDC-ACM (Abstract Control Model) for USB serial adapters, Arduinos, GPS receivers, and debug consoles. Interface class `0x02` subclass `0x02`. Expose as a virtual serial port. `SET_LINE_CODING` + `SET_CONTROL_LINE_STATE` on open.

**Files:** `src/kernel/drivers/usb_cdc_acm.c` (new), `include/kernel/drivers/usb_cdc_acm.h` (new)

> [!NOTE]
> CDC-ACM uses two interfaces: a control interface (class `0x02`, subclass `0x02`) with an interrupt-IN endpoint for serial state notifications, and a data interface (class `0x0A`) with bulk-IN/OUT. `SET_LINE_CODING` (request `0x20`, value=0, index=control interface): 7-byte body `{ dwDTERate(4), bCharFormat(1), bParityType(1), bDataBits(1) }`. `SET_CONTROL_LINE_STATE` (request `0x22`): value bit0=DTR, bit1=RTS.

- [ ] Detect control interface (`class=0x02, subclass=0x02`); find paired data interface (`class=0x0A`)
- [ ] `acm_open(baud, bits, parity, stop)`: issue `SET_LINE_CODING` with requested parameters; issue `SET_CONTROL_LINE_STATE(DTR=1, RTS=1)`; submit bulk-IN transfer for incoming data; submit interrupt-IN for state notifications
- [ ] `acm_write(buf, len)`: transmit over bulk-OUT endpoint
- [ ] `acm_read(buf, max)`: drain bulk-IN completion ring; copy to caller buffer
- [ ] Virtual serial port: assign path `\Device\Serial%u` (or `/dev/ttyUSB%u` equivalent); register with VFS or device-object table
- [ ] State notifications (interrupt-IN): `SERIAL_STATE` notification (serial line status: DCD, DSR, break, ring)
- [ ] Hot-plug: on disconnect, invalidate serial port; pending reads return `STATUS_DEVICE_NOT_CONNECTED`
- [ ] Boot log: `[USB-ACM] CDC-ACM serial adapter → \\Device\\Serial%u (baud=115200 default)`
- [ ] Commit: `"drivers: USB CDC-ACM -- SET_LINE_CODING, bulk RX/TX, virtual serial port registration"`

---

## 13. Bluetooth HCI via USB `[Opus]`

Detect USB Bluetooth adapters (class `0xE0` subclass `0x01` protocol `0x01`). Send `HCI_RESET` command over control endpoint. Receive HCI Command Complete event over interrupt-IN. Expose `hci_send_command()` / `hci_recv_event()` API for the future Bluetooth stack.

**Files:** `src/kernel/drivers/bt_hci_usb.c` (new), `include/kernel/drivers/bt_hci.h` (new)

> [!NOTE]
> Bluetooth HCI USB transport (USB 2.0 Bluetooth Class Spec): control endpoint for HCI commands (request type `0x20`, request `0x00`, data = HCI command packet); interrupt-IN endpoint for HCI events; bulk-IN/OUT for ACL data. `HCI_RESET` opcode: `0x0C03`; parameter total length: 0. Expected response: `HCI_COMMAND_COMPLETE` event (code `0x0E`) with `Num_HCI_Command_Packets + Command_Opcode + Return_Parameters`.

- [ ] USB interface match: class `0xE0`, subclass `0x01`, protocol `0x01`
- [ ] `bt_hci_probe(dev)`: configure interrupt-IN endpoint (§4 path); allocate bulk-IN/OUT endpoints; submit HCI_RESET via `usb_control_msg()`; wait for Command Complete on interrupt-IN
- [ ] `hci_send_command(opcode, params, len)`: format HCI command packet `[opcode(2), len(1), params[len]]`; issue control transfer (bmRequestType=`0x20`, bRequest=`0x00`, value=0, index=0, data=packet)
- [ ] `hci_recv_event(buf, max)`: service interrupt-IN completion; copy event bytes into `buf`; return actual length
- [ ] `hci_send_acl(handle, flags, data, len)` / `hci_recv_acl(buf, max)`: bulk-OUT / bulk-IN wrappers
- [ ] Register `bt_hci_ops_t bt_hci_usb_ops = { .send_cmd, .recv_event, .send_acl, .recv_acl }` with future Bluetooth stack (`bt_hci_register()` stub)
- [ ] Boot log: `[BT-HCI] USB Bluetooth adapter detected, HCI_RESET OK`
- [ ] Commit: `"drivers: Bluetooth HCI USB -- HCI_RESET, interrupt-IN events, ACL bulk, hci_ops registration"`

## OS Comparison


| ⭐  | Feature                                  | 🪟 Win11                                 | 🐧 Linux                                 | 🚀 Impossible OS                         |
| --- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- |
| 💎  | USB core abstraction (HCD-agnostic API)  | ✅ USBD (`usb_submit_urb` equivalent)    | ✅ `usb_submit_urb()` + `struct usb_hcd` | ⬜ §1 -- `usb_device_t`, `usb_hcd_ops_t` vtable |
| 💎  | USB string descriptors (device names)    | ✅ Automatic in PnP Manager              | ✅ `usb_string()` in usb core            | ⬜ §2 -- `usb_get_string_ascii()`, manufacturer/product/serial |
| 💎  | Isochronous endpoint transfers           | ✅ USBD isochronous URBs                 | ✅ `usb_submit_urb()` with iso packets   | ⬜ §3 -- xHCI Isoch TRB, ESIT payload    |
| ⚠️   | xHCI controller init + basic enumeration | ✅ `USBXHCI.sys` full xHCI implementation | ✅ `xhci_hcd` full xHCI; slot/ring management | ⚠️ Partial -- controller init, DCBAA,     |
| 💎  | USB HID keyboard + mouse                 | ✅ `HIDCLASS.sys` + `HIDUSB.sys`; full HID | ✅ `usbhid` driver; boot + full          | ⬜ §4+§5; interrupt-IN endpoint setup, 8-byte report |
| 💎  | USB MSC BOT / SCSI + multi-LUN           | ✅ `USBSTOR.sys`; multi-LUN; PnP mount   | ✅ `usb-storage`; multi-LUN; block device | ⬜ §7 -- CBW/CSW, GET_MAX_LUN, per-LUN blkdev |
| 💎  | USB hot-plug attach/detach with desktop notification | ✅ `cfgmgr32.dll` PnP; AutoPlay toast; safe | ✅ `udevd` uevent; udisks2 auto-mount; systemd | ⬜ §8 -- Port Status Change TRB handler  |
| 💎  | USB hub class driver                     | ✅ `usbhub.sys`; hub descriptor; cascaded port | ✅ `usbhub` driver; hub class; cascaded  | ⬜ §9 -- GET_DESCRIPTOR(HUB), port power, interrupt-IN bitmap |
| 💎  | EHCI fallback for USB 2.0 systems        | ✅ `USBEHCI.sys` (legacy; phased out)    | ✅ `ehci_hcd`; async + periodic schedule | ⬜ §10 -- QH/QTD, `usb_hcd_register()` via §1 vtable |
| 💎  | PS/2 yields to USB HID; auto-resumes on disconnect | ✅ ACPI `_PRS`/`_CRS` resource arbitration; PS/2 | ✅ `i8042` suppressed if USB HID         | ⬜ §6 -- `usb_keyboard_active` flag; PS/2 skips inject |
| 💎  | Bluetooth HCI via USB                    | ✅ `BTHUSB.sys`; HCI over USB transport  | ✅ `btusb.c`; HCI over USB; `hci_register_dev()` | ⬜ §13 -- HCI_RESET, interrupt-IN events, ACL bulk |
| 💎  | USB CDC-ECM Ethernet                     | ✅ `rndiscmp.sys` (RNDIS); CDC-ECM via Windows | ✅ `cdc_ether.c`; `net_device` registration; auto DHCP | ⬜ §11 -- `usb_get_string_ascii()` for MAC, bulk RX/TX |
| 💎  | USB CDC-ACM serial                       | ✅ `usbser.sys` inbox CDC-ACM driver     | ✅ `cdc_acm.c`; `/dev/ttyACM%u`; `tty_register_driver()` | ⬜ §12 -- SET_LINE_CODING, bulk RX/TX, virtual serial port |
| 💎  | USB selective suspend / LPM              | ✅ Selective suspend policy per device   | ✅ `autosuspend` + USB 3.x U1/U2 LPM     | ⬜ Deferred → `02-kernel-core/TODO-26 §9` |

> **After §1–§13:** Impossible OS's USB stack reaches full production parity -- USB core abstraction, string descriptors, isochronous transfers, keyboard, mouse, mass storage (multi-LUN), hubs, hot-plug, EHCI fallback, serial, Ethernet, and Bluetooth HCI. The key advantage over Windows: no reboot required for USB class-driver changes (loadable `.kmod`). The key advantage over Linux: the hot-plug notification (§8) posts a desktop toast through the compositor message bus, not via a separate udev/udisks2 daemon -- one kernel path from TRB event to user-visible notification. USB selective suspend and Link Power Management are deferred to `02-kernel-core/TODO-26-power-management.md §9` (device D-states) -- required for laptop battery life but not blocking for desktop/server USB functionality.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] USB core: `usb_submit_control()` / `usb_submit_bulk()` dispatch through xHCI HCD vtable; existing MSC boot path works via new API
- [ ] String descriptors: serial log shows `"[USB] Device: <manufacturer> <product>"` for attached devices
- [ ] QEMU `-device usb-xhci -device usb-kbd,bus=xhci.0`: boot log `[USB-HID] Keyboard slot=1`; typing in terminal works; PS/2 keyboard events are suppressed
- [ ] QEMU `-device usb-xhci -device usb-mouse,bus=xhci.0`: boot log `[USB-HID] Mouse slot=1`; mouse moves; PS/2 mouse events suppressed
- [ ] QEMU USB drive: `-device usb-xhci -drive if=none,id=stick,file=test.img -device usb-storage,bus=xhci.0,drive=stick` → boot log `[USB-MSC] mounted as E:\\`; `ls E:\` lists image contents
- [ ] Multi-LUN: USB card reader with 2 slots → two block devices registered (`usb0`, `usb0.1`)
- [ ] Hot-plug test: remove USB storage in QEMU monitor (`device_del`); toast "USB drive E:\\ safely removed" appears; PS/2 keyboard re-activates if USB keyboard removed
- [ ] QEMU hub: `-device usb-xhci -device usb-hub,bus=xhci.0 -device usb-kbd,bus=usb-hub.0` → downstream keyboard enumerated; works same as direct connect
- [ ] EHCI: QEMU `-device usb-ehci -device usb-kbd,bus=ehci.0` → `[EHCI] USB 2.0 controller`; keyboard works via EHCI path; class drivers use same usb_core API
- [ ] CDC-ECM: QEMU with CDC-ECM USB NIC emulation → `[USB-ECM] MAC ..., registered as NIC`; `ping` to gateway succeeds
- [ ] Bluetooth: hardware with USB Bluetooth dongle → `[BT-HCI] USB Bluetooth adapter detected, HCI_RESET OK`
- [ ] Serial: Arduino or USB-serial adapter → `[USB-ACM] → \\Device\\Serial0`; write test bytes received
- [ ] Commit: `"drivers: USB stack complete -- core API, string descriptors, isoch, HID, MSC multi-LUN, hub, hot-plug, EHCI, CDC-ECM, CDC-ACM, BT-HCI"`
