# 100 — Developer Tools

## Overview
Built-in tools for developers and power users: debug console, memory inspector, syscall tracer, performance profiler.

## Tools

### Debug Console (F12)
Overlay panel showing real-time kernel messages, like browser DevTools.
```
┌── Debug Console ──────────────────────┐
│ [Kernel] [Memory] [Network] [Syscalls]│
│ 18:05:01 wm: window_create(notepad)  │
│ 18:05:01 mem: alloc 4096 @ 0x40000   │
│ 18:05:02 kbd: scancode 0x1E (A)      │
│ 18:05:02 sys: SYS_WRITE(fd=1, n=12)  │
│ > _                                   │
└───────────────────────────────────────┘
```

### Memory Inspector
- View physical/virtual memory map
- Show per-process memory usage
- Heap fragmentation visualization

### Syscall Tracer (`strace` equivalent)
```bash
strace notepad.exe
# SYS_OPEN("readme.txt", READ) = 3
# SYS_READ(3, buf, 4096) = 245
# SYS_WRITE(1, buf, 245) = 245
```

### Performance Profiler
- FPS counter overlay for compositor
- Function-level timing (how long did each syscall take?)
- IPC throughput stats

### Pixel Inspector
- Hover overlay showing exact RGBA values under cursor
- Shows window ID, z-order, compositor layer info

## Shortcut: F12 toggles debug console
## Codex: `System\Developer\DebugConsole = 0`, `System\Developer\ShowFPS = 0`
## Files: `src/kernel/debug/` (~600 lines total) | 1-2 weeks
