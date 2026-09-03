/* boot_trend.c -- rolling boot-perf trend file + 3-run-median regression
 * alarm.  See docs/boot/boot-trend-schema.md for the wire-format spec. */

#include "kernel/types.h"
#include "kernel/boot_trend.h"
#include "kernel/boot_init.h"
#include "kernel/boot_timing.h"
#include "kernel/boot_info.h"
#include "kernel/fs/vfs.h"
#include "kernel/util/json_builder.h"
#include "kernel/json.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/heap.h"
#include "kernel/klog.h"
#include "kernel/printk.h"

#define BOOT_TREND_PATH        "X:\\Perf\\boot-trend.json"
#define BOOT_TREND_TMP_PATH    "X:\\Perf\\boot-trend.json.tmp"
#define BOOT_TREND_BUF_PAGES   4u
#define BOOT_TREND_BUF_SIZE    (BOOT_TREND_BUF_PAGES * 4096u)
#define BOOT_TREND_QUARANTINE_PREFIX "X:\\Perf\\boot-trend.json.corrupt-"

uint32_t boot_trend_median3(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t hi = a > b ? a : b;
    hi = hi > c ? hi : c;
    uint32_t lo = a < b ? a : b;
    lo = lo < c ? lo : c;
    uint64_t sum = (uint64_t)a + (uint64_t)b + (uint64_t)c;
    return (uint32_t)(sum - hi - lo);
}

uint32_t boot_trend_compute_growth_pct(uint32_t prior_median,
                                        uint32_t newest_median)
{
    if (prior_median == 0)
        return 0;
    if (newest_median <= prior_median)
        return 0;
    uint64_t delta = (uint64_t)(newest_median - prior_median) * 100u;
    return (uint32_t)(delta / prior_median);
}

static int trend_read_file(uint8_t *buf, uint32_t cap, uint32_t *out_len)
{
    struct vfs_node *f = vfs_open(BOOT_TREND_PATH, VFS_O_READ);
    if (!f) {
        *out_len = 0;
        return 0;
    }
    struct vfs_stat st;
    if (vfs_stat(BOOT_TREND_PATH, &st) != 0 || st.size == 0) {
        vfs_close(f);
        *out_len = 0;
        return 0;
    }
    if (st.size >= cap) {
        klog(LOG_WARN, "boot_trend",
             "existing file too large (%u bytes); ignoring",
             (uint64_t)st.size);
        vfs_close(f);
        *out_len = 0;
        return 0;
    }
    int got = vfs_read(f, 0, (uint32_t)st.size, buf);
    vfs_close(f);
    if (got < 0 || (uint32_t)got != (uint32_t)st.size) {
        klog(LOG_WARN, "boot_trend",
             "short read (got=%u expected=%u); ignoring",
             (uint64_t)got, (uint64_t)st.size);
        *out_len = 0;
        return 0;
    }
    buf[got] = '\0';
    *out_len = (uint32_t)got;
    return 0;
}

static void trend_quarantine_corrupt(uint32_t seq)
{
    char path[64];
    int n = 0;
    const char *p = BOOT_TREND_QUARANTINE_PREFIX;
    while (*p && n < (int)sizeof(path) - 12) path[n++] = *p++;
    char num[11]; int ni = 0;
    if (seq == 0) num[ni++] = '0';
    else { uint32_t v = seq; while (v) { num[ni++] = (char)('0' + (v % 10)); v /= 10; } }
    while (ni && n < (int)sizeof(path) - 1) path[n++] = num[--ni];
    path[n] = '\0';
    int rc = vfs_rename_ex(BOOT_TREND_PATH, path,
                            VFS_RENAME_REPLACE_EXISTING);
    if (rc == 0)
        klog(LOG_WARN, "boot_trend",
             "quarantined corrupt trend file -> %s", path);
    else
        klog(LOG_WARN, "boot_trend",
             "failed to quarantine corrupt trend file (rc=%d)",
             (uint64_t)rc);
}

static int trend_lookup_phase_ms(struct cJSON *boots_arr, uint32_t idx,
                                  const char *phase_name, uint32_t *out_ms)
{
    struct cJSON *entry = json_array_get(boots_arr, idx);
    if (!entry) return 0;
    struct cJSON *durations = json_get(entry, "phase_durations_ms");
    if (!durations) return 0;
    struct cJSON *val = json_get(durations, phase_name);
    if (!val) return 0;
    int valid = 0;
    uint32_t ms = json_u32(val, &valid);
    if (!valid) return 0;
    *out_ms = ms;
    return 1;
}

/* Walk live boot-perf step list, compare prior-3 median vs newest-3
 * median per phase, emit BOOT-TREND WARN when growth > threshold.
 * Caller has already prepended current boot to boots_arr (indices
 * 0..2 = newest including this boot, 3..5 = prior). */
static void trend_check_regressions(struct cJSON *boots_arr)
{
    if (json_array_size(boots_arr) < 6) {
        klog(LOG_INFO, "boot_trend",
             "need 6 boots for trend window (have %u); skipping",
             (uint64_t)json_array_size(boots_arr));
        return;
    }

    const boot_timing_step_t *steps = (const boot_timing_step_t *)0;
    uint32_t step_count = boot_timing_get_steps(&steps);

    for (uint32_t i = 0; i < step_count; i++) {
        const char *name = steps[i].step;
        if (!name) continue;
        uint32_t n0 = 0, n1 = 0, n2 = 0;
        uint32_t p0 = 0, p1 = 0, p2 = 0;
        int have_newest = trend_lookup_phase_ms(boots_arr, 0, name, &n0)
                        + trend_lookup_phase_ms(boots_arr, 1, name, &n1)
                        + trend_lookup_phase_ms(boots_arr, 2, name, &n2);
        int have_prior  = trend_lookup_phase_ms(boots_arr, 3, name, &p0)
                        + trend_lookup_phase_ms(boots_arr, 4, name, &p1)
                        + trend_lookup_phase_ms(boots_arr, 5, name, &p2);
        if (have_newest != 3 || have_prior != 3)
            continue;

        uint32_t newest_med = boot_trend_median3(n0, n1, n2);
        uint32_t prior_med  = boot_trend_median3(p0, p1, p2);
        uint32_t pct = boot_trend_compute_growth_pct(prior_med, newest_med);
        if (pct > BOOT_TREND_WARN_THRESHOLD) {
            klog(LOG_WARN, "boot_trend",
                 "BOOT-TREND: %s 3-run median grew %u%% (%ums -> %ums)",
                 name, (uint64_t)pct,
                 (uint64_t)prior_med, (uint64_t)newest_med);
        }
    }
}

static void trend_emit_current_entry(struct json_builder *jb,
                                      uint32_t boot_seq, uint32_t unix_time)
{
    jb_putc(jb, '{');
    jb_puts(jb, "\"boot_seq\":");  jb_u32_dec(jb, boot_seq);
    jb_puts(jb, ",\"unix_time\":"); jb_u32_dec(jb, unix_time);
    jb_puts(jb, ",\"phase_durations_ms\":{");

    const boot_timing_step_t *steps = (const boot_timing_step_t *)0;
    uint32_t step_count = boot_timing_get_steps(&steps);
    int first = 1;
    /* Mirrors boot_perf_budget_check + boot_health perf_breaches: for
     * each step i (i+1 < step_count), elapsed = steps[i+1].tsc -
     * steps[i].tsc, labeled with steps[i].step. */
    for (uint32_t i = 0; i + 1 < step_count; i++) {
        const char *name = steps[i].step;
        if (!name) continue;
        uint64_t delta_ticks = (steps[i + 1].tsc > steps[i].tsc)
                             ? steps[i + 1].tsc - steps[i].tsc : 0;
        uint32_t elapsed_ms = boot_timing_tsc_delta_ms(delta_ticks);
        if (!first) jb_putc(jb, ',');
        first = 0;
        jb_str(jb, name);
        jb_putc(jb, ':');
        jb_u32_dec(jb, elapsed_ms);
    }
    jb_puts(jb, "}}");
}

/* Probe whether a parsed boots[] entry has the required fields without
 * touching jb.  Returns 1 when entry is well-formed enough to emit. */
static int trend_entry_is_valid(struct cJSON *entry)
{
    int v = 0;
    (void)json_u32(json_get(entry, "boot_seq"), &v);
    if (!v) return 0;
    if (!json_get(entry, "phase_durations_ms")) return 0;
    return 1;
}

/* Re-emit a parsed boots[] entry into jb.  Walks the live step list to
 * pick up phase keys; phases dropped from the live kernel will not be
 * preserved across this rewrite. */
static int trend_emit_existing_entry(struct json_builder *jb,
                                      struct cJSON *entry)
{
    int valid = 0;
    uint32_t seq = json_u32(json_get(entry, "boot_seq"), &valid);
    if (!valid) return 0;
    uint32_t ut = json_u32(json_get(entry, "unix_time"), &valid);
    if (!valid) ut = 0;
    struct cJSON *durations = json_get(entry, "phase_durations_ms");
    if (!durations) return 0;

    jb_putc(jb, '{');
    jb_puts(jb, "\"boot_seq\":");   jb_u32_dec(jb, seq);
    jb_puts(jb, ",\"unix_time\":"); jb_u32_dec(jb, ut);
    jb_puts(jb, ",\"phase_durations_ms\":{");

    const boot_timing_step_t *steps = (const boot_timing_step_t *)0;
    uint32_t step_count = boot_timing_get_steps(&steps);
    int first = 1;
    for (uint32_t i = 0; i < step_count; i++) {
        const char *name = steps[i].step;
        if (!name) continue;
        struct cJSON *val = json_get(durations, name);
        if (!val) continue;
        int v = 0;
        uint32_t ms = json_u32(val, &v);
        if (!v) continue;
        if (!first) jb_putc(jb, ',');
        first = 0;
        jb_str(jb, name);
        jb_putc(jb, ':');
        jb_u32_dec(jb, ms);
    }
    jb_puts(jb, "}}");
    return 1;
}

void boot_trend_publish_json(void)
{
    /* Every path below is hardcoded to X:\\Perf\\ (BOOT_TREND_PATH and its two
     * siblings). Without BlackBox mounted, X: does not exist, so the writer
     * allocated two 16 KiB buffers, parsed and rebuilt the whole document,
     * and only then failed at vfs_open with a warning that reads like an I/O
     * error rather than "this system has no BlackBox". Refuse up front
     * instead: no artifact is produced either way, and this states the
     * limitation rather than discovering it at the end.
     *
     * boot-profile and boot-timeline DO fall back to klog_dir on C: (see
     * boot_progress.c:387). Matching them means building three paths at
     * runtime, which the current kernel image ceiling has no room for; that
     * half is parked in this TODO's section 20. This gate is not the parity
     * fix and does not replace it -- it only stops paying for an outcome
     * that is already known. */
    {
        extern int klog_using_blackbox;
        if (!klog_using_blackbox) {
            klog(LOG_INFO, "boot_trend",
                 "BlackBox not mounted; no trend file (no C: fallback yet)");
            return;
        }
    }

    /* Separate input + output buffers, each at full BOOT_TREND_BUF_SIZE.
     * Splitting one allocation into halves caps each at 8 KiB and
     * silently truncates once the 16-boot ring fills up. */
    uintptr_t in_phys = pmm_alloc_contiguous(BOOT_TREND_BUF_PAGES);
    if (!in_phys) {
        klog(LOG_WARN, "boot_trend",
             "PMM alloc failed for input buffer; skipping");
        return;
    }
    uintptr_t out_phys = pmm_alloc_contiguous(BOOT_TREND_BUF_PAGES);
    if (!out_phys) {
        klog(LOG_WARN, "boot_trend",
             "PMM alloc failed for output buffer; skipping");
        for (uint32_t pp = 0; pp < BOOT_TREND_BUF_PAGES; pp++)
            pmm_free_frame(in_phys + pp * 4096u);
        return;
    }
    char *buf = (char *)in_phys;

    uint32_t cur_seq = boot_history_kernel_phase3_committed_seq();
    uint32_t cur_unix = 0;
    if (cur_seq != 0) {
        struct boot_error_history_entry ring[BOOT_HIST_RING_LEN];
        (void)boot_history_read(ring);
        for (uint32_t j = 0; j < BOOT_HIST_RING_LEN; j++) {
            if (ring[j].boot_seq == cur_seq) {
                cur_unix = ring[j].unix_time;
                break;
            }
        }
    }

    uint32_t in_len = 0;
    /* Pass BOOT_TREND_BUF_SIZE so trend_read_file accepts files up to
     * BOOT_TREND_BUF_SIZE - 1 bytes (writer can emit exactly that
     * size before truncation; reader's NUL goes at byte cap-1). */
    (void)trend_read_file((uint8_t *)buf, BOOT_TREND_BUF_SIZE, &in_len);

    struct cJSON *root = (struct cJSON *)0;
    struct cJSON *boots_arr = (struct cJSON *)0;
    int existing_valid = 0;
    if (in_len > 0) {
        root = json_parse(buf);
        if (root) {
            int v = 0;
            uint32_t sv = json_u32(json_get(root, "schema_version"), &v);
            struct cJSON *b = json_get(root, "boots");
            if (v && sv == 1u && b && json_is_array(b)) {
                boots_arr = b;
                existing_valid = 1;
            } else {
                klog(LOG_WARN, "boot_trend",
                     "existing file failed top-level schema check; quarantining");
                json_free(root);
                root = (struct cJSON *)0;
                trend_quarantine_corrupt(cur_seq);
            }
        } else {
            klog(LOG_WARN, "boot_trend",
                 "existing file unparseable; quarantining");
            trend_quarantine_corrupt(cur_seq);
        }
    }

    char *out_buf = (char *)out_phys;
    uint32_t out_cap = BOOT_TREND_BUF_SIZE;
    struct json_builder jb;
    jb_init(&jb, out_buf, out_cap);

    jb_putc(&jb, '{');
    jb_puts(&jb, "\"schema_version\":1,\"boots\":[");
    trend_emit_current_entry(&jb, cur_seq, cur_unix);

    uint32_t kept = 1;
    if (existing_valid && boots_arr) {
        /* Linear walk over cJSON's child list. The previous indexed form
         * called json_array_get once per iteration, and that is itself an
         * O(N) walk under a linked list of children, so the loop was O(N^2)
         * on a dense or malformed file.
         *
         * The scan is bounded SEPARATELY from what it keeps: a file holding
         * thousands of entries that all fail trend_entry_is_valid would
         * otherwise be walked in full to keep nothing. Hitting the bound is
         * NOT corruption -- the document parsed and its top-level schema
         * checked out -- so it is canonicalized rather than quarantined:
         * warn, keep the valid prefix, and let the atomic rewrite below
         * bring the file back to at most BOOT_TREND_RING_DEPTH entries.
         * Quarantine stays reserved for a document that does not parse or
         * fails the schema check, which is the only case where the bytes on
         * disk cannot be repaired by rewriting them. Quarantining a merely
         * oversized file would throw away the whole history and suppress
         * regression comparison until enough boots accumulate again. */
        uint32_t seen = 0;
        struct cJSON *e = json_array_first(boots_arr);
        for (; e; e = json_array_next(e)) {
            enum boot_trend_scan_action act =
                boot_trend_scan_action(++seen, kept);
            if (act == BOOT_TREND_SCAN_STOP) break;
            if (act == BOOT_TREND_SCAN_COUNT_ONLY) continue;
            if (!trend_entry_is_valid(e)) continue;
            jb_putc(&jb, ',');
            (void)trend_emit_existing_entry(&jb, e);
            kept++;
        }
        if (seen > BOOT_TREND_MAX_SCAN)
            klog(LOG_WARN, "boot_trend",
                 "existing boots array over %u entries; kept the valid prefix",
                 (uint64_t)BOOT_TREND_MAX_SCAN);
    }
    jb_puts(&jb, "]}");

    if (jb_truncated(&jb)) {
        klog(LOG_WARN, "boot_trend",
             "JSON serialization truncated; skipping disk write");
        if (root) json_free(root);
        for (uint32_t pp = 0; pp < BOOT_TREND_BUF_PAGES; pp++) {
            pmm_free_frame(in_phys + pp * 4096u);
            pmm_free_frame(out_phys + pp * 4096u);
        }
        return;
    }

    /* json_builder never writes a trailing NUL; cJSON's strlen-based
     * parser would otherwise read past jb_pos into uninitialized PMM
     * bytes.  Pos < cap is guaranteed since !jb_truncated. */
    out_buf[jb_pos(&jb)] = '\0';

    /* Re-parse just-built output to feed regression scan with the
     * prepended array (boots_arr is the OLD array). */
    struct cJSON *new_root = json_parse(jb_buf(&jb));
    if (new_root) {
        struct cJSON *new_boots = json_get(new_root, "boots");
        if (new_boots && json_is_array(new_boots))
            trend_check_regressions(new_boots);
        json_free(new_root);
    }

    if (root) json_free(root);

    struct vfs_node *f = vfs_open(BOOT_TREND_TMP_PATH,
                                   VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (!f) {
        klog(LOG_WARN, "boot_trend",
             "could not open %s for write", BOOT_TREND_TMP_PATH);
        for (uint32_t pp = 0; pp < BOOT_TREND_BUF_PAGES; pp++) {
            pmm_free_frame(in_phys + pp * 4096u);
            pmm_free_frame(out_phys + pp * 4096u);
        }
        return;
    }
    int wrote = vfs_write(f, 0, (uint32_t)jb_pos(&jb),
                           (uint8_t *)jb_buf(&jb));
    vfs_close(f);
    if (wrote != (int)jb_pos(&jb)) {
        klog(LOG_WARN, "boot_trend",
             "short write to %s (wrote=%d expected=%u); abandoning .tmp",
             BOOT_TREND_TMP_PATH, (uint64_t)wrote, (uint64_t)jb_pos(&jb));
        (void)vfs_unlink(BOOT_TREND_TMP_PATH);
        for (uint32_t pp = 0; pp < BOOT_TREND_BUF_PAGES; pp++) {
            pmm_free_frame(in_phys + pp * 4096u);
            pmm_free_frame(out_phys + pp * 4096u);
        }
        return;
    }

    int rc = vfs_rename_ex(BOOT_TREND_TMP_PATH, BOOT_TREND_PATH,
                            VFS_RENAME_REPLACE_EXISTING);
    if (rc != 0) {
        klog(LOG_WARN, "boot_trend",
             "atomic rename failed (rc=%d); .tmp left in place",
             (uint64_t)rc);
    } else {
        klog(LOG_INFO, "boot_trend",
             "wrote %s (%u bytes, %u boots)",
             BOOT_TREND_PATH, (uint64_t)jb_pos(&jb), (uint64_t)kept);
    }

    for (uint32_t pp = 0; pp < BOOT_TREND_BUF_PAGES; pp++) {
        pmm_free_frame(in_phys + pp * 4096u);
        pmm_free_frame(out_phys + pp * 4096u);
    }
}
