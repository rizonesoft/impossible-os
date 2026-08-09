# 06 Networking

This domain tracks networking inside the OS runtime: packet flow, protocol layers, and the kernel-side network API surface.

## Belongs Here

- Kernel network stack work such as Ethernet, ARP, IPv4, ICMP, UDP, DHCP, and future protocol layers.
- Socket-like runtime behavior, packet routing, and kernel-side network integration.
- Networking features that are shared infrastructure for many apps.

## Does Not Belong Here

- NIC driver implementation details. Put that in [04 Drivers Hardware](../04-drivers-hardware/INDEX.md).
- User-facing internet applications such as browser, SSH, FTP, or email. Put that in [10 Apps](../11-apps/INDEX.md).

## Likely Source Areas

- [src/kernel/net](../src/kernel/net/)
- [src/kernel/drivers/rtl8139.c](../src/kernel/drivers/rtl8139.c)
- [include/kernel/net](../include/kernel/net/)

## Epics

- None yet.

## Active TODOs

- [`TODO-01-tcp-network-infrastructure.md`](TODO-01-tcp-network-infrastructure.md) -- TCP header + 11-state RFC 793 machine, connect/send/recv/close API, retransmit + Nagle + slow-start, `net_interface` manager (replaces global `net_cfg`), loopback `lo`, stateful connection tracking hash table
- [`TODO-02-dns-sockets.md`](TODO-02-dns-sockets.md) -- DNS query builder + response parser + 64-entry LRU cache (`dns_resolve`, `nslookup`), AAAA query stub, `AF_INET` kernel socket layer (`SOCK_STREAM`/`SOCK_DGRAM`, `connect/send/recv/bind/listen/accept`), `setsockopt/getsockopt`, syscalls 39–49, `user/lib/socket.c` wrappers, `getaddrinfo`, `select()` 64-fd bitmask
- [`TODO-03-http-tls.md`](TODO-03-http-tls.md) -- URL parser (no heap), HTTP GET + POST + chunked TE, `wget`/`curl` shell commands with progress bar, Mbed TLS 3.x as freestanding kernel static library (`RDRAND` entropy, `kmalloc` redirect), Mozilla CA bundle PEM parser + chain verify, HTTPS GET/POST (SNI, cert reject), HTTP/1.1 keep-alive pool (8 conns/host, LRU, spinlock)
- [`TODO-04-ipv6-dual-stack.md`](TODO-04-ipv6-dual-stack.md) -- `struct ipv6_header` + ethertype 0x86DD dispatch, `ipv6_send/receive`, ICMPv6 NS/NA/RS/RA + `ndp_resolve()`, EUI-64 link-local + DAD, 128-entry NDP neighbor cache (REACHABLE/STALE/PROBE/FAILED, spinlock), SLAAC global address + RDNSS, DHCPv6 Solicit/Request/Reply, DNS AAAA activation, `AF_INET6` dual-stack sockets + `IPV6_V6ONLY`, `ifconfig` IPv6 + `ndp -an`
- [`TODO-05-firewall.md`](TODO-05-firewall.md) -- `struct fw_rule` 64-rule packet filter engine (first-match-wins, `fw_check()`), IPv4+IPv6 hooks in `ipv4_handle`/`ipv4_send`/`ipv6_receive`/`ipv6_send`, stateful CT 4-tuple auto-allow, SYN guard, default allow-out/block-in ruleset, atomic per-rule hit counters + `/sys/firewall` VFS, `fw` CLI, Registry persistence (`HKLM\SYSTEM\Network\Firewall\Rules`), `firewall.cpl` applet with block-log viewer
- [`TODO-06-ntp-status-winsock.md`](TODO-06-ntp-status-winsock.md) -- DHCP lease renewal daemon (T1/T2/expiry, unicast/broadcast REQUEST), NTP v4 client (`pool.ntp.org`, FILETIME convert, TSC slew/`uefi_set_time`, 24 h thread, Registry), `ifconfig` multi-NIC + stats, network stats API + tray icon (🌐/⚠), `ping` IPv6 + flags, `traceroute` TTL probe + reverse DNS, `netstat` socket table, `/sys/net` unified VFS file, `ws2_32.dll` Winsock stubs
- [`TODO-07-web-browser.md`](TODO-07-web-browser.md) -- text-only browser (tag strip → scrollable window → link nav), HTML tokenizer + DOM tree (20 tags), 16-tab management (Ctrl+T/W/Tab), block/inline layout engine (word wrap, scroll, box model), `<img src>` HTTP fetch + `image_load_mem` + 16-entry cache, CSS cascade engine (specificity, class/ID selectors, em/%), JS stub (`[JavaScript disabled]` placeholder, QuickJS hook), bookmarks JSON + HTML import/export, download manager (HTTP Range resume, progress window), TLS padlock + mixed-content block + cookie jar + ETag cache
- [`TODO-08-ssh-ftp-clients.md`](TODO-08-ssh-ftp-clients.md) -- FTP protocol (TCP 21, PASV, LIST/RETR/STOR, CWD/PWD/MKD/RMD/DELE), interactive `ftp host` + `wget ftp://`, FTPS/FTPES (AUTH TLS + PROT P via Mbed TLS), SSH2 transport (monocypher port: Curve25519 KEX, ChaCha20-Poly1305, `known_hosts`), SSH password + Ed25519 auth + `ssh-keygen`, SSH channel + PTY relay, `ssh`/`scp` shell commands, SSH agent stub (`ssh-add`), SFTP v3 subsystem (`sftp` interactive + `sftp://` File Manager)
- [`TODO-09-email-client.md`](TODO-09-email-client.md) -- RFC 2822 + MIME multipart parser (base64/QP decode, attachment save), SMTP TCP 587 STARTTLS + AUTH LOGIN + QP MIME send, POP3 TCP 995 TLS + UIDL dedup + `.eml` store, IMAP TCP 993 TLS + tagged protocol + FETCH/STORE/SEARCH/EXPUNGE/IDLE push, account manager (Registry, MX autodiscover), toast + tray envelope badge, three-panel GUI (sidebar/list/viewer, sort, keyboard), compose (To/CC/BCC, inline markup, attachments, reply/forward), contacts JSON + autocomplete, full-text local + IMAP search
- [`TODO-10-pdf-viewer.md`](TODO-10-pdf-viewer.md) -- PDF structure parser
- [`TODO-11-syslog-forwarding.md`](TODO-11-syslog-forwarding.md) -- RFC 5424 syslog UDP forwarding, Registry-gated, network-down resilient (moved from kernel-core/TODO-02 §7) (xref table + stream, trailer, ObjStm, incremental updates), object model (indirect resolver, FlateDecode via stbi_zlib, LZW/ASCII85, type system), page tree (catalog/Pages/Kids flat map), content stream interpreter (text+path+color+XObject operators, CTM stack), embedded TrueType fonts (FontFile2, ToUnicode CMap, stb_truetype render), image rendering (FlateDecode + DCTDecode/JPEG via stbi, CMYK→RGB, fb_blit), page renderer (scanline fill, alpha composite, bezier flatten, DPI scale), viewer app (zoom 50–400%/fit-width/page, thumbnail sidebar, text search, print), HTTP streaming (`pdf_open_mem` + `https_get`, no disk write), AcroForm stub (Tx/Btn/Ch widgets, FDF/XFDF export)

## Completed / Doc-converted

- None yet.

## Local Naming

- Use `TODO-01-ipv4-routing.md` as the filename style for new leaf TODOs in this folder.
- Create a parent TODO only when one topic needs multiple leaf files or shared verification.
