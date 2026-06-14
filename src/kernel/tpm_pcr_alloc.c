/* ============================================================================
 * tpm_pcr_alloc.c -- PCR allocation table + derived policy masks
 *
 * The single source of truth for which boot event extends which PCR. Pure data
 * + pure derivation (no TPM transaction, no global mutable state), so it is
 * safe to call from any context and trivially unit-testable. See
 * docs/boot/pcr-allocation.md for the human-facing table and the rationale for
 * each mask's membership.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/tpm_pcr_alloc.h"

/* Ordered event allocation table. Order within a PCR is by `ordering`. Policy
 * flags drive the derived masks below -- change membership HERE, never in a
 * separate mask constant.
 *
 * Mask design (see docs/boot/pcr-allocation.md):
 *   - SEAL default = PCR 7 only. BitLocker's PCR7+PCR11 default re-seals on any
 *     PCR-11 change, so a kernel rebuild would brick FDE unseal; Impossible OS
 *     keeps PCR 11 OUT of the seal default (opt-in) and seals to the Secure Boot
 *     policy (PCR 7), which is stable across kernel updates under the same keys.
 *   - QUOTE + BASELINE = PCR 0-7 (firmware + Secure Boot) plus PCR 11
 *     (kernel-ABI manifest + UKI kernel-boot), so a manifest-only change is
 *     attributed to the kernel-ABI layer rather than a blanket mismatch. */
#define POL_QB    (TPM_PCR_POL_QUOTE | TPM_PCR_POL_BASELINE)
#define POL_SQB   (TPM_PCR_POL_SEAL | TPM_PCR_POL_QUOTE | TPM_PCR_POL_BASELINE)
#define POL_QBV   (TPM_PCR_POL_QUOTE | TPM_PCR_POL_BASELINE | TPM_PCR_POL_VOLATILE)
#define FW        TPM_PCR_PRODUCER_FIRMWARE
#define BL        TPM_PCR_PRODUCER_BOOTLOADER

static const struct tpm_pcr_event_alloc s_alloc[] = {
    { "Firmware code",          0u, FW, TPM_PCR_LAYER_FIRMWARE,        0u, POL_QB },
    { "Firmware configuration", 1u, FW, TPM_PCR_LAYER_FIRMWARE_CONFIG, 0u, POL_QB },
    { "Option ROM code",        2u, FW, TPM_PCR_LAYER_OPTION_ROM,      0u, POL_QB },
    { "Option ROM config",      3u, FW, TPM_PCR_LAYER_OPTION_ROM,      0u, POL_QB },
    { "IPL / boot app",         4u, BL, TPM_PCR_LAYER_BOOTLOADER,      0u, POL_QB },
    { "IPL configuration",      5u, BL, TPM_PCR_LAYER_BOOTLOADER,      0u, POL_QB },
    { "State transition",       6u, FW, TPM_PCR_LAYER_FIRMWARE,        0u, POL_QB },
    { "Secure Boot policy",     7u, FW, TPM_PCR_LAYER_SECUREBOOT,      0u, POL_SQB },
    /* PCR 11: two distinct events share the bank. UKI kernel-boot is the
     * lowest-ordering (the convention Impossible OS aligns to); the kernel-ABI
     * manifest is a separate higher-ordering event. Both are VOLATILE (they move
     * on a legitimate kernel rebuild) -- which is exactly why PCR 11 is excluded
     * from the seal default. */
    { "UKI kernel-boot",        TPM_PCR_MANIFEST_INDEX, BL, TPM_PCR_LAYER_UKI_KERNEL, 0u, POL_QBV },
    { TPM_PCR_MANIFEST_EVENT,   TPM_PCR_MANIFEST_INDEX, BL, TPM_PCR_LAYER_KERNEL_ABI, 1u, POL_QBV },
};
#undef POL_QB
#undef POL_SQB
#undef POL_QBV
#undef FW
#undef BL

#define TPM_PCR_ALLOC_COUNT (sizeof(s_alloc) / sizeof(s_alloc[0]))

/* Every allocated PCR index must be in range so a (1u << pcr) mask shift is
 * always defined and the masks stay 24-bit. */
_Static_assert(sizeof(s_alloc) / sizeof(s_alloc[0]) >= 9u,
    "PCR allocation table lost its firmware/SB/manifest baseline entries");

uint32_t tpm_pcr_alloc_count(void)
{
    return (uint32_t)TPM_PCR_ALLOC_COUNT;
}

const struct tpm_pcr_event_alloc *tpm_pcr_alloc_get(uint32_t i)
{
    if (i >= (uint32_t)TPM_PCR_ALLOC_COUNT)
        return 0;
    return &s_alloc[i];
}

int tpm_pcr_owner(uint32_t pcr_index)
{
    int best_layer = -1;
    uint32_t best_ord = 0xFFFFFFFFu;
    if (pcr_index >= 24u)
        return -1;
    for (uint32_t i = 0; i < (uint32_t)TPM_PCR_ALLOC_COUNT; i++) {
        if (s_alloc[i].pcr != (uint8_t)pcr_index)
            continue;
        if (best_layer < 0 || s_alloc[i].ordering < best_ord) {
            best_ord = s_alloc[i].ordering;
            best_layer = (int)s_alloc[i].layer;
        }
    }
    return best_layer;
}

/* OR (1u << pcr) over every table event carrying `flag`. PCR indices are bounded
 * (< 24) by the entries, so the shift is always defined. */
static uint32_t tpm_pcr_mask_for(uint8_t flag)
{
    uint32_t mask = 0;
    for (uint32_t i = 0; i < (uint32_t)TPM_PCR_ALLOC_COUNT; i++) {
        if (s_alloc[i].pcr < 24u && (s_alloc[i].policy & flag))
            mask |= (1u << s_alloc[i].pcr);
    }
    return mask;
}

uint32_t tpm_pcr_seal_mask(void)     { return tpm_pcr_mask_for(TPM_PCR_POL_SEAL); }
uint32_t tpm_pcr_quote_mask(void)    { return tpm_pcr_mask_for(TPM_PCR_POL_QUOTE); }
uint32_t tpm_pcr_baseline_mask(void) { return tpm_pcr_mask_for(TPM_PCR_POL_BASELINE); }

/* Enumerate the BASELINE-policy PCRs (ascending) -- the canonical measured-boot set
 * derived from the allocation table (single source of truth). The baseline / replay /
 * PCR-cache consumers each used to hard-code this as {0-7,11}; this derives it so a
 * table change propagates everywhere. Writes up to `cap` indices into `out`, but
 * returns the TOTAL matching count (NOT the number written) so a caller can detect
 * overflow: a return > cap means the set did not fit and out holds only the prefix.
 * This lets the baseline drift guard (`count != TPM_BASELINE_MAX_PCRS`) fail closed
 * when the table grows past the v1 blob capacity instead of silently truncating. */
uint8_t tpm_pcr_baseline_pcrs(uint8_t *out, uint8_t cap)
{
    uint32_t mask = tpm_pcr_mask_for(TPM_PCR_POL_BASELINE);
    uint8_t total = 0;
    for (uint8_t pcr = 0; pcr < 24u; pcr++) {
        if (mask & (1u << pcr)) {
            if (out && total < cap)
                out[total] = pcr;
            total++;
        }
    }
    return total;
}
