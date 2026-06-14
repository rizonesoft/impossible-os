/* ============================================================================
 * tpm_attest_report.c -- TPM-rooted boot attestation report export.
 *
 * Currently: the immutable bootloader-to-kernel handoff snapshot the report
 * builder consumes instead of live `g_boot_info` (whose capability words are
 * refined by the kernel after handoff). Pure value copy, MMIO-free, unit-tested.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/tpm_attest_report.h"
#include "kernel/boot_info.h"
#include "libc/string.h"

void tpm_attest_handoff_capture(const struct boot_info *bi,
                                struct boot_attest_handoff *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof *out);
    if (!bi)
        return;   /* valid stays 0 -> the report builder fails closed */
    out->caps_required       = bi->caps_required;
    out->caps_present        = bi->caps_present;
    out->caps_degraded       = bi->caps_degraded;
    out->boot_path           = bi->boot_path;
    out->boot_reason         = bi->boot_reason;
    out->boot_source_flags   = bi->boot_source_flags;
    out->boot_fallback_depth = bi->boot_fallback_depth;
    out->valid               = 1u;
}

/* The one canonical Phase-0 handoff snapshot. Written ONCE on the BSP at Phase 0
 * (before APs run and before any caps refinement), then read-only -- the same
 * write-once-at-boot, lock-free-read discipline as the PCR cache and the integrity
 * report, so no lock is needed. The latch ENFORCES write-once: a later accidental
 * call with already-refined g_boot_info no-ops instead of replacing loader evidence
 * (and a lock-free reader can never race a second write because there isn't one). */
static struct boot_attest_handoff s_handoff;
static uint8_t s_handoff_latched;

void tpm_attest_handoff_snapshot_init(const struct boot_info *bi)
{
    if (s_handoff_latched)
        return;                       /* write-once: the first Phase-0 capture wins */
    tpm_attest_handoff_capture(bi, &s_handoff);
    if (s_handoff.valid)
        s_handoff_latched = 1u;       /* latch only a SUCCESSFUL capture */
}

const struct boot_attest_handoff *tpm_attest_handoff_get(void)
{
    return &s_handoff;
}
