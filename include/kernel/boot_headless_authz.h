/*
 * boot_headless_authz.h -- kernel side of the headless-enrollment
 * authorization transport.
 *
 * The bootloader publishes an offline-signed authorization blob found on the
 * ESP as a BOOT_PAYLOAD_HEADLESS_AUTHZ descriptor. This header exposes the one
 * operation the kernel performs on it: take it, once, and retire the
 * descriptor so it cannot be presented twice.
 *
 * The bytes are NOT trusted here. Authenticity is decided in
 * tpm_headless_authz.c by the Ed25519 signature, the EK device binding, the
 * transition binding and the monotonic counter; this transport only proves the
 * bytes arrived intact and in the right shape.
 */

#pragma once

#include "kernel/types.h"

struct boot_info;

/* Why a presented descriptor was or was not usable. Distinct values rather than
 * a boolean because the operator's remedy differs: a wrong length means the
 * file is not this format, a CRC mismatch means the transfer corrupted, and an
 * unreserved range means the loader published something the reservation pass
 * never pinned. */
typedef enum {
    BOOT_HL_AUTHZ_USABLE          = 0,
    BOOT_HL_AUTHZ_NOT_RESERVED    = 1,  /* no FLAG_RESERVED: frames may be allocator-owned */
    BOOT_HL_AUTHZ_OUT_OF_MAP      = 2,  /* outside the boot identity map, cannot be read */
    BOOT_HL_AUTHZ_BAD_LENGTH      = 3,  /* not exactly TPM_HEADLESS_BLOB_LEN */
    BOOT_HL_AUTHZ_NOT_CHECKSUMMED = 4,  /* no FLAG_CHECKSUMMED: nothing detects corruption */
    BOOT_HL_AUTHZ_CAP_ABSENT      = 5,  /* BOOT_CAP_PAYLOAD_DESCRIPTORS not negotiated */
} boot_hl_authz_class_t;

/* Classify a descriptor WITHOUT touching the memory it names. PURE and TOTAL:
 * no globals, no dereference, every input yields exactly one class.
 *
 * Split out of the take path so the whole refusal matrix is unit-testable: the
 * take itself dereferences a physical address through the boot identity map,
 * which a test cannot synthesize, so a classifier folded into it could only
 * ever be exercised on the one shape a live boot happens to present. Same
 * split, and the same reason, as boot_seed_desc_classify.
 *
 * ORDER IS PART OF THE CONTRACT. The capability gate comes first, then
 * RESERVED, then the map bound, and all three before the length -- a descriptor
 * that fails any earlier check must not have its memory read even to measure it.
 *
 * THE CAPABILITY GATE IS NOT REDUNDANT WITH FLAG_RESERVED. The PMM reservation
 * pass skips its whole reservation loop when BOOT_CAP_PAYLOAD_DESCRIPTORS is
 * absent (src/kernel/mm/boot_reserved.c), so on a degraded or version-skewed
 * handoff a descriptor can carry FLAG_RESERVED and still name frames the
 * allocator already owns. Reading them -- even to CRC or wipe them -- is
 * early-kernel memory corruption. The warm-update consumer carries the same
 * pair of gates for the same reason (src/kernel/main/boot_hw.c). */
boot_hl_authz_class_t boot_headless_authz_classify(uint32_t caps_present,
                                                   uint32_t flags,
                                                   uint64_t phys_start,
                                                   uint64_t length);

/* Stable label for a class, for logs. Never NULL. */
const char *boot_headless_authz_class_label(boot_hl_authz_class_t cls);

/* Take the presented authorization.
 *
 * On success returns the blob length and, when `out_blob` is non-NULL, writes
 * a pointer to kernel-owned bytes that stay valid for the rest of the boot.
 * Returns 0 when no usable authorization was presented -- absent, wrong
 * length, corrupt, unreserved, out of the boot identity map, or already taken.
 * Zero is a complete answer and never a partial one: the caller passes NULL/0
 * onward and the enrollment gate refuses exactly as it does on a machine that
 * carries no authorization at all.
 *
 * ONE-SHOT. Every descriptor of this type is retired by the first call,
 * accepted or not, so a second call returns 0.
 */
uint32_t boot_headless_authz_take(const uint8_t **out_blob);

/* The take, against a caller-supplied handoff. `boot_headless_authz_take` is
 * this over `&g_boot_info`.
 *
 * It exists so the DANGEROUS half is testable. The classifier is pure and can
 * be asserted directly, but the guard that matters is the one in this loop:
 * whether a refused class is genuinely left untouched, or merely classified and
 * then read anyway. A test builds a synthetic handoff whose descriptor names
 * memory the test owns, and asserts the bytes are still intact afterwards --
 * which no classifier-only assertion can do.
 *
 * `info` IS MUTABLE, deliberately rather than by oversight: retiring a
 * descriptor clears its FLAG_VALID in place, so a genuinely read-only handoff
 * cannot be passed here. Encoding that in the signature is better than casting
 * the constness away inside, which is what the first version did.
 *
 * The one-shot rule is shared with the wrapper: both are gated by the same
 * flag, so a test must call boot_headless_authz_reset_for_test() between
 * cases. */
uint32_t boot_headless_authz_take_from(struct boot_info *info,
                                       const uint8_t **out_blob);

#ifdef KERNEL_TESTS
/* Test seam: forget the taken authorization so a suite can drive the take path
 * more than once. COMPILED OUT of release builds -- the one-shot rule is the
 * production contract, and a globally linked reset would let any later caller
 * re-present a spent authorization. */
void boot_headless_authz_reset_for_test(void);
#endif
