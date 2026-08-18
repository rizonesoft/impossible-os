/* ============================================================================
 * tpm_authz.h -- the authorization boundary for NV record writes
 *
 * tpm_nv.h marshals the commands, tpm_record.h defines what a record IS, and
 * this file decides WHO may replace one. It answers two questions that nothing
 * below it can:
 *
 *   1. Which index is the enrolled one? The contract is DERIVED from a compiled
 *      manifest, never read from storage. tpm_nv_verify_and_read follows the
 *      handle inside the contract it is given, so a contract read back from the
 *      ESP is an attacker-controlled handle: swap it and the "verified" read is
 *      redirected to an index the attacker defined to match. A stored contract
 *      may therefore only ever be a CACHE, compared byte-for-byte against the
 *      manifest before it is believed.
 *
 *   2. Who authorized this write? An offline authority signs an APPROVED POLICY
 *      naming the exact command, the exact record bytes and the exact counter
 *      generation. The kernel hands the signature to the TPM, gets a ticket, and
 *      satisfies the index's PolicyAuthorize authPolicy with it. The kernel
 *      never verifies a signature itself and holds no key material.
 *
 * THE COMMIT POINT IS THE COUNTER, AND IT COMES SECOND. The order is
 * write-then-increment: an authorized record is written and read back while the
 * counter still reads the OLD generation, and only then is the counter advanced.
 * A cpHash binds ONE command, so nothing can make the (write, increment) pair
 * atomic; what CAN be arranged is that the irreversible half happens last, over
 * bytes that are already on the device and already verified. The reverse order
 * advances irreversible state before the authorized replacement exists.
 *
 * That leaves one legitimate window -- a record present and current-plus-one
 * while the counter has not moved -- and readers must keep using the committed
 * value through it. Detecting and RECOVERING the opposite skew (a counter ahead
 * of any record) is crash-consistency work owned elsewhere.
 * That work is the crash-consistent record pairing, owned by the measured-boot
 * attestation roadmap alongside the verified-read boot budget.
 *
 * Phase contract: Phase-1 transport, not ISR-safe. The whole transition -- read
 * the counter, verify the identity, write, read back, increment -- runs inside
 * ONE bounded transport sequence, and THAT is the mutual exclusion. A spinlock
 * would be the wrong instrument here and is forbidden by the kernel gates: this
 * sequence blocks on TPM I/O for milliseconds, and holding a spinlock across it
 * is the exact hazard those gates exist to stop. tpm2_seq_run's busy gate
 * already serializes whole transactions, so a second concurrent advance is
 * refused with TPM_NV_BUSY rather than interleaving -- which is the behaviour
 * the lock was wanted for, without the lock. Two rotations can therefore never
 * both observe the same generation and both believe they advanced it.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/tpm_nv.h"
#include "kernel/tpm_record.h"

/* Manifest revision. A machine enrolled under one revision is NOT silently
 * judged against another: a bump is an allowlisted migration, which is the
 * whole difference between a versioned manifest and an arbitrary stored
 * contract. */
#define TPM_ENROLL_MANIFEST_VERSION 1u

/* How an index's authPolicy is derived. */
typedef enum {
    TPM_ENROLL_POLICY_NONE      = 0u, /* owner-auth index; authPolicy is empty */
    TPM_ENROLL_POLICY_AUTHORIZE = 1u, /* PolicyAuthorize over the authority key */
} tpm_enroll_policy_t;

/* One compiled contract. This is the AUTHORITY on what the index must look
 * like; nothing about it is read from the device or from storage. */
struct tpm_enroll_entry {
    uint32_t nv_index;
    uint16_t name_alg;
    uint32_t attrs;          /* normalized: TPMA_NV_STATUS_MASK bits are 0 */
    uint16_t data_size;
    uint8_t  expect_written; /* 1 = the anchor is written once enrolled */
    uint8_t  policy_kind;    /* tpm_enroll_policy_t */
};

/* The offline update authority. `public_area` is a marshalled TPMT_PUBLIC (the
 * bytes TPM2_LoadExternal takes) and `policy_ref` is the qualifier the authority
 * co-signs, which lets one key authorize different roles without one role's
 * signature satisfying another.
 *
 * UNPROVISIONED IS THE DEFAULT AND IT FAILS CLOSED. A kernel built without an
 * authority key refuses every authorized write rather than falling back to owner
 * auth, exactly as Secure Boot with an empty db refuses every image rather than
 * booting whatever is there. Provisioning a real key is a release-engineering
 * step, not something this module can invent. */
struct tpm_authz_authority {
    const uint8_t *public_area;
    uint16_t       public_len;
    const uint8_t *policy_ref;
    uint16_t       policy_ref_len;
};

/* One authority-issued grant: the approved policy digest and the detached
 * signature over it. Supplied by whatever carries the update (an A/B payload, a
 * rotation request); this module never mints one. */
struct tpm_authz_grant {
    const uint8_t *approved_policy;
    uint16_t       approved_len;
    const uint8_t *signature;     /* marshalled TPMT_SIGNATURE */
    uint16_t       sig_len;
};

/* A transition is TWO co-issued grants, and it has to be.
 *
 * PolicyCpHash pins ONE command with ONE parameter set, so a single approved
 * policy can authorize the record write or the counter increment but never
 * both. Leaving the increment on owner auth was the alternative and it is
 * strictly worse than it looks: an attacker who can increment at will
 * manufactures the counter-ahead-of-record state deliberately, which turns a
 * crash-recovery path into an attack surface. So the authority co-issues both
 * halves of the transition it approved, and neither half authorizes the other. */
struct tpm_authz_transition {
    struct tpm_authz_grant write;  /* authorizes NV_Write of these exact bytes */
    struct tpm_authz_grant commit; /* authorizes NV_Increment of the counter */
};

/* Is this transition structurally usable? Both halves must carry an approved
 * policy of the right size and a signature. Exported for a consumer whose first
 * act is irreversible: it must be able to refuse a malformed grant BEFORE the
 * mutation, not after. Returns TPM_NV_OK or TPM_NV_BADARG. Pure. */
tpm_nv_status_t tpm_authz_grant_wellformed(const struct tpm_authz_transition *tr);

/* Install the authority for this boot. ONE-WAY: once an authority is installed
 * this returns TPM_NV_AUTH for every later call, including a NULL one.
 *
 * That is the enforcement behind everything else here. Both the plain baseline
 * enroll's refusal and the verifier's bind check ask tpm_authz_provisioned(),
 * so a clearable authority would make the whole boundary optional to any caller
 * that could reach this setter. Rotating a live authority key is a separate
 * question with its own authorization, not a store.
 *
 * Returns TPM_NV_OK, TPM_NV_AUTH when an authority is already installed, or
 * TPM_NV_BADARG when the argument is a misconfiguration (empty or too-short
 * public area, or a NULL policyRef carrying a length). Pure; no transport. */
tpm_nv_status_t tpm_authz_set_authority(const struct tpm_authz_authority *auth);

#ifdef KERNEL_TESTS
/* Test-only teardown, because the setter above is deliberately one-way. */
void tpm_authz_test_clear_authority(void);
#endif

/* 1 when an authority is installed. A caller deciding whether an authorized
 * rotation is even offerable asks this rather than attempting one and reading
 * the refusal. */
int tpm_authz_provisioned(void);

/* The authority key's Name BODY: nameAlg || H(publicArea), 34 bytes for
 * SHA-256, the same value the TPM computes at LoadExternal.
 *
 * BODY, not a marshalled TPM2B, and the distinction is load-bearing. The wire
 * builders add their own TPM2B size prefix, so returning a prefixed value here
 * made tpm2_build_policy_authorize emit the size twice and the TPM read 0x0022
 * as the name algorithm. tpm2_parse_load_external likewise hands back the body
 * with the prefix stripped, so ONE representation flows end to end and the two
 * Names can be compared directly.
 *
 * Returns TPM_NV_OK, TPM_NV_BADARG on a short buffer or a non-SHA-256 nameAlg,
 * or TPM_NV_UNAVAIL when no authority is installed. Pure. */
tpm_nv_status_t tpm_authz_authority_name(uint8_t *out, uint16_t cap,
                                         uint16_t *out_len);

/* Look up the compiled manifest entry for an index. Returns NULL when the index
 * is not one this kernel enrolls -- which is itself the answer to "may I verify
 * against this handle": no manifest row, no contract, no verified read. Pure. */
const struct tpm_enroll_entry *tpm_enroll_lookup(uint32_t nv_index);

/* Derive the enrolled contract for `nv_index` from the compiled manifest.
 *
 * For a PolicyAuthorize index this computes the authPolicy on the TPM via a
 * TRIAL session rather than reproducing the policyDigest formula in kernel code.
 * A hardcoded formula that is subtly wrong produces an index nobody can ever
 * satisfy, and the mistake only surfaces on real firmware; letting the TPM
 * compute its own digest cannot drift from the TPM's own rules.
 *
 * Returns TPM_NV_OK, TPM_NV_BADARG (NULL / unknown index), TPM_NV_UNAVAIL when
 * the entry needs an authority and none is installed, or any status the trial
 * session produces. Uses the transport for policy-bearing entries; pure for
 * owner-auth ones. */
tpm_nv_status_t tpm_authz_contract(uint32_t nv_index,
                                   struct tpm_nv_identity *out);

/* Judge a PERSISTED contract cache against the manifest-derived contract, field
 * by field. A cache that disagrees is discarded, never preferred: this is the
 * entire mechanism that keeps a stored contract from becoming an authority.
 * Returns TPM_NV_OK on an exact match, TPM_NV_MISMATCH otherwise, plus whatever
 * tpm_authz_contract can return. */
tpm_nv_status_t tpm_authz_contract_cache_ok(const struct tpm_nv_identity *cache);

/* Write `record` to `nv_index` under the authority's grant, then commit it by
 * advancing `counter_index`.
 *
 * The whole sequence -- read the counter, verify the index identity, write,
 * read back, increment -- runs under ONE transition lock and ONE bounded
 * transport sequence. `expect_counter` is the generation the caller believes is
 * current; a disagreement is TPM_NV_MISMATCH rather than a retry, because the
 * grant was signed for one specific transition and no other.
 *
 * Returns TPM_NV_OK only when the record is on the device AND the counter has
 * advanced. TPM_NV_UNAVAIL when unprovisioned, TPM_NV_BADARG on caller misuse,
 * TPM_NV_MISMATCH when the observed generation or the readback disagrees, and
 * any transport/TPM status otherwise. A failure after the write but before the
 * increment leaves the uncommitted-record window described above, which readers
 * already treat as not-current. */
tpm_nv_status_t tpm_authz_write_record(uint32_t nv_index, uint32_t counter_index,
                                       const uint8_t *record, uint16_t record_len,
                                       uint64_t expect_counter,
                                       const struct tpm_authz_transition *tr);

/* ---- The two consumers ---- */

/* Read the A/B anti-rollback floor: verify the index identity against the
 * manifest contract, read the record in the SAME sequence, validate it, and
 * check it against the committed counter.
 *
 * Returns TPM_NV_OK with *out_version set only for a record that is BOTH
 * well-formed and current. A written-but-uncommitted record is TPM_NV_MISMATCH:
 * the floor a slot is judged against must be the committed one, and treating an
 * uncommitted record as the floor would let a half-finished update raise or
 * lower it. */
tpm_nv_status_t tpm_ab_floor_read(uint32_t *out_version, uint64_t *out_generation);

/* Advance the floor to `new_version` under an authority grant. Refuses a
 * version BELOW the stored one locally before spending a TPM transaction, which
 * is a convenience rather than the boundary: the boundary is that the grant
 * authorized this exact record. */
tpm_nv_status_t tpm_ab_floor_advance(uint32_t new_version,
                                     const struct tpm_authz_transition *tr);

/* Verify a baseline blob against its authenticated bind record.
 *
 * The baseline blob itself stays in its owner-writable index; this proves the
 * blob is the one an authority approved. An attacker who relabels an old blob
 * with the current generation and a recomputed CRC fails HERE, on a digest they
 * cannot forge and a bind record they cannot write.
 *
 * Returns TPM_NV_OK when the blob matches the committed bind record,
 * TPM_NV_MISMATCH when it does not, TPM_NV_NOTFOUND when NO bind record exists
 * -- which is a LEGACY, unauthenticated baseline. A legacy baseline is reported,
 * never silently accepted as golden and never auto-wrapped into a bind record:
 * its content is owner-writable, so wrapping it would authenticate whatever an
 * attacker last wrote. Migrating one belongs to the versioned baseline growth
 * and NV index migration work. */
tpm_nv_status_t tpm_baseline_bind_verify(const uint8_t *blob, uint32_t blob_len,
                                         uint64_t *out_generation);

/* Bind a baseline blob under an authority grant: build the bind record over the
 * blob's digest and write it through the authorized path. */
tpm_nv_status_t tpm_baseline_bind_write(const uint8_t *blob, uint32_t blob_len,
                                        const struct tpm_authz_transition *tr);
