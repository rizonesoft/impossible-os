# Contributing to Impossible OS

Thank you for your interest in contributing to Impossible OS! This guide covers
everything you need to get started.

---

## 🚀 Getting Started

### Prerequisites

- **Ubuntu/Debian**, **Fedora**, or **Arch Linux** (WSL 2 on Windows works great)
- Git

### Development Environment Setup

```bash
git clone https://github.com/rizonesoft/impossible-os.git
cd impossible-os
bash scripts/setup.sh          # Installs Clang-19, NASM, QEMU, OVMF, mtools, etc.
bash scripts/build.sh run      # Build + boot in QEMU to verify everything works
```

`setup.sh` handles all dependencies automatically. If you're on an unsupported
distro, check the script for the package list and install manually.

### Enable Git Hooks

```bash
git config core.hooksPath .githooks
```

This enables the post-commit line count hook that keeps `COUNT.md` up to date.

---

## 📐 Code Style

Impossible OS is written in **C** and **x86-64 Assembly (NASM)**. Follow these
conventions to keep the codebase consistent.

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
- **Include guards:** Use `#pragma once`
- **Indentation:** 4 spaces (no tabs)

### Headers

- One header per source file (e.g., `pmm.c` → `include/mm/pmm.h`)
- All headers live in `include/` — never in `src/`
- Only freestanding headers allowed: `<stdint.h>`, `<stddef.h>`, `<stdbool.h>`, `<stdarg.h>`
- **Never** include `<stdio.h>`, `<stdlib.h>`, `<string.h>`, or any user-space headers

### Hardware Interactions

Comment all non-obvious port I/O, MMIO, and register manipulation:

```c
// Send EOI to Local APIC — must be done AFTER reading the ISR,
// otherwise the interrupt may fire again before we handle it
lapic_write(LAPIC_EOI, 0);
```

### Memory Allocation

This is the most common source of bugs. **Read carefully:**

| Allocation | When to Use | Max Size |
|------------|------------|----------|
| `kmalloc()` | Small kernel bookkeeping: VFS nodes, task structs, linked-list nodes | ≤ 4 KB |
| `pmm_alloc_contiguous()` | Everything else: buffers, images, font data, framebuffers | No limit |

> [!CAUTION]
> The kernel heap is only **2 MiB**. Using `kmalloc()` for anything larger than
> a few KB causes silent heap exhaustion. When in doubt, use PMM.

---

## 💬 Commit Messages

We use **conventional commits** with scope prefixes:

```
scope: short description
```

### Common Scopes

| Scope | Usage |
|-------|-------|
| `kernel` | Kernel core (memory, scheduler, syscalls) |
| `boot` | UEFI bootloader |
| `desktop` | Window manager, compositor, shell |
| `drivers` | Hardware drivers (AHCI, NIC, keyboard, etc.) |
| `gfx` | Graphics library (rendering, fonts, effects) |
| `fs` | Filesystem (VFS, FAT32, IXFS) |
| `net` | Networking (Ethernet, TCP/IP, DHCP) |
| `build` | Build system, Makefile, scripts |
| `docs` | Documentation, README, TODO updates |
| `agent` | Agent skills, workflows, rules |

### Examples

```
kernel: add seqlock synchronization primitive
drivers: implement RTL8139 receive interrupt handler
desktop: dirty rectangle compositor for drag optimization
build: migrate from GCC to Clang-19 + LLD
docs: professional README with feature table
```

---

## 🔄 Pull Request Process

1. **Fork** the repository
2. **Branch** from `main`:
   ```bash
   git checkout -b feature/your-feature-name
   ```
3. **Implement** your changes following the code style above
4. **Test** before submitting:
   ```bash
   bash scripts/build.sh clean run    # Must boot successfully in QEMU
   tail -1 build/build.log            # Must show "=== BUILD OK ==="
   ```
5. **Commit** with a conventional commit message
6. **Push** and open a Pull Request against `main`

### PR Checklist

- [ ] Code compiles without warnings (`-Wall -Wextra -Werror`)
- [ ] Tested in QEMU — boots and runs correctly
- [ ] No new `kmalloc()` calls for buffers > 4 KB
- [ ] Serial output checked for new warnings/errors
- [ ] Commit message follows `"scope: description"` format

---

## 🗺️ Finding Work Items

All development is tracked in the TODO system:

- **Start here:** [`todo/TODO-000-INDEX.md`](todo/TODO-000-INDEX.md) — master index of all work
- **Each TODO file** contains detailed sections with implementation prompts
- **Status markers:** `[ ]` uncompleted, `[/]` in progress, `[x]` completed
- **Look for** sections marked with priority: 🔴 P0, 🟠 P1, 🟡 P2, 🟢 P3

### Good First Issues

If you're new to OS development, look for:
- Documentation improvements
- Adding missing kernel log messages
- Small driver enhancements
- Test coverage gaps

---

## ⚠️ Important Rules

1. **Always use `bash scripts/build.sh`** — never raw `make` commands
2. **APIC-only interrupts** — do NOT write 8259 PIC routing code
3. **DMA-only storage** — do NOT use legacy IDE/ATA PIO polling
4. **UEFI GOP framebuffer** — do NOT write VGA text mode (0xB8000) code
5. **No standard library** — this is a freestanding kernel, not user-space
6. **Use `printk()`** for output, not `printf()`

---

## 📄 License

By contributing, you agree that your contributions will be licensed under the
[GPL-3.0 License](LICENSE).
