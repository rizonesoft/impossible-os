# 13 — File Manager

## Overview

Explorer-equivalent for browsing `C:\`, creating/copying/moving/deleting files and folders, with icon view and detail view.

---

## Layout

```
┌──────────────────────────────────────────────────────┐
│  File Manager                                   ─ □ ×│
├──────────────────────────────────────────────────────┤
│  ← → ↑  │ C:\Users\Default\Documents               │
├──────────┬───────────────────────────────────────────┤
│ Quick    │  📁 Projects    📁 Photos                 │
│ Access   │  📁 Downloads   📄 notes.txt              │
│ ──────── │  🖼 photo.jpg   📄 report.md              │
│ Desktop  │                                           │
│ Documents│                                           │
│ Downloads│                                           │
│ Pictures │                                           │
│ ──────── │                                           │
│ C:\      │                                           │
│ D:\      │                                           │
├──────────┴───────────────────────────────────────────┤
│  6 items  |  2 folders, 4 files  |  12.4 KB          │
└──────────────────────────────────────────────────────┘
```

## Features

| Feature | Priority |
|---------|----------|
| Browse directories | P0 |
| Icon view / detail view | P0 |
| Navigate (back, forward, up, address bar) | P0 |
| Create folder | P1 |
| Delete / rename | P1 |
| Copy / paste files | P1 |
| Drag and drop | P2 |
| File/folder properties dialog | P2 |
| Search within folder | P2 |
| Preview pane | P3 |
| Tabs | P3 |

## Key design
- **Icons**: from icon store (`icon_for_extension()`)
- **File operations**: VFS `vfs_create`, `vfs_delete`, `vfs_rename`
- **Sorting**: by name, date, size, type
- **Double-click**: opens file with associated app (from Codex `System\FileTypes`)

## Files: `src/apps/filemgr/filemgr.c` (~800 lines)
## Implementation: 2-3 weeks
