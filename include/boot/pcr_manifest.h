/* ============================================================================
 * pcr_manifest.h -- shared manifest PCR-extend constants (BOOTLOADER-SAFE)
 *
 * The SINGLE source of truth for the PCR index + TCG event name that the
 * bootloader pre-jump path extends with the kernel `.bootproto` manifest
 * sha256, and that the kernel PCR allocation table / attestation consumers
 * classify against. Deliberately dependency-free (NO kernel/types.h, no other
 * include) so the freestanding UEFI bootloader and the kernel both consume the
 * exact same values without an include-path or type dependency -- a producer
 * (BOOTX64.EFI) that hard-codes a different PCR/name would be a drift bug, not a
 * second source of truth.
 *
 * Consumed by: include/kernel/tpm_pcr_alloc.h (kernel allocation table) and the
 * bootloader manifest-extend path (TODO-13 attestation export section).
 * Canonical doc: docs/boot/pcr-allocation.md.
 * ============================================================================ */

#pragma once

/* PCR 11 -- shared with the UKI kernel-boot convention (systemd-stub / pcrlock),
 * NOT a dedicated PCR. */
#define TPM_PCR_MANIFEST_INDEX  11u

/* Stable TCG event name for the kernel-ABI manifest measurement. */
#define TPM_PCR_MANIFEST_EVENT  "IMPOSSIBLE_OS_KERNEL_ABI_MANIFEST"
