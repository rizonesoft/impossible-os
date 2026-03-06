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
- Use the cross-compiler (`x86_64-elf-gcc`) when available; fall back to system GCC with freestanding flags
- Target architecture: **x86-64** (Long Mode)
- Final output: an ISO file named **`os-build.iso`** in the `build/` directory

## Testing Protocol

- **Always** test changes via `make run` (QEMU) before committing
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