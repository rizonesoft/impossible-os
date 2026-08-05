---
schema_version: 1
id: wifi-drivers
domain: 04-drivers-hardware
status: active
title: "TODO-15 -- WiFi Hardware Drivers"
---

# TODO-15 -- WiFi Hardware Drivers

> **Goal:** Implement the complete WiFi hardware driver layer -- a `wifi_device_t` vtable abstraction, four real-hardware drivers (RTL8188/8192 USB, RTL8821CE/8822BE PCIe, Intel AX200/AX210 iwlwifi, MediaTek MT7921/MT7922), a WPA2-PSK EAPOL supplicant with CCMP decryption, 802.11 frame layer, scan/association state machine, power management, and `netsh wlan` + `ncpa.cpl` WiFi UI integration -- making Impossible OS functional on any laptop or device that lacks Ethernet.

> [!IMPORTANT]
> **No WiFi infrastructure exists.** The networking stack (`src/kernel/net/`) handles Ethernet frames via `net_receive_ethernet()`; WiFi feeds into the same hook after stripping the 802.11 header and LLC/SNAP. The WiFi driver layer is architecturally independent of the Ethernet NIC drivers (`04-drivers-hardware/TODO-14`). The WPA2-PSK supplicant (§5) is the most security-critical piece -- it must use constant-time comparison for MIC verification to prevent timing side-channels. WiFi UI (`ncpa.cpl` §10) and connection manager live here; the `ncpa.cpl` base infrastructure belongs to `10-platform-services/TODO-08 §10`.

## Inputs

- [`src/kernel/net/ethernet.c`](../../src/kernel/net/ethernet.c) -- `net_receive_ethernet(buf, len)` hook; WiFi data frames deliver here after 802.11 header strip (§2)
- [`src/kernel/drivers/virtio/virtio.c`](../../src/kernel/drivers/virtio/virtio.c) -- VirtIO transport pattern (reference only; WiFi uses its own DMA rings)
- → XREF: `04-drivers-hardware/TODO-14-network-drivers.md §8` -- WiFi 802.11 MAC stub (`wifi_mac_t`) defined there; this TODO supersedes those stubs with full implementations; the vtable names must be reconciled
- → XREF: `04-drivers-hardware/TODO-04-security-hardware.md §2` -- `hwrng_read()` required by WPA2 supplicant for `SNonce` generation (§5) and CCMP nonce (§5); must be available before §5
- → XREF: `10-platform-services/TODO-08` -- `ncpa.cpl` base window (§10) is defined there; WiFi tab (§10 here) extends it; coordinate to avoid duplicate window registration
- → XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md §9` -- Wake-on-WLAN (§9) triggers S3 resume; integrate with ACPI power button SCI path

## Outcome

- `wifi_device_t` vtable in place; `wifi_manager.c` owns the active device and exposes `SYS_WIFI_*` syscalls.
- RTL8188EUS USB WiFi: firmware uploaded via USB control transfer; RX/TX via bulk endpoints; association and WPA2 work.
- RTL8821CE/8822BE PCIe: firmware loaded from `C:\Impossible\System\Firmware\`; DMA rings; MSI IRQ.
- Intel AX200/AX210 (iwlwifi): `iwlwifi-cc-a0-72.ucode` loaded; transport layer; most common laptop WiFi chipset.
- MediaTek MT7921/MT7922: WFDMA rings; firmware from open MediaTek firmware project; common AMD laptop chipset.
- WPA2-PSK 4-way handshake completes; CCMP-encrypted data frames decrypted; open networks work without handshake.
- Scan/association state machine covers: scan → auth → associate → WPA2 → DHCP.
- `netsh wlan show networks`, `netsh wlan connect`, `ncpa.cpl` WiFi tab provide end-user connection UI.

## Implementation Order

| ⭐   | Order | Deliverable                              | Depends On                               | Status |
| --- | :---: | ---------------------------------------- | ---------------------------------------- | :----: |
| ⭐   |   1   | §1 WiFi driver abstraction layer -- `wifi_device_t`, `wifi_manager.c`, syscalls | none                                     |  [ ]   |
| 💎   |   2   | §2 802.11 frame layer -- header parse/build, LLC/SNAP, `net_receive_ethernet` | §1 (vtable), `ethernet.c`                |  [ ]   |
| 💎   |   3   | §3 WiFi scan & association state machine | §2 (frame layer), §1 (state in manager)  |  [ ]   |
| 💎   |   4   | §4 RTL8188/8192 USB WiFi driver          | §1 (vtable), §2 (frame layer), TODO-10 USB stack |  [ ]   |
| 💎   |   5   | §5 WPA2-PSK supplicant -- EAPOL 4-way handshake, CCMP decrypt | §3 (association path), hwrng (TODO-04 §2) |  [ ]   |
| 💎   |   6   | §6 RTL8821CE/8822BE PCIe driver          | §1, §2, §5 (WPA2 shared path)            |  [ ]   |
| 💎   |   7   | §7 Intel AX200/AX210 iwlwifi PCIe driver | §1, §2, §5                               |  [ ]   |
| 💎   |   8   | §8 MediaTek MT7921/MT7922 PCIe driver    | §1, §2, §5                               |  [ ]   |
| 💎   |   9   | §9 WiFi power management -- PS-Poll, wake-on-WLAN | §3 (associated), TODO-03 §9 ACPI S3      |  [ ]   |
| 💎   |  10   | §10 `ncpa.cpl` WiFi tab + `netsh wlan` commands | §3 (scan/connect API), §5 (WPA2)         |  [ ]   |

> §1 `wifi_device_t` is `⭐` exclusive by architecture: Windows uses NDIS 6.x miniport with a fixed DDI; Linux uses `mac80211` + `cfg80211`. Impossible OS defines a lean 7-function vtable that any driver can satisfy, with `wifi_manager.c` as the single ownership point for scan results, preferred networks, and syscall dispatch -- no kernel socket layer or `wpa_supplicant` daemon required.

---

## 1. WiFi Driver Abstraction Layer `[Opus]`

Define `wifi_device_t` vtable and `wifi_manager.c` singleton. Expose `SYS_WIFI_SCAN`, `SYS_WIFI_CONNECT`, `SYS_WIFI_DISCONNECT`, and `SYS_WIFI_STATUS` syscalls. Maintain preferred-network list in Registry.

**Files:** `include/kernel/net/wifi_device.h` (new), `src/kernel/net/wifi_manager.c` (new)

> [!NOTE]
> `wifi_manager.c` is the single source of truth for WiFi state. It owns the `wifi_scan_results[]` list, the current `wifi_state_t` FSM state, and the `active_device` pointer. No other code modifies WiFi state directly -- all changes go through `wifi_manager_*` functions. This is the equivalent of Windows `WLAN AutoConfig Service` but in-kernel without a service daemon.

- [ ] Define:
  ```c
  typedef struct {
      int  (*scan)(wifi_scan_callback_t cb);
      int  (*connect)(const char *ssid, const char *passphrase, wifi_security_t sec);
      void (*disconnect)(void);
      int  (*get_rssi)(void);
      int  (*set_channel)(uint8_t ch);
      int  (*tx)(const uint8_t *frame, uint16_t len);
      void (*rx_poll)(void);
  } wifi_device_t;
  ```
- [ ] `wifi_manager_register(wifi_device_t *dev)` -- set active device; called from driver probe
- [ ] `wifi_state_t` enum: `WIFI_DISCONNECTED`, `WIFI_SCANNING`, `WIFI_AUTHENTICATING`, `WIFI_ASSOCIATING`, `WIFI_4WAY_HANDSHAKE`, `WIFI_CONNECTED`
- [ ] `wifi_scan_result_t { char ssid[33]; uint8_t bssid[6]; int8_t rssi; uint8_t channel; wifi_security_t security; }` -- up to 32 results in `g_scan_results[]`
- [ ] Registry: `HKLM\SYSTEM\Network\WiFi\AutoConnect (REG_DWORD)`, `HKLM\SYSTEM\Network\WiFi\PreferredNetworks` (multi-string: `SSID\0passphrase\0...`)
- [ ] Syscalls: `SYS_WIFI_SCAN(callback_fn)`, `SYS_WIFI_CONNECT(ssid, passphrase, security)`, `SYS_WIFI_DISCONNECT()`, `SYS_WIFI_STATUS(out_buf)` → fills `wifi_status_t { state, ssid, bssid, rssi, channel, ip }`
- [ ] Auto-connect on boot: after scan completes, compare `ssid` against `PreferredNetworks` list; if match and `AutoConnect=1`, call `wifi_manager_connect()`
- [ ] Commit: `"net: wifi_device_t abstraction -- vtable, wifi_manager.c, SYS_WIFI_* syscalls, preferred networks"`

## 2. 802.11 Frame Layer `[Sonnet]`

Parse and build IEEE 802.11 data frames. Strip the 802.11 header + LLC/SNAP on RX and deliver the Ethernet payload to `net_receive_ethernet()`. Add 802.11 header + LLC/SNAP on TX. Handle management frame subtypes for scan and association.

**Files:** `src/kernel/net/wifi_frame.c` (new), `include/kernel/net/wifi_frame.h` (new)

> [!NOTE]
> 802.11 data frame layout: `Frame Control (2)` + `Duration (2)` + `Addr1 (6)` + `Addr2 (6)` + `Addr3 (6)` + `Seq Ctrl (2)` + optional `Addr4 (6)` + `QoS Ctl (2, if QoS)` + `HT Ctl (4, if HT)` + `LLC/SNAP (8)` + payload. LLC/SNAP: `AA AA 03 00 00 00 EE EE` where `EEEE` is the ethertype. ToDS=1, FromDS=0 for station→AP; ToDS=0, FromDS=1 for AP→station.

- [ ] `ieee80211_hdr_t { uint16_t fc; uint16_t dur; uint8_t addr1[6], addr2[6], addr3[6]; uint16_t seq; }` in `wifi_frame.h`
- [ ] Frame Control field: `type(bits 3:2)`, `subtype(bits 7:4)`, `ToDS(bit 8)`, `FromDS(bit 9)`, `Protected(bit 14)` (encrypted)
- [ ] `wifi_rx_data_frame(frame, len)`: check `FC.type == DATA`; skip QoS/HT extension headers; find LLC/SNAP; verify `AA AA 03 00 00 00`; extract `ethertype` and payload; if `FC.Protected`: pass to `aes_ccm_decrypt()` first; call `net_receive_ethernet(payload - 14, len + 14)` with reconstructed Ethernet header (Addr3=dst, Addr2=src for FromDS)
- [ ] `wifi_tx_data_frame(dst[6], ethertype, payload, len)`: build 802.11 data header (ToDS=1, Addr1=BSSID, Addr2=own_mac, Addr3=dst); append LLC/SNAP `AA AA 03 00 00 00 ethertype`; if associated and `FC.Protected`: encrypt with CCMP; call `dev->tx(frame, frame_len)`
- [ ] Management frames: `wifi_send_probe_request(ssid, ssid_len)`: FC=`0x0040` (PROBE_REQ); Addr1=broadcast, Addr2=own, Addr3=broadcast; body = SSID IE + supported rates IE
- [ ] `wifi_send_auth(bssid)`: FC=`0x00B0`; algo=0 (open), seq=1, status=0
- [ ] `wifi_send_assoc_req(bssid, ssid, ssid_len, rsn_ie)`: FC=`0x0000`; capability=`0x0431`; SSID IE + supported rates IE + RSN IE
- [ ] Commit: `"net: 802.11 frame layer -- RX strip/deliver, TX header build, LLC/SNAP, mgmt frames"`

## 3. WiFi Scan & Association State Machine `[Opus]`

Implement `wifi_scan()` (channel sweep with probe requests) and `wifi_connect()` state machine (scan → authenticate → associate → WPA2 handshake → DHCP). Retry logic with exponential backoff.

**Files:** `src/kernel/net/wifi_manager.c` (extend §1)

- [ ] `wifi_scan(callback)`: for each channel 1–13 (2.4 GHz), 36/40/44/48/52…165 (5 GHz): call `dev->set_channel(ch)`; send `PROBE_REQUEST(broadcast SSID)`; dwell 20 ms; collect `PROBE_RESPONSE` and `BEACON` frames; parse SSID IE, BSSID from Addr3, RSSI from driver RX metadata, channel from DS Parameter Set IE, RSN IE presence → security type; after sweep: call `callback(results, count)`
- [ ] `wifi_connect(ssid, passphrase, security)` state machine transitions:
  - `DISCONNECTED → SCANNING`: call `wifi_scan()` filtered to target SSID; on found → `AUTHENTICATING`
  - `AUTHENTICATING → ASSOCIATING`: send `AUTH_REQ`; wait `AUTH_RESP` (seq=2, status=0); timeout 2 s; on failure → retry (max 3×)
  - `ASSOCIATING → 4WAY_HANDSHAKE` (WPA2) or `CONNECTED` (open): send `ASSOC_REQ`; wait `ASSOC_RESP`; if security=WPA2 → start `wpa2_start_handshake()`; if open → `CONNECTED` → start DHCP
  - `4WAY_HANDSHAKE → CONNECTED`: M1..M4 complete → install keys → start DHCP via `dhcp_start()`
- [ ] `wifi_disconnect()`: send `DEAUTH` management frame; clear key table; return to `DISCONNECTED`
- [ ] Retry: on any state timeout (2 s), retry up to 3×; on 3× failure: `DISCONNECTED`; log `[WiFi] Connect failed: %s`
- [ ] Auto-reconnect: if `AutoConnect=1` and SSID in `PreferredNetworks`, schedule reconnect after 10 s disconnect
- [ ] Boot log: `[WiFi] Scanning... found %u networks`; `[WiFi] Connected to %s (ch %u, RSSI %d dBm)`
- [ ] Commit: `"net: WiFi scan+association SM -- channel sweep, auth/assoc/WPA2/DHCP, retry, auto-reconnect"`

## 4. RTL8188 / RTL8192 USB WiFi Driver `[Opus]`

Implement USB WiFi for Realtek RTL8188EUS/RTL8192EU/RTL8812AU. Upload embedded firmware via USB control transfer. Use bulk-IN for RX, bulk-OUT for TX. Inject received frames into the 802.11 frame layer (§2).

**Files:** `src/kernel/drivers/rtl8188_usb.c` (new), `include/kernel/drivers/rtl8188_usb.h` (new), `resources/firmware/rtl8188eufw.bin` (binary, embedded)

> [!NOTE]
> RTL8188EUS firmware upload: send `USB_DEVICE_REQUEST (0x40, 0x05, 0, 0, NULL, 0)` to reset; then write firmware in 4-byte chunks via `USB_BULK_OUT` to register `0x1000`; verify by reading `REG_SYS_CFG (0x04)` bit `FWRDY (1<<13)`. RX: bulk-IN endpoint delivers raw 802.11 frames with a 24-byte Realtek RX descriptor prepended. TX: write Realtek TX descriptor (8 bytes) + 802.11 frame to bulk-OUT.

- [ ] USB match: `{ 0x0BDA, 0x8179 }` (RTL8188EUS), `{ 0x0BDA, 0x818B }` (RTL8192EU), `{ 0x0BDA, 0x8812 }` (RTL8812AU dual-band)
- [ ] Firmware: embed `rtl8188eufw.bin` as `const uint8_t rtl8188eu_fw[]` in `rtl8188_fw.c`; load via `USB_BULK_OUT` firmware-write command sequence; poll `REG_SYS_CFG.FWRDY` (timeout 1 s)
- [ ] MAC address: read from `REG_MACID (0x0050)` 6 bytes after firmware ready
- [ ] RX endpoint: pre-fill bulk-IN transfers; on completion, strip 24-byte RTL RX descriptor; pass frame to `wifi_rx_frame(frame, len)`
- [ ] TX: `rtl8188_tx(frame, len)`: prepend 8-byte RTL TX descriptor (`txdw0`: aggregate=0, DISDATAFB=0, AMSDU=0); send via bulk-OUT endpoint; poll `REG_TDECTRL` queue status for completion
- [ ] Channel set: write `REG_CCK_CHECK (0x454)` RF channel frequency via `rtl8188_rf_write(path, reg, val)` (3-wire SPI via registers `RF_DATA/RF_CTRL`)
- [ ] Register `wifi_device_t rtl8188_ops` via `wifi_manager_register()`
- [ ] Boot log: `[RTL8188] USB WiFi %04x:%04x FW ready, MAC %02x:%02x:...`
- [ ] Commit: `"drivers: RTL8188EUS USB WiFi -- firmware upload, bulk RX/TX, channel set, MAC read"`

## 5. WPA2-PSK Supplicant -- EAPOL 4-Way Handshake + CCMP `[Opus]`

Implement the WPA2-PSK 4-way EAPOL handshake and AES-128-CCM (CCMP) per-frame decryption. Security-critical code: constant-time MIC comparison, no timing side-channels in key derivation.

**Files:** `src/kernel/net/wpa2.c` (new), `include/kernel/net/wpa2.h` (new), `src/kernel/crypto/aes_ccm.c` (new), `src/kernel/crypto/pbkdf2.c` (new), `src/kernel/crypto/hmac_sha1.c` (new)

> [!IMPORTANT]
> All cryptographic comparisons (MIC verification, PTK comparison) **must** use `memcmp_constant_time(a, b, len)` -- a volatile byte-by-byte loop that cannot be optimised away. Use `hwrng_read()` (TODO-04 §2) for `SNonce` generation. Do not use any GPL-licensed crypto implementation -- implement from NIST FIPS publications (PBKDF2 = RFC 2898, HMAC-SHA1 = RFC 2202, AES-CCM = NIST SP 800-38C).

- [ ] `wpa2_derive_pmk(passphrase, ssid, pmk_out[32])`: PBKDF2-SHA1 with 4096 iterations; output 256-bit PMK
- [ ] `wpa2_derive_ptk(pmk, aa, spa, anonce, snonce, ptk_out[64])`: PRF-512 = `HMAC-SHA1(PMK, "Pairwise key expansion" || min(AA,SPA) || max(AA,SPA) || ANonce || SNonce)` iterated; KCK=ptk[0:16], KEK=ptk[16:32], TK=ptk[32:48]
- [ ] 4-way handshake state machine:
  - M1 receive: extract `ANonce` from EAPOL key frame; validate RSN IE; generate `SNonce = hwrng_read(32)`
  - M2 send: build EAPOL key frame with `SNonce`, RSN IE; compute `MIC = HMAC-SHA1(KCK, eapol_frame)[:16]`; send
  - M3 receive: verify MIC constant-time; decrypt GTK from RSN IE using `AES_UNWRAP(KEK, gtk_enc)`; install PTK+GTK via `wifi_install_keys()`
  - M4 send: EAPOL key frame with MIC, no key data; mark `WIFI_CONNECTED`
- [ ] `aes_ccm_decrypt(TK[16], PN[6], AAD, ciphertext, plaintext, len)`: AES-128-CCM as per IEEE 802.11 CCMP spec; verify MIC (8 bytes); write plaintext; return `DECRYPTION_FAIL` on MIC mismatch (constant-time)
- [ ] `aes_ccm_encrypt(TK, PN, AAD, plaintext, ciphertext, len)` for TX
- [ ] `detect_eapol(frame)`: check LLC/SNAP ethertype `0x888E`; dispatch to `wpa2_handle_eapol(frame, len)`
- [ ] Key installation: `wifi_install_keys(bssid, ptk, gtk)` writes TK to hardware key table via driver-specific register (or software decrypt path if no hardware offload)
- [ ] Commit: `"net: WPA2-PSK supplicant -- PBKDF2/HMAC-SHA1 PMK/PTK, EAPOL 4-way, AES-128-CCM CCMP"`

## 6. RTL8821CE / RTL8822BE PCIe WiFi Driver `[Opus]`

Implement PCIe WiFi for RTL8821CE and RTL8822BE -- the most common WiFi chipset in 2018–2023 laptops. BAR0 MMIO. Load firmware from VFS. DMA descriptor rings for RX/TX. PCIe MSI IRQ for RX.

**Files:** `src/kernel/drivers/rtl8821ce.c` (new), `include/kernel/drivers/rtl8821ce.h` (new)

> [!NOTE]
> Firmware file path: `C:\Impossible\System\Firmware\rtl8821cfw.bin`. Loaded via `vfs_open` / `vfs_read` at driver init. DMA rings: 256-entry RX ring at `RX_RING_BASE_REG`; 256-entry TX ring per AC (BE/BK/VI/VO) at `TX_RING_BASE_*_REG`. Each descriptor is 32 bytes: `OWN` bit, buffer physical address, length, flags. MSI IRQ triggered on `HISR` register bit `RX_OK (1<<0)`.

- [ ] PCI match: `{ 0x10EC, 0xC821 }` (RTL8821CE), `{ 0x10EC, 0xB822 }` (RTL8822BE), `{ 0x10EC, 0xC822 }` (RTL8822CE)
- [ ] BAR0 MMIO map; controller reset: write `0x01` to `REG_SYS_FUNC_EN`, then `0xFF` to `REG_CR`; wait 100 ms
- [ ] Firmware load: `vfs_open("C:\\Impossible\\System\\Firmware\\rtl8821cfw.bin")`; read into buffer; write via MMIO firmware-download registers (`FW_8192C_START_ADDRESS`, write 4-byte chunks, poll `REG_MCUFWDL.FWDL_CHKSUM_RPT`)
- [ ] DMA ring init: `pmm_alloc_contiguous(256 * 32)` for RX ring; fill each descriptor with `OWN=1`, buffer phys addr, `len=4096`; write ring base to `RX_RING_BASE_REG`; enable DMA via `REG_CR`
- [ ] TX ring per AC: similar; `OWN=0` (host owns until TX); write frame, set `OWN=1`, write `TX_POLLING_REG` to kick
- [ ] MSI IRQ: configure via PCIe MSI (→ XREF `TODO-08 §3`); `rtl8821_irq_handler`: check `HISR`; on `RX_OK`: walk RX ring for `OWN=0` descriptors; extract frame; call `wifi_rx_frame()`; recycle descriptor
- [ ] Channel/RF: write channel frequency to `REG_RFPGA0_ANALOGPARAMETER4` and related RF registers (RTL8821C RF table from datasheet)
- [ ] Register `wifi_device_t rtl8821ce_ops`
- [ ] Boot log: `[RTL8821CE] PCIe WiFi BAR0 0x%lx FW ready, MAC %02x:%02x:...`
- [ ] Commit: `"drivers: RTL8821CE PCIe WiFi -- MMIO, firmware load from VFS, DMA rings, MSI IRQ"`

## 7. Intel AX200 / AX210 iwlwifi PCIe Driver `[Opus]`

Implement Intel iwlwifi for AX200 and AX210 -- the most prevalent WiFi chipset in modern x86-64 laptops (>40% market share). BAR0 MMIO transport layer. Load `iwlwifi-cc-a0-72.ucode` (BSD-licensed) from VFS. Firmware command/response protocol.

**Files:** `src/kernel/drivers/iwlwifi.c` (new), `include/kernel/drivers/iwlwifi.h` (new)

> [!IMPORTANT]
> Intel iwlwifi firmware is BSD-2-Clause licensed (redistributable). Source: Intel Linux firmware repository `git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git`. Firmware file: `iwlwifi-cc-a0-72.ucode` (AX200) or `iwlwifi-ty-a0-gf-a0-72.ucode` (AX210). The host-command interface is derived from public Intel driver documentation (iwlwifi transport spec) -- **not** from the GPL `drivers/net/wireless/intel/iwlwifi/` source. Add to `NOTICE.md`: firmware BSD-2, driver clean-room.

- [ ] PCI match: `{ 0x8086, 0x2723 }` (AX200), `{ 0x8086, 0x51F0 }` (AX211), `{ 0x8086, 0x2725 }` (AX210); class `0x0280`
- [ ] BAR0 MMIO; `iwl_write32(offset, val)` / `iwl_read32(offset)` wrappers
- [ ] Firmware load: `vfs_open("C:\\Impossible\\System\\Firmware\\iwlwifi-cc-a0-72.ucode")`; parse `.ucode` sections (header: magic `0x5A4D4F57`, version, CPU1/CPU2 sections); DMA transfer via `iwl_dma_upload(buf, len)`
- [ ] Transport init: write firmware to DMA buffer; `iwl_write32(CSR_RESET, CSR_RESET_REG_FLAG_SW_RESET)`; wait `HW_IF_CONFIG_REG.NIC_READY`; raise `INTERRUPT_ENABLE` register
- [ ] Host command ring (TXQ 0): 256-entry TX descriptors for `WIDE_ID` commands; `iwl_send_cmd(opcode, params, len)` enqueues command TRB; waits for RX response ring notification
- [ ] Key commands: `MVM_ALIVE` (verify firmware alive); `PHY_CONFIGURATION_CMD` (set channel/band); `SCAN_OFFLOAD_REQUEST_CMD` (passive/active scan); `MAC_CONTEXT_CMD` (BSSID association); `ADD_STA_CMD` (peer station for AP)
- [ ] RX ring: 512-entry descriptors; `iwl_rx_handler`: demux by `pkt_type` -- `FRAME_RELEASE` → call `wifi_rx_frame()`, `SCAN_COMPLETE` → notify scan callback
- [ ] No QEMU native emulation → document USB WiFi passthrough as the test path: `qemu -usb -device usb-host,vendorid=0x8086,productid=0x2723`
- [ ] Register `wifi_device_t iwlwifi_ops`
- [ ] Add to `NOTICE.md`: firmware BSD-2 (Intel), driver clean-room
- [ ] Boot log: `[iwlwifi] AX200/AX210 FW v%u.%u, MAC %02x:%02x:...`
- [ ] Commit: `"drivers: iwlwifi -- AX200/AX210 transport, ucode load, host command ring, RX demux"`

## 8. MediaTek MT7921 / MT7922 PCIe WiFi Driver `[Sonnet]`

Implement PCIe WiFi for MT7921/MT7922 -- common in AMD Ryzen laptops (ASUS, MSI, Lenovo). WFDMA (WiFi DMA) TX/RX rings. Load firmware from MediaTek open-source firmware project.

**Files:** `src/kernel/drivers/mt7921.c` (new), `include/kernel/drivers/mt7921.h` (new)

> [!NOTE]
> MT7921 firmware: `WIFI_MT7961_patch_mcu_1_2_hdr.bin` + `WIFI_MT7961_ram_code_1_hdr.bin` from [MediaTek open-source firmware project](https://github.com/openwrt/mt76/tree/master/firmware). Both MIT-licensed. WFDMA registers: `WFDMA_TX_RING_BASE (0xD4000)`, `WFDMA_RX_RING_BASE (0xD4400)`. Ring entries are 8 bytes: `DDONE | len | buf_phys_addr`. Add to `NOTICE.md`.

- [ ] PCI match: `{ 0x14C3, 0x7961 }` (MT7921), `{ 0x14C3, 0x0608 }` (MT7922); class `0x0280`
- [ ] BAR0 MMIO; reset via `MT_CONN_ON_MISC_REG`; wait `MT_WLAN_STATUS_REG.READY`
- [ ] Firmware load: load patch MCU firmware first (`WIFI_MT7961_patch_mcu_1_2_hdr.bin`); write via `MT_PATCH_DL` DMA sequence; then load RAM code (`WIFI_MT7961_ram_code_1_hdr.bin`); verify `MT_FW_STATUS_REG == 0x01`
- [ ] WFDMA TX ring (256 entries): allocate descriptors; write `WFDMA_TX_RING_BASE`; `WFDMA_TX_RING_CNT=256`; `WFDMA_TX_RING_CIDX=0`; `WFDMA_TX_RING_DIDX=0`
- [ ] WFDMA RX ring (256 entries): pre-fill with 2 KiB buffers; write `WFDMA_RX_RING_BASE`; enable `WFDMA_INT_STA.RX_DONE` IRQ
- [ ] `mt7921_tx(frame, len)`: fill TX descriptor; advance `WFDMA_TX_RING_CIDX`; write `WFDMA_TX_RING_BASE + CIDX_REG`
- [ ] IRQ handler: check `WFDMA_INT_STA`; on `RX_DONE`: walk RX ring for `DDONE=1` entries; call `wifi_rx_frame()`; recycle
- [ ] Channel set via `MT_CHANNEL_FREQ_SEL` register and band select
- [ ] Register `wifi_device_t mt7921_ops`; add firmware to `NOTICE.md` (MIT)
- [ ] Boot log: `[MT7921] PCIe WiFi FW ready, MAC %02x:%02x:...`
- [ ] Commit: `"drivers: MT7921/MT7922 PCIe WiFi -- WFDMA rings, MediaTek open firmware, MIT license"`

## 9. WiFi Power Management `[Sonnet]`

Implement 802.11 Power Save Poll (PS-Poll) after association: set the Power Management bit in the frame control field so the AP buffers frames while the station sleeps. Wire wake-on-WLAN to the ACPI S3 resume path.

**Files:** `src/kernel/net/wifi_manager.c` (extend), `src/kernel/drivers/rtl8821ce.c` / `iwlwifi.c` / `mt7921.c` (extend)

> [!NOTE]
> → XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md §9` -- ACPI S3 suspend/resume is the platform suspend path; Wake-on-WLAN requires configuring the NIC to assert a wake signal on a matching Magic Packet while the system is in S3. Each NIC driver must save/restore register state across S3.

- [ ] `wifi_set_powersave(enable)`: if `enable`: set `FC.PM=1` on all subsequent data/null frames; send `NULL_FUNC` frame with `PM=1` to notify AP; AP starts buffering
- [ ] PS-Poll wake: on DTIM beacon received (AP signals buffered frames via TIM IE), send `PS_POLL` control frame to retrieve buffered frame; repeat until TIM cleared
- [ ] `wifi_suspend()`: called from ACPI S3 handler; save NIC register state; program NIC for Magic Packet wake (`WAKE_ON_LAN` equivalent: 6× `0xFF` + 16× station MAC); assert wake enable
- [ ] `wifi_resume()`: called from ACPI S3 resume; restore NIC register state; re-send `NULL_FUNC` with `PM=0`; re-associate if `DEAUTH` was received during sleep
- [ ] Per-driver `wifi_device_t.suspend/resume` ops added: `int (*suspend)(void)`, `void (*resume)(void)`
- [ ] `wifi_set_powersave` Registry key: `HKLM\SYSTEM\Network\WiFi\PowerSave (REG_DWORD, default 1)`
- [ ] Commit: `"net: WiFi power management -- PS-Poll, wake-on-WLAN, S3 register save/restore"`

## 10. `ncpa.cpl` WiFi Tab + `netsh wlan` Commands `[Sonnet]`

Add a WiFi tab to `ncpa.cpl` (Network Connections): scan results list, Connect/Disconnect, passphrase dialog, signal strength bars, Forget network. Add `netsh wlan show networks` and `netsh wlan connect ssid=...` shell commands.

**Files:** `src/desktop/ncpa_cpl.c` (extend or new WiFi tab), `src/shell/cmd_netsh.c` (extend)

> [!NOTE]
> → XREF: `10-platform-services/TODO-08` -- `ncpa.cpl` base window infrastructure is defined there; this section adds the WiFi tab. The WiFi tab only appears if `wifi_manager_get_device() != NULL` (WiFi hardware present). Signal strength bars: map RSSI dBm to 0–4 bars: `< -80 dBm = 0`, `-80..-70 = 1`, `-70..-60 = 2`, `-60..-50 = 3`, `>= -50 = 4`.

- [ ] WiFi tab: scan-results list box with columns `SSID | Security | Signal (bars) | Channel`; auto-refresh scan every 30 s
- [ ] "Connect" button: if security != OPEN, show passphrase input dialog; call `SYS_WIFI_CONNECT(ssid, passphrase, security)`; poll `SYS_WIFI_STATUS` every 500 ms; show progress: "Connecting…" → "Obtaining IP…" → "Connected"
- [ ] "Disconnect" button: call `SYS_WIFI_DISCONNECT()`
- [ ] "Forget" button: remove SSID from `PreferredNetworks` Registry key
- [ ] Connection status strip: shows current SSID, RSSI bars, IP address, "Connected ✅" / "Disconnected ❌"
- [ ] Device Manager integration (→ TODO-07): WiFi device entry shows firmware version + RSSI + current BSSID via `driver_health_set("iwlwifi", OK, "RSSI=-55dBm BSSID=aa:bb:...")`
- [ ] `netsh wlan show networks`: call `SYS_WIFI_SCAN()` (blocking or callback); print `SSID: ... / Authentication: WPA2-Personal / Signal: 80% / BSSID: aa:bb:...` (Windows `netsh wlan show networks` format)
- [ ] `netsh wlan connect ssid="..." key="..."`: call `SYS_WIFI_CONNECT`; poll status; print `Connection request was completed successfully.`
- [ ] `netsh wlan disconnect`: call `SYS_WIFI_DISCONNECT`
- [ ] `netsh wlan show profiles`: list `PreferredNetworks` Registry entries
- [ ] Commit: `"desktop: ncpa.cpl WiFi tab + netsh wlan -- scan list, connect dialog, signal bars, profiles"`

---

## OS Comparison


| ⭐   | Feature                                  | 🪟 Win11                                  | 🐧 Linux                                  | 🚀 Impossible OS                          |
| --- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- |
| ⭐   | WiFi driver vtable                       | ❌ NDIS 6.x miniport DDI --               | ❌ `mac80211` + `cfg80211` -- two-layer   | ⬜ §1 -- 7-function vtable, `wifi_manager.c` singleton, no |
| 💎   | Realtek RTL8188/8192 USB WiFi            | ✅ `rtwlane.sys` inbox; some via WU       | ✅ `rtl8xxxu.c`; embedded firmware; USB bulk | ⬜ §4 -- firmware embed, USB control upload, |
| 💎   | Realtek RTL8821CE/8822BE PCIe WiFi       | ✅ `rtwlane6.sys` inbox                   | ✅ `rtw88_8821ce.c`; DMA rings; MSI IRQ;  | ⬜ §6 -- MMIO, VFS firmware load, DMA     |
| 💎   | Intel AX200/AX210 WiFi 6                 | ✅ `iwifi65.sys` inbox driver             | ✅ `iwlwifi` GPL + BSD ucode;             | ⬜ §7 -- BSD ucode, clean-room transport, host |
| 💎   | MediaTek MT7921/MT7922                   | ✅ `netvwifibus.inf` via WU               | ✅ `mt7921e.c`; WFDMA rings; MIT firmware | ⬜ §8 -- WFDMA rings, MIT firmware, PCIe  |
| 💎   | WPA2-PSK 4-way handshake + CCMP decrypt  | ✅ `dot11krnl.sys`; WPA2 in kernel; `wlansvc` | ✅ `mac80211` CCMP; `wpa_supplicant` user-space for | ⬜ §5 -- PBKDF2+HMAC-SHA1+AES-CCM all in kernel; no |
| 💎   | 802.11 frame layer                       | ✅ `dot11krnl.sys` 802.11 frame processing | ✅ `ieee80211_rx_napi()` in `mac80211`    | ⬜ §2 -- `ieee80211_hdr_t`, LLC/SNAP strip, `net_receive_ethernet` delivery |
| 💎   | WiFi scan + association state machine    | ✅ `wlansvc` AutoConfig; `WLAN_CONNECTION_NOTIFICATION` | ✅ `wpa_supplicant`; `cfg80211` scan; `iw` commands | ⬜ §3 -- channel sweep, auth/assoc/WPA2/DHCP SM, auto-reconnect, |
| 💎   | 802.11 PS-Poll power management + wake-on-WLAN | ✅ NDIS selective suspend; `NdisMIndicateStatus(WLAN_POWER_STATE)` | ✅ `mac80211` PS mode; `rfkill`; wake-on-WLAN | ⬜ §9 -- PS-Poll PM bit, DTIM wake,       |
| 💎   | WiFi UI                                  | ✅ WiFi tray flyout; Settings WiFi        | ✅ `nm-applet`; `nmcli`; GNOME WiFi settings | ⬜ §10 -- `ncpa.cpl` WiFi tab, passphrase dialog, |

> **After §1–10:** Impossible OS covers the four most prevalent laptop WiFi chipsets (Realtek USB, Realtek PCIe, Intel AX200/AX210, MediaTek MT7921) plus complete WPA2-PSK in kernel. The `wifi_device_t` vtable (`⭐`) is the differentiating architecture: the entire WiFi stack -- scan, association, 4-way handshake, CCMP decryption, DHCP -- runs in the kernel without a `wpa_supplicant` daemon or NDIS intermediate driver. This eliminates the 50–200 ms handshake delays caused by user-space daemon round-trips in Linux and the NDIS miniport marshalling overhead in Windows.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] QEMU + USB RTL8188EUS passthrough: `[RTL8188] USB WiFi ... FW ready`; `netsh wlan show networks` lists visible SSIDs; WPA2 network: `netsh wlan connect ssid="TestAP" key="password"` → `Connection request was completed successfully.`; `ping 8.8.8.8` succeeds
- [ ] Laptop with RTL8821CE: `[RTL8821CE] PCIe WiFi FW ready`; scan + WPA2 connect + DHCP + ping
- [ ] Laptop with AX200/AX210: `[iwlwifi] AX200/AX210 FW v72`; scan + WPA2 connect + ping
- [ ] AMD laptop with MT7921: `[MT7921] PCIe WiFi FW ready`; scan + WPA2 connect + ping
- [ ] Open network: `wifi_connect(ssid, NULL, WIFI_OPEN)` → no 4-way handshake → DHCP → connected
- [ ] WPA2 MIC verification: inject modified M2 frame → `wpa2_handle_eapol()` rejects (MIC mismatch); no key installation
- [ ] PS-Poll: `wifi_set_powersave(true)` → `NULL_FUNC PM=1` sent; AP buffers; DTIM wakes station; frames delivered
- [ ] `ncpa.cpl`: WiFi tab visible; scan shows SSIDs with signal bars; Connect → passphrase → progress → Connected; Forget removes from PreferredNetworks
- [ ] `netsh wlan show profiles`: lists saved network SSID
- [ ] Commit: `"drivers+net: WiFi hardware layer -- RTL8188/8821CE, iwlwifi, MT7921, WPA2, scan/connect SM"`
