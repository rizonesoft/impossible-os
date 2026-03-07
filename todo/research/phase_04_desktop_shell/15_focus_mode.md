# 88 — Focus / Do Not Disturb

## Overview
Suppress notifications and visual distractions during focused work. Like Windows Focus Assist.

## Modes
| Mode | Behavior |
|------|----------|
| **Off** | All notifications shown normally |
| **Priority only** | Only essential notifications (alarms, reminders) |
| **Do Not Disturb** | All notifications silenced, saved to history |

## Features
- Toggle via Quick Settings or Win+N
- Auto-activate during: full-screen apps, presentations, scheduled hours
- Notification badge on system tray shows count of suppressed notifications
- When DND ends, show summary: "You missed 5 notifications"

## Codex: `System\Shell\FocusMode = "off"`, `...\FocusScheduleStart = "09:00"`, `...\FocusScheduleEnd = "17:00"`
## Files: `src/desktop/focus_mode.c` (~100 lines) | 1-2 days
