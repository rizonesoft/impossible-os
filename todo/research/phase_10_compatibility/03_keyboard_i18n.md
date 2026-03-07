# 35 — Keyboard Layouts & Internationalization

## Overview

Support multiple keyboard layouts (QWERTY, AZERTY, DVORAK, etc.), Unicode/UTF-8 text, and localized UI strings.

---

## Keyboard Layouts

Current state: US QWERTY scancode → ASCII mapping hardcoded in `keyboard.c`.

### Layout data structure
```c
struct kbd_layout {
    char     name[32];           /* "US English", "French (AZERTY)" */
    char     code[8];            /* "en-US", "fr-FR" */
    uint8_t  normal[128];        /* scancode → character (no modifier) */
    uint8_t  shift[128];         /* scancode → character (Shift held) */
    uint8_t  altgr[128];         /* scancode → character (AltGr held) */
};
```

### Supported layouts (initial)
| Layout | Code | Region |
|--------|------|--------|
| US English (QWERTY) | `en-US` | Default |
| UK English | `en-GB` | |
| German (QWERTZ) | `de-DE` | |
| French (AZERTY) | `fr-FR` | |
| Spanish | `es-ES` | |
| Dvorak | `en-DV` | Alternative |

### Switching: Win+Space cycles layouts (indicator in system tray: `EN`, `FR`, `DE`)

## Unicode / UTF-8

Current state: ASCII only (0-127). Need UTF-8 for international text.

- Strings stored as UTF-8 internally
- Font rendering uses `stb_truetype` which supports Unicode codepoints
- Noto Sans as fallback font (covers all scripts)

## Localization (future)

UI strings loaded from `C:\Impossible\System\Locale\{code}.ini`:
```ini
; en-US.ini
StartMenu = "Start"
Settings = "Settings"
Shutdown = "Shut down"
Restart = "Restart"

; fr-FR.ini
StartMenu = "Démarrer"
Settings = "Paramètres"
Shutdown = "Arrêter"
Restart = "Redémarrer"
```

## Codex: `System\Input\KeyboardLayout = "en-US"`
## Files: `src/kernel/kbd_layout.c` (~200 lines), `resources/layouts/*.h`
## Implementation: 1-2 weeks
