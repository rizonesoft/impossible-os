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
> `06-networking/TODO-06-ntp-status-winsock.md §4–§7` -- do not re-implement. §4 and §5 here
> cover only the gaps (`arp -a`, `route print`) and act as cross-references.
> RSS fetching depends on `http_get`/`https_get` from `06-networking/TODO-03`.
> IRC TLS uses `tls_connect`/`tls_send`/`tls_recv` from `06-networking/TODO-03`.
> VNC auth (DES challenge-response) is security-critical -- marked `[Opus]`.

---

## Inputs

- `06-networking/TODO-06-ntp-status-winsock.md §4–§7` -- `ping`, `traceroute`, `ifconfig`, `netstat`, `nslookup`, `arp_cache_dump()` already specified there
- `include/kernel/net/net.h` -- `icmp_send_echo()`, `SYS_PING=15`, `dns_resolve()`, `dns_resolve_reverse()`
- `06-networking/TODO-03-http-tls.md` -- `http_get()`, `https_get()`, `tls_connect()`, `tls_send()`, `tls_recv()`
- `06-networking/TODO-02-dns-sockets.md §3` -- `kern_socket()`, `kern_connect()`, `kern_send()`, `kern_recv()`, `kern_close()`
- `include/desktop/controls.h` -- `CTRL_BUTTON`, `CTRL_TEXTBOX`, `CTRL_LISTVIEW`, `CTRL_SCROLLBAR_VERT`
- `include/desktop/wm.h` -- `wm_create_window()`, `wm_mark_dirty()`
- `include/gfx.h` -- `gfx_blit()`, `gfx_fill_rect()`, `gfx_surface_create()`
- `include/registry.h` -- `reg_set_string`, `reg_get_string`, `reg_enum_keys`
- `include/kernel/scheduler_tasks.h` -- `sched_task_add()` (→ XREF `09-desktop-shell/TODO-04 §8`)
- `include/kernel/vfs.h` -- `vfs_open`, `vfs_read`, `vfs_write`, `vfs_create`, `vfs_mkdir`
- `notify_send(title, body, icon_id, timeout_ms)` / `SYS_NOTIFY_SEND=54` (→ XREF `08-graphics-ui/TODO-09 §6`)

---

## Outcome

`vnc.exe host:port` opens a remote desktop viewer. `chat.exe` connects to an IRC server for real-time messaging. `news.exe` shows RSS/Atom feeds with unread badges and auto-refresh. `arp -a` and `route print` complete the network diagnostics toolkit. VNC server stretch enables headless QEMU remote access.

---

## Implementation Order

| Step | Section                                  | 💎/⭐ | Dependency                               |
| ---- | ---------------------------------------- | ----- | ---------------------------------------- |
| 1    | VNC Client                               | 💎    | TCP sockets TODO-02, TLS TODO-03         |
| 2    | IRC / Chat Client                        | 💎    | TCP sockets TODO-02, TLS TODO-03         |
| 3    | RSS / News Reader                        | 💎    | `http_get`/`https_get` TODO-03, `sched_task_add` |
| 4    | `ping` + `traceroute`                    | 💎    | → XREF TODO-06 §5–§6 (already specified) |
| 5    | Network Diagnostic Tools (`arp -a`, `route print`) | ⭐    | `arp_cache_dump` TODO-06 §8; routing table |
| 6    | VNC Server (Stretch)                     | ⭐    | §1 RFB protocol known, compositor framebuffer |

---

## 1. VNC Client `[Opus]`

> VNC DES challenge-response auth is security-critical (password derived key, 8-byte random
> challenge, DES-ECB encryption). `[Opus]` covers §1.2 auth; the rest of the section wires
> standard TCP protocol flows.

**Source file:** `src/apps/vnc/vnc.c`; header `include/apps/vnc/vnc.h`

- [ ] **RFB 3.8 handshake**:
  - [ ] TCP connect to port 5900 (or custom); recv `"RFB 003.008\n"`; send `"RFB 003.008\n"`
  - [ ] Recv `SecurityTypes` list (uint8 count + list); select `2` (VNC auth) if available; else `1` (None)
- [ ] **VNC auth (DES challenge-response)** `[Opus]`:
  - [ ] Recv 16-byte random challenge
  - [ ] Derive DES key from VNC password: take first 8 chars, pad with `\0`; reverse each byte's bit order (VNC-specific: key bit order is reversed per spec); run 2 × `des_ecb_encrypt(challenge_8bytes, key_8bytes)` to produce 16-byte response
  - [ ] Send 16-byte response; recv `SecurityResult` (0 = OK, 1 = fail → show error dialog)
  - [ ] Implement a minimal `des_encrypt_block(key8, data8, out8)` in `vnc_des.c` (~120 lines); no external library needed
- [ ] **ServerInit**:
  - [ ] Send `ClientInit` (shared = 1); recv `ServerInit`: framebuffer `W` × `H`, pixel format (bits-per-pixel, depth, big-endian, true-color, RGB max + shift), server name
  - [ ] Allocate viewer surface `gfx_surface_create(W, H)`
- [ ] **SetEncodings**: send supported encodings list: `Raw (0)`, `CopyRect (1)`, `RRE (2)`, `Hextile (5)` in preferred order
- [ ] **FramebufferUpdateRequest**: send continuous update request (incremental=1, x=y=0, w=W, h=H)
- [ ] **FramebufferUpdate receive loop** (dedicated kernel thread):
  - [ ] Read message type byte: `FramebufferUpdate (2)` → read `NumRects`; for each rect: x, y, w, h, encoding_type
  - [ ] `Raw`: read `w × h × bytespp` pixels; blit to viewer surface with `gfx_blit()`
  - [ ] `CopyRect`: read `src_x, src_y`; blit sub-rectangle within viewer surface
  - [ ] `RRE`: read `num_subrects`, background pixel; fill background; for each sub-rect: pixel + x, y, w, h → `gfx_fill_rect()`
  - [ ] `Hextile`: process 16×16 tiles with raw / background-colour / subrect encoding flags
  - [ ] Call `wm_mark_dirty()` after each update
- [ ] **Input forwarding**:
  - [ ] `KeyEvent`: translate kernel scancode → XKeysym (lookup table in `vnc_keysym.h`); send `0x04 down_flag padding padding keysym`
  - [ ] `PointerEvent`: send `0x05 button_mask x_hi x_lo y_hi y_lo` on every mouse move / click in viewer window
  - [ ] Ctrl+Alt+Del toolbar button → send `KeyEvent` for Ctrl, Alt, Delete (pressed then released)
- [ ] **Viewer window**: `wm_create_window("VNC -- {server_name}", W, H)` with scrollbars if desktop larger than window; toolbar: `[Ctrl+Alt+Del]` `[Disconnect]` `[Fit to window]`; status bar: `"{host}:{port} -- {WxH}"`
- [ ] **Registry**: `HKCU\Software\Impossible\VNC\LastHost`, `LastPort`, `LastPassword` (encrypted via CNG key store); connection dialog pre-fills from Registry
- [ ] **Stretch -- RDP client**: note in code comments that RDP requires NLA auth (NTLM/Kerberos), SSL, and bitmap compression (RDP4/5 bitstream) -- defer to FreeRDP port when Win32 layer is complete

---

## 2. IRC / Chat Client `[Sonnet]`

**Source file:** `src/apps/chat/chat.c`; header `include/apps/chat/chat.h`

- [ ] **Connection**: `kern_socket` + `kern_connect(fd, ip, 6667)` (plain) or `tls_connect(fd, host)` for port 6697 (TLS); `dns_resolve(host, &ip)` first
- [ ] **IRC registration**: send `NICK {nick}\r\n`; `USER {user} 0 * :{realname}\r\n`; wait for `001` Welcome numeric
- [ ] **Channel join**: send `JOIN #{channel}\r\n`; receive `366` (end of NAMES) → channel is active
- [ ] **Receive parser**: read lines until `\r\n`; parse IRC message format `[:{prefix} ] {command} {params}`:
  - [ ] `PRIVMSG #chan :text` → append to channel scrollback
  - [ ] `NOTICE * :text` → display as server notice
  - [ ] `JOIN #chan` → add user to channel user list
  - [ ] `PART #chan` → remove user from list; show "X has left" message
  - [ ] `QUIT :reason` → remove user; show "X has quit" message
  - [ ] `PING :token` → immediately send `PONG :{token}\r\n` (keep-alive)
  - [ ] `432`/`433` (nick collision) → append `_` to nick, retry `NICK`
- [ ] **UI layout**:
  - [ ] Left sidebar (160 px): `CTRL_LISTVIEW` -- server node + channel nodes; click channel → switch to that scrollback
  - [ ] Message area (scrollable `CTRL_SCROLLBAR_VERT`): each line prefixed with `[HH:MM] <nick>` or `* nick action`; username color set by `hash(nick) % palette_size` (8 pastel colors)
  - [ ] Input field (`CTRL_TEXTBOX`, bottom): Enter → send `PRIVMSG {channel} :{text}\r\n`; up-arrow history (last 16 lines)
  - [ ] Tab completion: press Tab → scan user list for `nick` prefix match → cycle through completions; first Tab: append `: ` after nick
- [ ] **Slash commands**: parse input starting with `/`:
  - [ ] `/nick <new>` → `NICK <new>\r\n`
  - [ ] `/join <#channel>` → `JOIN <#channel>\r\n`
  - [ ] `/part [reason]` → `PART <current_channel> :{reason}\r\n`
  - [ ] `/msg <nick> <text>` → `PRIVMSG <nick> :{text}\r\n`; open private chat tab in sidebar
  - [ ] `/quit [reason]` → `QUIT :{reason}\r\n`; disconnect + close window
  - [ ] `/me <action>` → `PRIVMSG {channel} :\x01ACTION {action}\x01\r\n` (CTCP ACTION)
- [ ] **Registry**: `HKCU\Software\Impossible\Chat\{network}\Server`, `Nick`, `AutoJoin`, `UseTLS`; on connect pre-fill from Registry; save on successful connect
- [ ] **Notifications**: `notify_send("IRC", "{nick} in #{channel}: {text}", ICON_CHAT, 4000)` on PRIVMSG when window is not focused
- [ ] **Stretch -- Matrix client**: note that Matrix uses HTTP+JSON (can reuse `https_get`/POST) + Olm E2E encryption (monocypher Curve25519 + AES-GCM); defer full implementation to `10-platform-services` domain when Win32/SDK is stable

---

## 3. RSS / News Reader `[Sonnet]`

**Source file:** `src/apps/news/news.c`; `src/apps/news/rss_parse.c`; header `include/apps/news/news.h`

- [ ] **Feed fetch**: `news_fetch_feed(url, buf, max_len)` → `https_get(url, buf, max_len)` or `http_get()`; accept `application/rss+xml`, `application/atom+xml`, `text/xml`
- [ ] **Minimal XML parser** (`xml_parse.c`): recursive descent without a full DOM tree; tokenize `<tag attr="val">`, `</tag>`, `text content`, CDATA `<![CDATA[...]]>` (pass through raw); callback-based: `on_start_tag(name, attrs)`, `on_end_tag(name)`, `on_text(content)` -- no allocation needed beyond a small stack
- [ ] **RSS 2.0 parser**: on `<item>` → collect `<title>`, `<link>`, `<pubDate>`, `<description>`, `<guid>`; emit `struct news_item { title[256], link[512], pub_date_str[64], description[2048], guid[256], read }`
- [ ] **Atom 1.0 parser**: map `<entry>` → same `news_item`; `<title>` → title; `<id>` → guid; `<updated>` → pub_date; `<summary>` or `<content>` → description; `<link href="...">` → link
- [ ] **Storage**: items saved to `C:\Users\{name}\AppData\News\{feed_hash}\{guid_hash}.dat` (VFS write); `feed_hash = crc32(url) % 10000`; `guid_hash = crc32(guid) % 1000000`; `read` flag persisted in filename suffix `_r` (read) vs no suffix (unread)
- [ ] **Feed registry**: `HKCU\Software\Impossible\News\Feeds\{n}\{URL,Title,LastFetch}` -- enumerate to build feed list
- [ ] **UI layout**:
  - [ ] Left sidebar (200 px): `CTRL_LISTVIEW` showing feed name + unread count badge; `[+ Add Feed]` button at top → dialog prompts URL → `news_fetch_feed` once → extract channel/feed title → save to Registry
  - [ ] Item list (middle, `CTRL_LISTVIEW`): title (bold if unread) + date; click → open in reading pane
  - [ ] Reading pane (right): feed title + item title (large TTF); date; description HTML stripped to plain text (same tag-strip logic as email viewer); `[Open in Browser]` button → `file_assoc_open(link_url)` → browser
  - [ ] `[Mark All Read]` button per feed
- [ ] **Auto-refresh**: `sched_task_add("news_refresh", news_refresh_all_cb, 1800, 1)` (30 min = 1800 s); `news_refresh_all_cb()` fetches all registered feeds, saves new items, updates unread counts; `notify_send("News", "N new articles in {feed_name}", ICON_NEWS, 4000)` if new items found
- [ ] **Feed discovery**: if entered URL returns HTML, scan `<link rel="alternate" type="application/rss+xml" href="...">` in `<head>` → offer discovered feed URL as suggestion
- [ ] **Remove feed**: right-click sidebar → `Remove Feed` → `reg_delete_key` + optionally delete local cache files

---

## 4. `ping` + `traceroute` Commands `[Sonnet]`

> → XREF: `06-networking/TODO-06-ntp-status-winsock.md §5` -- `ping` (IPv4 + IPv6 + flags).
> → XREF: `06-networking/TODO-06-ntp-status-winsock.md §6` -- `traceroute` (TTL probe + rDNS).
>
> Both commands are fully specified in TODO-06. This section registers them as shell commands
> in `src/shell/` and ensures the `tracert` alias is wired. No new protocol code here.

- [ ] Shell command `ping` registered in `src/shell/cmd_ping.c` → calls `icmp_send_echo_request()` (already in `src/kernel/net/icmp.c`); flags `-t`, `-c N`, `-4`, `-6` as specified in TODO-06 §5
- [ ] Shell command `traceroute` (+ `tracert` alias) in `src/shell/cmd_traceroute.c` → TTL 1..30 UDP probe loop + `dns_resolve_reverse()` as specified in TODO-06 §6
- [ ] Both commands print summary to serial + terminal: RTT per hop, packet loss %, `* * *` for non-responding hops

---

## 5. Network Diagnostic Tools: `arp -a` + `route print` `[Sonnet]`

> → XREF: `06-networking/TODO-06-ntp-status-winsock.md §4` -- `ifconfig` (fully specified).
> → XREF: `06-networking/TODO-06-ntp-status-winsock.md §7` -- `netstat` (fully specified).
> → XREF: `06-networking/TODO-02-dns-sockets.md` -- `nslookup` (specified in DNS TODO).
> This section adds only the two gap commands not covered in TODO-06: `arp -a` and `route print`.

**Source file:** `src/shell/cmd_arp.c`; `src/shell/cmd_route.c`

- [ ] **`arp -a`**: call `arp_cache_dump(buf, sizeof(buf))` (added to `src/kernel/net/arp.c` in TODO-06 §8); print each entry as `  {IP}    {MAC}    {age_ms}ms` with column alignment; no-entries case: print `ARP cache is empty`
- [ ] **`arp -d <ip>`**: remove specific entry -- `arp_cache_delete(ip)` (new function in `arp.c`); print `Entry for {ip} deleted`
- [ ] **`route print`**: enumerate the kernel routing table (`struct route_entry[] g_routes` in `src/kernel/net/ip.c`):
  - [ ] Columns: `Destination`, `Netmask`, `Gateway`, `Interface`, `Metric`
  - [ ] Print in same format as Windows `route print` output (two sections: IPv4 Route Table, then Active Routes header)
  - [ ] Include default route (0.0.0.0/0.0.0.0 → gateway) and loopback (127.0.0.0/255.0.0.0)
- [ ] **`route add <dest> mask <mask> <gateway>`**: `route_add(dest, mask, gateway, metric)` → insert into `g_routes[]`; persist to Registry `HKLM\SYSTEM\Network\Routes\{n}\{Dest,Mask,GW,Metric}`
- [ ] **`route delete <dest>`**: `route_delete(dest)` + Registry cleanup

---

## 6. VNC Server (Stretch) `[Opus]`

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

---

## OS Comparison


| ⭐  | Feature                                  | 🪟 Win11                           | 🐧 Linux                         | 🚀 Impossible OS                         |
| --- | ---------------------------------------- | ---------------------------------- | -------------------------------- | ---------------------------------------- |
| 💎  | VNC viewer                               | ✅ no built-in; TightVNC / RealVNC | ✅ Remmina / TigerVNC            | ⬜ §1 -- DES auth, keyboard/mouse forward, viewer |
| 💎  | IRC client                               | ✅ no built-in; HexChat / mIRC     | ✅ HexChat / irssi / WeeChat     | ⬜ §2 -- nick completion, colored usernames, PING/PONG |
| ⭐  | IRC nick-tab-completion + hash-colored usernames | ❌ no built-in IRC                 | ✅ HexChat built-in              | ⬜ §2 -- `hash(nick) % 8` palette        |
| 💎  | RSS 2.0 + Atom 1.0 feed reader           | ✅ no built-in (removed in Win10)  | ✅ Liferea / Newsboat            | ⬜ §3 -- callback XML parser, 30-min auto-refresh, |
| ⭐  | RSS auto-refresh + toast notification    | ❌ removed from Windows            | ⚠️ Liferea plugin                 | ⬜ §3 -- `sched_task_add(1800)` + `notify_send` |
| 💎  | `ping` + `traceroute` commands           | ✅ built-in                        | ✅ built-in                      | ⬜ §5–§6 -- → XREF TODO-06 ; shell       |
| 💎  | `arp -a` + `route print`                 | ✅ `arp -a`, `route print`         | ✅ `arp -n`, `ip route`          | ⬜ §5 -- `arp_cache_dump` wrapper + routing table |
| ⭐  | VNC server for headless remote access    | ❌ no built-in VNC server          | ⚠️ `x11vnc` / `wayvnc` (external) | ⬜ §6 -- (Stretch) -- ; compositor fb    |

Impossible OS ships RSS news reader and IRC client out of the box -- features Windows 11 dropped
years ago -- plus a built-in VNC server enabling zero-install remote desktop for QEMU testing,
all without any external dependencies beyond the existing TCP/TLS stack.

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
