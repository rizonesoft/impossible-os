# 12 — Start Menu

## Overview

App launcher, pinned apps, recent files, search, and power buttons. Launches from the Start button on the taskbar.

---

## Layout (Windows 11 style — centered)

```
┌─────────────────────────────────────┐
│  🔍 Search apps and files           │
├─────────────────────────────────────┤
│  Pinned                             │
│  ┌────┐ ┌────┐ ┌────┐ ┌────┐       │
│  │Edge│ │Docs│ │Calc│ │Term│       │
│  └────┘ └────┘ └────┘ └────┘       │
│  ┌────┐ ┌────┐ ┌────┐ ┌────┐       │
│  │Paint│ │Set │ │Files│ │    │       │
│  └────┘ └────┘ └────┘ └────┘       │
├─────────────────────────────────────┤
│  Recommended                        │
│  📄 report.txt — Yesterday          │
│  🖼 photo.jpg — 2 days ago          │
│  📄 notes.md — This week            │
├─────────────────────────────────────┤
│  👤 User     ⏻ Power               │
└─────────────────────────────────────┘
```

## Data
- **Pinned apps**: stored in Codex `User\{name}\Shell\PinnedApps`
- **Recent files**: stored in Codex `User\{name}\Shell\RecentFiles`
- **All apps**: scanned from `C:\Impossible\Bin\` and `C:\Programs\`
- **Search**: filter app names + file names by typed query

## Power submenu
- Shut down → calls `power_shutdown()`
- Restart → calls `power_restart()`
- Sleep → calls `power_sleep()` (future)
- Lock → shows lock screen (future)

## Animation
- Slide-up from taskbar with `GFX_EASE_OUT_CUBIC` (200ms)
- Acrylic blur background via `gfx_acrylic()`

## Files: `src/desktop/start_menu.c` (~400 lines)
## Implementation: 1 week
