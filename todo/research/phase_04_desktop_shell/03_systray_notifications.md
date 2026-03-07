# 14 — System Tray & Notifications

## Overview

The right side of the taskbar shows status icons (network, volume, battery, clock) and popup notification toasts.

---

## System tray area

```
                        ┌─────────────────────────┐
... taskbar ...         │ 🔊  🌐  🔋  │  2:30 PM  │
                        │             │  3/7/2026  │
                        └─────────────────────────┘
                         tray icons      clock
```

## Tray icons
| Icon | Source | Click action |
|------|--------|-------------|
| 🔊 Volume | Audio system | Volume slider popup |
| 🌐 Network | Network stack | Network status popup |
| 🔋 Battery | ACPI (laptops) | Battery details popup |
| 🔔 Notifications | Notification service | Open notification center |

## Notification toasts (Windows 11 style)

```
┌──────────────────────────────┐
│  📧 New Mail                 │
│  You have 3 new messages     │
│                    [Dismiss] │
└──────────────────────────────┘
   ↑ slides in from bottom-right, auto-dismiss after 5s
```

## API
```c
/* notify.h */
int  notify_send(const char *title, const char *msg, system_icon_t icon);
void notify_dismiss(int id);
```

## Codex: `System\Shell\TrayIcons`, `System\Shell\NotifyHistory`
## Files: `src/desktop/systray.c` (~300 lines), `src/desktop/notify.c` (~200 lines)
## Implementation: 1 week
