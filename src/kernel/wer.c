/* ============================================================================
 * wer.c -- Windows Error Reporting style crash report staging
 *
 * Writes a JSON crash report to X:\Crash\WER\ for each user-mode crash.
 * FAT32-readable by any OS -- no network required for post-mortem analysis.
 * ============================================================================ */

#include "kernel/wer.h"
#include "kernel/idt.h"
#include "kernel/except.h"        /* except_vector_to_status -- report status field */
#include "kernel/vectors.h"       /* VECTOR_NMI/DOUBLE_FAULT/MACHINE_CHECK -- abort-ctx gate */
#include "kernel/rtl/unwind.h"    /* RtlCaptureStackBackTrace (TODO-23 s7) -- crash frames */
#include "kernel/sched/task.h"
#include "kernel/fs/vfs.h"
#include "kernel/time/wall_clock.h"
#include "kernel/nt/filetime.h"
#include "kernel/klog.h"
#include "kernel/timer.h"
#include "kernel/drivers/serial.h" /* serial_write_recoverable -- bounded fault-safe serial for the WER hook */
#include "libc/string.h"

/* ---- Path helpers ------------------------------------------------------- */

#define WER_DIR_BB     "X:\\Crash\\WER\\"
#define WER_DIR_FB     "C:\\Impossible\\System\\Logs\\"
/* WER retention prune tunables. WER_PRUNE_BATCH is how many of the oldest
 * reports are deleted per directory enumeration: each unlink invalidates the
 * FAT32 per-directory readdir cache, so batching amortizes the cache rebuild.
 * WER_PRUNE_SCAN_GUARD bounds dirents walked per pass and is deliberately NOT
 * tied to any filesystem cache size -- vfs_readdir returning NULL is the real
 * end-of-enumeration signal; this is only a runaway backstop. */
#define WER_PRUNE_BATCH         8u
#define WER_PRUNE_SCAN_GUARD    8192u

static const char *wer_dir(void)
{
    extern int klog_using_blackbox;
    return klog_using_blackbox ? WER_DIR_BB : WER_DIR_FB;
}

/* True if `name` ends with ".json" (case-sensitive, as the writer emits). */
static int wer_name_is_json(const char *name)
{
    int len = 0;
    while (name[len]) len++;
    return len >= 5 && name[len-5] == '.' && name[len-4] == 'j'
        && name[len-3] == 's' && name[len-2] == 'o' && name[len-1] == 'n';
}

/* Extract the 14-digit YYYYMMDDHHMMSS timestamp from a PID_<ts>.json report
 * name into *out. Returns 1 only when exactly 14 digits followed by '.' come
 * after the first '_'; the wall-clock-not-ready writer fallback
 * (PID_<ticks>.json) has a non-14-digit tail and returns 0 so the caller
 * treats it as oldest (pruned first). */
static int wer_name_timestamp(const char *name, uint64_t *out)
{
    int i = 0;
    uint64_t ts = 0;
    int dgt;
    while (name[i] && name[i] != '_') i++;
    if (name[i] != '_') return 0;
    i++;
    for (dgt = 0; dgt < 14; dgt++) {
        char c = name[i + dgt];
        if (c < '0' || c > '9') return 0;
        ts = ts * 10 + (uint64_t)(c - '0');
    }
    if (name[i + 14] != '.') return 0;   /* more digits == PID_<ticks> fallback */
    *out = ts;
    return 1;
}

/* FNV-1a hash of a report name, for the bounded failed-unlink set below
 * (storing full names would blow the kernel stack). */
static uint32_t wer_name_hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

/* Keep the WER_PRUNE_BATCH oldest deletable candidates in vict/vkey,
 * insertion-sorted ascending by timestamp key (oldest first). A new (name,key)
 * is dropped when the batch is full and the candidate is not older than the
 * youngest victim already held. */
static void wer_victim_insert(char vict[][VFS_MAX_NAME], uint64_t *vkey,
                              int *nvict, const char *name, uint64_t key)
{
    int cap = (int)WER_PRUNE_BATCH;
    int pos, j;

    if (*nvict >= cap && key >= vkey[cap - 1])
        return;
    pos = (*nvict < cap) ? *nvict : cap - 1;
    while (pos > 0 && vkey[pos - 1] > key) {
        int k;
        vkey[pos] = vkey[pos - 1];
        for (k = 0; k < VFS_MAX_NAME; k++) vict[pos][k] = vict[pos - 1][k];
        pos--;
    }
    vkey[pos] = key;
    for (j = 0; name[j] && j < VFS_MAX_NAME - 1; j++) vict[pos][j] = name[j];
    vict[pos][j] = '\0';
    if (*nvict < cap) (*nvict)++;
}

/* WER retention: prune the WER report directory toward at most `max` JSON
 * reports, deleting oldest-first (oldest = smallest 14-digit timestamp; a name
 * lacking a valid 14-digit timestamp sorts as key 0, so the writer's
 * PID_<ticks> wall-clock-not-ready fallback reports are pruned first). Each
 * pass enumerates the directory, collects the WER_PRUNE_BATCH oldest deletable
 * reports, unlinks them, then re-enumerates until a full pass sees <= max
 * reports. Batching bounds the FAT32 dir-cache rebuilds an unlink forces.
 *
 * Best-effort, not exact: FAT32 vfs_readdir surfaces only the first
 * FAT32_MAX_DIR_ENTRIES live entries with no truncation signal, so a WER
 * directory padded with that many *foreign* (non-report) files ahead of real
 * reports can hide reports past the window and converge above the cap. In
 * practice X:\Crash\WER holds only PID_*.json reports, so the window always
 * exposes real reports and the loop drives the count to the cap; exact
 * enforcement under arbitrary directory contents needs a VFS readdir
 * truncation signal (owned by the FAT32 hardening TODO).
 *
 * NOT called from wer_write_crash_report (the ISR exception path) -- only at X:
 * mount and in the BlackBox low-space disk cleanup, both non-exception
 * contexts: adding VFS enumeration/deletion to the fault path would compound
 * the deferred crash-report-writer reentrancy/deadlock risk. Returns the
 * number of reports deleted. */
uint32_t wer_prune_reports(const char *dir, uint32_t max)
{
    char dirpath[VFS_MAX_PATH];
    int n = 0;
    uint32_t total_deleted = 0;
    int guard = 0;
    uint32_t failed_h[32];   /* hashes of reports whose unlink failed; skipped */
    int nfailed = 0;

    /* Caller passes the WER directory explicitly (callers run before
     * klog_resolve_dir(), so wer_dir()'s klog_using_blackbox is not yet set).
     * Strip any trailing separator -- vfs_open of the directory wants none. */
    while (dir[n] && n < (int)sizeof(dirpath) - 1) { dirpath[n] = dir[n]; n++; }
    if (n > 0 && (dirpath[n-1] == '\\' || dirpath[n-1] == '/')) n--;
    dirpath[n] = '\0';

    for (;;) {
        struct vfs_node *dnode;
        struct vfs_dirent *de;
        uint32_t idx = 0, count = 0;
        char vict[WER_PRUNE_BATCH][VFS_MAX_NAME];
        uint64_t vkey[WER_PRUNE_BATCH];
        int nvict = 0;
        uint32_t surplus;
        int batch, v, progressed = 0;

        if (guard++ > 4096) break;   /* hard bound on passes */

        dnode = vfs_open(dirpath, VFS_O_READ);
        if (!dnode) break;

        while ((de = vfs_readdir(dnode, idx++)) != 0) {
            uint64_t ts, key;
            uint32_t h;
            int fi, skip = 0;

            if (idx > WER_PRUNE_SCAN_GUARD) break;   /* runaway backstop */
            if (de->type != VFS_FILE || !wer_name_is_json(de->name))
                continue;
            count++;
            h = wer_name_hash(de->name);
            for (fi = 0; fi < nfailed; fi++)
                if (failed_h[fi] == h) { skip = 1; break; }
            if (skip) continue;   /* a report we already failed to unlink */
            key = wer_name_timestamp(de->name, &ts) ? ts : 0;
            wer_victim_insert(vict, vkey, &nvict, de->name, key);
        }
        vfs_close(dnode);

        if (count <= max)
            break;            /* visible report count within the cap -- done */
        if (nvict == 0)
            break;            /* over cap but nothing deletable (all failed) */

        surplus = count - max;
        batch = (surplus < (uint32_t)nvict) ? (int)surplus : nvict;
        for (v = 0; v < batch; v++) {
            char path[VFS_MAX_PATH];
            int p = 0, j;
            for (j = 0; dirpath[j] && p < (int)sizeof(path) - 2; j++) path[p++] = dirpath[j];
            path[p++] = '\\';
            for (j = 0; vict[v][j] && p < (int)sizeof(path) - 1; j++) path[p++] = vict[v][j];
            path[p] = '\0';
            if (vfs_unlink(path) != 0) {
                /* Record so this report cannot stall pruning of newer deletable
                 * reports. When the failed set is full, stop rather than burn
                 * the pass guard making no progress on a corrupt directory. */
                if (nfailed >= (int)(sizeof(failed_h) / sizeof(failed_h[0]))) {
                    klog(LOG_WARN, "wer",
                         "WER retention: %d undeletable report(s); pruning stopped",
                         nfailed);
                    goto done;
                }
                failed_h[nfailed++] = wer_name_hash(vict[v]);
                continue;
            }
            total_deleted++;
            progressed = 1;
        }
        if (!progressed)
            continue;   /* whole batch failed (recorded); re-enumerate */
    }

done:
    if (total_deleted)
        klog(LOG_INFO, "wer", "WER retention: pruned %u old report(s) (cap %u)",
             (uint64_t)total_deleted, (uint64_t)max);
    return total_deleted;
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

/* ---- WER fault hook (TODO-23 s12) --------------------------------------- */

/* Pure, allocation-free formatter for the WerpReportFault serial line. Kept
 * separate from klog so the exact "wer: fault report ..." wording is
 * unit-testable without emitting to live serial. */
int wer_format_fault_line(char *buf, uint32_t bufsz, uint32_t code, uint64_t fault_addr)
{
    /* Emits the COMPLETE logical line incl. "wer: " prefix and a trailing LF, the
     * exact buffer WerpReportFault hands to serial_write -- so it is unit-testable
     * end to end. (serial_write normalizes the LF to CRLF on the UART; the wire
     * byte is CR LF, the logical line is LF.) */
    static const char pre[] = "wer: fault report code=";
    static const char mid[] = ", addr=";
    int pos = 0, j;

    if (!buf || bufsz < WER_FAULT_LINE_MAX)
        return 0;
    for (j = 0; pre[j]; j++) buf[pos++] = pre[j];
    pos += hex64(buf + pos, (uint64_t)code);
    for (j = 0; mid[j]; j++) buf[pos++] = mid[j];
    pos += hex64(buf + pos, fault_addr);
    buf[pos++] = '\n';
    buf[pos] = '\0';
    return pos;
}

/* WER fault hook: serial-only stub. Called from the fault terminal once a user
 * fault is unhandled. A real werfault.exe reporter (named pipe) is ntdll/user
 * side -> XREF: 12-user-platform-sdk/TODO-04 s5. Deliberately does NO VFS and NO
 * allocation so it is safe on the interrupts-disabled fault path (unlike the
 * JSON report writer below, whose VFS I/O carries a filed reentrancy risk --
 * TODO-24 s.BlackBox). Emits via serial_write_recoverable (the bounded
 * non-blocking writer, sized for a path the system survives), NOT klog: klog's live-disk path (klog_disk_flush)
 * allocates pages and does VFS, which would defeat the point of a safe
 * fallback. The "wer: " prefix matches the former klog subsystem tag. */
void WerpReportFault(uint32_t code, uint64_t fault_addr)
{
    /* wer_format_fault_line builds the whole "wer: ...\n" line; emit it with a
     * SINGLE call so another CPU cannot interleave its output mid-record.
     *
     * serial_write_recoverable, not serial_write: this runs on a FAULT path with
     * interrupts disabled, and plain serial_write both blocks on g_serial_lock
     * (self-deadlock if the interrupted code held it) and spins unbounded on the
     * UART transmit bit (a stuck UART stalls the report -- the risk except.c
     * documents at its WerpReportFault call site). The emergency path try-locks
     * and bounds every hardware wait, so the worst case degrades from "hang" to
     * "interleaved bytes". Unlike panic.c this caller does NOT arm emergency
     * mode, and it uses the RECOVERABLE budget -- a short per-byte wait and a
     * per-call allowance -- so a wedged UART cannot make a survivable user
     * fault stall other CPUs behind g_serial_lock. */
    char line[WER_FAULT_LINE_MAX];
    if (wer_format_fault_line(line, sizeof(line), code, fault_addr) > 0)
        serial_write_recoverable(line);
}

/* ---- Report writer ------------------------------------------------------ */

void wer_write_crash_report(struct interrupt_frame *frame, uint32_t exception,
                            uint64_t fault_addr)
{
    struct task *t;
    char path[80];
    char buf[1536];     /* header + registers + up to WER_CRASH_MAX_FRAMES frames */
    int pos = 0;
    int pi, j;
    const char *dir;
    struct vfs_node *f;

    if (!frame) return;

    /* Caller-independent abort-context invariant: NEVER do VFS/task/clock/klog
     * work from a #DF/#MC/NMI abort handler (IST stack, #PF recovery
     * unavailable). A CS-based caller gate is insufficient -- a machine check
     * can interrupt CPL3, so a user-mode #MC carries CS RPL 3 yet runs in abort
     * context. In practice this writer's only caller (except.c) already passes a
     * non-abort vector; this guard is the belt-and-suspenders safety net. Abort
     * vectors terminate via panic_screen (its trap-frame serial dump is the crash
     * evidence there). */
    if (exception == VECTOR_NMI || exception == VECTOR_DOUBLE_FAULT ||
        exception == VECTOR_MACHINE_CHECK)
        return;

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
        /* Fallback (wall clock not ready): rebased uptime ms, not raw ticks
         * (which rewind across a KeSetTimerResolution rate change). */
        pi += dec32(path + pi, (uint32_t)(uptime_ns() / 1000000ULL));
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
     * pos < 820: worst case is a name ending at the cap (+6-byte escape ~826)
     * followed by the unconditional fixed tail -- closing quote, exception/
     * status/fault_addr/rip/rsp/error_code keys+values (~166 bytes), the
     * registers block (self-skips once pos>1200), cs, the stack array (up to
     * WER_CRASH_MAX_FRAMES 0x-hex frames, self-skips near the buffer end), the
     * user_trace_available field, and closing braces/newline -- which together
     * stay within buf[1536]. */
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

    /* ,"exception":N -- the raw fault vector (e.g. 14 for #PF). */
    { const char *k = ",\"exception\":"; for (j = 0; k[j]; j++) buf[pos++] = k[j]; }
    pos += dec32(buf + pos, exception);

    /* ,"status":"0xNNN" -- the mapped exception NTSTATUS (e.g. 0xc0000005). Only
     * the 9 general-dispatch vectors map; unregistered fallback vectors (#MF/#XM/
     * #DF/#MC) have no NTSTATUS here, so emit JSON null rather than a misleading
     * "0x0" that reads as a valid status. */
    {
        NTSTATUS st = except_vector_to_status((uint8_t)exception);
        if (st != 0) {
            const char *k = ",\"status\":\""; for (j = 0; k[j]; j++) buf[pos++] = k[j];
            pos += hex64(buf + pos, (uint64_t)(uint32_t)st);
            buf[pos++] = '"';
        } else {
            const char *k = ",\"status\":null"; for (j = 0; k[j]; j++) buf[pos++] = k[j];
        }
    }

    /* ,"fault_addr":"0xNNN" or null. The sole current JSON-writer caller
     * (except.c's user-fault terminal) supplies 0 = unknown -- #GP/#NP carry no
     * CR2, and the real #PF address goes to the serial WerpReportFault line, not
     * here. Emit JSON null for the unknown case so it is not conflated with a
     * genuine fault at address 0 (matching the "status":null convention above). */
    if (fault_addr != 0) {
        const char *k = ",\"fault_addr\":\""; for (j = 0; k[j]; j++) buf[pos++] = k[j];
        pos += hex64(buf + pos, fault_addr);
        buf[pos++] = '"';
    } else {
        const char *k = ",\"fault_addr\":null"; for (j = 0; k[j]; j++) buf[pos++] = k[j];
    }

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
        for (ri = 0; ri < 8 && pos < 1200; ri++) {
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

    /* ,"stack":["0x<rip>",...] -- frame 0 is the faulting RIP (the crash PC);
     * the remainder are the kernel terminal call chain via RtlCaptureStackBackTrace
     * (TODO-23 s7, fault-safe RBP walk). User-space frames beyond frame 0 are NOT
     * walkable yet (the RBP walker rejects user PCs; user-mode RtlWalkFrameChain is
     * deferred), so "user_trace_available" is false rather than emitting a
     * misleading or attacker-forgeable trace. */
    { const char *k = ",\"stack\":[\""; for (j = 0; k[j]; j++) buf[pos++] = k[j]; }
    pos += hex64(buf + pos, frame->rip);
    buf[pos++] = '"';
    {
        /* Frame 0 (the faulting RIP above) is always safe -- it is a register
         * value, no memory walk. The kernel-frame walk below reads the live stack
         * via __kstack_read_u64, whose #PF recovery is UNAVAILABLE in a #DF/#MC/
         * NMI abort context -- but those vectors already returned early at the top
         * of this function (the caller-independent abort invariant), so control
         * only reaches here for a non-abort context where the walk is safe. */
        void *kframes[WER_CRASH_MAX_FRAMES];
        uint16_t nk = RtlCaptureStackBackTrace(0, WER_CRASH_MAX_FRAMES - 1u, kframes, 0);
        uint16_t ki;
        /* Reserve room for one more frame (<=21B) PLUS the trailing "]",
         * ,"user_trace_available":false, "}" and "\n" (~32B) so a fired guard
         * cannot overflow buf. */
        for (ki = 0; ki < nk && pos < (int)sizeof(buf) - 64; ki++) {
            buf[pos++] = ','; buf[pos++] = '"';
            pos += hex64(buf + pos, (uint64_t)(uintptr_t)kframes[ki]);
            buf[pos++] = '"';
        }
    }
    buf[pos++] = ']';

    /* ,"user_trace_available":false -- see the stack comment above. */
    { const char *k = ",\"user_trace_available\":false";
      for (j = 0; k[j]; j++) buf[pos++] = k[j]; }

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
