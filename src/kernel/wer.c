/* ============================================================================
 * wer.c -- Windows Error Reporting style crash report staging
 *
 * Writes a JSON crash report to X:\Crash\WER\ for each user-mode crash.
 * FAT32-readable by any OS -- no network required for post-mortem analysis.
 * ============================================================================ */

#include "kernel/wer.h"
#include "kernel/idt.h"
#include "kernel/sched/task.h"
#include "kernel/fs/vfs.h"
#include "kernel/time/wall_clock.h"
#include "kernel/nt/filetime.h"
#include "kernel/klog.h"
#include "kernel/timer.h"
#include "libc/string.h"

/* ---- Path helpers ------------------------------------------------------- */

#define WER_DIR_BB     "X:\\Crash\\WER\\"
#define WER_DIR_FB     "C:\\Impossible\\System\\Logs\\"
#define WER_MAX_REPORTS 50

static const char *wer_dir(void)
{
    extern int klog_using_blackbox;
    return klog_using_blackbox ? WER_DIR_BB : WER_DIR_FB;
}

/* ---- Hex formatting ----------------------------------------------------- */

static int hex64(char *buf, uint64_t val)
{
    static const char hx[] = "0123456789abcdef";
    int i, pos = 0;
    buf[pos++] = '0'; buf[pos++] = 'x';
    /* Skip leading zeros */
    int started = 0;
    for (i = 60; i >= 0; i -= 4) {
        int nibble = (int)((val >> i) & 0xF);
        if (nibble || started || i == 0) {
            buf[pos++] = hx[nibble];
            started = 1;
        }
    }
    return pos;
}

static int dec32(char *buf, uint32_t val)
{
    char tmp[12];
    int i = 0, pos = 0;
    if (val == 0) { buf[0] = '0'; return 1; }
    while (val > 0 && i < 10) {
        tmp[i++] = '0' + (char)(val % 10);
        val /= 10;
    }
    while (i > 0) buf[pos++] = tmp[--i];
    return pos;
}

/* ---- Report writer ------------------------------------------------------ */

void wer_write_crash_report(struct interrupt_frame *frame, uint32_t exception)
{
    struct task *t;
    char path[80];
    char buf[1024];
    int pos = 0;
    int pi, j;
    const char *dir;
    struct vfs_node *f;

    if (!frame) return;
    if (!vfs_is_mounted('X') && !vfs_is_mounted('C')) return;

    t = task_current();
    if (!t) return;

    dir = wer_dir();

    /* Build filename: PID_YYMMDDHHMMSS.json */
    pi = 0;
    for (j = 0; dir[j]; j++) path[pi++] = dir[j];

    /* PID prefix */
    pi += dec32(path + pi, t->pid);
    path[pi++] = '_';

    /* Timestamp from wall clock */
    if (wall_clock_ready()) {
        FILETIME ft = KeQuerySystemTime();
        int n = filetime_to_string(ft, path + pi, (uint32_t)(sizeof(path) - pi - 10));
        if (n > 0) {
            /* Convert ISO 8601 to filename-safe: replace : and - with nothing,
             * keep only digits + T */
            char ts[24];
            int ti = 0, si;
            for (si = 0; si < n && ti < 14; si++) {
                char c = path[pi + si];
                if (c >= '0' && c <= '9') ts[ti++] = c;
            }
            ts[ti] = '\0';
            for (si = 0; si < ti; si++) path[pi++] = ts[si];
        }
    } else {
        /* Fallback: use PIT ticks */
        pi += dec32(path + pi, (uint32_t)system_get_ticks());
    }

    path[pi++] = '.'; path[pi++] = 'j'; path[pi++] = 's';
    path[pi++] = 'o'; path[pi++] = 'n';
    path[pi] = '\0';

    /* Build JSON report */
    pos = 0;
    buf[pos++] = '{';

    /* "pid":N */
    { const char *k = "\"pid\":"; for (j = 0; k[j]; j++) buf[pos++] = k[j]; }
    pos += dec32(buf + pos, t->pid);

    /* ,"name":"xxx" -- JSON-escape so a crafted task name cannot break the
     * report syntax or inject forged fields into post-mortem tooling. Cap at
     * pos < 820 (not 890): worst case is a name ending at the cap (+6-byte
     * escape ~826) followed by the unconditional fixed tail -- closing quote,
     * exception/rip/rsp/error_code keys+values, "registers" key, the register
     * loop (which self-skips once pos>900), cs, and closing braces/newline
     * (~120 bytes) -- which together stay comfortably within buf[1024]. */
    { const char *k = ",\"name\":\""; for (j = 0; k[j]; j++) buf[pos++] = k[j]; }
    if (t->name) {
        for (j = 0; t->name[j] && pos < 820; j++) {
            unsigned char c = (unsigned char)t->name[j];
            if (c == '"' || c == '\\') {
                buf[pos++] = '\\'; buf[pos++] = (char)c;
            } else if (c < 0x20) {
                static const char hexd[] = "0123456789abcdef";
                buf[pos++] = '\\'; buf[pos++] = 'u';
                buf[pos++] = '0'; buf[pos++] = '0';
                buf[pos++] = hexd[(c >> 4) & 0xF];
                buf[pos++] = hexd[c & 0xF];
            } else {
                buf[pos++] = (char)c;
            }
        }
    }
    buf[pos++] = '"';

    /* ,"exception":N */
    { const char *k = ",\"exception\":"; for (j = 0; k[j]; j++) buf[pos++] = k[j]; }
    pos += dec32(buf + pos, exception);

    /* ,"rip":"0xNNN" */
    { const char *k = ",\"rip\":\""; for (j = 0; k[j]; j++) buf[pos++] = k[j]; }
    pos += hex64(buf + pos, frame->rip);
    buf[pos++] = '"';

    /* ,"rsp":"0xNNN" */
    { const char *k = ",\"rsp\":\""; for (j = 0; k[j]; j++) buf[pos++] = k[j]; }
    pos += hex64(buf + pos, frame->rsp);
    buf[pos++] = '"';

    /* ,"error_code":N */
    { const char *k = ",\"error_code\":"; for (j = 0; k[j]; j++) buf[pos++] = k[j]; }
    pos += dec32(buf + pos, (uint32_t)frame->err_code);

    /* ,"registers":{"rax":"0x...","rbx":"0x...",...} */
    { const char *k = ",\"registers\":{"; for (j = 0; k[j]; j++) buf[pos++] = k[j]; }
    {
        static const struct { const char *name; int off; } regs[] = {
            {"rax", 112}, {"rbx", 104}, {"rcx", 96}, {"rdx", 88},
            {"rsi", 80},  {"rdi", 72},  {"rbp", 64}, {"r8", 56},
        };
        int ri;
        for (ri = 0; ri < 8 && pos < 900; ri++) {
            if (ri > 0) buf[pos++] = ',';
            buf[pos++] = '"';
            for (j = 0; regs[ri].name[j]; j++) buf[pos++] = regs[ri].name[j];
            buf[pos++] = '"'; buf[pos++] = ':'; buf[pos++] = '"';
            pos += hex64(buf + pos, *(uint64_t *)((uint8_t *)frame + regs[ri].off));
            buf[pos++] = '"';
        }
    }
    buf[pos++] = '}';

    /* ,"cs":N -- helps identify user vs kernel mode */
    { const char *k = ",\"cs\":"; for (j = 0; k[j]; j++) buf[pos++] = k[j]; }
    pos += dec32(buf + pos, (uint32_t)frame->cs);

    buf[pos++] = '}';
    buf[pos++] = '\n';

    /* Write report */
    f = vfs_open(path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (f) {
        vfs_write(f, 0, (uint32_t)pos, (const uint8_t *)buf);
        vfs_close(f);
        klog(LOG_WARN, "wer", "Crash report: %s (PID %u, exception %u, RIP 0x%x)",
             path, (uint64_t)t->pid, (uint64_t)exception, frame->rip);
    }
}
