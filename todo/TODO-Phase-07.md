# Phase 07 — Networking (Advanced)

> **Goal:** Evolve the basic UDP/ICMP network stack into full internet capability:
> TCP connections, DNS name resolution, BSD sockets API, HTTP/HTTPS clients, packet
> firewall, and certificate trust — enabling Impossible OS applications to access
> the modern internet.

---

## 1. TCP Protocol
> *Research: [01_internet_wireless.md](research/phase_07_networking/01_internet_wireless.md) § TCP*

### 1.1 TCP Header & Checksum

- [ ] Create `src/kernel/net/tcp.c` and `include/tcp.h`
- [ ] Define `struct tcp_header` (src_port, dst_port, seq_num, ack_num, data_offset, flags, window, checksum, urgent_ptr)
- [ ] Define TCP flag constants: `TCP_SYN`, `TCP_ACK`, `TCP_FIN`, `TCP_RST`, `TCP_PSH`
- [ ] Implement TCP checksum calculation (pseudo-header + TCP header + payload)
- [ ] Route TCP packets in `ip.c`: protocol number 6 → `tcp_receive()`
- [ ] Commit: `"net: TCP header parsing and checksum"`

### 1.2 TCP Connection State Machine

- [ ] Define `struct tcp_connection` (local/remote IP+port, seq/ack numbers, state, recv buffer)
- [ ] Connection table: `MAX_TCP_CONNECTIONS` (32) entries
- [ ] Implement TCP states: CLOSED, LISTEN, SYN_SENT, SYN_RECEIVED, ESTABLISHED, FIN_WAIT_1, FIN_WAIT_2, CLOSE_WAIT, LAST_ACK, TIME_WAIT
- [ ] State transitions on:
  - [ ] Active open: CLOSED → SYN_SENT (send SYN)
  - [ ] Receive SYN+ACK: SYN_SENT → ESTABLISHED (send ACK)
  - [ ] Active close: ESTABLISHED → FIN_WAIT_1 (send FIN)
  - [ ] Receive FIN: ESTABLISHED → CLOSE_WAIT (send ACK)
  - [ ] Receive RST: any state → CLOSED
- [ ] Commit: `"net: TCP connection state machine"`

### 1.3 TCP API

- [ ] Implement `tcp_connect(remote_ip, remote_port)` — 3-way handshake (SYN → SYN+ACK → ACK)
- [ ] Implement `tcp_send(conn, data, len)` — send data with PSH+ACK, properly sequenced
- [ ] Implement `tcp_recv(conn, buf, max_len)` — read from receive ring buffer
- [ ] Implement `tcp_close(conn)` — graceful close (FIN → ACK)
- [ ] Receive ring buffer: per-connection circular buffer (4 KB default)
- [ ] ACK incoming data: update ack number, send ACK packet
- [ ] Handle incoming data: write to recv buffer, wake waiting process
- [ ] Allocate ephemeral ports (49152–65535) for outbound connections
- [ ] Commit: `"net: TCP connect, send, recv, close"`

### 1.4 TCP Robustness

- [ ] Retransmission timer: resend unacked segments after timeout (1–3 seconds initial)
- [ ] Sequence number validation: drop out-of-window packets
- [ ] RST handling: abort connection on unexpected RST
- [ ] *(Stretch)* Nagle's algorithm: coalesce small writes
- [ ] *(Stretch)* Congestion window (slow start / congestion avoidance)
- [ ] *(Stretch)* Out-of-order reassembly (hold queue)
- [ ] *(Stretch)* Window scaling option (for large transfers)
- [ ] Test: TCP connect to an HTTP server, send GET request, receive response
- [ ] Commit: `"net: TCP retransmission and robustness"`

---

## 2. DNS Resolver
> *Research: [01_internet_wireless.md](research/phase_07_networking/01_internet_wireless.md) § DNS*

### 2.1 DNS Query

- [ ] Create `src/kernel/net/dns.c` and `include/dns.h`
- [ ] Define `struct dns_header` (id, flags, qdcount, ancount, nscount, arcount)
- [ ] Implement hostname encoding: `"www.google.com"` → length-prefixed labels (`\x03www\x06google\x03com\x00`)
- [ ] Build standard query packet: header + question (QTYPE=A, QCLASS=IN)
- [ ] Set transaction ID (random 16-bit)
- [ ] Send via UDP to DNS server IP (port 53) — use DHCP-provided DNS address
- [ ] Commit: `"net: DNS query builder"`

### 2.2 DNS Response Parser

- [ ] Parse response header: check ID matches, RCODE == 0 (no error), ancount > 0
- [ ] Skip question section (re-parse encoded name + type + class)
- [ ] Parse answer records: handle name compression (pointer `0xC0xx`)
- [ ] Extract A record (type 1): 4-byte IPv4 address
- [ ] Implement `dns_resolve(hostname, ip_out)` — blocking: send query, wait for response (timeout 3s)
- [ ] Commit: `"net: DNS response parser"`

### 2.3 DNS Cache

- [ ] Cache resolved hostnames → IPs (LRU, 64 entries)
- [ ] Respect TTL from DNS response (expire entries)
- [ ] Check cache before sending network query
- [ ] Shell command: `nslookup <hostname>` — display resolved IP
- [ ] Test: resolve `google.com`, `time.google.com`, `example.com`
- [ ] Commit: `"net: DNS caching"`

---

## 3. BSD Sockets API
> *Research: [01_internet_wireless.md](research/phase_07_networking/01_internet_wireless.md) § Sockets*

### 3.1 Kernel Socket Layer

- [ ] Create `src/kernel/net/socket.c` and `include/socket.h`
- [ ] Define socket types: `SOCK_STREAM` (TCP), `SOCK_DGRAM` (UDP)
- [ ] Socket file descriptor table (per-process, or global for now)
- [ ] Implement `socket(domain, type, protocol)` — allocate socket fd
- [ ] Implement `connect(fd, ip, port)` — TCP connect or UDP set peer
- [ ] Implement `send(fd, buf, len)` — TCP send or UDP send
- [ ] Implement `recv(fd, buf, len)` — TCP recv or UDP recv (blocking)
- [ ] Implement `close(fd)` — TCP close or free UDP socket
- [ ] Commit: `"net: BSD sockets API (kernel)"`

### 3.2 Server Sockets

- [ ] Implement `bind(fd, port)` — bind socket to a local port
- [ ] Implement `listen(fd, backlog)` — enter LISTEN state (TCP)
- [ ] Implement `accept(fd, remote_ip, remote_port)` — accept incoming connection
- [ ] Commit: `"net: server sockets (bind/listen/accept)"`

### 3.3 Socket Syscalls

- [ ] Add syscalls: `SYS_SOCKET` (18), `SYS_CONNECT` (19), `SYS_SEND` (20), `SYS_RECV` (21), `SYS_BIND` (22), `SYS_LISTEN` (23), `SYS_ACCEPT` (24), `SYS_DNS` (25)
- [ ] Implement syscall handlers in `syscall.c`
- [ ] Create user-mode socket library in `user/lib/socket.c`
- [ ] Implement `getaddrinfo(hostname, ip_out)` — wrapper around `SYS_DNS`
- [ ] Commit: `"net: socket syscalls and user library"`

---

## 4. HTTP Client
> *Research: [01_internet_wireless.md](research/phase_07_networking/01_internet_wireless.md) § HTTP*

### 4.1 URL Parser

- [ ] Create `src/kernel/net/http.c` and `include/http.h`
- [ ] Parse URL: scheme (`http://`), hostname, port (default 80), path
- [ ] Handle: `http://example.com/`, `http://example.com:8080/path`, `https://...`
- [ ] Commit: `"net: URL parser"`

### 4.2 HTTP GET

- [ ] Implement `http_get(url, response, max_len)`:
  - [ ] Parse URL → hostname + port + path
  - [ ] `dns_resolve(hostname)` → IP
  - [ ] TCP connect to IP:port
  - [ ] Send: `"GET /path HTTP/1.1\r\nHost: hostname\r\nConnection: close\r\n\r\n"`
  - [ ] Receive full response into buffer
  - [ ] TCP close
- [ ] Parse response: status line (200 OK, 404, etc.), headers, body
- [ ] Return body content to caller
- [ ] Commit: `"net: HTTP GET client"`

### 4.3 HTTP POST

- [ ] Implement `http_post(url, content_type, body, body_len, response, max_len)`
- [ ] Send Content-Length and Content-Type headers
- [ ] *(Stretch)* Chunked transfer encoding support
- [ ] Commit: `"net: HTTP POST client"`

### 4.4 Shell Integration

- [ ] Shell command: `wget <url>` — download to file or print to stdout
- [ ] Shell command: `curl <url>` — alias for wget (print response)
- [ ] Show download progress (bytes received / content-length)
- [ ] Commit: `"shell: wget/curl commands"`

---

## 5. TLS / HTTPS
> *Research: [01_internet_wireless.md](research/phase_07_networking/01_internet_wireless.md) § HTTPS, [03_certificate_store.md](research/phase_07_networking/03_certificate_store.md)*

### 5.1 TLS Library Port

- [ ] Choose library: **BearSSL** (MIT, ~30K lines) or **Mbed TLS** (Apache 2.0, ~60K lines)
- [ ] Port to Impossible OS freestanding environment:
  - [ ] Redirect malloc/free to kmalloc/kfree
  - [ ] Provide time function (`time_now()`)
  - [ ] Provide random number source (RDRAND or PIT-based entropy)
- [ ] Compile as static library: `libtls.a`
- [ ] Commit: `"libs: TLS library port (BearSSL/Mbed TLS)"`

### 5.2 Certificate Store

- [ ] Download **Mozilla CA bundle** (MPL 2.0, ~130 root CAs, ~200 KB)
- [ ] Place at `resources/certs/ca-bundle.crt` (PEM format)
- [ ] Install to `C:\Impossible\System\Certs\ca-bundle.crt`
- [ ] Implement `tls_load_ca_bundle(path)` — parse PEM, load into TLS library trust store
- [ ] Implement `tls_verify_cert(cert_der, len)` — verify server certificate against CA bundle
- [ ] Update CA bundle via OS updates
- [ ] Commit: `"net: certificate store + CA bundle"`

### 5.3 HTTPS Client

- [ ] Implement TLS handshake over TCP connection
- [ ] Implement `https_get(url, response, max_len)` — same as HTTP but with TLS wrapper
- [ ] Handle TLS alerts and errors gracefully
- [ ] Certificate validation: reject self-signed unless explicitly trusted
- [ ] `wget` and `curl` commands: auto-detect `https://` URLs → use TLS
- [ ] Test: `wget https://example.com/`
- [ ] Commit: `"net: HTTPS client"`

---

## 6. Firewall
> *Research: [02_firewall.md](research/phase_07_networking/02_firewall.md)*

### 6.1 Packet Filter Engine

- [ ] Create `src/kernel/net/firewall.c` and `include/firewall.h`
- [ ] Define `fw_rule_t` (src_ip, dst_ip, src_port, dst_port, protocol, action, direction)
- [ ] Rule table: ordered list of rules (first match wins)
- [ ] Implement `fw_add_rule(rule)` — append rule
- [ ] Implement `fw_remove_rule(rule_id)` — remove by index
- [ ] Implement `fw_check(ip_header, direction)` — evaluate packet against rules, return ALLOW/BLOCK
- [ ] Implement `fw_set_default(action)` — default policy (ALLOW or BLOCK)
- [ ] Commit: `"net: firewall packet filter engine"`

### 6.2 Firewall Hook

- [ ] Hook `fw_check()` in `ip.c`:
  - [ ] Inbound: check before passing to TCP/UDP/ICMP handlers
  - [ ] Outbound: check before sending via Ethernet
- [ ] Drop blocked packets silently (no response)
- [ ] Log blocked packets to serial (debug)
- [ ] Commit: `"net: firewall hook in IP layer"`

### 6.3 Default Rules

- [ ] Allow all outbound traffic
- [ ] Block all inbound except established TCP connections (stateful)
- [ ] Allow inbound ICMP (ping)
- [ ] Allow inbound DHCP (ports 67/68)
- [ ] Allow inbound DNS response (port 53)
- [ ] Allow inbound NTP response (port 123)
- [ ] Commit: `"net: firewall default rules"`

### 6.4 Firewall Management

- [ ] Store rules in Codex: `System\Network\Firewall\Enabled`, `System\Network\Firewall\Rules`
- [ ] `firewall.spl` settings panel applet — toggle firewall, view/add/remove rules
- [ ] Shell command: `fw list` — show all rules
- [ ] Shell command: `fw add <direction> <protocol> <port> <action>` — add rule
- [ ] Shell command: `fw enable` / `fw disable` — toggle firewall
- [ ] Commit: `"net: firewall management"`

---

## 7. Virtio-Net Driver
> *Research: [01_internet_wireless.md](research/phase_07_networking/01_internet_wireless.md) § WiFi/Drivers*

### 7.1 Virtio Network Driver

- [ ] Create `src/kernel/drivers/virtio_net.c` and `include/virtio_net.h`
- [ ] Detect virtio-net device via PCI (vendor `0x1AF4`, device `0x1000`)
- [ ] Map MMIO BAR, negotiate features (VIRTIO_NET_F_MAC, etc.)
- [ ] Initialize receive + transmit virtqueues
- [ ] Implement `virtio_net_send(packet, len)` — submit to TX virtqueue
- [ ] Implement virtio-net IRQ handler — process RX used ring → Ethernet receive
- [ ] Read MAC address from device config
- [ ] Register with Ethernet layer as NIC
- [ ] QEMU flag: `-device virtio-net-pci,netdev=net0 -netdev user,id=net0`
- [ ] Test: DHCP + ping over virtio-net
- [ ] Commit: `"drivers: virtio-net NIC driver"`

---

## 8. Win32 Winsock Mapping
> *Research: [01_internet_wireless.md](research/phase_07_networking/01_internet_wireless.md) § Winsock*

### 8.1 ws2_32.dll Stubs

- [ ] Create `src/kernel/win32/ws2_32.c`
- [ ] `WSAStartup(version, data)` → no-op (return 0)
- [ ] `WSACleanup()` → no-op
- [ ] `socket(af, type, protocol)` → `sys_socket()`
- [ ] `connect(s, addr, len)` → `sys_connect()`
- [ ] `send(s, buf, len, flags)` → `sys_send()`
- [ ] `recv(s, buf, len, flags)` → `sys_recv()`
- [ ] `closesocket(s)` → `sys_close()`
- [ ] `bind(s, addr, len)` → `sys_bind()`
- [ ] `listen(s, backlog)` → `sys_listen()`
- [ ] `accept(s, addr, len)` → `sys_accept()`
- [ ] `gethostbyname(name)` → `dns_resolve()` wrapper
- [ ] `htons()` / `ntohs()` / `htonl()` / `ntohl()` — byte-order macros
- [ ] Add to builtin DLL stub table
- [ ] Commit: `"win32: Winsock (ws2_32.dll) stubs"`

---

## 9. Agent-Recommended Additions

> Items not in the research files but important for a complete networking subsystem.

### 9.1 Network Interface Manager

- [ ] Abstraction over multiple NICs (RTL8139, virtio-net, future drivers)
- [ ] `struct net_interface` (name, mac, ipv4, netmask, gateway, dns, mtu, driver)
- [ ] `netif_register(iface)` — add interface
- [ ] `netif_get_default()` — return primary interface for routing
- [ ] `netif_set_ip(iface, ip, mask, gateway)` — manual IP configuration
- [ ] Support for multiple simultaneous interfaces
- [ ] Commit: `"net: network interface manager"`

### 9.2 Connection Tracking (Stateful Firewall)

- [ ] Track established TCP connections: hash table of (src_ip, src_port, dst_ip, dst_port)
- [ ] Auto-allow inbound packets belonging to established outbound connections
- [ ] Expire entries after connection close or timeout (300s)
- [ ] Commit: `"net: stateful connection tracking"`

### 9.3 Network Status API

- [ ] `net_status()` — connected/disconnected, link speed
- [ ] `net_stats()` — packets in/out, bytes in/out, errors, drops
- [ ] Expose via system tray icon: 🌐 connected / ⚠ disconnected
- [ ] Used by: `ifconfig` command, Settings → Network, Task Manager performance tab
- [ ] Commit: `"net: network status and statistics API"`

### 9.4 Loopback Interface

- [ ] Virtual interface for `127.0.0.1` (localhost)
- [ ] Packets to 127.0.0.0/8 → loop back directly to receive path
- [ ] Required for local server testing
- [ ] Commit: `"net: loopback interface (127.0.0.1)"`

### 9.5 WiFi Framework (Future)

- [ ] *(Stretch)* 802.11 management frame parsing (beacons, probe requests)
- [ ] *(Stretch)* WiFi scan → list SSIDs, signal strength, security
- [ ] *(Stretch)* WPA2 4-way handshake (requires HMAC-SHA1, AES, PBKDF2 from monocypher)
- [ ] *(Stretch)* WiFi association: select network → authenticate → associate → DHCP
- [ ] *(Stretch)* One hardware driver: USB RTL8188 or QEMU virtio-wifi (if available)

---

## Priority Order

| Priority | Section | Reason |
|----------|---------|--------|
| 🔴 P0 | 1.1–1.3 TCP Core | Foundation for HTTP, HTTPS, and all internet protocols |
| 🔴 P0 | 2.1–2.2 DNS Resolver | Required to connect by hostname instead of IP |
| 🔴 P0 | 3.1 Sockets API (kernel) | Standard API for all networking apps |
| 🟠 P1 | 4.1–4.2 HTTP GET | Web requests — enables wget, API calls |
| 🟠 P1 | 3.3 Socket Syscalls | User-mode networking |
| 🟠 P1 | 9.1 Network Interface Manager | Multi-NIC support |
| 🟠 P1 | 9.4 Loopback Interface | Localhost networking |
| 🟡 P2 | 6.1–6.3 Firewall | Security — packet filtering |
| 🟡 P2 | 7. Virtio-Net Driver | Modern paravirtual NIC (faster than RTL8139) |
| 🟡 P2 | 1.4 TCP Robustness | Retransmission, reliability |
| 🟡 P2 | 2.3 DNS Cache | Performance |
| 🟡 P2 | 4.4 Shell wget/curl | User-facing download commands |
| 🟢 P3 | 5.1–5.2 TLS + Certificates | HTTPS support |
| 🟢 P3 | 8. Winsock Stubs | Win32 network compatibility |
| 🟢 P3 | 9.2 Connection Tracking | Stateful firewall |
| 🟢 P3 | 9.3 Network Status | Monitoring and system tray |
| 🟢 P3 | 4.3 HTTP POST | Full HTTP client |
| 🔵 P4 | 5.3 HTTPS Client | Secure web access |
| 🔵 P4 | 6.4 Firewall Management | GUI + shell tools |
| 🔵 P4 | 9.5 WiFi Framework | Long-term — real hardware only |
