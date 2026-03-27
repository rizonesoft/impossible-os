# .impossible — Agent-Agnostic Project Guidance

> **Single source of truth for all AI agent rules and workflows.**
> This directory is tool-neutral. Rules are plain markdown. Every AI coding tool — Cursor, Claude Code, GitHub Copilot, Antigravity — reads from here via thin adapters.

## What's Here

| Path | Purpose |
|------|---------|
| `rules/` | Coding conventions and constraints (tool-neutral) |
| `workflows/` | Step-by-step procedures (tool-neutral) |
| `context.md` | Current project state — read this first each session |
| `index.md` | This file |

## Agent Adapters

| Agent | Instructions file | Skills/Commands | Rules |
|-------|-------------------|-----------------|-------|
| **Cursor** | `.cursor/rules/*.mdc` → references here | `.cursor/skills/` (native) | Thin MDC wrappers |
| **Claude Code** | `CLAUDE.md` → references here | `.claude/commands/` + `.claude/skills/` | Inline critical rules |
| **Antigravity** | `AGENTS.md` → references here | `.agents/workflows/` (redirect stubs) | Inline via AGENTS.md |
| **GitHub Copilot** | `.github/copilot-instructions.md` → references here | None | Inline critical rules |

## Rules Index

| File | Applies To | Summary |
|------|-----------|---------|
| [build.md](rules/build.md) | All source files, scripts | Build commands, safety boundaries, command discipline |
| [freestanding.md](rules/freestanding.md) | `src/kernel/`, `include/kernel/` | No stdlib, allocator rules, crash debugging |
| [assembly.md](rules/assembly.md) | `**/*.asm` | NASM x86-64, UEFI-era constraints |
| [api-surface.md](rules/api-surface.md) | All source, docs, SDKs | Win32-first, Windows paths, production standard |
| [doc-sync.md](rules/doc-sync.md) | All files | Update docs in same task as code changes |
| [mcp-usage.md](rules/mcp-usage.md) | All files | Srclight-only MCP; repo truth over MCP state |
| [todo-style.md](rules/todo-style.md) | `todo/**/*.md` | Lean TODO markdown, compact tables |

## Workflows Index

| File | Description |
|------|------------|
| [build.md](workflows/build.md) | Build + test in QEMU |
| [implement-todo.md](workflows/implement-todo.md) | Implement a TODO section from start to commit |
| [todo-create.md](workflows/todo-create.md) | Create a new TODO file |
| [todo-validate.md](workflows/todo-validate.md) | Review a TODO for gaps and fix everything |
| [todo-done-check.md](workflows/todo-done-check.md) | Check for completed TODO items and mark done |
| [todo-master-sync.md](workflows/todo-master-sync.md) | Fix implementation order, numbering, inconsistencies |
| [todo-table-format.md](workflows/todo-table-format.md) | Align TODO tables and format columns |
| [docs-convert-todo.md](workflows/docs-convert-todo.md) | Convert a completed TODO to documentation |
| [docs-validate.md](workflows/docs-validate.md) | Validate documentation structure and fix violations |
| [docs-verify-todo.md](workflows/docs-verify-todo.md) | Verify a previously implemented TODO section |
| [add-asset.md](workflows/add-asset.md) | Load any asset (font, image, cursor) from disk |
| [specs-create.md](workflows/specs-create.md) | Create a spec document from research |
| [specs-fact-check.md](workflows/specs-fact-check.md) | Fact-check a spec document |
| [test-fs-fat32.md](workflows/test-fs-fat32.md) | FAT32 filesystem test workflow |
| [test-hardware.md](workflows/test-hardware.md) | Test on real hardware via USB boot |
| [release.md](workflows/release.md) | Tag a release and publish |

## Project Overview

See [AGENTS.md](../AGENTS.md) for the full project overview, repository layout, build system, and coding conventions.

## Key Facts

- **Kernel target**: x86-64 Long Mode, freestanding, no stdlib
- **Compiler**: `clang-19 --target=x86_64-elf` with `-nostdinc -nostdlib -ffreestanding`
- **Build**: `bash scripts/build.sh` (never raw `make`)
- **API surface**: Win32-compatible (native), POSIX via compat layer only
- **Canonical paths**: `C:\Impossible\System32\` style (Windows backslash)
