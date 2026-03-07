# 47 — Bluetooth (Long-term)

## Overview

Bluetooth is extremely complex. Each BT controller needs a specific HCI driver, plus the BT protocol stack (L2CAP, SDP, RFCOMM, A2DP).

---

## Complexity

| Component | Lines | Notes |
|-----------|-------|-------|
| HCI driver (USB-BT) | ~2,000 | Hardware-specific |
| L2CAP (link layer) | ~3,000 | Packet multiplexing |
| SDP (service discovery) | ~1,000 | Find devices |
| RFCOMM (serial port) | ~1,500 | Serial-over-BT |
| A2DP (audio streaming) | ~2,000 | Stereo audio |
| BLE (Low Energy) | ~3,000 | Modern IoT devices |
| **Linux BT stack** | ~100,000+ | For reference |

## QEMU: **No Bluetooth emulation** — requires real hardware + USB passthrough

## Recommendation: Defer indefinitely. Focus on USB and WiFi first.
## If needed: port **BlueKitchen BTstack** (MIT license, ~15K lines, designed for embedded)

## Priority: 🔴 Long-term / Nice-to-have
## Implementation: months
