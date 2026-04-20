/* ============================================================================
 * klog.c -- Unified kernel logging
 *
 * Outputs formatted messages with level prefix and subsystem tag.
 * LOG_DEBUG goes to serial only; LOG_INFO and above go to both
 * serial and framebuffer.
 *
 * Ring buffer stores the last KLOG_RING_SIZE entries for the debug console.
 * ============================================================================ */

#include "kernel/klog.h"
#include "kernel/drivers/serial.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/timer.h"
#include "kernel/smp.h"
#include "kernel/sched/task.h"
#include "kernel/sched/spinlock.h"
#include "kernel/boot_init.h"


/* GCC built-in variadic args (no libc needed) */
typedef __builtin_va_list va_list;
#define va_start(ap, last)  __builtin_va_start(ap, last)
#define va_end(ap)          __builtin_va_end(ap)
#define va_arg(ap, type)    __builtin_va_arg(ap, type)

/* ---- Configuration ---- */

static log_level_t screen_min_level = LOG_INFO;

/* Per-subsystem verbosity overrides */
#define KLOG_MAX_OVERRIDES 32

typedef struct {
    const char *tag;       /* subsystem string (pointer compare + strcmp) */
    log_level_t min_level; /* entries below this level are dropped */
} klog_level_override_t;

static klog_level_override_t s_overrides[KLOG_MAX_OVERRIDES];
static uint32_t              s_override_count;
static log_level_t           s_global_min = LOG_DEBUG;  /* default: keep all */

/* Rate limiting: per-subsystem message count within a 1-second window */
#define KLOG_RATE_SLOTS     32
#define KLOG_RATE_DEFAULT   100     /* msgs per window */
#define KLOG_RATE_WINDOW    100     /* ticks (100 Hz = 1 second) */

typedef struct {
    const char *tag;
    uint32_t    count;          /* messages this window */
    uint32_t    dropped;        /* dropped this window */
    uint32_t    window_start;   /* tick at window start */
    uint32_t    max_rate;       /* 0 = use default */
} klog_rate_slot_t;

static klog_rate_slot_t s_rate[KLOG_RATE_SLOTS];
static uint32_t         s_rate_count;

/* ---- Ring buffer ---- */

static klog_entry_t klog_ring[KLOG_RING_SIZE];
static uint32_t     klog_ring_head = 0;
static uint32_t     klog_ring_count = 0;
static uint64_t     klog_ring_seq = 0;   /* monotonic sequence -- never wraps */

/* SMP lock: protects ring head/count/seq and rate-limit slot mutations.
 * Uses irqsave because klog() can be called from interrupt context. */
static DEFINE_SPINLOCK(s_klog_lock);

/* ---- Output helpers ---- */

/* Write a string to framebuffer only */
static void fb_str(const char *s)
{
    while (*s)
        fb_putchar(*s++);
}

/* ---- Format engine ---- */

static uint32_t vformat_buf(char *buf, uint32_t bufsize, const char *fmt,
                             va_list ap)
{
    uint32_t pos = 0;

    #define BUF_PUT(c) do { if (pos < bufsize - 1) buf[pos++] = (c); } while(0)

    while (*fmt) {
        if (*fmt != '%') {
            BUF_PUT(*fmt++);
            continue;
        }
        fmt++; /* skip '%' */
        if (*fmt == '\0') break;
        if (*fmt == '%') { BUF_PUT('%'); fmt++; continue; }

        /* Parse zero-pad flag */
        int zero_pad = 0;
        if (*fmt == '0') { zero_pad = 1; fmt++; }

        /* Parse width */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* Parse length modifier: skip l, ll, h, hh */
        while (*fmt == 'l' || *fmt == 'h') fmt++;

        /* Specifier */
        switch (*fmt) {
        case 'd': case 'i': {
            int64_t v = va_arg(ap, int64_t);
            char tmp[20]; int n = 0;
            if (v < 0) { BUF_PUT('-'); v = -v; }
            if (v == 0) { tmp[n++] = '0'; }
            else { while (v > 0) { tmp[n++] = '0' + (char)(v % 10); v /= 10; } }
            while (n < width) { BUF_PUT(zero_pad ? '0' : ' '); width--; }
            while (n > 0) BUF_PUT(tmp[--n]);
            break;
        }
        case 'u': {
            uint64_t v = va_arg(ap, uint64_t);
            char tmp[20]; int n = 0;
            if (v == 0) { tmp[n++] = '0'; }
            else { while (v > 0) { tmp[n++] = '0' + (char)(v % 10); v /= 10; } }
            while (n < width) { BUF_PUT(zero_pad ? '0' : ' '); width--; }
            while (n > 0) BUF_PUT(tmp[--n]);
            break;
        }
        case 'x': case 'X': {
            const char *hex = (*fmt == 'X') ? "0123456789ABCDEF"
                                            : "0123456789abcdef";
            uint64_t v = va_arg(ap, uint64_t);
            char tmp[16]; int n = 0;
            if (v == 0) { tmp[n++] = '0'; }
            else { while (v > 0) { tmp[n++] = hex[v & 0xF]; v >>= 4; } }
            while (n < width) { BUF_PUT(zero_pad ? '0' : ' '); width--; }
            while (n > 0) BUF_PUT(tmp[--n]);
            break;
        }
        case 'p': {
            const char hex[] = "0123456789abcdef";
            uint64_t v = va_arg(ap, uint64_t);
            BUF_PUT('0'); BUF_PUT('x');
            for (int sh = 60; sh >= 0; sh -= 4)
                BUF_PUT(hex[(v >> sh) & 0xF]);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s) BUF_PUT(*s++);
            break;
        }
        case 'c':
            BUF_PUT((char)va_arg(ap, int));
            break;
        case '\0':
            goto done;
        default:
            BUF_PUT('%');
            BUF_PUT(*fmt);
            break;
        }
        fmt++;
    }
done:
    buf[pos] = '\0';
    #undef BUF_PUT
    return pos;
}


/* ---- Level prefixes, ANSI serial colors, and framebuffer colors ---- */

static const char *level_prefix[] = {
    "[INFO] ",   /* LOG_DEBUG */
    "[ OK ] ",   /* LOG_INFO  */
    "[WARN] ",   /* LOG_WARN  */
    "[FAIL] ",   /* LOG_ERROR */
    "[CRIT] ",   /* LOG_FATAL */
};

/* ANSI escape sequences applied to the serial output.
 *
 * Two styles depending on severity:
 *   Badge-only  (LOG_DEBUG, LOG_INFO):  color wraps just [LEVEL], rest default
 *   Full-line   (LOG_WARN and above):   color starts at [LEVEL] and extends
 *                                       through subsystem + message to EOL
 *
 * This matches dmesg/journalctl behavior: low-priority lines don't distract,
 * warnings/errors make the entire line stand out instantly in a wall of text.
 *
 *   LOG_DEBUG  [INFO]  dark-grey, badge only  -- background chatter
 *   LOG_INFO   [ OK ]  green,     badge only  -- happy-path confirmation
 *   LOG_WARN   [WARN]  yellow,    full line   -- degraded / non-fatal
 *   LOG_ERROR  [FAIL]  red,       full line   -- recoverable error
 *   LOG_FATAL  [CRIT]  bold+red,  full line   -- fatal halt
 */
#define ANSI_RESET    "\033[0m"
#define ANSI_DGREY    "\033[90m"
#define ANSI_GREEN    "\033[32m"
#define ANSI_YELLOW   "\033[33m"
#define ANSI_RED      "\033[31m"
#define ANSI_BOLD_RED "\033[1;31m"
#define ANSI_CYAN     "\033[36m"
#define ANSI_MAGENTA  "\033[35m"

static const char *level_ansi[] = {
    ANSI_DGREY,     /* LOG_DEBUG  [INFO] */
    ANSI_GREEN,     /* LOG_INFO   [ OK ] */
    ANSI_YELLOW,    /* LOG_WARN   [WARN] */
    ANSI_RED,       /* LOG_ERROR  [FAIL] */
    ANSI_BOLD_RED,  /* LOG_FATAL  [CRIT] */
};

/* 0 = badge only, 1 = color extends through subsystem + message */
static const int level_full_line[] = {
    0,  /* LOG_DEBUG */
    0,  /* LOG_INFO  */
    1,  /* LOG_WARN  */
    1,  /* LOG_ERROR */
    1,  /* LOG_FATAL */
};

static const uint32_t level_color[] = {
    FB_COLOR_FG_DEFAULT,  /* LOG_DEBUG -- shouldn't reach FB */
    FB_COLOR_GREEN,       /* LOG_INFO  */
    FB_COLOR_YELLOW,      /* LOG_WARN  */
    FB_COLOR_RED,         /* LOG_ERROR */
    FB_COLOR_RED,         /* LOG_FATAL */
};

/* ---- Split init ---- */

void klog_early_init(void)
{
    /* Ring buffer and serial output are static -- nothing to allocate.
     * This function exists to formalize the Phase 0 init contract. */
    klog_ring_head  = 0;
    klog_ring_count = 0;
    klog_ring_seq   = 0;
}

void klog_disk_enable(void)
{
    /* Phase 2 disk init: allocate FAT32 buffer and open log files.
     * Triggers the first flush of accumulated ring entries to disk. */
    klog_disk_init();
    klog_crash_write_to_disk();
    klog_disk_flush();
}

/* ---- Crash-persistent log capture ---------------------------------------- */

#include "kernel/boot_init.h"
#include "kernel/mm/pmm.h"

/* Serialized entry: no pointers, fixed-size for physical memory layout */
typedef struct {
    uint32_t level;
    uint32_t timestamp;
    uint8_t  cpu_id;
    uint8_t  _pad[3];
    uint32_t pid;
    uint32_t tid;
    char     subsystem[16];
    char     message[128];
} klog_crash_entry_t;  /* 160 bytes */

/* Reserved physical memory region for crash log persistence */
static uint8_t *s_crash_region;       /* phys addr, identity-mapped */
static uint32_t s_crash_region_size;  /* KLOG_CRASH_PAGES * 4096 */

/* Recovered entries from previous crash (held in static buffer until disk write) */
static klog_crash_entry_t s_recovered[KLOG_RING_SIZE];
static uint32_t           s_recovered_count;

/* Simple inline CRC32 (IEEE 802.3, polynomial 0xEDB88320) */
static uint32_t crash_crc32(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;
    uint32_t i, j;
    for (i = 0; i < len; i++) {
        crc ^= p[i];
        for (j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
    }
    return crc ^ 0xFFFFFFFF;
}

static void str_copy_n(char *dst, const char *src, uint32_t max)
{
    uint32_t i = 0;
    if (src) {
        while (i + 1 < max && src[i]) { dst[i] = src[i]; i++; }
    }
    dst[i] = '\0';
}

void klog_crash_persist(void)
{
    /* Called from panic_screen() -- no kmalloc, no VFS, no locks.
     * Direct physical memory write to pre-reserved region. */
    if (!s_crash_region || s_crash_region_size == 0)
        return;

    klog_crash_header_t *hdr = (klog_crash_header_t *)s_crash_region;
    klog_crash_entry_t  *dst = (klog_crash_entry_t *)(s_crash_region + sizeof(klog_crash_header_t));

    /* How many entries fit after the header? */
    uint32_t max_entries = (s_crash_region_size - sizeof(klog_crash_header_t)) /
                           sizeof(klog_crash_entry_t);
    uint32_t count = klog_ring_count < KLOG_RING_SIZE ? klog_ring_count : KLOG_RING_SIZE;
    if (count > max_entries)
        count = max_entries;

    /* Serialize ring entries (oldest first) */
    uint32_t start = 0;
    if (klog_ring_count >= KLOG_RING_SIZE)
        start = klog_ring_head;  /* ring wrapped -- oldest is at head */

    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = (start + i) % KLOG_RING_SIZE;
        dst[i].level     = (uint32_t)klog_ring[idx].level;
        dst[i].timestamp = klog_ring[idx].timestamp;
        dst[i].cpu_id    = klog_ring[idx].cpu_id;
        dst[i]._pad[0]   = 0;
        dst[i]._pad[1]   = 0;
        dst[i]._pad[2]   = 0;
        dst[i].pid       = klog_ring[idx].pid;
        dst[i].tid       = klog_ring[idx].tid;
        str_copy_n(dst[i].subsystem, klog_ring[idx].subsystem, 16);
        str_copy_n(dst[i].message, klog_ring[idx].message, 128);
    }

    /* Write header */
    hdr->magic          = KLOG_CRASH_MAGIC;
    hdr->entry_count    = count;
    hdr->ring_head      = klog_ring_head;
    hdr->boot_timestamp = system_get_ticks();
    hdr->crc32          = 0;  /* zero before computing */

    /* CRC32 over all serialized entries */
    hdr->crc32 = crash_crc32(dst, count * sizeof(klog_crash_entry_t));

    /* POST code: crash log persisted */
    POST16(POST16_CRASHLOG);
}

/* UEFI NVRAM variable for crash region address -- same GUID as ImpossiblePOST */
#include "kernel/uefi_runtime.h"

static const struct boot_uefi_guid s_crash_guid = {
    0x494D504F, 0x5354, 0x4F53,
    { 0x50, 0x4F, 0x53, 0x54, 0x47, 0x55, 0x49, 0x44 }
};
static const uint16_t s_crash_varname[] = {
    'I','m','p','o','s','s','i','b','l','e',
    'C','r','a','s','h','L','o','g', 0
};

#define CRASH_NVRAM_ATTRS (EFI_VARIABLE_NON_VOLATILE | \
                           EFI_VARIABLE_BOOTSERVICE_ACCESS | \
                           EFI_VARIABLE_RUNTIME_ACCESS)

void klog_crash_recover(void)
{
    s_recovered_count = 0;

    POST16(POST16_CRASHLOG);

    /* Step 1: Read previous crash region address from NVRAM */
    uint64_t prev_phys = 0;
    {
        uint64_t sz = sizeof(prev_phys);
        uint32_t attrs = 0;
        uint64_t status = uefi_get_variable(&s_crash_guid, s_crash_varname,
                                            &attrs, &sz, &prev_phys);
        if (status == 0 && sz == sizeof(prev_phys) && prev_phys != 0) {
            /* Delete NVRAM variable immediately to prevent stale-address
             * crash loops if the stored physical address now overlaps a
             * guard page or unmapped region (memory layout shifts between
             * builds as BSS grows). */
            uefi_set_variable(&s_crash_guid, s_crash_varname,
                              CRASH_NVRAM_ATTRS, 0, (const void *)0);

            /* Sanity check: crash region from pmm_alloc_contiguous is
             * always page-aligned and above the kernel+heap+user area.
             * Anything below USER_ELF_END (0x900000) overlaps the
             * kernel image, heap, guard pages, or user ELF range --
             * stale from a previous build with different memory layout. */
            if ((prev_phys & 0xFFF) != 0 || prev_phys < 0x900000) {
                klog(LOG_WARN, "boot",
                     "crash recovery: stale NVRAM address %p -- skipped",
                     prev_phys);
                goto crash_alloc;
            }

            /* Check for crash data at the previous region */
            POST16(POST16_CRASHLOG_CHECK);
            klog_crash_header_t *prev_hdr = (klog_crash_header_t *)(uintptr_t)prev_phys;

            if (prev_hdr->magic == KLOG_CRASH_MAGIC) {
                uint32_t count = prev_hdr->entry_count;
                if (count > 0 && count <= KLOG_RING_SIZE) {
                    klog_crash_entry_t *src = (klog_crash_entry_t *)
                        ((uint8_t *)(uintptr_t)prev_phys + sizeof(klog_crash_header_t));
                    uint32_t expected_crc = prev_hdr->crc32;
                    uint32_t actual_crc = crash_crc32(src, count * sizeof(klog_crash_entry_t));

                    if (actual_crc == expected_crc) {
                        /* Valid crash data -- replay to serial */
                        serial_write("[CRASH-PREV] === Recovered ");
                        {
                            char num[12]; uint32_t n = count, pos = 0;
                            if (n == 0) { num[pos++] = '0'; }
                            else { char tmp[12]; uint32_t t = 0;
                                   while (n) { tmp[t++] = '0' + (n % 10); n /= 10; }
                                   while (t) num[pos++] = tmp[--t]; }
                            num[pos] = '\0';
                            serial_write(num);
                        }
                        serial_write(" entries from previous crash ===\n");

                        for (uint32_t i = 0; i < count; i++) {
                            serial_write("[CRASH-PREV] ");
                            serial_write(src[i].subsystem);
                            serial_write(": ");
                            serial_write(src[i].message);
                            serial_write("\n");
                            if (s_recovered_count < KLOG_RING_SIZE)
                                s_recovered[s_recovered_count++] = src[i];
                        }
                    } else {
                        serial_write("[CRASH-PREV] recovery failed: CRC32 mismatch\n");
                    }
                }
                /* Clear magic so it doesn't replay again */
                prev_hdr->magic = 0;
            }
        }
    }

crash_alloc:
    ;  /* C11 requires a statement after a label */
    /* Step 2: Allocate fresh crash persistence region for THIS boot */
    uint64_t phys = pmm_alloc_contiguous(KLOG_CRASH_PAGES);
    if (!phys) {
        serial_write("[CRASH] Failed to allocate crash log region\n");
        POST16(POST16_CRASHLOG_ALLOC);
        POST16(POST16_CRASHLOG_DONE);
        return;
    }
    s_crash_region = (uint8_t *)(uintptr_t)phys;
    s_crash_region_size = KLOG_CRASH_PAGES * 4096;

    /* Zero the new region */
    for (uint32_t i = 0; i < s_crash_region_size; i++)
        s_crash_region[i] = 0;

    /* Save this region's address to NVRAM so next boot can find it */
    uefi_set_variable(&s_crash_guid, s_crash_varname,
                      CRASH_NVRAM_ATTRS, sizeof(phys), &phys);

    POST16(POST16_CRASHLOG_ALLOC);
    POST16(POST16_CRASHLOG_DONE);
}

void klog_crash_write_to_disk(void)
{
    extern struct vfs_node *vfs_open(const char *, uint32_t);
    extern int32_t vfs_write(struct vfs_node *, uint64_t, uint32_t, const uint8_t *);
    extern void vfs_close(struct vfs_node *);
    extern int vfs_is_mounted(char drive);

    if (s_recovered_count == 0)
        return;

    if (!vfs_is_mounted('X') && !vfs_is_mounted('C')) {
        klog(LOG_WARN, "CRASH", "Cannot write crash_recovery.log -- no writable volume");
        return;
    }

    #define VFS_O_WRITE  0x02
    #define VFS_O_CREATE 0x04
    #define VFS_O_TRUNC  0x08

    /* Write to X:\Crash\ (BlackBox) or C:\Impossible\System\Logs\ (fallback) */
    const char *cr_dir = klog_using_blackbox ? "X:\\Crash\\" : klog_dir;
    char cr_path[64];
    {
        int cp = 0, cj;
        for (cj = 0; cr_dir[cj]; cj++) cr_path[cp++] = cr_dir[cj];
        const char *fn = "crash_recovery.log";
        for (cj = 0; fn[cj]; cj++) cr_path[cp++] = fn[cj];
        cr_path[cp] = '\0';
    }
    struct vfs_node *file = vfs_open(cr_path,
        VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (!file) {
        klog(LOG_WARN, "CRASH", "Cannot create crash_recovery.log");
        return;
    }

    /* Write header */
    static const char file_hdr[] = "# Impossible OS Crash Recovery Log\n"
                                    "# Entries recovered from previous boot crash\n\n";
    uint32_t offset = 0;
    vfs_write(file, offset, sizeof(file_hdr) - 1, (const uint8_t *)file_hdr);
    offset += sizeof(file_hdr) - 1;

    /* Write each recovered entry */
    for (uint32_t i = 0; i < s_recovered_count; i++) {
        char line[192];
        uint32_t pos = 0;

        /* [timestamp] LEVEL subsystem: message\n */
        line[pos++] = '[';
        /* Simple decimal for timestamp */
        {
            uint32_t ts = s_recovered[i].timestamp;
            char tmp[12]; uint32_t t = 0;
            if (ts == 0) { tmp[t++] = '0'; }
            else { while (ts) { tmp[t++] = '0' + (ts % 10); ts /= 10; } }
            while (t) line[pos++] = tmp[--t];
        }
        line[pos++] = ']'; line[pos++] = ' ';

        /* Level */
        {
            static const char *lvl_names[] = { "DEBUG", "INFO", "WARN", "ERROR", "FATAL" };
            uint32_t lv = s_recovered[i].level;
            if (lv > 4) lv = 4;
            const char *ln = lvl_names[lv];
            while (*ln) line[pos++] = *ln++;
        }
        line[pos++] = ' ';

        /* Subsystem */
        {
            const char *s = s_recovered[i].subsystem;
            while (*s && pos < 180) line[pos++] = *s++;
        }
        line[pos++] = ':'; line[pos++] = ' ';

        /* Message */
        {
            const char *m = s_recovered[i].message;
            while (*m && pos < 190) line[pos++] = *m++;
        }
        line[pos++] = '\n';

        vfs_write(file, offset, pos, (const uint8_t *)line);
        offset += pos;
    }

    vfs_close(file);
    klog(LOG_INFO, "CRASH", "Crash recovery log: %u entries written to %scrash_recovery.log",
         (uint64_t)s_recovered_count, cr_dir);

    /* Clear recovered buffer */
    s_recovered_count = 0;

    #undef VFS_O_WRITE
    #undef VFS_O_CREATE
    #undef VFS_O_TRUNC
}

/* ---- Per-subsystem verbosity ---- */

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static log_level_t subsys_min_level(const char *subsystem)
{
    uint32_t i;
    if (!subsystem || !subsystem[0])
        return s_global_min;
    for (i = 0; i < s_override_count; i++) {
        if (s_overrides[i].tag == subsystem)  /* fast pointer compare */
            return s_overrides[i].min_level;
        if (str_eq(s_overrides[i].tag, subsystem))
            return s_overrides[i].min_level;
    }
    return s_global_min;
}

/* ---- Rate limiting ---- */

/* Find or create a rate slot for a subsystem. Returns NULL if table full. */
static klog_rate_slot_t *rate_slot(const char *subsystem)
{
    uint32_t i;
    uint32_t now = (uint32_t)system_get_ticks();

    if (!subsystem || !subsystem[0])
        return (klog_rate_slot_t *)0;

    for (i = 0; i < s_rate_count; i++) {
        if (s_rate[i].tag == subsystem || str_eq(s_rate[i].tag, subsystem)) {
            /* Reset window if expired */
            if (now - s_rate[i].window_start >= KLOG_RATE_WINDOW) {
                /* Emit summary for dropped messages before resetting */
                if (s_rate[i].dropped > 0) {
                    s_rate[i].dropped = 0;  /* clear before recursive klog */
                }
                s_rate[i].count = 0;
                s_rate[i].window_start = now;
            }
            return &s_rate[i];
        }
    }

    /* New slot */
    if (s_rate_count < KLOG_RATE_SLOTS) {
        klog_rate_slot_t *s = &s_rate[s_rate_count++];
        s->tag = subsystem;
        s->count = 0;
        s->dropped = 0;
        s->window_start = now;
        s->max_rate = 0;
        return s;
    }
    return (klog_rate_slot_t *)0;  /* table full, no limiting */
}

/* Check rate limit. Returns 1 if the message should be emitted, 0 if dropped.
 * When dropping transitions happen, emits a summary line. */
static int rate_check(const char *subsystem)
{
    klog_rate_slot_t *sl = rate_slot(subsystem);
    uint32_t limit;

    if (!sl) return 1;  /* no slot = no limiting */

    limit = sl->max_rate ? sl->max_rate : KLOG_RATE_DEFAULT;
    sl->count++;

    if (sl->count <= limit)
        return 1;  /* within limit */

    sl->dropped++;
    return 0;  /* drop */
}

void klog_set_level(const char *subsystem, log_level_t min_level)
{
    uint32_t i;

    /* NULL or "" sets the global default */
    if (!subsystem || !subsystem[0]) {
        s_global_min = min_level;
        return;
    }

    /* Update existing override */
    for (i = 0; i < s_override_count; i++) {
        if (str_eq(s_overrides[i].tag, subsystem)) {
            s_overrides[i].min_level = min_level;
            return;
        }
    }

    /* Add new override */
    if (s_override_count < KLOG_MAX_OVERRIDES) {
        s_overrides[s_override_count].tag = subsystem;
        s_overrides[s_override_count].min_level = min_level;
        s_override_count++;
    }
}

log_level_t klog_get_level(const char *subsystem)
{
    uint32_t i;

    if (!subsystem || !subsystem[0])
        return s_global_min;

    /* Return existing override if one is active. */
    for (i = 0; i < s_override_count; i++) {
        if (str_eq(s_overrides[i].tag, subsystem))
            return s_overrides[i].min_level;
    }

    /* No per-subsystem override -- fall back to the global default. */
    return s_global_min;
}

int klog_has_override(const char *subsystem)
{
    uint32_t i;

    if (!subsystem || !subsystem[0])
        return 0;
    for (i = 0; i < s_override_count; i++) {
        if (str_eq(s_overrides[i].tag, subsystem))
            return 1;
    }
    return 0;
}

void klog_remove_override(const char *subsystem)
{
    uint32_t i;

    if (!subsystem || !subsystem[0])
        return;
    for (i = 0; i < s_override_count; i++) {
        if (str_eq(s_overrides[i].tag, subsystem)) {
            /* Swap-with-last + decrement (NOT shift).
             *
             * The klog() hot path walks s_overrides without locking
             * on the assumption the table is append-only plus
             * in-place level edits. A mid-walk SHIFT would let a
             * reader observe slot i with tag A's value then slot i+1
             * with tag A's (shifted) value, effectively returning
             * the wrong min_level for the tag the reader is looking
             * up. Swap-with-last exchanges ONE struct (slot i <- last
             * slot) and then decrements the count. A concurrent
             * reader sees either the original tag at slot i or the
             * swapped-in tag -- both are VALID entries that pass the
             * str_eq compare correctly for their own lookups; a
             * reader looking up the removed tag either finds it at
             * slot i (old pre-swap view) and returns its level, or
             * does not find it and falls back to global default.
             *
             * The residual race window (reader reads old count then
             * misses the swapped-in tag at slot count-1) is bounded
             * and benign: at worst the reader sees stale data for
             * one entry in one concurrent emission, matching the
             * existing pre-lock contract of klog_set_level.
             *
             * True removal with concurrent-reader correctness would
             * need a shared spinlock covering klog()'s filter path,
             * which is a klog-wide refactor outside §5 scope. */
            s_override_count--;
            if (i != s_override_count)
                s_overrides[i] = s_overrides[s_override_count];
            return;
        }
    }
}

void klog_load_levels_from_registry(void)
{
    /* Known subsystem tags to check in registry */
    static const char *tags[] = {
        "net", "boot", "fs", "mm", "drv", "sec", "ahci", "pci",
        "lapic", "acpi", "smp", "UEFI", "TPM", "vfs", "ixfs", "fat32"
    };
    uint32_t i;
    uint32_t tag_count = sizeof(tags) / sizeof(tags[0]);

    /* Use RegReadKeyValue to check HKLM\SYSTEM\Logs\Levels\<tag> */
    extern long RegReadKeyValue(void *hRootKey, const char *lpPath,
                                const char *lpValueName, uint32_t *lpType,
                                uint8_t *lpData, uint32_t *lpcbData);

    for (i = 0; i < tag_count; i++) {
        char val[16];
        uint32_t val_type = 0;
        uint32_t val_size = sizeof(val);
        long rc;

        rc = RegReadKeyValue((void *)(uintptr_t)0x80000002,  /* HKEY_LOCAL_MACHINE */
                             "SYSTEM\\Logs\\Levels",
                             tags[i], &val_type, (uint8_t *)val, &val_size);
        if (rc == 0 && val_type == 1 && val_size > 0) {  /* REG_SZ = 1 */
            val[val_size < sizeof(val) ? val_size : sizeof(val) - 1] = '\0';
            log_level_t lvl = LOG_DEBUG;
            if (str_eq(val, "INFO"))       lvl = LOG_INFO;
            else if (str_eq(val, "WARN"))  lvl = LOG_WARN;
            else if (str_eq(val, "ERROR")) lvl = LOG_ERROR;
            else if (str_eq(val, "FATAL")) lvl = LOG_FATAL;
            klog_set_level(tags[i], lvl);
        }
    }

    /* Also load per-subsystem rate limits from HKLM\SYSTEM\Logs\RateLimit\<tag> */
    for (i = 0; i < tag_count; i++) {
        uint32_t val = 0, val_type = 0, val_size = sizeof(val);
        if (RegReadKeyValue((void *)(uintptr_t)0x80000002,
                            "SYSTEM\\Logs\\RateLimit",
                            tags[i], &val_type, (uint8_t *)&val, &val_size) == 0 &&
            val_type == 4 && val > 0) {  /* REG_DWORD */
            klog_rate_slot_t *sl = rate_slot(tags[i]);
            if (sl) sl->max_rate = val;
        }
    }
}

/* ---- Public API ---- */

uint32_t klog_get_dropped(const char *subsystem)
{
    uint32_t i;
    if (!subsystem || !subsystem[0]) return 0;
    for (i = 0; i < s_rate_count; i++) {
        if (s_rate[i].tag == subsystem || str_eq(s_rate[i].tag, subsystem))
            return s_rate[i].dropped;
    }
    return 0;
}

void klog_set_screen_level(log_level_t min_level)
{
    screen_min_level = min_level;
}

const klog_entry_t *klog_get_ring(uint32_t *out_count, uint32_t *out_head)
{
    if (out_count) *out_count = klog_ring_count;
    if (out_head)  *out_head  = klog_ring_head;
    return klog_ring;
}

uint64_t klog_get_seq(void)
{
    return klog_ring_seq;
}

void klog(log_level_t level, const char *subsystem, const char *fmt, ...)
{
    va_list ap;
    klog_entry_t snapshot;   /* local copy for output outside the lock */
    uint64_t irq_flags;
    int rate_dropped = 0;
    int first_drop = 0;

    /* Per-subsystem verbosity filter: drop entries below threshold.
     * Read-only check on s_overrides -- safe without lock (overrides are
     * append-only and only modified during single-threaded boot or with
     * explicit klog_set_level calls). */
    if (level < subsys_min_level(subsystem))
        return;

    /* ---- Lock: protect ring buffer + rate-limit mutations ---- */
    spin_lock_irqsave(&s_klog_lock, &irq_flags);

    /* Per-subsystem rate limit: drop if over budget this window */
    if (!rate_check(subsystem)) {
        klog_rate_slot_t *sl = rate_slot(subsystem);
        if (sl && sl->dropped == 1)
            first_drop = 1;
        rate_dropped = 1;
    }

    if (rate_dropped) {
        spin_unlock_irqrestore(&s_klog_lock, irq_flags);
        if (first_drop) {
            /* Emit summary outside lock -- NULL subsystem bypasses rate check */
            klog(LOG_WARN, (const char *)0,
                 "[%s] rate limit active (>%u msgs/sec)",
                 subsystem ? subsystem : "???",
                 KLOG_RATE_DEFAULT);
        }
        return;
    }

    /* ---- Store formatted message in ring buffer ---- */
    {
        klog_entry_t *e = &klog_ring[klog_ring_head];
        e->level     = level;
        e->subsystem = subsystem;
        e->timestamp = (uint32_t)system_get_ticks();

        /* Per-entry context: CPU, PID, TID */
        {
            struct per_cpu_data *cpu = smp_this_cpu();
            e->cpu_id = cpu ? cpu->cpu_id : 0;
        }
        if (kernel_subsystem_ready(SUBSYS_SCHED)) {
            struct task *t = task_current();
            if (t) {
                e->pid = t->pid;
                e->tid = 0;
            } else {
                e->pid = 0;
                e->tid = 0;
            }
        } else {
            e->pid = 0;
            e->tid = 0;
        }

        va_start(ap, fmt);
        vformat_buf(e->message, sizeof(e->message), fmt, ap);
        va_end(ap);

        /* Snapshot for output outside the lock */
        snapshot = *e;

        klog_ring_head = (klog_ring_head + 1) % KLOG_RING_SIZE;
        if (klog_ring_count < KLOG_RING_SIZE)
            klog_ring_count++;
        klog_ring_seq++;
    }

    spin_unlock_irqrestore(&s_klog_lock, irq_flags);

    /* ---- Build complete serial line in a stack buffer, then write atomically ----
     *
     * Uses snapshot (local copy) so we don't hold the ring lock during I/O.
     * serial_write() holds its own spinlock for the entire string. */
    {
        /* Line format: "[  X.XXX] [LEVEL] subsystem: message\n"
         * Max size: 11 (ts) + 7 (level) + 16 (subsys+": ") + 256 (msg) + ANSI ~40 = 330 */
        char line[512];
        uint32_t pos = 0;

        #define LP(c) do { if (pos < sizeof(line)-1) line[pos++] = (c); } while(0)
        #define LS(s) do { const char *_p = (s); while (*_p && pos < sizeof(line)-1) line[pos++] = *_p++; } while(0)

        /* Timestamp: [  X.XXX] */
        uint64_t ms  = (uint64_t)snapshot.timestamp * 10;
        uint32_t sec = (uint32_t)(ms / 1000);
        uint32_t fms = (uint32_t)(ms % 1000);

        LP('[');
        {
            char tmp[8]; int n = 0;
            uint32_t v = sec;
            if (v == 0) { tmp[n++] = '0'; }
            else { while (v > 0) { tmp[n++] = '0' + (char)(v % 10); v /= 10; } }
            int pad = 3 - n;
            while (pad-- > 0) LP(' ');
            while (n > 0) LP(tmp[--n]);
        }
        LP('.');
        LP('0' + (char)((fms / 100) % 10));
        LP('0' + (char)((fms /  10) % 10));
        LP('0' + (char)( fms        % 10));
        LP(']'); LP(' ');

        /* CPU tag: [cpu:N] after timestamp on SMP (skip during single-CPU early boot) */
        if (snapshot.cpu_id > 0 || kernel_subsystem_ready(SUBSYS_SMP)) {
            LP('['); LP('c'); LP('p'); LP('u'); LP(':');
            if (snapshot.cpu_id >= 10)
                LP('0' + (char)((snapshot.cpu_id / 10) % 10));
            LP('0' + (char)(snapshot.cpu_id % 10));
            LP(']'); LP(' ');
        }

        /* Colored level prefix + subsystem + pre-formatted message.
         * For badge-only levels: reset after [LEVEL], rest is default.
         * For full-line levels:  reset after message, whole tail colored.
         * Special: "TEST" subsystem -- badge keeps level color,
         *          subsystem + message text is cyan.
         * Special: "UTEST" subsystem (user-mode test launcher) --
         *          badge keeps level color, subsystem + message text
         *          is magenta so kernel TEST and user-mode UTEST lines
         *          remain visually distinct in the same boot log. */
        {
            int is_test = (subsystem && subsystem[0] == 'T' &&
                           subsystem[1] == 'E' && subsystem[2] == 'S' &&
                           subsystem[3] == 'T' &&
                           (subsystem[4] == '\0' || subsystem[4] == ':'));
            int is_utest = (subsystem && subsystem[0] == 'U' &&
                            subsystem[1] == 'T' && subsystem[2] == 'E' &&
                            subsystem[3] == 'S' && subsystem[4] == 'T' &&
                            (subsystem[5] == '\0' || subsystem[5] == ':'));

            /* Badge: normal level color */
            LS(level_ansi[level]);
            LS(level_prefix[level]);
            LS(ANSI_RESET);

            /* Subsystem + message:
             *   TEST  -> cyan
             *   UTEST -> magenta
             *   else  -> default (or full-line color for WARN/ERROR/FATAL) */
            if (is_test || is_utest) {
                LS(is_test ? ANSI_CYAN : ANSI_MAGENTA);
                if (subsystem[0]) { LS(subsystem); LS(": "); }
                LS(snapshot.message);
                LS(ANSI_RESET);
            } else {
                if (level_full_line[level]) LS(level_ansi[level]);
                if (subsystem && subsystem[0]) { LS(subsystem); LS(": "); }
                LS(snapshot.message);
                if (level_full_line[level]) LS(ANSI_RESET);
            }
        }
        /* Ensure line always ends with newline + NUL, even if truncated */
        if (pos >= sizeof(line) - 2) {
            pos = sizeof(line) - 2;
            line[pos - 1] = '~';  /* truncation marker */
        }
        LP('\n');
        line[pos] = '\0';

        #undef LP
        #undef LS

        serial_write(line);
    }

    /* ---- Output to framebuffer if level >= screen threshold ---- */
    if (level >= screen_min_level) {
        int fb_is_test = (subsystem && subsystem[0] == 'T' &&
                          subsystem[1] == 'E' && subsystem[2] == 'S' &&
                          subsystem[3] == 'T' &&
                          (subsystem[4] == '\0' || subsystem[4] == ':'));
        int fb_is_utest = (subsystem && subsystem[0] == 'U' &&
                           subsystem[1] == 'T' && subsystem[2] == 'E' &&
                           subsystem[3] == 'S' && subsystem[4] == 'T' &&
                           (subsystem[5] == '\0' || subsystem[5] == ':'));
        uint32_t fb_color = fb_is_test  ? FB_COLOR_CYAN
                          : fb_is_utest ? FB_COLOR_MAGENTA
                          : level_color[level];

        fb_set_color(fb_color, FB_COLOR_BG_DEFAULT);
        fb_str(level_prefix[level]);
        fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);

        if (subsystem && subsystem[0]) {
            fb_str(subsystem);
            fb_str(": ");
        }
        fb_str(snapshot.message);
        fb_putchar('\n');
    }

    /* ---- Live debug log: write to X:\BOOT_NNN.LOG immediately ---- */
    if (klog_disk_live_active()) {
        klog_disk_append(&snapshot);
        klog_disk_flush();
    }

    /* ---- FATAL: halt ---- */
    if (level == LOG_FATAL) {
        serial_write("[**] FATAL -- system halted\r\n");
        for (;;)
            __asm__ volatile ("hlt");
    }
}
