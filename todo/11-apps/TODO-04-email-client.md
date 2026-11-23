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
> Implement §1–§3 in coordination with TODO-09 §2–§4; do not re-implement what TODO-09 already
> specifies -- use XREFs to stay aligned.
>
> Mbed TLS (`tls_connect`, `tls_send`, `tls_recv`) must be complete before STARTTLS/TLS work
> begins -- `→ XREF: 07-networking/TODO-03`.
> CNG AES-256-GCM and key store must be complete before §6 credential storage --
> `→ XREF: 09-desktop-shell/TODO-07 §1 + §4`.

---

## Inputs

- `07-networking/TODO-09-email-client.md` -- canonical protocol implementation reference (§1 parser, §2 SMTP, §3 POP3, §4 IMAP, §5 accounts, §7 GUI, §8 compose, §9 contacts, §10 search)
- `include/kernel/net/tls.h` -- `tls_connect(fd, hostname)`, `tls_send()`, `tls_recv()`, `tls_close()` (→ XREF `07-networking/TODO-03`)
- `include/kernel/net/dns.h` -- `dns_resolve(hostname, &ip)`
- `include/kernel/cng/cng.h` -- `cng_aes256gcm_encrypt()`, `cng_aes256gcm_decrypt()` (→ XREF `09-desktop-shell/TODO-07 §1`)
- `include/kernel/cng/cng_keystore.h` -- `cng_keystore_get_kek()`, `cng_key_store_import()`, `cng_key_store_get()` (→ XREF `09-desktop-shell/TODO-07 §4`)
- `include/desktop/controls.h` -- `ctrl_create_button`, `ctrl_create_textbox`, `ctrl_create_listview`, `ctrl_create_scrollbar`
- `include/desktop/wm.h` -- `wm_create_window()`
- `include/desktop/systray.h` (→ XREF `08-graphics-ui/TODO-11-startmenu-tray-notifications.md §4`) -- `tray_register()`, `tray_unregister()`
- `include/kernel/scheduler_tasks.h` (→ XREF `09-desktop-shell/TODO-04 §6`) -- `sched_task_add(name, cb, interval_s, enabled)`
- `include/registry.h` -- `reg_set_string`, `reg_get_string`, `reg_create_key`, `reg_delete_key`
- `include/kernel/vfs.h` -- `vfs_open`, `vfs_read`, `vfs_write`, `vfs_create`, `vfs_mkdir`, `vfs_unlink`
- `notify_send(title, body, icon_id, timeout_ms)` / `SYS_NOTIFY_SEND=54` (→ XREF `08-graphics-ui/TODO-11 §5`)

---

## Outcome

`mail.exe` opens a three-panel window (account sidebar / message list / viewer). Users can send email via SMTP with STARTTLS, receive via POP3 (or IMAP stretch), compose with To-autocomplete, reply, forward, and delete to Trash. Account passwords are encrypted at rest via AES-256-GCM with the user's login KEK. The client polls every 5 minutes and fires a toast notification + tray badge on new mail. A keyword-based spam filter moves junk to the Junk folder, trainable via `[Mark as Spam]` / `[Not Spam]`.

---

## Implementation Order

| Step | Section                               | 💎/⭐ | Dependency                                                                             | Status |
| ---- | ------------------------------------- | ----- | -------------------------------------------------------------------------------------- | :----: |
| 1    | SMTP Client                           | 💎    | §10                                                                                    |  [ ]   |
| 2    | POP3 Client + Message Storage         | 💎    | §10, §1                                                                                |  [ ]   |
| 3    | IMAP Client (Stretch)                 | 💎    | §10, §1                                                                                |  [ ]   |
| 4    | Three-Panel Email GUI                 | 💎    | §10, §2                                                                                |  [ ]   |
| 5    | Compose Window                        | 💎    | §10, §4, §8                                                                            |  [ ]   |
| 6    | Account Management + Credential Store | ⭐    | §10                                                                                    |  [ ]   |
| 7    | Auto-Check + Notifications + Tray     | ⭐    | §10, §2                                                                                |  [ ]   |
| 8    | Contacts Store                        | 💎    | §10                                                                                    |  [ ]   |
| 9    | Spam & Junk Filter                    | ⭐    | §10, §4                                                                                |  [ ]   |
| 10   | Import Mail                           | 💎    | `10-platform-services/TODO-07` §7 (PE programs run), `10-platform-services/TODO-13` §5 |  [ ]   |

---

## 1. SMTP Client

**Moved:** mail

Built in Mail, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §10 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §10 brings it
- [ ] Commit: `"imports: mail brings SMTP Client"`

---

## 2. POP3 Client + Message Storage

**Moved:** mail

Built in Mail, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §10 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §10 brings it
- [ ] Commit: `"imports: mail brings POP3 Client + Message Storage"`

---

## 3. IMAP Client (Stretch)

**Moved:** mail

Built in Mail, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §10 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §10 brings it
- [ ] Commit: `"imports: mail brings IMAP Client (Stretch)"`

---

## 4. Three-Panel Email GUI

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

**Moved:** mail

Built in Mail, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §10 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §10 brings it
- [ ] Commit: `"imports: mail brings Three-Panel Email GUI"`

---

## 5. Compose Window

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

**Moved:** mail

Built in Mail, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §10 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §10 brings it
- [ ] Commit: `"imports: mail brings Compose Window"`

---

## 6. Account Management + Credential Store

**Moved:** mail

Built in Mail, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §10 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §10 brings it
- [ ] Commit: `"imports: mail brings Account Management + Credential Store"`

---

## 7. Auto-Check + Notifications + Tray

**Design:** [`shell.md#taskbar`](../../docs/design/shell.md#taskbar), [`shell.md#toast-notifications`](../../docs/design/shell.md#toast-notifications)

**Moved:** mail

Built in Mail, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §10 has brought it and it is shown to work.

What is this OS's own work stays here:

- [ ] **Toast notification**: on `new_count > 0`: `notify_send("New mail", "New email from {From}: {Subject}", ICON_MAIL, 5000)` via `SYS_NOTIFY_SEND=54`; show at most one toast per check cycle even if multiple new messages (bundle: `"3 new messages from {From1}, {From2}…"`)
- [ ] **Taskbar badge** (unread count): call the taskbar badge API owned by `08-graphics-ui/TODO-10` per `docs/design/shell.md#taskbar` (accent pill, 16 px tall, "99+" above 99)
  - Add the badge there once the spec defines it, and do not add a `SYS_TASKBAR_SET_BADGE` syscall here
- [ ] **System tray icon**: register `tray_icon_t { .icon_id=ICON_MAIL_ENVELOPE, .tooltip="Mail -- {n} unread", .click_cb=mail_show_window }` via `tray_register()` at startup; `tray_unregister()` on exit
  - Update tooltip on each check; the icon lives in the overflow chevron flyout (`08-graphics-ui/TODO-11 §4`); new mail is announced by the toast, not an animated icon
- [ ] Shown to work here on the imported copy, as §10 brings it
- [ ] Commit: `"imports: mail brings Auto-Check + Notifications + Tray"`

---

## 8. Contacts Store

**Moved:** mail

Built in Mail, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §10 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §10 brings it
- [ ] Commit: `"imports: mail brings Contacts Store"`

---

## 9. Spam & Junk Filter

**Moved:** mail

Built in Mail, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §10 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §10 brings it
- [ ] Commit: `"imports: mail brings Spam & Junk Filter"`

## 10. Import Mail

> **Spawned-by:** root

**Design:** n/a -- imports a program, it designs nothing

**Import:** mail

Mail is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull mail` into `src/apps/mail/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open Mail on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose mail`
- [ ] Commit: `"apps: Mail imported"`

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

Impossible OS encrypts credentials natively in the Registry using the same CNG key store as the rest of the OS (no separate credential manager needed), and ships a built-in trainable spam filter with zero plugin dependencies -- parity with Windows Credential Manager + Outlook Junk Filter from day one.

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
