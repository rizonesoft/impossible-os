/* ============================================================================
 * klog_disk.c — Unified disk logging (replaces klog_flush.c + klog_live.c)
 *
 * Handles all disk-based log output:
 *   - Batch flush to C:\Impossible\System\Logs\kernel.log (IXFS, appendable)
 *   - Full buffer write to X:\BOOT_NNN.LOG (FAT32, numbered per boot session)
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

/* ---- Per-subsystem log dispatch ---- */

typedef struct {
    const char *tag;         /* subsystem tag to match (case-sensitive) */
    const char *filename;    /* file in C:\Impossible\System\Logs\ */
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

/* IXFS (C:) flush tracking */
static uint32_t ixfs_flush_index;
static int      ixfs_inited;

/* FAT32 (X:) buffer + live mode */
static uint8_t  *fat32_buf      = (void *)0;
static uint32_t  fat32_pos      = 0;
static uint32_t  fat32_buf_size = 0;
static int       fat32_inited   = 0;
static int       live_enabled   = 0;
static int       flushing       = 0;  /* reentrancy guard */

/* Numbered log filename: "BOOT_NNN.LOG" */
static char      log_filename[16];

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

    /* Timestamp */
    line[pos++] = '[';
    pos += u32_to_str(e->timestamp, line + pos, 10);
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
static void make_log_filename(uint8_t yy, uint8_t mm, uint8_t dd,
                               uint8_t seq, char *out)
{
    out[0] = '0' + (char)(yy / 10);
    out[1] = '0' + (char)(yy % 10);
    out[2] = '0' + (char)(mm / 10);
    out[3] = '0' + (char)(mm % 10);
    out[4] = '0' + (char)(dd / 10);
    out[5] = '0' + (char)(dd % 10);
    out[6] = '0' + (char)(seq / 10);
    out[7] = '0' + (char)(seq % 10);
    out[8] = '.'; out[9] = 'L'; out[10] = 'O'; out[11] = 'G';
    out[12] = '\0';
}

/* Parse "YYMMDDnn.LOG" → 1 if valid log file, fills out fields.
 * Returns 0 if not a log file. */
static int parse_log_filename(const char *name, uint8_t *yy, uint8_t *mm,
                               uint8_t *dd, uint8_t *seq)
{
    int i;

    /* Must be exactly 12 chars: 8 digits + ".LOG" */
    for (i = 0; i < 8; i++)
        if (name[i] < '0' || name[i] > '9') return 0;
    if (name[8] != '.' || name[9] != 'L' || name[10] != 'O' || name[11] != 'G')
        return 0;
    if (name[12] != '\0') return 0;

    *yy  = (uint8_t)((name[0] - '0') * 10 + (name[1] - '0'));
    *mm  = (uint8_t)((name[2] - '0') * 10 + (name[3] - '0'));
    *dd  = (uint8_t)((name[4] - '0') * 10 + (name[5] - '0'));
    *seq = (uint8_t)((name[6] - '0') * 10 + (name[7] - '0'));

    /* Basic sanity: month 01–12, day 01–31 */
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

/* Scan X: for log files, pick today's next sequence, enforce 100-file cap. */
static void pick_log_number(void)
{
    struct vfs_node *x_root;
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

    x_root = vfs_get_drive_root('X');
    if (!x_root || !x_root->ops || !x_root->ops->finddir)
        goto fallback;

    /* Enumerate X: root directory to find all log files */
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
        klog(LOG_DEBUG, "klog", "deleted oldest log: X:\\%s (%u files)", oldest_name, count);
    }

fallback:
    /* Build today's filename with next sequence number */
    {
        uint8_t next_seq = max_seq_today + 1;
        if (next_seq > 99) next_seq = 99;  /* safety clamp */

        make_log_filename(today_yy, today_mm, today_dd, next_seq, log_filename);
        klog(LOG_INFO, "klog", "writing to X:\\%s", log_filename);
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

    /* Determine numbered log filename */
    if (vfs_is_mounted('X')) {
        pick_log_number();

        /* Create the log file on X: */
        struct vfs_node *x_root = vfs_get_drive_root('X');
        if (x_root && x_root->ops && x_root->ops->create)
            x_root->ops->create(x_root, log_filename, VFS_FILE);

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

    /* ---- Flush to IXFS C: drive (append mode) ---- */
    if (vfs_is_mounted('C')) {
        if (!ixfs_inited) {
            ensure_log_dirs();
            ixfs_inited = 1;
            ixfs_flush_index = 0;

            /* Create kernel.log if needed */
            struct vfs_node *root = vfs_get_drive_root('C');
            if (root && root->ops && root->ops->finddir) {
                struct vfs_node *imp = root->ops->finddir(root, "Impossible");
                if (imp && imp->ops && imp->ops->finddir) {
                    struct vfs_node *sys_dir = imp->ops->finddir(imp, "System");
                    if (sys_dir && sys_dir->ops && sys_dir->ops->finddir) {
                        struct vfs_node *logs = sys_dir->ops->finddir(sys_dir, "Logs");
                        if (logs && logs->ops && logs->ops->create) {
                            logs->ops->create(logs, "kernel.log", VFS_FILE);
                        }
                    }
                }
            }
        }

        ring = klog_get_ring(&ring_count, &ring_head);
        if (ring && ring_count > 0 && ixfs_flush_index < ring_count) {
            logfile = vfs_open("C:\\Impossible\\System\\Logs\\kernel.log",
                               VFS_O_WRITE);
            if (logfile) {
                /* Batch all entries into a single write to avoid
                 * per-entry AHCI DMA overhead (was causing 19s stall) */
                uint32_t batch_pages = 8;  /* 32 KB batch buffer */
                uint8_t *batch = (uint8_t *)(uintptr_t)
                    pmm_alloc_contiguous(batch_pages);

                if (batch) {
                    uint32_t batch_size = batch_pages * 4096;
                    uint32_t batch_pos = 0;
                    uint32_t write_offset = (uint32_t)logfile->size;

                    for (i = ixfs_flush_index; i < ring_count; i++) {
                        uint32_t idx;
                        if (ring_count < KLOG_RING_SIZE) {
                            idx = i;
                        } else {
                            idx = (ring_head +
                                   (i - (ring_count - KLOG_RING_SIZE)))
                                  % KLOG_RING_SIZE;
                        }

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

                    {
                        uint32_t pg;
                        for (pg = 0; pg < batch_pages; pg++)
                            pmm_free_frame((uintptr_t)batch + pg * 4096);
                    }
                }

                ixfs_flush_index = ring_count;
                vfs_close(logfile);
            }

            /* ---- Per-subsystem log routing ---- */
            /* Write each entry to its subsystem-specific log file.
             * Uses the same ring range that was just flushed to kernel.log.
             * Each subsystem file is opened, appended-to, and closed per flush
             * to avoid holding many VFS handles open across boot phases. */
            {
                uint32_t si;
                for (si = 0; si < SUBSYS_LOG_COUNT; si++) {
                    char spath[64];
                    uint32_t sp = 0;
                    struct vfs_node *sf;
                    const char *base = "C:\\Impossible\\System\\Logs\\";
                    const char *fname = s_subsys_filenames[si];
                    int k;

                    /* Build path */
                    for (k = 0; base[k]; k++) spath[sp++] = base[k];
                    for (k = 0; fname[k]; k++) spath[sp++] = fname[k];
                    spath[sp] = '\0';

                    sf = vfs_open(spath, VFS_O_WRITE);
                    if (!sf) continue;

                    {
                        uint32_t sfl_start = (ring_count > KLOG_RING_SIZE)
                            ? ring_count - KLOG_RING_SIZE : 0;
                        uint32_t write_off = (uint32_t)sf->size;

                        for (i = sfl_start; i < ring_count; i++) {
                            uint32_t idx;
                            if (ring_count < KLOG_RING_SIZE)
                                idx = i;
                            else
                                idx = (ring_head + (i - (ring_count - KLOG_RING_SIZE)))
                                      % KLOG_RING_SIZE;

                            const char *df = dispatch_filename(ring[idx].subsystem);
                            /* Check if this entry belongs to this subsystem file */
                            {
                                const char *a = df;
                                const char *b = fname;
                                while (*a && *a == *b) { a++; b++; }
                                if (*a != '\0' || *b != '\0')
                                    continue;  /* not this file */
                            }

                            {
                                char line[256];
                                int pos = format_entry(&ring[idx], line, 256);
                                vfs_write(sf, write_off, (uint32_t)pos, (uint8_t *)line);
                                write_off += (uint32_t)pos;
                            }
                        }
                    }
                    vfs_close(sf);
                }
            }
        }
    }

    /* ---- Flush to FAT32 X: drive (full-file overwrite) ---- */
    if (vfs_is_mounted('X') && fat32_buf && fat32_pos > 0) {
        /* Lazy init: if we haven't set up yet, do it now */
        if (!fat32_inited) {
            pick_log_number();
            struct vfs_node *x_root = vfs_get_drive_root('X');
            if (x_root && x_root->ops && x_root->ops->create)
                x_root->ops->create(x_root, log_filename, VFS_FILE);
            fat32_inited = 1;
        }

        /* If no init happened yet or no filename, fall back to serial.log */
        if (log_filename[0]) {
            /* Build full path: "X:\BOOT_NNN.LOG" */
            char path[20];
            int p = 0;
            path[p++] = 'X';
            path[p++] = ':';
            path[p++] = '\\';
            {
                int j;
                for (j = 0; log_filename[j]; j++)
                    path[p++] = log_filename[j];
            }
            path[p] = '\0';

            logfile = vfs_open(path, VFS_O_WRITE);
            if (logfile) {
                vfs_write(logfile, 0, fat32_pos, fat32_buf);
                vfs_close(logfile);
            }
        }
    } else if (vfs_is_mounted('X') && !fat32_buf) {
        /* No PMM buffer — allocate on demand and do a ring-buffer dump */
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
            struct vfs_node *x_root = vfs_get_drive_root('X');
            if (x_root && x_root->ops && x_root->ops->create)
                x_root->ops->create(x_root, log_filename, VFS_FILE);
            fat32_inited = 1;
        }

        if (log_filename[0]) {
            char path[20];
            int p = 0;
            path[p++] = 'X';
            path[p++] = ':';
            path[p++] = '\\';
            {
                int j;
                for (j = 0; log_filename[j]; j++)
                    path[p++] = log_filename[j];
            }
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
