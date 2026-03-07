# 20 — Task Manager

## Overview

Process list, CPU/RAM usage monitoring, and ability to kill processes. Like Windows Task Manager.

---

## Layout

```
┌──────────────────────────────────────────────────┐
│  Task Manager                               ─ □ ×│
├──────────────────────────────────────────────────┤
│  Processes  │ Performance │ Details              │
├──────────────────────────────────────────────────┤
│  Name            CPU    RAM     PID   Status     │
│  ──────────────────────────────────────────────  │
│  desktop.exe     12%    2.1 MB   1    Running    │
│  shell.exe        3%    0.4 MB   2    Running    │
│  terminal.exe     1%    1.2 MB   5    Running    │
│  notepad.exe      0%    0.8 MB   7    Running    │
│                                                  │
│                                                  │
├──────────────────────────────────────────────────┤
│  Processes: 4  |  CPU: 16%  |  RAM: 4.5/2048 MB │
│                               [End Task]         │
└──────────────────────────────────────────────────┘
```

## Performance tab: CPU/RAM usage graph (rolling line chart)
## API: reads from scheduler task list + PMM for memory stats
## Shortcut: Ctrl+Shift+Esc to open
## Files: `src/apps/taskmgr/taskmgr.c` (~500 lines)
## Implementation: 1 week
