<!-- docs: covers=todo/03-memory-concurrency/TODO-10-concurrency-diagnostics.md sources=include/kernel/mm/vmm.h,src/kernel/mm/heap.c,src/kernel/sched/task.c,include/kernel/sched/task.h,include/kernel/smp.h,src/kernel/nt/nt_syscall.c,src/kernel/test/test_vmm.c,src/kernel/test/test_heap.c reviewed=2026-09-28 order=10 -->
# Concurrency and Memory Diagnostics

## What is it?

The debugging tools that catch concurrency and memory bugs while the kernel runs: stack guard pages, heap canaries, a lock-order validator, a soft-lockup watchdog, data-race and address sanitisers, a lock browser and one memory statistics file. Two of these exist in other forms (guard pages on the main kernel stack of each process, and heap cookies with redzones); the validators, sanitisers, watchdog and browsers are not built, and every row of this roadmap's table is still open.

## How does it work?

**Kernel stack guard pages.** `task_create()` in [`task.c`](../../src/kernel/sched/task.c) allocates each kernel stack of `TASK_STACK_SIZE` (8 KiB) plus one page, and turns the lowest page into a guard with `vmm_install_guard_page()` and the label `GUARD: kernel task stack overflow`. If the guard cannot be installed the task is refused rather than run unguarded, and user thread kernel stacks get the same treatment. Two creation paths do not: `task_fork()` gives the child a kernel stack from `kmalloc()`, and so does `kthread_create()` for kernel threads, so an overflow there corrupts the heap instead of faulting. On a guarded stack an overflow faults on the guard, and the page-fault handler prints the label from `vmm_guard_page_label()` ([`vmm.h`](../../include/kernel/mm/vmm.h)). The guard table holds `VMM_MAX_GUARD_PAGES` (640) entries, sized for `TASK_MAX` (32) tasks with up to `THREAD_MAX` (16) threads each plus headroom.

**Heap corruption checks.** Every `kmalloc()` block in [`heap.c`](../../src/kernel/mm/heap.c) carries a header cookie, a front redzone and an 8-byte back redzone. They are checked on free and realloc; a mismatch poisons the heap and bugchecks with `BUGCHECK_IOS_HEAP_CORRUPTION`, reporting whether the front or back redzone was hit. This is always on, not a debug build option, and it covers the general heap only; there is no SLAB allocator for it to cover yet (see [Kernel Heap and Allocators](advanced-allocator.md)).

**Preemption counter.** Each CPU's `struct per_cpu_data` has a `preempt_count` field ([`smp.h`](../../include/kernel/smp.h)), zeroed at CPU bring-up. No API increments it yet.

**Memory statistics.** `NtQuerySystemInformation(SystemPerformanceInformation)` in [`nt_syscall.c`](../../src/kernel/nt/nt_syscall.c) returns three page counts (available, used and total frames from the frame allocator) and requires `SeSystemProfilePrivilege` from a user-mode caller.

## What are its interfaces?

| Interface | Purpose |
| --- | --- |
| `vmm_install_guard_page()`, `vmm_uninstall_guard_page()`, `vmm_guard_page_label()`, `vmm_guard_pages_free()` | Labelled guard pages ([`vmm.h`](../../include/kernel/mm/vmm.h)) |
| `kmalloc()`, `kfree()` | Cookie and redzone checking on every block ([`heap.h`](../../include/kernel/mm/heap.h)) |
| `per_cpu_data.preempt_count` | Per-CPU preemption nesting field, not yet driven |
| `NtQuerySystemInformation` class 2 | Available, used and total page counts |

## How do I use it?

An overflow of a guarded kernel stack, or heap corruption, shows up on the serial log and the panic screen with its label or fault class; there is nothing to enable. Guard page installation, saturation and refusal paths are tested in [`test_vmm.c`](../../src/kernel/test/test_vmm.c), and redzone exact fit in [`test_heap.c`](../../src/kernel/test/test_heap.c):

```bash
bash scripts/test.sh SUITE=mm
```

## What is not implemented yet?

- **Guards on every stack.** Guards ship under different names on the `task_create()` and user-thread paths, but forked tasks and kernel threads still run on unguarded `kmalloc()` stacks ([Thread Stack Guard Pages](../../todo/03-memory-concurrency/TODO-10-concurrency-diagnostics.md#1-thread-stack-guard-pages-sonnet)).
- **Preemption debugging.** No `preempt_disable()`, depth limit or `preemptible()` check ([Preemption Count -- Debug Extensions](../../todo/03-memory-concurrency/TODO-10-concurrency-diagnostics.md#2-preemption-count----debug-extensions-opus)).
- **SLAB red zones and a free-time canary** ([Heap Canaries + SLAB Red Zones](../../todo/03-memory-concurrency/TODO-10-concurrency-diagnostics.md#3-heap-canaries--slab-red-zones-sonnet)).
- **Lockdep** for lock-order validation ([Lock Dependency Validator](../../todo/03-memory-concurrency/TODO-10-concurrency-diagnostics.md#4-lock-dependency-validator-lockdep-opus)).
- **A soft-lockup watchdog thread.** The ACPI hardware watchdog in [Boot Watchdog](../boot/boot-watchdog.md) is a different feature ([Kernel Watchdog Thread](../../todo/03-memory-concurrency/TODO-10-concurrency-diagnostics.md#5-kernel-watchdog-thread-sonnet)).
- **KCSAN and KASAN** ([KCSAN](../../todo/03-memory-concurrency/TODO-10-concurrency-diagnostics.md#6-kcsan----kernel-concurrency-sanitiser-opus), [KASAN](../../todo/03-memory-concurrency/TODO-10-concurrency-diagnostics.md#9-kasan----kernel-address-sanitiser-opus)).
- **A graphical deadlock view and `/sys/locks`** ([Graphical Deadlock Visualisation](../../todo/03-memory-concurrency/TODO-10-concurrency-diagnostics.md#7-graphical-deadlock-visualisation-opus), [Named Lock Browser](../../todo/03-memory-concurrency/TODO-10-concurrency-diagnostics.md#8-named-lock-browser-syslocks-sonnet)).
- **`/sys/mem`**, one memory statistics file for Win32 and Linux-style readers ([`/sys/mem` Unified Memory Observability](../../todo/03-memory-concurrency/TODO-10-concurrency-diagnostics.md#10-sysmem-unified-memory-observability-sonnet)).

## How does it compare with Windows 11 and Linux?

Windows 11 has stack guards, Driver Verifier special pool and deadlock detection, a watchdog bugcheck and KASAN in test builds; Linux has `CONFIG_SLUB_DEBUG`, lockdep, the lockup detector, KCSAN, KASAN and `/proc/meminfo`. Linux also guards kernel stacks in production through `CONFIG_VMAP_STACK`. Impossible OS has stack guards on most kernel stacks and always-on heap redzones, which Linux enables only with SLUB debugging, but none of the validators or sanitisers. The two planned additions neither ships are a framebuffer graph of a deadlock cycle and a unified `/sys/mem`.

## See also

- [Concurrency and Memory Diagnostics roadmap](../../todo/03-memory-concurrency/TODO-10-concurrency-diagnostics.md)
- [Kernel Heap and Allocators](advanced-allocator.md)
- [Kernel Security Hardening](../kernel/kernel-security-hardening.md)
- [Panic Screen and Crash Experience](../kernel/panic-screen-crash-experience.md)
- [Boot Watchdog](../boot/boot-watchdog.md)
