# 16 — Window Snapping & Virtual Desktops

## Overview

Window management enhancements: snap windows to halves/quarters, and multiple virtual desktops.

---

## Window Snapping (like Windows 11)

| Action | Result |
|--------|--------|
| Win+Left | Snap to left half |
| Win+Right | Snap to right half |
| Win+Up | Maximize |
| Win+Down | Restore / minimize |
| Drag to top edge | Maximize |
| Drag to left/right edge | Snap to half |
| Drag to corner | Snap to quarter |

### Snap layout popup (hover maximize button)
```
┌──┬──┐  ┌──┬──┐  ┌───┬─┐  ┌─┬─┬─┐
│  │  │  │  │  │  │   │ │  │ │ │ │
│  │  │  ├──┤  │  │   │ │  │ │ │ │
│  │  │  │  │  │  │   │ │  │ │ │ │
└──┴──┘  └──┴──┘  └───┴─┘  └─┴─┴─┘
 50/50    50/50    66/33    33/33/33
```

## Virtual Desktops

| Shortcut | Action |
|----------|--------|
| Win+Tab | Overview of all desktops |
| Ctrl+Win+Left/Right | Switch between desktops |
| Ctrl+Win+D | New desktop |
| Ctrl+Win+F4 | Close desktop |

### Data
```c
#define MAX_VIRTUAL_DESKTOPS 8
struct virtual_desktop {
    struct wm_window *windows;  /* linked list of windows on this desktop */
    char name[32];              /* "Desktop 1", "Desktop 2" */
};
```

## Files: `src/kernel/wm_snap.c` (~200 lines), `src/kernel/wm_vdesktop.c` (~200 lines)
## Implementation: 1-2 weeks
