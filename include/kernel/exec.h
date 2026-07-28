/* ============================================================================
 * exec.h -- Multi-format executable loader dispatcher
 *
 * Auto-detects binary format by magic bytes and dispatches to the correct
 * loader (ELF, PE32+, EIF, shebang). Replaces the direct elf_load() call
 * in task_exec().
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/boot_init.h"
#include "kernel/ob/ob_process.h"

/* process_t alias for the exec API. Wraps PROCESS_OBJECT from the Object
 * Manager. Currently unused by exec_load (which takes raw buffers), but
 * needed by future elf_exec(path, proc) and pe_exec(path, proc) loaders
 * that operate on a specific process context. */
typedef PROCESS_OBJECT process_t;

/* ---- Format loader function type ----------------------------------------
 * Takes raw file data + size, returns entry point on success or 0 on failure.
 * The loader is responsible for copying segments into the user address range
 * using the existing identity mapping (or VMM-backed pages in future). */
typedef uint64_t (*exec_loader_fn)(const uint8_t *data, uint64_t size);

/* ---- Format descriptor -------------------------------------------------- */

#define EXEC_MAGIC_MAX  4   /* max magic bytes to match */
#define EXEC_MAX_FORMATS 8  /* max registered formats */

typedef struct exec_format {
    uint8_t         magic[EXEC_MAGIC_MAX];  /* magic bytes to match */
    uint32_t        magic_len;              /* number of magic bytes (1-4) */
    const char     *name;                   /* human-readable name */
    exec_loader_fn  loader;                 /* format-specific loader */
} exec_format_t;

/* ---- Module list --------------------------------------------------- */

/* Binary format identifiers */
#define EXEC_FMT_ELF  0
#define EXEC_FMT_PE   1
#define EXEC_FMT_EIF  2

/* Maximum modules tracked globally (crash dump, debugger, stack traces) */
#define EXEC_MAX_MODULES  64

/* Module name / path size limits (including NUL terminator) */
#define EXEC_MODULE_NAME_MAX  64
#define EXEC_MODULE_PATH_MAX  256

/* Per-module registration entry.
 * Populated by exec_register_module() for every loaded binary.
 * Consumed by: crash dump, SEH unwind, debugger, and PEB->Ldr module walks. */
typedef struct loaded_module {
    uint64_t    base_address;                   /* load VA */
    uint64_t    size_of_image;                  /* total mapped size */
    uint64_t    entry_point;                    /* entry function VA */
    uint64_t    pdata_base;                     /* .pdata section VA (0 if none) */
    uint64_t    pdata_size;                     /* .pdata section size in bytes */
    char        full_path[EXEC_MODULE_PATH_MAX]; /* e.g. "C:\Programs\hello.exe" */
    char        name[EXEC_MODULE_NAME_MAX];      /* base name, e.g. "hello.exe" */
    uint32_t    format;                          /* EXEC_FMT_ELF / PE / EIF */
    uint32_t    _pad;                            /* align to 8 bytes */
} loaded_module_t;

_Static_assert(sizeof(loaded_module_t) == 368,
    "loaded_module_t size -- update serializers if changed");

/* ---- Public API --------------------------------------------------------- */

/* Initialize the exec subsystem. Called once during Phase 3 boot.
 * Returns BOOT_FATAL if the native ELF loader cannot register (no usable
 * userspace), BOOT_DEGRADED if only an optional format (EIF/PE) failed,
 * BOOT_OK otherwise. */
boot_result_t exec_init(void);

/* Register a binary format handler. Returns 0 on success, -1 if table full. */
int exec_register_format(const exec_format_t *fmt);

/* Load and execute a binary from a raw buffer.
 * Detects format by magic bytes, dispatches to the registered loader.
 * Returns entry point address on success, 0 on failure.
 * Sets *err to ENOEXEC (bad format) or ENOMEM (allocation failure). */
uint64_t exec_load(const uint8_t *data, uint64_t size, int *err);

/* Same as exec_load but also returns the matched format name via
 * `*out_fmt_name` (e.g. "ELF", "PE32+", "EIF"). Writes to the out-param
 * on EVERY call: on success with the chosen loader's name, on failure
 * with (const char *)0. Used by task_exec to stash the format in the
 * task struct so the user-mode launcher can log which loader picked
 * each binary without re-inspecting the magic bytes. Pointer aliases
 * into the static format-registry table (s_formats); lifetime is
 * forever. Passing NULL for out_fmt_name collapses to the same
 * behavior as exec_load. */
uint64_t exec_load_fmt(const uint8_t *data, uint64_t size, int *err,
                       const char **out_fmt_name);

/* Load a binary from a VFS path.
 * Opens the file, reads it into a kernel buffer, calls exec_load().
 * Returns entry point address on success, 0 on failure.
 * Sets *err to ENOENT (not found), ENOEXEC, or ENOMEM. */
uint64_t exec_load_path(const char *path, int *err);

/* ---- Module registration ------------------------------------------- */

/* Register a loaded module in the global crash registry and the per-process
 * PEB->Ldr module list (for PE32+ modules).
 * 'proc' may be NULL for kernel-mode modules (crash registry only).
 * Returns 0 on success, -1 if the global registry is full. */
int exec_register_module(process_t *proc, const loaded_module_t *mod);

/* Find a registered module by instruction pointer (binary search by address
 * range). Copies the matching module into 'out' (caller-provided buffer).
 * Returns 0 on success, -1 if no module contains the given RIP.
 * Used for stack traces, .pdata lookup, and crash dump module identification.
 * Thread-safe: copy is performed under irqsave spinlock, so the returned
 * data is a stable snapshot even if the registry is concurrently modified.
 * NOT safe from NMI context (spinlock may be held by interrupted CPU).
 * NMI crash dump path should use a lockless fallback or bounded trylock. */
int exec_find_module_by_pc(uint64_t rip, loaded_module_t *out);

/* Snapshot-copy all registered modules into caller-provided buffer.
 * 'out' must point to an array of at least 'max_count' loaded_module_t.
 * Returns the number of modules copied (capped at max_count).
 * Uses irqsave spinlock -- NOT NMI-safe. For NMI crash dump context,
 * use exec_iterate_modules_lockless() which reads without locking
 * (may see a torn entry if the registry was being modified at crash time,
 * but that is acceptable for crash diagnostics). */
uint32_t exec_iterate_modules(loaded_module_t *out, uint32_t max_count);

/* Lockless variant for crash dump / NMI context.
 * Same interface as exec_iterate_modules() but skips the spinlock.
 * ONLY safe when called from a panic path where ALL other CPUs have
 * been halted via IPI FREEZE (see crash dump writer). If called
 * while another CPU is still running exec_register_module(), the copy
 * may contain torn entries. The caller MUST ensure global CPU freeze
 * before invoking this. */
uint32_t exec_iterate_modules_lockless(loaded_module_t *out,
                                        uint32_t max_count);

/* Return the current number of registered modules (for diagnostics). */
uint32_t exec_module_count(void);

/* Upper bound on a RAW executable image staged into kernel memory before any
 * format loader sees it. This is a staging/heap bound and is deliberately NOT
 * USER_ELF_SIZE: that constant bounds the MAPPED user image window, and a valid
 * ELF can exceed it on disk through non-PT_LOAD sections, debug data, or file
 * alignment while every PT_LOAD segment still lands inside the window. Capping
 * the raw file at the mapped size rejects loadable binaries before the loader
 * gets to validate them; each format loader enforces its own mapped bounds. */
#define EXEC_MAX_IMAGE_SIZE  (16u * 1024u * 1024u)

/* Upper bound for a raw image staged on the KMALLOC HEAP specifically.
 *
 * Two different limits, because they answer two different questions.
 * EXEC_MAX_IMAGE_SIZE above is what a FORMAT LOADER can be handed. This one is
 * what the kmalloc arena can afford to lend a single unprivileged exec: the
 * heap is a fixed 512-page / 2 MiB region (`heap.c` HEAP_INITIAL_PAGES), so a
 * multi-megabyte staging request either fails outright or holds most of the
 * global heap for the whole read-and-load, starving every concurrent kernel
 * allocation. The format bound is therefore NOT usable as the allocation bound.
 *
 * The number is DERIVED, not inherited: a single unprivileged exec must not be
 * able to claim more than roughly a third of the heap that is actually free at
 * boot baseline. Measured 2026-07-28: the arena is 2 MiB and about 454 KiB is
 * already in use once boot settles, leaving ~1.6 MiB, so the ceiling is 512
 * KiB. The stagers previously enforced 1 MiB by mis-borrowing USER_ELF_SIZE
 * (the MAPPED-window constant) -- safe-by-accident, wrong reason, and ~62% of
 * free heap, which is too much for a path any user binary can trigger.
 *
 * Not lowered further (256 KiB was proposed): that is only ~3.7x the largest
 * executable in the tree today and would become a functional ceiling that
 * rejects legitimate mid-size binaries well before the allocator work lands.
 * The arena can also come up SHORT of 2 MiB if heap_init takes its partial
 * fallback, which argues for finishing the vmalloc routing rather than for
 * tuning this number again.
 *
 * Lifting it means staging large images off the kmalloc heap -- a locked
 * page-backed large-buffer allocator, tracked by the advanced-allocator work
 * that `shell_loader.c` already cites. Until then a raw image between this
 * bound and EXEC_MAX_IMAGE_SIZE is loadable only by a path that stages it with
 * pmm_alloc_contiguous, not by the kmalloc stagers. */
#define EXEC_KMALLOC_STAGE_MAX  (512u * 1024u)
