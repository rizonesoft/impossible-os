/* ============================================================================
 * klog.h -- Unified kernel logging
 *
 * Levels:
 *   LOG_DEBUG  [INFO] -- serial only (suppressed from framebuffer)
 *   LOG_INFO   [ OK ] -- serial + framebuffer (green prefix)
 *   LOG_WARN   [WARN] -- serial + framebuffer (yellow prefix)
 *   LOG_ERROR  [FAIL] -- serial + framebuffer (red prefix)
 *   LOG_FATAL  [CRIT] -- serial + framebuffer (red prefix), then halt
 *
 * Usage:
 *   klog(LOG_INFO, "drv", "PS/2 mouse initialized (IRQ %u)", 12);
 *   klog(LOG_ERROR, "fs", "failed to mount C:");
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

typedef enum {
    LOG_DEBUG = 0,   /* [INFO] serial only */
    LOG_INFO  = 1,   /* [ OK ] serial + framebuffer */
    LOG_WARN  = 2,   /* [WARN] serial + framebuffer */
    LOG_ERROR = 3,   /* [FAIL] serial + framebuffer */
    LOG_FATAL = 4,   /* [CRIT] serial + framebuffer, then halt */
} log_level_t;

/* Primary log directory (BlackBox X:\Logs\) with C:\ fallback.
 * Resolved at runtime by klog_disk_init(): prefers X:\Logs\ if BlackBox
 * is mounted, falls back to C:\Impossible\System\Logs\ if not. */
#define KLOG_DIR_BLACKBOX  "X:\\Logs\\"
#define KLOG_DIR_FALLBACK  "C:\\Impossible\\System\\Logs\\"

/* Runtime-resolved log directory (set by klog_disk_init) */
extern const char *klog_dir;
extern int klog_using_blackbox;  /* 1 if X:\Logs\, 0 if C:\ fallback */

/* Log a message with level and subsystem tag.
 * fmt supports: %d, %u, %x, %p, %s, %c, %%  (same as printk)
 *
 * INTEGER ARGUMENTS MUST BE 64-BIT. The format engine reads every numeric
 * conversion as a full 64-bit vararg, so a 32-bit value must be cast at the
 * call site: `klog(..., "%u", (uint64_t)n)`. Passing a bare `int` is not a
 * style nit and does not always show up in testing -- a value that lands in a
 * REGISTER slot is usually zero-extended by the compiler and prints correctly,
 * while the same value in a STACK slot (roughly, the 4th vararg onward) leaves
 * the upper 4 bytes uninitialized and prints garbage. Observed on the test
 * runner's failing-assertion line, whose `int line` was the 7th argument and
 * rendered as `test_harness.c:-194693637781585198`.
 *
 * Messages are truncated at `klog_entry_t.message[256]`, marked with a
 * trailing `~`, and the record still reaches every sink. A caller that cannot
 * afford to lose the tail composes its own bounded record and passes a single
 * "%s" (see `test_fail_record_format` in include/kernel/test/test.h). */
void klog(log_level_t level, const char *subsystem, const char *fmt, ...);

/* Like klog() but bypasses ONLY the per-subsystem rate limiter -- for a caller
 * that already applied its own (e.g. per-process) rate limit, so the shared
 * subsystem cap cannot clip one caller's events on behalf of another. Same
 * verbosity filter, ring/disk/serial sink, and lock discipline as klog(). */
void klog_unrated(log_level_t level, const char *subsystem, const char *fmt, ...);

/* Delivery receipt for ONE record: the acknowledgement klog publishes at the
 * point that record has either reached the wire or been declined.
 *
 * `cookie` is the value handed to klog_receipted(); `delivered` is 1 once the
 * record's serial write has returned and 0 when klog declined to emit it. The
 * only decline a receipted caller can meet TODAY is the verbosity filter,
 * because klog_receipted bypasses the rate limiter; klog_emit acknowledges its
 * rate-limited exit too, so the guarantee belongs to that function's exits
 * rather than to the current set of wrappers. Exactly one call per klog_receipted()
 * invocation, always before that call returns, so a caller that settles an
 * obligation against the receipt is no longer beaten by its own death across
 * the whole remainder of its life -- only across the single call between
 * serial_write returning and the acknowledgement. That residue is NOT zero
 * and the difference matters: an emitter preempted inside that call and then
 * reaped never runs the callback at all. Narrowing it further is not
 * available here (the alternatives are publishing inside serial's own
 * critical section, which couples the UART driver to its consumers, or
 * holding a consumer lock across serial output, which the kernel forbids);
 * closing it belongs to whatever reconciles the resulting stream.
 *
 * `delivered=1` means serial_write RETURNED, which is not the same as bytes
 * having left a UART: serial_putchar_raw returns silently when no port was
 * detected (src/kernel/drivers/serial.c), so on a machine with no serial the
 * receipt still reports delivery. That is the honest boundary klog can offer
 * -- it owns the write, not the wire -- and a consumer whose whole channel is
 * the serial log is unaffected by the case, because a host reading that log
 * does not exist in the configuration that produces it.
 *
 * It is also a statement about ONE sink. The receipt fires after serial and
 * BEFORE the framebuffer draw and the synchronous disk append, so `delivered`
 * says nothing about either -- including the disk sink, which is the one that
 * can actually fail (short write, flush failure, unwritable media). Early is
 * the point for the consumer this exists for, whose channel IS serial and
 * which must not wait behind a disk flush; it is spelled out because a later
 * consumer reading `delivered=1` as "this record is durable" would be wrong.
 * Per-sink delivery is a different mechanism, and this is not a down payment
 * on one.
 *
 * CONTRACT -- the callback runs in the emitter's own context at the delivery
 * boundary, holding no klog lock, so it must be:
 *   - synchronous and bounded: no sleeping, no blocking, no yielding;
 *   - safe with interrupts DISABLED. klog is callable from interrupt context
 *     and the irqsave locks restore the CALLER's prior IRQ state, so the
 *     callback inherits whatever the caller had rather than a known state;
 *   - safe to run concurrently on several CPUs;
 *   - frugal with STACK, not just with time. It is invoked with klog_emit's
 *     whole frame live -- the ring-entry snapshot plus the 512-byte serial
 *     line buffer -- so roughly 800 bytes are already spent on whatever stack
 *     the emitter was using, and klog is reachable from the IST stacks;
 *   - free of any klog call, receipted or not. A nested record would complete
 *     its framebuffer and disk output before the outer record reaches those
 *     sinks, reversing sink order against serial and the ring, and a nested
 *     RECEIPTED record can recurse without bound.
 * A spin_lock_irqsave-guarded counter update -- what the user-mode capture
 * accounting does -- fits this contract exactly.
 *
 * The CALLER carries the mirror obligation: never emit a receipted record
 * while holding a lock the callback itself takes. The receipt fires in the
 * emitter's own context, so that is a plain self-deadlock -- these spinlocks
 * are non-recursive and the CPU is already spinning with interrupts off, so
 * it hangs with no diagnostic at all. The shape is one careless edit away
 * wherever a lock guards the decision to log AND the callback settles under
 * the same lock, which is exactly the capture accounting's arrangement: it
 * reserves under s_capture_budget_lock, RELEASES, and only then emits.
 *
 * NOT usable from a panic or fault funnel. Those paths forbid spinlocks
 * outright, because the faulting CPU may already hold the very lock a
 * callback would take (klog_panic_snapshot exists for exactly that reason),
 * so the counter-update shape sanctioned above is illegal there. No receipted
 * record is emitted from such a path today; keep it that way. */
typedef void (*klog_receipt_fn)(uint64_t cookie, int delivered);

/* The callback and its cookie travel as ONE pointer, and that packaging is a
 * measured cost decision rather than a style choice. klog_emit takes the level,
 * subsystem, rate-bypass flag, receipt, format and va_list; passing the
 * callback and cookie separately makes seven integer arguments, which pushes
 * the va_list past r9 and onto the stack. Measured on the release -O2 build:
 * every klog() and klog_unrated() call in the kernel grew an outgoing stack
 * store (`mov %rcx,(%rsp)`) and 16 bytes of frame, on the emitted, filtered
 * AND rate-dropped paths alike. Collapsed into one pointer the argument list
 * fits the six registers again, and an ordinary caller passes a NULL it never
 * dereferences.
 *
 * What that does NOT buy, stated because the obvious reading is wrong: the
 * receipt is not free INSIDE klog_emit. It has to stay live from entry to the
 * delivery boundary several hundred lines later, so the register allocator
 * spills it once into the frame and reloads it before each of the three
 * tests; klog_emit's frame is 856 bytes against 840 before. The residual is
 * accepted rather than engineered away: the only way to compile the ordinary
 * path without a live receipt value is a second copy of klog_emit, which
 * duplicates the hottest function in the kernel to save one store, one
 * reload and 16 bytes of stack. */
struct klog_receipt {
    klog_receipt_fn fn;
    uint64_t        cookie;
};

/* Like klog_unrated() plus a per-record delivery receipt (see above).
 *
 * The receipt is opt-in PER CALL rather than a registered sink so the ordinary
 * path pays one pointer test per exit and nothing else: no global load, no
 * per-thread state, no extra lock on the line that every subsystem in the
 * kernel emits. `ack` may be NULL, which makes this identical to
 * klog_unrated(). */
void klog_receipted(log_level_t level, const char *subsystem,
                    klog_receipt_fn ack, uint64_t cookie,
                    const char *fmt, ...);

/* Capacity of the SERIALIZED subsystem tag, including its NUL.
 *
 * The ring entry below stores the tag as a `const char *` and never copies
 * it, but two serialized records DO copy it into a fixed field: the
 * crash-region record (`klog_crash_entry_t` in src/kernel/klog.c, whose
 * on-region layout the next boot reads back) and the panic evidence block
 * (`struct panic_klog_entry` in include/kernel/panic.h). A caller that BUILDS a tag at runtime -- rather
 * than passing a string literal -- must fit this bound, or its evidence is
 * truncated in exactly the paths that matter after a crash, and must give
 * that tag storage that outlives every entry logged under it. */
#define KLOG_SUBSYSTEM_MAX 16

/* Render `tag` as `alias` on the DISK sink only, leaving serial unchanged.
 *
 * Exists for one caller class: a subsystem tag that AUTHENTICATES its
 * records to a host reading serial. The live disk log (X:\Logs\Serial_*.log)
 * is openable from ring 3, so a tag that is secret on serial stops being
 * secret the moment it is also written there -- a user process can read the
 * value back and then emit records the host accepts as kernel-owned. The
 * alias keeps the records in the on-disk log for post-mortem diagnosis
 * while keeping the authenticating value out of it.
 *
 * Both pointers must have storage outliving every entry logged under them.
 * Registering is one-shot per boot: a second call is refused, because
 * rewriting the tag would re-attribute entries already queued under the
 * first. Returns 1 when THIS call published the alias, 0 when it was
 * refused -- a caller whose security property depends on the alias being
 * live must check, since a refusal leaves the real tag reaching disk. */
int klog_set_disk_alias(const char *tag, const char *alias);

/* Disk rendering for `subsystem`: the registered alias, or `subsystem`
 * itself when none applies. Applied by the disk sink rather than its
 * callers, because the sink has two of them -- the live per-message flush
 * and the batch ring drain -- and a caller-side substitution silently
 * covers only the first. */
const char *klog_disk_subsystem(const char *subsystem);

/* Ring buffer access for debug console */
typedef struct {
    log_level_t level;
    const char *subsystem;
    uint32_t    timestamp;  /* 10 ms units since boot (KeQueryInterruptTimeCoarse) */
    uint8_t     cpu_id;     /* CPU that logged this entry (0 = BSP) */
    uint8_t     _pad[3];    /* alignment padding */
    uint32_t    pid;        /* process ID (0 during boot) */
    uint32_t    tid;        /* thread ID (0 during boot) */
    char        message[256];
} klog_entry_t;

/* Get pointer to ring buffer and current count.
 * Returns pointer to static array of KLOG_RING_SIZE entries.
 * out_seq: monotonic sequence number (total entries ever written). */
#define KLOG_RING_SIZE 1000

const klog_entry_t *klog_get_ring(uint32_t *out_count, uint32_t *out_head);
uint64_t klog_get_seq(void);

/* ring + count + head + monotonic seq, all from ONE lock acquisition.
 *
 * Use this instead of pairing klog_get_ring() with klog_get_seq() whenever the
 * (head, seq) pair is load-bearing: an append landing between two separate
 * calls advances seq past the captured head, so a window derived from the pair
 * is wider than its own anchor and a consumer walking back from head reads an
 * entry from before the window. */
const klog_entry_t *klog_get_ring_snapshot(uint32_t *out_count,
                                           uint32_t *out_head,
                                           uint64_t *out_seq);

/* Lock-free best-effort snapshot of the last `max` ring entries into `out`
 * (oldest-first); returns the number copied. For the panic path ONLY: takes no
 * lock (the faulting CPU may already hold s_klog_lock) and tolerates a torn
 * entry. Never use outside a crash collector. */
uint32_t klog_panic_snapshot(klog_entry_t *out, uint32_t max);

/* Set minimum level that appears on the framebuffer.
 * Default: LOG_INFO (i.e., DEBUG is serial-only).
 * Set to LOG_DEBUG to show everything on screen. */
void klog_set_screen_level(log_level_t min_level);

/* Set per-subsystem minimum log level.
 * Entries below this level are dropped entirely (not stored in ring).
 * subsystem: tag string (e.g. "net", "fs"). NULL or "" sets the global default.
 * Up to 32 subsystem overrides can be active simultaneously. */
void klog_set_level(const char *subsystem, log_level_t min_level);

/* Get the current per-subsystem minimum log level.
 * Returns the active override for `subsystem` if one exists, otherwise
 * the global default. NULL or "" returns the global default directly.
 * Used by test infrastructure (TEST_KLOG_SUPPRESS) to save-restore
 * per-subsystem levels around an error-path test. */
log_level_t klog_get_level(const char *subsystem);

/* Query whether an explicit override exists for `subsystem`. Returns
 * 1 if a klog_set_level(subsystem, ...) is active for this tag, 0 if
 * the subsystem falls back to the global default. Used by
 * TEST_KLOG_SUPPRESS so restore can either remove the temporary
 * override (when none existed) or restore its value (when it did). */
int klog_has_override(const char *subsystem);

#ifdef KERNEL_TESTS
/* Test seam over the internal tag classifier `klog_tag_is()`. Returns 1 when
 * `tag` is exactly `name`, optionally followed by ':'.
 *
 * The classifier is static because nothing outside the renderer should call
 * it, but it is the exact helper whose fixed-offset predecessor read past the
 * end of every short tag -- and proving it stays inside its argument needs a
 * direct call under the poisoned-boundary fixture. Routing that proof through
 * klog() itself is not an option: the renderer runs under s_klog_lock with
 * interrupts disabled, and kernel SEH declines to unwind with RFLAGS.IF clear
 * (src/kernel/except.c:761), so the fault would be terminal instead of caught.
 *
 * XREF: 00-infrastructure/TODO-03-kernel-test-harness.md section 11 */
int klog_probe_tag_is(const char *tag, const char *name);
#endif

/* Remove the explicit override for `subsystem`, making the tag follow
 * the global default again. No-op if no override exists. NULL / "" is
 * a no-op (the global default cannot be removed).
 *
 * SMP note: the override table is a seqlock (include/kernel/sched/seqlock.h).
 * Writers serialize on the seqlock's internal spinlock and bracket every
 * mutation with the odd/even sequence; the hot lock-free reader (the klog()
 * verbosity filter) and the cold query APIs (klog_get_level / klog_has_override)
 * use the seqlock retry loop, so none ever observes a torn {tag, level} pair
 * during the swap-with-last + decrement. */
void klog_remove_override(const char *subsystem);

/* Load per-subsystem log levels from Registry.
 * Reads HKLM\SYSTEM\Logs\Levels\<subsystem> for each known tag.
 * Call after registry_init(). */
void klog_load_levels_from_registry(void);

/* ---- Split init (Phase 0 / Phase 2) ----
 * klog_early_init(): Phase 0 safe -- ring buffer + serial only, no VFS.
 * klog_disk_enable(): Phase 2 safe -- opens log files, starts disk flushing. */
void klog_early_init(void);            /* Phase 0: ring buffer ready */
int  klog_disk_enable(void);           /* Phase 2: VFS-backed disk logging; 1 iff live */
int  klog_disk_active(void);           /* 1 iff disk log buffer + mounted target */

/* Get rate-limited dropped count for a subsystem (0 if no drops). */
uint32_t klog_get_dropped(const char *subsystem);

/* ---- Unified disk logging (klog_disk.c) ---- */
void klog_resolve_dir(void);           /* Resolve log dir: X:\ or C:\ fallback */
void klog_disk_init(void);             /* Allocate buffer, scan for log number */
void klog_disk_flush(void);            /* Write ring to C: + buffer to X: (no-op in deferred mode) */
void klog_disk_flush_all(void);        /* Forced single flush -- clears deferred mode (boot end) */
/* Optional progress callback: invoked periodically during a flush with
 * (entries_written, entries_total) so the boot splash can show "Writing boot
 * log... N/M" on a slow USB drain instead of looking hung. NULL disables. */
typedef void (*klog_flush_progress_fn)(uint32_t written, uint32_t total);
void klog_disk_set_flush_progress_cb(klog_flush_progress_fn cb);
int  klog_flush_progress_due(uint32_t total);  /* pure: flush big enough to show progress? tested */
void klog_set_deferred(int enabled);   /* Deferred-flush mode: batch in RAM until flush_all */
#define KLOG_DEFER_ACTIVE    0x1u      /* deferred-flush state: klog_disk_flush() no-op */
#define KLOG_DEFER_DISABLED  0x2u      /* boot-end latch: dominant -- auto-enable suppressed */
int  klog_defer_active(uint32_t state);/* effective deferral: ACTIVE set AND DISABLED clear */
int  klog_slow_media_detected(void);   /* 1 if a flush exceeded KLOG_SLOW_MEDIA_MS (slow boot media) */
uint32_t klog_flush_window(uint64_t cur_seq, uint64_t cursor); /* bounded unflushed count (<= KLOG_RING_SIZE) */
uint32_t klog_lost_count(uint64_t cur_seq, uint64_t cursor);   /* entries lost to ring overflow (> KLOG_RING_SIZE) */
int  klog_dispatch_slot(const char *subsystem);  /* subsystem log slot, or -1 (kernel.log only); pure */
/* Decompress a `.N.lz4` rotated-log archive (klog_lz4_hdr_t + LZ4 block) into dst.
 * Validates magic/version/bounds/CRC32 and that the decode is exactly the recorded
 * uncompressed size. Returns the decompressed byte count, or -1 on malformed/
 * corrupt/truncated input. For the in-OS viewer + host extractor. */
int  klog_decompress_rotated(const void *src, uint32_t src_size,
                             uint8_t *dst, uint32_t dst_cap);
/* Compress src into a [header + LZ4 block] rotated-log archive (the in-memory half
 * of the .N.lz4 writer). Returns total archive size, or -1. dst_cap must be >=
 * 20 + lz4_compress_bound(src_size). Pure; exposed for the roundtrip test. */
int  klog_compress_buffer(const void *src, uint32_t src_size,
                          uint8_t *dst, uint32_t dst_cap);
void klog_disk_set_live(int on);       /* Enable/disable per-entry live mode */
int  klog_disk_live_active(void);      /* Returns 1 if live mode is on */
void klog_disk_append(const klog_entry_t *e);  /* Append entry to FAT32 buffer */

/* ---- Crash-persistent log capture (klog.c) ---- */

#define KLOG_CRASH_MAGIC    0x4B4C4F47  /* "KLOG" */
#define KLOG_CRASH_PAGES    32          /* 128 KiB reserved region */

/* Header at start of crash persistence region (physical memory) */
typedef struct {
    uint32_t magic;             /* KLOG_CRASH_MAGIC */
    uint32_t entry_count;       /* number of ring entries saved */
    uint32_t crc32;             /* IEEE CRC32 of entries after header */
    uint32_t ring_head;         /* ring head at time of crash */
    uint64_t boot_timestamp;    /* 10 ms units since boot (KeQueryInterruptTimeCoarse) */
} klog_crash_header_t;

/* Persist ring buffer to reserved physical memory (no kmalloc, no VFS).
 * Called from panic_screen() after BSOD render, before halt. */
void klog_crash_persist(void);

/* Check reserved region for valid crash data from previous boot.
 * If found, replays to serial with [CRASH-PREV] prefix.
 * Called early in klog_early_init(). */
void klog_crash_recover(void);

/* Write recovered crash entries to disk log file.
 * Called after VFS mount in klog_disk_enable(). */
void klog_crash_write_to_disk(void);

