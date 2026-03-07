# 76 — Calendar App

## Overview
Monthly/weekly/daily calendar view with events. Syncs with online calendars (CalDAV) in the future.

## Layout
```
┌──────────────────────────────────────┐
│  Calendar — March 2026          ─ □ ×│
├──────────────────────────────────────┤
│  ◀  March 2026  ▶                    │
│  Mon Tue Wed Thu Fri Sat Sun         │
│                   1   2   3          │
│   4   5   6  [7]  8   9  10         │
│  11  12  13  14  15  16  17         │
│  18  19  20  21  22  23  24         │
│  25  26  27  28  29  30  31         │
├──────────────────────────────────────┤
│  Events for March 7:                 │
│  🔵 10:00 AM — Team meeting          │
│  🔴  2:00 PM — Dentist appointment   │
│                         [+ Add Event]│
└──────────────────────────────────────┘
```

## Events stored in Codex: `User\{name}\Calendar\Events\{date}\*`
## Also opens when clicking taskbar clock
## Files: `src/apps/calendar/calendar.c` (~400 lines) | 3-5 days
