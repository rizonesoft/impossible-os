/* Boot load/status log (ntbtlog parity).
 *
 * See include/kernel/boot_load_status.h for the contract. Design notes:
 *  - Slot allocation is a lock-free atomic fetch-add: boot probes can run on
 *    parallel CPUs (boot_async_group fans storage drivers across cores), so a
 *    plain shared-count append would race. Each claimer owns its slot index
 *    exclusively, fills it, then release-publishes a `published` flag the
 *    formatter reads with acquire ordering.
 *  - The monotonic claim counter is the single source of truth for both the
 *    recorded count (min(claimed, MAX)) and the dropped count (claimed - MAX
 *    when over capacity), so overflow can never read as a complete log.
 *  - duration_ms is MEASURED across begin..finish, never inferred from the
 *    gap to a neighbouring entry -- async probes overlap, so a gap model would
 *    mis-attribute a slow driver's time to whichever entry happened to follow.
 */

#include "kernel/types.h"
#include "kernel/boot_load_status.h"
#include "kernel/boot_progress.h"   /* boot_get_elapsed_ms */
#include "kernel/boot_splash.h"     /* boot_splash_diag */
#include "kernel/klog.h"
#include "kernel/fs/vfs.h"
#include "kernel/mm/pmm.h"
#include "kernel/atomic.h"

#define BOOT_LOAD_PATH      "X:\\Diag\\boot-load-status.txt"
#define BOOT_LOAD_BUF_PAGES 2u                          /* 64*~80 = ~5 KiB > kmalloc cap */
#define BOOT_LOAD_BUF_SIZE  (BOOT_LOAD_BUF_PAGES * 4096u)

static struct boot_load_entry s_pool[BOOT_LOAD_MAX];
static atomic_t s_claimed = ATOMIC_INIT(0);

/* ---- slot allocation (SMP-safe, lock-free) ----------------------------- */

/* Claim a unique slot. Returns the index, or -1 when the pool is full (the
 * claim counter still advances so dropped_count() stays accurate). */
static int claim_slot(void)
{
    int32_t idx = atomic_fetch_add(&s_claimed, 1);   /* returns OLD value */
    if (idx < 0 || idx >= (int32_t)BOOT_LOAD_MAX)
        return -1;
    return (int)idx;
}

static uint32_t recorded_count(void)
{
    int32_t c = atomic_read(&s_claimed);
    if (c < 0)
        return 0u;
    return (uint32_t)c < BOOT_LOAD_MAX ? (uint32_t)c : BOOT_LOAD_MAX;
}

static uint32_t dropped_count(void)
{
    int32_t c = atomic_read(&s_claimed);
    if (c <= (int32_t)BOOT_LOAD_MAX)
        return 0u;
    return (uint32_t)c - BOOT_LOAD_MAX;
}

static void copy_name(char *dst, const char *src)
{
    uint32_t i = 0u;
    if (src)
        for (; i < BOOT_LOAD_NAME_MAX - 1u && src[i]; i++)
            dst[i] = src[i];
    dst[i] = '\0';
}

/* ---- public record API ------------------------------------------------- */

int boot_load_begin(const char *name, uint8_t cls)
{
    int idx = claim_slot();
    if (idx < 0)
        return -1;
    struct boot_load_entry *e = &s_pool[idx];
    copy_name(e->name, name);
    e->cls         = cls;
    e->state       = (uint8_t)BOOT_LOAD_ATTEMPTED;
    e->err_code    = 0u;
    e->post_code   = 0u;
    e->start_ms    = boot_get_elapsed_ms();
    e->duration_ms = 0u;
    __atomic_store_n(&e->published, 1u, __ATOMIC_RELEASE);
    return idx;
}

void boot_load_finish(int token, uint8_t state, uint16_t err_code,
                      uint16_t post_code)
{
    if (token < 0 || token >= (int)BOOT_LOAD_MAX)
        return;
    struct boot_load_entry *e = &s_pool[token];
    uint32_t now = boot_get_elapsed_ms();
    e->state       = state;
    e->err_code    = err_code;
    e->post_code   = post_code;
    e->duration_ms = (now >= e->start_ms) ? (now - e->start_ms) : 0u;
    __atomic_store_n(&e->published, 1u, __ATOMIC_RELEASE);
}

void boot_load_record(const char *name, uint8_t cls, uint8_t state,
                      uint16_t err_code, uint16_t post_code)
{
    int idx = claim_slot();
    if (idx < 0)
        return;
    struct boot_load_entry *e = &s_pool[idx];
    copy_name(e->name, name);
    e->cls         = cls;
    e->state       = state;
    e->err_code    = err_code;
    e->post_code   = post_code;
    e->start_ms    = boot_get_elapsed_ms();
    e->duration_ms = 0u;
    __atomic_store_n(&e->published, 1u, __ATOMIC_RELEASE);
}

/* ---- bounded text helpers (no stdio) ----------------------------------- */

static uint32_t put_char(char *buf, uint32_t pos, uint32_t cap, char c)
{
    if (pos + 1u < cap)
        buf[pos++] = c;
    return pos;
}

static uint32_t put_str(char *buf, uint32_t pos, uint32_t cap, const char *s)
{
    if (s)
        while (*s && pos + 1u < cap)
            buf[pos++] = *s++;
    return pos;
}

/* Write s then pad with spaces to at least `width` columns (never truncates). */
static uint32_t put_field(char *buf, uint32_t pos, uint32_t cap, const char *s,
                          uint32_t width)
{
    uint32_t i = 0u;
    if (s)
        for (; s[i] && pos + 1u < cap; i++)
            buf[pos++] = s[i];
    while (i < width) {
        pos = put_char(buf, pos, cap, ' ');
        i++;
    }
    return pos;
}

static uint32_t put_u32(char *buf, uint32_t pos, uint32_t cap, uint32_t v)
{
    char tmp[10];
    int n = 0;
    if (v == 0u)
        return put_char(buf, pos, cap, '0');
    while (v && n < 10) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n-- > 0)
        pos = put_char(buf, pos, cap, tmp[n]);
    return pos;
}

static uint32_t put_hex16(char *buf, uint32_t pos, uint32_t cap, uint16_t v)
{
    static const char hexd[] = "0123456789abcdef";
    pos = put_char(buf, pos, cap, hexd[(v >> 12) & 0xFu]);
    pos = put_char(buf, pos, cap, hexd[(v >> 8) & 0xFu]);
    pos = put_char(buf, pos, cap, hexd[(v >> 4) & 0xFu]);
    pos = put_char(buf, pos, cap, hexd[v & 0xFu]);
    return pos;
}

static const char *state_name(uint8_t s)
{
    switch (s) {
    case BOOT_LOAD_ATTEMPTED: return "ATTEMPTED";
    case BOOT_LOAD_LOADED:    return "LOADED";
    case BOOT_LOAD_SKIPPED:   return "SKIPPED";
    case BOOT_LOAD_FAILED:    return "FAILED";
    case BOOT_LOAD_DEGRADED:  return "DEGRADED";
    default:                  return "?";
    }
}

static const char *class_name(uint8_t c)
{
    switch (c) {
    case BOOT_LOAD_CLASS_CORE:    return "CORE";
    case BOOT_LOAD_CLASS_STORAGE: return "STORAGE";
    case BOOT_LOAD_CLASS_INPUT:   return "INPUT";
    case BOOT_LOAD_CLASS_NET:     return "NET";
    case BOOT_LOAD_CLASS_ACPI:    return "ACPI";
    case BOOT_LOAD_CLASS_GFX:     return "GFX";
    default:                      return "?";
    }
}

static int entry_published(const struct boot_load_entry *e)
{
    return __atomic_load_n(&e->published, __ATOMIC_ACQUIRE) != 0u;
}

static int entry_degraded(const struct boot_load_entry *e)
{
    return e->state == (uint8_t)BOOT_LOAD_FAILED ||
           e->state == (uint8_t)BOOT_LOAD_DEGRADED;
}

/* ---- formatters (pure; unit-testable without VFS) ---------------------- */

uint32_t boot_load_status_format(char *buf, uint32_t cap)
{
    uint32_t pos = 0u, rec, drop, i;
    if (!buf || cap == 0u) {
        if (buf && cap != 0u)
            buf[0] = '\0';
        return 0u;
    }
    rec  = recorded_count();
    drop = dropped_count();

    pos = put_str(buf, pos, cap, "# boot-load-status: ");
    pos = put_u32(buf, pos, cap, rec);
    pos = put_str(buf, pos, cap, " recorded / ");
    pos = put_u32(buf, pos, cap, BOOT_LOAD_MAX);
    pos = put_str(buf, pos, cap, " cap, ");
    pos = put_u32(buf, pos, cap, drop);
    pos = put_str(buf, pos, cap, " dropped");
    if (drop)
        pos = put_str(buf, pos, cap, " TRUNCATED");
    pos = put_char(buf, pos, cap, '\n');

    for (i = 0u; i < rec; i++) {
        struct boot_load_entry *e = &s_pool[i];
        if (!entry_published(e))
            continue;
        pos = put_field(buf, pos, cap, state_name(e->state), 9u);
        pos = put_char(buf, pos, cap, ' ');
        pos = put_field(buf, pos, cap, class_name(e->cls), 8u);
        pos = put_char(buf, pos, cap, ' ');
        pos = put_field(buf, pos, cap, e->name, 16u);
        pos = put_str(buf, pos, cap, " err=0x");
        pos = put_hex16(buf, pos, cap, e->err_code);
        pos = put_str(buf, pos, cap, " post=0x");
        pos = put_hex16(buf, pos, cap, e->post_code);
        pos = put_str(buf, pos, cap, " start=");
        pos = put_u32(buf, pos, cap, e->start_ms);
        pos = put_str(buf, pos, cap, "ms dur=");
        pos = put_u32(buf, pos, cap, e->duration_ms);
        pos = put_str(buf, pos, cap, "ms\n");
    }

    if (pos >= cap)
        pos = cap - 1u;
    buf[pos] = '\0';
    return pos;
}

uint32_t boot_load_status_degraded_summary(char *buf, uint32_t cap)
{
    uint32_t pos = 0u, rec, drop, i, count = 0u, emitted = 0u;
    if (!buf || cap == 0u) {
        if (buf && cap != 0u)
            buf[0] = '\0';
        return 0u;
    }
    rec  = recorded_count();
    drop = dropped_count();

    for (i = 0u; i < rec; i++) {
        struct boot_load_entry *e = &s_pool[i];
        if (entry_published(e) && entry_degraded(e))
            count++;
    }
    if (count == 0u && drop == 0u) {
        buf[0] = '\0';
        return 0u;
    }

    /* Emit the fixed-size overflow marker FIRST (right after the count), then
     * the variable-length name list. A full degraded pool can fill a small
     * report buffer (report_summary uses sum[256]); putting "D dropped" before
     * the names guarantees the overflow signal is never the part truncated. */
    pos = put_u32(buf, pos, cap, count);
    pos = put_str(buf, pos, cap, " degraded");
    if (drop) {
        pos = put_str(buf, pos, cap, ", ");
        pos = put_u32(buf, pos, cap, drop);
        pos = put_str(buf, pos, cap, " dropped");
    }
    if (count) {
        pos = put_str(buf, pos, cap, ": ");
        for (i = 0u; i < rec; i++) {
            struct boot_load_entry *e = &s_pool[i];
            if (!entry_published(e) || !entry_degraded(e))
                continue;
            if (emitted)
                pos = put_str(buf, pos, cap, ", ");
            pos = put_str(buf, pos, cap, e->name);
            pos = put_str(buf, pos, cap, "(0x");
            pos = put_hex16(buf, pos, cap, e->err_code);
            pos = put_char(buf, pos, cap, ')');
            emitted++;
        }
    }
    buf[pos] = '\0';
    return count;
}

/* ---- Phase-3 sinks ----------------------------------------------------- */

void boot_load_status_report_summary(void)
{
    char sum[256];
    (void)boot_load_status_degraded_summary(sum, sizeof sum);
    if (sum[0] == '\0') {
        klog(LOG_INFO, "boot", "[BOOT-LOAD] all subsystems loaded clean");
        return;
    }
    /* %s arg is passed as (uint64_t)(uintptr_t)ptr per the kernel klog ABI. */
    klog(LOG_WARN, "boot", "[BOOT-LOAD] %s", (uint64_t)(uintptr_t)sum);
    boot_splash_diag(sum);
}

void boot_load_status_dump_to_blackbox(void)
{
    extern int klog_using_blackbox;
    if (!klog_using_blackbox)
        return;

    uintptr_t phys = pmm_alloc_contiguous(BOOT_LOAD_BUF_PAGES);
    if (!phys) {
        klog(LOG_WARN, "boot",
             "boot-load-status: cannot alloc %u pages",
             (uint64_t)BOOT_LOAD_BUF_PAGES);
        return;
    }
    char *buf = (char *)phys;
    uint32_t pos = boot_load_status_format(buf, BOOT_LOAD_BUF_SIZE);

    struct vfs_node *f = vfs_open(BOOT_LOAD_PATH,
                                  VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (f) {
        int wrote = vfs_write(f, 0, pos, (const uint8_t *)buf);
        vfs_close(f);
        if (wrote >= 0 && (uint32_t)wrote == pos) {
            klog(LOG_INFO, "boot",
                 "boot load status dumped to " BOOT_LOAD_PATH " (%u bytes)",
                 (uint64_t)pos);
        } else {
            /* Short write left a partial file; VFS_O_TRUNC already zeroed it
             * at open, so re-truncate to empty -- "no data" beats a torn log
             * a consumer would mis-parse. */
            klog(LOG_WARN, "boot",
                 "boot-load-status: short write (wrote=%d of %u); truncating",
                 (uint64_t)wrote, (uint64_t)pos);
            struct vfs_node *t = vfs_open(BOOT_LOAD_PATH,
                                          VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
            if (t)
                vfs_close(t);
        }
    } else {
        klog(LOG_WARN, "boot",
             "boot-load-status: vfs_open failed for " BOOT_LOAD_PATH);
    }

    for (uint32_t p = 0u; p < BOOT_LOAD_BUF_PAGES; p++)
        pmm_free_frame(phys + p * 4096u);
}

/* ---- test seam --------------------------------------------------------- */

void boot_load_status_test_save(struct boot_load_test_state *st)
{
    uint32_t i;
    if (!st)
        return;
    for (i = 0u; i < BOOT_LOAD_MAX; i++)
        st->saved[i] = s_pool[i];
    st->saved_claimed = atomic_read(&s_claimed);
}

void boot_load_status_test_restore(const struct boot_load_test_state *st)
{
    uint32_t i;
    if (!st)
        return;
    for (i = 0u; i < BOOT_LOAD_MAX; i++)
        s_pool[i] = st->saved[i];
    atomic_set(&s_claimed, st->saved_claimed);
}
