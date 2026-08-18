/* ============================================================================
 * tpm_record.c -- authenticated NV records and their authorized transitions
 *
 * Pure format + validator half of the record layer (see tpm_record.h for the
 * threat model). Nothing here touches the transport: every function is a total
 * function of its arguments, so the fixtures can drive every refusal shape
 * without a TPM.
 * ============================================================================ */

#include "kernel/tpm_record.h"

/* Digest coverage is defined by ZEROING the digest field, not by skipping it.
 * Skipping would leave the 32 bytes outside the hash entirely, so an attacker
 * could park content there; zeroing keeps the region inside the covered span
 * with a value the producer cannot choose. */
tpm_record_status_t tpm_record_digest_compute(const uint8_t *buf, uint32_t len,
                                              uint8_t out[TPM_RECORD_DIGEST])
{
    static const uint8_t zeros[TPM_RECORD_DIGEST] = { 0 };
    const uint32_t doff = (uint32_t)__builtin_offsetof(struct tpm_record_hdr, digest);
    struct sha256_ctx ctx;

    if (!buf || !out || len < TPM_RECORD_HDR_LEN)
        return TPM_RECORD_BADARG;

    sha256_init(&ctx);
    sha256_update(&ctx, buf, doff);
    sha256_update(&ctx, zeros, TPM_RECORD_DIGEST);
    sha256_update(&ctx, buf + doff + TPM_RECORD_DIGEST,
                  len - doff - TPM_RECORD_DIGEST);
    sha256_final(&ctx, out);
    return TPM_RECORD_OK;
}

/* Constant-time-ish compare. The digest is not a secret, so this is about
 * refusing to leak WHICH byte differed through timing to a co-resident
 * attacker, not about protecting the value itself. */
static int digest_equal(const uint8_t *a, const uint8_t *b)
{
    uint8_t diff = 0u;
    uint32_t i;
    for (i = 0u; i < TPM_RECORD_DIGEST; i++)
        diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0u;
}

/* Every reserved byte in a record is required to be zero AND is inside the
 * digest. Either check alone is weak: the digest alone would happily
 * authenticate attacker-chosen padding once the authority signed it, and the
 * zero check alone would let padding be rewritten after authorization. */
static int payload_reserved_zero(tpm_record_kind_t kind, const uint8_t *payload,
                                 uint32_t payload_len)
{
    uint32_t i;

    if (kind == TPM_RECORD_KIND_AB_FLOOR) {
        const struct tpm_ab_floor_payload *p =
            (const struct tpm_ab_floor_payload *)payload;
        if (payload_len != (uint32_t)sizeof(*p))
            return 0;
        for (i = 0u; i < 3u; i++)
            if (p->reserved[i] != 0u)
                return 0;
        return 1;
    }
    if (kind == TPM_RECORD_KIND_BASELINE) {
        const struct tpm_baseline_bind_payload *p =
            (const struct tpm_baseline_bind_payload *)payload;
        if (payload_len != (uint32_t)sizeof(*p))
            return 0;
        for (i = 0u; i < 3u; i++)
            if (p->reserved[i] != 0u)
                return 0;
        return 1;
    }
    return 0;
}

tpm_record_status_t tpm_record_parse(const uint8_t *buf, uint32_t len,
                                     tpm_record_kind_t want_kind,
                                     uint32_t want_payload_len,
                                     struct tpm_record_view *out)
{
    const struct tpm_record_hdr *h;
    uint8_t want[TPM_RECORD_DIGEST];
    tpm_record_status_t st;

    if (!buf || !out || want_kind == TPM_RECORD_KIND_NONE)
        return TPM_RECORD_BADARG;
    /* An exact total length, not a minimum: trailing bytes would sit inside the
     * digest with no field describing them, which is the same covert-content
     * hole the reserved-byte rule closes. */
    if (want_payload_len > (0xFFFFFFFFu - TPM_RECORD_HDR_LEN))
        return TPM_RECORD_BADARG;
    if (len != TPM_RECORD_HDR_LEN + want_payload_len)
        return TPM_RECORD_MALFORMED;

    h = (const struct tpm_record_hdr *)buf;
    if (h->magic != TPM_RECORD_MAGIC || h->layout != (uint16_t)TPM_RECORD_LAYOUT)
        return TPM_RECORD_MALFORMED;
    if (h->reserved != 0u)
        return TPM_RECORD_MALFORMED;
    if (h->payload_len != want_payload_len)
        return TPM_RECORD_MALFORMED;

    /* KIND is checked before the digest so a substituted-but-authentic record
     * reports what it actually is. Both records are authorized under the same
     * authority key, so their digests are equally valid and only this check
     * separates a floor record served from the baseline index from the real
     * thing. */
    if (h->kind != (uint16_t)want_kind)
        return TPM_RECORD_KIND;

    /* Generation 0 is the never-written state and can never appear in a stored
     * record: the first authorized write is generation 1. */
    if (h->generation == 0u)
        return TPM_RECORD_MALFORMED;

    if (!payload_reserved_zero(want_kind, buf + TPM_RECORD_HDR_LEN, want_payload_len))
        return TPM_RECORD_MALFORMED;

    st = tpm_record_digest_compute(buf, len, want);
    if (st != TPM_RECORD_OK)
        return st;
    if (!digest_equal(want, h->digest))
        return TPM_RECORD_DIGEST_BAD;

    out->kind        = want_kind;
    out->generation  = h->generation;
    out->payload     = buf + TPM_RECORD_HDR_LEN;
    out->payload_len = want_payload_len;
    return TPM_RECORD_OK;
}

tpm_record_status_t tpm_record_build(uint8_t *buf, uint32_t cap,
                                     tpm_record_kind_t kind, uint64_t generation,
                                     const uint8_t *payload, uint32_t payload_len)
{
    struct tpm_record_hdr *h;
    uint32_t i;

    if (!buf || !payload || kind == TPM_RECORD_KIND_NONE)
        return TPM_RECORD_BADARG;
    if (payload_len > (0xFFFFFFFFu - TPM_RECORD_HDR_LEN))
        return TPM_RECORD_BADARG;
    if (cap != TPM_RECORD_HDR_LEN + payload_len)
        return TPM_RECORD_BADARG;
    /* Refuse to MINT the never-written generation. A builder that produced one
     * would create a record the parser must reject, so the mistake would only
     * surface on the next boot rather than at the call that made it. */
    if (generation == 0u)
        return TPM_RECORD_BADARG;
    if (!payload_reserved_zero(kind, payload, payload_len))
        return TPM_RECORD_BADARG;

    h = (struct tpm_record_hdr *)buf;
    h->magic       = TPM_RECORD_MAGIC;
    h->layout      = (uint16_t)TPM_RECORD_LAYOUT;
    h->kind        = (uint16_t)kind;
    h->generation  = generation;
    h->payload_len = payload_len;
    h->reserved    = 0u;
    for (i = 0u; i < TPM_RECORD_DIGEST; i++)
        h->digest[i] = 0u;
    for (i = 0u; i < payload_len; i++)
        buf[TPM_RECORD_HDR_LEN + i] = payload[i];

    return tpm_record_digest_compute(buf, cap, h->digest);
}

/* The kind-specific "did the value move backwards" test. The floor's ordering
 * is its security version; the baseline's content digest has no ordering at all
 * (a rotation legitimately measures a different machine state), so its
 * anti-rollback comes wholly from the generation step and the counter. */
static int value_regressed(const struct tpm_record_view *cur,
                           const struct tpm_record_view *next)
{
    if (cur->kind == TPM_RECORD_KIND_AB_FLOOR) {
        const struct tpm_ab_floor_payload *a;
        const struct tpm_ab_floor_payload *b;
        /* A view is normally produced by tpm_record_parse, which pins the
         * payload length for the kind -- but this is a public entry point and a
         * hand-built view must not be able to steer a read past its buffer. A
         * view too short to hold the ordering field cannot be judged, so it is
         * treated as a regression rather than waved through. */
        if (!cur->payload || !next->payload ||
            cur->payload_len != (uint32_t)sizeof(*a) ||
            next->payload_len != (uint32_t)sizeof(*b))
            return 1;
        a = (const struct tpm_ab_floor_payload *)cur->payload;
        b = (const struct tpm_ab_floor_payload *)next->payload;
        return b->security_version < a->security_version;
    }
    return 0;
}

tpm_record_status_t tpm_record_transition_ok(const struct tpm_record_view *cur,
                                             const struct tpm_record_view *next)
{
    if (!next || next->kind == TPM_RECORD_KIND_NONE)
        return TPM_RECORD_BADARG;

    if (!cur) {
        /* No current record: the first authorized write. Its generation is NOT
         * pinned to one, and an earlier draft of this function got that wrong.
         * TPM 2.0 Part 1 section 37.2.6.3 requires a freshly defined
         * TPM_NT_COUNTER to initialize on its first increment to the LARGEST
         * value any NV counter has held over the TPM's lifetime, not to zero,
         * so on a TPM that has ever run a counter to 500 the first authorized
         * generation is at least 501. Demanding 1 would have refused every
         * first enrollment on a previously-used TPM.
         *
         * Nothing is lost by accepting it: the starting point is the TPM's to
         * choose, not the caller's, and that same rule is what makes a deleted
         * and redefined counter unable to restart lower. The binding to the
         * real counter is tpm_record_counter_ok's job; here we only refuse the
         * never-written sentinel, which a counter can never produce because its
         * value after the first increment is strictly above the watermark. */
        return (next->generation != 0u) ? TPM_RECORD_OK : TPM_RECORD_STEP;
    }

    if (cur->kind != next->kind)
        return TPM_RECORD_KIND;
    /* Saturated counter first, so the +1 below cannot wrap to zero. Ordering
     * matters: with the wrap check second it is dead code, because a wrapped
     * comparison only ever produces the STEP the first check already returned. */
    if (cur->generation == 0xFFFFFFFFFFFFFFFFull)
        return TPM_RECORD_STEP;
    /* Exactly one. A jump is not an authorized step even in the "safe"
     * direction: the authorization binds a single counter transition, so a
     * record claiming two of them was authorized for neither. */
    if (next->generation != cur->generation + 1u)
        return TPM_RECORD_STEP;
    if (value_regressed(cur, next))
        return TPM_RECORD_ROLLBACK;
    return TPM_RECORD_OK;
}

tpm_record_status_t tpm_record_counter_ok(const struct tpm_record_view *rec,
                                          uint64_t counter)
{
    if (!rec || rec->kind == TPM_RECORD_KIND_NONE)
        return TPM_RECORD_BADARG;
    return (rec->generation == counter) ? TPM_RECORD_OK : TPM_RECORD_SKEW;
}

tpm_record_status_t tpm_record_ab_floor_version(const struct tpm_record_view *rec,
                                                uint32_t *out_version)
{
    const struct tpm_ab_floor_payload *p;

    if (!rec || !out_version || rec->kind != TPM_RECORD_KIND_AB_FLOOR)
        return TPM_RECORD_BADARG;
    if (!rec->payload || rec->payload_len != (uint32_t)sizeof(*p))
        return TPM_RECORD_MALFORMED;
    p = (const struct tpm_ab_floor_payload *)rec->payload;
    *out_version = p->security_version;
    return TPM_RECORD_OK;
}
