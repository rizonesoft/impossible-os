# 66 — Notification Center

## Overview
Full notification history panel — slide-in from the right edge. Shows past toasts, grouped by app, with dismiss/clear all.

## Layout
```
                    ┌──────────────────┐
                    │ Notifications    │
                    │ Today            │
                    │ ┌──────────────┐ │
                    │ │📧 Mail (2)   │ │
                    │ │ New message  │ │
                    │ │ Reply from.. │ │
                    │ └──────────────┘ │
                    │ ┌──────────────┐ │
                    │ │⚠ System      │ │
                    │ │ Update ready │ │
                    │ └──────────────┘ │
                    │   [Clear all]    │
                    └──────────────────┘
```
## Open: click 🔔 in system tray or swipe from right edge
## Codex: `System\Shell\NotifyHistory` (last 100 notifications)
## Files: `src/desktop/notify_center.c` (~250 lines) | 3-5 days
