# 68 — Window Animations

## Overview
Smooth transitions for window open, close, minimize, maximize, snap. Makes the desktop feel polished and alive.

## Animations
| Action | Animation | Duration |
|--------|-----------|----------|
| Window open | Scale 90%→100% + fade in | 200ms |
| Window close | Scale 100%→90% + fade out | 150ms |
| Minimize | Shrink toward taskbar button | 250ms |
| Restore | Expand from taskbar button | 250ms |
| Maximize | Expand to fill screen | 200ms |
| Snap left/right | Slide + resize to half | 200ms |
| Focus switch | Subtle scale pulse | 100ms |
| Menu popup | Scale Y 0→100% from top | 150ms |

## Easing: `GFX_EASE_OUT_CUBIC` from `04_2d_compositor.md`
## Option: "Reduce motion" disables all (see `49_accessibility.md`)
## Codex: `System\Theme\EnableAnimations = 1`, `System\Theme\AnimationSpeed = 100`

## Files: `src/kernel/wm_anim.c` (~200 lines) | 3-5 days
