# Impossible OS — Architecture Documentation

Comprehensive documentation of every implemented subsystem in Impossible OS.

## Contents

| Document | Subsystem |
|----------|-----------|
| [Bootloader](bootloader.md) | UEFI boot chain, GRUB, Multiboot2, Long Mode transition |
| [Kernel Core](kernel-core.md) | Framebuffer console, GDT/TSS, IDT, PIC, PIT, keyboard |
| [Memory Management](memory-management.md) | PMM (bitmap), VMM (4-level paging), heap (first-fit) |
| [Storage & Filesystem](storage-filesystem.md) | ATA, VFS (drive letters), initrd, FAT32, IXFS |
| [Multitasking](multitasking.md) | Scheduler, context switch, user mode, syscalls, ELF loader |
| [C Library](libc.md) | CRT0, stdio, string, stdlib, ctype, math, syscall ABI |
| [Shell](shell.md) | REPL, built-in commands, line editing, command history |
| [Networking](networking.md) | RTL8139, Ethernet, ARP, IPv4, ICMP, UDP, DHCP |
| [Desktop Environment](desktop-environment.md) | Framebuffer graphics, fonts, mouse, WM, taskbar, controls |
| [System Services](system-services.md) | Logging, RTC, ACPI, PCI |
