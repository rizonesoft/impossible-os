---
schema_version: 1
id: ftp-wget-wifi
domain: 11-apps
status: active
title: "TODO-02 -- FTP Client, wget/curl & WiFi"
---

# TODO-02 -- FTP Client, wget/curl & WiFi

**Domain:** `11-apps`
**Goal:** Deliver the file-transfer and command-line download toolbox (FTP, wget, curl) that complete the networking app layer, plus a WiFi framework (stretch) for wireless hardware support.

> [!IMPORTANT]
> **Depends on:** `07-networking/TODO-03-http-tls.md` -- `http_get()`, `https_get()`, `http_post()`, `kern_socket()`, `kern_connect()`, `kern_send()`, `kern_recv()`, `kern_close()`, `dns_resolve()` must all be complete before any section here begins.
> **Migration:** Consolidates `TODO-420-FTP.md` (all sections) + `TODO-400-Networking.md` §4.4 (wget/curl) + §9.5 (WiFi) into one file. All other sections of `TODO-400-Networking.md` live in `07-networking`.

---

## Important Notes

- FTP uses **two TCP connections**: control (port 21, ASCII commands) and data (negotiated via `PASV`). Both use `kern_connect()`/`kern_send()`/`kern_recv()` from `07-networking/TODO-02`.
- `https_get()` / `http_get()` from `07-networking/TODO-03` are the only network primitives `wget` and `curl` call -- no raw socket work needed in those tools.
- `vfs_open()`, `vfs_write()`, `vfs_read()`, `vfs_stat()` from `include/kernel/fs/vfs.h` are used for file I/O in download and upload operations.
- `CTRL_LISTVIEW` and `CTRL_PROGRESSBAR` are defined in `08-graphics-ui/TODO-05` -- the FTP GUI (§3) and WiFi settings (§6) depend on them.
- WiFi (§6) is a **stretch goal** with a hardware dependency (USB RTL8188 or QEMU virtio-wifi). Do not start §6 until §1–5 are complete and working.
- `wget -r` (recursive crawl) depends on `TODO-01 §2` (HTML link extractor) for `<a href>` extraction -- cross-link only, don't duplicate.
- Anonymous FTP uses `USER anonymous` / `PASS user@host` by convention.
- Progress display (bytes / total / KB/s / ETA) is common to `ftp_download`, `wget`, and `curl` -- extract into a shared `progress_bar_print(done, total, elapsed_ms)` helper.

---

## Inputs

| Path                                               | Purpose                                                                              |
| -------------------------------------------------- | ------------------------------------------------------------------------------------ |
| `include/kernel/fs/vfs.h`                          | `vfs_open`, `vfs_write`, `vfs_read`, `vfs_stat` -- local file I/O                    |
| `07-networking/TODO-03-http-tls.md`                | `http_get`, `https_get`, `http_post` -- wget/curl network ops                        |
| `07-networking/TODO-02-dns-sockets.md`             | `kern_socket`, `kern_connect`, `kern_send`, `kern_recv`, `kern_close` -- FTP raw TCP |
| `include/registry.h`                               | `registry_get/set()` -- WiFi credentials, ncpa.cpl settings                          |
| `08-graphics-ui/TODO-05 §*`                        | `CTRL_LISTVIEW`, `CTRL_PROGRESSBAR` -- FTP GUI + WiFi settings panel                 |
| → XREF: `07-networking/TODO-03`                    | HTTP/HTTPS client -- mandatory for wget/curl                                         |
| → XREF: `11-apps/TODO-01 §2`                       | HTML link extractor for `wget -r` recursive crawl                                    |
| → XREF: `04-drivers-hardware/TODO-10-usb-stack.md` | USB Stack Completion -- USB WiFi adapter driver prerequisite                         |

---

## Outcome

- `ftp <host>` opens an interactive FTP session with `ls`, `cd`, `get`, `put`, `pwd`, `mkdir`, `rm`, `bye` subcommands.
- `wget <url>` and `wget ftp://host/path` download files with a live progress bar.
- `curl <url>` prints response to stdout; supports `-o`, `-X POST`, `-H`, `-s`, `-I`, `-L`, `-u` flags.
- FTP GUI (stretch): dual-pane local↔remote file manager with drag-to-transfer.
- WiFi framework (stretch): scan → connect (WPA2) → DHCP; `ncpa.cpl` WiFi tab.

---

## Implementation Order

| #   | Section                                                   | Tag        | Dep                                                                                    | Mark | Status |
| --- | --------------------------------------------------------- | ---------- | -------------------------------------------------------------------------------------- | ---- | :----: |
| 1   | FTP protocol core (`ftp_connect`, `PASV`, `RETR`, `STOR`) | `[Sonnet]` | §7                                                                                     | 💎   |  [ ]   |
| 2   | FTP shell commands + `wget ftp://`                        | `[Sonnet]` | §7, §1                                                                                 | 💎   |  [ ]   |
| 3   | `wget` command (HTTP/HTTPS + progress)                    | `[Sonnet]` | §7                                                                                     | 💎   |  [ ]   |
| 4   | `curl` command (full flag set)                            | `[Sonnet]` | §7, §3                                                                                 | 💎   |  [ ]   |
| 5   | FTP GUI client (dual-pane, stretch)                       | `[Sonnet]` | §7, §1                                                                                 | 💎   |  [ ]   |
| 6   | WiFi framework (stretch)                                  | `[Opus]`   | 04-drivers-hardware/TODO-15                                                            | 💎   |  [ ]   |
| 7   | Import FTP                                                | `[Sonnet]` | `10-platform-services/TODO-07` §7 (PE programs run), `10-platform-services/TODO-13` §5 | 💎   |  [ ]   |

---

## 1. FTP Protocol Core

**Moved:** ftp

Built in FTP, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §7 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §7 brings it
- [ ] Commit: `"imports: ftp brings FTP Protocol Core"`

---

## 2. FTP Shell Commands + `wget ftp://`

**Moved:** ftp

Built in FTP, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §7 has brought it and it is shown to work.

What is this OS's own work stays here:

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
- [ ] Shown to work here on the imported copy, as §7 brings it
- [ ] Commit: `"imports: ftp brings FTP Shell Commands + wget ftp://"`

---

## 3. `wget` Command (HTTP/HTTPS + Progress)

**Moved:** ftp

Built in FTP, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §7 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §7 brings it
- [ ] Commit: `"imports: ftp brings wget Command (HTTP/HTTPS + Progress)"`

---

## 4. `curl` Command (Full Flag Set)

**Moved:** ftp

Built in FTP, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §7 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §7 brings it
- [ ] Commit: `"imports: ftp brings curl Command (Full Flag Set)"`

---

## 5. FTP GUI Client (Dual-Pane, Stretch)

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

**Moved:** ftp

Built in FTP, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §7 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §7 brings it
- [ ] Commit: `"imports: ftp brings FTP GUI Client (Dual-Pane, Stretch)"`

---

## 6. WiFi Framework (Stretch)

> Long-term stretch goal. Requires USB host stack (`04-drivers-hardware/TODO-10-usb-stack.md`) and real hardware or QEMU virtio-wifi. Do not start until §1–5 are complete.

- [/] Owned by `04-drivers-hardware/TODO-15-wifi-drivers.md` (device vtable, 802.11 frames, WPA2 supplicant, RTL8188, `ncpa.cpl` tab); this section predates it and names other headers
  - Build Wi-Fi there, not here; keep only anything TODO-15 does not cover. Also note monocypher has no PBKDF2, HMAC-SHA1 or AES, which WPA2 needs.
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

## 7. Import FTP

> **Spawned-by:** root

**Design:** n/a -- imports a program, it designs nothing

**Import:** ftp

FTP is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull ftp` into `src/apps/ftp/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open FTP on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose ftp`
- [ ] Commit: `"apps: FTP imported"`

---

## OS Comparison


| ⭐  | Feature                                        | 🪟 Win11                           | 🐧 Linux                   | 🚀 Impossible OS                        |
| --- | ---------------------------------------------- | ---------------------------------- | -------------------------- | --------------------------------------- |
| 💎  | FTP client                                     | ✅ `ftp.exe` built-in              | ✅ `ftp`/`lftp`            | ⬜ `ftp` shell command + PASV/RETR/STOR |
| 💎  | `wget` file downloader                         | ❌ Not built-in (requires install) | ✅ Built-in                | ⬜ built-in with `-O/-q/-c/-r` flags    |
| 💎  | `curl` HTTP tool                               | ✅ Built-in (Windows 10+)          | ✅ Built-in                | ⬜ `-X/-d/-H/-I/-L/-u/-v/-k` flags      |
| 💎  | FTP GUI dual-pane client                       | ✅ FileZilla (3rd party)           | ✅ FileZilla/gFTP          | ⬜ `ftpgui.exe` native dual-pane        |
| 💎  | WiFi connection management                     | ✅ Windows WiFi                    | ✅ NetworkManager          | ⬜ §6 -- (stretch ) -- WPA2 +           |
| 💎  | WPA2/WPA3 association                          | ✅ wpa_supplicant via WlanAPI      | ✅ wpa_supplicant          | ⬜ §6 -- (stretch ) -- 4-way handshake  |
| ⭐  | `wget` + `curl` built in from first boot       | ❌ `wget` requires manual install  | ✅ Built-in                | ⬜ both ship with OS                    |
| ⭐  | Progress bar shared across `ftp`/`wget`/`curl` | ❌ Inconsistent per tool           | ❌ Inconsistent per tool   | ⬜ `progress_bar_print()` shared helper |
| ⭐  | FTP GUI file manager with native IxUI          | ❌ Requires FileZilla              | ❌ Requires FileZilla/gFTP | ⬜ `ftpgui.exe` built in                |

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
