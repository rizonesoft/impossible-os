# P0009 — Scheduled Tasks

> **Goal:** Run registered commands at specified intervals — powers NTP sync,
> Registry flush, search index rebuild, log rotation, and user-defined tasks.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 1. Task Scheduler

**Prompt:** The task scheduler runs registered commands at specified intervals. Each `struct sched_task` has a name, callback function pointer (or command string), interval in seconds, next_run_timestamp, enabled flag, and last_run_result. `sched_task_tick()` is called by the PIT timer handler every second — it checks each task's next_run against current wall-clock time (from TODO-210 §1) and executes due tasks. Tasks run in a short interrupt-deferred worker context — they must be non-blocking. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: task scheduler"`. Add notes directly in this TODO section covering the PIT hook, task execution context, and blocking task restrictions.

> **Beats:** Windows Task Scheduler is a COM-based service. Linux cron is a separate daemon. Impossible OS: in-kernel scheduler, zero processes, PIT-driven — always available.

- [ ] Define `struct sched_task`:
  - [ ] `name[32]` — human-readable task name
  - [ ] `callback` — function pointer `void (*fn)(void)` OR `cmd[128]` string
  - [ ] `interval_seconds` — run every N seconds
  - [ ] `next_run` — Unix timestamp of next execution
  - [ ] `enabled` — bool, can disable without removing
  - [ ] `last_result` — exit code of last run
- [ ] Create `src/kernel/sched_tasks.c` and `include/kernel/sched_tasks.h`
- [ ] Static task table: `SCHED_TASK_MAX = 16` tasks (no dynamic allocation needed)
- [ ] `sched_task_add(task)` — register a task (return -1 if table full)
- [ ] `sched_task_remove(name)` — unregister by name
- [ ] `sched_task_enable(name, bool)` — enable/disable without removing
- [ ] `sched_task_tick()` — check all tasks, execute if `now >= next_run`
- [ ] Hook `sched_task_tick()` into PIT timer (call once per second, not per tick)
- [ ] Boot log: `[OK] Task scheduler: N tasks registered`
- [ ] Commit: `"kernel: task scheduler"`

---

## 2. Built-In Scheduled Tasks

**Prompt:** Register the core OS maintenance tasks in the scheduler. Each task is added during kernel init. The NTP sync runs every 60 minutes and calls `ntp_sync()` from TODO-210 §4. The Registry flush runs every 2 seconds and calls `registry_flush_dirty()`. The search index rebuild runs every 30 minutes and calls `search_index_rebuild()` from TODO-260 §1. Log rotation runs every 24 hours and trims `klog.txt` to the last 5000 lines. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: built-in scheduled tasks"`.

- [ ] **NTP sync** — every 3600s — calls `ntp_sync()` (TODO-210 §4)
- [ ] **Registry flush** — every 2s — calls `registry_flush_dirty()` (TODO-050)
- [ ] **Search index rebuild** — every 1800s — calls `search_index_rebuild()` (TODO-260 §1)
- [ ] **Log rotate** — every 86400s — trim `C:\Impossible\System32\klog.txt` to last 5000 lines
- [ ] Store task configs in Registry: `HKLM\SYSTEM\Scheduler\Tasks\{name}\Interval`, `HKLM\SYSTEM\Scheduler\Tasks\{name}\Enabled`
- [ ] Load tasks from Registry at boot — override built-in defaults with Registry values
- [ ] Commit: `"kernel: built-in scheduled tasks"`

---

## 3. Shell Command: `at`

**Prompt:** The `at` command lets users schedule one-shot commands from the terminal: `at 14:30 shutdown` schedules a command to run at 2:30 PM today. `at list` shows pending one-shot tasks. `at cancel <id>` removes a pending task. One-shot tasks have `interval = 0` and are removed after running once. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"shell: at command for one-shot tasks"`.

- [ ] Shell command: `at <HH:MM> <command>` — schedule one-shot task
- [ ] `at list` — show all pending scheduled tasks (built-in + user)
- [ ] `at cancel <id>` — cancel a pending task
- [ ] One-shot: `interval = 0` flag — auto-remove after running once
- [ ] Commit: `"shell: at command for one-shot tasks"`

---

## 4. Control Panel Applet: Task Manager *(Stretch)*

**Prompt:** A "Scheduled Tasks" applet in the Control Panel shows all registered tasks, their interval, last run time, and last result. Toggle enable/disable. Add a custom task via a dialog: name, command, interval (minutes). After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: scheduled tasks control panel applet"`.

- [ ] *(Stretch)* Create `src/apps/control/taskschd.cpl`
- [ ] *(Stretch)* List all tasks: name, interval, last run, last result
- [ ] *(Stretch)* Toggle enable/disable
- [ ] *(Stretch)* Add custom task via dialog
- [ ] *(Stretch)* Commit: `"apps: scheduled tasks control panel applet"`

---

## Priority Order

| Priority | Section                         | Reason                                     |
|----------|---------------------------------|--------------------------------------------|
| 🔴 P0    | §1 Task Scheduler               | Foundation — built-in tasks need the tick  |
| 🔴 P0    | §2 Built-In Tasks               | Registry flush + NTP + search index        |
| 🟡 P2    | §3 Shell `at` Command           | One-shot scheduling from terminal          |
| 🔵 P4    | §4 Control Panel Applet (Stretch)    | GUI task management                        |

---

## Key Files

| File                              | Purpose                                    |
|-----------------------------------|--------------------------------------------|
| `src/kernel/sched_tasks.c`        | [NEW] Scheduler tick + task table          |
| `include/kernel/sched_tasks.h`    | [NEW] Scheduler API header                 |

---

## OS Comparison

| Feature                           | Windows 11 (Task Scheduler svchost)  | Linux (cron / systemd timers)         | Impossible OS                          |
|-----------------------------------|--------------------------------------|---------------------------------------|----------------------------------------|
| Interval-based task scheduling    | ✅ Task Scheduler (COM)               | ✅ cron / `systemd.timer`             | ⬜ §1 P0 — PIT-driven in-kernel tick  |
| Persist tasks in Registry/file    | ✅ XML task definitions               | ✅ crontab / .timer unit files        | ⬜ §2 — Registry `HKLM\SYSTEM\Scheduler\` |
| One-shot scheduling               | ✅ Task Scheduler triggers            | ✅ `at` command                       | ⬜ §3 P2 — `at <HH:MM> <cmd>`         |
| List/cancel scheduled tasks       | ✅ `schtasks /query`                  | ✅ `crontab -l` / `systemctl list-timers` | ⬜ §3 — `at list` / `at cancel`   |
| GUI task management               | ✅ Task Scheduler MMC snap-in        | ✅ GNOME/KDE crontab editors          | ⬜ §4 P4 (stretch)                    |
| **No external process**           | ❌ svchost.exe Task Scheduler service| ❌ cron daemon / systemd timers        | ✅ **In-kernel PIT hook — zero overhead** |
| **Always-available scheduler**    | ❌ Service must be running            | ❌ cron daemon must be running         | ✅ **PIT-driven: always fires, even before full init** |
