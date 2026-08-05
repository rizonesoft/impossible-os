---
schema_version: 1
id: network-drivers
domain: 04-drivers-hardware
status: active
title: "TODO-14 -- Network Drivers"
---

# TODO-14 -- Network Drivers

> **Goal:** Expand the Impossible OS wired NIC roster beyond the working RTL8139 by delivering Intel e1000, VirtIO-net, RTL8169/8111, Intel igc (2.5 GbE), and RTL8125 (2.5 GbE) as loadable `.kmod` modules, add a WiFi 802.11 MAC-layer stub plus Intel iwlwifi and Realtek rtw89 device stubs, and maintain a complete license-tracking record for all ported files.

> [!IMPORTANT]
> **Already complete:** RTL8139 built-in driver (`src/kernel/drivers/rtl8139.c`) -- functional, uses port I/O, registers with `ethernet_receive()`. The kernel module loader from `04-drivers-hardware/TODO-05-kernel-module-system.md` is a prerequisite for all loadable-module sections below. WiFi stubs (§8–§10) are **P4 stretch** -- placeholder infrastructure only; no firmware or WPA supplicant implementation is expected here.

## Inputs

- [`src/kernel/drivers/rtl8139.c`](../../src/kernel/drivers/rtl8139.c) -- reference for the existing NIC registration and `ethernet_receive()` call pattern
- [`src/kernel/drivers/virtio/virtio.c`](../../src/kernel/drivers/virtio/virtio.c) -- VirtIO transport (reused by VirtIO-net)
- → XREF: `04-drivers-hardware/TODO-05-kernel-module-system.md` -- kernel module loader, `EXPORT_SYMBOL`, `blkdev_register`/`net_ops` HAL vtables; must be complete before §2–§6
- → XREF: `06-networking` domain -- `ethernet_receive(buf, len)` is the hook into the protocol stack; NIC modules call this on RX; no networking protocol changes needed here
- → XREF: `04-drivers-hardware/TODO-08-core-driver-enhancements.md §3` -- MSI/MSI-X interrupt support used by e1000, igc, and RTL8125 for high-performance interrupt delivery

## Outcome

- Intel e1000 loadable module works in VirtualBox (default NIC); `ping` reaches gateway.
- VirtIO-net loadable module works with QEMU `-device virtio-net-pci`; `ping` reaches gateway.
- RTL8169/8111 gigabit module works on real hardware or QEMU RTL8169 emulation.
- Intel igc (I225/I226 2.5 GbE) and RTL8125 modules provide 2.5 GbE support for modern hardware.
- WiFi 802.11 MAC-layer stub (`wifi_mac_t`) with station-mode state machine and WPA2-PSK handshake stubs is in place; iwlwifi and rtw89 device stubs register against it.
- `LICENSES/` directory tracks all ported files with SPDX identifiers; `NOTICE.md` lists origins.

## Implementation Order

| ⭐   | Order | Deliverable                              | Depends On                          | Status |
| --- | :---: | ---------------------------------------- | ----------------------------------- | :----: |
| 💎   |   1   | §1 License tracking -- `LICENSES/`, `NOTICE.md` | none (create before porting begins) |  [ ]   |
| 💎   |   2   | §2 Intel e1000 module                    | TODO-05 module loader, §1           |  [ ]   |
| 💎   |   3   | §3 VirtIO-net module                     | TODO-05 module loader, VirtIO core  |  [ ]   |
| 💎   |   4   | §4 RTL8169/RTL8111 gigabit module        | TODO-05 module loader, §1           |  [ ]   |
| 💎   |   5   | §5 Intel igc (I225/I226 2.5 GbE) module  | TODO-05 module loader, §1           |  [ ]   |
| 💎   |   6   | §6 RTL8125 2.5 GbE module                | TODO-05 module loader, §1           |  [ ]   |
| 💎   |   7   | §7 Network driver test suite             | §2–§6 all complete                  |  [ ]   |
| 💎   |   8   | §8 WiFi 802.11 MAC layer stub            | none (infrastructure only)          |  [ ]   |
| 💎   |   9   | §9 Intel iwlwifi stub (P4 stretch)       | §8                                  |  [ ]   |
| 💎   |  10   | §10 Realtek rtw89 stub (P4 stretch)      | §8                                  |  [ ]   |

> All rows are 💎 parity: Windows 11 ships inbox drivers for all listed NICs; Linux ships them in `drivers/net/ethernet/`. The differentiator is delivery as proper `.kmod` files loaded by the Impossible OS module system, proving end-to-end loadable-module infrastructure with real network hardware.

---

## 1. License Tracking `[Sonnet]`

Create `LICENSES/` with the BSD-2-Clause and MIT license texts, and `NOTICE.md` with a table of every ported file, upstream source, and SPDX identifier. Must be in place before any porting work begins.

**Files:** `LICENSES/BSD-2-Clause.txt` (new), `LICENSES/MIT.txt` (new), `NOTICE.md` (new or update if exists)

- [ ] `LICENSES/BSD-2-Clause.txt` -- canonical SPDX BSD-2-Clause text
- [ ] `LICENSES/MIT.txt` -- canonical SPDX MIT text
- [ ] `NOTICE.md` header: `# Third-Party Notices`; table columns: `File | Upstream Source | License | SPDX ID | Notes`
- [ ] Initial rows (filled in as porting proceeds): e1000 → SerenityOS `E1000NetworkAdapter.cpp` BSD-2; RTL8169 → FreeBSD `re(4)` BSD-2; igc → FreeBSD `igc(4)` BSD-2; RTL8125 → FreeBSD `re(4)` extended BSD-2
- [ ] VirtIO-net, iwlwifi stub, rtw89 stub: clean-room rows with `N/A` upstream, `GPL-compatible` note
- [ ] Ensure every `src/modules/*/` file contains `// SPDX-License-Identifier: BSD-2-Clause` (or MIT) header comment where applicable
- [ ] Commit: `"legal: LICENSES/ + NOTICE.md -- BSD-2-Clause, MIT, ported file inventory"`

---

## 2. Intel e1000 Module `[Sonnet]`

Port the Intel e1000 driver (BSD-2 source; SerenityOS reference) as a loadable `.kmod`. PCI match on common VirtualBox/QEMU e1000 device IDs. 16-entry TX+RX descriptor rings. IRQ handler drains RX descriptors into `ethernet_receive()`.

**Files:** `src/modules/e1000/e1000.c` (new), `include/kernel/drivers/e1000.h` (new)

> [!NOTE]
> VirtualBox default NIC is `Intel PRO/1000 MT Desktop (82540EM)` = PCI `{0x8086, 0x100E}`. QEMU default e1000 = `{0x8086, 0x100E}`. Server variant for real hardware = `{0x8086, 0x100F}`.

- [ ] PCI match table: `{ 0x8086, 0x100E }`, `{ 0x8086, 0x100F }`, `{ 0x8086, 0x153A }`, `{ 0x8086, 0x10D3 }` (I217-LM)
- [ ] BAR0 MMIO map via `vmm_map_mmio(bar0_addr, 128*1024, PAGE_NO_CACHE)`
- [ ] MAC address: try `RAL`/`RAH` registers first; fall back to EEPROM read via EERD (`EECD` bit-bang)
- [ ] Allocate 16-entry TX descriptor ring + 16 × 2 KiB TX buffers (physically contiguous); write `TDBAL`/`TDBAH`/`TDLEN`/`TDH`/`TDT`
- [ ] Allocate 16-entry RX descriptor ring + 16 × 2 KiB RX buffers; write `RDBAL`/`RDBAH`/`RDLEN`/`RDH`/`RDT`; set `RCTL` (EN, BAM, BSEX=0, BSIZE=2K)
- [ ] `e1000_send(buf, len)`: write descriptor `addr`+`len`+`CMD_EOP|RS`; bump `TDT`; poll `DD` bit for completion
- [ ] IRQ handler: check `ICR`; on `RXT0` (RX timer): walk RX descriptors with `DD` set, call `ethernet_receive(desc.buf, desc.length)`, recycle descriptor, advance `RDT`
- [ ] Register `net_ops_t e1000_ops = { .send = e1000_send, .get_mac = e1000_get_mac }` via kernel module HAL
- [ ] Boot log: `[e1000] MAC %02x:%02x:... @ BAR0 0x%lx`
- [ ] Add to `NOTICE.md`: ported from SerenityOS `Kernel/Net/Intel/E1000NetworkAdapter.cpp` (BSD-2)
- [ ] Commit: `"modules: Intel e1000 -- 16-entry TX/RX rings, IRQ drain, VirtualBox default NIC"`

## 3. VirtIO-net Module `[Sonnet]`

Port VirtIO-net as a loadable module reusing the existing VirtIO transport (`virtio.c`). RX queue 0 pre-populated with descriptors; TX queue 1 sends with kick. MAC from VirtIO config space.

**Files:** `src/modules/virtio_net/virtio_net.c` (new), `include/kernel/drivers/virtio_net.h` (new)

> [!NOTE]
> → XREF: `04-drivers-hardware/TODO-09-hypervisor-abstraction.md §8` -- VirtIO GPU (§8 there) uses the same VirtIO transport pattern. VirtIO-net uses the same `virtio_init_device()`, `virtq_add_buf()`, `virtq_kick()` primitives from `virtio.c`.

- [ ] PCI match: `{ 0x1AF4, 0x1000 }` (legacy), `{ 0x1AF4, 0x1041 }` (modern virtio 1.0)
- [ ] Negotiate features: `VIRTIO_NET_F_MAC` (bit 5), `VIRTIO_NET_F_STATUS` (bit 16)
- [ ] Read MAC from VirtIO net config space (bytes 0–5 of device-specific config)
- [ ] RX queue (queue 0): pre-fill all descriptors with `virtio_net_hdr_t (12 bytes) + 1514 bytes` RX buffers; kick queue after fill
- [ ] RX ISR: drain used ring; for each used buffer, skip `sizeof(virtio_net_hdr_t)` header, call `ethernet_receive(buf + hdr_size, len - hdr_size)`; recycle descriptor back to available ring; kick
- [ ] TX queue (queue 1): `virtio_net_send(buf, len)`: prepend zeroed `virtio_net_hdr_t`; add to available ring; kick queue 1; wait for used-ring notification (or poll timeout)
- [ ] Register `net_ops_t virtio_net_ops` via module HAL
- [ ] Boot log: `[virtio-net] MAC %02x:%02x:... (modern=%d)`
- [ ] Commit: `"modules: VirtIO-net -- RX pre-fill, TX kick, VIRTIO_NET_F_MAC, QEMU test"`

## 4. RTL8169 / RTL8111 Gigabit Module `[Sonnet]`

Port the RTL8169 gigabit driver as a loadable module using FreeBSD `re(4)` (BSD-2) as reference. DMA descriptor rings. Covers RTL8169, RTL8110, RTL8168, RTL8111 (8111 is the most common onboard NIC in 2015–2020 motherboards).

**Files:** `src/modules/rtl8169/rtl8169.c` (new), `include/kernel/drivers/rtl8169.h` (new)

- [ ] PCI match: `{ 0x10EC, 0x8169 }`, `{ 0x10EC, 0x8110 }`, `{ 0x10EC, 0x8168 }`, `{ 0x10EC, 0x8111 }`
- [ ] BAR1 MMIO map (RTL8169 uses BAR1 for MMIO; BAR0 is I/O port)
- [ ] MAC from IDR0–IDR5 registers (6 bytes at MMIO offset 0x00)
- [ ] Allocate 256-entry TX descriptor ring (each 16 bytes) + 256 × 1536 byte TX buffers (DMA-coherent)
- [ ] Allocate 256-entry RX descriptor ring + 256 × 1536 byte RX buffers; set `RCR` (AB|AM|APM|AAP, RX buffer size field)
- [ ] Write descriptor ring physical addresses to `TNPDS` / `RDSAR` registers; set `TCR`, enable `TE`/`RE` in `CR`
- [ ] `rtl8169_send`: write TX descriptor (OWN|FS|LS|len); bump `TxDescIndex`; write `0x40` to `TPPoll` to trigger TX
- [ ] ISR: check `IntrStatus`; ACK by writing back; process RX ring (OWN bit clear → `ethernet_receive`); recycle RX descriptors
- [ ] Register `net_ops_t rtl8169_ops`; add to `NOTICE.md` (FreeBSD `re(4)`, BSD-2)
- [ ] Boot log: `[rtl8169] MAC %02x:%02x:... PCI %04x:%04x`
- [ ] Commit: `"modules: RTL8169/8111 -- DMA rings, ISR RX drain, FreeBSD re(4) port"`

## 5. Intel igc (I225 / I226 2.5 GbE) Module `[Sonnet]`

Port the igc driver for Intel I225-V and I226-V (2.5 GbE) NICs, common on modern consumer and workstation motherboards. FreeBSD `igc(4)` BSD-2 as reference. P3 priority (newer hardware; skip in emulated environments).

**Files:** `src/modules/igc/igc.c` (new), `include/kernel/drivers/igc.h` (new)

- [ ] PCI match: `{ 0x8086, 0x15F2 }` (I225-V), `{ 0x8086, 0x15F3 }` (I225-LM), `{ 0x8086, 0x125B }` (I226-V), `{ 0x8086, 0x125C }` (I226-LM)
- [ ] BAR0 MMIO; MAC from RAL0/RAH0 (same register layout as e1000 family); EEPROM fallback
- [ ] 16-entry TX + RX descriptor rings (igc uses advanced descriptors -- 32-byte TX, 32-byte RX); allocate with `pmm_alloc_contiguous()`
- [ ] RX advanced descriptor: `pkt_addr` (8B), `hdr_addr` (8B), rsvd (8B), status/error/length (8B); check `DD` + `EOP` bits
- [ ] TX advanced descriptor: `buf_addr` (8B), `cmd_type_len` (4B), `olinfo_status` (4B), padding (16B); set `DCMD_EOP|DCMD_RS|DCMD_IFCS`
- [ ] `igc_send`, IRQ handler following e1000 pattern; register `net_ops_t igc_ops`
- [ ] Add to `NOTICE.md` (FreeBSD `igc(4)`, BSD-2)
- [ ] Boot log: `[igc] I225/I226 2.5GbE MAC %02x:%02x:...`
- [ ] Commit: `"modules: Intel igc -- I225/I226 2.5GbE, advanced TX/RX descriptors, FreeBSD igc(4) port"`

## 6. RTL8125 2.5 GbE Module `[Sonnet]`

Port RTL8125 2.5 GbE as a loadable module. Common on AM5 and Intel 12th-gen+ motherboards. FreeBSD `re(4)` extended variant as reference.

**Files:** `src/modules/rtl8125/rtl8125.c` (new), `include/kernel/drivers/rtl8125.h` (new)

- [ ] PCI match: `{ 0x10EC, 0x8125 }`, `{ 0x10EC, 0x3000 }` (RTL8125B)
- [ ] MMIO BAR0; MAC from registers at offsets 0x00–0x05
- [ ] RTL8125 uses a 2-queue architecture (TX queue 0 for normal, TX queue 1 optional); implement single TX queue for simplicity
- [ ] Descriptor ring layout differs from RTL8169: 32-byte TX descriptors with VLAN/checksum offload fields; implement minimal variant (no offload)
- [ ] RX ring: 256 entries, 9K jumbo buffer support optional (start with 1536-byte standard buffers)
- [ ] MAC init sequence: issue software reset (`CR` bit 4), wait for reset complete, apply power-on sequence (`ERI` / `CSI` indirect access registers for PHY setup)
- [ ] Register `net_ops_t rtl8125_ops`; add to `NOTICE.md`
- [ ] Boot log: `[rtl8125] RTL8125 2.5GbE MAC %02x:%02x:...`
- [ ] Commit: `"modules: RTL8125 2.5GbE -- 32-byte descriptors, single TX queue, FreeBSD re(4) port"`

## 7. Network Driver Test Suite `[Sonnet]`

Verify all five NIC modules end-to-end: each NIC sends an ARP request and receives an ARP reply, then `ping 8.8.8.8` (or gateway) succeeds. Document the QEMU command lines.

**Files:** `docs/guides/network-driver-testing.md` (new)

- [ ] e1000 test: QEMU `-netdev user,id=n0 -device e1000,netdev=n0`; boot log shows MAC; `ping` succeeds
- [ ] VirtIO-net test: QEMU `-netdev user,id=n0 -device virtio-net-pci,netdev=n0`; boot log shows MAC; `ping` succeeds
- [ ] RTL8169 test: QEMU `-netdev user,id=n0 -device rtl8139,netdev=n0` (QEMU exposes RTL8139; use VirtualBox RTL8169 emulation or real hardware for full RTL8169 test)
- [ ] igc / RTL8125: real hardware only (no QEMU emulation); document bare-metal test procedure
- [ ] VirtualBox test: set VM NIC to `Intel PRO/1000 MT Desktop`; boot; e1000 module loads; `ping` succeeds
- [ ] Document `NOTICE.md` verification: every ported file listed with SPDX identifier and upstream source
- [ ] RTL8139 RX DPC-first: after TODO-07 §12 drain-on-lower lands, move `rtl8139_irq_body` RX drain into a KDPC + adopt `KeSynchronizeExecution`. → XREF: 02-kernel-core/TODO-07-irql-model-dpcs.md §10 + §12
- [ ] Commit: `"docs: network driver test guide -- e1000, virtio-net, rtl8169 QEMU/VBox test commands"`

## 8. WiFi 802.11 MAC Layer Stub `[Opus]`

Define the `wifi_mac_t` abstraction and station-mode state machine (DISCONNECTED → SCANNING → ASSOCIATING → ASSOCIATED). Provide WPA2-PSK handshake stubs. No firmware or supplicant implementation -- stub hooks only.

**Files:** `include/kernel/net/wifi_mac.h` (new), `src/kernel/net/wifi_mac.c` (new)

> [!NOTE]
> This section intentionally stops at stub infrastructure. Full 802.11 MAC, MLME, and WPA2-PSK four-way handshake are P1 features tracked in the `06-networking` domain. This section creates the interface so WiFi device drivers (§9, §10) have a registration point.

- [ ] `wifi_mac_t { void (*scan)(void); int (*associate)(const char *ssid, const char *psk); void (*deassociate)(void); int (*send_frame)(uint8_t *frame, size_t len); }`
- [ ] `wifi_state_t` enum: `WIFI_DISCONNECTED`, `WIFI_SCANNING`, `WIFI_ASSOCIATING`, `WIFI_ASSOCIATED`
- [ ] `wifi_register_device(wifi_mac_t *ops)` -- register a WiFi backend; stored in `g_wifi_dev`
- [ ] Station state machine: `wifi_scan()` transitions `DISCONNECTED→SCANNING`; `wifi_associate(ssid, psk)` transitions `SCANNING→ASSOCIATING`; on success callback → `ASSOCIATED`
- [ ] WPA2-PSK stubs: `wifi_pmk_derive(psk, ssid)` → 256-bit PMK stub (returns zeros); `wifi_4way_handshake()` → `STATUS_NOT_IMPLEMENTED`
- [ ] Registry: `HKLM\SYSTEM\Network\WiFi\SSID` (`REG_SZ`), `HKLM\SYSTEM\Network\WiFi\PSK` (`REG_SZ`, stored; full encryption P1)
- [ ] Commit: `"net: WiFi 802.11 MAC stub -- wifi_mac_t, station state machine, WPA2-PSK hooks"`

## 9. Intel iwlwifi Stub (P4 Stretch) `[Sonnet]`

Register an iwlwifi PCI device stub against the WiFi MAC layer. Clean-room implementation from Intel firmware interface public spec -- **not** derived from the Linux GPL driver.

**Files:** `src/modules/iwlwifi/iwlwifi_stub.c` (new)

> [!IMPORTANT]
> **P4 stretch goal.** This section creates the PCI detection and stub registration only. No firmware loading, no 802.11 MLME, no TX/RX. A real iwlwifi driver requires the Intel iwlwifi open firmware (`.ucode` files, MIT/ISC-licensed) and a clean-room reimplementation of the firmware host-command interface. That work belongs in a later TODO.

- [ ] PCI match table for AX200 (`{ 0x8086, 0x2723 }`), AX201 (`{ 0x8086, 0x02F0 }`), AX210 (`{ 0x8086, 0x2725 }`)
- [ ] On PCI match: log `[iwlwifi] Intel WiFi %04x detected (stub -- no firmware loaded)`; return stub `wifi_mac_t` with all ops returning `STATUS_NOT_IMPLEMENTED`
- [ ] `wifi_register_device(&iwlwifi_stub_ops)`
- [ ] Add stub source origin to `NOTICE.md` (clean-room, no upstream copy)
- [ ] Commit: `"modules: iwlwifi stub -- AX200/201/210 PCI detection, wifi_mac_t registration (P4)"`

## 10. Realtek rtw89 Stub (P4 Stretch) `[Sonnet]`

Register an rtw89 PCI device stub against the WiFi MAC layer. Clean-room. Covers RTL8852AE (Wi-Fi 6) and RTL8852BE.

**Files:** `src/modules/rtw89/rtw89_stub.c` (new)

> [!IMPORTANT]
> **P4 stretch goal.** Same constraints as §9: PCI detection + stub registration only. No firmware loading or 802.11 MLME. Real rtw89 requires Realtek's open firmware blobs and a clean-room host-command driver.

- [ ] PCI match: `{ 0x10EC, 0x8852 }` (RTL8852AE), `{ 0x10EC, 0xB852 }` (RTL8852BE)
- [ ] On match: log `[rtw89] Realtek WiFi %04x detected (stub -- no firmware loaded)`; return stub `wifi_mac_t`
- [ ] `wifi_register_device(&rtw89_stub_ops)`
- [ ] Add to `NOTICE.md` (clean-room)
- [ ] Commit: `"modules: rtw89 stub -- RTL8852AE/BE PCI detection, wifi_mac_t registration (P4)"`

## OS Comparison


| ⭐   | Feature                                  | 🪟 Win11                                  | 🐧 Linux                                  | 🚀 Impossible OS                          |
| --- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- |
| 💎   | Intel e1000 / e1000e wired NIC           | ✅ `e1000e.sys` inbox NDIS driver         | ✅ `drivers/net/ethernet/intel/e1000/`    | ⬜ §2 -- loadable `.kmod`, VirtualBox default NIC, |
| 💎   | VirtIO-net paravirtual NIC               | ✅ `netkvm.sys` (Red Hat VirtIO drivers   | ✅ `drivers/net/virtio_net.c`; widely used in QEMU/KVM | ⬜ §3 -- loadable `.kmod`, reuses VirtIO transport |
| 💎   | RTL8169 / RTL8111 gigabit NIC            | ✅ `rtwlane.sys` and variants; or third-party | ✅ `drivers/net/ethernet/realtek/r8169.c` | ⬜ §4 -- loadable `.kmod`, 256-entry DMA rings, |
| 💎   | Intel igc (I225 / I226) 2.5 GbE          | ✅ `igc.sys` (Windows Update driver)      | ✅ `drivers/net/ethernet/intel/igc/`; mainline since 5.6 | ⬜ §5 -- loadable `.kmod`, 32-byte advanced descriptors, |
| 💎   | RTL8125 2.5 GbE                          | ✅ Realtek inbox driver via Windows       | ✅ `drivers/net/ethernet/realtek/r8169.c` (8125 support merged 5.9) | ⬜ §6 -- loadable `.kmod`, 32-byte descriptors, FreeBSD |
| 💎   | WiFi 802.11 MAC layer + station state machine | ✅ `wlan.sys` WLAN API; native 802.11     | ✅ `net/mac80211/`; full MLME, `cfg80211` | ⬜ §8 -- `wifi_mac_t` stub, DISCONNECTED→SCANNING→ASSOC state machine |
| 💎   | Intel Wi-Fi 6                            | ✅ `iwifi65.sys` inbox driver             | ✅ `drivers/net/wireless/intel/iwlwifi/`; open firmware + GPL | ⬜ §9 -- PCI detection stub (P4); full    |
| 💎   | Realtek Wi-Fi 6                          | ✅ `rtwlane6.sys` inbox driver            | ✅ `drivers/net/wireless/realtek/rtw89/`; open firmware + GPL | ⬜ §10 -- PCI detection stub (P4); full   |
| ⭐   | License tracking                         | ❌ Closed-source; no per-driver attribution table | ⚠️ `LICENSES/` directory exists; no per-file | ⬜ §1 -- `NOTICES.md` table with file, upstream, |

> **After §1–10:** Impossible OS covers five wired NIC families spanning the three most common PC environments (VirtualBox, QEMU, bare metal) and a WiFi stub infrastructure that positions the OS for future AX200/rtw89 support. The `NOTICE.md` per-file attribution table (§1, `⭐`) is Impossible OS's strongest open-source governance commitment -- more granular than Linux's `LICENSES/` directory and filling the gap entirely absent from Windows.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] QEMU e1000: `-device e1000,netdev=user0` → boot log `[e1000] MAC 52:54:...`; `ping 10.0.2.2` succeeds
- [ ] QEMU VirtIO-net: `-device virtio-net-pci,netdev=user0` → boot log `[virtio-net] MAC 52:54:...`; `ping 10.0.2.2` succeeds
- [ ] VirtualBox (Intel PRO/1000): e1000 module loaded at boot; `ping` to host-only adapter succeeds
- [ ] RTL8169: QEMU with `-device rtl8139` or real hardware with RTL8168/8111; boot log `[rtl8169]`; `ping` succeeds
- [ ] igc / RTL8125: bare-metal systems with I225/I226 or RTL8125; boot log entry; `ping` succeeds
- [ ] WiFi stub: `wifi_register_device()` called; `wifi_scan()` transitions state to `SCANNING` (log); returns `STATUS_NOT_IMPLEMENTED` for `associate()`
- [ ] iwlwifi stub: QEMU passthrough or bare-metal AX200; boot log `[iwlwifi] Intel WiFi 2723 detected (stub)`
- [ ] rtw89 stub: bare-metal RTL8852AE; boot log `[rtw89] Realtek WiFi 8852 detected (stub)`
- [ ] `NOTICE.md` present with rows for all five wired ports; `LICENSES/BSD-2-Clause.txt` readable; each ported source file has `// SPDX-License-Identifier:` header
- [ ] Commit: `"modules: network drivers -- e1000, virtio-net, rtl8169, igc, rtl8125, WiFi stub, NOTICE.md"`
