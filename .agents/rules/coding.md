---
globs: ["**/*.c", "**/*.h", "**/*.asm"]
---

> **Migrated to Cursor rules. Cursor is the single source of truth.**
>
> This content has been split into:
> - **[`.cursor/rules/freestanding-kernel-code.mdc`](../../.cursor/rules/freestanding-kernel-code.mdc)** — `-nostdinc`, `kernel/types.h`, allocator discipline (`kmalloc` vs `pmm_alloc_contiguous`), crash debug tools (`llvm-addr2line-19`), and root-cause-fix discipline.
> - **[`.cursor/rules/bare-metal-assembly.mdc`](../../.cursor/rules/bare-metal-assembly.mdc)** — NASM x86-64, UEFI-era constraints, APIC model, no BIOS/VGA/PIC assumptions, minimal and explicit assembly.
>
> This file is a redirect stub only. Do not add new guidance here.
