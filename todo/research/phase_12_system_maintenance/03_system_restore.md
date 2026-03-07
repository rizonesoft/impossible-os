# 62 — System Restore & Backup

## Overview

Create restore points before major changes. Roll back system files and Codex if something breaks.

---

## Restore points
```
C:\Impossible\System\Restore\
├── 2026-03-07_1800/        ← restore point
│   ├── manifest.ini        ← files included, timestamp, description
│   ├── codex_backup/       ← copy of all .codex files
│   └── system_files.tar    ← snapshot of changed system files
└── 2026-03-05_1200/
    └── ...
```

## When restore points are created
- Before OS update (`50_system_updates.md`)
- Before app install (`45_app_installer.md`)
- Manual: Settings → System → "Create restore point"

## API
```c
int  restore_create(const char *description);
int  restore_list(struct restore_point *out, int max);
int  restore_apply(const char *restore_id);  /* rollback to this point */
void restore_cleanup(int keep_count);        /* keep last N points */
```

## Depends on: `23_disk_persistent_fs.md`

## Files: `src/kernel/restore.c` (~300 lines)
## Implementation: 1 week (after persistent FS)
