# Freestanding Kernel Code Rules

> **Applies to:** `src/kernel/`, `include/kernel/`
> **Canonical location:** `.impossible/rules/freestanding.md`

## No Standard Library

- **Do not use angle-bracket headers** in kernel code. `-nostdinc` removes `<stdint.h>`, `<stddef.h>`, `<string.h>`, and all stdlib headers. They do not exist.
- Use `#include "kernel/types.h"` for all integer types (`uint8_t`, `uint64_t`, etc.), `size_t`, and `NULL`.
- Only project-local headers via double-quotes are safe: `#include "kernel/mm/pmm.h"`.

## Allocation Rules

- **Do not use `malloc()` or `printf()`** — they don't exist. Use kernel equivalents:
  - `kmalloc()` / `kfree()` — small allocations **≤ 4 KB only**
  - `pmm_alloc_contiguous(n)` — large buffers, DMA, fonts, images, framebuffers
  - `printk()` / `klog()` — kernel logging
- Cast PMM returns explicitly: `(void *)(uintptr_t)pmm_alloc_contiguous(n)`
- Do not let large static arrays grow kernel BSS into user-mode load space (BSS must stay below `0x800000`).

## Hardware Paths

- New low-level hardware work follows the current platform model: **APIC/IOAPIC** interrupts, **DMA-first** storage paths, **UEFI GOP** framebuffer.
- Do not introduce new PIC-routing, IDE PIO, or VGA text-mode paths.
- SSE2 is disabled kernel-wide (`-mno-sse2`). Modules needing SSE2 require special build handling.

## Crash Debugging

- For BSOD, panic, PAGE_FAULT, or RIP-based crashes, use symbolication tools **before** speculating:
  ```bash
  llvm-addr2line-19 -e build/kernel.exe -f <RIP>
  llvm-objdump-19 -d build/kernel.exe | grep -A 5 <RIP>
  ```
- Fix root causes and allocator misuse directly. Do not normalize temporary workarounds.

## Example

```c
#include "kernel/types.h"   // uint64_t, size_t, NULL
#include "kernel/mm/pmm.h"  // pmm_alloc_contiguous

// Small struct — kmalloc is fine
my_struct_t *s = kmalloc(sizeof(my_struct_t));

// Large buffer — use PMM
uint64_t phys = pmm_alloc_contiguous(pages);
uint8_t *buf = (uint8_t *)(uintptr_t)phys;
```
