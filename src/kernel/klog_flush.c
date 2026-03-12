/* ============================================================================
 * klog_flush.c — Flush kernel log ring buffer to disk
 *
 * Writes accumulated klog entries to C:\Impossible\System\Logs\kernel.log.
 * Called once after VFS + IXFS mount completes, and periodically via timer.
 * Handles the case where the filesystem isn't mounted yet by buffering.
 * ============================================================================ */

#include "kernel/klog.h"
#include "kernel/fs/vfs.h"
#include "kernel/drivers/serial.h"

/* Track how many ring entries we've already flushed */
static uint32_t flush_index;
static int       flush_inited;

/* Level name strings for log file output */
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

/* Simple unsigned-to-decimal helper (avoids printk dependency) */
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



/* Ensure directory exists (creates recursively) */
static void ensure_dir(const char *path)
{
    struct vfs_node *root;

    if (!vfs_is_mounted('C'))
        return;

    root = vfs_get_drive_root('C');
    if (!root || !root->ops || !root->ops->create)
        return;

    /* Create each directory level:
     * Impossible, Impossible\System, Impossible\System\Logs */
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

    (void)path;
}

void klog_flush_to_disk(void)
{
    uint32_t ring_count, ring_head;
    const klog_entry_t *ring;
    struct vfs_node *logfile;
    uint32_t write_offset;
    uint32_t i;

    if (!vfs_is_mounted('C'))
        return;

    /* Ensure log directory exists */
    if (!flush_inited) {
        ensure_dir("C:\\Impossible\\System\\Logs");
        flush_inited = 1;
        flush_index = 0;

        /* Create the log file if it doesn't exist */
        {
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
    }

    /* Get current ring buffer state */
    ring = klog_get_ring(&ring_count, &ring_head);
    if (!ring || ring_count == 0)
        return;

    /* Nothing new to flush? */
    if (flush_index >= ring_count)
        return;

    /* Open the log file for appending */
    logfile = vfs_open("C:\\Impossible\\System\\Logs\\kernel.log",
                       VFS_O_WRITE);
    if (!logfile)
        return;

    /* Get current file size for append offset */
    write_offset = (uint32_t)logfile->size;

    /* Write each new entry as a text line */
    for (i = flush_index; i < ring_count; i++) {
        /* Calculate actual ring index (oldest first) */
        uint32_t idx;
        if (ring_count < KLOG_RING_SIZE) {
            idx = i;
        } else {
            idx = (ring_head + (i - (ring_count - KLOG_RING_SIZE)))
                  % KLOG_RING_SIZE;
        }

        const klog_entry_t *e = &ring[idx];

        /* Format: "[TICKS] LEVEL subsys: message\n" */
        char line[256];
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
            for (j = 0; ls[j] && pos < 240; j++)
                line[pos++] = ls[j];
        }
        line[pos++] = ' ';

        /* Subsystem */
        {
            const char *ss = e->subsystem ? e->subsystem : "???";
            int j;
            for (j = 0; ss[j] && pos < 240; j++)
                line[pos++] = ss[j];
        }
        line[pos++] = ':';
        line[pos++] = ' ';

        /* Message */
        {
            int j;
            for (j = 0; e->message[j] && pos < 254; j++)
                line[pos++] = e->message[j];
        }
        line[pos++] = '\n';

        vfs_write(logfile, write_offset, (uint32_t)pos,
                  (const uint8_t *)line);
        write_offset += (uint32_t)pos;
    }

    flush_index = ring_count;
    vfs_close(logfile);
}
