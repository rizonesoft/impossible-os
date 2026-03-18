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
                }
            }
        }
    }
}

/* ---- Numbered log file on X: (FAT32) ---- */

/* Scan X: for BOOT_NNN.LOG files, find highest N, set log_filename */
static void pick_log_number(void)
{
    struct vfs_node *x_root;
    uint32_t highest = 0;

    x_root = vfs_get_drive_root('X');
    if (!x_root || !x_root->ops || !x_root->ops->finddir)
        goto fallback;

    /* Scan for BOOT_001.LOG through BOOT_999.LOG */
    {
        uint32_t n;
        char probe[16];
        for (n = 1; n <= 999; n++) {
            int p = 0;
            probe[p++] = 'B';
            probe[p++] = 'O';
            probe[p++] = 'O';
            probe[p++] = 'T';
            probe[p++] = '_';
            /* 3-digit number with leading zeros */
            probe[p++] = '0' + (char)((n / 100) % 10);
            probe[p++] = '0' + (char)((n / 10) % 10);
            probe[p++] = '0' + (char)(n % 10);
            probe[p++] = '.';
            probe[p++] = 'L';
            probe[p++] = 'O';
            probe[p++] = 'G';
            probe[p] = '\0';

            struct vfs_node *found = x_root->ops->finddir(x_root, probe);
            if (found) {
                highest = n;
            }
        }
    }

fallback:
    /* Next number (or 001 if none exist) */
    {
        uint32_t next = highest + 1;
        int p = 0;
        if (next > 999) next = 1;  /* wrap around */

        log_filename[p++] = 'B';
        log_filename[p++] = 'O';
        log_filename[p++] = 'O';
        log_filename[p++] = 'T';
        log_filename[p++] = '_';
        log_filename[p++] = '0' + (char)((next / 100) % 10);
        log_filename[p++] = '0' + (char)((next / 10) % 10);
        log_filename[p++] = '0' + (char)(next % 10);
        log_filename[p++] = '.';
        log_filename[p++] = 'L';
        log_filename[p++] = 'O';
        log_filename[p++] = 'G';
        log_filename[p] = '\0';

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
                uint32_t write_offset = (uint32_t)logfile->size;

                for (i = ixfs_flush_index; i < ring_count; i++) {
                    uint32_t idx;
                    if (ring_count < KLOG_RING_SIZE) {
                        idx = i;
                    } else {
                        idx = (ring_head + (i - (ring_count - KLOG_RING_SIZE)))
                              % KLOG_RING_SIZE;
                    }

                    char line[256];
                    int pos = format_entry(&ring[idx], line, 256);

                    vfs_write(logfile, write_offset, (uint32_t)pos,
                              (const uint8_t *)line);
                    write_offset += (uint32_t)pos;
                }

                ixfs_flush_index = ring_count;
                vfs_close(logfile);
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
