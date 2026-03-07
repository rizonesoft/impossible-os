# 52 — Recycle Bin / Trash

## Overview

Deleted files go to `C:\Recycle\` instead of being destroyed. Users can restore or permanently delete.

---

## How it works

```
1. User deletes "report.txt" from C:\Users\Derick\Documents\
2. File is MOVED to: C:\Recycle\report.txt
3. Metadata saved: C:\Recycle\_meta\report.txt.meta
   → original path, deletion date, file size
4. Desktop Recycle Bin icon changes: empty → full
5. User can:
   a. Right-click → Restore → moves back to original path
   b. Right-click → Delete permanently → actually deletes
   c. "Empty Recycle Bin" → delete ALL
```

## Metadata file
```ini
; C:\Recycle\_meta\report.txt.meta
[Trash]
OriginalPath = "C:\Users\Derick\Documents\report.txt"
DeletedAt = 1741363200
Size = 4096
```

## API
```c
int  trash_delete(const char *path);        /* move to recycle bin */
int  trash_restore(const char *trash_name); /* restore to original path */
void trash_empty(void);                     /* permanently delete all */
int  trash_count(void);                     /* items in bin */
uint64_t trash_size(void);                  /* total bytes */
```

## Desktop icon: `ICON_TRASH_EMPTY` / `ICON_TRASH_FULL` from icon store
## Codex: `System\Recycle\MaxSize = 1073741824` (1 GB limit)

## Files: `src/kernel/trash.c` (~200 lines)
## Implementation: 2-3 days
