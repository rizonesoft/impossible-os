# 75 — Email Client

## Overview
Basic email client: send/receive via SMTP/POP3 or IMAP. Compose, inbox, sent.

## Protocols
| Protocol | Port | Purpose |
|----------|------|---------|
| SMTP | 587 (TLS) | Send email |
| POP3 | 995 (TLS) | Receive (download + delete) |
| IMAP | 993 (TLS) | Receive (sync, keep on server) |

## Layout
```
┌────────────────────────────────────────────┐
│  Mail                              ─ □ ×  │
├────────┬───────────────────────────────────┤
│ Inbox  │ From: alice@example.com          │
│ Sent   │ Subject: Hello!                  │
│ Drafts │ Date: March 7, 2026             │
│ Trash  │ ──────────────────────           │
│        │ Hi, this is a test email.        │
│        │                                   │
│ [✏ New]│                     [Reply] [Fwd]│
└────────┴───────────────────────────────────┘
```

## Depends on: TCP (`22`), TLS (`22` Phase 5), DNS (`22` Phase 2)
## Files: `src/apps/mail/mail.c` (~800 lines) | 2-4 weeks
## Priority: 🔴 Long-term
