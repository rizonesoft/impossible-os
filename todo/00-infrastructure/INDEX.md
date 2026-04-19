# 00 Infrastructure

This domain tracks the tooling and workflow work that supports the whole project.

## Belongs Here

- Build scripts, toolchain setup, host-side utilities, CI, and automation.
- Test harnesses, VM launchers, debug workflows, and developer diagnostics.
- Repo-wide workflow improvements that affect how the project is built and validated.

## Does Not Belong Here

- Bootloader or kernel feature implementation. Put that in [01 Boot Platform](../01-boot-platform/INDEX.md), [02 Kernel Core](../02-kernel-core/INDEX.md), or [03 Memory Concurrency](../03-memory-concurrency/INDEX.md).
- Installer and release media work. Put that in [15 Installer Release](../15-installer-release/INDEX.md).

## Likely Source Areas

- [scripts](../../scripts/)
- [tools](../../tools/)
- [docs](../../docs/)

## Epics

- None currently.

## Active TODOs

- [TODO-01 Developer Tooling Stack](./TODO-01-developer-tooling-stack.md) - Canonical host-side
  developer workflow for setup, build, test, run, debug, hooks, CI, artifacts, and a tooling
  doctor/regression pack.
- [TODO-02 AI Development System](./TODO-02-ai-development-system.md) - Canonical ownership for
  Claude Code doctrine, skills, hooks, permissions, external reviewers (Codex/Copilot),
  drift audit, and AI workflow regression checks.
- [TODO-03 Kernel Test Harness](./TODO-03-kernel-test-harness.md) - Kernel-internal test-time
  infrastructure: `kmalloc_fail_countdown` fault injection, `test_race_barrier_t` deterministic
  race fence, `TEST_SCRATCH_KBUF` > 4 KiB scratch buffers. Closes the "Test gaps (NO current
  owner)" block in TODO-12 §5 and similar deferred gaps elsewhere.
- [TODO-04 User-Mode Test Framework](./TODO-04-usermode-test-framework.md) - Test binaries
  for syscalls, libc, IPC, process lifecycle, file I/O, Win32 API -- real user-mode programs
  exercising the real syscall interface.
- [TODO-05 Desktop & UI Test Framework](./TODO-05-desktop-ui-test-framework.md) - Framebuffer
  snapshots, input injection, terminal verification, visual regression CI, WM state introspection.
- [TODO-06 TODO Metadata Layer](./TODO-06-todo-metadata-layer.md) - Stable-ID frontmatter
  on every TODO file + generator/validator/query CLI that builds a derived graph cache. Fixes
  the renumbering-drift and stale-XREF pain exposed by TODO-02 §1-§3 work. Canonical markdown
  stays authoritative; the cache is a read-only projection.

## Completed / Doc-converted

- None yet.

## Local Naming

- Use `TODO-01-build-toolchain.md` as the filename style for new leaf TODOs in this folder.
- Create a parent TODO only when one topic needs multiple leaf files or shared verification.
