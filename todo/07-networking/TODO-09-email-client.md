---
schema_version: 1
id: email-client-networking
domain: 07-networking
status: active
title: "TODO-09 -- Email Client"
---

# TODO-09 -- Email Client

> **Goal:** Build a full-featured email client: SMTP send (port 587 + STARTTLS), POP3 receive (port 995 + TLS), IMAP sync (port 993 + TLS with IDLE push), account manager (multiple accounts, MX autodiscover, Registry persistence), RFC 2822 message parser + MIME multipart + base64/QP decode, three-panel GUI (sidebar/list/viewer), compose window with attachments, full-text search, desktop notifications with tray icon, and contacts integration.

> [!IMPORTANT]
> Mbed TLS (`tls_connect()`, `tls_send()`, `tls_recv()`, `tls_close()`) from TODO-03 is the mandatory TLS foundation for SMTP STARTTLS, POP3/IMAP over TLS. DNS `dns_resolve()` from TODO-02 is used for MX autodiscover and hostname resolution. The message parser (§3 in implementation order) defines the `mail_message_t` struct and base64/quoted-printable decode utilities used by all protocol sections -- it must be built first. The `ctrl_create_button/textbox/scrollbar` API from `include/desktop/controls.h` and `wm_create_window()` from `include/desktop/wm.h` are the GUI primitives. All message storage uses VFS paths under `C:\Users\Default\AppData\Mail\{account}\{folder}\`; all buffers > 4 KB use `pmm_alloc_contiguous()`.

## Inputs

- `src/kernel/net/tls.c` (TODO-03) -- `tls_connect(fd, host)`, `tls_send()`, `tls_recv()`, `tls_close()` for SMTP STARTTLS, POP3 TLS, IMAP TLS
- `src/kernel/net/dns.c` (TODO-02) -- `dns_resolve(hostname, &ip)` for server connection; add `dns_resolve_mx(domain, mx_host_out)` for MX autodiscover (§5)
- `include/desktop/controls.h` -- `ctrl_create_button/label/textbox/scrollbar`, `ctrl_draw_all`, `ctrl_handle_mouse/key` for all GUI sections
- `include/desktop/wm.h` -- `wm_create_window()` for main mail window, compose window, search dialog
- `src/kernel/fs/vfs.c` -- `vfs_open/read/write/readdir` for `.eml` storage and contacts JSON
- `include/registry.h` -- account settings in `HKCU\Software\ImpossibleMail\Accounts\{name}\*`
- → XREF: `06-networking/TODO-03-http-tls.md` -- Mbed TLS (`tls_connect`) is the mandatory prerequisite for all three protocol sections
- → XREF: `06-networking/TODO-02-dns-sockets.md` -- `dns_resolve()` for SMTP/POP3/IMAP server IPs; extend with `dns_resolve_mx()` for §5 autodiscover
- → XREF: `06-networking/TODO-06-ntp-status-winsock.md` -- tray icon pattern from §3 (network tray) is reused by §9 mail tray envelope icon

## Outcome

- `smtp_send(account, to, subject, body, attachments)` delivers email via port 587 STARTTLS.
- `pop3_sync(account)` downloads new messages as `.eml` files to the local mailbox.
- `imap_sync(account)` full sync with IDLE push; `\Seen`/`\Flagged` state reflected locally.
- Multiple accounts in Registry; MX autodiscover configures server from domain.
- `mail_message_t` parsed from `.eml`: headers (From/To/Subject/Date), MIME multipart, attachments saved to disk.
- Three-panel GUI: folder sidebar, sortable message list (from/subject/date/read indicator), formatted body viewer.
- Compose window: To/CC/BCC/Subject, plain-text + inline markup body, attachment drag-and-drop, Send/Save Draft/Discard.
- Full-text local search and IMAP server-side `SEARCH` forwarding; `SEARCH <term>` shell command.
- Desktop toast notifications + tray envelope icon with unread count; 5-minute background poll.
- Contacts JSON (`contacts.json`) built from received `From:` addresses; autocomplete in compose.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                             | Depends On                                                             | Status |
| --- | :---: | ------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------- | :----: |
| 💎  |   1   | §5 Message parser -- RFC 2822 headers, MIME multipart, base64/QP decode, attachment save               | Nothing -- standalone parser; defines `mail_message_t` used by all     |  [ ]   |
| 💎  |   2   | §1 SMTP client -- TCP 587 + STARTTLS, EHLO, AUTH LOGIN, MAIL FROM/RCPT TO/DATA, MIME UTF-8 send        | §1 parser (MIME body build); Mbed TLS (TODO-03)                        |  [ ]   |
| 💎  |   3   | §2 POP3 client -- TCP 995 + TLS, STAT/LIST/RETR/DELE, `.eml` local store                              | §1 parser (parse downloaded messages); Mbed TLS (TODO-03)              |  [ ]   |
| 💎  |   4   | §3 IMAP client -- TCP 993 + TLS, CAPABILITY/SELECT/FETCH/STORE/SEARCH/EXPUNGE/IDLE                    | §3 POP3 as baseline; Mbed TLS; §1 parser for FETCH body                |  [ ]   |
| 💎  |   5   | §4 Account manager -- settings, multiple accounts, Registry, MX autodiscover wizard                    | §2 SMTP + §3 POP3 + §4 IMAP (all protocol clients must exist to test)  |  [ ]   |
| 💎  |   6   | §9 Notifications -- 5-min poll thread, desktop toast, tray envelope + unread count                    | §3 POP3 or §4 IMAP (poll needs a working sync function); desktop tray  |  [ ]   |
| 💎  |   7   | §6 Email GUI -- three-panel layout (sidebar/list/viewer), keyboard shortcuts, folder navigation        | §1 parser (body display); §5 account manager (folder source); §6 notif |  [ ]   |
| 💎  |   8   | §7 Compose window -- To/CC/BCC/Subject, body markup, attachments, Send/Draft/Discard                   | §2 SMTP (must be able to send); §7 GUI (compose is a sub-window)       |  [ ]   |
| 💎  |   9   | §10 Contacts integration -- `contacts.json`, From: address harvest, autocomplete in compose            | §7 compose (autocomplete target); §1 parser (From: extraction)         |  [ ]   |
| 💎  |  10   | §8 Search -- full-text local grep, `SEARCH` shell command, IMAP server-side SEARCH forward            | §7 GUI (search box in sidebar); §4 IMAP (server search); §1 parser     |  [ ]   |

---

## 1. Message Parser `[Sonnet]`

RFC 2822 header parsing (From, To, CC, BCC, Subject, Date, Message-ID, In-Reply-To). MIME multipart (text/plain + text/html alternatives, `application/octet-stream` attachments). `Content-Transfer-Encoding: base64` decode. Quoted-printable decode. Attachment save-to-disk. Defines `mail_message_t` used by all protocol sections.

**Files:** `src/apps/mail/mime.c` (new), `include/apps/mail/mail.h` (new)

> [!NOTE]
> RFC 2822 header parsing: scan lines until `\r\n\r\n` (empty line); for each line: if starts with `<whitespace>`: continuation of previous header; else: split on first `:`. Store up to 32 headers as `{ name[64], value[512] }` pairs. MIME boundary: `Content-Type: multipart/alternative; boundary="XXXX"` -- scan body for `--XXXX\r\n` part delimiters; each part has its own sub-headers. Base64 decode: RFC 4648 standard alphabet; ignore whitespace; 4-byte → 3-byte groups; use the `base64_decode()` from `src/kernel/net/tls.c` (already exists for PEM parsing). Quoted-printable: `=XX` hex pairs; `=\r\n` soft line breaks (discard). Attachment save: `Content-Disposition: attachment; filename="name"` -- save decoded bytes to `C:\Users\Default\AppData\Mail\Attachments\name` via `vfs_open(CREATE)` + `vfs_write()`.

- [ ] `mail_header_t { char name[64]; char value[512]; }` and `mail_part_t { char content_type[128]; char charset[32]; char encoding[32]; char *body; size_t body_len; uint8_t is_attachment; char filename[256]; }` in `mail.h`
- [ ] `mail_message_t { mail_header_t headers[32]; int header_count; mail_part_t parts[16]; int part_count; char *raw; size_t raw_len; }` in `mail.h`
- [ ] `mail_parse_headers(src, len, &msg)` → header_count: scan lines; split on first `:` after trimming whitespace; fold continuation lines (leading `\t` or ` `)
- [ ] `mail_header_get(msg, name)` → value string or NULL: case-insensitive header name lookup
- [ ] `mime_parse_boundary(content_type_val, boundary_out)`: find `boundary=` parameter; strip quotes
- [ ] `mime_parse_parts(body, body_len, boundary, &msg)`: split on `--boundary`; for each part: `mail_parse_headers()`; detect content-type + encoding; decode body; append to `msg.parts[]`
- [ ] `mime_decode_base64(src, len, out, max)` → decoded_len: use or copy `base64_decode()` from `tls.c`; wrap for inline use
- [ ] `mime_decode_qp(src, len, out, max)` → decoded_len: `=XX` hex decode; `=\r\n` discard; pass-through other bytes
- [ ] `mail_parse(raw, len, &msg)` → 0 or -errno: split at first `\r\n\r\n`; `mail_parse_headers()`; detect top-level Content-Type; if multipart: `mime_parse_parts()`; if plain/html: single part
- [ ] `mail_save_attachment(part, dir_path)` → 0 or -errno: decode body; `vfs_open(CREATE)` path; `vfs_write()`; log filename + size
- [ ] Commit: `"apps/mail: message parser -- RFC 2822 headers, MIME multipart, base64/QP decode, attachment save"`

## 2. SMTP Client `[Sonnet]`

TCP port 587 + STARTTLS upgrade (EHLO → `STARTTLS` capability → `tls_connect` → re-EHLO). AUTH LOGIN (base64 username + base64 password). MAIL FROM, RCPT TO (multiple), DATA + `\r\n.\r\n`. MIME encoding for UTF-8 body (Content-Type + `Content-Transfer-Encoding: quoted-printable`).

**Files:** `src/apps/mail/smtp.c` (new), `include/apps/mail/smtp.h` (new)

> [!NOTE]
> STARTTLS flow (RFC 3207): connect TCP 587; recv `220` greeting; send `EHLO hostname\r\n`; recv capability list (multi-line `250-` prefix + `250 ` final); if list contains `STARTTLS`: send `STARTTLS\r\n`; recv `220`; call `tls_connect(fd, server_host)` -- from now all I/O via `tls_send()`/`tls_recv()`; send `EHLO hostname\r\n` again. AUTH LOGIN (RFC 4616): send `AUTH LOGIN\r\n`; recv `334 <base64 "Username:">`; send `base64(username)\r\n`; recv `334 <base64 "Password:">`; send `base64(password)\r\n`; recv `235 Authentication successful`. DATA: send `DATA\r\n`; recv `354`; send headers + body; end with `\r\n.\r\n` (dot-stuffing: any line starting with `.` must be doubled). QP encoding for body: encode non-ASCII and `=` characters as `=XX`; max 76 chars/line with `=\r\n` soft break.

- [ ] `smtp_session_t { int fd; tls_conn_t *tls; uint8_t use_tls; char server[256]; uint16_t port; }` in `smtp.h`
- [ ] `smtp_readline(sess, buf, max)` → len: use `tls_recv()` or `kern_recv()` based on `use_tls`; read until `\r\n`; handle multi-line `NNN-` prefix (return on `NNN ` final line)
- [ ] `smtp_sendline(sess, line)`: `tls_send()` or `kern_send()` of `line\r\n`
- [ ] `smtp_connect(host, port, &sess)` → 0 or -errno: `dns_resolve(host)` → `kern_connect()`; recv `220`; send `EHLO`; check STARTTLS; upgrade if offered; re-EHLO
- [ ] `smtp_auth_login(sess, username, password)` → 0 or -EACCES: AUTH LOGIN flow; `mime_encode_base64(username, ...)` + `mime_encode_base64(password, ...)`
- [ ] `smtp_send_message(sess, from, to_list[], to_count, subject, body_utf8, attachments[])` → 0 or -errno: MAIL FROM → RCPT TO loop → DATA → build MIME headers + QP-encoded body → send → `\r\n.\r\n` → expect `250`
- [ ] `mime_encode_qp(src, len, out, max)` → encoded_len: encode non-ASCII + `=` as `=XX`; insert soft breaks at col 76
- [ ] `mime_encode_base64(src, len, out, max)` → encoded_len: RFC 4648; output lines ≤ 76 chars
- [ ] `smtp_quit(sess)`: send `QUIT\r\n`; recv `221`; `tls_close(tls)` + `kern_close(fd)`
- [ ] Commit: `"apps/mail/smtp: TCP 587 STARTTLS, AUTH LOGIN base64, MAIL FROM/RCPT TO/DATA, QP-encoded MIME"`

## 3. POP3 Client `[Sonnet]`

TCP port 995 + TLS (immediate TLS -- no STARTTLS for POP3S). USER/PASS auth. STAT (count+size), LIST (per-message sizes), RETR N (download message N), DELE N (mark for deletion), QUIT (commits deletions). Store downloaded messages as `.eml` files in local mailbox folder.

**Files:** `src/apps/mail/pop3.c` (new), `include/apps/mail/pop3.h` (new)

> [!NOTE]
> POP3S (port 995): `tls_connect(fd, host)` immediately after TCP connect (no STARTTLS handshake -- immediate TLS unlike SMTP). All responses start with `+OK` or `-ERR`. Multi-line responses (for RETR, LIST, UIDL): terminated by `\r\n.\r\n` on a line by itself. UIDL command: `UIDL N` returns the unique ID for message N -- store UIDs in `C:\Users\Default\AppData\Mail\{account}\.uidl` to track which messages have been downloaded across sessions; skip messages whose UID is already in the file. Message storage: one `.eml` file per message; filename = `{uid}.eml`; write raw RFC 2822 message bytes without modification (parse on display).

- [ ] `pop3_session_t { int fd; tls_conn_t *tls; char server[256]; }` in `pop3.h`
- [ ] `pop3_connect(host, &sess)` → 0 or -errno: `dns_resolve()` → `kern_connect(port=995)` → `tls_connect(fd, host)`; recv `+OK` greeting
- [ ] `pop3_auth(sess, user, pass)` → 0 or -EACCES: `USER user\r\n` → recv `+OK`; `PASS pass\r\n` → recv `+OK` or `-ERR`
- [ ] `pop3_stat(sess, &count, &total_bytes)`: send `STAT\r\n`; parse `+OK N BYTES`
- [ ] `pop3_list(sess, sizes_out[], max_msgs)` → msg_count: send `LIST\r\n`; read multi-line; parse `N SIZE` per line; terminate on `\r\n.\r\n`
- [ ] `pop3_uidl(sess, uid_out[], max_msgs)` → count: send `UIDL\r\n`; parse `N UID` per line
- [ ] `pop3_retr(sess, msg_num, eml_path)` → bytes: send `RETR N\r\n`; recv `+OK N bytes`; stream body bytes to `vfs_write()` until `\r\n.\r\n` terminator; dot-unstuff
- [ ] `pop3_dele(sess, msg_num)`: send `DELE N\r\n`; recv `+OK`
- [ ] `pop3_quit(sess)`: send `QUIT\r\n`; `tls_close()` + `kern_close()`
- [ ] `pop3_sync(account)`: connect + auth; UIDL → compare with stored `.uidl` file; for new UIDs: RETR → save `.eml`; append UID to file; QUIT
- [ ] Commit: `"apps/mail/pop3: TCP 995 TLS, USER/PASS, STAT/LIST/UIDL/RETR/DELE, .eml local store"`

## 4. IMAP Client `[Opus]`

TCP port 993 + TLS. Tagged command protocol: `A001 CAPABILITY`, `A002 LOGIN`, `A003 SELECT INBOX`, `A004 FETCH 1:* (FLAGS ENVELOPE)`, `A005 FETCH N BODY[]`, `A006 STORE N +FLAGS (\Seen)`, `A007 SEARCH DATE/FROM/SUBJECT`, `A008 EXPUNGE`. IDLE command for push notification. Sync state to local message cache.

**Files:** `src/apps/mail/imap.c` (new), `include/apps/mail/imap.h` (new)

> [!NOTE]
> This is `[Opus]` -- IMAP's tagged asynchronous command protocol is novel: untagged responses (`* N FETCH`, `* EXISTS`, `* RECENT`) can arrive at any time (including during IDLE), interleaved with tagged responses (`A001 OK`, `A001 NO`). Implement a simple synchronous dispatcher: after sending a tagged command, read lines until the matching `TAG OK/NO/BAD` line; buffer any untagged responses seen during the wait. IMAP string types: `NIL`, `{N}` literal (`N` bytes follow on next line), `"quoted"`, unquoted atom. **FETCH response parsing**: `* N FETCH (FLAGS (\Seen) ENVELOPE ("date" "subject" (("from" NIL "name" "domain")) ...))` -- parenthesized list parser needed. **IDLE** (RFC 2177): send `TAG IDLE\r\n`; server sends `+ idling`; client holds connection; server sends `* N EXISTS` on new mail; client sends `DONE\r\n` to return to normal command mode. Use a background thread for IDLE; wake main thread on new message event. **Local cache**: store IMAP UID → local `.eml` path mapping in `C:\Users\Default\AppData\Mail\{account}\.imap_cache`.

- [ ] `imap_session_t { int fd; tls_conn_t *tls; uint32_t tag_counter; char selected_mailbox[128]; uint32_t exists_count; }` in `imap.h`
- [ ] `imap_send_cmd(sess, &tag, fmt, ...)` → tag string: format `A%04u CMD\r\n`; send; increment `sess->tag_counter`
- [ ] `imap_recv_response(sess, expected_tag, untagged_cb, userdata)` → 0 (OK) or -1 (NO/BAD): read lines; pass `* ...` lines to `untagged_cb`; stop on `TAG OK/NO/BAD`
- [ ] `imap_connect(host, &sess)` → 0 or -errno: `kern_connect(port=993)` → `tls_connect(fd, host)`; recv `* OK` greeting
- [ ] `imap_login(sess, user, pass)` → 0 or -EACCES: `LOGIN user pass`; recv tagged OK
- [ ] `imap_select(sess, mailbox, &exists, &recent)`: `SELECT mailbox`; parse `* N EXISTS` + `* N RECENT` untagged; parse `\Flags` list
- [ ] `imap_fetch_envelope(sess, range, envelopes_out[], max)` → count: `FETCH range (FLAGS ENVELOPE)`; parse envelope list per message
- [ ] `imap_fetch_body(sess, msg_uid, eml_path)` → bytes: `FETCH N BODY[]`; recv `{N}` literal; stream to `.eml` file
- [ ] `imap_store(sess, msg_num, flag, set)`: `STORE N +FLAGS (\Seen)` or `-FLAGS`; recv untagged FETCH
- [ ] `imap_search(sess, criteria, results_out[], max)` → count: `SEARCH criteria`; parse `* SEARCH N N N...` untagged
- [ ] `imap_expunge(sess)`: `EXPUNGE`; recv `* N EXPUNGE` for each deleted message
- [ ] `imap_idle_thread(sess, new_mail_cb)`: `IDLE` → wait for `* N EXISTS` → call callback → `DONE`; loops
- [ ] `imap_sync(account)`: select INBOX; fetch all envelopes; for each new UID not in cache: `imap_fetch_body()`; update cache; update unread count
- [ ] Commit: `"apps/mail/imap: TCP 993 TLS, LOGIN/SELECT/FETCH/STORE/SEARCH/EXPUNGE/IDLE, local cache sync"`

## 5. Email Account Manager `[Sonnet]`

Account settings (server host, port, TLS mode, username, password in credential store). Multiple accounts. `HKCU\Software\ImpossibleMail\Accounts\{name}\*` Registry. Account setup wizard with MX autodiscover from domain.

**Files:** `src/apps/mail/accounts.c` (new), `include/apps/mail/accounts.h` (new)

> [!NOTE]
> Registry layout per account: `HKCU\Software\ImpossibleMail\Accounts\<name>\` with DWORD/SZ values: `SMTPHost`, `SMTPPort`, `SMTPTLS` (0=none, 1=STARTTLS, 2=SSL), `POPHost`, `POPPort`, `IMAPHost`, `IMAPPort`, `Username`. Password stored separately in a credential store: `HKCU\Software\ImpossibleMail\Credentials\<account>` as a SZ -- no encryption at this stage (future TODO: DPAPI-style encryption). MX autodiscover (RFC 7208): `dns_resolve_mx(domain, mx_host_out)` -- add to `dns.c`: send DNS query with QTYPE=15 (MX); parse `PREFERENCE EXCHANGE` rdata; sort by preference; return lowest-preference MX host. Try common SMTP ports (587, 465, 25) and IMAP ports (993, 143) using EHLO/CAPABILITY probe to confirm server. Maximum 8 accounts.

- [ ] `mail_account_t { char name[64]; char smtp_host[256]; uint16_t smtp_port; uint8_t smtp_tls; char pop_host[256]; uint16_t pop_port; char imap_host[256]; uint16_t imap_port; char username[256]; char display_name[128]; }` + `mail_accounts[8]` + `mail_account_count`
- [ ] `accounts_load()`: enumerate `HKCU\Software\ImpossibleMail\Accounts\`; read each account's subkeys; populate `mail_accounts[]`
- [ ] `accounts_save(account)`: `RegCreateKeyEx` + `RegSetValueEx` for all fields
- [ ] `accounts_get_password(account_name, pw_out)`: `RegGetValue(HKCU, "Software\\ImpossibleMail\\Credentials\\<name>", ...)`
- [ ] `accounts_set_password(account_name, pw)`: `RegSetValueEx(HKCU, "Software\\ImpossibleMail\\Credentials\\<name>", ...)`
- [ ] `dns_resolve_mx(domain, mx_host_out)` → 0 or -errno: in `dns.c`; QTYPE=15 query; parse MX RDATA (2-byte preference + encoded exchange name); return lowest-preference host
- [ ] `accounts_autodiscover(email_address, &account)`: extract domain from `@`; `dns_resolve_mx(domain, &mx_host)`; probe SMTP/IMAP ports; fill account struct; prompt user to confirm + enter password
- [ ] `accounts_wizard_open()`: `wm_create_window("Add Email Account", ...)`; fields: email, password; button "Auto-configure"; fallback to manual fields; Save → `accounts_save()` + `accounts_set_password()`
- [ ] Commit: `"apps/mail: account manager -- Registry, MX autodiscover, credential store, 8-account multi"`

## 6. Notifications `[Sonnet]`

Background thread polls POP3/IMAP every 5 minutes. Desktop toast: `"New mail from Alice: Re: Meeting"`. System-tray envelope icon with unread count badge. Update badge on read/delete.

**Files:** `src/apps/mail/notify.c` (new), `src/desktop/taskbar.c` (extend)

> [!NOTE]
> Poll thread: `mail_poll_thread()` -- `ksleep(300000)` (5 min) between iterations; for each account: if IMAP: `imap_sync()`; if POP3: `pop3_sync()`; compare new `.eml` count to previous; if delta > 0: call `mail_toast(from, subject)`; update `mail_unread_count`. Toast notification: `mail_toast(from_name, subject)` -- draw a 320×72 rounded rectangle at bottom-right corner of desktop, Z-order top; text `"New mail from <from>: <subject>"`; auto-dismiss after 5 s via `ksleep(5000)` in a brief thread; click → open mail app. Tray icon: envelope glyph (📧 or custom IRES icon) in taskbar tray area; small red badge with unread count (draw integer in 10 px font). IDLE integration: IMAP IDLE thread calls `mail_toast()` directly on `* N EXISTS` without waiting for the 5-min timer.

- [ ] `mail_unread_count` global atomic counter; updated by `pop3_sync()`/`imap_sync()` on new message detection
- [ ] `mail_poll_thread()`: loop: `ksleep(300000)`; iterate `mail_accounts[]`; call appropriate sync; compute delta; trigger toast
- [ ] `mail_toast(from_name, subject)`: create desktop overlay window (320×72, bottom-right, no title bar); draw text; `ksleep(5000)` thread → `wm_destroy_window()`; click handler → `mail_app_open()`
- [ ] `mail_tray_draw(unread)`: in `taskbar.c`; draw envelope icon; if `unread > 0`: draw red badge with count `"N"` (capped display at `"99+"`)
- [ ] `mail_tray_click()`: open mail app or bring to front if already open
- [ ] Call `mail_poll_thread()` as a `task_create("mail_poll", ...)` SCHED_IDLE thread from mail app init
- [ ] Commit: `"apps/mail: notifications -- 5-min poll, toast auto-dismiss, tray envelope badge"`

## 7. Email GUI `[Opus]`

Three-panel layout: sidebar (account tree: Inbox/Sent/Drafts/Trash/custom folders), message list (from, subject, date, read/unread dot, attachment icon), message viewer (formatted header + body). Keyboard: R reply, F forward, Del delete, N new.

**Files:** `src/apps/mail/mail_gui.c` (new), `include/apps/mail/mail_gui.h` (new)

> [!NOTE]
> This is `[Opus]` -- the three-panel mail GUI is the most complex UI in the OS to date, with dynamic pane resizing, folder tree with collapse/expand, and a message list with sort-by-column. **Window layout**: 800×600; sidebar width=200 px (fixed); list width=300 px (resizable); viewer fills remaining right. **Sidebar**: folder tree per account; `Inbox (N)` shows unread count; click → load message list for that folder. **Message list**: scrollable using `ctrl_create_scrollbar`; one row per `.eml` in folder; row = read/unread dot + from + subject + date; click → load viewer; unread rows in bold font. **Viewer pane**: render `mail_header_get(msg, "From")` + `"To"` + `"Subject"` + `"Date"` as formatted header block; body = `text/plain` part rendered via `font_draw_string()` with line wrap; if only `text/html`: strip tags (same state machine as browser §1); attachment links at bottom with "Save" button. **Sort**: click column header cycles ASC/DESC by date/from/subject; re-sort `msg_list[]` in-place.

- [ ] `mail_gui_t { int win_handle; int active_account; int active_folder; int active_msg_index; mail_message_t *msg_list; int msg_count; int scroll_offset; int sort_col; int sort_asc; }` in `mail_gui.h`
- [ ] `mail_gui_open()`: `wm_create_window("Impossible Mail", 100, 60, 800, 600)`; draw 3-pane dividers; load accounts into sidebar; select first account Inbox
- [ ] `mail_sidebar_draw(gui)`: iterate accounts + folders; draw tree rows; highlight selected; unread counts in `(N)` suffix
- [ ] `mail_list_load(gui, account, folder_path)`: scan folder via `vfs_readdir()`; sort `.eml` files by mtime; parse envelope headers only (`mail_parse_headers()` on first 4 KB); store in `msg_list[]`
- [ ] `mail_list_draw(gui)`: draw rows in `[scroll_offset, scroll_offset + visible_rows]`; unread dot (blue circle, 8px) if `\Seen` flag not set; attachment icon if `part_count > 1`; highlight selected row
- [ ] `mail_viewer_load(gui, eml_path)`: `vfs_read()` full `.eml`; `mail_parse()`; render in viewer pane
- [ ] `mail_viewer_draw(gui)`: header block (From/To/Subject/Date in fixed layout); body text with word wrap; attachment rows at bottom with filenames + "Save" buttons
- [ ] `mail_handle_key(gui, key)`: `R` → open compose pre-filled Reply-To; `F` → compose pre-filled Forward; `Del` → move `.eml` to Trash folder + IMAP `STORE \Deleted`; `N` → `mail_compose_open()`; arrows → navigate list
- [ ] `mail_list_sort(gui, col)`: qsort `msg_list[]` by date/from/subject; toggle `sort_asc`
- [ ] Commit: `"apps/mail: three-panel GUI -- sidebar/list/viewer, sort, keyboard shortcuts, attachment viewer"`

## 8. Compose Window `[Sonnet]`

To/CC/BCC/Subject fields. Rich-text body (bold/italic/underline via inline markup). Attachment drag-and-drop. Inline spell-check stub. Send/Save Draft/Discard buttons.

**Files:** `src/apps/mail/compose.c` (new), `include/apps/mail/compose.h` (new)

> [!NOTE]
> Compose window: 640×480 `wm_create_window("New Message", ...)`; toolbar row (Send/Draft/Discard buttons via `ctrl_create_button`); header fields (To/CC/BCC/Subject) as `ctrl_create_textbox`; body as a large `ctrl_create_textbox` (multi-line, scrollable). Reply: pre-fill To = original From; Subject = `"Re: " + original subject`; body = `"\r\n\r\n--- Original ---\r\n" + quoted original body`. Forward: pre-fill Subject = `"Fwd: "`; body = quoted original; no To. Inline markup: `Ctrl+B` → insert `*bold*` markers; `Ctrl+I` → `_italic_`; `Ctrl+U` → `~underline~` (these are plain-text conventions; actual HTML encoding happens in `smtp_send_message()` when building the body part). Attachment drag-and-drop: `ctrl_handle_mouse()` detects a drag event from the desktop file list; capture the file path; add to `attach_list[]`. Spell-check stub: after each word-break char, look up word in a small static dictionary stub; underline unknown words in red -- dictionary is empty initially, so no false positives.

- [ ] `compose_state_t { char to[4][512]; int to_count; char cc[4][512]; int cc_count; char bcc[4][512]; char subject[512]; char body[65536]; char attach_paths[8][2048]; int attach_count; uint8_t is_reply; uint8_t is_draft; }` in `compose.h`
- [ ] `mail_compose_open(pre_fill)`: create window; create 6 `ctrl_create_textbox` controls (To/CC/BCC/Subject/body); 3 buttons (Send/Draft/Discard); register click callbacks
- [ ] `compose_on_send(state)`: build `to_list[]` from comma-split To field; call `smtp_send_message(active_account, from, to_list, subject, body, attachments)`; close window on `250` response
- [ ] `compose_on_draft(state)`: serialize to `.eml` format; write to `C:\Users\Default\AppData\Mail\{account}\Drafts\{timestamp}.eml`
- [ ] `compose_on_discard(state)`: confirm dialog ("Discard message?"); close window
- [ ] Keyboard in body: `Ctrl+B/I/U` insert inline markup markers; `Ctrl+Enter` → Send
- [ ] Attachment panel: drag-drop file path → `attach_list[]`; draw attachment chips (filename × remove-button) below body field
- [ ] `compose_reply(original_msg)` / `compose_forward(original_msg)`: pre-fill state from original message
- [ ] Commit: `"apps/mail: compose window -- To/CC/BCC/Subject, body markup, attachments, reply/forward"`

## 9. Contacts Integration `[Sonnet]`

Parse `From:` addresses from all received messages into a contacts cache. Autocomplete To/CC from contacts in compose. Store in `C:\Users\Default\AppData\Contacts\contacts.json`.

**Files:** `src/apps/mail/contacts.c` (new), `include/apps/mail/contacts.h` (new)

> [!NOTE]
> `contacts.json` format: `[{"name":"Alice Smith","email":"alice@example.com","count":5}, …]` -- flat array, sorted by `count` descending (most-contacted first). After each `pop3_sync()`/`imap_sync()`: parse `From:` header of each new message; extract display name + email address (RFC 2822 format: `"Display Name" <email@host>` or bare `email@host`); if email not in contacts: add with `count=1`; else: increment `count`. Autocomplete in compose: in `ctrl_handle_key()` for To/CC textbox fields: if current input matches a contact email/name prefix: show a dropdown popup with up to 5 matching contacts; Enter/click selects and appends `"Name" <email>`. JSON I/O: hand-written serializer/deserializer (same pattern as browser bookmarks in TODO-07 §8).

- [ ] `contact_t { char name[128]; char email[256]; uint32_t count; }` + `contacts[512]` + `contact_count`
- [ ] `contacts_load(path)` → count: `vfs_open` + read JSON; parse array of `{name,email,count}` objects
- [ ] `contacts_save(path)`: serialize all contacts to JSON; `vfs_write()`
- [ ] `contacts_update(display_name, email)`: if email found: `count++`; else: add new entry at end; re-sort by count descending after batch updates
- [ ] `contacts_autocomplete(prefix, results_out[], max)` → count: case-insensitive prefix match on name and email; return up to `max` matches sorted by `count`
- [ ] `compose_autocomplete_popup(ctrl_id, prefix)`: call `contacts_autocomplete()`; draw popup below To/CC field; arrow keys navigate; Enter selects
- [ ] `mail_harvest_contacts(msg)`: call `contacts_update()` from `From:` and `Reply-To:` header values of each newly synced message; call `contacts_save()` after batch
- [ ] Commit: `"apps/mail: contacts -- JSON load/save, From: harvest, autocomplete in compose To/CC"`

## 10. Search `[Sonnet]`

Full-text search over local message cache (grep headers + body). `SEARCH <term>` shell command. IMAP server-side `SEARCH` forwarding. Search box in sidebar.

**Files:** `src/apps/mail/search.c` (new), `src/shell/cmd_search_mail.c` (new)

> [!NOTE]
> Local search: `mail_search_local(term, account, folder, results_out[], max)` → count: iterate `.eml` files in folder; for each: read first 4 KB (headers); if term found: add to results; else: read full body; check; case-insensitive substring match using `kmemchr`-based loop. IMAP server-side search: if the active folder is an IMAP folder: call `imap_search(sess, criteria)` first; combine local + server results (deduplicate by UID). GUI integration: search text box in sidebar header row; on Enter: `mail_search_local()` → replace message list with search results; "Clear search" button restores folder view. `SEARCH <term>` shell command: iterate all accounts + all folders; print matching messages as `account/folder/uid: From: ... Subject: ...`.

- [ ] `mail_search_result_t { char eml_path[2048]; char from[256]; char subject[256]; char date[64]; }` in `search.c`
- [ ] `mail_search_local(term, account, folder_path, results[], max)` → count: `vfs_readdir()` loop; `vfs_read()` up to 4 KB; case-insensitive scan headers; if not found: read remainder of file; scan body
- [ ] `mail_search_imap(term, account, results[], max)` → count: build IMAP search criteria from term (use `SUBJECT` + `FROM` + `BODY` OR criteria); `imap_search()`; fetch envelope for each matching UID
- [ ] `mail_search(term, account, folder, results[], max)` → count: local first; if IMAP account and online: append server results; deduplicate by UID/path
- [ ] GUI search box in `mail_gui_t`: `ctrl_create_textbox` in sidebar top area; on Enter → `mail_search()` → populate `msg_list[]` with results; add `[Clear]` button
- [ ] `cmd_search_mail(argc, argv)`: `SEARCH <term>` shell command; iterate all accounts + `Inbox/Sent`; print matches; register in shell command table
- [ ] Commit: `"apps/mail: search -- local grep + IMAP SEARCH, sidebar search box, SEARCH shell command"`

---

## OS Comparison


| ⭐  | Feature                                  | 🪟 Win11                                 | 🐧 Linux                                 | 🚀 Impossible OS                         |
| --- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- |
| 💎  | Message parser                           | ✅ Windows Mail / Outlook: full          | ✅ Thunderbird/Evolution: full MIME; `libmime` / | ⬜ §1 -- hand-written MIME parser; `base64_decode` reused |
| 💎  | SMTP client                              | ✅ `System.Net.Mail.SmtpClient`; Outlook SMTP; full AUTH | ✅ `sendmail`/`postfix` + `libesmtp`; Thunderbird | ⬜ §2 -- STARTTLS via Mbed TLS; QP       |
| 💎  | POP3 client                              | ✅ Windows Mail (POP3); Outlook POP3;    | ✅ Thunderbird/Evolution POP3; `fetchmail` | ⬜ §3 -- immediate TLS on port 995       |
| 💎  | IMAP client                              | ✅ Windows Mail (IMAP); Outlook IMAP;    | ✅ Thunderbird/Evolution IMAP; `mutt`; full IMAP4rev1 | ⬜ §4 -- tagged command protocol; parenthesized response |
| 💎  | Account manager                          | ✅ Windows Mail account wizard; Outlook  | ✅ Thunderbird account setup wizard; `autoconfig` | ⬜ §5 -- `dns_resolve_mx()` MX QTYPE=15; port probing |
| 💎  | Toast notifications + tray envelope badge | ✅ Windows notification center; badge on | ✅ GNOME/KDE desktop notifications; tray indicator | ⬜ §6 -- 5-s auto-dismiss toast overlay; tray |
| 💎  | Three-panel GUI                          | ✅ Outlook / Windows Mail: full          | ✅ Thunderbird/Evolution: three-panel; GNOME Mail | ⬜ §7 -- `controls.h` widget library; sort by |
| 💎  | Compose window                           | ✅ Outlook compose; HTML rich-text; drag-and-drop | ✅ Thunderbird compose; HTML editor; attachment | ⬜ §8 -- plain-text with `*bold*` inline markers |
| 💎  | Contacts integration                     | ✅ Outlook/People app; Exchange GAL; vCard | ✅ GNOME Contacts + Evolution integration; | ⬜ §9 -- `contacts.json`; count-sorted autocomplete; batch harvest |
| 💎  | Full-text search                         | ✅ Windows Search indexes mail; Outlook  | ✅ Thunderbird search; `notmuch` / `mu`  | ⬜ §10 -- local header+body grep; IMAP `SEARCH` |

> **After §1–§10:** Impossible OS has a kernel-native email client rivalling Windows 11 Mail and Linux Thunderbird -- SMTP/POP3/IMAP with TLS, three-panel GUI, compose with attachments, IMAP IDLE push, contacts autocomplete, and full-text search. All built on the Mbed TLS and DNS infrastructure already in place, with zero external email library dependencies.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Message parser: parse a hand-crafted `.eml` with MIME multipart (plain + html parts + attachment); `mail_header_get(&msg, "Subject")` returns correct subject; attachment saved to `Attachments\` folder
- [ ] SMTP: `smtp_send_message(account, "test@example.com", "Hello", "Body text", NULL)` → serial log shows `[SMTP] 250 OK`; STARTTLS log shows `[SMTP] TLS secured`
- [ ] POP3: `pop3_sync(account)` with a real QEMU-routed POP3S server → new `.eml` files appear in `Inbox\`; repeat sync skips already-downloaded UIDs
- [ ] IMAP: `imap_select(sess, "INBOX", &exists, &recent)` → `exists > 0`; `imap_fetch_envelope()` returns From/Subject for first 5 messages; `imap_store(N, "\\Seen", 1)` → next fetch shows `\Seen` flag
- [ ] IMAP IDLE: leave IDLE active; deliver a message to the server; serial log shows `[IMAP] IDLE: new EXISTS=N` within 30 s; toast appears on desktop
- [ ] Account manager: autodiscover `gmail.com` → MX resolves to `aspmx.l.google.com`; wizard populates SMTP/IMAP fields; save to Registry; reload after reboot
- [ ] Notifications: `ksleep(5000)` in test harness triggers poll; new test message → toast visible at bottom-right; tray badge shows `"1"`; open mail app → unread dot on message row
- [ ] GUI: mail app opens; sidebar shows account/Inbox; click Inbox → message list with dates; click message → body visible in viewer; `Del` moves message to Trash folder
- [ ] Compose: `N` key → compose window; fill To + Subject + body; `Ctrl+Enter` → send → `[SMTP] 250 OK`; "Reply" from message viewer pre-fills To + `"Re: "`
- [ ] Contacts: after sync harvests From addresses; open compose; type first 3 chars of contact name → autocomplete dropdown shows matching contact; select → To field populated
- [ ] Search: `SEARCH meeting` in sidebar → message list filters to matching messages; `SEARCH meeting` shell command → prints matching messages from all folders
- [ ] Commit: `"apps/mail: complete email client -- SMTP/POP3/IMAP, three-panel GUI, compose, contacts, search"`
