# 11 User Platform SDK

This domain owns the contract between the OS and user-mode software: executable formats, Win32 surface, SDK assets, and developer-facing platform behavior.

## Belongs Here

- User-mode ABI, executable loading behavior, PE or EIF integration, and Win32-compatible API surface work.
- SDK headers, samples, developer tooling, and documentation for building user-mode software.
- Compatibility layers and developer contract decisions that shape how apps target the platform.

## Does Not Belong Here

- One specific built-in app backlog. Put that in [10 Apps](../10-apps/INDEX.md).
- Kernel-internal implementation details that have no user-mode contract impact. Put that in the owning kernel domain.

## Likely Source Areas

- [user](../user/)
- [sdk](../sdk/)
- [include](../include/)
- [src/kernel/elf.c](../src/kernel/elf.c)

## Epics

- None yet.

## Active TODOs

- None yet.

## Completed / Doc-converted

- None yet.

## Local Naming

- Use `TODO-01-win32-api-surface.md` as the filename style for new leaf TODOs in this folder.
- Create a parent TODO only when one topic needs multiple leaf files or shared verification.
