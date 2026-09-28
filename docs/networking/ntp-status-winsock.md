<!-- docs: covers=todo/07-networking/TODO-06-ntp-status-winsock.md sources=src/kernel/net/dhcp.c,src/kernel/net/icmp.c,src/kernel/net/ip.c,user/cmd.c,include/kernel/time/ntp_adj.h,src/kernel/time/wall_clock.c,src/kernel/pe.c reviewed=2026-09-29 order=6 -->
# Time Sync, Network Status and Winsock

## What is it?

This roadmap finishes the everyday networking tools around the core stack: DHCP lease renewal, an NTP client that keeps the clock right, network statistics with a taskbar status icon, a fuller `ifconfig` and `ping`, `traceroute`, `netstat`, a `/sys/net` summary file, and `ws2_32.dll` so Windows programs can use Winsock. Today Impossible OS has a basic `ifconfig` and a send-only `ping`, and the kernel side of clock discipline already exists, but none of the nine sections has started.

## How does it work?

**What exists.**

- **DHCP** ([`dhcp.c`](../../src/kernel/net/dhcp.c)) gets one lease at boot. It reads only the subnet, router, DNS and message-type options, not the lease time, so it never renews: a machine that stays up past its lease keeps using an address the server may give to someone else.
- **`ifconfig`** (also `ipconfig`) in [`user/cmd.c`](../../user/cmd.c) prints the one interface's MAC, IPv4 address, subnet, gateway and DNS server. It has no MTU, counters, IPv6 or settings.
- **`ping <ip>`** sends four ICMP echo requests through `icmp_send_echo()` ([`icmp.c`](../../src/kernel/net/icmp.c)) and prints `Sent echo request seq=N`. It never collects replies, although the kernel records them (`icmp_ping_got_reply()`) and logs each one.
- **Clock discipline.** [`ntp_adj.h`](../../include/kernel/time/ntp_adj.h) already provides `ke_ntp_adjtime()` and `ke_ntp_get_status()`, implemented in [`wall_clock.c`](../../src/kernel/time/wall_clock.c). An NTP client passes a measured offset and frequency error; the kernel slews offsets of up to one second (`NTP_STEP_THRESHOLD_NS`) at no more than 500 ppm and steps larger ones. The kernel never sends NTP traffic itself.
- **Win32 DLLs.** The loader's built-in export tables in [`pe.c`](../../src/kernel/pe.c) cover `kernel32.dll` and `ntdll.dll` only; there is no `ws2_32.dll`.

**Planned design.**

1. DHCP renewal: lease time, renewal (T1) and rebinding (T2) timers, and a renewal thread.
2. An NTP client that queries `pool.ntp.org` on UDP port 123 at boot and every 24 hours, hands the measured offset to `ke_ntp_adjtime()`, and records the last sync in the Registry. Because `ke_ntp_adjtime()` ignores corrections when the machine never had a firmware clock, and offsets over ten years, the client sets the time outright with `KeSetSystemTime()` in those cases.
3. Per-interface byte, packet and error counters, a `net_stats()` call and a connected or disconnected icon in the notification area.
4. `ifconfig` for several interfaces with IPv6, MTU and counters, plus manual address, up and down.
5. `ping` that waits for replies, with `-4`, `-6`, `-c N` and `-t`.
6. `traceroute`, sending probes with a rising TTL and reading the ICMP Time Exceeded replies. `ipv4_send()` in [`ip.c`](../../src/kernel/net/ip.c) always uses TTL 64 today, so it needs a TTL argument.
7. `netstat`, listing TCP connections and UDP and listening sockets from the socket table.
8. `/sys/net`, one read-only file with interface statistics, the ARP and neighbour tables, the DNS cache and firewall counters.
9. `ws2_32.dll` stubs (`WSAStartup`, `socket`, `connect`, `send`, `recv`, `getaddrinfo`, `select` and the rest) mapped onto the kernel socket layer.

```mermaid
flowchart LR
    DHCP[DHCP renewal] --> IF[interface]
    NTP[NTP client] --> ADJ[ke_ntp_adjtime]
    IF --> ST[net_stats counters]
    ST --> TRAY[tray icon]
    ST --> CLI[ifconfig / netstat]
    ST --> SYS[/sys/net]
    WS[ws2_32.dll] --> SOCK[socket layer]
```

## What are its interfaces?

| Interface | Status |
| --- | --- |
| `ifconfig`, `ipconfig`, `ping <ip>` | Shipped, basic |
| `ke_ntp_adjtime()`, `ke_ntp_get_status()` | Shipped kernel side of NTP |
| `ntp_sync()`, `net_stats()` | Planned |
| `traceroute`, `netstat` | Planned commands |
| `/sys/net` | Planned read-only file |
| `ws2_32.dll` | Planned Winsock DLL |

## How do I use it?

Run `ifconfig` to see the address DHCP assigned. Run `ping 10.0.2.2` under QEMU user networking to send requests to the virtual gateway, and look for `[ICMP] Echo reply from 10.0.2.2` lines in the serial log to confirm replies arrive, since the command itself does not show them.

## What is not implemented yet?

- **Address and time**: [DHCP Renewal Daemon](../../todo/07-networking/TODO-06-ntp-status-winsock.md#1-dhcp-renewal-daemon-sonnet) and [NTP Client](../../todo/07-networking/TODO-06-ntp-status-winsock.md#2-ntp-client-opus).
- **Status and tools**: [Network Statistics API](../../todo/07-networking/TODO-06-ntp-status-winsock.md#3-network-statistics-api-sonnet), [`ifconfig` Command](../../todo/07-networking/TODO-06-ntp-status-winsock.md#4-ifconfig-command-sonnet), [Extended `ping` Command](../../todo/07-networking/TODO-06-ntp-status-winsock.md#5-extended-ping-command-sonnet), [`traceroute` Command](../../todo/07-networking/TODO-06-ntp-status-winsock.md#6-traceroute-command-opus), [`netstat` Command](../../todo/07-networking/TODO-06-ntp-status-winsock.md#7-netstat-command-sonnet) and [`/sys/net` VFS Aggregation File](../../todo/07-networking/TODO-06-ntp-status-winsock.md#8-sysnet-vfs-aggregation-file-sonnet).
- **Win32**: [`ws2_32.dll` Winsock Stubs](../../todo/07-networking/TODO-06-ntp-status-winsock.md#9-ws2_32dll-winsock-stubs-sonnet).
- **DHCP safety**: the client uses the fixed transaction ID `0x12345678`, so another host on the network can answer in place of the real server. Randomising it is filed with the renewal work.

## How does it compare with Windows 11 and Linux?

Windows 11 renews leases in the DHCP Client service, syncs time with `w32time`, and has `ipconfig`, `ping`, `tracert`, `netstat`, `GetAdaptersInfo` and a full Winsock. Linux uses a DHCP client daemon, `chronyd` or `systemd-timesyncd`, `ip`, `ping`, `traceroute` and `ss`, with statistics spread across `/proc/net`. Impossible OS plans the same tools, an in-kernel NTP client instead of a service, and a single `/sys/net` file in place of many `/proc/net` entries.

## See also

- [Time sync, network status and Winsock roadmap](../../todo/07-networking/TODO-06-ntp-status-winsock.md)
- [Time and FILETIME](../kernel/time-filetime.md)
- [DNS Resolver and Sockets](dns-sockets.md)
- [TCP and Network Infrastructure](tcp-network-infrastructure.md)
- [Networking](index.md)
