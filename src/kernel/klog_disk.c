/* ============================================================================
 * klog_disk.c -- Unified disk logging (replaces klog_flush.c + klog_live.c)
 *
 * Handles all disk-based log output:
 *   - Batch flush to C:\Impossible\System\Logs\kernel.log (IXFS, appendable)
 *   - Full buffer write to X:\Logs\Serial\Serial_YYMMDDNN.log (per boot session)
 *   - Live mode: every klog() entry appended + flushed immediately
 *
 * Buffer: 256 KB via pmm_alloc_contiguous (identity-mapped).
 * FAT32 limitation: vfs_write() is full-file overwrite, so we buffer the
 * entire log and rewrite on each flush.
 * ============================================================================ */

#include "kernel/klog.h"
#include "kernel/fs/vfs.h"
#include "kernel/mm/pmm.h"
#include "kernel/drivers/serial.h"
#include "kernel/drivers/rtc.h"
#include "kernel/nt/filetime.h"
#include "kernel/time/wall_clock.h"
#include "kernel/timer.h"
#include "kernel/kchecksum.h"     /* kcrc32 for the .lz4 header integrity check */
#include "libs/lz4.h"            /* lz4_compress / lz4_decompress / lz4_compress_bound */
#include "libc/string.h"

/* ---- Runtime log directory (BlackBox X:\ preferred, C:\ fallback) ---- */

const char *klog_dir = KLOG_DIR_FALLBACK;  /* default until resolved */
static char klog_serial_dir[48];           /* klog_dir + "Serial\\" */
int         klog_using_blackbox;           /* 1 if X:\Logs\, 0 if C:\ fallback */

void klog_resolve_dir(void)
{
    int i;
    if (vfs_is_mounted('X')) {
        klog_dir = KLOG_DIR_BLACKBOX;
        klog_using_blackbox = 1;
    } else {
        klog_dir = KLOG_DIR_FALLBACK;
        klog_using_blackbox = 0;
        if (!vfs_is_mounted('C'))
            return;
        klog(LOG_WARN, "klog",
             "BlackBox not mounted, using C:\\ for logs");
    }
    /* Build serial dir path */
    for (i = 0; klog_dir[i] && i < 38; i++)
        klog_serial_dir[i] = klog_dir[i];
    klog_serial_dir[i++] = 'S'; klog_serial_dir[i++] = 'e';
    klog_serial_dir[i++] = 'r'; klog_serial_dir[i++] = 'i';
    klog_serial_dir[i++] = 'a'; klog_serial_dir[i++] = 'l';
    klog_serial_dir[i++] = '\\'; klog_serial_dir[i] = '\0';
}

/* ---- Per-subsystem log dispatch ---- */

typedef struct {
    const char *tag;         /* subsystem tag to match (case-sensitive) */
    const char *filename;    /* file in log directory */
} log_dispatch_entry_t;

static const log_dispatch_entry_t s_dispatch[] = {
    { "net",    "network.log" },
    { "boot",   "boot.log"    },
    { "fs",     "fs.log"      },
    { "mm",     "mm.log"      },
    { "drv",    "drivers.log" },
    { "sec",    "security.log"},
    { "ahci",   "drivers.log" },
    { "pci",    "drivers.log" },
    { "lapic",  "drivers.log" },
    { "ioapic", "drivers.log" },
    { "acpi",   "drivers.log" },
    { "smp",    "boot.log"    },
    { "UEFI",   "boot.log"    },
    { "TPM",    "security.log"},
    { "vfs",    "fs.log"      },
    { "ixfs",   "fs.log"      },
    { "fat32",  "fs.log"      },
    { "blk",    "drivers.log" },
};

#define DISPATCH_COUNT (sizeof(s_dispatch) / sizeof(s_dispatch[0]))

/* Cached VFS handles for subsystem log files (opened once at disk-enable) */
#define SUBSYS_LOG_COUNT 6
static const char *s_subsys_filenames[SUBSYS_LOG_COUNT] = {
    "network.log", "boot.log", "fs.log", "mm.log", "drivers.log", "security.log"
};
static int s_subsys_files_created;

/* Match a subsystem tag to a log filename. Returns "kernel.log" for unmatched. */
static const char *dispatch_filename(const char *subsystem)
{
    uint32_t i;
    if (!subsystem || !subsystem[0])
        return "kernel.log";
    for (i = 0; i < DISPATCH_COUNT; i++) {
        const char *a = s_dispatch[i].tag;
        const char *b = subsystem;
        while (*a && *a == *b) { a++; b++; }
        if (*a == '\0' && (*b == '\0' || *b == ':'))
            return s_dispatch[i].filename;
    }
    return "kernel.log";
}

/* Subsystem-file slot index (0..SUBSYS_LOG_COUNT-1) for a log tag, or -1 when the
 * entry routes only to kernel.log (unmatched tag). Used by single-pass routing to
 * bin each ring entry without re-scanning the ring per file. Pure; exposed for
 * tests (klog_dispatch_slot). */
int klog_dispatch_slot(const char *subsystem)
{
    const char *fn = dispatch_filename(subsystem);
    int i;
    for (i = 0; i < SUBSYS_LOG_COUNT; i++) {
        const char *a = fn, *b = s_subsys_filenames[i];
        while (*a && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0')
            return i;
    }
    return -1;  /* "kernel.log" / unmatched -- no per-subsystem slot */
}

/* ---- State ---- */

/* IXFS (C:) flush tracking -- monotonic sequence cursor */
static uint64_t ixfs_flush_seq;
static int      ixfs_inited;

/* FAT32 (X:) buffer + live mode */
static uint8_t  *fat32_buf      = (void *)0;
static uint32_t  fat32_pos      = 0;
static uint32_t  fat32_buf_size = 0;
static int       fat32_inited   = 0;
static int       live_enabled   = 0;
static int       flushing       = 0;  /* reentrancy guard */

/* Flush progress logging + slow-media detection. */
#define KLOG_SLOW_MEDIA_MS       5000u  /* a flush slower than this = slow media */
#define KLOG_FLUSH_PROGRESS_MIN  64u    /* only log progress for non-trivial flushes */
static int       s_klog_slow_media = 0; /* set when a flush exceeds KLOG_SLOW_MEDIA_MS;
                                         * the deferred-flush mode consumes it */

/* Optional splash-progress callback (registered around the boot-end forced flush).
 * The flush path snapshots it with an atomic acquire load; the setter stores with an
 * atomic release so a flusher on another CPU never sees a torn/transient pointer. */
static klog_flush_progress_fn s_flush_progress_cb;

void klog_disk_set_flush_progress_cb(klog_flush_progress_fn cb)
{
    __atomic_store_n(&s_flush_progress_cb, cb, __ATOMIC_RELEASE);
}

/* Pure: is a flush of `total` entries large enough to warrant a splash progress
 * display? Small flushes finish fast and would only flicker the diagnostic line, so
 * progress is reported only at/above KLOG_FLUSH_PROGRESS_MIN. */
int klog_flush_progress_due(uint32_t total)
{
    return total >= KLOG_FLUSH_PROGRESS_MIN;
}
/* Deferred-flush state in ONE atomic word so the disabled-vs-active transition has a
 * single modification order: once the boot-end forced drain latches DISABLED, the
 * auto-enable CAS can never set ACTIVE again (a concurrent/late slow flush cannot
 * re-arm deferral and strand post-drain logs in RAM). DISABLED is dominant -- see
 * klog_defer_active(). Bit constants are in klog.h (shared with the unit test). */
static uint32_t  s_klog_defer_state = 0;

/* Numbered serial log filename: "Serial_YYMMDDNN.log" */
static char      log_filename[24];  /* "Serial_YYMMDDNN.log" + NUL */

/* Log rotation config (loaded from Registry, or defaults) */
static uint32_t  rot_max_size    = 4 * 1024 * 1024;  /* 4 MB default */
static uint32_t  rot_max_rotated = 3;                  /* keep .1, .2, .3 */
static int       rot_compress    = 1;                  /* LZ4-compress rotated .N to .N.lz4 (boot.conf log_compress) */

/* Per-file size tracking for O(1) rotation check */
static uint32_t  kernel_log_size;  /* tracked across flushes */

/* JSON Lines event log state -- monotonic sequence cursor */
static uint64_t  jsonl_flush_seq;    /* ring entries already flushed */
static uint32_t  jsonl_file_size;    /* tracked for rotation */
static int       jsonl_inited;
static int       jsonl_resync_nl;    /* a prior degraded-media flush left an
                                      * unterminated partial record -- write a
                                      * newline before the next replay so it
                                      * becomes one skipped line, not a merge */

/* ---- Helpers ---- */

static const char *level_str(log_level_t level)
{
    switch (level) {
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO:  return "INFO ";
    case LOG_WARN:  return "WARN ";
    case LOG_ERROR: return "ERROR";
    case LOG_FATAL: return "FATAL";
    default:        return "?????";
    }
}

/* Unsigned 32-bit to decimal string, returns chars written */
static int u32_to_str(uint32_t val, char *buf, int max)
{
    char tmp[12];
    int i = 0, j, len;
    if (val == 0) { buf[0] = '0'; return 1; }
    while (val > 0 && i < 11) {
        tmp[i++] = '0' + (char)(val % 10);
        val /= 10;
    }
    len = i;
    if (len > max) len = max;
    for (j = 0; j < len; j++)
        buf[j] = tmp[len - 1 - j];
    return len;
}

/* ---- Buffer append helpers (for FAT32 full-file buffer) ---- */

static void buf_putc(char c)
{
    if (fat32_buf && fat32_pos < fat32_buf_size - 1)
        fat32_buf[fat32_pos++] = (uint8_t)c;
}

static void buf_puts(const char *s)
{
    while (*s && fat32_buf && fat32_pos < fat32_buf_size - 1)
        fat32_buf[fat32_pos++] = (uint8_t)*s++;
}

static void buf_putu(uint32_t val)
{
    char tmp[12];
    int n = u32_to_str(val, tmp, 12);
    int i;
    for (i = 0; i < n; i++)
        buf_putc(tmp[i]);
}

/* ---- Format a log entry into a line buffer ---- */

static int format_entry(const klog_entry_t *e, char *line, int max)
{
    int pos = 0;

    /* Timestamp: ISO 8601 of the EVENT time if wall clock ready, else the raw
     * unit. e->timestamp is a 10 ms unit from the rebased monotonic source
     * (uptime_ns), captured at emit time; reconstruct its wall time by
     * subtracting the units elapsed since then from now -- using a flush-instant
     * read directly would stamp every flushed line (incl. a replayed/batched
     * burst) with the flush instant, collapsing the timeline. The "now" anchor
     * is the precise wall clock (flush path is not panic context, so the seqlock
     * read is fine and avoids a torn coarse system/interrupt pair); the elapsed
     * delta uses the coarse interrupt-time 10 ms unit -- the SAME unit as the
     * ring writer -- so a KeSetTimerResolution rate change cannot skew the
     * reconstruction. (uint32 modular delta tolerates one wrap.) */
    line[pos++] = '[';
    if (wall_clock_ready()) {
        uint64_t now_ft = (uint64_t)KeQuerySystemTime();
        uint32_t now_units = (uint32_t)(KeQueryInterruptTimeCoarse() / 100000ULL);
        uint32_t elapsed_ticks = now_units - e->timestamp;
        uint64_t elapsed_ft = (uint64_t)elapsed_ticks * 100000ULL; /* 10ms in 100ns */
        FILETIME ft = (now_ft > elapsed_ft) ? (FILETIME)(now_ft - elapsed_ft)
                                            : (FILETIME)now_ft;
        int n = filetime_to_string(ft, line + pos, (uint32_t)(max - pos - 2));
        if (n > 0) pos += n;
    } else {
        pos += u32_to_str(e->timestamp, line + pos, 10);
    }
    line[pos++] = ']';
    line[pos++] = ' ';

    /* Level */
    {
        const char *ls = level_str(e->level);
        int j;
        for (j = 0; ls[j] && pos < max - 16; j++)
            line[pos++] = ls[j];
    }
    line[pos++] = ' ';

    /* Subsystem */
    {
        const char *ss = klog_disk_subsystem(e->subsystem);

        if (!ss)
            ss = "???";
        int j;
        for (j = 0; ss[j] && pos < max - 8; j++)
            line[pos++] = ss[j];
    }
    line[pos++] = ':';
    line[pos++] = ' ';

    /* Message */
    {
        int j;
        for (j = 0; e->message[j] && pos < max - 2; j++)
            line[pos++] = e->message[j];
    }
    line[pos++] = '\n';

    return pos;
}

/* ---- Directory creation helpers ---- */

static void ensure_log_dirs(void)
{
    struct vfs_node *root;

    /* BlackBox (X:\Logs\) dirs are created by the boot skeleton.
     * This function only creates the C:\Impossible\System\Logs\ tree
     * for the fallback path. */
    if (klog_using_blackbox)
        return;

    if (!vfs_is_mounted('C'))
        return;

    root = vfs_get_drive_root('C');
    if (!root || !root->ops || !root->ops->create)
        return;

    /* Create: Impossible, Impossible\System, Impossible\System\Logs */
    root->ops->create(root, "Impossible", VFS_DIRECTORY);
    {
        struct vfs_node *imp = root->ops->finddir(root, "Impossible");
        if (imp && imp->ops && imp->ops->create) {
            imp->ops->create(imp, "System", VFS_DIRECTORY);
            {
                struct vfs_node *sys = imp->ops->finddir(imp, "System");
                if (sys && sys->ops && sys->ops->create) {
                    sys->ops->create(sys, "Logs", VFS_DIRECTORY);

                    /* Create per-subsystem log files */
                    if (!s_subsys_files_created) {
                        struct vfs_node *logs = sys->ops->finddir(sys, "Logs");
                        if (logs && logs->ops && logs->ops->create) {
                            uint32_t si;
                            for (si = 0; si < SUBSYS_LOG_COUNT; si++)
                                logs->ops->create(logs, s_subsys_filenames[si], VFS_FILE);
                            s_subsys_files_created = 1;
                        }
                    }
                }
            }
        }
    }
}

/* ---- Log rotation ----
 *
 * Rotates a log file when it exceeds rot_max_size bytes:
 *   foo.log -> foo.log.1 -> foo.log.2 -> foo.log.3 (deleted)
 *
 * Returns the new file size (0 after rotation, or current_size if no rotation). */

/* Build "<dir><filename>[.<gen>]" into out[cap] with a hard bound. gen 0 = no
 * suffix (the current file); gen 1..9 = ".N" rotated generation; gen 255 = ".tmp"
 * staging suffix. Returns 1 on success, 0 if it would overflow (caller skips the
 * op rather than smashing the stack -- the dir is runtime-resolved by
 * klog_resolve_dir, so an explicit bound + hard-fail is the disk-sourced-config
 * rule, not an implicit "it fits today"). */
static int __attribute__((noinline)) klog_build_log_path(char *out, uint32_t cap,
                          const char *dir, const char *filename, uint8_t gen)
{
    uint32_t p = 0, j;
    for (j = 0; dir[j]; j++)      { if (p + 1 >= cap) return 0; out[p++] = dir[j]; }
    for (j = 0; filename[j]; j++) { if (p + 1 >= cap) return 0; out[p++] = filename[j]; }
    if (gen == 255) {
        const char *t = ".tmp";
        for (j = 0; t[j]; j++)    { if (p + 1 >= cap) return 0; out[p++] = t[j]; }
    } else if (gen >= 1) {
        if (p + 2 >= cap) return 0;
        out[p++] = '.';
        out[p++] = '0' + (char)(gen % 10);
    }
    out[p] = '\0';
    return 1;
}

/* Self-describing header prefixing every `.N.lz4` rotated archive. A raw LZ4
 * block carries no size/integrity metadata, so the on-disk format pins magic,
 * version, the uncompressed/compressed sizes (so the decompressor can size its
 * output and reject truncation), and a CRC32 over the compressed block. */
#define KLOG_LZ4_MAGIC    0x345A4C4Bu   /* "KLZ4" */
#define KLOG_LZ4_VERSION  1u
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t hdr_size;            /* sizeof(klog_lz4_hdr_t) */
    uint32_t uncompressed_size;
    uint32_t compressed_size;
    uint32_t crc32;              /* kcrc32 over the compressed block */
} klog_lz4_hdr_t;
_Static_assert(sizeof(klog_lz4_hdr_t) == 20, "klog_lz4_hdr_t on-disk layout pinned at 20 bytes");
/* Pin every field offset, not just the size: the .N.lz4 format is shared with the
 * host extractor (-> 14-host-tools/TODO-08), so a same-size field reorder must
 * fail the build rather than silently break archive compatibility. Byte order is
 * native little-endian (kernel is permanently x86-64; cross-arch readers are the
 * host extractor's concern, tracked in 14-host-tools/TODO-08). */
_Static_assert(__builtin_offsetof(klog_lz4_hdr_t, magic)             == 0,  "magic at 0");
_Static_assert(__builtin_offsetof(klog_lz4_hdr_t, version)           == 4,  "version at 4");
_Static_assert(__builtin_offsetof(klog_lz4_hdr_t, hdr_size)          == 6,  "hdr_size at 6");
_Static_assert(__builtin_offsetof(klog_lz4_hdr_t, uncompressed_size) == 8,  "uncompressed_size at 8");
_Static_assert(__builtin_offsetof(klog_lz4_hdr_t, compressed_size)   == 12, "compressed_size at 12");
_Static_assert(__builtin_offsetof(klog_lz4_hdr_t, crc32)             == 16, "crc32 at 16");
/* The compress path sizes out_bytes (uint32) as header + lz4_compress_bound(usize)
 * with usize <= LZ4_BLOCK_INPUT_MAX. Pin the worst case under UINT32_MAX so the
 * uint32 narrow + page-count math can never wrap: lz4_compress_bound(n) =
 * n + n/255 + 16 (mirrors LZ4_COMPRESSBOUND). If LZ4_BLOCK_INPUT_MAX is ever
 * raised toward UINT32_MAX this fails the build instead of silently overflowing. */
_Static_assert((uint64_t)sizeof(klog_lz4_hdr_t) + (uint64_t)LZ4_BLOCK_INPUT_MAX +
               (uint64_t)LZ4_BLOCK_INPUT_MAX / 255u + 16u <= 0xFFFFFFFFu,
    "max .N.lz4 archive (header + LZ4 worst-case bound) must fit uint32 out_bytes");

/* Build "<dir><filename>.<gen>.lz4" (dot_tmp -> append ".tmp" staging suffix). */
static int klog_build_lz4_path(char *out, uint32_t cap, const char *dir,
                               const char *fn, uint8_t gen, int dot_tmp)
{
    uint32_t p;
    if (!klog_build_log_path(out, cap, dir, fn, gen))
        return 0;
    for (p = 0; out[p]; p++) {}
    {
        const char *sfx = dot_tmp ? ".lz4.tmp" : ".lz4";
        uint32_t j;
        for (j = 0; sfx[j]; j++) { if (p + 1 >= cap) return 0; out[p++] = (char)sfx[j]; }
    }
    out[p] = '\0';
    return 1;
}

/* Compress `src` into a self-describing [klog_lz4_hdr_t + LZ4 block] archive in
 * dst. Returns the total archive size (header + block), or -1 if dst is too small
 * or compression fails. dst_cap must be >= sizeof(klog_lz4_hdr_t) +
 * lz4_compress_bound(src_size). Pure in-memory half of klog_compress_archive,
 * exposed for the decompress roundtrip unit test. */
int klog_compress_buffer(const void *src, uint32_t src_size,
                         uint8_t *dst, uint32_t dst_cap)
{
    klog_lz4_hdr_t *h = (klog_lz4_hdr_t *)dst;
    int csize;

    if (!src || !dst || dst_cap < sizeof(klog_lz4_hdr_t))
        return -1;
    csize = lz4_compress(src, src_size, dst + sizeof(*h),
                         dst_cap - (uint32_t)sizeof(*h));
    if (csize <= 0)
        return -1;
    h->magic = KLOG_LZ4_MAGIC;
    h->version = (uint16_t)KLOG_LZ4_VERSION;
    h->hdr_size = (uint16_t)sizeof(*h);
    h->uncompressed_size = src_size;
    h->compressed_size = (uint32_t)csize;
    h->crc32 = kcrc32(dst + sizeof(*h), (size_t)csize);
    return (int)((uint32_t)sizeof(*h) + (uint32_t)csize);
}

/* Read `src_path`, LZ4-compress it behind a klog_lz4_hdr_t, and write the result
 * ATOMICALLY to <dir><fn>.1.lz4 (write to .1.lz4.tmp, verify the full write/close,
 * then rename into place). Returns 0 iff a durable compressed archive now exists
 * (the caller may delete src_path); -1 on ANY failure (alloc / read / compress /
 * write), in which case the caller keeps src_path as the plain .1 generation so a
 * rotation never loses the only copy. */
static int klog_compress_archive(const char *dir, const char *fn, const char *src_path)
{
    struct vfs_node *sf = vfs_open(src_path, VFS_O_READ);
    uint32_t usize, in_pages, out_pages, out_bytes;
    size_t   bound;
    uint8_t *in, *out;
    int rc = -1, rd;

    if (!sf)
        return -1;
    /* sf->size is 64-bit; reject empty and anything above the LZ4 single-block
     * maximum BEFORE narrowing to uint32_t. A stale/corrupt oversized staging
     * file must not silently truncate (compressing only a low-32-bit prefix and
     * then deleting the complete .tmp) nor balloon in_pages on a doomed alloc.
     * On rejection the caller keeps the plain .tmp -> .1 path (no data loss). */
    if (sf->size == 0 || sf->size > (uint64_t)LZ4_BLOCK_INPUT_MAX) {
        vfs_close(sf);
        return -1;
    }
    usize = (uint32_t)sf->size;

    in_pages  = (usize + 4095u) / 4096u;
    bound     = lz4_compress_bound(usize);
    out_bytes = (uint32_t)(sizeof(klog_lz4_hdr_t) + bound);
    out_pages = (out_bytes + 4095u) / 4096u;
    in  = (uint8_t *)(uintptr_t)pmm_alloc_contiguous(in_pages);
    out = in ? (uint8_t *)(uintptr_t)pmm_alloc_contiguous(out_pages) : (uint8_t *)0;
    if (!in || !out) {
        uint32_t p;
        if (in)  for (p = 0; p < in_pages;  p++) pmm_free_frame((uintptr_t)in  + p * 4096);
        if (out) for (p = 0; p < out_pages; p++) pmm_free_frame((uintptr_t)out + p * 4096);
        vfs_close(sf);
        return -1;  /* low memory -> fall back to plain .N (no data loss) */
    }

    rd = vfs_read(sf, 0, usize, in);
    vfs_close(sf);
    if (rd == (int)usize) {
        int total = klog_compress_buffer(in, usize, out, out_bytes);
        if (total > 0) {
            char tmp_path[96], final_path[96];

            if (klog_build_lz4_path(final_path, sizeof final_path, dir, fn, 1, 0) &&
                klog_build_lz4_path(tmp_path,   sizeof tmp_path,   dir, fn, 1, 1)) {
                struct vfs_node *af;
                vfs_unlink(tmp_path);
                af = vfs_open(tmp_path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
                if (af) {
                    int wr = vfs_write(af, 0, total, out);
                    /* Durable boundary: the .1.lz4 bytes exist nowhere else, and
                     * klog_archive_stage deletes the staged plain copy on rc==0,
                     * so the archive must reach stable storage BEFORE it becomes
                     * the sole copy. A write left in the FS cache could leave
                     * .1.lz4 truncated/replaced after a reset while the stage is
                     * already gone -- same flush discipline as klog.c
                     * crash_recovery.log and panic.c last-panic.txt. The rename
                     * then publishes those durable bytes under the final name at
                     * the SAME metadata-durability level as the plain-.N
                     * fallback's stage->.1 rename (pre-existing rotation
                     * convention), so no second flush is needed: this function
                     * never deletes src_path, so any post-flush commit failure
                     * falls back to a plain .1 from the intact stage with zero
                     * data loss. */
                    int fl = vfs_flush(af);
                    vfs_close(af);
                    if (wr == (int)total && fl == 0) {
                        vfs_unlink(final_path);  /* clear any stale .1.lz4 */
                        if (vfs_rename(tmp_path, final_path) == 0)
                            rc = 0;  /* durable compressed archive committed */
                    }
                    if (rc != 0)
                        vfs_unlink(tmp_path);  /* drop the partial staging file */
                }
            }
        }
    }
    {
        uint32_t p;
        for (p = 0; p < in_pages;  p++) pmm_free_frame((uintptr_t)in  + p * 4096);
        for (p = 0; p < out_pages; p++) pmm_free_frame((uintptr_t)out + p * 4096);
    }
    return rc;
}

/* Decompress a `.N.lz4` archive (header + LZ4 block) into dst. Validates magic,
 * version, header size, bounds, the CRC32 of the compressed block, and that the
 * decode produced exactly uncompressed_size bytes. Returns the decompressed byte
 * count, or -1 on any malformed/corrupt/truncated input. For the in-OS viewer and
 * the host extractor (-> XREF: 14-host-tools/TODO-08). */
int klog_decompress_rotated(const void *src, uint32_t src_size,
                            uint8_t *dst, uint32_t dst_cap)
{
    const klog_lz4_hdr_t *h;
    const uint8_t *block;
    int n;

    if (!src || !dst || src_size < sizeof(klog_lz4_hdr_t))
        return -1;
    h = (const klog_lz4_hdr_t *)src;
    if (h->magic != KLOG_LZ4_MAGIC || h->version != KLOG_LZ4_VERSION ||
        h->hdr_size != sizeof(klog_lz4_hdr_t))
        return -1;
    if ((uint64_t)h->hdr_size + (uint64_t)h->compressed_size > (uint64_t)src_size)
        return -1;
    if (h->uncompressed_size > dst_cap)
        return -1;
    block = (const uint8_t *)src + h->hdr_size;
    if (kcrc32(block, h->compressed_size) != h->crc32)
        return -1;
    /* Bound the decode by the archive-declared uncompressed_size, NOT the
     * caller's (possibly larger) scratch capacity. A valid-CRC but hostile
     * block could declare a small uncompressed_size yet expand toward dst_cap;
     * capping at uncompressed_size makes that over-expansion a clean
     * LZ4_decompress_safe failure instead of clobbering bytes past the recorded
     * payload in a larger viewer/extractor buffer. */
    n = lz4_decompress(block, h->compressed_size, dst, h->uncompressed_size);
    if (n < 0 || (uint32_t)n != h->uncompressed_size)
        return -1;
    return n;
}

/* Existence probe: open read-only, close, report whether it was there. The flush
 * body is single-threaded (the `flushing` reentrancy guard), so TOCTOU between
 * this probe and the follow-up rename/unlink is not a concern here. */
static int klog_path_exists(const char *path)
{
    struct vfs_node *n = vfs_open(path, VFS_O_READ);
    if (n) { vfs_close(n); return 1; }
    return 0;
}

/* Is generation slot 1 occupied in EITHER family (.1 or .1.lz4)? Used to gate the
 * generation shift so orphan recovery is idempotent: an interruption AFTER the
 * shift already freed slot 1 must NOT shift a second time (that would age out an
 * extra generation). Shift only when slot 1 actually needs freeing. */
static int klog_slot1_occupied(const char *dir, const char *filename)
{
    char p[80];
    if (klog_build_log_path(p, sizeof p, dir, filename, 1) && klog_path_exists(p))
        return 1;
    if (klog_build_lz4_path(p, sizeof p, dir, filename, 1, 0) && klog_path_exists(p))
        return 1;
    return 0;
}

/* Distinguish reset window W3 (compress committed .1.lz4 but the stage delete was
 * lost to a reset, so slot 1 ALREADY holds this orphan) from W1 (the stage is a
 * genuinely new orphan and slot 1 is an OLDER generation). Returns 1 only when
 * generation slot 1 holds a byte-exact copy of the log at stage_path (a .1.lz4 is
 * decompressed first). On ANY read/alloc failure returns 0 -- the safe bias:
 * recovery then preserves the orphan (shift + re-archive, at worst one bounded
 * duplicate) instead of risking deletion of still-un-archived data. Recovery-only
 * and rare (a crash mid-rotation), so the transient double buffer is acceptable.
 *
 * Identity by bytes (not a transaction id) is deliberate and lossless: the W3
 * unlink only fires when the orphan is byte-for-byte equal to slot 1, so the
 * identical bytes are already retained there -- dropping the orphan loses zero log
 * INFORMATION even in the (practically unreachable for monotonic-timestamped logs)
 * case of two distinct rotations producing identical content. A per-rotation nonce
 * would buy nothing here and would force a header onto the plain .N family + the
 * host extractor, so byte equality is the right check. */
static int klog_orphan_already_archived(const char *dir, const char *filename,
                                        const char *stage_path)
{
    struct vfs_node *sf, *af;
    char path[80];
    uint8_t *sbuf = 0, *cbuf = 0, *dbuf = 0;
    uint32_t ssize, spages = 0, cpages = 0;
    int match = 0, rd = -1;

    sf = vfs_open(stage_path, VFS_O_READ);
    if (!sf)
        return 0;
    if (sf->size == 0 || sf->size > (uint64_t)LZ4_BLOCK_INPUT_MAX) {
        vfs_close(sf);
        return 0;
    }
    ssize  = (uint32_t)sf->size;
    spages = (ssize + 4095u) / 4096u;
    sbuf   = (uint8_t *)(uintptr_t)pmm_alloc_contiguous(spages);
    if (sbuf)
        rd = vfs_read(sf, 0, ssize, sbuf);
    vfs_close(sf);
    if (rd != (int)ssize)
        goto out;

    /* Compressed slot 1: read the whole archive, decompress into dbuf (<= ssize),
     * byte-compare. */
    if (klog_build_lz4_path(path, sizeof path, dir, filename, 1, 0) &&
        (af = vfs_open(path, VFS_O_READ)) != 0) {
        uint32_t csize = (uint32_t)af->size;
        /* A genuine same-log archive decompresses to ssize, so its on-disk size
         * is at most header + lz4_compress_bound(ssize). Bound by THAT, not the
         * raw input ceiling LZ4_BLOCK_INPUT_MAX: for a near-ceiling incompressible
         * log the worst-case archive exceeds LZ4_BLOCK_INPUT_MAX, and the input
         * ceiling would wrongly reject a legal archive this writer can produce --
         * making recovery miss a committed-but-stage-not-deleted (W3) duplicate. */
        uint64_t max_arc = (uint64_t)sizeof(klog_lz4_hdr_t) + lz4_compress_bound(ssize);
        if (af->size > (uint64_t)sizeof(klog_lz4_hdr_t) && af->size <= max_arc) {
            cpages = (csize + 4095u) / 4096u;
            cbuf   = (uint8_t *)(uintptr_t)pmm_alloc_contiguous(cpages);
            dbuf   = cbuf ? (uint8_t *)(uintptr_t)pmm_alloc_contiguous(spages) : 0;
            if (cbuf && dbuf && vfs_read(af, 0, csize, cbuf) == (int)csize) {
                int dn = klog_decompress_rotated(cbuf, csize, dbuf, ssize);
                if (dn == (int)ssize && memcmp(dbuf, sbuf, ssize) == 0)
                    match = 1;
            }
        }
        vfs_close(af);
        goto out;
    }
    /* Plain slot 1: same size + direct byte compare (cbuf is the read buffer). */
    if (klog_build_log_path(path, sizeof path, dir, filename, 1) &&
        (af = vfs_open(path, VFS_O_READ)) != 0) {
        if ((uint64_t)af->size == (uint64_t)ssize) {
            cbuf   = (uint8_t *)(uintptr_t)pmm_alloc_contiguous(spages);
            cpages = cbuf ? spages : 0;
            if (cbuf && vfs_read(af, 0, ssize, cbuf) == (int)ssize &&
                memcmp(cbuf, sbuf, ssize) == 0)
                match = 1;
        }
        vfs_close(af);
    }
out:
    {
        uint32_t p;
        if (sbuf) for (p = 0; p < spages; p++) pmm_free_frame((uintptr_t)sbuf + p * 4096);
        if (cbuf) for (p = 0; p < cpages; p++) pmm_free_frame((uintptr_t)cbuf + p * 4096);
        if (dbuf) for (p = 0; p < spages; p++) pmm_free_frame((uintptr_t)dbuf + p * 4096);
    }
    return match;
}

/* Shift the rotated-generation chain down one slot, freeing slot 1. Drops the
 * oldest generation of BOTH families (.N and .N.lz4) and renames each .N-1 ->
 * .N for both. Shifting both families keeps a Compress toggle between boots from
 * stranding generations outside the retention chain. Best-effort: a generation
 * legitimately may not exist in a given family. */
static void klog_shift_generations(const char *dir, const char *filename)
{
    char a_path[80], b_path[80];
    uint32_t n;

    if (klog_build_log_path(a_path, sizeof a_path, dir, filename, (uint8_t)rot_max_rotated))
        vfs_unlink(a_path);
    if (klog_build_lz4_path(a_path, sizeof a_path, dir, filename, (uint8_t)rot_max_rotated, 0))
        vfs_unlink(a_path);
    for (n = rot_max_rotated; n >= 2; n--) {
        if (klog_build_log_path(a_path, sizeof a_path, dir, filename, (uint8_t)(n - 1)) &&
            klog_build_log_path(b_path, sizeof b_path, dir, filename, (uint8_t)n))
            vfs_rename(a_path, b_path);
        if (klog_build_lz4_path(a_path, sizeof a_path, dir, filename, (uint8_t)(n - 1), 0) &&
            klog_build_lz4_path(b_path, sizeof b_path, dir, filename, (uint8_t)n, 0))
            vfs_rename(a_path, b_path);
    }
}

/* Commit the staged log at `stage_path` into generation slot 1 (assumed empty
 * after a shift). With compression on, klog_compress_archive writes .1.lz4
 * ATOMICALLY; on ANY compression failure it returns -1 and we fall back to
 * renaming the stage to a plain .1. Returns 0 iff the staged data now lives
 * durably in slot 1 AND `stage_path` has been consumed; -1 iff `stage_path` is
 * still the only complete copy (caller MUST keep it).
 *
 * Strict post-condition (no orphan ambiguity): on return 0 exactly one copy
 * exists, in slot 1, and the stage is gone; on return -1 exactly one copy
 * exists, at `stage_path`, and slot 1 holds no NEW copy. The compress path
 * enforces this: if .1.lz4 committed but the stage will not delete (rare FS
 * error), it ROLLS BACK the .1.lz4 so the sole copy stays at `stage_path` --
 * never leaving the same log in both places for Phase 0 to re-archive as a
 * duplicate. The plain path's rename is atomic, so it cannot leave a leftover. */
static int klog_archive_stage(const char *dir, const char *filename,
                              const char *stage_path)
{
    char a_path[80];

    if (rot_compress && klog_compress_archive(dir, filename, stage_path) == 0) {
        vfs_unlink(stage_path);            /* drop the staged plain copy */
        if (!klog_path_exists(stage_path))
            return 0;                      /* .1.lz4 durable AND stage consumed */
        /* .1.lz4 committed but the stage would not delete. Roll the archive back
         * so the only copy stays in `stage_path`; a later pass retries cleanly
         * rather than duplicating the generation. */
        if (klog_build_lz4_path(a_path, sizeof a_path, dir, filename, 1, 0))
            vfs_unlink(a_path);
        return -1;
    }
    if (klog_build_log_path(a_path, sizeof a_path, dir, filename, 1) &&
        vfs_rename(stage_path, a_path) == 0)
        return 0;  /* plain .1 committed (rename consumed the stage) */
    return -1;     /* nothing committed -- stage_path is the only copy, keep it */
}

static uint32_t rotate_log_file(const char *dir, const char *filename,
                                uint32_t current_size)
{
    char cur_path[80], tmp_path[80];

    if (current_size < rot_max_size)
        return current_size;

    if (!klog_build_log_path(cur_path, sizeof cur_path, dir, filename, 0) ||
        !klog_build_log_path(tmp_path, sizeof tmp_path, dir, filename, 255))
        return current_size;  /* path too long -- skip rather than overflow */

    /* Phase 0 -- recover an orphaned stage from an interrupted prior rotation.
     * A reset (or compression interruption) between staging current->.tmp and
     * committing slot 1 can leave the ONLY complete copy of that log in .tmp.
     * Earlier this path unconditionally unlinked the stale .tmp, discarding it.
     * Instead, fold it into the retention chain (shift, then archive .tmp->.1)
     * before starting a new rotation. If recovery cannot commit (archive fails:
     * full / read-only media), KEEP .tmp and skip this rotation -- the live log
     * keeps growing but no rotated log is lost; a later flush retries. Safe to
     * klog here: the flush body runs under the `flushing` reentrancy guard. */
    if (klog_path_exists(tmp_path)) {
        if (klog_orphan_already_archived(dir, filename, tmp_path)) {
            /* W3: the orphan is ALREADY durably archived in slot 1 -- a reset lost
             * only the stage delete. Drop the duplicate stage; do NOT shift or
             * re-archive (that would duplicate the log and age out a generation). */
            vfs_unlink(tmp_path);
        } else {
            /* W1/W2: a genuinely un-archived orphan. Shift ONLY if slot 1 is still
             * occupied -- an interruption AFTER the shift already freed it, and
             * shifting again would age out an extra generation; archive straight
             * into the empty slot 1 in that case. */
            if (klog_slot1_occupied(dir, filename))
                klog_shift_generations(dir, filename);
            if (klog_archive_stage(dir, filename, tmp_path) != 0) {
                klog(LOG_WARN, "klog",
                     "log rotation: orphan stage of '%s' kept; recovery retried later",
                     filename);
                return current_size;
            }
        }
    }

    /* Phase 1 -- stage the LIVE log aside BEFORE any destructive op by renaming
     * current->.tmp (no orphan remains after Phase 0). If this rename fails (the
     * persistent failure mode: read-only / full media), NOTHING destructive has
     * run, so the existing rotated generations are untouched and a retry next
     * flush does not churn or discard them; the live log is still at its path. */
    if (vfs_rename(cur_path, tmp_path) != 0) {
        klog(LOG_WARN, "klog",
             "log rotation: stage '%s' failed; rotations untouched, keeping current",
             filename);
        return current_size;
    }

    /* Phase 2 -- live data is safe in .tmp, so the destructive shift + archive
     * run only here. Shift only to free an occupied slot 1 (same idempotent gate
     * as Phase 0). On archive failure the data stays in .tmp (recovered by the
     * next rotation's Phase 0), so a rotation NEVER loses the only copy. */
    if (klog_slot1_occupied(dir, filename))
        klog_shift_generations(dir, filename);
    if (klog_archive_stage(dir, filename, tmp_path) != 0)
        klog(LOG_WARN, "klog",
             "log rotation: archive staged copy of '%s' to slot 1 failed", filename);

    /* Fresh empty current file. */
    {
        struct vfs_node *f = vfs_open(cur_path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
        if (f) vfs_close(f);
    }

    return 0;  /* size reset to 0 */
}

/* Load rotation config from Registry (called after registry_init) */
static void load_rotation_config(void)
{
    extern long RegReadKeyValue(void *hRootKey, const char *lpPath,
                                const char *lpValueName, uint32_t *lpType,
                                uint8_t *lpData, uint32_t *lpcbData);
    uint32_t val, val_type, val_size;

    val_size = sizeof(val);
    val_type = 0;
    if (RegReadKeyValue((void *)(uintptr_t)0x80000002,
                        "SYSTEM\\Logs", "MaxSize",
                        &val_type, (uint8_t *)&val, &val_size) == 0 &&
        val_type == 4 && val_size == sizeof(val) && val > 0) {  /* REG_DWORD = 4 */
        /* Clamp to the LZ4 single-block input ceiling (also well under the
         * uint32 VFS offset limit). The append path uses uint32 offsets and the
         * compress path rejects a staged log above LZ4_BLOCK_INPUT_MAX, so an
         * unclamped MaxSize near UINT32_MAX would let the log grow toward the
         * 4 GiB FAT32 boundary AND force the compress path into its plain-.N
         * fallback. Bounding the rotation trigger keeps both paths in range. */
        rot_max_size = (val > LZ4_BLOCK_INPUT_MAX) ? LZ4_BLOCK_INPUT_MAX : val;
    }

    val_size = sizeof(val);
    val_type = 0;
    if (RegReadKeyValue((void *)(uintptr_t)0x80000002,
                        "SYSTEM\\Logs", "MaxRotated",
                        &val_type, (uint8_t *)&val, &val_size) == 0 &&
        val_type == 4 && val_size == sizeof(val) && val > 0 && val <= 9)
        rot_max_rotated = val;

    /* log_compress: LZ4-compress rotated generations to .N.lz4 (default on). 0
     * keeps plain .N. Registry-driven like MaxSize/MaxRotated (HKLM\SYSTEM\Logs). */
    val_size = sizeof(val);
    val_type = 0;
    if (RegReadKeyValue((void *)(uintptr_t)0x80000002,
                        "SYSTEM\\Logs", "Compress",
                        &val_type, (uint8_t *)&val, &val_size) == 0 &&
        val_type == 4 && val_size == sizeof(val))
        rot_compress = (val != 0);
}

/* ---- Date-stamped log files on X: (FAT32) ----
 *
 * Filename format: YYMMDDnn.LOG  (8.3 FAT32-safe)
 *   YY   = 2-digit year   (from RTC)
 *   MM   = 2-digit month  (01–12)
 *   DD   = 2-digit day    (01–31)
 *   nn   = sequence within day (01–99)
 *
 * Example: 26031802.LOG = March 18, 2026, second boot of the day
 */

/* Helper: build a "YYMMDDnn.LOG" filename */
/* Serial log subdirectory (built at runtime by klog_resolve_dir) */
#define KLOG_SERIAL_DIR  klog_serial_dir

static void make_log_filename(uint8_t yy, uint8_t mm, uint8_t dd,
                               uint8_t seq, char *out)
{
    /* Format: "Serial_YYMMDDNN.log" (19 chars + NUL) */
    out[0]  = 'S'; out[1]  = 'e'; out[2]  = 'r'; out[3]  = 'i';
    out[4]  = 'a'; out[5]  = 'l'; out[6]  = '_';
    out[7]  = '0' + (char)(yy / 10);
    out[8]  = '0' + (char)(yy % 10);
    out[9]  = '0' + (char)(mm / 10);
    out[10] = '0' + (char)(mm % 10);
    out[11] = '0' + (char)(dd / 10);
    out[12] = '0' + (char)(dd % 10);
    out[13] = '0' + (char)(seq / 10);
    out[14] = '0' + (char)(seq % 10);
    out[15] = '.'; out[16] = 'l'; out[17] = 'o'; out[18] = 'g';
    out[19] = '\0';
}

/* Parse "Serial_YYMMDDnn.log" -> 1 if valid log file, fills out fields.
 * Returns 0 if not a log file. */
static int parse_log_filename(const char *name, uint8_t *yy, uint8_t *mm,
                               uint8_t *dd, uint8_t *seq)
{
    int i;

    /* Must start with "Serial_" (7 chars) */
    if (name[0] != 'S' || name[1] != 'e' || name[2] != 'r' ||
        name[3] != 'i' || name[4] != 'a' || name[5] != 'l' || name[6] != '_')
        return 0;

    /* Then 8 digits */
    for (i = 7; i < 15; i++)
        if (name[i] < '0' || name[i] > '9') return 0;

    /* Then ".log" */
    if (name[15] != '.' || name[16] != 'l' || name[17] != 'o' || name[18] != 'g')
        return 0;
    if (name[19] != '\0') return 0;

    *yy  = (uint8_t)((name[7]  - '0') * 10 + (name[8]  - '0'));
    *mm  = (uint8_t)((name[9]  - '0') * 10 + (name[10] - '0'));
    *dd  = (uint8_t)((name[11] - '0') * 10 + (name[12] - '0'));
    *seq = (uint8_t)((name[13] - '0') * 10 + (name[14] - '0'));

    if (*mm < 1 || *mm > 12 || *dd < 1 || *dd > 31) return 0;
    return 1;
}

/* Compare two log dates: returns <0 (a earlier), 0 (equal), >0 (a later) */
static int log_date_cmp(uint8_t ya, uint8_t ma, uint8_t da, uint8_t sa,
                         uint8_t yb, uint8_t mb, uint8_t db, uint8_t sb)
{
    uint32_t a = ((uint32_t)ya << 24) | ((uint32_t)ma << 16)
               | ((uint32_t)da << 8) | sa;
    uint32_t b = ((uint32_t)yb << 24) | ((uint32_t)mb << 16)
               | ((uint32_t)db << 8) | sb;
    if (a < b) return -1;
    if (a > b) return  1;
    return 0;
}

#define KLOG_MAX_LOG_FILES 100

/* Scan C:\Impossible\System\Logs\ for log files, pick today's next sequence, enforce 100-file cap. */
static void pick_log_number(void)
{
    struct vfs_node *x_root;  /* legacy name -- actually the Logs directory */
    uint8_t today_yy, today_mm, today_dd;
    uint8_t max_seq_today = 0;
    uint32_t count = 0;

    /* Oldest log tracking (for cap enforcement). Serial filenames are
     * "Serial_YYMMDDNN.log" = 19 chars + NUL; a short buffer would truncate
     * off ".log" and the unlink would silently miss, defeating the 100-file
     * cap on every platform. */
    uint8_t old_yy = 99, old_mm = 12, old_dd = 31, old_seq = 99;
    char    oldest_name[20];
    oldest_name[0] = '\0';

    /* Get current date from the RTC. rtc_try_read() honors the CMOS-absence
     * gate and validates the tuple, so it returns 0 on a no-CMOS platform
     * instead of the year-0 zero-fill the void rtc_read() leaves. The wall
     * clock is not seeded yet at this boot point (klog_disk_enable runs before
     * wall_clock_init), so the RTC is the only date source here. With no RTC,
     * fall back to a parser-VALID sentinel (00/01/01): month/day 0 would be
     * rejected by parse_log_filename(), reusing Serial_00000001.log every boot.
     * The sentinel keeps the filename valid and bounds growth to the 100-file
     * cap; recency-correct rotation on the degenerate fixed date (the YYMMDDNN
     * scheme cannot encode a cross-boot counter) is owned by the no-RTC serial
     * log rotation item in the system-logging TODO. */
    {
        struct rtc_time rt;
        if (rtc_try_read(&rt)) {
            today_yy = (uint8_t)(rt.year % 100);
            today_mm = (uint8_t)rt.month;
            today_dd = (uint8_t)rt.day;
        } else {
            today_yy = 0;
            today_mm = 1;
            today_dd = 1;
        }
    }

    x_root = vfs_open(KLOG_SERIAL_DIR, VFS_O_READ);
    if (!x_root || !x_root->ops || !x_root->ops->finddir)
        goto fallback;

    /* Enumerate Serial directory to find all log files */
    {
        uint32_t dir_idx = 0;
        struct vfs_dirent *de;
        while ((de = vfs_readdir(x_root, dir_idx)) != 0) {
            uint8_t yy, mm, dd, seq;
            if (parse_log_filename(de->name, &yy, &mm, &dd, &seq)) {
                count++;

                /* Track highest sequence number for today */
                if (yy == today_yy && mm == today_mm && dd == today_dd) {
                    if (seq > max_seq_today)
                        max_seq_today = seq;
                }

                /* Track oldest file for deletion when capped */
                if (log_date_cmp(yy, mm, dd, seq,
                                 old_yy, old_mm, old_dd, old_seq) < 0) {
                    old_yy = yy; old_mm = mm; old_dd = dd; old_seq = seq;
                    {
                        int k;
                        for (k = 0; de->name[k] && k < 19; k++)
                            oldest_name[k] = de->name[k];
                        oldest_name[k] = '\0';
                    }
                }
            }
            dir_idx++;
        }
    }

    /* Cap at 100 files: delete oldest when full */
    if (count >= KLOG_MAX_LOG_FILES && x_root->ops->unlink && oldest_name[0]) {
        int urc = x_root->ops->unlink(x_root, oldest_name);
        if (urc == 0)
            klog(LOG_DEBUG, "klog", "deleted oldest log: %s%s (%u files)",
                 klog_dir, oldest_name, count);
        else
            klog(LOG_WARN, "klog", "log cap: unlink %s%s failed (rc=%d, %u)",
                 klog_dir, oldest_name, (uint64_t)urc, count);
    }

fallback:
    /* Build today's filename with next sequence number */
    {
        uint8_t next_seq = max_seq_today + 1;
        if (next_seq > 99) next_seq = 99;  /* safety clamp */

        make_log_filename(today_yy, today_mm, today_dd, next_seq, log_filename);
        klog(LOG_INFO, "klog", "writing to %s%s", klog_dir, log_filename);
    }
}

/* ---- Public API ---- */

void klog_disk_init(void)
{
    /* Idempotent: the C:\DEBUG path runs klog_disk_set_live(1) (which inits when
     * fat32_buf is absent) BEFORE the Phase 2 klog_disk_enable() that also calls
     * here. Without this guard the second call would overwrite fat32_buf,
     * leaking the first 256 KiB buffer and discarding the replayed live log. */
    if (fat32_buf)
        return;

    /* Allocate FAT32 buffer: 64 pages = 256 KB */
    uint32_t pages = 64;
    fat32_buf = (uint8_t *)pmm_alloc_contiguous(pages);
    if (!fat32_buf)
        return;

    fat32_buf_size = pages * 4096;
    fat32_pos = 0;

    /* Resolve log directory: X:\Logs\ (BlackBox) or C:\ fallback */
    klog_resolve_dir();

    /* Determine numbered log filename */
    if (vfs_is_mounted('X') || vfs_is_mounted('C')) {
        pick_log_number();

        /* Create the log file in the Serial subdirectory */
        {
            struct vfs_node *serial_dir = vfs_open(KLOG_SERIAL_DIR, VFS_O_READ);
            if (serial_dir && serial_dir->ops && serial_dir->ops->create)
                serial_dir->ops->create(serial_dir, log_filename, VFS_FILE);
        }

        fat32_inited = 1;
    }
}

void klog_disk_set_live(int on)
{
    /* Ensure buffer is allocated */
    if (!fat32_buf && on)
        klog_disk_init();

    live_enabled = on;

    if (on) {
        /* Replay existing ring buffer into the FAT32 buffer */
        uint32_t ring_count, ring_head;
        const klog_entry_t *ring = klog_get_ring(&ring_count, &ring_head);
        if (ring && ring_count > 0) {
            uint32_t i;
            uint32_t start = (ring_count > KLOG_RING_SIZE)
                             ? ring_count - KLOG_RING_SIZE : 0;
            for (i = start; i < ring_count; i++) {
                uint32_t idx;
                if (ring_count < KLOG_RING_SIZE) {
                    idx = i;
                } else {
                    idx = (ring_head + (i - (ring_count - KLOG_RING_SIZE)))
                          % KLOG_RING_SIZE;
                }
                klog_disk_append(&ring[idx]);
            }
        }
    }
}

int klog_disk_live_active(void)
{
    return live_enabled;
}

/* 1 iff disk logging is actually persisting: the FAT32 buffer is allocated AND
 * a log-target volume (X: BlackBox or C:) is mounted. Lets the boot-path
 * distinguish "klog_disk_enable attempted" from "disk logging is live" so a
 * silent alloc-fail / no-mount does not get reported as success. */
int klog_disk_active(void)
{
    return fat32_buf != NULL && (vfs_is_mounted('X') || vfs_is_mounted('C'));
}

void klog_disk_append(const klog_entry_t *e)
{
    if (!fat32_buf)
        return;

    /* Format entry into the FAT32 buffer */
    buf_putc('[');
    buf_putu(e->timestamp);
    buf_puts("] ");
    buf_puts(level_str(e->level));
    buf_putc(' ');
    {
        /* Aliased: an authenticating tag must not reach a ring-3-readable
         * sink. No-op for every ordinary subsystem. */
        const char *ss = klog_disk_subsystem(e->subsystem);

        if (ss && ss[0]) {
            buf_puts(ss);
            buf_puts(": ");
        }
    }
    buf_puts(e->message);
    buf_putc('\n');
}

/* Format an unsigned decimal directly to serial (no klog -- avoids re-entering
 * the disk flush under the reentrancy guard). */
static void serial_write_u32(uint32_t v)
{
    char tmp[12];
    int t = 0;
    if (v == 0) { serial_putchar('0'); return; }
    while (v) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
    while (t) serial_putchar(tmp[--t]);
}

/* True once a disk flush has exceeded KLOG_SLOW_MEDIA_MS. The deferred-flush mode
 * reads this to switch to batched RAM buffering on slow boot media. */
int klog_slow_media_detected(void)
{
    return __atomic_load_n(&s_klog_slow_media, __ATOMIC_RELAXED);
}

/* Bounded flush window: how many unflushed ring entries to write this pass. The
 * cap to KLOG_RING_SIZE is the core bound -- without it a saturated ring (cur_seq
 * far ahead of the cursor) produced the 10-minute USB 2.0 flush hang. `cursor` is
 * the persisted seq of the last successful flush; `cur_seq` is the live monotonic
 * sequence. The cur_seq < cursor guard keeps the result sane under any reordering.
 * Pure; exposed for tests. */
uint32_t klog_flush_window(uint64_t cur_seq, uint64_t cursor)
{
    uint64_t pend = cur_seq > cursor ? cur_seq - cursor : 0;
    return pend > KLOG_RING_SIZE ? KLOG_RING_SIZE : (uint32_t)pend;
}

/* Entries lost to ring overflow: unflushed entries beyond KLOG_RING_SIZE were
 * overwritten in the ring and can never reach disk. Deferred mode (which stops
 * advancing the cursor until boot end) widens this window, so the count is logged
 * to keep the persisted log from looking complete. Pure; exposed for tests. */
uint32_t klog_lost_count(uint64_t cur_seq, uint64_t cursor)
{
    uint64_t pend = cur_seq > cursor ? cur_seq - cursor : 0;
    uint64_t lost = pend > KLOG_RING_SIZE ? pend - KLOG_RING_SIZE : 0;
    /* Saturate rather than truncate: a 64-bit loss past UINT32_MAX must not wrap to
     * a small (or zero) count and hide a catastrophic log-loss condition. */
    return lost > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)lost;
}

/* Run the actual flush. The caller MUST already own the `flushing` guard (this is
 * the shared body behind klog_disk_flush() and the forced klog_disk_flush_all()). */
static void klog_disk_flush_locked(void)
{
    uint32_t ring_count, ring_head;
    const klog_entry_t *ring;
    struct vfs_node *logfile;
    uint32_t i;

    /* Flush timing + progress: measure wall-clock across the whole flush and, for
     * non-trivial flushes, report start/done on serial so a slow USB 2.0 write is
     * visibly progressing rather than appearing hung. `flush_pending` is the
     * kernel.log unflushed count (seq cursor minus snapshot, capped to the ring).
     * uptime_ns() is monotonic; if the timer HAL is not yet up both reads are
     * equal so the elapsed-ms is 0 (no false slow-media trip). `flush_persisted`
     * tracks whether the kernel.log write actually succeeded so the done-path
     * diagnostic never reports success over a failed/partial persistence. */
    uint64_t flush_t0   = uptime_ns();
    int      log_mounted = vfs_is_mounted('X') || vfs_is_mounted('C');
    int      flush_persisted = 1;
    uint32_t flush_pending = 0;
    if (log_mounted) {
        uint64_t cs = klog_get_seq();
        flush_pending = klog_flush_window(cs, ixfs_flush_seq);
        /* Ring-overflow loss: if more than KLOG_RING_SIZE entries went unflushed
         * (deferred mode can accumulate a long boot tail), the oldest were
         * overwritten in the ring and can never reach disk -- log the count so the
         * persisted log is not silently incomplete. */
        {
            uint32_t lost = klog_lost_count(cs, ixfs_flush_seq);
            if (lost) {
                serial_write("[WARN] [KLOG] ");
                serial_write_u32(lost);
                serial_write(" early log entries lost (ring overflow before flush)\n");
            }
        }
        if (flush_pending >= KLOG_FLUSH_PROGRESS_MIN) {
            serial_write("[KLOG] Flushing ");
            serial_write_u32(flush_pending);
            serial_write(" entries to disk...\n");
        }
    }

    /* ---- Flush to log directory (X:\ BlackBox or C:\ fallback) ---- */
    if (vfs_is_mounted('X') || vfs_is_mounted('C')) {
        if (!ixfs_inited) {
            ensure_log_dirs();
            load_rotation_config();
            ixfs_inited = 1;
            ixfs_flush_seq = 0;

            /* Create kernel.log via klog_dir path AND seed kernel_log_size from its
             * real on-disk size: a warm reboot can leave an already-oversized
             * kernel.log; seeding from 0 would skip the first-flush rotate and let
             * it grow past MaxSize until the post-write size update catches up. */
            {
                char kl_path[64];
                int kp = 0, kj;
                struct vfs_node *kf;
                for (kj = 0; klog_dir[kj]; kj++) kl_path[kp++] = klog_dir[kj];
                { const char *fn = "kernel.log";
                  for (kj = 0; fn[kj]; kj++) kl_path[kp++] = fn[kj]; }
                kl_path[kp] = '\0';
                vfs_create(kl_path, VFS_FILE);
                kf = vfs_open(kl_path, VFS_O_WRITE | VFS_O_CREATE);
                if (kf) { kernel_log_size = (uint32_t)kf->size; vfs_close(kf); }
            }
        }

        /* Rotate kernel.log if it exceeds max size */
        kernel_log_size = rotate_log_file(
            klog_dir, "kernel.log", kernel_log_size);

        /* Shared batch buffer for kernel.log + subsystem files */
        uint32_t batch_pages = 8;  /* 32 KB */
        uint8_t *batch = (uint8_t *)(uintptr_t)pmm_alloc_contiguous(batch_pages);
        uint32_t batch_size = batch ? batch_pages * 4096 : 0;

        /* Snapshot seq and ring state -- seq-based flush cursor avoids
         * stalling after the ring saturates at KLOG_RING_SIZE entries. */
        uint64_t cur_seq = klog_get_seq();
        ring = klog_get_ring(&ring_count, &ring_head);

        /* Number of unflushed entries (bounded to ring capacity). */
        uint64_t unflushed = klog_flush_window(cur_seq, ixfs_flush_seq);

        if (ring && unflushed > 0) {
            uint64_t start_seq = cur_seq - unflushed;
            int flush_ok = 0;  /* only advance cursor on successful write */
            {
                char kpath[64];
                int kp = 0, kj;
                for (kj = 0; klog_dir[kj]; kj++) kpath[kp++] = klog_dir[kj];
                for (kj = 0; "kernel.log"[kj]; kj++) kpath[kp++] = "kernel.log"[kj];
                kpath[kp] = '\0';
                logfile = vfs_open(kpath, VFS_O_WRITE);
            }
            if (logfile) {
                if (batch) {
                    uint32_t batch_pos = 0;
                    uint32_t write_offset = (uint32_t)logfile->size;
                    int      write_failed = 0;  /* any short/error vfs_write */

                    /* Persist a gap marker AHEAD of the surviving tail when ring
                     * overflow dropped older entries, so an offline reader of
                     * kernel.log (the durable copy) sees the log is not complete --
                     * the serial WARN alone is gone once the machine reboots. */
                    {
                        uint32_t lost = klog_lost_count(cur_seq, ixfs_flush_seq);
                        if (lost) {
                            char mk[80];
                            int ml = snprintf(mk, sizeof(mk),
                                "--- %u earlier log entries lost to ring overflow ---\n",
                                (unsigned)lost);
                            int mi;
                            for (mi = 0; mi < ml && batch_pos < batch_size - 1; mi++)
                                batch[batch_pos++] = (uint8_t)mk[mi];
                        }
                    }

                    /* Iterate from oldest unflushed to newest.
                     * Ring slot for sequence s is: s % KLOG_RING_SIZE.
                     *
                     * Note: entries are read without holding s_klog_lock.
                     * Concurrent writers may overwrite the oldest slots
                     * during iteration. This is inherent ring buffer
                     * behavior -- a 1000-entry ring is lossy under extreme
                     * load. The alternative (locking during disk I/O) would
                     * block all klog() callers. */
                    /* Splash progress: report entries actually PERSISTED so the count
                     * never overstates what reached disk -- the cb fires after each
                     * successful chunk write (and the final write), not as entries are
                     * formatted (formatting is fast memory work; the blocking vfs_write
                     * is the slow part). A failed chunk breaks out before reporting, so
                     * the splash stays at the last persisted value, never a false high.
                     * An initial 0/total shows "Writing boot log... N entries" while the
                     * first slow write is in flight. The cb is snapshotted with an
                     * acquire load to pair with the setter's release store. */
                    klog_flush_progress_fn pcb =
                        __atomic_load_n(&s_flush_progress_cb, __ATOMIC_ACQUIRE);
                    uint32_t flush_total = (uint32_t)(cur_seq - start_seq);
                    int report_progress = pcb && klog_flush_progress_due(flush_total);
                    if (report_progress)
                        pcb(0, flush_total);

                    for (uint64_t s = start_seq; s < cur_seq; s++) {
                        uint32_t idx = (uint32_t)(s % KLOG_RING_SIZE);

                        char line[256];
                        int pos = format_entry(&ring[idx], line, 256);

                        /* Flush batch if it would overflow. Stop on the FIRST
                         * short/error write: continuing would place later chunks at
                         * offsets computed as if the failed chunk landed, holing the
                         * file. The cursor is retained (below) so the whole range
                         * retries next call. */
                        if (batch_pos + (uint32_t)pos > batch_size - 1) {
                            if (vfs_write(logfile, write_offset, batch_pos,
                                          batch) != (int)batch_pos) {
                                write_failed = 1;
                                break;
                            }
                            write_offset += batch_pos;
                            batch_pos = 0;
                            /* Entries start_seq..s-1 are now on disk (entry s is not yet
                             * copied into the batch). Report the persisted count. */
                            if (report_progress)
                                pcb((uint32_t)(s - start_seq), flush_total);
                        }

                        {
                            int k;
                            for (k = 0; k < pos; k++)
                                batch[batch_pos++] = (uint8_t)line[k];
                        }
                    }

                    /* Final flush of remaining data (skipped once a write failed). */
                    if (!write_failed && batch_pos > 0) {
                        if (vfs_write(logfile, write_offset, batch_pos, batch)
                                != (int)batch_pos)
                            write_failed = 1;
                    }
                    /* NB: no terminal 100% tick here. kernel.log is the durable copy
                     * but klog_disk_flush_locked continues into per-subsystem routing +
                     * events.jsonl + serial persistence below, so firing N/N now would
                     * show "done" while the drain is still writing on slow media. The
                     * per-chunk ticks above give kernel.log liveness; a whole-drain
                     * completion tick lands with the splash flush-progress render
                     * follow-up, where the true flush-end is the right place. */

                    /* Cursor-advance-on-success: only acknowledge the range when
                     * every write returned the requested byte count. */
                    flush_ok = !write_failed;
                }

                kernel_log_size = (uint32_t)logfile->size;
                vfs_close(logfile);
            }

            /* ---- Per-subsystem log routing (single-pass classify, BEST-EFFORT) ---- */
            /* ONE ring scan over the kernel.log seq range classifies each entry into
             * its subsystem slot (slot_of[]) and counts per slot -- collapsing the
             * former 6-scans-per-flush (one full ring walk + format per file) into a
             * single classify scan. Phase 2 then writes only non-empty subsystems,
             * each REUSING the full shared batch and chunk-flushing at the 32 KB
             * boundary, so a burst-heavy subsystem (boot.log / drivers.log during a
             * deferred slow-media flush) keeps its WHOLE view instead of being capped
             * to batch_size/6. The expensive work (the ring walk + format_entry) runs
             * once per entry; the per-slot Phase 2 passes only re-read the in-cache
             * slot_of[] byte array, not the ring. NO VFS writes happen during the
             * classify scan: the ring is read without s_klog_lock (best-effort views;
             * kernel.log, gated above, is the DURABLE copy of every entry), so
             * subsystem write failures are tolerated. */
            if (batch && flush_ok) {
                int8_t   slot_of[KLOG_RING_SIZE];  /* per-entry slot, -1 = none */
                uint32_t sub_count[SUBSYS_LOG_COUNT];
                uint32_t window = (uint32_t)(cur_seq - start_seq);
                uint32_t si, j;

                if (window > KLOG_RING_SIZE)
                    window = KLOG_RING_SIZE;  /* defensive: window is ring-bounded */
                for (si = 0; si < SUBSYS_LOG_COUNT; si++)
                    sub_count[si] = 0;

                /* Phase 1: single ring scan -- classify each entry, count per slot. */
                for (j = 0; j < window; j++) {
                    uint32_t idx = (uint32_t)((start_seq + j) % KLOG_RING_SIZE);
                    int slot = klog_dispatch_slot(ring[idx].subsystem);
                    slot_of[j] = (int8_t)slot;  /* -1 -> kernel.log only */
                    if (slot >= 0)
                        sub_count[slot]++;
                }

                /* Phase 2: write only non-empty subsystems. Each reuses the full
                 * batch, chunk-flushed at batch_size so nothing is dropped; empty
                 * subsystems never trigger vfs_open. */
                for (si = 0; si < SUBSYS_LOG_COUNT; si++) {
                    char spath[64];
                    uint32_t sp = 0;
                    struct vfs_node *sf;
                    const char *base = klog_dir;
                    const char *fname = s_subsys_filenames[si];
                    uint32_t bp = 0, woff;
                    int k;

                    if (sub_count[si] == 0)
                        continue;

                    for (k = 0; base[k]; k++) spath[sp++] = base[k];
                    for (k = 0; fname[k]; k++) spath[sp++] = fname[k];
                    spath[sp] = '\0';

                    /* VFS_O_CREATE: the X:\Logs BlackBox skeleton creates only
                     * directories, so the per-subsystem files must be created
                     * on first write or every split log is silently skipped
                     * (kernel.log alone would persist). */
                    sf = vfs_open(spath, VFS_O_WRITE | VFS_O_CREATE);
                    if (!sf)
                        continue;

                    /* Rotate this split log when it exceeds MaxSize, same policy as
                     * kernel.log -- a high-volume subsystem (boot/drivers.log) must
                     * not grow unbounded. The size is read from the freshly-opened
                     * handle (no cached state to go stale across reboots), and
                     * rotate_log_file renames by path, so close before rotating and
                     * reopen the now-fresh file. */
                    if ((uint32_t)sf->size >= rot_max_size) {
                        uint32_t cursz = (uint32_t)sf->size;
                        vfs_close(sf);
                        rotate_log_file(klog_dir, fname, cursz);
                        sf = vfs_open(spath, VFS_O_WRITE | VFS_O_CREATE);
                        if (!sf)
                            continue;
                    }
                    woff = (uint32_t)sf->size;

                    for (j = 0; j < window; j++) {
                        uint32_t idx;
                        char line[256];
                        int pos;

                        if (slot_of[j] != (int8_t)si)
                            continue;
                        idx = (uint32_t)((start_seq + j) % KLOG_RING_SIZE);
                        /* Re-validate against the entry's CURRENT tag: a concurrent
                         * logger may have overwritten this ring slot since Phase 1
                         * classified it (the ring is read without s_klog_lock). Only
                         * route to si when the live entry still maps here -- otherwise
                         * skip it from the subsystem view (kernel.log keeps it, and the
                         * new occupant is flushed in its own window next call). This
                         * narrows the classify-vs-format mismatch window back to the
                         * adjacent re-check/format reads, matching the old per-pass loop. */
                        if (klog_dispatch_slot(ring[idx].subsystem) != (int)si)
                            continue;
                        pos = format_entry(&ring[idx], line, 256);

                        /* Chunk-flush when the next entry would overflow the batch;
                         * a single entry (<= 256 bytes) always fits batch_size. Stop
                         * this file on the FIRST short/error write: advancing woff past
                         * a partial chunk would place later chunks at fabricated offsets
                         * and hole the best-effort view (mirrors the kernel.log
                         * write_failed guard above). Drop the buffered chunk so the
                         * post-loop final write does not re-attempt it. */
                        if (bp + (uint32_t)pos > batch_size) {
                            if (vfs_write(sf, woff, bp, batch) != (int)bp) {
                                bp = 0;
                                break;
                            }
                            woff += bp;
                            bp = 0;
                        }
                        for (k = 0; k < pos; k++)
                            batch[bp++] = (uint8_t)line[k];
                    }
                    /* Final partial chunk: last write for this file, so a short write
                     * here cannot hole a following chunk (none follows). */
                    if (bp > 0)
                        vfs_write(sf, woff, bp, batch);
                    vfs_close(sf);
                }
            }

            /* Only advance cursor after successful persistence */
            if (flush_ok)
                ixfs_flush_seq = cur_seq;
            flush_persisted = flush_ok;  /* drives the done-path diagnostic */
        }

        /* Free batch buffer after both kernel.log and subsystem files */
        if (batch) {
            uint32_t pg;
            for (pg = 0; pg < batch_pages; pg++)
                pmm_free_frame((uintptr_t)batch + pg * 4096);
        }
    }

    /* ---- Flush JSON Lines to events.jsonl ---- */
    if ((vfs_is_mounted('X') || vfs_is_mounted('C')) && ixfs_inited) {
        static char jsonl_path[64];
        if (!jsonl_path[0]) {
            int jp = 0, jj;
            for (jj = 0; klog_dir[jj]; jj++) jsonl_path[jp++] = klog_dir[jj];
            for (jj = 0; "events.jsonl"[jj]; jj++) jsonl_path[jp++] = "events.jsonl"[jj];
            jsonl_path[jp] = '\0';
        }

        if (!jsonl_inited) {
            /* Create events.jsonl on first flush */
            struct vfs_node *f = vfs_open(jsonl_path,
                                          VFS_O_WRITE | VFS_O_CREATE);
            if (f) { jsonl_file_size = (uint32_t)f->size; vfs_close(f); }
            jsonl_flush_seq = 0;
            jsonl_inited = 1;
        }

        /* Rotate events.jsonl if needed */
        jsonl_file_size = rotate_log_file(
            klog_dir, "events.jsonl", jsonl_file_size);

        /* Seq-based JSON flush -- same pattern as kernel.log */
        {
            uint64_t jcur_seq = klog_get_seq();
            ring = klog_get_ring(&ring_count, &ring_head);
            uint64_t junflushed = klog_flush_window(jcur_seq, jsonl_flush_seq);

            if (ring && junflushed > 0) {
                /* Batch JSON lines into a 16 KB buffer */
                uint32_t jpages = 4;
                uint8_t *jbuf = (uint8_t *)(uintptr_t)pmm_alloc_contiguous(jpages);
                if (jbuf) {
                    uint32_t jsize = jpages * 4096;
                    uint32_t jpos = 0;
                    static const char *lvl_names[] = {
                        "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
                    };
                    uint64_t jstart = jcur_seq - junflushed;
                    /* Open once and chunk-flush at the batch boundary so a window
                     * larger than jsize (up to KLOG_RING_SIZE entries x hundreds of
                     * bytes) is never silently dropped. This matches kernel.log's
                     * durable verified-write contract: jsonl_flush_seq advances only
                     * after EVERY line in the window is written, so a short/error
                     * write leaves entries for the next flush. (The per-subsystem
                     * split logs are deliberately best-effort -- kernel.log is the
                     * durable copy -- so they are NOT the precedent here.) */
                    struct vfs_node *jf = vfs_open(jsonl_path, VFS_O_WRITE);
                    uint32_t woff = jf ? (uint32_t)jf->size : 0;  /* last fully-committed offset */
                    int jwrite_ok = (jf != (struct vfs_node *)0);
                    uint64_t jcommitted = jstart;  /* seq durably written so far */
                    /* A prior degraded-media flush left an unterminated partial
                     * record at EOF. Terminate it with a newline so it is one
                     * skipped malformed line and the replayed records below start
                     * clean on their own JSONL lines (no <partial><full> merge). */
                    if (jf && jsonl_resync_nl) {
                        if (woff == 0) {
                            /* File was rotated/recreated since the partial tail: the
                             * partial now lives in events.jsonl.1 and this fresh file
                             * has none, so no separator is needed. */
                            jsonl_resync_nl = 0;
                        } else {
                            uint8_t nl = (uint8_t)'\n';
                            if (vfs_write(jf, woff, 1, &nl) == 1) {
                                woff += 1;
                                jsonl_resync_nl = 0;
                            } else {
                                /* Cannot write the separator -> cannot safely replay
                                 * (records would merge onto the partial tail). Abort
                                 * this flush entirely: skip the replay loop, do not
                                 * advance the cursor, and keep the flag so the next
                                 * flush retries the separator first. */
                                jwrite_ok = 0;
                            }
                        }
                    }

                    for (uint64_t s = jstart; s < jcur_seq; s++) {
                        uint32_t idx = (uint32_t)(s % KLOG_RING_SIZE);
                        const klog_entry_t *e = &ring[idx];
                        const char *lvl = ((uint32_t)e->level < 5)
                            ? lvl_names[e->level] : "?";

                        /* JSON-escape subsystem and message strings.
                         * Escapes: \ -> \\, " -> \", control chars < 0x20 dropped. */
                        char esc_sub[48], esc_msg[300];
                        {
                            /* Aliased like every other disk rendering: this
                             * sink is in the same ring-3-readable directory,
                             * so serializing the raw tag here would publish
                             * an authenticating value the text logs are
                             * careful to withhold. */
                            const char *src = klog_disk_subsystem(e->subsystem);

                            if (!src)
                                src = "";
                            uint32_t ep = 0, emax = sizeof(esc_sub) - 1;
                            while (*src && ep < emax) {
                                if (*src == '"' || *src == '\\') {
                                    if (ep + 1 < emax) { esc_sub[ep++] = '\\'; esc_sub[ep++] = *src; }
                                } else if ((uint8_t)*src >= 0x20) {
                                    esc_sub[ep++] = *src;
                                }
                                src++;
                            }
                            esc_sub[ep] = '\0';
                        }
                        {
                            const char *src = e->message;
                            uint32_t ep = 0, emax = sizeof(esc_msg) - 1;
                            while (*src && ep < emax) {
                                if (*src == '"' || *src == '\\') {
                                    if (ep + 1 < emax) { esc_msg[ep++] = '\\'; esc_msg[ep++] = *src; }
                                } else if (*src == '\n') {
                                    if (ep + 1 < emax) { esc_msg[ep++] = '\\'; esc_msg[ep++] = 'n'; }
                                } else if (*src == '\r') {
                                    if (ep + 1 < emax) { esc_msg[ep++] = '\\'; esc_msg[ep++] = 'r'; }
                                } else if (*src == '\t') {
                                    if (ep + 1 < emax) { esc_msg[ep++] = '\\'; esc_msg[ep++] = 't'; }
                                } else if ((uint8_t)*src >= 0x20) {
                                    esc_msg[ep++] = *src;
                                }
                                src++;
                            }
                            esc_msg[ep] = '\0';
                        }

                        char line[512];
                        int len = snprintf(line, sizeof(line),
                            "{\"ts\":%llu,\"lvl\":\"%s\",\"sub\":\"%s\","
                            "\"cpu\":%u,\"pid\":%u,\"tid\":%u,"
                            "\"msg\":\"%s\",\"dropped\":%u}\n",
                            (uint64_t)e->timestamp * 10ull,  /* ticks->ms in 64-bit (32-bit wraps at ~5 days) */
                            lvl, esc_sub,
                            (unsigned)e->cpu_id,
                            (unsigned)e->pid,
                            (unsigned)e->tid,
                            esc_msg,
                            (unsigned)klog_get_dropped(e->subsystem));

                        if (!jwrite_ok || len <= 0)
                            continue;
                        /* snprintf returns the would-be length (C99); clamp to the
                         * bytes actually present in line[] so the copy never
                         * overreads the stack buffer. */
                        if ((uint32_t)len >= sizeof(line))
                            len = (int)sizeof(line) - 1;
                        /* Chunk-flush when this line would overflow jbuf; a single
                         * line (<= 512) always fits the 16 KB buffer afterward. */
                        if (jpos + (uint32_t)len > jsize) {
                            if (vfs_write(jf, woff, jpos, jbuf) != (int)jpos) {
                                jwrite_ok = 0;
                                break;
                            }
                            woff += jpos;
                            jpos = 0;
                            jcommitted = s;  /* entries [..s-1] are now durable */
                        }
                        {
                            uint32_t k;
                            for (k = 0; k < (uint32_t)len; k++)
                                jbuf[jpos++] = (uint8_t)line[k];
                        }
                    }

                    /* Final partial chunk; a short write here cannot hole a later
                     * chunk (none follows). */
                    if (jwrite_ok && jpos > 0) {
                        if (vfs_write(jf, woff, jpos, jbuf) != (int)jpos)
                            jwrite_ok = 0;
                        else
                            woff += jpos;
                    }
                    /* If no write failed, the whole window is handled (every line
                     * either written or legitimately empty) -- commit to jcur_seq. */
                    if (jwrite_ok)
                        jcommitted = jcur_seq;
                    if (jf) {
                        uint32_t real_size = (uint32_t)jf->size;  /* incl. any partial */
                        vfs_close(jf);
                        if (jwrite_ok) {
                            jsonl_file_size = woff;
                        } else if (vfs_truncate(jsonl_path, woff) == 0) {
                            /* Rolled the partial failed chunk back: file is clean at
                             * the last committed offset, tail replays from jcommitted. */
                            jsonl_file_size = woff;
                        } else {
                            /* Degraded media: truncate rejected. The failed vfs_write
                             * (FAT32/IXFS return -1 on error but can side-effect
                             * partial clusters onto disk, growing node->size) left
                             * (real_size - woff) bytes of this chunk on disk. jbuf
                             * still holds the chunk, so count the COMPLETE
                             * newline-terminated records in that landed prefix and
                             * advance jcommitted past them -- the replay then never
                             * duplicates an already-durable line; only the trailing
                             * partial record (if any) survives as one skippable
                             * malformed JSONL line. Safe to klog -- the `flushing`
                             * guard no-ops a recursive flush. */
                            uint32_t landed = (real_size > woff) ? (real_size - woff) : 0;
                            uint32_t b, lines = 0;
                            if (landed > jpos) landed = jpos;
                            for (b = 0; b < landed; b++)
                                if (jbuf[b] == (uint8_t)'\n') lines++;
                            jcommitted += lines;
                            jsonl_file_size = real_size;
                            /* If the landed prefix ends mid-record (no trailing
                             * newline), the next flush must emit a separator first
                             * so the replayed record does not merge onto this
                             * partial line. */
                            if (landed > 0 && jbuf[landed - 1] != (uint8_t)'\n')
                                jsonl_resync_nl = 1;
                            klog(LOG_WARN, "klog",
                                 "events.jsonl: write+rollback failed; %u rec(s) salvaged, partial may remain",
                                 (unsigned)lines);
                        }
                    }
                    /* Advance to the last fully-committed chunk (== jcur_seq on full
                     * success); a mid-window failure retries only the uncommitted
                     * tail rather than replaying already-durable lines. */
                    jsonl_flush_seq = jcommitted;
                    {
                        uint32_t pg;
                        for (pg = 0; pg < jpages; pg++)
                            pmm_free_frame((uintptr_t)jbuf + pg * 4096);
                    }
                }
            }
        }
    }

    /* ---- Flush serial log to Serial\ subdirectory (full-file overwrite) ---- */
    if ((vfs_is_mounted('X') || vfs_is_mounted('C')) && fat32_buf && fat32_pos > 0) {
        /* Lazy init: if we haven't set up yet, do it now */
        if (!fat32_inited) {
            pick_log_number();
            {
                struct vfs_node *serial_dir = vfs_open(KLOG_SERIAL_DIR, VFS_O_READ);
                if (serial_dir && serial_dir->ops && serial_dir->ops->create)
                    serial_dir->ops->create(serial_dir, log_filename, VFS_FILE);
            }
            fat32_inited = 1;
        }

        if (log_filename[0]) {
            /* Build full path: KLOG_SERIAL_DIR + filename */
            char path[80];
            int p = 0, j;
            for (j = 0; KLOG_SERIAL_DIR[j]; j++) path[p++] = KLOG_SERIAL_DIR[j];
            for (j = 0; log_filename[j]; j++) path[p++] = log_filename[j];
            path[p] = '\0';

            logfile = vfs_open(path, VFS_O_WRITE);
            if (logfile) {
                vfs_write(logfile, 0, fat32_pos, fat32_buf);
                vfs_close(logfile);
            }
        }
    } else if ((vfs_is_mounted('X') || vfs_is_mounted('C')) && !fat32_buf) {
        /* No PMM buffer -- allocate on demand and do a ring-buffer dump */
        uint32_t buf_pages = 64;
        uint8_t *tmp_buf = (uint8_t *)pmm_alloc_contiguous(buf_pages);
        uint32_t tmp_size = buf_pages * 4096;
        uint32_t total = 0;

        if (!tmp_buf)
            goto done;

        ring = klog_get_ring(&ring_count, &ring_head);
        if (!ring || ring_count == 0)
            goto free_tmp;

        for (i = 0; i < ring_count && total < tmp_size - 256; i++) {
            uint32_t idx;
            if (ring_count < KLOG_RING_SIZE) {
                idx = i;
            } else {
                idx = (ring_head + (i - (ring_count - KLOG_RING_SIZE)))
                      % KLOG_RING_SIZE;
            }

            char line[256];
            int pos = format_entry(&ring[idx], line, 256);
            {
                int k;
                for (k = 0; k < pos; k++)
                    tmp_buf[total++] = (uint8_t)line[k];
            }
        }

        /* Lazy init for filename */
        if (!fat32_inited) {
            pick_log_number();
            {
                struct vfs_node *serial_dir = vfs_open(KLOG_SERIAL_DIR, VFS_O_READ);
                if (serial_dir && serial_dir->ops && serial_dir->ops->create)
                    serial_dir->ops->create(serial_dir, log_filename, VFS_FILE);
            }
            fat32_inited = 1;
        }

        if (log_filename[0]) {
            char path[64];
            int p = 0, j;
            for (j = 0; klog_dir[j]; j++) path[p++] = klog_dir[j];
            for (j = 0; log_filename[j]; j++) path[p++] = log_filename[j];
            path[p] = '\0';

            logfile = vfs_open(path, VFS_O_WRITE);
            if (logfile) {
                vfs_write(logfile, 0, total, tmp_buf);
                vfs_close(logfile);
            }
        }

free_tmp:
        {
            uint32_t pg;
            for (pg = 0; pg < buf_pages; pg++)
                pmm_free_frame((uintptr_t)tmp_buf + pg * 4096);
        }
    }

done:
    /* Flush timing summary + slow-media detection (all goto paths converge here). */
    if (log_mounted) {
        uint32_t flush_ms = (uint32_t)((uptime_ns() - flush_t0) / 1000000ull);
        if (flush_pending >= KLOG_FLUSH_PROGRESS_MIN) {
            /* Report success only when kernel.log actually persisted; a failed or
             * partial write left the cursor unadvanced (entries retried next call),
             * so reporting "done" would hide the failure on the exact slow/degraded
             * media this diagnostic targets. */
            if (flush_persisted) {
                serial_write("[KLOG] flush done (");
                serial_write_u32(flush_pending);
                serial_write(" entries, ");
                serial_write_u32(flush_ms);
                serial_write(" ms)\n");
            } else {
                serial_write("[WARN] [KLOG] flush FAILED (");
                serial_write_u32(flush_pending);
                serial_write(" entries retained for retry, ");
                serial_write_u32(flush_ms);
                serial_write(" ms)\n");
            }
        }
        if (flush_ms > KLOG_SLOW_MEDIA_MS) {
            serial_write("[WARN] [KLOG] Slow media detected (");
            serial_write_u32(flush_ms);
            serial_write(" ms) -- switching to deferred flush\n");
            __atomic_store_n(&s_klog_slow_media, 1, __ATOMIC_RELAXED);
            /* Auto-enable deferred mode after the first slow flush: subsequent
             * per-subsystem flushes become no-ops and the accumulated entries are
             * written once by the forced klog_disk_flush_all() at boot end. CAS so
             * the set-ACTIVE is atomic with the DISABLED check on the SAME word: once
             * the boot-end drain latches DISABLED, no auto-enable can re-arm ACTIVE. */
            {
                uint32_t old = __atomic_load_n(&s_klog_defer_state, __ATOMIC_RELAXED);
                while (!(old & KLOG_DEFER_DISABLED)) {
                    if (__atomic_compare_exchange_n(&s_klog_defer_state, &old,
                            old | KLOG_DEFER_ACTIVE, 0,
                            __ATOMIC_RELAXED, __ATOMIC_RELAXED))
                        break;
                    /* old reloaded by the CAS on failure; retry unless now DISABLED. */
                }
            }
        }
    }
}

/* Enable/disable deferred-flush mode. When enabled, klog_disk_flush() is a no-op
 * (entries accumulate in the ring) until the forced klog_disk_flush_all(). */
void klog_set_deferred(int enabled)
{
    /* Explicit control (pre-drain / tests): toggle the ACTIVE bit, preserve DISABLED. */
    if (enabled)
        __atomic_fetch_or(&s_klog_defer_state, KLOG_DEFER_ACTIVE, __ATOMIC_RELAXED);
    else
        __atomic_fetch_and(&s_klog_defer_state, ~KLOG_DEFER_ACTIVE, __ATOMIC_RELAXED);
}

/* Effective deferral: ACTIVE set AND DISABLED clear. DISABLED is DOMINANT, so once
 * the boot-end drain latches it, even an explicit klog_set_deferred(1) (which can
 * leave ACTIVE|DISABLED) does not make flushes no-op again. Pure; exposed for tests. */
int klog_defer_active(uint32_t state)
{
    return (state & KLOG_DEFER_ACTIVE) && !(state & KLOG_DEFER_DISABLED);
}

void klog_disk_flush(void)
{
    /* Guard: reentrancy (vfs_write → klog → klog_disk_flush) AND SMP -- an atomic
     * test-and-set so two CPUs never run the body (and mutate the shared cursors /
     * FAT32 buffer) concurrently. A skipped flusher's entries stay in the ring and
     * persist on the next call. */
    if (__atomic_exchange_n(&flushing, 1, __ATOMIC_ACQUIRE))
        return;
    /* Deferred mode: no-op (release the guard we just took). DISABLED-dominant. */
    if (klog_defer_active(__atomic_load_n(&s_klog_defer_state, __ATOMIC_RELAXED))) {
        __atomic_store_n(&flushing, 0, __ATOMIC_RELEASE);
        return;
    }
    klog_disk_flush_locked();
    __atomic_store_n(&flushing, 0, __ATOMIC_RELEASE);
}

/* Forced single flush of everything accumulated in deferred mode -- the one
 * boot-end write. Guard-AWARE: it spin-waits to ACQUIRE the flushing guard (so it
 * cannot silently no-op behind a concurrent flusher), then runs the flush body.
 * If the guard cannot be acquired within the bound it returns WITHOUT running the
 * body or touching the guard -- it must never run _locked() or release a guard it
 * does not own. The auto-defer latch + deferred clear are done UP FRONT (before the
 * spin), not under the guard: they are independent atomic flags, and setting them
 * first guarantees that even on the timeout path -- or a concurrent slow flush whose
 * auto-defer runs while we spin -- deferred mode ends OFF and can never re-arm before
 * userland. A concurrent flush's auto-defer sits at the END of its (slow) body, long
 * after this store is visible, so it sees the latch and is suppressed. */
void klog_disk_flush_all(void)
{
    uint32_t spin = 10000000u;  /* bounded; boot-end is single-threaded so normally
                                 * acquires on the first try. */
    /* Latch DISABLED and clear ACTIVE in ONE atomic store BEFORE the spin: a single
     * modification order means even on the timeout path -- or against a concurrent
     * auto-enable CAS -- deferral ends OFF and can never re-arm before userland. */
    __atomic_store_n(&s_klog_defer_state, KLOG_DEFER_DISABLED, __ATOMIC_RELAXED);
    while (__atomic_exchange_n(&flushing, 1, __ATOMIC_ACQUIRE)) {
        if (spin-- == 0) {
            /* Never acquired -- do NOT run the body or clear the guard we do not own.
             * Deferral is already latched off above, so userland is still safe. */
            serial_write("[WARN] [KLOG] flush_all: flush guard busy -- deferred logs not drained\n");
            return;
        }
        __asm__ volatile("pause" ::: "memory");
    }
    klog_disk_flush_locked();
    __atomic_store_n(&flushing, 0, __ATOMIC_RELEASE);
}
