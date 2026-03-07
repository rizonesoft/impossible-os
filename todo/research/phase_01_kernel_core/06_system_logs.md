# 57 — System Logs & Event Viewer

## Overview

Unified logging system: kernel messages, driver events, app errors, and a viewer app.

---

## Log levels
```c
#define LOG_DEBUG   0
#define LOG_INFO    1
#define LOG_WARN    2
#define LOG_ERROR   3
#define LOG_FATAL   4

void sys_log(uint8_t level, const char *source, const char *fmt, ...);
/* sys_log(LOG_INFO, "net", "DHCP: got IP %s", ip_str); */
```

## Log storage
```
C:\Impossible\System\Logs\
├── kernel.log       ← kernel + driver messages
├── network.log      ← network events
├── security.log     ← login attempts, permission denied
├── app.log          ← application errors/crashes
└── boot.log         ← messages from current boot
```

## Event Viewer app
```
┌──────────────────────────────────────────────┐
│  Event Viewer                           ─ □ ×│
├──────────────────────────────────────────────┤
│  All │ Kernel │ Network │ Security │ Apps    │
├──────────────────────────────────────────────┤
│  ⓘ  18:04:11  net      DHCP: got IP 10.0.2.15 │
│  ⓘ  18:04:12  net      NTP: synced             │
│  ⚠  18:04:15  kernel   Low memory warning      │
│  ✕  18:04:20  app      notepad.exe crashed     │
└──────────────────────────────────────────────┘
```

## Ring buffer in RAM (last 1000 entries) + flush to disk periodically

## Files: `src/kernel/syslog.c` (~200 lines), `src/apps/eventview/eventview.c` (~300 lines)
## Implementation: 3-5 days
