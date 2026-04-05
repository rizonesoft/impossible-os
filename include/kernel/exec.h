/* ============================================================================
 * exec.h -- Multi-format executable loader dispatcher
 *
 * Auto-detects binary format by magic bytes and dispatches to the correct
 * loader (ELF, PE32+, EIF, shebang). Replaces the direct elf_load() call
 * in task_exec().
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Format loader function type ----------------------------------------
 * Takes raw file data + size, returns entry point on success or 0 on failure.
 * The loader is responsible for copying segments into the user address range
 * using the existing identity mapping (or VMM-backed pages in future §2). */
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

/* ---- Public API --------------------------------------------------------- */

/* Initialize the exec subsystem. Called once during Phase 3 boot. */
void exec_init(void);

/* Register a binary format handler. Returns 0 on success, -1 if table full. */
int exec_register_format(const exec_format_t *fmt);

/* Load and execute a binary from a raw buffer.
 * Detects format by magic bytes, dispatches to the registered loader.
 * Returns entry point address on success, 0 on failure.
 * Sets *err to ENOEXEC (bad format) or ENOMEM (allocation failure). */
uint64_t exec_load(const uint8_t *data, uint64_t size, int *err);

/* Load a binary from a VFS path.
 * Opens the file, reads it into a kernel buffer, calls exec_load().
 * Returns entry point address on success, 0 on failure.
 * Sets *err to ENOENT (not found), ENOEXEC, or ENOMEM. */
uint64_t exec_load_path(const char *path, int *err);
