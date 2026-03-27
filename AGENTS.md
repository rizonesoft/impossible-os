# AGENTS.md — Impossible OS

> A 64-bit operating system built from scratch for modern x86-64 hardware.
> No Linux kernel, no borrowed foundations — custom UEFI bootloader, kernel, graphical desktop, and everything in between.

> **Single source of truth for all agent rules and workflows:** [`.impossible/`](.impossible/index.md)
> Rules content lives in `.impossible/rules/`. Workflows live in `.impossible/workflows/`.
> Each tool uses a thin adapter that points here:
>
> | Agent | Instructions | Skills / Commands | Rules |
> |-------|-------------|------------------|-------|
> | **Cursor** | This file (`AGENTS.md`) | `.cursor/skills/` — native `/` commands | `.cursor/rules/*.mdc` (adapters) |
> | **Claude Code** | [`CLAUDE.md`](CLAUDE.md) | `.claude/commands/` — Claude-specific commands (pending) | Inline in `CLAUDE.md` |
> | **Antigravity** | This file (`AGENTS.md`) | `.agents/workflows/` — thin redirects to `.cursor/skills/` | Inline in this file |
> | **GitHub Copilot** | [`.github/copilot-instructions.md`](.github/copilot-instructions.md) | None | Inline in copilot file |
>
> **Cursor Project Skills** (`.cursor/skills/` — `/command-name` in Cursor):
> - [`create-todo`](.cursor/skills/create-todo/SKILL.md) — create lean TODO files, choose the correct domain, and update indexes
> - [`implement-todo-section`](.cursor/skills/implement-todo-section/SKILL.md) — execute one scoped TODO section with build and test evidence
> - [`validate-todo-file`](.cursor/skills/validate-todo-file/SKILL.md) — validate TODO structure, execution order, XREFs, and gap-free continuity
> - [`verify-todo-section`](.cursor/skills/verify-todo-section/SKILL.md) — reconcile section status against code, build, and runtime evidence
> - [`improve-implementation-order`](.cursor/skills/improve-implementation-order/SKILL.md) — audit and rewrite the Implementation Order table
> - [`sync-ai-system`](.cursor/skills/sync-ai-system/SKILL.md) — keep all agent adapters aligned when conventions change
>
> **Antigravity Workflows** (`.agents/workflows/` — thin redirects to `.cursor/skills/`):
> `build`, `create-todo`, `implement-todo-section`, `validate-todo-file`, `verify-todo-section`, `sync-ai-system`, `improve-implementation-order`, `add-asset`, `docs-convert-todo`, `docs-validate`, `todo-done-check`, `todo-master-sync`, `todo-table-format`, `specs-create`, `specs-fact-check`, `test-fs-fat32`, `test-hardware`, `release`

---

## Philosophy

> **Nothing is impossible.**

Impossible OS is a **production-level operating system** — not a hobby project, not a prototype. Every decision, every line of code, every fix is held to this standard:

1. **Proper solutions over quick fixes.** Don't patch symptoms — find root causes, implement correct solutions, and optimize them. If a fix requires restructuring, restructure.
2. **No workarounds, no hacks, no temporary fixes.** Every change must be long-term thinking. Code that "works for now" is technical debt that compounds. Do it right or don't do it.
3. **Outperform Windows 11 and Linux.** This is non-negotiable. Every subsystem — boot time, I/O throughput, memory management, UI responsiveness — must meet or exceed what the industry leaders deliver.
4. **Expansive mindset.** Think bigger. If a feature exists in Windows or Linux, we implement it better. If it doesn't exist anywhere, we invent it. Constraints are challenges, not stop signs.
5. **Nothing is impossible.** The name is the mission. A from-scratch OS with a custom bootloader, kernel, compositor, and Win32-compatible API — built to compete with decades-old platforms. We don't accept "can't be done."

---

## Project Overview

Impossible OS is a freestanding, bare-metal OS targeting x86-64 Long Mode. The native API surface is **Win32-compatible** (PE32+ executables, `CreateFile`/`ReadFile`/`CreateProcess`). POSIX APIs exist only in a secondary Linux compatibility layer. Canonical paths use Windows style: `C:\Impossible\System32\`.

The system boots via a **custom UEFI bootloader** (`BOOTX64.EFI`) that initializes GOP, loads the kernel ELF, and hands off a `boot_info` struct. The kernel handles GDT, IDT, APIC, PMM, VMM, VFS, and drivers, then launches a compositing desktop shell.

| Component | Status |
|-----------|--------|
| Custom UEFI bootloader | ✅ |
| 64-bit kernel (PMM, VMM, scheduler, VFS) | ✅ |
| AHCI + VirtIO storage (DMA) | ✅ |
| Compositing desktop + TrueType fonts | ✅ |
| Networking (RTL8139, ARP, IPv4, ICMP, UDP, DHCP) | ✅ |
| Win32-compatible API | 🔄 In progress |

---

## Repository Layout

```
src/
├── boot/uefi/          Custom UEFI bootloader (PE/COFF, GOP, ELF loader)
├── kernel/             Kernel core
│   ├── drivers/            PCI, AHCI, VirtIO, framebuffer, keyboard, mouse, NIC
│   ├── fs/                 VFS, IXFS, FAT32, GPT/MBR partitioning
│   ├── gfx/                2D graphics: blending, gradients, blur, text rendering
│   ├── ipc/                Pipes, signals, shared memory
│   ├── main/               Kernel entry, initialization, blkdev adapters
│   ├── mm/                 PMM, VMM, heap, swap, mmap
│   ├── net/                Ethernet, ARP, IPv4, ICMP, UDP, DHCP
│   ├── sched/              Scheduler, threads, syscalls, sync primitives
│   └── smp/                Symmetric multi-processing
├── desktop/            Window manager, compositor, terminal, desktop shell
├── shell/              Command-line shell
├── installer/          OS installer
└── libc/               Minimal kernel libc

include/                All header files (mirrors src/ hierarchy)
resources/              Fonts, icons, wallpapers, cursors
scripts/                Build, test, deploy scripts
tools/                  Host-side build tools (jpg2raw, irespack, asset converters)
todo/                   Development roadmap (14 domains, 86 TODO files)
docs/                   Documentation (architecture, specs, guides)
  ├── architecture/     System architecture and implementation docs
  ├── specs/            External reference specs (AHCI, FAT32, UEFI, ...)
  └── guides/           How-to guides
user/                   User-mode programs (hello.exe, cmd.exe)
sdk/                    SDK for user-mode development

.impossible/            Agent-agnostic rules and workflows (single source of truth)
  ├── rules/            Portable coding conventions (readable by all tools)
  └── workflows/        Portable procedures (readable by all tools)
.claude/                Slash commands (Cursor + Claude Code) and skills
  └── commands/         Canonical slash commands + supporting subdirs
.cursor/                Cursor rule adapters only (.mdc files)
.agents/                Antigravity workflow redirects (point to .claude/commands/)
```

### Win32 Naming Conventions

Executables and system components follow **Windows 10/11 naming** for consistency with the Win32-compatible API surface:

| Impossible OS | Windows equivalent | Role | Binary Format |
|---------------|-------------------|------|:-------------:|
| `BOOTX64.EFI` | `BOOTX64.EFI` | UEFI bootloader | PE/COFF |
| `kernel.exe` | `ntoskrnl.exe` | Kernel (Ring 0) | ELF |
| `cmd.exe` | `cmd.exe` | Command-line interpreter | EIF → PE32+ |
| `diskpart.exe` | `diskpart.exe` | CLI disk partitioning tool | EIF → PE32+ |
| *(future)* `explorer.exe` | `explorer.exe` | Desktop shell (taskbar, Start menu, file manager) | EIF → PE32+ |
| `hello.exe` | — | Test program | EIF → PE32+ |

> [!NOTE]
> **Binary formats:** The kernel is the **loader**, not the **loaded** — it stays ELF
> (loaded by the UEFI bootloader). All user-mode apps use **EIF** (Executable Impossible
> Format, the native format) or **PE32+** (for Windows compatibility). EIF → PE32+ means:
> start with EIF, add PE32+ support when the Win32 import resolver is complete. Both use
> the same Win32 API underneath. See `TODO-042-Binary-System.md` for details.

> [!NOTE]
> The desktop shell currently runs kernel-mode in `src/desktop/`. When it becomes a user-mode process, it should be named **`explorer.exe`** following the Windows convention.

---

## Build System

### Toolchain

| Tool | Binary |
|------|--------|
| C compiler | `clang-19 --target=x86_64-elf` |
| Assembler | `nasm` (NASM syntax, `.asm` extension) |
| Linker | `ld.lld-19` |
| Object tools | `llvm-objcopy-19`, `llvm-ar-19`, `llvm-nm-19` |
| Host compiler | `gcc` (for build tools only) |

### Compiler Flags

```
-Wall -Wextra -Werror -ffreestanding -nostdlib -nostdinc
-fno-stack-protector -fno-pie -mno-red-zone
-mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel -std=gnu11 -O2 -g
```

> [!CAUTION]
> `-nostdinc` strips **all** include paths. `<stdint.h>`, `<stddef.h>`, `<stdbool.h>` are **not available**. Use `#include "kernel/types.h"` for all integer types, `size_t`, and `NULL`. Only project-local headers via double-quotes are safe.

### Build Commands

**Always use `bash scripts/build.sh`** — never raw `make` commands.

| Command | Description |
|---------|-------------|
| `bash scripts/build.sh` | Incremental build |
| `bash scripts/build.sh clean` | Full clean build |
| `bash scripts/build.sh run` | Build + launch QEMU |
| `bash scripts/build.sh clean run` | Clean build + launch QEMU |

Verify success: `tail -1 build/build.log` → must show `=== BUILD OK ===`

> [!WARNING]
> **`command_status` gets stuck on builds.** Do NOT poll `command_status` — it falsely reports `RUNNING`. Wait ~30s (incremental) or ~90s (clean), then check with `tail -1 build/build.log`. See [`safety-build.mdc`](.cursor/rules/safety-build.mdc) for the active rule version.

### Output

The build produces a bootable GPT disk image: `build/system-disk.img`

Disk layout: **EFI partition** (64 MiB, FAT32) + **Logs partition** (16 MiB, FAT32) + **IXFS partition** (remaining, custom filesystem)

### Dependencies

Run `bash scripts/setup.sh` to install all dependencies automatically (Ubuntu/Debian, Fedora, or Arch Linux; WSL 2 recommended on Windows).

---

## Architecture

### Boot Chain

```
UEFI Firmware → BOOTX64.EFI → kernel.exe (ELF) → Desktop Shell
                    │                │
              GOP + MemMap      boot_info struct
              RSDP pointer      framebuffer, memory map
```

The bootloader is a **PE32+/COFF EFI application** (`src/boot/uefi/bootx64.c`). It runs in Long Mode from the start — no 16→32→64 transition.

**Boot Sequence:**

1. **GOP init** — locate Graphics Output Protocol, set 32bpp mode
2. **Load kernel ELF** — read `\boot\kernel.exe` from FAT32 ESP
3. **Find ACPI RSDP** — scan UEFI ConfigurationTable
4. **Get memory map** — call GetMemoryMap(), convert to boot_info format
5. **ExitBootServices()** — no UEFI calls after this point
6. **Setup page tables** — identity-map first 4 GiB with 2 MiB pages
7. **Jump to kernel** — `kernel_main(0x55454649, &boot_info)`

**Key Constants:**

| Constant | Value |
|----------|-------|
| boot_info address | `0x10000` (64 KiB) |
| Page tables (PML4) | `0x70000` |
| UEFI magic | `0x55454649` ("UEFI") |
| Kernel path | `\boot\kernel.exe` on FAT32 ESP |

### Memory Model

```
PMM (physical pages) → VMM (page tables) → kmalloc (2 MiB heap)
                                           ↓
                           Small structs only (≤ 4 KB)
                           Everything else → pmm_alloc_contiguous()
```

> [!IMPORTANT]
> Kernel BSS must stay below `0x800000` (user-mode ELF base in `user/user.ld`). The build script checks this automatically. See [`freestanding-kernel-code.mdc`](.cursor/rules/freestanding-kernel-code.mdc) for the active freestanding rule summary.

### Storage Stack

```
AHCI / VirtIO → blkdev → GPT/MBR → FAT32 / IXFS → VFS (C:\, D:\)
```

---

## OS Filesystem Layout

Impossible OS uses **Windows-style drive letters** and **backslash paths**. The root filesystem is `C:\`.

```
C:\
├── Impossible\                  ← System root (like C:\Windows)
│   ├── System\                  ← Core system files, drivers
│   │   ├── Drivers\             ← Device drivers
│   │   ├── Config\Registry\     ← Registry hive files
│   │   ├── Cursors\             ← Cursor theme files
│   │   └── Logs\                ← System logs
│   ├── System32\                ← System executables
│   ├── Fonts\                   ← System fonts (.ttf)
│   ├── Icons\                   ← System icons (IRES)
│   ├── Media\                   ← System sounds
│   ├── Web\Wallpaper\           ← Default wallpapers
│   ├── Themes\                  ← UI theme definitions
│   └── Temp\                    ← Temporary files
├── Program Files\               ← Installed applications
├── Users\{Username}\            ← User home directories
│   ├── Desktop\ Documents\ Downloads\ Pictures\
│   └── AppData\{AppName}\       ← Per-user app settings
└── Recycle\                     ← Recycle Bin
```

**Path Conventions:**

| Rule | Example |
|------|---------|
| Use backslashes | `C:\Impossible\Fonts\Inter.ttf` |
| Drive letter + colon | `C:\`, `D:\` |
| Case-insensitive | `C:\impossible\fonts` == `C:\Impossible\Fonts` |
| In C code: double backslash | `vfs_open("C:\\Impossible\\Fonts\\Inter.ttf", ...)` |

**Build-time → Runtime Asset Mapping:**

| Build-time path | OS runtime path |
|----------------|-----------------|
| `resources/fonts/*.ttf` | `C:\Impossible\Fonts\*.ttf` |
| `resources/cursors/*` | `C:\Impossible\System\Cursors\*` |
| `resources/backgrounds/background.jpg` | `C:\Impossible\Web\Wallpaper\default.jpg` |
| `resources/icons/color/` | `C:\Impossible\Icons\icons.ires` |

---

## Source & Header Organization

Headers mirror the source tree under `include/`. The Makefile uses `-Iinclude` as the include root.

### Rules for Adding New Files

| Component | Source path | Header path | Include as |
|-----------|------------|-------------|------------|
| Kernel driver | `src/kernel/drivers/<name>.c` | `include/kernel/drivers/<name>.h` | `"kernel/drivers/<name>.h"` |
| Filesystem | `src/kernel/fs/<name>.c` | `include/kernel/fs/<name>.h` | `"kernel/fs/<name>.h"` |
| Desktop component | `src/desktop/<name>.c` | `include/desktop/<name>.h` | `"desktop/<name>.h"` |
| Kernel subsystem | `src/kernel/<sub>/<name>.c` | `include/kernel/<sub>/<name>.h` | `"kernel/<sub>/<name>.h"` |

### Include Style

- **Always use subdirectory paths**: `#include "kernel/drivers/serial.h"` (not `#include "serial.h"`)
- **NEVER use angle-bracket includes** — `-nostdinc` strips the compiler's include path. See [`freestanding-kernel-code.mdc`](.cursor/rules/freestanding-kernel-code.mdc) for the active freestanding rule summary.
- **Use `#pragma once`** for include guards
- **No circular includes** — use forward declarations when needed

---

## Coding Conventions

### Naming

| Element | Convention | Example |
|---------|-----------|---------|
| Functions | `snake_case` | `pmm_alloc_frame()` |
| Variables | `snake_case` | `frame_count` |
| Macros / Constants | `UPPER_CASE` | `PAGE_SIZE`, `MAX_THREADS` |
| Types / Structs | `snake_case_t` | `task_t`, `vfs_node_t` |

### Formatting

- **Line length:** ≤ 120 characters
- **Function length:** < 50 lines (split into helpers if longer)
- **Include guards:** `#pragma once`
- **Indentation:** 4 spaces (no tabs)

### Headers

- One header per source file (e.g., `pmm.c` → `include/kernel/mm/pmm.h`)
- All headers live in `include/` — never in `src/`
- Comment all non-obvious hardware interactions (port I/O, MMIO, register manipulation)

### Commit Messages

Conventional commits with scope prefixes:

```
scope: short description
```

Common scopes: `kernel`, `boot`, `desktop`, `drivers`, `gfx`, `fs`, `net`, `build`, `docs`, `agent`

---

## Critical Rules

> Canonical rule content lives in [`.impossible/rules/`](.impossible/rules/) — portable markdown readable by every tool.
> Cursor adapters in `.cursor/rules/*.mdc` point to this content.
>
> | Rule | Canonical | Cursor adapter |
> |------|-----------|----------------|
> | Safety & build | [`.impossible/rules/build.md`](.impossible/rules/build.md) | [`safety-build.mdc`](.cursor/rules/safety-build.mdc) |
> | Freestanding kernel | [`.impossible/rules/freestanding.md`](.impossible/rules/freestanding.md) | [`freestanding-kernel-code.mdc`](.cursor/rules/freestanding-kernel-code.mdc) |
> | Bare-metal assembly | [`.impossible/rules/assembly.md`](.impossible/rules/assembly.md) | [`bare-metal-assembly.mdc`](.cursor/rules/bare-metal-assembly.mdc) |
> | API surface & design | [`.impossible/rules/api-surface.md`](.impossible/rules/api-surface.md) | [`api-surface-direction.mdc`](.cursor/rules/api-surface-direction.mdc) |
> | Documentation sync | [`.impossible/rules/doc-sync.md`](.impossible/rules/doc-sync.md) | [`doc-sync-discipline.mdc`](.cursor/rules/doc-sync-discipline.mdc) |
> | MCP usage | [`.impossible/rules/mcp-usage.md`](.impossible/rules/mcp-usage.md) | [`mcp-usage-discipline.mdc`](.cursor/rules/mcp-usage-discipline.mdc) |
> | TODO style | [`.impossible/rules/todo-style.md`](.impossible/rules/todo-style.md) | [`todo-markdown-style.mdc`](.cursor/rules/todo-markdown-style.mdc) |

---

## Testing

Always test changes via QEMU before committing:

```bash
bash scripts/build.sh run
```

| Platform | Method |
|----------|--------|
| **QEMU** | `bash scripts/build.sh run` — default, fastest iteration |
| **VirtualBox** | Convert to VDI, configure 64-bit EFI VM |
| **Hyper-V** | Gen 2 VM, Secure Boot disabled |
| **Real hardware** | Write `build/system-disk.img` to USB |

Serial output (`-serial stdio`) is the primary debug channel. Check `build/serial.log` for captured output.

---

## Development Roadmap

All work is tracked in the TODO system under `todo/`. Start at [`todo/TODO-00-INDEX.md`](todo/TODO-00-INDEX.md) for the master index.

Status markers: `[ ]` uncompleted, `[/]` in progress, `[x]` completed
Priority levels: 🔴 P0, 🟠 P1, 🟡 P2, 🟢 P3

### TODO Folder Structure

| Prefix | Layer | Examples |
|--------|-------|----------|
| `000-` | Infrastructure | Build system, tooling, CI |
| `010-` | Kernel Foundations | Bootloader, threading, filesystem, registry |
| `060-` | Hardware & Drivers | Keyboard, mouse, power management |
| `110-` | GFX & UI Framework | UI controls, theme, animation |
| `160-` | Desktop Shell | Taskbar, start menu, boot splash |
| `230-` | Core Services | Clipboard, search, security |
| `310-` | Core Apps | Terminal, file manager, notepad |
| `380-` | Multimedia | Audio, paint |
| `400-` | Networking | Browser, FTP, SSH, email |
| `460-` | Polish & Extras | DPI, screensaver, widgets |
| `510-` | Long-Term Stretch | Win32, Linux compat, SDK |

### TODO File Anatomy

```markdown
## N. Section Title              ← numbered section
### N.M Subsection *(status)* ✅  ← status emoji: ⏳ 🔄 ✅

**Prompt:** Instructions...       ← what to do + verification steps

> [!IMPORTANT]                    ← cross-references, prerequisites
> → XREF: `TODO-NNN §M.M`

- [ ] Task item                   ← unchecked
- [/] Task in progress            ← in progress
- [x] Completed task              ← done
- [ ] Commit: `"scope: msg"`      ← final commit instruction
```

### TODO Rules

- **Don't create duplicate TODOs** — always check the index first
- **Don't renumber existing TODOs** — gaps are intentional (allows inserting)
- **Don't remove completed items** — keep them checked off for history
- **Convert completed TODOs to docs** — when all items are `[x]`, use the `/docs-convert-todo` workflow to generate documentation in `docs/` and leave a stub

---

## Known Gotchas & Code Intelligence

> Full freestanding gotchas and allocator discipline are summarized in [`freestanding-kernel-code.mdc`](.cursor/rules/freestanding-kernel-code.mdc). Legacy migration source material remains in [`coding.md`](.agents/rules/coding.md).
> Srclight usage and MCP boundaries are summarized in [`mcp-usage-discipline.mdc`](.cursor/rules/mcp-usage-discipline.mdc). Legacy migration source material remains in [`intelligence.md`](.agents/rules/intelligence.md).

---

## License

GPL-3.0 — see [LICENSE](LICENSE) for details.
Copyright © 2026 [Rizonesoft](https://github.com/rizonesoft)
