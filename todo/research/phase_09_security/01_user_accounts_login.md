# 33 — User Accounts & Login Screen

## Overview

Multiple user accounts, password authentication, and a login screen at boot.

---

## Login screen

```
┌──────────────────────────────────────┐
│                                      │
│         Impossible OS                │
│                                      │
│          👤 Derick                    │
│       ┌───────────────┐              │
│       │ ••••••••      │              │
│       └───────────────┘              │
│          [Sign in →]                 │
│                                      │
│   ⏻  🌐  ♿                          │
└──────────────────────────────────────┘
```

## User data
```
C:\Users\
├── Derick\
│   ├── Desktop\
│   ├── Documents\
│   └── AppData\
└── Guest\
    ├── Desktop\
    └── Documents\
```

## Codex: `User\{name}\PasswordHash`, `User\{name}\Avatar`, `User\{name}\AutoLogin`
## Password hashing: Argon2 or bcrypt (via monocypher)
## Privilege levels: Admin (full access), Standard (own files only), Guest (read-only)

## Files: `src/kernel/auth.c` (~300 lines), `src/desktop/login.c` (~300 lines)
## Implementation: 2-3 weeks
