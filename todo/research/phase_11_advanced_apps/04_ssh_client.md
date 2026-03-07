# 73 — SSH Client

## Overview
Secure shell client — connect to remote servers via encrypted terminal session.

## Protocol: SSH2 (port 22, over TCP)
## Library options
| Library | License | Size | Notes |
|---------|---------|------|-------|
| **libssh2** | BSD-3 | ~30K lines | Widely used, C |
| **dropbear** | MIT | ~15K lines | Lightweight, designed for embedded |
| **Custom minimal** | — | ~2K lines | SSH2 transport + auth only |

## Features (minimal)
- Password authentication
- Public key authentication (future)
- Interactive terminal session
- SCP file transfer (future)

## Depends on: TCP (`22`), TLS/crypto (`31`, `63`)
## UI: runs inside Terminal app (`18_terminal.md`)
## Shell command: `ssh user@host`

## Files: `src/apps/ssh/ssh.c` (~500 lines if custom, or port dropbear) | 2-4 weeks
## Priority: 🟡 Medium-term
