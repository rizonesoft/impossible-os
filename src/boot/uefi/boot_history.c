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

/* Canonical attribute set the writer side ALWAYS uses.  GetVariable
 * results carrying any other shape are treated as untrusted (pre-OS
 * tool, corrupted NVRAM, or attacker-seeded data) and repaired via
 * delete-then-create.  Mirrors the BootError repair pattern in
 * bootx64.c nvram_read_boot_error to avoid the UEFI 2.10 7.2.1
 * "SetVariable cannot change attributes on existing variable"
 * (returns EFI_INVALID_PARAMETER) trap. */
#define BOOT_HIST_EXPECTED_ATTRS (EFI_VARIABLE_NON_VOLATILE | \
                                  EFI_VARIABLE_BOOTSERVICE_ACCESS | \
                                  EFI_VARIABLE_RUNTIME_ACCESS)

/* Delete a variable using its CURRENT attributes (UEFI 2.10 requires
 * delete with the existing attrs).  Returns EFI_SUCCESS on success or
 * if the variable was already absent; any other error means the
 * subsequent canonical write may still trip EFI_INVALID_PARAMETER. */
static EFI_STATUS boot_history_delete(CHAR16 *var, UINT32 current_attrs)
{
    if (!gST || !gST->RuntimeServices || !gST->RuntimeServices->SetVariable)
        return EFI_UNSUPPORTED;
    return gST->RuntimeServices->SetVariable(
        var, &g_impossible_os_guid, current_attrs, 0, (void *)0);
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
    /* Per UEFI 2.10 7.2.1, GetVariable populates DataSize and
     * Attributes on success AND on EFI_BUFFER_TOO_SMALL.  An
     * oversized variable means a found-but-malformed record (a
     * pre-OS tool, corrupted NVRAM, or attacker-seeded data wrote
     * under our name+GUID).  Treat it the same as a wrong-attrs
     * mismatch: delete with the returned attrs and report absent. */
    int malformed = 0;
    if (s == EFI_BUFFER_TOO_SMALL) {
        malformed = 1;
    } else if (EFI_ERROR(s)) {
        return 0;
    } else if (size != sizeof(seq) || attrs != BOOT_HIST_EXPECTED_ATTRS) {
        malformed = 1;
    }
    if (malformed) {
        serial_early_print("[WARN] boot_history: BootHistorySeq untrusted "
                           "(attrs/size mismatch); repairing\n");
        (void)boot_history_delete(g_hist_seq_var, attrs);
        return 0;
    }
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
    /* EFI_BUFFER_TOO_SMALL = oversized variable found-but-malformed.
     * Other EFI_ERROR codes (NOT_FOUND / unsupported / access) mean
     * absent or unwritable -- leave the buffer zeroed and skip
     * delete.  On success, validate size + attrs; mismatch repairs
     * the same way as oversized. */
    int malformed = 0;
    if (s == EFI_BUFFER_TOO_SMALL) {
        malformed = 1;
    } else if (EFI_ERROR(s)) {
        return;
    } else if (size != BOOT_HIST_BIN_SIZE || attrs != BOOT_HIST_EXPECTED_ATTRS) {
        malformed = 1;
    }
    if (malformed) {
        for (UINTN i = 0; i < BOOT_HIST_BIN_SIZE; i++)
            p[i] = 0;
        serial_early_print("[WARN] boot_history: BootErrorHistory untrusted "
                           "(attrs/size mismatch); repairing\n");
        (void)boot_history_delete(g_hist_ring_var, attrs);
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
    UINT32 new_seq = seq + 1;
    /* Guard against u32 wrap producing new_seq=0.  boot_seq=0 is the
     * empty-slot sentinel that the consumer's "count non-zero entries"
     * pass uses to detect ring boundaries; storing 0 here would hide
     * the latest entry and corrupt the diagnostic.  An attacker-seeded
     * cookie at UINT32_MAX hits this on the very next append, not just
     * after 4 billion clean boots.  Skip 0 by promoting to 1; the
     * diagnostic value of "newest" beats strict sequential continuity. */
    if (new_seq == 0)
        new_seq = 1;

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
