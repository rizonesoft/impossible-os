# 59 — Scheduled Tasks

## Overview

Run tasks on a schedule — periodic maintenance, backups, index rebuilds. Like Windows Task Scheduler / cron.

---

## API
```c
struct sched_task {
    char     name[64];
    char     command[256];       /* path to executable */
    char     arguments[256];
    uint32_t interval_seconds;   /* repeat every N seconds (0 = one-shot) */
    uint64_t next_run;           /* Unix timestamp of next run */
    uint8_t  enabled;
};

int sched_task_add(const struct sched_task *task);
int sched_task_remove(const char *name);
void sched_task_tick(void);  /* called by timer — check if any tasks due */
```

## Built-in scheduled tasks
| Task | Interval | Purpose |
|------|----------|---------|
| NTP sync | 60 min | Re-sync clock |
| Codex flush | 2 sec | Write dirty Codex to disk |
| Search index | 30 min | Rebuild file index |
| Log rotate | 24 hours | Trim old log entries |
| Update check | 24 hours | Check for OS updates |

## Codex: `System\Scheduler\Tasks\{name}\*`

## Files: `src/kernel/scheduler_tasks.c` (~150 lines)
## Implementation: 2-3 days
