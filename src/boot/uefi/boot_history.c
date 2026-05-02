/* ============================================================================
 * boot_history.c -- bootloader-side append into the boot-error history ring
 *
 * Producer half of the boot-error history feature.  Three append sites
 * call into boot_history_append():
 *   1. boot_fatal()             -- any fatal pre-EBS, source_section = g_boot_section
 *   2. EBS retry success branch -- sentinel BOOT_SECTION_EBS_OK
 *   3. (kernel-side)            -- sentinel BOOT_SECTION_KERNEL_PHASE3 via
 *                                  boot_history_kernel_mark_phase3()
 *
 * Storage model:
 *   - `BootErrorHistory` UEFI variable: 128 bytes (8 entries x 16 bytes),
 *     NV+BS+RT attrs, IMPOSSIBLE_OS_VENDOR_GUID.
 *   - `BootHistorySeq` UEFI variable:   4 bytes (u32 monotonic cookie),
 *     same attrs and GUID.
 *
 * Atomicity:
 *   SetVariable is per-variable -- there is no transactional batch.
 *   This writer always writes the RING first, then increments the
 *   COOKIE.  A torn write between the two leaves the cookie pointing
 *   at the previous slot (head = seq % 8 returns a stale-but-bounded
 *   value), never UB.  The reader (consumer half) recomputes head from
 *   the cookie and treats out-of-range indices as zero.
 *
 * Failure mode:
 *   Any SetVariable failure WARNs to serial and returns; the call site
 *   never blocks on persistence and never aborts the boot or fatal
 *   path.  History is best-effort diagnostics, not load-bearing state.
 *
 * The kernel-side reader, BlackBox transcribe, and renderer are owned
 * by the consumer half. */

#include "efi.h"
#include "boot_info_mirror.h"

extern EFI_SYSTEM_TABLE *gST;
extern EFI_GUID g_impossible_os_guid;
extern void serial_early_print(const char *s);

/* Variable names (UCS-2 / CHAR16); kept file-local so the writer is
 * the sole producer of these names in the bootloader. */
static CHAR16 g_hist_ring_var[] = u"BootErrorHistory";
static CHAR16 g_hist_seq_var[]  = u"BootHistorySeq";

/* EFI_TIME layout (UEFI 2.10 Table 50): Year(u16) Month(u8) Day(u8)
 * Hour(u8) Minute(u8) Second(u8) Pad1(u8) Nanosecond(u32) TimeZone(i16)
 * Daylight(u8) Pad2(u8).  Our efi.h declares GetTime as VOID*; read
 * the leading 7 bytes through a byte buffer and convert. */
static UINT32 efi_time_to_unix(UINT16 year, UINT8 month, UINT8 day,
                               UINT8 hour, UINT8 minute, UINT8 second)
{
    /* Civil-from-days algorithm (Howard Hinnant, public domain).
     * Returns days since 1970-01-01 for any proleptic Gregorian date
     * in [1970-01-01, 2106-02-07]; outside that the u32 result wraps. */
    int y = (int)year - (month <= 2 ? 1 : 0);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);              /* [0, 399] */
    unsigned doy = (unsigned)((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1); /* [0, 365] */
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;  /* [0, 146096] */
    long long days = (long long)era * 146097 + (long long)doe - 719468;
    long long unix_seconds = days * 86400
                           + (long long)hour * 3600
                           + (long long)minute * 60
                           + (long long)second;
    if (unix_seconds < 0)
        return 0;
    if (unix_seconds > 0xFFFFFFFFLL)
        return 0xFFFFFFFFu;
    return (UINT32)unix_seconds;
}

static UINT32 boot_history_now_unix(void)
{
    if (!gST || !gST->RuntimeServices || !gST->RuntimeServices->GetTime)
        return 0;
    UINT8 tbuf[20] = { 0 };
    EFI_STATUS s = gST->RuntimeServices->GetTime(tbuf, (void *)0);
    if (EFI_ERROR(s))
        return 0;
    UINT16 year   = (UINT16)tbuf[0] | ((UINT16)tbuf[1] << 8);
    UINT8  month  = tbuf[2];
    UINT8  day    = tbuf[3];
    UINT8  hour   = tbuf[4];
    UINT8  minute = tbuf[5];
    UINT8  second = tbuf[6];
    /* Reject obviously-invalid clocks (firmware before BIOS time-set);
     * the consumer renders unix_time = 0 as "(no clock)". */
    if (year < 1970 || year > 2106 || month < 1 || month > 12 ||
        day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60)
        return 0;
    return efi_time_to_unix(year, month, day, hour, minute, second);
}

static UINT32 boot_history_read_seq(void)
{
    if (!gST || !gST->RuntimeServices || !gST->RuntimeServices->GetVariable)
        return 0;
    UINT32 seq = 0;
    UINTN size = sizeof(seq);
    UINT32 attrs = 0;
    EFI_STATUS s = gST->RuntimeServices->GetVariable(
        g_hist_seq_var, &g_impossible_os_guid, &attrs, &size, &seq);
    if (EFI_ERROR(s) || size != sizeof(seq))
        return 0;
    return seq;
}

static void boot_history_read_ring(struct boot_error_history_entry *out_ring)
{
    /* Zero the buffer so a missing/truncated/oversized variable yields
     * an empty ring, never partial garbage from a prior boot. */
    UINT8 *p = (UINT8 *)out_ring;
    for (UINTN i = 0; i < BOOT_HIST_BIN_SIZE; i++)
        p[i] = 0;
    if (!gST || !gST->RuntimeServices || !gST->RuntimeServices->GetVariable)
        return;
    UINTN size = BOOT_HIST_BIN_SIZE;
    UINT32 attrs = 0;
    EFI_STATUS s = gST->RuntimeServices->GetVariable(
        g_hist_ring_var, &g_impossible_os_guid, &attrs, &size, out_ring);
    if (EFI_ERROR(s) || size != BOOT_HIST_BIN_SIZE) {
        /* Re-zero if firmware returned a partial buffer on a too-small
         * size mismatch; defensive against firmware that writes and
         * then reports a different DataSize. */
        for (UINTN i = 0; i < BOOT_HIST_BIN_SIZE; i++)
            p[i] = 0;
    }
}

static EFI_STATUS boot_history_write_ring(const struct boot_error_history_entry *ring)
{
    if (!gST || !gST->RuntimeServices || !gST->RuntimeServices->SetVariable)
        return EFI_UNSUPPORTED;
    UINT32 attrs = EFI_VARIABLE_NON_VOLATILE |
                   EFI_VARIABLE_BOOTSERVICE_ACCESS |
                   EFI_VARIABLE_RUNTIME_ACCESS;
    return gST->RuntimeServices->SetVariable(
        g_hist_ring_var, &g_impossible_os_guid,
        attrs, BOOT_HIST_BIN_SIZE, (void *)ring);
}

static EFI_STATUS boot_history_write_seq(UINT32 seq)
{
    if (!gST || !gST->RuntimeServices || !gST->RuntimeServices->SetVariable)
        return EFI_UNSUPPORTED;
    UINT32 attrs = EFI_VARIABLE_NON_VOLATILE |
                   EFI_VARIABLE_BOOTSERVICE_ACCESS |
                   EFI_VARIABLE_RUNTIME_ACCESS;
    return gST->RuntimeServices->SetVariable(
        g_hist_seq_var, &g_impossible_os_guid,
        attrs, sizeof(seq), &seq);
}

void boot_history_append(UINT16 source_section, UINT16 err_code)
{
    UINT32 seq = boot_history_read_seq();
    UINT32 new_seq = seq + 1;     /* u32 wrap at 4 billion boots is fine */

    struct boot_error_history_entry ring[BOOT_HIST_RING_LEN];
    boot_history_read_ring(ring);

    UINTN head = (UINTN)(new_seq % BOOT_HIST_RING_LEN);
    ring[head].boot_seq       = new_seq;
    ring[head].unix_time      = boot_history_now_unix();
    ring[head].err_code       = err_code;
    ring[head].source_section = source_section;
    ring[head]._pad           = 0;

    /* Ring FIRST, cookie LAST.  See file-header atomicity note. */
    EFI_STATUS s = boot_history_write_ring(ring);
    if (EFI_ERROR(s)) {
        serial_early_print("[WARN] boot_history: write ring failed\n");
        return;
    }
    s = boot_history_write_seq(new_seq);
    if (EFI_ERROR(s)) {
        /* Ring advanced but cookie didn't -- next read sees the prior
         * head and the new entry is invisible until a future append
         * succeeds in writing the cookie.  Bounded loss; not UB. */
        serial_early_print("[WARN] boot_history: write seq failed\n");
        return;
    }

    /* Single-line success log so smoke can grep
     * "[BOOT] history: append seq=" without a multi-line shape. */
    serial_early_print("[BOOT] history: append seq=");
    {
        char dec[11];
        UINT32 v = new_seq;
        int n = 0;
        if (v == 0) {
            dec[n++] = '0';
        } else {
            char tmp[11];
            int t = 0;
            while (v) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
            while (t) dec[n++] = tmp[--t];
        }
        dec[n] = 0;
        serial_early_print(dec);
    }
    serial_early_print(" src=0x");
    {
        const char *hex = "0123456789ABCDEF";
        char buf[5];
        buf[0] = hex[(source_section >> 12) & 0xF];
        buf[1] = hex[(source_section >> 8) & 0xF];
        buf[2] = hex[(source_section >> 4) & 0xF];
        buf[3] = hex[source_section & 0xF];
        buf[4] = 0;
        serial_early_print(buf);
    }
    serial_early_print(" err=0x");
    {
        const char *hex = "0123456789ABCDEF";
        char buf[5];
        buf[0] = hex[(err_code >> 12) & 0xF];
        buf[1] = hex[(err_code >> 8) & 0xF];
        buf[2] = hex[(err_code >> 4) & 0xF];
        buf[3] = hex[err_code & 0xF];
        buf[4] = 0;
        serial_early_print(buf);
    }
    serial_early_print("\n");
}
