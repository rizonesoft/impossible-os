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

- [TODO-01 AI Development System](./TODO-01-ai-development-system.md) - Cursor and Claude
  Code rules, skills, and MCP policy.

## Active TODOs

- [TODO-01 AI Development System](./TODO-01-ai-development-system.md) - Establish the
  canonical AI operating model before expanding the rest of the infrastructure backlog.
- [TODO-04 CI Notifications & Build Status](./TODO-04-ci-notifications.md) - Branch
  protection, build badge, failure notifications, nightly builds, PR comment bot.
- [TODO-05 Kernel Test Suites & Coverage](./TODO-05-test-suites-coverage.md) - Per-subsystem
  test suites (OB, security, timer, IPC, VMM, ELF, NVMe, USB), 100+ assertions, coverage report.
- [TODO-06 User-Mode Test Framework](./TODO-06-usermode-test-framework.md) - Test binaries
  for syscalls, libc, IPC, process lifecycle, file I/O, Win32 API — real user-mode programs
  exercising the real syscall interface.
- [TODO-07 Desktop & UI Test Framework](./TODO-07-desktop-ui-test-framework.md) - Framebuffer
  snapshots, input injection, terminal verification, visual regression CI, WM state introspection.

## Completed / Doc-converted

- ~~TODO-02 Developer Tooling Stack~~ → [docs/infrastructure/development-tooling.md](../../docs/infrastructure/development-tooling.md) — toolchain, build, run/debug, host utilities, GitHub sync (completed 2026-04-02)
- ~~TODO-03 Kernel Test Framework~~ → [docs/infrastructure/kernel-test-framework.md](../../docs/infrastructure/kernel-test-framework.md) — test_runner, make test, pre-push hook, coverage tracking (completed 2026-04-02)

## Local Naming

- Use `TODO-01-build-toolchain.md` as the filename style for new leaf TODOs in this folder.
- Create a parent TODO only when one topic needs multiple leaf files or shared verification.
