---
schema_version: 1
id: http-tls
domain: 07-networking
status: active
title: "TODO-03 -- HTTP/HTTPS Client & TLS"
---

# TODO-03 -- HTTP/HTTPS Client & TLS

> **Goal:** Build a complete HTTP/HTTPS client stack: URL parser, HTTP GET/POST (including chunked transfer), `wget`/`curl` shell commands, Mbed TLS ported as a static kernel library, Mozilla CA bundle, HTTPS GET/POST with certificate chain verification, and an HTTP/1.1 keep-alive connection pool. HTTP and TLS are the gateway to the modern internet and prerequisites for the browser, email client, and OS update service.

> [!IMPORTANT]
> DNS resolution (`dns_resolve()`) and the BSD socket layer (`socket/connect/send/recv/close`) must be complete (→ XREF: `06-networking/TODO-02-dns-sockets.md`) before HTTP GET can be built. The TLS library port (§3) depends on `kmalloc`/`kfree` (`include/kernel/mm/heap.h`) and the kernel time service (→ XREF: `02-kernel-core/TODO-17` or the `uptime_ms()` equivalent) for certificate validity windows. Mbed TLS is compiled under `-ffreestanding -nostdlib -nostdinc` -- **no standard library functions** are permitted; only project headers and Mbed TLS's own self-contained headers are allowed. The keep-alive pool (§6) introduces shared-state concurrency; guard the pool with a spinlock from the kernel sync primitives.

## Inputs

- `src/kernel/net/tcp.c`, `src/kernel/net/socket.c` -- `dns_resolve()`, `kern_socket()`, `kern_connect()`, `kern_send()`, `kern_recv()`, `kern_close()` from TODO-01/02; keep-alive pool wraps these
- `include/kernel/mm/heap.h` -- `kmalloc()`/`kfree()` for Mbed TLS memory redirects
- -> XREF: `02-kernel-core/TODO-03-kernel-libraries.md` section 7 -- canonical freestanding Mbed TLS + CSPRNG; this file section 5 consumes that port (no re-vendor of upstream Mbed sources here)
- `include/kernel/sched/syscall.h` -- syscall table reference; no new syscall numbers needed (HTTP/TLS is kernel-library only for now; user-mode calls go through socket syscalls)
- `src/kernel/fs/vfs.c` -- `vfs_open()`/`vfs_read()` to load `C:\Impossible\System\Certs\ca-bundle.crt` at runtime
- `resources/certs/ca-bundle.crt` (to be added) -- Mozilla CA bundle (MPL-2.0, ~130 root CAs); installed to `C:\Impossible\System\Certs\ca-bundle.crt` on disk image
- `src/shell/` -- `cmd_wget.c` + `cmd_curl.c` new shell commands (§4); shell integration as in existing shell command pattern
- → XREF: `06-networking/TODO-02-dns-sockets.md` -- DNS (`dns_resolve`) and socket layer (§3–§8) are mandatory prerequisites for §2–§4 and §7–§8
- Related (no stable XREF target): `06-networking/TODO-04-*` (future IPv6) -- `http_get_v6()` stub deferred; URL parser handles IPv6 literal `[::1]` syntax for future use
- Related (no stable XREF target): `10-apps/TODO-*-browser` (future) -- §7 HTTPS client and §8 keep-alive pool are the direct foundation for the web browser TODO

## Outcome

- `url_parse(str, &url)` correctly splits scheme/host/port/path/query for HTTP and HTTPS URLs (no heap allocation).
- `http_get(url, buf, max)` and `http_post(url, ct, body, blen, buf, max)` work over plain TCP with redirect following (up to 5 hops) and chunked transfer decoding.
- `wget <url> [-o file]` and `curl <url>` shell commands with progress bars.
- Mbed TLS compiled and linked as a freestanding kernel static library (`lib/libmbedtls.a`).
- Mozilla CA bundle loaded from `C:\Impossible\System\Certs\ca-bundle.crt`; PEM chain verify passes for top-100 HTTPS sites in QEMU with internet NAT.
- `https_get(url, buf, max)` / `https_post(url, ct, body, blen, buf, max)` with full TLS 1.2/1.3 handshake.
- HTTP/1.1 keep-alive pool of 8 connections per host; re-used across `http_get`/`https_get` calls.

## Implementation Order

| ⭐  | Order | Deliverable                                                                             | Depends On                                                       | Status |
| --- | :---: | --------------------------------------------------------------------------------------- | ---------------------------------------------------------------- | :----: |
| 💎  |   1   | §1 URL parser -- `struct url`, scheme/host/port/path/query split, no heap               | Nothing -- standalone parser                                      |  [ ]   |
| 💎  |   2   | §2 HTTP GET -- `http_get()`, status line + header parse, body read, 5-hop redirect      | §1 (URL); DNS + socket layer (TODO-02)                           |  [ ]   |
| 💎  |   3   | §3 HTTP POST + chunked TE -- `http_post()`, `Content-Length`, hex chunk decoder         | §2 (baseline HTTP send/receive)                                  |  [ ]   |
| ⭐  |   4   | §4 `wget`/`curl` shell commands -- progress bar, file save, FTP stub                   | §2, §3 (HTTP client must work before shell commands wire it)     |  [ ]   |
| ⭐  |   5   | §5 Mbed TLS port -- static kernel lib, `malloc→kmalloc`, `time_now`, `RDRAND` RNG      | `kmalloc/kfree`; kernel time; compile flags verified             |  [ ]   |
| 💎  |   6   | §6 CA cert store -- PEM parser, `tls_load_ca_bundle()`, `tls_verify_cert()`             | §5 (Mbed TLS must compile before cert loading can be wired)      |  [ ]   |
| 💎  |   7   | §7 HTTPS GET/POST -- TLS handshake after `tcp_connect`, `wget`/`curl` detect `https://` | §5, §6 (TLS lib + CA bundle); §2, §3 (HTTP logic reused)         |  [ ]   |
| 💎  |   8   | §8 HTTP/1.1 keep-alive pool -- 8-conn LRU per host, `Connection: keep-alive`, spinlock  | §2, §7 (both plain and TLS paths needed before pool abstracts)   |  [ ]   |

---

## 1. URL Parser `[Sonnet]`

Define `struct url`. Parse `http://host:port/path?query#fragment` and `https://` URLs in-place (no heap allocation). Set default ports 80/443. Support IPv6 literal hostnames `[::1]`.

**Files:** `src/kernel/net/url.c` (new), `include/kernel/net/http.h` (new)

> [!NOTE]
> In-place splitting: the parser writes `\0` terminators into a caller-supplied mutable copy of the URL string; pointers in `struct url` point into that buffer. Maximum component lengths: scheme 8, host 253, path 2048, query 2048. Default port: `url.port = (url.https) ? 443 : 80` if no port component found. IPv6 literal: if host starts with `[`, scan to `]`, strip brackets, store in `url.host`. Fragment (`#`) is parsed but not stored (irrelevant to HTTP requests). Empty path normalizes to `"/"`.

- [ ] `struct url { char *scheme; char *host; uint16_t port; char *path; char *query; uint8_t https; }` in `http.h`
- [ ] `url_parse(const char *input, char *buf, size_t buf_len, struct url *out)` → 0 or -EINVAL: copy `input` into `buf`; scan and set `\0` delimiters; fill `out`; normalize empty path to `"/"`
- [ ] `url_is_https(url)` inline: returns `url->https`
- [ ] IPv6 host strip: detect `[` prefix; advance pointer past `]` before looking for `:port`
- [ ] Test cases in serial log: `http://example.com/path?q=1` → host=`example.com`, port=80, path=`/path`, query=`q=1`; `https://[::1]:8443/` → host=`::1`, port=8443, https=1
- [ ] Commit: `"net/http: URL parser -- scheme/host/port/path/query, IPv6 literal, no-heap in-place split"`

## 2. HTTP GET `[Sonnet]`

`http_get(url_str, response_buf, max_len)`: parse URL → `dns_resolve` → `kern_connect` → send `GET` request → receive and parse status line + headers → read body → `kern_close`. Follow up to 5 redirects (301/302 Location header). Return status code.

**Files:** `src/kernel/net/http.c` (new), `include/kernel/net/http.h` (extend)

> [!NOTE]
> Request format: `"GET /path HTTP/1.1\r\nHost: hostname\r\nConnection: close\r\nUser-Agent: ImpossibleOS/1.0\r\n\r\n"`. Status line parse: `"HTTP/1.1 NNN reason\r\n"` -- extract 3-digit code as integer. Header parsing: scan lines until `\r\n\r\n`; look for `Content-Length: N`, `Content-Type: ...`, `Location: url` (for redirects), `Transfer-Encoding: chunked`. Body read: if `Content-Length` known: `kern_recv()` loop until all bytes received; if chunked: delegate to `http_read_chunked()` (§3 prerequisite, call stub returning -ENOTSUP until §3 complete). Redirect: if status 301 or 302 and `Location` header present: re-parse Location URL; recurse up to 5 times; return -ELOOP if exceeded. Caller buffer must hold headers + body; if response exceeds `max_len`: truncate and return -ENOSPC (do not overflow).

- [ ] `http_response_t { int status; char content_type[64]; size_t content_length; uint8_t chunked; char location[512]; }` in `http.h`
- [ ] `http_send_request(fd, method, host, path, extra_headers, body, body_len)` → 0 or -errno: write formatted HTTP request to socket fd
- [ ] `http_parse_status_line(buf, len, &status)` → bytes consumed: parse `"HTTP/1.x NNN"` extract status code
- [ ] `http_parse_headers(buf, len, &resp)` → bytes consumed: scan header block; extract Content-Length, Content-Type, Location, Transfer-Encoding; stop at `\r\n\r\n`
- [ ] `http_read_body(fd, resp, out_buf, max_len)` → bytes read: if `resp.chunked`: call `http_read_chunked()` stub; else recv `content_length` bytes in a loop
- [ ] `http_get(url_str, buf, max_len)` → status code or -errno: `url_parse` → `dns_resolve` → `kern_socket` + `kern_connect` → `http_send_request` → recv headers → `http_read_body` → `kern_close`; redirect loop up to 5
- [ ] Log: `[HTTP] GET %s → %d (%zu bytes)` or `[HTTP] Redirect %d → %s`
- [ ] Commit: `"net/http: HTTP GET -- status parse, header parse, Content-Length body, 5-hop redirect follow"`

## 3. HTTP POST + Chunked Transfer Encoding `[Sonnet]`

`http_post(url_str, content_type, body, body_len, response_buf, max)`: send `POST` with `Content-Length` + `Content-Type` headers. Implement the chunked transfer decoder (`hex-size\r\ndata\r\n…0\r\n\r\n`).

**Files:** `src/kernel/net/http.c` (extend)

> [!NOTE]
> POST request: `"POST /path HTTP/1.1\r\nHost: …\r\nContent-Type: …\r\nContent-Length: N\r\nConnection: close\r\n\r\nbody"`. Chunked decoder: each chunk is `<hex-size>\r\n<data>\r\n`; final chunk is `0\r\n\r\n`. Hex size: parse up to 8 hex digits (chunk size limit 256 MiB); reject if size exceeds `max_len`. The decoder must handle chunk boundaries split across multiple `kern_recv()` calls -- maintain a small parse state machine: `CHUNK_SIZE`, `CHUNK_DATA`, `CHUNK_CRLF`, `CHUNK_DONE`. Chunked responses are common for large REST API responses; the `wget` command relies on chunked support to handle servers that don't know content-length upfront.

- [ ] `http_chunked_ctx_t { uint8_t state; uint32_t remaining; char size_buf[10]; uint8_t size_pos; }` state machine struct
- [ ] `http_read_chunked(fd, ctx, out_buf, max_len)` → total bytes or -errno: loop: in `CHUNK_SIZE` state: recv bytes until `\r\n`; parse hex; in `CHUNK_DATA`: recv `ctx.remaining` bytes into output; in `CHUNK_CRLF`: discard `\r\n`; detect `0\r\n\r\n` terminal chunk
- [ ] Wire `http_read_body()` to call `http_read_chunked()` when `resp.chunked == 1` (replaces the stub from §2)
- [ ] `http_post(url_str, content_type, body, body_len, buf, max_len)` → status code or -errno: identical flow to `http_get` except method=POST + Content-Type/Content-Length headers + body appended
- [ ] Trailer headers after final chunk: skip (do not parse; no application currently needs them)
- [ ] Log: `[HTTP] POST %s → %d (%zu bytes); chunked=%d`
- [ ] Commit: `"net/http: POST + chunked TE decoder -- hex chunk size state machine, chunked body assembly"`

## 4. `wget` / `curl` Shell Commands `[Sonnet]`

`wget <url> [-o file]` downloads to file or stdout. `curl <url>` always prints to stdout. Progress bar (bytes / Content-Length + KB/s). Auto-detect `ftp://` for FTP passthrough stub.

**Files:** `src/shell/cmd_wget.c` (new), `src/shell/cmd_curl.c` (new)

> [!NOTE]
> Progress bar format: `[====>     ] 45% 128 KB / 284 KB  @ 512 KB/s`. Update every 100 ms using `uptime_ms()` delta and bytes-received counter; on terminals narrower than 80 chars, omit the bar and print only the percentage. Content-Length unknown (chunked or server omits): display `?? KB` for total and omit percentage. `-o file` flag: `vfs_open()` + `vfs_write()` loop; flush on close. Default (no `-o`): write to stdout via `SYS_WRITE`. `ftp://` detection: print `[wget] FTP not yet supported, use ftp:// TODO-05` and return -ENOTSUP. `wget` and `curl` share the same download engine (`http_get`/`http_post`) -- only the output destination differs.

- [ ] `wget_download(url_str, out_fd, &progress_cb)` shared engine: `url_parse` + `http_get` / `https_get`; stream body in 4 KiB chunks; invoke `progress_cb(received, total)` every chunk
- [ ] `cmd_wget(argc, argv)`: parse `-o filename` flag; open output fd; call `wget_download`; close; print final stats
- [ ] `cmd_curl(argc, argv)`: always stdout fd; call `wget_download`; print final stats
- [ ] `wget_progress_bar(received, total, elapsed_ms)`: render bar string to 80-char buffer; write via `SYS_WRITE`
- [ ] FTP stub: if `url.scheme == "ftp"`: print error + XREF; return -ENOTSUP
- [ ] Register `wget` and `curl` in shell command table
- [ ] Commit: `"shell: wget + curl commands -- progress bar, -o file, Content-Length + chunked, FTP stub"`

## 5. Mbed TLS Kernel Port `[Opus]`

Integrate Mbed TLS (Apache-2.0, ~60 K lines) as a static kernel library (`lib/libmbedtls.a`). Redirect `malloc`/`free` → `kmalloc`/`kfree`. Provide `time_now()` from kernel uptime. Provide entropy via `RDRAND`. Compile under `-ffreestanding -nostdlib -nostdinc`.

**Files:** `lib/mbedtls/` (new -- Mbed TLS source tree), `lib/mbedtls/platform_port.c` (new), `scripts/build_mbedtls.sh` (new)

> [!NOTE]
> This is `[Opus]` -- porting a 60 K-line cryptographic library to a freestanding kernel environment is a novel integration task. Key concerns: (1) **Memory**: Mbed TLS calls `mbedtls_calloc`/`mbedtls_free` through `MBEDTLS_PLATFORM_MEMORY`; redirect to `kmalloc`/`kfree` in `platform_port.c`. (2) **Time**: Mbed TLS certificate validation calls `mbedtls_platform_gmtime_r()` and `time(NULL)`; provide a shim returning seconds since Unix epoch from `uptime_ms()` + a build-time base timestamp (good enough for cert window checks). (3) **Entropy**: Mbed TLS calls `mbedtls_entropy_add_source()`; register a single hardware source that reads 8 bytes via `RDRAND` repeated 4 times; supplement with TSC. (4) **Compile flags**: Mbed TLS must compile with `-ffreestanding -nostdlib -nostdinc`; disable `MBEDTLS_NET_C` (socket layer is replaced by ours), `MBEDTLS_TIMING_C` (no `setitimer`), `MBEDTLS_THREADING_C` (single-threaded per-connection TLS context). (5) **Config**: use a custom `mbedtls_config.h` enabling TLS 1.2 + TLS 1.3, RSA + EC, AES-GCM, SHA-256/384/512, `MBEDTLS_KEY_EXCHANGE_RSA_PSK_ENABLED`, `MBEDTLS_SSL_SERVER_NAME_INDICATION`.

- [ ] Vendor Mbed TLS source into `lib/mbedtls/` (3.x LTS branch); add `lib/mbedtls/include/mbedtls/mbedtls_config.h` with the restricted feature set above
- [ ] `lib/mbedtls/platform_port.c`: `mbedtls_platform_set_calloc_free(kmalloc_wrapper, kfree)`; `mbedtls_platform_set_time(kernel_time_shim)`; `kernel_time_shim()` returns `(uptime_ms() / 1000) + BUILD_UNIX_BASE`
- [ ] `lib/mbedtls/entropy_hw.c`: entropy source backed by the kernel CSPRNG (`csprng_fill()`, `02-kernel-core/TODO-03` §5); register via `mbedtls_entropy_add_source()`; RDRAND x4 + TSC only as pre-CSPRNG fallback
- [ ] `scripts/build_mbedtls.sh`: compile all Mbed TLS `.c` files with kernel flags (`-target x86_64-elf -ffreestanding -nostdlib -nostdinc -I include -I lib/mbedtls/include -O2`); archive to `lib/libmbedtls.a`; called from `scripts/build.sh` before kernel link
- [ ] Linker: add `lib/libmbedtls.a` to the kernel link command in the main `Makefile`
- [ ] Smoke test: `mbedtls_sha256("test", 4, digest, 0)` → correct known digest; printed to serial on boot
- [ ] Log: `[TLS] Mbed TLS 3.x initialized -- entropy OK, sha256 OK`
- [ ] Commit: `"net/tls: Mbed TLS 3.x kernel port -- kmalloc, RDRAND entropy, time shim, ffreestanding build"`

## 6. CA Certificate Store `[Sonnet]`

Embed Mozilla CA bundle at `resources/certs/ca-bundle.crt` (MPL-2.0, ~130 root CAs). Install to `C:\Impossible\System\Certs\ca-bundle.crt` on the disk image. `tls_load_ca_bundle(path)` PEM parser. `tls_verify_cert()` validates chain against trust store.

**Files:** `resources/certs/ca-bundle.crt` (new), `src/kernel/net/tls.c` (new), `include/kernel/net/tls.h` (new)

> [!NOTE]
> PEM format: base64-encoded DER between `-----BEGIN CERTIFICATE-----` and `-----END CERTIFICATE-----` markers. Parser: scan line-by-line; when marker found: accumulate base64 lines; on `END` marker: base64-decode to DER buffer; call `mbedtls_x509_crt_parse_der()` on the DER; append to the trust store chain. Trust store: a global `mbedtls_x509_crt` chain; `tls_load_ca_bundle()` called once during kernel init. `tls_verify_cert()` wraps `mbedtls_x509_crt_verify()` with the trust store chain and `mbedtls_ssl_conf_verify()` callback. Build-time disk image packing: add `resources/certs/ca-bundle.crt` → `C:\Impossible\System\Certs\ca-bundle.crt` to the build script's IXFS copy list.

- [ ] `resources/certs/ca-bundle.crt`: download current Mozilla CA bundle (curl-format, MPL-2.0); commit stripped of expired roots
- [ ] Build script: copy `resources/certs/ca-bundle.crt` → `C:\Impossible\System\Certs\ca-bundle.crt` in IXFS pack step
- [ ] `tls_init()`: call `mbedtls_x509_crt_init(&tls_ca_chain)`; `tls_load_ca_bundle("C:\\Impossible\\System\\Certs\\ca-bundle.crt")`; log count of loaded CAs
- [ ] `tls_load_ca_bundle(path)` → loaded CA count or -errno: `vfs_open` → scan PEM markers → base64-decode → `mbedtls_x509_crt_parse_der()` loop
- [ ] `base64_decode(input, in_len, out, &out_len)` helper in `tls.c` (standalone; no std library)
- [ ] `tls_verify_cert(mbedtls_ssl_context *ssl)` → 0 or error flags: wrap `mbedtls_ssl_conf_verify()` + `mbedtls_x509_crt_verify()`; log CN of cert being verified
- [ ] Log: `[TLS] CA bundle loaded -- N root CAs` and `[TLS] Cert OK: CN=hostname`
- [ ] Commit: `"net/tls: CA bundle PEM parser, tls_load_ca_bundle(), tls_verify_cert(), base64 decoder"`

## 7. HTTPS GET / POST `[Opus]`

TLS handshake after `tcp_connect` using Mbed TLS (ClientHello → ServerHello → certificate verify → key exchange). `https_get()`/`https_post()` identical to HTTP variants but over the encrypted channel. `wget`/`curl` auto-detect `https://`. Reject self-signed unless explicitly trusted.

**Files:** `src/kernel/net/tls.c` (extend), `src/kernel/net/http.c` (extend)

> [!NOTE]
> This is `[Opus]` -- the TLS handshake is security-critical: certificate chain verification failure **must** abort the connection. Implementation flow: (1) `tls_connect(fd, hostname)`: allocate `mbedtls_ssl_context` + `mbedtls_ssl_config`; configure with trust store from §6; set `mbedtls_ssl_set_hostname(hostname)` (enables SNI + CN/SAN check); wire Mbed TLS BIO callbacks to `kern_send`/`kern_recv`; call `mbedtls_ssl_handshake()`; on verify failure: `kern_close(fd)`; return -EACCES. (2) `tls_send`/`tls_recv` wrappers around `mbedtls_ssl_write`/`mbedtls_ssl_read`. (3) `https_get(url_str, buf, max)`: same as `http_get()` but after `kern_connect()`: call `tls_connect(fd, host)` before sending the HTTP request; use `tls_send`/`tls_recv` instead of `kern_send`/`kern_recv`. (4) `tls_close(fd)`: `mbedtls_ssl_close_notify()` + `mbedtls_ssl_free()` + `kern_close()`.

- [ ] `tls_conn_t { mbedtls_ssl_context ssl; mbedtls_ssl_config conf; mbedtls_entropy_context entropy; mbedtls_ctr_drbg_context drbg; int sock_fd; }` -- allocated via `kmalloc`
- [ ] `tls_connect(fd, hostname)` → `tls_conn_t*` or NULL: init ctx; `mbedtls_ssl_conf_ca_chain(&conf, &tls_ca_chain, NULL)`; set BIO callbacks `(kern_send_bio, kern_recv_bio)`; `mbedtls_ssl_handshake()`; verify result; on failure: free + close fd + return NULL
- [ ] `kern_send_bio(ctx, buf, len)` / `kern_recv_bio(ctx, buf, len)`: Mbed TLS BIO callbacks that call `kern_send(fd, ...)` / `kern_recv(fd, ...)`
- [ ] `tls_send(conn, buf, len)` → bytes: `mbedtls_ssl_write()`
- [ ] `tls_recv(conn, buf, max)` → bytes: `mbedtls_ssl_read()`
- [ ] `tls_close(conn)`: `mbedtls_ssl_close_notify()`; `mbedtls_ssl_free()`; `kfree(conn)`; `kern_close(fd)`
- [ ] `https_get(url_str, buf, max)` / `https_post(url_str, ct, body, blen, buf, max)`: call `http_get()`/`http_post()` internal with a `tls_conn_t*` override for send/recv (add `conn_override` parameter to `http_send_request` and `http_read_body`)
- [ ] Update `cmd_wget`/`cmd_curl` to call `https_get`/`https_post` when `url.https == 1`
- [ ] Log: `[TLS] Handshake OK: %s TLS %s cipher=%s` and `[TLS] Handshake FAILED: %s (-0x%04x)`
- [ ] Commit: `"net/tls: HTTPS client -- Mbed TLS handshake, SNI, cert verify, BIO wrappers, https_get/post"`

## 8. HTTP/1.1 Keep-Alive Connection Pool `[Opus]`

Pool of 8 connections per host (`host:port` key). Reuse existing TCP (or TLS) connection for subsequent requests. LRU eviction. `Connection: keep-alive` header. Spinlock-guarded shared state.

**Files:** `src/kernel/net/http.c` (extend), `include/kernel/net/http.h` (extend)

> [!NOTE]
> This is `[Opus]` -- the pool is shared state between concurrent callers (future: multiple tasks calling `http_get()` simultaneously). Guard the pool table with a kernel spinlock; hold only while reading/writing pool metadata, not during `kern_send`/`kern_recv`. Pool entry: `{ char host[256]; uint16_t port; int fd; tls_conn_t *tls; uint64_t last_used_ms; uint8_t in_use; }`. `Connection: keep-alive` request: send header; check response for `Connection: close` -- if server closes: evict entry. Keep-alive timeout: if `last_used_ms` is more than 30 s ago: close and evict before reuse attempt. LRU eviction when pool is full (8 entries): find entry with smallest `last_used_ms` that is not `in_use`; close its fd/tls; replace. Concurrent protection: `in_use = 1` before handing fd to a caller; `in_use = 0` on return -- no two callers share the same fd.

- [ ] `http_pool_entry_t { char host[256]; uint16_t port; int fd; tls_conn_t *tls; uint64_t last_used_ms; uint8_t in_use; uint8_t valid; }` + `http_pool[8]` + `pool_lock` (spinlock)
- [ ] `http_pool_acquire(host, port, https)` → `http_pool_entry_t*` or NULL: lock; scan for matching valid + not in_use entry; check 30 s liveness; if found: mark in_use; unlock; return; else: unlock; open new TCP/TLS connection; insert into free/LRU slot
- [ ] `http_pool_release(entry, keep)`: lock; if `keep && server_sent_keepalive`: mark not in_use; update `last_used_ms`; unlock; else: close fd/tls; mark invalid; unlock
- [ ] `http_pool_lru_evict()`: find min `last_used_ms` where `!in_use && valid`; close; return slot index
- [ ] Modify `http_get()`/`https_get()` to call `http_pool_acquire`/`http_pool_release`; send `Connection: keep-alive` header; on response `Connection: close`: call `http_pool_release(entry, keep=0)`
- [ ] `Connection: keep-alive` parsing: add to `http_parse_headers()` -- `resp.keep_alive = 1` if header present and not `close`
- [ ] Log: `[HTTP] Pool hit: %s:%u (reused); Pool miss: connecting`
- [ ] Commit: `"net/http: keep-alive pool -- 8 conns/host, LRU evict, spinlock guard, 30s liveness check"`

---

## OS Comparison


| ⭐  | Feature                                  | 🪟 Win11                                 | 🐧 Linux                                 | 🚀 Impossible OS                         |
| --- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- |
| 💎  | URL parser                               | ✅ `wininet.dll` `InternetCrackUrl()`; heap-based | ✅ glibc `getaddrinfo()`; various userspace URL | ⬜ §1 -- stack-only in-place parse; kernel-native; no |
| 💎  | HTTP GET                                 | ✅ `winhttp.dll` `WinHttpSendRequest()`; full redirect follow | ✅ `libcurl`/`wget`/glibc; kernel HTTP via eBPF | ⬜ §2 -- kernel-native `http_get()`; no userspace DLL |
| 💎  | HTTP POST + chunked transfer decoder     | ✅ `winhttp.dll` `WinHttpWriteData()`; chunked handled transparently | ✅ `libcurl` chunked support             | ⬜ §3 -- hex-chunk state machine; chunk-boundary-safe across |
| ⭐  | `wget` + `curl` built-in with progress bar | ✅ `curl.exe` bundled since Win10 21H1   | ✅ `wget` + `curl` as standard           | ⬜ §4 -- `⭐` both built into the        |
| ⭐  | Mbed TLS as freestanding kernel static library | ✅ `schannel.dll` (kernel TLS in `ncrypt.sys` | ✅ kernel TLS via `ktls` +               | ⬜ §5 -- `⭐` single self-contained kernel library |
| 💎  | CA cert store                            | ✅ Windows Certificate Store (`certmgr`); Mozilla | ✅ `/etc/ssl/certs/`; `update-ca-certificates`; `x509_verify_cert()` | ⬜ §6 -- 130 Mozilla root CAs at         |
| 💎  | HTTPS GET/POST                           | ✅ `WinHttpSendRequest()` with TLS via SChannel; | ✅ `libcurl`/`wget` with OpenSSL; TLS 1.3 | ⬜ §7 -- Mbed TLS handshake; SNI; CN/SAN |
| 💎  | HTTP/1.1 keep-alive pool                 | ✅ `WinHTTP` session-level connection pooling; transparent | ✅ `libcurl` multi-handle persistent connections; kernel | ⬜ §8 -- kernel-native pool shared by `http_get`+`https_get` |

> **After §1–§8:** Impossible OS has a fully self-contained, kernel-native HTTP/HTTPS client with zero userspace library dependencies. The combination of `wget` + `curl` built into the base OS (`⭐`), Mbed TLS as a freestanding kernel library (`⭐`), and the in-kernel connection pool positions the networking stack to directly support a browser, package manager, and OS update service without any additional runtime libraries.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===` (Mbed TLS must compile without warnings under kernel flags)
- [ ] URL parser: `url_parse("http://example.com:8080/path?q=1#frag", ...)` → host=`example.com`, port=8080, path=`/path`, query=`q=1`; `https://[::1]/` → host=`::1`, port=443, https=1
- [ ] HTTP GET: `http_get("http://example.com/", buf, 65536)` → status=200, body contains `<!DOCTYPE html>` (requires QEMU with NAT)
- [ ] Redirect: `http_get("http://httpbin.org/redirect/3", buf, 65536)` → status=200 after following 3 redirects; log shows 3 `[HTTP] Redirect` entries
- [ ] Chunked: `http_get("http://httpbin.org/stream/3", buf, 16384)` → full body assembled from 3 chunks; no boundary corruption
- [ ] HTTP POST: `http_post("http://httpbin.org/post", "application/json", "{\"k\":1}", 7, buf, 4096)` → status=200, response JSON contains echoed body
- [ ] `wget http://example.com/ -o /tmp/test.html` → file written; progress bar shows `100%`; serial log confirms file size matches Content-Length
- [ ] `curl http://httpbin.org/ip` → prints JSON `{"origin": "..."}` to stdout
- [ ] Mbed TLS: serial boot log shows `[TLS] Mbed TLS 3.x initialized` + `[TLS] CA bundle loaded -- N root CAs`
- [ ] HTTPS GET: `https_get("https://example.com/", buf, 65536)` → status=200; serial shows `[TLS] Handshake OK: example.com TLS 1.3`
- [ ] Cert rejection: `https_get("https://self-signed.badssl.com/", buf, ...)` → returns -EACCES; log shows `[TLS] Handshake FAILED`
- [ ] Keep-alive: two consecutive `http_get()` to the same host → second call log shows `[HTTP] Pool hit`; only one TCP handshake in QEMU packet capture
- [ ] Commit: `"net: complete HTTP/HTTPS client -- URL parser, GET/POST, chunked, TLS, CA bundle, keep-alive pool"`
