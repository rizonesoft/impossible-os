# 02 Kernel Core

This domain holds kernel work that is foundational but is not primarily memory management, concurrency, drivers, or filesystems.

## Belongs Here

- Kernel entry sequencing, panic handling, logging, versioning, and core runtime policies.
- Binary loading, executable format integration, registry, and process model foundations.
- Cross-cutting kernel behavior that does not belong to a more specialized subsystem.

## Does Not Belong Here

- PMM, VMM, heap, scheduler, IPC, or SMP work. Put that in [03 Memory Concurrency](../03-memory-concurrency/INDEX.md).
- Driver or filesystem implementation. Put that in [04 Drivers Hardware](../04-drivers-hardware/INDEX.md) or [05 Storage Filesystems](../05-storage-filesystems/INDEX.md).

## Likely Source Areas

- [src/kernel](../src/kernel/)
- [src/kernel/main](../src/kernel/main/)
- [include/kernel](../include/kernel/)

## Epics

- None yet.

## Active TODOs

- [TODO-01 Kernel Init Sequencing](./TODO-01-kernel-init-sequencing.md) — Formal phase model, readiness oracle, dependency gates, and failure policy replacing the current ad-hoc boot sequence.
- [TODO-02 System Logging](./TODO-02-system-logging.md) — Per-subsystem log splitting, rotation, structured JSON events, rate limiting, and remote syslog forwarding. Core klog infrastructure is complete.
- [TODO-03 Object Manager](./TODO-03-object-manager.md) — OBJECT_HEADER/OBJECT_TYPE infrastructure, reference counting, per-process handle table, named object namespace, security descriptors, and Win32 handle APIs. Greenfield — no ObXxx layer exists yet.
- [TODO-04 PEB / TEB & User-Mode ABI](./TODO-04-peb-teb-user-abi.md) — PEB, TEB, swapgs, KERNEL_GS_BASE, RTL_USER_PROCESS_PARAMETERS, Ldr module list, TLS slots, and initial stack frame. Greenfield — task_exec currently enters ring 3 with all-zero registers and no PEB/TEB.

## Completed / Doc-converted

- None yet.

## Local Naming

- Use `TODO-01-kernel-init-sequencing.md` as the filename style for new leaf TODOs in this folder.
- Create a parent TODO only when one topic needs multiple leaf files or shared verification.
