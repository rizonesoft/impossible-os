# 67 — Desktop Widgets

## Overview
Small floating panels on the desktop: clock, weather, CPU meter, memory usage, sticky notes, calendar. Like Windows 11 Widgets or Vista Gadgets.

## Widget API (`.wgt` files — like `.spl` applets)
```c
typedef int (*widget_fn)(uint32_t msg, gfx_surface_t *surface, void *ctx);
#define WGT_INIT     0
#define WGT_RENDER   1   /* draw one frame */
#define WGT_TICK     2   /* periodic update (every 1s) */
#define WGT_CLOSE    3
```

## Built-in widgets
| Widget | Size | Updates | Content |
|--------|------|---------|---------|
| Clock | 200×100 | 1/sec | Analog or digital clock |
| CPU Meter | 200×120 | 1/sec | Usage bar graph |
| RAM Monitor | 200×100 | 5/sec | Used/free memory |
| Calendar | 200×200 | Daily | Month view |
| Quick Notes | 200×150 | On edit | Sticky note text |

## Files: `src/desktop/widgets.c` (~300 lines) + per-widget files | 1-2 weeks
