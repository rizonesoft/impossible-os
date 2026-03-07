# 94 — Snap Layouts

## Overview
Hover over a window's maximize button to see layout options. Click a zone to snap the window there. Like Windows 11 Snap Layouts.

## Popup (shown on maximize button hover)
```
      ┌───────────────────┐
      │ ┌───┬───┐ ┌──┬──┐ │
      │ │   │   │ │  │  │ │
      │ │   │   │ ├──┤  │ │
      │ │   │   │ │  │  │ │
      │ └───┴───┘ └──┴──┘ │
      │ ┌────┬──┐ ┌─┬─┬─┐ │
      │ │    │  │ │ │ │ │ │
      │ │    │  │ │ │ │ │ │
      │ │    │  │ │ │ │ │ │
      │ └────┴──┘ └─┴─┴─┘ │
      └───────────────────┘
```

## Layouts: 50/50, 50/50 (top/bottom), 66/33, 33/33/33, 25/50/25
## Selecting a zone: snaps current window, then prompts for remaining zones
## Extends: `16_window_snapping_vdesktops.md`

## Files: integrated into `src/kernel/wm_snap.c` (+100 lines) | 2-3 days
