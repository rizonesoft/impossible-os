# 93 — On-Screen Keyboard

## Overview
Virtual keyboard rendered on screen — for touch devices, accessibility, or when no physical keyboard is available.

## Layout
```
┌──────────────────────────────────────────────────────┐
│ ` 1 2 3 4 5 6 7 8 9 0 - = [Bksp]                   │
│ [Tab] Q W E R T Y U I O P [ ] \                     │
│ [Caps] A S D F G H J K L ; ' [Enter]                │
│ [Shift]  Z X C V B N M , . / [Shift]                │
│ [Ctrl] [Win] [Alt]  [  Space  ]  [Alt] [Ctrl] [←→]  │
└──────────────────────────────────────────────────────┘
```

## Features
- Click/tap keys → inject keypress into WM
- Shift/Caps Lock/Ctrl modifiers
- Auto-show when text input focused (touch mode)
- Auto-hide when physical keyboard detected
- Resize/move the keyboard window
- Supports current keyboard layout (from `35_keyboard_i18n.md`)

## Files: `src/apps/osk/osk.c` (~400 lines) | 3-5 days
