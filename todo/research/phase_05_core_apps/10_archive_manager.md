# 90 — Archive Manager

## Overview
GUI app for browsing and extracting archives (ZIP, TAR, GZ). Like WinRAR or 7-Zip.

## Layout
```
┌──────────────────────────────────────────┐
│  Archive Manager — files.zip        ─ □ ×│
├──────────────────────────────────────────┤
│  [Extract All]  [Add Files]  [New]       │
├──────────────────────────────────────────┤
│  Name            Size     Modified       │
│  📁 src/         —        Mar 7, 2026    │
│  📄 readme.txt   2.1 KB   Mar 6, 2026   │
│  📄 main.c       8.4 KB   Mar 5, 2026   │
│  🖼 logo.png     45 KB    Mar 1, 2026   │
├──────────────────────────────────────────┤
│  4 items | 55.5 KB compressed            │
└──────────────────────────────────────────┘
```

## Features: browse archive contents, extract selected files, drag files out
## Double-click `.zip` → opens in Archive Manager
## Depends on: `89_zip_compression.md`
## Files: `src/apps/archiver/archiver.c` (~400 lines) | 3-5 days
