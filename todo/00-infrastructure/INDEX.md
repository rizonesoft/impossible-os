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
- [TODO-02 Developer Tooling Stack](./TODO-02-developer-tooling-stack.md) - Parent
  epic for the git-tracked developer tooling contract, including build, run, host
  utilities, and GitHub synchronization.

## Active TODOs

- [TODO-01 AI Development System](./TODO-01-ai-development-system.md) - Establish the
  canonical AI operating model before expanding the rest of the infrastructure backlog.
- [TODO-02 Developer Tooling Stack](./TODO-02-developer-tooling-stack.md) - Define the
  parent roadmap for toolchain bootstrap, build orchestration, run/debug tooling, host
  utilities, and GitHub workflow parity.

## Completed / Doc-converted

- None yet.

## Local Naming

- Use `TODO-01-build-toolchain.md` as the filename style for new leaf TODOs in this folder.
- Create a parent TODO only when one topic needs multiple leaf files or shared verification.
