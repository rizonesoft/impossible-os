# 95 — Window Peek / Aero Peek

## Overview
Hover over a taskbar button → all other windows become transparent, revealing only that window. Like Windows Aero Peek.

## How it works
1. User hovers taskbar button for 500ms
2. All windows except the hovered one → set opacity to 10% (glass effect)
3. Mouse leaves taskbar → restore all window opacity to 100%
4. Uses `gfx_set_opacity()` from compositor

## Also: "Show Desktop" peek — hover the far-right corner of taskbar → all windows become transparent, showing desktop

## Codex: `System\Shell\EnablePeek = 1`
## Files: integrated into `src/desktop/taskbar_winlist.c` (+50 lines) | 1-2 days
