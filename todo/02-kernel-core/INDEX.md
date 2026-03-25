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

- None yet.

## Completed / Doc-converted

- None yet.

## Local Naming

- Use `TODO-01-kernel-init-sequencing.md` as the filename style for new leaf TODOs in this folder.
- Create a parent TODO only when one topic needs multiple leaf files or shared verification.
