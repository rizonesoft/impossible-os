# 42 — Image Viewer

## Overview

Open and display images (JPEG, PNG, BMP). Zoom, pan, and navigate between images in a folder.

---

## Layout

```
┌──────────────────────────────────────────────────┐
│  Image Viewer — photo.jpg               ─ □ ×│
├──────────────────────────────────────────────────┤
│                                                  │
│                 ┌──────────────┐                 │
│                 │              │                 │
│                 │   [Image]    │                 │
│                 │              │                 │
│                 └──────────────┘                 │
│                                                  │
├──────────────────────────────────────────────────┤
│  ◀ Previous  │  100%  │  🔍+ 🔍-  │  Next ▶    │
│  1 of 12     │ 1920×1080  │  2.4 MB              │
└──────────────────────────────────────────────────┘
```

## Features
- Open from File Manager double-click or right-click → Open
- Zoom: mouse wheel, fit-to-window, actual size
- Pan: click and drag when zoomed in
- Navigate: ← → arrows for prev/next image in folder
- Slideshow mode: auto-advance every 3-5 seconds
- Set as wallpaper: right-click option
- Uses `image_load()` from `06_runtime_image_decoding.md`

## Files: `src/apps/imgview/imgview.c` (~400 lines)
## Implementation: 3-5 days
