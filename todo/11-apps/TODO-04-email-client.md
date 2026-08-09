---
schema_version: 1
id: email-client
domain: 11-apps
status: active
title: "TODO-04 -- Email Client"
---

# TODO-04 -- Email Client

> **Goal:** Build `mail.exe` -- a production-quality email client for Impossible OS with SMTP,
> POP3, IMAP, a three-panel IxUI GUI, CNG-encrypted credential storage, a contacts book,
> scheduled auto-check with toast + tray notifications, and a keyword-based spam/junk filter.

> [!IMPORTANT]
> `07-networking/TODO-09-email-client.md` is the canonical email **protocol** implementation TODO
> (message parser, SMTP, POP3, IMAP, account manager, notifications, GUI, compose, contacts,
> search -- §1–§10). This TODO extends that spec with: richer GUI detail (attachment indicators,
> search bar, read/unread state), CNG credential encryption specifics (`cng_aes256gcm_encrypt`
> with user login KEK), contacts CSV path and autocomplete wiring, spam/junk filter (not covered
> in TODO-09), taskbar badge + system tray envelope icon, and the 5-minute `sched_task_add` poll.
> Implement §1–§3 in coordination with TODO-09 §2–§5; do not re-implement what TODO-09 already
> specifies -- use XREFs to stay aligned.
>
> Mbed TLS (`tls_connect`, `tls_send`, `tls_recv`) must be complete before STARTTLS/TLS work
> begins -- `→ XREF: 07-networking/TODO-03`.
> CNG AES-256-GCM and key store must be complete before §6 credential storage --
> `→ XREF: 09-desktop-shell/TODO-07 §1 + §4`.

---

## Inputs

- `07-networking/TODO-09-email-client.md` -- canonical protocol implementation reference (§1 parser, §2 SMTP, §4 POP3, §5 IMAP, §8 GUI, §9 compose, §10 contacts, §11 search)
- `include/kernel/net/tls.h` -- `tls_connect(fd, hostname)`, `tls_send()`, `tls_recv()`, `tls_close()` (→ XREF `07-networking/TODO-03`)
- `include/kernel/net/dns.h` -- `dns_resolve(hostname, &ip)`
- `include/kernel/cng/cng.h` -- `cng_aes256gcm_encrypt()`, `cng_aes256gcm_decrypt()` (→ XREF `09-desktop-shell/TODO-07 §1`)
- `include/kernel/cng/cng_keystore.h` -- `cng_keystore_get_kek()`, `cng_key_store_import()`, `cng_key_store_get()` (→ XREF `09-desktop-shell/TODO-07 §4`)
- `include/desktop/controls.h` -- `ctrl_create_button`, `ctrl_create_textbox`, `ctrl_create_listview`, `ctrl_create_scrollbar`
- `include/desktop/wm.h` -- `wm_create_window()`
- `include/desktop/systray.h` (→ XREF `08-graphics-ui/TODO-09 §5`) -- `tray_register()`, `tray_unregister()`
- `include/kernel/scheduler_tasks.h` (→ XREF `09-desktop-shell/TODO-04 §8`) -- `sched_task_add(name, cb, interval_s, enabled)`
- `include/registry.h` -- `reg_set_string`, `reg_get_string`, `reg_create_key`, `reg_delete_key`
- `include/kernel/vfs.h` -- `vfs_open`, `vfs_read`, `vfs_write`, `vfs_create`, `vfs_mkdir`, `vfs_unlink`
- `notify_send(title, body, icon_id, timeout_ms)` / `SYS_NOTIFY_SEND=54` (→ XREF `08-graphics-ui/TODO-09 §6`)

---

## Outcome

`mail.exe` opens a three-panel window (account sidebar / message list / viewer). Users can send email via SMTP with STARTTLS, receive via POP3 (or IMAP stretch), compose with To-autocomplete, reply, forward, and delete to Trash. Account passwords are encrypted at rest via AES-256-GCM with the user's login KEK. The client polls every 5 minutes and fires a toast notification + tray badge on new mail. A keyword-based spam filter moves junk to the Junk folder, trainable via `[Mark as Spam]` / `[Not Spam]`.

---

## Implementation Order

| Step | Section                               | 💎/⭐ | Dependency                             |
| ---- | ------------------------------------- | ----- | -------------------------------------- |
| 1    | SMTP Client                           | 💎    | Mbed TLS TODO-03, DNS TODO-02          |
| 2    | POP3 Client + Message Storage         | 💎    | §1 complete                            |
| 3    | IMAP Client (Stretch)                 | 💎    | §1 complete, TLS                       |
| 4    | Three-Panel Email GUI                 | 💎    | §2 complete, controls.h, wm.h          |
| 5    | Compose Window                        | 💎    | §4 complete, §7 contacts               |
| 6    | Account Management + Credential Store | ⭐    | CNG TODO-07 §1+§4, Registry            |
| 7    | Auto-Check + Notifications + Tray     | ⭐    | §2 complete, TODO-04 §8, TODO-09 §5+§6 |
| 8    | Contacts Store                        | 💎    | VFS, §5 compose window                 |
| 9    | Spam & Junk Filter                    | ⭐    | §4 complete, Registry                  |

---

## 1. SMTP Client `[Sonnet]`

> → XREF: `07-networking/TODO-09-email-client.md §2` -- base implementation spec.
> This section adds the STARTTLS negotiation detail and error-code retry behavior.

**Source file:** `src/apps/mail/smtp.c`; header `include/apps/mail/smtp.h`

- [ ] `smtp_connect(host, port)` → `kern_socket` + `kern_connect(fd, ip, port)` (default port 587)
- [ ] Read server greeting `220`; send `EHLO impossible-os`; parse multi-line `250-` capability list
- [ ] STARTTLS upgrade: if capability `STARTTLS` advertised → send `STARTTLS\r\n`; recv `220 Go ahead`; call `tls_connect(fd, hostname)`; re-send `EHLO impossible-os` after TLS established
- [ ] `AUTH LOGIN`: send `AUTH LOGIN\r\n`; Base64-encode username → send; Base64-encode password → send; check `235` OK
- [ ] `smtp_send(session, from, to, subject, body)`:
  - [ ] `MAIL FROM:<{from}>\r\n` → expect `250`
  - [ ] `RCPT TO:<{to}>\r\n` → expect `250` (call once per recipient)
  - [ ] `DATA\r\n` → expect `354`; send headers: `Date: {RFC 2822 date}\r\nFrom: {from}\r\nTo: {to}\r\nSubject: {subject}\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n{body}\r\n.\r\n`
  - [ ] Expect `250 OK`; `QUIT\r\n`
- [ ] Error handling: `421` (temp fail) → retry after 30 s, up to 3 attempts; `550` (reject) → log to serial + notify caller; other `5xx` → log + fail

---

## 2. POP3 Client + Message Storage `[Sonnet]`

> → XREF: `07-networking/TODO-09-email-client.md §4` -- base implementation spec.

**Source file:** `src/apps/mail/pop3.c`; header `include/apps/mail/pop3.h`

- [ ] `pop3_connect(host)` → `kern_socket` + `kern_connect(fd, ip, 995)` → `tls_connect(fd, host)` (implicit TLS on 995)
- [ ] `USER {username}\r\n` → `+OK`; `PASS {password}\r\n` → `+OK`
- [ ] `STAT\r\n` → parse `+OK {count} {total_bytes}` -- store message count
- [ ] `LIST\r\n` → read multi-line: map message number → size in bytes
- [ ] For each new message (not already stored locally):
  - [ ] `RETR {n}\r\n` → read until `.\r\n` into `pmm_alloc_contiguous()` buffer
  - [ ] Parse RFC 822 headers: `Date:`, `From:`, `To:`, `Subject:`, `Message-ID:`, `Content-Type:`
  - [ ] Store as `C:\Users\{name}\AppData\Mail\{account}\Inbox\{id}.eml` via `vfs_create` + `vfs_write`; `id` = sanitized `Message-ID` or `msg_{n}_{timestamp}`
  - [ ] `DELE {n}\r\n` → `+OK` (mark for deletion on server)
- [ ] `QUIT\r\n` → server deletes marked messages
- [ ] Return new-message count to §7 auto-check

---

## 3. IMAP Client (Stretch) `[Opus]`

> → XREF: `07-networking/TODO-09-email-client.md §5` -- base implementation spec.
> This section adds IDLE push notification and flag sync detail.

**Source file:** `src/apps/mail/imap.c`; header `include/apps/mail/imap.h`

- [ ] `imap_connect(host)` → port 993 + `tls_connect(fd, host)` → read `* OK` greeting
- [ ] `{tag} LOGIN {user} {pass}\r\n` → `{tag} OK`
- [ ] `{tag} SELECT INBOX\r\n` → parse `* {n} EXISTS`, `* {n} RECENT`, `[UIDNEXT {n}]`
- [ ] `{tag} FETCH 1:* (FLAGS ENVELOPE)\r\n` → parse each message: UID, flags (`\Seen`, `\Flagged`, `\Deleted`), envelope (Date, Subject, From, To); store in in-memory message list
- [ ] `{tag} FETCH {n} BODY[]\r\n` → read literal `{N}` bytes → store to VFS `.eml` file
- [ ] `{tag} STORE {n} +FLAGS (\Seen)\r\n` → mark read on server
- [ ] `{tag} EXPUNGE\r\n` → purge deleted messages
- [ ] IDLE: after sync, send `{tag} IDLE\r\n`; recv `+ idling`; wait for `* {n} EXISTS` (new mail) or `DONE` timeout (29 min per RFC); send `DONE\r\n` to exit IDLE; re-FETCH new messages
- [ ] `{tag} LOGOUT\r\n` → close TLS connection

---

## 4. Three-Panel Email GUI `[Opus]`

> → XREF: `07-networking/TODO-09-email-client.md §8`

**Source file:** `src/apps/mail/mail.c`; window title `Mail`

Layout (fixed proportions): sidebar 200 px | message list 350 px | viewer fills remainder

- [ ] **Sidebar** (`CTRL_LISTVIEW` in tree mode): per-account nodes expandable to Inbox / Sent / Drafts / Trash / Junk; unread count shown in parentheses next to Inbox; click folder → refresh message list
- [ ] **Message List** (scrollable `CTRL_LISTVIEW`): columns -- read/unread dot (● blue = unread), From (truncated), Subject, Date, attachment clip icon (📎 if `.eml` has `Content-Type: multipart`); click row → load viewer; double-click → open in separate window; right-click → context menu (Reply / Forward / Delete / Mark as Spam / Mark as Read)
- [ ] **Message Viewer**: header bar: From, To, Date, Subject in styled TTF; attachment list if multipart (click attachment → `vfs_open` + launch or save dialog); body: plain text rendered via `ttf_draw_string()` word-wrapped to panel width; HTML body stripped to plain text (remove tags, preserve whitespace, decode `&amp;`/`&lt;`/`&gt;`/`&nbsp;`); external images blocked by default (link "Show images" → re-render)
- [ ] **Toolbar**: `[✏ Compose]` `[↩ Reply]` `[↪ Forward]` `[🗑 Delete]` `[🔄 Sync]`; search bar on right (type → filter message list by From + Subject)
- [ ] **Status bar**: shows `Inbox -- {n} messages, {k} unread` | sync status (`Last synced: {time}` or `Syncing...`)
- [ ] Load messages from VFS `C:\Users\{name}\AppData\Mail\{account}\{folder}\` on folder select; parse `.eml` file headers for list display; full body loaded only when message is opened

---

## 5. Compose Window `[Sonnet]`

> → XREF: `07-networking/TODO-09-email-client.md §9`

**Source file:** `src/apps/mail/compose.c`

- [ ] `compose_open(to, subject, body_prefix)` → `wm_create_window()` modal or top-level
- [ ] Fields: **To** (`CTRL_TEXTBOX` with autocomplete -- see §8 contacts), **Subject** (`CTRL_TEXTBOX`), **Body** (`CTRL_TEXTBOX` multiline with scrollbar)
- [ ] Toolbar: `[Send]` `[📎 Attach]` `[✕ Discard]`
- [ ] `[📎 Attach]` → open file-picker dialog → read file via VFS → add as MIME `multipart/mixed` attachment; show attached filename below toolbar
- [ ] `[Send]` → validate To field (must contain `@`); call `smtp_send()` → on success: move draft to Sent folder (`C:\Users\{name}\AppData\Mail\{account}\Sent\`); close window; on failure: show error toast `"Send failed: {error}"`
- [ ] **Reply**: `compose_open(original_from, "Re: {subject}", "> {original_body_lines}")`
- [ ] **Forward**: `compose_open("", "Fwd: {subject}", "---------- Forwarded message ----------\n{original_body}")`
- [ ] **Auto-save draft**: every 60 s write body to `C:\Users\{name}\AppData\Mail\{account}\Drafts\{timestamp}.eml`

---

## 6. Account Management + Credential Store `[Sonnet]`

> → XREF: `07-networking/TODO-09-email-client.md §6` -- base spec.
> This section specifies the CNG AES-256-GCM credential encryption and Registry layout.

**Source file:** `src/apps/mail/mail_account.c`

- [ ] `acct_add(name, email, smtp_host, smtp_port, pop3_host, pop3_port, user, pass)`:
  - [ ] Registry key: `HKCU\Software\Impossible\Mail\Accounts\{name}\` with values: `Email`, `SmtpHost`, `SmtpPort`, `Pop3Host`, `Pop3Port`, `User`, `EncPass`, `EncPassNonce`, `EncPassTag`
  - [ ] Password encryption: `cng_keystore_get_kek(kek)` → `csprng_read(nonce, 12)` → `cng_aes256gcm_encrypt(kek, nonce, pass, strlen(pass), enc_out, tag)` → store hex-encoded `EncPass`/`EncPassNonce`/`EncPassTag`
- [ ] `acct_load(name, &acct)`: read Registry → `cng_key_store_get` or direct AES-GCM decrypt with stored nonce/tag → fill `mail_account_t` struct with plaintext password in RAM (zeroed after use)
- [ ] `acct_list(names[], max)`: enumerate `HKCU\Software\Impossible\Mail\Accounts\` subkeys
- [ ] `acct_delete(name)`: `reg_delete_key(HKCU\Software\Impossible\Mail\Accounts\{name})`; remove local mail folders
- [ ] **Default account**: `HKCU\Software\Impossible\Mail\DefaultAccount` = `{name}`
- [ ] **Account Settings UI**: Settings → Mail → add/edit/remove account dialog (IxUI); test button calls `smtp_connect` + `EHLO` to verify credentials before saving

---

## 7. Auto-Check + Notifications + Tray `[Sonnet]`

> → XREF: `07-networking/TODO-09-email-client.md §7` -- base spec.

**Source file:** `src/apps/mail/mail_notify.c`; `mail_systray.c`

- [ ] **Auto-check**: `sched_task_add("mail_check", mail_check_cb, 300, 1)` (300 s = 5 min) at app startup; `mail_check_cb()` calls `pop3_connect()` → `pop3_fetch_new()` → returns new-message count
- [ ] **Toast notification**: on `new_count > 0`: `notify_send("New mail", "New email from {From}: {Subject}", ICON_MAIL, 5000)` via `SYS_NOTIFY_SEND=54`; show at most one toast per check cycle even if multiple new messages (bundle: `"3 new messages from {From1}, {From2}…"`)
- [ ] **Taskbar badge** (unread count): `SYS_TASKBAR_SET_PROGRESS` (id 52) -- repurpose or add new syscall `SYS_TASKBAR_SET_BADGE=60`; badge displays unread count as red circle overlay on taskbar icon; cleared when user opens Inbox
- [ ] **System tray icon**: register `tray_icon_t { .icon_id=ICON_MAIL_ENVELOPE, .tooltip="Mail -- {n} unread", .click_cb=mail_show_window }` via `tray_register()` at startup; `tray_unregister()` on exit; update tooltip on each check; animated envelope on new mail for 3 s
- [ ] **Sync on open**: when `mail.exe` window is focused, trigger immediate `mail_check_cb()` outside the 5-min schedule

---

## 8. Contacts Store `[Sonnet]`

> → XREF: `07-networking/TODO-09-email-client.md §10` -- base spec.

**Source file:** `src/apps/mail/contacts.c`; header `include/apps/mail/contacts.h`

- [ ] **Storage**: `C:\Users\{name}\AppData\Mail\contacts.csv` -- one row per contact, fields: `Name,Email,Phone` (UTF-8, comma-separated, first row = header)
- [ ] `contact_load_all(contacts[], max)` → `vfs_open` + parse CSV line-by-line → fill `struct contact { name[128], email[256], phone[64] }[256]`
- [ ] `contact_lookup(partial_email, results[], max)` → scan loaded contacts, return entries where `strstr(c->email, partial_email)` (case-insensitive) or `strstr(c->name, partial_email)`
- [ ] `contact_add(name, email, phone)` → append line to CSV via `vfs_open(APPEND)`
- [ ] `contact_delete(email)` → rewrite CSV without matching line (read-modify-write)
- [ ] **To-field autocomplete**: on each keypress in To `CTRL_TEXTBOX` → `contact_lookup(typed, results, 5)` → show dropdown popup with up to 5 matches; arrow keys + Enter to select → fill textbox with `Name <email>`
- [ ] **"Add to Contacts"**: message viewer right-click → `Add {From} to contacts` → `contact_add(display_name, from_email, "")`
- [ ] **Contacts viewer**: Settings → Mail → Contacts → list all contacts; add/edit/delete buttons

---

## 9. Spam & Junk Filter `[Sonnet]`

**Source file:** `src/apps/mail/spam.c`; header `include/apps/mail/spam.h`

- [ ] **Trigger-word list**: default list in `src/apps/mail/spam_words.h` (`"free money"`, `"click here"`, `"unsubscribe"`, `"guaranteed"`, `"limited offer"`, etc.); configurable via Registry `HKCU\Software\Impossible\Mail\Spam\TriggerWords` (newline-separated)
- [ ] **Sender whitelist**: `HKCU\Software\Impossible\Mail\Spam\Whitelist` (newline-separated email addresses); whitelisted senders are never filtered
- [ ] **Sender blacklist**: `HKCU\Software\Impossible\Mail\Spam\Blacklist`; blacklisted senders always go to Junk
- [ ] `spam_check(msg)` → returns `SPAM_CLEAN`, `SPAM_SUSPECT`, or `SPAM_JUNK`:
  - [ ] If From is in blacklist → `SPAM_JUNK`
  - [ ] If From is in whitelist or contacts → `SPAM_CLEAN`
  - [ ] Count trigger-word hits in Subject + body (case-insensitive); score = hit_count / (subject_len + body_len) × 1000; score > 50 → `SPAM_SUSPECT`; score > 150 → `SPAM_JUNK`
  - [ ] Unknown sender (not in contacts, not white/blacklisted) + score > 25 → `SPAM_SUSPECT`
- [ ] On `SPAM_JUNK`: move `.eml` to Junk folder; do not issue toast notification
- [ ] On `SPAM_SUSPECT`: move to Junk with banner `"This message may be spam"` in viewer
- [ ] **`[Mark as Spam]`** button / context-menu: `contact_delete(from_email)` if present; add From to blacklist; move to Junk; decrement score threshold by 10 (learning)
- [ ] **`[Not Spam]`** button: add From to whitelist; move to Inbox; remove from blacklist if present; increment score threshold by 10 (learning)
- [ ] Adjusted thresholds persisted: `HKCU\Software\Impossible\Mail\Spam\SuspectThreshold` + `JunkThreshold` (int, default 50 / 150)

---

## OS Comparison


| ⭐  | Feature                                        | 🪟 Win11                              | 🐧 Linux                        | 🚀 Impossible OS                                               |
| --- | ---------------------------------------------- | ------------------------------------- | ------------------------------- | -------------------------------------------------------------- |
| 💎  | SMTP + STARTTLS                                | ✅ Outlook / New Outlook              | ✅ Thunderbird / Evolution      | ⬜ §1 -- EHLO + STARTTLS + AUTH                                |
| 💎  | POP3 over TLS                                  | ✅ Outlook                            | ✅ Thunderbird                  | ⬜ §2 -- USER/PASS/STAT/LIST/RETR/DELE                         |
| 💎  | IMAP + IDLE                                    | ✅ Outlook (push via IDLE)            | ✅ Thunderbird / Mutt           | ⬜ §3 -- (Stretch) -- ; IDLE push                              |
| 💎  | Three-panel GUI                                | ✅ Outlook / New Outlook              | ✅ Thunderbird                  | ⬜ §4 -- IxUI CTRL_LISTVIEW, attachment icons                  |
| 💎  | Compose with reply/forward/attach              | ✅ Outlook                            | ✅ Thunderbird                  | ⬜ §5 -- MIME multipart attach, auto-save draft                |
| ⭐  | Account passwords encrypted in Registry        | ✅ Windows Credential Manager (DPAPI) | ⚠️ Seahorse / plaintext configs | ⬜ §6 -- `cng_aes256gcm_encrypt` + login-derived KEK           |
| ⭐  | 5-min auto-check + toast + tray badge          | ✅ Outlook background service         | ✅ Thunderbird background agent | ⬜ §7 -- `sched_task_add(300)`, `notify_send`, `tray_register` |
| 💎  | Contacts book with To-field autocomplete       | ✅ Outlook + People app               | ✅ Thunderbird address book     | ⬜ §8 -- CSV store, `contact_lookup()`, dropdown               |
| ⭐  | Built-in spam filter with trainable thresholds | ✅ Outlook (server-side, Junk filter) | ⚠️ SpamAssassin plugin needed   | ⬜ §9 -- keyword score + whitelist/blacklist, `[Mark           |

Impossible OS encrypts credentials natively in the Registry using the same CNG key store as the
rest of the OS (no separate credential manager needed), and ships a built-in trainable spam filter
with zero plugin dependencies -- parity with Windows Credential Manager + Outlook Junk Filter
from day one.

---

## Verification

Run `bash scripts/build.sh run` for each verification step.

- [ ] **SMTP:** send email to `user@localhost` (QEMU SMTP listener); serial log shows `EHLO`, `STARTTLS upgrade`, `AUTH LOGIN 235`, `DATA 354`, `250 OK`; message appears in server mailbox
- [ ] **POP3:** connect to QEMU POP3 stub; `STAT` returns count; `RETR 1` downloads message; `.eml` file created at correct VFS path
- [ ] **Credential store:** `acct_add(...)` writes `EncPass`/`EncPassNonce`/`EncPassTag` to Registry; `acct_load()` decrypts and returns correct plaintext password; plaintext is zeroed after use
- [ ] **GUI:** `mail.exe` opens; sidebar shows Inbox/Sent/Drafts/Trash/Junk; click Inbox → message list populates from VFS `.eml` files; click message → viewer shows From/Subject/body
- [ ] **Compose:** `[✏ Compose]` → compose window; fill To/Subject/Body; `[Send]` → SMTP send; message appears in Sent folder; sent `.eml` created at VFS path
- [ ] **Reply/Forward:** reply pre-fills From + `Re:` subject + quoted body; forward pre-fills `Fwd:` subject + forwarded body block
- [ ] **Auto-check:** `sched_task_add("mail_check", ..., 300, 1)` registered; simulate time advance → `mail_check_cb()` fires → new message → toast appears with `notify_send`; tray icon tooltip updated
- [ ] **Contacts:** add contact via compose "Add to Contacts"; type partial email in To field → dropdown shows match; select → fills `Name <email>`
- [ ] **Spam filter:** inject `.eml` with trigger words → moved to Junk; click `[Not Spam]` → moved to Inbox; sender added to whitelist; same sender on next check → `SPAM_CLEAN`
- [ ] **Blacklist:** add sender to blacklist → incoming message immediately → Junk; `[Mark as Spam]` on unknown sender → adds to blacklist
- [ ] Commit: `"apps: email client -- SMTP/POP3, GUI, contacts, credential store, spam filter"`
