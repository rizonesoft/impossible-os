# Multitasking & Processes

Impossible OS implements preemptive multitasking with ring 3 user-mode processes,
ELF binary loading, and a Unix-style process lifecycle (fork/exec/waitpid/exit).

## Architecture Overview

```
User programs (ring 3)
  │  INT 0x80 (syscall)
  ▼
┌──────────────────────┐
│   Syscall Dispatcher │  SYS_WRITE, SYS_READ, SYS_EXIT, SYS_FORK, SYS_EXEC...
│    (syscall.c)       │
└──────┬───────────────┘
       │
┌──────▼───────────────┐
│     Scheduler        │  Round-robin, 50ms quantum
│    (task.c)          │  PIT IRQ 0 → schedule()
└──────┬───────────────┘
       │
┌──────▼───────────────┐
│   Context Switch     │  Save/restore all GPRs + RSP + RIP
│ (switch_context.asm) │  TSS provides kernel stack for ring transitions
└──────────────────────┘
```

## Task Control Block (TCB)

Each task has a `struct task` containing:

| Field | Description |
|-------|-------------|
| `pid` | Process ID |
| `name` | Task name string |
| `state` | RUNNING, READY, BLOCKED, DEAD |
| `rsp` | Saved stack pointer |
| `stack_base` | Bottom of the kernel stack |
| `exit_status` | Return code from `sys_exit()` |
| `parent_pid` | For `waitpid()` support |
| `threads[]` | Per-task thread pool (`THREAD_MAX` = 16) |
| `num_threads` | Active thread count (thread 0 = main) |

## Kernel Threads

Each task can spawn up to 16 threads that share the same PID and address space.
Threads are the actual schedulable units — the scheduler picks the next ready task,
then picks its active thread.

### Thread Control Block

| Field | Description |
|-------|-------------|
| `id` | Thread ID (unique within parent task) |
| `state` | `THREAD_RUNNING`, `THREAD_READY`, `THREAD_BLOCKED`, `THREAD_DEAD` |
| `rsp` | Saved stack pointer (interrupt frame) |
| `stack_base` | Base of allocated stack (for `kfree`) |
| `stack_size` | Allocated stack size (default: 8 KiB) |
| `parent_task` | Index into `tasks[]` (owning task) |
| `exit_status` | Set on `THREAD_DEAD` |
| `join_tid` | Thread we're blocked on (`-1` = none) |

### Thread API

| Function | Description |
|----------|-------------|
| `thread_create(entry, arg, stack_size)` | Spawn a new thread in current task (returns TID) |
| `thread_exit(status)` | Exit current thread, wake joiners |
| `thread_join(thread_id)` | Block until target thread exits, return status |
| `thread_yield()` | Cooperative context switch (same as `yield()`) |
| `thread_current()` | Get current `struct thread *` |

### Design

| Property | Value |
|----------|-------|
| Max threads per task | 16 (`THREAD_MAX`) |
| Default stack size | 8 KiB (`THREAD_STACK_SIZE`) |
| Scheduling | Threads share the round-robin scheduler with tasks |
| Shared state | Threads share PID, address space, and heap |
| Thread 0 | Main thread — created implicitly by `task_create()` |

### Thread Lifecycle

```
thread_create()
      │
      ▼
   READY ──── scheduled ───► RUNNING
      │                         │
      │                    thread_exit()
      │                         │
      │                         ▼
      │                       DEAD ── thread_join() collects
      │
      ├── thread_join(tid) ──► BLOCKED
      │                         │
      │                    target exits
      │                         │
      └──── woken ◄────────────┘
```

## Mutex Synchronization

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/sched/mutex.c` | Mutex implementation |
| `include/kernel/sched/mutex.h` | `mutex_t` struct and API |

### `mutex_t` Structure

| Field | Description |
|-------|-------------|
| `locked` | Locked flag (0/1, volatile) |
| `owner_task` | Task index of current holder |
| `owner_thread` | Thread index of current holder |
| `lock_order` | Ordering ID for deadlock detection (0 = unchecked) |
| `waiter_tasks[]` | Wait queue — up to 16 blocked threads |
| `name` | Debug name (e.g., "heap_lock") |

### API

| Function | Description |
|----------|-------------|
| `mutex_init(m, name)` | Initialize a mutex |
| `mutex_init_ordered(m, name, order)` | Init with lock order for deadlock detection |
| `mutex_lock(m)` | Acquire — blocks via `yield()` if locked; detects self-deadlock |
| `mutex_unlock(m)` | Release — owner-only; wakes one waiter |
| `mutex_trylock(m)` | Non-blocking acquire (returns 1=success, 0=fail) |
| `mutex_is_locked(m)` | Check lock state (debug) |
| `MUTEX_INIT(name)` | Static initializer macro |

### Deadlock Detection

- **Self-deadlock**: `mutex_lock()` detects if the calling thread already holds the mutex
- **Lock ordering**: `mutex_init_ordered()` assigns a sequential order ID; locks must be acquired in ascending order to prevent ABBA deadlocks

## Semaphore Synchronization

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/sched/semaphore.c` | Semaphore implementation |
| `include/kernel/sched/semaphore.h` | `semaphore_t` struct and API |

### `semaphore_t` Structure

| Field | Description |
|-------|-------------|
| `count` | Current count (volatile `int32_t`) |
| `waiter_tasks[]` | Wait queue — up to 16 blocked task indices |
| `waiter_threads[]` | Corresponding thread indices |
| `num_waiters` | Number of currently blocked waiters |
| `name` | Debug name |

### API

| Function | Description |
|----------|-------------|
| `sem_init(s, name, count)` | Initialize with an initial count |
| `sem_wait(s)` | Decrement — blocks via `yield()` if count < 0 |
| `sem_signal(s)` | Increment — wakes one blocked waiter |
| `sem_trywait(s)` | Non-blocking decrement (returns 1=success, 0=would block) |
| `sem_value(s)` | Get current count (debug) |
| `SEM_INIT(name, count)` | Static initializer macro |

### Use Cases

- **Binary semaphore** (init count = 1): Mutual exclusion (like a mutex)
- **Counting semaphore** (init count = N): Limit concurrent access to N slots
- **Event signaling** (init count = 0): Producer signals, consumer waits

## Pipes (IPC)

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/ipc/pipe.c` | Pipe implementation (202 lines) |
| `include/kernel/ipc/pipe.h` | `pipe_t` struct and API |

### `pipe_t` Structure

| Field | Description |
|-------|-------------|
| `buf[4096]` | 4 KiB ring buffer |
| `read_pos` | Read cursor into ring buffer |
| `write_pos` | Write cursor into ring buffer |
| `count` | Bytes currently in buffer |
| `lock` | `mutex_t` — protects buffer state |
| `readable` | `semaphore_t` — signaled when data available |
| `writable` | `semaphore_t` — signaled when space available |
| `read_open` | 1 if read end is open |
| `write_open` | 1 if write end is open |

### API

| Function | Description |
|----------|-------------|
| `pipe_create(fds[2])` | Allocate pipe, set `fds[0]`=read, `fds[1]`=write |
| `pipe_write(pipe, data, len)` | Write bytes — blocks if full |
| `pipe_read(pipe, buf, len)` | Read bytes — blocks if empty, returns 0 on EOF |
| `pipe_close(pipe, end)` | Close one end — broken pipe on write to closed read end |
| `SYS_PIPE` (33) | Syscall wrapper |

### Data Flow

```
Writer                          Reader
  │                               │
  ├─ sem_wait(writable) ──────►  blocked on sem_wait(readable)
  ├─ mutex_lock(lock)             │
  ├─ copy byte to ring buf        │
  ├─ mutex_unlock(lock)           │
  ├─ sem_signal(readable) ───────► woken, reads byte
  │                               ├─ sem_signal(writable)
  ▼                               ▼
```

## Signals (IPC)

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/ipc/signal.c` | Signal delivery and dispatch |
| `include/kernel/ipc/signal.h` | Signal constants, `signal_state` struct |

### Signal Constants

| Signal | Number | Default Action |
|--------|--------|----------------|
| `SIGINT` | 2 | Terminate (Ctrl+C) |
| `SIGKILL` | 9 | Always terminate (uncatchable) |
| `SIGTERM` | 15 | Clean termination |
| `SIGCHLD` | 17 | Ignore |

### Per-Task Signal State

| Field | Description |
|-------|-------------|
| `handlers[SIG_MAX]` | Per-signal handler table (`SIG_DFL` / `SIG_IGN` / function) |
| `pending` | Bitmask of pending signals |

### API

| Function | Description |
|----------|-------------|
| `signal_send(pid, sig)` | Send signal to task — sets pending bit, wakes blocked tasks |
| `signal_handler(sig, handler)` | Register handler (SIGKILL uncatchable) — returns previous |
| `signal_check()` | Dispatch pending signals (called from scheduler/yield) |
| `signal_ctrl_c()` | Send SIGINT to foreground task (from keyboard driver) |
| `signal_init_task(ss)` | Initialize signal state for new task |
| `SYS_SIGNAL` (34) | Syscall wrapper |

### Signal Dispatch Flow

1. `signal_send()` sets bit in target's `pending` mask
2. If target is BLOCKED/WAITING, wake it to READY
3. `signal_check()` scans pending bits each scheduler tick
4. SIGKILL → always terminate (ignoring handler)
5. SIG_IGN → skip
6. SIG_DFL → default action table
7. Custom handler → direct call

## Scheduler

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/sched/task.c` | Task management and scheduling |
| `include/kernel/sched/task.h` | TCB struct and API |
| `src/boot/switch_context.asm` | Assembly context switch |

### Design

| Property | Value |
|----------|-------|
| Algorithm | **Round-robin** |
| Time quantum | 5 PIT ticks (50 ms at 100 Hz) |
| Preemption | Timer IRQ (PIT) calls `schedule()` |
| Idle handling | Dead tasks yield via INT 0x81 |

### Cooperative API

| Function | Description |
|----------|-------------|
| `yield()` | Voluntarily give up the CPU |

### Context Switch

The assembly routine `switch_context` saves and restores:
- All general-purpose registers (RAX–R15)
- Stack pointer (RSP)
- Instruction pointer (RIP, via return address)
- RFLAGS

On ring 3→0 transitions, the CPU automatically loads the kernel stack
from the TSS (`RSP0`). `tss_set_kernel_stack()` is called on every
context switch to set the correct kernel stack for the next task.

## User Mode

### Ring Transition

```
Ring 3 (user)                Ring 0 (kernel)
  │                               │
  │  INT 0x80 ──────────────────► │  Syscall handler
  │                               │  (uses kernel stack from TSS)
  │  ◄──────────────────── iretq  │  Return to user
```

User-mode tasks are created with `task_create_user()`, which sets up an
interrupt frame on the kernel stack with:
- `CS` = user code segment (0x18 | DPL 3)
- `SS` = user data segment (0x20 | DPL 3)
- `RSP` = user stack pointer
- `RIP` = program entry point
- `RFLAGS` = interrupts enabled

### System Calls (INT 0x80)

| Number | Name | Arguments | Description |
|--------|------|-----------|-------------|
| 0 | `SYS_WRITE` | fd, buf, len | Write to stdout |
| 1 | `SYS_READ` | fd, buf, len | Blocking read from stdin |
| 2 | `SYS_EXIT` | code | Terminate process |
| 3 | `SYS_YIELD` | — | Voluntary context switch |
| 4 | `SYS_FORK` | — | Duplicate process |
| 5 | `SYS_EXEC` | filename | Replace with new program |
| 6 | `SYS_WAITPID` | pid | Wait for child to exit |
| 7 | `SYS_READFILE` | name, buf, size | Read file from C:\\ |
| 8 | `SYS_READDIR` | buf, size, idx | List directory entry |
| 9 | `SYS_GETPROCS` | buf, size | List all processes |
| 10 | `SYS_KILL` | pid | Kill a process |
| 11 | `SYS_UPTIME` | — | Get uptime in seconds |
| 12 | `SYS_REBOOT` | — | ACPI reboot |
| 13 | `SYS_SHUTDOWN` | — | ACPI shutdown |
| 14 | `SYS_PING` | ip, seq | Send ICMP echo |
| 15 | `SYS_NETINFO` | buf, size | Get network config |

Syscall convention: number in `RAX`, arguments in `RDI`, `RSI`, `RDX`.
Return value placed in `RAX`.

## Process Lifecycle

```
task_create_user()          task_fork()
      │                         │
      ▼                         ▼
   READY ◄──────────────────► READY
      │                         │
      │  scheduled              │  scheduled
      ▼                         ▼
   RUNNING ◄──── schedule() ── RUNNING
      │
      │  sys_exit() or sys_kill()
      ▼
    DEAD ── waitpid() collects ── cleaned up
```

### Process API

| Function | Description |
|----------|-------------|
| `task_create_user(entry, name)` | Create a new user-mode process |
| `task_fork(frame)` | Duplicate the current process |
| `task_exec(elf_data, size)` | Replace process image with ELF binary |
| `task_exit(code)` | Terminate with exit code |
| `task_waitpid(pid)` | Block until child exits, return exit status |
| `task_kill(pid)` | Force-terminate a process |
| `task_count()` | Total number of tasks |
| `task_get_by_pid(pid)` | Look up task by PID |

## ELF Loader

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/elf.c` | ELF binary parser and loader |

### Supported Format

| Property | Value |
|----------|-------|
| Class | ELF64 |
| Architecture | x86-64 |
| Type | Executable (ET_EXEC) |
| Segments | PT_LOAD only |

The loader:
1. Validates the ELF header (magic, class, architecture)
2. Iterates PT_LOAD program headers
3. Copies each segment to its specified virtual address
4. Returns the entry point address for `task_exec()`
