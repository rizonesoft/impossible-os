# 97 — Parental Controls

## Overview
Restrict what children can do: app time limits, website blocking, content filtering. Settings accessible only by admin.

## Features
| Control | Description |
|---------|-------------|
| Screen time | Max hours per day (e.g., 2 hours) |
| App blocking | Block specific apps |
| Time scheduling | Only allow PC use 3 PM – 8 PM |
| Website filter | Block list of domains (after browser exists) |
| Activity report | Log apps used + duration |

## Codex: `User\{child}\ParentalControls\ScreenTimeLimit = 120` (minutes)
## Settings: `parental.spl` — admin-only applet
## Depends on: `33_user_accounts_login.md`, `40_security_permissions.md`
## Files: `src/kernel/parental.c` (~200 lines) | 3-5 days
## Priority: 🔴 Long-term
