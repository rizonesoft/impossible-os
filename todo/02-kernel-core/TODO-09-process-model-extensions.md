# TODO-09 -- Process Model Extensions

> **Goal:** Extend the kernel process model with the per-process state fields and syscalls that don't belong to the scheduler, VMM, or Object Manager individually: current working directory, standard handle pre-wiring, user-mode program break (Linux compat heap), process priority classes mapped to Win32 `SetPriorityClass`, scheduling policy per-task (`SCHED_FIFO`/`SCHED_IDLE`), and a process capability/privilege bitmask. All of these hang off `struct task` and are needed before any non-trivial user-mode program can run correctly.

> [!IMPORTANT]
> **Current state:** `struct task` has `pid`, `state`, `rsp`, stacks, `name`, `parent_pid`, `exit_status`, `wait_pid`, and `signals`. Thread-level priority is fully implemented (`THREAD_PRIO_IDLE`…`THREAD_PRIO_REALTIME`, priority-aware scheduler, PI boosting). Missing from `struct task`: `cwd`, `capabilities`, `brk`/`program_break`, `sched_policy`, and process-class priority. Handle table infrastructure is in TODO-03. PEB `RTL_USER_PROCESS_PARAMETERS.Environment` pointer layout is in D02 T04 §2 and §5; per-process env arrays and argv are owned by D02 T14.

## Inputs

- [`include/kernel/sched/task.h`](../../include/kernel/sched/task.h) -- `struct task`, `struct thread`, priority constants
- [`src/kernel/sched/task.c`](../../src/kernel/sched/task.c) -- `task_create`, `task_fork`, `task_exec`, `task_exit`, scheduler loop
- [`src/kernel/fs/vfs.c`](../../src/kernel/fs/vfs.c) -- `vfs_open`, relative path lookup entry point
- [`src/kernel/mm/vmm.c`](../../src/kernel/mm/vmm.c) -- `vmm_map_page` (singular) for program-break page allocation; call in a loop for multi-page `brk` extensions
- → XREF: `TODO-03-object-manager.md §3` -- `HANDLE_TABLE` and `ObpAllocateHandle` / `ObpFreeHandle` provide the handle table; §9 (`NtClose` / `NtDuplicateObject`) is the handle release path
- → XREF: `TODO-04-peb-teb-user-abi.md §5` -- `RTL_USER_PROCESS_PARAMETERS.Environment` covers the environment block; `CurrentDirectory` field lives in `RTL_USER_PROCESS_PARAMETERS`
- → XREF: `TODO-14-environment-variables.md` (section 1) -- kernel environ and argv pointers on `struct task`; `env_copy()` when `NtCreateProcess` clones parent to child (see `TODO-05-native-api-ssdt.md` section 7)
- → XREF: `TODO-05-native-api-ssdt.md §9` -- `NtAllocateVirtualMemory` is the Win32-native heap path; `brk`/`sbrk` here is the Linux-compat path only
- → XREF: `TODO-05-native-api-ssdt.md §4` -- SSDT indices 0x0160–0x0167 reserved for Job Object syscalls
- → XREF: `TODO-07-time-filetime-management.md §6` -- `KeDelayExecutionThread` is the sleep implementation; `NtDelayExecution` syscall wiring belongs there
- → XREF: `TODO-08-binary-system.md §1` -- `exec_load()` dispatcher sets `brk` to end of BSS at load time
- → XREF: `03-memory-concurrency/TODO-05-scheduler-enhancement.md §4,§6` -- `SCHED_FIFO`/`SCHED_RR` classes (§4) and CPU affinity (§6) are implemented in the scheduler; §5 and §10 here define the process-level policy fields and `NtSetInformationProcess` API; actual scheduler loop changes are authoritative THERE
- → XREF: `TODO-17-kernel-security-hardening.md §1` -- NX/DEP is consumed by §11 per-process mitigation policy flags
- → XREF: `TODO-05-native-api-ssdt.md §10` -- `NtQueryInformationProcess` wires the syscall; §8 here adds the accounting fields that populate `ProcessTimes`, `ProcessIoCounters`, `ProcessVmCounters` responses
- → XREF: `TODO-11-security-reference-monitor.md §7` -- token duplication at process spawn; TODO-11 §7 hooks into `task_exec()` to attach a copy of the parent's ACCESS_TOKEN to the child task

## Outcome

- `struct task` carries `cwd[MAX_PATH]`, `sched_policy`, `priority_class`, `capabilities`, `program_break`, accounting fields (`kernel_time_ns`, `user_time_ns`, `io_read_count`, `io_write_count`, `page_fault_count`, `peak_working_set`), resource limits (`rlimits[RLIMIT_COUNT]`), `affinity_mask`, `mitigation_flags`, and `pledge_mask`/`unveil_paths`.
- VFS relative path resolution prepends `task->cwd` for any path without a drive-letter prefix.
- Every new process has `STD_INPUT_HANDLE`, `STD_OUTPUT_HANDLE`, and `STD_ERROR_HANDLE` wired into its handle table at creation (using the TODO-03 handle table infrastructure).
- `brk` / `sbrk` syscalls allocate user pages via VMM for Linux-compat user-mode `malloc`.
- `SetPriorityClass` maps process priority class (`IDLE`, `NORMAL`, `HIGH`, `REALTIME`) to the existing thread priority range; `NtSetInformationProcess(ProcessPriorityClass)` is the native entry point.
- Per-task `SCHED_POLICY_FIFO` and `SCHED_POLICY_IDLE` policies set the process-level scheduling hint; scheduler loop changes live in `03-memory-concurrency/TODO-05`.
- A `capabilities` bitmask on `struct task` gates privileged kernel operations; user processes receive a restricted default set; capabilities are inherited and can only be dropped, never gained.
- Process accounting fields populate `NtQueryInformationProcess` responses for `ProcessTimes`, `ProcessIoCounters`, and `ProcessVmCounters`.
- Per-process resource limits (`rlimit_t` soft/hard pairs) enforce `RLIMIT_AS`, `RLIMIT_NOFILE`, `RLIMIT_CPU`, `RLIMIT_STACK`, `RLIMIT_NPROC`, and `RLIMIT_FSIZE`.
- `NtPledge()` / `NtUnveil()` provide irreversible syscall-category restriction and filesystem visibility scoping -- simpler and stronger than seccomp or Capsicum.

## Implementation Order

| ⭐  | Order | Deliverable                                          | Depends On                    | Status |
| --- | :---: | ---------------------------------------------------- | ----------------------------- | :----: |
| 💎  |   1   | Working directory (`cwd` field + Nt/VFS wiring)      | VFS                           |  [ ]   |
| 💎  |   2   | Standard handle pre-wiring at process creation       | TODO-03 §3                    |  [ ]   |
| 💎  |   3   | User-mode program break (brk/sbrk Linux compat)      | VMM, TODO-08 §1               |  [ ]   |
| 💎  |   4   | Process priority class (Win32 `SetPriorityClass`)    | sched (exists)                |  [ ]   |
| 💎  |   5   | Per-task scheduling policy (`SCHED_FIFO`/`IDLE`)     | 4                             |  [ ]   |
| 💎  |   6   | Process capabilities and privilege bitmask           | --                            |  [ ]   |
| ⭐  |   7   | Capability inheritance and drop-only policy          | 6                             |  [ ]   |
| 💎  |   8   | Process accounting fields (times, I/O, VM counters)  | 1                             |  [ ]   |
| 💎  |   9   | Per-process resource limits (rlimits)                | 3, 6                          |  [ ]   |
| 💎  |  10   | CPU affinity per process                             | 4, TODO-05-sched §6           |  [ ]   |
| 💎  |  11   | Per-process mitigation policy                        | 6, TODO-17 §1                 |  [ ]   |
| ⭐  |  12   | Pledge/unveil-style process restriction              | 6, 7                          |  [ ]   |
| 💎  |  13   | Job Object syscalls wired to SSDT                    | 6, TODO-05 §4                 |  [ ]   |
| 💎  |  14   | Process exit cleanup -- release all per-process resources | §8, §9                   |  [ ]   |

> 💎 = parity -- Windows NT (tokens + priority classes + accounting + rlimits) and Linux (capabilities + scheduling + getrusage + rlimits) both provide these.
> ⭐ = exclusive -- strict drop-only inheritance and pledge/unveil-style restriction are more auditable than both Windows token elevation and Linux `setcap`.

---

## 1. Working Directory

`struct task` has no `cwd` field. All VFS paths are currently treated as absolute. Relative path resolution must be added before any shell navigation or portable app path handling works.

- [ ] Add `char cwd[MAX_PATH]` to `struct task`; initialize to `"C:\\"` at `task_init()` and `task_create()`
- [ ] Inherit CWD from parent at `task_fork()` -- copy the string
- [ ] In `task_exec()` / `exec_load()`: preserve CWD from the calling process (do not reset on exec)
- [ ] Add VFS relative-path resolver: if the path does not begin with a drive letter (`X:\`), prepend `task->cwd` before passing to VFS lookup
- [ ] `NtSetCurrentDirectory(UNICODE_STRING *path)`: validate path exists via VFS, update `task->cwd`
- [ ] `NtQueryCurrentDirectory(buffer, length)`: copy `task->cwd` to user buffer
- [ ] Register both in SSDT (→ XREF TODO-05 §4)
- [ ] Win32 wrappers: `SetCurrentDirectory` → `NtSetCurrentDirectory`; `GetCurrentDirectory` → `NtQueryCurrentDirectory`
- [ ] Commit: `"kernel: task -- working directory field and Nt API wiring"`

> [!NOTE]
> **Current-tree prerequisites and ownership:**
> - [ ] `struct task` still has no `cwd`; this requires edits in `include/kernel/sched/task.h` plus `task_init()`/`task_create()`/`task_fork()`/`task_exec()` paths in `src/kernel/sched/task.c`.
> - [ ] VFS currently expects drive-letter absolute paths (`X:\\...`) at parse/open boundaries in `src/kernel/fs/vfs.c`; relative path support needs resolver logic in this section before path consumers can pass relative strings.
> - [ ] `NtSetCurrentDirectory` / `NtQueryCurrentDirectory` are not wired in SSDT registration yet; add service numbers and table registration as part of this section and `TODO-05-native-api-ssdt.md §4`.
> - [ ] `MAX_PATH` is not a kernel-wide constant today; use or define a constant aligned with `VFS_MAX_PATH` to avoid path-size drift.
> - [ ] PEB process-parameter `CurrentDirectory` is currently hardcoded to `C:\\`; keep `RTL_USER_PROCESS_PARAMETERS.CurrentDirectory` synchronized with `task->cwd` (→ XREF `TODO-04-peb-teb-user-abi.md §5`).
> - [ ] Win32 wrappers are separate user-mode/API-surface work: `GetCurrentDirectoryW`/`SetCurrentDirectoryW` in `todo/05-storage-filesystems/TODO-05-win32-file-io-api.md §6`, and A-suffixed API exports in `todo/10-platform-services/TODO-08-win32-api-surface.md §2`.

**Test checkpoint:** New process `task->cwd` is `"C:\\"`. `NtSetCurrentDirectory("C:\\Impossible")` updates CWD; `NtQueryCurrentDirectory` returns `"C:\\Impossible"`. `NtSetCurrentDirectory` on non-existent path returns error (CWD unchanged). Relative path `"System\\Logs"` resolves to `"C:\\System\\Logs"` when CWD is `"C:\\"`. `task_fork()` child inherits parent CWD. `POST16(0xD010)` on entry, `POST16(0xD011)` after VFS resolver wired. Range `0xD01x` confirmed free. Test on: QEMU WHPX + TCG.

## 2. Standard Handle Pre-Wiring at Process Creation

When a process is created, `STD_INPUT_HANDLE` (0), `STD_OUTPUT_HANDLE` (1), and `STD_ERROR_HANDLE` (2) must already be open in its handle table. This requires TODO-03 §3 to be done first.

- [ ] Define `STD_INPUT_HANDLE`, `STD_OUTPUT_HANDLE`, `STD_ERROR_HANDLE` pseudo-handle constants in `include/kernel/nt/nt_types.h`
- [ ] At `task_create()` / `NtCreateProcess()`: allocate the process handle table (TODO-03 §3); wire the three standard handles:
  - STD_INPUT: terminal read end (initially the keyboard VFS node)
  - STD_OUTPUT / STD_ERROR: terminal write end (initially the framebuffer console VFS node)
- [ ] `GetStdHandle(nStdHandle)` → look up the pre-wired slot; `SetStdHandle(nStdHandle, handle)` → replace it
- [ ] `DuplicateHandle`: duplicate a handle slot into the child process at `NtCreateProcess` time for I/O redirection (`cmd > file` pipes the child stdout to a file handle before execution)
- [ ] Inherit standard handles into child at `task_fork()` if `HANDLE_FLAG_INHERIT` is set (TODO-03 §10)
- [ ] When `NtCreateProcess` gains a caller-supplied UTF-16 environment block, set `CREATE_UNICODE_ENVIRONMENT` in creation flags per Microsoft Learn ("Changing Environment Variables"); coordinate with D02 T14 §10 and D02 T05 §7 (distinct from firmware `NtQuerySystemEnvironmentValue*` at SSDT indices 0x00D2 through 0x00D6).
- [ ] Commit: `"kernel: task -- STD handle pre-wiring at process creation"`

**Test checkpoint:** After `task_create()`, `STD_INPUT_HANDLE`, `STD_OUTPUT_HANDLE`, `STD_ERROR_HANDLE` are valid handles in the process handle table. `WriteFile(STD_OUTPUT_HANDLE, ...)` produces console output. `DuplicateHandle` into child works for I/O redirection. `POST16(0xD020)` on entry, `POST16(0xD021)` after handles wired. Range `0xD02x` confirmed free. Test on: QEMU WHPX + TCG.

## 3. User-Mode Program Break (brk/sbrk -- Linux Compat)

Win32 programs use `NtAllocateVirtualMemory` (TODO-05 §9) for heap. Linux-compat programs call `brk(2)` / `sbrk(2)` which the kernel must handle by growing user-space pages.

> [!WARNING]
> `exec_load()` does not exist yet (→ TODO-08 §1, `[ ]`). Until it lands, initialize `program_break` to a fixed user-space address (e.g., end of the loaded ELF BSS in the existing `task_exec()` path) so brk/sbrk can be tested independently.

- [ ] Add `uint64_t program_break` to `struct task`; set to end of BSS segment in `exec_load()` / `task_exec()` (→ XREF TODO-08 §1)
- [ ] `sys_brk(addr)`: if `addr == 0`, return current break; if `addr > program_break`, allocate VMM pages to cover the new range; update `program_break`; reject addresses below BSS end
- [ ] `sys_sbrk(increment)`: return old break; advance by `increment` bytes via `sys_brk`
- [ ] Reject `increment < 0` if it would shrink below BSS end (simplification; full shrink support is optional)
- [ ] Add `SYS_BRK` to the Linux-compat syscall table (keep separate from SSDT -- POSIX compat only)
- [ ] Verify: user-mode `malloc` backed by a musl-style `sbrk` wrapper can allocate and free without crashing
- [ ] Commit: `"kernel: task -- program break (brk/sbrk) for Linux compat heap"`

**Test checkpoint:** `sys_brk(0)` returns current break (non-zero). `sys_sbrk(4096)` returns old break; new address is 4096 bytes higher; memory at new address is writable. `sys_sbrk` below BSS end rejected. Serial log shows `"task: brk extended to 0x<addr>"`. `POST16(0xD030)` on entry, `POST16(0xD031)` after VMM pages allocated. Range `0xD03x` confirmed free. Test on: QEMU WHPX + TCG.

## 4. Process Priority Class

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
- [ ] Register `NtSetInformationProcess` and `NtQueryInformationProcess` in SSDT (→ XREF `TODO-05 §4`)
- [ ] Commit: `"kernel: task -- process priority class and NtSetInformationProcess"`

**Test checkpoint:** `NtSetInformationProcess(ProcessPriorityClass, PROCESS_PRIORITY_HIGH)` updates all thread base priorities to `THREAD_PRIO_HIGH`. `PROCESS_PRIORITY_REALTIME` without `CAP_REALTIME` returns `STATUS_PRIVILEGE_NOT_HELD`. `NtQueryInformationProcess(ProcessPriorityClass)` returns current class. Serial log shows `"task: priority class set to <class>"`. `POST16(0xD040)` on entry, `POST16(0xD041)` after threads updated. Range `0xD04x` confirmed free. Test on: QEMU WHPX + TCG.

## 5. Per-Task Scheduling Policy

> [!NOTE]
> → XREF: `03-memory-concurrency/TODO-05-scheduler-enhancement.md §4` -- scope overlap: the scheduler TODO implements `SCHED_FIFO`/`SCHED_RR`/`SCHED_DEADLINE` classes with RT run queues, priority levels, and the actual `schedule()` loop changes. This section defines only the process-level `sched_policy` field on `struct task` and the `NtSetInformationProcess` API surface. Scheduler loop changes are authoritative in TODO-05-sched.

Add a per-task `sched_policy` field for tasks that need non-time-sliced execution.

- [ ] Add `uint8_t sched_policy` to `struct task` with values:
  - `SCHED_POLICY_NORMAL = 0` -- default round-robin with `SCHED_QUANTUM` time slice
  - `SCHED_POLICY_FIFO   = 1` -- run until block or yield; no preemptive time-slice; requires `CAP_SCHED_FIFO`
  - `SCHED_POLICY_IDLE   = 2` -- only scheduled when no `SCHED_POLICY_NORMAL` or `SCHED_POLICY_FIFO` task is runnable
- [ ] Update `schedule()` in `task.c`: skip `SCHED_POLICY_FIFO` tasks for quantum-based preemption; skip `SCHED_POLICY_IDLE` tasks when higher-policy tasks are runnable
- [ ] `NtSetInformationProcess(ProcessHandle, ProcessSchedulingPolicy, ...)`: validate `CAP_SCHED_FIFO` for FIFO; update field
- [ ] Kernel-internal convenience: `task_set_sched_policy(pid, SCHED_POLICY_FIFO)` callable from boot paths
- [ ] `NtSetInformationProcess(ProcessSchedulingPolicy)` registered in SSDT shares the entry with §4 (→ XREF `TODO-05 §4`)
- [ ] Commit: `"kernel: sched -- per-task scheduling policy (FIFO, IDLE)"`

**Test checkpoint:** `SCHED_POLICY_FIFO` task runs without quantum preemption until yield. `SCHED_POLICY_IDLE` task only runs when no NORMAL/FIFO tasks are runnable. `SCHED_POLICY_FIFO` without `CAP_SCHED_FIFO` returns `STATUS_PRIVILEGE_NOT_HELD`. Serial log shows `"sched: policy set to FIFO for pid <N>"`. `POST16(0xD050)` on entry, `POST16(0xD051)` after field set, `POST16(0xD052)` after `schedule()` modification verified. Range `0xD05x` confirmed free. If crash, check last POST -- 0xD050 = never entered, 0xD052 = schedule() modification broke quantum preemption. Test on: QEMU WHPX + TCG. Verify on bare metal -- scheduler changes may expose timing differences.

## 6. Process Capabilities and Privilege Bitmask

Every process carries a `capabilities` bitmask. Privileged syscalls check it before executing. Capabilities flow from parent to child and can only be dropped.

- [ ] Define capability constants in `include/kernel/security/capabilities.h`:
  - `CAP_SYS_ADMIN       (1ULL << 0)` -- general admin (mount, kmod, etc.)
  - `CAP_RAW_IO          (1ULL << 1)` -- direct disk / port I/O
  - `CAP_NET_ADMIN       (1ULL << 2)` -- raw socket / network config
  - `CAP_LOAD_DRIVER     (1ULL << 3)` -- load kernel-mode drivers
  - `CAP_KILL_ALL        (1ULL << 4)` -- signal any process
  - `CAP_SET_TIME        (1ULL << 5)` -- call `NtSetSystemTime` (→ XREF TODO-07 §9)
  - `CAP_REALTIME        (1ULL << 6)` -- set `PROCESS_PRIORITY_REALTIME`
  - `CAP_SCHED_FIFO      (1ULL << 7)` -- use `SCHED_POLICY_FIFO`
  - `CAP_DEBUG           (1ULL << 8)` -- attach debugger to another process
- [ ] Add `uint64_t capabilities` to `struct task`; system processes get `CAP_ALL = ~0ULL` at kernel init
- [ ] `CAP_DEFAULT_USER`: mask granting no privileged capabilities to user processes
- [ ] `capability_check(uint64_t cap)` -- returns `STATUS_SUCCESS` if `task_current()->capabilities & cap`, else `STATUS_PRIVILEGE_NOT_HELD`
- [ ] Gate privileged paths: `CAP_SET_TIME` in `KeSetSystemTime`, `CAP_RAW_IO` in raw disk syscalls, `CAP_LOAD_DRIVER` in driver load path, `CAP_REALTIME` in priority-class enforcement
- [ ] Commit: `"kernel: security -- process capabilities bitmask"`

**Test checkpoint:** System process has `CAP_ALL`; `capability_check(CAP_RAW_IO)` returns `STATUS_SUCCESS`. User process has `CAP_DEFAULT_USER`; `capability_check(CAP_RAW_IO)` returns `STATUS_PRIVILEGE_NOT_HELD`. `CAP_SET_TIME` gates `KeSetSystemTime`. Serial log shows `"security: capability check -- cap=<N> result=<status>"`. Test on: QEMU WHPX + TCG.

## 7. Capability Inheritance and Drop-Only Policy

Capabilities can be inherited across `fork` / `exec` but can only be dropped, never gained. This makes privilege de-escalation auditable and prevents accidental escalation.

- [ ] At `task_fork()`: child inherits parent `capabilities` exactly
- [ ] At `task_exec()` via `exec_load()`: on EIF binaries, apply capability mask from EIF header flags; strip any bits not in the parent's set; never add new capabilities on exec (→ XREF `TODO-08 §4` -- EIF capabilities field is not yet defined in the spec; extend `eif_header_t` there before implementing here)
- [ ] `NtDropCapability(cap_mask)`: clear one or more capability bits from the calling process; irreversible for the lifetime of the process; register in SSDT (→ XREF `TODO-05 §4`)
- [ ] No syscall or path grants new capabilities -- escalation requires a restart or a privileged parent spawning with a specific mask
- [ ] Document the invariant: `child->capabilities ⊆ parent->capabilities` is enforced at fork and exec
- [ ] Commit: `"kernel: security -- capability inheritance and drop-only policy"`

**Test checkpoint:** `task_fork()` child inherits exact parent capabilities. `NtDropCapability(CAP_RAW_IO)` clears the bit; subsequent `capability_check(CAP_RAW_IO)` fails. Child drop does not affect parent. `NtDropCapability` is irreversible -- re-granting returns error. Serial log shows `"security: capability dropped -- cap=<N> remaining=0x<mask>"`. Test on: QEMU WHPX + TCG.

## 8. Process Accounting Fields (Times, I/O Counters, VM Counters)

Both Win11 (`NtQueryInformationProcess` with `ProcessTimes`, `ProcessIoCounters`, `ProcessVmCounters`) and Linux (`getrusage`, `times(2)`, `/proc/[pid]/stat`) track per-process resource usage. These fields must exist in `struct task` before TODO-05 §10 can return meaningful data.

- [ ] Add to `struct task`:
  - `uint64_t create_time_ns` -- set once at `task_create()` from `uptime_ns()`
  - `uint64_t user_time_ns` -- accumulated in scheduler tick handler when running in ring 3
  - `uint64_t kernel_time_ns` -- accumulated in scheduler tick handler when running in ring 0
  - `uint64_t io_read_count`, `io_read_bytes` -- incremented in VFS read path
  - `uint64_t io_write_count`, `io_write_bytes` -- incremented in VFS write path
  - `uint64_t page_fault_count` -- incremented in `#PF` handler
  - `uint64_t peak_working_set` -- updated on page allocation; tracks high-water mark
- [ ] In the scheduler tick ISR: determine ring from saved CS on interrupt frame; add tick duration to `user_time_ns` or `kernel_time_ns`
- [ ] In VFS `vfs_read()` / `vfs_write()`: increment `io_read_count`/`io_write_count` and byte counters on the current task
- [ ] In `#PF` handler: increment `task_current()->page_fault_count`
- [ ] `getrusage(RUSAGE_SELF)` Linux-compat wrapper: populate `struct rusage` from task accounting fields
- [ ] Commit: `"kernel: task -- process accounting fields for times, I/O, and VM counters"`

**Test checkpoint:** After running a process, `NtQueryInformationProcess(ProcessTimes)` returns non-zero `KernelTime` and `UserTime`. `ProcessVmCounters` returns non-zero `PageFaultCount`. `ProcessIoCounters` returns non-zero `ReadOperationCount` after a file read. Serial log shows `"task: accounting -- user=<N>ns kernel=<M>ns faults=<F>"` at process exit. `POST16(0xD080)` on entry, `POST16(0xD081)` after struct fields added, `POST16(0xD082)` after scheduler tick ISR instrumented, `POST16(0xD083)` after `#PF` handler instrumented. Range `0xD08x` confirmed free. If crash at 0xD082: scheduler tick ISR modification broke -- revert ISR change and fall back to un-instrumented tick. Test on: QEMU WHPX + TCG. Verify on bare metal -- ISR timing may differ.

## 9. Per-Process Resource Limits (rlimits)

Both Win11 (Job Object quotas + `QUOTA_LIMITS` via `NtQueryInformationProcess`) and Linux (`getrlimit`/`setrlimit`/`prlimit`) enforce per-process resource limits. This prevents runaway processes from exhausting system resources.

- [ ] Define `rlimit_t` in `include/kernel/task_limits.h`: `{ uint64_t rlim_cur; uint64_t rlim_max; }` with `RLIM_INFINITY = UINT64_MAX`
- [ ] Define limit indices: `RLIMIT_AS` (address space), `RLIMIT_NOFILE` (open files -- coordinates with TODO-03 §14), `RLIMIT_CPU` (CPU seconds), `RLIMIT_STACK` (stack size), `RLIMIT_NPROC` (child processes), `RLIMIT_FSIZE` (file write size), `RLIMIT_COUNT`
- [ ] Add `rlimit_t rlimits[RLIMIT_COUNT]` to `struct task`; populate with sane defaults at `task_create()` (e.g., `RLIMIT_NOFILE.rlim_cur = 256`, `RLIMIT_AS.rlim_cur = RLIM_INFINITY`)
- [ ] Inherit rlimits from parent at `task_fork()`
- [ ] `sys_getrlimit(resource, &rlimit)` / `sys_setrlimit(resource, &rlimit)`: unprivileged process can lower `rlim_max` (irreversible) or set `rlim_cur` within `[0, rlim_max]`; raising `rlim_max` requires `CAP_SYS_ADMIN`
- [ ] `sys_prlimit(pid, resource, new, old)`: get/set limits for another process (requires `CAP_SYS_ADMIN` if pid != self)
- [ ] Enforce `RLIMIT_AS` in VMM `vmm_map_page()` / `sys_brk()`: reject if total mapped pages would exceed limit
- [ ] Enforce `RLIMIT_CPU`: in scheduler tick, check `user_time_ns + kernel_time_ns > rlim_cur * 1e9`; send `SIGXCPU` (or terminate) if exceeded
- [ ] Register as Linux-compat syscalls; `NtQueryInformationProcess(ProcessQuotaLimits)` returns `QUOTA_LIMITS_EX` populated from rlimits
- [ ] Commit: `"kernel: task -- per-process resource limits (rlimits)"`

**Test checkpoint:** `sys_setrlimit(RLIMIT_AS, 64MB)` then `sys_brk()` beyond 64 MB returns `ENOMEM`. `sys_setrlimit(RLIMIT_CPU, 2)` causes process termination after 2 seconds of CPU time. Unprivileged `sys_setrlimit` raising `rlim_max` returns `EPERM`. Serial log shows `"task: rlimit RLIMIT_AS enforced -- rejected allocation"`. `POST16(0xD090)` on entry, `POST16(0xD091)` after struct/defaults set, `POST16(0xD092)` after `vmm_map_page()` enforcement wired. Range `0xD09x` confirmed free. If crash at 0xD092: VMM enforcement check broke page allocation -- revert the `vmm_map_page` guard. Test on: QEMU WHPX + TCG.

## 10. CPU Affinity per Process

> [!NOTE]
> → XREF: `03-memory-concurrency/TODO-05-scheduler-enhancement.md §6` -- thread-level CPU affinity (`affinity_mask` in `task_t`, `NtSetInformationThread(ThreadAffinityMask)`) is implemented there. This section adds the process-level API: `SetProcessAffinityMask` / `NtSetInformationProcess(ProcessAffinityMask)` which sets the affinity for all threads in the process.

> [!WARNING]
> Thread-level `affinity_mask` field on `struct thread` does not exist yet (→ TODO-05-sched §6, `[ ]`). If TODO-05-sched §6 has not landed, add `uint64_t affinity_mask` to `struct thread` here with a default of `~0ULL` (all CPUs). The scheduler does not need to consume the mask until TODO-05-sched §6 adds the per-CPU run queue logic.

- [ ] Add `uint64_t affinity_mask` to `struct thread` if not already present from TODO-05-sched §6 (default `~0ULL`)
- [ ] `NtSetInformationProcess(ProcessHandle, ProcessAffinityMask, &mask, sizeof(mask))`: iterate all threads in the process; set each thread's `affinity_mask` to the intersection of the new process mask and the thread's current mask; store process-level mask in `struct task`
- [ ] `NtQueryInformationProcess(ProcessHandle, ProcessAffinityMask, ...)`: return the process-level mask
- [ ] Win32 wrappers: `SetProcessAffinityMask(hProcess, dwMask)` → `NtSetInformationProcess`; `GetProcessAffinityMask(hProcess, &procMask, &sysMask)` → returns process mask and system mask (all CPUs)
- [ ] New threads inherit the process affinity mask at creation
- [ ] Commit: `"kernel: task -- per-process CPU affinity (SetProcessAffinityMask)"`

**Test checkpoint:** `SetProcessAffinityMask(current, 0x3)` restricts process to CPUs 0-1. `GetProcessAffinityMask` returns `0x3`. New thread created after affinity change has `affinity_mask = 0x3`. Serial log shows `"task: process affinity set to 0x<mask>"`. Test on: QEMU WHPX + TCG (2+ vCPUs). Verify on bare metal -- SMP affinity behavior differs.

## 11. Per-Process Mitigation Policy

Win11 provides `SetProcessMitigationPolicy` to control per-process security features: DEP enforcement mode, mandatory ASLR, CFG strictness, child process creation restrictions, image load restrictions. Linux uses `prctl` with `PR_SET_NO_NEW_PRIVS`, `PR_SET_SECCOMP`, etc. Impossible OS needs a unified per-process mitigation flags field that coordinates with the security features implemented in other TODOs.

> [!NOTE]
> → XREF: `TODO-17-kernel-security-hardening.md §1` (NX/DEP), `TODO-08-binary-system.md §15` (ASLR), `TODO-08 §12` (CFG), `TODO-05-native-api-ssdt.md §25` (syscall filtering). This section defines the per-process flags and API surface; enforcement is authoritative in those TODOs.

- [ ] Add `uint64_t mitigation_flags` to `struct task` with bit definitions:
  - `MIT_DEP_ENABLE (1 << 0)` -- permanent DEP/NX for the process
  - `MIT_ASLR_FORCE (1 << 1)` -- force ASLR even for non-PIE binaries
  - `MIT_CFG_STRICT (1 << 2)` -- CFG strict mode (no suppressed exports)
  - `MIT_NO_CHILD_PROCESS (1 << 3)` -- process cannot create child processes
  - `MIT_NO_REMOTE_IMAGES (1 << 4)` -- process cannot load images from network paths
  - `MIT_NO_LOW_INTEGRITY_IMAGES (1 << 5)` -- reject low-integrity DLLs
  - `MIT_NO_NEW_PRIVS (1 << 6)` -- Linux `PR_SET_NO_NEW_PRIVS` equivalent; exec cannot gain capabilities
- [ ] `NtSetInformationProcess(ProcessHandle, ProcessMitigationPolicy, &policy, size)`: set mitigation bits; once set, bits cannot be cleared (monotonically increasing restriction)
- [ ] `NtQueryInformationProcess(ProcessHandle, ProcessMitigationPolicy, ...)`: return current flags
- [ ] Win32 wrapper: `SetProcessMitigationPolicy(MitigationType, &info, size)` → `NtSetInformationProcess`
- [ ] Enforce `MIT_NO_CHILD_PROCESS` in `NtCreateProcess` / `task_fork()` path
- [ ] Inherit mitigation flags from parent at `task_fork()` -- child gets at least the parent's flags
- [ ] Commit: `"kernel: task -- per-process mitigation policy flags"`

**Test checkpoint:** `SetProcessMitigationPolicy(DEP_ENABLE)` sets the bit; subsequent query returns it set. Attempting to clear the bit returns `STATUS_ACCESS_DENIED`. Process with `MIT_NO_CHILD_PROCESS` calling `NtCreateProcess` returns `STATUS_CHILD_PROCESS_BLOCKED`. Serial log shows `"task: mitigation flags updated: 0x<flags>"`. Test on: QEMU WHPX + TCG.

## 12. Pledge/Unveil-Style Process Restriction

> [!TIP]
> Neither Windows nor Linux provides a simple, auditable process restriction API. Windows has `SetProcessMitigationPolicy` (limited to security flags) and restricted tokens (complex). Linux has seccomp-bpf (requires writing BPF programs) and Landlock (filesystem only). OpenBSD's `pledge()` and `unveil()` are widely admired for their simplicity: a single syscall restricts what a process can do, irreversibly. Impossible OS provides both, unified with the capability model.

- [ ] `NtPledge(const char *promises)`: restrict the calling process to a set of named syscall categories. Categories: `"stdio"` (read/write/close), `"rpath"` (read-only file access), `"wpath"` (write file access), `"cpath"` (create/delete files), `"inet"` (network sockets), `"proc"` (fork/exec), `"exec"` (exec only), `"dns"` (DNS resolution), `"tty"` (terminal I/O). Once pledged, attempting a syscall outside the pledged set terminates the process with `STATUS_PLEDGE_VIOLATION`.
- [ ] Add `uint64_t pledge_mask` to `struct task`; `0` = not pledged (all allowed); non-zero = bitmask of allowed categories
- [ ] In SSDT dispatcher: if `task->pledge_mask != 0`, check if the invoked syscall's category bit is set; if not, terminate with `STATUS_PLEDGE_VIOLATION` and log the violation (→ XREF: `TODO-05-native-api-ssdt.md §25` -- per-index bitmap filter runs in the same dispatcher path; pledge category check runs AFTER the bitmap filter; both must pass for the syscall to proceed)
- [ ] `NtUnveil(const char *path, const char *permissions)`: restrict filesystem visibility. After the first `NtUnveil` call, only unveiled paths are accessible. `permissions` is a subset of `"rwxc"` (read, write, execute, create). Calling `NtUnveil(NULL, NULL)` locks the unveil set -- no further calls allowed.
- [ ] Add `unveil_entry_t *unveil_list` to `struct task`; VFS path resolution checks against this list if non-NULL
- [ ] Both `NtPledge` and `NtUnveil` are irreversible -- once applied, restrictions can only be tightened, never loosened
- [ ] Register both in SSDT (→ XREF TODO-05 §4)
- [ ] Commit: `"kernel: task -- pledge/unveil process restriction (OpenBSD-inspired)"`

**Test checkpoint:** After `NtPledge("stdio rpath")`, calling `NtCreateFile` for write returns `STATUS_PLEDGE_VIOLATION` and process terminates. After `NtUnveil("/C:\\Impossible\\System", "rx")`, reading from `C:\Impossible\System\shell.exe` succeeds; reading from `C:\Users\` returns `STATUS_ACCESS_DENIED`. Serial log shows `"task: pledge violation -- syscall <N> not in pledge set"`. `POST16(0xD0C0)` on entry, `POST16(0xD0C1)` after pledge_mask set, `POST16(0xD0C2)` after SSDT dispatcher check wired. Range `0xD0Cx` confirmed free. If crash at 0xD0C2: SSDT dispatcher modification broke -- revert the dispatch check and fall back to un-pledged execution. Test on: QEMU WHPX + TCG.

---

## 13. Job Object Syscalls Wired to SSDT

Register Job Object management syscalls in the SSDT for process-group resource control. Win11 Job Objects are the primary mechanism for process-group resource limits (CPU rate, memory cap, I/O throttle). Linux uses cgroups v2 for equivalent functionality. (→ XREF: TODO-05-native-api-ssdt.md §4)

- [ ] `NtCreateJobObject(JobHandle, DesiredAccess, ObjectAttributes)` → SSDT 0x0160
- [ ] `NtOpenJobObject(JobHandle, DesiredAccess, ObjectAttributes)` → SSDT 0x0161
- [ ] `NtAssignProcessToJobObject(JobHandle, ProcessHandle)` → SSDT 0x0162
- [ ] `NtTerminateJobObject(JobHandle, ExitStatus)` → SSDT 0x0163
- [ ] `NtQueryInformationJobObject(JobHandle, InfoClass, Buffer, Length, RetLen)` → SSDT 0x0164
- [ ] `NtSetInformationJobObject(JobHandle, InfoClass, Buffer, Length)` → SSDT 0x0165
- [ ] `NtIsProcessInJob(ProcessHandle, JobHandle)` → SSDT 0x0166
- [ ] `NtCreateJobSet(NumJob, UserJobSet, Flags)` → SSDT 0x0167
- [ ] All functions return `NTSTATUS`
- [ ] Commit: `"kernel: wire Job Object syscalls to SSDT (0x0160–0x0167)"`

**Test checkpoint:** `NtCreateJobObject` creates a job. `NtAssignProcessToJobObject` assigns a child process. `NtQueryInformationJobObject` returns accounting data. `NtTerminateJobObject` kills all processes in the job. Serial log shows `"job: created job <handle>, assigned pid <N>"`. Test on: QEMU WHPX + TCG.

## 14. Process Exit Cleanup

Central cleanup point for all per-process resources when a process terminates. Windows calls this from `PspExitProcess()` -- it walks every subsystem that holds per-process state and releases it. Without this, resources leak on process exit.

- [ ] In `task_exit()` (or a new `process_cleanup(struct task *t)` called from it):
  - Release timer resolution requests held by this PID (-> XREF: TODO-07 §8 `KeSetTimerResolution`)
  - Close all open handles in the process handle table (-> XREF: TODO-03 §3 OB handle table)
  - Release all byte-range locks held by this process (-> XREF: 05-storage-filesystems/TODO-04 §10 `vfs_lock_file`)
  - Release all share-mode handle entries for open files (-> XREF: 05-storage-filesystems/TODO-04 §8 `vfs_open_handle_t`)
  - Trigger delete-on-close for files marked by this process (-> XREF: 05-storage-filesystems/TODO-04 §9)
  - Release any oplock held by this process (-> XREF: 05-storage-filesystems/TODO-04 §14)
  - Free per-process memory: PEB, TEB, user stack, address space (-> XREF: 02-kernel-core/TODO-04 §5)
  - Release per-process resource limits and accounting (-> XREF: §8, §9 of this TODO)
  - Remove from job object if assigned (-> XREF: §13)
- [ ] Log: `klog(LOG_DEBUG, "task", "PID %u exit cleanup: %u handles, %u locks released", ...)`
- [ ] Commit: `"kernel: task -- process exit cleanup (handles, locks, timer res, memory)"`

**Test checkpoint:** Create a process that opens files with locks + timer resolution request. Kill the process. Verify: all locks released, timer resolution reverts to default, handles closed, no resource leak. Serial log shows cleanup counts. Test on QEMU WHPX, TCG.

---

## OS Comparison

| ⭐ | Feature                       | 🪟 Win11                       | 🐧 Linux                        | 🚀 Impossible OS                 |
|----|-------------------------------|-----------------------------|------------------------------|-------------------------------|
| 💎 | Per-process CWD               | ✅ SetCurrentDirectory      | ✅ chdir / getcwd            | ⬜ §1                        |
| 💎 | STD handle pre-wiring         | ✅ CreateProcess inherit     | ✅ fd 0/1/2 via fork         | ⬜ §2                        |
| 💎 | User-mode heap (brk)          | ✅ NtAllocateVirtualMemory  | ✅ brk / sbrk                | ⬜ §3                        |
| 💎 | Process priority class        | ✅ SetPriorityClass          | ✅ nice / setpriority        | ⬜ §4                        |
| 💎 | Scheduling policy             | ✅ REALTIME_PRIORITY_CLASS  | ✅ SCHED_FIFO / SCHED_IDLE   | ⬜ §5                        |
| 💎 | Capability / privilege model  | ✅ Access tokens             | ✅ POSIX capabilities        | ⬜ §6                        |
| 💎 | Process accounting            | ✅ ProcessTimes + IoCounters | ✅ getrusage / times         | ⬜ §8                        |
| 💎 | Per-process resource limits   | ✅ Job Object quotas         | ✅ getrlimit / setrlimit     | ⬜ §9                        |
| 💎 | Process CPU affinity          | ✅ SetProcessAffinityMask   | ✅ sched_setaffinity          | ⬜ §10                       |
| 💎 | Per-process mitigation policy | ✅ SetProcessMitigationPolicy| ⚠️ prctl + seccomp           | ⬜ §11                       |
| 💎 | Job Objects / cgroups         | ✅ NtCreateJobObject         | ✅ cgroups v2                | ⬜ §13                       |
| 💎 | Process exit cleanup          | ✅ PspExitProcess            | ✅ do_exit + __put_task      | ⬜ §14                       |
| 💎 | Per-process I/O priority      | ✅ ProcessIoPriority          | ✅ ioprio_set/get             | ⬜ Deferred (→ TODO-05 §10)  |
| ⭐ | Drop-only cap inheritance     | ⚠️ Token elevation           | ⚠️ setcap raises ambient     | ⬜ §7 -- monotonic decrease   |
| ⭐ | Pledge/unveil restriction     | ❌ None                      | ❌ No simple equivalent      | ⬜ §12 -- OpenBSD-inspired    |

> **After §1–§6:** Impossible OS matches Windows NT and Linux on all core per-process state APIs.
> **§7** enforces a strictly drop-only capability model -- neither Windows (token elevation) nor Linux (ambient capabilities) provide this guarantee out of the box.
> **§8–§10** close parity gaps: process accounting, resource limits, and CPU affinity are foundational for `NtQueryInformationProcess`, `getrusage`, and performance-critical workloads.
> **§11** unifies Win11 `SetProcessMitigationPolicy` and Linux `prctl` under a single per-process flags field.
> **§12** is the most auditable process restriction API in any production OS -- simpler than seccomp, stronger than Capsicum.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_proc_ext()` -- register in `src/kernel/test/test_runner.c`.
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_proc_ext.c` with:
  - New process `task->cwd` defaults to `"C:\\"` after `task_create()`
  - `NtSetCurrentDirectory("C:\\Impossible")` updates `task->cwd`; `NtQueryCurrentDirectory` returns `"C:\\Impossible"`
  - `NtSetCurrentDirectory` on non-existent path returns error (CWD unchanged)
  - Relative path `"System\\Logs"` resolves to `"C:\\System\\Logs"` when CWD is `"C:\\"`
  - `task_fork()` child inherits parent CWD string exactly
  - `STD_INPUT_HANDLE`, `STD_OUTPUT_HANDLE`, `STD_ERROR_HANDLE` are valid handles after process creation
  - `sys_brk(0)` returns current program break (non-zero, above BSS end)
  - `sys_sbrk(4096)` returns old break; new address is 4096 bytes higher; memory is writable
  - `sys_sbrk` with negative increment that would shrink below BSS end is rejected
  - `PROCESS_PRIORITY_NORMAL` maps thread base to `THREAD_PRIO_NORMAL`
  - `PROCESS_PRIORITY_HIGH` maps thread base to `THREAD_PRIO_HIGH`
  - `PROCESS_PRIORITY_REALTIME` without `CAP_REALTIME` returns `STATUS_PRIVILEGE_NOT_HELD`
  - `SCHED_POLICY_FIFO` without `CAP_SCHED_FIFO` returns `STATUS_PRIVILEGE_NOT_HELD`
  - System process has `CAP_ALL`; `capability_check(CAP_RAW_IO)` returns `STATUS_SUCCESS`
  - User process has `CAP_DEFAULT_USER`; `capability_check(CAP_RAW_IO)` returns `STATUS_PRIVILEGE_NOT_HELD`
  - `NtDropCapability(CAP_RAW_IO)` clears the bit; subsequent `capability_check(CAP_RAW_IO)` fails
  - `task_fork()` child inherits exact capability set; child drop does not affect parent
  - After CPU work, `NtQueryInformationProcess(ProcessTimes)` returns `KernelTime > 0` (§8)
  - After VFS read, `ProcessIoCounters.ReadOperationCount > 0` (§8)
  - `sys_setrlimit(RLIMIT_AS, {64MB, 64MB})` then `sys_brk` beyond 64 MB returns `ENOMEM` (§9)
  - `sys_setrlimit` raising `rlim_max` without `CAP_SYS_ADMIN` returns `EPERM` (§9)
  - `SetProcessAffinityMask(current, 0x1)` restricts to CPU 0; new thread inherits mask (§10)
  - `SetProcessMitigationPolicy(MIT_NO_CHILD_PROCESS)` blocks subsequent `NtCreateProcess` (§11)
  - Mitigation bit once set cannot be cleared -- returns `STATUS_ACCESS_DENIED` (§11)
  - `NtPledge("stdio rpath")` then `NtCreateFile(WRITE)` terminates with `STATUS_PLEDGE_VIOLATION` (§12)
  - `NtUnveil("/C:\\Tmp", "rw")` then `NtUnveil(NULL, NULL)` locks; access to `/C:\\Users` fails (§12)
  - `NtCreateJobObject` creates a job; `NtAssignProcessToJobObject` assigns child (§13)
- [ ] Register in `test_runner_init()`: `test_register_proc_ext()`
- [ ] Commit: `"test: add process model extensions test suite"`

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
- [ ] `NtQueryInformationProcess(ProcessTimes)` returns non-zero `KernelTime` and `UserTime` after process runs
- [ ] `NtQueryInformationProcess(ProcessIoCounters)` returns non-zero `ReadOperationCount` after file read
- [ ] `sys_setrlimit(RLIMIT_AS, 64MB)` then `sys_brk()` beyond 64 MB returns `ENOMEM`
- [ ] `SetProcessAffinityMask(current, 0x1)` restricts to CPU 0; `GetProcessAffinityMask` returns `0x1`
- [ ] `SetProcessMitigationPolicy(MIT_NO_CHILD_PROCESS)` then `NtCreateProcess` returns `STATUS_CHILD_PROCESS_BLOCKED`
- [ ] After `NtPledge("stdio rpath")`, write syscall terminates process with `STATUS_PLEDGE_VIOLATION`
- [ ] After `NtUnveil("/C:\\System", "rx")` and lock, access to `C:\Users` returns `STATUS_ACCESS_DENIED`
- [ ] `NtCreateJobObject` creates a job; `NtAssignProcessToJobObject` assigns a child; `NtTerminateJobObject` kills all
- [ ] Commit: `"kernel: task -- process model extensions complete"`
