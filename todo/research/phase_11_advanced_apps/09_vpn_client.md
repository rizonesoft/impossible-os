# 72 — VPN Client

## Overview
Connect to VPN servers for encrypted tunneling. Basic WireGuard or OpenVPN-style connectivity.

## Recommended: WireGuard
- Simple protocol (~4,000 lines vs 100K+ for OpenVPN)
- Uses modern crypto (ChaCha20, Curve25519)
- UDP-based (we already have UDP)
- MIT-licensed reference implementation

## Requires: TCP/UDP (`22`), crypto (`31`), TUN/TAP virtual interface
## Settings: `vpn.spl` — server address, key, connect/disconnect

## Files: `src/kernel/net/wireguard.c` (~500 lines) | 2-4 weeks
## Priority: 🔴 Long-term (after TCP/TLS working)
