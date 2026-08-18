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
 *   PCR-set digest -- the exact measured state the operator approved. Without
 *     it a blob signed for a known-good boot authorizes enrolling a tampered
 *     one as golden, which inverts the whole point of a baseline.
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
 * endorsement seed, so it is stable across reboots and changes on TPM2_Clear.
 * That is the right epoch: a cleared TPM must not honor authorizations issued
 * before the clear, and this binding gives that for free.
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
#include "kernel/tpm_nv.h"   /* tpm_nv_status_t, for the provisioning verb */

/* Wire constants. The blob is an EXTERNAL format: an offline signing tool must
 * reproduce it byte for byte, so every field offset is pinned by a
 * _Static_assert below rather than left to the compiler's discretion. */
#define TPM_HEADLESS_MAGIC        0x5A414849u  /* 'I','H','A','Z' little-endian */
#define TPM_HEADLESS_VERSION      1u
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
    uint8_t  pcr_set[TPM_HEADLESS_ID_LEN];     /* SHA-256 over the PCR set */
    uint64_t valid_at_counter;                 /* exact counter value authorized */
    uint8_t  signature[TPM_HEADLESS_SIG_LEN];  /* Ed25519 over the bytes above */
};

/* Bytes the signature covers: everything up to (not including) the signature. */
#define TPM_HEADLESS_SIGNED_LEN   88u
#define TPM_HEADLESS_BLOB_LEN     152u

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
_Static_assert(__builtin_offsetof(struct tpm_headless_authz_blob, pcr_set) == 48u,
               "headless authz wire layout: pcr_set at 48");
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
    TPM_HEADLESS_WRONG_PCR_SET   = 6u,  /* signed for a different measured state */
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

/* Every input the verification decision depends on, BY VALUE. Taking a struct
 * rather than reading globals is what makes the refusal matrix unit-testable
 * without a live TPM, which matters because the dev host has no swtpm. */
struct tpm_headless_authz_inputs {
    const uint8_t *blob;        /* presented bytes; NULL = absent */
    uint32_t blob_len;
    uint8_t  authority_present; /* 1 = authority_pub holds a provisioned key */
    uint8_t  device_known;      /* 1 = device_id holds this machine's identity */
    uint8_t  pcr_set_known;     /* 1 = pcr_set holds the current measured state */
    uint8_t  counter_known;     /* 1 = counter was read through a verified index */
    uint8_t  authority_pub[TPM_HEADLESS_PUBKEY_LEN];
    uint8_t  device_id[TPM_HEADLESS_ID_LEN];
    uint8_t  pcr_set[TPM_HEADLESS_ID_LEN];
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
 * private half never enters this tree. */
int tpm_headless_authz_set_authority(const uint8_t pub[TPM_HEADLESS_PUBKEY_LEN]);

/* 1 when an authority is installed. With no authority the escape hatch does
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
 * before returning -- gather the device identity, the current measured state
 * and the verified counter, evaluate, then increment the counter.
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
                                                    uint32_t operation);
