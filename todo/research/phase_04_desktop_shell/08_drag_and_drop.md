# 37 — Drag and Drop

## Overview

Drag files between windows, drag text in editors, drag windows from taskbar. System-wide drag-and-drop protocol.

---

## Drag types
| Type | Source → Target | Example |
|------|----------------|---------|
| Files | File Manager → Desktop | Move/copy files |
| Files | File Manager → Notepad | Open file |
| Text | Notepad → Notepad | Move selected text |
| Images | File Manager → Paint | Open image |
| Taskbar | Taskbar → Desktop | Reorder pinned apps |

## Protocol
```c
/* drag.h */
typedef struct {
    uint8_t        active;
    clip_format_t  format;       /* TEXT, IMAGE, FILES */
    void          *data;
    uint32_t       data_size;
    int32_t        cursor_x, cursor_y;
    const icon_t  *drag_icon;    /* icon shown under cursor while dragging */
    wm_window_t   *source_window;
} drag_state_t;

void drag_begin(clip_format_t fmt, const void *data, uint32_t size, const icon_t *icon);
void drag_update(int32_t mx, int32_t my);  /* called on mouse move */
int  drag_drop(wm_window_t *target);       /* called on mouse release */
void drag_cancel(void);                     /* ESC to cancel */
```

## Visual: semi-transparent icon follows cursor during drag, drop target highlights on hover
## Files: `src/desktop/drag.c` (~200 lines)
## Implementation: 3-5 days
