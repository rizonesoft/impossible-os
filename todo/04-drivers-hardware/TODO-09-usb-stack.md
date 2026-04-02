# TODO-09 — USB Stack Completion

> **Goal:** Complete the USB class-driver layer on top of the existing partial xHCI implementation — adding hot-plug event handling, hub class driver, EHCI fallback, Bluetooth HCI via USB, CDC-ECM Ethernet, and CDC-ACM serial — reaching parity with Windows 11's `USBXHCI.sys`/`HIDCLASS.sys`/`USBSTOR.sys` and Linux's `xhci_hcd`/`usbhid`/`usb-storage`.
>
> → **Boot-critical USB sections extracted to `01-boot-platform/TODO-10-xhci-usb-boot.md`:** xHCI bring-up, USB MSC BOT, and USB HID boot-protocol keyboard/mouse. This TODO covers advanced/non-boot USB features.

> [!IMPORTANT]
> **Partial implementation exists.** `src/kernel/drivers/xhci.c` (381 lines), `xhci_dev.c` (912 lines), `xhci_ring.c` (269 lines) implement: controller halt/reset, DCBAA, scratchpad, TRB command/event rings, port scanning, slot enable, Address Device, GET_DESCRIPTOR (device + configuration), SET_CONFIGURATION, Configure Endpoint, and bulk-endpoint setup for MSC. **Do not rewrite** these files — complete them. What is missing: interrupt-endpoint setup for HID, HID boot-protocol report parsers, MSC BOT CBW/CSW transport (bulk endpoints exist but SCSI never sent), hot-plug interrupt handling, hub class driver, and EHCI fallback.

## Inputs

- [`src/kernel/drivers/xhci.c`](../../src/kernel/drivers/xhci.c) — xHCI controller init, event ring, port scanning
- [`src/kernel/drivers/xhci_dev.c`](../../src/kernel/drivers/xhci_dev.c) — slot enable, Address Device, GET_DESCRIPTOR, Configure Endpoint, bulk-endpoint setup
- [`src/kernel/drivers/xhci_ring.c`](../../src/kernel/drivers/xhci_ring.c) — TRB ring allocation, enqueue/dequeue, doorbell
- [`src/kernel/drivers/keyboard.c`](../../src/kernel/drivers/keyboard.c), [`src/kernel/drivers/mouse.c`](../../src/kernel/drivers/mouse.c) — injection targets for HID events
- → XREF: `04-drivers-hardware/TODO-07-network-drivers.md §2` — CDC-ECM (§9) calls `net_register_nic()` into the same network stack as VirtIO-net; NIC registration API must be compatible
- → XREF: `04-drivers-hardware/TODO-01-kernel-module-system.md` — EHCI fallback (§6) and Bluetooth HCI (§8) are good candidates for loadable `.kmod`; module loader should be available
- → XREF: `04-drivers-hardware/TODO-05-input-system.md §5` — PS/2↔USB fallback (§7) coordinates with raw-grab ownership; if a raw-grab task owns the mouse, USB HID events must also be routed through `mouse_raw_grab()` path

## Outcome

- USB HID keyboards and mice work via xHCI interrupt endpoints; boot-protocol reports decoded; events injected into existing keyboard/mouse subsystem.
- USB mass storage drives mount as new drive letters (E:\, F:\, …) via BOT SCSI transport; hot-plug attach/detach with desktop toast notifications.
- USB hubs enumerate downstream devices; cascaded hot-plug events work.
- EHCI fallback for USB 2.0-only systems shares the same HID and MSC class drivers.
- PS/2 input gracefully yields to USB HID when a USB keyboard/mouse is connected; re-activates on disconnect.
- Bluetooth HCI via USB provides the foundation for the Bluetooth stack (→ future TODO).
- CDC-ECM USB-to-Ethernet adapters register as NICs and reach the DHCP path.
- CDC-ACM serial adapters expose a virtual serial port for debugging and device communication.

## Implementation Order

| ⭐  | Order | Deliverable                                                          | Depends On                                       | Status |
| --- | :---: | -------------------------------------------------------------------- | ------------------------------------------------ | :----: |
| 💎  |   1   | §1 Interrupt endpoint setup for HID (`xhci_configure_interrupt_ep`) | `xhci_dev.c` bulk-endpoint baseline              |  [ ]   |
| 💎  |   2   | §2 USB HID class driver — keyboard + mouse boot protocol            | §1 (interrupt-IN endpoint)                       |  [ ]   |
| 💎  |   3   | §7 PS/2 ↔ USB input fallback                                        | §2 (USB HID active flag)                         |  [ ]   |
| 💎  |   4   | §3 USB MSC BOT completion — SCSI CBW/CSW, blkdev registration       | `xhci_dev.c` bulk-endpoint baseline              |  [ ]   |
| 💎  |   5   | §4 Hot-plug interrupt handling — port status change TRB             | §2 + §4 (HID + MSC attach/detach paths needed)   |  [ ]   |
| 💎  |   6   | §5 USB hub class driver                                              | §5 (hot-plug path established)                   |  [ ]   |
| 💎  |   7   | §6 EHCI fallback (USB 2.0)                                          | §2, §4 (class drivers exist to reuse)            |  [ ]   |
| 💎  |   8   | §9 USB CDC-ECM Ethernet                                              | §1 (interrupt-IN), bulk IN/OUT established       |  [ ]   |
| 💎  |   9   | §10 USB CDC-ACM serial                                               | §1 (interrupt-IN for notifications)              |  [ ]   |
| 💎  |  10   | §8 Bluetooth HCI via USB                                             | §1 (interrupt-IN for HCI events), §6 (EHCI/xHCI) |  [ ]   |

> All rows are 💎 parity: Windows 11 and Linux ship all listed USB class drivers in their inbox/mainline driver sets. Closing these gaps completes Impossible OS's USB stack to production-OS standards.

---

## 1. Interrupt Endpoint Setup for HID `[Opus]`

Extend `xhci_dev.c` to configure interrupt-IN endpoints. HID devices have a single interrupt-IN endpoint on their first interface; call `xhci_configure_interrupt_ep()` from the enumeration path when interface class == `0x03`.

**Files:** `src/kernel/drivers/xhci_dev.c` (extend), `include/kernel/drivers/xhci_dev.h` (extend)

> [!NOTE]
> The bulk-endpoint configuration in `xhci_dev.c` sets endpoint context type `0x2` (BULK_IN) or `0x6` (BULK_OUT). Interrupt endpoint type is `0x3` (INTERRUPT_IN) or `0x7` (INTERRUPT_OUT). The Configure Endpoint command submission path is identical; only the endpoint context type and `Interval` field differ. `Interval` = log2 of the polling period in 125 µs microframes (e.g., `Interval=3` → 1 ms for FS/LS, `Interval=8` → 1 ms for HS).

- [ ] `xhci_configure_interrupt_ep(dev, ep_addr, max_pkt, interval)`: allocate endpoint context in the input context; set `EP Type = INTERRUPT_IN (0x3)`; set `Max Packet Size = max_pkt`; set `Interval = interval`; set `Average TRB Length = max_pkt`; set `Max Burst Size = 0` for FS/LS; submit Configure Endpoint command; wait for Command Completion Event with `CC == SUCCESS`
- [ ] `xhci_submit_interrupt_transfer(dev, ep_addr, buf, len, callback)`: enqueue Normal TRB on the endpoint's transfer ring with `IOC=1`; ring doorbell; on Transfer Event received on event ring with matching TRB pointer, call `callback(buf, actual_len, status)`
- [ ] Update `xhci_enumerate_device()`: after SET_CONFIGURATION, scan parsed interface descriptors; if `bInterfaceClass == 0x03` (HID), call `xhci_configure_interrupt_ep()` then `usb_hid_probe(dev, ep_addr, max_pkt, interval)`
- [ ] Error path: if Configure Endpoint fails (CC != SUCCESS), log `[xHCI] EP config failed slot=%u ep=%u cc=%u`; mark device as non-functional
- [ ] Commit: `"drivers: xhci — interrupt-IN endpoint setup, INTERRUPT_IN context type, HID probe hook"`

## 2. USB HID Class Driver `[Sonnet]`

Implement `usb_hid.c` with boot-protocol keyboard and mouse parsers. `usb_hid_probe()` issues `SET_PROTOCOL(0)` to request boot-protocol reports. Completions decode reports and inject into the existing keyboard/mouse event pipeline.

**Files:** `src/kernel/drivers/usb_hid.c` (new), `include/kernel/drivers/usb_hid.h` (new)

> [!NOTE]
> Boot-protocol keyboard report (8 bytes): `[modifier][reserved][keycode0][keycode1][keycode2][keycode3][keycode4][keycode5]`. Modifier byte bits: `b0=LCtrl, b1=LShift, b2=LAlt, b3=LMeta, b4=RCtrl, b5=RShift, b6=RAlt, b7=RMeta`. Boot-protocol mouse report (3 bytes): `[buttons][Δx][Δy]` — `buttons` bits `b0=L, b1=R, b2=M`.

- [ ] `usb_hid_probe(dev, ep_addr, max_pkt, interval)`: determine HID subclass (`bInterfaceSubClass==1` = boot device) and protocol (`1`=keyboard, `2`=mouse); issue `SET_PROTOCOL(0)` control transfer (request type `0x21`, request `0x0B`, value `0`, index = interface, length `0`); submit first interrupt transfer
- [ ] Keyboard completion: on 8-byte report received, decode `modifier` byte (bits 0–7 → modifier flags); iterate `keycode[0..5]`; compare with previous report to detect key-down (new code not in previous) and key-up (old code not in current); call `keyboard_driver_handle_event(keycode, modifier, DOWN/UP)`
- [ ] Mouse completion: decode 3-byte report; call `mouse_driver_handle_event(Δx, Δy, buttons)` — same path as PS/2 mouse events
- [ ] Re-submit interrupt transfer in completion handler for continuous polling
- [ ] `usb_hid_disconnect(dev)`: cancel pending interrupt transfer; clear `usb_keyboard_active` / `usb_mouse_active` flags (§7)
- [ ] Boot log: `[USB-HID] Keyboard slot=%u` / `[USB-HID] Mouse slot=%u`
- [ ] Commit: `"drivers: USB HID — boot-protocol keyboard/mouse, SET_PROTOCOL(0), event injection"`

## 3. USB MSC BOT Completion `[Opus]`

Complete the USB Mass Storage Class Bulk-Only Transport layer. Bulk endpoints are already set up in `xhci_dev.c`; add `usb_msc_scsi.c` implementing SCSI CBW/CSW framing and `INQUIRY`, `READ CAPACITY`, `READ(10)`, `WRITE(10)` commands. Register via `blkdev_register()`.

**Files:** `src/kernel/drivers/usb_msc_scsi.c` (new), `include/kernel/drivers/usb_msc.h` (new)

> [!NOTE]
> BOT Command Block Wrapper (CBW): 31-byte structure — `dCBWSignature=0x43425355 ("USBC")`, `dCBWTag` (unique per command), `dCBWDataTransferLength`, `bmCBWFlags` (bit7=IN), `bCBWLUN`, `bCBWCBLength`, `CBWCB[16]` (SCSI CDB). Command Status Wrapper (CSW): 13-byte — `dCSWSignature=0x53425355 ("USBS")`, `dCSWTag`, `dCSWDataResidue`, `bCSWStatus` (0=pass, 1=fail, 2=phase error).

- [ ] `usb_msc_send_cbw(dev, tag, len, flags, lun, cdb, cdb_len)`: build 31-byte CBW; transmit over bulk-OUT endpoint; wait for Transfer Event
- [ ] `usb_msc_recv_csw(dev, tag)`: read 13-byte CSW from bulk-IN; verify signature and matching tag; return `bCSWStatus`
- [ ] `usb_msc_inquiry(dev)`: CDB `0x12` (INQUIRY), allocation length 36; parse `Peripheral Device Type` and `Product Identification`; log vendor/product string
- [ ] `usb_msc_read_capacity(dev, &sectors, &block_size)`: CDB `0x25` (READ CAPACITY(10)); parse 8-byte response
- [ ] `usb_msc_read(dev, lba, count, buf)`: CDB `0x28` (READ(10)); CBW flags=`0x80` (IN); receive `count * block_size` bytes from bulk-IN; verify CSW
- [ ] `usb_msc_write(dev, lba, count, buf)`: CDB `0x2A` (WRITE(10)); CBW flags=`0x00` (OUT); transmit data to bulk-OUT; verify CSW
- [ ] Register `blkdev_t usb_msc_blkdev { .read=usb_msc_read, .write=usb_msc_write, .sectors, .block_size }` via `blkdev_register()`; VFS auto-mounts next available drive letter (E:\, F:\, …)
- [ ] Boot log: `[USB-MSC] slot=%u vendor="%s" product="%s" %llu sectors mounted as %c:\\`
- [ ] Commit: `"drivers: USB MSC BOT — SCSI INQUIRY/READ_CAPACITY/READ/WRITE CBW/CSW, blkdev_register"`

## 4. Hot-Plug Interrupt Handling `[Opus]`

Process xHCI Port Status Change Events (TRB type `0x22`) from the event ring. On connect: enumerate device. On disconnect: tear down class driver, unregister blkdev, notify desktop with toast.

**Files:** `src/kernel/drivers/xhci.c` (extend event loop), `src/kernel/drivers/usb_hid.c`, `src/kernel/drivers/usb_msc_scsi.c`

> [!NOTE]
> Port Status Change Event TRB: `Port ID` field (bits 31:24) identifies the port. After receiving the event, read `PORTSC[Port ID - 1]` to determine current state: `CCS (bit 0)` = device present, `PED (bit 1)` = port enabled. A CCS=1 transition means a new device connected; CCS=0 means disconnect.

- [ ] In `xhci_event_loop()`: handle TRB type `0x22` (Port Status Change); extract port number; read `PORTSC`
- [ ] CCS=1 path (connect): reset port via `PR` bit in PORTSC; wait for `PRC` (port reset complete); call `xhci_enumerate_device(hc, port)` which triggers HID or MSC probe
- [ ] CCS=0 path (disconnect): find device slot by port number in `hc->slot_map[]`; call `usb_device_detach(slot)`
  - If MSC: call `blkdev_unregister(blkdev)`, trigger VFS unmount, send desktop toast: `"USB drive %c:\\ safely removed"`
  - If HID: call `usb_hid_disconnect(dev)`; PS/2 re-activates via §7
- [ ] `usb_device_detach(slot)`: cancel all pending TRBs on all endpoints (Stop Endpoint command); Disable Slot command; free slot context
- [ ] Boot log: `[xHCI] Port %u: device connected` / `Port %u: device disconnected`
- [ ] Commit: `"drivers: xHCI hot-plug — Port Status Change TRB, attach/detach, MSC unmount, HID disconnect"`

## 5. USB Hub Class Driver `[Opus]`

Implement the USB hub class driver (interface class `0x09`). Issue GET_DESCRIPTOR(HUB) to read port count. Power each port. Poll via interrupt-IN for port-change bitmask. Enumerate or detach downstream devices on change events. Cascade hot-plug to xHCI.

**Files:** `src/kernel/drivers/usb_hub.c` (new), `include/kernel/drivers/usb_hub.h` (new)

> [!NOTE]
> Hub descriptor (type `0x29`): `bNbrPorts` at offset 2; `wHubCharacteristics` at offset 3; `bPwrOn2PwrGood` at offset 5. Port power-on: `SET_FEATURE(PORT_POWER)` = `SetPortFeature(port, PORT_POWER=8)` via control endpoint. Port status: `GET_STATUS(PORT_N)` returns 4 bytes — bits `b0=PORT_CONNECTION, b1=PORT_ENABLE, b4=PORT_RESET`.

- [ ] `usb_hub_probe(dev)`: issue GET_DESCRIPTOR(HUB); parse `bNbrPorts`; store in `usb_hub_t`
- [ ] Power sequence: for each port 1..N, issue `SET_FEATURE(PORT_POWER)`; wait `bPwrOn2PwrGood * 2` ms
- [ ] Set up interrupt-IN endpoint (§1 path); submit interrupt transfer for port-change bitmap (1 + ⌈N/8⌉ bytes)
- [ ] Port-change completion: for each set bit `p` in bitmap, issue `GET_STATUS(PORT_p)`; if `PORT_CONNECTION` changed:
  - Connect: `SET_FEATURE(PORT_RESET)` → wait `PORT_RESET` clear → call `xhci_enumerate_device()` for downstream slot
  - Disconnect: `usb_device_detach(downstream_slot)`
- [ ] Re-submit interrupt transfer after each change notification
- [ ] Boot log: `[USB-HUB] slot=%u ports=%u`
- [ ] Commit: `"drivers: USB hub class — GET_DESCRIPTOR(HUB), port power, interrupt-IN, downstream enumerate"`

## 6. EHCI Fallback (USB 2.0) `[Opus]`

Implement an EHCI host controller driver for USB 2.0-only systems. PCI prog-if `0x20`. Async schedule (QH→QTD chain) for control/bulk; periodic schedule (256-entry frame list) for interrupt transfers. Reuse HID and MSC class drivers from §2 and §3.

**Files:** `src/kernel/drivers/ehci.c` (new), `include/kernel/drivers/ehci.h` (new)

> [!NOTE]
> EHCI MMIO: BAR0 offset 0 = capability registers (`CAPLENGTH`, `HCIVERSION`, `HCSPARAMS`, `HCCPARAMS`). Operational registers start at BAR0 + `CAPLENGTH`: `USBCMD`, `USBSTS`, `USBINTR`, `FRINDEX`, `CTRLDSSEGMENT`, `PERIODICLISTBASE`, `ASYNCLISTADDR`. Async schedule: QH (Queue Head) linked list; each QH has a QTD (Queue Transfer Descriptor) chain. Companion controllers (OHCI/UHCI) handle FS/LS devices via port release.

- [ ] PCI match: PCI class `0x0C`, subclass `0x03`, prog-if `0x20` (EHCI)
- [ ] Map BAR0; read `CAPLENGTH`; operational base = BAR0 + CAPLENGTH
- [ ] Controller init: halt (`USBCMD.RS=0`); reset (`USBCMD.HCRESET=1`); wait; configure `PERIODICLISTBASE` (256-entry frame list, 1 ms frames); configure `ASYNCLISTADDR` (dummy QH head, circular)
- [ ] Port scan: read `PORTSC[0..N-1]`; for connected FS/HS ports: reset port, determine speed from `PORTSC.PSPD`, enumerate with control transfers (same GET_DESCRIPTOR / SET_CONFIGURATION sequence as xHCI)
- [ ] Async schedule: control/bulk transfers via QH→QTD chain; `ehci_submit_control(dev, setup, data, len)` / `ehci_submit_bulk(dev, ep, buf, len)`
- [ ] Periodic schedule: interrupt transfers — insert QH into frame list at appropriate interval slot; poll QTD `HALTED+ACTIVE` bits in interrupt handler
- [ ] On enumeration success: call `usb_hid_probe()` or `usb_msc_probe()` (same class drivers as xHCI path)
- [ ] QEMU test: `-device usb-ehci,id=ehci -device usb-kbd,bus=ehci.0` → HID keyboard works
- [ ] Boot log: `[EHCI] USB 2.0 controller %04x:%04x, %u ports`
- [ ] Commit: `"drivers: EHCI — async/periodic schedule, QH/QTD, USB 2.0 enumeration, reuse HID/MSC"`

## 7. PS/2 ↔ USB Input Fallback `[Sonnet]`

When USB HID claims a keyboard or mouse, the PS/2 driver must yield and stop injecting duplicate events. When the USB HID device disconnects, PS/2 must automatically re-activate. No application-layer changes required.

**Files:** `src/kernel/drivers/keyboard.c`, `src/kernel/drivers/mouse.c`, `src/kernel/drivers/usb_hid.c`

- [ ] Add `bool usb_keyboard_active` and `bool usb_mouse_active` to the keyboard/mouse driver state (or a shared `input_sources.h` flags header)
- [ ] `usb_hid_probe()` (§2): after successful HID probe, set `usb_keyboard_active = true` / `usb_mouse_active = true`
- [ ] `keyboard_process_event()` (PS/2 path): check `usb_keyboard_active`; if true, return early (discard PS/2 scan code)
- [ ] `mouse_process_event()` (PS/2 path): check `usb_mouse_active`; if true, return early
- [ ] `usb_hid_disconnect()` (§2 hot-plug path, §4): clear `usb_keyboard_active` / `usb_mouse_active`; PS/2 re-activates on next scan code automatically
- [ ] Boot log: `[INPUT] USB keyboard claimed — PS/2 keyboard suspended` / `PS/2 keyboard resumed`
- [ ] Commit: `"drivers: PS/2↔USB fallback — usb_keyboard/mouse_active flag, yield/resume on attach/detach"`

## 8. Bluetooth HCI via USB `[Opus]`

Detect USB Bluetooth adapters (class `0xE0` subclass `0x01` protocol `0x01`). Send `HCI_RESET` command over control endpoint. Receive HCI Command Complete event over interrupt-IN. Expose `hci_send_command()` / `hci_recv_event()` API for the future Bluetooth stack.

**Files:** `src/kernel/drivers/bt_hci_usb.c` (new), `include/kernel/drivers/bt_hci.h` (new)

> [!NOTE]
> Bluetooth HCI USB transport (USB 2.0 Bluetooth Class Spec): control endpoint for HCI commands (request type `0x20`, request `0x00`, data = HCI command packet); interrupt-IN endpoint for HCI events; bulk-IN/OUT for ACL data. `HCI_RESET` opcode: `0x0C03`; parameter total length: 0. Expected response: `HCI_COMMAND_COMPLETE` event (code `0x0E`) with `Num_HCI_Command_Packets + Command_Opcode + Return_Parameters`.

- [ ] USB interface match: class `0xE0`, subclass `0x01`, protocol `0x01`
- [ ] `bt_hci_probe(dev)`: configure interrupt-IN endpoint (§1 path); allocate bulk-IN/OUT endpoints; submit HCI_RESET via control transfer; wait for Command Complete on interrupt-IN
- [ ] `hci_send_command(opcode, params, len)`: format HCI command packet `[opcode(2), len(1), params[len]]`; issue control transfer (bmRequestType=`0x20`, bRequest=`0x00`, value=0, index=0, data=packet)
- [ ] `hci_recv_event(buf, max)`: service interrupt-IN completion; copy event bytes into `buf`; return actual length
- [ ] `hci_send_acl(handle, flags, data, len)` / `hci_recv_acl(buf, max)`: bulk-OUT / bulk-IN wrappers
- [ ] Register `bt_hci_ops_t bt_hci_usb_ops = { .send_cmd, .recv_event, .send_acl, .recv_acl }` with future Bluetooth stack (`bt_hci_register()` stub)
- [ ] Boot log: `[BT-HCI] USB Bluetooth adapter detected, HCI_RESET OK`
- [ ] Commit: `"drivers: Bluetooth HCI USB — HCI_RESET, interrupt-IN events, ACL bulk, hci_ops registration"`

## 9. USB CDC-ECM Ethernet `[Sonnet]`

Implement CDC-ECM (Communication Device Class, Ethernet Control Model) for USB-to-Ethernet adapters and USB-tethered devices. Interface class `0x02` subclass `0x06`. Bulk IN/OUT for data; interrupt-IN for network notifications. Register as a NIC via `net_register_nic()`.

**Files:** `src/kernel/drivers/usb_cdc_ecm.c` (new), `include/kernel/drivers/usb_cdc_ecm.h` (new)

> [!NOTE]
> CDC-ECM uses two interfaces: a control interface (class `0x02`, subclass `0x06`) with an interrupt-IN endpoint, and a data interface (class `0x0A`) with bulk-IN and bulk-OUT endpoints. The MAC address is retrieved via a `GET_DESCRIPTOR(CS_INTERFACE, Ethernet Networking Functional Descriptor)` which contains an iMACAddress string index — read via GET_DESCRIPTOR(STRING, iMACAddress).

- [ ] Detect control interface (`class=0x02, subclass=0x06`); find paired data interface (`class=0x0A`); both must be present
- [ ] Parse Ethernet Functional Descriptor (bDescriptorSubType=`0x0F`) for `iMACAddress` and `wMaxSegmentSize`
- [ ] Retrieve MAC: GET_DESCRIPTOR(STRING, iMACAddress index) → 26-byte UTF-16 string → parse 12 hex chars → 6-byte MAC
- [ ] `SET_ETHERNET_PACKET_FILTER(0x0F)` control transfer: enable directed, broadcast, multicast, promiscuous
- [ ] Set up bulk-IN endpoint for RX: submit large (1514-byte) interrupt transfer; on completion: `ethernet_receive(buf, len)` then re-submit
- [ ] `cdc_ecm_send(buf, len)`: write frame to bulk-OUT endpoint
- [ ] `net_register_nic(&cdc_ecm_nic_ops, mac)` → DHCP path activates for this adapter
- [ ] Hot-plug aware: on disconnect, `net_unregister_nic()`
- [ ] Boot log: `[USB-ECM] MAC %02x:%02x:... registered as NIC`
- [ ] Commit: `"drivers: USB CDC-ECM — iMACAddress, bulk RX/TX, SET_PACKET_FILTER, net_register_nic"`

## 10. USB CDC-ACM Serial `[Sonnet]`

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
- [ ] Commit: `"drivers: USB CDC-ACM — SET_LINE_CODING, bulk RX/TX, virtual serial port registration"`

---

## OS Comparison


| ⭐ | Feature                                              | 🪟 Win11                                                         | 🐧 Linux                                                    | 🚀 Impossible OS                                                         |
|----|------------------------------------------------------|---------------------------------------------------------------|----------------------------------------------------------|-----------------------------------------------------------------------|
| ⚠️ | xHCI controller init + basic enumeration             | ✅ `USBXHCI.sys` full xHCI implementation                     | ✅ `xhci_hcd` full xHCI; slot/ring management            | ⚠️ Partial — controller init, DCBAA,                                  |
| 💎 | USB HID keyboard + mouse                             | ✅ `HIDCLASS.sys` + `HIDUSB.sys`; full HID                    | ✅ `usbhid` driver; boot + full                          | ⬜ §1 — +2; interrupt-IN endpoint setup, 8-byte                       |
| 💎 | USB MSC BOT (Bulk-Only Transport) / SCSI             | ✅ `USBSTOR.sys`; INQUIRY/READ_CAPACITY/READ/WRITE; PnP mount | ✅ `usb-storage`; SCSI passthrough; block device         | ⬜ §3 — CBW/CSW framing, SCSI CDB, `blkdev_register()`,               |
| 💎 | USB hot-plug attach/detach with desktop notification | ✅ `cfgmgr32.dll` PnP; AutoPlay toast; safe                   | ✅ `udevd` uevent; udisks2 auto-mount; systemd           | ⬜ §4 — Port Status Change TRB handler,                               |
| 💎 | USB hub class driver                                 | ✅ `usbhub.sys`; hub descriptor; cascaded port                | ✅ `usbhub` driver; hub class; cascaded                  | ⬜ §5 — GET_DESCRIPTOR(HUB), port power, interrupt-IN bitmap,         |
| 💎 | EHCI fallback for USB 2.0 systems                    | ✅ `USBEHCI.sys` (legacy; phased out for                      | ✅ `ehci_hcd`; async + periodic schedule;                | ⬜ §6 — QH/QTD async schedule, 256-entry periodic,                    |
| 💎 | PS/2 yields to USB HID; auto-resumes on disconnect   | ✅ ACPI `_PRS`/`_CRS` resource arbitration; PS/2              | ✅ `i8042` suppressed if USB HID                         | ⬜ §7 — `usb_keyboard_active` flag; PS/2 skips inject                 |
| 💎 | Bluetooth HCI via USB                                | ✅ `BTHUSB.sys`; HCI over USB transport                       | ✅ `btusb.c`; HCI over USB; `hci_register_dev()`         | ⬜ §8 — HCI_RESET, interrupt-IN events, ACL bulk,                     |
| 💎 | USB CDC-ECM Ethernet                                 | ✅ `rndiscmp.sys` (RNDIS); CDC-ECM via Windows                | ✅ `cdc_ether.c`; `net_device` registration; auto DHCP   | ⬜ §9 — iMACAddress parse, bulk RX/TX, `net_register_nic()`,          |
| 💎 | USB CDC-ACM serial                                   | ✅ `usbser.sys` inbox CDC-ACM driver                          | ✅ `cdc_acm.c`; `/dev/ttyACM%u`; `tty_register_driver()` | ⬜ §10 — SET_LINE_CODING, bulk RX/TX, `\Device\Serial%u` registration |

> **After §1–10:** Impossible OS's USB stack reaches full production parity — keyboard, mouse, mass storage, hubs, hot-plug, EHCI fallback, serial, Ethernet, and Bluetooth HCI. The key advantage over Windows: no reboot required for USB class-driver changes (loadable `.kmod`). The key advantage over Linux: the hot-plug notification (§4) posts a desktop toast through the compositor message bus, not via a separate udev/udisks2 daemon — one kernel path from TRB event to user-visible notification.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] QEMU `-device usb-xhci -device usb-kbd,bus=xhci.0`: boot log `[USB-HID] Keyboard slot=1`; typing in terminal works; PS/2 keyboard events are suppressed
- [ ] QEMU `-device usb-xhci -device usb-mouse,bus=xhci.0`: boot log `[USB-HID] Mouse slot=1`; mouse moves; PS/2 mouse events suppressed
- [ ] QEMU USB drive: `-device usb-xhci -drive if=none,id=stick,file=test.img -device usb-storage,bus=xhci.0,drive=stick` → boot log `[USB-MSC] mounted as E:\\`; `ls E:\` lists image contents
- [ ] Hot-plug test: remove USB storage in QEMU monitor (`device_del`); toast "USB drive E:\\ safely removed" appears; PS/2 keyboard re-activates if USB keyboard removed
- [ ] QEMU hub: `-device usb-xhci -device usb-hub,bus=xhci.0 -device usb-kbd,bus=usb-hub.0` → downstream keyboard enumerated; works same as direct connect
- [ ] EHCI: QEMU `-device usb-ehci -device usb-kbd,bus=ehci.0` → `[EHCI] USB 2.0 controller`; keyboard works via EHCI path
- [ ] CDC-ECM: QEMU with CDC-ECM USB NIC emulation → `[USB-ECM] MAC ..., registered as NIC`; `ping` to gateway succeeds
- [ ] Bluetooth: hardware with USB Bluetooth dongle → `[BT-HCI] USB Bluetooth adapter detected, HCI_RESET OK`
- [ ] Serial: Arduino or USB-serial adapter → `[USB-ACM] → \\Device\\Serial0`; write test bytes received
- [ ] Commit: `"drivers: USB stack complete — HID, MSC, hub, hot-plug, EHCI, CDC-ECM, CDC-ACM, BT-HCI"`
