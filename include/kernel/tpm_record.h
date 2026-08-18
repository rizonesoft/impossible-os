/* ============================================================================
 * tpm_record.h -- authenticated NV records and their authorized transitions
 *
 * The RECORD layer. tpm_nv.h owns the index a record lives in (who may create
 * it, who may destroy it, and proving the handle answers with the index that
 * was enrolled). This file owns what is INSIDE that index, and it fails the
 * other way round: the index is perfectly intact, its Name verifies, the
 * counter is monotonic, and the value stored in it is still a lie an attacker
 * chose.
 *
 * Two records share one construction. The A/B anti-rollback floor holds a
 * security version that may JUMP (a fresh install at ordinal 500, an upgrade
 * skipping releases), so the VALUE lives in an ordinary data index and the
 * UPDATE SEQUENCE lives in a separate TPM_NT_COUNTER. The measured-boot
 * baseline has the identical hole in the other direction: its index is
 * owner-writable, so an old vulnerable blob can be relabelled with the current
 * generation and a recomputed CRC without touching any counter at all.
 *
 * THE PAIRING ALONE IS NOT ANTI-ROLLBACK. A holder of record-write authority
 * can increment the counter once and store a LOWER version: the counter
 * advanced, the record matches it, nothing is skewed, and every cooperative
 * check passes. The AUTHORIZATION carries the monotonicity; the arithmetic
 * never does. So a record write is authorized against the exact (counter
 * transition, whole-record digest) pair and nothing weaker.
 *
 * A CRC is an integrity check against corruption and was never an authenticity
 * check. tpm_baseline.c's gpt_crc32 keeps doing exactly the job it is fit for;
 * the digest here is a different job.
 *
 * The format, the digest and the transition validator are PURE (no MMIO, no
 * transport, fixture-testable). The read/advance wrappers drive the NV layer
 * and are not ISR-safe.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/crypto/sha256.h"

#define TPM_RECORD_MAGIC   0x43525049u /* "IPRC" little-endian */
#define TPM_RECORD_LAYOUT  1u          /* on-NV layout revision of this header */
#define TPM_RECORD_DIGEST  SHA256_DIGEST_LEN

/* Which record a blob claims to be. Stored in the header and CHECKED against
 * what the reader asked for: a floor record served from the baseline index (or
 * the reverse) is a substitution the digest alone cannot catch, because both
 * records are authorized under the same authority key. */
typedef enum {
    TPM_RECORD_KIND_NONE     = 0u,
    TPM_RECORD_KIND_AB_FLOOR = 1u, /* A/B anti-rollback security version */
    TPM_RECORD_KIND_BASELINE = 2u, /* measured-boot baseline generation */
} tpm_record_kind_t;

/* Verdicts. Deliberately distinct from tpm_nv_status_t: an NV status describes
 * what the TRANSPORT or the INDEX did, and these describe what the CONTENT
 * says. Collapsing them would make "the record is a forgery" indistinguishable
 * from "the TPM was busy", which is precisely the distinction a caller must act
 * on differently. */
typedef enum {
    TPM_RECORD_OK        = 0u,
    TPM_RECORD_BADARG    = 1u,  /* caller misuse: NULL, short buffer, bad kind */
    TPM_RECORD_MALFORMED = 2u,  /* magic / layout / length / reserved-byte fault */
    TPM_RECORD_KIND      = 3u,  /* a valid record, but not the kind asked for */
    TPM_RECORD_DIGEST_BAD= 4u,  /* whole-record digest does not match content */
    TPM_RECORD_STEP      = 5u,  /* generation did not advance by exactly one */
    TPM_RECORD_ROLLBACK  = 6u,  /* value moved BACKWARDS across an authorized step */
    TPM_RECORD_SKEW      = 7u,  /* record generation disagrees with the NV counter */
} tpm_record_status_t;

/* The on-NV record header. The struct IS the wire format.
 *
 * `digest` covers the WHOLE record with its own 32 bytes taken as zero, which
 * is the only self-consistent definition: hashing a record that already
 * contains its digest is circular. Every reserved byte is inside the hash AND
 * required to be zero on parse, so padding can never become a covert channel
 * for content the authorization did not approve.
 *
 * `generation` is the value the TPM_NT_COUNTER holds AFTER the transition this
 * record was authorized for. A record is therefore readable-but-not-yet-current
 * while the counter still reads generation-1, which is what makes
 * write-then-increment a commit point rather than a race. */
struct tpm_record_hdr {
    uint32_t magic;      /* TPM_RECORD_MAGIC */
    uint16_t layout;     /* TPM_RECORD_LAYOUT */
    uint16_t kind;       /* tpm_record_kind_t */
    uint64_t generation; /* NV counter value this record is bound to */
    uint32_t payload_len;/* bytes of payload following this header */
    uint32_t reserved;   /* MUST be zero */
    uint8_t  digest[TPM_RECORD_DIGEST]; /* SHA-256, this field taken as zero */
};

/* The A/B floor payload: the security version the loader compares a slot's
 * rollback_index against. */
struct tpm_ab_floor_payload {
    uint32_t security_version;
    uint32_t reserved[3];  /* MUST be zero */
};

/* The baseline payload: binds the authorized generation to the exact baseline
 * blob it describes, so relabelling an old blob fails on the content digest
 * rather than passing a CRC the attacker recomputed. */
struct tpm_baseline_bind_payload {
    uint8_t  blob_digest[TPM_RECORD_DIGEST]; /* SHA-256 of the baseline blob */
    uint32_t blob_len;
    uint32_t reserved[3];  /* MUST be zero */
};

#define TPM_RECORD_HDR_LEN      ((uint32_t)sizeof(struct tpm_record_hdr))
#define TPM_AB_FLOOR_RECORD_LEN (TPM_RECORD_HDR_LEN + \
                                 (uint32_t)sizeof(struct tpm_ab_floor_payload))
#define TPM_BASELINE_BIND_LEN   (TPM_RECORD_HDR_LEN + \
                                 (uint32_t)sizeof(struct tpm_baseline_bind_payload))

/* Layer 1 of the 5-layer defense: the header is a cross-file wire contract, so
 * every load-bearing offset and the total size are pinned here. A field that
 * moves silently would be read as a different field by a machine enrolled
 * under the old layout, and the digest would still validate because it covers
 * whatever bytes are there. */
_Static_assert(__builtin_offsetof(struct tpm_record_hdr, magic) == 0u,
               "tpm_record_hdr.magic must lead the record");
_Static_assert(__builtin_offsetof(struct tpm_record_hdr, layout) == 4u,
               "tpm_record_hdr.layout offset pinned");
_Static_assert(__builtin_offsetof(struct tpm_record_hdr, kind) == 6u,
               "tpm_record_hdr.kind offset pinned");
_Static_assert(__builtin_offsetof(struct tpm_record_hdr, generation) == 8u,
               "tpm_record_hdr.generation offset pinned");
_Static_assert(__builtin_offsetof(struct tpm_record_hdr, payload_len) == 16u,
               "tpm_record_hdr.payload_len offset pinned");
_Static_assert(__builtin_offsetof(struct tpm_record_hdr, reserved) == 20u,
               "tpm_record_hdr.reserved offset pinned");
_Static_assert(__builtin_offsetof(struct tpm_record_hdr, digest) == 24u,
               "tpm_record_hdr.digest offset pinned");
_Static_assert(sizeof(struct tpm_record_hdr) == 56u,
               "tpm_record_hdr is a wire format; its size is pinned");
_Static_assert(sizeof(struct tpm_ab_floor_payload) == 16u,
               "A/B floor payload is a wire format; its size is pinned");
_Static_assert(sizeof(struct tpm_baseline_bind_payload) == 48u,
               "baseline bind payload is a wire format; its size is pinned");

/* Compute the canonical whole-record digest over `buf[0..len)`, treating the
 * header's own digest field as 32 zero bytes. `len` must be at least
 * TPM_RECORD_HDR_LEN. Pure; no transport. Returns TPM_RECORD_OK, or
 * TPM_RECORD_BADARG on a NULL argument or a length below the header. */
tpm_record_status_t tpm_record_digest_compute(const uint8_t *buf, uint32_t len,
                                              uint8_t out[TPM_RECORD_DIGEST]);

/* A parsed, digest-checked record. Points INTO the caller's buffer; it does not
 * copy the payload, so the view is valid only while that buffer is. */
struct tpm_record_view {
    tpm_record_kind_t kind;
    uint64_t          generation;
    const uint8_t    *payload;
    uint32_t          payload_len;
};

/* Parse and fully validate a record blob: magic, layout, expected kind, exact
 * total length, zero reserved bytes, and the canonical digest.
 *
 * `want_kind` is REQUIRED (never TPM_RECORD_KIND_NONE): both records are
 * authorized under the same authority key, so a floor record served from the
 * baseline index carries a perfectly valid digest and only the kind check
 * separates them. `want_payload_len` is likewise exact rather than a minimum,
 * because a longer blob would leave trailing bytes inside the digest that no
 * field describes.
 *
 * Returns TPM_RECORD_OK / BADARG / MALFORMED / KIND / DIGEST_BAD. Pure. */
tpm_record_status_t tpm_record_parse(const uint8_t *buf, uint32_t len,
                                     tpm_record_kind_t want_kind,
                                     uint32_t want_payload_len,
                                     struct tpm_record_view *out);

/* Serialize a record: writes the header, copies the payload, zeroes the
 * reserved bytes and stamps the canonical digest. `cap` must be exactly
 * TPM_RECORD_HDR_LEN + payload_len. Returns TPM_RECORD_OK or BADARG. Pure. */
tpm_record_status_t tpm_record_build(uint8_t *buf, uint32_t cap,
                                     tpm_record_kind_t kind, uint64_t generation,
                                     const uint8_t *payload, uint32_t payload_len);

/* Judge a proposed transition from `cur` to `next`, both already parsed.
 *
 * Enforces exactly three things and deliberately nothing else: the kinds match,
 * the generation advances by EXACTLY one (a jump is not an authorized step, and
 * the +1 is what the authorization binds), and the record does not move
 * backwards in whatever ordering its kind defines. The VALUE may jump freely
 * where the counter may not, which is the whole reason the value lives in a
 * data record.
 *
 * `cur` NULL means "no current record" (the first authorized write). Its
 * generation is NOT pinned to one: TPM 2.0 Part 1 section 37.2.6.3 initializes
 * a freshly defined TPM_NT_COUNTER on its first increment to the LARGEST value
 * any NV counter has held over the TPM's lifetime, so a first enrollment on a
 * previously-used TPM legitimately starts in the hundreds. Only the
 * never-written sentinel 0 is refused, and the counter can never produce it.
 *
 * Returns TPM_RECORD_OK / BADARG / KIND / STEP / ROLLBACK. Pure. */
tpm_record_status_t tpm_record_transition_ok(const struct tpm_record_view *cur,
                                             const struct tpm_record_view *next);

/* Judge a record against the NV counter that commits it.
 *
 * `counter` is the value the bound TPM_NT_COUNTER reads NOW. A record whose
 * generation equals it is CURRENT (TPM_RECORD_OK). A record one AHEAD of the
 * counter is a written-but-uncommitted transition, reported as TPM_RECORD_SKEW
 * rather than accepted -- write-then-increment makes that window legitimate,
 * and the reader's job is to keep using the committed value rather than to
 * trust bytes the commit point has not reached yet. Anything else is
 * TPM_RECORD_SKEW as well: a record BEHIND the counter is stale, and a record
 * more than one ahead cannot have come from a single authorized step.
 *
 * Detecting the counter-ahead-of-record direction (a torn pairing) and choosing
 * the recovery for it belongs to the crash-consistency owner, not here; this
 * function reports the disagreement rather than resolving it.
 * That belongs to the crash-consistent record pairing work, not here.
 *
 * Returns TPM_RECORD_OK / BADARG / TPM_RECORD_SKEW. Pure. */
tpm_record_status_t tpm_record_counter_ok(const struct tpm_record_view *rec,
                                          uint64_t counter);

/* Read the A/B floor's security version out of a validated floor record.
 * Returns TPM_RECORD_OK / BADARG / MALFORMED (payload shorter than the
 * payload type). Pure. */
tpm_record_status_t tpm_record_ab_floor_version(const struct tpm_record_view *rec,
                                                uint32_t *out_version);
