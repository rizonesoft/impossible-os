# 49 — Accessibility

## Overview

Make Impossible OS usable for people with disabilities: high contrast mode, magnifier, large text, and keyboard navigation.

---

## Features

| Feature | Priority | Description |
|---------|----------|-------------|
| **High contrast mode** | P1 | Swap to high-contrast theme (black/white/yellow) |
| **Large text / DPI override** | P1 | Force 150-300% DPI scale |
| **Sticky keys** | P1 | Press Shift/Ctrl/Alt sequentially instead of together |
| **Mouse keys** | P2 | Move cursor with numpad |
| **Magnifier** | P2 | Zoom into area around cursor (2× - 8×) |
| **On-screen keyboard** | P2 | Virtual keyboard for touch/mouse |
| **Reduced motion** | P2 | Disable animations |
| **Cursor size** | P1 | Large cursor option (48px, 64px) |
| **Screen reader** | P3 | Text-to-speech (requires audio + TTS engine) |
| **Color blind mode** | P3 | Adjust colors for deuteranopia/protanopia |

## High contrast themes (Codex)
```ini
[Theme.HighContrast]
Background = "#000000"
Foreground = "#FFFFFF"
AccentColor = "#FFFF00"
LinkColor = "#00FFFF"
ErrorColor = "#FF0000"
```

## Keyboard shortcuts
| Shortcut | Action |
|----------|--------|
| 5× Shift | Toggle Sticky Keys |
| Win+Plus | Magnifier zoom in |
| Win+Minus | Magnifier zoom out |
| Win+Esc | Close Magnifier |

## Codex: `System\Accessibility\HighContrast`, `System\Accessibility\StickyKeys`, etc.
## Files: `src/desktop/accessibility.c` (~400 lines), `accessibility.spl` settings applet
## Implementation: 1-2 weeks (incremental)
