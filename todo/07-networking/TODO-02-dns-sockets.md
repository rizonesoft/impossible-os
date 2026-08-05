---
schema_version: 1
id: dns-sockets
domain: 07-networking
status: active
title: "TODO-02 -- DNS Resolver & BSD Sockets API"
---

# TODO-02 -- DNS Resolver & BSD Sockets API

> **Goal:** Build the DNS resolver (query builder, response parser, 64-entry LRU cache, AAAA support) and a BSD-compatible kernel socket layer (`SOCK_STREAM`/`SOCK_DGRAM`, `socket`/`connect`/`send`/`recv`/`bind`/`listen`/`accept`/`select`), expose them via 8 new syscalls, and provide thin user-mode wrappers. DNS and sockets turn raw IP+port into the hostname-based, file-descriptor API every application uses.

> [!IMPORTANT]
> TCP (`tcp_connect`/`tcp_send`/`tcp_recv`/`tcp_close`) must be complete (→ XREF: `06-networking/TODO-01-tcp-network-infrastructure.md`) before the `SOCK_STREAM` socket type can be wired. DNS sends queries via `udp_send()` (already working) and receives responses via a `udp_register_handler()` callback. Existing syscall numbers 1–17 and 33–38 are occupied; socket syscalls start at **39** (`SYS_SOCKET=39` through `SYS_SELECT=46`) leaving room below 39 for future kernel syscalls. The 64-bit `select()` event bitmask is capped at 64 sockets -- sufficient for the initial implementation; `poll()` parity is deferred.

## Inputs

- `src/kernel/net/udp.c` + `include/kernel/net/net.h` -- `udp_send()` for DNS query TX; add `udp_register_handler(port, cb)` to receive DNS responses on port 53 reply (ephemeral port)
- `src/kernel/net/tcp.c` -- `tcp_connect()`, `tcp_send()`, `tcp_recv()`, `tcp_close()`, `tcp_listen()`, `tcp_accept()` from TODO-01; socket layer wraps these
- `include/kernel/sched/syscall.h` -- add `SYS_SOCKET=39` through `SYS_SELECT=46`; next free slot after existing `SYS_MUNMAP=38`
- `src/kernel/sched/syscall.c` -- add 8 new dispatch entries in the syscall handler
- `user/lib/` -- `socket.c` thin wrappers called from user-mode programs
- → XREF: `06-networking/TODO-01-tcp-network-infrastructure.md` -- TCP API (§3) and `netif_get_default()` (§5) are prerequisites for `SOCK_STREAM` and `getaddrinfo`
- Related (no stable XREF target): `06-networking/TODO-04-*` (future IPv6) -- §6 AAAA query stub is the hook point for dual-stack; `dns_resolve6()` is left as a stub returning -ENOTSUP until IPv6 is complete

## Outcome

- `dns_resolve(hostname, &ip)` works end-to-end with DNS compression pointer support and 3 s timeout.
- 64-entry LRU DNS cache with TTL expiry; `nslookup <hostname>` shell command.
- AAAA query stub for future IPv6 dual-stack; preference logic scaffolded.
- `socket()`, `connect()`, `send()`, `recv()`, `bind()`, `listen()`, `accept()`, `close()` kernel-layer functions.
- `setsockopt()` / `getsockopt()` for `SO_REUSEADDR`, `SO_RCVTIMEO`, `SO_SNDBUF`.
- `select()` blocking on up to 64 socket fds with millisecond timeout.
- 8 new syscalls (`SYS_SOCKET` 39 through `SYS_SELECT` 46) with user-mode wrappers in `user/lib/socket.c`.

## Implementation Order

| ⭐  | Order | Deliverable                                                                          | Depends On                                                              | Status |
| --- | :---: | ------------------------------------------------------------------------------------ | ----------------------------------------------------------------------- | :----: |
| 💎  |   1   | §1 DNS query builder -- `dns_header`, hostname encoding, A-record query, `udp_send`  | `udp_send()` working; `udp_register_handler()` for reply RX             |  [ ]   |
| 💎  |   2   | §2 DNS response parser -- ID verify, RCODE, compression pointers, A-record extract   | §1 (query must be sent before response can be validated)                |  [ ]   |
| 💎  |   3   | §3 DNS cache -- 64-entry LRU, TTL expiry, `nslookup` shell command                   | §2 (populate cache on successful resolve)                               |  [ ]   |
| 💎  |   4   | §4 AAAA query stub -- parallel A+AAAA, prefer AAAA, fall back to A                   | §1–§3 (A-record path must be proven before AAAA is added)               |  [ ]   |
| 💎  |   5   | §5 Kernel socket layer -- `socket/connect/send/recv/close`, `SOCK_STREAM`/`DGRAM`    | TCP API from TODO-01; `dns_resolve()` for connect-by-hostname           |  [ ]   |
| 💎  |   6   | §6 Server sockets -- `bind/listen/accept`, backlog queue                             | §5 (socket fd table must exist before bind/listen/accept can be added)  |  [ ]   |
| 💎  |   7   | §7 Socket options + syscalls -- `setsockopt/getsockopt`, 8 new `SYS_*` numbers      | §5, §6 (all socket operations must be complete before syscall dispatch)  |  [ ]   |
| 💎  |   8   | §8 `select()` -- 64-fd event bitmask, data-ready + writable, ms timeout              | §5 (socket fd table); §6 (server sockets contribute ACCEPT readiness)   |  [ ]   |

---

## 1. DNS Query Builder `[Sonnet]`

Define `struct dns_header`. Encode a hostname as a DNS label sequence. Build a standard A-record (QTYPE=1) query with a random transaction ID. Send via `udp_send()` to the DHCP-provided DNS server.

**Files:** `src/kernel/net/dns.c` (new), `include/kernel/net/net.h` (extend)

> [!NOTE]
> DNS message format: `struct dns_header { uint16_t id; uint16_t flags; uint16_t qdcount, ancount, nscount, arcount; }` (all big-endian). Flags for a standard query: `0x0100` (QR=0 query, OPCODE=0 standard, RD=1 recursion desired). A-record query: QTYPE=`0x0001`, QCLASS=`0x0001` (IN). Hostname encoding: split on `.`; for each label: emit length byte + label bytes; terminate with `0x00`. `"www.example.com"` → `\x03www\x07example\x03com\x00`. Random ID: use `rdrand` if available (via `cpu_has(CPU_FEATURE_RDRAND)`) else TSC low-16 bits. DNS server IP: `netif_get_default()->dns`; port 53. Source port for reply: allocate an ephemeral UDP port (e.g., `dns_reply_port = 1024 + (id % 48128)`) and register a callback on that port via `udp_register_handler(reply_port, dns_rx_cb)`.

- [ ] `struct dns_header` + `struct dns_question { uint16_t qtype, qclass; }` in `net.h` (packed)
- [ ] `dns_encode_hostname(hostname, buf, buf_len)` → encoded length: walk hostname; emit label lengths + bytes; null terminator
- [ ] `dns_build_query(hostname, id, out_buf, &out_len)`: fill `dns_header`; append encoded hostname + question record; cap at 512 bytes
- [ ] `dns_query_send(hostname, id)`: compute `reply_port`; `udp_register_handler(reply_port, dns_rx_cb)`; `udp_send(dns_server_ip, reply_port, 53, query_buf, query_len)`
- [ ] `udp_register_handler(port, cb)`: add to UDP demultiplexer (extend `udp_handle()` to dispatch by dst_port to registered callbacks); at most 16 concurrent handlers
- [ ] Log: `[DNS] Query id=%04x type=A %s → server %u.%u.%u.%u`
- [ ] Commit: `"net/dns: query builder -- dns_header, hostname label encode, A-record query, udp_send to port 53"`

## 2. DNS Response Parser `[Sonnet]`

Verify transaction ID, RCODE == 0, and `ancount > 0`. Skip the question section. Handle DNS name compression pointers (`0xC0xx`). Extract the first `A` record (type 1) IPv4 address.

**Files:** `src/kernel/net/dns.c` (extend)

> [!NOTE]
> DNS compression: a name component starting with `0xC0` means the next byte is an offset from the start of the message, pointing to another label sequence. Implementation: a name-skip helper must handle both inline labels and pointer jumps; cap pointer depth at 8 to prevent loops. Response parsing algorithm: (1) verify `header.id` matches the pending query ID; (2) check `(flags >> 15) == 1` (QR=response) and `(flags & 0xF) == 0` (RCODE=no error); (3) skip question section: parse and discard `qdcount` question entries; (4) walk `ancount` answer records: `{ name (compressed), type (2), class (2), ttl (4), rdlength (2), rdata[rdlength] }`; stop at the first record where `type == 0x0001` (A record) and `rdlength == 4`; extract 4-byte IPv4; store TTL.

- [ ] `dns_skip_name(buf, offset, len)` → new offset: advance past a DNS-encoded name, handling `0xC0` pointer jumps; return offset after the name
- [ ] `dns_parse_response(buf, len, expected_id, &ip_out, &ttl_out)` → 0 or -1: full parse as described above; validate all bounds before reading
- [ ] `dns_rx_cb(src_ip, src_port, data, len)`: check `src_port == 53`; call `dns_parse_response()`; store result in `dns_pending.ip` and `dns_pending.ttl`; set `dns_pending.done = 1`
- [ ] `dns_pending_t { uint16_t id; uint32_t ip; uint32_t ttl; uint8_t done; uint8_t error; }` -- static for simplicity (single outstanding query at a time for the initial implementation)
- [ ] RCODE mapping: `RCODE=1` = format error; `RCODE=3` = NXDOMAIN; return distinct error codes
- [ ] Log: `[DNS] Response id=%04x ip=%u.%u.%u.%u ttl=%u` or `[DNS] NXDOMAIN for id=%04x`
- [ ] Commit: `"net/dns: response parser -- RCODE check, compression pointer, A-record extract, TTL"`

## 3. DNS Cache `[Sonnet]`

64-entry LRU table mapping hostname → (IPv4, TTL, timestamp). Check cache before querying the network. Expire entries when TTL elapses. Implement `dns_resolve()` blocking API and `nslookup` shell command.

**Files:** `src/kernel/net/dns.c` (extend), `src/shell/cmd_nslookup.c` (new)

> [!NOTE]
> LRU policy: entries ordered by last-access time; on a cache miss that fills the 64th slot, evict the entry with the oldest `last_access_ms`. TTL expiry: `dns_resolve()` checks `now_ms() - entry.timestamp > entry.ttl * 1000`; on expiry, re-query. `dns_resolve()` blocking: check cache → on hit return immediately; on miss: `dns_query_send()`; spin-poll `dns_pending.done` up to 3000 ms (3 s timeout with 1 ms sleeps); on success: `dns_cache_insert(hostname, ip, ttl)`; return IP.

- [ ] `dns_cache_entry_t { char hostname[256]; uint32_t ip; uint32_t ttl_s; uint32_t timestamp_ms; uint8_t valid; }` + `dns_cache[64]` static array
- [ ] `dns_cache_lookup(hostname, &ip)` → 1 (hit) or 0 (miss/expired): case-insensitive compare; check `ttl_s`; update `last_access_ms` on hit
- [ ] `dns_cache_insert(hostname, ip, ttl_s)`: find free slot or LRU evict slot; fill entry
- [ ] `dns_cache_evict_lru()` → slot index: find entry with smallest `last_access_ms` among valid entries
- [ ] `dns_resolve(hostname, &ip_out)` → 0 or -errno: check cache; on miss: `dns_query_send()`; spin-poll 3000 ms; on success: insert + return; on timeout: return -ETIMEDOUT; on NXDOMAIN: return -ENOENT
- [ ] `nslookup <hostname>` shell command: call `dns_resolve()`; print `hostname → W.X.Y.Z (TTL=Ns, cached=yes/no)`
- [ ] Commit: `"net/dns: 64-entry LRU cache, TTL expiry, dns_resolve() blocking 3s timeout, nslookup command"`

## 4. AAAA Query Stub `[Sonnet]`

Build and send AAAA (type=28) queries in parallel with A queries. Prefer AAAA results for IPv6 dual-stack readiness. Fall back to A on AAAA timeout or NXDOMAIN.

**Files:** `src/kernel/net/dns.c` (extend)

> [!NOTE]
> AAAA query is identical to an A query except QTYPE=`0x001C` (28). The response rdata is 16 bytes (IPv6 address) instead of 4. For IPv6 dual-stack (TODO-04), the caller receives both an `ip4` and an `ip6` result; the socket layer prefers IPv6. For now (IPv4 only), `dns_resolve6()` sends the AAAA query, receives the response, but returns -ENOTSUP with the 16-byte address stored in the cache entry's `ip6[16]` field for future use. Parallel query: send both A and AAAA in the same `dns_resolve_dual()` call with different transaction IDs; spin-poll for whichever responds first; prefer AAAA if both respond.

- [ ] Extend `dns_cache_entry_t` with `uint8_t ip6[16]; uint8_t has_aaaa;`
- [ ] `dns_build_query_aaaa(hostname, id, out_buf, &out_len)`: same as `dns_build_query()` but QTYPE=28
- [ ] `dns_resolve6(hostname, ip6_out)` → 0 or -ENOTSUP: send AAAA query; parse 16-byte rdata; store in cache; return -ENOTSUP until IPv6 socket layer exists
- [ ] `dns_resolve_dual(hostname, &ip4, ip6_or_null)`: send A + AAAA simultaneously with distinct IDs; collect both responses within 3 s; populate ip4 and ip6; prefer AAAA for future use
- [ ] Log: `[DNS] AAAA response for %s: stored (IPv6 not yet active)`
- [ ] Commit: `"net/dns: AAAA query + dual resolve stub, ip6 in cache entry, ENOTSUP until IPv6 active"`

## 5. Kernel Socket Layer `[Opus]`

Per-process socket fd table (64 entries). `socket(AF_INET, SOCK_STREAM/SOCK_DGRAM, 0)`. `connect(fd, ip, port)`. `send(fd, buf, len)` / `recv(fd, buf, len)`. `close(fd)`.

**Files:** `src/kernel/net/socket.c` (new), `include/kernel/net/socket.h` (new)

> [!NOTE]
> This is `[Opus]` -- the socket layer introduces a new concurrency domain: task-context `recv()` blocks waiting for data that arrives from interrupt-context `tcp_handle()` / `udp_handle()`. The spin-poll approach from TODO-01's `tcp_recv()` is acceptable here too, but the socket layer must ensure no fd alias bugs (two tasks with the same fd pointing to different connections after fork) and correct reference-counted cleanup on `close()`. Socket fd table: per-process (stored in `task_t`), 64 entries, index 3–66 (0=stdin, 1=stdout, 2=stderr reserved). `socket_t` struct: type, state, underlying TCP connection or UDP port, receive buffer, `so_rcvtimeo` (0=blocking), `so_reuseaddr`, `so_sndbuf`.

- [ ] `socket_t { uint8_t type; uint8_t state; tcp_connection *tcp_conn; uint16_t udp_port; uint32_t so_rcvtimeo_ms; uint8_t so_reuseaddr; uint32_t so_sndbuf; uint8_t used; }`
- [ ] `sock_table[64]` in `task_t` (or global for initial kernel-only implementation); `sock_alloc()` → fd (3–66); `sock_free(fd)`
- [ ] `kern_socket(domain, type, protocol)` → fd: validate `domain==AF_INET`; validate type; `sock_alloc()`; set `sock->type`
- [ ] `kern_connect(fd, dst_ip, dst_port)` → 0 or -errno: for `SOCK_STREAM`: `tcp_connect(dst_ip, dst_port)` → store in `sock->tcp_conn`; for `SOCK_DGRAM`: store dst_ip/port, allocate ephemeral UDP source port
- [ ] `kern_send(fd, buf, len)` → bytes: `SOCK_STREAM`: `tcp_send(sock->tcp_conn, buf, len)`; `SOCK_DGRAM`: `udp_send(dst_ip, sock->udp_port, dst_port, buf, len)`
- [ ] `kern_recv(fd, buf, max_len)` → bytes: `SOCK_STREAM`: `tcp_recv(sock->tcp_conn, buf, max_len)` with `so_rcvtimeo_ms` timeout; `SOCK_DGRAM`: spin-poll for incoming UDP on `sock->udp_port`
- [ ] `kern_close(fd)`: `SOCK_STREAM`: `tcp_close(sock->tcp_conn)`; `SOCK_DGRAM`: `udp_unregister_handler(sock->udp_port)`; `sock_free(fd)`
- [ ] `AF_INET`, `SOCK_STREAM`, `SOCK_DGRAM`, `IPPROTO_TCP`, `IPPROTO_UDP` constants in `socket.h`
- [ ] Commit: `"net/socket: kernel socket layer -- AF_INET STREAM/DGRAM, connect/send/recv/close, 64-fd table"`

## 6. Server Sockets `[Sonnet]`

`bind(fd, port)` assigns a local port. `listen(fd, backlog)` enters TCP LISTEN. `accept(fd, &remote_ip, &remote_port)` blocks until a client SYN completes the handshake and returns a new connected fd.

**Files:** `src/kernel/net/socket.c` (extend)

> [!NOTE]
> Backlog queue: a `tcp_connection*` ring buffer of `backlog` entries in the listening socket's `socket_t`. When `tcp_accept()` (from TODO-01) returns an ESTABLISHED connection, push it onto the queue. `kern_accept()` pops the head of the queue (spin-polling if empty). `SO_REUSEADDR`: if a previous socket used the same port and is now in `TIME_WAIT`, allow binding. Server fd lifecycle: `kern_bind()` sets `sock->local_port`; `kern_listen()` calls `tcp_listen(local_port)` and stores the LISTEN connection; `kern_accept()` calls `tcp_accept(listener)` to get an ESTABLISHED connection and creates a *new* socket fd for it (leaving the original listener available for further accepts).

- [ ] `kern_bind(fd, local_port)` → 0 or -EADDRINUSE: check no other socket owns the port (unless `so_reuseaddr`); set `sock->local_port`; for `SOCK_DGRAM`: `udp_register_handler(local_port, ...)`
- [ ] `kern_listen(fd, backlog)` → 0 or -errno: validate `SOCK_STREAM` and bound; `tcp_listen(sock->local_port)` → store listener connection; init backlog ring buffer `sock->backlog[backlog]`; set `sock->state = SOCK_LISTENING`
- [ ] `kern_accept(fd, &remote_ip_out, &remote_port_out)` → new_fd or -errno: spin-poll `tcp_accept(listener)` up to `so_rcvtimeo_ms`; on ESTABLISHED conn: `sock_alloc()` new fd; populate with new TCP conn; fill `remote_ip/port`; return new fd
- [ ] Backlog integration: when a SYN arrives for a LISTEN-state `tcp_connection`, the state machine (TODO-01 §2) completes the handshake automatically; `kern_accept()` picks up the ESTABLISHED connection from `tcp_accept()`
- [ ] `SOMAXCONN = 16` constant; `backlog` parameter clamped to `SOMAXCONN`
- [ ] Commit: `"net/socket: server sockets -- bind/listen/accept, backlog queue, SO_REUSEADDR, accept fd lifecycle"`

## 7. Socket Options + Syscalls `[Sonnet]`

`setsockopt`/`getsockopt` for `SO_REUSEADDR`, `SO_RCVTIMEO`, `SO_SNDBUF`. Add 8 new syscall numbers (39–46) to `syscall.h`. Dispatch in `syscall.c`. User-mode wrappers in `user/lib/socket.c`.

**Files:** `src/kernel/net/socket.c` (extend), `include/kernel/sched/syscall.h` (extend), `src/kernel/sched/syscall.c` (extend), `user/lib/socket.c` (new)

> [!NOTE]
> Syscall numbers (appended after existing `SYS_MUNMAP=38`): `SYS_SOCKET=39`, `SYS_CONNECT=40`, `SYS_SEND=41`, `SYS_RECV=42`, `SYS_BIND=43`, `SYS_LISTEN=44`, `SYS_ACCEPT=45`, `SYS_SELECT=46`, `SYS_DNS=47`. `SO_RCVTIMEO` is passed as a millisecond timeout (not a `timeval`); `SO_SNDBUF` clamps `tcp_send()` calls to the specified buffer size. `setsockopt` level is `SOL_SOCKET=1`; option names: `SO_REUSEADDR=2`, `SO_RCVTIMEO=20`, `SO_SNDBUF=7`. User-mode wrappers use `INT 0x80` with syscall number in `RAX` and arguments in `RDI`, `RSI`, `RDX`, `R10`, `R8` (up to 5 args).

- [ ] `kern_setsockopt(fd, level, optname, optval, optlen)`: `SOL_SOCKET` + `SO_REUSEADDR` → `sock->so_reuseaddr`; `SO_RCVTIMEO` → `sock->so_rcvtimeo_ms`; `SO_SNDBUF` → `sock->so_sndbuf`; return 0 or -EINVAL
- [ ] `kern_getsockopt(fd, level, optname, optval_out, &optlen_out)`: reverse of above
- [ ] Add to `syscall.h`: `SYS_SOCKET=39` through `SYS_DNS=47`; add `SYS_SETSOCKOPT=48`, `SYS_GETSOCKOPT=49`
- [ ] Dispatch in `syscall.c`: 9 new `case` entries; validate fd range; call `kern_*()` functions; marshal return value
- [ ] `user/lib/socket.c`: `socket()`, `connect()`, `send()`, `recv()`, `bind()`, `listen()`, `accept()`, `close()` -- each a single `syscall(SYS_*, ...)` inline; `getaddrinfo(hostname, service, hints, &res)` wraps `SYS_DNS` + fills `addrinfo` struct; `htons()`/`ntohs()`/`htonl()`/`ntohl()` inline byte-swap helpers
- [ ] `user/include/socket.h`: `AF_INET`, `SOCK_STREAM`, `SOCK_DGRAM`, `SOL_SOCKET`, `SO_*`, `struct sockaddr_in { sin_family, sin_port, sin_addr }`, `struct addrinfo { ai_family, ai_socktype, ai_addr, ai_addrlen, *ai_next }`
- [ ] Commit: `"net/socket: setsockopt/getsockopt, syscalls 39–49, user/lib/socket.c wrappers, getaddrinfo"`

## 8. `select()` `[Opus]`

Block on up to 64 socket fds simultaneously. Bitmask-based readiness: data-ready (RECV), writable (SEND), and error. Millisecond timeout. Wake as soon as any fd becomes ready.

**Files:** `src/kernel/net/socket.c` (extend)

> [!NOTE]
> This is `[Opus]` -- `select()` requires a novel wait mechanism: the call must atomically check all fds, then sleep, then re-check on any network event. Strategy: introduce a per-socket `data_ready` flag (set by `tcp_handle()`/`udp_handle()` when data arrives); `kern_select()` scans all fds in the input set, builds a ready bitmask, returns it immediately if any are ready; otherwise calls `ksleep(1)` and re-scans, up to `timeout_ms` iterations. Wakeup on write-readiness: a `SOCK_STREAM` fd is write-ready when `tcp_conn->state == ESTABLISHED` and `cwnd > 0`. Error condition: fd is in error set if `tcp_conn->state == CLOSED` and no data was received cleanly.

- [ ] `kern_select(nfds, read_fds, write_fds, except_fds, timeout_ms)` → ready count or -errno:
  - Validate all fd bits ≤ 63; validate `nfds ≤ 64`
  - `deadline = now_ms() + timeout_ms`
  - Loop: scan `read_fds` → for each fd bit: if `sock->tcp_conn->recv_head != recv_tail` (data) OR `sock->state == SOCK_LISTENING && backlog_count > 0`: set bit in `out_read`; scan `write_fds` → if `ESTABLISHED && cwnd > 0`: set bit in `out_write`; if any bit set: return ready count
  - If `now_ms() >= deadline`: return 0 (timeout)
  - `ksleep(1)`; loop
- [ ] `fd_set` type: `uint64_t` (bitmask for fds 0–63); `FD_ZERO`, `FD_SET(fd, set)`, `FD_CLR`, `FD_ISSET` macros in `socket.h`
- [ ] `data_ready` flag in `socket_t`: set by `tcp_handle()` when data pushed to recv buffer; cleared by `kern_recv()`
- [ ] `SYS_SELECT` syscall (number 46): args `(nfds, read_ptr, write_ptr, except_ptr, timeout_ms)`; pointers validated before dereference
- [ ] User-mode `select()` wrapper in `user/lib/socket.c`: thin `syscall(SYS_SELECT, ...)`
- [ ] Commit: `"net/socket: select() -- 64-fd bitmask, data-ready + writable scan, ms timeout, FD_SET macros"`

---

## OS Comparison


| ⭐   | Feature                                  | 🪟 Win11                                  | 🐧 Linux                                  | 🚀 Impossible OS                          |
| --- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- |
| 💎   | DNS query builder                        | ✅ `dnsapi.dll` + `dns.exe` resolver; kernel | ✅ `net/dns/` in kernel; `glibc` resolver | ⬜ §1 -- kernel-native `dns_build_query()` + `udp_register_handler()` reply |
| 💎   | DNS response parser                      | ✅ Full RFC 1035 + EDNS0                  | ✅ `net/dns_resolve.c`; compression pointer handling | ⬜ §2 -- pointer depth cap 8; RCODE       |
| 💎   | DNS LRU cache                            | ✅ DNS Client Service cache; configurable | ✅ `nscd` or `systemd-resolved` (userspace); no | ⬜ §3 -- in-kernel 64-entry LRU; `nslookup` shell |
| 💎   | AAAA query + dual A+AAAA resolve, AAAA preference | ✅ Full IPv6 DNS in `dnsapi.dll`;         | ✅ `getaddrinfo()` prefers AAAA; kernel resolves | ⬜ §4 -- AAAA query stub; dual resolve    |
| 💎   | Kernel socket layer                      | ✅ `afd.sys` (Ancillary Function Driver); Winsock2 | ✅ `net/socket.c`; full BSD socket API    | ⬜ §5 -- direct kernel functions; 64-fd table |
| 💎   | Server sockets                           | ✅ `afd.sys`; full Winsock server socket  | ✅ `net/socket.c`; `SOMAXCONN`, backlog, `SO_REUSEADDR` | ⬜ §6 -- backlog ring buffer; `SOMAXCONN=16`; `kern_accept()` |
| 💎   | Socket syscalls 39–49 + `user/lib/socket.c` wrappers + `getaddrinfo` | ✅ Winsock2 `WSA*` functions (kernel +    | ✅ glibc `socket()` → `syscall(SYS_socket, ...)` | ⬜ §7 -- `INT 0x80` socket syscalls; `user/lib/socket.c` |
| 💎   | `select()`                               | ✅ `select()` + `WSAPoll()` in Winsock2;  | ✅ `select()` + `poll()` + `epoll()`      | ⬜ §8 -- 64-fd `uint64_t` bitmask; 1 ms   |

> **After §1–§8:** Impossible OS has a hostname-based, file-descriptor–driven networking API sufficient for every application protocol: DNS resolution, TCP clients and servers, UDP sockets, and multiplexed I/O via `select()`. Every higher-level TODO (TLS, HTTP, SSH) is unblocked. The in-kernel DNS LRU cache (§3) is a `⭐` advantage over Linux which handles DNS caching only in userspace daemons.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] DNS query: `dns_resolve("google.com", &ip)` → non-zero IP returned within 3 s; serial log shows `[DNS] Query id=XXXX` and `[DNS] Response ip=...`
- [ ] DNS compression: resolve a hostname whose response uses pointer compression (`0xC0xx` offset); correct IP returned
- [ ] DNS cache: resolve same hostname twice; second call returns instantly without sending a UDP packet; `nslookup google.com` prints `google.com → W.X.Y.Z (TTL=Ns, cached=no)` then `(cached=yes)` on repeat
- [ ] DNS timeout: configure an unreachable DNS server; `dns_resolve()` returns -ETIMEDOUT after 3 s
- [ ] `SOCK_STREAM` client: `fd = socket(AF_INET, SOCK_STREAM, 0); connect(fd, server_ip, 80); send(fd, "GET / HTTP/1.0\r\n\r\n", ...); recv(fd, buf, 4096)` → receives HTTP response; `close(fd)` cleanly
- [ ] `SOCK_DGRAM`: `fd = socket(AF_INET, SOCK_DGRAM, 0); bind(fd, 5000); send(fd, ...)` → packet delivered; `recv()` returns data
- [ ] Server socket: `bind(fd, 1234); listen(fd, 5); accept(fd, &ip, &port)` blocks until QEMU `nc` connects; new_fd received and usable for `send()`/`recv()`
- [ ] `SO_RCVTIMEO`: `setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, 500)` → `recv()` on idle connection returns -ETIMEDOUT after 500 ms
- [ ] `select()`: two sockets -- one with data, one idle; `select(64, read_set, ...)` returns 1 and sets bit for the data socket only
- [ ] Syscall: user-mode `hello.exe` calls `socket(AF_INET, SOCK_STREAM, 0)` via `INT 0x80` `SYS_SOCKET=39`; returns fd ≥ 3; subsequent `connect(fd, ip, port)` succeeds
- [ ] Commit: `"net: complete DNS resolver + BSD socket API -- dns_resolve, socket/connect/send/recv, select, syscalls 39–49"`
