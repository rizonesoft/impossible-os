---
schema_version: 1
id: process-model-extensions
domain: 02-kernel-core
status: active
title: "TODO-21 -- Process Model Extensions"
---

# TODO-21 -- Process Model Extensions

> **Validated:** 2026-07-10 | validate-todo-file clean (structure / IO table / XREF / test wiring); normalized Depends On column to `§`-prefix + same-domain `T` shorthand

> **Gap-audited:** 2026-07-10 | gap-audit-todo + mandatory codex-gap-audit (needs-attention, 4 findings, all confirmed via receiving-code-review). Filed 3 new sections: §15 parenting/reaping/wait4-waitid, §16 Protected Process Light (PS_PROTECTION), §17 process groups/sessions. Branch-A: §8 fault-split + ctx-switch + RUSAGE_CHILDREN counters, §9 RLIMIT_CORE/RLIMIT_MEMLOCK, §11 Win11-subset note + CET/signature XREFs. uid/gid kept out (owned TODO-10/TODO-15)

> **Goal:** Extend the kernel process model with the per-process state fields and syscalls that don't belong to the scheduler, VMM, or Object Manager individually: current working directory, standard handle pre-wiring, user-mode program break (Linux compat heap), process priority classes mapped to Win32 `SetPriorityClass`, scheduling policy per-task (`SCHED_FIFO`/`SCHED_IDLE`), and a process capability/privilege bitmask. All of these hang off `struct task` and are needed before any non-trivial user-mode program can run correctly.

> [!IMPORTANT]
> **Current state:** `struct task` has `pid`, `state`, `rsp`, stacks, `name`, `parent_pid`, `exit_status`, `wait_pid`, and `signals`. Thread-level priority is fully implemented (`THREAD_PRIO_IDLE`…`THREAD_PRIO_REALTIME`, priority-aware scheduler, PI boosting). Missing from `struct task`: `cwd`, `capabilities`, `brk`/`program_break`, `sched_policy`, and process-class priority. Handle table infrastructure is in TODO-05. PEB `RTL_USER_PROCESS_PARAMETERS.Environment` pointer layout is in D02 T11 §4 and §5; per-process env arrays and argv are owned by D02 T22.

## Inputs

- [`include/kernel/sched/task.h`](../../include/kernel/sched/task.h) -- `struct task`, `struct thread`, priority constants
- [`src/kernel/sched/task.c`](../../src/kernel/sched/task.c) -- `task_create`, `task_fork`, `task_exec`, `task_exit`, scheduler loop
- [`src/kernel/fs/vfs.c`](../../src/kernel/fs/vfs.c) -- `vfs_open`, relative path lookup entry point
- [`src/kernel/mm/vmm.c`](../../src/kernel/mm/vmm.c) -- `vmm_map_page` (singular) for program-break page allocation; call in a loop for multi-page `brk` extensions
- → XREF: `TODO-05-object-manager.md §3` -- `HANDLE_TABLE` and `ObpAllocateHandle` / `ObpFreeHandle` provide the handle table; §9 (`NtClose` / `NtDuplicateObject`) is the handle release path
- → XREF: `TODO-11-peb-teb-user-abi.md §2` -- `RTL_USER_PROCESS_PARAMETERS` struct (incl. `CurrentDirectory` + `Environment`); populated at `task_exec` in §5
- → XREF: `TODO-22-environment-variables.md` (section 1) -- kernel environ and argv pointers on `struct task`; `env_copy()` when `NtCreateProcess` clones parent to child (see `TODO-12-native-api-ssdt.md` section 7)
- → XREF: `TODO-12-native-api-ssdt.md §9` -- `NtAllocateVirtualMemory` is the Win32-native heap path; `brk`/`sbrk` here is the Linux-compat path only
- → XREF: `TODO-12-native-api-ssdt.md §5` -- SSDT indices 0x0160–0x0167 reserved for Job Object syscalls
- → XREF: `TODO-08-time-filetime-management.md §6` -- `KeDelayExecutionThread` is the sleep implementation; `NtDelayExecution` syscall wiring belongs there
- → XREF: `TODO-17-binary-system.md §5` -- `exec_load()` dispatcher sets `brk` to end of BSS at load time
- → XREF: `03-memory-concurrency/TODO-06-scheduler-enhancement.md §4,§6` -- `SCHED_FIFO`/`SCHED_RR` classes (§4) and CPU affinity (§6) are implemented in the scheduler; §5 and §10 here define the process-level policy fields and `NtSetInformationProcess` API; actual scheduler loop changes are authoritative THERE
- → XREF: `TODO-10-kernel-security-hardening.md §1` -- NX/DEP is consumed by §13 per-process mitigation policy flags
- → XREF: `TODO-12-native-api-ssdt.md §10` -- `NtQueryInformationProcess` wires the syscall; §7 here adds the accounting fields that populate `ProcessTimes`, `ProcessIoCounters`, `ProcessVmCounters` responses
- → XREF: `TODO-15-security-reference-monitor.md §7` -- token duplication at process spawn; §7 (Process/Thread Token Assignment) attaches a copy of the parent's ACCESS_TOKEN to the child at `task_exec`
- → XREF: `TODO-10-kernel-security-hardening.md` -- uid/gid are NOT owned here: `getuid`/`geteuid` live there (replace the hardcoded 1000 with a token-derived id) and `setuid`/`setgid` are token transitions owned by `TODO-15-security-reference-monitor.md` SRM + Linux adapters in TODO-10. No uid field/section belongs in this TODO

## Outcome

- `struct task` carries `cwd[MAX_PATH]`, `sched_policy`, `priority_class`, `capabilities`, `program_break`, accounting fields (`kernel_time_ns`, `user_time_ns`, `io_read_count`, `io_write_count`, `page_fault_count`, `peak_working_set`), resource limits (`rlimits[RLIMIT_COUNT]`), `affinity_mask`, `mitigation_flags`, and `pledge_mask`/`unveil_paths`.
- VFS relative path resolution prepends `task->cwd` for any path without a drive-letter prefix.
- Every new process has `STD_INPUT_HANDLE`, `STD_OUTPUT_HANDLE`, and `STD_ERROR_HANDLE` wired into its handle table at creation (using the TODO-05 handle table infrastructure).
- `brk` / `sbrk` syscalls allocate user pages via VMM for Linux-compat user-mode `malloc`.
- `SetPriorityClass` maps process priority class (`IDLE`, `NORMAL`, `HIGH`, `REALTIME`) to the existing thread priority range; `NtSetInformationProcess(ProcessPriorityClass)` is the native entry point.
- Per-task `SCHED_POLICY_FIFO` and `SCHED_POLICY_IDLE` policies set the process-level scheduling hint; scheduler loop changes live in `03-memory-concurrency/TODO-06-scheduler-enhancement.md`.
- A `capabilities` bitmask on `struct task` gates privileged kernel operations; user processes receive a restricted default set; capabilities are inherited and can only be dropped, never gained.
- Process accounting fields populate `NtQueryInformationProcess` responses for `ProcessTimes`, `ProcessIoCounters`, and `ProcessVmCounters`.
- Per-process resource limits (`rlimit_t` soft/hard pairs) enforce `RLIMIT_AS`, `RLIMIT_NOFILE`, `RLIMIT_CPU`, `RLIMIT_STACK`, `RLIMIT_NPROC`, and `RLIMIT_FSIZE`.
- `NtPledge()` / `NtUnveil()` provide irreversible syscall-category restriction and filesystem visibility scoping -- simpler and stronger than seccomp or Capsicum.

## Implementation Order

| ⭐   | Order | Deliverable                                               | Depends On     | Status |
| --- | :---: | --------------------------------------------------------- | -------------- | :----: |
| 💎   |   1   | Working directory (`cwd` field + Nt/VFS wiring)           | VFS            |  [x]   |
| 💎   |   2   | Standard handle pre-wiring at process creation            | T05 §3         |  [/]   |
| 💎   |   3   | User-mode program break (brk/sbrk Linux compat)           | VMM, T17 §5    |  [ ]   |
| 💎   |   4   | Process priority class (Win32 `SetPriorityClass`)         | sched (exists) |  [ ]   |
| 💎   |   5   | Per-task scheduling policy (`SCHED_FIFO`/`IDLE`)          | §4             |  [ ]   |
| 💎   |   6   | Process capabilities and privilege bitmask                | --             |  [ ]   |
| ⭐   |   7   | Capability inheritance and drop-only policy               | §6             |  [ ]   |
| 💎   |   8   | Process accounting fields (times, I/O, VM counters)       | §1             |  [ ]   |
| 💎   |   9   | Per-process resource limits (rlimits)                     | §3, §6         |  [ ]   |
| 💎   |  10   | CPU affinity per process                                  | §4, D03 T06 §6 |  [ ]   |
| 💎   |  11   | Per-process mitigation policy                             | §6, T10 §1     |  [ ]   |
| ⭐   |  12   | Pledge/unveil-style process restriction                   | §6, §7         |  [ ]   |
| 💎   |  13   | Job Object syscalls wired to SSDT                         | §6, T12 §5     |  [ ]   |
| 💎   |  14   | Process exit cleanup -- release all per-process resources | §8, §9         |  [ ]   |
| 💎   |  15   | Parenting, reaping, and wait4/waitid semantics            | §14            |  [ ]   |
| 💎   |  16   | Protected Process Light (PS_PROTECTION)                   | D02 T19 §1     |  [ ]   |
| 💎   |  17   | Process groups and sessions (setpgid/setsid)              | --             |  [ ]   |

> 💎 = parity -- Windows NT (tokens + priority classes + accounting + rlimits) and Linux (capabilities + scheduling + getrusage + rlimits) both provide these.
> ⭐ = exclusive -- strict drop-only inheritance and pledge/unveil-style restriction are more auditable than both Windows token elevation and Linux `setcap`.

---

## 1. Working Directory

`struct task` has no `cwd` field. All VFS paths are currently treated as absolute. Relative path resolution must be added before any shell navigation or portable app path handling works.

- [x] Add `char cwd[TASK_CWD_MAX]` (512, `_Static_assert`-pinned to `VFS_MAX_PATH`) + `spinlock_t cwd_lock` to `struct task`; init `"C:\\"` at `task_init`/`task_create`/`task_create_user`
- [x] Inherit CWD from parent at `task_fork()` (and `task_create_user`) -- snapshot parent under its lock, commit to child
- [x] `task_exec()` preserves CWD (mutates in place, never touches cwd); PEB `CurrentDirectory` re-synced from `task->cwd` in `peb_alloc_for_task`
- [x] `vfs_resolve_path(cwd, in, out, size)` canonicalizer: join relative onto cwd, `/`->`\`, collapse `.`/empty, apply `..` without escaping the drive root, reject overflow (no truncation)
- [x] `task_get_cwd`/`task_set_cwd`/`task_resolve_path` -- lock-guarded snapshot/commit so a concurrent set is never observed half-written
- [x] `NtSetCurrentDirectory(UNICODE_STRING *)` (SSDT 0x03D9): decode+narrow, resolve, require `VFS_DIRECTORY`, release the probe ref, leave cwd unchanged on failure
- [x] `NtQueryCurrentDirectory(WCHAR *buf, ULONG bytes)` (SSDT 0x03DA): widen `task->cwd` to UTF-16, `STATUS_BUFFER_TOO_SMALL` if it does not fit, probe+copy_to_user
- [x] Relative-path resolution wired into ALL fs pathname consumers -- NtCreateFile (`nt_syscall.c`), NtDeleteFile + NtQueryAttributesFile (`nt_file.c`); `vfs_open` stays absolute-only (design review F3)
- [x] `oa_extract_path` hardened to bound its read by `ObjectName->Length` + output size (kills the unbounded-read; adversarial F1)
- [x] Register both syscalls in SSDT (→ XREF: `TODO-12-native-api-ssdt.md §5`)
- [/] Win32 A/W wrappers `SetCurrentDirectory`/`GetCurrentDirectory` are user-mode surface owned elsewhere (→ XREF: `TODO-05-win32-file-io-api.md §6` W-forms; `TODO-08-win32-api-surface.md §4` A-forms) -- kernel syscalls shipped here
- [ ] Commit: `"kernel: task -- working directory field and Nt API wiring"`

**Test checkpoint:** New process `task->cwd` is `"C:\\"`. `NtSetCurrentDirectory("C:\\Impossible")` updates CWD; `NtQueryCurrentDirectory` returns it. Setting a non-existent or non-directory path returns error, CWD unchanged. Relative `"System\\Logs"` resolves to `"C:\\System\\Logs"` from `"C:\\"`; `".."` pops one component but never escapes `X:\`. `task_fork()` child inherits parent CWD. Unit tests: 18 `ProcExt:` suites (TEST_CAT_SCHED) cover the resolver + cwd storage + boundary rejects. Test on: QEMU WHPX + TCG.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | 18 ProcExt suites, 0 failures

> **Notes:**
> - Shipped `cwd[512]`+`cwd_lock` on `struct task`, `vfs_resolve_path` canonicalizer, `task_get_cwd`/`task_set_cwd`/`task_resolve_path`, and `NtSetCurrentDirectory`/`NtQueryCurrentDirectory` (SSDT 0x03D9/0x03DA).
> - All cwd access is lock-guarded (snapshot/commit); relative resolution wired into NtCreateFile + NtDeleteFile + NtQueryAttributesFile; `vfs_open` stays absolute-only.
> - `task->cwd` is the single authoritative cwd; the user PEB `CurrentDirectory` is a creation-time mirror (F2, writeback owned by TODO-05 §6). Design + adversarial adoptions in the commit message.
> - Canonical doc: `include/kernel/fs/vfs.h` (`vfs_resolve_path`) + `include/kernel/sched/task.h` (cwd fields).
> - Scope boundary: §1 owns kernel cwd + Nt syscalls; Win32 A/W wrappers → TODO-05 §6 / TODO-08 §4; full OA probe + UTF-16 decode → TODO-12 §6 / TODO-14 §5.
> **Verified:** 2026-07-11 | commit `a05f4688` | 10/11 items | build OK | tests 18/18 PASS
> **Accepted:** [H] full OBJECT_ATTRIBUTES + UNICODE_STRING copy_from_user probe + UTF-16-vs-ASCII decode unification for NtCreateFile are pre-existing cross-file ABI work -> XREF: `TODO-12-native-api-ssdt.md §6` (item: "[Critical] NtCreateFile: snapshot OBJECT_ATTRIBUTES ... via copy_from_user" at line 362) + `TODO-14 §5` (kernel-wide UTF-16 decode)
> **Accepted:** [M] user PEB `CurrentDirectoryDosPath` not written on `NtSetCurrentDirectory` (task->cwd is the single kernel-authoritative source; all kernel resolution uses it) -> XREF: `TODO-05-win32-file-io-api.md §6` (item: "SetCurrentDirectoryW must also sync PEB CurrentDirectoryDosPath")
> **Quality reviewed:** 2026-07-11 | Codex 8x (design, adversarial, consistency, perf, re-adversarial) | 4M fixed, 1H+1M accepted-XREF | scope: kernel-code-quality

## 2. Standard Handle Pre-Wiring at Process Creation

When a process is created, `STD_INPUT_HANDLE` (0), `STD_OUTPUT_HANDLE` (1), and `STD_ERROR_HANDLE` (2) must already be open in its handle table. This requires TODO-05 §2 to be done first.

- [ ] Define `STD_INPUT_HANDLE`, `STD_OUTPUT_HANDLE`, `STD_ERROR_HANDLE` pseudo-handle constants in `include/kernel/nt/nt_types.h`
- [ ] At `task_create()` / `NtCreateProcess()`: allocate the process handle table (TODO-05 §2); wire the three standard handles:
  - STD_INPUT: terminal read end (initially the keyboard VFS node)
  - STD_OUTPUT / STD_ERROR: terminal write end (initially the framebuffer console VFS node)
- [ ] `GetStdHandle(nStdHandle)` → look up the pre-wired slot; `SetStdHandle(nStdHandle, handle)` → replace it
- [ ] `DuplicateHandle`: duplicate a handle slot into the child process at `NtCreateProcess` time for I/O redirection (`cmd > file` pipes the child stdout to a file handle before execution)
- [ ] Inherit standard handles into child at `task_fork()` if `HANDLE_FLAG_INHERIT` is set (TODO-12 §10)
- [ ] When `NtCreateProcess` gains a caller-supplied UTF-16 environment block, set `CREATE_UNICODE_ENVIRONMENT` in creation flags per Microsoft Learn ("Changing Environment Variables"); coordinate with D02 T22 §10 and D02 T12 §7 (distinct from firmware `NtQuerySystemEnvironmentValue*` at SSDT indices 0x00D2 through 0x00D6).
- [ ] Commit: `"kernel: task -- STD handle pre-wiring at process creation"`

**Test checkpoint:** After `task_create()`, `STD_INPUT_HANDLE`, `STD_OUTPUT_HANDLE`, `STD_ERROR_HANDLE` are valid handles in the process handle table. `WriteFile(STD_OUTPUT_HANDLE, ...)` produces console output. `DuplicateHandle` into child works for I/O redirection. Test on: QEMU WHPX + TCG.

> **Deferred:** [M] STD-handle pre-wiring is blocked on the console I/O endpoint model: the project uses PIPE-based console I/O (`pipe_create`/`pipe_read`/`pipe_write`), not the "keyboard VFS node / framebuffer console VFS node" this section assumes -- those device nodes do not exist. The terminal wires shell stdin/stdout pipes at spawn; a default console endpoint for arbitrary `task_create` processes is not defined yet. Pre-wire STD slots to those endpoints once the process-creation console model is settled -> XREF: `09-desktop-shell/TODO-08-terminal.md §1` (item: "pipe shell stdout → terminal_put_char; WM keyboard → shell stdin")

## 3. User-Mode Program Break (brk/sbrk -- Linux Compat)

Win32 programs use `NtAllocateVirtualMemory` (TODO-12 §9) for heap. Linux-compat programs call `brk(2)` / `sbrk(2)` which the kernel must handle by growing user-space pages.

> [!NOTE]
> **Design (codex design review, 6 findings adopted).** The 1 MiB user ELF window (0x800000-0x900000) is FULLY occupied (image + stack at 0x8FC000 + EIF dispatch table at 0x8F0000 + guard at 0x900000) -- a brk heap has NO room there. It gets a DEDICATED region instead, and `program_break` starts at that fixed base (not the ELF BSS end -- which would require threading `elf_load` `load_end` through the shared `exec_loader_fn` signature across ELF/EIF/PE). `elf_load` already computes `load_end` (elf.c) but the format-agnostic wrapper discards it.

- [ ] Add a dedicated brk region `USER_BRK_BASE`/`USER_BRK_LIMIT` (outside the ELF window AND the `SECTION_VIEW` region) in `include/kernel/mm/user_range.h`; the heap grows here
- [ ] `struct task`: add `uintptr_t program_break` (0=uninit) + `uintptr_t program_break_start` (floor) + a per-task VM lock; init break = start = `USER_BRK_BASE` at `task_exec` (fixed base, no loader-signature / BSS-end dependency)
- [ ] `sys_brk(addr)` grow: map PRIVATE frames via `vmm_map_user_page` tagged `VMM_FLAG_PAGE_OWNED` so `vmm_destroy_user_pml4` frees them at exit (design H1); reject `addr` below `program_break_start` or above `USER_BRK_LIMIT`
- [ ] Failure-atomic growth (design M1): on any `pmm_alloc_frame`/map failure mid-grow, unmap every page THIS call added + free the pending frame, keep the old break, return Linux failure semantics
- [ ] Shrink-with-unmap (design M2): `addr < program_break` unmaps + frees page-rounded pages down to the new break (credible malloc top-chunk trim); never below `program_break_start`
- [ ] Serialize validate/map/rollback/publish as ONE transaction under a SLEEPABLE per-task VM mutex (Executive guarded mutex), NOT a spinlock -- a max-range grow maps too many pages to hold a spinlock (design H4)
- [ ] `task_exec` CR3-reuse path: unmap + free the prior image's brk range before resetting the break fields, so a replaced image never inherits stale heap (design H2)
- [ ] `task_fork`: eager-copy every committed brk page into the child PML4 + copy `program_break`/`program_break_start` (COW is not available; reset-in-child would violate fork semantics -- design H3)
- [ ] `sys_sbrk(increment)`: return old break, then `sys_brk(break+increment)`; `increment==0` returns the current break
- [ ] `SYS_BRK=48` in `include/kernel/sched/syscall.h` + `case SYS_BRK` in `syscall_handler` (`syscall.c`); regen `abi_numbers.h` via `gen-user-abi.py`; hand-add `sys_brk`/`sys_sbrk` wrappers in `user/include/syscall.h`
- [ ] Rewire `user/lib/stdlib.c` `malloc`/`free` from the static 64 KiB BSS bump arena to an `sys_sbrk`-backed growable arena (the current arena has no sbrk consumer)
- [ ] Commit: `"kernel: task -- program break (brk/sbrk) for Linux compat heap"`

**Test checkpoint:** `sys_brk(0)` returns the current break (== `USER_BRK_BASE` after exec). `sys_sbrk(4096)` returns the old break; the new page is writable. A grow then shrink returns the break and the freed pages fault on access. `sys_brk` below `program_break_start` or above `USER_BRK_LIMIT` is rejected. Frame-leak test: exec a process that grows the heap, exit it, assert the PMM free-count returns to baseline (design H1). Fork test: child sees the parent's heap contents (design H3). Test on: QEMU WHPX + TCG; bare metal.

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
- [ ] Register `NtSetInformationProcess` and `NtQueryInformationProcess` in SSDT (→ XREF `TODO-12 §5`)
- [ ] Commit: `"kernel: task -- process priority class and NtSetInformationProcess"`

**Test checkpoint:** `NtSetInformationProcess(ProcessPriorityClass, PROCESS_PRIORITY_HIGH)` updates all thread base priorities to `THREAD_PRIO_HIGH`. `PROCESS_PRIORITY_REALTIME` without `CAP_REALTIME` returns `STATUS_PRIVILEGE_NOT_HELD`. `NtQueryInformationProcess(ProcessPriorityClass)` returns current class. Serial log shows `"task: priority class set to <class>"`. `POST16(0xD040)` on entry, `POST16(0xD041)` after threads updated. Range `0xD04x` confirmed free. Test on: QEMU WHPX + TCG.

## 5. Per-Task Scheduling Policy

> [!NOTE]
> → XREF: `03-memory-concurrency/TODO-06-scheduler-enhancement.md §4` -- scope overlap: the scheduler TODO implements `SCHED_FIFO`/`SCHED_RR`/`SCHED_DEADLINE` classes with RT run queues, priority levels, and the actual `schedule()` loop changes. This section defines only the process-level `sched_policy` field on `struct task` and the `NtSetInformationProcess` API surface. Scheduler loop changes are authoritative in `03-memory-concurrency/TODO-06-scheduler-enhancement.md`.

Add a per-task `sched_policy` field for tasks that need non-time-sliced execution.

- [ ] Add `uint8_t sched_policy` to `struct task` with values:
  - `SCHED_POLICY_NORMAL = 0` -- default round-robin with `SCHED_QUANTUM` time slice
  - `SCHED_POLICY_FIFO   = 1` -- run until block or yield; no preemptive time-slice; requires `CAP_SCHED_FIFO`
  - `SCHED_POLICY_IDLE   = 2` -- only scheduled when no `SCHED_POLICY_NORMAL` or `SCHED_POLICY_FIFO` task is runnable
- [ ] Update `schedule()` in `task.c`: skip `SCHED_POLICY_FIFO` tasks for quantum-based preemption; skip `SCHED_POLICY_IDLE` tasks when higher-policy tasks are runnable
- [ ] `NtSetInformationProcess(ProcessHandle, ProcessSchedulingPolicy, ...)`: validate `CAP_SCHED_FIFO` for FIFO; update field
- [ ] Kernel-internal convenience: `task_set_sched_policy(pid, SCHED_POLICY_FIFO)` callable from boot paths
- [ ] `NtSetInformationProcess(ProcessSchedulingPolicy)` registered in SSDT shares the entry with §4 (→ XREF `TODO-12 §5`)
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
  - `CAP_SET_TIME        (1ULL << 5)` -- call `NtSetSystemTime` (→ XREF TODO-08 §9)
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
- [ ] At `task_exec()` via `exec_load()`: on EIF binaries, apply capability mask from EIF header flags; strip any bits not in the parent's set; never add new capabilities on exec (→ XREF `TODO-17 §1` -- EIF capabilities field is not yet defined in the spec; extend `eif_header_t` there before implementing here)
- [ ] `NtDropCapability(cap_mask)`: clear one or more capability bits from the calling process; irreversible for the lifetime of the process; register in SSDT (→ XREF `TODO-12 §5`)
- [ ] No syscall or path grants new capabilities -- escalation requires a restart or a privileged parent spawning with a specific mask
- [ ] Document the invariant: `child->capabilities ⊆ parent->capabilities` is enforced at fork and exec
- [ ] Commit: `"kernel: security -- capability inheritance and drop-only policy"`

**Test checkpoint:** `task_fork()` child inherits exact parent capabilities. `NtDropCapability(CAP_RAW_IO)` clears the bit; subsequent `capability_check(CAP_RAW_IO)` fails. Child drop does not affect parent. `NtDropCapability` is irreversible -- re-granting returns error. Serial log shows `"security: capability dropped -- cap=<N> remaining=0x<mask>"`. Test on: QEMU WHPX + TCG.

## 8. Process Accounting Fields (Times, I/O Counters, VM Counters)

Both Win11 (`NtQueryInformationProcess` with `ProcessTimes`, `ProcessIoCounters`, `ProcessVmCounters`) and Linux (`getrusage`, `times(2)`, `/proc/[pid]/stat`) track per-process resource usage. These fields must exist in `struct task` before TODO-12 §10 can return meaningful data.

- [ ] Add to `struct task`:
  - `uint64_t create_time_ns` -- set once at `task_create()` from `uptime_ns()`
  - `uint64_t user_time_ns` -- accumulated in scheduler tick handler when running in ring 3
  - `uint64_t kernel_time_ns` -- accumulated in scheduler tick handler when running in ring 0
  - `uint64_t io_read_count`, `io_read_bytes` -- incremented in VFS read path
  - `uint64_t io_write_count`, `io_write_bytes` -- incremented in VFS write path
  - `uint64_t minor_faults`, `major_faults` -- split at the `#PF` handler by whether a backing frame was already resident (`ru_minflt`/`ru_majflt`)
  - `uint64_t vol_ctxsw`, `invol_ctxsw` -- voluntary (yield/block) vs involuntary (preempt) context switches, incremented in the scheduler (`ru_nvcsw`/`ru_nivcsw`)
  - `uint64_t peak_working_set` -- updated on page allocation; tracks high-water mark
- [ ] In the scheduler tick ISR: determine ring from saved CS on interrupt frame; add tick duration to `user_time_ns` or `kernel_time_ns`
- [ ] In the scheduler switch path: increment `vol_ctxsw` when the outgoing thread yielded/blocked, `invol_ctxsw` when it was preempted
- [ ] In VFS `vfs_read()` / `vfs_write()`: increment `io_read_count`/`io_write_count` and byte counters on the current task
- [ ] In `#PF` handler: increment `page_fault_count`, and classify into `minor_faults` vs `major_faults` by whether the frame was already backed
- [ ] `getrusage(RUSAGE_SELF)` Linux-compat wrapper: populate `struct rusage` (incl. `ru_minflt`/`ru_majflt`/`ru_nvcsw`/`ru_nivcsw`) from task accounting fields
- [ ] `getrusage(RUSAGE_CHILDREN)`: return usage accumulated from reaped children (fed by §15 reap path)
- [ ] Commit: `"kernel: task -- process accounting fields for times, I/O, and VM counters"`

**Test checkpoint:** After running a process, `NtQueryInformationProcess(ProcessTimes)` returns non-zero `KernelTime` and `UserTime`. `ProcessVmCounters` returns non-zero `PageFaultCount`. `ProcessIoCounters` returns non-zero `ReadOperationCount` after a file read. Serial log shows `"task: accounting -- user=<N>ns kernel=<M>ns faults=<F>"` at process exit. `POST16(0xD080)` on entry, `POST16(0xD081)` after struct fields added, `POST16(0xD082)` after scheduler tick ISR instrumented, `POST16(0xD083)` after `#PF` handler instrumented. Range `0xD08x` confirmed free. If crash at 0xD082: scheduler tick ISR modification broke -- revert ISR change and fall back to un-instrumented tick. Test on: QEMU WHPX + TCG. Verify on bare metal -- ISR timing may differ.

## 9. Per-Process Resource Limits (rlimits)

Both Win11 (Job Object quotas + `QUOTA_LIMITS` via `NtQueryInformationProcess`) and Linux (`getrlimit`/`setrlimit`/`prlimit`) enforce per-process resource limits. This prevents runaway processes from exhausting system resources.

> [!NOTE]
> → XREF: `03-memory-concurrency/TODO-06-scheduler-enhancement.md §12` -- dynamic thread storage and thread admission are implemented in the scheduler domain. This section owns the quota/accounting surface; §12 consumes those limits so thread creation fails on real resource pressure instead of a fixed `THREAD_MAX` array ceiling.

- [ ] Define `rlimit_t` in `include/kernel/task_limits.h`: `{ uint64_t rlim_cur; uint64_t rlim_max; }` with `RLIM_INFINITY = UINT64_MAX`
- [ ] Define limit indices `RLIMIT_AS`, `RLIMIT_NOFILE` (coordinates with TODO-05 §14), `RLIMIT_CPU`, `RLIMIT_STACK`, `RLIMIT_NPROC`, `RLIMIT_FSIZE`, `RLIMIT_CORE`, `RLIMIT_MEMLOCK`, `RLIMIT_COUNT`
- [ ] Wire `RLIMIT_CORE` as the size gate consumed by crash-dump generation (-> XREF: `TODO-27-crash-dump-generation.md`; a value of 0 suppresses the dump, matching the `PR_SET_DUMPABLE`/§15 policy flag)
- [ ] Wire `RLIMIT_MEMLOCK` as the ceiling checked by the memory-pinning path (-> XREF: `TODO-12-native-api-ssdt.md §9` VirtualLock / the VMM pin owner); reject a lock that would exceed the cap
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
> → XREF: `03-memory-concurrency/TODO-06-scheduler-enhancement.md §6` -- thread-level CPU affinity (`affinity_mask` in `task_t`, `NtSetInformationThread(ThreadAffinityMask)`) is implemented there. This section adds the process-level API: `SetProcessAffinityMask` / `NtSetInformationProcess(ProcessAffinityMask)` which sets the affinity for all threads in the process.

> [!WARNING]
> Thread-level `affinity_mask` field on `struct thread` does not exist yet (→ 03-memory-concurrency/TODO-06-scheduler-enhancement.md §6, `[ ]`). If 03-memory-concurrency/TODO-06-scheduler-enhancement.md §6 has not landed, add `uint64_t affinity_mask` to `struct thread` here with a default of `~0ULL` (all CPUs). The scheduler does not need to consume the mask until 03-memory-concurrency/TODO-06-scheduler-enhancement.md §6 adds the per-CPU run queue logic.

- [ ] Add `uint64_t affinity_mask` to `struct thread` if not already present from 03-memory-concurrency/TODO-06-scheduler-enhancement.md §6 (default `~0ULL`)
- [ ] `NtSetInformationProcess(ProcessHandle, ProcessAffinityMask, &mask, sizeof(mask))`: iterate all threads in the process; set each thread's `affinity_mask` to the intersection of the new process mask and the thread's current mask; store process-level mask in `struct task`
- [ ] `NtQueryInformationProcess(ProcessHandle, ProcessAffinityMask, ...)`: return the process-level mask
- [ ] Win32 wrappers: `SetProcessAffinityMask(hProcess, dwMask)` → `NtSetInformationProcess`; `GetProcessAffinityMask(hProcess, &procMask, &sysMask)` → returns process mask and system mask (all CPUs)
- [ ] New threads inherit the process affinity mask at creation
- [ ] Commit: `"kernel: task -- per-process CPU affinity (SetProcessAffinityMask)"`

**Test checkpoint:** `SetProcessAffinityMask(current, 0x3)` restricts process to CPUs 0-1. `GetProcessAffinityMask` returns `0x3`. New thread created after affinity change has `affinity_mask = 0x3`. Serial log shows `"task: process affinity set to 0x<mask>"`. Test on: QEMU WHPX + TCG (2+ vCPUs). Verify on bare metal -- SMP affinity behavior differs.

## 11. Per-Process Mitigation Policy

Win11 provides `SetProcessMitigationPolicy` to control per-process security features: DEP enforcement mode, mandatory ASLR, CFG strictness, child process creation restrictions, image load restrictions. Linux uses `prctl` with `PR_SET_NO_NEW_PRIVS`, `PR_SET_SECCOMP`, etc. Impossible OS needs a unified per-process mitigation flags field that coordinates with the security features implemented in other TODOs.

> [!NOTE]
> → XREF: `TODO-10-kernel-security-hardening.md §1` (NX/DEP), `TODO-17-binary-system.md §15` (ASLR), `TODO-17 §12` (CFG), `TODO-12-native-api-ssdt.md §25` (syscall filtering). This section defines the per-process flags and API surface; enforcement is authoritative in those TODOs. This is a deliberate SUBSET of Win11's ~21 `PROCESS_MITIGATION_*` policies -- the high-value ones with an enforcement owner in-tree; CET (IBT/SHSTK) enforcement → `TODO-10-kernel-security-hardening.md §10`, dynamic-code and image-signature policy → `TODO-19-code-integrity-trust-policy.md`.

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
- [ ] In SSDT dispatcher: if `task->pledge_mask != 0`, check if the invoked syscall's category bit is set; if not, terminate with `STATUS_PLEDGE_VIOLATION` and log the violation (→ XREF: `TODO-12-native-api-ssdt.md §25` -- per-index bitmap filter runs in the same dispatcher path; pledge category check runs AFTER the bitmap filter; both must pass for the syscall to proceed)
- [ ] `NtUnveil(const char *path, const char *permissions)`: restrict filesystem visibility. After the first `NtUnveil` call, only unveiled paths are accessible. `permissions` is a subset of `"rwxc"` (read, write, execute, create). Calling `NtUnveil(NULL, NULL)` locks the unveil set -- no further calls allowed.
- [ ] Add `unveil_entry_t *unveil_list` to `struct task`; VFS path resolution checks against this list if non-NULL
- [ ] Both `NtPledge` and `NtUnveil` are irreversible -- once applied, restrictions can only be tightened, never loosened
- [ ] Register both in SSDT (→ XREF TODO-12 §5)
- [ ] Commit: `"kernel: task -- pledge/unveil process restriction (OpenBSD-inspired)"`

**Test checkpoint:** After `NtPledge("stdio rpath")`, calling `NtCreateFile` for write returns `STATUS_PLEDGE_VIOLATION` and process terminates. After `NtUnveil("/C:\\Impossible\\System", "rx")`, reading from `C:\Impossible\System\shell.exe` succeeds; reading from `C:\Users\` returns `STATUS_ACCESS_DENIED`. Serial log shows `"task: pledge violation -- syscall <N> not in pledge set"`. `POST16(0xD0C0)` on entry, `POST16(0xD0C1)` after pledge_mask set, `POST16(0xD0C2)` after SSDT dispatcher check wired. Range `0xD0Cx` confirmed free. If crash at 0xD0C2: SSDT dispatcher modification broke -- revert the dispatch check and fall back to un-pledged execution. Test on: QEMU WHPX + TCG.

---

## 13. Job Object Syscalls Wired to SSDT

Register Job Object management syscalls in the SSDT for process-group resource control. Win11 Job Objects are the primary mechanism for process-group resource limits (CPU rate, memory cap, I/O throttle). Linux uses cgroups v2 for equivalent functionality. (→ XREF: TODO-12-native-api-ssdt.md §5)

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
  - Release timer resolution requests held by this PID (-> XREF: TODO-17 §6 `KeSetTimerResolution`)
  - Close all open handles in the process handle table (-> XREF: TODO-05 §2 OB handle table)
  - Release all byte-range locks held by this process (-> XREF: 05-storage-filesystems/TODO-04 §10 `vfs_lock_file`)
  - Release all share-mode handle entries for open files (-> XREF: 05-storage-filesystems/TODO-04 §8 `vfs_open_handle_t`)
  - Trigger delete-on-close for files marked by this process (-> XREF: 05-storage-filesystems/TODO-04 §9)
  - Release any oplock held by this process (-> XREF: 05-storage-filesystems/TODO-04 §14)
  - Free per-process memory: PEB, TEB, user stack, address space (-> XREF: 02-kernel-core/TODO-26 §5)
  - Invoke ELF `DT_FINI_ARRAY` / PE `DLL_PROCESS_DETACH` destructors, then deregister every module from the `loaded_module_t` registry before unmap (-> XREF: 02-kernel-core/TODO-17 §20 fini-array, §6 module registry)
  - Release per-process resource limits and accounting (-> XREF: §8, §9 of this TODO)
  - Remove from job object if assigned (-> XREF: §13)
- [ ] Log: `klog(LOG_DEBUG, "task", "PID %u exit cleanup: %u handles, %u locks released", ...)`
- [ ] Commit: `"kernel: task -- process exit cleanup (handles, locks, timer res, memory)"`

**Test checkpoint:** Create a process that opens files with locks + timer resolution request. Kill the process. Verify: all locks released, timer resolution reverts to default, handles closed, no resource leak. Serial log shows cleanup counts. Test on QEMU WHPX, TCG.

---

## 15. Process Parenting, Reaping, and Wait Semantics

Exit cleanup (§14) frees resources but does not define WHEN exit status is observable, WHO reaps an orphan, or how competing waits serialize. `task_waitpid` today accepts one exact child PID, blocks unconditionally, and immediately reaps (`task.c`), so it cannot support `waitpid(-1)`, `WNOHANG`/`WUNTRACED`/`WCONTINUED`, `waitid`, `wait4` child usage, or subreaper adoption. This is the child-lifecycle contract §14 depends on.

- [ ] Add a `RUNNING -> ZOMBIE -> REAPED` lifecycle to `struct task`: on exit, transition to ZOMBIE retaining `exit_status` + accumulated child `rusage`; free the task struct only at REAPED (a successful wait).
- [ ] Extend `task_waitpid` to `sys_wait4(pid, status, options, rusage)`: `pid` selectors (any-child, process-group, exact), `WNOHANG`/`WUNTRACED`/`WCONTINUED` options, atomic single-reaper (one waiter per zombie).
- [ ] `sys_waitid(idtype, id, siginfo, options)` with `WEXITED`/`WSTOPPED`/`WCONTINUED`/`WNOWAIT`; accumulate reaped-child usage into the parent for `RUSAGE_CHILDREN` (-> XREF: §8).
- [ ] Orphan reparenting: on parent exit, reparent live/zombie children to the nearest `PR_SET_CHILD_SUBREAPER` ancestor, else to init (pid 1).
- [ ] `prctl(PR_SET_PDEATHSIG)` signals a child on parent death; `prctl(PR_SET_CHILD_SUBREAPER)` marks a subreaper. Linux-compat adapters -> XREF: `TODO-10-kernel-security-hardening.md`.
- [ ] NT non-reaping: a process HANDLE stays signalable after exit (all `NtWaitForSingleObject` waiters wake), independent of the POSIX single-reaper -> XREF: `TODO-12-native-api-ssdt.md` (wait-vs-reap).
- [ ] `dumpable`/core-dump policy flag per process (`PR_SET_DUMPABLE`): gates whether a crashing process produces a dump (-> XREF: `TODO-27-crash-dump-generation.md`; `RLIMIT_CORE` gate in §9).
- [ ] Overflow-safe status/rusage retention; a reaped zombie's storage is reclaimed exactly once (no double-free, no premature status loss).
- [ ] Commit: `"kernel: task -- parenting, reaping, and wait4/waitid semantics"`

**Test checkpoint:** `sys_wait4(-1, ...)` reaps any child; `WNOHANG` returns 0 when no child exited; a double-reap of the same zombie returns ECHILD to the second waiter; an orphan's parent-pid becomes 1 (or the subreaper) after its parent exits; `PR_SET_PDEATHSIG` delivers on parent death. `klog(LOG_DEBUG, "task", "reaped pid %u status %u")`. Test on: QEMU WHPX, QEMU TCG; bare metal.

---

## 16. Protected Process Light (PS_PROTECTION)

Windows exposes per-process protection levels (`PROCESS_PROTECTION_LEVEL_INFORMATION`, `PsIsProtectedProcessLight`) so a lower-protection process cannot terminate, VM-write, duplicate-handle, or debug a higher one -- the anti-tamper backbone for LSASS/AV/DRM. TODO-15 records the field + signer enforcement as ownerless and requests TODO-21 ownership; TODO-05 §13 supplies only the `ObRegisterCallbacks` handle-filter mechanism.

- [ ] Add a `ps_protection` field to `struct task`: level (None, ProtectedLight-Authenticode, Antimalware, Lsa, WinTcbLight, WinTcb) + signer class.
- [ ] Assignment + inheritance: a process's protection is set at create from its image signer class (sourced from code-integrity verification -> XREF: `TODO-19-code-integrity-trust-policy.md`); a child cannot exceed its creator's protection.
- [ ] Protection-dominance access matrix: an EXPLICIT tested (accessor, target) -> allowed-mask table, NOT numeric-enum-ordering; lower protection is stripped of TERMINATE/VM_WRITE/DUP_HANDLE/SET_INFORMATION/debug against higher.
- [ ] Consume the matrix in the `ObRegisterCallbacks` open-handle filter (-> XREF: `TODO-05-object-manager.md §13` -- shipped handle-filter mechanism).
- [ ] `NtQueryInformationProcess(ProcessProtectionInformation)` returns the level (-> XREF: `TODO-12-native-api-ssdt.md §10`).
- [ ] Commit: `"kernel: task -- protected process light (PS_PROTECTION) + dominance matrix"`

**Test checkpoint:** a None-protection process opening a WinTcb process for TERMINATE/VM_WRITE/DUP_HANDLE is denied (STATUS_ACCESS_DENIED); a higher-level process opening a lower one succeeds; the dominance matrix test exercises every (level, level) pair. `klog(LOG_WARN, "ob", "PPL: denied 0x%x from level %u to level %u")`. Test on: QEMU WHPX, QEMU TCG; bare metal.

---

## 17. Process Groups and Sessions

No section owns session ID, process-group ID, session leadership, the foreground terminal group, or group signal delivery -- so a shell cannot background jobs, `kill -pgid` a pipeline, or fan Ctrl+C out to a process tree. TODO-10 currently sends SIGINT to a single foreground task; `GenerateConsoleCtrlEvent` cannot target a group. This section owns the per-process fields + invariants; the Linux syscall adapters live in TODO-10 and the terminal consumes the foreground-group API.

- [ ] Add `pgid` + `sid` fields to `struct task`; a new process inherits its parent's pgid/sid.
- [ ] `sys_setpgid(pid, pgid)` with POSIX invariants (same session, not a session leader, target in the caller's session); `sys_setsid()` creates a new session + process group led by the caller (fails for a group leader).
- [ ] `getpgid`/`getsid`/`getpgrp` queries.
- [ ] Foreground-console-group state on the controlling terminal; `tcsetpgrp`/`tcgetpgrp` equivalent to set/query the foreground group.
- [ ] Group signal fan-out: a signal to `-pgid` (or the foreground group on Ctrl+C) is delivered to every process in the group; orphaned-process-group handling per POSIX.
- [ ] `GenerateConsoleCtrlEvent(CTRL_C_EVENT/CTRL_BREAK_EVENT, pgid)` targets the console group; the terminal TODO consumes this API. Linux `setpgid`/`setsid` adapters -> `TODO-10-kernel-security-hardening.md`.
- [ ] Commit: `"kernel: task -- process groups and sessions (setpgid/setsid + group signals)"`

**Test checkpoint:** `setsid()` makes the caller a session+group leader (getsid==getpid); `setpgid` moves a child into a new group; a signal to `-pgid` reaches all members; Ctrl+C on the foreground group interrupts a pipeline, not just the leader. `klog(LOG_DEBUG, "task", "pgid %u signal %u -> %u procs")`. Test on: QEMU WHPX, QEMU TCG; bare metal.

---

## OS Comparison

| ⭐   | Feature                       | 🪟 Win11                       | 🐧 Linux                   | 🚀 Impossible OS            |
| --- | ----------------------------- | ----------------------------- | ------------------------- | -------------------------- |
| 💎   | Per-process CWD               | ✅ SetCurrentDirectory         | ✅ chdir / getcwd          | ✅ §1 cwd + Nt syscalls     |
| 💎   | STD handle pre-wiring         | ✅ CreateProcess inherit       | ✅ fd 0/1/2 via fork       | ⬜ §2                       |
| 💎   | User-mode heap (brk)          | ✅ NtAllocateVirtualMemory     | ✅ brk / sbrk              | ⬜ §3                       |
| 💎   | Process priority class        | ✅ SetPriorityClass            | ✅ nice / setpriority      | ⬜ §4                       |
| 💎   | Scheduling policy             | ✅ REALTIME_PRIORITY_CLASS     | ✅ SCHED_FIFO / SCHED_IDLE | ⬜ §5                       |
| 💎   | Capability / privilege model  | ✅ Access tokens               | ✅ POSIX capabilities      | ⬜ §6                       |
| 💎   | Process accounting            | ✅ ProcessTimes + IoCounters   | ✅ getrusage / times       | ⬜ §8                       |
| 💎   | Per-process resource limits   | ✅ Job Object quotas           | ✅ getrlimit / setrlimit   | ⬜ §9                       |
| 💎   | Process CPU affinity          | ✅ SetProcessAffinityMask      | ✅ sched_setaffinity       | ⬜ §10                      |
| 💎   | Per-process mitigation policy | ✅ SetProcessMitigationPolicy  | ⚠️ prctl + seccomp        | ⬜ §11                      |
| 💎   | Job Objects / cgroups         | ✅ NtCreateJobObject           | ✅ cgroups v2              | ⬜ §13                      |
| 💎   | Process exit cleanup          | ✅ PspExitProcess              | ✅ do_exit + __put_task    | ⬜ §14                      |
| 💎   | Reaping / wait semantics      | ⚠️ Handle signaling (no reap) | ✅ wait4 / waitid          | ⬜ §15                      |
| 💎   | Protected Process Light       | ✅ PS_PROTECTION               | ❌ No equivalent           | ⬜ §16                      |
| 💎   | Process groups / sessions     | ⚠️ Console ctrl groups        | ✅ setpgid / setsid        | ⬜ §17                      |
| 💎   | Per-process I/O priority      | ✅ ProcessIoPriority           | ✅ ioprio_set/get          | ⬜ Deferred (→ TODO-12 §10) |
| ⭐   | Drop-only cap inheritance     | ⚠️ Token elevation            | ⚠️ setcap raises ambient  | ⬜ §7 -- monotonic decrease |
| ⭐   | Pledge/unveil restriction     | ❌ None                        | ❌ No simple equivalent    | ⬜ §12 -- OpenBSD-inspired  |

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
  - `sys_wait4(-1, ...)` reaps any child; a second reap of the same zombie returns `ECHILD` (§15)
  - `WNOHANG` on a still-running child returns 0 without blocking (§15)
  - After a parent exits, its orphaned child's parent-pid becomes 1 (or the nearest subreaper) (§15)
  - `getrusage(RUSAGE_CHILDREN)` returns non-zero usage after a child with CPU work is reaped (§15)
  - A None-protection process opening a WinTcb process for TERMINATE is denied `STATUS_ACCESS_DENIED` (§16)
  - The PPL dominance matrix: a higher-level process opening a lower one for VM_WRITE succeeds (§16)
  - `setsid()` makes the caller a session+group leader (`getsid() == getpid()`) (§17)
  - `setpgid` moves a child into a new group; a signal to `-pgid` reaches every member (§17)
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
- [ ] `sys_wait4(-1)` reaps any child; `WNOHANG` returns 0 for a running child; an orphan reparents to init/subreaper (§15)
- [ ] A lower-protection process cannot TERMINATE/VM_WRITE/DUP_HANDLE a Protected Process Light target (§16)
- [ ] `setsid`/`setpgid` establish session/group IDs; a signal to `-pgid` reaches the whole group (§17)
- [ ] Commit: `"kernel: task -- process model extensions complete"`
