# 03 Memory Concurrency

This domain tracks the execution and memory model of the OS: allocation, virtual memory, tasks, locks, IPC, and multi-core behavior.

## Belongs Here

- PMM, VMM, heap, swap, mmap, and other memory-management work.
- Task and thread lifecycle, locks, scheduler behavior, workqueues, and timing-critical synchronization.
- IPC and SMP coordination, including pipes, signals, shared memory, and cross-core execution behavior.

## Does Not Belong Here

- Bootloader and early handoff work. Put that in [01 Boot Platform](../01-boot-platform/INDEX.md).
- Peripheral and bus driver work. Put that in [04 Drivers Hardware](../04-drivers-hardware/INDEX.md).

## Likely Source Areas

- [src/kernel/mm](../src/kernel/mm/)
- [src/kernel/sched](../src/kernel/sched/)
- [src/kernel/ipc](../src/kernel/ipc/)
- [src/kernel/smp](../src/kernel/smp/)

## Epics

- None yet.

## Active TODOs

- [TODO-01 VMM Memory Protection & Diagnostics](./TODO-01-vmm-memory-protection.md) -- `mprotect`, W^X, demand paging, `NtQueryVirtualMemory`, `VirtualAlloc` Win32 wrappers, kmalloc lint, heap canaries, leak detector, PMM statistics.
- [TODO-02 Memory Security Hardening](./TODO-02-memory-security.md) -- ASLR, KASLR, SMEP, SMAP + `copy_from_user`, NX/DEP audit, KPTI, CET shadow stack stub, security layout report.
- [TODO-03 Advanced Kernel Allocator](./TODO-03-advanced-allocator.md) -- The ultimate kernel allocator: growable SMP-safe heap, SLAB with lock-free per-CPU fast path, vmalloc, tagged allocation, NonPagedPool/PagedPool, memory pressure. Beyond parity: out-of-band metadata, type-isolated pools (kalloc_type model), probabilistic guard pages (KFENCE model), production quarantine, per-page encoding keys, zero-on-free, bulk alloc/free. 15 sections (8 parity + 7 exclusive).
- [TODO-04 Pager, Reclaim, and Working Set Manager](./TODO-04-pager-reclaim-working-set.md) -- pagefile ownership, working-set trim/residency, background reclaim, modified-page writer, lazy mapped-file faults, refault-aware replacement policy, and pager telemetry.
- [TODO-05 Advanced Virtual Memory](./TODO-05-advanced-virtual-memory.md) -- COW `fork()`, 2 MiB/1 GiB huge pages, `madvise`/`MEM_RESET`, Section Objects, Job Object memory limits, zero-copy DMA pool, compressed memory, NUMA PMM, THP collapser.
- [TODO-06 Scheduler Enhancement](./TODO-06-scheduler-enhancement.md) -- 40-level O(1) priority queues, dynamic priority aging, CFS vruntime, `SCHED_FIFO`/`SCHED_RR`/`SCHED_DEADLINE`, CPU affinity, tick calibration, `/sys/sched`, CPU frequency scaling hook.
- [TODO-07 SMP Phase 2](./TODO-07-smp-phase2.md) -- SMP-safe atomics audit, TLB shootdown IPI, per-CPU run queues, work-stealing load balancer, adaptive mutex spin-on-owner, per-CPU RCU with `call_rcu`, CPU hotplug stub, `READ_ONCE`/`WRITE_ONCE`, CPU feature intersection.
- [TODO-08 Advanced Synchronisation Primitives](./TODO-08-advanced-sync.md) -- ticket locks, preemption count, TLS via `FS` base MSR, futexes, `pthread_once`/`call_once`, `pthread_barrier_t` (kernel-level), thread cancellation, `WaitForMultipleObjects`/`waitable_t`, unified `_timeout(ms)` API.
- [TODO-09 Win32 IPC Extensions & Async I/O](./TODO-09-win32-ipc-extensions.md) -- named pipes (NPFS), pipe instances + overlapped I/O, mailslots (MSFS), IOCP, IOCP worker pool + DMA hook, Ob-backed named sync objects, LPC, ALPC, `ImpossibleRing` zero-syscall async submission.
- [TODO-10 Concurrency & Memory Diagnostics](./TODO-10-concurrency-diagnostics.md) -- stack guard pages, preemption count debug, heap canaries + SLAB red zones, lockdep (lock-class graph + DFS), kernel watchdog, KCSAN, graphical deadlock visualiser, `/sys/locks` named lock browser, KASAN, `/sys/mem` unified memory stats.

## Completed / Doc-converted

- None yet.

## Local Naming

- Use `TODO-01-vmm-foundations.md` as the filename style for new leaf TODOs in this folder.
- Create a parent TODO only when one topic needs multiple leaf files or shared verification.
