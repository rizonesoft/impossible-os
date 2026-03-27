# .impossible — Agent-Agnostic Project Guidance

> **Single source of truth for all AI agent rules and workflows.**
> This directory is tool-neutral. Rules are plain markdown. Every AI coding tool — Cursor, Claude Code, GitHub Copilot — reads from here via thin adapters.

## What's Here

| Path | Purpose |
|------|---------|
| `rules/` | Coding conventions and constraints (tool-neutral) |
| `workflows/` | Step-by-step procedures (tool-neutral) |
| `context.md` | Current project state — read this first each session |
| `index.md` | This file |

## Agent Adapters

| Agent | Instructions file | Skills | Rules |
|-------|-------------------|--------|-------|
| **Cursor** | `.cursor/rules/*.mdc` → references here | `.cursor/skills/` — thin adapters → `workflows/` | Thin MDC wrappers |
| **Claude Code** | `CLAUDE.md` → references here | `.claude/skills/` — thin adapters → `workflows/` | Inline critical rules |
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
| [implement-todo-section.md](workflows/implement-todo-section.md) | Implement a TODO section from start to commit |
| [create-todo.md](workflows/create-todo.md) | Create a new TODO file |
| [validate-todo-file.md](workflows/validate-todo-file.md) | Review a TODO for gaps and fix everything |
| [verify-todo-section.md](workflows/verify-todo-section.md) | Reconcile section status against code/build/runtime evidence |
| [improve-implementation-order.md](workflows/improve-implementation-order.md) | Audit and rewrite an Implementation Order table |
| [sync-ai-system.md](workflows/sync-ai-system.md) | Keep AI guidance aligned when conventions change |
| [todo-master-sync.md](workflows/todo-master-sync.md) | Fix implementation order, numbering, inconsistencies |
| [todo-table-format.md](workflows/todo-table-format.md) | Align TODO tables and format columns |
| [docs-convert-todo.md](workflows/docs-convert-todo.md) | Convert a completed TODO to documentation |
| [docs-validate.md](workflows/docs-validate.md) | Validate documentation structure and fix violations |
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
