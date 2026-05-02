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

static int s_phase3_marked;   /* 1 = boot_history_kernel_mark_phase3 has run */

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

    s_phase3_marked = 1;
    klog(LOG_INFO, "boot_history",
         "Phase-3 mark: append seq=%u src=0xFFFF err=0x0000",
         (unsigned)new_seq);
}
