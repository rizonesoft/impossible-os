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
#include "kernel/cache.h"
#include "kernel/fs/vfs.h"
#include "kernel/drivers/serial.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/timer.h"
#include "kernel/time/wall_clock.h"
#include "kernel/smp.h"
#include "kernel/sched/task.h"
#include "kernel/sched/spinlock.h"
#include "kernel/sched/seqlock.h"
#include "kernel/boot_init.h"
#include "kernel/test/test_usermode.h"  /* test_usermode_color_active (no-op when KERNEL_TESTS=off) */


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
/* The override table is read-mostly: writers (klog_set_level / klog_remove_override)
 * run at runtime on a live SMP system (tunables live-consumer, klog_suppress, the
 * boot registry load) while every klog() call reads the table on its verbosity
 * path. A canonical seqlock gives the hot reader a lock-free, internally-consistent
 * {tag, min_level, count} snapshot -- it retries rather than observing a slot
 * mid-add / mid-edit / mid-swap-remove. seqlock_write_lock serializes writers and
 * publishes the odd->even sequence with RELEASE; seqlock_read_retry carries the
 * rmb() that orders the slot loads before the final sequence sample. Writers must
 * NOT klog() while holding the write lock (the reader spins on odd -> deadlock);
 * the table-full warning below is emitted AFTER seqlock_write_unlock.
 * s_override_count is plain data covered by the seqlock (no separate atomic). */
static DEFINE_SEQLOCK(s_override_seq);

/* Rate limiting: per-subsystem message count within a 1-second window */
#define KLOG_RATE_SLOTS     32
#define KLOG_RATE_DEFAULT   100     /* msgs per window */
#define KLOG_RATE_WINDOW_MS 1000    /* monotonic window -- raw ticks would
                                     * shrink the window when the tick
                                     * rate rises (KeSetTimerResolution) */

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
    int truncated = 0;

    /* A zero capacity has no valid answer -- there is not even room for the
     * NUL -- and the bound below is `pos < bufsize - 1` on an unsigned, which
     * would wrap to 0xFFFFFFFF and turn this function into the unbounded write
     * it exists to prevent. Unreachable from the single caller today; refused
     * here because the comment above states the bound as a guarantee, and a
     * guarantee with an unstated precondition is how the next caller gets it
     * wrong. */
    if (!buf || bufsize == 0) return 0;

    /* Truncation-marker contract: the BUF_PUT macro silently drops
     * chars once pos reaches bufsize-1 (reserved for NUL); a long %s
     * that overflows the message buffer would otherwise leave the
     * message silently short because the live klog() line-level
     * truncation marker does not fire when the already-truncated
     * message fits within the line buffer's remaining space.  Track
     * the silent-drop case and append '~' as the final byte before
     * NUL when truncation actually happened. */
    /* The argument is evaluated EXACTLY ONCE, into a local, BEFORE the capacity
     * test -- never inside the store branch.  Every caller below passes an
     * expression whose side effect is what advances the loop (`*s++`,
     * `*fmt++`, `tmp[--n]`), so a form that skipped evaluation on the
     * full-buffer branch left `while (*s) BUF_PUT(*s++);` spinning forever on
     * the first byte that did not fit -- inside `klog_emit`'s ring lock with
     * interrupts disabled, which silences the machine with no fault to report.
     * Any message crossing `klog_entry_t.message[256]` hung the boot that way. */
    #define BUF_PUT(c) do { \
        char _bp_c = (char)(c); \
        if (pos < bufsize - 1) buf[pos++] = _bp_c; \
        else truncated = 1; \
    } while(0)

    /* STOP AT THE FIRST BYTE THAT DOES NOT FIT.
     *
     * Every loop below is guarded on `!truncated`, so total work is bounded by
     * `bufsize` plus the length of the FORMAT string -- never by the length of
     * an argument. Merely fixing the macro's side effect was not enough: the
     * `%s` loop still walked a caller's whole string after the buffer filled,
     * and the pad loop still ran `width` times, so `%20000000u` (a typo, not an
     * attack) spun for tens of millions of iterations. This runs inside
     * `klog_emit`'s ring lock with interrupts disabled, where unbounded work is
     * indistinguishable from the hang this function just stopped causing. */
    while (*fmt && !truncated) {
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

        /* Parse width, accumulated UNSIGNED and clamped to what the buffer
         * could ever hold. Accumulating into an int and clamping afterwards
         * would still overflow on the way there (signed overflow is UB, not a
         * wrap), and no width past `bufsize` can change the output anyway. */
        uint64_t wacc = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            if (wacc <= (uint64_t)bufsize)
                wacc = wacc * 10u + (uint64_t)(*fmt - '0');
            fmt++;
        }
        int width = (wacc > (uint64_t)bufsize) ? (int)bufsize : (int)wacc;

        /* Parse length modifier: skip l, ll, h, hh */
        while (*fmt == 'l' || *fmt == 'h') fmt++;

        /* Specifier */
        switch (*fmt) {
        case 'd': case 'i': {
            int64_t v = va_arg(ap, int64_t);
            /* Magnitude taken UNSIGNED. `-v` is undefined for INT64_MIN (its
             * negation is not representable), and on the ordinary two's
             * complement result v stays negative, the digit loop never runs,
             * and the record renders as a bare "-": a diagnostic that silently
             * loses its value at exactly the boundary worth printing. */
            uint64_t mag = (v < 0) ? (0u - (uint64_t)v) : (uint64_t)v;
            char tmp[20]; int n = 0;
            if (v < 0) BUF_PUT('-');
            if (mag == 0) { tmp[n++] = '0'; }
            else { while (mag > 0) { tmp[n++] = '0' + (char)(mag % 10u); mag /= 10u; } }
            while (n < width && !truncated) { BUF_PUT(zero_pad ? '0' : ' '); width--; }
            while (n > 0 && !truncated) BUF_PUT(tmp[--n]);
            break;
        }
        case 'u': {
            uint64_t v = va_arg(ap, uint64_t);
            char tmp[20]; int n = 0;
            if (v == 0) { tmp[n++] = '0'; }
            else { while (v > 0) { tmp[n++] = '0' + (char)(v % 10); v /= 10; } }
            while (n < width && !truncated) { BUF_PUT(zero_pad ? '0' : ' '); width--; }
            while (n > 0 && !truncated) BUF_PUT(tmp[--n]);
            break;
        }
        case 'x': case 'X': {
            const char *hex = (*fmt == 'X') ? "0123456789ABCDEF"
                                            : "0123456789abcdef";
            uint64_t v = va_arg(ap, uint64_t);
            char tmp[16]; int n = 0;
            if (v == 0) { tmp[n++] = '0'; }
            else { while (v > 0) { tmp[n++] = hex[v & 0xF]; v >>= 4; } }
            while (n < width && !truncated) { BUF_PUT(zero_pad ? '0' : ' '); width--; }
            while (n > 0 && !truncated) BUF_PUT(tmp[--n]);
            break;
        }
        case 'p': {
            const char hex[] = "0123456789abcdef";
            uint64_t v = va_arg(ap, uint64_t);
            BUF_PUT('0'); BUF_PUT('x');
            for (int sh = 60; sh >= 0 && !truncated; sh -= 4)
                BUF_PUT(hex[(v >> sh) & 0xF]);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s && !truncated) BUF_PUT(*s++);
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
    if (truncated && pos >= 1 && pos < bufsize) {
        /* Replace last char with '~' so the marker survives even when
         * pos == bufsize-1. Caller's line-level fallback can still add
         * its own marker, but this one survives the snapshot copy
         * regardless of what the line-level path does. */
        buf[pos - 1] = '~';
    }
    buf[pos] = '\0';
    #undef BUF_PUT
    return pos;
}


/* 1 when `tag` is exactly `name`, optionally followed by ':' (the test runner
 * tags suites as "TEST:sub"). Bounded by BOTH strings: the walk stops at the
 * first mismatch, and a tag shorter than `name` mismatches at its own NUL
 * rather than being indexed past it.
 *
 * The fixed-offset form this replaces read `subsystem[4]` and `subsystem[5]`
 * unconditionally to find the terminator, so EVERY record logged under an
 * ordinary short tag -- "ob", "irq", "idt" -- read one or two bytes past the
 * end of its string literal. It never faulted in practice (literals sit
 * mid-.rodata), which is exactly why it survived: the classification result
 * was still correct, so nothing downstream ever looked wrong. */
static int klog_tag_is(const char *tag, const char *name)
{
    uint32_t i = 0;

    while (name[i]) {
        if (tag[i] != name[i]) return 0;
        i++;
    }
    return tag[i] == '\0' || tag[i] == ':';
}

#ifdef KERNEL_TESTS
/* Test seam -- see the contract in include/kernel/klog.h. Deliberately a thin
 * forwarder: the fixture must exercise the SAME function the renderer calls,
 * not a copy that could drift away from it. */
int klog_probe_tag_is(const char *tag, const char *name)
{
    return klog_tag_is(tag, name);
}
#endif

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
#define ANSI_MAGENTA  "\033[95m"  /* legacy fallback (no longer routed to) */

/* Test-runner palette (24-bit truecolor, terminal-side):
 *   Kernel TEST   = #D2A8FF  light lavender
 *   User UTEST    = #84B2E9  light blue
 *   Desktop DTEST = #C586B5  pink-mauve  (reserved -- DTEST runner
 *                                          ships with the desktop
 *                                          UI test framework) */
#define ANSI_TEST_KERNEL  "\033[38;2;210;168;255m"  /* D2A8FF */
#define ANSI_TEST_USER    "\033[38;2;132;178;233m"  /* 84B2E9 */
#define ANSI_TEST_DESKTOP "\033[38;2;197;134;181m"  /* C586B5 */

/* Framebuffer (24-bit ARGB) mirrors of the above. */
#define FB_TEST_KERNEL  0x00D2A8FFu
#define FB_TEST_USER    0x0084B2E9u
#define FB_TEST_DESKTOP 0x00C586B5u

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
    /* Ring buffer and serial output are static (BSS zero-init), so there is
     * nothing to allocate. This function formalizes the Phase 0 init contract
     * and is the named point SUBSYS_KLOG readiness is registered against.
     *
     * It MUST NOT reset the ring: boot_phase0 emits klog() lines (boot_info /
     * config / UEFI / TPM diagnostics) BEFORE this is called, and those early
     * entries are the highest-value records for an early-boot failure. Zeroing
     * head/count/seq here would silently discard them before the first disk
     * flush and before crash recovery -- the opposite of the contract. */
}

int klog_disk_enable(void)
{
    /* Phase 2 disk init: allocate FAT32 buffer and open log files.
     * Triggers the first flush of accumulated ring entries to disk. Returns 1
     * iff disk logging is actually live (buffer + mounted log target) so the
     * boot path does not report a silent alloc-fail / no-mount as success. */
    klog_disk_init();
    klog_crash_write_to_disk();
    klog_disk_flush();
    return klog_disk_active();
}

/* ---- Crash-persistent log capture ---------------------------------------- */

#include "kernel/boot_init.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/user_range.h"   /* USER_ELF_END (0x900000) crash-region floor */
#include "kernel/kchecksum.h"       /* kcrc32 -- the tree's one table-driven CRC-32 */

/* klog_crash_entry_t and its layout assert live in kernel/klog.h, beside the
 * header type they are serialized next to -- the crash-region format is one
 * contract and describing half of it here left the other half undiscoverable
 * to a caller sizing a region. */

/* Reserved physical memory region for crash log persistence */
static uint8_t *s_crash_region;       /* phys addr, identity-mapped */
static uint32_t s_crash_region_size;  /* KLOG_CRASH_PAGES * 4096 */

/* Recovered entries from a previous crash, held until the disk write.
 *
 * FRAME-BACKED, not static BSS. At KLOG_RECOVERED_MAX * sizeof(klog_crash_entry_t)
 * this is 131036 bytes, which is why it moved: the kernel image ends at a hard
 * ceiling (USER_BASE, enforced by scripts/build.sh) and a static array this
 * size spends 40 pages of that budget permanently.
 *
 * NO LOCK, and the justification belongs here rather than at the free site: both
 * statics are written ONLY by klog_crash_recover (boot_phase0, BSP, single-CPU,
 * interrupts disabled) and klog_crash_write_to_disk (Phase 2, BSP, from
 * klog_disk_enable). Nothing else in the tree touches either. They are
 * phase-confined, not synchronized, and a new reader on another CPU would need
 * a lock added here first.
 *
 * Allocated once, inside klog_crash_recover (the only writer), and deliberately
 * NOT at that function's entry: it is bought only after the previous boot's
 * crash region has been located and validated, because that region is not
 * reserved in this boot's PMM bitmap and allocating over it would erase the
 * evidence recovery exists to read. A boot with no prior crash allocates
 * nothing.
 *
 * Allocation failure is DEGRADED, never fatal. Crash-log recovery is a
 * diagnostic: without the pool the entries are still printed to serial as they
 * are read, s_recovered_count stays 0, and klog_crash_write_to_disk returns
 * early. Halting a bootable kernel because a 41-frame contiguous run was
 * unavailable would trade a working machine for a log file. The crash REGION
 * allocation further down already degrades the same way. */
static klog_crash_entry_t *s_recovered;
static uint32_t            s_recovered_count;
/* Frame bookkeeping for the pool above, kept for exactly ONE purpose: the
 * Phase-0 fallback in klog_crash_recover that gives the run back if this boot's
 * own crash region cannot otherwise be allocated. It is NOT a general teardown
 * handle -- see klog_crash_write_to_disk for why that path must not free. */
static uintptr_t           s_recovered_phys;
static uint64_t            s_recovered_pages;

/* Bounded, NULL-safe read of one recovered crash entry.
 *
 * PURE, and public so the degraded contract can be asserted directly. The pool
 * behind s_recovered is frame-backed and may be ABSENT, so every read has to be
 * able to answer "no entry" instead of indexing a null base. It takes the pool
 * and count as parameters rather than reading the statics because a unit test
 * cannot call klog_crash_recover (live boot infrastructure) to set them up, and
 * a contract that is only reachable through the boot path is a contract nothing
 * asserts.
 *
 * `count` is validated against the pool's real capacity as well as against
 * `index`: the pool holds exactly KLOG_RECOVERED_MAX entries, so a count above that
 * is corrupted bookkeeping and must not be turned into an out-of-bounds read. */
const klog_crash_entry_t *klog_recovered_at(const klog_crash_entry_t *pool,
                                            uint32_t count, uint32_t index)
{
    if (!pool || count > KLOG_RECOVERED_MAX || index >= count)
        return (const klog_crash_entry_t *)0;
    return &pool[index];
}

/* Is the recovered set coherent enough to serialize?
 *
 * PURE, and separate from klog_recovered_at because the CALLER's response to an
 * incoherent set is not "skip this entry" but "write nothing and keep what you
 * have". A count with no pool behind it, or one past the pool's capacity, is
 * corrupted bookkeeping; treating it as an ordinary end-of-loop would report a
 * complete log and then clear the only in-memory copy of the evidence.
 *
 * An EMPTY set (count 0) is coherent. Emptiness is a separate question, and the
 * writer answers it before it reaches this check. */
int klog_recovered_set_ok(const klog_crash_entry_t *pool, uint32_t count)
{
    if (!pool && count != 0)
        return 0;
    if (count > KLOG_RECOVERED_MAX)
        return 0;
    return 1;
}

/* CRC-32 over the serialized crash region: `kcrc32` (kernel/kchecksum.h), the
 * tree's one table-driven IEEE implementation.
 *
 * The bit-at-a-time copy that used to live here ran in PANIC CONTEXT over up to
 * KLOG_RING_SIZE * sizeof(klog_crash_entry_t) bytes -- 164 KB at a full ring,
 * about 53 instructions per byte -- while the machine was already dying. Same
 * reflected polynomial, init and xorout as the retired loop, so a crash region
 * written by an older kernel still passes the check on the next boot. */

static void str_copy_n(char *dst, const char *src, uint32_t max)
{
    uint32_t i = 0;
    if (src) {
        while (i + 1 < max && src[i]) { dst[i] = src[i]; i++; }
    }
    dst[i] = '\0';
}

uint32_t klog_crash_serialize_region(uint8_t *region, uint32_t region_size)
{
    /* Called from panic_screen() via klog_crash_persist() -- no kmalloc, no
     * VFS, no locks. Direct memory write to a pre-reserved region.
     *
     * Takes the region as a PARAMETER rather than reading s_crash_region so
     * the publish sequence below is reachable from a unit test on a
     * caller-owned buffer. The live crash region and the ring globals are
     * untouched by such a call: the ring is only read. */
    if (!region || region_size < sizeof(klog_crash_header_t))
        return 0u;

    klog_crash_header_t *hdr = (klog_crash_header_t *)region;
    klog_crash_entry_t  *dst = (klog_crash_entry_t *)(region + sizeof(klog_crash_header_t));

    /* How many entries fit after the header? */
    uint32_t max_entries = (region_size - sizeof(klog_crash_header_t)) /
                           sizeof(klog_crash_entry_t);
    uint32_t count = klog_ring_count < KLOG_RING_SIZE ? klog_ring_count : KLOG_RING_SIZE;
    if (count > max_entries)
        count = max_entries;

    /* UN-PUBLISH, but deliberately WITHOUT flushing the clear.
     *
     * The magic is what makes klog_crash_recover accept this region, so it
     * must not stand while the entries under it are rewritten -- hence the
     * store. What it must NOT be is DURABLE yet, because this region has no
     * ownership arbitration and the panic owner is same-CPU re-entrant: a
     * nested panic (or an NMI or machine check, which CLI does not block)
     * reaches this function while a complete record from the outer invocation
     * is already published. Pushing the zero to memory here would destroy that
     * record before the replacement exists, and a nested writer that faults or
     * resets before republishing would leave the boot with nothing.
     *
     * Unflushed, the clear still orders correctly in cache, and the body flush
     * below is what makes it durable -- by which point the replacement content
     * is durable with it. Entries that reach memory by ordinary eviction under
     * the OLD magic fail the CRC and are rejected, which is fail-closed.
     *
     * This NARROWS the nested-erasure window; it does not close it. Not
     * issuing a flush is not a guarantee that the line stays dirty: ordinary
     * cache replacement can write the zeroed magic back at any point, and this
     * function then serializes up to the whole region before republishing. A
     * nested panic or reset inside that interval can still find memory holding
     * a zero magic and lose the outer record. Closing it needs a publication
     * protocol that does not overwrite a complete record in place -- two slots
     * with a generation, or real arbitration -- which is a mechanism this
     * region does not have and which is parked in TODO-10 with an owner. */
    hdr->magic = 0u;

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
        str_copy_n(dst[i].subsystem, klog_ring[idx].subsystem,
                   KLOG_SUBSYSTEM_MAX);
        str_copy_n(dst[i].message, klog_ring[idx].message, 128);
    }

    /* Write header (every field except the magic, which publishes below) */
    hdr->entry_count    = count;
    hdr->ring_head      = klog_ring_head;
    /* 10 ms units from the coarse cached interrupt time -- lock-free (no
     * seqlock/clocksource read), required because klog_crash_persist() runs in
     * panic context where a seqlock left odd by the faulting path (or the PIT
     * backend's pit_lock held by the fault) would hang a mono_ns()/uptime_ns()/
     * system_get_ticks() read. Stamps 0 if a crash occurs before any monotonic
     * source is up; that is preferable to deadlocking and losing the dump. */
    hdr->boot_timestamp = KeQueryInterruptTimeCoarse() / 100000ULL;
    hdr->crc32          = 0;  /* zero before computing */

    /* CRC32 over all serialized entries */
    hdr->crc32 = kcrc32(dst, count * sizeof(klog_crash_entry_t));

    /* PUBLISH, in the only order a reset cannot turn into a lie. Everything
     * the magic vouches for goes to memory first, while the region still reads
     * as empty; only then is the magic stored and its own line committed. The
     * two flushes cannot be merged and cannot be swapped: the entries start
     * inside the header's cache line, so one flush of both would commit a
     * valid magic alongside content that later lines have not caught up with.
     * A reset between them costs the log, which is the honest outcome; a reset
     * with the order reversed hands the next boot a CRC-valid header over
     * entries from the previous crash. */
    cache_writeback_range(region,
                          sizeof *hdr + (uint64_t)count * sizeof(klog_crash_entry_t));
    hdr->magic = KLOG_CRASH_MAGIC;
    cache_writeback_range(hdr, sizeof *hdr);

    return count;
}

void klog_crash_persist(void)
{
    if (!s_crash_region || s_crash_region_size == 0)
        return;

    (void)klog_crash_serialize_region(s_crash_region, s_crash_region_size);

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

/* Acquire and zero the recovered-entry pool, or return NULL to degrade.
 *
 * WHY THIS RETRIES. pmm_alloc_contiguous is a FIRST-FIT scan from frame 0 with
 * no cursor (src/kernel/mm/pmm.c), and the previous boot allocated its own
 * 32-frame crash region from the same scan at the same boot stage. This boot
 * never reserves that region, so a single request here comes back with the very
 * run holding it -- not occasionally, but on the ordinary crash-then-reboot
 * path, which would make the overlap guard fire every time and silently retire
 * crash_recovery.log. So on overlap we HOLD the colliding run, ask again (the
 * scan now steps past it), and only then give the first one back UNTOUCHED.
 *
 * The frames are never written before the overlap verdict: pmm_alloc_contiguous
 * only sets bitmap bits, so an allocation that turns out to overlap costs
 * nothing and is handed back with the previous boot's evidence intact.
 *
 * Everything here runs in boot_phase0, single-CPU with interrupts disabled, so
 * the unlocked PMM bitmap has no concurrent mutator. */
static klog_crash_entry_t *klog_recovered_pool_acquire(uint64_t prev_phys,
                                                       uint32_t pool_bytes,
                                                       uintptr_t *out_phys,
                                                       uint64_t *out_pages)
{
    const uint64_t prev_end = prev_phys + (uint64_t)KLOG_CRASH_PAGES * PMM_FRAME_SIZE;
    uintptr_t phys  = 0;
    uint64_t  pages = 0;
    klog_crash_entry_t *pool;

    POST16(POST16_CRASHLOG_POOL);

    pool = (klog_crash_entry_t *)pmm_alloc_pages_hhdm(pool_bytes, &phys, &pages);
    if (pool && phys < prev_end && prev_phys < phys + pages * PMM_FRAME_SIZE) {
        /* Colliding run stays HELD across the second request, which is the only
         * reason first-fit returns something different. */
        uintptr_t held_phys  = phys;
        uint64_t  held_pages = pages;
        klog_crash_entry_t *alt;

        phys = 0; pages = 0;
        alt = (klog_crash_entry_t *)pmm_alloc_pages_hhdm(pool_bytes, &phys, &pages);
        pmm_free_contiguous(held_phys, held_pages);
        pool = alt;

        if (pool && phys < prev_end && prev_phys < phys + pages * PMM_FRAME_SIZE) {
            pmm_free_contiguous(phys, pages);
            pool = (klog_crash_entry_t *)0;
        }
    }

    if (!pool) {
        klog(LOG_WARN, "CRASH",
             "recovered-entry pool unavailable (prev region %p, %u bytes wanted); "
             "serial replay is the record, no crash_recovery.log",
             prev_phys, (uint64_t)pool_bytes);
        return (klog_crash_entry_t *)0;
    }

    {
        uint8_t *p = (uint8_t *)pool;
        for (uint32_t z = 0; z < pool_bytes; z++)
            p[z] = 0;
    }
    if (out_phys)  *out_phys  = phys;
    if (out_pages) *out_pages = pages;
    return pool;
}

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

            /* Sanity check the NVRAM-supplied physical address before
             * dereferencing it: a crash region from pmm_alloc_contiguous is
             * page-aligned, above the kernel+heap+user area (>= USER_ELF_END
             * 0x900000), and the WHOLE [prev_phys, prev_phys + 128 KiB) range
             * must sit inside the boot identity map (< BOOT_INFO_EARLY_MAP_END,
             * no overflow). A stale/crafted value outside that window would fault
             * or hit MMIO/unowned memory in Phase 0 before any header/CRC check. */
            if ((prev_phys & 0xFFF) != 0 || prev_phys < USER_ELF_END ||
                prev_phys > BOOT_INFO_EARLY_MAP_END - (uint64_t)KLOG_CRASH_PAGES * 4096) {
                klog(LOG_WARN, "boot",
                     "crash recovery: stale/out-of-range NVRAM address %p -- skipped",
                     prev_phys);
                goto crash_alloc;
            }

            /* Check for crash data at the previous region */
            POST16(POST16_CRASHLOG_CHECK);
            klog_crash_header_t *prev_hdr = (klog_crash_header_t *)(uintptr_t)prev_phys;

            if (prev_hdr->magic == KLOG_CRASH_MAGIC) {
                uint32_t count = prev_hdr->entry_count;
                /* Bound by the REGION capacity, not just KLOG_RING_SIZE: a
                 * 128 KiB region holds fewer than KLOG_RING_SIZE 164-byte
                 * entries, so a corrupt count in (region_max, KLOG_RING_SIZE]
                 * would make crash_crc32 read past the reserved region. Must
                 * match the persist-side clamp (klog_crash_persist max_entries). */
                uint32_t region_max = (uint32_t)
                    ((KLOG_CRASH_PAGES * 4096u - sizeof(klog_crash_header_t)) /
                     sizeof(klog_crash_entry_t));
                if (count > 0 && count <= KLOG_RING_SIZE && count <= region_max) {
                    klog_crash_entry_t *src = (klog_crash_entry_t *)
                        ((uint8_t *)(uintptr_t)prev_phys + sizeof(klog_crash_header_t));
                    uint32_t expected_crc = prev_hdr->crc32;
                    uint32_t actual_crc = kcrc32(src, count * sizeof(klog_crash_entry_t));

                    if (actual_crc == expected_crc) {
                        /* Back the recovered-entry pool with frames reached
                         * through the HHDM, HERE and not at function entry.
                         *
                         * ORDERING IS THE WHOLE POINT. The previous boot's crash
                         * region is named only by an NVRAM variable and is NOT
                         * reserved in this boot's PMM bitmap, so the allocator
                         * is free to hand back the very run holding it.
                         * Allocating before prev_phys was read would erase the
                         * evidence this function exists to recover. By here
                         * prev_phys is validated, the magic and CRC have passed
                         * and count > 0, so the extent to avoid is known and the
                         * pool is only bought when there is something to put in
                         * it -- a clean boot with no prior crash now allocates
                         * nothing at all. The collision handling, the zeroing
                         * and the degrade policy live in the helper. */
                        if (!s_recovered) {
                            const uint32_t pool_bytes = (uint32_t)
                                ((uint64_t)KLOG_RECOVERED_MAX * sizeof(klog_crash_entry_t));
                            uintptr_t pool_phys  = 0;
                            uint64_t  pool_pages = 0;
                            klog_crash_entry_t *pool =
                                klog_recovered_pool_acquire(prev_phys, pool_bytes,
                                                            &pool_phys, &pool_pages);
                            if (pool) {
                                s_recovered_phys  = pool_phys;
                                s_recovered_pages = pool_pages;
                                s_recovered = pool;
                            }
                        }

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
                            /* COPY FIRST, then force-terminate the COPY. Both
                             * arrays are fixed-width fields out of untrusted
                             * cross-boot memory, and serial_write scans to a
                             * NUL -- an unterminated field walks off the end of
                             * the entry and out of the region, leaking adjacent
                             * memory over serial or faulting. A matching CRC
                             * does not make the bytes safe: it proves the
                             * entries are the ones that were written, not that
                             * they contain a terminator. The evidence restore
                             * path sanitizes the same class of field.
                             *
                             * The termination must NOT be written through
                             * `src`. That points into the PREVIOUS boot's
                             * physical run, named only by an NVRAM variable,
                             * and this boot never reserved it -- recovery runs
                             * after PMM, VMM and heap init, so those pages may
                             * already be allocated to live structures. The
                             * check upstream proves the address is mapped, not
                             * that it is still ours. Sanitize an OWNED copy. */
                            klog_crash_entry_t e = src[i];

                            e.subsystem[sizeof e.subsystem - 1u] = '\0';
                            e.message[sizeof e.message - 1u] = '\0';

                            serial_write("[CRASH-PREV] ");
                            serial_write(e.subsystem);
                            serial_write(": ");
                            serial_write(e.message);
                            serial_write("\n");
                            if (s_recovered && s_recovered_count < KLOG_RECOVERED_MAX)
                                s_recovered[s_recovered_count++] = e;
                        }
                    } else {
                        serial_write("[CRASH-PREV] recovery failed: CRC32 mismatch\n");
                    }
                }
                /* The old header is deliberately NOT cleared here. This boot
                 * does not own that memory: `prev_phys` names the PREVIOUS
                 * boot's run, recovery runs after PMM, VMM and heap init, and
                 * the check above proves only that the address is mapped, not
                 * that it is still unallocated. A store there can land in a
                 * live kernel structure.
                 *
                 * Nothing needs the clear. Replay is already prevented by
                 * deleting the NVRAM variable above, which happens before this
                 * region is read at all, so the next boot has no pointer to
                 * find it with. And if the allocator happens to hand this same
                 * run back as this boot's crash region, the full zeroing of
                 * the new region below clears the magic through a pointer we
                 * do own. */
            }
        }
    }

crash_alloc:
    ;  /* C11 requires a statement after a label */
    /* Step 2: Allocate fresh crash persistence region for THIS boot */
    uint64_t phys = pmm_alloc_contiguous(KLOG_CRASH_PAGES);

    /* THIS BOOT'S CRASH REGION OUTRANKS THE PREVIOUS BOOT'S DISK COPY.
     *
     * The recovered-entry pool above is 41 contiguous frames and is taken
     * BEFORE this 32-frame allocation. Under fragmentation it can occupy the
     * last run large enough, and then this fails and the machine boots with no
     * crash logging armed at all -- a regression the old static buffer could
     * not cause, because it consumed no PMM run. Re-arming persistence for the
     * crash that has not happened yet is worth more than a disk copy of one
     * that already has, especially since the recovered entries HAVE ALREADY
     * been replayed to serial by the loop above: giving the pool back costs the
     * crash_recovery.log file, not the evidence.
     *
     * Freeing here is safe where freeing in klog_crash_write_to_disk is not:
     * this runs from boot_phase0, single-CPU with interrupts disabled, so the
     * unlocked PMM bitmap has no concurrent mutator. */
    if (!phys && s_recovered) {
        pmm_free_contiguous(s_recovered_phys, s_recovered_pages);
        s_recovered       = (klog_crash_entry_t *)0;
        s_recovered_count = 0;
        s_recovered_phys  = 0;
        s_recovered_pages = 0;
        serial_write("[CRASH] released the recovered-entry pool to arm this "
                     "boot's crash region -- serial replay above is the record\n");
        phys = pmm_alloc_contiguous(KLOG_CRASH_PAGES);
    }

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

    /* Persist the zeroed header BEFORE the address reaches NVRAM. The moment
     * NVRAM names this run, the next boot will read whatever memory holds
     * there -- and if the zero is still only in cache when a reset lands, that
     * is the previous tenant's log, complete with a valid magic and a CRC that
     * matches its own entries. Publishing the pointer first is what would make
     * a stale log indistinguishable from this boot's. */
    cache_writeback_range(s_crash_region, sizeof(klog_crash_header_t));

    /* Save this region's address to NVRAM so next boot can find it */
    uefi_set_variable(&s_crash_guid, s_crash_varname,
                      CRASH_NVRAM_ATTRS, sizeof(phys), &phys);

    POST16(POST16_CRASHLOG_ALLOC);
    POST16(POST16_CRASHLOG_DONE);
}

void klog_crash_write_to_disk(void)
{
    /* vfs_open/vfs_write/vfs_close/vfs_is_mounted + the canonical VFS_O_*
     * flags come from kernel/fs/vfs.h. The previous function-local externs
     * and #defines had drifted: VFS_O_TRUNC was defined 0x08 (== canonical
     * VFS_O_APPEND), so the crash log opened in APPEND mode and a shorter
     * recovered log left stale bytes from an older crash. */
    if (s_recovered_count == 0)
        return;

    /* Refuse BEFORE opening: the open below TRUNCATES, so a set that cannot be
     * fully serialized must not be allowed to replace an older, intact log.
     * Returning here preserves s_recovered_count, which is the only in-memory
     * copy of the previous boot's evidence. */
    if (!klog_recovered_set_ok(s_recovered, s_recovered_count)) {
        klog(LOG_ERROR, "CRASH",
             "recovered set incoherent (pool %s, count %u); keeping entries, writing nothing",
             s_recovered ? "present" : "absent", (uint64_t)s_recovered_count);
        return;
    }

    if (!vfs_is_mounted('X') && !vfs_is_mounted('C')) {
        klog(LOG_WARN, "CRASH", "Cannot write crash_recovery.log -- no writable volume");
        return;
    }

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
    int hw = vfs_write(file, offset, sizeof(file_hdr) - 1, (const uint8_t *)file_hdr);
    if (hw < 0 || (uint32_t)hw != sizeof(file_hdr) - 1) {
        /* Short/failed write: the artifact is now a partial header. Do NOT
         * clear s_recovered_count -- keep the only in-memory copy so the
         * evidence is not silently lost and a later attempt can retry. */
        klog(LOG_WARN, "CRASH",
             "crash_recovery.log header write failed (%d of %u); keeping %u recovered entries",
             (int64_t)hw, (uint64_t)(sizeof(file_hdr) - 1),
             (uint64_t)s_recovered_count);
        vfs_close(file);
        return;
    }
    offset += sizeof(file_hdr) - 1;

    /* Write each recovered entry */
    for (uint32_t i = 0; i < s_recovered_count; i++) {
        char line[192];
        uint32_t pos = 0;
        /* Bounded through the accessor rather than indexed directly: the pool
         * is frame-backed now and a torn count must not become a wild read. */
        const klog_crash_entry_t *ent =
            klog_recovered_at(s_recovered, s_recovered_count, i);
        if (!ent) {
            /* The precondition above already proved the set coherent, so this
             * is an INVARIANT FAILURE, not the end of the loop. Falling through
             * to the flush/close path would report every entry as written and
             * then clear the count, destroying the evidence at exactly the
             * moment the guard fired. Keep the entries; report the truncation. */
            klog(LOG_ERROR, "CRASH",
                 "crash_recovery.log truncated at entry %u of %u; keeping recovered entries",
                 (uint64_t)i, (uint64_t)s_recovered_count);
            vfs_close(file);
            return;
        }

        /* [timestamp] LEVEL subsystem: message\n */
        line[pos++] = '[';
        /* Simple decimal for timestamp */
        {
            uint32_t ts = ent->timestamp;
            char tmp[12]; uint32_t t = 0;
            if (ts == 0) { tmp[t++] = '0'; }
            else { while (ts) { tmp[t++] = '0' + (ts % 10); ts /= 10; } }
            while (t) line[pos++] = tmp[--t];
        }
        line[pos++] = ']'; line[pos++] = ' ';

        /* Level */
        {
            static const char *lvl_names[] = { "DEBUG", "INFO", "WARN", "ERROR", "FATAL" };
            uint32_t lv = ent->level;
            if (lv > 4) lv = 4;
            const char *ln = lvl_names[lv];
            while (*ln) line[pos++] = *ln++;
        }
        line[pos++] = ' ';

        /* Subsystem */
        {
            /* NOT alias-resolved, deliberately: s_recovered holds the
             * PREVIOUS boot's entries, recovered from the crash region, and
             * that boot's frame nonce is dead by construction -- it can
             * authenticate nothing in this boot. Resolving here would instead
             * rewrite a prior boot's evidence with this boot's alias. */
            const char *s = ent->subsystem;
            while (*s && pos < 180) line[pos++] = *s++;
        }
        line[pos++] = ':'; line[pos++] = ' ';

        /* Message */
        {
            const char *m = ent->message;
            while (*m && pos < 190) line[pos++] = *m++;
        }
        line[pos++] = '\n';

        int ew = vfs_write(file, offset, pos, (const uint8_t *)line);
        if (ew < 0 || (uint32_t)ew != pos) {
            /* Preserve recovered entries on a short/failed write rather
             * than reporting a truncated log as complete. */
            klog(LOG_WARN, "CRASH",
                 "crash_recovery.log entry %u write failed (%d of %u); keeping recovered entries",
                 (uint64_t)i, (int64_t)ew, (uint64_t)pos);
            vfs_close(file);
            return;
        }
        offset += pos;
    }

    /* Durable boundary: flush to the device cache (FAT32 vfs_flush syncs)
     * before discarding the only in-memory copy. A write accepted into the
     * FS cache but not flushed could leave the artifact truncated/replaced
     * while the evidence is lost. Same flush discipline panic.c uses for
     * last-panic.txt. Preserve s_recovered_count unless write+flush+close
     * all succeed. */
    if (vfs_flush(file) != 0) {
        klog(LOG_WARN, "CRASH",
             "crash_recovery.log flush failed; keeping %u recovered entries",
             (uint64_t)s_recovered_count);
        vfs_close(file);
        return;
    }
    if (vfs_close(file) != 0) {
        klog(LOG_WARN, "CRASH",
             "crash_recovery.log close failed; keeping %u recovered entries",
             (uint64_t)s_recovered_count);
        return;
    }
    klog(LOG_INFO, "CRASH", "Crash recovery log: %u entries written to %scrash_recovery.log",
         (uint64_t)s_recovered_count, cr_dir);

    /* Clear the recovered COUNT only after a durable, fully successful write.
     *
     * The POOL ITSELF IS DELIBERATELY NOT FREED HERE, and that is a decision
     * rather than an oversight. Releasing it was implemented and then reverted:
     * this function runs in Phase 2 from klog_disk_enable, after smp_init, and
     * pmm_free_contiguous mutates the frame bitmap and used_frames with no
     * synchronization, so a release here would race a timed-out boot_async_group
     * storage worker that is still allocating. That unlocked-bitmap window is a
     * known, owned defect, and adding a WRITE to it buys nothing: this array was
     * static BSS before, held unconditionally for the life of every boot, so
     * retaining 41 frames on the rare boot that actually recovered a crash is
     * strictly better than what it replaced, not a regression. Revisit only once
     * PMM allocation and free are synchronized.
     *
     * The overlap path in klog_crash_recover DOES free, and that is not the same
     * situation: that function runs from boot_phase0, single-CPU and long before
     * smp_init, so there is no concurrent allocator to race. The hazard here is
     * the PHASE, not the call. */
    s_recovered_count = 0;
}

/* ---- Per-subsystem verbosity ---- */

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* ---- Disk-sink subsystem alias ----
 *
 * One pair, published once per boot. See klog_set_disk_alias() in klog.h for
 * why this exists; the short version is that the live disk log is readable
 * from ring 3, so an authenticating tag must not reach it.
 *
 * Both pointers are caller-owned with boot-long storage. `s_disk_alias_as`
 * is published with release ordering and read with acquire, so a reader that
 * observes the alias also observes the tag it belongs to. */
static const char *s_disk_alias_tag;
static const char *s_disk_alias_as;
/* The one-shot claim. Separate from the published alias so a refused caller
 * never writes s_disk_alias_tag: the pair is initialized only by the winner,
 * between claiming and publishing. */
static uint32_t     s_disk_alias_claimed;

int klog_set_disk_alias(const char *tag, const char *alias)
{
    uint32_t unclaimed = 0;

    if (!tag || !tag[0] || !alias)
        return 0;

    /* CLAIM the registration before touching either field. Writing the tag
     * first and then testing the alias -- which is what this did -- let a
     * refused SECOND call overwrite the winner's tag and return 0: the
     * resolver then paired the winner's alias with the loser's tag, and the
     * winner's real tag started reaching disk verbatim. For an authenticating
     * tag that silently undoes the whole mechanism, and the caller has no way
     * to notice because it was told it failed.
     *
     * The claim is the single atomic transition; the tag is initialized
     * inside it, and the alias is release-published last so any reader that
     * acquires the alias also observes the tag it belongs to. */
    if (!__atomic_compare_exchange_n(&s_disk_alias_claimed, &unclaimed, 1u, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return 0;

    s_disk_alias_tag = tag;
    __atomic_store_n(&s_disk_alias_as, alias, __ATOMIC_RELEASE);
    return 1;
}

const char *klog_disk_subsystem(const char *subsystem)
{
    const char *alias = __atomic_load_n(&s_disk_alias_as, __ATOMIC_ACQUIRE);

    if (!alias || !subsystem)
        return subsystem;
    if (s_disk_alias_tag == subsystem || str_eq(s_disk_alias_tag, subsystem))
        return alias;
    return subsystem;
}

static log_level_t subsys_min_level(const char *subsystem)
{
    log_level_t gmin = __atomic_load_n(&s_global_min, __ATOMIC_ACQUIRE);
    log_level_t result;
    uint64_t seq;

    if (!subsystem || !subsystem[0])
        return gmin;

    /* Lock-free seqlock read: scan the table, retry if a writer intervened so a
     * torn {tag, min_level} pair is never USED. Config writes are rare, so this
     * almost always runs the loop body exactly once. */
    do {
        uint32_t i, n;
        seq = seqlock_read_begin(&s_override_seq);
        n = s_override_count;
        result = gmin;
        for (i = 0; i < n; i++) {
            if (s_overrides[i].tag == subsystem ||
                str_eq(s_overrides[i].tag, subsystem)) {
                result = s_overrides[i].min_level;
                break;
            }
        }
    } while (seqlock_read_retry(&s_override_seq, seq));

    return result;
}

/* ---- Rate limiting ---- */

/* Find or create a rate slot for a subsystem. Returns NULL if table full. */
static klog_rate_slot_t *rate_slot(const char *subsystem)
{
    uint32_t i;
    /* Lock-free coarse interrupt time in ms (100 ns / 10000). Must NOT use
     * uptime_ns()/mono_ns()/system_get_ticks() here: rate_slot() is on the
     * klog() path, which runs in panic context, and those routes can spin on a
     * mono seqlock or the PIT backend's pit_lock left held by the faulting
     * path. KeQueryInterruptTimeCoarse() is a single atomic load. */
    uint32_t now = (uint32_t)(KeQueryInterruptTimeCoarse() / 10000ULL);  /* ms */

    if (!subsystem || !subsystem[0])
        return (klog_rate_slot_t *)0;

    for (i = 0; i < s_rate_count; i++) {
        if (s_rate[i].tag == subsystem || str_eq(s_rate[i].tag, subsystem)) {
            /* Reset window if expired */
            if (now - s_rate[i].window_start >= KLOG_RATE_WINDOW_MS) {
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

    /* New slot: fill all fields THEN publish the incremented count with a RELEASE
     * store, so the lock-free reader (klog_get_dropped) that ACQUIRE-loads the count
     * never indexes a slot whose tag pointer is still unwritten (str_eq on a NULL
     * tag would fault). Writers are serialized by s_klog_lock; only the reader is
     * lock-free. */
    if (s_rate_count < KLOG_RATE_SLOTS) {
        uint32_t n = s_rate_count;
        klog_rate_slot_t *s = &s_rate[n];
        s->tag = subsystem;
        s->count = 0;
        s->dropped = 0;
        s->window_start = now;
        s->max_rate = 0;
        __atomic_store_n(&s_rate_count, n + 1, __ATOMIC_RELEASE);
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
    uint32_t i, n;

    /* NULL or "" sets the global default (atomic so the lock-free reader sees a
     * whole value, not a torn one). */
    if (!subsystem || !subsystem[0]) {
        __atomic_store_n(&s_global_min, min_level, __ATOMIC_RELEASE);
        return;
    }

    seqlock_write_lock(&s_override_seq);
    n = s_override_count;

    /* Update existing override in place. The seqlock makes a concurrent lock-free
     * reader retry rather than observe the level mid-store. */
    for (i = 0; i < n; i++) {
        if (str_eq(s_overrides[i].tag, subsystem)) {
            s_overrides[i].min_level = min_level;
            seqlock_write_unlock(&s_override_seq);
            return;
        }
    }

    /* Add a new override: fill the slot fully, bump the count, then close the
     * write section. A reader either sees the whole new entry or retries. */
    if (n < KLOG_MAX_OVERRIDES) {
        s_overrides[n].tag = subsystem;
        s_overrides[n].min_level = min_level;
        s_override_count = n + 1;
        seqlock_write_unlock(&s_override_seq);
    } else {
        /* Emit the warning OUTSIDE the write section -- klog() -> subsys_min_level
         * would spin forever on the odd sequence if called while we hold it. */
        seqlock_write_unlock(&s_override_seq);
        klog(LOG_WARN, "klog",
             "verbosity override table full (%u) -- '%s' level not applied",
             (uint64_t)KLOG_MAX_OVERRIDES, subsystem);
    }
}

log_level_t klog_get_level(const char *subsystem)
{
    log_level_t gmin = __atomic_load_n(&s_global_min, __ATOMIC_ACQUIRE);
    log_level_t result;
    uint64_t seq;

    if (!subsystem || !subsystem[0])
        return gmin;

    /* Same lock-free seqlock read as the hot path (test save/restore caller). */
    do {
        uint32_t i, n;
        seq = seqlock_read_begin(&s_override_seq);
        n = s_override_count;
        result = gmin;
        for (i = 0; i < n; i++) {
            if (str_eq(s_overrides[i].tag, subsystem)) {
                result = s_overrides[i].min_level;
                break;
            }
        }
    } while (seqlock_read_retry(&s_override_seq, seq));
    return result;
}

int klog_has_override(const char *subsystem)
{
    int found;
    uint64_t seq;

    if (!subsystem || !subsystem[0])
        return 0;

    do {
        uint32_t i, n;
        seq = seqlock_read_begin(&s_override_seq);
        n = s_override_count;
        found = 0;
        for (i = 0; i < n; i++) {
            if (str_eq(s_overrides[i].tag, subsystem)) {
                found = 1;
                break;
            }
        }
    } while (seqlock_read_retry(&s_override_seq, seq));
    return found;
}

void klog_remove_override(const char *subsystem)
{
    uint32_t i, n;

    if (!subsystem || !subsystem[0])
        return;
    seqlock_write_lock(&s_override_seq);
    n = s_override_count;
    for (i = 0; i < n; i++) {
        if (str_eq(s_overrides[i].tag, subsystem)) {
            /* Swap-with-last + decrement (NOT shift), inside the seqlock write
             * section. The hot reader (subsys_min_level) walks s_overrides
             * lock-free; the two field stores below are not a single atomic
             * publish, but a concurrent reader either snapshots the odd sequence
             * and spins, or detects the sequence change straddling its scan and
             * retries -- so it never USES the swapped-in tag paired with the
             * removed tag's stale level. Overwrite slot i first, then drop the
             * count, so a reader using the new count never indexes the dead slot. */
            if (i != n - 1) {
                s_overrides[i].tag = s_overrides[n - 1].tag;
                s_overrides[i].min_level = s_overrides[n - 1].min_level;
            }
            s_override_count = n - 1;
            seqlock_write_unlock(&s_override_seq);
            return;
        }
    }
    seqlock_write_unlock(&s_override_seq);
}

void klog_load_levels_from_registry(void)
{
    /* Known subsystem tags to check in registry */
    static const char *tags[] = {
        "net", "boot", "fs", "mm", "drv", "sec", "wx", "ahci", "pci",
        "lapic", "ioapic", "acpi", "smp", "UEFI", "TPM", "vfs", "ixfs",
        "fat32", "blk"
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

    /* Also load per-subsystem rate limits from HKLM\SYSTEM\Logs\RateLimit\<tag>.
     * rate_slot() mutates the shared s_rate table (creates slots, resets windows);
     * it is normally called with s_klog_lock held (from klog()). This loader runs in
     * Phase 2 with APs already up, so take s_klog_lock around the rate-slot mutation
     * to serialize against a concurrent klog() on another CPU. (The level loop above
     * uses klog_set_level, which is independently seqlock-synchronized.) */
    for (i = 0; i < tag_count; i++) {
        uint32_t val = 0, val_type = 0, val_size = sizeof(val);
        if (RegReadKeyValue((void *)(uintptr_t)0x80000002,
                            "SYSTEM\\Logs\\RateLimit",
                            tags[i], &val_type, (uint8_t *)&val, &val_size) == 0 &&
            val_type == 4 && val_size == sizeof(uint32_t) && val > 0) {  /* REG_DWORD */
            unsigned long flags;
            klog_rate_slot_t *sl;
            spin_lock_irqsave(&s_klog_lock, &flags);
            sl = rate_slot(tags[i]);
            if (sl) sl->max_rate = val;
            spin_unlock_irqrestore(&s_klog_lock, flags);
        }
    }
}

/* ---- Public API ---- */

uint32_t klog_get_dropped(const char *subsystem)
{
    uint32_t i;
    /* ACQUIRE-load the count (paired with the RELEASE store in rate_slot's new-slot
     * publish) so a fully-initialized tag is visible before this slot is scanned.
     * Lock-free: called once per JSON event during the events.jsonl flush, so a
     * per-call s_klog_lock would land on the live-mode logging path. `dropped` is a
     * best-effort aligned uint32 read (a concurrent ++/window-reset gives a stale
     * but non-torn value -- fine for a stats field). */
    uint32_t n = __atomic_load_n(&s_rate_count, __ATOMIC_ACQUIRE);

    if (!subsystem || !subsystem[0]) return 0;
    for (i = 0; i < n; i++) {
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
    /* Take s_klog_lock around the head/count read so the caller sees
     * a consistent (count, head) pair.  An unlocked read can observe
     * count advanced but head still at the old value (or vice versa)
     * when a concurrent klog() append races with this read.  The
     * returned ring pointer is still the live array (caller reads at
     * their own race risk for entry contents); a fully snapshot-safe
     * API would need to copy entries into caller storage, deferred
     * because no production caller currently iterates entries
     * concurrently with logging. */
    unsigned long flags;
    spin_lock_irqsave(&s_klog_lock, &flags);
    if (out_count) *out_count = klog_ring_count;
    if (out_head)  *out_head  = klog_ring_head;
    spin_unlock_irqrestore(&s_klog_lock, flags);
    return klog_ring;
}

uint64_t klog_get_seq(void)
{
    /* ACQUIRE, not a plain load: klog_emit() advances klog_ring_seq under
     * s_klog_lock from any CPU and from interrupt context, and the sibling
     * accessor above takes the lock for exactly that reason. A torn or stale
     * read here is not cosmetic -- test windows are derived from this value. */
    return __atomic_load_n(&klog_ring_seq, __ATOMIC_ACQUIRE);
}

const klog_entry_t *klog_get_ring_snapshot(uint32_t *out_count,
                                           uint32_t *out_head,
                                           uint64_t *out_seq)
{
    /* head AND seq from ONE lock acquisition.
     *
     * Sampling them separately -- klog_get_ring() under the lock, then
     * klog_get_seq() after it -- looks equivalent and is not: an append
     * landing between the two calls advances seq without advancing the head
     * the caller already captured, so a window derived from (head, seq) is one
     * entry wider than the head anchor warrants. A consumer walking back from
     * head then reaches one slot too far, into whatever was there before the
     * window opened. The runner runs with interrupts enabled, so the timer ISR
     * alone is enough to hit it on a single CPU. */
    unsigned long flags;
    spin_lock_irqsave(&s_klog_lock, &flags);
    if (out_count) *out_count = klog_ring_count;
    if (out_head)  *out_head  = klog_ring_head;
    if (out_seq)   *out_seq   = klog_ring_seq;
    spin_unlock_irqrestore(&s_klog_lock, flags);
    return klog_ring;
}

uint32_t klog_panic_snapshot(klog_entry_t *out, uint32_t max)
{
    /* Lock-free by design: a collector running at the top of a fault funnel may
     * be on a CPU that already holds s_klog_lock inside klog(), so taking it
     * here could self-deadlock. We read head/count without the lock and copy
     * the last entries; a concurrent append on another CPU may yield a torn
     * tail entry, which is acceptable for best-effort crash forensics. */
    if (!out || max == 0u)
        return 0u;
    uint32_t head  = klog_ring_head;     /* next write slot */
    uint32_t count = klog_ring_count;    /* valid entries, capped below */
    if (count > KLOG_RING_SIZE)
        count = KLOG_RING_SIZE;
    uint32_t n = (count < max) ? count : max;
    for (uint32_t i = 0u; i < n; i++) {
        /* oldest-of-the-last-n first: ring index head-1-(n-1-i) = head-n+i */
        uint32_t idx = (head + KLOG_RING_SIZE - n + i) % KLOG_RING_SIZE;
        out[i] = klog_ring[idx];
    }
    return n;
}

/* Shared core for klog() and klog_unrated(). `bypass_rate` skips ONLY the
 * per-subsystem rate limiter (the verbosity filter, ring/disk/serial path, and
 * lock discipline are identical) so an already-prelimited caller -- e.g. the
 * per-process exception-dispatch telemetry (except.c) -- keeps its subsystem tag
 * and the unified sink without one process's flood clipping another's budget.
 * `ap` is owned by the caller (va_start/va_end live in the thin wrappers).
 *
 * `rcpt` is the optional per-record delivery receipt (klog.h). It fires
 * EXACTLY ONCE on every exit from this function -- both drops and the
 * delivering path -- because its meaning is "this record is no longer owed",
 * and an exit that fired nothing would strand whatever obligation the caller
 * settles against it. `delivered` is what distinguishes the outcomes. A NULL
 * `rcpt` is the ordinary path and costs one pointer test per exit -- the
 * value is spilled once and reloaded before each test, because it has to
 * stay live from here to the delivery boundary below (see klog.h for the
 * measurement and why the residual is accepted rather than split away).
 *
 * ONE pointer rather than a callback/cookie pair, because the sixth integer
 * argument is the last one that fits a register: as a pair this function took
 * seven, the va_list spilled to the stack, and EVERY klog line in the kernel
 * paid an outgoing store plus 16 bytes of frame (measured on release -O2).
 * See struct klog_receipt in klog.h. */
static void klog_emit(log_level_t level, const char *subsystem, int bypass_rate,
                      const struct klog_receipt *rcpt,
                      const char *fmt, va_list ap)
{
    klog_entry_t snapshot;   /* local copy for output outside the lock */
    uint64_t irq_flags;
    int rate_dropped = 0;
    int first_drop = 0;

    /* Per-subsystem verbosity filter: drop entries below threshold.
     * Read-only check on s_overrides -- safe without lock (overrides are
     * append-only and only modified during single-threaded boot or with
     * explicit klog_set_level calls). */
    if (level < subsys_min_level(subsystem)) {
        /* Declined, not delivered -- but still acknowledged. The record was
         * never handed to serial, so a caller counting what the host must
         * have seen learns that from `delivered`, while its outstanding
         * obligation is discharged either way. */
        if (rcpt)
            rcpt->fn(rcpt->cookie, 0);
        return;
    }

    /* ---- Lock: protect ring buffer + rate-limit mutations ---- */
    spin_lock_irqsave(&s_klog_lock, &irq_flags);

    /* Per-subsystem rate limit: drop if over budget this window. An
     * already-prelimited caller (bypass_rate) skips this shared cap. */
    if (!bypass_rate && !rate_check(subsystem)) {
        klog_rate_slot_t *sl = rate_slot(subsystem);
        if (sl && sl->dropped == 1)
            first_drop = 1;
        rate_dropped = 1;
    }

    if (rate_dropped) {
        spin_unlock_irqrestore(&s_klog_lock, irq_flags);
        /* UNREACHABLE for every receipted caller that exists today, and kept
         * anyway. The only wrapper passing a non-NULL ack is klog_receipted,
         * which passes bypass_rate=1, so this branch cannot run with an ack in
         * hand. It stays because the acknowledgement is a property of THIS
         * function's exits rather than of the current wrapper set: a rated
         * receipted wrapper added later without it would strand its caller's
         * obligation permanently, and there is no diagnostic for that -- the
         * run simply waits out its drain budget and reports a loss that never
         * happened.
         *
         * Acknowledged BEFORE the summary line below, not after: the summary
         * is an unrelated record that pays a whole serial write, and ordering
         * this caller's settlement behind it would hand the receipt exactly
         * the latency the receipt exists to remove. */
        if (rcpt)
            rcpt->fn(rcpt->cookie, 0);
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
        /* 10 ms units from the coarse cached interrupt time: lock-free (single
         * __atomic load, NO seqlock/clocksource read) so klog() is panic-safe on
         * EVERY timer backend (system_get_ticks() takes pit_lock on the PIT/TCG
         * backend -- unusable in panic context), AND rate-change-safe (interrupt
         * time is monotonic, not raw lifetime ticks that rewind on a
         * KeSetTimerResolution change). The cache is published from the first
         * tick after mono_clock_init(); entries logged before any monotonic
         * source exists stamp 0 (unavoidable -- no lock-free early clock).
         * klog_disk reconstructs the entry FILETIME from this same 10 ms unit. */
        e->timestamp = (uint32_t)(KeQueryInterruptTimeCoarse() / 100000ULL);

        /* Per-entry context: CPU, PID, TID */
        {
            struct per_cpu_data *cpu = smp_this_cpu();
            e->cpu_id = cpu ? cpu->cpu_id : 0;
        }
        /* pid/tid come from the scheduler's current task/thread cursor. The
         * scheduler today is a single GLOBAL cursor (task.c current_task /
         * current_thread), so these are accurate for the scheduling context and
         * best-effort otherwise; true per-CPU attribution awaits per-CPU run
         * queues (see the Accepted limitation in the section stamp). tid is the
         * real thread id (was previously hardcoded 0). */
        if (kernel_subsystem_ready(SUBSYS_SCHED)) {
            struct task   *t  = task_current();
            struct thread *th = thread_current();
            e->pid = t  ? t->pid : 0;
            e->tid = th ? th->id : 0;
        } else {
            e->pid = 0;
            e->tid = 0;
        }

        vformat_buf(e->message, sizeof(e->message), fmt, ap);

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
        int line_truncated = 0;

        /* Reserve TAIL_RESERVE bytes at the end of `line` for
         * ANSI_RESET + truncation marker + '\n' + '\0'.  Letting LS()
         * consume right up to sizeof(line)-1 lets a long subsystem
         * /message fill the buffer before the trailing ANSI_RESET is
         * appended, leaving the terminal in the warning/error/test
         * color for subsequent output.  With the reserve, ANSI_RESET
         * always fits in the unconditional trailer below.  ANSI_RESET
         * = "\x1b[0m" is 4 chars; reserve 16 for any future suffix
         * additions. */
        #define KLOG_LINE_TAIL_RESERVE 16U
        #define KLOG_LINE_USABLE (sizeof(line) - 1U - KLOG_LINE_TAIL_RESERVE)

        /* Same one-evaluation contract as vformat_buf's BUF_PUT, and for the
         * same reason: the timestamp digit loop below is `while (n > 0)
         * LP(tmp[--n])`, so a form that skipped the argument on the full
         * branch would spin forever. It is unreachable today only because the
         * timestamp is the FIRST thing written into an empty line buffer --
         * an ordering property, not a guarantee, and not one a later edit
         * would know it was preserving. */
        #define LP(c) do { \
            char _lp_c = (char)(c); \
            if (pos < KLOG_LINE_USABLE) line[pos++] = _lp_c; \
            else line_truncated = 1; \
        } while(0)
        #define LS(s) do { \
            const char *_p = (s); \
            while (*_p) { \
                if (pos >= KLOG_LINE_USABLE) { line_truncated = 1; break; } \
                line[pos++] = *_p++; \
            } \
        } while(0)

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
         * Special test-runner subsystems keep the level-colored badge but
         * recolor the subsystem + message text so the three test layers
         * are visually distinct in a mixed boot log:
         *   "TEST"  (kernel)      -> #D2A8FF  light lavender
         *   "UTEST" (user mode)   -> #84B2E9  light blue
         *   "DTEST" (desktop UI)  -> #C586B5  pink-mauve (reserved) */
        {
            const char *test_color = (const char *)0;

            if (subsystem) {
                if (klog_tag_is(subsystem, "TEST"))
                    test_color = ANSI_TEST_KERNEL;
                else if (klog_tag_is(subsystem, "UTEST"))
                    test_color = ANSI_TEST_USER;
                else if (klog_tag_is(subsystem, "DTEST"))
                    test_color = ANSI_TEST_DESKTOP;
            }

            /* UTEST color scope: when the user-mode test launcher has
             * the scope flag active (between task_create and
             * task_cleanup for a test_*.exe), every kernel klog line
             * emitted in that window -- regardless of its subsystem
             * tag -- was produced on behalf of that binary's spawn /
             * exec_load / ELF segment loads / signal delivery. Render
             * them in the UTEST color so the visual grouping in the
             * boot log matches the logical grouping. The TEST/UTEST/
             * DTEST subsystem-name overrides above still win if a
             * nested test harness explicitly tags its output. */
            if (!test_color && subsystem) {
                if (test_usermode_color_active())
                    test_color = ANSI_TEST_USER;
            }

            /* Badge: normal level color */
            LS(level_ansi[level]);
            LS(level_prefix[level]);
            LS(ANSI_RESET);

            if (test_color) {
                LS(test_color);
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
        /* Emit ANSI_RESET unconditionally (the reserved tail bytes
         * guarantee it fits even when the main format truncated).
         * This stops the terminal from staying in color after a
         * long-line truncation. */
        {
            const char *reset = ANSI_RESET;
            while (*reset && pos < sizeof(line) - 4)
                line[pos++] = *reset++;
        }
        if (line_truncated && pos < sizeof(line) - 3) {
            line[pos++] = '~';  /* truncation marker (after ANSI_RESET) */
        }
        if (pos < sizeof(line) - 2) line[pos++] = '\n';
        line[pos] = '\0';

        #undef LP
        #undef LS
        #undef KLOG_LINE_TAIL_RESERVE
        #undef KLOG_LINE_USABLE

        serial_write(line);

        /* THE DELIVERY BOUNDARY. serial_write has returned, so this record is
         * on the wire a host reading serial will see -- and the receipt fires
         * here rather than after the framebuffer and disk sinks below because
         * serial is the stream the host reconciles. Waiting for the other
         * sinks would delay the acknowledgement past work no consumer of the
         * receipt is waiting on, and the disk sink in particular performs a
         * synchronous append+flush.
         *
         * Before the LOG_FATAL halt at the bottom for the same reason: a
         * record whose level halts the machine still reached the wire, and a
         * receipt that never fired would leave its caller's obligation
         * outstanding forever. */
        if (rcpt)
            rcpt->fn(rcpt->cookie, 1);
    }

    /* ---- Output to framebuffer if level >= screen threshold ---- */
    if (level >= screen_min_level) {
        uint32_t fb_color = level_color[level];

        if (subsystem) {
            if (klog_tag_is(subsystem, "TEST"))
                fb_color = FB_TEST_KERNEL;
            else if (klog_tag_is(subsystem, "UTEST"))
                fb_color = FB_TEST_USER;
            else if (klog_tag_is(subsystem, "DTEST"))
                fb_color = FB_TEST_DESKTOP;
        }

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

    /* ---- Live debug log: flush to X:\Logs\Serial\Serial_YYMMDDNN.log
     * (klog_dir + "Serial\\") immediately ----
     *
     * The disk sink is openable from ring 3, so a subsystem whose tag
     * authenticates its records to a host reading serial must not have that
     * value written where a user process can read it back. The substitution
     * lives inside klog_disk_append (klog_disk.c), not here: this is only
     * ONE of its two callers, and the batch ring drain is the one that
     * actually fills kernel.log. Serial and the ring keep the real tag. */
    if (klog_disk_live_active()) {
        klog_disk_append(&snapshot);
        klog_disk_flush();
    }

    /* ---- FATAL: halt ---- */
    if (level == LOG_FATAL) {
        serial_write("[**] FATAL -- system halted\r\n");
        /* ARCH: x86-64 -- will move to arch/. klog.c is otherwise
         * architecture-neutral code, and this is its one inline asm: the
         * terminal halt after a fatal record reaches the wire. It needs a HAL
         * cpu_halt() to become neutral, which does not exist yet. */
        for (;;)
            __asm__ volatile ("hlt");
    }
}

void klog(log_level_t level, const char *subsystem, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    klog_emit(level, subsystem, 0 /*bypass_rate*/,
              (const struct klog_receipt *)0, fmt, ap);
    va_end(ap);
}

/* Like klog() but bypasses the per-subsystem rate limiter. For callers that have
 * already applied their OWN rate limit (e.g. per-process exception-dispatch
 * telemetry) so the shared subsystem cap cannot clip one caller's events for
 * another. Keeps the subsystem tag and the unified ring/disk/serial sink. */
void klog_unrated(log_level_t level, const char *subsystem, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    klog_emit(level, subsystem, 1 /*bypass_rate*/,
              (const struct klog_receipt *)0, fmt, ap);
    va_end(ap);
}

/* Like klog_unrated() plus a delivery receipt. The rate limiter is bypassed
 * for the same reason klog_unrated bypasses it and for one more: a receipted
 * record is emitted by a caller that is ACCOUNTING for it, so letting an
 * unrelated subsystem's flood clip it would turn another caller's noise into
 * this caller's missing record. The contract the `ack` must satisfy is stated
 * on klog_receipt_fn in klog.h. */
void klog_receipted(log_level_t level, const char *subsystem,
                    klog_receipt_fn ack, uint64_t cookie,
                    const char *fmt, ...)
{
    struct klog_receipt rcpt;
    va_list ap;

    rcpt.fn     = ack;
    rcpt.cookie = cookie;

    va_start(ap, fmt);
    /* NULL through when there is no callback, so a caller that passes one
     * conditionally lands on exactly the ordinary path rather than a receipt
     * whose fn would be dereferenced. */
    klog_emit(level, subsystem, 1 /*bypass_rate*/,
              ack ? &rcpt : (const struct klog_receipt *)0, fmt, ap);
    va_end(ap);
}
