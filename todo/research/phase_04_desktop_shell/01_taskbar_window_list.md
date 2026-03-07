# 36 — Taskbar Window List

## Overview

Show running apps as buttons/thumbnails on the taskbar. Click to switch, right-click for window controls.

---

## Layout

```
┌──────────────────────────────────────────────────────────────┐
│ ⊞ │ ▪ Notepad │ ▪ Terminal │ ▪ Paint │        │ 🔊 🌐 2:30 PM │
└──────────────────────────────────────────────────────────────┘
      ← window buttons, active one highlighted →
```

## Hover thumbnail preview (Windows 11)
```
         ┌──────────────┐
         │  Notepad     │
         │ ┌──────────┐ │
         │ │ preview  │ │
         │ │ of window│ │
         │ └──────────┘ │
         └──────────────┘
         ▪ Notepad ← taskbar button
```

## API (WM integration)
```c
/* Each WM window registers on the taskbar */
struct taskbar_entry {
    wm_window_t  *window;
    char          title[64];
    const icon_t *icon;
    uint8_t       active;     /* currently focused */
    uint8_t       flashing;   /* app requesting attention */
};

void taskbar_add_window(wm_window_t *win);
void taskbar_remove_window(wm_window_t *win);
void taskbar_set_active(wm_window_t *win);
void taskbar_flash(wm_window_t *win);  /* blink to get attention */
```

## Click: focus/raise window. Click active: minimize. Right-click: Close/maximize/minimize menu.
## Files: `src/desktop/taskbar_winlist.c` (~200 lines)
## Implementation: 3-5 days
