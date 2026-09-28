---
schema_version: 1
id: ipv6-dual-stack
domain: 07-networking
status: active
title: "TODO-04 -- IPv6 Dual-Stack"
---

# TODO-04 -- IPv6 Dual-Stack

> **Goal:** Add full IPv6 support alongside IPv4: IPv6 header + ethertype routing, `ipv6_send()`/`ipv6_receive()`, ICMPv6 Neighbor Discovery Protocol (NDP) with NS/NA/RS/RA, EUI-64 link-local autoconfiguration, SLAAC global address, DHCPv6 client, dual-stack socket API (`AF_INET6`), DNS AAAA query activation, NDP neighbor cache with STALE/PROBE/FAILED state machine, and `ifconfig` IPv6 display. IPv6 is a production requirement -- many corporate and mobile networks are IPv6-only, and all modern OSes (Windows, Linux, macOS, Android) support dual-stack by default.

> [!IMPORTANT]
> IPv4 (`ipv4_send`/`ipv4_handle`), ARP, TCP, UDP, and DNS are complete prerequisites. The `net_rx()` switch in `src/kernel/net/ethernet.c` already dispatches on `ethertype`; add `case 0x86DD` without touching IPv4 dispatch. The DNS AAAA stub from TODO-02 §4 (`dns_resolve6()` returning -ENOTSUP) is activated here in §8 -- do not re-implement, only wire. The dual-stack socket API in §9 extends the socket layer from TODO-02 §5; do not duplicate socket fd table logic. NDP neighbor cache (§3) must exist before SLAAC (§6) and DHCPv6 (§7) can register addresses with a reachable gateway.

## Inputs

- `src/kernel/net/ethernet.c` -- `eth_send()` + ethertype switch; add `case 0x86DD: ipv6_receive()`
- `src/kernel/net/ip.c` -- IPv4 reference for header format conventions; IPv6 goes in new `src/kernel/net/ip6.c`
- `src/kernel/net/udp.c` -- `udp_send()`/`udp_receive()`; DHCPv6 uses UDP ports 546/547 over IPv6
- `src/kernel/net/icmp.c` -- ICMPv4 reference; ICMPv6 is a new `src/kernel/net/icmp6.c`
- `include/kernel/net/net.h` -- add IPv6 constants, `struct ipv6_header`, NDP structs, extend `net_interface` from TODO-01 §5 with `ip6_local[16]` (link-local) and `ip6_global[16]` (SLAAC/DHCPv6)
- `src/kernel/net/socket.c` + `include/kernel/net/socket.h` -- extend with `AF_INET6`, `struct sockaddr_in6`, `IPV6_V6ONLY` from TODO-02 §5 socket layer
- `src/kernel/net/dns.c` -- activate `dns_resolve6()` AAAA stub (from TODO-02 §4); wire AAAA + A dual query into `dns_resolve_dual()`
- → XREF: `07-networking/TODO-01-tcp-network-infrastructure.md` -- `net_interface` manager (§5) needs `ip6_local`/`ip6_global` fields added here
- → XREF: `07-networking/TODO-02-dns-sockets.md` -- DNS AAAA stub (§4) and socket layer (§5-§8) are direct prerequisites and extension points
- → XREF: `07-networking/TODO-03-http-tls.md` -- `https_get()`/`https_post()` will work over IPv6 once §9 dual-stack sockets are in place (no code change to HTTP layer needed)

## Outcome

- `ipv6_send()` and `ipv6_receive()` work for TCP/UDP/ICMPv6 payloads.
- NDP NS/NA resolves IPv6 addresses to MACs; RS/RA discovers the default gateway.
- Boot-time EUI-64 link-local `fe80::/10` address assigned and logged.
- SLAAC assigns a global address from RA prefix; default gateway and DNS set from RA.
- DHCPv6 client obtains an IA_NA address + DNS server as fallback to SLAAC.
- `AF_INET6` sockets: `connect(fd, ::1, port)` and `bind(fd, ::, port)` work.
- `dns_resolve()` sends both A + AAAA and prefers AAAA results when available.
- NDP neighbor cache: 128 entries, REACHABLE→STALE→PROBE→FAILED lifecycle.
- `ifconfig` shows link-local + global IPv6 addresses; `ndp -an` prints neighbor cache.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                   | Depends On                                                               | Status |
| --- | :---: | --------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------ | :----: |
| 💎  |   1   | §1 IPv6 header -- `struct ipv6_header`, constants, ethertype 0x86DD routing                  | `ethernet.c` ethertype switch (exists); no other net code needed         |  [ ]   |
| 💎  |   2   | §2 IPv6 send/receive -- `ipv6_send()` + `ipv6_receive()` dispatch to TCP/UDP/ICMPv6          | §1 (header struct must exist)                                            |  [ ]   |
| 💎  |   3   | §3 ICMPv6 -- NS/NA (replace ARP for IPv6), RS/RA; `ndp_resolve()` basic send/receive        | §2 (IPv6 send/receive must work); §1 constants                           |  [ ]   |
| 💎  |   4   | §4 Link-local EUI-64 -- `fe80::/10` from MAC, boot-time register with `netif`                | §3 (NS/NA needed to confirm reachability on link)                        |  [ ]   |
| 💎  |   5   | §5 NDP neighbor cache -- 128-entry table, REACHABLE/STALE/PROBE/FAILED state machine         | §3 (NS/NA primitives); §4 (link-local address as NS source)              |  [ ]   |
| 💎  |   6   | §6 SLAAC -- RA prefix + EUI-64 suffix → global address, default gateway, RDNSS              | §4 (link-local must be set before RS is sent); §5 (cache stores gateway) |  [ ]   |
| 💎  |   7   | §7 DHCPv6 client -- Solicit→Advertise→Request→Reply, IA_NA + DNS, UDP 546/547               | §4 (link-local source address for DHCPv6 Solicit); §6 attempted first   |  [ ]   |
| 💎  |   8   | §8 DNS AAAA activation -- wire `dns_resolve_dual()`, prefer AAAA, activate `-ENOTSUP` stub  | §6 or §7 (global IPv6 address required to use AAAA results)              |  [ ]   |
| 💎  |   9   | §9 Dual-stack socket API -- `AF_INET6`, `struct sockaddr_in6`, `IPV6_V6ONLY`, `::` listen   | §2 (`ipv6_send()`); §8 (`getaddrinfo` AAAA preference); TODO-02 sockets  |  [ ]   |
| 💎  |  10   | §10 `ifconfig` IPv6 display -- show addresses, `ndp -an` neighbor table, `::/0` route       | §4–§7 (addresses must exist to display); §5 (neighbor cache readable)    |  [ ]   |

---

## 1. IPv6 Header + Ethertype Routing `[Sonnet]`

Define `struct ipv6_header`. Add IPv6 constants. Route `ethertype 0x86DD` frames to `ipv6_receive()` in `net_rx()`.

**Files:** `include/kernel/net/net.h` (extend), `src/kernel/net/ethernet.c` (extend)

> [!NOTE]
> IPv6 header layout (40 bytes fixed, RFC 8200): `version:4 | traffic_class:8 | flow_label:20 | payload_length:16 | next_header:8 | hop_limit:8 | src[16] | dst[16]`. **No checksum in the IPv6 header** -- unlike IPv4. `next_header` carries the same protocol values as IPv4 `protocol` field: `IPPROTO_TCP=6`, `IPPROTO_UDP=17`, `IPPROTO_ICMPV6=58`. `flow_label` stored big-endian; treated as opaque -- set to 0 on send. The ethertype field is big-endian in the Ethernet frame; use `ntohs()` before comparing. Extension headers (hop-by-hop, routing, fragment) are not needed at this stage -- skip any packet with a next_header value other than 6/17/58 and log a warning.

- [ ] `struct ipv6_header { uint32_t ver_tc_fl; uint16_t payload_len; uint8_t next_header; uint8_t hop_limit; uint8_t src[16]; uint8_t dst[16]; } __attribute__((packed));` in `net.h`
- [ ] Constants in `net.h`: `ETHERTYPE_IPV6=0x86DD`, `IPPROTO_ICMPV6=58`, `IPV6_HOP_LIMIT_DEFAULT=64`
- [ ] Helper macros: `IPV6_VERSION(h)` = `(ntohl((h)->ver_tc_fl) >> 28)`; `IPV6_FLOW(h)` = `(ntohl((h)->ver_tc_fl) & 0xFFFFF)`
- [ ] `ipv6_addr_equal(a, b)` inline: `memcmp(a, b, 16) == 0`; `ipv6_addr_is_zero(a)` inline
- [ ] `ethernet.c`: add `case ETHERTYPE_IPV6: ipv6_receive(data + sizeof(eth_header), len - sizeof(eth_header)); break;` in the ethertype switch
- [ ] Log: `[IPv6] Received ethertype 0x86DD len=%u next_hdr=%u`
- [ ] Commit: `"net/ipv6: ipv6_header struct, ETHERTYPE_IPV6, IPPROTO_ICMPV6, ethertype dispatch"`

## 2. IPv6 Send / Receive `[Sonnet]`

`ipv6_send(dst[16], next_header, payload, len)` builds the 40-byte IPv6 header and calls `eth_send()`. `ipv6_receive(packet, len)` dispatches to TCP/UDP/ICMPv6 by `next_header`.

**Files:** `src/kernel/net/ip6.c` (new), `include/kernel/net/net.h` (extend)

> [!NOTE]
> `ipv6_send()` source address selection: use `netif->ip6_global` if non-zero and destination is not link-local; else use `netif->ip6_local`. If both are zero (no address configured yet), use the unspecified address `::` -- valid only for DAD/RS before address assignment. Destination MAC for `eth_send()`: if destination is a multicast IPv6 address (first byte `0xFF`), the Ethernet multicast MAC is derived as `33:33:XX:XX:XX:XX` where XX:XX:XX:XX are the last 4 bytes of the IPv6 destination. Otherwise: look up the destination in the NDP neighbor cache (§5) for its MAC; if not found: send NS (§3) and drop the packet (caller retries). `ipv6_receive()` must handle extension headers gracefully: skip bytes until a known `next_header` (6/17/58) or an unknown type (log + drop).

- [ ] Extend `net_interface` (from TODO-01 §5) in `net.h`: add `uint8_t ip6_local[16]; uint8_t ip6_global[16]; uint8_t ip6_prefix_len;`
- [ ] `ipv6_multicast_mac(dst_ipv6, mac_out)`: derive `33:33` + last 4 bytes of IPv6 dst
- [ ] `ipv6_send(const uint8_t dst[16], uint8_t next_header, const void *payload, uint16_t len)`: source addr select; fill header (ver=6, tc=0, flow=0, hop=64); `eth_send(dst_mac, ETHERTYPE_IPV6, ...)`
- [ ] `ipv6_receive(const void *data, uint32_t len)`: validate `IPV6_VERSION(hdr) == 6`; check `payload_len`; dispatch: `case IPPROTO_TCP: tcp_handle6()`; `case IPPROTO_UDP: udp_handle6()`; `case IPPROTO_ICMPV6: icmp6_handle()`; unknown: log warn + drop
- [ ] `tcp_handle6()` + `udp_handle6()` stubs: forward to existing `tcp_handle()`/`udp_handle()` with IPv6 source address stored in per-connection state (extend `tcp_connection` with `is_ipv6` flag + `remote_ip6[16]`)
- [ ] Log: `[IPv6] Send dst=%x..%x next=%u len=%u` and `[IPv6] Receive src=%x..%x next=%u`
- [ ] Commit: `"net/ipv6: ipv6_send + ipv6_receive, multicast MAC derive, tcp/udp/icmp6 dispatch"`

## 3. ICMPv6 -- Neighbor Discovery (NS / NA / RS / RA) `[Opus]`

Implement ICMPv6 type 135 (Neighbor Solicitation) and type 136 (Neighbor Advertisement) to replace ARP for IPv6. Implement RS (type 133) and RA (type 134) for gateway discovery. `ndp_resolve(ipv6)` sends NS and waits for NA.

**Files:** `src/kernel/net/icmp6.c` (new), `include/kernel/net/net.h` (extend)

> [!NOTE]
> This is `[Opus]` -- NDP is novel (no prior implementation in Impossible OS); NS/NA and RS/RA require careful multicast addressing, option parsing, and the interaction between address assignment and link-local state. **NS format** (RFC 4861 §4.3): ICMPv6 type=135, code=0, checksum, reserved (4 bytes), target address (16 bytes), options (Source Link-Layer Address option: type=1, len=1, MAC). Send NS to the **solicited-node multicast** address `ff02::1:ffXX:XXXX` (last 24 bits of target). **NA format** (type=136): target address, flags (R/S/O bits), options (Target Link-Layer Address type=2). **ICMPv6 checksum**: pseudo-header = src[16] + dst[16] + upper_layer_length (4 bytes BE) + zeros (3 bytes) + next_header=58 (1 byte); same RFC 1071 algorithm as UDP. **RS**: type=133, send to `ff02::2` (all routers), source = link-local. **RA** receipt: parse Prefix Information option (type=3) for SLAAC prefix, RDNSS option (type=25) for DNS; store in `netif`.

- [ ] `struct icmp6_header { uint8_t type; uint8_t code; uint16_t checksum; }` in `net.h`
- [ ] `struct ndp_ns { struct icmp6_header hdr; uint32_t reserved; uint8_t target[16]; uint8_t options[]; }` and `struct ndp_na { ... uint32_t flags; uint8_t target[16]; ... }` (packed)
- [ ] `icmp6_checksum(src6, dst6, payload, len)` → uint16_t: pseudo-header checksum using RFC 1071 algorithm with next_header=58
- [ ] `ndp_send_ns(target_ip6)`: build NS to solicited-node multicast; include SLLA option; `ipv6_send(solicit_mcast, IPPROTO_ICMPV6, ...)`
- [ ] `ndp_send_na(target_ip6, dst_ip6, dst_mac)`: build NA with S+O flags; include TLLA option; `ipv6_send(dst_ip6, IPPROTO_ICMPV6, ...)`
- [ ] `ndp_send_rs()`: send RS to `ff02::2`; include SLLA option; log `[NDP] Sent RS`
- [ ] `icmp6_handle(src6, data, len)`: dispatch by type: 135 → respond with NA if target is our address; 136 → update NDP cache; 134 → parse RA (prefix + RDNSS); other: log + drop
- [ ] `ndp_resolve(target_ip6, mac_out)` → 0 or -ETIMEDOUT: check neighbor cache; on miss: `ndp_send_ns(target_ip6)`; spin-poll cache 3 s with 1 ms sleep; return MAC or -ETIMEDOUT
- [ ] Log: `[NDP] NS → %x..%x`, `[NDP] NA from %x..%x mac=%02x:%02x:...`, `[NDP] RA from %x..%x prefix=%x..%x/N`
- [ ] Commit: `"net/icmp6: ICMPv6 NS/NA/RS/RA, ndp_resolve(), solicited-node multicast, icmp6 checksum"`

## 4. Link-Local EUI-64 Autoconfiguration `[Sonnet]`

Derive a `fe80::/10` link-local address from the interface MAC using EUI-64. Register with `netif` at boot time. Use as the source address for NDP and DHCPv6.

**Files:** `src/kernel/net/ip6.c` (extend)

> [!NOTE]
> EUI-64 derivation (RFC 4291 §2.5.1): take the 6-byte MAC `AA:BB:CC:DD:EE:FF`; insert `0xFF 0xFE` between bytes 3 and 4 to get 8 bytes `AA BB CC FF FE DD EE FF`; **flip bit 6** of byte 0 (`AA ^ 0x02`); the link-local address is `fe80::` + these 8 bytes. Example: MAC `52:54:00:12:34:56` → EUI-64 `50:54:00:FF:FE:12:34:56` → link-local `fe80::5054:00ff:fe12:3456`. Duplicate Address Detection (DAD): before marking the address confirmed, send one NS to the solicited-node multicast with source=`::` (unspecified); if a conflicting NA arrives within 1 s, the address is a duplicate (log error and abort). In QEMU, DAD will succeed because addresses are unique; DAD is mandatory per RFC 4862.

- [ ] `ipv6_eui64_from_mac(mac, eui64_out)`: 3-byte OUI + FF:FE + 3-byte NIC bytes; flip bit 6 of byte 0
- [ ] `ipv6_linklocal_from_mac(mac, ip6_out)`: set bytes 0–1 = `0xFE 0x80`; bytes 2–7 = 0; bytes 8–15 = EUI-64
- [ ] `ipv6_dad(ip6)` → 0 (success) or -EADDRINUSE: send NS with src=`::` to solicited-node multicast; wait up to 1 s for NA; if no NA: return 0 (address unique); if NA received: return -EADDRINUSE
- [ ] `netif_assign_linklocal(netif)`: call `ipv6_linklocal_from_mac(netif->mac, netif->ip6_local)`; call `ipv6_dad()`; on success: log address; on failure: log warning (continue with zero address -- QEMU won't conflict)
- [ ] Call `netif_assign_linklocal()` from network init after MAC is known
- [ ] Log: `[IPv6] Link-local: fe80::%016llx (DAD OK)` or `[IPv6] DAD conflict -- no link-local address`
- [ ] Commit: `"net/ipv6: EUI-64 link-local from MAC, DAD via NS with src=::, netif_assign_linklocal()"`

## 5. NDP Neighbor Cache `[Opus]`

128-entry neighbor cache table keyed on IPv6 address → MAC. Entries follow the REACHABLE → STALE → PROBE → FAILED state machine per RFC 4861. Expire after 30 s of inactivity. Re-resolve before expiry via unicast NS.

**Files:** `src/kernel/net/ndp_cache.c` (new), `include/kernel/net/net.h` (extend)

> [!NOTE]
> This is `[Opus]` -- the NDP cache state machine has subtle concurrency: `ipv6_send()` (data path) reads the cache to get MACs, while `icmp6_handle()` (interrupt context) writes entries on NA receipt. Use a spinlock to guard all cache reads and writes. **States** (RFC 4861 §7.3): `INCOMPLETE` (NS sent, no NA yet), `REACHABLE` (NA received within reachability time ~30 s), `STALE` (not used for >30 s), `PROBE` (unicast NS sent to re-confirm), `FAILED` (3 probes sent with no response). **Eviction**: when all 128 slots are full, evict the oldest `FAILED` entry, then oldest `STALE`, then oldest `REACHABLE`. The cache must be accessible from both `ndp_resolve()` (task context) and `icmp6_handle()` (receive-path context); the spinlock must be held only for cache mutation, not during `ndp_send_ns()` (which calls `ipv6_send()`).

- [ ] `ndp_entry_t { uint8_t ip6[16]; uint8_t mac[6]; uint8_t state; uint64_t last_reachable_ms; uint64_t last_used_ms; uint8_t probe_count; uint8_t valid; }` states: `NDP_INCOMPLETE=0`, `NDP_REACHABLE=1`, `NDP_STALE=2`, `NDP_PROBE=3`, `NDP_FAILED=4`
- [ ] `ndp_cache[128]` + `ndp_lock` (spinlock)
- [ ] `ndp_cache_lookup(ip6, mac_out)` → state or NDP_INVALID: lock; find match; check `last_reachable_ms`; update `last_used_ms`; if STALE and `last_used_ms > 30s`: begin PROBE (send unicast NS); unlock; return state
- [ ] `ndp_cache_update(ip6, mac, reachable)`: lock; find or allocate slot (evict if full); set state REACHABLE or STALE; update timestamps; unlock
- [ ] `ndp_cache_age()`: called every 5 s from a kernel timer or idle hook: for each REACHABLE entry > 30 s since `last_reachable_ms`: → STALE; for each STALE entry where a PROBE was sent > 3 times: → FAILED
- [ ] `ndp_probe(entry)`: send unicast NS to `entry->ip6` (not solicited-node multicast); increment `probe_count`; schedule retry in 1 s
- [ ] Integrate into `icmp6_handle()` for NA type 136: call `ndp_cache_update(src_ip6, tlla_mac, reachable=1)`
- [ ] Integrate into `ipv6_send()`: before `eth_send()`, call `ndp_cache_lookup()`; if INCOMPLETE/FAILED: send NS + drop; if STALE/PROBE: use stale MAC (best effort)
- [ ] Commit: `"net/ndp: 128-entry neighbor cache, REACHABLE/STALE/PROBE/FAILED states, spinlock, aging"`

## 6. SLAAC -- Stateless Address Autoconfiguration `[Sonnet]`

On receiving an RA with M=0, combine the 64-bit RA prefix with the EUI-64 interface identifier to form a global IPv6 address. Set default gateway from RA source. Set DNS from RA RDNSS option.

**Files:** `src/kernel/net/ip6.c` (extend)

> [!NOTE]
> SLAAC (RFC 4862): triggered by receiving an RA message in `icmp6_handle()` when the RA has M=0 (Managed Address Config flag) -- meaning use SLAAC, not DHCPv6. **Prefix Information option** (type 3, RFC 4861 §4.6.2): includes prefix (16 bytes), prefix_length (1 byte), L/A flags, valid_lifetime, preferred_lifetime. Valid SLAAC prefix: `prefix_length == 64` and `A` flag set. Global address = RA prefix (first 8 bytes) + EUI-64 suffix (last 8 bytes from `netif->ip6_local`). **Preferred lifetime**: treat as address validity; if 0, remove address. Default gateway: RA source link-local address; add to NDP cache as REACHABLE. **RDNSS option** (RFC 8106, type 25): parse first DNS server IPv6 address; store in `netif->dns6[16]`; pass to `dns_resolve()` for future AAAA queries. After SLAAC assignment: log the new global address and send a DAD NS for it.

- [ ] `ipv6_slaac_configure(prefix, prefix_len, lifetime, ra_src_ip6, ra_src_mac)`: verify `prefix_len == 64`; combine prefix bytes 0–7 + EUI-64 bytes 8–15; run DAD; set `netif->ip6_global`; add RA source to NDP cache as default gateway
- [ ] Extend `netif` in `net.h`: add `uint8_t ip6_gateway[16]; uint8_t dns6[16];`
- [ ] RA parsing in `icmp6_handle()` type 134: iterate options (type/len fields, len in units of 8 bytes); parse type=3 (Prefix Info): call `ipv6_slaac_configure()`; parse type=25 (RDNSS): store first DNS IPv6 in `netif->dns6`
- [ ] `ipv6_slaac_deprecate()`: set `netif->ip6_global` to zero when preferred_lifetime==0 or valid_lifetime==0
- [ ] Send RS on boot (from `ndp_send_rs()` in §3) to trigger RA from QEMU's router; retry RS every 4 s up to 3 times if no RA received
- [ ] Log: `[SLAAC] Global: %x..%x/%u (DAD OK)`, `[SLAAC] Gateway: %x..%x`, `[SLAAC] DNS: %x..%x`
- [ ] Commit: `"net/ipv6: SLAAC -- RA prefix + EUI-64, global address, default gateway, RDNSS DNS option"`

## 7. DHCPv6 Client `[Sonnet]`

Solicit → Advertise → Request → Reply exchange on UDP ports 546 (client) / 547 (server). Obtain IA_NA address and DNS server. Use as fallback when SLAAC is unavailable (no RA, M=1 in RA).

**Files:** `src/kernel/net/dhcp6.c` (new), `include/kernel/net/net.h` (extend)

> [!NOTE]
> DHCPv6 (RFC 8415): messages sent to multicast `ff02::1:2` (all-DHCP-relay-servers), source = link-local. Message types: `SOLICIT=1`, `ADVERTISE=2`, `REQUEST=3`, `REPLY=7`. Transaction ID: random 24-bit value. Options format: `option_code (2) | option_len (2) | data (option_len)`. Key options: `OPTION_CLIENTID=1` (DUID-LL: type=3 + hw_type=1 + MAC), `OPTION_SERVERID=2` (copy from Advertise), `OPTION_IA_NA=3` (IAID=4 bytes + T1/T2 + IA_ADDR sub-option), `OPTION_IA_ADDR=5` (IPv6 address + preferred/valid lifetimes), `OPTION_DNS_SERVERS=23` (list of DNS IPv6 addresses). Flow: send Solicit (with IA_NA, no address hint); wait for Advertise (≤ 4 s); send Request (echo ServerID + IA_NA from Advertise); wait for Reply (≤ 4 s); extract IA_ADDR from Reply; set `netif->ip6_global`; set `netif->dns6` from OPTION_DNS_SERVERS. Triggered when: (a) no RA received within 5 s after 3 RS retries, or (b) RA received with M=1 flag.

- [ ] `dhcp6_message_t { uint8_t msg_type; uint8_t xid[3]; uint8_t options[]; }` (packed)
- [ ] `dhcp6_build_solicit(buf, &len, xid, mac)`: build SOLICIT with CLIENTID (DUID-LL) + IA_NA options
- [ ] `dhcp6_build_request(buf, &len, xid, mac, server_id, server_id_len, ia_addr)`: build REQUEST echoing ServerID + IA_NA with offered address
- [ ] `dhcp6_parse_advertise(buf, len, &server_id, &server_id_len, &offered_addr)`: extract SERVERID + IA_NA/IA_ADDR from Advertise
- [ ] `dhcp6_parse_reply(buf, len, &assigned_addr, dns6_out)`: extract IA_ADDR + OPTION_DNS_SERVERS from Reply
- [ ] `dhcp6_client_run()`: send Solicit; wait for Advertise (4 s timeout); send Request; wait for Reply; set `netif->ip6_global` + `netif->dns6`; log result
- [ ] `udp_register_handler(546, dhcp6_rx_cb)` in `dhcp6_client_run()`; `dhcp6_rx_cb` stores received packet for poll
- [ ] Trigger `dhcp6_client_run()` from network init if SLAAC address is still zero after 5 s boot
- [ ] Log: `[DHCPv6] Solicit sent`, `[DHCPv6] Advertise: addr=%x..%x`, `[DHCPv6] Reply: assigned %x..%x DNS %x..%x`
- [ ] Commit: `"net/dhcp6: DHCPv6 Solicit/Request/Reply, IA_NA addr, OPTION_DNS_SERVERS, UDP 546/547"`

## 8. DNS AAAA Activation `[Sonnet]`

Activate the `dns_resolve6()` stub from TODO-02 §4. Wire `dns_resolve_dual()` to prefer AAAA when `netif->ip6_global` is non-zero. Update `getaddrinfo()` to return both A and AAAA results with AAAA first.

**Files:** `src/kernel/net/dns.c` (extend), `user/lib/socket.c` (extend)

> [!NOTE]
> The DNS AAAA query builder and response parser are already in place from TODO-02 §4 -- they stored results in `dns_cache_entry_t.ip6[16]` and returned -ENOTSUP. Activation here: remove the `-ENOTSUP` guard in `dns_resolve6()`; use `netif->dns6` (from SLAAC §6 / DHCPv6 §7) as the DNS server for AAAA queries if it is non-zero; otherwise fall back to `netif->dns` (IPv4 DNS). The `dns_resolve_dual()` function already sends both A + AAAA queries -- now it returns the AAAA result in `ip6_out` if non-zero, and the caller uses it for IPv6 connections. Preference rule: if `netif->ip6_global` is zero (no IPv6 connectivity), force `ip4` result even if AAAA was received; this prevents using IPv6 DNS results when the data plane is IPv4-only.

- [ ] Remove `-ENOTSUP` early return from `dns_resolve6()`; complete the 16-byte rdata extraction path
- [ ] `dns_resolve6(hostname, ip6_out)` → 0 or -errno: send AAAA query to `netif->dns6` if non-zero, else to IPv4 DNS via `netif->dns`; parse 16-byte rdata; populate `ip6_out`; insert into cache
- [ ] `dns_resolve_dual(hostname, ip4_out, ip6_out)`: prefer AAAA if `netif->ip6_global != ::` else force ip4
- [ ] Extend `dns_cache_entry_t.has_aaaa` flag: already planned in TODO-02 §4; confirm it is used to cache AAAA alongside A
- [ ] `getaddrinfo(hostname, service, hints, &res)` in `user/lib/socket.c`: call `dns_resolve_dual()`; build `addrinfo` list with `AF_INET6` entry first (if has_aaaa), `AF_INET` entry second; `ai_addrlen` = `sizeof(struct sockaddr_in6)` for IPv6 entries
- [ ] Log: `[DNS] AAAA activated: %s → %x..%x (via %s DNS server)`
- [ ] Commit: `"net/dns: AAAA activation -- remove ENOTSUP, resolve_dual prefers AAAA when IPv6 active, getaddrinfo dual"`

## 9. Dual-Stack Socket API `[Opus]`

Extend the socket layer (TODO-02 §5) with `AF_INET6`, `struct sockaddr_in6`, `IPV6_V6ONLY` socket option, `connect(fd, ::1, port)` IPv6 loopback, and `bind(fd, ::, port)` dual-stack listen.

**Files:** `src/kernel/net/socket.c` (extend), `include/kernel/net/socket.h` (extend), `user/include/socket.h` (extend)

> [!NOTE]
> This is `[Opus]` -- dual-stack introduces a structural change to the socket layer: a single socket fd may now hold either an IPv4 or IPv6 connection, and `bind(fd, ::, port)` must accept both IPv4 and IPv6 connections (unless `IPV6_V6ONLY` is set). The tricky part: when `bind(fd, ::, port)` is called without `IPV6_V6ONLY`, an incoming IPv4 connection must be presented as an IPv4-mapped IPv6 address `::ffff:W.X.Y.Z`. Extend `socket_t` with `is_ipv6` flag and `remote_ip6[16]`. `tcp_connect6(ip6, port)` is needed from the TCP layer (TODO-01); add a `tcp_connect6()` wrapper that sets `is_ipv6=1` on the connection. IPv6 loopback `::1` short-circuits to `ipv6_receive()` directly (analogous to `127.0.0.1` loopback from TODO-01 §6).

- [ ] `struct sockaddr_in6 { uint16_t sin6_family; uint16_t sin6_port; uint32_t sin6_flowinfo; uint8_t sin6_addr[16]; uint32_t sin6_scope_id; }` in `socket.h`
- [ ] `AF_INET6=10`, `IPV6_V6ONLY=26` constants in `socket.h`
- [ ] Extend `socket_t`: add `uint8_t is_ipv6; uint8_t remote_ip6[16]; uint8_t local_ip6[16]; uint8_t ipv6_only;`
- [ ] `kern_connect_v6(fd, dst_ip6, port)`: validate `AF_INET6`; check `::1` → loopback shortcut; call `tcp_connect6(dst_ip6, port)`; set `sock->is_ipv6 = 1`; set `sock->remote_ip6`
- [ ] `kern_bind_v6(fd, local_ip6, port)`: set `sock->local_ip6`; set `sock->ipv6_only` from `IPV6_V6ONLY`; register UDP handler if `SOCK_DGRAM`
- [ ] `kern_accept_v6(fd, remote_ip6_out, port_out)`: pop from backlog; if connection is IPv4 and `!ipv6_only`: present as `::ffff:W.X.Y.Z` IPv4-mapped address
- [ ] `kern_send_v6(fd, buf, len)`: `tcp_send6()` or `udp_send6()`
- [ ] `kern_recv_v6(fd, buf, max)`: `tcp_recv6()` or `udp_recv6()`
- [ ] IPv6 loopback `::1`: in `ipv6_send()`, check if `dst == ::1`; if so call `ipv6_receive()` directly (no NDP, no Ethernet)
- [ ] Commit: `"net/socket: AF_INET6 dual-stack -- sockaddr_in6, IPV6_V6ONLY, connect6/bind6/accept6, ::1 loopback"`

## 10. `ifconfig` IPv6 Display `[Sonnet]`

Show link-local and global IPv6 addresses in `ifconfig` output. `ndp -an` shell command prints the NDP neighbor cache. Route table entry for `::/0` (default IPv6 gateway).

**Files:** `src/shell/cmd_ifconfig.c` (extend), `src/shell/cmd_ndp.c` (new)

> [!NOTE]
> `ifconfig` output extension: after existing IPv4 line, add `inet6 fe80::XXXX/10` (link-local) and `inet6 GGGG::/N` (global, if SLAAC/DHCPv6 assigned). Use `ipv6_ntop()` helper to convert 16-byte address to text form. `ipv6_ntop()`: RFC 5952 canonical form -- compress longest run of zero groups with `::`, omit leading zeros per group. `ndp -an` command: iterate `ndp_cache[128]`; print `IPv6 addr | MAC | State | Age(ms)`; skip invalid entries. Route table: add a logical `::/0 via fe80::GW` entry derived from `netif->ip6_gateway`; display in `route -6 print`.

- [ ] `ipv6_ntop(ip6, buf, buf_len)` → `buf`: RFC 5952 canonical form; `::` for longest zero run; leading-zero suppression per group
- [ ] `ipv4_ntop()` analog already exists; add `ipv6_ntop()` to `net.h`
- [ ] Extend `cmd_ifconfig()`: if `netif->ip6_local != ::`: print `inet6 <link-local>/10`; if `netif->ip6_global != ::`: print `inet6 <global>/<prefix_len>`
- [ ] `cmd_ndp(argc, argv)`: parse `-an` flag; iterate `ndp_cache`; print table; register `ndp` in shell command table
- [ ] `cmd_route` extension (or standalone): add `-6` flag; print `::/0 via <ip6_gateway> dev eth0` if `ip6_gateway != ::`
- [ ] Log at boot: `[IPv6] ifconfig: fe80::... / global: ...` (summary)
- [ ] Commit: `"shell: ifconfig IPv6 display, ipv6_ntop RFC5952, ndp -an neighbor cache, route -6"`

---

## OS Comparison


| ⭐  | Feature                                         | 🪟 Win11                                                     | 🐧 Linux                                                            | 🚀 Impossible OS                                              |
| --- | ----------------------------------------------- | ------------------------------------------------------------ | ------------------------------------------------------------------- | ------------------------------------------------------------- |
| 💎  | IPv6 header + ethertype 0x86DD routing          | ✅ `tcpip.sys` full IPv6; dual-stack on                      | ✅ `net/ipv6/ip6_input.c`; full dual-stack                          | ⬜ §1 -- `struct ipv6_header`, ethertype dispatch; no         |
| 💎  | IPv6 send/receive -- TCP/UDP/ICMPv6 dispatch    | ✅ Full IPv6 in `tcpip.sys`; extension                       | ✅ Full extension header support in                                 | ⬜ §2 -- extension header skip (log+drop); full               |
| 💎  | ICMPv6 NS/NA/RS/RA -- NDP replaces ARP for IPv6 | ✅ `tcpip.sys` NDP; solicited-node multicast; RA-triggered   | ✅ `net/ipv6/ndisc.c`; full RFC 4861 NDP                            | ⬜ §3 -- NS/NA/RS/RA; solicited-node multicast MAC; ICMPv6    |
| 💎  | Link-local EUI-64 from MAC, DAD                 | ✅ Auto-derives `fe80::/10` on interface up;                 | ✅ `ipv6_generate_eui64()` + DAD in kernel                          | ⬜ §4 -- EUI-64 derivation, bit-6 flip, DAD                   |
| 💎  | NDP neighbor cache                              | ✅ `tcpip.sys` neighbor cache; RFC 4861                      | ✅ `net/ipv6/ndisc.c` neighbor table; GC via                        | ⬜ §5 -- 128-entry spinlock-guarded; 30 s reachability        |
| 💎  | SLAAC                                           | ✅ SLAAC by default; RA processing                           | ✅ `net/ipv6/addrconf.c`; full RFC 4862 SLAAC                       | ⬜ §6 -- Prefix Info option parsing; DAD                      |
| 💎  | DHCPv6 client                                   | ✅ `dhcpcsvc.dll` + `tcpip.sys`; full DHCPv6                 | ✅ `dhclient`/`systemd-networkd`/`NetworkManager` (userspace)       | ⬜ §7 -- in-kernel DHCPv6 (`⭐` vs Linux's                    |
| 💎  | DNS AAAA queries                                | ✅ Dual A+AAAA by default; RFC                               | ✅ `getaddrinfo()` prefers AAAA; kernel DNS                         | ⬜ §8 -- activates TODO-02 stub; `dns_resolve_dual()` prefers |
| 💎  | Dual-stack socket API                           | ✅ Winsock2 `AF_INET6`; `IPV6_V6ONLY`; IPv4-mapped `::ffff:` | ✅ `net/socket.c`; `AF_INET6`; `IPV6_V6ONLY`; IPv4-mapped addresses | ⬜ §9 -- `AF_INET6=10`; IPv4-mapped in dual-stack accept      |
| 💎  | `ifconfig` IPv6 display, `ndp -an`, `route -6`  | ✅ `ipconfig /all` shows IPv6; `netsh                        | ✅ `ip -6 addr`, `ip neigh`,                                        | ⬜ §10 -- `ipv6_ntop()` RFC 5952; `ifconfig` extended         |

> **After §1–§10:** Impossible OS supports full IPv6 dual-stack -- link-local + global addresses via SLAAC (and DHCPv6 fallback), NDP replacing ARP, AAAA DNS resolution, and `AF_INET6` sockets. The in-kernel DHCPv6 client (`⭐` over Linux's userspace daemon approach) means zero userspace daemon dependencies. Every higher-level TODO (browser, email, SSH, OS updates) transparently benefits from IPv6 dual-stack routing without changes to their HTTP/TLS layer.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Boot log shows `[IPv6] Link-local: fe80::5054:00ff:fe12:3456 (DAD OK)` (QEMU MAC-derived EUI-64)
- [ ] ICMPv6: `ndp_resolve(ipv6_of_qemu_router)` → MAC returned within 3 s; serial log shows `[NDP] NS → ff02::...` and `[NDP] NA from fe80::...`
- [ ] RS/RA: boot log shows `[NDP] Sent RS` followed by `[SLAAC] Global: 2001:db8::5054:00ff:fe12:3456/64` (or QEMU's assigned prefix)
- [ ] SLAAC: `ifconfig` shows both `inet 192.168.x.x` (IPv4) and `inet6 fe80::...` (link-local) and `inet6 2001:...` (global)
- [ ] NDP cache: `ndp -an` prints at least one REACHABLE entry for the QEMU router after SLAAC completes
- [ ] DHCPv6 (if SLAAC unavailable): trigger `dhcp6_client_run()` manually; log shows `[DHCPv6] Reply: assigned X::/64`
- [ ] DNS AAAA: `dns_resolve_dual("ipv6.google.com", &ip4, ip6)` → `ip6` non-zero after SLAAC complete
- [ ] IPv6 loopback: `connect(fd, ::1, port)` from two tasks; `send()`/`recv()` exchange works without going to Ethernet
- [ ] `AF_INET6` socket: server `bind(::, 8080); listen; accept` in QEMU; host `nc -6 ::1 8080` → connection accepted
- [ ] `IPV6_V6ONLY`: `setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, 1)`; attempt IPv4 connect on same port → rejected
- [ ] `route -6` shows `::/0 via fe80::QEMU_GW dev eth0`
- [ ] Commit: `"net: complete IPv6 dual-stack -- NDP, EUI-64, SLAAC, DHCPv6, AF_INET6 sockets, ndp -an"`
