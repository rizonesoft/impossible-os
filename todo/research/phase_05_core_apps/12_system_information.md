# 78 — System Information

## Overview
Detailed system info panel: CPU, RAM, OS version, hardware, drivers, uptime. Like Windows' "About" + "System Information".

## Layout
```
┌──────────────────────────────────────────┐
│  System Information                 ─ □ ×│
├──────────────────────────────────────────┤
│  Impossible OS                           │
│  Version 0.2.0 (Build 2026.03.07)       │
│                                          │
│  Processor: x86-64 (QEMU Virtual CPU)   │
│  RAM: 2048 MB (1.8 GB available)         │
│  Uptime: 2 hours 15 minutes             │
│                                          │
│  Display: 1280×720 @ 32bpp              │
│  Network: RTL8139 — 10.0.2.15           │
│  Storage: VirtIO — 2.0 GB               │
│  Audio: AC97                             │
│                                          │
│  Kernel: built with x86_64-elf-gcc      │
│  Boot: Multiboot2 via GRUB2             │
└──────────────────────────────────────────┘
```

## Data: reads from CPUID, `g_boot_info`, PMM, PCI, Codex
## Also used by `about.spl` in Settings Panel
## Files: `src/apps/sysinfo/sysinfo.c` (~300 lines) | 2-3 days
