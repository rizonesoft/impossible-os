/* ============================================================================
 * tpm_pcr_alloc.h -- PCR allocation + policy-mask single source of truth
 *
 * One ordered, EVENT-centric table of which boot event extends which PCR, with
 * per-event policy attributes. PCR digests are aggregates, so a per-PCR "owner"
 * cannot classify the multiple events on PCR 11 (kernel-ABI manifest vs UKI
 * kernel-boot); the table allows multiple entries per PCR and the sealed-secret
 * / quote / baseline masks are DERIVED from per-event policy flags (never
 * duplicated per consumer). A manifest-only change is therefore attributable to
 * the kernel-ABI layer instead of reading as a blanket mismatch.
 *
 * Consumers: baseline enrollment, sealed-secret policy, and attestation quote
 * each call the derived-mask helpers below rather than hard-coding a bitmap.
 *
 * Canonical doc: docs/boot/pcr-allocation.md.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
/* Manifest PCR index + event name come from the dependency-free bootloader-safe
 * header so the bootloader producer and these kernel consumers share ONE
 * definition (no drift at the trust boundary). Do not redefine here. */
#include "boot/pcr_manifest.h"

/* The producer that extends an event (provenance, for diagnostics/replay). */
typedef enum {
    TPM_PCR_PRODUCER_FIRMWARE  = 0,  /* platform firmware / UEFI */
    TPM_PCR_PRODUCER_BOOTLOADER = 1, /* BOOTX64.EFI */
    TPM_PCR_PRODUCER_KERNEL    = 2,  /* kernel.exe */
} tpm_pcr_producer_t;

/* The boot-integrity LAYER an event belongs to -- what a mismatch points at. */
typedef enum {
    TPM_PCR_LAYER_FIRMWARE        = 0,  /* firmware code */
    TPM_PCR_LAYER_FIRMWARE_CONFIG = 1,  /* firmware settings */
    TPM_PCR_LAYER_OPTION_ROM      = 2,
    TPM_PCR_LAYER_BOOTLOADER      = 3,  /* IPL / boot app */
    TPM_PCR_LAYER_SECUREBOOT      = 4,  /* db/dbx/KEK/PK policy */
    TPM_PCR_LAYER_KERNEL_ABI      = 5,  /* .bootproto manifest sha256 */
    TPM_PCR_LAYER_UKI_KERNEL      = 6,  /* UKI kernel-boot measurement */
    TPM_PCR_LAYER_OS_DATA         = 7,  /* OS-owned runtime data */
} tpm_pcr_layer_t;

/* Per-event policy flags: which default masks an event participates in. */
#define TPM_PCR_POL_NONE      0x00u
#define TPM_PCR_POL_SEAL      0x01u  /* in the sealed-secret default mask */
#define TPM_PCR_POL_QUOTE     0x02u  /* in the attestation quote default mask */
#define TPM_PCR_POL_BASELINE  0x04u  /* in the baseline-enrollment mask */
#define TPM_PCR_POL_VOLATILE  0x08u  /* changes across legitimate rebuilds/updates */

/* One ordered event allocation. `ordering` disambiguates multiple events on the
 * same PCR (deterministic replay); `policy` is the OR of TPM_PCR_POL_* flags. */
struct tpm_pcr_event_alloc {
    const char *event_name;
    uint8_t pcr;        /* PCR index 0..23 */
    uint8_t producer;   /* tpm_pcr_producer_t */
    uint8_t layer;      /* tpm_pcr_layer_t */
    uint8_t ordering;   /* extend order within the PCR (0 = first) */
    uint8_t policy;     /* TPM_PCR_POL_* bitmask */
};

/* Number of entries in the allocation table, and accessor for entry i. */
uint32_t tpm_pcr_alloc_count(void);
const struct tpm_pcr_event_alloc *tpm_pcr_alloc_get(uint32_t i);

/* Layer view for a PCR index: the layer of the LOWEST-ordering event on that
 * PCR, or -1 if no event is allocated to it. (For PCR 11 the lowest-ordering
 * event is the UKI kernel-boot measurement; the kernel-ABI manifest is a
 * distinct higher-ordering event -- both are in the table.) */
int tpm_pcr_owner(uint32_t pcr_index);

/* Derived 24-bit PCR masks (bit i = PCR i). Each is the OR of (1<<pcr) over
 * every table event carrying the matching policy flag -- the masks are NEVER a
 * separate hard-coded constant, so they cannot drift from the table. The seal
 * default deliberately EXCLUDES PCR 11 so a kernel-ABI-manifest change (a kernel
 * rebuild) does not break FDE unseal; PCR 11 is opt-in for sealing. */
uint32_t tpm_pcr_seal_mask(void);
uint32_t tpm_pcr_quote_mask(void);
uint32_t tpm_pcr_baseline_mask(void);

/* Canonical measured-boot PCR list (BASELINE policy), ascending order. Fills `out`
 * (caller sizes it to the known max) and returns the count. Single source of truth
 * for the baseline / replay / PCR-cache consumers; replaces their hard-coded {0-7,11}. */
uint8_t tpm_pcr_baseline_pcrs(uint8_t *out, uint8_t cap);
