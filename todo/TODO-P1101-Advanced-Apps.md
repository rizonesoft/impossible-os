# P1101 — Advanced Applications (Hub)

> **Goal:** Build the advanced, network-dependent applications that make Impossible OS
> a capable internet-connected platform: web browser, email client, media player,
> SSH/FTP clients, PDF viewer, remote desktop, network file sharing, and VPN.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 1. Web Browser

> **See [TODO-P1102-Browser.md](TODO-P1102-Browser.md)** — Text-only → HTML renderer → CSS → JavaScript.

---

## 2. Email Client

> **See [TODO-P1103-Email.md](TODO-P1103-Email.md)** — SMTP/POP3/IMAP protocols and three-panel UI.

---

## 3. Media Player

> **See [TODO-P0006-Audio.md](TODO-P0006-Audio.md) §6** — Media Player app, playlist, file associations, ID3 tags.

---

## 4. SSH Client

> **See [TODO-P1104-SSH.md](TODO-P1104-SSH.md)** — SSH2 protocol, shell integration.

---

## 5. FTP Client

> **See [TODO-P1105-FTP.md](TODO-P1105-FTP.md)** — FTP protocol, shell commands, GUI.

---

## 6. PDF Viewer

> **See [TODO-P1106-PDF-Viewer.md](TODO-P1106-PDF-Viewer.md)** — PDF parser, renderer, viewer UI.

---

## 7. Remote Desktop

> *(Stretch)* VNC/custom protocol, server, client. Kept in this file.

- [ ] *(Stretch)* Remote desktop protocol (VNC or custom)
- [ ] *(Stretch)* Server: capture compositor, compress, send
- [ ] *(Stretch)* Client: connect, recv frame, forward input
- [ ] Commit: `"apps: remote desktop (VNC client + server)"`

---

## 8. Network Shares (SMB/NFS)

> *(Stretch)* LAN file sharing with drive letter mounting.

- [ ] *(Stretch)* Choose: NFS / SMB2 / custom HTTP
- [ ] *(Stretch)* Mount remote share as drive letter (Z:\)
- [ ] *(Stretch)* File Manager: browse `\\server\share`
- [ ] *(Stretch)* Shell: `net use Z: \\server\share`
- [ ] Commit: `"fs: mount network shares as drive letters"`

---

## 9. VPN Client

> *(Stretch)* WireGuard over UDP with Curve25519 + ChaCha20.

- [ ] *(Stretch)* WireGuard protocol via monocypher
- [ ] *(Stretch)* TUN virtual interface, route traffic
- [ ] *(Stretch)* Settings applet + system tray icon
- [ ] Commit: `"net: WireGuard VPN client"`

---

## 10. Agent-Recommended Additions

- [ ] Download Manager (centralized download tracking)
- [ ] *(Stretch)* BitTorrent client
- [ ] *(Stretch)* IRC chat client
- [ ] Browser bookmarks & history (address bar auto-complete)
- [ ] *(Stretch)* Text-to-speech (accessibility)

---

## Priority Order

| Priority | Section | File |
|----------|---------|------|
| 🔴 P0 | Media Player | `P0006-Audio.md §6` |
| 🔴 P0 | Text-Only Browser | `P1102-Browser.md` |
| 🟠 P1 | HTML Renderer | `P1102-Browser.md` |
| 🟠 P1 | FTP Client | `P1105-FTP.md` |
| 🟠 P1 | SSH Client | `P1104-SSH.md` |
| 🟡 P2 | PDF Viewer | `P1106-PDF-Viewer.md` |
| 🟡 P2 | Email Client | `P1103-Email.md` |
| 🟡 P2 | Download Manager | This file §10 |
| 🟢 P3 | CSS Support | `P1102-Browser.md` |
| 🟢 P3 | Network Shares | This file §8 |
| 🔵 P4 | Remote Desktop | This file §7 |
| 🔵 P4 | VPN Client | This file §9 |
| 🔵 P4 | JavaScript | `P1102-Browser.md` |
