# 53 — Shortcut Files (.lnk)

## Overview

Desktop and Start Menu shortcuts that point to apps or files. Like Windows `.lnk` files.

---

## Format: `.lnk` files (simple INI-based, not Windows binary format)

```ini
; C:\Users\Derick\Desktop\Notepad.lnk
[Shortcut]
Target = "C:\Impossible\Bin\notepad.exe"
Arguments = ""
Icon = "C:\Impossible\Icons\notepad.raw"
WorkingDir = "C:\Users\Derick\Documents\"
Description = "Open Notepad text editor"
```

## API
```c
/* shortcut.h */
struct shortcut {
    char target[256];
    char arguments[256];
    char icon_path[128];
    char working_dir[256];
    char description[128];
};

int shortcut_create(const char *lnk_path, const struct shortcut *s);
int shortcut_read(const char *lnk_path, struct shortcut *s);
int shortcut_execute(const char *lnk_path); /* resolve target + exec */
```

## Usage
- Desktop shows `.lnk` files with their custom icon
- Start Menu pinned apps are `.lnk` files in `C:\Users\{name}\AppData\StartMenu\`
- Double-click `.lnk` → executes target with arguments
- App installer creates shortcuts on Desktop + Start Menu

## Files: `src/kernel/shortcut.c` (~100 lines)
## Implementation: 1-2 days
