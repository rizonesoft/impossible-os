# Memory and Concurrency

Memory management and concurrency: virtual memory protection, memory security, the kernel heap, paging and reclaim, advanced virtual memory, the scheduler, SMP, synchronisation primitives, Win32 IPC, concurrency diagnostics and warm kernel update.

## Roadmap Overviews

One page per memory and concurrency roadmap file, each following the [page contract](../contributing/docs-page-contract.md).

| Document | Topics |
| --- | --- |
| [Virtual Memory Protection](vmm-memory-protection.md) | Page permissions, guard pages, MMIO mappings, user address spaces, NT memory services |
| [Memory Security Hardening](memory-security.md) | NX, SMEP and SMAP status, user copies, KPTI, ASLR and CET plans |
| [Kernel Heap and Allocators](advanced-allocator.md) | Fixed 2 MiB heap, cookies and redzones, tags, planned SLAB and pools |
| [Pager, Reclaim and Working Sets](pager-reclaim-working-set.md) | Pagefile slots, clock victims, eager file mappings, planned reclaim |
| [Advanced Virtual Memory](advanced-virtual-memory.md) | Large pages, fork, section objects, Job Objects, AWE |
| [Scheduler](scheduler.md) | Flat round-robin, 32 priorities, priority inheritance, per-thread kernel stacks |
| [SMP Phase 2](smp-phase2.md) | Online CPU set, rendezvous, barriers, RCU, planned TLB shootdown |
| [Synchronisation Primitives](advanced-sync.md) | Spinlocks, mutexes, events, NT wait objects, planned futexes |
| [Win32 IPC Extensions](win32-ipc-extensions.md) | Anonymous pipes, completion ports, planned named pipes and mailslots |
| [Concurrency and Memory Diagnostics](concurrency-diagnostics.md) | Stack guards, heap redzones, planned lockdep, KCSAN and KASAN |
| [Warm Kernel Update](warm-kernel-update.md) | Warm-update handoff contract, sealed selection, planned runtime |
