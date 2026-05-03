/* ============================================================================
 * boot_history.c -- kernel-side Phase-3 sentinel append for the
 * boot-error history ring (producer half).
 *
 * The bootloader writes two earlier appends per successful boot
 * (the source-section hint at any pre-EBS fatal, and BOOT_SECTION_EBS_OK
 * after ExitBootServices).  This file adds the third append site --
 * BOOT_SECTION_KERNEL_PHASE3 -- once subsystem initialization has
 * succeeded but before userland threads start consuming CPU.
 *
 * RuntimeServices->SetVariable survives ExitBootServices, so the same
 * `BootErrorHistory` ring + `BootHistorySeq` cookie variables that the
 * bootloader writes are reachable here through the kernel's
 * uefi_var_set / uefi_var_set_u32 wrappers (see uefi_vars.h).
 *
 * Atomicity: ring FIRST, cookie LAST -- mirrors the bootloader writer
 * so a torn write between the two leaves a stale-but-bounded read,
 * never UB.  Idempotent: a second call within the same boot is a
 * no-op (the once-latch lives in this file).
 *
 * The reader, BlackBox transcribe, and renderer are owned by the
 * consumer half. */

#include "kernel/types.h"
#include "kernel/boot_info.h"
#include "kernel/klog.h"
#include "kernel/uefi_vars.h"
#include "kernel/uefi_runtime.h"
#include "kernel/nt/ntstatus.h"

static int      s_phase3_marked;        /* 1 = mark_phase3 ran */
static uint32_t s_phase3_committed_seq; /* boot_seq successfully committed
                                         * to NVRAM; 0 = not committed (mark
                                         * skipped, ring write failed, or
                                         * cookie write failed).  Consumers
                                         * use this to distinguish "this
                                         * boot's seq" from "highest seq in
                                         * the ring (might be a prior boot)"
                                         * when the Phase-3 mark fails. */

/* Variable names as UCS-2 / uint16_t arrays.  Match the bootloader's
 * names exactly (include/kernel/boot_info.h header comment is the
 * canonical source). */
static const uint16_t k_hist_ring_var[] = {
    'B','o','o','t','E','r','r','o','r','H','i','s','t','o','r','y', 0
};
static const uint16_t k_hist_seq_var[] = {
    'B','o','o','t','H','i','s','t','o','r','y','S','e','q', 0
};

/* Civil-from-days algorithm (Howard Hinnant, public domain).  Matches
 * the bootloader's converter byte-for-byte so a kernel-stamped entry
 * carries the same epoch shape as a bootloader-stamped one. */
static uint32_t ymdhms_to_unix(uint16_t year, uint8_t month, uint8_t day,
                               uint8_t hour, uint8_t minute, uint8_t second)
{
    int y = (int)year - (month <= 2 ? 1 : 0);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = (long long)era * 146097 + (long long)doe - 719468;
    long long unix_seconds = days * 86400
                           + (long long)hour * 3600
                           + (long long)minute * 60
                           + (long long)second;
    if (unix_seconds < 0)
        return 0;
    if (unix_seconds > 0xFFFFFFFFLL)
        return 0xFFFFFFFFu;
    return (uint32_t)unix_seconds;
}

/* Best-effort wall-clock read via the kernel's UEFI runtime wrapper.
 * uefi_get_time returns 0 (EFI_SUCCESS) on success and an EFI status
 * code otherwise; both shapes are tolerated since the consumer
 * renderer treats unix_time == 0 as "(no clock)". */
static uint32_t boot_history_kernel_now_unix(void)
{
    struct efi_time t = { 0 };
    uint64_t s = uefi_get_time(&t, NULL);
    if (s != 0)
        return 0;
    if (t.year < 1970 || t.year > 2106 || t.month < 1 || t.month > 12 ||
        t.day < 1 || t.day > 31 || t.hour > 23 || t.minute > 59 || t.second > 60)
        return 0;
    return ymdhms_to_unix(t.year, t.month, t.day, t.hour, t.minute, t.second);
}

void boot_history_kernel_mark_phase3(void)
{
    if (s_phase3_marked)
        return;

    efi_guid_t guid = IMPOSSIBLE_OS_VENDOR_GUID_INIT;

    /* Canonical attribute set the writer side ALWAYS uses (matches
     * UEFI_VAR_NV_BOOT_RUNTIME).  Reads carrying any other shape are
     * treated as untrusted (pre-OS tool, corrupted NVRAM, attacker-
     * seeded data) and repaired via delete-then-create so the next
     * uefi_var_set cannot trip EFI_INVALID_PARAMETER per UEFI 2.10
     * 7.2.1 ("SetVariable cannot change attributes on existing
     * variable").  Mirrors the bootloader-side repair in
     * src/boot/uefi/boot_history.c. */
    const uint32_t expected_attrs = UEFI_VAR_NV_BOOT_RUNTIME;

    /* Read existing cookie (0 if absent or untrusted-and-repaired).
     * uefi_var_get maps EFI_BUFFER_TOO_SMALL -> STATUS_BUFFER_TOO_SMALL;
     * an oversized cookie is a found-but-malformed record (pre-OS
     * tool, corrupted NVRAM, attacker-seeded data) and must follow
     * the same delete-then-create repair as a wrong-attrs mismatch
     * -- mirroring the bootloader-side path in boot_history.c. */
    uint32_t seq = 0;
    {
        size_t cookie_size = sizeof(seq);
        uint32_t cookie_attrs = 0;
        NTSTATUS st = uefi_var_get(k_hist_seq_var, &guid,
                                   &seq, &cookie_size, &cookie_attrs);
        int malformed = 0;
        if (st == STATUS_BUFFER_TOO_SMALL) {
            malformed = 1;
        } else if (!NT_SUCCESS(st)) {
            seq = 0;  /* absent / unsupported -- no delete needed */
        } else if (cookie_size != sizeof(seq) || cookie_attrs != expected_attrs) {
            malformed = 1;
        }
        if (malformed) {
            klog(LOG_WARN, "boot_history",
                 "BootHistorySeq untrusted (attrs/size mismatch); repairing");
            (void)uefi_var_set(k_hist_seq_var, &guid, NULL, 0, cookie_attrs);
            seq = 0;
        }
    }

    /* Read existing 128 B ring (zero-init on absence/wrong shape). */
    struct boot_error_history_entry ring[BOOT_HIST_RING_LEN];
    for (size_t i = 0; i < BOOT_HIST_RING_LEN; i++) {
        ring[i].boot_seq = 0;
        ring[i].unix_time = 0;
        ring[i].err_code = 0;
        ring[i].source_section = 0;
        ring[i]._pad = 0;
    }
    {
        size_t ring_size = sizeof(ring);
        uint32_t ring_attrs = 0;
        NTSTATUS st = uefi_var_get(k_hist_ring_var, &guid,
                                   ring, &ring_size, &ring_attrs);
        int malformed = 0;
        if (st == STATUS_BUFFER_TOO_SMALL) {
            malformed = 1;
        } else if (!NT_SUCCESS(st)) {
            /* absent / unsupported -- leave zeroed, no delete */
        } else if (ring_size != sizeof(ring) || ring_attrs != expected_attrs) {
            malformed = 1;
        }
        if (malformed) {
            for (size_t i = 0; i < BOOT_HIST_RING_LEN; i++) {
                ring[i].boot_seq = 0;
                ring[i].unix_time = 0;
                ring[i].err_code = 0;
                ring[i].source_section = 0;
                ring[i]._pad = 0;
            }
            klog(LOG_WARN, "boot_history",
                 "BootErrorHistory untrusted (attrs/size mismatch); repairing");
            (void)uefi_var_set(k_hist_ring_var, &guid, NULL, 0, ring_attrs);
        }
    }

    uint32_t new_seq = seq + 1;
    /* Guard against u32 wrap producing new_seq=0.  boot_seq=0 is the
     * empty-slot sentinel; storing 0 hides the latest entry.  An
     * attacker-seeded cookie at UINT32_MAX hits this immediately, not
     * just after 4 billion clean boots.  Promote to 1 -- diagnostic
     * value of "newest entry" beats strict sequential continuity. */
    if (new_seq == 0)
        new_seq = 1;
    size_t head = (size_t)(new_seq % BOOT_HIST_RING_LEN);
    ring[head].boot_seq       = new_seq;
    ring[head].unix_time      = boot_history_kernel_now_unix();
    ring[head].err_code       = 0;
    ring[head].source_section = BOOT_SECTION_KERNEL_PHASE3;
    ring[head]._pad           = 0;

    /* Ring FIRST, cookie LAST.  See file-header atomicity note. */
    NTSTATUS st = uefi_var_set(k_hist_ring_var, &guid, ring, sizeof(ring),
                               UEFI_VAR_NV_BOOT_RUNTIME);
    if (!NT_SUCCESS(st)) {
        klog(LOG_WARN, "boot_history",
             "Phase-3 mark: write ring failed (NTSTATUS=0x%llx)",
             (unsigned long long)st);
        return;
    }
    st = uefi_var_set_u32(k_hist_seq_var, &guid, new_seq);
    if (!NT_SUCCESS(st)) {
        klog(LOG_WARN, "boot_history",
             "Phase-3 mark: write seq failed (NTSTATUS=0x%llx)",
             (unsigned long long)st);
        return;
    }

    s_phase3_marked        = 1;
    s_phase3_committed_seq = new_seq;
    klog(LOG_INFO, "boot_history",
         "Phase-3 mark: append seq=%u src=0xFFFF err=0x0000",
         (unsigned)new_seq);
}

/* Return the boot_seq successfully committed to NVRAM by
 * boot_history_kernel_mark_phase3(), or 0 if that mark has not run yet
 * or its NVRAM writes failed.  Consumers (boot_health publisher) use this
 * to avoid mis-attributing the current JSON to an older ring entry when
 * the Phase-3 mark could not be persisted. */
uint32_t boot_history_kernel_phase3_committed_seq(void)
{
    return s_phase3_committed_seq;
}

/* ============================================================================
 * Consumer side: reader + decoder + renderer
 * ============================================================================ */

size_t boot_history_read(struct boot_error_history_entry out_ring[BOOT_HIST_RING_LEN])
{
    /* Always zero-fill before any I/O so a partial-read failure leaves
     * the caller with a defined empty ring rather than stack garbage. */
    for (size_t i = 0; i < BOOT_HIST_RING_LEN; i++) {
        out_ring[i].boot_seq = 0;
        out_ring[i].unix_time = 0;
        out_ring[i].err_code = 0;
        out_ring[i].source_section = 0;
        out_ring[i]._pad = 0;
    }

    efi_guid_t guid = IMPOSSIBLE_OS_VENDOR_GUID_INIT;
    const uint32_t expected_attrs = UEFI_VAR_NV_BOOT_RUNTIME;

    /* Read the cookie FIRST.  The producer writes the ring first then
     * the cookie last; a torn append (ring stamped, cookie write
     * failed) leaves an entry with boot_seq > cookie that the
     * atomicity contract says must NOT be visible to the renderer.
     * The cookie is the "latest-committed sequence" oracle: any
     * ring entry with boot_seq > cookie is uncommitted, and any
     * entry with boot_seq <= cookie is durable history. */
    uint32_t committed_seq = 0;
    {
        size_t cookie_size = sizeof(committed_seq);
        uint32_t cookie_attrs = 0;
        NTSTATUS cst = uefi_var_get(k_hist_seq_var, &guid,
                                    &committed_seq, &cookie_size, &cookie_attrs);
        if (!NT_SUCCESS(cst) ||
            cookie_size != sizeof(committed_seq) ||
            cookie_attrs != expected_attrs) {
            /* Cookie absent or untrusted -- treat the ring as empty
             * regardless of its contents.  A poisoned cookie cannot
             * silently inflate the visible history. */
            return 0;
        }
    }

    /* Read ring.  Mirrors the writer-side validation: any size or
     * attrs mismatch is treated as untrusted and reported empty.  The
     * writer-side repair path (delete + recreate) runs at append time;
     * the consumer is read-only and does not delete. */
    size_t ring_size = sizeof(struct boot_error_history_entry) * BOOT_HIST_RING_LEN;
    uint32_t ring_attrs = 0;
    NTSTATUS st = uefi_var_get(k_hist_ring_var, &guid,
                               out_ring, &ring_size, &ring_attrs);
    if (!NT_SUCCESS(st))
        return 0;
    if (ring_size != sizeof(struct boot_error_history_entry) * BOOT_HIST_RING_LEN ||
        ring_attrs != expected_attrs) {
        /* Re-zero on shape mismatch; producer-side repair will land on
         * the next append. */
        for (size_t i = 0; i < BOOT_HIST_RING_LEN; i++) {
            out_ring[i].boot_seq = 0;
            out_ring[i].unix_time = 0;
            out_ring[i].err_code = 0;
            out_ring[i].source_section = 0;
            out_ring[i]._pad = 0;
        }
        return 0;
    }

    /* Count entries that are BOTH non-zero AND committed (boot_seq <=
     * committed_seq).  Filter uncommitted entries in place by zeroing
     * them so the caller's renderer cannot accidentally include a
     * torn-write entry by reading out_ring directly. */
    size_t count = 0;
    for (size_t i = 0; i < BOOT_HIST_RING_LEN; i++) {
        if (out_ring[i].boot_seq == 0)
            continue;
        if (out_ring[i].boot_seq > committed_seq) {
            /* Uncommitted: ring slot stamped but cookie never caught
             * up.  Per the atomicity contract this entry must not
             * render.  Zero it so the caller treats the slot as
             * empty. */
            out_ring[i].boot_seq = 0;
            out_ring[i].unix_time = 0;
            out_ring[i].err_code = 0;
            out_ring[i].source_section = 0;
            out_ring[i]._pad = 0;
            continue;
        }
        count++;
    }
    return count;
}

/* Append a decimal u16 to `out` at position *pos, capped at cap-1.
 * Returns nothing; updates *pos.  Used by the section decoder for
 * the "section-0xNNNN" fallback path. */
static void hex16_into(char *out, size_t *pos, size_t cap, uint16_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    if (*pos + 4 > cap - 1)
        return;
    out[(*pos)++] = hex[(v >> 12) & 0xF];
    out[(*pos)++] = hex[(v >> 8)  & 0xF];
    out[(*pos)++] = hex[(v >> 4)  & 0xF];
    out[(*pos)++] = hex[v & 0xF];
}

static void str_into(char *out, size_t *pos, size_t cap, const char *s)
{
    while (*s && *pos < cap - 1)
        out[(*pos)++] = *s++;
}

size_t boot_history_decode_source_section(uint16_t src, char *out, size_t cap)
{
    if (!out || cap == 0)
        return 0;
    size_t pos = 0;
    switch (src) {
        case BOOT_SECTION_UNKNOWN:        str_into(out, &pos, cap, "unknown");        break;
        case BOOT_SECTION_EBS_OK:         str_into(out, &pos, cap, "ebs-success");    break;
        case BOOT_SECTION_KERNEL_PHASE3:  str_into(out, &pos, cap, "kernel-Phase3");  break;
        case 0x0101u:                     str_into(out, &pos, cap, "bl-init");        break;
        case 0x0102u:                     str_into(out, &pos, cap, "bl-conf");        break;
        case 0x0103u:                     str_into(out, &pos, cap, "bl-kernel");      break;
        case 0x0104u:                     str_into(out, &pos, cap, "bl-pagetables");  break;
        case 0x0105u:                     str_into(out, &pos, cap, "bl-ebs");         break;
        default:
            str_into(out, &pos, cap, "section-0x");
            hex16_into(out, &pos, cap, src);
            break;
    }
    out[pos] = '\0';
    return pos;
}

/* Format a unix_time u32 as "YYYY-MM-DDTHH:MM:SSZ" (20 bytes incl NUL).
 * Returns 0 if unix_time == 0 ("(no clock)") or on bound failure. */
static void format_unix_time(uint32_t unix_time, char *out, size_t cap)
{
    if (cap < 21) {
        if (cap > 0) out[0] = '\0';
        return;
    }
    if (unix_time == 0) {
        const char *s = "(no clock)";
        size_t i = 0;
        while (s[i] && i < cap - 1) {
            out[i] = s[i];
            i++;
        }
        out[i] = '\0';
        return;
    }
    /* Civil-from-days decode (Howard Hinnant) -- inverse of the
     * producer-side ymdhms_to_unix converter. */
    int64_t z = (int64_t)unix_time / 86400;
    uint32_t sec_of_day = unix_time % 86400;
    int64_t days_from_epoch = z;
    int64_t z_shift = days_from_epoch + 719468;
    int64_t era = (z_shift >= 0 ? z_shift : z_shift - 146096) / 146097;
    uint32_t doe = (uint32_t)(z_shift - era * 146097);
    uint32_t yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    uint32_t doy = doe - (365*yoe + yoe/4 - yoe/100);
    uint32_t mp = (5*doy + 2)/153;
    uint32_t d = doy - (153*mp + 2)/5 + 1;
    uint32_t m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    uint32_t hh = sec_of_day / 3600;
    uint32_t mm = (sec_of_day / 60) % 60;
    uint32_t ss = sec_of_day % 60;
    /* YYYY-MM-DDTHH:MM:SSZ -- 20 chars + NUL.  Year clamped to
     * [1970, 9999] for fixed-width. */
    if (y < 1970) y = 1970;
    if (y > 9999) y = 9999;
    out[0] = '0' + (y / 1000) % 10;
    out[1] = '0' + (y / 100) % 10;
    out[2] = '0' + (y / 10) % 10;
    out[3] = '0' + y % 10;
    out[4] = '-';
    out[5] = '0' + (m / 10) % 10;
    out[6] = '0' + m % 10;
    out[7] = '-';
    out[8] = '0' + (d / 10) % 10;
    out[9] = '0' + d % 10;
    out[10] = 'T';
    out[11] = '0' + (hh / 10) % 10;
    out[12] = '0' + hh % 10;
    out[13] = ':';
    out[14] = '0' + (mm / 10) % 10;
    out[15] = '0' + mm % 10;
    out[16] = ':';
    out[17] = '0' + (ss / 10) % 10;
    out[18] = '0' + ss % 10;
    out[19] = 'Z';
    out[20] = '\0';
}

/* Comparator key for stable oldest-first ordering by boot_seq.  Empty
 * slots (boot_seq == 0) are excluded by the caller. */
static int by_seq_asc(const struct boot_error_history_entry *a,
                      const struct boot_error_history_entry *b)
{
    if (a->boot_seq < b->boot_seq) return -1;
    if (a->boot_seq > b->boot_seq) return 1;
    return 0;
}

void boot_history_render(void)
{
    struct boot_error_history_entry ring[BOOT_HIST_RING_LEN];
    size_t count = boot_history_read(ring);
    if (count == 0)
        return;  /* First-ever boot or read failure -- silent. */

    /* Compact non-zero entries into a contiguous prefix, then sort
     * oldest-first by boot_seq.  Insertion sort is fine for N=8. */
    struct boot_error_history_entry sorted[BOOT_HIST_RING_LEN];
    size_t n = 0;
    for (size_t i = 0; i < BOOT_HIST_RING_LEN; i++) {
        if (ring[i].boot_seq != 0)
            sorted[n++] = ring[i];
    }
    for (size_t i = 1; i < n; i++) {
        struct boot_error_history_entry key = sorted[i];
        size_t j = i;
        while (j > 0 && by_seq_asc(&sorted[j-1], &key) > 0) {
            sorted[j] = sorted[j-1];
            j--;
        }
        sorted[j] = key;
    }

    klog(LOG_INFO, "boot_history",
         "Recent boot history (%u attempts):", (unsigned)n);
    for (size_t i = 0; i < n; i++) {
        char src_label[24];
        char time_buf[24];
        boot_history_decode_source_section(sorted[i].source_section,
                                           src_label, sizeof(src_label));
        format_unix_time(sorted[i].unix_time, time_buf, sizeof(time_buf));
        klog(LOG_INFO, "boot_history",
             "  seq=%u time=%s src=%s err=0x%04X",
             (unsigned)sorted[i].boot_seq,
             time_buf,
             src_label,
             (unsigned)sorted[i].err_code);
    }
}
