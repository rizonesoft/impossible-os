# 99 — Recovery Environment

## Overview
Minimal boot environment for repairing a broken OS. Like Windows Recovery Environment (WinRE). Boots from ISO or recovery partition.

## Boot menu (hold F8)
```
Impossible OS Recovery
──────────────────────
1. Reset to factory defaults
2. System Restore (rollback to restore point)
3. Command Prompt (recovery shell)
4. Startup Repair (fix boot issues)
5. Reinstall OS (keep user files)
6. Boot from USB
```

## Included tools
- Text-mode shell with basic commands
- Disk tools: check filesystem, repair MBR/GPT
- Codex reset: restore default settings
- File backup: copy user files to USB before reset

## Architecture: separate minimal kernel in recovery partition (no GUI, text-mode only, ~50KB)
## Depends on: `23_disk_persistent_fs.md`, `51_boot_splash.md`, `62_system_restore.md`
## Files: `src/recovery/` (~500 lines total) | 1-2 weeks
## Priority: 🟡 Medium-term (important for real hardware)
