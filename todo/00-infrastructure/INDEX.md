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
  Claude Code doctrine, skills, hooks, permissions, sole external reviewer (Codex; Copilot CLI
  retired 2026-04-28 per TODO-08 §14), drift audit, and AI workflow regression checks.
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
- [TODO-07 LSP to MCP Bridge](./TODO-07-lsp-mcp-bridge.md) - Host-side MCP server that proxies
  five language servers (clangd for C/H, asm-lsp for NASM, bash-language-server, pyright,
  PowerShellEditorServices) and exposes six read-only MCP tools (hover, definition, references,
  diagnostics, workspace-symbol, document-symbol). Gives Claude Code + any MCP-aware agent
  compiler-grade code intelligence across every language the repo uses. XREFs: TODO-01 §1
  (dep tier), TODO-02 §5/§8 (MCP + autonomous-agent boundary), TODO-06 §8 (FastMCP precedent).
- [TODO-08 Automation Hardening](./TODO-08-automation-hardening.md) - Wire the two MCP servers
  (todo-graph + lsp-bridge) into Codex CLI alongside Claude Code; document and enforce the
  no-`--model`/no-`--effort` Codex invocation policy; convert advisory hooks into hard gates
  for `receiving-code-review`, the section-commit pipeline, and `review-todo-section` step-8
  four-dispatch policy; audit and dedupe the hook system; reconcile the superpowers plugin
  catalog with Impossible OS doctrine; cross-tool drift detection. XREFs: TODO-02 §3/§4 (Hook
  Routing Matrix + External-Reviewer Contract -- mechanism owner; TODO-02 owns doctrine),
  TODO-06 §8 (todo-graph MCP consumer), TODO-07 §1 (lsp-bridge MCP consumer).
- [TODO-09 Repository Transfer to rizonetech](./TODO-09-repository-transfer-rizonetech.md) -
  Transfer `rizonesoft/impossible-os` to `rizonesoft/impossible-os` without avoidable GitHub
  Pages, custom-domain, workflow, release-link, or policy disruption; preserve a documented
  move-back path for the later public-visibility transition.
  Make the development driver interchangeable between Claude Code and Codex while Codex remains
  the required reviewer, using a shared lease, evidence ledger, obligation resolver, stamp writer,
  and cross-driver gates to preserve flow and avoid duplicate work.

## Completed / Doc-converted

- None yet.

## Local Naming

- Use `TODO-01-build-toolchain.md` as the filename style for new leaf TODOs in this folder.
- Create a parent TODO only when one topic needs multiple leaf files or shared verification.
