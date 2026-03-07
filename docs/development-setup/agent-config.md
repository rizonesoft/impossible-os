# Agent Configuration

The development environment includes agent rules, skills, and workflows that
guide the AI coding assistant during OS development.

## Rules (`rules.md`)

Loaded automatically by the IDE as `MEMORY[rules.md]`. Key constraints:

| Category | Rules |
|----------|-------|
| **Scope** | Work only inside `~/impossible-os/` |
| **Languages** | C, C++, x86-64 Assembly (NASM syntax) |
| **Compilation** | Always use `-Wall -Wextra -Werror -ffreestanding -nostdlib -nostdinc` |
| **Compiler** | Use `x86_64-elf-gcc` cross-compiler |
| **Architecture** | x86-64 Long Mode |
| **Output** | ISO file at `build/os-build.iso` |
| **Testing** | Always test via `make run` (QEMU) before committing |
| **Boundaries** | No access to `/mnt/c/`, no real disk operations, no host modifications |
| **Code style** | `snake_case` functions/variables, `UPPER_CASE` macros, `#pragma once` |
| **Functions** | Keep under 50 lines where possible |
| **Commits** | Conventional format: `"scope: short description"` |
| **Git** | Never commit build artifacts |

## Skills (`.agent/skills/`)

| Skill | Path | Purpose |
|-------|------|---------|
| GitHub | `.agent/skills/github/SKILL.md` | Git/GitHub commands, handling silent completions |
| Impossible OS | `.agent/skills/impossible-os/SKILL.md` | Project-specific conventions |
| Source Code Organization | `.agent/skills/source-code-organization/SKILL.md` | Header/source file layout rules |

### Source Code Organization

Headers mirror the source tree under `include/`:

```
include/
├── kernel/                # Core kernel headers
│   ├── drivers/           # Hardware drivers (serial, keyboard, mouse, etc.)
│   ├── mm/                # Memory management (pmm, vmm, heap)
│   ├── fs/                # Filesystems (vfs, initrd, fat32, ixfs)
│   ├── sched/             # Scheduler (task, syscall)
│   └── net/               # Networking (net)
└── desktop/               # Desktop environment (wm, desktop, font, controls)
```

**Rule:** New files follow the pattern:
- Source: `src/kernel/drivers/foo.c`
- Header: `include/kernel/drivers/foo.h`
- Include as: `#include "kernel/drivers/foo.h"`

## Workflows (`.agent/workflows/`)

| Workflow | Slash Command | Description |
|----------|--------------|-------------|
| Build | `/build` | Build the OS from source and test in QEMU |
| Release | `/release` | Tag a release, update changelog, build and publish the ISO |

## Versioning System

Automatic versioning is integrated into the build system:

| Component | File | Description |
|-----------|------|-------------|
| SemVer | `VERSION` | `MAJOR.MINOR.PATCH` (e.g., `0.1.0`), bumped manually |
| Build counter | `.build_number` | Auto-incremented on every `make all` (gitignored) |
| Git hash | Makefile | 8-char short hash of HEAD |
| CFLAGS injection | Makefile | `-DVERSION_MAJOR=0 -DVERSION_MINOR=1 ...` |
| Runtime API | `version.h` / `version.c` | `version_string()`, `version_print()` |

**Format:** `MAJOR.MINOR.PATCH.BUILD` (e.g., `0.1.0.42`)

Each `make all` produces a unique build. The boot banner shows:
```
Impossible OS v0.1.0.42 (build 42, git a1b2c3d4)
```
