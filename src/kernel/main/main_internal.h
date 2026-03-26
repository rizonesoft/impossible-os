/* ============================================================================
 * main_internal.h — Shared declarations for kernel boot modules
 *
 * Internal header for the src/kernel/main/ directory.
 * Each boot phase is a separate .c file that exposes one entry function.
 * ============================================================================ */
#pragma once

#include "kernel/types.h"
#include "kernel/boot_info.h"
#include "kernel/boot_init.h"

/* ---- Formal boot phase entry points ------------------------------------- */
/* These replace the legacy boot_*_init() functions once §§2–5 are complete. */

/* boot_init.c — Critical init: serial, memory, klog, CPUID */
void boot_phase0(uint64_t magic, uint64_t mbi);

/* boot_interrupts.c — Platform services: GDT/IDT, APIC, timer, display */
void boot_phase1(void);

/* boot_storage.c — System services: PCI, storage, VFS, registry, network */
void boot_phase2(void);

/* boot_desktop.c — User platform: scheduler, IPC, exec loader, desktop */
void boot_phase3(void);

/* ---- Legacy boot phase entry points (active until §§2–5 land) ---------- */

/* boot_hw.c — Memory managers, CPUID, SIMD, disk drivers, block devices */
void boot_hw_init(uint64_t magic, uint64_t mbi);

/* boot_interrupts.c — GDT, IDT, PIC, PIT, RTC, keyboard, mouse, ACPI, SMP */
void boot_interrupts_init(void);

/* boot_storage.c — VFS, partition scan, filesystem mount, disk logging */
void boot_storage_init(uint64_t magic);

/* boot_tests.c — All boot-time verification tests */
void boot_tests_run(void);

/* boot_desktop.c — Font/icon/cursor/WM/desktop init, demo window */
void boot_desktop_init(void);

/* compositor.c — Main compositor event loop (never returns) */
void compositor_run(void);

/* ---- Helpers shared across boot modules ---- */

/* Forward declaration — full definition in kernel/fs/vfs.h */
struct vfs_node;

/* dump_dir_tree (blkdev_adapters.c) — Recursively log a directory tree */
uint32_t dump_dir_tree(const char *parent_path, struct vfs_node *node,
                       uint32_t depth);

/* blkdev_register_all (blkdev_adapters.c) — Register all detected drives */
void blkdev_register_all(void);
