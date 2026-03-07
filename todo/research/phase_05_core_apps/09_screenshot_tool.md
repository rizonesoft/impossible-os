# 21 — Screenshot Tool

## Overview

Capture full screen, single window, or selected region. Save as PNG to `C:\Users\{name}\Pictures\Screenshots\`.

---

## Methods

| Shortcut | Action |
|----------|--------|
| PrtSc | Full screen → clipboard + auto-save |
| Alt+PrtSc | Active window only |
| Win+Shift+S | Region select (snipping tool) |

## Region select overlay
```
┌─────────────────────────────────────┐
│  ████████████████████████████████   │  ← dimmed overlay
│  ████┌──────────────┐████████████  │
│  ████│  Selected    │████████████  │  ← clear region
│  ████│  Region      │████████████  │
│  ████└──────────────┘████████████  │
│  ████████████████████████████████   │
└─────────────────────────────────────┘
```

## Implementation
```c
image_t *screenshot_capture(void);            /* full screen */
image_t *screenshot_window(wm_window_t *win); /* single window */
image_t *screenshot_region(int x, int y, int w, int h); /* region */
```

Reads pixels directly from the compositor backbuffer. Saves via `stb_image_write`.

## Files: `src/apps/screenshot/screenshot.c` (~200 lines)
## Implementation: 2-3 days
