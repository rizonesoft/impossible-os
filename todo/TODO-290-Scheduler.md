# P0009 — Scheduled Tasks

> **Goal:** Run registered commands at specified intervals — powers NTP sync,
> Registry flush, search index rebuild, and log rotation.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 1. Task Scheduler

**Prompt:** The task scheduler runs registered commands at specified intervals. Each `struct sched_task` has a name, command string, interval in seconds, next_run timestamp, and enabled flag. `sched_task_tick()` is called by the PIT timer handler every second — it checks each task's next_run against current time and executes due tasks. After completing all items, create `docs/architecture/scheduler.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: task scheduler"`.


- [ ] Define `struct sched_task` (name, command, arguments, interval_seconds, next_run, enabled)
- [ ] Create `src/kernel/scheduler_tasks.c`
- [ ] Implement `sched_task_add(task)` — register a scheduled task
- [ ] Implement `sched_task_remove(name)` — unregister
- [ ] Implement `sched_task_tick()` — called by timer, check if any tasks are due
- [ ] Hook `sched_task_tick()` into PIT timer (check every second)
- [ ] Commit: `"kernel: task scheduler"`

---

## 2. Built-In Scheduled Tasks

- [ ] NTP sync — every 60 minutes
- [ ] Registry flush — every 2 seconds (dirty-flag)
- [ ] Search index rebuild — every 30 minutes
- [ ] Log rotate — every 24 hours (trim old entries)
- [ ] Store tasks in Registry: `HKLM\SYSTEM\Scheduler\Tasks\{name}\*`
- [ ] Commit: `"kernel: built-in scheduled tasks"`
