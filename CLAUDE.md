# CLAUDE.md — Impossible OS

> Claude Code project instructions. The canonical rules live in `.impossible/rules/` — read them for full detail. This file provides the critical constraints inline so Claude Code has them immediately available.

## Quick Orientation

Read `.impossible/context.md` at the start of each session for current project state.
Read `.impossible/index.md` for the complete rules and workflows index.
Full project overview: `AGENTS.md`.

## Critical Rules (Inline)

### Build — Never use raw `make`
```bash
bash scripts/build.sh           # incremental
bash scripts/build.sh clean     # full clean
bash scripts/build.sh run       # build + QEMU
```
Check `tail -1 build/build.log` for result — must show `=== BUILD OK ===`.

### Freestanding Kernel — No stdlib
- No `<stdint.h>`, `<string.h>`, etc. — use `#include "kernel/types.h"`
- No `malloc()`/`printf()` — use `kmalloc()`, `pmm_alloc_contiguous()`, `printk()`
- `kmalloc()` for ≤ 4 KB only; `pmm_alloc_contiguous()` for everything larger

### Assembly — NASM x86-64 only
- UEFI-era, Long Mode, APIC environment — no BIOS/VGA/PIC assumptions

### API Surface — Win32 native
- Win32 is the native API; POSIX via Linux compat layer only
- Canonical paths use Windows style: `C:\Impossible\System32\`

### Safety Gates
Stop and ask before: security-sensitive changes, destructive operations, ABI changes, dependency additions, large refactors.

### Doc Sync
When you change code or conventions, update `.impossible/rules/`, `AGENTS.md`, and affected TODO files in the same task.

## Toolchain
- Compiler: `clang-19 --target=x86_64-elf`
- Assembler: `nasm`
- Linker: `ld.lld-19`
- Crash debug: `llvm-addr2line-19 -e build/kernel.exe -f <RIP>`

## Slash Commands

Claude Code-specific commands will live in `.claude/commands/` — not yet populated.
Run Cursor skills via `@`-mentioning the SKILL.md file:

```
@.cursor/skills/implement-todo-section/SKILL.md <target TODO and section>
```

| Cursor Skill | Description |
|---|---|
| `implement-todo-section` | Implement one TODO section end-to-end |
| `create-todo` | Create a new TODO file |
| `validate-todo-file` | Validate a TODO for gaps |
| `verify-todo-section` | Verify a TODO section against code evidence |
| `improve-implementation-order` | Audit and fix an Implementation Order table |
| `sync-ai-system` | Sync AI guidance across all agent adapters |

## Repository Layout

```
src/
├── boot/uefi/      Custom UEFI bootloader
├── kernel/         Kernel core (PMM, VMM, scheduler, VFS, drivers)
├── desktop/        Compositing desktop shell
├── shell/          Command-line shell
└── libc/           Minimal kernel libc
include/            All headers (mirrors src/)
resources/          Fonts, icons, wallpapers
todo/               Development roadmap (14 domains, 86 TODO files)
.impossible/        Agent-agnostic rules and workflows (single source of truth)
.cursor/            Cursor-specific rules and skills
.claude/            Claude Code commands and skills
.agents/            Antigravity workflows
```
