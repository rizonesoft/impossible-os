# TODO-09 — Process Model Extensions

> **Goal:** Extend the kernel process model with the per-process state fields and syscalls that don't belong to the scheduler, VMM, or Object Manager individually: current working directory, standard handle pre-wiring, user-mode program break (Linux compat heap), process priority classes mapped to Win32 `SetPriorityClass`, scheduling policy per-task (`SCHED_FIFO`/`SCHED_IDLE`), and a process capability/privilege bitmask. All of these hang off `struct task` and are needed before any non-trivial user-mode program can run correctly.

> [!IMPORTANT]
> **Current state:** `struct task` has `pid`, `state`, `rsp`, stacks, `name`, `parent_pid`, `exit_status`, `wait_pid`, and `signals`. Thread-level priority is fully implemented (`THREAD_PRIO_IDLE`…`THREAD_PRIO_REALTIME`, priority-aware scheduler, PI boosting). Missing from `struct task`: `cwd`, `capabilities`, `brk`/`program_break`, `sched_policy`, and process-class priority. Handle table infrastructure is in TODO-03. Environment block is in TODO-04 §5.

## Inputs

- [`include/kernel/sched/task.h`](../../include/kernel/sched/task.h) — `struct task`, `struct thread`, priority constants
- [`src/kernel/sched/task.c`](../../src/kernel/sched/task.c) — `task_create`, `task_fork`, `task_exec`, `task_exit`, scheduler loop
- [`src/kernel/fs/vfs.c`](../../src/kernel/fs/vfs.c) — `vfs_open`, relative path lookup entry point
- [`src/kernel/mm/vmm.c`](../../src/kernel/mm/vmm.c) — `vmm_map_pages` for program-break page allocation
- → XREF: `TODO-03-object-manager.md` §3 — `HANDLE_TABLE` and `ObpAllocateHandle` / `ObpFreeHandle` provide the handle table; §9 provides `NtClose` / `NtDuplicateObject`
- → XREF: `TODO-04-peb-teb-user-abi.md` §5 — `RTL_USER_PROCESS_PARAMETERS.Environment` covers the environment block; `CurrentDirectory` field lives in `RTL_USER_PROCESS_PARAMETERS`
- → XREF: `TODO-05-native-api-layer.md` §9 — `NtAllocateVirtualMemory` is the Win32-native heap path; `brk`/`sbrk` here is the Linux-compat path only
- → XREF: `TODO-07-time-filetime-management.md` §6 — `KeDelayExecutionThread` is the sleep implementation; `NtDelayExecution` syscall wiring belongs there
- → XREF: `TODO-08-binary-system.md` §1 — `exec_load()` dispatcher sets `brk` to end of BSS at load time

## Outcome

- `struct task` carries `cwd[MAX_PATH]`, `sched_policy`, `priority_class`, `capabilities`, and `program_break`.
- VFS relative path resolution prepends `task->cwd` for any path without a drive-letter prefix.
- Every new process has `STD_INPUT_HANDLE`, `STD_OUTPUT_HANDLE`, and `STD_ERROR_HANDLE` wired into its handle table at creation (using the TODO-03 handle table infrastructure).
- `brk` / `sbrk` syscalls allocate user pages via VMM for Linux-compat user-mode `malloc`.
- `SetPriorityClass` maps process priority class (`IDLE`, `NORMAL`, `HIGH`, `REALTIME`) to the existing thread priority range; `NtSetInformationProcess(ProcessPriorityClass)` is the native entry point.
- Per-task `SCHED_POLICY_FIFO` and `SCHED_POLICY_IDLE` policies are enforced by the scheduler loop.
- A `capabilities` bitmask on `struct task` gates privileged kernel operations; user processes receive a restricted default set; capabilities are inherited and can only be dropped, never gained.

## Implementation Order

| ⭐  | Order | Deliverable                                          | Depends On             | Status |
| --- | :---: | ---------------------------------------------------- | ---------------------- | :----: |
| 💎  |   1   | Working directory (`cwd` field + Nt/VFS wiring)      | VFS                    |  [ ]   |
| 💎  |   2   | Standard handle pre-wiring at process creation       | TODO-03 §3             |  [ ]   |
| 💎  |   3   | User-mode program break (brk/sbrk Linux compat)      | VMM, TODO-08 §1        |  [ ]   |
| 💎  |   4   | Process priority class (Win32 `SetPriorityClass`)    | sched (exists)         |  [ ]   |
| 💎  |   5   | Per-task scheduling policy (`SCHED_FIFO`/`IDLE`)     | 4                      |  [ ]   |
| 💎  |   6   | Process capabilities and privilege bitmask           | —                      |  [ ]   |
| ⭐  |   7   | Capability inheritance and drop-only policy          | 6                      |  [ ]   |

> 💎 = parity — Windows NT (tokens + priority classes) and Linux (capabilities + scheduling policies) both provide these.
> ⭐ = exclusive — strict drop-only capability inheritance with no privilege escalation path is more auditable than both Windows token elevation and Linux `setcap`.

---

## 1. Working Directory `[Sonnet]`

`struct task` has no `cwd` field. All VFS paths are currently treated as absolute. Relative path resolution must be added before any shell navigation or portable app path handling works.

- [ ] Add `char cwd[512]` to `struct task`; initialize to `"C:\\"` at `task_init()` and `task_create()`
- [ ] Inherit CWD from parent at `task_fork()` — copy the string
- [ ] In `task_exec()` / `exec_load()`: preserve CWD from the calling process (do not reset on exec)
- [ ] Add VFS relative-path resolver: if the path does not begin with a drive letter (`X:\`), prepend `task->cwd` before passing to VFS lookup
- [ ] `NtSetCurrentDirectory(UNICODE_STRING *path)`: validate path exists via VFS, update `task->cwd`
- [ ] `NtQueryCurrentDirectory(buffer, length)`: copy `task->cwd` to user buffer
- [ ] Register both in SSDT (→ XREF TODO-05 §4)
- [ ] Win32 wrappers: `SetCurrentDirectory` → `NtSetCurrentDirectory`; `GetCurrentDirectory` → `NtQueryCurrentDirectory`
- [ ] Commit: `"kernel: task — working directory field and Nt API wiring"`

## 2. Standard Handle Pre-Wiring at Process Creation `[Sonnet]`

When a process is created, `STD_INPUT_HANDLE` (0), `STD_OUTPUT_HANDLE` (1), and `STD_ERROR_HANDLE` (2) must already be open in its handle table. This requires TODO-03 §3 to be done first.

- [ ] Define `STD_INPUT_HANDLE`, `STD_OUTPUT_HANDLE`, `STD_ERROR_HANDLE` pseudo-handle constants in `include/kernel/nt/nt_types.h`
- [ ] At `task_create()` / `NtCreateProcess()`: allocate the process handle table (TODO-03 §3); wire the three standard handles:
  - STD_INPUT: terminal read end (initially the keyboard VFS node)
  - STD_OUTPUT / STD_ERROR: terminal write end (initially the framebuffer console VFS node)
- [ ] `GetStdHandle(nStdHandle)` → look up the pre-wired slot; `SetStdHandle(nStdHandle, handle)` → replace it
- [ ] `DuplicateHandle`: duplicate a handle slot into the child process at `NtCreateProcess` time for I/O redirection (`cmd > file` pipes the child stdout to a file handle before execution)
- [ ] Inherit standard handles into child at `task_fork()` if `HANDLE_FLAG_INHERIT` is set (TODO-03 §10)
- [ ] Commit: `"kernel: task — STD handle pre-wiring at process creation"`

## 3. User-Mode Program Break (brk/sbrk — Linux Compat) `[Sonnet]`

Win32 programs use `NtAllocateVirtualMemory` (TODO-05 §9) for heap. Linux-compat programs call `brk(2)` / `sbrk(2)` which the kernel must handle by growing user-space pages.

- [ ] Add `uint64_t program_break` to `struct task`; set to end of BSS segment in `exec_load()` / `task_exec()` (→ XREF TODO-08 §1)
- [ ] `sys_brk(addr)`: if `addr == 0`, return current break; if `addr > program_break`, allocate VMM pages to cover the new range; update `program_break`; reject addresses below BSS end
- [ ] `sys_sbrk(increment)`: return old break; advance by `increment` bytes via `sys_brk`
- [ ] Reject `increment < 0` if it would shrink below BSS end (simplification; full shrink support is optional)
- [ ] Add `SYS_BRK` to the Linux-compat syscall table (keep separate from SSDT — POSIX compat only)
- [ ] Verify: user-mode `malloc` backed by a musl-style `sbrk` wrapper can allocate and free without crashing
- [ ] Commit: `"kernel: task — program break (brk/sbrk) for Linux compat heap"`

## 4. Process Priority Class `[Sonnet]`

Thread priority already exists. This section adds the Win32 `PROCESS_PRIORITY_CLASS` concept: a process-level class that sets a base priority range, and `NtSetInformationProcess` / `NtQueryInformationProcess` as the kernel entry points.

- [ ] Add `uint8_t priority_class` to `struct task` with constants:
  - `PROCESS_PRIORITY_IDLE = 0` → thread base `THREAD_PRIO_IDLE`
  - `PROCESS_PRIORITY_BELOW_NORMAL = 1` → thread base `THREAD_PRIO_LOW`
  - `PROCESS_PRIORITY_NORMAL = 2` → thread base `THREAD_PRIO_NORMAL`
  - `PROCESS_PRIORITY_ABOVE_NORMAL = 3` → thread base `THREAD_PRIO_HIGH - 4`
  - `PROCESS_PRIORITY_HIGH = 4` → thread base `THREAD_PRIO_HIGH`
  - `PROCESS_PRIORITY_REALTIME = 5` → thread base `THREAD_PRIO_REALTIME`; requires `CAP_REALTIME`
- [ ] `NtSetInformationProcess(ProcessHandle, ProcessPriorityClass, &class, size)`: update `priority_class`; apply base priority to all existing threads in the process
- [ ] `NtQueryInformationProcess(ProcessHandle, ProcessPriorityClass, ...)`: return current class
- [ ] Changing `priority_class` adjusts all thread `base_priority` values but does not override active PI boosts
- [ ] Compositor and audio: spawned at `PROCESS_PRIORITY_HIGH`; background tasks at `PROCESS_PRIORITY_IDLE`
- [ ] Commit: `"kernel: task — process priority class and NtSetInformationProcess"`

## 5. Per-Task Scheduling Policy `[Sonnet]`

Add a per-task `sched_policy` field for tasks that need non-time-sliced execution.

- [ ] Add `uint8_t sched_policy` to `struct task` with values:
  - `SCHED_POLICY_NORMAL = 0` — default round-robin with `SCHED_QUANTUM` time slice
  - `SCHED_POLICY_FIFO   = 1` — run until block or yield; no preemptive time-slice; requires `CAP_SCHED_FIFO`
  - `SCHED_POLICY_IDLE   = 2` — only scheduled when no `SCHED_POLICY_NORMAL` or `SCHED_POLICY_FIFO` task is runnable
- [ ] Update `schedule()` in `task.c`: skip `SCHED_POLICY_FIFO` tasks for quantum-based preemption; skip `SCHED_POLICY_IDLE` tasks when higher-policy tasks are runnable
- [ ] `NtSetInformationProcess(ProcessHandle, ProcessSchedulingPolicy, ...)`: validate `CAP_SCHED_FIFO` for FIFO; update field
- [ ] Kernel-internal convenience: `task_set_sched_policy(pid, SCHED_POLICY_FIFO)` callable from boot paths
- [ ] Commit: `"kernel: sched — per-task scheduling policy (FIFO, IDLE)"`

## 6. Process Capabilities and Privilege Bitmask `[Opus]`

Every process carries a `capabilities` bitmask. Privileged syscalls check it before executing. Capabilities flow from parent to child and can only be dropped.

- [ ] Define capability constants in `include/kernel/security/capabilities.h`:
  - `CAP_SYS_ADMIN       (1ULL << 0)` — general admin (mount, kmod, etc.)
  - `CAP_RAW_IO          (1ULL << 1)` — direct disk / port I/O
  - `CAP_NET_ADMIN       (1ULL << 2)` — raw socket / network config
  - `CAP_LOAD_DRIVER     (1ULL << 3)` — load kernel-mode drivers
  - `CAP_KILL_ALL        (1ULL << 4)` — signal any process
  - `CAP_SET_TIME        (1ULL << 5)` — call `NtSetSystemTime` (→ XREF TODO-07 §7)
  - `CAP_REALTIME        (1ULL << 6)` — set `PROCESS_PRIORITY_REALTIME`
  - `CAP_SCHED_FIFO      (1ULL << 7)` — use `SCHED_POLICY_FIFO`
  - `CAP_DEBUG           (1ULL << 8)` — attach debugger to another process
- [ ] Add `uint64_t capabilities` to `struct task`; system processes get `CAP_ALL = ~0ULL` at kernel init
- [ ] `CAP_DEFAULT_USER`: mask granting no privileged capabilities to user processes
- [ ] `capability_check(uint64_t cap)` — returns `STATUS_SUCCESS` if `task_current()->capabilities & cap`, else `STATUS_PRIVILEGE_NOT_HELD`
- [ ] Gate privileged paths: `CAP_SET_TIME` in `KeSetSystemTime`, `CAP_RAW_IO` in raw disk syscalls, `CAP_LOAD_DRIVER` in driver load path, `CAP_REALTIME` in priority-class enforcement
- [ ] Commit: `"kernel: security — process capabilities bitmask"`

## 7. Capability Inheritance and Drop-Only Policy `[Opus]`

Capabilities can be inherited across `fork` / `exec` but can only be dropped, never gained. This makes privilege de-escalation auditable and prevents accidental escalation.

- [ ] At `task_fork()`: child inherits parent `capabilities` exactly
- [ ] At `task_exec()` via `exec_load()`: apply EIF capability mask from EIF header flags (§4.1 SIGNED + capabilities field); strip any bits not in the parent's set; never add new capabilities on exec
- [ ] `NtDropCapability(cap_mask)`: clear one or more capability bits from the calling process; irreversible for the lifetime of the process
- [ ] No syscall or path grants new capabilities — escalation requires a restart or a privileged parent spawning with a specific mask
- [ ] Document the invariant: `child->capabilities ⊆ parent->capabilities` is enforced at fork and exec
- [ ] Commit: `"kernel: security — capability inheritance and drop-only policy"`

---

## OS Comparison

| ⭐  | Feature                                      | 🪟 Windows 11 / NT                              | 🐧 Linux                                        | 🚀 Impossible OS                                           |
| --- | -------------------------------------------- | ----------------------------------------------- | ----------------------------------------------- | ---------------------------------------------------------- |
| 💎  | Per-process working directory                | ✅ `SetCurrentDirectory` / `NtSetCurDir`         | ✅ `chdir(2)` / `getcwd(2)`                      | ⬜ Planned — §1                                            |
| 💎  | STD handle pre-wiring (STDIN/OUT/ERR)        | ✅ inherited or set via `CreateProcess`          | ✅ FDs 0/1/2 inherited via `fork`/`exec`         | ⬜ Planned — §2                                            |
| 💎  | User-mode heap growth syscall                | ✅ `NtAllocateVirtualMemory` (Win32 native)      | ✅ `brk(2)` / `sbrk(2)`                          | ⚠️ Partial — Win32 path in TODO-05 §9; Linux compat in §3 |
| 💎  | Process priority class                       | ✅ `SetPriorityClass` / `PROCESS_PRIORITY_*`     | ✅ `setpriority(2)` / `nice(2)`                  | ⬜ Planned — §4                                            |
| 💎  | Per-task scheduling policy                   | ✅ `REALTIME_PRIORITY_CLASS` → no time-slice     | ✅ `SCHED_FIFO` / `SCHED_IDLE` via `sched_setscheduler` | ⬜ Planned — §5                                  |
| 💎  | Process privilege / capability model         | ✅ Access tokens + privileges (`SeXxxPrivilege`) | ✅ POSIX capabilities (`CAP_*`)                  | ⬜ Planned — §6                                            |
| ⭐  | Drop-only capability inheritance             | ⚠️ Token elevation allows gaining privileges     | ⚠️ `setcap` can raise ambient capabilities       | ⬜ **Planned — §7 — monotonically decreasing, auditable** 🚀 |

> **After §1–§6:** Impossible OS matches Windows NT and Linux on all core per-process state APIs.
> **§7** enforces a strictly drop-only capability model — neither Windows (token elevation) nor Linux (ambient capabilities) provide this guarantee out of the box.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] New process CWD defaults to `C:\`; `NtSetCurrentDirectory("C:\\Impossible")` updates it; `NtQueryCurrentDirectory` returns the new value
- [ ] Relative path `"System\\Logs\\kernel.log"` resolves correctly against `task->cwd` in VFS
- [ ] Child process from `task_fork()` inherits parent CWD
- [ ] Newly created process has valid `STD_INPUT_HANDLE`, `STD_OUTPUT_HANDLE`, `STD_ERROR_HANDLE` in its handle table; `WriteFile(STD_OUTPUT_HANDLE, ...)` produces console output
- [ ] `sys_sbrk(4096)` returns old break; new page is writable; write/read round-trip succeeds
- [ ] `SetPriorityClass(REALTIME)` without `CAP_REALTIME` returns `STATUS_PRIVILEGE_NOT_HELD`
- [ ] `SetPriorityClass(PROCESS_PRIORITY_HIGH)` raises all thread `base_priority` values to `THREAD_PRIO_HIGH`
- [ ] `SCHED_POLICY_FIFO` task runs without quantum preemption until it yields
- [ ] `capability_check(CAP_RAW_IO)` returns success for system processes and failure for normal user processes
- [ ] `task_fork()` child has exactly the parent's capability set; `NtDropCapability` reduces it; subsequent `capability_check` fails for dropped cap
- [ ] Commit: `"kernel: task — process model extensions complete"`
