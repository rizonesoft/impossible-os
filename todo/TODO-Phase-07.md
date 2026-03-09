# Phase 07 — Networking (Advanced)

> **Goal:** Evolve the basic UDP/ICMP network stack into full internet capability:
> TCP connections, DNS name resolution, BSD sockets API, HTTP/HTTPS clients, packet
> firewall, and certificate trust — enabling Impossible OS applications to access
> the modern internet.

---

## 1. TCP Protocol
> *Research: [01_internet_wireless.md](research/phase_07_networking/01_internet_wireless.md) § TCP*

### 1.1 TCP Header & Checksum

**Prompt:** TCP is protocol number 6 in the IP layer. The TCP header is 20 bytes minimum: src_port(16), dst_port(16), seq_num(32), ack_num(32), data_offset(4 bits) + reserved(3) + flags(9), window(16), checksum(16), urgent_ptr(16). The checksum covers a pseudo-header (src_ip, dst_ip, zero, protocol=6, TCP_length) + the full TCP header + payload — identical method to UDP checksum but mandatory. Route TCP packets by adding `case IP_PROTO_TCP:` in `ip_receive()` (analogous to the existing ICMP and UDP routing in `ip.c`). After completing all items, create `docs/architecture/tcp.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"net: TCP header parsing and checksum"`.


- [ ] Create `src/kernel/net/tcp.c` and `include/tcp.h`
- [ ] Define `struct tcp_header` (src_port, dst_port, seq_num, ack_num, data_offset, flags, window, checksum, urgent_ptr)
- [ ] Define TCP flag constants: `TCP_SYN`, `TCP_ACK`, `TCP_FIN`, `TCP_RST`, `TCP_PSH`
- [ ] Implement TCP checksum calculation (pseudo-header + TCP header + payload)
- [ ] Route TCP packets in `ip.c`: protocol number 6 → `tcp_receive()`
- [ ] Commit: `"net: TCP header parsing and checksum"`

### 1.2 TCP Connection State Machine

**Prompt:** Each TCP connection has a state from the RFC 793 state diagram. The connection table holds `MAX_TCP_CONNECTIONS` (32) entries, each with local/remote IP+port, sequence/acknowledgment numbers, state, and a receive ring buffer. Key transitions: Active open → SYN_SENT (send SYN), receive SYN+ACK → ESTABLISHED (send ACK), active close → FIN_WAIT_1 (send FIN). RST resets to CLOSED from any state. Implement all 11 TCP states for full correctness. Each incoming packet is matched to a connection by (local_ip, local_port, remote_ip, remote_port) tuple. After completing all items, update `docs/architecture/tcp.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"net: TCP connection state machine"`.


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

**Prompt:** `tcp_connect` performs the 3-way handshake: send SYN (with initial sequence number from a counter or random), wait for SYN+ACK, send ACK, transition to ESTABLISHED. `tcp_send` wraps data in a TCP segment with PSH+ACK flags and the current sequence number, then increments the sequence number by the data length. `tcp_recv` reads from a per-connection circular buffer (4 KB) that incoming data is written into by the receive handler. `tcp_close` sends FIN and transitions through the close states. Allocate ephemeral source ports from range 49152-65535 (maintain a next-port counter). After completing all items, update `docs/architecture/tcp.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"net: TCP connect, send, recv, close"`.


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

**Prompt:** Reliability is TCP's core contract. The retransmission timer re-sends unacknowledged segments after a timeout (start at 1 second, double on each retry up to 3 retries, then abort). Validate incoming sequence numbers against the receive window — drop out-of-window packets. Handle RST by aborting the connection immediately. Stretch goals: Nagle's algorithm (coalesce small writes into one segment), congestion window (slow start doubles window each RTT until packet loss), out-of-order reassembly (hold queue for future-sequence packets), and window scaling for transfers >64KB. Test by connecting to an HTTP server, sending a GET request, and receiving the full response. After completing all items, update `docs/architecture/tcp.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"net: TCP retransmission and robustness"`.


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

**Prompt:** DNS queries are sent via UDP to port 53 on the DNS server (IP obtained from DHCP — use the existing `g_dns_server` from the DHCP client). The DNS packet has a 12-byte header followed by the question section. Encode the hostname by splitting on dots and prefixing each label with its length byte: `"www.google.com"` becomes `\x03www\x06google\x03com\x00`. Set QTYPE=1 (A record) and QCLASS=1 (Internet). Use a random 16-bit transaction ID to match responses. Send via the existing `udp_send()` from the network stack. After completing all items, create `docs/architecture/dns.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"net: DNS query builder"`.


- [ ] Create `src/kernel/net/dns.c` and `include/dns.h`
- [ ] Define `struct dns_header` (id, flags, qdcount, ancount, nscount, arcount)
- [ ] Implement hostname encoding: `"www.google.com"` → length-prefixed labels (`\x03www\x06google\x03com\x00`)
- [ ] Build standard query packet: header + question (QTYPE=A, QCLASS=IN)
- [ ] Set transaction ID (random 16-bit)
- [ ] Send via UDP to DNS server IP (port 53) — use DHCP-provided DNS address
- [ ] Commit: `"net: DNS query builder"`

### 2.2 DNS Response Parser

**Prompt:** Parse the DNS response by first checking that the transaction ID matches the sent query and RCODE is 0 (no error). Skip the question section by re-parsing the encoded name (count length-prefixed labels until null terminator). Parse answer records: handle name compression pointers (first two bits = 11, remaining 14 bits = offset into packet). Extract A records (type 1) which contain a 4-byte IPv4 address. `dns_resolve(hostname, ip_out)` is a blocking call: send the query, then poll for a response with a 3-second timeout. After completing all items, update `docs/architecture/dns.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"net: DNS response parser"`.


- [ ] Parse response header: check ID matches, RCODE == 0 (no error), ancount > 0
- [ ] Skip question section (re-parse encoded name + type + class)
- [ ] Parse answer records: handle name compression (pointer `0xC0xx`)
- [ ] Extract A record (type 1): 4-byte IPv4 address
- [ ] Implement `dns_resolve(hostname, ip_out)` — blocking: send query, wait for response (timeout 3s)
- [ ] Commit: `"net: DNS response parser"`

### 2.3 DNS Cache

**Prompt:** Cache resolved hostname→IP mappings in a 64-entry LRU table. Each entry stores the hostname string, resolved IP, TTL from the DNS response, and a timestamp. On `dns_resolve()`, check the cache first — if found and not expired, return immediately without a network query. The `nslookup <hostname>` shell command calls `dns_resolve()` and prints the result. Test with well-known hostnames: `google.com`, `time.google.com`, `example.com`. After completing all items, update `docs/architecture/dns.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"net: DNS caching"`.


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

**Prompt:** The socket layer abstracts TCP and UDP into a uniform API. Each socket is a file descriptor in a global table. `socket(AF_INET, SOCK_STREAM, 0)` allocates a TCP socket, `socket(AF_INET, SOCK_DGRAM, 0)` allocates a UDP socket. `connect()` performs TCP 3-way handshake or sets the UDP peer address. `send()` and `recv()` dispatch to `tcp_send/recv` or `udp_send/recv` based on socket type. `close()` performs TCP graceful close or frees the UDP socket. This is the kernel-side implementation — user-mode syscalls come in §3.3. After completing all items, create `docs/architecture/sockets.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"net: BSD sockets API (kernel)"`.


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

**Prompt:** Server sockets enable incoming connections. `bind(fd, port)` assigns a local port to the socket. `listen(fd, backlog)` puts the socket into LISTEN state and creates a backlog queue for pending connections. `accept(fd, remote_ip, remote_port)` blocks until a SYN arrives, completes the handshake, creates a new connected socket, and returns its fd. The backlog queue holds up to `backlog` half-open connections. After completing all items, update `docs/architecture/sockets.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"net: server sockets (bind/listen/accept)"`.


- [ ] Implement `bind(fd, port)` — bind socket to a local port
- [ ] Implement `listen(fd, backlog)` — enter LISTEN state (TCP)
- [ ] Implement `accept(fd, remote_ip, remote_port)` — accept incoming connection
- [ ] Commit: `"net: server sockets (bind/listen/accept)"`

### 3.3 Socket Syscalls

**Prompt:** Expose the socket API to user-mode processes via syscalls: SYS_SOCKET (18), SYS_CONNECT (19), SYS_SEND (20), SYS_RECV (21), SYS_BIND (22), SYS_LISTEN (23), SYS_ACCEPT (24), SYS_DNS (25). Each syscall handler in `syscall.c` validates parameters and calls the kernel socket functions. Create a user-mode library `user/lib/socket.c` that provides `socket()`, `connect()`, `send()`, `recv()`, etc. as thin wrappers around `syscall()`. `getaddrinfo(hostname, ip_out)` wraps SYS_DNS. After completing all items, update `docs/architecture/sockets.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"net: socket syscalls and user library"`.


- [ ] Add syscalls: `SYS_SOCKET` (18), `SYS_CONNECT` (19), `SYS_SEND` (20), `SYS_RECV` (21), `SYS_BIND` (22), `SYS_LISTEN` (23), `SYS_ACCEPT` (24), `SYS_DNS` (25)
- [ ] Implement syscall handlers in `syscall.c`
- [ ] Create user-mode socket library in `user/lib/socket.c`
- [ ] Implement `getaddrinfo(hostname, ip_out)` — wrapper around `SYS_DNS`
- [ ] Commit: `"net: socket syscalls and user library"`

---

## 4. HTTP Client
> *Research: [01_internet_wireless.md](research/phase_07_networking/01_internet_wireless.md) § HTTP*

### 4.1 URL Parser

**Prompt:** Parse URLs into components: scheme ("http" or "https"), hostname, port (default 80 for HTTP, 443 for HTTPS), and path (default "/"). Handle forms: `http://example.com/`, `http://example.com:8080/path`, `https://host/path?query`. Store results in a `struct url` with string pointers and port integer. This parser is used by both HTTP and HTTPS clients. After completing all items, create `docs/architecture/http.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"net: URL parser"`.


- [ ] Create `src/kernel/net/http.c` and `include/http.h`
- [ ] Parse URL: scheme (`http://`), hostname, port (default 80), path
- [ ] Handle: `http://example.com/`, `http://example.com:8080/path`, `https://...`
- [ ] Commit: `"net: URL parser"`

### 4.2 HTTP GET

**Prompt:** `http_get(url, response, max_len)` is the primary web request function. Pipeline: parse URL → `dns_resolve(hostname)` → `tcp_connect(ip, port)` → send `"GET /path HTTP/1.1\r\nHost: hostname\r\nConnection: close\r\n\r\n"` → receive full response → `tcp_close()`. Parse the response: status line (e.g., "HTTP/1.1 200 OK"), header lines (terminated by `\r\n\r\n`), and body. Extract Content-Length for progress reporting. Return the body content to the caller. Handle common status codes: 200 (OK), 301/302 (redirect), 404 (not found). After completing all items, update `docs/architecture/http.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"net: HTTP GET client"`.


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

**Prompt:** `http_post(url, content_type, body, body_len, response, max_len)` sends a POST request with Content-Type and Content-Length headers followed by the body data. This enables form submissions and API calls. Stretch goal: chunked transfer encoding parsing (response body may come in `Transfer-Encoding: chunked` format where each chunk is preceded by its hex length). After completing all items, update `docs/architecture/http.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"net: HTTP POST client"`.


- [ ] Implement `http_post(url, content_type, body, body_len, response, max_len)`
- [ ] Send Content-Length and Content-Type headers
- [ ] *(Stretch)* Chunked transfer encoding support
- [ ] Commit: `"net: HTTP POST client"`

### 4.4 Shell Integration

**Prompt:** `wget <url>` downloads a URL to a file (use the last path component as the filename, or allow `-o filename`) or prints to stdout if no file specified. `curl <url>` is an alias that always prints to stdout. Show download progress: bytes received / Content-Length (from HTTP header) as a percentage and bytes/second. These commands use `http_get()` internally. After completing all items, update `docs/user/shell-commands.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"shell: wget/curl commands"`.


- [ ] Shell command: `wget <url>` — download to file or print to stdout
- [ ] Shell command: `curl <url>` — alias for wget (print response)
- [ ] Show download progress (bytes received / content-length)
- [ ] Commit: `"shell: wget/curl commands"`

---

## 5. TLS / HTTPS
> *Research: [01_internet_wireless.md](research/phase_07_networking/01_internet_wireless.md) § HTTPS, [03_certificate_store.md](research/phase_07_networking/03_certificate_store.md)*

### 5.1 TLS Library Port

**Prompt:** HTTPS requires a TLS implementation. BearSSL (~30K lines, MIT) is the recommended choice for a freestanding kernel — it has minimal dependencies and is designed for embedded systems. Port requirements: redirect `malloc/free` to `kmalloc/kfree`, provide `time_now()` for certificate validation, and provide a random number source (RDRAND instruction if available, otherwise PIT-based entropy pool). Compile as a static library `libtls.a` with `-ffreestanding` flags. After completing all items, create `docs/architecture/tls.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"libs: TLS library port (BearSSL/Mbed TLS)"`.


- [ ] Choose library: **BearSSL** (MIT, ~30K lines) or **Mbed TLS** (Apache 2.0, ~60K lines)
- [ ] Port to Impossible OS freestanding environment:
  - [ ] Redirect malloc/free to kmalloc/kfree
  - [ ] Provide time function (`time_now()`)
  - [ ] Provide random number source (RDRAND or PIT-based entropy)
- [ ] Compile as static library: `libtls.a`
- [ ] Commit: `"libs: TLS library port (BearSSL/Mbed TLS)"`

### 5.2 Certificate Store

**Prompt:** Download the Mozilla CA bundle (MPL 2.0, ~130 root CAs, ~200 KB PEM) which contains the trusted root certificates needed to verify HTTPS server certificates. Store at `resources/certs/ca-bundle.crt` in the source tree, install to `C:\Impossible\System\Certs\ca-bundle.crt` during first-boot setup. `tls_load_ca_bundle(path)` parses the PEM file (base64-encoded DER blocks between BEGIN/END CERTIFICATE markers) and loads each certificate into the TLS library's trust store. `tls_verify_cert()` validates a server's certificate chain against these trusted CAs. After completing all items, update `docs/architecture/tls.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"net: certificate store + CA bundle"`.


- [ ] Download **Mozilla CA bundle** (MPL 2.0, ~130 root CAs, ~200 KB)
- [ ] Place at `resources/certs/ca-bundle.crt` (PEM format)
- [ ] Install to `C:\Impossible\System\Certs\ca-bundle.crt`
- [ ] Implement `tls_load_ca_bundle(path)` — parse PEM, load into TLS library trust store
- [ ] Implement `tls_verify_cert(cert_der, len)` — verify server certificate against CA bundle
- [ ] Update CA bundle via OS updates
- [ ] Commit: `"net: certificate store + CA bundle"`

### 5.3 HTTPS Client

**Prompt:** `https_get()` works like `http_get()` but wraps the TCP connection in a TLS session. After `tcp_connect()`, perform the TLS handshake (ClientHello → ServerHello → Certificate verification → key exchange → encrypted communication). All subsequent send/recv calls go through the TLS encrypt/decrypt layer. The `wget` and `curl` commands auto-detect `https://` URLs and switch to the TLS path. Reject self-signed certificates unless the user explicitly adds them to the trust store. Test with `wget https://example.com/`. After completing all items, update `docs/architecture/tls.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"net: HTTPS client"`.


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

**Prompt:** The firewall uses a rule table (ordered list, first match wins). Each rule specifies: source/destination IP (or any), source/destination port (or any), protocol (TCP/UDP/ICMP/any), direction (inbound/outbound), and action (ALLOW/BLOCK). `fw_check(ip_header, direction)` iterates rules in order, returns ALLOW or BLOCK. If no rule matches, the default policy applies. Keep the rule table small (max 64 rules) and fast (linear scan is fine for a kernel firewall). After completing all items, create `docs/architecture/firewall.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"net: firewall packet filter engine"`.


- [ ] Create `src/kernel/net/firewall.c` and `include/firewall.h`
- [ ] Define `fw_rule_t` (src_ip, dst_ip, src_port, dst_port, protocol, action, direction)
- [ ] Rule table: ordered list of rules (first match wins)
- [ ] Implement `fw_add_rule(rule)` — append rule
- [ ] Implement `fw_remove_rule(rule_id)` — remove by index
- [ ] Implement `fw_check(ip_header, direction)` — evaluate packet against rules, return ALLOW/BLOCK
- [ ] Implement `fw_set_default(action)` — default policy (ALLOW or BLOCK)
- [ ] Commit: `"net: firewall packet filter engine"`

### 6.2 Firewall Hook

**Prompt:** Hook `fw_check()` into the IP layer at two points: inbound (in `ip_receive()` before dispatching to TCP/UDP/ICMP handlers) and outbound (in `ip_send()` before passing to Ethernet). Blocked packets are dropped silently (no ICMP unreachable response — this is standard stealth behavior). Log blocked packets to serial output for debugging. The hook must be extremely fast since it runs on every packet. After completing all items, update `docs/architecture/firewall.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"net: firewall hook in IP layer"`.


- [ ] Hook `fw_check()` in `ip.c`:
  - [ ] Inbound: check before passing to TCP/UDP/ICMP handlers
  - [ ] Outbound: check before sending via Ethernet
- [ ] Drop blocked packets silently (no response)
- [ ] Log blocked packets to serial (debug)
- [ ] Commit: `"net: firewall hook in IP layer"`

### 6.3 Default Rules

**Prompt:** Provide sensible default rules for a consumer OS: allow all outbound traffic (user-initiated connections should always work), block all inbound traffic except established TCP connections (stateful — requires connection tracking from §9.2), allow inbound ICMP (so ping works), allow inbound DHCP (UDP ports 67/68), allow inbound DNS responses (UDP port 53), and allow inbound NTP responses (UDP port 123). These defaults should be loaded at boot from Codex or hardcoded as fallback. After completing all items, update `docs/architecture/firewall.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"net: firewall default rules"`.


- [ ] Allow all outbound traffic
- [ ] Block all inbound except established TCP connections (stateful)
- [ ] Allow inbound ICMP (ping)
- [ ] Allow inbound DHCP (ports 67/68)
- [ ] Allow inbound DNS response (port 53)
- [ ] Allow inbound NTP response (port 123)
- [ ] Commit: `"net: firewall default rules"`

### 6.4 Firewall Management

**Prompt:** Persist firewall configuration in Codex: `System\Network\Firewall\Enabled` (BOOL) and `System\Network\Firewall\Rules` (serialized rule list). Create a `firewall.spl` settings applet (Phase 05 §4 SPL framework) with an enable/disable toggle and a rule list with add/remove buttons. Shell commands: `fw list` (show all rules with index numbers), `fw add <direction> <protocol> <port> <action>`, `fw remove <index>`, `fw enable/disable`. After completing all items, update `docs/architecture/firewall.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"net: firewall management"`.


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

**Prompt:** Virtio-net is the paravirtual NIC for QEMU/KVM — much faster than RTL8139 because it avoids emulating real hardware. Detect via PCI (vendor `0x1AF4`, device `0x1000`). Reuse the modern VirtIO MMIO transport from `virtio.c` (already used by virtio-blk). Initialize two virtqueues: RX (queue 0) and TX (queue 1). Pre-populate the RX queue with empty buffers. On TX: build an Ethernet frame, submit to the TX virtqueue via descriptor chain, kick the queue. On RX interrupt: process used buffers from the RX queue, pass complete Ethernet frames to `ethernet_receive()`. Read the MAC address from the device configuration space. Register as a NIC with the Ethernet layer. After completing all items, create `docs/architecture/virtio-net.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"drivers: virtio-net NIC driver"`.


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

**Prompt:** Winsock is Windows' socket API. Map each Winsock function to the corresponding kernel socket function: `WSAStartup/WSACleanup` are no-ops (our stack is always ready), `socket/connect/send/recv/closesocket/bind/listen/accept` map directly to the BSD socket calls from §3. `gethostbyname` wraps `dns_resolve()`. `htons/ntohs/htonl/ntohl` are byte-order conversion macros (x86 is little-endian, network is big-endian). Add all stubs to the builtin DLL stub table alongside existing Win32 stubs (Phase 10). After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"win32: Winsock (ws2_32.dll) stubs"`.


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

**Prompt:** Abstract multiple NICs (RTL8139, virtio-net, future drivers) behind a `struct net_interface`. Each interface has a name ("eth0", "eth1"), MAC address, IPv4 address, netmask, gateway, DNS server, MTU, and driver function pointers. `netif_register()` adds an interface. `netif_get_default()` returns the primary interface used for routing. `netif_set_ip()` allows manual IP configuration. The IP layer consults the default interface for the gateway and MAC when sending packets. This replaces the current global NIC state with a proper multi-NIC abstraction. After completing all items, create `docs/architecture/network-interfaces.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"net: network interface manager"`.


- [ ] Abstraction over multiple NICs (RTL8139, virtio-net, future drivers)
- [ ] `struct net_interface` (name, mac, ipv4, netmask, gateway, dns, mtu, driver)
- [ ] `netif_register(iface)` — add interface
- [ ] `netif_get_default()` — return primary interface for routing
- [ ] `netif_set_ip(iface, ip, mask, gateway)` — manual IP configuration
- [ ] Support for multiple simultaneous interfaces
- [ ] Commit: `"net: network interface manager"`

### 9.2 Connection Tracking (Stateful Firewall)

**Prompt:** Stateful connection tracking is essential for the firewall's default "block inbound except established" rule. Maintain a hash table of active connections keyed by (src_ip, src_port, dst_ip, dst_port). When an outbound TCP SYN or UDP packet is sent, create a tracking entry. When an inbound packet arrives, check if it matches an existing tracking entry — if so, auto-allow it regardless of firewall rules. Expire entries: TCP entries expire on FIN/RST or after 300s of inactivity; UDP entries expire after 60s. After completing all items, update `docs/architecture/firewall.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"net: stateful connection tracking"`.


- [ ] Track established TCP connections: hash table of (src_ip, src_port, dst_ip, dst_port)
- [ ] Auto-allow inbound packets belonging to established outbound connections
- [ ] Expire entries after connection close or timeout (300s)
- [ ] Commit: `"net: stateful connection tracking"`

### 9.3 Network Status API

**Prompt:** `net_status()` returns the current connection state (connected/disconnected) and link speed from the active NIC driver. `net_stats()` returns cumulative packet/byte counts (in/out) and error/drop counts. The system tray (Phase 04 §3.2) shows a network icon: connected (🌐) or disconnected (⚠️). These stats feed into: `ifconfig` shell command, Settings → Network applet, and Task Manager's Performance tab. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"net: network status and statistics API"`.


- [ ] `net_status()` — connected/disconnected, link speed
- [ ] `net_stats()` — packets in/out, bytes in/out, errors, drops
- [ ] Expose via system tray icon: 🌐 connected / ⚠ disconnected
- [ ] Used by: `ifconfig` command, Settings → Network, Task Manager performance tab
- [ ] Commit: `"net: network status and statistics API"`

### 9.4 Loopback Interface

**Prompt:** The loopback interface handles packets addressed to `127.0.0.0/8` (localhost). When `ip_send()` detects a destination in the 127.0.0.0/8 range, instead of passing the packet to the Ethernet layer, loop it directly back to `ip_receive()`. This enables local server testing (e.g., a web server on localhost:8080). Register loopback as a virtual network interface with name "lo" and IP 127.0.0.1. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"net: loopback interface (127.0.0.1)"`.


- [ ] Virtual interface for `127.0.0.1` (localhost)
- [ ] Packets to 127.0.0.0/8 → loop back directly to receive path
- [ ] Required for local server testing
- [ ] Commit: `"net: loopback interface (127.0.0.1)"`

### 9.5 WiFi Framework (Future)

**Prompt:** WiFi support is a long-term stretch goal requiring 802.11 management frame parsing (beacons for SSID discovery, probe requests), WPA2 authentication (4-way handshake using HMAC-SHA1, AES-CCMP, and PBKDF2 from a crypto library), and a hardware driver for a specific WiFi chipset. The most feasible approach in QEMU is virtio-wifi (if available in the QEMU version). For real hardware, USB WiFi adapters like RTL8188 are the simplest to support. This depends on USB host controller support. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"net: WiFi framework"`.


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
