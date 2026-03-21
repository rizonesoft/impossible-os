# AGENTS.md — Impossible OS

> A 64-bit operating system built from scratch for modern x86-64 hardware.
> No Linux kernel, no borrowed foundations — custom UEFI bootloader, kernel, graphical desktop, and everything in between.

> **Agent Rules:** Scoped rule files live in `.agents/rules/`:
> - [`safety.md`](.agents/rules/safety.md) — `always_on` — workspace boundaries, build script, `command_status` workaround
> - [`coding.md`](.agents/rules/coding.md) — `globs: *.c, *.h, *.asm` — freestanding C, memory gotchas, hardware constraints
> - [`intelligence.md`](.agents/rules/intelligence.md) — model decision — API surface (Win32), Srclight, Memory MCP

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
todo/                   Development roadmap (100+ TODO items across 50+ files)
specs/                  Specification documents (filesystem formats, etc.)
user/                   User-mode programs (hello.exe, shell.exe)
sdk/                    SDK for user-mode development
```

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
> **`command_status` gets stuck on builds.** The `command_status` tool frequently reports `RUNNING` with `No output` for commands that have already finished — especially builds with progress bars or high output. **Do NOT poll `command_status` after starting a build.** Instead, follow this pattern:

**After starting a build:**

1. Run the build via `run_command` with `WaitMsBeforeAsync: 500` (sends it to background immediately)
2. Wait an appropriate time — 30s for incremental, 90s for clean builds
3. Check the result with a **separate** `run_command`: `tail -1 build/build.log`
4. Expected output: `=== BUILD OK ===` or `=== BUILD FAILED ===`
5. If neither sentinel is present, wait 30s more and check again — **maximum 2 checks**

| Build Type | Wait Before Checking |
|------------|---------------------|
| Incremental | ~30 seconds |
| Clean | ~90 seconds |
| Build + QEMU | Don't check — QEMU stays open |

**For general long-running commands** (not builds), append a sentinel:

```bash
some-command 2>&1; echo "=== DONE ==="
```

Then verify with `tail -1` on the output. **Never re-run a command** just because `command_status` reported no output.

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
- **NEVER use angle-bracket includes** — `-nostdinc` strips the compiler's include path
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

### Freestanding Environment

- **No standard library.** Never include `<stdio.h>`, `<stdlib.h>`, `<string.h>`, or any user-space headers. The build uses `-nostdinc` so even freestanding headers like `<stdint.h>`, `<stddef.h>`, `<stdbool.h>` are **NOT available**. Use `#include "kernel/types.h"` for all integer types, `size_t`, and `NULL`.
- **No `<stdint.h>`.** Use `#include "kernel/types.h"` — it defines `uint8_t`–`uint64_t`, `int8_t`–`int64_t`, `size_t`, `ssize_t`, `uintptr_t`, and `NULL`.
- **No `malloc()`.** Use `kmalloc()` (≤ 4 KB) or `pmm_alloc_contiguous()` (everything else). See memory-allocation decision tree in coding rules.
- **No `printf()`.** Use `printk()` for kernel output, `klog()` for logging.
- **PMM returns `uintptr_t`, not `void *`.** Always cast: `(void *)(uintptr_t)pmm_alloc_contiguous(n)`.

### Memory Allocation — Most Common Bug Source

| Allocator | When to Use | Max Size |
|-----------|------------|----------|
| `kmalloc()` | Small kernel bookkeeping: VFS nodes, task structs, linked-list nodes, short strings | ≤ 4 KB |
| `pmm_alloc_contiguous()` | Everything else: buffers, images, font data, framebuffers, file read buffers | No limit |

> [!CAUTION]
> The kernel heap is only **2 MiB**. Using `kmalloc()` for anything larger than a few KB causes **silent heap exhaustion** that is extremely difficult to debug. When in doubt, use PMM.

### Hardware Constraints

- **APIC-only interrupts.** Route all hardware interrupts via LAPIC/IOAPIC. Do NOT write new 8259 PIC routing code. The PIC is masked at boot. *(Legacy PIC masking code in `pic.c` is kept for boot-time disable only.)*
- **DMA-only storage.** Use AHCI (DMA + NCQ) or VirtIO for disk I/O. Do NOT use legacy IDE/ATA PIO polling (port 0x1F0–0x1F7).
- **UEFI GOP framebuffer.** The framebuffer is a linear 32bpp buffer from UEFI GOP. Do NOT write VGA text mode (0xB8000) code.
- **RCU for read-heavy structures.** Prefer Read-Copy-Update over spinlocks for VFS mount list, process tree, and Registry cache.

### API Surface

- **Win32 is the native API.** User-space programs are PE32+ executables using Win32-style APIs (CreateFile, ReadFile, CreateProcess). POSIX APIs (open, read, fork) are secondary — for the Linux compat layer only.
- **Windows paths are canonical.** Use `C:\Impossible\System32\`, not `/usr/bin/`. Use `C:\Program Files\`, not `/usr/local/`.
- **Control Panel uses `.cpl` applets.** Settings are exposed via Windows-standard Control Panel Library applets (CPlApplet interface, `.cpl` extension).

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

All work is tracked in the TODO system under `todo/`. Start at [`todo/TODO-000-INDEX.md`](todo/TODO-000-INDEX.md) for the master index.

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

---

## Code Intelligence

- **Srclight is available.** The `.srclight/` index at the repo root provides deep code indexing via MCP. Use it for symbol search, call graph navigation, type hierarchy, semantic search, git blame, and hotspot analysis.
- **Start sessions with `codebase_map()`** to orient before navigating the codebase.
- **Prefer `hybrid_search()`** for most queries — it combines keyword + semantic search via RRF fusion.
- **Memory MCP is available** for persistent cross-session knowledge. Use it to:
  - **Read the graph at session start** (`read_graph`) to recall prior context — architectural decisions, known bugs, in-progress work, and component relationships.
  - **Create entities** for significant discoveries: hardware quirks, driver behavior, subsystem interfaces, and design decisions that future sessions will need. Use descriptive entity types (`bug`, `design_decision`, `driver`, `subsystem`, `gotcha`, `pattern`).
  - **Add observations** to existing entities as you learn more — don't duplicate entities, enrich them.
  - **Create relations** to link entities (e.g., `virtio_blk` → `uses` → `blkdev_interface`, `ahci_driver` → `depends_on` → `pci_subsystem`). Use active-voice relation types: `uses`, `depends_on`, `implements`, `conflicts_with`, `fixed_by`, `discovered_in`.
  - **Search nodes** (`search_nodes`) before creating new entities to avoid duplicates.
  - **Don't store transient data** — build errors, one-off debugging notes, or anything already in the TODO system. Memory is for hard-won knowledge that would be costly to rediscover.

---

## Known Gotchas

1. **PMM is the default allocator. `kmalloc` is the exception.** The kernel heap is only 2 MiB. `kmalloc` is strictly for small kernel bookkeeping: VFS nodes, task structs, Codex values, short strings, linked-list nodes — typically tens of bytes to a few KB each. **NEVER use `kmalloc` for image buffers, file read buffers, pixel data, font data, or any allocation that could plausibly exceed a few KB.** Use `pmm_alloc_contiguous()` for everything else — it allocates directly from physical memory (identity-mapped) with no size limit. Violating this rule causes silent heap exhaustion that is extremely difficult to debug. *(Learned from framebuffer bug `9722a74`, JPEG decode bug `f673e46`)*
2. **Framebuffer back buffer must use PMM, not kmalloc.** The back buffer for 1280×720×32bpp is 3.6 MiB. `kmalloc` fails silently → `back_buf = hw_addr` → zero double buffering → compositor flicker. Always use `pmm_alloc_contiguous()`. Boot log must show `[OK] Framebuffer back buffer:` and `[OK] VBE page flip enabled` to confirm double buffering is active. *(Fixed in commit `9722a74`)*
3. **Never `#include <stdint.h>` or any angle-bracket header.** The build uses `-nostdinc` which strips the compiler's include search path entirely. Use `#include "kernel/types.h"` instead — it defines `uint8_t` through `uint64_t`, `int8_t` through `int64_t`, `size_t`, `ssize_t`, `uintptr_t`, and `NULL`. Only project-local headers via double-quotes are safe. *(Learned from VMBus build failure `daafc39`)*
4. **SSE2 modules have special build rules.** Files using floating-point math (stb_truetype, stb_image, gfx_simd) are compiled with `-msse2` override. See the Makefile pattern rules.
5. **CalVer versioning.** Version is auto-generated from build date: `YY.M.D` (e.g., `26.3.21`). Build number auto-increments from `.build_number`.

---

## License

GPL-3.0 — see [LICENSE](LICENSE) for details.
Copyright © 2026 [Rizonesoft](https://github.com/rizonesoft)
