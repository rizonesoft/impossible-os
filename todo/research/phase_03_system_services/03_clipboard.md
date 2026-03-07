# 03 — Clipboard

## Overview

System-wide clipboard for copy/paste between all apps. Supports text, images, and files.

---

## Design

```c
/* clipboard.h */
typedef enum {
    CLIP_TEXT = 0,     /* UTF-8 text string */
    CLIP_IMAGE,        /* BGRA pixel data (from Paint, screenshots) */
    CLIP_FILES,        /* List of file paths (from File Manager) */
} clip_format_t;

typedef struct {
    clip_format_t format;
    void         *data;
    uint32_t      size;
} clipboard_t;

int clipboard_set(clip_format_t fmt, const void *data, uint32_t size);
int clipboard_get(clip_format_t fmt, void *buf, uint32_t max_size);
int clipboard_has(clip_format_t fmt);  /* check if format available */
void clipboard_clear(void);
```

## Keyboard shortcuts (system-wide)
| Shortcut | Action |
|----------|--------|
| Ctrl+C | Copy selection to clipboard |
| Ctrl+X | Cut selection to clipboard |
| Ctrl+V | Paste from clipboard |

## Win32 mapping
| Win32 | Clipboard API |
|-------|--------------|
| `OpenClipboard()` | No-op (no locking needed for single-core) |
| `SetClipboardData(CF_TEXT, data)` | `clipboard_set(CLIP_TEXT, ...)` |
| `GetClipboardData(CF_TEXT)` | `clipboard_get(CLIP_TEXT, ...)` |
| `CloseClipboard()` | No-op |

## Files: `src/kernel/clipboard.c` (~100 lines), `include/clipboard.h` (~30 lines)
## Implementation: 1-2 days
