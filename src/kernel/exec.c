/* ============================================================================
 * exec.c -- Multi-format executable loader dispatcher
 *
 * Auto-detects binary format by magic bytes and dispatches to the correct
 * loader. Currently supports ELF; PE32+ and EIF registration points ready.
 * ============================================================================ */

#include "kernel/exec.h"
#include "kernel/errno.h"
#include "kernel/elf.h"
#include "kernel/eif.h"
#include "kernel/pe.h"
#include "kernel/fs/vfs.h"
#include "kernel/mm/heap.h"
#include "kernel/klog.h"
#include "kernel/sched/spinlock.h"
#include "kernel/ob/peb.h"
#include "kernel/boot_init.h"
#include "kernel/sched/task.h"

/* ---- Format registry ---------------------------------------------------- */

static exec_format_t s_formats[EXEC_MAX_FORMATS];
static uint32_t      s_format_count;

/* ---- Global module registry (§6) ----------------------------------------
 * Fixed-size array protected by a spinlock. Sorted by base_address for
 * binary search in exec_find_module_by_pc(). Accessed from:
 *   - exec_register_module() -- thread context (exec path)
 *   - exec_find_module_by_pc() -- thread or IRQ context (stack trace, crash)
 * Use irqsave variants for IRQ safety. NOT NMI-safe -- NMI crash dump
 * path needs a lockless fallback (TODO-16 §3 will provide one). */

static loaded_module_t s_modules[EXEC_MAX_MODULES];
static uint32_t        s_module_count;
static spinlock_t      s_module_lock = SPINLOCK_INIT;

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

    /* Register built-in EIF format */
    static const exec_format_t eif_fmt = {
        .magic     = { 'E', 'I', 'F', '!' },
        .magic_len = 4,
        .name      = "EIF",
        .loader    = eif_load,
    };

    /* Register built-in PE32+ format */
    static const exec_format_t pe_fmt = {
        .magic     = { 'M', 'Z' },
        .magic_len = 2,
        .name      = "PE32+",
        .loader    = pe_load,
    };

    s_format_count = 0;
    exec_register_format(&elf_fmt);
    exec_register_format(&eif_fmt);
    exec_register_format(&pe_fmt);

    klog(LOG_INFO, "exec", "Exec subsystem initialized (%u format(s))",
         (uint64_t)s_format_count);
}

/* ---- Registration -------------------------------------------------------
 * Format registration is init-only: called from exec_init() during
 * single-threaded Phase 3 boot. match_format() uses acquire-load on
 * s_format_count; registration uses release-store after fully writing
 * the slot. This is safe because no concurrent exec_load() can run
 * until after exec_init() completes and the scheduler starts.
 * Do NOT call exec_register_format() after boot. */

int exec_register_format(const exec_format_t *fmt)
{
    if (!fmt || s_format_count >= EXEC_MAX_FORMATS)
        return -1;
    if (fmt->magic_len == 0 || fmt->magic_len > EXEC_MAGIC_MAX)
        return -1;
    if (!fmt->loader)
        return -1;

    s_formats[s_format_count] = *fmt;
    /* Release-store: ensures the fully-written slot is visible to any
     * concurrent acquire-load reader in match_format(). */
    __atomic_store_n(&s_format_count, s_format_count + 1, __ATOMIC_RELEASE);

    klog(LOG_INFO, "exec", "Registered format: %s (magic %u bytes)",
         fmt->name, (uint64_t)fmt->magic_len);
    return 0;
}

/* ---- Magic matching ----------------------------------------------------- */

static const exec_format_t *match_format(const uint8_t *data, uint64_t size)
{
    uint32_t count = __atomic_load_n(&s_format_count, __ATOMIC_ACQUIRE);
    uint32_t i, j;

    for (i = 0; i < count; i++) {
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
        /* Build magic for log without unaligned type-pun */
        uint32_t m = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                     ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
        klog(LOG_DEBUG, "exec", "Unknown binary format (magic: 0x%x)",
             (uint64_t)m);
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
    uint64_t sz;

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

    /* Snapshot size once to prevent TOCTOU between alloc and read */
    sz = file->size;

    if (sz == 0 || sz > 16 * 1024 * 1024) {
        klog(LOG_ERROR, "exec", "Invalid file size: %u", sz);
        vfs_close(file);
        if (err) *err = ENOEXEC;
        return 0;
    }

    buf = (uint8_t *)kmalloc((uint32_t)sz);
    if (!buf) {
        vfs_close(file);
        if (err) *err = ENOMEM;
        return 0;
    }

    vfs_read(file, 0, (uint32_t)sz, buf);
    vfs_close(file);

    entry = exec_load(buf, sz, err);

    /* ELF loader copies segments via elf_memcpy to identity-mapped user
     * range. The staging buffer is no longer needed regardless of outcome. */
    kfree(buf);

    return entry;
}

/* ---- Module registration (§6) ------------------------------------------- */

/* Insert 'mod' into the sorted s_modules[] array at index 'pos'.
 * Caller must hold s_module_lock. */
static void module_insert_at(uint32_t pos, const loaded_module_t *mod)
{
    uint32_t i;
    /* Shift entries right to make room */
    for (i = s_module_count; i > pos; i--)
        s_modules[i] = s_modules[i - 1];
    s_modules[pos] = *mod;
    s_module_count++;
}

/* Find insertion index to keep s_modules[] sorted by base_address.
 * Caller must hold s_module_lock. */
static uint32_t module_find_insert_pos(uint64_t base)
{
    uint32_t lo = 0, hi = s_module_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (s_modules[mid].base_address < base)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* Insert an LDR_DATA_TABLE_ENTRY into a PEB->Ldr list.
 * 'head' is the list head (in PEB_LDR_DATA).
 * 'entry' is the LIST_ENTRY field within the new LDR_DATA_TABLE_ENTRY
 * (e.g., &ldr_entry->InLoadOrderLinks). */
static void ldr_list_insert_tail(LIST_ENTRY *head, LIST_ENTRY *entry)
{
    entry->Blink = head->Blink;
    entry->Flink = head;
    head->Blink->Flink = entry;
    head->Blink = entry;
}

/* Bounded ASCII-to-UTF16 copy. Copies up to 'max_chars' characters from
 * 'src' into 'dst' (UTF-16). Always NUL-terminates. Returns number of
 * uint16_t written INCLUDING the NUL terminator. */
static uint32_t ascii_to_utf16_bounded(uint16_t *dst, const char *src,
                                        uint32_t max_chars)
{
    uint32_t i = 0;
    while (i < max_chars && src[i]) {
        dst[i] = (uint16_t)(uint8_t)src[i];
        i++;
    }
    dst[i] = 0;
    return i + 1;
}

/* LDR_DATA_TABLE_ENTRY allocation size: struct + two UTF-16 strings.
 * Each string: max chars * 2 bytes per char + 2 bytes NUL. */
#define LDR_ENTRY_ALLOC_SIZE \
    (sizeof(LDR_DATA_TABLE_ENTRY) + \
     (EXEC_MODULE_PATH_MAX * 2 + 2) + \
     (EXEC_MODULE_NAME_MAX * 2 + 2))

int exec_register_module(process_t *proc, const loaded_module_t *mod)
{
    uint64_t flags;
    uint32_t pos;
    uint64_t mod_end;

    POST16(0xD809);

    if (!mod) {
        klog(LOG_DEBUG, "exec", "exec_register_module: NULL module");
        return -1;
    }

    if (mod->base_address == 0 || mod->size_of_image == 0) {
        klog(LOG_DEBUG, "exec", "exec_register_module: invalid base/size");
        return -1;
    }

    /* Overflow check: base + size must not wrap */
    mod_end = mod->base_address + mod->size_of_image;
    if (mod_end < mod->base_address) {
        klog(LOG_ERROR, "exec",
             "exec_register_module: base+size overflow (0x%x + 0x%x)",
             mod->base_address, mod->size_of_image);
        return -1;
    }

    /* ---- Insert into global crash registry (sorted by base_address) ---- */
    spin_lock_irqsave(&s_module_lock, &flags);

    if (s_module_count >= EXEC_MAX_MODULES) {
        spin_unlock_irqrestore(&s_module_lock, flags);
        klog(LOG_ERROR, "exec", "Module registry full (%u/%u)",
             (uint64_t)s_module_count, (uint64_t)EXEC_MAX_MODULES);
        return -1;
    }

    pos = module_find_insert_pos(mod->base_address);

    /* Reject duplicate base address */
    if (pos < s_module_count &&
        s_modules[pos].base_address == mod->base_address) {
        spin_unlock_irqrestore(&s_module_lock, flags);
        klog(LOG_WARN, "exec", "Module at 0x%x already registered",
             mod->base_address);
        return 0;
    }

    /* Reject overlapping ranges: check predecessor and successor */
    if (pos > 0) {
        uint64_t prev_end = s_modules[pos - 1].base_address +
                            s_modules[pos - 1].size_of_image;
        if (mod->base_address < prev_end) {
            spin_unlock_irqrestore(&s_module_lock, flags);
            klog(LOG_ERROR, "exec",
                 "Module at 0x%x overlaps with module at 0x%x",
                 mod->base_address, s_modules[pos - 1].base_address);
            return -1;
        }
    }
    if (pos < s_module_count) {
        if (mod_end > s_modules[pos].base_address) {
            spin_unlock_irqrestore(&s_module_lock, flags);
            klog(LOG_ERROR, "exec",
                 "Module at 0x%x (end 0x%x) overlaps with module at 0x%x",
                 mod->base_address, mod_end, s_modules[pos].base_address);
            return -1;
        }
    }

    module_insert_at(pos, mod);

    spin_unlock_irqrestore(&s_module_lock, flags);

    klog(LOG_INFO, "exec", "exec: registered module '%s' at 0x%x (size=%u)",
         mod->name, mod->base_address, mod->size_of_image);

    /* ---- Insert into PEB->Ldr lists ----
     * Per-process Ldr insertion is safe without a per-process lock because
     * exec_register_module() is called from the exec path (task_exec),
     * which is serial per-process. When multi-threaded DLL loading (§14)
     * is implemented, a per-process loader lock will be needed here. */
    if (proc && proc->task && proc->task->peb) {
        PEB *peb = (PEB *)proc->task->peb;
        PEB_LDR_DATA *ldr = peb->Ldr;

        if (ldr && ldr->Initialized) {
            _Static_assert(LDR_ENTRY_ALLOC_SIZE <= 4096,
                "LDR entry alloc must fit in kmalloc limit");

            LDR_DATA_TABLE_ENTRY *ldr_entry =
                (LDR_DATA_TABLE_ENTRY *)kmalloc(
                    (uint32_t)LDR_ENTRY_ALLOC_SIZE);

            if (ldr_entry) {
                /* Zero the allocation */
                uint8_t *p = (uint8_t *)ldr_entry;
                uint32_t i;
                for (i = 0; i < (uint32_t)LDR_ENTRY_ALLOC_SIZE; i++)
                    p[i] = 0;

                ldr_entry->DllBase = (void *)mod->base_address;
                ldr_entry->EntryPoint = (void *)mod->entry_point;
                ldr_entry->SizeOfImage = mod->size_of_image;
                ldr_entry->Flags = 0x00004000;  /* LDRP_ENTRY_PROCESSED */
                ldr_entry->LoadCount = 1;

                /* Build UNICODE_STRING fields in bounded buffer after struct */
                uint16_t *str_buf = (uint16_t *)(
                    (uint8_t *)ldr_entry + sizeof(LDR_DATA_TABLE_ENTRY));

                /* FullDllName from full_path (bounded copy) */
                {
                    uint16_t *start = str_buf;
                    uint32_t n = ascii_to_utf16_bounded(
                        str_buf, mod->full_path, EXEC_MODULE_PATH_MAX - 1);
                    str_buf += n;
                    uint32_t byte_len = (n - 1) * 2;
                    ldr_entry->FullDllName.Length = (uint16_t)byte_len;
                    ldr_entry->FullDllName.MaximumLength =
                        (uint16_t)(byte_len + 2);
                    ldr_entry->FullDllName._pad = 0;
                    ldr_entry->FullDllName.Buffer = start;
                }

                /* BaseDllName from name (bounded copy) */
                {
                    uint16_t *start = str_buf;
                    uint32_t n = ascii_to_utf16_bounded(
                        str_buf, mod->name, EXEC_MODULE_NAME_MAX - 1);
                    uint32_t byte_len = (n - 1) * 2;
                    ldr_entry->BaseDllName.Length = (uint16_t)byte_len;
                    ldr_entry->BaseDllName.MaximumLength =
                        (uint16_t)(byte_len + 2);
                    ldr_entry->BaseDllName._pad = 0;
                    ldr_entry->BaseDllName.Buffer = start;
                }

                /* Insert into all three Ldr lists (tail insertion) */
                ldr_list_insert_tail(&ldr->InLoadOrderModuleList,
                                     &ldr_entry->InLoadOrderLinks);
                ldr_list_insert_tail(&ldr->InMemoryOrderModuleList,
                                     &ldr_entry->InMemoryOrderLinks);
                ldr_list_insert_tail(&ldr->InInitializationOrderModuleList,
                                     &ldr_entry->InInitializationOrderLinks);

                klog(LOG_DEBUG, "exec",
                     "Inserted LDR_DATA_TABLE_ENTRY for '%s' into PEB->Ldr",
                     mod->name);
            } else {
                klog(LOG_WARN, "exec",
                     "Failed to allocate LDR entry for '%s' -- module is in "
                     "crash registry but not visible via PEB->Ldr walks",
                     mod->name);
            }
        }
    }

    /* ---- ELF link_map chain (Linux compat) ----
     * For ELF modules, insert into per-process link_map for dl_iterate_phdr().
     * Currently tracked in global crash registry only; per-process link_map
     * chain deferred to dynamic linker (§14) when process-private address
     * spaces exist. */
    if (mod->format == EXEC_FMT_ELF) {
        klog(LOG_DEBUG, "exec",
             "ELF module '%s' registered (link_map deferred to dynamic linker §14)",
             mod->name);
    }

    POST16(0xD80A);
    return 0;
}

int exec_find_module_by_pc(uint64_t rip, loaded_module_t *out)
{
    uint64_t flags;
    int found = -1;

    if (!out)
        return -1;

    spin_lock_irqsave(&s_module_lock, &flags);

    /* Binary search: find the last module whose base_address <= rip */
    uint32_t lo = 0, hi = s_module_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (s_modules[mid].base_address <= rip)
            lo = mid + 1;
        else
            hi = mid;
    }

    /* lo-1 is the candidate (largest base_address <= rip) */
    if (lo > 0) {
        const loaded_module_t *candidate = &s_modules[lo - 1];
        if (rip < candidate->base_address + candidate->size_of_image) {
            *out = *candidate;  /* copy under lock -- stable snapshot */
            found = 0;
        }
    }

    spin_unlock_irqrestore(&s_module_lock, flags);
    return found;
}

uint32_t exec_module_count(void)
{
    return __atomic_load_n(&s_module_count, __ATOMIC_ACQUIRE);
}
