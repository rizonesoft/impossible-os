---
schema_version: 1
id: collaboration-apps
domain: 11-apps
status: active
title: "TODO-07 -- Collaboration & Network Client Apps"
---

# TODO-07 -- Collaboration & Network Client Apps

> **Goal:** Deliver the remaining network client apps needed for Windows 11 / Linux parity:
> a VNC viewer (with RDP stretch), a minimal IRC chat client, and an RSS/news reader. Also
> adds `arp -a` and `route print` shell commands to the diagnostics toolkit, and a VNC server
> stretch for headless remote access.

> [!IMPORTANT]
> `ping`, `traceroute`, `ifconfig`, `netstat`, and `nslookup` are already fully specified in
> `07-networking/TODO-06-ntp-status-winsock.md §4–§7` -- do not re-implement. §4 and §5 here
> cover only the gaps (`arp -a`, `route print`) and act as cross-references.
> RSS fetching depends on `http_get`/`https_get` from `07-networking/TODO-03`.
> IRC TLS uses `tls_connect`/`tls_send`/`tls_recv` from `07-networking/TODO-03`.
> VNC auth (DES challenge-response) is security-critical -- marked `[Opus]`.

---

## Inputs

- `07-networking/TODO-06-ntp-status-winsock.md §4–§7` -- `ping`, `traceroute`, `ifconfig`, `netstat`, `nslookup`, `arp_cache_dump()` already specified there
- `include/kernel/net/net.h` -- `icmp_send_echo()`, `SYS_PING=15`, `dns_resolve()`, `dns_resolve_reverse()`
- `07-networking/TODO-03-http-tls.md` -- `http_get()`, `https_get()`, `tls_connect()`, `tls_send()`, `tls_recv()`
- `07-networking/TODO-02-dns-sockets.md §5` (and §6 for `kern_bind`/`kern_listen`) -- `kern_socket()`, `kern_connect()`, `kern_send()`, `kern_recv()`, `kern_close()`
- `include/desktop/controls.h` -- `CTRL_BUTTON`, `CTRL_TEXTBOX`, `CTRL_LISTVIEW`, `CTRL_SCROLLBAR_VERT`
- `include/desktop/wm.h` -- `wm_create_window()`, `wm_mark_dirty()`
- `include/gfx.h` -- `gfx_blit()`, `gfx_fill_rect()`, `gfx_surface_create()`
- `include/registry.h` -- `RegSetString`, `RegGetString`, `RegEnumKeyEx`
- `include/kernel/scheduler_tasks.h` -- `sched_task_add()` (→ XREF `09-desktop-shell/TODO-04 §6`)
- `include/kernel/fs/vfs.h` -- `vfs_open`, `vfs_read`, `vfs_write`, `vfs_create` (`vfs_mkdir` is not declared yet)
- `notify_send(title, body, icon_id, timeout_ms)` / `SYS_NOTIFY_SEND=54` (→ XREF `08-graphics-ui/TODO-11 §5`)

---

## Outcome

`vnc.exe host:port` opens a remote desktop viewer. `chat.exe` connects to an IRC server for real-time messaging. `news.exe` shows RSS/Atom feeds with unread badges and auto-refresh. `arp -a` and `route print` complete the network diagnostics toolkit. VNC server stretch enables headless QEMU remote access.

---

## Implementation Order

| Step | Section                                            | 💎/⭐ | Dependency                                                                             | Status |
| ---- | -------------------------------------------------- | ----- | -------------------------------------------------------------------------------------- | :----: |
| 1    | VNC Client                                         | 💎    | §7                                                                                     |  [ ]   |
| 2    | IRC / Chat Client                                  | 💎    | §8                                                                                     |  [ ]   |
| 3    | RSS / News Reader                                  | 💎    | §9                                                                                     |  [ ]   |
| 4    | `ping` + `traceroute`                              | 💎    | → XREF TODO-06 §5–§6 (already specified)                                               |  [ ]   |
| 5    | Network Diagnostic Tools (`arp -a`, `route print`) | ⭐    | `arp_cache_dump` TODO-06 §8; routing table                                             |  [ ]   |
| 6    | VNC Server (Stretch)                               | ⭐    | §1 RFB protocol known, compositor framebuffer                                          |  [ ]   |
| 7    | Import Remote Desktop                              | 💎    | `10-platform-services/TODO-07` §7 (PE programs run), `10-platform-services/TODO-13` §5 |  [ ]   |
| 8    | Import Chat                                        | 💎    | `10-platform-services/TODO-07` §7 (PE programs run), `10-platform-services/TODO-13` §5 |  [ ]   |
| 9    | Import News Reader                                 | 💎    | `10-platform-services/TODO-07` §7 (PE programs run), `10-platform-services/TODO-13` §5 |  [ ]   |

---

## 1. VNC Client

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`shell.md#app-window-layout`](../../docs/design/shell.md#app-window-layout), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

**Moved:** remote-desktop

Built in RemoteDesktop, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §7 has brought it and it is shown to work.

What is this OS's own work stays here:

- [ ] **FramebufferUpdate receive loop** (dedicated kernel thread):
  - [ ] Read message type byte: `FramebufferUpdate (2)` → read `NumRects`; for each rect: x, y, w, h, encoding_type
  - [ ] `Raw`: read `w × h × bytespp` pixels; blit to viewer surface with `gfx_blit()`
  - [ ] `CopyRect`: read `src_x, src_y`; blit sub-rectangle within viewer surface
  - [ ] `RRE`: read `num_subrects`, background pixel; fill background; for each sub-rect: pixel + x, y, w, h → `gfx_fill_rect()`
  - [ ] `Hextile`: process 16×16 tiles with raw / background-colour / subrect encoding flags
  - [ ] Call `wm_mark_dirty()` after each update
- [ ] Shown to work here on the imported copy, as §7 brings it
- [ ] Commit: `"imports: remote-desktop brings VNC Client"`

---

## 2. IRC / Chat Client

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`shell.md#app-window-layout`](../../docs/design/shell.md#app-window-layout), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

**Moved:** chat

Built in Chat, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §8 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §8 brings it
- [ ] Commit: `"imports: chat brings IRC / Chat Client"`

---

## 3. RSS / News Reader

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`shell.md#app-window-layout`](../../docs/design/shell.md#app-window-layout), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

**Moved:** news-reader

Built in NewsReader, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §9 has brought it and it is shown to work.

What is this OS's own work stays here:

- [ ] **UI layout**:
  - [ ] Left sidebar (280 px, `docs/design/shell.md#app-window-layout`): `CTRL_LISTVIEW` of feeds with unread counts
    - `CTRL_LISTVIEW` showing feed name + unread count badge; `[+ Add Feed]` button at top → dialog prompts URL → `news_fetch_feed` once → extract channel/feed title → save to Registry
  - [ ] Item list (middle, `CTRL_LISTVIEW`): title (bold if unread) + date; click → open in reading pane
  - [ ] Reading pane (right): feed title + item title (large TTF); date; description HTML stripped to plain text (same tag-strip logic as email viewer); `[Open in Browser]` button → `file_assoc_open(link_url)` → browser
  - [ ] `[Mark All Read]` button per feed
- [ ] Shown to work here on the imported copy, as §9 brings it
- [ ] Commit: `"imports: news-reader brings RSS / News Reader"`

---

## 4. `ping` + `traceroute` Commands

> → XREF: `07-networking/TODO-06-ntp-status-winsock.md §5` -- `ping` (IPv4 + IPv6 + flags).
> → XREF: `07-networking/TODO-06-ntp-status-winsock.md §6` -- `traceroute` (TTL probe + rDNS).
>
> Both commands are fully specified in TODO-06. This section registers them as shell commands
> in the shell (`user/cmd.c`; `src/shell/` is empty) and ensures the `tracert` alias is wired. No new protocol code here.

- [ ] Extend the existing `cmd_ping` in `user/cmd.c` (today four echoes via `SYS_PING`, no flags, replies only in the kernel log) over `icmp_send_echo()` (`src/kernel/net/icmp.c`); flags `-t`, `-c N`, `-4`, `-6` as specified in TODO-06 §5
- [ ] Shell command `traceroute` (+ `tracert` alias) in `user/cmd.c` → TTL 1..30 UDP probe loop + `dns_resolve_reverse()` as specified in TODO-06 §6
- [ ] Both commands print summary to serial + terminal: RTT per hop, packet loss %, `* * *` for non-responding hops

---

## 5. Network Diagnostic Tools: `arp -a` + `route print`

> → XREF: `07-networking/TODO-06-ntp-status-winsock.md §4` -- `ifconfig` (fully specified).
> → XREF: `07-networking/TODO-06-ntp-status-winsock.md §7` -- `netstat` (fully specified).
> → XREF: `07-networking/TODO-02-dns-sockets.md` -- `nslookup` (specified in DNS TODO).
> This section adds only the two gap commands not covered in TODO-06: `arp -a` and `route print`.

**Source file:** `user/cmd.c` (the shell; `src/shell/` is empty)

- [ ] **`arp -a`**: call `arp_cache_dump(buf, sizeof(buf))` (added to `src/kernel/net/arp.c` in TODO-06 §8); print each entry as `  {IP}    {MAC}    {age_ms}ms` with column alignment; no-entries case: print `ARP cache is empty`
- [ ] **`arp -d <ip>`**: remove specific entry -- `arp_cache_delete(ip)` (new function in `arp.c`); print `Entry for {ip} deleted`
- [ ] **`route print`**: enumerate a new kernel routing table (`struct route_entry[] g_routes` in `src/kernel/net/ip.c`; today `ip.c` sends off-link traffic to the one `net_cfg.gateway`):
  - [ ] Columns: `Destination`, `Netmask`, `Gateway`, `Interface`, `Metric`
  - [ ] Print in same format as Windows `route print` output (two sections: IPv4 Route Table, then Active Routes header)
  - [ ] Include default route (0.0.0.0/0.0.0.0 → gateway) and loopback (127.0.0.0/255.0.0.0)
- [ ] **`route add <dest> mask <mask> <gateway>`**: `route_add(dest, mask, gateway, metric)` → insert into `g_routes[]`; persist to Registry `HKLM\SYSTEM\Network\Routes\{n}\{Dest,Mask,GW,Metric}`
- [ ] **`route delete <dest>`**: `route_delete(dest)` + Registry cleanup

---

## 6. VNC Server (Stretch)

> Captures the live compositor framebuffer and serves it via RFB to remote VNC clients.
> Injects keyboard and mouse events into the WM input queue. Novel: no prior Impossible OS
> component captures the output surface for network streaming.

**Source file:** `src/apps/vnc/vncsrv.c`; header `include/apps/vnc/vncsrv.h`

- [ ] **Listen**: `kern_socket(AF_INET, SOCK_STREAM, 0)` + `kern_bind(fd, 0, 5900)` + `kern_listen(fd, 2)` + `kern_accept()` → one client at a time (single-viewer server)
- [ ] **RFB handshake** (server side): send `"RFB 003.008\n"`; recv client version; send `SecurityTypes=[2]` (VNC auth); recv client selection; send 16-byte random challenge; recv 16-byte DES response; verify against stored password hash (same `des_ecb_encrypt` from §1); send `SecurityResult=0`
- [ ] **ServerInit**: send compositor framebuffer dimensions (`fb_width`, `fb_height`), pixel format (32 bpp, 24 depth, little-endian, true-color, RGB max=255 each, R-shift=16, G-shift=8, B-shift=0), name `"Impossible OS"`
- [ ] **Framebuffer capture**: `vncsrv_get_framebuffer(dirty_rect)` → pointer into compositor output surface (read-only); dirty tracking: maintain `uint8_t dirty_tiles[fb_w/16][fb_h/16]` -- set on each WM composite call via `wm_register_dirty_cb(vncsrv_dirty_cb)`
- [ ] **FramebufferUpdateRequest handler**: client sends request → build `FramebufferUpdate` message from dirty tiles; encode as `Raw` (simple, correct); send `NumRects` + each dirty rect header + pixel data; clear dirty flags
- [ ] **Input injection**:
  - [ ] `KeyEvent` → translate XKeysym back to scan code via reverse `vnc_keysym.h` table → `wm_inject_keyevent(scancode, down)`
  - [ ] `PointerEvent` → translate x,y + button_mask → `wm_inject_mouseevent(x, y, buttons)`
- [ ] **Password**: `HKLM\SYSTEM\VNCServer\Password` (AES-256-GCM encrypted, same key store as §1); length ≤ 8 chars (VNC spec limit); empty = NoAuth
- [ ] **Shell commands**: `vncsrv start` → spawn vncsrv thread + begin listening; `vncsrv stop` → close listener + disconnect client; `vncsrv status` → print bound port + client IP (if connected)
- [ ] **Security note**: VNC server binds to `127.0.0.1` by default; `vncsrv --bind 0.0.0.0` for external access; print warning when binding externally

## 7. Import Remote Desktop

> **Spawned-by:** root

**Design:** n/a -- imports a program, it designs nothing

**Import:** remote-desktop

Remote Desktop is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull remote-desktop` into `src/apps/remote-desktop/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open Remote Desktop on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose remote-desktop`
- [ ] Commit: `"apps: Remote Desktop imported"`

## 8. Import Chat

> **Spawned-by:** root

**Design:** n/a -- imports a program, it designs nothing

**Import:** chat

Chat is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull chat` into `src/apps/chat/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open Chat on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose chat`
- [ ] Commit: `"apps: Chat imported"`

## 9. Import News Reader

> **Spawned-by:** root

**Design:** n/a -- imports a program, it designs nothing

**Import:** news-reader

News Reader is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull news-reader` into `src/apps/news-reader/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open News Reader on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose news-reader`
- [ ] Commit: `"apps: News Reader imported"`

---

## OS Comparison


| ⭐  | Feature                                          | 🪟 Win11                           | 🐧 Linux                          | 🚀 Impossible OS                                       |
| --- | ------------------------------------------------ | ---------------------------------- | --------------------------------- | ------------------------------------------------------ |
| 💎  | VNC viewer                                       | ✅ no built-in; TightVNC / RealVNC | ✅ Remmina / TigerVNC             | ⬜ §1 -- DES auth, keyboard/mouse forward, viewer      |
| 💎  | IRC client                                       | ✅ no built-in; HexChat / mIRC     | ✅ HexChat / irssi / WeeChat      | ⬜ §2 -- nick completion, colored usernames, PING/PONG |
| ⭐  | IRC nick-tab-completion + hash-colored usernames | ❌ no built-in IRC                 | ✅ HexChat built-in               | ⬜ §2 -- `hash(nick) % 8` palette                      |
| 💎  | RSS 2.0 + Atom 1.0 feed reader                   | ✅ no built-in (removed in Win10)  | ✅ Liferea / Newsboat             | ⬜ §3 -- callback XML parser, 30-min auto-refresh,     |
| ⭐  | RSS auto-refresh + toast notification            | ❌ removed from Windows            | ⚠️ Liferea plugin                 | ⬜ §3 -- `sched_task_add(1800)` + `notify_send`        |
| 💎  | `ping` + `traceroute` commands                   | ✅ built-in                        | ✅ built-in                       | ⬜ §5–§6 -- → XREF TODO-06 ; shell                     |
| 💎  | `arp -a` + `route print`                         | ✅ `arp -a`, `route print`         | ✅ `arp -n`, `ip route`           | ⬜ §5 -- `arp_cache_dump` wrapper + routing table      |
| ⭐  | VNC server for headless remote access            | ❌ no built-in VNC server          | ⚠️ `x11vnc` / `wayvnc` (external) | ⬜ §6 -- (Stretch) -- ; compositor fb                  |

Impossible OS ships RSS news reader and IRC client out of the box -- features Windows 11 dropped years ago -- plus a built-in VNC server enabling zero-install remote desktop for QEMU testing, all without any external dependencies beyond the existing TCP/TLS stack.

---

## Verification

Run `bash scripts/build.sh run` for each verification step.

- [ ] **VNC client:** `vnc.exe 10.0.2.2:5900` with a QEMU VNC server (`-vnc :0`); handshake completes; framebuffer displays desktop; mouse move → pointer moves on server; type key → key input registered on server; `[Ctrl+Alt+Del]` button sends the key combo
- [ ] **VNC auth:** start QEMU VNC with a password (`-vnc :0,password`); enter wrong password → `"Authentication failed"` error; enter correct password → connects
- [ ] **IRC connect:** `chat.exe` → connect to `irc.libera.chat:6667`; registration succeeds (`001` Welcome received); join `#test`; send `/msg NickServ help` → response appears in private tab; Tab-complete a partial nickname
- [ ] **IRC TLS:** connect to port 6697 → TLS handshake via `tls_connect`; chat messages appear encrypted in serial packet trace
- [ ] **RSS fetch:** add feed URL `https://news.ycombinator.com/rss` → items appear in item list; unread count shown in sidebar; click item → description in reading pane; `[Open in Browser]` → browser opens link
- [ ] **RSS auto-refresh:** `sched_task_add("news_refresh", ..., 1800, 1)` registered; after simulated 30 min → re-fetch fires; new items appear; toast shown for new articles
- [ ] **`arp -a`:** after sending ICMP ping to 10.0.2.2; `arp -a` prints `10.0.2.2  52:54:00:12:34:56  Nms`; `arp -d 10.0.2.2` removes entry; `arp -a` shows empty
- [ ] **`route print`:** prints at least default route `0.0.0.0 → 10.0.2.2` and loopback `127.0.0.0`; `route add 192.168.0.0 mask 255.255.0.0 10.0.2.2` → new entry appears
- [ ] **VNC server (stretch):** `vncsrv start`; connect with TigerVNC from host → see Impossible OS desktop; mouse/keyboard work; `vncsrv stop` → connection dropped
- [ ] Commit: `"apps: VNC client, IRC, RSS reader, arp/route commands, VNC server stretch"`
