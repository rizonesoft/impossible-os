---
trigger: always_on
---

# Impossible OS — Agent Rules

## Identity & Scope

You are an expert low-level OS developer. You are operating **strictly inside a sandboxed Ubuntu WSL 2 environment**. You have permission to:

- Create and edit files within this workspace (`~/impossible-os/`)
- Write C, C++, and x86-64 Assembly (NASM syntax)
- Use the integrated terminal to compile, link, and test the OS

## Boundaries — DO NOT

- **DO NOT** access directories outside of this workspace
- **DO NOT** modify host Windows configurations or registries
- **DO NOT** run `dd`, `mkfs`, or any disk tool targeting real devices (`/dev/sda`, `/dev/nvme*`, etc.)
- **DO NOT** access `/mnt/c/` or any Windows-mounted paths
- **DO NOT** install system-wide packages without explicit user approval
- **DO NOT** make external network requests unless instructed

## Build Constraints

- Always compile with: `-Wall -Wextra -Werror -ffreestanding -nostdlib -nostdinc`
- Use `clang-19 --target=x86_64-elf` with `ld.lld-19` (LLVM toolchain)
- Target architecture: **x86-64** (Long Mode)
- Final output: a bootable GPT disk image named **`system-disk.img`** in the `build/` directory

## Build Script — MANDATORY

- **ALWAYS** use `bash scripts/build.sh` instead of raw `make` commands
- Incremental build: `bash scripts/build.sh`
- Clean build: `bash scripts/build.sh clean`
- Build + QEMU test: `bash scripts/build.sh run`
- Clean build + QEMU: `bash scripts/build.sh clean run`
- Verify success: `tail -1 build/build.log` → must show `=== BUILD OK ===`
- **NEVER** run `make`, `make all`, `make clean`, or `make run` directly

## Testing Protocol

- **Always** test changes via `bash scripts/build.sh run` (QEMU) before committing
- Never push untested code to `main`
- Use serial output (`-serial stdio`) for kernel debug logging

## Code Style

- C source files: snake_case for functions and variables, UPPER_CASE for macros/constants
- Assembly files: NASM syntax, `.asm` extension
- One header per source file, with `#pragma once` or include guards
- Keep functions short (< 50 lines where possible)
- Comment all non-obvious hardware interactions (port I/O, MMIO, register manipulation)

## Commit Discipline

- Write clear, conventional commit messages: `"scope: short description"`
- Commit after each completed TODO item
- Never commit build artifacts (`build/`, `*.o`, `*.bin`, `*.iso`)

## Known Gotchas

- **PMM is the default allocator. `kmalloc` is the exception.** The kernel heap is only 2 MiB. `kmalloc` is strictly for small kernel bookkeeping: VFS nodes, task structs, Codex values, short strings, linked-list nodes — typically tens of bytes to a few KB each. **NEVER use `kmalloc` for image buffers, file read buffers, pixel data, font data, or any allocation that could plausibly exceed a few KB.** Use `pmm_alloc_contiguous()` for everything else — it allocates directly from physical memory (identity-mapped) with no size limit. Violating this rule causes silent heap exhaustion that is extremely difficult to debug. *(Learned from framebuffer bug `9722a74`, JPEG decode bug `f673e46`)*

- **Framebuffer back buffer must use PMM, not kmalloc.** The back buffer for 1280×720×32bpp is 3.6 MiB. `kmalloc` fails silently → `back_buf = hw_addr` → zero double buffering → compositor flicker. Always use `pmm_alloc_contiguous()`. Boot log must show `[OK] Framebuffer back buffer:` and `[OK] VBE page flip enabled` to confirm double buffering is active. *(Fixed in commit `9722a74`)*

## Hardware Constraints

- **APIC-only interrupts.** Route all hardware interrupts via LAPIC/IOAPIC. Do NOT write new 8259 PIC routing code. The PIC is masked at boot. *(Legacy PIC masking code in `pic.c` is kept for boot-time disable only.)*
- **DMA-only storage.** Use AHCI (DMA + NCQ) or VirtIO for disk I/O. Do NOT use legacy IDE/ATA PIO polling (port 0x1F0–0x1F7).
- **UEFI GOP framebuffer.** The framebuffer is a linear 32bpp buffer from UEFI GOP. Do NOT write VGA text mode (0xB8000) code.
- **RCU for read-heavy structures.** Prefer Read-Copy-Update over spinlocks for VFS mount list, process tree, and Registry cache.

## API Surface

- **Win32 is the native API.** User-space programs are PE32+ executables using Win32-style APIs (CreateFile, ReadFile, CreateProcess). POSIX APIs (open, read, fork) are secondary — for the Linux compat layer only.
- **Windows paths are canonical.** Use `C:\Impossible\System32\`, not `/usr/bin/`. Use `C:\Program Files\`, not `/usr/local/`.
- **Control Panel uses .cpl applets.** Settings are exposed via Windows-standard Control Panel Library applets (CPlApplet interface, .cpl extension).