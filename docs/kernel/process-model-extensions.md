<!-- docs: covers=todo/02-kernel-core/TODO-21-process-model-extensions.md sources=include/kernel/sched/task.h,src/kernel/sched/task.c,include/kernel/nt/pledge.h,src/kernel/nt/pledge.c,include/kernel/ipc/pgroup.h,src/kernel/ipc/pgroup.c,include/kernel/task_limits.h,include/kernel/fs/vfs.h,src/kernel/fs/vfs.c reviewed=2026-09-28 order=21 -->
# Process Model Extensions

## What is it?

`struct task` needs per-process state that belongs to none of the scheduler, VMM or Object Manager individually: a current working directory, resource limits, process groups and sessions, and a transactional exec commit point. This roadmap adds exactly that fields-and-syscalls layer. Some pieces are complete and reviewed: working directory resolution, OpenBSD-style `pledge`/`unveil` restriction, process groups and sessions, and the `task_exec` commit point. Most of the rest (program break, priority classes, capabilities, CPU affinity, Protected Process Light) is deliberately deferred: each was design-reviewed and found to need infrastructure (per-process physical isolation, fault-safe usercopy, real OB process handles) that does not exist yet, and the project's own rule is never to ship a half-enforced security feature.

## How does it work?

`struct task` carries `cwd[TASK_CWD_MAX]` under a `cwd_lock`, initialized to `"C:\\"` and inherited across fork. `vfs_resolve_path()` (`vfs.h`) is the canonicalizer: it joins a relative path onto the cwd, normalizes separators, collapses `.` and empty components, and applies `..` without ever escaping the drive root; `NtSetCurrentDirectory`/`NtQueryCurrentDirectory` (SSDT `0x03D9`/`0x03DA`) are the syscalls on top of `task_get_cwd()`/`task_set_cwd()`.

`pledge.c` implements the OpenBSD-inspired restriction pair. `NtPledge()` (SSDT `0x03DB`) narrows a `pledge_mask` bitfield by intersection only, never by union, so a second pledge call can only shrink the promise set; `NtUnveil()` (`0x03DC`) builds a folded, longest-prefix-wins allowlist of paths and permissions that locks when called with `NULL, NULL`. Enforcement is split: a coarse category check runs in `ssdt_dispatch` after the syscall bitmap filter, and fine per-call checks live in the four path-resolving NT file handlers, because the dispatcher itself only sees a service number. The legacy INT 0x80 ABI is gated the same way, closing a bypass an early adversarial review found. A violation terminates the process with `STATUS_PLEDGE_VIOLATION` (`0xE0000201`).

`pgroup.c` adds `pgid`/`sid`/`has_execed` to `struct task`, inherited from the creator at fork or create. `pgroup_setpgid()`/`pgroup_setsid()` implement the full POSIX errno matrix (a session-group leader cannot `setsid`, an already-exec'd child cannot `setpgid`), and `signal_send_group()` fans a signal out to every live member of a group, including the Ctrl+C path, via a singleton console job-control object tracking the foreground group. The fan-out sets each member's pending-signal bit; nothing yet drains it (see below).

`task_exec()` gained a real transactional commit point: the fallible allocations it can stage (the argv table, the replacement kernel stack, the private-frame table) happen before an irrevocable remap, so one of those failing returns `-1` into the caller's still-intact old image instead of corrupting it. Past that point, nothing returns to ring 3: a post-commit failure, whether the loader rejecting the binary or the PEB or TEB allocation failing, terminates the task with the named exit status `TASK_EXIT_EXEC_IMAGE_DESTROYED` rather than the ambiguous bare `-1` that a signal kill also produces. Publication (PEB/TEB, `kernel_gs_base`, the new image) is the last act, under `local_irq_save`, and the previous kernel stack is parked in `task.stack_pending_free` for reclamation at the next exec or at reap.

Per-process resource limits (`rlimits[RLIM_NLIMITS]` in `task_limits.h`) use Linux UAPI numbering so a future `getrlimit` can index the array directly; `task_rlimit_get()`/`task_rlimit_set()` are locked accessors that require privilege to raise a hard limit, and every create/fork path inherits the creator's full array.

## What are its interfaces?

| Interface | Purpose |
| --- | --- |
| `task_get_cwd()`, `task_set_cwd()`, `vfs_resolve_path()` | Per-process working directory and relative-path resolution ([`task.h`](../../include/kernel/sched/task.h), [`vfs.h`](../../include/kernel/fs/vfs.h)) |
| `NtSetCurrentDirectory`, `NtQueryCurrentDirectory` | SSDT `0x03D9`/`0x03DA` |
| `NtPledge`, `NtUnveil` | SSDT `0x03DB`/`0x03DC`; irreversible syscall and filesystem restriction ([`pledge.h`](../../include/kernel/nt/pledge.h)) |
| `pgroup_setpgid()`, `pgroup_setsid()`, `pgroup_tcsetpgrp()`, `signal_send_group()` | Process groups, sessions and group signal fan-out ([`pgroup.h`](../../include/kernel/ipc/pgroup.h)) |
| `NtCreateJobObject` .. `NtCreateJobSet` | SSDT `0x0160`-`0x0167`; job lifecycle, membership and accounting queries |
| `task_rlimit_get()`, `task_rlimit_set()`, `task_rlimit_inherit()` | Locked resource-limit accessors ([`task_limits.h`](../../include/kernel/task_limits.h)) |
| `task_exec()`, `struct task_exec_staging`, `TASK_EXIT_EXEC_IMAGE_DESTROYED` | The transactional exec commit point and its named failure status |

## How do I use it?

These are kernel-internal fields and syscalls with no separate enable step; they are exercised through the SSDT and INT 0x80 surfaces.

```bash
bash scripts/test.sh SUITE=sched   # or: make test-sched
```

The `ProcExt:` suites cover the working-directory resolver, pledge/unveil decision cores, process-group POSIX errno matrix, rlimit accessors, and the exec commit-point's kernel-stack reclamation. Fault-injected exec lifecycle coverage (post-commit termination, pre-commit refusal with the old image intact) is ring-3, in `user/test/test_process.c`, because a kernel unit test cannot safely call `task_exec` itself. The Windows batch runner is `scripts\debug\kernel\run-sched-tests.bat` (`SUITE=sched`).

## What is not implemented yet?

- Standard handles (`STD_INPUT_HANDLE` etc.) are not pre-wired at process creation: the section assumed keyboard/framebuffer VFS device nodes that do not exist, since console I/O actually runs over pipes ([Standard Handle Pre-Wiring](../../todo/02-kernel-core/TODO-21-process-model-extensions.md#2-standard-handle-pre-wiring-at-process-creation)).
- `brk`/`sbrk` Linux-compat heap is blocked on per-process physical frame isolation: a private mapping inside the kernel's identity-mapped low memory would shadow kernel physical-frame access under a user CR3 ([User-Mode Program Break](../../todo/02-kernel-core/TODO-21-process-model-extensions.md#3-user-mode-program-break-brksbrk----linux-compat)).
- Process priority classes, capabilities, and CPU affinity are all deferred pending the same two blockers: raw-user-pointer syscall handlers with no fault-safe usercopy, and process handles that resolve by raw PID with no granted-access check ([Process Priority Class](../../todo/02-kernel-core/TODO-21-process-model-extensions.md#4-process-priority-class), [Process Capabilities](../../todo/02-kernel-core/TODO-21-process-model-extensions.md#6-process-capabilities-and-privilege-bitmask), [CPU Affinity per Process](../../todo/02-kernel-core/TODO-21-process-model-extensions.md#10-cpu-affinity-per-process)).
- Per-process mitigation policy ships only `MIT_NO_CHILD_PROCESS`; every other Win11 `PROCESS_MITIGATION_*` flag is deferred until it ships with real enforcement, and the ring-3 query/set syscalls both return `STATUS_NOT_SUPPORTED` ([Per-Process Mitigation Policy](../../todo/02-kernel-core/TODO-21-process-model-extensions.md#11-per-process-mitigation-policy)).
- Rich `wait4`/`waitid` semantics (`WNOHANG`, orphan reparenting, a real `TASK_ZOMBIE` state) are blocked on separating a stable PID from its storage slot ([Process Parenting, Reaping, and Wait Semantics](../../todo/02-kernel-core/TODO-21-process-model-extensions.md#15-process-parenting-reaping-and-wait-semantics)).
- Protected Process Light is blocked on real OB process handles: today process operations resolve a target by raw PID, so stripping access at handle-open would enforce nothing ([Protected Process Light](../../todo/02-kernel-core/TODO-21-process-model-extensions.md#16-protected-process-light-ps_protection)).
- Group-signal fan-out (including Ctrl+C) sets a pending bit that nothing drains yet: `signal_check()` is defined but never called from any thread-context return path ([Process Groups and Sessions](../../todo/02-kernel-core/TODO-21-process-model-extensions.md#17-process-groups-and-sessions)).

## How does it compare with Windows 11 and Linux?

Windows NT and Linux both provide the full set of process-level controls this roadmap targets: access tokens vs. POSIX capabilities, `SetPriorityClass` vs. `nice`, Job Objects vs. cgroups, `wait`/handle-signaling semantics, and PS_PROTECTION vs. no equivalent on the Linux side. Impossible OS currently matches both on the working directory, matches Linux on process groups and sessions, and matches Linux's exec point of no return: a failure after the commit point kills the process (here with the named exit status `TASK_EXIT_EXEC_IMAGE_DESTROYED`), while an earlier failure returns to the intact old image. Windows has no exec, so that row does not apply to it. Section 12's pledge/unveil pair has no equivalent in either OS: it is simpler than Linux seccomp-bpf and stronger than Capsicum, and section 7's strictly drop-only capability inheritance is a stronger guarantee than either Windows token elevation or Linux ambient capabilities once section 6 lands. Priority classes, capabilities, resource-limit enforcement, CPU affinity, Job Object quota enforcement, and Protected Process Light are all still open gaps against both operating systems.

## See also

- [Process Model Extensions roadmap](../../todo/02-kernel-core/TODO-21-process-model-extensions.md)
- [Object Manager](object-manager.md)
- [Native API and SSDT](native-api-ssdt.md)
- [Code Integrity and Trust Policy](code-integrity-trust-policy.md)
