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
> **Current state:** `struct task` has `pid`, `state`, `rsp`, stacks, `name`, `parent_pid`, `exit_status`, `wait_pid`, and `signals`. Thread-level priority is fully implemented (`THREAD_PRIO_IDLE`…`THREAD_PRIO_REALTIME`, priority-aware scheduler, PI boosting). Missing from `struct task` (pre-§1 baseline; `cwd` shipped in §1): `capabilities`, `brk`/`program_break`, `sched_policy`, and process-class priority. Handle table infrastructure is in TODO-05. PEB `RTL_USER_PROCESS_PARAMETERS.Environment` pointer layout is in D02 T11 §4 and §5; per-process env arrays and argv are owned by D02 T22.

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

| ⭐   | Order | Deliverable                                               | Depends On         | Status |
| --- | :---: | --------------------------------------------------------- | ------------------ | :----: |
| 💎   |   1   | Working directory (`cwd` field + Nt/VFS wiring)           | VFS                |  [x]   |
| 💎   |   2   | Standard handle pre-wiring at process creation            | T05 §3             |  [/]   |
| 💎   |   3   | User-mode program break (brk/sbrk Linux compat)           | VMM, T17 §5        |  [/]   |
| 💎   |   4   | Process priority class (Win32 `SetPriorityClass`)         | sched (exists)     |  [/]   |
| 💎   |   5   | Per-task scheduling policy (`SCHED_FIFO`/`IDLE`)          | §4                 |  [/]   |
| 💎   |   6   | Process capabilities and privilege bitmask                | --                 |  [/]   |
| ⭐   |   7   | Capability inheritance and drop-only policy               | §6                 |  [/]   |
| 💎   |   8   | Process accounting fields (times, I/O counters)           | §1                 |  [/]   |
| 💎   |   9   | Per-process resource limits (rlimits)                     | §3, §6             |  [/]   |
| 💎   |  10   | CPU affinity per process                                  | §4, D03 T06 §6     |  [/]   |
| 💎   |  11   | Per-process mitigation policy                             | §6, T10 §1         |  [/]   |
| ⭐   |  12   | Pledge/unveil-style process restriction                   | §6, §7             |  [x]   |
| 💎   |  13   | Job Object syscalls wired to SSDT                         | §6, T12 §5         |  [/]   |
| 💎   |  14   | Process exit cleanup -- release all per-process resources | §8, §9             |  [/]   |
| 💎   |  15   | Parenting, reaping, wait4 + ZOMBIE lifecycle              | §14                |  [/]   |
| 💎   |  16   | Protected Process Light (PS_PROTECTION)                   | D02 T19 §1, T12 §7 |  [/]   |
| 💎   |  17   | Process groups and sessions (setpgid/setsid)              | --                 |  [/]   |
| 💎   |  18   | Rich wait variants + NT multi-waiter wake + dumpable      | §15, §17           |  [/]   |
| 💎   |  19   | `task_exec` commit point + kernel-stack reclamation       | §14, §15           |  [x]   |
| 💎   |  20   | Page-table lifetime across reap + fork/exec               | §15, §19           |  [ ]   |

> 💎 = parity -- Windows NT (tokens + priority classes + accounting + rlimits) and Linux (capabilities + scheduling + getrusage + rlimits) both provide these.
> ⭐ = exclusive -- strict drop-only inheritance and pledge/unveil-style restriction are more auditable than both Windows token elevation and Linux `setcap`.

---

## 1. Working Directory

`struct task` has no `cwd` field. All VFS paths are currently treated as absolute. Relative path resolution must be added before any shell navigation or portable app path handling works.

- [x] Add `char cwd[TASK_CWD_MAX]` (512, `_Static_assert`-pinned to `VFS_MAX_PATH`) + `spinlock_t cwd_lock` to `struct task`; init `"C:\\"` at `task_init`/`task_create`/`task_create_user`
- [x] Inherit CWD from parent at `task_fork()` (and `task_create_user`) -- snapshot parent under its lock, commit to child
- [ ] Child environ inheritance (incl. this fork path) owned by `TODO-12` §7 (env_copy fail-closed pre-publish across ALL constructors; publish-lock + slot-reserve prereqs are the two `TODO-12` §7 items)
- [x] `task_exec()` preserves CWD (mutates in place, never touches cwd); PEB `CurrentDirectory` re-synced from `task->cwd` in `peb_alloc_for_task`
- [x] `vfs_resolve_path(cwd, in, out, size)` canonicalizer: join relative onto cwd, `/`->`\`, collapse `.`/empty, apply `..` without escaping the drive root, reject overflow (no truncation)
- [x] `task_get_cwd`/`task_set_cwd`/`task_resolve_path` -- lock-guarded snapshot/commit so a concurrent set is never observed half-written
- [ ] Concurrent `NtSetCurrentDirectory` non-linearizable (resolves vs a cwd snapshot outside `chdir_lock`; racing relative chdirs leave a non-serial cwd). Low-pri (Win not-thread-safe); fix via cwd gen-counter + verify-retry
- [x] `NtSetCurrentDirectory(UNICODE_STRING *)` (SSDT 0x03D9): decode+narrow, resolve, require `VFS_DIRECTORY`, release the probe ref, leave cwd unchanged on failure
- [x] `NtQueryCurrentDirectory(WCHAR *buf, ULONG bytes)` (SSDT 0x03DA): widen `task->cwd` to UTF-16, `STATUS_BUFFER_TOO_SMALL` if it does not fit, probe+copy_to_user
- [x] Relative-path resolution wired into ALL fs pathname consumers -- NtCreateFile (`nt_syscall.c`), NtDeleteFile + NtQueryAttributesFile (`nt_file.c`); `vfs_open` stays absolute-only (design review F3)
- [x] `oa_extract_path` hardened to bound its read by `ObjectName->Length` + output size (kills the unbounded-read; adversarial F1)
- [x] Register both syscalls in SSDT (→ XREF: `TODO-12-native-api-ssdt.md §5`)
- [/] Win32 A/W wrappers `SetCurrentDirectory`/`GetCurrentDirectory` are user-mode surface owned elsewhere (→ XREF: `TODO-05-win32-file-io-api.md §6` W-forms; `TODO-08-win32-api-surface.md §4` A-forms) -- kernel syscalls shipped here
- [ ] Non-ASCII CWD: NtSetCurrentDirectory narrows via `nt_unicode_to_ascii` (rejects >0x7F), NtQuery/PEB byte-widen `cwd`; store cwd UTF-8 + strict UTF-16<->UTF-8 at NtSet/NtQuery/PEB so `SearchPathW` CWD supports non-ASCII -> XREF: `TODO-22 §14`.
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
- [ ] Persist a kernel-owned canonical image path on `struct task` at creation/exec (resolved absolute, user-immutable) -- trusted source for image identity + `SearchPathW` leg-1; PEB `ImagePathName` is caller-writable -> XREF: `TODO-22 §14`.
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

> **Deferred:** 2026-07-11 | Codex design review (5 blocking findings, each verified at file:line via `superpowers:receiving-code-review`). §3's correct implementation is blocked on memory infrastructure not yet in the tree; a bare-metal-safe brk cannot ship without it, and a syscall wired to an unimplementable handler is a forbidden stub. Blockers: (1) [Critical] a private brk mapping anywhere in the kernel's identity-mapped low 4 GiB shadows kernel physical-frame access under a user CR3 (verified `pmm.c:227-237` reserves the ELF window for exactly this reason -- "kernel data placed anywhere in the window would vanish under a user CR3"); a correct user heap needs true per-process physical isolation, not identity-map clones -> XREF: `03-memory-concurrency/TODO-01-vmm-memory-protection.md §3` (item: "Per-process physical isolation: `vmm_create_user_pml4()` must allocate unique physical pages per process instead of cloning the kernel's identity-mapped frames" at line 121). (2) [High] runtime brk shrink/rollback unmap needs an SMP TLB shootdown IPI (verified `task.c:2553` defers ALL live user-page unmaps to the TASK_DEAD path "because `vmm_unmap_page` lacks SMP TLB shootdown") -> XREF: `03-memory-concurrency/TODO-07-smp-phase2.md §2` (item: "Hook `tlb_shootdown()` into `vmm_unmap()`..." at line 94). (3) [High] the per-task VM lock's `mutex_t` wait-queue is not yet SMP-safe -> XREF: `03-memory-concurrency/TODO-08-advanced-sync.md §11` (item: "Wait-queue protection: add a per-mutex `spinlock_t wait_lock`"). Re-enters when (1)+(2) ship. Fix-on-re-entry details (implementation-level findings, verified): fork eager-copy must resolve parent frames with a CR3-explicit walk (`vmm_get_physical` walks only `kernel_pml4`, `vmm.c:486`) and use a copy-preserving owned mapper (the zero-filling path erases the copy, `vmm.c:777`), holding the parent VM lock across snapshot+copy and building the child fully before `num_tasks++`; `sbrk` must be a kernel-atomic increment op, not a user-space query-then-set (TOCTOU between two threads). Design decisions that survived review (reuse on re-entry): opt-in `vmm_map_user_page_owned` variant (safer than mutating existing callers); `mutex_t` is the correct lock class; exclusive `SECTION_VIEW_LIMIT` boundary + first-GiB destroy coverage are numerically correct -- the identity-map collision is the region blocker.

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

> **Deferred:** 2026-07-11 | Codex design review (needs-attention: 1 Critical + 4 High, each verified at file:line via `superpowers:receiving-code-review`). §4's correct implementation is blocked on infrastructure owned elsewhere -- the NT trust-boundary campaign, the OB process-object handle model, and the scheduler rework. A thin priority-class wiring would ship a bypassable/racy/DoS-enabling feature (forbidden: never ship broken), so §4 defers with XREFs and advances. Blockers, verified: (1) [Critical] the `ProcessPriorityClass` query/set handlers deref user pointers raw -- `*pclass = t->threads[0].base_priority` with no `ProbeForWriteIfUser`/`copy_to_user` (`nt_process.c:548-554`), symmetric on the set path (`nt_process.c:620-628`); with SMAP off and shared frames a ring-3 caller can point the buffer at kernel memory for an arbitrary kernel write. This is the SAME systemic gap already filed + deferred -> XREF: `02-kernel-core/TODO-12-native-api-ssdt.md §29` (item: "[Critical] NtQuery/SetInformationProcess ... input/output user pointers deref'd raw ... probe + copy_from_user/copy_to_user via kernel bounce buffers" at line 1221). (2) [High] process handles are raw PIDs with no granted-access/lifetime check -- `task_from_handle()` returns `task_get_by_pid()` for any non-zero handle (`nt_process.c:27-35`), so a caller can reprioritize an arbitrary PID; Windows requires `PROCESS_SET_INFORMATION`/query rights. Owned by the process-object handle model -> XREF: `02-kernel-core/TODO-12-native-api-ssdt.md §7` (item: "NtCreateProcess/NtOpenProcess/NtOpenThread return raw PID/TID/task-struct, not OB PROCESS/THREAD objects" at line 413) + `02-kernel-core/TODO-05-object-manager.md §2` (item: "Reference Counting and Object Lifetime" at line 112). (3) [High] exposing ABOVE_NORMAL/HIGH/REALTIME is an unprivileged starvation DoS -- `find_next_task()` always selects the numerically-highest READY thread with no aging (`task.c:142-203`), so a CPU-bound HIGH (24) thread starves NORMAL (16), PID 0, and kernel workers even after `yield()`. Blocked on scheduler starvation prevention -> XREF: `03-memory-concurrency/TODO-06-scheduler-enhancement.md §2` (item: "In the scheduler tick ISR ... `ticks_waiting` ... decrement `effective_priority` ... starvation prevention" at line 93). (4) [High] class application is lockless -- `thread_set_priority()` does an unsynchronized compound RMW of `priority`+`base_priority` (`task.c:3459-3468`) that races the PI boost/restore path (`thread_boost_priority`/`thread_restore_priority`, `task.c:3472-3493`): the class write can clobber an active donation or leave effective priority below the new base. Needs a scheduler-owned priority-sync protocol -> XREF: `03-memory-concurrency/TODO-06-scheduler-enhancement.md §1` (item: "`thread_set_priority(tid, prio)` syscall; validated range" at line 82, rebuilt with effective/base separation) + `03-memory-concurrency/TODO-08-advanced-sync.md §11` (item: "Mutex Wait-Queue SMP-Safety Backfill" at line 268). Fix-on-re-entry (design facts, verified): the wire ABI is Windows `PROCESS_PRIORITY_CLASS { BOOLEAN Foreground; UCHAR PriorityClass; }` (2 bytes, IDs IDLE=1/NORMAL=2/HIGH=3/REALTIME=4/BELOW_NORMAL=5/ABOVE_NORMAL=6), not the 4-byte 0..5 contract this section proposed [finding 2, High]; store a distinct `priority_class` field (not thread `base_priority`), translate the Win32 class separately from a per-thread relative level, require exact length, build results in kernel-local storage, and restrict to `CURRENT_PROCESS` until (2) lands. Re-enters when the §29 usercopy campaign + the OB process-object model + TODO-06 §1/§2 scheduler rework ship.

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

> **Deferred:** 2026-07-11 | §5's own design note already declares scheduler-loop changes authoritative in `03-memory-concurrency/TODO-06-scheduler-enhancement.md §4`; the shippable-here remainder is a scheduler-ignored field + a gated stub syscall, which is a forbidden half-measure (never ship broken). Blockers: (1) FIFO/IDLE enforcement (item 2's `schedule()` change) is owned by TODO-06 §4 -- exposing `SCHED_POLICY_FIFO` (run-until-yield, no preemption) safely requires the separate RT run queue AND the kernel watchdog pinned at `SCHED_FIFO` priority 99 so an un-yielding FIFO task cannot starve it; without that, a `CAP_SCHED_FIFO` caller has the same unprivileged-starvation DoS as §4 finding 4 (`find_next_task()` has no aging, `task.c:142-203`) -> XREF: `03-memory-concurrency/TODO-06-scheduler-enhancement.md §4` (item: "RT run queue: a separate fixed-priority queue checked before all `SCHED_NORMAL` queues" at line 130, + "Kernel watchdog thread: spawn at `SCHED_FIFO`, `rt_priority = 99`" at line 134). (2) TODO-06 §4's richer `sched_class` {NORMAL,FIFO,RR,DEADLINE} + `rt_priority` model subsumes this section's `sched_policy` field (line 129); implementing a competing `sched_policy` now forces a later reconciliation -- the field is defined once, there, when the enforcement lands. (3) the `NtSetInformationProcess(ProcessSchedulingPolicy)` handler shares §4's ring-3 trust-boundary gap -- the same `NtSetInformationProcess_handler` derefs `*(uint32_t *)buffer` raw (`nt_process.c:620-628`) and resolves the target as a raw PID with no granted-access (`task_from_handle`, `nt_process.c:27-35`) -> XREF: `02-kernel-core/TODO-12-native-api-ssdt.md §29` (item: "[Critical] NtQuery/SetInformationProcess ... user pointers deref'd raw ... probe + copy_from_user/copy_to_user" at line 1221) + `02-kernel-core/TODO-12-native-api-ssdt.md §7` (item: "NtCreateProcess/NtOpenProcess/NtOpenThread return raw PID/TID/task-struct" at line 413). Re-enters when TODO-06 §4 RT classes + watchdog protection ship (then define `sched_policy`/`sched_class` once and wire the self-restricted, `copy_from_user`-probed API).

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

> **Deferred:** 2026-07-11 | Operator-reserved architecture decision. Design review (needs-attention, `.claude/overnight/reviews/20260711-123231-design.out`) blocks §6 as specified: a process `capabilities` bitmask is a SECOND privilege authority parallel to the shipped NT token-privilege model (`SeSinglePrivilegeCheck` / `sep_effective_token`, `src/kernel/security/privileges.c:149-185`, impersonation-aware). Before §6 can gate anything the operator must define ONE authoritative CAP-to-LUID composition + enforcement-owner table (capabilities as a denial-only ceiling ANDed with the effective-token check, OR CAP names mapped onto token LUIDs) -- a security-model decision expensive to reverse. Verified enforcement blockers to resolve in that redesign: (1) CAP_ALL seeding contaminates ring-3 -- boot shell (`src/kernel/main/boot_desktop.c:722` `task_create`) and NtCreateProcess staging (`src/kernel/nt/nt_process.c:95` `task_create`) build user-destined tasks via `task_create`, not `task_create_user`, so an explicit child ceiling must be threaded before ring-3 publication; (2) non-atomic drop races on SMP -- plain `uint64_t` load/AND/store needs an atomic acquire-load + `fetch_and` acq-rel + a single fork snapshot (SMP-from-day-one); (3) CAP_SET_TIME gate is incomplete + non-reporting -- `NtSetSystemTime`'s non-NULL `PreviousTime` branch calls `KeSetSystemTimeEx` directly (`src/kernel/time/wall_clock.c:912-922`), bypassing `KeSetSystemTime`, and both setters are `void` so a denial cannot return `STATUS_PRIVILEGE_NOT_HELD` (the correct fix is a `void`->`NTSTATUS` change across all setters/callers -- an ABI change, CLAUDE.md stop-and-ask); (4) CAP_REALTIME has a live bypass -- `NtSetInformationProcess(ProcessPriorityClass)` accepts `THREAD_PRIO_REALTIME` with no authorization (`src/kernel/nt/nt_process.c:620-628`) and the scheduler picks the strict-highest runnable priority (`src/kernel/sched/task.c:163-197`), a ring-3 starvation DoS. -> XREF `03-memory-concurrency/TODO-06-scheduler-enhancement.md §4` (items: "RT run queue: a separate fixed-priority queue checked before all `SCHED_NORMAL` queues" at line 130, "Kernel watchdog thread: spawn at `SCHED_FIFO`, `rt_priority = 99`" at line 134, and "`thread_set_sched(tid, class, rt_prio)` -- requires `CAP_SCHED_RT`" at line 133 which also reconciles §6's `CAP_SCHED_FIFO` name to the scheduler domain's `CAP_SCHED_RT`) -- the CAP_REALTIME / priority enforcement + watchdog protection land there. Re-enters when the operator rules on the capability-authority model; the enforcement gates then attach to that single authority.

## 7. Capability Inheritance and Drop-Only Policy

Capabilities can be inherited across `fork` / `exec` but can only be dropped, never gained. This makes privilege de-escalation auditable and prevents accidental escalation.

- [ ] At `task_fork()`: child inherits parent `capabilities` exactly
- [ ] At `task_exec()` via `exec_load()`: on EIF binaries, apply capability mask from EIF header flags; strip any bits not in the parent's set; never add new capabilities on exec (→ XREF `TODO-17 §1` -- EIF capabilities field is not yet defined in the spec; extend `eif_header_t` there before implementing here)
- [ ] `NtDropCapability(cap_mask)`: clear one or more capability bits from the calling process; irreversible for the lifetime of the process; register in SSDT (→ XREF `TODO-12 §5`)
- [ ] No syscall or path grants new capabilities -- escalation requires a restart or a privileged parent spawning with a specific mask
- [ ] Document the invariant: `child->capabilities ⊆ parent->capabilities` is enforced at fork and exec
- [ ] Commit: `"kernel: security -- capability inheritance and drop-only policy"`

**Test checkpoint:** `task_fork()` child inherits exact parent capabilities. `NtDropCapability(CAP_RAW_IO)` clears the bit; subsequent `capability_check(CAP_RAW_IO)` fails. Child drop does not affect parent. `NtDropCapability` is irreversible -- re-granting returns error. Serial log shows `"security: capability dropped -- cap=<N> remaining=0x<mask>"`. Test on: QEMU WHPX + TCG.

> **Deferred:** 2026-07-11 | Depends entirely on §6's capability model (inherit-at-fork, mask-at-exec, `NtDropCapability`, the `child->capabilities` subset-of `parent->capabilities` invariant). §6 is deferred pending the operator's capability-authority decision, so §7 cannot define inheritance/drop semantics on a model that does not exist yet. The drop path also carries §6 finding 2 (atomic `fetch_and` on the shared process-wide mask -- a plain load/AND/store loses concurrent restrictions on SMP), and the exec mask depends on the EIF capabilities header field that is still unspecified (already noted in-item as XREF `TODO-17 §1`). -> XREF `02-kernel-core/TODO-21-process-model-extensions.md §6` (item: "Add `uint64_t capabilities` to `struct task`; system processes get `CAP_ALL = ~0ULL` at kernel init"). Re-enters with §6.

## 8. Process Accounting Fields (Times, I/O Counters)

Both Win11 (`NtQueryInformationProcess` with `ProcessTimes`, `ProcessIoCounters`) and Linux (`getrusage`, `times(2)`) track per-process CPU time and I/O. These fields must exist in `struct task` before TODO-12 §10 can return meaningful `ProcessTimes`/`ProcessIoCounters`. VM/fault counters (`ProcessVmCounters`, page-fault counts, working set) are NOT owned here -- see the note below.

> [!NOTE]
> → XREF: `03-memory-concurrency/TODO-01-vmm-memory-protection.md §16` (item: "Add counters to `struct task`: `page_fault_count`, `working_set_pages`, `peak_working_set_pages`, `private_pages`") owns `ProcessVmCounters`, page-fault counts, minor/major fault classification, and residency-based working set. This section owns ONLY times + I/O + context-switch counters and does not add VM/fault fields.

**Accounting model (single-CPU-correct, SMP-approximate):** CPU time uses **statistical tick accounting** -- at each scheduler tick the interrupted ring (the tick ISR's own saved `CS`) decides whether the whole tick quantum is charged to `user_time_ns` or `kernel_time_ns` of the running task. This is classic Linux/NT tick-based accounting; it is exact on the single-CPU BSP scheduler today (a delta-since-last-schedule model over the coarse clock is NOT used -- it under-samples cooperative switches and mis-attributes DPC/callback work). Precise per-CPU cycle accounting is deferred to the SMP scheduler redesign (→ XREF `03-memory-concurrency/TODO-06-scheduler-enhancement.md` -- per-CPU run queues + per-CPU `task_current`). Counter fields are plain `uint64_t` accessed with `__atomic` builtins at RELAXED order (the repo's freestanding atomics convention -- there is no `_Atomic` qualifier precedent; independent monotonic counters need no multi-field snapshot coherence, so no acquire/release).

> [!NOTE]
> → XREF: `03-memory-concurrency/TODO-06-scheduler-enhancement.md §7` (item: "Increment counters at every context switch; distinguish voluntary ... from involuntary") owns per-THREAD ctxsw + CPU-time stats and the `/sys/sched` surface. This section owns the per-PROCESS aggregates on `struct task`; §7 should aggregate/consume these totals, not re-instrument the switch paths.

- [x] 10 per-process accounting fields on `struct task` (plain `uint64_t`, `__atomic` RELAXED; no `_Atomic` precedent): times, I/O count+bytes, ctxsw. Shared `task_init_accounting()` zeroes+stamps them from both create paths
- [x] `create_time_filetime` captured at `task_create()` via `KeQuerySystemTime()` IFF `wall_clock_time_sourced()`, else `FILETIME_NOW_PLACEHOLDER` -- stable, never recomputed (a `KeSetSystemTime`/NTP step cannot move it)
- [x] Tick ISR (`schedule()` top, before early-returns, guarded on `sched_enabled` + `system_get_freq()!=0`): charge quantum `NSEC_PER_SEC/freq` to `user_time_ns` ((CS&3)==3) or `kernel_time_ns` of `current_task`, every tick
- [x] Scheduler switch: `invol_ctxsw++` in `schedule()` (tick preempt), `vol_ctxsw++` in `schedule_now()` (yield/block) -- the entry path IS the switch reason
- [x] I/O counters bumped in `ob_file_read`/`ob_file_write` + the native `NtReadFile`/`NtWriteFile` paths on `task_current()` (actual transferred bytes); raw `vfs_read`/`vfs_write` NOT instrumented (kernel loaders call those)
- [/] `NtQueryInformationProcess(ProcessTimes/IoCounters)` ring-3 output DEFERRED: writing a ring-3 buffer is a kernel-write primitive until PTE-aware fault-recoverable usercopy exists (systemic NtQuery gap); fields ship, wiring in TODO-12 §10
- [/] `getrusage(RUSAGE_SELF)` helper -- DEFERRED: no `linux_syscall_table` / `struct rusage` exist yet (Linux-compat layer unbuilt); owner registers it in `10-platform-services/TODO-10-linux-compat.md §4`
- [ ] Commit: `"kernel: task -- process accounting fields for times and I/O counters"`

> [!NOTE]
> **Deferred within this section (XREF, not implemented here):** (1) Cross-process `ProcessTimes`/`ProcessIoCounters` via another process's handle -- `task_from_handle()` (`src/kernel/nt/nt_process.c`) currently casts `HANDLE`→PID and cannot safely resolve or ref-hold a *remote* `PROCESS_OBJECT`; blocked on typed process-handle lifetime (→ XREF `02-kernel-core/TODO-12-native-api-ssdt.md §7`, item: "process/thread lifecycle handlers"). (2) `getrusage(RUSAGE_CHILDREN)` -- accumulate from reaped children, fed by §15 reap path (→ XREF `02-kernel-core/TODO-21-process-model-extensions.md §15`, item: "Process Parenting, Reaping, and Wait Semantics"). Both land when their prerequisites ship; self-process accounting is the credible unit for this section.

**Test checkpoint:** After running a process, the per-task accounting fields accumulate: `user_time_ns`+`kernel_time_ns` grow over ticks (statistical -- either component may momentarily read zero while the sum tracks on-CPU ticks); `io_read_count`/`io_read_bytes` increase after a file read via a handle (native `NtReadFile` or `ob_file_read`); `vol_ctxsw`+`invol_ctxsw` increase across scheduling. `create_time_filetime` is a stable non-1601 FILETIME when the wall clock was sourced. (Ring-3 `NtQueryInformationProcess(ProcessTimes/IoCounters)` exposure is DEFERRED -- see the deferral note; the fields are the shippable unit.) Deterministic unit tests cover the FILETIME/quantum/ring math + CreateTime validity; behavioral charge/counter growth is serial-log-validated. No POST16 codes: post-Phase-3 runtime, so `klog` is the diagnostic surface. Test on: QEMU WHPX + TCG. Verify on bare metal -- ISR timing may differ.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | 4 accounting suites, 0 failures (deterministic FILETIME/quantum/ring math + CreateTime validity; behavioral charge/counters serial-log-validated on QEMU/bare metal)

> **Notes:**
> - **What shipped:** per-process times/I/O/ctxsw FIELDS on `struct task` (shared `task_init_accounting()`); statistical tick-charge + ctxsw in the scheduler; safe kernel-side I/O counters in `ob_file` + native `NtReadFile`/`NtWriteFile`
> - **How it integrates:** CPU time is statistical tick accounting (fixed `NSEC_PER_SEC/freq` quantum charged by interrupted ring); counters plain `uint64_t` + `__atomic` RELAXED; `CreateTime` a stable absolute FILETIME captured once at creation
> - **Scope boundary / deferred:** ring-3 `NtQuery(ProcessTimes/IoCounters)` -> `TODO-12 §10` pending PTE-aware fault-recoverable usercopy; VM/fault -> `TODO-01(mm) §16`; per-thread stats -> `TODO-06(mm) §7`; `getrusage` -> `TODO-10 §4`
> - **Canonical doc:** `include/kernel/sched/task.h` (accounting field block) + `src/kernel/sched/task.c` (`task_init_accounting` + tick charge)

> **Deferred:** 2026-07-11 | `[/]` `NtQueryInformationProcess(ProcessTimes/IoCounters)` ring-3 output -- copying a query result to a ring-3-supplied buffer via `copy_to_user` is a kernel-write primitive: `ProbeForWrite` has only an upper bound with no per-CR3 PTE check, and the kernel heap is identity-mapped inside the user VA window `[USER_ELF_BASE, MM_USER_PROBE_ADDRESS)`, so a ring-3 caller can aim the buffer at kernel memory (round-3 adversarial). Blocked on PTE-aware fault-recoverable usercopy; the whole NtQuery surface shares this systemic gap. The accounting FIELDS are populated + KernelMode-readable. Owner: -> XREF: `02-kernel-core/TODO-12-native-api-ssdt.md §10` (item: "`NtQueryInformationProcess` extended with 7 new info classes") + the systemic audit at `03-memory-concurrency/TODO-02-memory-security.md §4` (item: "Audit all syscall handlers: replace raw user-pointer dereference with `copy_from_user()` / `copy_to_user()`"). Re-enters when the usercopy hardening lands.

> **Deferred:** 2026-07-11 | `[/]` `getrusage(RUSAGE_SELF)` helper -- the Linux-compat syscall layer (`linux_syscall_table` + `struct rusage`) does not exist yet, so the wrapper cannot be written; the accounting FIELDS it consumes are shipped. Owner: -> XREF: `10-platform-services/TODO-10-linux-compat.md §4` (item: "`getrusage(98, who, usage)`"). Re-enters when the Linux-compat dispatch table lands.

## 9. Per-Process Resource Limits (rlimits)

Both Win11 (Job Object quotas + `QUOTA_LIMITS` via `NtQueryInformationProcess`) and Linux (`getrlimit`/`setrlimit`/`prlimit`) enforce per-process resource limits. This prevents runaway processes from exhausting system resources.

> [!NOTE]
> → XREF: `03-memory-concurrency/TODO-06-scheduler-enhancement.md §12` -- dynamic thread storage and thread admission are implemented in the scheduler domain. This section owns the quota/accounting surface; §12 consumes those limits so thread creation fails on real resource pressure instead of a fixed `THREAD_MAX` array ceiling.

This section ships the native rlimit STORAGE + a locked, privilege-aware accessor API. A faithful Windows `QUOTA_LIMITS` projection needs real VM/working-set counters and is owned by the unified quota authority; enforcement and the Linux syscalls are wired by their owners (below). The accessors are the ready call target for every deferred consumer.

> [!NOTE]
> Create-path inheritance (`task_rlimit_inherit` at `task_create`/`task_create_user`/`task_fork`) shares the task allocator's single-CPU-by-construction slot model with every other per-process inheritance (token, accounting, cwd): the child slot is fully written before `num_tasks++` publishes it, and `current_task` is the creator. Atomic slot reservation + per-CPU `current_task` are owned by `03-memory-concurrency/TODO-06-scheduler-enhancement.md §13`; when they land, all create-path inheritance becomes SMP-correct together.

- [x] `rlimit_t` + `RLIM_INFINITY` + Linux-UAPI-numbered `RLIMIT_*` (`CPU`=0..`AS`=9..`RTTIME`=15, `RLIM_NLIMITS`=16) in new `include/kernel/task_limits.h` -- Linux ABI numbering lets a future `getrlimit` index `rlimits[]`
- [x] `rlimit_t rlimits[RLIM_NLIMITS]` + leaf `spinlock_t rlimit_lock` on `struct task`; PID 0 seeded by `task_init_rlimits_defaults()` (8 MiB stack, 0 core, 256/4096 NOFILE, 8 MiB memlock; rest `RLIM_INFINITY`)
- [x] Inherit the creator's full array at `task_create()`/`task_create_user()`/`task_fork()` (defaults live at PID 0 only, so a lowered hard limit is not escapable via a child) and preserve it across `task_exec()`
- [x] Locked accessors `task_rlimit_get()`/`task_rlimit_set()` (irqsave snapshot/commit): lowering unprivileged, raising the hard limit needs `caller_privileged` (`SeIncreaseQuotaPrivilege`), `rlim_cur <= rlim_max` enforced
- [x] Commit: `"kernel: task -- per-process resource limits (rlimits)"`
- [/] Enforce `RLIMIT_AS` in `vmm_map_page()`/`sys_brk()` (reject when mapped pages would exceed the cap) -- deferred: needs per-process VM/page counters + the deferred §3 program break; owned here
- [/] Enforce `RLIMIT_CPU` in the timer-tick charger (joins §8 accounting in `schedule()`): `SIGXCPU`/terminate past `rlim_cur` seconds -- deferred hot-path follow-up owned here (needs ISR-context signal raise)
- [/] Wire `RLIMIT_MEMLOCK` as the pin ceiling (reject a lock past the cap) -- deferred (-> XREF: `TODO-12-native-api-ssdt.md §9` Virtual Memory, the `VirtualLock` pin path)
- [/] Wire `RLIMIT_CORE` as the crash-dump size gate (0 suppresses the dump) -- deferred (-> XREF: `TODO-27-crash-dump-generation.md`, the dump-writer size check)
- [/] Linux `sys_getrlimit`/`sys_setrlimit`/`sys_prlimit` -- deferred pending a `linux_syscall_table` registration point (same blocker as §8 getrusage); the accessors above are the ready call target
- [x] Windows `ProcessQuotaLimits` query/set shipped by the unified quota authority, projecting `RLIMIT_CPU` and reconciling `RLIMIT_NOFILE` with `handle_table.handle_limit` read-only (-> XREF: `TODO-25-kernel-resource-accounting-quotas.md §8`)
- [ ] Enforce working-set Min/Max so TODO-25 §9 pressure recovery can trim an offending process: needs the per-process VM/commit counters this section already owes `RLIMIT_AS`. -> XREF: `TODO-25-kernel-resource-accounting-quotas.md §9`

**Test checkpoint:** `task_rlimit_set(RLIMIT_NOFILE, {500,1500}, caller_privileged=0)` commits (lowering within the cap is unprivileged) and `task_rlimit_get` reads it back. `rlim_cur > rlim_max` returns `RLIMIT_ERR_INVAL`. Raising `rlim_max` with `caller_privileged=0` returns `RLIMIT_ERR_PERM` and leaves the hard limit intact; with `caller_privileged=1` it commits. Lowering `rlim_max` unprivileged is allowed. An out-of-range resource index returns `RLIMIT_ERR_INVAL` (get zeroes its output). PID 0 carries the 8 MiB stack / 4096 NOFILE-max defaults; the task running the suite carries the same inherited defaults. No POST16 (post-Phase-3 task code -- klog only). Test on: QEMU WHPX + TCG.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | 8 rlimit tests (roundtrip, cur>max, privilege matrix, bad-resource, PID0 defaults, inherit full-array, MEMLOCK ceiling) | 0 failures

> **Notes:**
> - **What shipped** -- `include/kernel/task_limits.h` (rlimit ABI + defaults) and `rlimits[RLIM_NLIMITS]` + `rlimit_lock` on `struct task`, with locked accessors `task_rlimit_get`/`task_rlimit_set` in `task.c` (irqsave snapshot/commit; privilege-aware).
> - **How it integrates** -- PID 0 gets defaults; every create/fork path inherits the creator's full array (preserved across `task_exec`); the accessors are the ready call target for the deferred syscalls/NT surface.
> - **Downstream effects** -- unblocks the deferred consumers once their owners land: `TODO-25 §8` (Windows `ProcessQuotaLimits` + NOFILE/handle-limit reconcile), `TODO-12 §9` (MEMLOCK pin), `TODO-27` (CORE dump gate).
> - **Canonical doc** -- `include/kernel/task_limits.h` header block (ABI numbering + scope/ownership map).
> - **Scope boundary** -- §9 owns rlimit STORAGE + accessors + inheritance; enforcement (AS/CPU) is a follow-up owned here, the Windows quota projection is `TODO-25 §8`, Linux `get/set/prlimit` await a `linux_syscall_table`.
> **Verified:** 2026-07-11 | commit `8e4bfc30` | 4/10 items | build OK | 8 rlimit tests PASS
> **Accepted:** [H] task-slot allocation race (a tick preempts ring-0 mid-`task_create`; two creators can claim the same `num_tasks` slot) -- pre-existing, systemic across all per-process inheritance -> XREF: `03-memory-concurrency/TODO-06-scheduler-enhancement.md §13` (item: "Atomic task-slot CLAIM" at line 405)
> **Quality reviewed:** 2026-07-11 | Codex 6x (design, adversarial, re-adversarial, consistency, perf) | 2H+6M fixed, 2H accepted-XREF | scope: kernel-code-quality

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

> **Deferred:** 2026-07-11 | Codex design review (`.claude/overnight/reviews/20260711-224907-design.out`, each claim verified at file:line via `superpowers:receiving-code-review`) confirms §10 cannot ship a credible core. The scheduler selects runnable threads by state + priority only (`src/kernel/sched/task.c:58-69,153-213`) and `struct thread` has no `affinity_mask` field (`include/kernel/sched/task.h:103-156`, grep of `src/kernel` is empty), so a stored process affinity mask would be ignored by dispatch: `SetProcessAffinityMask(0x1)` would falsely report success while the process still runs on any CPU -- the identical false-success failure already visible in the `ThreadAffinityMask` stub that returns `STATUS_SUCCESS` without even storing the mask (`src/kernel/nt/nt_process.c:472-474`). Storing a scheduler-ignored mask is a forbidden half-measure (never ship broken). The field + mask-aware `rq_enqueue()` + migration/requeue are owned by the scheduler domain, itself blocked on per-CPU run queues; the `NtSet/QueryInformationProcess(ProcessAffinityMask)` handlers would additionally inherit §4/§5's systemic trust-boundary gap -- raw user-buffer deref (`src/kernel/nt/nt_process.c:548-628`) and raw-PID handle resolution with no granted-access check. Blockers: -> XREF: `03-memory-concurrency/TODO-06-scheduler-enhancement.md §6` (item: "Add `affinity_mask` (`uint64_t`, bit N = CPU N allowed) to `task_t`; default `0xFFFFFFFFFFFFFFFF` (all CPUs)" at line 161, + "In `rq_enqueue()`: if `affinity_mask` has exactly one CPU bit set, force enqueue to that CPU's run queue" at line 162; itself blocked on TODO-06 §1 per-CPU run queues) + `02-kernel-core/TODO-12-native-api-ssdt.md §29` (item: "[Critical] NtQuery/SetInformationProcess ... input/output user pointers deref'd raw ... probe + copy_from_user/copy_to_user via kernel bounce buffers" at line 1221) + `02-kernel-core/TODO-12-native-api-ssdt.md §7` (item: "[Critical] NtCreateProcess/NtOpenProcess/NtOpenThread return raw PID/TID/task-struct, not OB PROCESS/THREAD objects ... Use real OB objects + rights" at line 413). Re-enters when TODO-06 §1+§6 provide per-CPU queues + mask-aware dispatch + migration/requeue; on re-entry, reject a zero effective mask atomically (do not blindly intersect every thread mask) and test actual CPU residency with a mask excluding at least one online CPU (not mask round-tripping).

## 11. Per-Process Mitigation Policy

Win11 provides `SetProcessMitigationPolicy` to control per-process security features: DEP enforcement mode, mandatory ASLR, CFG strictness, child process creation restrictions, image load restrictions. Linux uses `prctl` with `PR_SET_NO_NEW_PRIVS`, `PR_SET_SECCOMP`, etc. Impossible OS needs a unified per-process mitigation flags field that coordinates with the security features implemented in other TODOs.

> [!NOTE]
> → XREF: `TODO-10-kernel-security-hardening.md §1` (NX/DEP), `TODO-17-binary-system.md §15` (ASLR), `TODO-17 §12` (CFG), `TODO-12-native-api-ssdt.md §25` (syscall filtering). This section defines the per-process flags and API surface; enforcement is authoritative in those TODOs. This is a deliberate SUBSET of Win11's ~21 `PROCESS_MITIGATION_*` policies -- the high-value ones with an enforcement owner in-tree; CET (IBT/SHSTK) enforcement → `TODO-10-kernel-security-hardening.md §10`, dynamic-code and image-signature policy → `TODO-19-code-integrity-trust-policy.md`.

> Design rule adopted (each flag ships WITH its enforcement, never as dormant state): a mitigation bit reported as active but not actually enforced is false security. Only `MIT_NO_CHILD_PROCESS` has a live in-tree enforcement owner (child creation), so it is the only bit that ships; every other Win11 policy is added to `mitigation_policy.h` in the same change that wires its enforcement.

- [x] `uint64_t mitigation_flags` on `struct task` + `task_mitigation_apply`/`_get` accessors (`__atomic` ACQ_REL/ACQUIRE, monotonic); only `MIT_NO_CHILD_PROCESS (1<<3)` defined (`nt/mitigation_policy.h`)
- [/] `NtSetInformationProcess(ProcessMitigationPolicy)` deferred: reading a range-only-probed ring-3 buffer via non-fault-recoverable copy_from_user is an unprivileged kernel-crash DoS; NOT_SUPPORTED -> XREF: `TODO-23 §13` try_copy_from_user
- [/] `NtQueryInformationProcess(ProcessMitigationPolicy)` deferred: copy_to_user to a range-only-probed buffer is a kernel-write primitive (kernel heap identity-mapped low); NOT_SUPPORTED -> XREF: `TODO-12 §29` + `TODO-02(mm) §4`
- [x] Enforce `MIT_NO_CHILD_PROCESS` in `NtCreateProcess` (`STATUS_CHILD_PROCESS_BLOCKED` -> `ERROR_CHILD_PROCESS_BLOCKED` 367) + `task_fork` (fail -1); checked against creator before allocation
- [x] Inherit `mitigation_flags` from parent at `task_fork` via a single acquire snapshot committed before `num_tasks++`; zero-init on `task_create`/`task_create_user`
- [/] DEP/ASLR/CFG/NO_REMOTE_IMAGES/NO_LOW_INTEGRITY flags deferred (no enforcement = false security) -> XREF: `TODO-10 §1` NX/DEP + `TODO-17 §15` ASLR + `§12` CFG + `TODO-19 §9` image enforcement
- [/] `MIT_NO_NEW_PRIVS` deferred: exec-cannot-gain-caps needs the capability model -> XREF: `TODO-21 §6` (item: "Add `uint64_t capabilities` to `struct task`") + §7
- [/] Win32 `SetProcessMitigationPolicy` A/W wrapper is user-mode surface -> XREF: `TODO-08-win32-api-surface.md §4`
- [x] Commit: `"kernel: task -- per-process mitigation policy flags"`

**Test checkpoint:** the kernel helper `task_mitigation_child_set(NoChildProcessCreation)` sets `MIT_NO_CHILD_PROCESS`; a later clear attempt returns -1 with the bit intact; `task_mitigation_apply` is monotonic (an unrelated OR never clears it). A process carrying the bit is blocked from `NtCreateProcess` (`STATUS_CHILD_PROCESS_BLOCKED`) and `task_fork` (silent fail -1, reject before the capacity check). A fork child inherits the parent's `mitigation_flags`. Both ring-3 `NtSet/QueryInformationProcess(ProcessMitigationPolicy)` handlers return `STATUS_NOT_SUPPORTED` (deferred -- the fault-safe usercopy the per-selector validation needs is owned by TODO-23 §13). No POST16 (post-Phase-3; klog is the diagnostic surface). Unit tests: 6 `ProcExt:` mitigation suites (TEST_CAT_SCHED) cover the accessors + child-policy decision; `MIT_NO_CHILD_PROCESS` enforcement is serial-validated on WHPX/TCG. Test on: QEMU WHPX + TCG.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | 6 ProcExt mitigation suites, 0 failures

> **Notes:**
> - **What shipped** -- reduced §11 core: `mitigation_flags` + `MIT_NO_CHILD_PROCESS` (sole enforced flag) + `task_mitigation_apply/get/child_set` accessors + `nt/mitigation_policy.h` ABI; both ring-3 handlers defer (usercopy).
> - **How it integrates** -- monotonic atomic flag (ACQ_REL set / ACQUIRE get); enforced in `NtCreateProcess` + `task_fork`; inherited via one fork snapshot before `num_tasks++`; Codex design + adversarial adoptions in the commit message.
> - **Downstream effects** -- added `STATUS_CHILD_PROCESS_BLOCKED` (0xC000049D) + its `ERROR_CHILD_PROCESS_BLOCKED` (367) DOS-error mapping to the NTSTATUS tables.
> - **Canonical doc** -- `include/kernel/nt/mitigation_policy.h` (ABI + `MIT_*` bits + the flag-ships-with-enforcement rule).
> - **Scope boundary** -- §11 owns the field + child-process policy; DEP/ASLR/CFG/image enforcement -> TODO-10 §1 / TODO-17 §15,§12 / TODO-19 §9; `MIT_NO_NEW_PRIVS` -> §6; Win32 A/W wrapper -> TODO-08 §4.
> **Verified:** 2026-07-12 | impl `a22dabc1` + this review commit (both ring-3 handlers deferred) | 3/8 items | build OK | 6 ProcExt tests PASS | smoke PASS (TCG 6.0s)
> **Accepted:** [H] both ring-3 handlers return `STATUS_NOT_SUPPORTED` -- reading/writing a range-only-probed ring-3 buffer via the non-fault-recoverable `copy_from_user`/`copy_to_user` is an unprivileged kernel-crash (unmapped) / kernel-corruption (identity-mapped) path, systemic across every SSDT user-buffer handler; the field + enforcement + fork inheritance ship -> XREF: `02-kernel-core/TODO-23-exception-dispatch-seh.md §13` (item: "`include/kernel/probe.h` -- `ProbeForRead`, `ProbeForWrite`, `try_copy_from_user`, `try_copy_to_user`" at line 52)
> **Quality reviewed:** 2026-07-12 | Codex 11x (design, adversarial, re-adversarial, consistency, perf) + kernel-quality-auditor | 2H+3M fixed, 1C+1H accepted-XREF | scope: kernel-code-quality

## 12. Pledge/Unveil-Style Process Restriction

> [!TIP]
> Neither Windows nor Linux provides a simple, auditable process restriction API. Windows has `SetProcessMitigationPolicy` (limited to security flags) and restricted tokens (complex). Linux has seccomp-bpf (requires writing BPF programs) and Landlock (filesystem only). OpenBSD's `pledge()` and `unveil()` are widely admired for their simplicity: a single syscall restricts what a process can do, irreversibly. Impossible OS provides both, unified with the capability model.

- [x] `NtPledge(UNICODE_STRING *promises)` (SSDT `0x03DB`): tighten-only category pledge via `pledge_apply` CAS intersect -- a second pledge only narrows, never widens (design fix: an OR mask would expand privilege).
- [x] `uint64_t pledge_mask` in `struct task`: bit 63 `PLEDGE_PLEDGED` sentinel + bits 0-8 allowed categories; zeroed on slot reuse, inherited across fork/create.
- [x] Coarse `pledge_check_syscall` in `ssdt_dispatch` after `syscall_filter` (user-mode only): unclassified DENY, core allowed, file-open reaches the handler; terminate via `task_exit` after audit close (-> XREF TODO-12 §25).
- [x] Fine file checks map ACCESS_MASK/info-class to `rpath`/`wpath`/`cpath`: `pledge_check_file` (NtCreateFile/NtDeleteFile) + `pledge_check_setinfo` (NtSetInformationFile) -- the dispatcher sees only the service number.
- [x] Legacy INT 0x80 ABI gated too (`pledge_check_legacy` in `syscall_handler` + fine `unveil_check` in SYS_OPENFILE/READFILE/READDIR) -- SSDT-only enforcement was bypassable via INT 0x80 (adversarial fix).
- [x] `NtUnveil(UNICODE_STRING *path, *permissions)` (SSDT `0x03DC`, subset of `rwxc`): `unveil_check` at the 4 path NT handlers (not pure `vfs_resolve_path`); `NtUnveil(NULL,NULL)` locks; deny = `STATUS_ACCESS_DENIED`.
- [x] `unveil_entry_t *unveil_list` in `struct task` (leaf `unveil_lock`): folded (`vfs_path_fold`) prefixes matched on a component boundary, longest wins (design fix: `C:\AllowedEvil` matched under `C:\Allowed`); freed at reap.
- [x] Irreversible: pledge intersects tighten-only, unveil set locks. Fork clones pledge_mask + unveil list before `num_tasks++` (OOM aborts fork); NtCreateProcess inherits into the child (design fix: else child escapes).
- [x] `STATUS_PLEDGE_VIOLATION` = `0xE0000201` (customer NTSTATUS; no Windows equivalent; avoids the `0xE0000001` bugcheck). Registered via `pledge_register_ssdt` (-> XREF TODO-12 §5).
- [x] Commit: `"kernel: task -- pledge/unveil process restriction (OpenBSD-inspired)"`

**Test checkpoint:** After `NtPledge("stdio rpath")`, `NtCreateFile` for write returns `STATUS_PLEDGE_VIOLATION` and the process terminates (`klog(LOG_WARN, "pledge", "pledge violation -- syscall 0x%X not in pledge set ...")`). After `NtUnveil("C:\\Impossible\\System", "rx")` then `NtUnveil(NULL, NULL)`, reading under `C:\Impossible\System` succeeds; reading `C:\Users\` returns `STATUS_ACCESS_DENIED`. No POST16 (post-Phase-3 syscall path; klog is the diagnostic surface). The pure decision core (parse, tighten intersection, classifier, file/unveil category mapping, folded component-boundary matching) is unit-tested (TEST_CAT_SCHED); the terminate-on-violation behavior is serial-validated on QEMU WHPX + TCG. Test on: QEMU WHPX + TCG.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | 14 ProcExt pledge/unveil suites, 0 failures

> **Notes:**
> - Shipped `src/kernel/nt/pledge.c` + `pledge.h`: `NtPledge`/`NtUnveil` (SSDT `0x03DB`/`0x03DC`) with a pure unit-tested core (parse, tighten-intersect, classifier, folded boundary match) and a task-aware live API.
> - Split enforcement: coarse category check in `ssdt_dispatch` (after the §25 bitmap filter, terminate after audit close); fine ACCESS_MASK/info-class + `unveil_check` in the 4 path-resolving NT file handlers, not in pure `vfs_resolve_path`.
> - `struct task` gains `pledge_mask` (tighten-only CAS intersect) + a leaf-lock unveil list; zeroed on slot reuse, inherited fail-closed across fork and NtCreateProcess, freed at the reap barrier.
> - Design review reshaped the model (intersect-not-OR, unclassified-deny, terminate-after-audit, fork/create inheritance, folded boundary match); adoptions in commit `<hash>`.
> - Canonical doc: OpenBSD `pledge(2)`/`unveil(2)` semantics; in-tree contract in `include/kernel/nt/pledge.h`.
> - Scope boundary: §12 owns pledge/unveil; `inet`/`dns`/`tty` map to no syscall until those subsystems land; the §25 per-index bitmap filter is separate and complementary.
> **Verified:** 2026-07-12 | commit `b929d91f` | 9/9 items | build OK | 425 sched + 114 fs + 282 ipc PASS | smoke PASS
> **Accepted:** [H] pledge_terminate sibling-CPU quiescence: `task_exit` marks TASK_DEAD with no sibling-stop barrier (pre-existing; all `task_exit` callers) -> XREF: 02-kernel-core/TODO-21 §14 (item: "Coordinated SMP process termination" at line 399)
> **Accepted:** [H] child publication vs pledge/unveil inheritance ordering: NtCreateProcess publishes before inheriting (entry==0 mitigates), and task_fork inherits early then publishes without revalidating a concurrent tighten -- both need the atomic inherit-and-revalidate-before-publish construction -> XREF: 02-kernel-core/TODO-21 §14 (item: "Unpublished-child construction" at line 400)
> **Accepted:** [H] aliased/same-handle `FILE_OBJECT.path` goes stale after rename (needs node-shared canonical path; same-handle path-mutating setinfo now fails closed on a stale handle as an interim) -> XREF: 02-kernel-core/TODO-12 §13 (item: "`FILE_OBJECT` canonical-path sync across ALIASED handles on rename" at line 663)
> **Deferred:** [M] two heap-allocation optimizations (tail-pack `FILE_OBJECT.path`; variable-length `unveil_entry`) (reason: perf, code correct + bounded) -> XREF: 02-kernel-core/TODO-12 §13 (item: "Tail-pack `FILE_OBJECT.path` into the object-manager allocation" at line 665)
> **Deferred:** [M] finer NtSetInformationFile ACCESS_MASK precision (DELETE vs WRITE) beyond the interim any-write-access gate now enforced -> XREF: 02-kernel-core/TODO-12 §13 (item: "`NtSetInformationFile` NT ACCESS_MASK enforcement" at line 667)
> **Quality reviewed:** 2026-07-12 | Codex 22x (design, adversarial, re-adversarial, consistency, perf) | ~21H+6M fixed, 3H accepted-XREF, 3M deferred | scope: kernel-code-quality + kernel-quality-auditor (no C/H)

---

## 13. Job Object Syscalls Wired to SSDT

Register Job Object management syscalls in the SSDT for process-group resource control. Win11 Job Objects are the primary mechanism for process-group resource limits (CPU rate, memory cap, I/O throttle). Linux uses cgroups v2 for equivalent functionality. (→ XREF: TODO-12-native-api-ssdt.md §5)

- [x] `NtCreateJobObject` → SSDT 0x0160 -- `nt_job.c` handler + `ob_job_create` (anon/named via `\BaseNamedObjects` with collision redirect); `ObpJobType` registered in `ob_init`
- [x] `NtOpenJobObject` → SSDT 0x0161 -- opens an existing named job (`ob_job_open`)
- [x] `NtAssignProcessToJobObject` → SSDT 0x0162 -- `ob_job_assign`: fail-closed on terminated/active-process-limit/full; typed `task->job` holds one Ob ref per membership
- [x] `NtTerminateJobObject` → SSDT 0x0163 -- `ob_job_terminate`: snapshot-then-kill all members via centralized `task_terminate_remote`
- [x] `NtQueryInformationJobObject` → SSDT 0x0164 -- Basic/Io accounting, Basic/Extended limit, BasicProcessIdList (partial-fill `STATUS_BUFFER_OVERFLOW`)
- [x] `NtSetInformationJobObject` → SSDT 0x0165 -- Basic/Extended limit; rejects unenforceable LimitFlags (`STATUS_NOT_SUPPORTED`, no false-containment)
- [x] `NtIsProcessInJob` → SSDT 0x0166 -- any-job (NULL handle) + specific-job membership
- [x] `NtCreateJobSet` → SSDT 0x0167 -- registered + arg-validated; empty set succeeds; real job-set scheduling deferred (item below)
- [x] All functions return `NTSTATUS`
- [x] Commit: `"kernel: wire Job Object syscalls to SSDT (0x0160-0x0167)"`
- [ ] `JobObjectCpuRateControlInformation` (JOBOBJECT_CPU_RATE_CONTROL_INFORMATION): the Win10/11 CPU-throttling surface real tools use; distinct from `QUOTA_LIMITS_EX.CpuRateLimit`. -> XREF: `TODO-25-kernel-resource-accounting-quotas.md §8`
- [ ] Enforce memory limits (`PROCESS_MEMORY`/`JOB_MEMORY`): add to `JOB_SUPPORTED_LIMIT_FLAGS` + enforce on commit once VMM per-process accounting exists (today rejected at set)
- [ ] Enforce CPU-rate limit (`JOBOBJECT_CPU_RATE_CONTROL_INFORMATION`): per-job scheduler quota (→ XREF: 03-memory-concurrency/TODO-06-scheduler-enhancement.md §2, "starvation prevention" line 93)
- [ ] Real job-set scheduling (`NtCreateJobSet` NumJob>0, today `STATUS_NOT_IMPLEMENTED`): needs scheduler group support (→ XREF: 03-memory-concurrency/TODO-06-scheduler-enhancement.md §2)
- [ ] Nested-job topology: parent/child refs, subset-valid parentage + cycle check, chain-wide inheritance/breakaway, teardown, chain snapshot for aggregate charge. -> XREF: `TODO-25-kernel-resource-accounting-quotas.md §3`.

**Test checkpoint:** `NtCreateJobObject` creates a job (serial: `"job: created job handle 0x<h>"`). `NtAssignProcessToJobObject` assigns a process (serial: `"job: assigned pid <N> to job"`). `NtQueryInformationJobObject` returns accounting (process counts + summed CPU time). `NtTerminateJobObject` kills all members. `NtSetInformationJobObject` rejects an unenforceable limit flag. Test on: QEMU WHPX + TCG.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | 7 ProcExt job suites, 0 failures (limit-flag validation, accept/reject, membership scan, pid-list partial fill, accounting counts)

> **Notes:**
> - **What shipped** -- `ob_job.c`/`ob_job.h` (JOB_OBJECT: dense `member_pids[TASK_MAX]` + one spinlock + one Ob ref per membership) + `nt_job.c` (8 SSDT handlers) + typed `task->job`.
> - **How it integrates** -- `ob_job_type_init` in `ob_init`, `nt_job_register_ssdt` in `boot_desktop.c`; every process-death path detaches via `task_terminate_remote`/`ob_job_detach_task`; fork inherits before child publication, fail-closed.
> - **Downstream effects** -- SSDT rows 0x0160-0x0167 now have handlers; SYS_KILL + signal-kill gained the previously-missing `ob_process_mark_dead`. Design + adversarial adoptions in commit `01a14422`.
> - **Canonical doc** -- `include/kernel/ob/ob_job.h` (reference-ownership invariant + lifecycle contract).
> - **Scope boundary** -- §13 owns lifecycle + accounting + active-process-limit + kill-on-close; CPU/memory/IO enforcement + job sets are the open items above; process-handle access-checks owned elsewhere (see Deferred).
> - **OB type-table headroom (2026-07-12 landing fix)** -- registering the permanent `Job` object type tipped the append-only global type table to 32/32 (on top of the ob test suite's accumulated throwaway types in one boot) and failed "OB: namespace locking stress". Raised `OB_MAX_TYPES` 32 -> 64 (`include/kernel/ob/ob.h`) for NT-scale headroom + a Layer-1 `_Static_assert` pinning `sizeof(OBJECT_TYPES_INFORMATION) <= 4096`. Safe: `NtQueryObject(ObjectTypesInformation)` is size-negotiated (writes `number_of_types` entries, returns the exact needed length) with no external user-mode consumer, so a larger array cannot truncate or break a caller. Scoped Codex adversarial review of this delta: approve, no material findings. Landing evidence: build OK; full suite 20456 kernel + 16 user PASS (incl. 7 ProcExt job suites + OB namespace-stress now green); smoke PASS (TCG 2.9s). [/] partial: memory/CPU-rate limits + real job-set scheduling stay deferred to the items above w/ scheduler XREF.

> **Deferred:** 2026-07-12 | Codex design review (needs-attention: 4 High + 1 Medium, each verified at file:line via `superpowers:receiving-code-review`). Adopted: typed `task->job` + per-membership Ob ref (KILL_ON_JOB_CLOSE in on_close, not on_delete); centralized `task_terminate_remote` so remote kills detach job membership; fork-inherit before publication (fail-closed); reject-don't-echo unenforceable limits. Accepted out-of-scope: [High] process handles are raw PIDs with no granted-access check -- `proc_from_handle()` (`nt_job.c`) mirrors the codebase-wide PID model, so assign/is-in-job cannot enforce `PROCESS_SET_QUOTA`/`PROCESS_TERMINATE` rights; owned by the process-object handle model -> XREF: `02-kernel-core/TODO-12-native-api-ssdt.md §7` (item: "NtCreateProcess/NtOpenProcess/NtOpenThread return raw PID/TID/task-struct, not OB PROCESS/THREAD objects" at line 413) + `02-kernel-core/TODO-05-object-manager.md §2` (item: "Reference Counting and Object Lifetime" at line 112). Adversarial rounds surfaced several SMP membership races, all FIXED via a per-task `job_lock` serializing every `t->job` read/write (lock order `job_lock` -> the JOB_OBJECT spinlock): assign-vs-exit (dying task left a permanent member), the bulk-kill and assign-rollback raw-pointer UAFs (a task's membership reference keeps the job alive until its own detach drops it, so no assignment frees the job under a concurrent detacher), fork-inherit pinning the parent job under `parent->job_lock` + a temp ref, and every death path (task_exit, task_terminate_remote, task_wrapper normal return, NtCreateProcess OOM) routing through the centralized teardown that detaches membership. DEFERRED (needs a task-publication lock, entangled with the pre-existing unlocked `num_tasks++`): the terminate-vs-unpublished-child race (an inherited child not yet published is skipped by termination) -> XREF: this file §14 (item: "Job membership vs publication SMP").

> **Verified:** 2026-07-12 | commit `01a14422` | 9/12 items ([/] partial: memory/CPU-rate limit enforcement + real job-set scheduling deferred w/ scheduler XREF) | build OK | full suite 20456 kernel + 16 user PASS (incl. 7 ProcExt job suites + OB namespace-stress green) | smoke PASS (TCG 2.9s)
>
> **Quality reviewed:** 2026-07-12 | Codex adversarial (overnight run 2026-07-12: "no material findings") + consistency ("approve") + scoped OB_MAX_TYPES delta review ("approve, no material findings") | UAF-class findings fixed via per-task `job_lock`; 1H accepted-XREF (raw-PID process handles) + terminate-vs-unpublished-child SMP race deferred to §14 | scope: kernel-code-quality

## 14. Process Exit Cleanup

Central cleanup point for all per-process resources when a process terminates. Windows calls this from `PspExitProcess()` -- it walks every subsystem that holds per-process state and releases it. Without this, resources leak on process exit. Impossible OS splits the walk across the DEAD transition (`task_death_teardown`, shared by every death path) and the off-CPU reap barrier (`task_cleanup`): resources reclaimable immediately go at death; stacks/CR3/page tables wait for the reap where the task is proven off-CPU.

- [/] Per-process resource release via `task_death_teardown(struct task *)`, a shared DEAD-transition helper wired into all four death paths; reap-barrier memory (stacks/CR3/PML4) stays in `task_cleanup`:
  - [x] Release timer-resolution requests held by this PID -- `timer_resolution_release_process()` bulk-clears the pid's slots and re-arbitrates (-> XREF: `TODO-08-time-filetime-management.md §8` `KeSetTimerResolution`, which accepted process-exit reaping to here)
  - [x] Close all open handles in the process handle table -- `ob_handle_table_destroy` at the reap barrier + a cleanup-count log (-> XREF: `TODO-05-object-manager.md §3` OB handle table)
  - [x] Release share-mode entries + trigger delete-on-close -- driven by `ObpFreeHandle -> file_on_close -> vfs_close` during the handle sweep (-> XREF: `05-storage-filesystems/TODO-04 §8` `vfs_open_handle_t`, `§9` delete-on-close)
  - [x] Free per-process address space -- user stack, TLS expansion, secondary-thread stacks/TEBs, and the whole per-process PML4 via `vmm_destroy_user_pml4` at the reap barrier
  - [x] Release security tokens (primary + per-thread impersonation) and detach from Job Object -- `ob_job_detach_task` now fires on ALL death paths via `task_death_teardown` (-> XREF: `§13`)
  - [x] Per-process rlimits + accounting are plain `struct task` fields reclaimed with the slot -- no separate release call (-> XREF: `§8`, `§9`)
  - [ ] Release all byte-range locks held by this process -- BLOCKED: `vfs_lock_file` is per-node with no per-process index; needs a per-task lock-ownership list populated at lock time and swept here (-> XREF: `05-storage-filesystems/TODO-04 §10` `vfs_lock_file`)
  - [ ] Release oplocks held by this process -- deferred: `vfs_close` leaves oplock state untouched; a dead process's oplock breaks lazily on the next opener's `vfs_open`. Eager release needs the same per-task node index (-> XREF: `05-storage-filesystems/TODO-04 §14`)
  - [ ] Invoke ELF `DT_FINI_ARRAY` / PE `DLL_PROCESS_DETACH` destructors and deregister modules -- BLOCKED: no `exec_unregister_module` exists and the fini-array runtime is unimplemented (-> XREF: `02-kernel-core/TODO-17 §6` module registry, `§20` fini-array)
  - [ ] Free the PEB/TEB physical frames -- BLOCKED: mapped at fixed shared VAs in the kernel PML4; freeing needs per-task phys tracking + the per-process-private-frame refactor (-> XREF: `02-kernel-core/TODO-11 §24` PEB/TEB lifecycle)
- [x] Log cleanup counts -- `task_cleanup` logs `"PID %u reap cleanup: %u handles closed"` at PASSIVE_LEVEL (`task_death_teardown` stays log-free for its raised-IRQL callers)
- [ ] Unpublished-child pledge/unveil generation revalidate at publish -- DEFERRED, only racy under mid-`task_fork` preemption the single-cursor scheduler cannot expose (-> XREF: `03-memory-concurrency/TODO-07-smp-phase2.md §3`)
- [ ] Coordinated SMP termination (sibling-stop reap barrier before CR3/stack free) -- DEFERRED, no cross-CPU rendezvous primitive (-> XREF: `03-memory-concurrency/TODO-07-smp-phase2.md §2` TLB-shootdown IPI, `§3`)
- [ ] Job membership vs publication lock (`num_tasks++` committed atomically) -- DEFERRED, needs a tasks-publication lock; races only under true concurrency (-> XREF: `03-memory-concurrency/TODO-07-smp-phase2.md §3`)
- [ ] `JOBOBJECT_BASIC_ACCOUNTING_INFORMATION.TotalPageFaultCount` is never assigned and no per-task fault counter exists, so every job reports 0 -- indistinguishable from no faults. -> XREF: `TODO-25-kernel-resource-accounting-quotas.md §7`
- [ ] `ThisPeriodTotalUserTime`/`KernelTime` are hard-set to the lifetime totals (`ob_job.c:918`); Windows resets them on a limit set unless `JOB_OBJECT_LIMIT_PRESERVE_JOB_TIME`, which we accept but never honor. -> XREF: `TODO-25 §19`
- [x] Self-directed `SYS_KILL` routes through the non-returning `task_exit` (never resumes in ring-3), so a self-killed task cannot issue further syscalls; `task_terminate_remote` stays remote-only
- [ ] `ob_job_create` inserts a named job into `\BaseNamedObjects` BEFORE `ObpAllocateHandle`, so a handle-alloc failure leaks the directory entry, the `JOB_OBJECT` and its quota block. -> XREF: `TODO-25-kernel-resource-accounting-quotas.md §3`
- [ ] Wire `signal_check()` into the scheduler / kernel-entry boundary: it is defined but never called, so `signal_default_action` (and its `task_death_teardown` leg) is dormant -- fatal signals set a pending bit nothing dispatches
- [ ] Remote/signal death (`task_terminate_remote`, `signal_default_action`) skips the thread-0 APC rundown + THREAD_DEAD publish the self-death paths do inline; run it once the target is stopped so queued thread-0 APCs are not leaked
- [x] Commit: `"kernel: task -- process exit cleanup (timer-res reap, shared death teardown, cleanup log)"`

**Test checkpoint:** A process that requests a fast timer resolution and exits (via any death path) has its request reaped and the tick re-arbitrated; handles/share-modes/delete-on-close/tokens/job-membership/address-space are released at exit; the serial log shows the reap handle count. Unit tests: 2 `time:` suites (TEST_CAT_SCHED) cover the timer-res reap no-op + round-trip. Byte-range-lock / oplock / PEB-TEB-frame / SMP-barrier release are tracked as concrete follow-ups above. Test on QEMU WHPX, TCG.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | 2 timer-res reap suites, 0 failures

> **Notes:**
> - Shipped `timer_resolution_release_process()` (bulk per-pid slot reap + re-arbitrate, no-restore-on-refuse for a dead owner) and `task_death_teardown(struct task *)`, a shared DEAD-transition helper wired into all four death paths.
> - Self-directed `SYS_KILL` routes through the non-returning `task_exit` so a dead task cannot recreate a reaped slot; the fatal-signal leg is wired but dormant until `signal_check` delivery lands.
> - Most per-process resources were already freed (handles, tokens, pledge/unveil, stacks, PML4); the real gaps were the timer-res leak + firing job-detach on every death path. Codex design + adversarial adoptions in the commit message.
> - Canonical doc: `include/kernel/time/timer_resolution.h` + `include/kernel/sched/task.h` (`task_death_teardown`).
> - Scope boundary: §14 owns the death-path release walk; byte-lock/oplock -> TODO-04, PEB/TEB frames -> TODO-11 §24, fini/module-deregister -> TODO-17, SMP barrier + tasks-lock -> TODO-07.
> **Verified:** 2026-07-12 | commit `55678cd7` | 8/18 items | build OK | tests 20465/20465 PASS
> **Accepted:** [H] remote/self death has no off-CPU SMP reap barrier -- task_cleanup can free CR3/handle-table under a still-running victim (incl. concurrent NtClose vs ob_handle_table_destroy) on real SMP (reason: not-functional-today on the single-cursor scheduler) -> XREF: 03-memory-concurrency/TODO-07-smp-phase2.md §3 (item: "task_cleanup reap barrier: prove a TASK_DEAD task is off-CPU on ALL CPUs" at line 122)
> **Deferred:** [M] fatal-signal death path dormant -- `signal_check()` is never called, so `signal_default_action` + its `task_death_teardown` leg never run -> XREF: 02-kernel-core/TODO-21-process-model-extensions.md §14 (item: "Wire `signal_check()` into the scheduler / kernel-entry boundary" at line 457)
> **Quality reviewed:** 2026-07-12 | Codex 9x (design + adversarial + re-adversarial + consistency + perf) | 5H+2M fixed, 1H+3M+1L accepted-XREF | scope: kernel-code-quality

---

## 15. Process Parenting, Reaping, and Wait Semantics

Exit cleanup (§14) frees resources but does not define WHEN exit status is observable, WHO reaps an orphan, or how competing waits serialize. `task_waitpid` today accepts one exact child PID, blocks unconditionally, and immediately reaps (`task.c`), so it cannot support `waitpid(-1)`, `WNOHANG`/`WUNTRACED`/`WCONTINUED`, `waitid`, `wait4` child usage, or subreaper adoption. This is the child-lifecycle contract §14 depends on.

- [ ] Add a `TASK_ZOMBIE` state (distinct from the transient `TASK_DEAD` mark): on exit, transition RUNNING -> ZOMBIE retaining `exit_status` + accumulated child rusage; `task_cleanup` (REAPED) frees the struct only via a reaper.
- [ ] `sys_wait4(pid, status, options, rusage)`: any-child (-1) and exact-pid selectors, `WNOHANG`; atomic single-reaper CAS on the zombie (one waiter reaps, a lost race returns `ECHILD`). Process-group selector + `WUNTRACED`/`WCONTINUED` -> §18.
- [ ] `RUSAGE_CHILDREN` accumulator on `struct task`; on reap, fold the child's user/kernel time + IO counters into the parent (mirror `ob_job_detach_task`) (-> XREF: §8).
- [ ] Orphan reparenting: on parent ZOMBIE, reparent live/zombie children to the nearest `PR_SET_CHILD_SUBREAPER` ancestor, else PID 0 (reaper-of-last-resort); an orphan zombie under a non-waiting init is auto-reaped (no slot leak at `TASK_MAX`).
- [ ] `task_set_child_subreaper` + a `child_subreaper` field consumed by the reparent selector above. Linux `prctl(PR_SET_CHILD_SUBREAPER)` adapter -> XREF: `TODO-10-kernel-security-hardening.md`.
- [ ] NT non-reap decouple (ZOMBIE safety): `nt_sync.c` process-wait observes ZOMBIE + `exit_status` without `task_cleanup`, so a waiter's `struct task*` stays valid until a POSIX reaper (-> XREF: `TODO-12-native-api-ssdt.md`).
- [ ] Overflow-safe status/rusage retention, reclaimed exactly once (no double-free, no lost status); the single-reaper CAS is the exactly-once barrier.
- [ ] `task_waitpid`/`sys_wait4` must LOOP until the child is DEAD/ZOMBIE: one `yield()` + unconditional `exit_status` read false-completes on any spurious wake (e.g. §17 group SIGINT); interrupted wait returns EINTR when delivery lands.
- [ ] Commit: `"kernel: task -- parenting, reaping, and wait4 semantics"`

> **Scope note (SPLIT 2026-07-12):** the richer wait variants (`sys_waitid`, `WUNTRACED`/`WCONTINUED` job-control waits), `PR_SET_PDEATHSIG`, full NT multi-waiter wake, and the `PR_SET_DUMPABLE` core-dump gate moved to **§18** -- they layer on the §15 foundation and cross into job-control signals / crash-dump policy. The minimal NT non-reap decouple stays in §15 because ZOMBIE retention is unsafe otherwise (a process-handle waiter reads `t->state` off a `struct task*` that `task_cleanup` would free).

**Test checkpoint:** `sys_wait4(-1, ...)` reaps any child; `WNOHANG` returns 0 when no child exited; a double-reap of the same zombie returns ECHILD to the second waiter; an orphan's parent-pid becomes 0 (or the subreaper) after its parent exits; an NT process-handle wait observes exit without reaping. `klog(LOG_DEBUG, "task", "reaped pid %u status %u")`. Test on: QEMU WHPX, QEMU TCG; bare metal.

> **Deferred:** 2026-07-12 | Codex design review (needs-attention, 4 [H]) established §15 cannot be safely/completely implemented on the current process-identity model. Blocked on: (1) stable-PID + slot-reuse -> XREF: `TODO-06-scheduler-enhancement.md` (item: "Separate stable PID from storage slot. Add monotonic PID allocation with reuse-after-drain" at line 357; regression item at line 362) -- `task_waitpid` is named there as a `pid==slot_index` consumer to fix, and without reuse "reaping" never returns a `tasks[]` slot (hard-cap `TASK_MAX`, `task.c:616`); (2) the SMP off-all-CPUs reap barrier (`task.c:3052`, "lands with the per-CPU run queues work / SMP phase 2") -- required before a deferred reaper can free an orphan zombie; (3) an operator decision on `PROCESS_OBJECT` terminal-state ownership (`ob_process.h:17` holds a raw `struct task*` into static `tasks[]`; under slot reuse an NT handle must observe generation-stable terminal state -- refcounted terminal-state vs task tombstone vs moving `exit_status` into `PROCESS_OBJECT`).
>
> **Design spec (implement when unblocked):** one atomic `task_publish_exit()` routed through ALL four death writers (`task_exit`, `task_wrapper` normal-return `task.c:118`, `task_terminate_remote` `task.c:2711`, fatal-signal `signal.c:76-100`) that publishes status/rusage with a release-store before marking terminal state, reparents children, and wakes waiters exactly once; a deferred PASSIVE_LEVEL reaper (NEVER inline `task_cleanup` from the raised-IRQL/log-free death-teardown path) gated on the off-CPU barrier; a task-lifecycle lock covering selector-scan + reap-claim + `parent_pid` change + `rusage_children` fold + waiter enrollment (closes the `task.c:2815` observe-live-child vs `task.c:2755` one-shot-wake lost-wakeup race); NT observers use acquire reads and never claim.

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

> **Deferred:** 2026-07-12 | Codex design review (needs-attention; 4 High confirmed via receiving-code-review). Real PPL is a security boundary that CANNOT be delivered until the process-handle-rights model lands: process operations resolve targets by raw PID and never check handle `granted_access` (`task_from_handle` at `nt_process.c:29-38`; NtTerminateProcess/NtWriteVirtualMemory/NtSetInformationProcess all raw-PID), so stripping access at handle-open (OB callback or inline) enforces nothing -- shipping it would be non-enforcing security theatre. Blocked on -> XREF: `02-kernel-core/TODO-12-native-api-ssdt.md §7` (item: "[Critical] NtCreateProcess/NtOpenProcess/NtOpenThread return raw PID/TID/task-struct, not OB PROCESS/THREAD objects" at line 413). Signer-class-derived assignment additionally blocked -> XREF: `02-kernel-core/TODO-19-code-integrity-trust-policy.md §4` (item: "EIF sig-block ABI decision (prereq)" at line 139).
> **Deferred (impl spec when unblocked):** 2026-07-12 | Build per the reviewed design: (a) pure dominance-matrix core mapping GENERIC_ALL/MAXIMUM_ALLOWED to concrete rights BEFORE filtering; deny TERMINATE/CREATE_THREAD/VM_OPERATION/VM_READ/VM_WRITE/DUP_HANDLE/SET_INFORMATION/SET_QUOTA/CREATE_PROCESS/SUSPEND_RESUME for non-dominating accessors, always preserve QUERY_LIMITED_INFORMATION + SYNCHRONIZE. (b) `ps_protection` set-once atomic field initialized with release semantics BEFORE slot publication, reset on slot reuse, fork-inherited by copy. (c) kernel-only CAS-from-None setter completed before `num_tasks++` (no ring-3 self-elevation; PID 0 = WinTcb). (d) pin the accessor task explicitly -- do NOT derive the principal from the global `current_task` cursor (single-CPU today at `task.c:65`/`1146`/`1390`). (e) consume via the ObpProcessType `ObRegisterCallbacks` filter once NtOpenProcess routes through the real `PROCESS_OBJECT`. Add SMP concurrent-open tests with distinct accessor levels.

---

## 17. Process Groups and Sessions

No section owns session ID, process-group ID, session leadership, the foreground terminal group, or group signal delivery -- so a shell cannot background jobs, `kill -pgid` a pipeline, or fan Ctrl+C out to a process tree. TODO-10 currently sends SIGINT to a single foreground task; `GenerateConsoleCtrlEvent` cannot target a group. This section owns the per-process fields + invariants; the Linux syscall adapters live in TODO-10 and the terminal consumes the foreground-group API.

- [x] `pgid` + `sid` (+ monotonic `has_execed`) on `struct task`; inherited from the creator at task_create/create_user/fork (pre-fork snapshot); PID 0 leads session 0. `src/kernel/sched/task.c`.
- [x] `pgroup_setpgid`/`setsid` + `SYS_SETPGID`/`SYS_SETSID`; full POSIX errno (ESRCH/EPERM/EINVAL/EACCES; exec'd child EACCES via `has_execed`); pure decide-cores unit-tested. `src/kernel/ipc/pgroup.c`.
- [x] `pgroup_getpgid`/`getsid`/`getpgrp` (+ `SYS_GETPGID`/`SYS_GETSID`/`SYS_GETPGRP`); `pid==0` means the caller.
- [x] Singleton `console_jobctl` (controlling sid + fg pgid + validity) + `pgroup_tcsetpgrp`/`tcgetpgrp` (`SYS_TCSETPGRP`/`SYS_TCGETPGRP`); enforces same-session + non-empty group (ENOTTY/EPERM/EINVAL).
- [x] `signal_send_group(pgid,sig)` reaches every live member (PID 0 excluded); `signal_ctrl_c` fans SIGINT to the console fg group -- NO-OP until a shell claims it; pending mask now atomic; `pgroup_is_orphaned` detector ships.
- [ ] Orphaned-pgroup SIGHUP+SIGCONT delivery: detector ships, delivery blocked on job-control stop/cont signals (SIGTSTP/SIGSTOP/SIGCONT) -> XREF: `§18` + `TODO-10-kernel-security-hardening.md`.
- [x] `GenerateConsoleCtrlEvent(event, pgid)` (`SYS_GENCONSOLECTRL`): CTRL_C/CTRL_BREAK -> SIGINT to the group. Consumer (terminal/user-lib shim) + Linux adapters owned elsewhere -> XREF: `TODO-10-kernel-security-hardening.md`.
- [x] Commit: `"kernel: task -- process groups and sessions (setpgid/setsid + group signals)"`

**Test checkpoint:** `setsid()` makes the caller a session+group leader (getsid==getpid); `setpgid` moves a child into a new group; a signal to `-pgid` sets pending SIGINT on all members; Ctrl+C fans to the foreground group, not just the leader. The pure POSIX decision cores (invariants + full errno matrix) are unit-tested (TEST_CAT_SCHED); the live cross-task fan-out is serial-validated on QEMU. **Delivery dependency:** the fan-out sets each member's pending SIGINT bit; the pre-existing `signal_check` drain is not yet wired into any thread-context return path, so end-to-end interruption/termination lands when the signal-delivery boundary ships -> XREF: `10-platform-services/TODO-10-linux-compat.md §8` (item: "SIGINT delivery (signal 2)" at line 265). Test on: QEMU WHPX, QEMU TCG; bare metal.

> **Test runner:** `scripts\debug\kernel\run-sched-tests.bat` (SUITE=sched) | 21 ProcExt group/session suites, 0 failures

> **Notes:**
> - **What shipped:** `src/kernel/ipc/pgroup.{c,h}` -- POSIX groups/sessions + a singleton console job-control object; `struct task` gains pgid/sid/has_execed; 8 INT 0x80 syscalls + GenerateConsoleCtrlEvent; 21 pure-decision unit tests.
> - **How it integrates:** Ctrl+C (keyboard ISR) fans SIGINT to the console foreground group via `signal_send_group` -- a NO-OP until a shell claims the console (never signals PID 0); the signal pending mask is now atomic, so group delivery is SMP-safe.
> - **Downstream effects:** unblocks a shell's job control + `kill -pgid`; §18 WUNTRACED/WCONTINUED and orphan SIGHUP/SIGCONT delivery still need job-control stop/cont signals. Design + review adoptions in the commit message.
> - **Scope boundary:** §17 owns the kernel primitives + foreground-group API; Linux syscall adapters -> TODO-10; the terminal/user-lib consumer -> terminal TODO; job-control stop/cont signals -> the orphan-delivery follow-up above.
> **Verified:** 2026-07-13 | commit `31fcdfde` | 6/7 items | build OK | tests 470/470 PASS (SUITE=sched subset; full suite green per §14) | smoke PASS (KVM 2.5s)
> **Accepted:** [H] concurrent creators can corrupt the task table -- slot reservation (`pid = num_tasks`) stays lock-free; §17's job-control lock only linearizes the pgid/sid+`num_tasks++` publish (pre-existing convention, Opus auditor rates LOW) -> XREF: 03-memory-concurrency/TODO-07-smp-phase2.md §3 (item: "Tasks-publication lock: serialize num_tasks++/slot-publish vs scheduler enumeration" at line 123)
> **Accepted:** [H] `signal_send` `t->state` wake can resurrect a DEAD task on SMP (pre-existing plain RMW; §17 amplifies via group fan-out) -> XREF: 03-memory-concurrency/TODO-07-smp-phase2.md §1 (item: "Audit signal_send t->state wake" at line 79)
> **Accepted:** [H] Ctrl+C fan-out queues SIGINT but no `signal_check` call site drains it (pre-existing; the delivery boundary is unbuilt) -> XREF: 10-platform-services/TODO-10-linux-compat.md §8 (item: "SIGINT delivery (signal 2)" at line 265)
> **Accepted:** [H] job-control lock holds IRQs off across a bounded O(TASK_MAX) scan + the Ctrl+C ISR fan-out scans the group (both bounded; the latency-critical ISR foreground read is lock-free atomic) -> XREF: 03-memory-concurrency/TODO-07-smp-phase2.md §1 (item: "Shrink IRQ-off time in ... job-control paths" at line 80)
> **Deferred:** [M] Ctrl+C wake can make `waitpid` false-complete a live child (pre-existing single-yield `task_waitpid`; §17 wakes more waiters) -> XREF: 02-kernel-core/TODO-21-process-model-extensions.md §15 (item: "task_waitpid/sys_wait4 must LOOP until the child is DEAD/ZOMBIE" at line 559)
> **Quality reviewed:** 2026-07-13 | Codex 25x (design + adversarial + consistency + perf + re-adversarial) | 2C+11H+9M fixed, 4H+1M accepted-XREF | scope: kernel-code-quality

---

## 18. Rich Wait Variants, NT Multi-Waiter Wake, and Dumpable Policy

Split from §15 (2026-07-12). Layers on §15's ZOMBIE + single-reaper foundation: the richer POSIX wait variants (`waitid`, job-control stop/continue waits), the NT semantic that every process-handle waiter wakes (not one reaper), the parent-death signal, and the per-process core-dump policy gate. Each crosses into a subsystem §15 does not own (job-control signals, the OB wait queue, crash-dump generation).

- [ ] `sys_waitid(idtype, id, siginfo, options)`: `WEXITED`/`WSTOPPED`/`WCONTINUED`/`WNOWAIT`; peek-without-reap on `WNOWAIT` (leaves the zombie for a later reaper).
- [ ] `WUNTRACED`/`WCONTINUED` in `sys_wait4`/`sys_waitid`: report stopped/continued children, gated on job-control stop/cont signals (-> XREF: §17 group signals; `TODO-10-kernel-security-hardening.md`).
- [ ] `task_set_pdeathsig` + a `pdeathsig` field; delivered to a child on the exiting parent's reparent sweep. Linux `prctl(PR_SET_PDEATHSIG)` adapter -> XREF: `TODO-10-kernel-security-hardening.md`.
- [ ] Full NT multi-waiter wake: every `NtWaitForSingleObject`/`NtWaitForMultipleObjects` waiter on an exited process handle wakes off the ZOMBIE signal (no single-reaper starvation) (-> XREF: `TODO-12-native-api-ssdt.md`).
- [ ] `PR_SET_DUMPABLE`/`dumpable` per-process flag gating core-dump generation on crash; default dumpable, cleared on privilege transition (Linux `SUID_DUMP`) -> XREF: `TODO-27-crash-dump-generation.md`.
- [ ] `RLIMIT_CORE == 0` also suppresses the dump (composes with the `dumpable` flag) -> XREF: §9.
- [ ] Commit: `"kernel: task -- rich wait variants, NT multi-waiter wake, dumpable policy"`

**Test checkpoint:** two `NtWaitForSingleObject` waiters on the same exited process handle BOTH wake; a POSIX `sys_wait4` reaps that process exactly once; `sys_waitid(WNOWAIT)` leaves the zombie reapable; a `dumpable=0` process produces no core dump on crash; `RLIMIT_CORE=0` suppresses the dump even when dumpable. `klog(LOG_DEBUG, "task", "proc %u signaled, %u NT waiters woken")`. Test on: QEMU WHPX, QEMU TCG; bare metal.

> **Deferred:** 2026-07-12 | Blocked on §15 -- these are the richer wait variants + NT multi-waiter wake that layer on §15's ZOMBIE + single-reaper + centralized `task_publish_exit()` foundation, which is itself deferred on the process-identity model -> XREF: §15 (item: "NT non-reap decouple (ZOMBIE safety)" and the §15 Deferred design spec). `PR_SET_DUMPABLE` + `RLIMIT_CORE` core-dump gate additionally needs the crash-dump generator -> XREF: `TODO-27-crash-dump-generation.md`.

---

## 19. `task_exec` Commit Point and Kernel-Stack Reclamation

`task_exec()` had no transactional commit point: it mutated the process image and THEN performed fallible work, so a late failure returned `-1` to a caller that iretqs back into an image that no longer exists. It also replaced `tasks[pid].stack_base` with a fresh guarded kernel stack and never reclaimed the old one. Found 2026-07-27 by Codex adversarial review of the exec frame-handoff fix (commit that added `task_exec_take_pending_frame`); all three defects predate that fix and none is caused by it. Filed rather than hot-fixed because the two obvious local patches are both wrong: hoisting the allocations does not close the `exec_load_fmt` failure return, and publishing `exec_pending` later lets a tick between the frame write and the store clobber the published frame via the save-gate.

A fourth defect of the same class was filed here 2026-07-27 and split out to §20 on 2026-07-28 (page-table use-after-free across reap + fork/exec). It is a distinct lifetime question with its own repro and its own decision, and this section carries ABI-affecting transactional work that a single section cannot hold; see §20.

- [x] Commit point established in `task_exec` (`src/kernel/sched/task.c`): argv-frame validation, the `argv_addrs`
      `kmalloc`, the replacement guarded kernel stack, and the fork+exec private-frame table are all acquired BEFORE the isolation remap, which is now the marked COMMIT POINT. The remap itself was made infallible -- every private frame is allocated and zeroed first, so an OOM returns `-1` pre-commit instead of the old partial remap that let `exec_load` write through mappings still shared with the fork parent
- [x] Past the commit point nothing returns `-1` into a destroyed image: `exec_load_fmt` failure, launcher
      `vmm_create_user_pml4` failure, and a NULL PEB or TEB all route through `exec_commit_failure()`, which frees the unpublished kernel stack + argv table and returns `TASK_EXEC_IMAGE_DESTROYED`. The four callers (SYS_EXEC, `shell_loader`, `test_threads`, the user-mode test loader) release their staging buffer and `task_exit()` on it; returning to the KERNEL caller is safe because it is on its own kernel stack, not ring 3
- [x] Previous kernel stack reclaimed: `task.stack_pending_free` parks the superseded stack (task_exec cannot free it
      -- its own caller is still running on it) and it is drained at the next `task_exec` for that task and at `task_cleanup`. Both go through the new `task_free_kernel_stack()`, extracted from `task_cleanup` so the `heap_owns` discrimination and the `vmm_uninstall_guard_page`-before-PMM-free order exist once, not twice
- [x] Publication race closed by making publication the LAST act of `task_exec` -- after PEB/TEB and
      `kernel_gs_base` -- inside `local_irq_save`/`local_irq_restore`. NOT `KeRaiseIrql(DISPATCH_LEVEL)`: that maps to TPR 0x20 (`irql.c`), which masks only priority groups 0-1, so the LAPIC timer still preempts. The iret frame's RCX slot is patched at publication instead of snapshotting the pre-exec PEB
- [x] A fork child that lost its PML4 to OOM can no longer exec: the `task.forked_shares_parent_image` origin flag makes
      the state explicit and `task_exec` refuses pre-commit rather than skipping isolation and overwriting the parent's image. SUPERSEDED 2026-07-28 as the primary defence -- `TODO-04` §21 made `task_fork` fail CLOSED on that allocation, so it no longer leaves `cr3 = 0` on a published child and this check has no reachable producer; it is retained as a backstop. -> XREF: `00-infrastructure/TODO-04-usermode-test-framework.md` §21 (item: "`FAULT_SITE_FORK_CHILD_PML4` (id 4, pmm-owned)")
- [x] `SYS_EXEC` now frees its staging buffer unconditionally -- it freed only on failure, and the success path never
      returns there, so every successful exec leaked one heap allocation of the file's size
- [x] Unit tests (`src/kernel/test/test_sched.c`, 6 shipped): parked stack clear on a never-exec'd task; kernel stacks
      never alias (live-live, parked-live, parked-parked, across the whole table); reclamation returns every frame through the production helper via a `KERNEL_TESTS` seam; plus the staging-token trio -- release idempotence (the double-free guard), empty/NULL token safety, and the shared `task_exec_staging_kfree` shape asserted against a `heap_get_free()` baseline so deleting the `kfree` cannot pass as success
- [x] Fault-injected exec lifecycle coverage (this section's Test checkpoint) shipped as four ring-3 sub-tests in
      `user/test/test_process.c`, because a kernel unit test cannot call `task_exec` (a post-commit failure terminates the calling task by design and the test runner IS a task). Post-commit: the child execs `hello.txt`, a real file that is not a loadable image, so it fails in the loader PAST the commit point; the child must be TERMINATED, asserted on the EXACT new `TASK_EXIT_EXEC_IMAGE_DESTROYED` status rather than a bare -1, which `SYS_KILL` also produces. Pre-commit: an injected `FAULT_KMALLOC_NEXT` and a 600-entry argv each assert the exec returned -1 with the child still alive to report it -- executing the report IS the proof the old image survived. Depth sweep: one child walks `FAULT_KMALLOC_COUNTDOWN` 1..20 over the exec path (looping IN the child, never forking per depth -- `TASK_MAX` is 32 and reaped slots are not reused yet) and must END on the exec-destroyed status, so the sweep cannot decay into pre-commit-only coverage while staying green. PMM path: `FAULT_PMM_NEXT` fails the replacement guarded kernel stack's `pmm_alloc_contiguous` (confirmed by its `Cannot allocate kernel stack` log) and must be refused pre-commit. Verified live: `sched: exec: post-commit failure (image load failed), terminating PID 15`
- [x] Release-before-publication is pinned by a RUNTIME guard, not just by tests: `task_exec_staging_release()` logs
      `LOG_ERROR` if a real release happens while `exec_pending` is set. The helper unit tests can only prove the helper's own semantics -- they stay green if `task_exec`'s call is deleted or moved below the publication window, which is exactly the regression the token exists to prevent. Both regression shapes were exercised: DELETING the call fails the build outright (`staging` becomes an unused parameter under `-Werror`), and MOVING it after publication builds clean but fires the guard 18 times in one `SUITE=sched` run, surfaced as `[FAIL]` lines and caught by the smoke test's log-cleanliness gate. Correct code leaves it silent
- [x] `TASK_EXIT_EXEC_IMAGE_DESTROYED` added (`include/kernel/sched/task.h`) and used by the three callers that exit on
      a destroyed image (`SYS_EXEC`, `shell_loader`, `exec_loader`). The exit status now NAMES the cause: a bare -1 is what kill and self-kill use, so a parent could not tell an exec-destroyed child from a killed one, and without that distinction the post-commit test has no honest oracle. Deliberate observable-behaviour change, raised by both review legs.
      It sits in a new reserved `TASK_EXIT_REASON_BASE` (-1000) block, NOT at a small negative: waitpid returns a raw int32 shared by every producer, and signal deaths already occupy `-(signum)` (`signal.c`) -- the first attempt used -2, which is exactly SIGINT. New kernel-originated termination reasons belong in that block, never as a bare literal at the call site
- [x] Site-targeted fault injection for the two `task_exec` pre-commit branches SHIPPED 2026-07-28 in `TODO-04` §20: the
      argv address table and the private-frame table are now named by `FAULT_SITE_EXEC_ARGV_TABLE` and `FAULT_SITE_EXEC_PRIVATE_FRAMES` rather than reached by a drifting kmalloc ordinal, and `user/test/test_faultinject.c` proves each one fires by requiring the arm to read CONSUMED via `FAULT_SITE_QUERY` after the refusal. (The guarded kernel stack was never in this set -- `FAULT_PMM_NEXT` lands on it directly, verified by its `Cannot allocate kernel stack` log.) `FAULT_PMM_COUNTDOWN` also exists now, so the Nth PMM frame IS failable from ring 3; the post-load PEB/TEB allocations have a `FAULT_SITE_PEB_FRAMES` id and gate coverage, with the end-to-end assertion still open because a post-commit failure terminates the child. -> XREF: `00-infrastructure/TODO-04-usermode-test-framework.md` §20 (item: "Site-targeted arming shipped as two ATOMIC allocator-specific selectors")
- [x] Fork-child-with-no-isolated-CR3 is CLOSED 2026-07-28 by `TODO-04` §21, and the refusal moved: fork now fails CLOSED on
      the PML4 allocation rather than publishing a child with `cr3 = 0` for `task_exec` to catch later. The "parent cannot arm for a not-yet-forked child" half of this item was wrong -- `task_fork` runs on the parent's thread, so self-PID arming always reached that allocation; the real obstacle was separability, closed by naming it `FAULT_SITE_FORK_CHILD_PML4`. `task.c`'s `forked_shares_parent_image` check is retained as a backstop with no reachable producer. -> XREF: `00-infrastructure/TODO-04-usermode-test-framework.md` §21 (item: "Ring-3 regression in `user/test/test_faultinject.c`")
- [/] PMM-free-page-count-returns-to-baseline across exec -> re-exec -> reap is NOT covered here and cannot pass today:
      §20 owns an open, unfixed defect where every re-exec orphans the previous exec's private image frames (~1 MiB). §20 already carries the matching test item, so the clause is owned there rather than duplicated. It is also the only automated net that would catch `task_exec` silently ceasing to release the staging token before publication. -> XREF: §20 (item: "Two-exec lifecycle test")
- [x] Guard-page registration is transactional (`src/kernel/mm/vmm.c`): capacity-aware failable registration, checked installs at every producer, and a teardown gate that proves the identity mapping before frames return to the PMM
      `guard_page_register` used to drop the entry once the table saturated -- AFTER `vmm_install_guard_page` had already cleared the PTE and returned success -- so `vmm_uninstall_guard_page` found no entry, skipped the remap, and the caller handed a still-unmapped frame to the PMM. Registration now reserves its slot BEFORE any PTE is touched and fails the install instead, restoring the fail-before-clearing contract `heap.c`'s heap-end guard already documented.
      Failure is now TYPED, because the two cases differ in whether the frame may go back to the PMM: `VMM_GUARD_UNAVAILABLE` (table full, split failed) leaves a reusable frame, `VMM_GUARD_VA_UNSAFE` says the run must be quarantined. Install checks reusability FIRST -- present, mapping ITSELF, Writable -- so a non-reusable run is never reported as merely unavailable, and it refuses rather than clearing a VA that maps somebody else: in an identity-mapped kernel a fixed VA is just a number, and `pmm.c` already reserves the user PT window against exactly that collision after it cost a boot hang. User set is deliberately NOT a refusal -- SMEP is off and the boot PML4 carries User on every 2 MiB page, so requiring it clear refused every legitimate guard and halted the boot in `ist_alloc()` (measured, not theoretical).
      Uninstall returns 0 only for a CONFIRMED kernel identity mapping (`phys == page`, Present + Writable, User clear -- it can demand kernel-only because it rewrites the leaf itself). A registered guard has its leaf rewritten and re-verified, and its registration is surrendered only as the COMMIT step so a failed restore can be retried; a present alias is refused; an unregistered VA is inspected, never rewritten, and fails closed. Presence comes from a new `vmm_translate()` reporting presence and address separately, because `vmm_get_physical()` answers 0 for both "absent" and "mapped to frame 0".
      Every producer checks the install and rolls back by result: `task_create`, `task_exec` (pre-commit), `uthread_create`, and AP startup (`src/kernel/smp/smp.c`), which skips that AP rather than launching it on writable padding. A shared `stack_run_release_after_guard_failure()` frees or quarantines per code. Thread-stack teardown routes through a new `uthread_free_kernel_stack()` -- that run's FIRST page is the guard -- so the four `uthread_create` rollbacks and `thread_free_stacks` can no longer free an unrestored frame.
      `guard_page_lock` spans the whole install/uninstall transaction, so a competing split cannot publish a stale table over a guard PTE another CPU just cleared; the fault-path lookup takes it with `spin_trylock` and degrades to no label rather than blocking in a panic path. Capacity is `VMM_MAX_GUARD_PAGES` 640, pinned by a `_Static_assert` against `TASK_MAX * (THREAD_MAX + 2)`.
      Tests (`src/kernel/test/test_vmm.c`, 5 added): saturation refuses with the reusable code and leaves the frame mapped; uninstall refuses an alias to frame 0, KEEPS the registration, and a retry after repair succeeds; it normalizes read-only and user-accessible same-frame leaves; it fails closed on unregistered read-only and unregistered absent VAs; install refuses aliased and read-only VAs while accepting a user-accessible one. The pre-existing guard-install test also stopped leaking its slot and frames.
- [/] A REFUSED guard teardown leaks its stack run and its guard-table slot for the life of the boot. No stack class has a reclaim path: needs one bounded quarantine + retry owner covering task, thread and AP stacks.
      -> XREF: `03-memory-concurrency/TODO-01-vmm-memory-protection.md` §1 (item: "Bounded quarantine + retry owner for stack runs whose guard teardown was REFUSED")
      `vmm_uninstall_guard_page()` retains the registration on failure so a retry CAN restore and release it, and `task_free_kernel_stack()` now returns a status so a caller can decline to clear its pointer -- but no caller can hold that pointer durably today. `task.stack_pending_free` survives a refused drain only until the next successful exec republishes it (`task.c`, the `stack_pending_free = stack_base` store); `task_cleanup` and the exec rollback paths clear their pointer regardless of status; `thread_free_stacks` and AP startup have no owner at all, and retaining a raw pointer across thread-slot reuse risks a stale pointer or a double free. The reclaim owner is real design work, not a fix-loop patch -- it needs a durable multi-entry list and a retry driver, plus lifecycle fault injection forcing a refusal through re-exec, post-commit rollback and reap.
      Bounded and fail-safe meanwhile: nothing unsafe is ever freed (that is the shipped gate), the path is reachable only through a refused uninstall -- a hijacked guard VA or an OOM in the restoring `vmm_map_page` -- and each occurrence costs one run plus one of the 640 slots and logs a `LOG_ERROR`.
- [/] Guards are installed only in the kernel root, but `vmm_create_user_pml4` clones the kernel PD BY VALUE, so a guard installed after a process root exists is not present under that CR3 and the overflow crosses it silently.
      Found 2026-07-28 by this section's adversarial review and confirmed at the clone loop (`pd[i] = kern_pd[i] & ~USER` in `src/kernel/mm/vmm.c`): a runtime guard splits the KERNEL PD entry and clears a PTE in the new page table, while a process root created earlier still holds the original 2 MiB huge value. Pre-existing for every runtime guard (`task_create`, `task_exec`, `uthread_create`, AP startup) since per-process roots landed; the transactional work in this section neither caused it nor can close it. Fix is address-space level: propagate kernel-PD mutations to every live root (needs a live-root registry), or share the kernel PD by reference -- which conflicts with the per-entry User clearing this clone does. A regression must create a user PML4 BEFORE guarding a frame in an unsplit low-1-GiB region, then assert the page is non-present under that CR3 and restored there before release. -> XREF: `03-memory-concurrency/TODO-01-vmm-memory-protection.md` §12 (item: "Per-Process User Page Mapping")
- [/] Guard installs are not cross-CPU coherent, and the teardown gate cannot bind mappers that do not take `guard_page_lock`. Both need the VMM-wide page-table lock + TLB shootdown. -> XREF: `03-memory-concurrency/TODO-07-smp-phase2.md` §2
      Accepted three times across this section's adversarial rounds as architectural, not local: a remote CPU can retain a stale present translation for a page this CPU guarded, and any mapper may re-point a VA between the teardown gate's verification and the caller's `pmm_free_frame`. `vmm.c` has no global page-table lock (see the note in `vmm_protect_range`) and the kernel has no shootdown IPI, so neither is closable inside this section. Shortening the guard critical section (preallocate the split's frames outside the lock, publish under it) belongs with the same work.
- [x] Caller staging-buffer release no longer races publication: `task_exec` takes a `struct task_exec_staging`
      ownership token and performs the release ITSELF, after its last read of `data` and immediately BEFORE the publication window -- the one point where the buffer is provably dead and the caller is provably still running. Previously, once the release-store made the new frame scheduler-visible and `local_irq_restore` re-enabled interrupts, a tick could switch the task into the new image before `task_exec` returned, so the caller's `kfree(buf)` never ran; `SYS_EXEC` escaped only because INT 0x80 is an interrupt gate (IF=0), and `exec_loader_func` had the deterministic form (success path HLT-loops with no `kfree` at all -- a leak on every successful exec, now closed).
      `task_exec_staging_release()` clears the release hook BEFORE invoking it, so it is idempotent: `task_exec` releases on the success path, every caller releases unconditionally after the call, and exactly one of them frees on all twelve caller x outcome combinations. Two allocator shapes are carried by the same token -- kmalloc via the shared `task_exec_staging_kfree`, and `pmm_alloc_contiguous` via `utest_staging_free_frames` in the user-mode test loader. All four callers updated (`SYS_EXEC`, `shell_loader`, `exec_loader`, the user-mode test loader); no `task_exec_ex` shim, per the no-backwards-compat rule.
      Release runs with interrupts ENABLED, deliberately outside the `local_irq_save` window, because `kfree`/`pmm_free_frame` must not run with interrupts disabled; a reschedule landing there is harmless because `exec_pending` is still clear. The PMM bitmap's SMP-unlocked mutation is unchanged by this -- the same call already ran in the same task context, just a few instructions later
- [x] Commit: `"kernel: task -- exec commit point + kernel-stack reclamation"` (first tranche) and
      `"kernel: task -- staging-release ordering + exec lifecycle coverage"` (this one). The remaining open items are all fault-injection REACH gaps owned by `TODO-04` §5 plus the §20 PMM-baseline clause -- no unshipped behaviour in this section

**Test checkpoint (met; rewritten 2026-07-28 to state what is actually checkable here).** A post-commit failure must TERMINATE the task with a named log instead of returning to ring 3 -- driven deterministically by exec'ing a non-image file and asserted on the exact `TASK_EXIT_EXEC_IMAGE_DESTROYED` status, with `klog(LOG_ERROR, "sched", "exec: post-commit failure (%s), terminating PID %u")` observed live. Each pre-commit refusal must return -1 with the child's OLD image intact, proven by the child continuing to run and report: injected kmalloc OOM, injected PMM-frame OOM (which lands on the replacement guarded stack), an over-cap argv, and an allocation-depth sweep in which no depth may return success into a replaced image. Two criteria from the original wording moved to their owners rather than being claimed here: PEB/TEB post-load injection needs an Nth-frame PMM selector that does not exist (`00-infrastructure/TODO-04` §20), and the PMM-free-page baseline across re-exec cannot pass while §20 of this file owns the unfixed frame-orphaning defect. Test on: QEMU TCG (8.2.2 and current), QEMU WHPX; bare metal.

> **Test runner:** `bash scripts/test.sh SUITE=sched` -- 520 Sched assertions, of which 8 are this section's (parked-stack clear, stack-pointer non-aliasing, reclamation leak-free, staging-release idempotence, empty/NULL token, kmalloc release shape against a heap baseline) plus five ring-3 exec-lifecycle sub-tests in `test_process.exe` (9 outcome assertions and their fork guards): post-commit termination on the exact exec-destroyed status, injected-kmalloc and over-cap-argv refusals with the old image intact, the allocation-depth sweep, and an injected PMM-frame refusal that lands on the replacement guarded stack. `bash scripts/test.sh SUITE=mm` -- 5 new VMM guard-table suites (saturation refusal, alias refusal + retry, flag normalization, unregistered fail-closed for read-only and absent, install-side non-identity refusal). Full gate: `bash scripts/test.sh` + `bash scripts/test-smoke.sh`.

> **Notes:**
> - Shipped: the `task_exec` commit point, non-returning post-commit failures, kernel-stack reclamation via `stack_pending_free`, publication last under `local_irq_save`, and the `task_exec_staging` release-before-publication token.
> - Integrates: all four `task_exec` callers pass an ownership token and implement the image-destroyed contract; `TASK_EXIT_EXEC_IMAGE_DESTROYED` makes the cause visible to a parent's `waitpid`.
> - Downstream: closes TODO-11 §20's RCX-ordering item; fork children carry `forked_shares_parent_image`; `UTEST_SKIP` added so an unavailable precondition is no longer recorded as a pass.
> - Canonical doc: `include/kernel/sched/task.h` documents both exec-destroyed constants, `struct task_exec_staging`, and `stack_pending_free`.
> - Scope boundary: every behaviour this section owns has shipped and is gate-verified. Open items are fault-injection REACH gaps (no site-targeted or PMM-countdown selector) owned by `TODO-04` §5, plus the PMM-baseline clause owned by §20.
> - Deferred elsewhere: cross-CPU guard coherence to `D03 T07 §2`; unchecked `vmm_map_page` in PEB/TEB to TODO-11 §20; the PMM bitmap SMP lock to `D03 T03 §1`.

> **Verified:** 2026-07-28 | commit `2454518f` | 13/19 items ([/] x6 owned elsewhere) | build OK | 26862 kernel + 16 user-mode tests pass; SMOKE TEST PASSED; lint 0 errors
> **Accepted:** [M] `UTEST_SKIP` prints but never reaches TAP/XML/JSON, so a binary with skipped sub-tests still reports PASS with zero skips (reason: needs launcher-protocol support, not a kernel change) -> XREF: `00-infrastructure/TODO-04-usermode-test-framework.md` §22 (item: "Track per-sub-test skip counts in harness state")
> **Accepted:** [M] the exec-destroyed exit status is hand-mirrored in `user/test/test_process.c` instead of riding the generated ABI header (reason: `gen-user-abi.py` extracts unsigned literals only and cannot express a negative expression) -> XREF: `00-infrastructure/TODO-04-usermode-test-framework.md` §23 (item: "Teach `scripts/gen-user-abi.py` to resolve signed and expression-valued constants")
> **Accepted:** [L] the staging-release runtime guard indexes the global `current_task` cursor rather than the exec'ing pid, so it would read another CPU's task under per-CPU run queues (reason: single-cursor scheduler today; matches how `task.c` already treats this assumption) -> XREF: `03-memory-concurrency/TODO-07-smp-phase2.md` §3 (item: "Per-CPU current-thread cursor" at line 120)
> **Deferred:** [M] one unprivileged exec can still hold 512 KiB of the fixed 2 MiB kmalloc arena for the whole read-and-load; the cap is a heap guard, not a fix (reason: needs staging off the kmalloc heap) -> XREF: `03-memory-concurrency/TODO-03-advanced-allocator.md` §3 (item: "Route exec image STAGING off the kmalloc heap" at line 151)
> **Quality reviewed:** 2026-07-28 | Codex 7x (adversarial, consistency, perf, re-adversarial x3, +1 crashed consistency leg re-dispatched) | 4H+6M+4L fixed, 4 open | scope: kernel-code-quality + userland-code-quality + kernel-quality-auditor + concurrency-evidence-mapper

> **Note:** the deterministic ring-3 crash that surfaced these (a synchronous `SYS_EXEC` returning to its pre-exec RIP
> when no timer tick landed) is FIXED and verified -- see `task_exec_take_pending_frame` in `src/kernel/sched/task.c`
> and the `Sched: exec frame handoff pass-through` test. This section is the remaining transactional work only.

---

## 20. Page-Table Lifetime Across Reap + fork/exec

A live task was observed executing against a REAPED task's PML4 whose frames had been recycled, panicking with `USER_ACCESS_VIOLATION`: free side `vmm_destroy_user_pml4` (`src/kernel/mm/vmm.c:1209`), reap side (`src/kernel/sched/task.c:3479`). Split out of §19 on 2026-07-28 -- same reap-versus-exec lifetime class, but a distinct decision and a distinct repro, so it is owned separately rather than bundled with §19's transactional work.

**Status correction (2026-07-28, measured).** The filing claimed this was "the sole cause of the currently-red `Build Impossible OS` job". That is false. The red run (`051df5e0`) failed on **Run tooling regression pack** -- `build_offload_sections` + `build_offload_type_specific` -- which `eb2af727` fixed by repairing the `_repo_root()` doubled-path bug that had silently disabled the gate; `build.yml` is green at `103a2db8` and after. **The repro no longer reproduces**: three consecutive `CI_PARITY=1 bash scripts/test.sh SUITE=quota QUIET=1` runs on `/usr/bin/qemu-system-x86_64` 8.2.2 with forced TCG -- the exact environment named below -- pass 3342 kernel + 16 user-mode tests with zero `USER_ACCESS_VIOLATION`. Three green runs of a timing-sensitive race are NOT a fix, and no commit claims to have addressed the lifetime question, so the section stays open on the invariant, not the crash.

- [ ] `vmm_set_user_page()` returns `void` and returns SILENTLY when it cannot allocate a PT to split a 2 MiB PDE
      (`src/kernel/mm/vmm.c`, the `if (!pt_frame)` arm), so every caller that marks a child's image or stack pages can half-succeed under memory pressure and publish a task whose user stack is supervisor-only -- its first ring-3 push then faults instead of the constructor refusing. NOT a rare path: the child user stack is `kmalloc`'d from the kernel heap, which lies OUTSIDE the region `vmm_create_user_pml4` pre-splits, so the split (and its extra frame allocation) is taken on EVERY fork. 11 call sites, 10 of them the three process constructors in `src/kernel/sched/task.c`. Make it fallible and give each constructor its unwind policy (fork and `task_create_user` refuse; `task_exec` must distinguish pre- from post-commit). -> XREF: `00-infrastructure/TODO-04-usermode-test-framework.md` §21 (item: "`FAULT_SITE_FORK_CHILD_PML4` (id 4, pmm-owned)")
- [ ] `task_create_user` (`src/kernel/sched/task.c`) still publishes a task with `cr3 = 0` and only a `LOG_WARN` when
      `vmm_create_user_pml4` fails -- the exact degrade `TODO-04` §21 removed from `task_fork`. It clears `forked_shares_parent_image`, so `task_exec`'s backstop does not catch it either. Make it refuse like fork now does. Until it lands, the "published task with no isolated address space" class is narrowed, not closed. -> XREF: `00-infrastructure/TODO-04-usermode-test-framework.md` §21 (item: "`FAULT_SITE_FORK_CHILD_PML4` (id 4, pmm-owned)")
- [ ] Extract `task_fork`'s pre-publication unwind ladder: it is now repeated SIX times in one function (filter, token,
      pledge/unveil, job, quota, kstack, ustack releases), which is past the extract-on-the-third rule, and the six differ in release ORDER even though each is individually correct. One static helper taking `(child_pid, token, filter, kstack, ustack)` with NULL-tolerant releases collapses them, so the next acquisition added before publication is threaded into one site instead of six. Raised by the `TODO-04` §21 kernel-quality audit.
- [ ] Decide which half of the race is wrong: whether reap may free a PML4 while a task still holds that CR3, or
      whether fork/exec can adopt a recycled one. The 2026-07-27 evidence does not distinguish them, and the fix differs
- [ ] Because the crash no longer reproduces, do NOT patch toward the symptom. Establish the INVARIANT instead: a task
      must never be schedulable on a CR3 whose PML4 has been freed. Enforce it where it can be checked cheaply and
      assert it in a debug build, so a re-emergence names itself instead of arriving as a recycled-data instruction fetch
- [ ] Original evidence, measured 2026-07-27: the transition ring records PID 12 at `cr3=0x16CD000 rsp=0xA6C4B0`; the
      panic context carries the SAME CR3 and an RSP 16 bytes up the SAME stack, after `task: PID 12 reap cleanup` and `sched: PID 13 -> entry 0x800000`. `RBP` little-endian reads `"PIPE-OK"` -- a pipe test's buffer, so the process was fetching instructions out of recycled data
- [ ] Repro as filed (no longer reproducing, see status correction): `CI_PARITY=1 bash scripts/test.sh SUITE=quota
      QUIET=1` (selects `/usr/bin/qemu-system-x86_64` 8.2.2 + forced TCG), then grep the serial log for `USER_ACCESS_VIOLATION` rather than waiting for the timeout. Only `test=1` ever reproduced it -- a plain 8.2.2 boot reaches `C:\>` and idles, so the smoke test reports green and proves nothing
- [ ] Do NOT close this by upgrading or pinning CI's QEMU. 8.2.2 was the TRIGGER, not the cause: the same kernel on
      10.2.1 and on KVM completed 25474 kernel + 16 user tests. It is a timing-sensitive race one emulator exposed, the
      exact class the bare-metal-first rule exists for
- [ ] Every re-exec orphans the previous exec's private image frames: `vmm_remap_user_page` overwrites a PTE and
      does not free the frame it displaced, so the second exec drops ~1 MiB of `PAGE_OWNED` frames the first exec installed and `vmm_destroy_user_pml4` only reaches the newest set. Capture each old PTE's frame + owned bit before the remap and free the displaced owned frames after it. Found by §19 round-2 adversarial review
- [ ] Two-exec lifecycle test: `pmm_get_free_frames()` across exec -> re-exec -> reap returns to baseline (today it
      drops by the image range per re-exec). Pairs with the kernel-stack reclamation §19 already closed
- [ ] Reap-versus-exec barrier on SMP: `task_terminate_remote` publishes `TASK_DEAD` immediately while
      `task_cleanup` treats `TASK_DEAD` as permission to free stacks, so another CPU could free `stack_base` or `stack_pending_free` while `task_exec` still runs on it. Safe today only because the scheduler is single-cursor. Needs a DYING -> QUIESCED transition acknowledged by every CPU before any stack is freed. Found by §19 design review; §19 relies on the same assumption its existing free path already did
- [ ] Unit test the reap/exec lifetime directly: a task must never be schedulable on a CR3 whose PML4 has been freed --
      assert the reaped PML4's frames are not reachable from any runnable thread before the PMM may hand them out
- [ ] Commit: `"kernel: mm -- page-table lifetime across reap and fork/exec"`

**Test checkpoint:** a unit test drives reap-then-reuse of a user PML4 and asserts no runnable thread's CR3 names a freed frame; the debug-build assertion fires on a deliberately-inverted ordering. Re-run the 8.2.2 CI-parity quota suite and record the result either way -- a green run is evidence about the trigger, not about the invariant. Test on: QEMU TCG (8.2.2 and current), QEMU KVM; bare metal.

---

## OS Comparison

| ⭐   | Feature                       | 🪟 Win11                       | 🐧 Linux                   | 🚀 Impossible OS                      |
| --- | ----------------------------- | ----------------------------- | ------------------------- | ------------------------------------ |
| 💎   | Per-process CWD               | ✅ SetCurrentDirectory         | ✅ chdir / getcwd          | ✅ §1 cwd + Nt syscalls               |
| 💎   | STD handle pre-wiring         | ✅ CreateProcess inherit       | ✅ fd 0/1/2 via fork       | ⬜ §2                                 |
| 💎   | User-mode heap (brk)          | ✅ NtAllocateVirtualMemory     | ✅ brk / sbrk              | ⬜ §3                                 |
| 💎   | Process priority class        | ✅ SetPriorityClass            | ✅ nice / setpriority      | ⬜ §4                                 |
| 💎   | Scheduling policy             | ✅ REALTIME_PRIORITY_CLASS     | ✅ SCHED_FIFO / SCHED_IDLE | ⬜ §5                                 |
| 💎   | Capability / privilege model  | ✅ Access tokens               | ✅ POSIX capabilities      | ⬜ §6                                 |
| 💎   | Process accounting            | ✅ ProcessTimes + IoCounters   | ✅ getrusage / times       | ⚠️ §8 fields (ring-3 query deferred) |
| 💎   | Per-process resource limits   | ✅ Job Object quotas           | ✅ getrlimit / setrlimit   | 🟡 §9 storage + accessors             |
| 💎   | Process CPU affinity          | ✅ SetProcessAffinityMask      | ✅ sched_setaffinity       | ⬜ §10                                |
| 💎   | Per-process mitigation policy | ✅ SetProcessMitigationPolicy  | ⚠️ prctl + seccomp        | 🟡 §11 NO_CHILD field+enforce (ring-3 API deferred) |
| 💎   | Job Objects / cgroups         | ✅ NtCreateJobObject           | ✅ cgroups v2              | 🟡 §13 lifecycle+accounting+active-limit; CPU/mem enforce deferred |
| 💎   | Process exit cleanup          | ✅ PspExitProcess              | ✅ do_exit + __put_task    | 🟡 §14 shared death-path release walk; byte-lock/oplock/PEB-frame/SMP-barrier deferred |
| 💎   | Exec commit point (no-return) | ❌ No exec (CreateProcess)     | ✅ point-of-no-return kill | ✅ §19 typed IMAGE_DESTROYED + named exit status |
| ⭐   | Exec staging-buffer handoff   | ❌ N/A                         | ⚠️ kernel-internal only    | ✅ §19 release-before-publication token |
| 💎   | Reaping / wait semantics      | ⚠️ Handle signaling (no reap) | ✅ wait4 / waitid          | ⬜ §15/§18 deferred (needs TODO-06 §357 slot reuse) |
| 💎   | Protected Process Light       | ✅ PS_PROTECTION               | ❌ No equivalent           | ⬜ §16 deferred (needs T12 §7 handle-rights model) |
| 💎   | Process groups / sessions     | ⚠️ Console ctrl groups        | ✅ setpgid / setsid        | 🟡 §17 groups+sessions+fanout+GenerateConsoleCtrlEvent; orphan SIGHUP/SIGCONT deferred |
| 💎   | Core-dump / dumpable policy   | ✅ WER / MiniDump              | ✅ core + PR_SET_DUMPABLE  | ⬜ §18 deferred (needs §15 + TODO-27) |
| 💎   | Per-process I/O priority      | ✅ ProcessIoPriority           | ✅ ioprio_set/get          | ⬜ Deferred (→ TODO-12 §10)           |
| ⭐   | Drop-only cap inheritance     | ⚠️ Token elevation            | ⚠️ setcap raises ambient  | ⬜ §7 -- monotonic decrease           |
| ⭐   | Pledge/unveil restriction     | ❌ None                        | ❌ No simple equivalent    | ✅ §12 pledge+unveil (SSDT dispatch)  |

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
  - `task_mitigation_apply(MIT_NO_CHILD_PROCESS)` then `NtCreateProcess` returns `STATUS_CHILD_PROCESS_BLOCKED` (§11; ring-3 setter deferred -- usercopy)
  - `task_mitigation_child_set` clear attempt on an already-set `MIT_NO_CHILD_PROCESS` returns -1 (monotonic) (§11)
  - `NtPledge("stdio rpath")` then `NtCreateFile(WRITE)` terminates with `STATUS_PLEDGE_VIOLATION` (§12)
  - `NtUnveil("/C:\\Tmp", "rw")` then `NtUnveil(NULL, NULL)` locks; access to `/C:\\Users` fails (§12)
  - `NtCreateJobObject` creates a job; `NtAssignProcessToJobObject` assigns child (§13)
  - `sys_wait4(-1, ...)` reaps any child; a second reap of the same zombie returns `ECHILD` (§15)
  - `WNOHANG` on a still-running child returns 0 without blocking (§15)
  - After a parent exits, its orphaned child's parent-pid becomes 1 (or the nearest subreaper) (§15)
  - `getrusage(RUSAGE_CHILDREN)` returns non-zero usage after a child with CPU work is reaped (§15)
  - A None-protection process opening a WinTcb process for TERMINATE is denied `STATUS_ACCESS_DENIED` (§16)
  - The PPL dominance matrix: a higher-level process opening a lower one for VM_WRITE succeeds (§16)
  - `pgroup_setsid_decide` denies a process-group leader (EPERM) and allows a non-leader (§17)
  - `pgroup_setpgid_decide` maps every POSIX case: self/child, exec'd child EACCES, cross-session/session-leader/bad-dest EPERM (§17)
  - `GenerateConsoleCtrlEvent` returns FALSE on a bad control event and on an empty target group (§17)
  - `sys_waitid(P_PID, child, WEXITED|WNOWAIT)` reports exit but leaves the zombie reapable by a later `sys_wait4` (§18)
  - Two NT `NtWaitForSingleObject` waiters on the same exited process handle both wake; a `sys_wait4` reaps it exactly once (§18)
  - A process with `dumpable=0` produces no core dump on crash; `RLIMIT_CORE=0` suppresses the dump even when dumpable (§18)
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
- [ ] `task_mitigation_apply(MIT_NO_CHILD_PROCESS)` then `NtCreateProcess` returns `STATUS_CHILD_PROCESS_BLOCKED` (ring-3 `SetProcessMitigationPolicy` deferred -- usercopy)
- [ ] After `NtPledge("stdio rpath")`, write syscall terminates process with `STATUS_PLEDGE_VIOLATION`
- [ ] After `NtUnveil("/C:\\System", "rx")` and lock, access to `C:\Users` returns `STATUS_ACCESS_DENIED`
- [ ] `NtCreateJobObject` creates a job; `NtAssignProcessToJobObject` assigns a child; `NtTerminateJobObject` kills all
- [ ] `sys_wait4(-1)` reaps any child; `WNOHANG` returns 0 for a running child; an orphan reparents to init/subreaper (§15)
- [ ] A lower-protection process cannot TERMINATE/VM_WRITE/DUP_HANDLE a Protected Process Light target (§16)
- [ ] `setsid`/`setpgid` establish session/group IDs; a signal to `-pgid` reaches the whole group (§17)
- [ ] Commit: `"kernel: task -- process model extensions complete"`
