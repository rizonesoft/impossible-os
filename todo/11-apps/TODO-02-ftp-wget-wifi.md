---
schema_version: 1
id: ftp-wget-wifi
domain: 11-apps
status: active
title: "TODO-02 -- FTP Client, wget/curl & WiFi"
---

# TODO-02 -- FTP Client, wget/curl & WiFi

**Domain:** `10-apps`
**Goal:** Deliver the file-transfer and command-line download toolbox (FTP, wget, curl) that complete the networking app layer, plus a WiFi framework (stretch) for wireless hardware support.

> [!IMPORTANT]
> **Depends on:** `06-networking/TODO-03-http-tls.md` -- `http_get()`, `https_get()`, `http_post()`, `kern_socket()`, `kern_connect()`, `kern_send()`, `kern_recv()`, `kern_close()`, `dns_resolve()` must all be complete before any section here begins.
> **Migration:** Consolidates `TODO-420-FTP.md` (all sections) + `TODO-400-Networking.md` §4.4 (wget/curl) + §9.5 (WiFi) into one file. All other sections of `TODO-400-Networking.md` live in `06-networking`.

---

## Important Notes

- FTP uses **two TCP connections**: control (port 21, ASCII commands) and data (negotiated via `PASV`). Both use `kern_connect()`/`kern_send()`/`kern_recv()` from `06-networking/TODO-02`.
- `https_get()` / `http_get()` from `06-networking/TODO-03` are the only network primitives `wget` and `curl` call -- no raw socket work needed in those tools.
- `vfs_open()`, `vfs_write()`, `vfs_read()`, `vfs_stat()` from `include/kernel/fs/vfs.h` are used for file I/O in download and upload operations.
- `CTRL_LISTVIEW` and `CTRL_PROGRESSBAR` are defined in `08-graphics-ui/TODO-05` -- the FTP GUI (§3) and WiFi settings (§6) depend on them.
- WiFi (§6) is a **stretch goal** with a hardware dependency (USB RTL8188 or QEMU virtio-wifi). Do not start §6 until §1–5 are complete and working.
- `wget -r` (recursive crawl) depends on `TODO-01 §2` (HTML link extractor) for `<a href>` extraction -- cross-link only, don't duplicate.
- Anonymous FTP uses `USER anonymous` / `PASS user@host` by convention.
- Progress display (bytes / total / KB/s / ETA) is common to `ftp_download`, `wget`, and `curl` -- extract into a shared `progress_bar_print(done, total, elapsed_ms)` helper.

---

## Inputs

| Path | Purpose |
|------|---------|
| `include/kernel/fs/vfs.h` | `vfs_open`, `vfs_write`, `vfs_read`, `vfs_stat` -- local file I/O |
| `06-networking/TODO-03-http-tls.md` | `http_get`, `https_get`, `http_post` -- wget/curl network ops |
| `06-networking/TODO-02-dns-sockets.md` | `kern_socket`, `kern_connect`, `kern_send`, `kern_recv`, `kern_close` -- FTP raw TCP |
| `include/registry.h` | `registry_get/set()` -- WiFi credentials, ncpa.cpl settings |
| `08-graphics-ui/TODO-05 §*` | `CTRL_LISTVIEW`, `CTRL_PROGRESSBAR` -- FTP GUI + WiFi settings panel |
| → XREF: `06-networking/TODO-03` | HTTP/HTTPS client -- mandatory for wget/curl |
| → XREF: `10-apps/TODO-01 §2` | HTML link extractor for `wget -r` recursive crawl |
| → XREF: `04-drivers-hardware/TODO-10-usb-stack.md` | USB HID stack -- USB WiFi adapter driver prerequisite |

---

## Outcome

- `ftp <host>` opens an interactive FTP session with `ls`, `cd`, `get`, `put`, `pwd`, `mkdir`, `rm`, `bye` subcommands.
- `wget <url>` and `wget ftp://host/path` download files with a live progress bar.
- `curl <url>` prints response to stdout; supports `-o`, `-X POST`, `-H`, `-s`, `-I`, `-L`, `-u` flags.
- FTP GUI (stretch): dual-pane local↔remote file manager with drag-to-transfer.
- WiFi framework (stretch): scan → connect (WPA2) → DHCP; `ncpa.cpl` WiFi tab.

---

## Implementation Order

| #   | Section                                  | Tag        | Dep                | Mark |
| --- | ---------------------------------------- | ---------- | ------------------ | ---- |
| 1   | FTP protocol core (`ftp_connect`, `PASV`, `RETR`, `STOR`) | `[Sonnet]` | 06-net/TODO-02     | 💎    |
| 2   | FTP shell commands + `wget ftp://`       | `[Sonnet]` | §1                 | 💎    |
| 3   | `wget` command (HTTP/HTTPS + progress)   | `[Sonnet]` | 06-net/TODO-03     | 💎    |
| 4   | `curl` command (full flag set)           | `[Sonnet]` | §3                 | 💎    |
| 5   | FTP GUI client (dual-pane, stretch)      | `[Sonnet]` | §1, 07-gfx/TODO-05 | 💎    |
| 6   | WiFi framework (stretch)                 | `[Opus]`   | 04-drivers/TODO-09 | 💎    |

---

## 1. FTP Protocol Core `[Sonnet]`

Create `src/apps/ftp/ftp.c` + `include/apps/ftp.h`. Pure protocol layer -- no UI, no shell integration.

- [ ] `struct ftp_session { int ctrl_sock; char host[256]; char cwd[512]; uint8_t logged_in; }`
- [ ] `ftp_connect(host, user, pass)`:
  - `kern_connect(host, 21)` → control socket
  - Read 220 greeting line (multi-line: lines until one starts without `-`)
  - Send `USER <user>\r\n` → read 331; send `PASS <pass>\r\n` → expect 230; return -errno on auth failure
  - Anonymous FTP: `user = "anonymous"`, `pass = "user@impossible-os.local"`
- [ ] `ftp_cmd(session, cmd, response, max)` → send `cmd\r\n`; read response line(s) until `NNN ` prefix (non-hyphen); return numeric code
- [ ] `ftp_pasv(session, data_host, data_port)`:
  - `ftp_cmd(session, "PASV", ...)` → parse `(h1,h2,h3,h4,p1,p2)` from response
  - `*data_host = h1.h2.h3.h4`; `*data_port = p1*256 + p2`
- [ ] `ftp_list(session, listing_buf, max)` → `PASV` → data connect → `LIST` → read data stream to `listing_buf` → close data socket; return byte count
- [ ] `ftp_download(session, remote, local, progress_cb)`:
  - `ftp_cmd(session, "TYPE I")` → binary mode
  - `ftp_cmd(session, "SIZE <remote>")` → parse total size for progress
  - `PASV` → data connect → `RETR <remote>` → stream to `vfs_open(local, VFS_WRITE|VFS_CREATE)` in 64 KiB chunks; call `progress_cb(done, total)` after each chunk; close data + VFS
- [ ] `ftp_upload(session, local, remote, progress_cb)`:
  - `vfs_open(local, VFS_READ)` → `vfs_stat()` for size
  - `PASV` → data connect → `STOR <remote>` → stream VFS chunks to data socket; `progress_cb`
- [ ] Additional commands:
  - `ftp_cwd(session, path)` → `CWD <path>` → expect 250
  - `ftp_pwd(session, buf, max)` → `PWD` → parse quoted path from 257 response
  - `ftp_mkdir(session, path)` → `MKD <path>` → expect 257
  - `ftp_rmdir(session, path)` → `RMD <path>` → expect 250
  - `ftp_delete(session, path)` → `DELE <path>` → expect 250
  - `ftp_size(session, path)` → `SIZE <path>` → parse bytes from 213 response
- [ ] `ftp_disconnect(session)` → `QUIT\r\n` → close control socket
- [ ] Commit: `"apps: FTP client protocol core (PASV, RETR, STOR, LIST)"`

---

## 2. FTP Shell Commands + `wget ftp://` `[Sonnet]`

Interactive `ftp` command and anonymous single-file `wget ftp://` integration in `cmd.exe`.

- [ ] **`ftp <host>`** built-in shell command:
  - Connect with `ftp_connect(host, "anonymous", "user@impossible-os.local")` initially; `login` subcommand prompts username/password
  - Interactive loop: print `ftp> ` prompt; read line; dispatch subcommands:
    - `ls [path]` → `ftp_list()` → print listing (parse `LIST` output: `drwxr-xr-x ... filename`)
    - `cd <dir>` → `ftp_cwd()`; update `session.cwd`
    - `get <remote> [local]` → `ftp_download()` with `progress_bar_print` callback
    - `put <local> [remote]` → `ftp_upload()` with progress
    - `pwd` → `ftp_pwd()` → print
    - `mkdir <dir>` → `ftp_mkdir()`
    - `rm <file>` → `ftp_delete()`
    - `bye` / `quit` → `ftp_disconnect()` + exit
    - `help` → list subcommands
- [ ] **`progress_bar_print(done, total, elapsed_ms)` helper** (shared by §2, §3, §4):
  - Format: `[████████░░░░░░░░] 512 KB / 1.2 MB | 320 KB/s | ETA 3s`
  - Uses `serial_write()` + `\r` (carriage return, no newline) to update in-place
  - If `total == 0` (unknown Content-Length): `512 KB | 320 KB/s` (no percentage/ETA)
- [ ] **`wget ftp://host/path`**: parse `ftp://` scheme → extract host + path; `ftp_connect(host, "anonymous", ...)` → `ftp_download(session, path, filename, progress_cb)` → `ftp_disconnect()`
  - `filename` derived from last `/`-separated component of path
- [ ] Commit: `"shell: ftp interactive command + wget ftp:// support"`

---

## 3. `wget` Command (HTTP/HTTPS + Progress) `[Sonnet]`

Implements the full `wget` command-line tool backed by `http_get()`/`https_get()`.

- [ ] `wget <url>` → derive output filename from last URL path segment (strip query string); call `https_get(url, buf, MAX_DOWNLOAD)` or `http_get()`; write to VFS file; show progress via `progress_bar_print`
- [ ] `wget -O <file> <url>` → explicit output filename (or `-O -` for stdout)
- [ ] `wget -q <url>` → quiet mode (no progress, no status messages; errors still to stderr)
- [ ] `wget -c <url>` → resume: if output file exists, read its size → set `Range: bytes=N-` HTTP header in request; append to existing file
- [ ] `wget -r <url>` → recursive: fetch HTML page; extract `<a href>` links (call `html_extract_links()` from `TODO-01 §1`); limit to same domain + max 2 levels deep; save each file preserving URL directory structure under `./hostname/path/`; max 100 files
- [ ] `wget --no-check-certificate <url>` → skip TLS cert verification (calls `https_get_insecure()` variant)
- [ ] Progress: `https_get()` streams response → parse `Content-Length` header → call `progress_bar_print(bytes_received, content_length, elapsed_ms)` per 64 KiB chunk
- [ ] Error messages: `wget: cannot resolve host`, `wget: HTTP 404 Not Found`, `wget: TLS handshake failed`
- [ ] Commit: `"shell: wget command (HTTP/HTTPS, -O, -q, -c, -r progress)"`

---

## 4. `curl` Command (Full Flag Set) `[Sonnet]`

`curl` is the developer-facing HTTP tool. Output defaults to stdout unless `-o` is given.

- [ ] `curl <url>` → `https_get(url, buf, MAX)` → `serial_write(buf)` (stdout)
- [ ] `curl -o <file> <url>` → write response body to file; progress to stderr
- [ ] `curl -X <METHOD> <url>` → override HTTP method (GET/POST/PUT/DELETE/HEAD/PATCH)
- [ ] `curl -d "data" <url>` → send as `POST` body with `Content-Type: application/x-www-form-urlencoded`
- [ ] `curl -d @<file> <url>` → read POST body from file
- [ ] `curl -H "Header: value" <url>` → inject custom header (repeatable; accumulated into header list)
- [ ] `curl -s <url>` → silent: suppress progress and informational messages
- [ ] `curl -I <url>` → send `HEAD` request; print response headers only
- [ ] `curl -L <url>` → follow `Location:` redirects (up to 10 hops; `http_get` already follows 5 -- extend limit)
- [ ] `curl -u user:pass <url>` → HTTP Basic Auth: set `Authorization: Basic base64(user:pass)` header
- [ ] `curl -v <url>` → verbose: print `> request headers` and `< response headers` to stderr, body to stdout
- [ ] `curl -k <url>` → insecure: skip TLS cert verification
- [ ] JSON pretty-print: if response `Content-Type: application/json` → format JSON with 2-space indent before printing (simple recursive formatter, no external library)
- [ ] `curl --version` → print version string with supported protocols: `curl/1.0 (impossible-os) http https ftp`
- [ ] Commit: `"shell: curl command (-X, -d, -H, -I, -L, -u, -v, -k, JSON pretty-print)"`

---

## 5. FTP GUI Client (Dual-Pane, Stretch) `[Sonnet]`

Standalone `ftpgui.exe` with a dual-pane file manager layout. Depends on `CTRL_LISTVIEW` from `08-graphics-ui/TODO-05`.

- [ ] Window layout: toolbar at top (Connect/Disconnect, address bar showing `ftp://host/path`); two equal-width panes side by side
  - **Left pane** (local): `CTRL_LISTVIEW` showing `C:\` directory (columns: Name, Size, Date); breadcrumb `CTRL_LABEL` showing current path; uses `vfs_readdir()`
  - **Right pane** (remote): `CTRL_LISTVIEW` showing FTP directory listing (columns: Name, Size, Permissions); breadcrumb showing `ftp://host/cwd`; populated from `ftp_list()`
- [ ] Connect dialog: `CTRL_TEXTBOX` for host, user, password; `[Connect]` button → `ftp_connect()`; `[Disconnect]` button
- [ ] Double-click folder → `ftp_cwd()` + refresh remote pane; double-click local folder → `vfs_readdir()` + refresh local pane
- [ ] `[→ Upload]` button: selected local file → `ftp_upload()` with `CTRL_PROGRESSBAR` in status bar
- [ ] `[← Download]` button: selected remote file → `ftp_download()` with `CTRL_PROGRESSBAR`
- [ ] Keyboard: Del on remote item → `ftp_delete()` / `ftp_rmdir()`; F5 → refresh; F2 → rename (via `RNFR`/`RNTO` commands)
- [ ] Status bar: `CTRL_PROGRESSBAR` (transfer progress) + `CTRL_LABEL` (bytes/rate/ETA, status messages)
- [ ] `ftp://` URL in File Manager address bar → launch `ftpgui.exe` with that URL as argument
- [ ] Commit: `"apps: FTP GUI client (dual-pane, upload/download, progress)"`

---

## 6. WiFi Framework (Stretch) `[Opus]`

> Long-term stretch goal. Requires USB host stack (`04-drivers-hardware/TODO-10-usb-stack.md`) and real hardware or QEMU virtio-wifi. Do not start until §1–5 are complete.

- [ ] Create `include/kernel/drivers/wifi.h`:
  - `struct wifi_network { char ssid[33]; uint8_t bssid[6]; int8_t signal_dbm; uint8_t security; /* WIFI_OPEN/WEP/WPA2/WPA3 */ uint8_t channel; }`
  - `wifi_scan(networks, max)` → active probe requests + passive beacon listen; return count
  - `wifi_connect(ssid, passphrase)` → 4-way handshake + DHCP; return 0 on success
  - `wifi_disconnect()` → send deauthentication frame
  - `wifi_get_status(ssid_buf, ip_buf)` → current connection info
- [ ] 802.11 management frame parsing (`src/kernel/drivers/wifi.c`):
  - Beacon frames: extract SSID IE (ID=0), RSN IE (ID=48) for WPA2/WPA3 detection, DSSS parameter (channel)
  - Probe response: same IE parsing
- [ ] WPA2 4-way handshake:
  - PMK derivation: `PBKDF2-HMAC-SHA1(passphrase, ssid, 4096, 32)` via monocypher
  - PTK derivation: `PRF-512(PMK, "Pairwise key expansion", min(AP_MAC,STA_MAC)||max(...)||min(ANonce,SNonce)||max(...))`
  - EAPOL frame construction/verification (MIC with HMAC-SHA1-128)
  - AES-CCMP for data encryption/decryption (frame by frame)
- [ ] Hardware drivers:
  - **USB RTL8188** (USB product ID `0x8176`): USB bulk transfer for TX/RX; register programming for scan + association
  - **QEMU virtio-wifi**: check QEMU version for `virtio-wifi-pci` device availability; fall back to USB passthrough if unavailable
- [ ] `ncpa.cpl` WiFi tab:
  - `[Scan]` button → `wifi_scan()` → populate `CTRL_LISTVIEW` with SSID, signal bars (4-level: < -80 / -70 / -60 / -50 dBm), security icon
  - Click network → `[Connect]` → password dialog (`CTRL_TEXTBOX`, type=password) → `wifi_connect(ssid, pass)`
  - `[Forget]` → delete stored passphrase from `HKLM\SYSTEM\Network\WiFi\{ssid}\Passphrase`
  - Connected network shown with ✓ and current IP address
- [ ] Store passphrase (encrypted with kernel key) in `HKLM\SYSTEM\Network\WiFi\{ssid}\Passphrase` -- auto-reconnect on boot
- [ ] Commit: `"drivers: WiFi framework (RTL8188, WPA2 4-way handshake, ncpa.cpl WiFi tab)"`

---

## OS Comparison


| ⭐   | Feature                                  | 🪟 Win11                           | 🐧 Linux                   | 🚀 Impossible OS                        |
| --- | ---------------------------------------- | --------------------------------- | ------------------------- | -------------------------------------- |
| 💎   | FTP client                               | ✅ `ftp.exe` built-in              | ✅ `ftp`/`lftp`            | ⬜ `ftp` shell command + PASV/RETR/STOR |
| 💎   | `wget` file downloader                   | ❌ Not built-in (requires install) | ✅ Built-in                | ⬜ built-in with `-O/-q/-c/-r` flags    |
| 💎   | `curl` HTTP tool                         | ✅ Built-in (Windows 10+)          | ✅ Built-in                | ⬜ `-X/-d/-H/-I/-L/-u/-v/-k` flags      |
| 💎   | FTP GUI dual-pane client                 | ✅ FileZilla (3rd party)           | ✅ FileZilla/gFTP          | ⬜ `ftpgui.exe` native dual-pane        |
| 💎   | WiFi connection management               | ✅ Windows WiFi                    | ✅ NetworkManager          | ⬜ §6 -- (stretch ) -- WPA2 +           |
| 💎   | WPA2/WPA3 association                    | ✅ wpa_supplicant via WlanAPI      | ✅ wpa_supplicant          | ⬜ §6 -- (stretch ) -- 4-way handshake  |
| ⭐   | `wget` + `curl` built in from first boot | ❌ `wget` requires manual install  | ✅ Built-in                | ⬜ both ship with OS                    |
| ⭐   | Progress bar shared across `ftp`/`wget`/`curl` | ❌ Inconsistent per tool           | ❌ Inconsistent per tool   | ⬜ `progress_bar_print()` shared helper |
| ⭐   | FTP GUI file manager with native IxUI    | ❌ Requires FileZilla              | ❌ Requires FileZilla/gFTP | ⬜ `ftpgui.exe` built in                |

**Impossible OS advantage:** `wget` and `curl` ship out-of-the-box on first boot -- no installation needed. A consistent `progress_bar_print()` helper gives every download tool the same KB/s + ETA display. The FTP GUI is a native IxUI dual-pane app with no third-party dependency.

---

## Verification

**§1: FTP protocol**
- QEMU (using a host `vsftpd` accessible via NAT): `ftp_connect("10.0.2.2", "testuser", "testpass")` → 230 logged in; `ftp_list()` returns directory listing bytes > 0
- `ftp_download(session, "/pub/test.txt", "C:\\Downloads\\test.txt", cb)` → file written to VFS; `progress_cb` called ≥ once

**§2: FTP shell**
- `ftp ftp.example.com` → `ftp> ls` → lists remote directory; `get README.txt` → file appears at `C:\` with progress bar; `bye` → disconnects cleanly

**§3: wget**
- `wget http://example.com/index.html` → file saved as `index.html`; progress bar shows bytes + KB/s
- `wget https://example.com/` → same via TLS; 🔒 shown in progress prefix
- `wget -O - http://example.com/` → response printed to stdout

**§4: curl**
- `curl https://httpbin.org/get` → JSON response printed to stdout with 2-space indent
- `curl -I https://example.com/` → only response headers printed; no body
- `curl -X POST -d "key=val" http://httpbin.org/post` → 200 response body with posted data reflected

**§5: FTP GUI**
- `ftpgui.exe` → connect to FTP server → both panes populate; select remote file + `[← Download]` → `CTRL_PROGRESSBAR` advances; file appears in local pane after transfer

**§6: WiFi (stretch)**
- `ncpa.cpl` WiFi tab → `[Scan]` → list of nearby SSIDs with signal bars
- Select WPA2 network → enter password → `[Connect]` → `wifi_connect()` completes 4-way handshake → `dhcp_request()` → IP assigned; tab shows ✓ + IP
