---
schema_version: 1
id: bluetooth
domain: 04-drivers-hardware
status: active
title: "TODO-16 -- Bluetooth Full Stack"
---

# TODO-16 -- Bluetooth Full Stack

> **Goal:** Build the complete Bluetooth stack -- HCI transport layer, chipset firmware loading (Intel AX200-BT, Realtek RTL8761B), L2CAP, SDP, HID profile, A2DP/SBC audio streaming with AVRCP, RFCOMM/SPP, BLE/GATT, pairing manager, and system UI (`bluetooth.cpl`, `btctl` shell) -- enabling wireless keyboards, mice, headphones, and game controllers essential on every modern laptop.

> [!IMPORTANT]
> **Partial foundation:** `04-drivers-hardware/TODO-10-usb-stack.md §13` defines `bt_hci_usb.c` with `hci_send_command()` / `hci_recv_event()` / `hci_send_acl()` / `hci_recv_acl()` and `bt_hci_ops_t` registration. This TODO builds the full protocol stack on top of that transport stub. The `bt_hci_usb.c` stub must be completed before §1 here can proceed -- verify it is in place. All cryptographic operations (A2DP random seed in §8 and SSP pairing in §9) must use constant-time comparison to prevent timing side-channels.

## Inputs

- `src/kernel/drivers/bt_hci_usb.c` (→ XREF: `04-drivers-hardware/TODO-10-usb-stack.md §13`) -- USB HCI transport: `hci_send_command()`, `hci_recv_event()`, `hci_send_acl()`, `hci_recv_acl()`; `bt_hci_ops_t` registration point
- `src/kernel/drivers/hid_parser.c` (→ XREF: `04-drivers-hardware/TODO-12-i2c-touchpad.md §5`) -- HID report descriptor parser reused by Bluetooth HID profile (§6)
- → XREF: `04-drivers-hardware/TODO-04-security-hardware.md §2` -- `hwrng_read()` required for SSP pairing nonce (§9) and A2DP SBC bitpool random seed
- → XREF: `11-apps` domain -- audio routing: when A2DP headphones connect (§8), the audio subsystem switches `audio_get_active()` to the BT A2DP device; coordinate with the audio mixer TODO
- → XREF: `09-desktop-shell` domain -- `bluetooth.cpl` (§9) is a control-panel applet; the quick settings Bluetooth tile is a shell component consuming `bt_manager_get_state()`

## Outcome

- HCI transport fully operational: HCI_RESET, BD_ADDR read, scan enable, ACL data, event demux.
- Intel AX200-BT and Realtek RTL8761B firmware loaded from `C:\Impossible\System\Firmware\bt\`.
- L2CAP channels (classic + BLE); SDP service discovery for remote devices.
- Bluetooth HID: wireless keyboard/mouse pair, connect, inject into existing input pipeline.
- A2DP: Bluetooth headphones stream SBC audio; AVRCP play/pause/volume controls.
- RFCOMM/SPP: serial-over-Bluetooth for GPS, legacy devices; `/dev/rfcomm0` VFS node.
- BLE scanning and GATT: battery level from BLE mice/keyboards; Device Information profile.
- Quick settings Bluetooth tile; `bluetooth.cpl` paired-device manager; `btctl` shell command.

## Implementation Order

| ⭐  | Order | Deliverable                                                                    | Depends On                                   | Status |
| --- | :---: | ------------------------------------------------------------------------------ | -------------------------------------------- | :----: |
| 💎  |   1   | §1 HCI transport layer -- commands, events, ACL, `hci_conn` table              | `bt_hci_ops_t` stub (TODO-10 §13)            |  [ ]   |
| 💎  |   2   | §2 HCI firmware loading -- Intel AX200-BT + Realtek RTL8761B                   | §1 (HCI commands available)                  |  [ ]   |
| 💎  |   3   | §3 L2CAP -- channel multiplexing, signalling, MTU negotiation                  | §1 (ACL data path)                           |  [ ]   |
| 💎  |   4   | §4 SDP -- `ServiceSearchAttributeRequest`, PSM/channel discovery, local server | §3 (L2CAP channels)                          |  [ ]   |
| 💎  |   5   | §5 HID Profile -- L2CAP PSM 0x11/0x13, report descriptor, input injection      | §3, §4 (SDP PSM lookup), `hid_parser.c`      |  [ ]   |
| 💎  |   6   | §6 BLE -- LE scan, `LE_CREATE_CONNECTION`, ATT/GATT Battery + DevInfo          | §1 (LE HCI commands), §3 (LE L2CAP CIDs)     |  [ ]   |
| 💎  |   7   | §7 RFCOMM & SPP -- L2CAP PSM 0x03, SABM/UA frames, `/dev/rfcomm0`              | §3, §4 (RFCOMM channel via SDP)              |  [ ]   |
| 💎  |   8   | §8 A2DP + SBC encoder + AVRCP                                                  | §3 (L2CAP PSM 0x19), §4 (AVDTP discovery)    |  [ ]   |
| ⭐  |   9   | §9 Bluetooth manager + pairing UI + `bluetooth.cpl`                            | §1–8 (all profiles), `hwrng` (pairing nonce) |  [ ]   |
| 💎  |  10   | §10 `btctl` shell command                                                      | §9 (manager API)                             |  [ ]   |

> §9 Bluetooth manager is `⭐` exclusive by architecture: Windows uses `bthserv` (user-space Bluetooth service) + `btpan.sys`; Linux uses `bluetoothd` (BlueZ user-space daemon). Impossible OS runs the full Bluetooth protocol stack in the kernel with no daemon -- pairing, profile negotiation, and audio routing happen inside the kernel without IPC round-trips to a user-space service.

---

## 1. HCI Transport Layer `[Opus]`

Build the HCI command/event/ACL layer on top of the `bt_hci_ops_t` stub from TODO-10 §13. Parse all required HCI events. Maintain a `hci_conn[]` table of 16 BR/EDR + BLE connections. Send the baseline HCI initialisation sequence.

**Files:** `src/kernel/drivers/bt_hci.c` (new/extend), `include/kernel/drivers/bt_hci.h` (new)

> [!NOTE]
> HCI packet types: `HCI_COMMAND_PKT (0x01)` = opcode(2) + param_len(1) + params; `HCI_ACL_DATA_PKT (0x02)` = handle(12)+PB(2)+BC(2) + data_len(2) + data; `HCI_EVENT_PKT (0x04)` = event_code(1) + param_total_len(1) + params. Event dispatch: interrupt-IN delivers `HCI_EVENT_PKT`; bulk-IN delivers `HCI_ACL_DATA_PKT`. Command send uses control endpoint (USB) or UART.

- [ ] `hci_send_cmd(opcode, params, len)` wrapper: build `HCI_COMMAND_PKT`; call `bt_hci_ops->send_cmd()`; wait for `HCI_COMMAND_COMPLETE_EVENT (0x0E)` or `COMMAND_STATUS (0x0F)` matching opcode
- [ ] HCI init sequence: `HCI_RESET (0x0C03)`; `READ_LOCAL_VERSION (0x1001)` (log HCI/LMP version); `READ_BD_ADDR (0x1009)` (store local MAC); `WRITE_SCAN_ENABLE (0x0C1A, 0x03)` (page + inquiry scan enabled)
- [ ] Event parser: `hci_event_dispatch(buf, len)`: switch on `event_code`:
  - `0x03 CONNECTION_COMPLETE`: fill `hci_conn[handle]`; set state `CONNECTED`; notify L2CAP
  - `0x05 DISCONNECTION_COMPLETE`: clear `hci_conn[handle]`; notify L2CAP
  - `0x02 INQUIRY_RESULT` / `0x22 INQUIRY_RESULT_WITH_RSSI`: append to scan results list
  - `0x3E LE_META_EVENT`: dispatch to BLE handler (§6)
  - `0x0E COMMAND_COMPLETE`: wake waiting command sender
- [ ] `hci_conn_t { uint8_t bd_addr[6]; uint16_t handle; uint8_t state; uint8_t type; }` -- 16-entry static table; `hci_find_conn_by_handle()` / `hci_find_conn_by_addr()`
- [ ] ACL send/recv: `hci_acl_send(handle, pb, data, len)` builds `HCI_ACL_DATA_PKT`; `hci_acl_recv_cb` registered with `bt_hci_ops->recv_acl`; dispatches to L2CAP (§3)
- [ ] Boot log: `[BT-HCI] BD_ADDR %02x:%02x:... LMP version %u.%u`
- [ ] Commit: `"drivers: BT HCI layer -- RESET/BD_ADDR/SCAN_ENABLE, event dispatch, hci_conn table, ACL"`

## 2. HCI Firmware Loading `[Sonnet]`

Bluetooth chipsets require firmware upload on each power-on. Implement vendor-specific firmware download for Intel AX200-BT and Realtek RTL8761B. Load firmware from `C:\Impossible\System\Firmware\bt\` via VFS.

**Files:** `src/kernel/drivers/bt_hci_intel.c` (new), `src/kernel/drivers/bt_hci_realtek.c` (new)

> [!NOTE]
> **Intel AX200 Bluetooth** USB ID: `{ 0x8087, 0x0029 }`. Firmware: `ibt-20-1-3.sfi` (TLV format). Download: `HCI_INTEL_BOOT_PARAMS` vendor opcode `0xFC05` → get boot stage; `HCI_INTEL_FIRMWARE_VERSION` `0xFC05` → get current FW version; download TLV fragments via `HCI_INTEL_WRITE_BOOT_PARAMS` `0xFC0E`. **Realtek RTL8761B** USB ID: `{ 0x0BDA, 0x8771 }`. Firmware: `rtl_bt/rtl8761bu_fw.bin` + `rtl8761bu_config.bin`. Download: send `HCI_RESET`; then vendor opcode `0xFC6D` (ROM version); `0xFC20` (enter download mode); send firmware chunks via `0xFC20`.

- [ ] Intel probe: match `{ 0x8087, 0x0029 }` and similar Intel BT IDs; call `bt_intel_fw_load()`
- [ ] `bt_intel_fw_load()`: `vfs_open("C:\\Impossible\\System\\Firmware\\bt\\ibt-20-1-3.sfi")`; parse TLV sections (type=CSS_HEADER, FW_VERSION, SIGNED_FW_IMG); send each section via `HCI_INTEL_WRITE_BOOT_PARAMS` vendor command; wait for `HCI_INTEL_BOOT_COMPLETE` vendor event; send `HCI_RESET` to activate
- [ ] Realtek probe: match `{ 0x0BDA, 0x8771 }`, `{ 0x0BDA, 0x8761 }`, `{ 0x0BDA, 0xB761 }` RTL8761 variants
- [ ] `bt_realtek_fw_load()`: `vfs_open("C:\\Impossible\\System\\Firmware\\bt\\rtl8761bu_fw.bin")`; send vendor `0xFC6D` ROM query; `0xFC20` with `DOWNLOAD_PHASE=1`; stream firmware 252 bytes per `0xFC20` command; `0xFC20` with `DOWNLOAD_PHASE=2` (config); `HCI_RESET`
- [ ] Add firmware files to build: copy `resources/firmware/bt/*.bin` to `C:\Impossible\System\Firmware\bt\` in disk image
- [ ] Add to `NOTICE.md`: Intel BT firmware BSD-2, Realtek BT firmware BSD-2 (from linux-firmware)
- [ ] Boot log: `[BT] Intel/Realtek firmware loaded, version %s`
- [ ] Commit: `"drivers: BT firmware -- Intel AX200-BT TLV download, Realtek RTL8761B vendor opcodes"`

## 3. L2CAP -- Logical Link Control and Adaptation Protocol `[Opus]`

Implement L2CAP channel-based multiplexing over HCI ACL data. Handle the signalling channel (CID 0x0001): Connection Request/Response, Configuration Request/Response, Disconnection. Provide `l2cap_send()` / `l2cap_recv()` per channel.

**Files:** `src/kernel/drivers/bt_l2cap.c` (new), `include/kernel/drivers/bt_l2cap.h` (new)

> [!NOTE]
> L2CAP PDU: `len(2) + CID(2) + payload`. Classic channels: CID 0x0001=signalling, 0x0040–0x007F=dynamic (allocated per connection). BLE channels: CID 0x0004=ATT, 0x0005=LE signalling, 0x0006=SM. Signalling codes: `CONNECTION_REQ (0x02)`, `CONNECTION_RSP (0x03)`, `CONFIGURATION_REQ (0x04)`, `CONFIGURATION_RSP (0x05)`, `DISCONNECTION_REQ (0x06)`, `DISCONNECTION_RSP (0x07)`, `INFORMATION_REQ (0x0A)`, `INFORMATION_RSP (0x0B)`.

- [ ] `l2cap_chan_t { uint16_t local_cid, remote_cid; uint16_t psm; uint16_t mtu; uint16_t handle; l2cap_state_t state; void (*recv_cb)(l2cap_chan_t*, uint8_t*, uint16_t); }` -- 32-entry channel table
- [ ] `hci_acl_recv` → `l2cap_rx(handle, payload, len)`: demux by CID; if 0x0001 → `l2cap_sig_handle()`; else → dispatch to registered channel `recv_cb`
- [ ] `l2cap_sig_handle(handle, sig_buf, len)`: parse signalling code:
  - `CONNECTION_REQ`: allocate local CID (next free 0x0040–0x007F); send `CONNECTION_RSP (PENDING)`; accept; send `CONNECTION_RSP (SUCCESS, local_cid, remote_cid)`
  - `CONFIGURATION_REQ`: reply with `CONFIGURATION_RSP (SUCCESS)` accepting MTU; store negotiated MTU in channel
  - `DISCONNECTION_REQ`: reply `DISCONNECTION_RSP`; free channel
- [ ] `l2cap_connect(handle, psm, recv_cb)` → send `CONNECTION_REQ`; wait for `CONNECTION_RSP`; send `CONFIGURATION_REQ (MTU=672)`; wait `CONFIGURATION_RSP`; return channel
- [ ] `l2cap_send(chan, data, len)`: build L2CAP PDU; call `hci_acl_send(chan->handle, ...)`
- [ ] `l2cap_disconnect(chan)`: send `DISCONNECTION_REQ`; free channel entry
- [ ] Commit: `"drivers: L2CAP -- channel multiplexing, sig handle, connect/config/disconnect, l2cap_send"`

## 4. SDP -- Service Discovery Protocol `[Sonnet]`

Issue `SDP_ServiceSearchAttributeRequest` to remote devices to discover PSM/channel for HID, A2DP, and RFCOMM. Build a minimal local SDP server with records for local services.

**Files:** `src/kernel/drivers/bt_sdp.c` (new), `include/kernel/drivers/bt_sdp.h` (new)

> [!NOTE]
> SDP uses L2CAP PSM 0x0001. SDP PDU: `PDU_ID(1) + TxnID(2) + ParamLen(2) + params`. `ServiceSearchAttributeRequest (0x06)`: `ServiceSearchPattern (UUID list)` + `MaxAttributeByteCount` + `AttributeIDList`. Response `(0x07)`: `AttributeListsByteCount` + `AttributeLists` (sequence of attribute lists). Key attribute IDs: `0x0001=ServiceClassIDList`, `0x0004=ProtocolDescriptorList`, `0x0100=ServiceName`.

- [ ] `sdp_query(handle, uuid, attribute_ids[], id_count, result_cb)`: open L2CAP channel to PSM 0x0001; send `ServiceSearchAttributeRequest`; parse response; extract `ProtocolDescriptorList` → find L2CAP PSM and RFCOMM channel; call `result_cb`
- [ ] Parse `ServiceClassIDList`: UUID16 comparison for `HID (0x1124)`, `AudioSink (0x110B)`, `SerialPort (0x1101)`, `HandsFree (0x111E)`
- [ ] Local SDP server: register records for local services -- `HIDHost (0x1131)`, `A2DP Source (0x110A)` (if supporting audio source), `RFCOMM`; respond to remote `ServiceSearchAttributeRequest` queries on PSM 0x0001
- [ ] `sdp_find_hid_psm(handle, &control_psm, &interrupt_psm)` convenience wrapper → query UUID `0x1124`, extract L2CAP PSMs from ProtocolDescriptorList
- [ ] `sdp_find_rfcomm_channel(handle, uuid, &channel)` → query given UUID, find RFCOMM channel number
- [ ] Commit: `"drivers: SDP -- ServiceSearchAttributeRequest, PSM discovery, local SDP server records"`

## 5. HID Profile -- Bluetooth HID `[Sonnet]`

Establish L2CAP control (PSM 0x0011) + interrupt (PSM 0x0013) channels. Receive HID INPUT reports on the interrupt channel. Reuse the HID report descriptor parser from TODO-12 §5. Register with the keyboard/mouse input pipeline identically to USB HID.

**Files:** `src/kernel/drivers/bt_hid.c` (new), `include/kernel/drivers/bt_hid.h` (new)

> [!NOTE]
> Bluetooth HID uses two L2CAP channels: control (PSM 0x0011) for `GET_REPORT` / `SET_REPORT` / `SET_PROTOCOL` messages, and interrupt (PSM 0x0013) for INPUT report streaming. Report messages are prefixed with a 1-byte `HID_MESSAGE` header: `0xA1`=INPUT, `0xB1`=OUTPUT, etc. The HID report descriptor is fetched via SDP attribute `0x0206 (HIDDescriptorList)` rather than via USB GET_DESCRIPTOR.

- [ ] `bt_hid_probe(handle)`: `sdp_find_hid_psm(handle, &ctrl_psm, &intr_psm)`; open L2CAP channel to `ctrl_psm`; open L2CAP channel to `intr_psm`; query SDP attribute `0x0206` for HID descriptor; call `hid_parse_report_descriptor()` (TODO-12 §5); determine HID subtype (keyboard/mouse)
- [ ] `SET_PROTOCOL(BOOT_PROTOCOL)` on control channel: HID control msg `0x71` (SET_PROTOCOL, protocol=0x00=Boot)
- [ ] Interrupt channel `recv_cb`: strip `0xA1` header; dispatch to `hid_dispatch_report(dev, buf+1, len-1)` → keyboard or mouse event injection (same pipeline as USB HID)
- [ ] `bt_hid_disconnect(handle)`: close both L2CAP channels; deregister from input subsystem
- [ ] Hot-connect: in `hci_event_dispatch()`: on `CONNECTION_COMPLETE`, if `remote_class & 0x0500` (peripheral), call `bt_hid_probe()`
- [ ] SSP Just-Works pairing for HID: `IO_CAPABILITY_RESPONSE` with `NoInputNoOutput`; auto-accept `USER_CONFIRMATION_REQUEST` event (§9)
- [ ] Boot log: `[BT-HID] Keyboard/Mouse paired: %s BD_ADDR %02x:%02x:...`
- [ ] Commit: `"drivers: BT HID -- L2CAP ctrl/intr channels, SDP descriptor, boot-protocol, input injection"`

## 6. BLE -- Bluetooth Low Energy `[Opus]`

Implement BLE scanning (LE_SET_SCAN_PARAMETERS + LE_SET_SCAN_ENABLE), LE connection creation, ATT protocol over L2CAP CID 0x0004, and GATT profiles for Battery Service (0x180F) and Device Information (0x180A). Used for modern BLE mice, keyboards, and peripherals.

**Files:** `src/kernel/drivers/bt_ble.c` (new), `include/kernel/drivers/bt_ble.h` (new)

> [!NOTE]
> LE HCI commands: `LE_SET_SCAN_PARAMETERS (0x200B)`: `scan_type=0x00` (passive), `scan_interval=0x00A0` (100 ms), `scan_window=0x0010` (10 ms), `own_addr_type=0x00`, `filter_policy=0x00`. `LE_SET_SCAN_ENABLE (0x200C)`: `enable=1, filter_dup=1`. `LE_CREATE_CONNECTION (0x200D)`: connect to specific BDADDR. ATT PDUs: `ATT_READ_BY_TYPE_REQ (0x08)` with UUID to enumerate characteristics.

- [ ] `bt_ble_scan_start(callback)`: `LE_SET_SCAN_PARAMETERS`; `LE_SET_SCAN_ENABLE(1)`; parse `LE_META_EVENT (0x3E)` subevent `0x02 LE_ADVERTISING_REPORT`: extract BD_ADDR, RSSI, AD type (0x09=Complete Name, 0x01=Flags, 0x07=UUID128 list); call `callback(addr, name, rssi, services)`
- [ ] `bt_ble_connect(bd_addr)`: `LE_CREATE_CONNECTION(addr, ...)` with scan interval/window; wait `LE_META_EVENT subevent 0x01 CONNECTION_COMPLETE`; store LE handle in `hci_conn[]` with type=BLE
- [ ] ATT: `att_read_by_type(handle, start_handle, end_handle, uuid16)` → `ATT_READ_BY_TYPE_REQ (0x08)`; parse `ATT_READ_BY_TYPE_RSP (0x09)` → list of `{ attr_handle, value }`
- [ ] `att_find_information(handle, start, end)` → `ATT_FIND_INFORMATION_REQ (0x04)`; parse `ATT_FIND_INFORMATION_RSP (0x05)` → UUID-to-handle map
- [ ] GATT Battery Service `0x180F`: `att_read_by_type(Battery Level char, UUID=0x2A19)` → 1 byte 0–100; stored in device's `bt_ble_device_t.battery_pct`; Device Manager health shows `[BLE] Mouse battery: 78%`
- [ ] GATT Device Information `0x180A`: read `Manufacturer Name (0x2A29)`, `Model Number (0x2A24)`; stored in `bt_ble_device_t.manufacturer`, `.model`
- [ ] BLE HID: if advertising data includes `HID (0x1812)` UUID, connect and call `bt_hid_probe()` (§5 via L2CAP LE credit-based channels)
- [ ] Boot log: `[BLE] Scan enabled; device found: %s RSSI %d dBm`
- [ ] Commit: `"drivers: BLE -- LE scan, LE_CREATE_CONNECTION, ATT, GATT Battery/DevInfo, BLE HID probe"`

## 7. RFCOMM & Serial Port Profile (SPP) `[Sonnet]`

Implement RFCOMM multiplexed serial over L2CAP PSM 0x0003. Support SABM/UA/DM/DISC frames, port negotiation (RPN), modem status (MSC). Expose `/dev/rfcomm0` VFS node for user-mode access.

**Files:** `src/kernel/drivers/bt_rfcomm.c` (new), `include/kernel/drivers/bt_rfcomm.h` (new)

> [!NOTE]
> RFCOMM frame format: `Address(1) + Control(1) + Length(1 or 2) + [Data] + FCS(1)`. Control field: `SABM=0x2F` (connect), `UA=0x63` (ack), `DM=0x0F` (reject), `DISC=0x43` (disconnect), `UIH=0xEF` (data/UIH). Multiplexer control (DLCI 0): `RPN (0x91)` for port settings, `MSC (0xE3)` for modem status. Data channels use DLCI = `channel*2 + direction`.

- [ ] Multiplexer init: `rfcomm_mux_start(l2cap_chan)`: send `SABM` on DLCI 0; wait `UA`; negotiation complete
- [ ] `rfcomm_open(handle, channel)`: allocate DLCI; send `RPN` (port negotiation: baud=9600, data=8N1); send `SABM(dlci)`; wait `UA`; send `MSC(dlci, DTR+RTS set)`; return handle
- [ ] `rfcomm_write(handle, buf, len)`: send `UIH(dlci, data)` frames; credit-based flow control (`UIH EA=1, C/R=1, credits`)
- [ ] `rfcomm_read(handle, buf, max)`: drain received `UIH` frames for DLCI; copy to buf
- [ ] `rfcomm_close(handle)`: send `DISC(dlci)`; wait `UA`/`DM`
- [ ] VFS node: `vfs_register_device("rfcomm0", &rfcomm_vfs_ops)` -- `open/read/write/close` backed by `rfcomm_open/read/write/close`; device appears at `\Device\Serial\rfcomm0`
- [ ] `sdp_find_rfcomm_channel(handle, SPP_UUID, &ch)` → `rfcomm_open(handle, ch)` in SPP connect flow
- [ ] Commit: `"drivers: BT RFCOMM/SPP -- SABM/UA/UIH frames, port negotiation, /dev/rfcomm0 VFS node"`

## 8. A2DP + SBC Encoder + AVRCP `[Opus]`

Implement A2DP audio streaming via AVDTP on L2CAP PSM 0x0019: SEP discovery, SBC configuration, stream start. Implement SBC encoder (8 subbands, 16 blocks, bitpool 53). Stream via RTP on the transport channel. Add AVRCP control channel for play/pause/volume.

**Files:** `src/kernel/drivers/bt_a2dp.c` (new), `src/kernel/drivers/bt_sbc.c` (new), `include/kernel/drivers/bt_a2dp.h` (new)

> [!NOTE]
> AVDTP PDU over L2CAP PSM 0x0019: `AVDTP_DISCOVER (0x01)` → SEP list; `AVDTP_GET_CAPABILITIES (0x02, seid)` → codec capabilities; `AVDTP_SET_CONFIGURATION (0x03, seid, ...)` → configure SBC; `AVDTP_OPEN (0x06, seid)` → open transport channel; `AVDTP_START (0x07, seid)` → start streaming. SBC frame: header (4 bytes) + scale factors + audio samples. RTP payload type 96, timestamp = frame number × 512 samples.

- [ ] AVDTP signalling: open L2CAP PSM 0x0019; `AVDTP_DISCOVER` to get SEP list; find SEP with `MEDIA_TYPE=Audio, SINK`; `GET_CAPABILITIES` → verify SBC codec supported; `SET_CONFIGURATION(SBC: subbands=8, blocks=16, allocation=loudness, bitpool=53, fs=44100, stereo=joint)`; `OPEN` → opens transport L2CAP channel; `START`
- [ ] SBC encoder: `sbc_encode(int16_t pcm_l[128], int16_t pcm_r[128], uint8_t *frame_out)`:
  - Analysis filter bank: 8-subband polyphase analysis of each 8-sample block × 16 blocks
  - Bit allocation: loudness allocation table; compute bits per subband from scale factors + bitpool
  - Quantise and pack: interleave L+R subband samples; pack into SBC frame with 4-byte header
  - Output: ~105 bytes per 512-sample stereo frame at 44100 Hz, bitpool 53 ≈ 328 kbps
- [ ] RTP wrapper: `rtp_send(sbc_frame, len, timestamp)`: build RTP header (V=2, PT=96, timestamp, SSRC); append SBC frame; send via AVDTP transport L2CAP channel
- [ ] Audio HAL hook: `bt_a2dp_device.write(pcm, frames)` encodes SBC and sends RTP; registered with `audio_register()` at highest priority when headphones connect
- [ ] AVRCP: open L2CAP PSM 0x0017; AV/C command frame: `PLAY (0x44)`, `PAUSE (0x46)`, `STOP (0x45)`, `FORWARD (0x4B)`, `BACKWARD (0x4C)`; `VOLUME (0x48)` → forward to `audio_set_volume()`
- [ ] A2DP hot-connect: on `CONNECTION_COMPLETE` for device class `0x0400` (Audio), call `bt_a2dp_probe()`; on connect: `audio_notify_attach(&bt_a2dp_device)`
- [ ] Boot log: `[BT-A2DP] Headphones connected: %s, SBC bitpool=53`
- [ ] Commit: `"drivers: BT A2DP -- AVDTP SEP discovery/config/start, SBC encoder, RTP, AVRCP control"`

## 9. Bluetooth Manager + Pairing UI + `bluetooth.cpl` `[Opus]`

**Design:** [`shell.md#settings-and-control-panel-frame`](../../docs/design/shell.md#settings-and-control-panel-frame), [`controls.md#cards-and-settings-rows`](../../docs/design/controls.md#cards-and-settings-rows)

Implement `bt_manager.c` as the central Bluetooth state controller. Handle SSP Just-Works and Numeric Comparison pairing. Store paired devices in Registry. Feed the quick settings Bluetooth tile. Provide `bluetooth.cpl` settings applet for scan, pair, connect, disconnect, forget.

**Files:** `src/kernel/drivers/bt_manager.c` (new), `include/kernel/drivers/bt_manager.h` (new), `src/desktop/bluetooth_cpl.c` (new)

> [!IMPORTANT]
> SSP (Secure Simple Pairing) requires `hwrng_read()` for the local nonce. Numeric comparison MIC verification **must** use `memcmp_constant_time()`. SSP phases: `IO_CAPABILITY_REQUEST` → send `IO_CAPABILITY_RESPONSE(NoInputNoOutput)`; `USER_CONFIRMATION_REQUEST` → auto-accept for Just-Works; `SIMPLE_PAIRING_COMPLETE` → store link key.

- [ ] `bt_manager_init()`: send HCI init sequence (§1); load firmware (§2 if needed); start scan on boot if `HKLM\SYSTEM\Bluetooth\AutoScan=1`
- [ ] SSP pairing: on `IO_CAPABILITY_REQUEST` event: reply `IO_CAPABILITY_RESPONSE(capability=NoInputNoOutput, OOB=0, auth=MITM_NOT_REQUIRED)`; on `USER_CONFIRMATION_REQUEST`: if Just-Works, `HCI_USER_CONFIRMATION_REQUEST_REPLY(bd_addr)`; if Numeric Comparison, show passkey dialog then reply; on `SIMPLE_PAIRING_COMPLETE (status=0)`: store link key via `LINK_KEY_REQUEST_REPLY(bd_addr, key[16])`
- [ ] Paired device Registry: `HKLM\SYSTEM\Bluetooth\PairedDevices\{BD_ADDR}\`: `Name (REG_SZ)`, `Class (REG_DWORD)`, `LinkKey (REG_BINARY, 16 bytes)`, `AutoConnect (REG_DWORD)`
- [ ] Auto-reconnect: at boot and on Bluetooth enable, iterate paired devices with `AutoConnect=1`; issue `HCI_CREATE_CONNECTION(bd_addr)`
- [ ] Bluetooth state in quick settings: `bt_manager_get_state()` drives the Bluetooth toggle tile of the quick settings flyout (`08-graphics-ui/TODO-09 §8`, `docs/design/shell.md#quick-settings`)
  - Label `Off / Ready / <DeviceName>`; right-click the tile → `Open bluetooth.cpl`; no standalone tray icon
- [ ] `bluetooth.cpl` applet: sections: **Scan** (5 s inquiry, results list: name + type icon + RSSI), **Pair** button → SSP flow, **Connected devices** list with disconnect/forget, **Settings**: discoverable toggle, auto-connect, adapter name
- [ ] Boot log: `[BT-MGR] Bluetooth ready, %u paired devices loaded from Registry`
- [ ] Bluetooth power state honours airplane mode: `bt_set_enabled()` is driven by the radio coordinator -> XREF: `04-drivers-hardware/TODO-15-wifi-drivers.md` §11 (Airplane Mode Radio Coordinator)
- [ ] Commit: `"desktop: BT manager -- SSP pairing, link key Registry, auto-reconnect, bluetooth.cpl applet"`

## 10. `btctl` Shell Command `[Sonnet]`

Implement `btctl` as a built-in shell command matching Linux `bluetoothctl` syntax for diagnostic and scripting compatibility.

**Files:** `src/shell/cmd_btctl.c` (new)

- [ ] `btctl scan` -- 5 s BR/EDR inquiry + BLE scan; print: `[NEW] Device AA:BB:CC:DD:EE:FF "DeviceName" class=0x0540 rssi=-65`
- [ ] `btctl scan le` -- BLE-only scan; print advertising data: `[LE] AA:BB:CC... "BLE Mouse" battery=82%`
- [ ] `btctl pair <addr>` -- initiate pairing; print pairing progress; confirm numeric comparison if needed
- [ ] `btctl connect <addr>` -- connect to paired device; print `[CONNECTED] ...`
- [ ] `btctl disconnect <addr>` -- send `HCI_DISCONNECT`; print `[DISCONNECTED] ...`
- [ ] `btctl devices` -- list paired devices with name, class, AutoConnect flag
- [ ] `btctl info <addr>` -- print: BD_ADDR, name, class, link key presence, GATT Battery level (if BLE), services (from SDP)
- [ ] `btctl remove <addr>` -- delete from Registry (forget device)
- [ ] `btctl power on/off` -- `WRITE_SCAN_ENABLE(0x03)` / `WRITE_SCAN_ENABLE(0x00)` + RF kill if supported
- [ ] Register in `src/shell/shell.c` dispatch table
- [ ] Commit: `"shell: btctl -- scan/pair/connect/disconnect/devices/info, bluetoothctl-compatible syntax"`

---

## OS Comparison


| ⭐  | Feature                                      | 🪟 Win11                                                 | 🐧 Linux                                                         | 🚀 Impossible OS                                                                |
| --- | -------------------------------------------- | -------------------------------------------------------- | ---------------------------------------------------------------- | ------------------------------------------------------------------------------- |
| ⚠️  | HCI transport (USB) -- commands, events, ACL | ✅ `BTHUSB.sys`; full HCI over USB                       | ✅ `btusb.c`; full HCI; `hci_register_dev()`                     | ⚠️ §8 -- Partial -- `bt_hci_usb.c` stub (TODO-10                                |
| 💎  | Chipset firmware loading                     | ✅ Firmware embedded in `ibtusb.sys` /                   | ✅ `btintel.c`/`btrtl.c`; firmware from `linux-firmware`; loaded | ⬜ §2 -- Intel TLV vendor opcode download,                                      |
| 💎  | L2CAP channel multiplexing + MTU negotiation | ✅ `bthport.sys`; L2CAP in-kernel                        | ✅ `l2cap_core.c`; in-kernel; `l2cap_sock`                       | ⬜ §3 -- 32-channel table, sig handle, `l2cap_connect/send/disconnect`          |
| 💎  | SDP service discovery + local SDP server     | ✅ `bthserv.dll`; SDP in user-space via                  | ✅ `sdp.c` in BlueZ user-space (`bluetoothd`)                    | ⬜ §4 -- `ServiceSearchAttributeRequest`, PSM/channel extraction, local records |
| 💎  | Bluetooth HID                                | ✅ `hidbth.sys`; HID profile; `HIDCLASS` integration     | ✅ `hidp.c` in-kernel; L2CAP PSM 0x11/0x13;                      | ⬜ §5 -- L2CAP ctrl/intr, SDP descriptor, `hid_parser.c`                        |
| 💎  | A2DP audio streaming                         | ✅ `bthA2dp.sys`; AVDTP; SBC; `Waveout` integration      | ✅ BlueZ A2DP + `pulseaudio-module-bluetooth`; SBC               | ⬜ §8 -- AVDTP in-kernel, SBC encoder, RTP,                                     |
| 💎  | RFCOMM / SPP                                 | ✅ `rfcomm.sys`; `bthport` RFCOMM; COM port              | ✅ `rfcomm.c` in-kernel; `/dev/rfcomm0`; `ttyBT` interface       | ⬜ §7 -- SABM/UA/UIH, RPN/MSC, VFS `\Device\Serial\rfcomm0`                     |
| 💎  | BLE scanning + GATT Battery/DevInfo profiles | ✅ `bthport.sys` BLE; WinRT Bluetooth API;               | ✅ `hci_le.c` + `att.c` in BlueZ;                                | ⬜ §6 -- LE scan, ATT read-by-type, GATT                                        |
| ⭐  | In-kernel BT stack (no daemon)               | ⚠️ `bthserv.exe` user-space service for pairing/profiles | ❌ BlueZ `bluetoothd` user-space daemon --                       | ⬜ §9 -- `bt_manager.c` in kernel, SSP Just-Works,                              |
| 💎  | `btctl` CLI -- scan/pair/connect/info        | ❌ No inbox `btctl`; PowerShell `Get-PnpDevice`          | ✅ `bluetoothctl`; `btmgmt`; `hcitool`; standard diagnostic      | ⬜ §10 `btctl scan/pair/connect/devices/info`, `bluetoothctl`-compatible syntax |

> **After §1–10:** Impossible OS ships a complete Bluetooth stack covering all essential modern use cases -- BT HID, A2DP headphones, BLE peripherals, and serial devices. The `⭐` architectural differentiator is §9: the entire stack, including SDP, profiles, pairing, and auto-reconnect, runs in the kernel without a `bluetoothd`-equivalent daemon. Windows runs `bthserv.exe` in user space; Linux runs all of BlueZ in user space via D-Bus. Impossible OS eliminates the IPC round-trip for every HCI event, reducing BT HID input latency and A2DP underrun frequency on loaded systems.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] USB Bluetooth dongle (generic): `[BT-HCI] BD_ADDR ... LMP version`; `btctl power on` → `[BT] Ready`
- [ ] Intel AX200-BT: firmware loaded, `[BT] Intel firmware loaded, version ...`; adapter shows BD_ADDR
- [ ] Bluetooth keyboard: `btctl scan` finds device; `btctl pair <addr>` → SSP Just-Works → `SIMPLE_PAIRING_COMPLETE`; `btctl connect <addr>` → HID INPUT reports arrive → typing in terminal works
- [ ] Bluetooth mouse: pair + connect → mouse movement works; device visible in Device Manager
- [ ] A2DP headphones: pair + connect → `[BT-A2DP] Headphones connected ... SBC bitpool=53`; `audio_get_active()` returns BT A2DP; audio plays through headphones; AVRCP pause button works
- [ ] RFCOMM/SPP device (e.g., GPS): `btctl connect <addr>` → `rfcomm_open` → `cat \Device\Serial\rfcomm0` shows NMEA data
- [ ] BLE mouse: `btctl scan le` shows device; connect → `GATT Battery Level: 85%`; mouse moves correctly
- [ ] Auto-reconnect: restart with BT keyboard in paired list and `AutoConnect=1` → keyboard reconnects automatically
- [ ] `bluetooth.cpl`: scan shows nearby devices; pair dialog appears for new device; paired device list shows Forget/Disconnect
- [ ] `btctl info <addr>`: prints name, class, services (HID/A2DP), battery (BLE)
- [ ] Commit: `"drivers: BT full stack -- HCI, firmware, L2CAP, SDP, HID, A2DP/SBC, RFCOMM, BLE/GATT, manager, btctl"`
