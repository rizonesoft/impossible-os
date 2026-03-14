# P1103 — Email Client

> **Goal:** SMTP/POP3/IMAP email with a three-panel GUI.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 2. Email Client

### 2.1 Email Protocols

**Prompt:** SMTP client (send, port 587+STARTTLS): EHLO → AUTH LOGIN → MAIL FROM → RCPT TO → DATA → body → QUIT. POP3 client (receive, port 995+TLS): USER → PASS → STAT → LIST → RETR → DELE → QUIT. Both require TLS from Phase 07 §5. Stretch: IMAP client (port 993+TLS) for server-side message sync. After all items, create `docs/user/email.md`, mark `[x]`, run `bash scripts/build.sh clean`, commit `"apps: email protocol clients (SMTP/POP3)"`.


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

**Prompt:** Three-panel layout: sidebar (Inbox/Sent/Drafts/Trash folders), message list (from, subject, date, read/unread), and message viewer. Compose window: To, Subject, Body fields with Send button. Reply/Forward buttons prepopulate fields. Delete moves to Trash. Account setup stores credentials in Phase 09 credential store. Auto-check every 5 minutes, notification toast on new mail. After all items, update `docs/user/email.md`, mark `[x]`, run `bash scripts/build.sh clean`, commit `"apps: email client UI"`.


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

