/* ============================================================================
 * main_internal.h -- Shared declarations for kernel boot modules
 *
 * Internal header for the src/kernel/main/ directory.
 * Each boot phase is a separate .c file that exposes one entry function.
 * ============================================================================ */
#pragma once

#include "kernel/types.h"
#include "kernel/boot_info.h"
#include "kernel/boot_init.h"

/* ---- Formal boot phase entry points ------------------------------------- */
/* These replace the legacy boot_*_init() functions once §–5 are complete. */

/* boot_init.c -- Critical init: serial, memory, klog, CPUID */
void boot_phase0(uint64_t magic, uint64_t mbi);

/* boot_interrupts.c -- Platform services: GDT/IDT, APIC, timer, display */
void boot_phase1(void);

/* boot_storage.c -- System services: PCI, storage, VFS, registry, network */
void boot_phase2(void);

/* boot_desktop.c -- User platform: scheduler, IPC, exec loader, desktop */
void boot_phase3(void);

/* ---- Legacy boot phase entry points (active until §–5 land) ---------- */

/* boot_hw.c -- Memory managers, CPUID, SIMD, disk drivers, block devices */
void boot_hw_init(uint64_t magic, uint64_t mbi);

/* boot_interrupts.c -- GDT, IDT, PIC, PIT, RTC, keyboard, mouse, ACPI, SMP */
void boot_interrupts_init(void);

/* boot_storage.c -- VFS, partition scan, filesystem mount, disk logging */
void boot_storage_init(uint64_t magic);

/* boot_tests.c -- All boot-time verification tests */
void boot_tests_run(void);

/* boot_desktop.c -- Font/icon/cursor/WM/desktop init, demo window */
void boot_desktop_init(void);

/* compositor.c -- Main compositor event loop.
 * Normal mode: never returns (runs as the BSP idle/main kernel thread).
 * Headless mode (boot.conf `compositor=headless`): the function HLT-
 * idles the BSP so tests / external drivers own frame advancement via
 * `compositor_step_frames()`. Bare-metal boot REJECTS headless mode;
 * see boot_desktop.c for the refusal path. */
void compositor_run(void);

/* Headless compositor + frame-lock stepping (TODO-05 desktop UI test
 * framework, headless compositor section). The set/get knobs are
 * always safe to call; `compositor_step_frames` is REJECTED (returns
 * 0) when headless mode is off, so it cannot race the main compositor
 * loop -- mixing the headless step path with a live compositor_run is
 * unsafe. */

/* Switch the compositor to headless mode. Must be called BEFORE
 * compositor_run() to suppress the infinite display loop. Normal
 * callers leave headless off (default 0). */
void compositor_set_headless(int headless);
int  compositor_is_headless(void);

/* Set the deterministic seed used for future animation / transition
 * RNG so byte-identical replay is possible under's virtual
 * clock. Today no compositor feature consumes the seed; the API slot
 * lands ahead of animation work so trace tooling can commit to the
 * contract. */
#ifdef KERNEL_TESTS
void     compositor_set_test_seed(uint64_t seed);
uint64_t compositor_get_test_seed(void);
#endif

/* Drive exactly `n` compositor frames: mark dirty, composite, present
 * (skipping real fb_swap in headless mode), advance the frame-
 * stats counters. Returns the number of frames actually presented.
 * Returns 0 when headless mode is OFF: running step_frames
 * concurrently with the main compositor loop would race windows[]
 * traversal against wm_destroy_window. Callers who want frame-locked
 * stepping MUST first `compositor_set_headless(1)` (which
 * suppresses the real display loop). */
uint32_t compositor_step_frames(uint32_t n);

/* ---- Helpers shared across boot modules ---- */

/* Forward declaration -- full definition in kernel/fs/vfs.h */
struct vfs_node;

/* dump_dir_tree (blkdev_adapters.c) -- Recursively log a directory tree */
uint32_t dump_dir_tree(const char *parent_path, struct vfs_node *node,
                       uint32_t depth);

/* Storage drivers a failed async init could NOT make safe (TODO-10 S27). Bit
 * index matches the async storage step index in boot_storage.c, so one loop
 * over the outcome record builds the mask.
 *
 * A driver is unsafe when its step ended SKIP: either its worker still owns the
 * step (it is inside the controller reset right now) or the step was POISONED
 * (the worker was cut at an arbitrary instruction, so the driver's globals are
 * arbitrarily partial). In both cases the BSP deliberately did NOT re-run the
 * initializer, so nothing ever completed it -- reading those globals to size a
 * disk or to arm an interrupt is reading half-written state. */
#define BLKDEV_UNSAFE_ATA     (1u << 0)
#define BLKDEV_UNSAFE_AHCI    (1u << 1)
#define BLKDEV_UNSAFE_NVME    (1u << 2)
#define BLKDEV_UNSAFE_VIRTIO  (1u << 3)

/* blkdev_register_all (blkdev_adapters.c) -- Register all detected drives.
 * `unsafe_mask` is a BLKDEV_UNSAFE_* OR; those drivers are skipped rather than
 * registered, because a block device built from partial driver state hands the
 * VFS a wrong capacity or sector size and corrupts on first write. Pass 0 when
 * every driver initialized to completion. */
void blkdev_register_all(uint32_t unsafe_mask);
