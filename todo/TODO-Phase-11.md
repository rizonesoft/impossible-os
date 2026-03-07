# Phase 11 — Advanced Applications

> **Goal:** Build the advanced, network-dependent applications that make Impossible OS
> a capable internet-connected platform: web browser, email client, media player,
> SSH/FTP clients, PDF viewer, remote desktop, network file sharing, and VPN —
> transforming the OS from a standalone system into a connected workstation.

---

## 1. Web Browser
> *Research: [01_web_browser.md](research/phase_11_advanced_apps/01_web_browser.md)*

### 1.1 Phase 1: Text-Only Browser (~500 lines)

- [ ] Create `src/apps/browser/browser.c`
- [ ] HTTP GET: fetch page via `http_get(url, buffer, max)` (from Phase 07)
- [ ] Strip HTML tags: simple state machine (in tag / not in tag)
- [ ] Display plain text content in a scrollable window
- [ ] Extract `<a href="...">` links: display as numbered list
- [ ] Click link → navigate to URL
- [ ] Address bar: type URL, press Enter
- [ ] Back/Forward navigation history (stack of URLs)
- [ ] Commit: `"apps: text-only web browser"`

### 1.2 Phase 2: Basic HTML Renderer (~5,000 lines)

- [ ] HTML parser: tokenizer → DOM tree (tag, attributes, children, text nodes)
- [ ] Support tags:
  - [ ] `<h1>`–`<h6>` — headings (larger font sizes)
  - [ ] `<p>` — paragraphs (vertical spacing)
  - [ ] `<br>` — line break
  - [ ] `<b>`, `<strong>` — bold
  - [ ] `<i>`, `<em>` — italic
  - [ ] `<a href>` — links (underlined, accent color, clickable)
  - [ ] `<ul>`, `<ol>`, `<li>` — lists (bullet/numbered)
  - [ ] `<img src>` — images via `image_load()` + HTTP fetch
  - [ ] `<table>`, `<tr>`, `<td>` — basic table layout
  - [ ] `<hr>` — horizontal rule
  - [ ] `<pre>`, `<code>` — preformatted text (monospace font)
- [ ] Layout engine: block flow (top-to-bottom), inline flow (left-to-right)
- [ ] Text wrapping at window width
- [ ] Vertical scrolling for long pages
- [ ] Commit: `"apps: HTML renderer (block/inline layout)"`

### 1.3 Phase 3: CSS Support (~15,000 lines)

- [ ] *(Stretch)* CSS parser: selectors + properties
- [ ] *(Stretch)* Box model: margin, padding, border, width, height
- [ ] *(Stretch)* Properties: color, background-color, font-size, font-family, text-align
- [ ] *(Stretch)* Selector types: element, class (`.foo`), ID (`#bar`)
- [ ] *(Stretch)* Cascading: inline style > `<style>` block > default
- [ ] Commit: `"apps: CSS support"`

### 1.4 Phase 4: JavaScript (Long-Term)

- [ ] *(Stretch)* Port **QuickJS** (MIT, ~35K lines, ES2020) or **Duktape** (MIT, ~60K lines, ES5.1)
- [ ] *(Stretch)* DOM bindings: `document.getElementById()`, `element.innerHTML`
- [ ] *(Stretch)* Event handling: `onclick`, `addEventListener`
- [ ] Commit: `"apps: JavaScript engine"`

### 1.5 Alternative: Port NetSurf / Dillo

- [ ] *(Stretch)* Evaluate **NetSurf** (GPL, ~200K lines) — full browser, ported to many hobby OSes
- [ ] *(Stretch)* Evaluate **Dillo** (GPL, ~30K lines) — minimalist browser
- [ ] *(Stretch)* Port selected engine to Impossible OS (requires: TCP, DNS, TLS, framebuffer, font rendering)

---

## 2. Email Client
> *Research: [02_email_client.md](research/phase_11_advanced_apps/02_email_client.md)*

### 2.1 Email Protocols

- [ ] Create `src/apps/mail/smtp.c` — SMTP client (send email)
  - [ ] Connect to SMTP server (port 587 with STARTTLS)
  - [ ] EHLO → AUTH LOGIN → MAIL FROM → RCPT TO → DATA → message body → QUIT
  - [ ] TLS encryption (via BearSSL/Mbed TLS from Phase 07)
- [ ] Create `src/apps/mail/pop3.c` — POP3 client (receive email)
  - [ ] Connect to POP3 server (port 995 with TLS)
  - [ ] USER → PASS → STAT → LIST → RETR → DELE → QUIT
  - [ ] Download messages to local mailbox
- [ ] *(Stretch)* Create `src/apps/mail/imap.c` — IMAP client (sync email)
  - [ ] Connect to IMAP server (port 993 with TLS)
  - [ ] LOGIN → SELECT INBOX → FETCH → SEARCH → STORE → LOGOUT
  - [ ] Keep messages on server, sync state
- [ ] Commit: `"apps: email protocol clients (SMTP/POP3)"`

### 2.2 Email App UI

- [ ] Create `src/apps/mail/mail.c`
- [ ] Layout: sidebar (Inbox/Sent/Drafts/Trash) + message list + message view
- [ ] Inbox: list of messages (from, subject, date, read/unread indicator)
- [ ] Click message → display full content in right pane
- [ ] [✏ New] button → compose window (To, Subject, Body, [Send])
- [ ] [Reply] / [Forward] buttons
- [ ] Delete → move to Trash
- [ ] Account setup: server, port, username, password (stored in credential store)
- [ ] Auto-check for new mail every 5 minutes
- [ ] Notification toast on new email
- [ ] Commit: `"apps: email client UI"`

---

## 3. Media Player
> *Research: [03_media_player.md](research/phase_11_advanced_apps/03_media_player.md)*

### 3.1 Media Player App

- [ ] Create `src/apps/mediaplayer/mediaplayer.c`
- [ ] Audio playback via `audio_play()` (from Phase 08 audio system)
- [ ] Load files via `audio_load()` (unified loader: WAV, MP3, OGG, FLAC)
- [ ] Transport controls:
  - [ ] ▶ Play / ⏸ Pause toggle
  - [ ] ⏹ Stop
  - [ ] ⏮ Previous track / ⏭ Next track
- [ ] Seek bar: click to jump to position in track
- [ ] Volume slider with mute toggle
- [ ] Display: song title, artist, album (from filename or ID3 tag)
- [ ] Current time / total time display
- [ ] Commit: `"apps: media player core"`

### 3.2 Playlist

- [ ] Playlist panel: list of tracks with title + duration
- [ ] Add files: drag + drop or File → Add to playlist
- [ ] Remove tracks from playlist
- [ ] Double-click track → play from that position
- [ ] Repeat modes: off / repeat all / repeat one
- [ ] Shuffle toggle
- [ ] Auto-advance to next track on completion
- [ ] Commit: `"apps: media player playlist"`

### 3.3 File Associations

- [ ] Register: `.mp3`, `.wav`, `.ogg`, `.flac` → Media Player
- [ ] Double-click audio file → opens and plays in Media Player
- [ ] Commit: `"apps: media player file associations"`

### 3.4 ID3 Tag Parser (Future)

- [ ] *(Stretch)* Parse MP3 ID3v1 tags (last 128 bytes: title, artist, album)
- [ ] *(Stretch)* Parse MP3 ID3v2 tags (header at start: TIT2, TPE1, TALB, APIC)
- [ ] *(Stretch)* Display album art from embedded APIC frame or folder `cover.jpg`
- [ ] Commit: `"apps: ID3 tag parser"`

---

## 4. SSH Client
> *Research: [04_ssh_client.md](research/phase_11_advanced_apps/04_ssh_client.md)*

### 4.1 SSH Protocol Implementation

- [ ] Create `src/apps/ssh/ssh.c`
- [ ] Choose approach:
  - [ ] Port **dropbear** (MIT, ~15K lines, lightweight) — or —
  - [ ] Custom minimal SSH2 client (~2K lines, transport + auth only)
- [ ] SSH2 transport layer:
  - [ ] TCP connect to server (port 22)
  - [ ] Version exchange: `SSH-2.0-ImpossibleOS`
  - [ ] Key exchange (Diffie-Hellman or Curve25519 via monocypher)
  - [ ] Symmetric encryption (ChaCha20 or AES-CTR)
  - [ ] MAC (HMAC-SHA256)
- [ ] Authentication:
  - [ ] Password auth: `SSH_MSG_USERAUTH_REQUEST` with password
  - [ ] *(Stretch)* Public key auth: Ed25519 key pair
- [ ] Channel:
  - [ ] Open interactive session channel
  - [ ] PTY request
  - [ ] Forward stdin/stdout between terminal and remote shell
- [ ] Commit: `"apps: SSH client"`

### 4.2 SSH Shell Integration

- [ ] Shell command: `ssh user@host` → connect and start interactive session
- [ ] Shell command: `ssh user@host -p 2222` → custom port
- [ ] Runs inside Terminal app (Terminal handles rendering, SSH handles network)
- [ ] *(Stretch)* `scp user@host:file local_file` — file transfer
- [ ] Commit: `"shell: ssh command"`

---

## 5. FTP Client
> *Research: [05_ftp_client.md](research/phase_11_advanced_apps/05_ftp_client.md)*

### 5.1 FTP Protocol

- [ ] Create `src/apps/ftp/ftp.c`
- [ ] Implement FTP control connection (TCP port 21):
  - [ ] `ftp_connect(host, user, pass)` — connect + authenticate (USER, PASS)
  - [ ] `ftp_list(session, listing, max)` — LIST command (directory listing)
  - [ ] `ftp_download(session, remote, local)` — RETR command (download file)
  - [ ] `ftp_upload(session, local, remote)` — STOR command (upload file)
  - [ ] `ftp_disconnect(session)` — QUIT
- [ ] Data connection: passive mode (PASV) — server sends data connection details
- [ ] Additional commands: CWD (cd), PWD, MKD (mkdir), RMD (rmdir), SIZE, DELE
- [ ] Commit: `"apps: FTP client protocol"`

### 5.2 FTP Shell Commands

- [ ] Shell command: `ftp ftp.example.com` → interactive FTP session
  - [ ] Subcommands: `ls`, `cd`, `get <file>`, `put <file>`, `pwd`, `bye`
- [ ] Shell command: `wget ftp://host/path` → anonymous download
- [ ] Commit: `"shell: ftp command"`

### 5.3 FTP GUI (Future)

- [ ] *(Stretch)* Standalone FTP app with dual-pane view (local ↔ remote)
- [ ] *(Stretch)* `ftp://` URL support in File Manager address bar

---

## 6. PDF Viewer
> *Research: [06_pdf_viewer.md](research/phase_11_advanced_apps/06_pdf_viewer.md)*

### 6.1 Minimal PDF Parser

- [ ] Create `src/apps/pdfview/pdfview.c`
- [ ] Parse PDF file structure:
  - [ ] Header: `%PDF-1.x`
  - [ ] Cross-reference table (xref) at end of file
  - [ ] Trailer: root catalog reference
- [ ] Parse page tree: catalog → pages → individual page objects
- [ ] Decompress streams: deflate/zlib via `miniz` (from Phase 03)
- [ ] Commit: `"apps: PDF parser core"`

### 6.2 PDF Rendering

- [ ] Render text content: extract text operators (Tf, Td, Tj, TJ)
- [ ] Map fonts (use system fonts as fallback)
- [ ] Render to `gfx_surface_t` (one surface per page)
- [ ] Render embedded images (via `image_load()`)
- [ ] Commit: `"apps: PDF text and image rendering"`

### 6.3 PDF Viewer UI

- [ ] Display rendered page centered in window
- [ ] Page navigation: Previous/Next buttons, page number input
- [ ] Zoom: fit-to-width, fit-to-page, percentage selection, mouse wheel
- [ ] Scrolling: vertical scroll through page
- [ ] *(Stretch)* Text search: find text on current page, highlight matches
- [ ] *(Stretch)* Multi-page continuous scroll view
- [ ] File association: `.pdf` → PDF Viewer
- [ ] Commit: `"apps: PDF viewer UI"`

---

## 7. Remote Desktop
> *Research: [07_remote_desktop.md](research/phase_11_advanced_apps/07_remote_desktop.md)*

### 7.1 Remote Desktop Protocol (Custom or VNC)

- [ ] *(Stretch)* Create `src/apps/rdp/rdp_protocol.c`
- [ ] *(Stretch)* Choose: VNC (RFB protocol, simpler) or custom protocol
- [ ] *(Stretch)* Frame encoding: compress framebuffer diffs (changed rectangles only)
- [ ] *(Stretch)* Compression: JPEG for lossy speed or zlib for lossless quality
- [ ] *(Stretch)* Input forwarding: keyboard keycodes + mouse position/clicks

### 7.2 Remote Desktop Server

- [ ] *(Stretch)* Create `src/apps/rdp/rdp_server.c`
- [ ] *(Stretch)* Capture compositor backbuffer periodically
- [ ] *(Stretch)* Detect changed tiles (compare with previous frame)
- [ ] *(Stretch)* Compress and send changed tiles to connected client
- [ ] *(Stretch)* Receive keyboard/mouse events from client → inject into WM
- [ ] *(Stretch)* Listen on TCP port 3389 (or custom)
- [ ] *(Stretch)* Password authentication

### 7.3 Remote Desktop Client

- [ ] *(Stretch)* Create `src/apps/rdp/rdp_client.c`
- [ ] *(Stretch)* Connect to remote server → authenticate
- [ ] *(Stretch)* Receive framebuffer tiles → decompress → render in window
- [ ] *(Stretch)* Forward local keyboard/mouse input to server
- [ ] *(Stretch)* Window title: "Remote Desktop — 192.168.1.100"
- [ ] Commit: `"apps: remote desktop (VNC client + server)"`

---

## 8. Network Shares (SMB/NFS)
> *Research: [08_network_shares.md](research/phase_11_advanced_apps/08_network_shares.md)*

### 8.1 Network File Sharing Protocol

- [ ] *(Stretch)* Choose protocol:
  - [ ] **NFS** (simpler, ~1000 lines for basic client) — or —
  - [ ] **SMB2** (Windows compatible, ~5000+ lines) — or —
  - [ ] **Custom** (simplest: HTTP-based file server)
- [ ] Implement chosen protocol client:
  - [ ] `net_share_connect(server, share, user, pass)` — connect to remote share
  - [ ] `net_share_list(session, path, listing, max)` — list remote directory
  - [ ] `net_share_read(session, path, buf, max)` — read remote file
  - [ ] `net_share_write(session, path, buf, len)` — write remote file
- [ ] Commit: `"net: network file sharing client"`

### 8.2 Mount Remote Shares

- [ ] *(Stretch)* Mount remote share as drive letter (e.g., `Z:\`)
- [ ] *(Stretch)* VFS layer: transparent access to remote files
- [ ] *(Stretch)* File Manager: browse `\\server\share` paths
- [ ] *(Stretch)* Shell: `net use Z: \\server\share` — map network drive
- [ ] Commit: `"fs: mount network shares as drive letters"`

---

## 9. VPN Client
> *Research: [09_vpn_client.md](research/phase_11_advanced_apps/09_vpn_client.md)*

### 9.1 WireGuard VPN

- [ ] *(Stretch)* Create `src/kernel/net/wireguard.c`
- [ ] *(Stretch)* WireGuard protocol:
  - [ ] Noise IK handshake (Curve25519 key exchange via monocypher)
  - [ ] ChaCha20-Poly1305 data encryption
  - [ ] UDP transport (we already have UDP)
- [ ] *(Stretch)* Create TUN virtual network interface
- [ ] *(Stretch)* Route traffic through VPN tunnel
- [ ] *(Stretch)* Configuration: server address, public/private keys, allowed IPs
- [ ] *(Stretch)* Settings: `vpn.spl` — connect/disconnect toggle, server config
- [ ] *(Stretch)* System tray: VPN icon when connected (🔒)
- [ ] Commit: `"net: WireGuard VPN client"`

---

## 10. Agent-Recommended Additions

> Items not in the research files but important for a complete app ecosystem.

### 10.1 Download Manager

- [ ] Centralized download tracking for browser, wget, FTP
- [ ] Show: filename, progress bar, speed, ETA
- [ ] Pause / Resume / Cancel downloads
- [ ] Save to `C:\Users\{name}\Downloads\`
- [ ] Notification toast when download completes
- [ ] Commit: `"apps: download manager"`

### 10.2 Torrent Client (Future)

- [ ] *(Stretch)* BitTorrent protocol: tracker announce, peer exchange, piece download
- [ ] *(Stretch)* Bencode parser (torrent file format)
- [ ] *(Stretch)* Depends on: TCP, UDP, advanced networking

### 10.3 Text-to-Speech (Future)

- [ ] *(Stretch)* Simple festival/espeak-like speech synthesis
- [ ] *(Stretch)* Accessibility feature: read UI elements aloud
- [ ] *(Stretch)* Used by: screen reader, browser read-aloud

### 10.4 Chat / Messaging App

- [ ] *(Stretch)* Simple IRC client (IRC protocol is text-based over TCP)
  - [ ] Connect to IRC server
  - [ ] Join channels, send/receive messages
  - [ ] UI: channel list + message area + input
- [ ] Commit: `"apps: IRC chat client"`

### 10.5 Browser Bookmarks & History

- [ ] Bookmarks: save URL + title, display in sidebar
- [ ] History: store visited URLs with timestamps
- [ ] Address bar auto-complete from bookmarks + history
- [ ] Both stored in Codex: `User\{name}\Browser\Bookmarks`, `User\{name}\Browser\History`
- [ ] Commit: `"apps: browser bookmarks and history"`

---

## Priority Order

| Priority | Section | Reason |
|----------|---------|--------|
| 🔴 P0 | 3. Media Player | Audio playback — extends Phase 08 audio work |
| 🔴 P0 | 1.1 Text-Only Browser | First internet app (~500 lines) |
| 🟠 P1 | 1.2 HTML Renderer | Visual web browsing |
| 🟠 P1 | 5.1 FTP Protocol | File transfer (simple protocol) |
| 🟠 P1 | 4.1 SSH Client | Remote server access |
| 🟡 P2 | 6. PDF Viewer | Document viewing |
| 🟡 P2 | 2. Email Client | Communication |
| 🟡 P2 | 10.1 Download Manager | Browser/wget integration |
| 🟡 P2 | 10.5 Browser Bookmarks | Browser usability |
| 🟢 P3 | 1.3 CSS Support | Modern web rendering |
| 🟢 P3 | 3.4 ID3 Tags | Rich media metadata |
| 🟢 P3 | 8. Network Shares | LAN file access |
| 🔵 P4 | 7. Remote Desktop | Remote control |
| 🔵 P4 | 9. VPN Client | Encrypted tunneling |
| 🔵 P4 | 1.4 JavaScript | Full web experience |
| 🔵 P4 | 10.2 Torrent Client | P2P downloads |
| 🔵 P4 | 10.4 IRC Chat | Messaging |
