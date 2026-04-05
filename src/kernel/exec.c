/* ============================================================================
 * exec.c -- Multi-format executable loader dispatcher
 *
 * Auto-detects binary format by magic bytes and dispatches to the correct
 * loader. Currently supports ELF; PE32+ and EIF registration points ready.
 * ============================================================================ */

#include "kernel/exec.h"
#include "kernel/errno.h"
#include "kernel/elf.h"
#include "kernel/fs/vfs.h"
#include "kernel/mm/heap.h"
#include "kernel/klog.h"

/* ---- Format registry ---------------------------------------------------- */

static exec_format_t s_formats[EXEC_MAX_FORMATS];
static uint32_t      s_format_count;

/* ---- ELF loader wrapper ------------------------------------------------- */

static uint64_t elf_exec_wrapper(const uint8_t *data, uint64_t size)
{
    struct elf_load_result r = elf_load(data, size);
    if (!r.success)
        return 0;
    return r.entry;
}

/* ---- Init --------------------------------------------------------------- */

void exec_init(void)
{
    /* Register built-in ELF format */
    static const exec_format_t elf_fmt = {
        .magic     = { 0x7F, 'E', 'L', 'F' },
        .magic_len = 4,
        .name      = "ELF",
        .loader    = elf_exec_wrapper,
    };

    s_format_count = 0;
    exec_register_format(&elf_fmt);

    klog(LOG_INFO, "exec", "Exec subsystem initialized (%u format(s))",
         (uint64_t)s_format_count);
}

/* ---- Registration ------------------------------------------------------- */

int exec_register_format(const exec_format_t *fmt)
{
    if (!fmt || s_format_count >= EXEC_MAX_FORMATS)
        return -1;

    s_formats[s_format_count] = *fmt;
    s_format_count++;

    klog(LOG_INFO, "exec", "Registered format: %s (magic %u bytes)",
         fmt->name, (uint64_t)fmt->magic_len);
    return 0;
}

/* ---- Magic matching ----------------------------------------------------- */

static const exec_format_t *match_format(const uint8_t *data, uint64_t size)
{
    uint32_t i, j;

    for (i = 0; i < s_format_count; i++) {
        const exec_format_t *f = &s_formats[i];
        if (size < f->magic_len)
            continue;

        int match = 1;
        for (j = 0; j < f->magic_len; j++) {
            if (data[j] != f->magic[j]) {
                match = 0;
                break;
            }
        }
        if (match)
            return f;
    }
    return (const exec_format_t *)0;
}

/* ---- Load from buffer --------------------------------------------------- */

uint64_t exec_load(const uint8_t *data, uint64_t size, int *err)
{
    const exec_format_t *fmt;
    uint64_t entry;

    if (!data || size < 4) {
        if (err) *err = ENOEXEC;
        return 0;
    }

    fmt = match_format(data, size);
    if (!fmt) {
        klog(LOG_WARN, "exec", "Unknown binary format (magic: 0x%x)",
             (uint64_t)(*(const uint32_t *)data));
        if (err) *err = ENOEXEC;
        return 0;
    }

    klog(LOG_DEBUG, "exec", "Loading %s binary (%u bytes)",
         fmt->name, size);

    entry = fmt->loader(data, size);
    if (entry == 0) {
        klog(LOG_ERROR, "exec", "%s loader failed", fmt->name);
        if (err) *err = ENOEXEC;
        return 0;
    }

    if (err) *err = 0;
    return entry;
}

/* ---- Load from VFS path ------------------------------------------------- */

uint64_t exec_load_path(const char *path, int *err)
{
    struct vfs_node *file;
    uint8_t *buf;
    uint64_t entry;

    if (!path) {
        if (err) *err = ENOENT;
        return 0;
    }

    file = vfs_open(path, VFS_O_READ);
    if (!file) {
        klog(LOG_WARN, "exec", "File not found: %s", path);
        if (err) *err = ENOENT;
        return 0;
    }

    if (file->size == 0 || file->size > 16 * 1024 * 1024) {
        klog(LOG_ERROR, "exec", "Invalid file size: %u", file->size);
        vfs_close(file);
        if (err) *err = ENOEXEC;
        return 0;
    }

    buf = (uint8_t *)kmalloc((uint32_t)file->size);
    if (!buf) {
        vfs_close(file);
        if (err) *err = ENOMEM;
        return 0;
    }

    vfs_read(file, 0, (uint32_t)file->size, buf);
    vfs_close(file);

    entry = exec_load(buf, file->size, err);

    /* Don't free buf -- the loaded segments are identity-mapped from it.
     * The buffer backing will be freed when the process exits and its
     * address space is torn down. Future VMM-backed loaders (§2) will
     * copy data to user pages and free the buffer here. */

    return entry;
}
