# Copilot Instructions -- Impossible OS

> Impossible OS is a production-grade 64-bit OS for x86-64. Custom UEFI bootloader, kernel, compositing desktop, and Win32-compatible API surface.

## Critical Rules

### Build
```bash
bash scripts/build.sh           # incremental build
bash scripts/build.sh clean     # full clean build
bash scripts/build.sh run       # build + QEMU
```
- **Never use raw `make`**
- Verify: `tail -1 build/build.log` must show `=== BUILD OK ===`

### Kernel C Code
- `-nostdinc` is active -- no `<stdint.h>`, `<string.h>`, or any angle-bracket headers
- Use `#include "kernel/types.h"` for all integer types and `size_t`
- No `malloc()` → use `kmalloc()` (≤ 4 KB) or `pmm_alloc_contiguous()` (larger)
- No `printf()` → use `printk()` or `klog()`

### Assembly
- NASM syntax, x86-64 Long Mode, UEFI-era / APIC environment
- No BIOS interrupts, no VGA text mode, no PIC assumptions

### API Surface
- Win32 is the native API -- POSIX only via Linux compat layer
- Use Windows-style paths: `C:\Impossible\System32\` with backslashes

### Safety
- Never run real-device disk commands (`dd`, `mkfs`, `/dev/sd*`) without explicit approval
- Stop and ask before security changes, destructive operations, ABI changes, large refactors

### Include Style
```c
// CORRECT
#include "kernel/types.h"
#include "kernel/mm/pmm.h"

// WRONG -- stripped by -nostdinc
#include <stdint.h>
```

## Naming Conventions

| Element | Convention | Example |
|---------|-----------|---------|
| Functions | `snake_case` | `pmm_alloc_frame()` |
| Variables | `snake_case` | `frame_count` |
| Macros/Constants | `UPPER_CASE` | `PAGE_SIZE` |
| Types/Structs | `snake_case_t` | `task_t` |

## Toolchain

- Compiler: `clang-19 --target=x86_64-elf -ffreestanding -nostdinc -nostdlib`
- Assembler: `nasm`
- Crash debug: `llvm-addr2line-19 -e build/kernel.exe -f <RIP>`

## Repository Structure

Key directories:
- `src/kernel/` -- kernel core
- `src/boot/uefi/` -- UEFI bootloader
- `include/` -- all headers
- `todo/` -- development roadmap
- `.cursor/` -- Cursor AI rules and skills
- `.claude/` -- Claude Code AI skills
