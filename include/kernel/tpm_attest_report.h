/* ============================================================================
 * tpm_attest_report.h -- TPM-rooted boot attestation report export.
 *
 * The attestation report exports the TPM-signed boot evidence (PCRs + quote +
 * AK public + EK-cert chain) plus the bootloader-to-kernel handoff context. This
 * header currently defines the IMMUTABLE handoff snapshot the report builder must
 * consume instead of the live `g_boot_info`:
 *
 *   The capability words (`caps_present` / `caps_degraded`) are MUTATED after
 *   handoff -- the kernel refines them via `boot_caps_mark_present()` and
 *   runtime-services degradation. A report that read live `g_boot_info` would
 *   publish kernel-refined caps rather than the loader-to-kernel handoff, so a
 *   remote verifier could not distinguish loader evidence from later kernel
 *   policy. The snapshot is captured once, early (Phase 0, before any caps
 *   refinement), and the report consumes it.
 *
 * The report struct + builder + JSON export + native query API + the bootloader
 * PCR-11 manifest extend are the remaining section-9 work; see the section Design
 * note in todo/01-boot-platform/TODO-13-tpm-measured-boot-attestation.md.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

struct boot_info;   /* forward decl: the capture reads the handoff fields */

/* Immutable snapshot of the bootloader-to-kernel handoff triple, taken before any
 * kernel-side capability refinement. Plain value type (copied, never aliased to the
 * live struct), so a later kernel `boot_caps_mark_present()` cannot change what the
 * attestation report attributes to the loader. */
struct boot_attest_handoff {
    uint64_t caps_required;        /* loader-asserted required capability bits */
    uint64_t caps_present;         /* capabilities the loader actually populated */
    uint64_t caps_degraded;        /* known-but-not-provided bits (adapter degradation) */
    uint32_t boot_path;            /* enum boot_path_type: which flow ran */
    uint32_t boot_reason;          /* enum boot_reason_code: the policy reason */
    uint32_t boot_source_flags;    /* BOOT_SOURCE_FLAG_* bitmask: inputs consulted */
    uint32_t boot_fallback_depth;  /* 0 = primary, N = Nth fallback */
    uint8_t  valid;                /* 1 once captured from a non-NULL boot_info */
    uint8_t  _pad[7];
};

/* Copy the handoff triple out of `bi` into `out` (pure, MMIO-free). On a NULL `bi`
 * or `out`, leaves/marks `out` invalid (valid = 0) so a consumer fails closed. Call
 * this ONCE at Phase 0 from the kernel snapshot point, before caps refinement; the
 * attestation report builder then reads the snapshot, never live `g_boot_info`. */
void tpm_attest_handoff_capture(const struct boot_info *bi,
                                struct boot_attest_handoff *out);

/* Capture the handoff snapshot into the module's one canonical store. Call ONCE at
 * Phase 0 on the BSP, after boot_info validation and BEFORE any capability
 * refinement (boot_caps_mark_present / runtime-services degradation). */
void tpm_attest_handoff_snapshot_init(const struct boot_info *bi);

/* The stored Phase-0 handoff snapshot, for the attestation report builder. Never
 * NULL; `valid` is 0 until snapshot_init has run. Consumers MUST read this, never
 * the live g_boot_info, for handoff-attributed fields. snapshot_init is write-once:
 * the first successful capture latches and later calls no-op. */
const struct boot_attest_handoff *tpm_attest_handoff_get(void);
