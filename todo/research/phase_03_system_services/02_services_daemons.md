# 61 — Services / Daemons

## Overview

Background processes that run without a window — network services, Codex sync, NTP, indexer. Like Windows Services / Linux systemd.

---

## Built-in services
| Service | Purpose | Auto-start |
|---------|---------|-----------|
| `netd` | Network stack (DHCP, ARP) | Yes |
| `ntpd` | Time sync | Yes (after network) |
| `codexd` | Codex dirty-flag flush | Yes |
| `indexd` | File search indexer | Yes |
| `updated` | Update checker | Yes |
| `printd` | Print spooler | On demand |

## API
```c
/* service.h */
typedef enum { SVC_STOPPED, SVC_RUNNING, SVC_STARTING } svc_state_t;

struct service {
    char       name[32];
    char       exe_path[256];
    svc_state_t state;
    uint64_t   pid;
    uint8_t    auto_start;  /* start at boot */
};

int svc_start(const char *name);
int svc_stop(const char *name);
int svc_restart(const char *name);
svc_state_t svc_status(const char *name);
int svc_list(struct service *out, int max);
```

## Managed via `services.spl` in Settings Panel or `sc` shell command
## Codex: `System\Services\{name}\AutoStart`, `System\Services\{name}\ExePath`

## Files: `src/kernel/service.c` (~250 lines)
## Implementation: 3-5 days
