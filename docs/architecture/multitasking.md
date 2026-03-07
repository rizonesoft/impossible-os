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
| 7 | `SYS_READFILE` | name, buf, size | Read initrd file |
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
