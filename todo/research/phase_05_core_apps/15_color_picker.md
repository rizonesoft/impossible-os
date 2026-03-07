# 70 — Color Picker Tool

## Overview
System-wide eyedropper to pick any color from the screen. Useful for Paint, theme customization, developers.

## Features
- Activate with shortcut (Win+Shift+C)
- Cursor changes to crosshair/eyedropper
- Shows a magnified circle around cursor with pixel grid
- Click to capture: copies hex color (#FF5733) to clipboard
- Shows RGB, HSL, HEX values in a popup
- Color history (last 10 picked colors)

## API
```c
uint32_t color_pick_from_screen(int x, int y); /* read pixel from compositor backbuffer */
```

## Files: `src/apps/colorpicker/colorpicker.c` (~150 lines) | 1-2 days
