# 00 Infrastructure

This domain tracks the tooling and workflow work that supports the whole project.

## Belongs Here

- Build scripts, toolchain setup, host-side utilities, CI, and automation.
- Test harnesses, VM launchers, debug workflows, and developer diagnostics.
- Repo-wide workflow improvements that affect how the project is built and validated.

## Does Not Belong Here

- Bootloader or kernel feature implementation. Put that in [01 Boot Platform](../01-boot-platform/INDEX.md), [02 Kernel Core](../02-kernel-core/INDEX.md), or [03 Memory Concurrency](../03-memory-concurrency/INDEX.md).
- Installer and release media work. Put that in [12 Installer Release](../12-installer-release/INDEX.md).

## Likely Source Areas

- [scripts](../scripts/)
- [tools](../tools/)
- [docs](../docs/)

## Epics

- None currently.

## Active TODOs

- [TODO-01 User-Mode Test Framework](./TODO-01-usermode-test-framework.md) - Test binaries
  for syscalls, libc, IPC, process lifecycle, file I/O, Win32 API -- real user-mode programs
  exercising the real syscall interface.
- [TODO-02 Desktop & UI Test Framework](./TODO-02-desktop-ui-test-framework.md) - Framebuffer
  snapshots, input injection, terminal verification, visual regression CI, WM state introspection.

## Completed / Doc-converted

- None yet.

## Local Naming

- Use `TODO-01-build-toolchain.md` as the filename style for new leaf TODOs in this folder.
- Create a parent TODO only when one topic needs multiple leaf files or shared verification.
