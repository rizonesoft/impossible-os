# 56 — Search & File Indexing

## Overview

System-wide file search — find files by name, content, or type. Powers Start Menu search and File Manager search bar.

---

## Search sources
| Source | What's searched | Speed |
|--------|----------------|-------|
| **File names** | VFS directory traversal | Fast (name only) |
| **File contents** | Read + scan text files | Slow (full scan) |
| **Apps** | Scan `C:\Impossible\Bin\` + `C:\Programs\` | Fast |
| **Settings** | Search Settings Panel applet names | Fast |

## Indexing (background)
```c
/* search.h */
struct search_result {
    char     path[256];
    char     match[64];      /* matched portion */
    uint8_t  type;           /* FILE, FOLDER, APP, SETTING */
    uint64_t modified;       /* last modified timestamp */
};

/* Rebuild index (runs in background thread) */
void search_index_rebuild(void);

/* Search by query string */
int search_query(const char *query, struct search_result *results, int max);
```

## Index stored in `C:\Impossible\System\Cache\search.idx` (simple flat file: path + name)
## Rebuilt periodically or on file change notification

## Files: `src/kernel/search.c` (~300 lines)
## Implementation: 1 week
