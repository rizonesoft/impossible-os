/* ============================================================================
 * tpm_headless_authz.h -- One-shot signed authorization for headless enrollment
 *
 * tpm_enroll_gate.h admits a baseline write on exactly one path: an
 * affirmative keypress on a local console. That is the right default and it
 * leaves a headless server unable to ever enroll a baseline, because
 * boot_confirm returns TPM_CONFIRM_UNAVAILABLE when no PS/2 controller
 * answers and the gate refuses.
 *
 * This module is the second admitting path, and it is built so that the thing
 * an attacker can actually write -- a file on the ESP -- authorizes nothing on
 * its own. The predecessor design was file-only authority beside the loader,
 * which is precisely what the enrollment gate exists to remove, so every
 * property below is there to stop that from coming back under a new name.
 *
 * WHAT AN AUTHORIZATION IS BOUND TO, AND WHY EACH BINDING EXISTS
 *
 *   device identity -- the digest of the EK PRIMARY public. Without it a blob
 *     captured from one machine authorizes enrollment on every machine that
 *     shares the operator's signing key.
 *   transition digest -- the exact record that would be written AND the record
 *     it replaces. Without it a blob signed for a known-good boot authorizes
 *     enrolling a tampered one as golden, which inverts the whole point of a
 *     baseline. It covers the PREDECESSOR as well as the candidate because a
 *     digest over the candidate alone still permits a rollback: a token issued
 *     for state A survives an intervening console enrollment of B, and the
 *     enroll path silently rotates, so A reinstalls at a HIGHER generation and
 *     the monotonic check waves it through. The two-sided digest makes such a
 *     token describe a transition that no longer exists, and it is refused.
 *   operation -- the specific request. Without it a blob captured for a first
 *     enrollment authorizes a rotation or a reset.
 *   counter value -- an authenticated TPM monotonic counter. Without it the
 *     same bytes replay on every subsequent boot, which is file-only authority
 *     with a signature stapled to it.
 *
 * THE DEVICE IDENTITY IS THE EK, NOT THE AK. tpm_attest.c provisions the AK
 * with TPM2_Create on EVERY boot and caches it for that boot only, so a digest
 * over the AK public differs each time and a pre-signed blob would be refused
 * as wrong-machine ON THE CORRECT MACHINE. The EK primary is derived from the
 * Endorsement Primary Seed, which is stable across reboots.
 *
 * TPM2_Clear does NOT invalidate the EK, and an earlier version of this
 * comment claimed it did -- corrected by a parity-research pass against the
 * TCG spec and tpm2-tools documentation. TPM2_Clear resets the Storage
 * Primary Seed and owner/lockout auth; only TPM2_ChangeEPS, a
 * platform-hierarchy-only command, changes the seed the EK derives from. So a
 * captured blob's device-identity binding survives a Clear on its own.
 *
 * What actually invalidates an outstanding authorization across a Clear is
 * the anti-rollback anchors already rely on: TPM_NV_INDEX_HEADLESS_SEQ is an
 * OWNER-hierarchy index, so TPM2_Clear undefines it, and the next boot's
 * tpm_headless_authz_provision() redefines and re-increments it -- but a
 * recreated TPM_NT_COUNTER cannot restart below the highest value ANY NV
 * counter has held over the TPM's lifetime (the same guarantee tpm_nv.h:217
 * and tpm_authz.c:67 already document and rely on for the record-transition
 * anchors). A blob signed for a counter value the TPM has already passed is
 * therefore refused STALE_OR_SPENT after a Clear exactly as it would be
 * without one.
 *
 * ONE SHOT MEANS CONSUMED BEFORE THE MUTATION, NOT AFTER IT. The counter is
 * incremented before any baseline write, and an increment that fails or whose
 * completion is unknown is treated as SPENT rather than retried. The token
 * therefore authorizes one ATTEMPT, not one success -- the alternative leaves
 * a reset window between a written baseline and a later increment in which the
 * identical blob is still acceptable.
 *
 * THE COUNTER IS READ THROUGH AN ENROLLED CONTRACT. tpm_nv_read_counter()
 * decodes eight bytes with no NV_ReadPublic, type, attribute or Name check, so
 * a replacement data index at the same handle would supply an attacker-chosen
 * value. Reads here go through tpm_nv_verify_and_read() against the contract
 * derived from tpm_authz.c's compiled manifest, which verifies identity and
 * lifecycle inside the same bounded sequence as the read.
 *
 * The counter index is OWNER-writable on purpose. An attacker holding owner
 * auth can increment it, and that only DENIES: it spends outstanding tokens.
 * It can never forge an acceptance, because acceptance needs a signature over
 * the exact counter value, and it can never roll the counter back, because an
 * NV counter cannot decrease and a recreated one cannot restart below the
 * highest value any NV counter has held over the TPM's lifetime.
 *
 * UNPROVISIONED IS THE DEFAULT AND IT FAILS CLOSED. A kernel with no authority
 * key installed has no escape hatch at all rather than a weak one, and the
 * enrollment gate behaves bit for bit as it does without this module.
 *
 * XREF: todo/01-boot-platform/TODO-13-tpm-measured-boot-attestation.md
 *       "Headless Enrollment Authorization Escape Hatch"
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/tpm_nv.h"
#include "kernel/tpm_baseline.h"   /* tpm_baseline_status_t, for the enroll dispatch */
#include "kernel/ci/ci_crypto.h"  /* CI_ED25519_* -- asserted equal below */
#include "kernel/crypto/sha256.h" /* SHA256_DIGEST_LEN -- asserted equal below */

/* Wire constants. The blob is an EXTERNAL format: an offline signing tool must
 * reproduce it byte for byte, so every field offset is pinned by a
 * _Static_assert below rather than left to the compiler's discretion. */
#define TPM_HEADLESS_MAGIC        0x5A414849u  /* 'I','H','A','Z' little-endian */
#define TPM_HEADLESS_VERSION      2u          /* v1 bound a PCR-set digest; v2 binds the whole transition */
#define TPM_HEADLESS_ID_LEN       32u          /* SHA-256 */
#define TPM_HEADLESS_SIG_LEN      64u          /* Ed25519 */
#define TPM_HEADLESS_PUBKEY_LEN   32u          /* Ed25519 */

/* The operation an authorization is signed for. Distinct values, never a
 * boolean: a blob captured for a first enrollment must not authorize a
 * rotation or a reset, and the refusal has to say which was asked for. */
typedef enum {
    TPM_HEADLESS_OP_NONE            = 0u,
    TPM_HEADLESS_OP_ENROLL_BASELINE = 1u,
    TPM_HEADLESS_OP_ROTATE_BASELINE = 2u,
    TPM_HEADLESS_OP_RESET_BASELINE  = 3u,
} tpm_headless_op_t;

#define TPM_HEADLESS_OP_MAX TPM_HEADLESS_OP_RESET_BASELINE

/* The signed authorization, exactly as it appears on the wire. Fixed layout,
 * little-endian scalars, no implicit padding: the signature covers bytes
 * [0, TPM_HEADLESS_SIGNED_LEN) and any change to this struct is a change to
 * the format every offline tool already emits. */
struct tpm_headless_authz_blob {
    uint32_t magic;                            /* TPM_HEADLESS_MAGIC */
    uint16_t version;                          /* TPM_HEADLESS_VERSION */
    uint16_t reserved0;                        /* must be 0 */
    uint32_t operation;                        /* tpm_headless_op_t */
    uint32_t reserved1;                        /* must be 0 */
    uint8_t  device_id[TPM_HEADLESS_ID_LEN];   /* SHA-256 over the EK public */
    uint8_t  transition_id[TPM_HEADLESS_ID_LEN]; /* SHA-256 over the whole authorized transition */
    uint64_t valid_at_counter;                 /* exact counter value authorized */
    uint8_t  signature[TPM_HEADLESS_SIG_LEN];  /* Ed25519 over the bytes above */
};

/* Bytes the signature covers: everything up to (not including) the signature. */
#define TPM_HEADLESS_SIGNED_LEN   88u
#define TPM_HEADLESS_BLOB_LEN     152u

/* THE DECODER READS THESE, NOT LITERALS, and that is what makes the asserts
 * below load-bearing. Pinning offsets in a struct no production path reads
 * protects a declaration: the parser could drift to different literals and
 * every assert would still pass. Deriving the offsets from the struct means
 * one edit moves the struct, the asserts and the decoder together. */
#define TPM_HEADLESS_OFF_MAGIC     __builtin_offsetof(struct tpm_headless_authz_blob, magic)
#define TPM_HEADLESS_OFF_VERSION   __builtin_offsetof(struct tpm_headless_authz_blob, version)
#define TPM_HEADLESS_OFF_RESERVED0 __builtin_offsetof(struct tpm_headless_authz_blob, reserved0)
#define TPM_HEADLESS_OFF_OPERATION __builtin_offsetof(struct tpm_headless_authz_blob, operation)
#define TPM_HEADLESS_OFF_RESERVED1 __builtin_offsetof(struct tpm_headless_authz_blob, reserved1)
#define TPM_HEADLESS_OFF_DEVICE_ID __builtin_offsetof(struct tpm_headless_authz_blob, device_id)
#define TPM_HEADLESS_OFF_TRANSITION __builtin_offsetof(struct tpm_headless_authz_blob, transition_id)
#define TPM_HEADLESS_OFF_COUNTER   __builtin_offsetof(struct tpm_headless_authz_blob, valid_at_counter)
#define TPM_HEADLESS_OFF_SIGNATURE __builtin_offsetof(struct tpm_headless_authz_blob, signature)

_Static_assert(__builtin_offsetof(struct tpm_headless_authz_blob, magic) == 0u,
               "headless authz wire layout: magic at 0");
_Static_assert(__builtin_offsetof(struct tpm_headless_authz_blob, version) == 4u,
               "headless authz wire layout: version at 4");
_Static_assert(__builtin_offsetof(struct tpm_headless_authz_blob, reserved0) == 6u,
               "headless authz wire layout: reserved0 at 6");
_Static_assert(__builtin_offsetof(struct tpm_headless_authz_blob, operation) == 8u,
               "headless authz wire layout: operation at 8");
_Static_assert(__builtin_offsetof(struct tpm_headless_authz_blob, reserved1) == 12u,
               "headless authz wire layout: reserved1 at 12");
_Static_assert(__builtin_offsetof(struct tpm_headless_authz_blob, device_id) == 16u,
               "headless authz wire layout: device_id at 16");
_Static_assert(__builtin_offsetof(struct tpm_headless_authz_blob, transition_id) == 48u,
               "headless authz wire layout: transition_id at 48");
_Static_assert(__builtin_offsetof(struct tpm_headless_authz_blob, valid_at_counter) == 80u,
               "headless authz wire layout: valid_at_counter at 80");
_Static_assert(__builtin_offsetof(struct tpm_headless_authz_blob, signature) == 88u,
               "headless authz wire layout: signature at 88");
_Static_assert(__builtin_offsetof(struct tpm_headless_authz_blob, signature)
               == TPM_HEADLESS_SIGNED_LEN,
               "the signature must cover exactly the bytes that precede it");
_Static_assert(sizeof(struct tpm_headless_authz_blob) == TPM_HEADLESS_BLOB_LEN,
               "headless authz blob size is an external format -- update the "
               "offline signing tool before changing it");

/* Exactly why an authorization was refused. Distinct values, never one
 * boolean: an operator holding a blob that does not work needs to know
 * whether it is stale, for the wrong machine, for a different measured state,
 * or for a different operation, because each has a different remedy. */
typedef enum {
    /* ABSENT IS ZERO, AND THAT IS LOAD-BEARING. This value is carried in
     * struct tpm_enroll_gate_inputs, which every caller and every test builds
     * by zeroing; if the admitting verdict were 0 a zeroed struct would read as
     * authorized, so the default has to be the refusal. It matches the rest of
     * the subsystem, where TPM_ENROLL_AUTH_NONE and TPM_CONFIRM_PENDING are
     * likewise the zero value. */
    TPM_HEADLESS_ABSENT          = 0u,  /* no authorization presented at all */
    TPM_HEADLESS_OK              = 1u,  /* verified; the caller may consume it */
    TPM_HEADLESS_NO_AUTHORITY    = 2u,  /* no signing authority provisioned */
    TPM_HEADLESS_BAD_FORMAT      = 3u,  /* wrong length, magic, version or padding */
    TPM_HEADLESS_BAD_SIGNATURE   = 4u,  /* signature does not verify */
    TPM_HEADLESS_WRONG_DEVICE    = 5u,  /* signed for a different EK identity */
    TPM_HEADLESS_WRONG_TRANSITION = 6u, /* signed for a different measured-state transition */
    TPM_HEADLESS_WRONG_OPERATION = 7u,  /* signed for a different request */
    TPM_HEADLESS_STALE_OR_SPENT  = 8u,  /* the counter has moved past it */
    TPM_HEADLESS_NOT_YET_VALID   = 9u,  /* the counter has not reached it */
    /* The device identity, the measured state or the counter could not be
     * established. Separate from every mismatch above because nothing was
     * compared: the remedy is to fix the TPM, not to reissue the blob. */
    TPM_HEADLESS_UNKNOWN_DEVICE  = 10u,
    TPM_HEADLESS_UNKNOWN_STATE   = 11u,
    TPM_HEADLESS_COUNTER_UNTRUSTED = 12u,
    /* Verified, but the one-shot consumption did not complete. Treated as
     * spent, never as admission. */
    TPM_HEADLESS_CONSUME_FAILED  = 13u,
} tpm_headless_verdict_t;

#define TPM_HEADLESS_VERDICT_MAX TPM_HEADLESS_CONSUME_FAILED

/* ABSENT == 0 is a SECURITY invariant, not a numbering accident: it is what
 * makes a zeroed struct tpm_enroll_gate_inputs refuse. Pinned here because the
 * enrollment gate pins TPM_REPLAY_VERIFIED == 0 for exactly the same reason,
 * and an unpinned one had the coverage inverted relative to its use. If a
 * future verdict were inserted such that OK became zero, this fires at compile
 * time instead of failing OPEN on a live gate. */
_Static_assert((int)TPM_HEADLESS_ABSENT == 0,
               "ABSENT must be the zero verdict: a zeroed gate-inputs struct "
               "has to read as a refusal, never as an authorization");
_Static_assert((int)TPM_HEADLESS_OK != 0,
               "OK must NOT be the zero verdict");

/* These three relations are USED as equalities every time a blob is verified:
 * the authority buffer is handed to ci_crypto_verify with the Ed25519 length,
 * the signature is read at the Ed25519 length out of a field sized by this
 * header, and sha256_final writes a digest into an ID-sized buffer. They agree
 * today; these asserts are what keeps them agreeing across two other headers. */
_Static_assert(TPM_HEADLESS_PUBKEY_LEN == CI_ED25519_PUBKEY_LEN,
               "the authority key buffer must be exactly what ci_crypto reads");
_Static_assert(TPM_HEADLESS_SIG_LEN == CI_ED25519_SIG_LEN,
               "the signature field must be exactly what ci_crypto reads");
_Static_assert(TPM_HEADLESS_ID_LEN == SHA256_DIGEST_LEN,
               "an identity field must hold exactly one SHA-256 digest");

/* Every input the verification decision depends on, BY VALUE. Taking a struct
 * rather than reading globals is what makes the refusal matrix unit-testable
 * without a live TPM, which matters because the dev host has no swtpm. */
struct tpm_headless_authz_inputs {
    const uint8_t *blob;        /* presented bytes; NULL = absent */
    uint32_t blob_len;
    uint8_t  authority_present; /* 1 = authority_pub holds a provisioned key */
    uint8_t  device_known;      /* 1 = device_id holds this machine's identity */
    uint8_t  transition_known;  /* 1 = transition_id holds the transition this boot would perform */
    uint8_t  counter_known;     /* 1 = counter was read through a verified index */
    uint8_t  authority_pub[TPM_HEADLESS_PUBKEY_LEN];
    uint8_t  device_id[TPM_HEADLESS_ID_LEN];
    uint8_t  transition_id[TPM_HEADLESS_ID_LEN];
    uint32_t operation;         /* the operation actually being requested */
    uint64_t counter;           /* the verified current counter value */
};

/* Evaluate an authorization. PURE and TOTAL: no globals, no I/O, no TPM, every
 * input combination yields exactly one verdict, and a NULL `in` is a
 * fail-closed refusal rather than a fault.
 *
 * Returns TPM_HEADLESS_OK only when the bytes parse, the signature verifies
 * under the provisioned authority, and the device, measured state, operation
 * and counter all match. OK does NOT authorize anything on its own -- the
 * caller must still consume the token before mutating, which is what
 * tpm_headless_authz_authorize() does. */
tpm_headless_verdict_t
tpm_headless_authz_evaluate(const struct tpm_headless_authz_inputs *in);

/* The purely LOCAL half of the decision: length, magic, version, reserved
 * bytes, operation range and the Ed25519 signature. Pure, total, and it touches
 * no TPM.
 *
 * It exists so an authorization can be REJECTED BEFORE any TPM transaction is
 * spent. The blob is attacker-writable on the ESP, so gathering the live device
 * identity and the counter first would let anyone force a CreatePrimary and two
 * NV operations on every boot with a file they simply made up. After this
 * check, only a blob genuinely signed by the installed authority can reach the
 * TPM at all.
 *
 * tpm_headless_authz_evaluate calls this FIRST and its refusals are unchanged,
 * so there is exactly one implementation of the ordering and no way for the two
 * to disagree. Returns TPM_HEADLESS_OK when the local half passes. */
tpm_headless_verdict_t
tpm_headless_authz_precheck(const uint8_t *blob, uint32_t blob_len,
                            uint8_t authority_present,
                            const uint8_t *authority_pub);

/* The local half, run against the INSTALLED authority. Same decision as
 * tpm_headless_authz_precheck, without the caller having to hold the authority
 * key -- it snapshots the installed one itself. Touches NO TPM.
 *
 * It exists so a CALLER can order its own work correctly. The caller must
 * gather the candidate measured state and read the predecessor baseline before
 * it can describe a transition, and both cost TPM transactions; running them
 * ahead of the signature check would let anyone who can write a file beside the
 * loader force that work on every boot. Call this first, and do the expensive
 * gathering only when it returns TPM_HEADLESS_OK.
 *
 * Returns TPM_HEADLESS_ABSENT for no blob, TPM_HEADLESS_NO_AUTHORITY when
 * none is installed, and the usual format/signature refusals otherwise.
 * A returned OK is NOT an admission: the bindings and the one-shot consume
 * still happen in tpm_headless_authz_authorize. */
tpm_headless_verdict_t
tpm_headless_authz_precheck_installed(const uint8_t *blob, uint32_t blob_len);

/* Prepare a headless enrollment: run the local half, and ONLY on success
 * gather the candidate measured state, read the predecessor baseline, derive
 * the transition digest, and decide which operation this boot is actually
 * requesting.
 *
 * THE ORDER IS THE CONTRACT, and it lives here rather than at the call site so
 * a test can pin it. Gathering the candidate costs a full PCR snapshot and the
 * predecessor costs a verified NV read; running either before the signature
 * check would let anyone who can write a file beside the loader force that work
 * on every boot. A refusal returned by this function is therefore guaranteed to
 * have spent ZERO TPM transactions.
 *
 * The operation follows the OBSERVED transition -- ENROLL when the index holds
 * no baseline, ROTATE when it does -- never a constant. The write path silently
 * rotates when a baseline already exists, so a hardcoded ENROLL would let a
 * token issued for one measured state reinstall it over a newer one at a higher
 * generation, with the monotonic check waving it through.
 *
 * Returns TPM_HEADLESS_OK with *out_cand, out_transition and *out_operation
 * populated. Any other verdict leaves them UNDEFINED and the caller must pass a
 * NULL transition to tpm_headless_authz_authorize, which refuses rather than
 * admitting unbound. A predecessor that cannot be established is
 * TPM_HEADLESS_UNKNOWN_STATE and never an assumed absence: reading an
 * unreadable predecessor as absent is how a rollback becomes a first enroll. */
struct tpm_baseline;
tpm_headless_verdict_t
tpm_headless_enroll_prepare(const uint8_t *blob, uint32_t blob_len,
                            uint32_t nv_index, uint16_t alg,
                            struct tpm_baseline *out_cand,
                            uint8_t out_transition[TPM_HEADLESS_ID_LEN],
                            uint32_t *out_operation);

/* Perform the admitted enrollment, choosing the write path from the AUTHORITY
 * the gate reported.
 *
 * This is the section's central trust boundary and it lives here so a test can
 * pin it. A headless admission MUST write the candidate the authorization
 * covered, through tpm_baseline_enroll_headless; routing it to the ordinary
 * tpm_baseline_enroll instead would take a FRESH SNAPSHOT and store bytes
 * nobody authorized, having already spent the operator's one-shot token.
 * Inlined at the Phase-1 call site that swap left every test green, because
 * boot_phase1 cannot be driven from a unit test.
 *
 * `authority` is the gate's tpm_enroll_authority_t. `have_transition` says
 * whether `cand` and `transition_id` were actually prepared.
 *
 * A headless authority WITHOUT a prepared transition REFUSES with
 * TPM_BASELINE_BADARG rather than falling back to the snapshot path. The gate
 * cannot report that combination today (its admitting verdict requires a
 * transition to have been supplied), and the refusal is the point: if that ever
 * became reachable, silently writing an unauthorized snapshot is the one
 * outcome this whole section exists to prevent. Every other authority takes the
 * unchanged console path. */
tpm_baseline_status_t
tpm_headless_enroll_dispatch(uint8_t authority, int have_transition,
                             uint32_t nv_index, uint16_t alg,
                             const struct tpm_baseline *cand,
                             const uint8_t *transition_id);

/* Stable label for a verdict, for logs and the integrity report. Never NULL --
 * an out-of-range value returns "unknown". */
const char *tpm_headless_verdict_label(uint8_t verdict);

/* Install the offline signing authority's Ed25519 PUBLIC key. ONE-WAY: a
 * second call with a different key is refused, so a later caller cannot widen
 * who may authorize a headless enrollment. Returns 0 on success, -1 on a NULL
 * key, and -2 when a DIFFERENT authority is already installed (re-installing
 * the identical key is idempotent and succeeds).
 *
 * This is a provisioning step, not something the kernel can invent: the
 * private half never enters this tree.
 *
 * The claim is INDIVISIBLE: exactly one caller wins the install, and a caller
 * that arrives while another is mid-copy gets -3 rather than a half-written
 * key. Release ordering alone would have let two first-installers interleave
 * their key bytes and both report success. */
int tpm_headless_authz_set_authority(const uint8_t pub[TPM_HEADLESS_PUBKEY_LEN]);

/* 1 when an authority is FULLY installed -- never while one is mid-install. With no authority the escape hatch does
 * not exist and every presented blob is refused NO_AUTHORITY. */
int tpm_headless_authz_authority_present(void);

/* Forget the installed authority. TEST SEAM ONLY, and COMPILED OUT of release
 * builds: the one-way install above is the entire production contract, so a
 * globally linked reset would let any present or future in-kernel caller
 * install a different authority, and the documented refusal would be a comment
 * rather than an invariant. A test that could not reset it could only ever
 * assert the first of the two states, which is why the seam exists at all. */
#ifdef KERNEL_TESTS
void tpm_headless_authz_reset_authority_for_test(void);
#endif

/* Provision the replay counter: define it if absent, then perform its FIRST
 * increment so it becomes readable.
 *
 * THIS IS REQUIRED AND IS NOT OPTIONAL BOOTSTRAP TIDINESS. A TPM_NT_COUNTER has
 * TPMA_NV_WRITTEN clear until its first increment, and NV_Read on that state
 * returns TPM_NV_UNINIT -- so without this call the counter can never be read,
 * every authorization refuses COUNTER_UNTRUSTED, and a freshly provisioned
 * machine can never consume its first token. The enrolled contract deliberately
 * does NOT demand written (that would refuse the index between definition and
 * first use), which is exactly why the bootstrap has to be explicit.
 *
 * Idempotent: an already-provisioned counter is left alone and reports OK. The
 * resulting value is NOT assumed to be any particular number -- read it with
 * tpm_headless_authz_next_counter and sign against what the TPM actually holds.
 *
 * Returns TPM_NV_OK, or the status of the define, increment or verifying read
 * that failed. A release-engineering step, beside installing the authority. */
tpm_nv_status_t tpm_headless_authz_provision(void);

/* The NV counter value a NEW authorization must be signed against, read
 * through the enrolled contract. Returns 0 on success and writes *out; a
 * non-zero return leaves *out untouched and means the counter could not be
 * trusted, not that it is zero. Exposed so an operator tool can be told which
 * value to sign without also being handed the consume path. */
int tpm_headless_authz_next_counter(uint64_t *out);

/* Verify a presented authorization for `operation` and, on success, CONSUME it
 * before returning -- gather the device identity and the verified counter,
 * evaluate against the caller's transition digest, then increment the counter.
 *
 * `transition_id` IS SUPPLIED BY THE CALLER, and that is the whole point of
 * the argument. This module used to derive the measured state itself by taking
 * its own tpm_baseline_snapshot, which meant the bytes it authorized and the
 * bytes the enrollment then wrote were two independent snapshots -- an
 * honestly authorized enrollment could write content nobody had authorized.
 * The caller now builds the candidate ONCE, derives the transition digest from
 * it with tpm_baseline_transition_id(), authorizes THAT, and hands the same
 * bytes to the write. A NULL transition_id is a refusal
 * (TPM_HEADLESS_UNKNOWN_STATE), never an unbound admission.
 *
 * Returns TPM_HEADLESS_OK only when the token was verified AND irreversibly
 * spent. An increment that fails or whose completion is unknown returns
 * TPM_HEADLESS_CONSUME_FAILED and is NEVER retried: the token authorizes one
 * attempt, so a caller that treats an ambiguous consume as a fresh chance
 * hands an attacker the replay this module exists to prevent.
 *
 * There is deliberately no verify-without-consume entry point. Splitting the
 * two would let a caller admit an enrollment on a verdict it never spent,
 * which is the exact ordering defect this API shape removes. */
tpm_headless_verdict_t tpm_headless_authz_authorize(const uint8_t *blob,
                                                    uint32_t blob_len,
                                                    uint32_t operation,
                                                    const uint8_t *transition_id);
