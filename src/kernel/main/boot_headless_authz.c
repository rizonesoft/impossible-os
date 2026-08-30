/*
 * boot_headless_authz.c -- take the signed headless-enrollment authorization
 * off the boot payload table.
 *
 * The loader publishes an offline-signed authorization blob it found on the
 * ESP as a BOOT_PAYLOAD_HEADLESS_AUTHZ descriptor (src/boot/uefi/bootx64.c,
 * publish_headless_authz_payload). This file is the whole kernel side of that
 * transport: find it, prove it is intact and the right shape, copy it out, and
 * retire the descriptor so nothing can present it twice.
 *
 * THE TRANSPORT CARRIES BYTES, NEVER TRUST. Anyone who can write the ESP can
 * write this file, so nothing here treats its presence as permission -- the
 * authorization's authenticity is decided entirely by the Ed25519 signature,
 * the device binding, the transition binding and the monotonic counter in
 * tpm_headless_authz.c. The CRC on the descriptor detects a corrupt transfer
 * and nothing more: an attacker rewriting the payload rewrites the CRC with it.
 *
 * EVERY failure degrades to NO authorization, never a partial one. A missing
 * payload, a wrong length, a bad CRC, an unreserved or out-of-map range and a
 * duplicate descriptor all leave the caller holding nothing, which is exactly
 * the state a machine without an authorization file is already in.
 */

#include "kernel/types.h"
#include "kernel/boot_info.h"
#include "kernel/mm/boot_reserved.h" /* boot_reserved_payload_is_pinned */
#include "kernel/kchecksum.h"
#include "kernel/klog.h"
#include "kernel/boot_init.h"   /* POST16_BOOT_HL_AUTHZ -- Phase-1 boot path */
#include "kernel/tpm_headless_authz.h"
#include "kernel/boot_headless_authz.h"
#include "libc/string.h"

/* The blob is 152 bytes and is taken exactly once per boot, so it is copied
 * out of the payload page into a file-static buffer rather than dereferenced
 * in place: the page is wiped and retired immediately after the copy, and a
 * caller holding a pointer into a retired page would be reading recycled
 * memory by the time it evaluated the signature.
 *
 * SMP: written once during Phase 1 before the scheduler starts, from the BSP,
 * and never touched from an interrupt. `s_taken` makes the take one-shot, so a
 * second caller receives nothing rather than the same bytes twice.
 */
static uint8_t s_blob[TPM_HEADLESS_BLOB_LEN];
static uint32_t s_blob_len;
static int s_taken;


boot_hl_authz_class_t boot_headless_authz_classify(uint32_t caps_present,
                                                   uint32_t flags,
                                                   uint64_t phys_start,
                                                   uint64_t length)
{
    /* CAPABILITY FIRST, because FLAG_RESERVED is only meaningful once the
     * producer negotiated the descriptor array. boot_reserved.c gates its
     * entire reservation loop on this bit, so without it a RESERVED-flagged
     * descriptor names frames PMM never pinned and may already have handed
     * out. This is the gate the warm-update consumer already carries. */
    if ((caps_present & BOOT_CAP_PAYLOAD_DESCRIPTORS) == 0u)
        return BOOT_HL_AUTHZ_CAP_ABSENT;
    /* RESERVED next: a descriptor without it was never pinned by the PMM
     * reservation pass, so by Phase 1 its frames may already belong to the
     * allocator. Reading them would be reading someone else's memory, so this
     * check has to come before anything that dereferences. */
    if ((flags & BOOT_PAYLOAD_FLAG_RESERVED) == 0u)
        return BOOT_HL_AUTHZ_NOT_RESERVED;
    /* phys_start is dereferenced through the boot identity map, which covers
     * the low 4 GiB only. Written as a subtraction against the map end rather
     * than phys_start + length so the sum cannot wrap past it. */
    if (phys_start >= BOOT_INFO_EARLY_MAP_END ||
        length > BOOT_INFO_EARLY_MAP_END - phys_start)
        return BOOT_HL_AUTHZ_OUT_OF_MAP;
    /* EXACT length. The blob is a fixed-layout record whose signature covers a
     * fixed byte range, so a short file is not a weaker authorization and a
     * long one is not this format.
     *
     * Asked through the SHARED contract rather than compared here, even
     * though this type's minimum and maximum are both TPM_HEADLESS_BLOB_LEN
     * and the two are numerically identical today. The reservation pass
     * decides what it PINS from that table; a second copy of the bound
     * living in the consumer is precisely the drift that let
     * canary_seed_desc_ok() keep its own length rule after the pass stopped
     * honouring it. One source, both sides. (The per-type payload length
     * contract.) */
    if (!boot_payload_length_reservable((uint32_t)BOOT_PAYLOAD_HEADLESS_AUTHZ,
                                        length))
        return BOOT_HL_AUTHZ_BAD_LENGTH;
    /* The loader always checksums this type. An unchecksummed descriptor did
     * not come from a producer that understands the contract, so there is
     * nothing to detect a corrupt transfer with. */
    if ((flags & BOOT_PAYLOAD_FLAG_CHECKSUMMED) == 0u)
        return BOOT_HL_AUTHZ_NOT_CHECKSUMMED;
    return BOOT_HL_AUTHZ_USABLE;
}

const char *boot_headless_authz_class_label(boot_hl_authz_class_t cls)
{
    switch (cls) {
    case BOOT_HL_AUTHZ_USABLE:          return "usable";
    case BOOT_HL_AUTHZ_NOT_RESERVED:    return "not-pmm-reserved";
    case BOOT_HL_AUTHZ_OUT_OF_MAP:      return "outside-boot-identity-map";
    case BOOT_HL_AUTHZ_BAD_LENGTH:      return "wrong-length";
    case BOOT_HL_AUTHZ_NOT_CHECKSUMMED: return "not-checksummed";
    case BOOT_HL_AUTHZ_CAP_ABSENT:      return "payload-capability-absent";
    }
    /* Totality: an out-of-range value cast in from outside the enum names
     * itself rather than falling through to a class it is not. */
    return "unknown";
}

uint32_t boot_headless_authz_take(const uint8_t **out_blob)
{
    return boot_headless_authz_take_from(&g_boot_info, out_blob);
}

uint32_t boot_headless_authz_take_from(struct boot_info *info,
                                       const uint8_t **out_blob)
{
    uint32_t idx;
    uint32_t accepted = 0;

    POST16(POST16_BOOT_HL_AUTHZ);

    if (out_blob)
        *out_blob = (const uint8_t *)0;
    if (!info || s_taken) {
        POST16(POST16_BOOT_HL_AUTHZ_OK);
        return 0;
    }
    s_taken = 1;

    /* Walk EVERY descriptor of this type, not just the first. Retiring one
     * clears its FLAG_VALID and boot_payload_find() skips retired entries, so
     * occurrence 0 advances each pass. Draining the whole set matters: a
     * second descriptor of the same type would otherwise sit in the table
     * shadowing nothing and holding a RESERVED page for the rest of the boot,
     * and only the first would ever be inspected. The FIRST intact payload
     * wins and every later one is retired unused -- an authorization is a
     * single token, so a boot presenting two of them is presenting neither. */
    for (idx = 0; idx < (uint32_t)BOOT_PAYLOAD_MAX; idx++) {
        struct boot_payload_desc *d = (struct boot_payload_desc *)
            boot_payload_find(info,
                              (uint32_t)BOOT_PAYLOAD_HEADLESS_AUTHZ, 0);
        const uint8_t *payload = (const uint8_t *)0;
        boot_hl_authz_class_t cls;
        int usable = 0;

        if (!d)
            break;

        cls = boot_headless_authz_classify(info->caps_present, d->flags,
                                           d->phys_start, d->length);
        /* Same correction as the seed consumer: the classifier reads the
         * handoff, this reads what the pass pinned. A budget- or
         * duplicate-skipped authorization is in contract and RESERVED-flagged
         * and was never pinned, so it is refused rather than dereferenced. */
        if (cls == BOOT_HL_AUTHZ_USABLE
            && !boot_reserved_payload_is_pinned(
                   (uint32_t)(d - &info->payload_descriptors[0]),
                   d->phys_start, d->length))
            cls = BOOT_HL_AUTHZ_NOT_RESERVED;

        /* A REJECTED DESCRIPTOR'S MEMORY IS NEVER TOUCHED, whatever the
         * reason. `payload` is assigned for the USABLE class alone, so every
         * refusal returns without a read and the retire below has nothing to
         * wipe.
         *
         * That is not tidiness. The earlier shape assigned `payload` for the
         * length and checksum refusals too, and the retire then wiped
         * `d->length` bytes -- a field the classifier had just declared WRONG,
         * bounded only by the 4 GiB identity map. A descriptor claiming three
         * gigabytes would have been rejected and then zeroed three gigabytes of
         * whatever sat at its address.
         *
         * Not wiping a rejected payload is also correct on its own terms: this
         * blob is signed, not secret, so there is nothing here to scrub. That
         * is the opposite of the seed transport, whose payload IS one-time key
         * material and must be wiped even when refused. */
        if (cls != BOOT_HL_AUTHZ_USABLE) {
            klog(LOG_WARN, "TPM",
                 "headless authz payload rejected: %s (0x%lx for %lu bytes)",
                 boot_headless_authz_class_label(cls),
                 (uint64_t)d->phys_start, (uint64_t)d->length);
        } else {
            payload = (const uint8_t *)(uintptr_t)d->phys_start;
            if (kcrc32c(payload, (size_t)TPM_HEADLESS_BLOB_LEN) !=
                (uint32_t)(d->checksum & 0xFFFFFFFFull)) {
                klog(LOG_WARN, "TPM",
                     "headless authz payload rejected: CRC mismatch");
            } else if (accepted) {
                klog(LOG_WARN, "TPM",
                     "headless authz payload ignored: a second authorization "
                     "was presented this boot");
            } else {
                usable = 1;
            }
        }

        if (usable) {
            memcpy(s_blob, payload, TPM_HEADLESS_BLOB_LEN);
            s_blob_len = (uint32_t)TPM_HEADLESS_BLOB_LEN;
            accepted = 1;
        }

        /* One retire path for EVERY discovered descriptor, accepted or not.
         *
         * The wipe length is the BLOB LENGTH, never `d->length`. `payload` is
         * non-NULL only for a USABLE descriptor, whose length the classifier
         * has already proven equal to TPM_HEADLESS_BLOB_LEN, so the two agree
         * -- but writing the constant is what makes that agreement structural
         * rather than a property of a field an attacker supplies.
         *
         * The frames are NOT freed: the loader allocated them as EfiLoaderData
         * and the reservation pass owns that decision. */
        if (payload)
            memset((void *)(uintptr_t)d->phys_start, 0,
                   (size_t)TPM_HEADLESS_BLOB_LEN);
        d->flags &= ~(uint32_t)BOOT_PAYLOAD_FLAG_VALID;
    }

    POST16(POST16_BOOT_HL_AUTHZ_OK);

    if (!accepted)
        return 0;
    if (out_blob)
        *out_blob = s_blob;
    klog(LOG_INFO, "TPM", "headless authz payload taken (%lu bytes)",
         (uint64_t)s_blob_len);
    return s_blob_len;
}

#ifdef KERNEL_TESTS
void boot_headless_authz_reset_for_test(void)
{
    memset(s_blob, 0, sizeof s_blob);
    s_blob_len = 0u;
    s_taken = 0;
}
#endif
