# 34 — File Associations

## Overview

Double-click a `.txt` → opens Notepad. Double-click `.jpg` → opens Image Viewer. Maps file extensions to applications.

---

## Codex storage

```ini
; System\FileTypes
[.txt]
App = "C:\Impossible\Bin\notepad.exe"
Icon = "file_text"
Description = "Text Document"

[.jpg]
App = "C:\Impossible\Bin\imgview.exe"
Icon = "file_image"
Description = "JPEG Image"

[.png]
App = "C:\Impossible\Bin\imgview.exe"
Icon = "file_image"
Description = "PNG Image"

[.exe]
Icon = "file_exe"
Description = "Application"

[.c]
App = "C:\Impossible\Bin\notepad.exe"
Icon = "file_code"
Description = "C Source File"
```

## API
```c
/* file_assoc.h */
const char *file_assoc_get_app(const char *extension);     /* ".txt" → path to app */
const char *file_assoc_get_icon(const char *extension);    /* ".txt" → "file_text" */
void file_assoc_set(const char *ext, const char *app_path);
void file_assoc_open(const char *filepath);                /* open with associated app */
```

## "Open with..." dialog: shows all installed apps, lets user choose + set default
## Right-click → "Open with..." adds to context menu (see 15_context_menus.md)

## Files: `src/kernel/file_assoc.c` (~150 lines)
## Implementation: 2-3 days
