# 15 — Context Menus

## Overview

Right-click context menus on desktop, files, folders, taskbar, and within apps.

---

## Desktop context menu
```
┌──────────────────┐
│ View             ►│ → Large icons, Medium, Small, List
│ Sort by          ►│ → Name, Date, Size, Type
│ Refresh           │
│ ──────────────── │
│ New              ►│ → Folder, Text Document, Shortcut
│ ──────────────── │
│ Paste             │
│ ──────────────── │
│ Display settings  │
│ Personalize       │
└──────────────────┘
```

## File context menu
```
┌──────────────────┐
│ Open              │
│ Open with...     ►│
│ ──────────────── │
│ Cut               │
│ Copy              │
│ Paste             │
│ Delete            │
│ Rename            │
│ ──────────────── │
│ Properties        │
└──────────────────┘
```

## API
```c
/* context_menu.h */
struct menu_item {
    char         label[32];
    system_icon_t icon;
    void        (*callback)(void *ctx);
    struct menu_item *submenu;  /* NULL if no submenu */
    uint8_t      separator;     /* draw a line instead of item */
    uint8_t      disabled;
    uint8_t      checked;
};

int context_menu_show(int x, int y, struct menu_item *items, int count);
```

## Rendered with: `gfx_fill_rounded_rect` + `gfx_drop_shadow` + `gfx_acrylic`
## Files: `src/desktop/context_menu.c` (~300 lines)
## Implementation: 3-5 days
