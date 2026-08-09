---
schema_version: 1
id: ntp-status-winsock
domain: 07-networking
status: active
title: "TODO-06 -- NTP, Network Status & Win32 Winsock"
---

# TODO-06 -- NTP, Network Status & Win32 Winsock

> **Goal:** Close out the kernel networking layer with nine deliverables: DHCP lease renewal daemon, NTP time-sync client (slew mode, Registry persistence), `ifconfig` multi-NIC display + manual config, network statistics API + system-tray icon, extended `ping` (IPv6/flags), `traceroute`, `netstat`, `/sys/net` VFS aggregation file, and `ws2_32.dll` Winsock compatibility stubs. Together these give Impossible OS a complete, observable, Win32-compatible network stack.

> [!IMPORTANT]
> DNS (`dns_resolve()`) and UDP (`udp_send()`) must be working before NTP can query `pool.ntp.org`. The `net_interface` manager from TODO-01 §5 is the foundation for multi-NIC `ifconfig` output and per-interface stats. The BSD socket layer from TODO-02 §3–§8 must be complete before `ws2_32.dll` stubs can map to it. ICMPv6 (TODO-04 §3) provides the Echo Request/Reply types needed by the IPv6 `ping` extension. The `/sys/net` VFS file (§7) aggregates outputs from firewall counters (TODO-05 §9), DNS cache (TODO-02 §4), and ARP/NDP tables (TODO-04 §5) -- implement it last. FILETIME epoch conversion uses the existing `FILETIME` type from `include/kernel/fs/ntfs.h` (100-ns units since 1601-01-01); `uefi_set_time()` is available in `include/kernel/uefi_runtime.h`.

## Inputs

- `src/kernel/net/dhcp.c` -- existing DISCOVER/REQUEST/ACK; add option 51 (lease time) parse + T1/T2 renewal states + renewal daemon thread
- `src/kernel/net/udp.c` + `src/kernel/net/dns.c` -- `udp_send()` + `dns_resolve()` for NTP query to `pool.ntp.org:123`
- `src/kernel/net/icmp.c` -- existing IPv4 ping (`icmp_ping_got_reply`, `icmp_send_echo_request`); extend with ICMPv6 echo and CLI flags
- `src/kernel/net/icmp6.c` -- ICMPv6 Echo Request (type 128) / Echo Reply (type 129) from TODO-04 §3
- `src/kernel/sched/syscall.c` -- `SYS_PING=15` already dispatches to `icmp_send_echo_request()`; extend for IPv6
- `include/kernel/uefi_runtime.h` -- `uefi_set_time()` for hard clock set after NTP response
- `include/kernel/fs/ntfs.h` -- `FILETIME` type (100-ns since 1601); NTP-to-FILETIME conversion utility
- `include/registry.h` -- `RegSetValueEx`/`RegGetValue`/`HKLM` for NTP Registry keys; also firewall and net config keys
- `include/kernel/net/net.h` + `net_interface` from TODO-01 §5 -- per-interface `rx_bytes`/`tx_bytes`/`rx_packets`/`tx_packets`/`rx_errors`/`tx_errors` counter fields
- `src/kernel/fs/sysfs.c` -- `/sys/` VFS mount point for `/sys/net` synthetic file (§8)
- `user/lib/socket.c` + `include/kernel/net/socket.h` -- BSD socket syscalls from TODO-02; `ws2_32.dll` maps to these
- → XREF: `07-networking/TODO-01-tcp-network-infrastructure.md` -- `net_interface` manager (§5) extended with stat counters here
- → XREF: `07-networking/TODO-02-dns-sockets.md` -- `dns_resolve()` for NTP hostname; BSD socket syscalls for `ws2_32.dll` mapping
- → XREF: `07-networking/TODO-04-ipv6-dual-stack.md` -- ICMPv6 Echo Request/Reply (§3) needed by §6 IPv6 ping; NDP cache for `ndp -an` in `/sys/net`
- → XREF: `07-networking/TODO-05-firewall.md` -- `/sys/firewall` counters included in `/sys/net` §8 summary
- → XREF: `02-kernel-core/TODO-17` -- NTP sets system time via `NtSetSystemTime`; coordinate epoch with the kernel time service

## Outcome

- DHCP lease renewal daemon: T1/T2/expiry states, automatic REQUEST renewal, `netif` IP/gateway/DNS updated on renewal.
- `ntp_sync()` queries `pool.ntp.org`, converts NTP timestamp to FILETIME, applies slew (|delta| < 128 ms) or hard set; retries 3×; runs every 24 h.
- `ifconfig` shows all interfaces (MAC, IPv4, IPv6, MTU, RX/TX stats); manual IP/up/down config.
- System tray shows 🌐 connected / ⚠ disconnected icon with IP tooltip; `net_stats()` API used by Task Manager.
- `ping` extended: `-6`/`-4`/`-c N`/`-t`; ICMPv6 Echo Request/Reply for IPv6 targets.
- `traceroute <host>` sends UDP with TTL 1→30, collects ICMP Time Exceeded, prints hop table.
- `netstat` lists TCP connections, UDP sockets, listening sockets from kernel socket table.
- `/sys/net` VFS file: per-interface stats + ARP + NDP + DNS cache + FW hit summary.
- `ws2_32.dll` stub table: `WSAStartup/Cleanup`, `socket/connect/send/recv/bind/listen/accept/closesocket`, `gethostbyname/getaddrinfo`, `WSAGetLastError`, byte-order macros, `setsockopt/select`.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                        | Depends On                                                                        | Status |
| --- | :---: | -------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------- | :----: |
| 💎  |   1   | §9 DHCP renewal daemon -- lease time opt 51, T1/T2 states, background renewal thread               | `dhcp.c` existing ACK state; no new net primitives needed                         |  [ ]   |
| 💎  |   2   | §1 NTP client -- 48-byte packet, `pool.ntp.org:123`, FILETIME convert, slew/hard-set, 24 h thread | `dns_resolve()` + `udp_send()` (TODO-02 §1); `uefi_set_time()` for hard set       |  [ ]   |
| 💎  |   3   | §7 Network statistics API -- per-interface counters in `net_interface`, `net_stats()`, tray icon   | `net_interface` from TODO-01 §5; compositor desktop for tray icon rendering        |  [ ]   |
| 💎  |   4   | §3 `ifconfig` command -- multi-NIC display, IPv4/IPv6/MTU/stats, manual IP/up/down config         | §7 (stats counters must exist to display); `net_interface` manager                |  [ ]   |
| 💎  |   5   | §4 `ping` improvements -- ICMPv6 echo, `-6/-4/-c/-t` flags                                        | ICMPv6 Echo from TODO-04 §3; existing `icmp_send_echo_request()`                  |  [ ]   |
| 💎  |   6   | §5 `traceroute` -- UDP TTL 1→30, ICMP Time Exceeded, reverse DNS, IPv4+IPv6                       | ICMP Time Exceeded handling (IPv4 `icmp.c`); `dns_resolve()` for reverse lookup   |  [ ]   |
| 💎  |   7   | §6 `netstat` -- TCP connections, UDP sockets, listening sockets from kernel socket table           | BSD socket table from TODO-02 §3–§6                                                |  [ ]   |
| ⭐  |   8   | §8 `/sys/net` VFS file -- per-interface stats, ARP, NDP, DNS cache, FW counters                   | §3 stats; §6 `netstat`; TODO-04 §5 NDP cache; TODO-05 §9 FW counters              |  [ ]   |
| 💎  |   9   | §2 `ws2_32.dll` Winsock stubs -- `WSAStartup`, socket/connect/send/recv, `getaddrinfo`, `select`  | BSD socket syscalls from TODO-02 §7; DLL stub table mechanism                      |  [ ]   |

---

## 1. DHCP Renewal Daemon `[Sonnet]`

Parse DHCP option 51 (lease time) in ACK. Derive T1 (50% of lease) and T2 (87.5% of lease). Background thread sends DHCP REQUEST at T1 (unicast to server), sends broadcast REQUEST at T2, triggers full DISCOVER on expiry. Update `netif` IP/gateway/DNS on renewal ACK.

**Files:** `src/kernel/net/dhcp.c` (extend)

> [!NOTE]
> Option 51 parsing: `case 51: if (opt_len==4) dhcp_memcpy(&lease_time_s, &opts[i], 4); lease_time_s = ntohl(lease_time_s);`. DHCP renewal (RFC 2131 §4.4.5): at T1 send unicast DHCP REQUEST to the same server (option 54 = server IP); at T2 send broadcast DHCP REQUEST; on expiry start fresh DISCOVER. The renewal daemon runs as a `task_create("dhcp_renew", dhcp_renew_thread, 0)` kernel thread at SCHED_IDLE priority. The thread sleeps via `ksleep(1000)` (1 s loops) and tracks `uint64_t lease_start_ms`, `uint32_t lease_time_s`, `uint32_t t1_s`, `uint32_t t2_s`. On renewal ACK: update `net_cfg` (or `netif` after TODO-01 §5) IP, gateway, DNS; re-register DNS server.

- [ ] Extend `dhcp_parse_options()`: add `case 51` → store `lease_time_s` in module-level static; add `case 58` (T1 renewal time option) and `case 59` (T2 rebinding time); fall back to 50%/87.5% if options absent
- [ ] `dhcp_state` extension: add states `DHCP_BOUND=3`, `DHCP_RENEWING=4`, `DHCP_REBINDING=5`, `DHCP_EXPIRED=6`
- [ ] On DHCP_ACK: record `lease_start_ms = uptime_ms()`; set `t1_ms = (t1_s ? t1_s : lease_time_s/2) * 1000`; set `t2_ms = (t2_s ? t2_s : lease_time_s*875/1000) * 1000`; set state DHCP_BOUND
- [ ] `dhcp_renew_thread()`: loop every 1 s; check `elapsed = uptime_ms() - lease_start_ms`; at T1: send unicast REQUEST (RENEWING); at T2: send broadcast REQUEST (REBINDING); at expiry: restart DISCOVER; on new ACK: update `netif->ip4`, `netif->gateway`, `netif->dns`; reset T1/T2 timers
- [ ] On renewal ACK: call `dns_set_server(netif->dns)` to update DNS resolver with new DNS IP
- [ ] Log: `[DHCP] Lease T1=%us T2=%us expire=%us`, `[DHCP] Renewing (unicast)`, `[DHCP] Rebinding (broadcast)`, `[DHCP] Renewed IP=%u.%u.%u.%u`
- [ ] Commit: `"net/dhcp: lease renewal daemon -- opt51/58/59, T1 unicast, T2 broadcast, rebind, netif update"`

## 2. NTP Client `[Opus]`

48-byte NTP v4 client packet. Resolve `pool.ntp.org` via DNS → `udp_send()` to port 123. Parse Transmit Timestamp (64-bit NTP fixed-point). Convert to FILETIME epoch. Apply slew mode (|delta| < 128 ms) or hard `uefi_set_time()`. Retry 3×. Background thread syncs at boot + every 24 h. Store last-sync and NTP server in Registry.

**Files:** `src/kernel/net/ntp.c` (new), `include/kernel/net/ntp.h` (new)

> [!NOTE]
> This is `[Opus]` -- NTP timestamp conversion and slew-mode clock adjustment are subtle algorithm design problems with no prior implementation in Impossible OS. **NTP packet** (RFC 5905 §7.3): 48 bytes; byte 0 = `(LI<<6) | (VN<<3) | Mode` = `0x1B` (LI=0, VN=3, Mode=3 client); bytes 1–3 = stratum/poll/precision = 0/6/0xEC; bytes 4–47 = 0. **Transmit Timestamp** offset 40: two uint32_t (big-endian): NTP seconds since 1900-01-01 + fractional seconds. **NTP→FILETIME conversion**: NTP epoch offset = `2208988800` seconds (difference between 1900 and 1970 epochs); FILETIME = (NTP_seconds - 2208988800 + 11644473600) × 10,000,000 (100-ns intervals from 1601). **Slew mode**: if |delta| < 128 ms: adjust kernel tick rate (`tsc_freq_hz += delta_tsc_correction`) -- compute correction as `delta_ms * tsc_freq / 1000` and apply as a linear drift correction factor over 60 s; else: hard `uefi_set_time()`. **DNS for NTP**: `dns_resolve("pool.ntp.org", &ntp_ip)` -- use one NTP server from the pool; rotate to backup `0.pool.ntp.org` on timeout.

- [ ] `struct ntp_packet { uint8_t flags; uint8_t stratum; uint8_t poll; uint8_t precision; uint32_t root_delay; uint32_t root_disp; uint32_t ref_id; uint32_t ref_ts_s, ref_ts_f; uint32_t orig_ts_s, orig_ts_f; uint32_t rx_ts_s, rx_ts_f; uint32_t tx_ts_s, tx_ts_f; } __attribute__((packed));` in `ntp.h`
- [ ] `ntp_build_request(buf)`: set `flags=0x1B`; all other fields 0; 48 bytes
- [ ] `ntp_parse_response(buf, len, &ntp_secs, &ntp_frac)` → 0 or -EINVAL: validate `len >= 48`; extract `tx_ts_s`/`tx_ts_f` (big-endian); `ntp_secs = ntohl(buf->tx_ts_s)`
- [ ] `ntp_to_filetime(ntp_secs)` → `uint64_t filetime`: `filetime = ((uint64_t)(ntp_secs - 2208988800ULL + 11644473600ULL)) * 10000000ULL`
- [ ] `ntp_apply_time(ntp_secs)`: compute `delta_ms` vs current kernel time; if `|delta_ms| < 128`: apply TSC slew (add `delta_tsc = delta_ms * tsc_freq_hz / 1000` to a global drift accumulator applied over 60 s); else: convert to `efi_time` struct; call `uefi_set_time()`; log hard-set
- [ ] `ntp_sync()` → 0 or -errno: `dns_resolve("pool.ntp.org", &ip)`; open UDP reply handler on ephemeral port; `udp_send(ip, ephemeral_port, 123, pkt, 48)`; spin-poll 3 s for reply; on timeout: retry up to 3×; on success: `ntp_apply_time()`; write Registry keys
- [ ] Registry writes: `RegSetValueEx(HKLM, "SYSTEM\\Time\\LastNTPSync", ...)` (DWORD Unix timestamp); `RegSetValueEx(HKLM, "SYSTEM\\Time\\NTPServer", ...)` (SZ "pool.ntp.org"); `RegSetValueEx(HKLM, "SYSTEM\\Time\\TimeZone", ...)` (SZ "UTC")
- [ ] `ntp_sync_thread()`: `ntp_sync()` at boot; `ksleep(86400000)` (24 h); loop
- [ ] Log: `[NTP] Synced to %u.%u.%u.%u: delta=%+d ms (slew=%d)` or `[NTP] Hard set to %04u-%02u-%02u %02u:%02u:%02u UTC`
- [ ] Commit: `"net/ntp: NTP v4 client -- 48-byte packet, pool.ntp.org, FILETIME convert, slew/hard-set, 24h thread"`

## 3. Network Statistics API `[Sonnet]`

Add per-interface RX/TX byte/packet/error counters to `net_interface`. `net_stats(iface, &stats)` API. System-tray icon (🌐 connected / ⚠ disconnected) with IP tooltip. Task Manager "Network" tab hook.

**Files:** `include/kernel/net/net.h` (extend), `src/kernel/net/ip.c` + `udp.c` (extend), `src/desktop/taskbar.c` (extend)

> [!NOTE]
> Counter placement: increment `netif->rx_bytes` and `netif->rx_packets` in `ipv4_handle()` (and `ipv6_receive()` after TODO-04) after header validation; increment `netif->tx_bytes` and `netif->tx_packets` in `eth_send()` (or `ipv4_send()` after payload length is known); increment `netif->rx_errors` on checksum failure or malformed header; increment `netif->tx_errors` if `eth_send()` fails. `net_status_t { uint8_t connected; uint32_t link_speed_mbps; }` -- determine `connected` by checking `netif->ip4 != 0`; `link_speed_mbps` from driver capability (RTL8139 = 100, VirtIO = 1000). Tray icon: rendered in `taskbar.c`; refreshed every 2 s via a timer; tooltip string = `"eth0: 192.168.x.x\nRX: N KB/s  TX: N KB/s"` computed from counter delta.

- [ ] Extend `net_interface` in `net.h`: add `uint64_t rx_bytes, tx_bytes, rx_packets, tx_packets, rx_errors, tx_errors`
- [ ] `net_status(netif, &status)`: set `connected = (netif->ip4 != 0)`; set `link_speed_mbps` from driver (constant 100 for RTL8139, 1000 for VirtIO; stored in `netif->link_speed_mbps`)
- [ ] `net_stats(netif, &stats_out)`: copy counter fields from `net_interface` into caller struct
- [ ] Counter increments: `ip.c` `ipv4_handle()` → `netif->rx_bytes += len; netif->rx_packets++`; `eth_send()` → `netif->tx_bytes += len; netif->tx_packets++`
- [ ] Tray icon in `taskbar.c`: `net_draw_tray_icon()` -- if connected: draw 🌐 glyph (blue globe or bar icon from IRES); if disconnected: draw ⚠ icon; tooltip popup on hover
- [ ] `net_tray_refresh_timer()`: called every 2 s; compute `rx_kbps = (rx_bytes_delta * 1000) / (2000ms)`; update tooltip string
- [ ] Task Manager hook: expose `net_stats()` result in `/sys/net` (§8); Task Manager reads that file for the network graph
- [ ] Commit: `"net/stats: per-interface RX/TX counters, net_stats() API, tray icon with IP tooltip, 2s refresh"`

## 4. `ifconfig` Command `[Sonnet]`

Display all interfaces: name, MAC, IPv4 (addr/mask/gateway), IPv6 (link-local + global), MTU, RX/TX bytes/packets/errors, up/down state. `ifconfig <iface> up|down|<ip> <mask>` for manual configuration.

**Files:** `src/shell/cmd_ifconfig.c` (new or extend from TODO-04 §10)

> [!NOTE]
> `ifconfig` with no arguments: iterate all registered `net_interface` entries; for each: print `<name>: flags=<UP|DOWN> mtu=<MTU>`; print `ether <MAC>` on the next line; print `inet <ip> netmask <mask> broadcast <bcast>` if IPv4 configured; print `inet6 <fe80::link-local>/10` and `inet6 <global>/<prefix>` if IPv6 configured (from TODO-04 §6/§8); print `RX packets <n> bytes <n> errors <n>` and `TX packets <n> bytes <n> errors <n>`. Manual config: `ifconfig eth0 192.168.1.10 255.255.255.0` → set `netif->ip4` + `netif->subnet`; `ifconfig eth0 up/down` → set `netif->flags`. This extends the `ifconfig` stub started in TODO-04 §10 -- if that section is complete, this section only adds stats rows and manual config.

- [ ] `ifconfig_print_iface(netif)`: format all rows as described above; use `%d.%d.%d.%d` IPv4 formatting; `ipv6_ntop()` for IPv6 (from TODO-04 §10)
- [ ] `cmd_ifconfig(argc, argv)`: no args → iterate `netif_all()` + print each; with args: parse `<iface> <verb> [params]`
- [ ] `ifconfig_set_ip(netif, ip4, subnet)`: write `netif->ip4`, `netif->subnet`; recompute broadcast; log change
- [ ] `ifconfig_set_state(netif, up)`: set `netif->flags |= NETIF_UP` or clear; when brought down: flush ARP cache for iface
- [ ] Broadcast address: `netif->ip4 | ~netif->subnet` (IPv4 host part all-ones)
- [ ] Register `ifconfig` in shell command table (or extend registration from TODO-04)
- [ ] Commit: `"shell/ifconfig: multi-NIC display with stats, IPv4/IPv6, RX/TX counters, manual IP/up/down config"`

## 5. Extended `ping` Command `[Sonnet]`

Add ICMPv6 Echo Request (type 128) / Echo Reply (type 129) for IPv6 targets. Add flags: `-6` (force IPv6), `-4` (force IPv4), `-c count` (loop N times), `-t` (continuous until Ctrl+C).

**Files:** `src/shell/cmd_ping.c` (extend), `src/kernel/net/icmp6.c` (extend from TODO-04)

> [!NOTE]
> Existing `SYS_PING=15` syscall calls `icmp_send_echo_request(ip4)` and polls `icmp_ping_got_reply()`. Extension: add `icmp6_send_echo_request(ip6, seq, id)` that builds an ICMPv6 type-128 packet (8-byte header: type=128, code=0, checksum, id, seq) with ICMPv6 pseudo-header checksum; add `icmp6_ping_got_reply()` state (analogous to IPv4). `cmd_ping` parses flags first: if `-6` or target hostname resolves to IPv6 only: use ICMPv6; if `-4` or target is IPv4: use ICMPv4; if both available and no flag: prefer IPv4 (safe default). `-c N`: loop N times with 1 s sleep between; print `NNN bytes from IP: icmp_seq=N ttl=T time=X ms` each. `-t`: loop until `ctrl_c_pending` flag set by keyboard handler (same mechanism as Ctrl+C in shell).

- [ ] `icmp6_send_echo_request(ip6, id, seq)`: build ICMPv6 header (type=128, code=0) + 8-byte payload (id + seq + zeros); compute checksum with `icmp6_checksum(src6, dst6, payload, len)`; `ipv6_send(ip6, IPPROTO_ICMPV6, ...)`
- [ ] `icmp6_handle()` (from TODO-04 §3): add `case 129` (Echo Reply): set `icmp6_ping_got_reply=1`; store `reply_src6` + `seq`
- [ ] `icmp6_ping_got_reply()` / `icmp6_ping_clear_reply()` accessors
- [ ] `cmd_ping` rewrite: parse flags `{-6, -4, -c, -t}`; resolve target (dns_resolve_dual); send ICMPv4 or ICMPv6 echo; print RTT using `uptime_ms()` delta; print summary `N packets transmitted, N received, N% loss` at end
- [ ] RTT measurement: record `uint64_t send_ms = uptime_ms()` before echo send; compute `rtt_ms = uptime_ms() - send_ms` on reply
- [ ] Commit: `"shell/ping: ICMPv6 echo, -6/-4/-c count/-t continuous flags, RTT per-packet, packet-loss summary"`

## 6. `traceroute` Command `[Opus]`

Send UDP packets with TTL 1→30. Collect ICMP Time Exceeded (type 11) responses. Reverse-DNS-lookup each hop IP. Print hop table with RTT. IPv4 and IPv6 modes.

**Files:** `src/shell/cmd_traceroute.c` (new), `src/kernel/net/icmp.c` (extend)

> [!NOTE]
> This is `[Opus]` -- `traceroute` requires a novel mechanism: deliberate TTL-expiry probing and correlation of asynchronous ICMP Time Exceeded replies back to the correct probe sequence. Implementation: for each TTL from 1 to 30: set `IP TTL` field in the IPv4 header (extend `ipv4_send()` to accept a TTL parameter; currently it hard-codes TTL=64); send 3 UDP probes to an unlikely destination port (33434–33464, as per traditional `traceroute`); register an ICMP Time Exceeded (type=11, code=0) handler that captures the source IP of the ICMP response; correlate by sequence (use a static `tracert_pending_ip` and `tracert_pending_ttl`); wait up to 2 s per hop; if no reply: print `* * *`. IPv4 only for the initial implementation; IPv6 uses ICMPv6 Time Exceeded (type=3) as a stub that prints `[IPv6 traceroute not yet implemented]`.

- [ ] Extend `ipv4_send()`: add `uint8_t ttl` parameter (use `IPV4_TTL_DEFAULT=64` for all existing callers via a compatibility macro); set `hdr->ttl = ttl` in the IPv4 header build
- [ ] `icmp_set_ttl_exceeded_cb(cb)`: register a callback invoked by `icmp_handle()` when ICMP type=11 received; callback receives `src_ip` (the router that sent the TTL-exceeded), `orig_dest_ip`, and `orig_ttl`
- [ ] `traceroute_probe(dst_ip, ttl, &reply_ip, &rtt_ms)` → 0 (reply) or -ETIMEDOUT: set `tracert_pending_ttl=ttl`; send 3 UDP datagrams to `dst_ip` port 33434 with TTL=ttl via extended `ipv4_send()`; wait up to 2 s for ICMP Time Exceeded callback; on receipt: store `reply_ip`; record RTT
- [ ] `cmd_traceroute(argc, argv)`: parse hostname → `dns_resolve()`; print header `traceroute to <host> (<ip>), 30 hops max`; loop TTL 1→30: call `traceroute_probe()` × 3; print `<ttl>  <rtt1> ms  <rtt2> ms  <rtt3> ms  <ip> (hostname)` (reverse DNS via `dns_resolve_reverse()` stub); on target reached (ICMP Port Unreachable type=3 from dst_ip itself): stop
- [ ] `dns_resolve_reverse(ip, hostname_out)` stub: format `N.M.L.K.in-addr.arpa`; send PTR query type=12; return hostname or dotted-decimal if no PTR record
- [ ] Register `traceroute` (and alias `tracert`) in shell command table
- [ ] Commit: `"shell/traceroute: TTL probe, ICMP Time Exceeded cb, per-hop RTT×3, reverse DNS, tracert alias"`

## 7. `netstat` Command `[Sonnet]`

List all active TCP connections (local addr:port, remote addr:port, TCP state), bound UDP sockets, and listening TCP sockets. Reads from the kernel socket table.

**Files:** `src/shell/cmd_netstat.c` (new), `include/kernel/net/socket.h` (extend)

> [!NOTE]
> `netstat` reads `sock_table[]` from `src/kernel/net/socket.c` (the 64-entry per-process socket table from TODO-02 §3). Output columns: `Proto | Local Address | Foreign Address | State`. TCP states mapped to string: `LISTEN`, `ESTABLISHED`, `SYN_SENT`, `SYN_RECEIVED`, `CLOSE_WAIT`, `TIME_WAIT`, `CLOSED`. UDP sockets show `State=UDP` with local addr/port and `0.0.0.0:*` for foreign. Listening sockets: `SOCK_STREAM` in `SOCK_LISTENING` state from TODO-02 §6. `-n` flag: suppress reverse DNS (print raw IPs). `-a` flag: include LISTEN sockets (default off). Addresses: format `W.X.Y.Z:port` for IPv4; `[ip6]:port` for IPv6 (if TODO-04 complete).

- [ ] `netstat_get_entries(buf, max)` → count: iterate `sock_table[0..63]`; for each `used` entry: fill `netstat_entry_t { char proto[4]; char local[48]; char foreign[48]; char state[16]; }`
- [ ] TCP state string map: `tcp_state_name(state)` → `"ESTABLISHED"` / `"LISTEN"` etc. from `tcp_connection.state`
- [ ] `cmd_netstat(argc, argv)`: parse `-n` (no DNS) and `-a` (include LISTEN) flags; call `netstat_get_entries()`; print table
- [ ] Header: `Proto  Local Address          Foreign Address        State`
- [ ] Expose `sock_table` read access: add `sock_iterate(cb, userdata)` function in `socket.c` to iterate without exposing the array directly; `cmd_netstat` uses this
- [ ] Register `netstat` in shell command table
- [ ] Commit: `"shell/netstat: TCP/UDP socket table display, state strings, -n -a flags, sock_iterate API"`

## 8. `/sys/net` VFS Aggregation File `[Sonnet]`

Read-only synthetic VFS file at `/sys/net`. On read: emit per-interface stats, ARP table, NDP neighbor cache, DNS cache contents, and firewall hit counter summary.

**Files:** `src/kernel/fs/sysfs.c` (extend)

> [!NOTE]
> `/sys/net` is a read-only synthetic file (same pattern as `/sys/firewall` from TODO-05 §9). `net_sysfs_dump(buf, max)` function: (1) per-interface block: name, IP, link speed, `rx_bytes`/`tx_bytes`/`rx_packets`/`tx_packets`/`rx_errors`/`tx_errors` (from §2 counters); (2) ARP table: iterate `arp_cache[]` (from `src/kernel/net/arp.c`); emit `IP → MAC (age ms)`; (3) NDP neighbor cache: iterate `ndp_cache[]` (TODO-04 §5); emit IPv6 → MAC + state; (4) DNS cache: call `dns_cache_dump(buf_ptr, remaining)` (add this helper to `dns.c`); (5) firewall summary: call `fw_dump_stats(buf_ptr, remaining)` (already in TODO-05 §9). Sections separated by `\n=== <Section> ===\n` headers.

- [ ] `net_sysfs_dump(buf, max)` → bytes: write all five sections in order with section headers
- [ ] `dns_cache_dump(buf, max)` → bytes: add to `dns.c`; format `hostname → W.X.Y.Z (TTL=Ns)` per entry
- [ ] `arp_cache_dump(buf, max)` → bytes: add to `arp.c`; format `W.X.Y.Z → AA:BB:CC:DD:EE:FF (Nms old)` per entry
- [ ] `ndp_cache_dump(buf, max)` → bytes: add to `ndp_cache.c` (TODO-04 §5); format `ip6 → MAC (state)` per entry; skip if IPv6 not yet implemented (return 0)
- [ ] Register `/sys/net`: `sysfs_register("net", net_sysfs_read_cb, NULL)`; `net_sysfs_read_cb` calls `net_sysfs_dump()`
- [ ] `cat /sys/net` accessible from shell; also readable by Task Manager network graph
- [ ] Commit: `"sysfs: /sys/net -- per-interface stats, ARP, NDP, DNS cache, firewall summary in one VFS file"`

## 9. `ws2_32.dll` Winsock Stubs `[Sonnet]`

`WSAStartup`/`WSACleanup` no-ops. `socket`/`connect`/`send`/`recv`/`closesocket`/`bind`/`listen`/`accept` map to BSD socket syscalls. `gethostbyname`/`getaddrinfo` wrap `dns_resolve`. `WSAGetLastError` maps `errno` to Winsock error codes. `htons`/`ntohs`/`htonl`/`ntohl`. `setsockopt`/`getsockopt`. `select`. Register in the built-in DLL stub table.

**Files:** `src/kernel/sched/dll_stubs.c` (new or extend), `include/kernel/sched/dll_stubs.h` (new or extend)

> [!NOTE]
> `ws2_32.dll` is the Win32 Winsock DLL. For user-mode programs that import from `ws2_32.dll`, the PE loader's IAT resolver must find these exports. Approach: the built-in DLL stub table (already planned for Win32 compatibility) maps `dll_name + export_name → function_ptr`. `WSAStartup(wVersionRequired, lpWSAData)`: write `wVersion=0x0202` to `lpWSAData`; return 0. `WSACleanup()`: return 0. `socket(af, type, protocol)`: call `kern_socket()` via `SYS_SOCKET` syscall; return fd. `closesocket(fd)`: map to `SYS_CLOSE(fd)` (add `SYS_CLOSE` syscall if not present -- close a socket fd). `WSAGetLastError()`: read thread-local `wsa_last_error`; map from `errno`: `ECONNREFUSED→WSAECONNREFUSED=10061`, `ETIMEDOUT→WSAETIMEDOUT=10060`, `EADDRINUSE→WSAEADDRINUSE=10048`, `ENOTSUP→WSAEOPNOTSUPP=10045`, etc. `gethostbyname(name)`: call `dns_resolve(name, &ip)`; fill `hostent` struct (static): `h_name=name`, `h_addrtype=AF_INET`, `h_length=4`, `h_addr_list[0]=&ip`.

- [ ] `dll_stub_register("ws2_32.dll", export_name, fn_ptr)` mechanism: add to IAT resolver lookup table; if not present yet, create it
- [ ] `WSAStartup`, `WSACleanup` → register as no-ops returning 0
- [ ] `socket`, `connect`, `send`, `recv`, `bind`, `listen`, `accept` → thin wrappers calling `syscall(SYS_SOCKET/SYS_CONNECT/SYS_SEND/SYS_RECV/SYS_BIND/SYS_LISTEN/SYS_ACCEPT, ...)`
- [ ] `closesocket` → `syscall(SYS_CLOSE, fd)` (add `SYS_CLOSE=50` if not yet defined)
- [ ] `setsockopt`, `getsockopt` → `syscall(SYS_SETSOCKOPT/SYS_GETSOCKOPT, ...)`
- [ ] `select` → `syscall(SYS_SELECT, ...)`
- [ ] `gethostbyname(name)` → `dns_resolve(name, &ip)`; fill static `hostent`; return pointer
- [ ] `getaddrinfo(node, service, hints, &res)` → `syscall(SYS_DNS, ...)`; build `addrinfo` linked list
- [ ] `freeaddrinfo(res)` → no-op (static allocation) or `kfree()` chain
- [ ] `WSAGetLastError()` → read `wsa_errno`; `WSASetLastError(e)` → set `wsa_errno`; errno mapping table (10 common codes)
- [ ] `htons`/`ntohs`/`htonl`/`ntohl` → inline byte-swap (already in `user/include/socket.h` from TODO-02; re-export from `ws2_32.dll`)
- [ ] `inet_addr(str)` → parse dotted-decimal to uint32_t; `inet_ntoa(addr)` → static buffer formatted string
- [ ] Register all exports in DLL stub table; test with a user-mode program that calls `WSAStartup` → `socket` → `connect` → `send` → `recv` → `closesocket`
- [ ] Commit: `"win32/ws2_32: Winsock stubs -- WSAStartup, socket/connect/send/recv, getaddrinfo, WSAGetLastError"`

---

## OS Comparison


| ⭐  | Feature                                                  | 🪟 Win11                                             | 🐧 Linux                                                               | 🚀 Impossible OS                                  |
| --- | -------------------------------------------------------- | ---------------------------------------------------- | ---------------------------------------------------------------------- | ------------------------------------------------- |
| 💎  | DHCP lease renewal                                       | ✅ `dhcpcsvc.dll` T1/T2 RFC 2131-compliant renewal;  | ✅ `dhclient`/`systemd-networkd` T1/T2; in-kernel DHCP not             | ⬜ §1 -- in-kernel daemon; `ksleep` 1 s           |
| 💎  | NTP v4 client                                            | ✅ `w32tm.exe` Windows Time Service; slew            | ✅ `ntpd`/`chronyd`/`systemd-timesyncd` (userspace); `adjtimex()` slew | ⬜ §2 -- `⭐` in-kernel (no userspace daemon)     |
| 💎  | `ifconfig` multi-NIC, RX/TX stats, manual IP/up/down     | ✅ `ipconfig /all`; `netsh interface ip              | ✅ `ifconfig` + `ip link`/`ip addr`;                                   | ⬜ §4 -- extends TODO-04 §10; adds stats          |
| 💎  | Network stats API + system-tray icon + Task Manager hook | ✅ `GetAdaptersInfo()`; tray in `explorer.exe`; Task | ✅ `/proc/net/dev`; `NetworkManager` tray; `gnome-task-manager`        | ⬜ §3 -- `net_stats()` kernel API; tray icon      |
| 💎  | `ping` IPv6 ICMPv6 echo, `-6/-4/-c/-t` flags             | ✅ `ping -6 <host>`; `-n count`;                     | ✅ `ping6`/`ping -6`; `-c count`; `-i                                  | ⬜ §5 -- ICMPv6 type-128 echo; unified `ping`     |
| 💎  | `traceroute`                                             | ✅ `tracert.exe` (ICMP-based, not UDP)               | ✅ `traceroute` (UDP-based by default); `tracepath`;                   | ⬜ §6 -- UDP-based; ICMP Time Exceeded cb         |
| 💎  | `netstat`                                                | ✅ `netstat.exe`; `-a/-n/-o/-p` flags                | ✅ `netstat`/`ss`; reads `/proc/net/tcp6`                              | ⬜ §7 -- reads kernel `sock_table`; `-n/-a` flags |
| ⭐  | `/sys/net`                                               | ✅ `netsh`, `ipconfig`, `arp -a` (separate           | ✅ `/proc/net/dev`, `/proc/net/arp`, `/proc/net/tcp` (separate files)  | ⬜ §8 -- `⭐` single `/sys/net` file aggregates   |
| 💎  | `ws2_32.dll` Winsock stubs                               | ✅ Full `ws2_32.dll` (kernel `afd.sys` +             | ✅ glibc `socket()`/`connect()`; no `ws2_32.dll` (POSIX                | ⬜ §9 -- DLL stub table maps Win32                |

> **After §1–§9:** The networking layer is fully closed out. Impossible OS has an in-kernel NTP client without a userspace daemon (`⭐`), a unified `/sys/net` observability file (`⭐`), and `ws2_32.dll` Winsock stubs that let Win32 applications use the network stack without modification. Every higher-level networking TODO (browser, email, SSH, OS updates) is unblocked.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] DHCP renewal: set `lease_time_s=10` in QEMU DHCP server; kernel logs `[DHCP] Renewing (unicast)` after 5 s; `[DHCP] Renewed IP=...` after ACK
- [ ] NTP: boot log shows `[NTP] Synced to X.X.X.X: delta=+Nms (slew=1)` or hard-set; Registry key `HKLM\SYSTEM\Time\LastNTPSync` readable after sync
- [ ] `ifconfig` (no args): shows `eth0` with MAC, `inet 192.168.x.x`, `inet6 fe80::...`, `RX packets N bytes N`; `ifconfig eth0 192.168.2.50 255.255.255.0` → `ifconfig` shows new IP
- [ ] Tray icon: after boot shows 🌐 icon; hover shows tooltip with IP + KB/s; disconnect QEMU NIC → icon changes to ⚠
- [ ] `net_stats(netif, &s)`: after `http_get()`: `s.rx_bytes > 0 && s.tx_bytes > 0`
- [ ] `ping -6 ::1` → `64 bytes from ::1: icmp_seq=0 time=Nms`; `ping -c 3 8.8.8.8` → 3 lines + `3 packets transmitted, 3 received, 0% loss`; `ping -t 8.8.8.8` → continuous until Ctrl+C
- [ ] `traceroute google.com` → at least 3 hops printed with RTT and hop IP; `*` for unresponsive hops
- [ ] `netstat -a`: shows at least one LISTEN entry after `kern_listen()`; shows ESTABLISHED after connecting TCP socket
- [ ] `cat /sys/net`: readable; shows 5 section headers; interface counters non-zero after network activity; ARP table shows QEMU router
- [ ] `ws2_32.dll`: user-mode test program calling `WSAStartup(0x0202, &wsa)` returns 0; `socket(AF_INET, SOCK_STREAM, 0)` returns fd ≥ 3; `connect` + `send` + `recv` + `closesocket` completes an HTTP GET
- [ ] Commit: `"net: close networking layer -- DHCP renewal, NTP, ifconfig++, ping6, traceroute, netstat, /sys/net, ws2_32"`
