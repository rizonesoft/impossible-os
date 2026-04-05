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

/* Numbered serial log filename: "Serial_YYMMDDNN.log" */
static char      log_filename[24];  /* "Serial_YYMMDDNN.log" + NUL */

/* Log rotation config (loaded from Registry, or defaults) */
static uint32_t  rot_max_size    = 4 * 1024 * 1024;  /* 4 MB default */
static uint32_t  rot_max_rotated = 3;                  /* keep .1, .2, .3 */

/* Per-file size tracking for O(1) rotation check */
static uint32_t  kernel_log_size;  /* tracked across flushes */

/* JSON Lines event log state -- monotonic sequence cursor */
static uint64_t  jsonl_flush_seq;    /* ring entries already flushed */
static uint32_t  jsonl_file_size;    /* tracked for rotation */
static int       jsonl_inited;

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

    /* Timestamp: ISO 8601 if wall clock ready, else tick count */
    line[pos++] = '[';
    if (wall_clock_ready()) {
        /* Reconstruct FILETIME from tick count at log time.
         * e->timestamp is PIT ticks (100 Hz); approximate FILETIME. */
        FILETIME ft = KeQuerySystemTime();
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
        const char *ss = e->subsystem ? e->subsystem : "???";
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

    /* BlackBox (X:\Logs\) dirs are created by boot skeleton (TODO-17 §4).
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

static uint32_t rotate_log_file(const char *dir, const char *filename,
                                uint32_t current_size)
{
    char old_path[80], new_path[80];
    uint32_t n;
    int j, dp;

    if (current_size < rot_max_size)
        return current_size;

    /* Delete the oldest rotated file: foo.log.N */
    {
        dp = 0;
        for (j = 0; dir[j]; j++) old_path[dp++] = dir[j];
        for (j = 0; filename[j]; j++) old_path[dp++] = filename[j];
        old_path[dp++] = '.';
        old_path[dp++] = '0' + (char)(rot_max_rotated % 10);
        old_path[dp] = '\0';
        vfs_unlink(old_path);
    }

    /* Shift existing rotated files: .N-1 -> .N, .N-2 -> .N-1, etc. */
    for (n = rot_max_rotated; n >= 2; n--) {
        int sp;
        dp = 0;
        for (j = 0; dir[j]; j++) old_path[dp++] = dir[j];
        for (j = 0; filename[j]; j++) old_path[dp++] = filename[j];
        old_path[dp++] = '.';
        old_path[dp++] = '0' + (char)((n - 1) % 10);
        old_path[dp] = '\0';

        sp = 0;
        for (j = 0; dir[j]; j++) new_path[sp++] = dir[j];
        for (j = 0; filename[j]; j++) new_path[sp++] = filename[j];
        new_path[sp++] = '.';
        new_path[sp++] = '0' + (char)(n % 10);
        new_path[sp] = '\0';

        vfs_rename(old_path, new_path);
    }

    /* Rename current file to .1 */
    {
        int sp;
        dp = 0;
        for (j = 0; dir[j]; j++) old_path[dp++] = dir[j];
        for (j = 0; filename[j]; j++) old_path[dp++] = filename[j];
        old_path[dp] = '\0';

        sp = 0;
        for (j = 0; dir[j]; j++) new_path[sp++] = dir[j];
        for (j = 0; filename[j]; j++) new_path[sp++] = filename[j];
        new_path[sp++] = '.';
        new_path[sp++] = '1';
        new_path[sp] = '\0';

        vfs_rename(old_path, new_path);
    }

    /* Create fresh empty file */
    {
        dp = 0;
        for (j = 0; dir[j]; j++) new_path[dp++] = dir[j];
        for (j = 0; filename[j]; j++) new_path[dp++] = filename[j];
        new_path[dp] = '\0';

        struct vfs_node *f = vfs_open(new_path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
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
        val_type == 4 && val > 0)  /* REG_DWORD = 4 */
        rot_max_size = val;

    val_size = sizeof(val);
    val_type = 0;
    if (RegReadKeyValue((void *)(uintptr_t)0x80000002,
                        "SYSTEM\\Logs", "MaxRotated",
                        &val_type, (uint8_t *)&val, &val_size) == 0 &&
        val_type == 4 && val > 0 && val <= 9)
        rot_max_rotated = val;
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

    /* Oldest log tracking (for cap enforcement) */
    uint8_t old_yy = 99, old_mm = 12, old_dd = 31, old_seq = 99;
    char    oldest_name[16];
    oldest_name[0] = '\0';

    /* Get current date from RTC */
    today_yy = (uint8_t)(rtc_get_year() % 100);
    today_mm = (uint8_t)rtc_get_month();
    today_dd = (uint8_t)rtc_get_day();

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
                        for (k = 0; de->name[k] && k < 15; k++)
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
        x_root->ops->unlink(x_root, oldest_name);
        klog(LOG_DEBUG, "klog", "deleted oldest log: %s%s (%u files)", klog_dir, oldest_name, count);
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
    if (e->subsystem && e->subsystem[0]) {
        buf_puts(e->subsystem);
        buf_puts(": ");
    }
    buf_puts(e->message);
    buf_putc('\n');
}

void klog_disk_flush(void)
{
    uint32_t ring_count, ring_head;
    const klog_entry_t *ring;
    struct vfs_node *logfile;
    uint32_t i;

    /* Reentrancy guard: vfs_write → klog → klog_disk_flush → infinite loop */
    if (flushing)
        return;
    flushing = 1;

    /* ---- Flush to log directory (X:\ BlackBox or C:\ fallback) ---- */
    if (vfs_is_mounted('X') || vfs_is_mounted('C')) {
        if (!ixfs_inited) {
            ensure_log_dirs();
            load_rotation_config();
            ixfs_inited = 1;
            ixfs_flush_seq = 0;

            /* Create kernel.log via klog_dir path */
            {
                char kl_path[64];
                int kp = 0, kj;
                for (kj = 0; klog_dir[kj]; kj++) kl_path[kp++] = klog_dir[kj];
                { const char *fn = "kernel.log";
                  for (kj = 0; fn[kj]; kj++) kl_path[kp++] = fn[kj]; }
                kl_path[kp] = '\0';
                vfs_create(kl_path, VFS_FILE);
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

        /* Number of unflushed entries (capped to ring capacity) */
        uint64_t unflushed = cur_seq - ixfs_flush_seq;
        if (unflushed > KLOG_RING_SIZE)
            unflushed = KLOG_RING_SIZE;  /* oldest entries overwritten */

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

                    /* Iterate from oldest unflushed to newest.
                     * Ring slot for sequence s is: s % KLOG_RING_SIZE.
                     *
                     * Note: entries are read without holding s_klog_lock.
                     * Concurrent writers may overwrite the oldest slots
                     * during iteration. This is inherent ring buffer
                     * behavior -- a 1000-entry ring is lossy under extreme
                     * load. The alternative (locking during disk I/O) would
                     * block all klog() callers. */
                    for (uint64_t s = start_seq; s < cur_seq; s++) {
                        uint32_t idx = (uint32_t)(s % KLOG_RING_SIZE);

                        char line[256];
                        int pos = format_entry(&ring[idx], line, 256);

                        /* Flush batch if it would overflow */
                        if (batch_pos + (uint32_t)pos > batch_size - 1) {
                            vfs_write(logfile, write_offset, batch_pos,
                                      batch);
                            write_offset += batch_pos;
                            batch_pos = 0;
                        }

                        {
                            int k;
                            for (k = 0; k < pos; k++)
                                batch[batch_pos++] = (uint8_t)line[k];
                        }
                    }

                    /* Final flush of remaining data */
                    if (batch_pos > 0) {
                        vfs_write(logfile, write_offset, batch_pos, batch);
                    }
                    flush_ok = 1;
                }

                kernel_log_size = (uint32_t)logfile->size;
                vfs_close(logfile);
            }

            /* ---- Per-subsystem log routing (batched) ---- */
            /* Uses the same seq range as kernel.log -- each entry is
             * routed exactly once to its subsystem file. */
            if (batch && flush_ok) {
                uint32_t si;
                for (si = 0; si < SUBSYS_LOG_COUNT; si++) {
                    char spath[64];
                    uint32_t sp = 0;
                    struct vfs_node *sf;
                    const char *base = klog_dir;
                    const char *fname = s_subsys_filenames[si];
                    int k;
                    uint32_t bp = 0;

                    for (k = 0; base[k]; k++) spath[sp++] = base[k];
                    for (k = 0; fname[k]; k++) spath[sp++] = fname[k];
                    spath[sp] = '\0';

                    sf = vfs_open(spath, VFS_O_WRITE);
                    if (!sf) continue;

                    /* Batch matching entries using same seq range */
                    for (uint64_t s = start_seq; s < cur_seq; s++) {
                        uint32_t idx = (uint32_t)(s % KLOG_RING_SIZE);
                        const char *df, *a, *b;

                        df = dispatch_filename(ring[idx].subsystem);
                        a = df; b = fname;
                        while (*a && *a == *b) { a++; b++; }
                        if (*a != '\0' || *b != '\0')
                            continue;

                        {
                            char line[256];
                            int pos = format_entry(&ring[idx], line, 256);
                            if (bp + (uint32_t)pos < batch_size) {
                                for (k = 0; k < pos; k++)
                                    batch[bp++] = (uint8_t)line[k];
                            }
                        }
                    }

                    /* Single write for all matching entries */
                    if (bp > 0)
                        vfs_write(sf, (uint32_t)sf->size, bp, batch);
                    vfs_close(sf);
                }
            }

            /* Only advance cursor after successful persistence */
            if (flush_ok)
                ixfs_flush_seq = cur_seq;
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
            uint64_t junflushed = jcur_seq - jsonl_flush_seq;
            if (junflushed > KLOG_RING_SIZE)
                junflushed = KLOG_RING_SIZE;

            if (ring && junflushed > 0) {
                /* Batch JSON lines into a 32 KB buffer */
                uint32_t jpages = 4;
                uint8_t *jbuf = (uint8_t *)(uintptr_t)pmm_alloc_contiguous(jpages);
                if (jbuf) {
                    uint32_t jsize = jpages * 4096;
                    uint32_t jpos = 0;
                    static const char *lvl_names[] = {
                        "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
                    };
                    uint64_t jstart = jcur_seq - junflushed;

                    for (uint64_t s = jstart; s < jcur_seq; s++) {
                        uint32_t idx = (uint32_t)(s % KLOG_RING_SIZE);
                        const klog_entry_t *e = &ring[idx];
                        const char *lvl = ((uint32_t)e->level < 5)
                            ? lvl_names[e->level] : "?";

                        /* JSON-escape subsystem and message strings.
                         * Escapes: \ -> \\, " -> \", control chars < 0x20 dropped. */
                        char esc_sub[48], esc_msg[300];
                        {
                            const char *src = e->subsystem ? e->subsystem : "";
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
                            "{\"ts\":%u,\"lvl\":\"%s\",\"sub\":\"%s\","
                            "\"cpu\":%u,\"pid\":%u,\"tid\":%u,"
                            "\"msg\":\"%s\",\"dropped\":%u}\n",
                            (unsigned)e->timestamp * 10,
                            lvl, esc_sub,
                            (unsigned)e->cpu_id,
                            (unsigned)e->pid,
                            (unsigned)e->tid,
                            esc_msg,
                            (unsigned)klog_get_dropped(e->subsystem));

                        if (len > 0 && jpos + (uint32_t)len < jsize) {
                            uint32_t k;
                            for (k = 0; k < (uint32_t)len; k++)
                                jbuf[jpos++] = (uint8_t)line[k];
                        }
                    }

                    if (jpos > 0) {
                        struct vfs_node *jf = vfs_open(jsonl_path, VFS_O_WRITE);
                        if (jf) {
                            vfs_write(jf, (uint32_t)jf->size, jpos, jbuf);
                            jsonl_file_size = (uint32_t)jf->size;
                            vfs_close(jf);
                            /* Only advance cursor after successful write */
                            jsonl_flush_seq = jcur_seq;
                        }
                    } else {
                        /* No entries to write (all filtered/truncated) -- safe to advance */
                        jsonl_flush_seq = jcur_seq;
                    }
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
    flushing = 0;
}
