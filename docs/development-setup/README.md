# Development Environment Setup

This directory documents the complete development environment for Impossible OS.

## Contents

| Document | Description |
|----------|-------------|
| [Repository](repository.md) | Git setup, directory structure, naming conventions |
| [Build System](build-system.md) | Makefile targets, build pipeline, versioning integration |
| [WSL Sandbox](wsl-sandbox.md) | WSL 2 setup, workspace isolation, safety boundaries |
| [Toolchain](toolchain.md) | Cross-compiler, NASM, build tools, and verified versions |
| [QEMU & UEFI](qemu-uefi.md) | QEMU configuration, OVMF firmware, testing and debugging |
| [Agent Configuration](agent-config.md) | IDE rules, skills, workflows, and coding conventions |

## Quick Reference

| Tool | Version | Path |
|------|---------|------|
| Ubuntu | 24.04.4 LTS | WSL 2 |
| WSL Kernel | 6.6.87.2-microsoft-standard-WSL2 | — |
| x86_64-elf-gcc | 13.3.0 | `~/opt/cross/bin/` |
| x86_64-elf-ld (Binutils) | 2.42 | `~/opt/cross/bin/` |
| NASM | 2.16.01 | system |
| QEMU | 8.2.2 | system |
| OVMF | 2024.02 | `/usr/share/OVMF/` |
| GRUB | 2.12 | system |
| GNU Make | 4.3 | system |
| Git | 2.43.0 | system |
