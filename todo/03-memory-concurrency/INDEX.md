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

- None yet.

## Completed / Doc-converted

- None yet.

## Local Naming

- Use `TODO-01-vmm-foundations.md` as the filename style for new leaf TODOs in this folder.
- Create a parent TODO only when one topic needs multiple leaf files or shared verification.
