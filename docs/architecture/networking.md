# Networking

Impossible OS implements a basic network stack with Ethernet, ARP, IPv4, ICMP,
UDP, and DHCP. The NIC driver supports the RTL8139 (emulated by QEMU).

## Architecture

```
Shell commands (ping, ifconfig)
        │
        ▼
  ┌──────────────┐
  │   Syscalls   │  SYS_PING, SYS_NETINFO
  └──────┬───────┘
         │
  ┌──────▼───────┐
  │  DHCP / ICMP │  Application protocols
  └──────┬───────┘
         │
  ┌──────▼───────┐
  │  UDP / ICMP  │  Transport layer
  └──────┬───────┘
         │
  ┌──────▼───────┐
  │    IPv4      │  Network layer (routing, checksums)
  └──────┬───────┘
         │
  ┌──────▼───────┐
  │    ARP       │  IP → MAC resolution
  └──────┬───────┘
         │
  ┌──────▼───────┐
  │  Ethernet    │  Frame encapsulation (14-byte header)
  └──────┬───────┘
         │
  ┌──────▼───────┐
  │  RTL8139     │  NIC driver (PCI, DMA, IRQ)
  └──────────────┘
```

## RTL8139 NIC Driver

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/drivers/rtl8139.c` | RTL8139 NIC driver |
| `include/kernel/drivers/rtl8139.h` | RTL8139 API |

### Initialization

1. Found via PCI scan (vendor `0x10EC`, device `0x8139`)
2. PCI bus mastering enabled (required for DMA)
3. NIC reset via command register
4. Receive buffer allocated (8 KiB + 16 + 1500 wrap)
5. Rx/Tx enabled with interrupt mask set
6. IRQ handler registered

### Configuration (QEMU)

```makefile
-device rtl8139,netdev=net0
-netdev user,id=net0
```

QEMU's user-mode networking provides NAT, a built-in DHCP server (10.0.2.2),
and DNS forwarding.

### API

| Function | Description |
|----------|-------------|
| `rtl8139_init()` | Detect, reset, configure NIC |
| `rtl8139_send(data, len)` | Transmit a packet |
| `rtl8139_get_mac()` | Read MAC address from EEPROM |
| IRQ handler | Processes received packets → protocol dispatch |

## Protocol Stack

### Ethernet (`ethernet.c`)

| Field | Size | Description |
|-------|------|-------------|
| Destination MAC | 6 bytes | Target hardware address |
| Source MAC | 6 bytes | Sender hardware address |
| EtherType | 2 bytes | `0x0800` = IPv4, `0x0806` = ARP |
| Payload | 46–1500 bytes | Upper-layer packet |

### ARP (`arp.c`)

Address Resolution Protocol — maps IPv4 addresses to MAC addresses.

| Feature | Status |
|---------|--------|
| ARP request (who-has) | ✅ |
| ARP reply | ✅ |
| ARP cache | ✅ |
| Gratuitous ARP | ✅ |

### IPv4 (`ip.c`)

| Feature | Status |
|---------|--------|
| IPv4 header construction | ✅ |
| Header checksum | ✅ |
| Packet sending | ✅ |
| Packet receiving + dispatch | ✅ |
| Fragmentation | ❌ |

### ICMP (`icmp.c`)

| Feature | Status |
|---------|--------|
| Echo request (ping) | ✅ |
| Echo reply | ✅ |

### UDP (`udp.c`)

| Feature | Status |
|---------|--------|
| Send datagrams | ✅ |
| Receive datagrams | ✅ |
| Checksum | ✅ |
| Port binding | Basic |

### TCP (`tcp.c`)

| Feature | Status |
|---------|--------|
| TCP | ❌ Stretch goal — not yet implemented |

## DHCP Client (`dhcp.c`)

Automatic IP configuration at boot:

```
dhcp_discover()
      │
      ▼
  DHCPDISCOVER → broadcast
      │
      ▼
  DHCPOFFER ← from server (10.0.2.2)
      │
      ▼
  DHCPREQUEST → accept offer
      │
      ▼
  DHCPACK ← confirmed
      │
      ▼
  net_cfg populated (IP, subnet, gateway, DNS)
```

### Network Configuration (`net_cfg`)

| Field | Typical Value (QEMU) |
|-------|---------------------|
| IP address | 10.0.2.15 |
| Subnet mask | 255.255.255.0 |
| Gateway | 10.0.2.2 |
| DNS server | 10.0.2.3 |
| MAC address | 52:54:00:xx:xx:xx |

## Shell Commands

| Command | Syscall | Description |
|---------|---------|-------------|
| `ping <ip>` | `SYS_PING` | Send ICMP echo request |
| `ifconfig` / `ipconfig` | `SYS_NETINFO` | Display IP, MAC, gateway, DNS |
