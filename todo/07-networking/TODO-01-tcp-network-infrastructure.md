---
schema_version: 1
id: tcp-network-infrastructure
domain: 07-networking
status: active
title: "TODO-01 -- TCP Protocol & Network Infrastructure"
---

# TODO-01 -- TCP Protocol & Network Infrastructure

> **Goal:** Build TCP on top of the working Ethernet/ARP/IPv4/UDP/DHCP stack, introduce a `net_interface` manager to replace the single global `net_cfg`, add a loopback interface, and implement stateful connection tracking as the backing layer for the network firewall. TCP is the foundation every higher-level protocol (HTTP, DNS, SSH, TLS) depends on.

> [!IMPORTANT]
> Ethernet, ARP, IPv4 (`ipv4_send`/`ipv4_handle`/`ipv4_checksum`), ICMP, UDP, and DHCP are complete in `src/kernel/net/`. `IP_PROTO_TCP=6` is already defined in `include/kernel/net/net.h`. The current stack uses a single global `struct net_config net_cfg` -- §5 replaces this with `struct net_interface` / `netif_*` API while keeping backward compatibility for existing callers via a `netif_get_default()` shim. §5 (netif) must land before §6 (loopback) and §7 (connection tracking) because both register as interfaces. The TCP implementation in §1–§4 builds on `ipv4_send()` directly; the pseudo-header checksum follows the same pattern as the existing UDP checksum in `src/kernel/net/udp.c`.

## Inputs

- `src/kernel/net/udp.c` + `include/kernel/net/net.h` -- `udp_send()`, `udp_handle()`, pseudo-header checksum pattern; `ipv4_send()` / `ipv4_handle()` for the TX/RX path; `net_cfg` for current IP/MAC -- all used by §1
- `src/kernel/net/ip.c` -- `ipv4_handle()` dispatches on `protocol` field; add `case IP_PROTO_TCP: tcp_handle(...)` here for §1
- `include/kernel/net/net.h` -- `struct net_config` → replaced/extended by `struct net_interface` in §5; all existing callers shim through `netif_get_default()`
- Related (no stable XREF target): `06-networking/TODO-02-*` (future DNS/TLS/HTTP TODOs) -- those callers use `tcp_connect()` / `tcp_send()` / `tcp_recv()` from this TODO
- Related (no stable XREF target): `10-platform-services/TODO-xx-firewall` -- connection tracking hash table (§7) is the backing store for the stateful firewall "allow established" rule

## Outcome

- `tcp_connect()`, `tcp_send()`, `tcp_recv()`, `tcp_close()` fully operational; full 11-state RFC 793 machine.
- TCP retransmission timer, Nagle's algorithm, slow-start congestion window; up to 32 simultaneous connections.
- `struct net_interface` / `netif_*` API replaces `net_cfg`; supports RTL8139 + future NICs.
- Loopback `lo` interface: `127.0.0.1/8` traffic short-circuits to `ip_receive()` without leaving kernel.
- Stateful connection tracking hash table keyed on 4-tuple; auto-expire TCP and UDP sessions.

## Implementation Order

| ⭐  | Order | Deliverable                                                                           | Depends On                                                       | Status |
| --- | :---: | ------------------------------------------------------------------------------------- | ---------------------------------------------------------------- | :----: |
| 💎  |   1   | §5 `net_interface` manager -- `struct net_interface`, `netif_register/get_default`, `net_cfg` shim | Existing `net_cfg`; `eth_send()`; RTL8139 driver registered at boot |  [ ]   |
| 💎  |   2   | §6 Loopback interface -- `lo`, 127/8 detection in `ipv4_send()`, short-circuit RX    | §5 (`netif_register()` needed to add `lo`)                       |  [ ]   |
| 💎  |   3   | §1 TCP header + checksum -- `tcp_header`, TCP flags, pseudo-header CRC, `ip_receive` routing | §5 (`netif_get_default()` for source IP in pseudo-header)        |  [ ]   |
| 💎  |   4   | §2 TCP state machine -- `tcp_connection`, 32-slot table, 11 RFC 793 states, transitions | §3 (TCP header parser needed before state machine can fire)      |  [ ]   |
| 💎  |   5   | §3 TCP API -- `tcp_connect`, `tcp_send`, `tcp_recv`, `tcp_close`, ephemeral ports     | §4 (state machine must be complete before API calls are safe)    |  [ ]   |
| 💎  |   6   | §4 TCP robustness -- retransmit timer, window validation, Nagle, slow-start, OOO hold | §5 (API layer provides the send path robustness hooks)           |  [ ]   |
| 💎  |   7   | §7 Connection tracking -- 4-tuple hash table, create on SYN/UDP, expire on FIN/RST   | §4 (TCP state machine fires CT creates/deletes)                  |  [ ]   |

---

## 1. TCP Header + Checksum `[Sonnet]`

Define `struct tcp_header`. Declare TCP flag constants. Implement the TCP pseudo-header checksum. Route `IP_PROTO_TCP` in `ipv4_handle()`.

**Files:** `src/kernel/net/tcp.c` (new), `include/kernel/net/net.h` (extend)

> [!NOTE]
> TCP header (20 bytes minimum, no options for the initial implementation): `src_port (2, BE)`, `dst_port (2, BE)`, `seq_num (4, BE)`, `ack_num (4, BE)`, `data_offset_flags (2, BE)` (upper 4 bits = data offset in 32-bit words, lower 12 = flags), `window (2, BE)`, `checksum (2, BE)`, `urgent_ptr (2, BE)`. Flag bits: `FIN=0x001`, `SYN=0x002`, `RST=0x004`, `PSH=0x008`, `ACK=0x010`, `URG=0x020`. TCP pseudo-header checksum: same structure as UDP's -- `src_ip (4)`, `dst_ip (4)`, `zero (1)`, `protocol=6 (1)`, `tcp_length (2)` prepended before the TCP header + data. Checksum function is identical to `ipv4_checksum()` -- call it on the pseudo-header + TCP segment concatenated. Route: in `ipv4_handle()`, add `case IP_PROTO_TCP: tcp_handle(src_ip, data, len); break;`.

- [ ] `struct tcp_header` in `net.h` (20 bytes, `__attribute__((packed))`)
- [ ] `TCP_FLAG_FIN`, `TCP_FLAG_SYN`, `TCP_FLAG_RST`, `TCP_FLAG_PSH`, `TCP_FLAG_ACK`, `TCP_FLAG_URG` constants
- [ ] `tcp_checksum(src_ip, dst_ip, tcp_seg, tcp_len)` → `uint16_t`: build 12-byte pseudo-header on stack; compute `ipv4_checksum()` over pseudo-header + segment
- [ ] `tcp_handle(src_ip, data, len)` stub (logs "TCP segment received" + verify checksum; dispatch to state machine in §2)
- [ ] Add `case IP_PROTO_TCP` branch in `ipv4_handle()` (`src/kernel/net/ip.c`)
- [ ] Log: `[TCP] RX %u.%u.%u.%u:%u → :%u seq=%u flags=0x%x len=%u`
- [ ] Commit: `"net/tcp: TCP header, flag constants, pseudo-header checksum, IP_PROTO_TCP routing"`

## 2. TCP Connection State Machine `[Opus]`

Implement `struct tcp_connection` with a 32-connection table. Drive all 11 RFC 793 states. Handle SYN, SYN+ACK, ACK, FIN, RST on both active and passive sides.

**Files:** `src/kernel/net/tcp.c` (extend), `include/kernel/net/net.h` (extend)

> [!NOTE]
> This is `[Opus]` -- the state machine has subtle concurrency requirements (interrupt context fires `tcp_handle()` which updates connection state; task context calls `tcp_send()`/`tcp_recv()`) and asymmetric open/close paths that are a common source of protocol bugs. States: `TCP_CLOSED`, `TCP_LISTEN`, `TCP_SYN_SENT`, `TCP_SYN_RECEIVED`, `TCP_ESTABLISHED`, `TCP_FIN_WAIT_1`, `TCP_FIN_WAIT_2`, `TCP_CLOSE_WAIT`, `TCP_CLOSING`, `TCP_LAST_ACK`, `TCP_TIME_WAIT`. State transitions: active open: `CLOSED→SYN_SENT` (send SYN); receive SYN+ACK → `ESTABLISHED` (send ACK). Passive open: `LISTEN→SYN_RECEIVED` (receive SYN, send SYN+ACK); receive ACK → `ESTABLISHED`. Active close: `ESTABLISHED→FIN_WAIT_1` (send FIN); receive ACK → `FIN_WAIT_2`; receive FIN → `TIME_WAIT` (send ACK); TIME_WAIT timer (2×MSL=120 s) → `CLOSED`. Passive close: `ESTABLISHED→CLOSE_WAIT` (receive FIN, send ACK); `CLOSE_WAIT→LAST_ACK` (application calls close, send FIN); receive ACK → `CLOSED`. RST: any state except CLOSED/LISTEN → `CLOSED`; log and free connection.

- [ ] `tcp_state_t` enum (11 values)
- [ ] `struct tcp_connection { uint32_t local_ip, remote_ip; uint16_t local_port, remote_port; tcp_state_t state; uint32_t snd_seq, snd_ack, rcv_nxt; uint16_t snd_win, rcv_win; uint8_t recv_buf[4096]; uint16_t recv_head, recv_tail; uint32_t retransmit_deadline; uint8_t retransmit_count; uint32_t cwnd, ssthresh; uint8_t used; }`
- [ ] `tcp_conn_table[32]` static array; `tcp_find_conn(local_ip, lport, remote_ip, rport)` → `tcp_connection*`; `tcp_alloc_conn()` → first free slot
- [ ] `tcp_state_transition(conn, flags, seq, ack, data, data_len)`: large switch on `conn->state × received flags`; all 11 RFC 793 transitions; call `tcp_send_segment()` for outgoing SYN/ACK/FIN/RST
- [ ] `tcp_send_segment(conn, flags, data, data_len)`: build `tcp_header`; set seq/ack from conn; compute checksum; call `ipv4_send(conn->remote_ip, IP_PROTO_TCP, ...)`
- [ ] TIME_WAIT: set `conn->retransmit_deadline = now_ms() + 120000`; timer tick clears the slot
- [ ] Commit: `"net/tcp: 11-state RFC 793 machine, 32-slot connection table, SYN/ACK/FIN/RST transitions"`

## 3. TCP API `[Sonnet]`

Expose `tcp_connect()`, `tcp_send()`, `tcp_recv()`, `tcp_close()`. Allocate ephemeral source ports from the range 49152–65535. Wire `tcp_recv()` to the per-connection 4 KiB ring buffer filled by `tcp_handle()`.

**Files:** `src/kernel/net/tcp.c` (extend), `include/kernel/net/net.h` (extend)

> [!NOTE]
> `tcp_connect(dst_ip, dst_port)` must block (spin-poll with a 5-second timeout) until the state machine reaches `ESTABLISHED` or returns an error. Blocking in kernel context: `tcp_connect()` sends the initial SYN, then spin-polls `conn->state == TCP_ESTABLISHED` with `ksleep(1)` between polls, up to 5000 iterations. `tcp_recv()` similarly spin-polls until `recv_head != recv_tail` (data available) or connection closes. `tcp_send()` may need to fragment -- if `data_len > conn->snd_win`: send `snd_win` bytes in this segment, the remainder in subsequent calls. Ephemeral port allocator: static `next_ephemeral_port` starting at 49152; increment modulo (65535 - 49152); check no collision in `tcp_conn_table`.

- [ ] `tcp_connect(dst_ip, dst_port)` → `tcp_connection*`: `tcp_alloc_conn()`; set remote ip/port, local ip = `netif_get_default()->ip`, local port = `tcp_alloc_ephemeral()`; send SYN; spin-poll 5 s; return conn or NULL
- [ ] `tcp_send(conn, data, len)` → bytes_sent: validate `conn->state == ESTABLISHED`; fragment if `len > conn->snd_win`; `tcp_send_segment(conn, PSH|ACK, data, len)`; advance `snd_seq`
- [ ] `tcp_recv(conn, buf, max_len)` → bytes_read: spin-poll until `recv_head != recv_tail` or `state ∈ {CLOSE_WAIT, CLOSED}`; copy from ring buffer; advance `recv_head`; return 0 on clean close
- [ ] `tcp_close(conn)`: if `ESTABLISHED`: send FIN, transition to `FIN_WAIT_1`; spin-poll until `TIME_WAIT` or `CLOSED`; free connection slot
- [ ] `tcp_listen(local_port)` → `tcp_connection*`: create a LISTEN-state connection; `tcp_accept(listener)` waits for `SYN_RECEIVED → ESTABLISHED`
- [ ] `tcp_alloc_ephemeral()` → `uint16_t`: advance `next_ephemeral_port`; skip any port already in `tcp_conn_table`
- [ ] Declare in `net.h`: `tcp_connection* tcp_connect(uint32_t dst_ip, uint16_t dst_port)`, `int tcp_send(...)`, `int tcp_recv(...)`, `void tcp_close(...)`, `tcp_connection* tcp_listen(uint16_t port)`, `tcp_connection* tcp_accept(...)`
- [ ] Commit: `"net/tcp: connect/send/recv/close API, listen/accept, ephemeral ports 49152–65535, ring buffer"`

## 4. TCP Robustness `[Opus]`

Retransmission timer (1 s, exponential backoff, 3 retries). Sequence window validation. RST abort. Nagle's algorithm. Congestion window slow-start. Out-of-order segment hold queue.

**Files:** `src/kernel/net/tcp.c` (extend)

> [!NOTE]
> This is `[Opus]` -- Nagle + slow-start interact with each other in subtle ways that require careful ordering of the send decision logic. Retransmission: each `tcp_send_segment()` saves the last-sent segment in `conn->retransmit_buf[1460]`; sets `conn->retransmit_deadline = now_ms() + 1000 << conn->retransmit_count`; on next tick, if `now_ms() > deadline && state != ESTABLISHED_FULLY_ACKED`: resend; increment `retransmit_count`; if `>3`: send RST and close. Sequence window: incoming segment is valid only if `rcv_nxt <= seg_seq < rcv_nxt + rcv_win`; drop silently if outside window; send ACK for duplicates. Nagle: hold outgoing data in a coalesce buffer if `len < MSS && unacknowledged data exists`; flush after 200 ms or when buffer reaches MSS. Slow-start: `cwnd = 1 MSS` at connection open; on each ACK: `cwnd += 1 MSS` until `cwnd >= ssthresh (64 KiB)`; on loss (retransmit): `ssthresh = cwnd / 2; cwnd = 1 MSS`. OOO hold queue: 4-slot ring of `{seq, data, len}` tuples; on receive of future-sequence segment: buffer it; when `rcv_nxt` advances to match: inject from hold queue.

- [ ] `tcp_tick()`: called from a 100 ms kernel timer; iterates `tcp_conn_table`; for each connection: check retransmit deadline; check TIME_WAIT expiry; flush Nagle buffer if 200 ms elapsed
- [ ] Retransmit timer: `conn->retransmit_buf[1460]`, `retransmit_len`, `retransmit_deadline`, `retransmit_count`; on `tcp_send_segment()`: copy segment to `retransmit_buf`; set deadline; on ACK received: clear buffer; reset count
- [ ] Sequence window check in `tcp_handle()`: compute `seg_seq_within_window = (seg_seq - conn->rcv_nxt) < conn->rcv_win`; if outside: log `[TCP] OOO drop seq=%u rcv_nxt=%u`; send ACK; return
- [ ] Nagle: `conn->nagle_buf[1460]`, `nagle_len`, `nagle_start_ms`; `tcp_send()`: if `len < MSS && conn->snd_seq != conn->snd_ack`: append to `nagle_buf`; else: flush immediately; `tcp_tick()` flushes after 200 ms
- [ ] Slow-start: `conn->cwnd` init = `1 * 1460`; `conn->ssthresh = 65535`; on ACK: if `cwnd < ssthresh: cwnd += 1460`; else `cwnd += 1460*1460/cwnd` (congestion avoidance); on loss: `ssthresh = max(cwnd/2, 2*1460); cwnd = 1460`; `tcp_send()` clamps `send_len = min(len, cwnd)`
- [ ] OOO hold queue: `conn->ooo_buf[4]` of `{uint32_t seq; uint8_t data[1460]; uint16_t len;}`; on segment with `seg_seq > rcv_nxt`: store in hold queue; on `rcv_nxt` advance: check if head of hold queue matches; inject
- [ ] Commit: `"net/tcp: retransmit 1s/3x backoff, window validation, Nagle 200ms, slow-start cwnd, OOO 4-slot"`

## 5. Network Interface Manager `[Sonnet]`

Replace the single global `net_cfg` with `struct net_interface` and `netif_register/get_default/set_ip/get_by_name`. Maintain backward compatibility via a `netif_get_default()` shim.

**Files:** `src/kernel/net/netif.c` (new), `include/kernel/net/net.h` (extend), `src/kernel/net/ip.c` (extend)

> [!NOTE]
> Migration strategy: keep `extern struct net_config net_cfg` as a global that `netif_get_default()` returns a pointer to; existing callers (`ip.c`, `dhcp.c`, `arp.c`) compile without change. New callers use `netif_get_default()` or `netif_get_by_name("eth0")`. The `net_interface` struct extends `net_config` with a `name[8]`, `mtu`, and a `tx` callback (`void (*tx)(const void*, uint32_t)`). RTL8139 registers itself on init as `"eth0"`. Multiple NIC support: up to 8 interfaces in `g_netif_table[8]`; `netif_register()` fills the next slot; `netif_get_default()` returns the first configured interface. `ipv4_send()` calls `netif_get_default()->tx()` instead of the hardcoded RTL8139 send function.

- [ ] `struct net_interface { char name[8]; uint32_t ip; uint32_t subnet; uint32_t gateway; uint32_t dns; uint8_t mac[6]; uint16_t mtu; uint8_t configured; void (*tx)(const void *data, uint32_t len); }`
- [ ] `g_netif_table[8]` static array; `netif_count` counter
- [ ] `netif_register(name, mac, mtu, tx_fn)` → `net_interface*`: fill next free slot; return pointer
- [ ] `netif_get_default()` → `net_interface*`: return first entry with `configured=1`; fall back to first entry regardless
- [ ] `netif_get_by_name(name)` → `net_interface*`: linear scan `g_netif_table` for matching `name`
- [ ] `netif_set_ip(netif, ip, subnet, gateway, dns)`: update fields; set `configured=1`; update `net_cfg` shim (copy fields to `net_cfg` for backward compat)
- [ ] Shim: `net_cfg` now backed by `g_netif_table[0]`; `net_cfg.ip` stays in sync via `netif_set_ip()`; existing `net_cfg.configured` reads still work
- [ ] Update `ipv4_send()`: replace `net_cfg.mac` / direct NIC call with `netif_get_default()->tx(frame, len)`
- [ ] RTL8139 init: call `netif_register("eth0", mac, 1500, rtl8139_send_frame)` during driver init
- [ ] Commit: `"net: netif manager -- struct net_interface, register/get_default, net_cfg shim, ipv4_send decoupled"`

## 6. Loopback Interface `[Sonnet]`

Register a virtual `lo` interface. Detect `127.0.0.0/8` in `ipv4_send()` and short-circuit directly to `ipv4_handle()` without touching the NIC.

**Files:** `src/kernel/net/netif.c` (extend), `src/kernel/net/ip.c` (extend)

> [!NOTE]
> Loopback TX callback: a stub function `loopback_tx(data, len)` that calls `net_rx(data_without_eth_header, len)` (or directly `ipv4_handle()` if the loopback frame has no Ethernet wrapper). Since `127.x.x.x` is the destination, skip ARP entirely. Detect loopback destination in `ipv4_send()`: if `(dst_ip & 0xFF000000) == 0x7F000000` (127.0.0.0/8 in big-endian): set `src_ip = 0x7F000001` (127.0.0.1); call `ipv4_handle()` directly with the built IP packet; return. This means TCP connections to `127.0.0.1` never leave the kernel and bypass ARP, Ethernet framing, and the NIC transmit queue.

- [ ] `netif_init_loopback()`: `netif_register("lo", zero_mac, 65535, loopback_tx_fn)`; `netif_set_ip(lo, 0x7F000001, 0xFF000000, 0, 0)` (127.0.0.1/8)
- [ ] `loopback_tx_fn(data, len)`: receives a complete Ethernet frame (or raw IP if we skip eth wrapping for lo); strip Ethernet header if present; call `ipv4_handle(data, len)` directly
- [ ] Detection in `ipv4_send()`: before ARP lookup -- `if ((dst_ip >> 24) == 0x7F)`: build IP header with `src=127.0.0.1`; call `ipv4_handle()` directly; return
- [ ] `netif_init_loopback()` called from `net_init()` before NIC registration
- [ ] `ping 127.0.0.1` in shell → round-trip via loopback, no NIC traffic; `[ICMP] echo reply from 127.0.0.1` in serial log
- [ ] Commit: `"net: loopback interface lo -- 127/8 short-circuit in ipv4_send, loopback_tx → ipv4_handle direct"`

## 7. Stateful Connection Tracking `[Opus]`

Hash table keyed on `(src_ip, src_port, dst_ip, dst_port)`. Create entries on outbound SYN and outbound UDP. Auto-allow inbound packets that match an established entry. Expire on FIN/RST (TCP) or timeout (UDP 60 s, TCP 300 s).

**Files:** `src/kernel/net/conntrack.c` (new), `include/kernel/net/net.h` (extend)

> [!NOTE]
> This is `[Opus]` -- connection tracking must be updated from two concurrent contexts (TX path: connection created on outbound SYN; RX path: connection matched/expired on inbound packet) with no locks in the critical path (interrupt context). Strategy: mark entries as `state = CT_DELETING` from the RX path; actual free happens from a cleanup task called from `tcp_tick()` -- never free from interrupt context. Hash function: `hash = (src_ip ^ dst_ip ^ ((uint32_t)src_port << 16 | dst_port)) % CT_TABLE_SIZE`; collision resolution: open addressing with linear probing (table size = power of 2, load factor < 75%). The firewall rule "allow inbound if ESTABLISHED" queries `conntrack_lookup(src_ip, src_port, dst_ip, dst_port)` -- must be O(1).

- [ ] `ct_state_t` enum: `CT_FREE`, `CT_SYN_SENT`, `CT_ESTABLISHED`, `CT_TIME_WAIT`, `CT_UDP`, `CT_DELETING`
- [ ] `struct ct_entry { uint32_t src_ip, dst_ip; uint16_t src_port, dst_port; ct_state_t state; uint32_t expire_ms; uint8_t proto; }` -- 32-byte aligned
- [ ] `ct_table[256]` (open-addressing hash table, power-of-2 for fast modulo); `ct_count` active entries
- [ ] `conntrack_hash(src_ip, src_port, dst_ip, dst_port)` → index
- [ ] `conntrack_create(src_ip, src_port, dst_ip, dst_port, proto)`: create entry; for TCP: `state = CT_SYN_SENT`, expire = `now_ms() + 300000`; for UDP: `state = CT_UDP`, expire = `now_ms() + 60000`
- [ ] `conntrack_lookup(src_ip, src_port, dst_ip, dst_port)` → `ct_entry*` or NULL: O(1) hash probe; check 4-tuple; return pointer if found and not deleting
- [ ] `conntrack_update(entry, flags)`: on TCP ACK after SYN+ACK → `CT_ESTABLISHED`; on FIN → start `CT_TIME_WAIT`; on RST → `CT_DELETING`
- [ ] `conntrack_expire_tick()`: called from `tcp_tick()` (100 ms timer); scan table; free entries where `now_ms() > expire_ms`; decrement `ct_count`
- [ ] Hook into TCP state machine (§2): on outbound SYN: `conntrack_create()`; on state transitions: `conntrack_update()`
- [ ] Hook into UDP send: on `udp_send()`: `conntrack_create()` for UDP 4-tuple
- [ ] `conntrack_lookup()` declared in `net.h` for use by firewall layer
- [ ] Commit: `"net: stateful connection tracking -- 256-slot hash table, TCP/UDP entries, SYN/FIN/RST hooks, 60/300s expire"`

---

## OS Comparison


| ⭐  | Feature                                                         | 🪟 Win11                                     | 🐧 Linux                                           | 🚀 Impossible OS                                            |
| --- | --------------------------------------------------------------- | -------------------------------------------- | -------------------------------------------------- | ----------------------------------------------------------- |
| 💎  | TCP header + pseudo-header checksum, IP_PROTO_TCP routing       | ✅ `tcpip.sys`; full TCP/IP stack            | ✅ `net/ipv4/tcp.c`; full TCP stack                | ⬜ §1 -- 20-byte header, flags, pseudo-CRC, ipv4_handle     |
| 💎  | TCP 11-state RFC 793 machine                                    | ✅ `tcpip.sys`; RFC 793 + RFC                | ✅ Linux TCP; RFC 793 +                            | ⬜ §2 -- 32-slot table, all 11 states,                      |
| 💎  | `tcp_connect/send/recv/close` + listen/accept + ephemeral ports | ✅ Winsock2 API wraps `tcpip.sys`            | ✅ BSD socket API over `net/ipv4/tcp.c`            | ⬜ §3 -- direct kernel API; ephemeral 49152–65535           |
| 💎  | TCP robustness                                                  | ✅ `tcpip.sys`; full RFC 5681 +              | ✅ Linux TCP; SACK, cubic congestion               | ⬜ §4 -- basic Nagle + slow-start sufficient                |
| 💎  | `net_interface` manager                                         | ✅ `tcpip.sys` / NDIS NIC abstraction        | ✅ `net_device` + `netif_*` infrastructure         | ⬜ §5 -- 8-slot table; `net_cfg` shim; RTL8139              |
| 💎  | Loopback `lo`                                                   | ✅ `tcpip.sys`; loopback fully optimized (no | ✅ `drivers/net/loopback.c`; `dev_loopback_xmit()` | ⬜ §6 -- detection in `ipv4_send()`, direct `ipv4_handle()` |
| 💎  | Stateful connection tracking                                    | ✅ `tcpip.sys`; full stateful NAT +          | ✅ `nf_conntrack`; nftables / iptables backing;    | ⬜ §7 -- 256-slot open-addressing table; firewall "allow    |

> **After §1–§7:** Impossible OS has a complete, standards-correct TCP stack with a multi-NIC interface abstraction and a stateful firewall backing layer -- all built in kernel space without any userspace networking daemon. Every higher-level protocol (DNS, TLS, HTTP, SSH) can be built directly on the `tcp_connect`/`tcp_send`/`tcp_recv` API from this TODO.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] `netif_get_default()` returns a non-NULL pointer with `configured=1` after DHCP; `net_cfg.ip` still readable by existing callers
- [ ] Loopback: `tcp_connect(0x7F000001, 80)` on a `tcp_listen(80)` in the same kernel → connection reaches `ESTABLISHED` without any NIC traffic; serial log shows `[TCP] loopback connection established`
- [ ] TCP 3-way handshake: connect to a QEMU-hosted TCP echo server (`nc -l 1234`); `tcp_connect(server_ip, 1234)` → `ESTABLISHED` logged; `tcp_send(conn, "hello", 5)` → server receives; `tcp_recv(conn, buf, 5)` → `"hello"` returned
- [ ] TCP state machine: after `tcp_close()`: states transition through `FIN_WAIT_1` → `FIN_WAIT_2` → `TIME_WAIT` → `CLOSED`; 32-slot entry freed after TIME_WAIT
- [ ] Retransmission: block the QEMU NIC for 1 s after sending a segment; verify `[TCP] retransmit count=1` in serial log; unblock → ACK received; `retransmit_count` reset
- [ ] Nagle: `tcp_send(conn, "a", 1)` × 10 rapid calls → only 1–2 segments emitted (coalesced); 200 ms later → segment flushed
- [ ] Slow-start: `cwnd` starts at 1460; after each ACK: verify `cwnd` doubles until `ssthresh`; simulate loss via retransmit → `cwnd` resets to 1460
- [ ] Checksum: corrupt 1 byte in a received TCP segment; `tcp_handle()` logs `[TCP] bad checksum` and drops without state change
- [ ] Connection tracking: `tcp_connect()` → `conntrack_lookup(src, src_port, dst, 1234)` returns non-NULL with `CT_ESTABLISHED`; `tcp_close()` → `CT_DELETING`; after expire tick → entry freed
- [ ] UDP tracking: `udp_send(dst, src_port, dst_port, ...)` → `conntrack_lookup()` returns UDP entry; expires after 60 s idle
- [ ] Commit: `"net/tcp: complete TCP stack -- header, state machine, API, robustness, netif, loopback, conntrack"`
