# Repository & Project Structure

## Git Repository

| Property | Value |
|----------|-------|
| Remote | `https://github.com/rizonesoft/impossible-os` |
| Branch | `main` |
| License | MIT |
| Initial commit | `f5772b2` |

## Directory Layout

```
impossible-os/
├── src/                       # All source code
│   ├── boot/                  # Bootloader (Multiboot2 header, Long Mode entry)
│   │   ├── entry.asm          # Kernel entry point (64-bit)
│   │   ├── multiboot2_header.asm
│   │   ├── grub.cfg           # GRUB menu configuration
│   │   └── linker.ld          # Linker script
│   │
│   ├── kernel/                # Kernel core
│   │   ├── main.c             # kernel_main() entry
│   │   ├── gdt.c              # Global Descriptor Table + TSS
│   │   ├── idt.c              # Interrupt Descriptor Table
│   │   ├── printk.c           # Kernel printf (serial + framebuffer)
│   │   ├── log.c              # Structured serial logging
│   │   ├── version.c          # Version info accessors
│   │   ├── acpi.c             # ACPI table parsing and power management
│   │   ├── elf.c              # ELF binary loader
│   │   ├── drivers/           # Hardware drivers
│   │   ├── mm/                # Memory management (PMM, VMM, heap)
│   │   ├── fs/                # Filesystems (VFS, initrd, FAT32, IXFS)
│   │   ├── sched/             # Scheduler, tasks, syscalls
│   │   └── net/               # Networking (Ethernet, ARP, IP, UDP, ICMP, DHCP)
│   │
│   ├── desktop/               # Desktop environment
│   │   ├── wm.c               # Window manager + compositor
│   │   ├── desktop.c          # Desktop shell (taskbar, start menu)
│   │   ├── font.c             # Bitmap font renderer
│   │   └── controls.c         # Common controls (Button, Label, TextBox, ScrollBar)
│   │
│   └── libc/                  # Minimal kernel C library
│       ├── string.c           # memcpy, memset, strlen, etc.
│       └── stdlib.c           # itoa, atoi, etc.
│
├── include/                   # Public headers (mirrors src/ structure)
│   ├── kernel/                # Kernel headers
│   │   ├── drivers/           # Driver headers
│   │   ├── mm/                # Memory management headers
│   │   ├── fs/                # Filesystem headers
│   │   ├── sched/             # Scheduler headers
│   │   └── net/               # Networking headers
│   └── desktop/               # Desktop headers
│
├── user/                      # User-mode programs
│   ├── shell.c                # Interactive shell
│   ├── include/               # User-mode headers (syscall.h)
│   └── lib/                   # User-mode libc
│
├── tools/                     # Build tools
│   └── make-initrd.c          # initrd image generator
│
├── assets/                    # Raw image assets
│
├── docs/                      # Project documentation
│   └── development-setup/     # Dev environment setup guides
│
├── todo/                      # TODO checklists
│   ├── TODO.md                # Main development checklist
│   ├── 099-TODO-ISOInstaller.md   # Deferred: installer
│   └── 100-TODO-ProductionTest.md # Deferred: Hyper-V testing
│
├── .agent/                    # AI agent configuration
│   ├── skills/                # Agent skill documents
│   └── workflows/             # Build/release workflows
│
├── build/                     # Build output (gitignored)
├── Makefile                   # Root build system
├── VERSION                    # SemVer version (0.1.0)
└── .build_number              # Auto-incrementing build counter (gitignored)
```

## .gitignore

The following are excluded from version control:

```gitignore
build/
*.o
*.bin
*.iso
*.img
cross-compiler/
.build_number
```

## Conventions

- **One header per source file** — `src/kernel/drivers/foo.c` → `include/kernel/drivers/foo.h`
- **Include paths** — `#include "kernel/drivers/foo.h"` (relative to `include/`)
- **Naming** — `snake_case` for functions/variables, `UPPER_CASE` for macros
- **Include guards** — `#pragma once` (no `#ifndef` boilerplate)
- **Commits** — Conventional format: `"scope: short description"`
