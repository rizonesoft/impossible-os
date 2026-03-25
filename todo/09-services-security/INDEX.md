# 09 Services Security

This domain holds shared services and policy engines that sit above kernel primitives but below end-user applications.

## Belongs Here

- Clipboard, search, accounts, crypto, resource services, and background service orchestration.
- Security and policy layers shared across apps and platform surfaces.
- System-wide service behavior that multiple apps depend on.

## Does Not Belong Here

- One-off app features or UX work. Put that in [10 Apps](../10-apps/INDEX.md).
- Low-level kernel primitives such as memory management, locks, or syscalls. Put that in [02 Kernel Core](../02-kernel-core/INDEX.md) or [03 Memory Concurrency](../03-memory-concurrency/INDEX.md).

## Likely Source Areas

- [src/kernel](../src/kernel/)
- [src/desktop](../src/desktop/)
- [user](../user/)

## Epics

- None yet.

## Active TODOs

- None yet.

## Completed / Doc-converted

- None yet.

## Local Naming

- Use `TODO-01-clipboard-service.md` as the filename style for new leaf TODOs in this folder.
- Create a parent TODO only when one topic needs multiple leaf files or shared verification.
