# 38 — Screensaver & Lock Screen

## Overview

Screensaver activates after idle timeout. Lock screen requires password to re-enter desktop.

---

## Screensaver

### Built-in screensavers
| Name | Effect |
|------|--------|
| **Blank** | Black screen (power saving) |
| **Starfield** | Moving stars toward viewer |
| **Matrix** | Falling green characters |
| **Bouncing logo** | OS logo bouncing off edges |
| **Clock** | Large floating digital clock |

### Screensaver API (`.scr` files — like Windows)
```c
/* screensaver.h */
typedef int (*scr_entry_fn)(uint32_t msg, gfx_surface_t *surface);
#define SCR_INIT    0   /* initialize */
#define SCR_FRAME   1   /* render one frame */
#define SCR_CLOSE   2   /* cleanup */
```

### Triggers
- Idle timeout (configurable: 1, 2, 5, 10, 15, 30 min, never)
- Dismissed by any mouse move or keypress
- Option: "Require password on wake" → shows lock screen

## Lock Screen
```
┌──────────────────────────────────────┐
│         [Wallpaper, blurred]         │
│                                      │
│            🕐 2:30 PM                │
│          Friday, March 7             │
│                                      │
│           👤 Derick                   │
│        ┌───────────────┐             │
│        │ ••••••••      │             │
│        └───────────────┘             │
│           [Unlock →]                 │
└──────────────────────────────────────┘
```
Uses `gfx_blur()` on wallpaper background + clock display.

## Codex: `System\Screensaver\IdleTimeout`, `System\Screensaver\Type`, `System\Screensaver\RequirePassword`
## Files: `src/desktop/screensaver.c` (~300 lines), `src/desktop/lockscreen.c` (~200 lines)
## Implementation: 1 week
