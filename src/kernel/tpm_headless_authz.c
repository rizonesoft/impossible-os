/* tpm_headless_authz.c -- one-shot signed authorization for headless enrollment.
 *
 * The security posture, the reason the device identity is the EK rather than
 * the AK, and why the token is consumed before the mutation rather than after
 * it, all live in the header banner. This file is the mechanism.
 */
#include "kernel/tpm_headless_authz.h"
#include "kernel/tpm_enroll_gate.h"
#include "kernel/tpm_baseline.h"
#include "kernel/tpm.h"
#include "kernel/tpm_attest.h"
#include "kernel/tpm_authz.h"
#include "kernel/tpm_nv.h"
#include "kernel/ci/ci_crypto.h"
#include "kernel/crypto/sha256.h"

/* ---- unaligned-safe little-endian loads -------------------------------------
 * The presented bytes arrive from outside the kernel and carry no alignment
 * guarantee, so the wire struct in the header anchors the layout asserts and
 * is never type-punned over the caller's buffer. */
static uint16_t hl_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t hl_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t hl_le64(const uint8_t *p)
{
    return (uint64_t)hl_le32(p) | ((uint64_t)hl_le32(p + 4) << 32);
}

/* Plain comparison, deliberately not constant-time: every value compared here
 * is a PUBLIC digest or counter, and the authenticity decision is already made
 * by the signature check that runs first. */
static int hl_eq(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i])
            return 0;
    }
    return 1;
}

/* ---- the installed authority ------------------------------------------------
 * Written once at provisioning and read-only thereafter. Not touched from an
 * interrupt, and enrollment runs single-threaded before the scheduler starts,
 * so no lock is required; the one-way install is what makes a later writer a
 * refused operation rather than a race. */
/* THREE states, not a flag, because publication order alone does not exclude a
 * second writer. Release-storing a flag after the key copy guarantees that a
 * reader seeing PRESENT sees all 32 bytes -- and guarantees nothing about two
 * callers that both observed EMPTY, interleaved their copies and both published.
 * The reader would then hold a mixed key and the documented second-install
 * refusal would never have fired. INSTALLING is the state that makes the
 * check-and-install one indivisible claim; readers demand PRESENT, so a key
 * being written is never a key that can authorize. */
#define HL_AUTH_EMPTY      0
#define HL_AUTH_INSTALLING 1
#define HL_AUTH_PRESENT    2

static uint8_t s_authority[TPM_HEADLESS_PUBKEY_LEN];
static volatile int s_authority_state;

int tpm_headless_authz_set_authority(const uint8_t pub[TPM_HEADLESS_PUBKEY_LEN])
{
    uint32_t i;
    int expect = HL_AUTH_EMPTY;

    if (!pub)
        return -1;
    /* Claim the install. Exactly one caller wins the EMPTY -> INSTALLING
     * transition; everyone else falls through to the state check below. */
    if (!__atomic_compare_exchange_n(&s_authority_state, &expect,
                                     HL_AUTH_INSTALLING, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        /* `expect` now holds the state that beat us. */
        if (expect == HL_AUTH_INSTALLING)
            return -3;              /* another install is mid-copy; retry */
        /* Re-installing the IDENTICAL key is idempotent; a different one is
         * refused, because widening who may authorize an enrollment after the
         * fact is exactly the escalation this module must not allow. The
         * acquire above orders this comparison after the winner's key copy. */
        return hl_eq(s_authority, pub, TPM_HEADLESS_PUBKEY_LEN) ? 0 : -2;
    }
    for (i = 0; i < TPM_HEADLESS_PUBKEY_LEN; i++)
        s_authority[i] = pub[i];
    /* Publish LAST, with a release store: a reader that observes PRESENT also
     * observes all 32 key bytes. */
    __atomic_store_n(&s_authority_state, HL_AUTH_PRESENT, __ATOMIC_RELEASE);
    return 0;
}

int tpm_headless_authz_authority_present(void)
{
    /* PRESENT only. Reporting INSTALLING as present would hand out a key that
     * is still being written, which is the whole reason the state exists. */
    return __atomic_load_n(&s_authority_state, __ATOMIC_ACQUIRE)
           == HL_AUTH_PRESENT ? 1 : 0;
}

/* Snapshot the authority, and TOUCH THE KEY ONLY WHEN IT IS FINISHED.
 *
 * Reporting INSTALLING as absent is not sufficient on its own: a caller that
 * then copies the buffer anyway still reads it concurrently with the winning
 * installer's writes, which is an unsynchronized race whatever verdict it goes
 * on to produce. The acquire load here orders the copy after the winner's
 * release store, and every non-PRESENT state returns without reading a byte.
 * Returns 1 and fills `out` when an authority is fully installed. */
static int hl_authority_snapshot(uint8_t out[TPM_HEADLESS_PUBKEY_LEN])
{
    uint32_t i;
    if (__atomic_load_n(&s_authority_state, __ATOMIC_ACQUIRE) != HL_AUTH_PRESENT)
        return 0;
    for (i = 0; i < TPM_HEADLESS_PUBKEY_LEN; i++)
        out[i] = s_authority[i];
    return 1;
}

#ifdef KERNEL_TESTS
void tpm_headless_authz_reset_authority_for_test(void)
{
    uint32_t i;
    for (i = 0; i < TPM_HEADLESS_PUBKEY_LEN; i++)
        s_authority[i] = 0u;
    __atomic_store_n(&s_authority_state, HL_AUTH_EMPTY, __ATOMIC_RELEASE);
}
#endif

/* ---- the pure predicate ----------------------------------------------------- */

tpm_headless_verdict_t
tpm_headless_authz_precheck(const uint8_t *blob, uint32_t blob_len,
                            uint8_t authority_present,
                            const uint8_t *authority_pub)
{
    uint32_t op;

    /* A NULL input means nothing could be read, which is the same observable
     * state as no blob at all. Fail closed either way. */
    if (!blob || blob_len == 0u)
        return TPM_HEADLESS_ABSENT;

    /* An unprovisioned machine has no escape hatch at all. Reported after the
     * absent check so an operator on a machine with no blob is told that,
     * rather than being told about a key they were never going to install. */
    if (!authority_present || !authority_pub)
        return TPM_HEADLESS_NO_AUTHORITY;

    if (blob_len != TPM_HEADLESS_BLOB_LEN)
        return TPM_HEADLESS_BAD_FORMAT;
    if (hl_le32(blob + TPM_HEADLESS_OFF_MAGIC) != TPM_HEADLESS_MAGIC)
        return TPM_HEADLESS_BAD_FORMAT;
    if (hl_le16(blob + TPM_HEADLESS_OFF_VERSION) != (uint16_t)TPM_HEADLESS_VERSION)
        return TPM_HEADLESS_BAD_FORMAT;
    /* Reserved fields are rejected rather than ignored: they are the only
     * place a future version can put a binding, and a kernel that silently
     * drops bytes it does not understand would accept a blob whose extra
     * restrictions it never applied. */
    if (hl_le16(blob + TPM_HEADLESS_OFF_RESERVED0) != 0u ||
        hl_le32(blob + TPM_HEADLESS_OFF_RESERVED1) != 0u)
        return TPM_HEADLESS_BAD_FORMAT;
    op = hl_le32(blob + TPM_HEADLESS_OFF_OPERATION);
    if (op == (uint32_t)TPM_HEADLESS_OP_NONE ||
        op > (uint32_t)TPM_HEADLESS_OP_MAX)
        return TPM_HEADLESS_BAD_FORMAT;

    /* AUTHENTICITY BEFORE BINDING. Until the signature verifies, every field
     * above is attacker-chosen, so reporting which binding failed first would
     * answer questions for a party that holds no key at all. */
    if (!ci_crypto_verify(CI_SIG_ED25519, blob, TPM_HEADLESS_SIGNED_LEN,
                          blob + TPM_HEADLESS_OFF_SIGNATURE, CI_ED25519_SIG_LEN,
                          authority_pub, CI_ED25519_PUBKEY_LEN))
        return TPM_HEADLESS_BAD_SIGNATURE;
    return TPM_HEADLESS_OK;
}

/* The bindings ONLY: device identity, transition, operation, counter. Assumes the
 * local half (format + signature) has ALREADY passed -- callers that have not
 * run it themselves must go through tpm_headless_authz_evaluate below, never
 * this directly, or a forged blob would reach a binding check unauthenticated. */
static tpm_headless_verdict_t
hl_evaluate_bindings(const struct tpm_headless_authz_inputs *in)
{
    const uint8_t *b = in->blob;
    uint32_t op = hl_le32(b + TPM_HEADLESS_OFF_OPERATION);

    /* Each binding reports whether it could not be ESTABLISHED separately from
     * whether it MISMATCHED: the first is a broken TPM and the second is the
     * wrong blob, and the operator's remedy differs. */
    if (!in->device_known)
        return TPM_HEADLESS_UNKNOWN_DEVICE;
    if (!hl_eq(b + TPM_HEADLESS_OFF_DEVICE_ID, in->device_id, TPM_HEADLESS_ID_LEN))
        return TPM_HEADLESS_WRONG_DEVICE;

    if (!in->transition_known)
        return TPM_HEADLESS_UNKNOWN_STATE;
    if (!hl_eq(b + TPM_HEADLESS_OFF_TRANSITION, in->transition_id, TPM_HEADLESS_ID_LEN))
        return TPM_HEADLESS_WRONG_TRANSITION;

    if (op != in->operation)
        return TPM_HEADLESS_WRONG_OPERATION;

    if (!in->counter_known)
        return TPM_HEADLESS_COUNTER_UNTRUSTED;
    {
        uint64_t want = hl_le64(b + TPM_HEADLESS_OFF_COUNTER);
        /* STALE_OR_SPENT rather than "replayed": the counter also moves past a
         * token whose enrollment attempt failed, so claiming a replay would
         * name a cause that did not necessarily happen. */
        if (in->counter > want)
            return TPM_HEADLESS_STALE_OR_SPENT;
        if (in->counter < want)
            return TPM_HEADLESS_NOT_YET_VALID;
    }
    return TPM_HEADLESS_OK;
}

tpm_headless_verdict_t
tpm_headless_authz_evaluate(const struct tpm_headless_authz_inputs *in)
{
    tpm_headless_verdict_t local;

    if (!in)
        return TPM_HEADLESS_ABSENT;
    /* ONE implementation of the local half, so this predicate and authorize's
     * own early-reject path can never disagree about the refusal ordering. */
    local = tpm_headless_authz_precheck(in->blob, in->blob_len,
                                        in->authority_present,
                                        in->authority_pub);
    if (local != TPM_HEADLESS_OK)
        return local;
    return hl_evaluate_bindings(in);
}

const char *tpm_headless_verdict_label(uint8_t verdict)
{
    switch (verdict) {
    case TPM_HEADLESS_OK:              return "ok";
    case TPM_HEADLESS_ABSENT:          return "absent";
    case TPM_HEADLESS_NO_AUTHORITY:    return "no-authority";
    case TPM_HEADLESS_BAD_FORMAT:      return "bad-format";
    case TPM_HEADLESS_BAD_SIGNATURE:   return "bad-signature";
    case TPM_HEADLESS_WRONG_DEVICE:    return "wrong-device";
    case TPM_HEADLESS_WRONG_TRANSITION: return "wrong-transition";
    case TPM_HEADLESS_WRONG_OPERATION: return "wrong-operation";
    case TPM_HEADLESS_STALE_OR_SPENT:  return "stale-or-spent";
    case TPM_HEADLESS_NOT_YET_VALID:   return "not-yet-valid";
    case TPM_HEADLESS_UNKNOWN_DEVICE:  return "unknown-device";
    case TPM_HEADLESS_UNKNOWN_STATE:   return "unknown-state";
    case TPM_HEADLESS_COUNTER_UNTRUSTED: return "counter-untrusted";
    case TPM_HEADLESS_CONSUME_FAILED:  return "consume-failed";
    default:                           return "unknown";
    }
}

/* ---- gathering the live inputs ---------------------------------------------- */

/* Domain separators. A bare SHA-256 over a public area could collide in
 * MEANING with any other digest the subsystem takes over similar bytes; the
 * tag makes each digest answer exactly one question. */
static const uint8_t HL_TAG_DEV[8] = { 'I','H','A','Z','D','E','V','1' };
/* There is no measured-state tag here any more. The transition digest is
 * computed by tpm_baseline_transition_id, which carries its own domain
 * separators, and it is computed there precisely so its coverage lives beside
 * the struct whose fields it covers. */

/* Device identity: SHA-256 over the EK PRIMARY public. Returns 0 on success. */
static int hl_device_id(uint8_t out[TPM_HEADLESS_ID_LEN])
{
    uint8_t pub[TPM_EK_PUB_MAX];
    uint16_t pub_len = 0;
    struct sha256_ctx ctx;
    uint8_t hdr[2];

    if (tpm_ek_public_get(pub, (uint16_t)sizeof pub, &pub_len) != TPM_ATTEST_OK)
        return -1;
    if (pub_len == 0u)
        return -1;
    hdr[0] = (uint8_t)(pub_len & 0xFFu);
    hdr[1] = (uint8_t)((pub_len >> 8) & 0xFFu);
    sha256_init(&ctx);
    sha256_update(&ctx, HL_TAG_DEV, (uint32_t)sizeof HL_TAG_DEV);
    sha256_update(&ctx, hdr, 2u);
    sha256_update(&ctx, pub, (uint32_t)pub_len);
    sha256_final(&ctx, out);
    return 0;
}

/* Read the replay counter through its ENROLLED contract.
 *
 * Never tpm_nv_read_counter(): that decodes eight bytes with no NV_ReadPublic,
 * type, attribute or Name check, so a replacement data index at the same
 * handle would hand back an attacker-chosen value. tpm_nv_verify_and_read()
 * owns the handle from the contract and verifies identity and lifecycle inside
 * the same bounded sequence as the read, so the verdict cannot expire between
 * the check and the use. */
static tpm_nv_status_t hl_counter_read(uint64_t *out)
{
    struct tpm_nv_identity id;
    uint8_t buf[TPM_NV_COUNTER_SIZE];
    uint16_t got = 0;
    tpm_nv_status_t st;
    uint32_t i;
    uint64_t v = 0;

    if (!out)
        return TPM_NV_BADARG;
    st = tpm_authz_contract(TPM_NV_INDEX_HEADLESS_SEQ, &id);
    if (st != TPM_NV_OK)
        return st;
    st = tpm_nv_verify_and_read(&id, 0u, buf, (uint16_t)sizeof buf, &got);
    if (st != TPM_NV_OK)
        return st;
    if (got != (uint16_t)TPM_NV_COUNTER_SIZE)
        return TPM_NV_TRANSPORT;
    /* An NV counter's contents are big-endian, like every other TPM value. */
    for (i = 0; i < TPM_NV_COUNTER_SIZE; i++)
        v = (v << 8) | (uint64_t)buf[i];
    *out = v;
    return TPM_NV_OK;
}

tpm_nv_status_t tpm_headless_authz_provision(void)
{
    uint64_t v = 0;
    tpm_nv_status_t st;

    /* Already readable means already provisioned. Checked FIRST so the call is
     * idempotent and never spends a gratuitous increment -- an increment is
     * irreversible, and one per boot would walk the counter away from every
     * token an operator has already signed. */
    st = hl_counter_read(&v);
    if (st == TPM_NV_OK)
        return TPM_NV_OK;
    /* Only an index that is absent or never written is ours to bootstrap. Any
     * other failure -- an identity mismatch, a recreated index, a transport
     * fault -- is a condition an authorized recovery has to look at, and
     * defining or incrementing over it would destroy the evidence. */
    if (st != TPM_NV_NOTFOUND && st != TPM_NV_UNINIT)
        return st;

    if (st == TPM_NV_NOTFOUND) {
        /* OWNERREAD|OWNERWRITE is required by tpm_nv_define_counter and is what
         * this anchor wants: the authorization is carried by the signature over
         * the counter value, not by a TPM policy. */
        st = tpm_nv_define_counter(TPM_NV_INDEX_HEADLESS_SEQ,
                                   TPMA_NV_OWNERREAD | TPMA_NV_OWNERWRITE);
        if (st != TPM_NV_OK && st != TPM_NV_DEFINED)
            return st;
    }
    /* The first increment is what sets TPMA_NV_WRITTEN and makes the value
     * readable at all. */
    st = tpm_nv_increment(TPM_NV_INDEX_HEADLESS_SEQ);
    if (st != TPM_NV_OK)
        return st;
    /* Prove it through the SAME verified path an authorization will use, so a
     * counter that provisions but cannot be verified is a failure here rather
     * than a mystery refusal on the first real token. */
    return hl_counter_read(&v);
}

int tpm_headless_authz_next_counter(uint64_t *out)
{
    return (hl_counter_read(out) == TPM_NV_OK) ? 0 : -1;
}

tpm_headless_verdict_t
tpm_headless_authz_precheck_installed(const uint8_t *blob, uint32_t blob_len)
{
    uint8_t pub[TPM_HEADLESS_PUBKEY_LEN];
    uint8_t present;

    /* ONE acquire decides both whether there is an authority and whether its
     * bytes may be read, exactly as the authorize path does. */
    present = (uint8_t)hl_authority_snapshot(pub);
    return tpm_headless_authz_precheck(blob, blob_len, present, pub);
}

tpm_headless_verdict_t
tpm_headless_enroll_prepare(const uint8_t *blob, uint32_t blob_len,
                            uint32_t nv_index, uint16_t alg,
                            struct tpm_baseline *out_cand,
                            uint8_t out_transition[TPM_HEADLESS_ID_LEN],
                            uint32_t *out_operation)
{
    struct tpm_baseline prev;
    uint32_t prev_gen = 0u;
    tpm_headless_verdict_t v;
    tpm_baseline_status_t ps;
    int have_prev = 0;

    if (!out_cand || !out_transition || !out_operation)
        return TPM_HEADLESS_ABSENT;

    /* THE LOCAL HALF FIRST, and nothing below it runs until it passes. */
    v = tpm_headless_authz_precheck_installed(blob, blob_len);
    if (v != TPM_HEADLESS_OK)
        return v;

    if (tpm_baseline_snapshot(alg, out_cand) != TPM_BASELINE_OK)
        return TPM_HEADLESS_UNKNOWN_STATE;

    ps = tpm_baseline_predecessor(nv_index, &prev, &prev_gen);
    if (ps == TPM_BASELINE_OK)
        have_prev = 1;
    else if (ps != TPM_BASELINE_NO_BASELINE)
        return TPM_HEADLESS_UNKNOWN_STATE;   /* FAIL CLOSED, never assumed absent */

    if (tpm_baseline_transition_id(have_prev ? &prev : (const struct tpm_baseline *)0,
                                   have_prev ? prev_gen : 0u,
                                   out_cand, out_transition) != 0)
        return TPM_HEADLESS_UNKNOWN_STATE;

    *out_operation = have_prev ? (uint32_t)TPM_HEADLESS_OP_ROTATE_BASELINE
                               : (uint32_t)TPM_HEADLESS_OP_ENROLL_BASELINE;
    return TPM_HEADLESS_OK;
}

tpm_baseline_status_t
tpm_headless_enroll_dispatch(uint8_t authority, int have_transition,
                             uint32_t nv_index, uint16_t alg,
                             const struct tpm_baseline *cand,
                             const uint8_t *transition_id)
{
    if (authority == (uint8_t)TPM_ENROLL_AUTH_HEADLESS_SIGNED_AUTHORIZATION) {
        /* REFUSE rather than fall back. Falling back would take a fresh
         * snapshot and store bytes nobody authorized, after the token was
         * already spent -- the exact failure this section closed. */
        if (!have_transition || !cand || !transition_id)
            return TPM_BASELINE_BADARG;
        return tpm_baseline_enroll_headless(nv_index, cand, transition_id);
    }
    return tpm_baseline_enroll(nv_index, alg);
}

tpm_headless_verdict_t tpm_headless_authz_authorize(const uint8_t *blob,
                                                    uint32_t blob_len,
                                                    uint32_t operation,
                                                    const uint8_t *transition_id)
{
    struct tpm_headless_authz_inputs in;
    tpm_headless_verdict_t v;
    uint64_t counter = 0;
    uint32_t i;

    for (i = 0; i < (uint32_t)sizeof in; i++)
        ((uint8_t *)&in)[i] = 0u;
    in.blob = blob;
    in.blob_len = blob_len;
    in.operation = operation;
    /* ONE acquire decides both whether there is an authority and whether its
     * bytes may be read; the key is never touched unless it is finished. */
    in.authority_present = (uint8_t)hl_authority_snapshot(in.authority_pub);

    /* EVERY LOCAL CHECK RUNS BEFORE THE FIRST TPM TRANSACTION, and that
     * ordering is the point rather than an optimization. The blob is a file an
     * attacker can write beside the loader, so gathering the device identity
     * and the counter first would let anyone force a CreatePrimary and two NV
     * operations on every boot with bytes they invented. After this, only a
     * blob genuinely signed by the installed authority costs the TPM anything
     * at all. */
    v = tpm_headless_authz_precheck(in.blob, in.blob_len, in.authority_present,
                                    in.authority_pub);
    if (v != TPM_HEADLESS_OK)
        return v;

    if (hl_device_id(in.device_id) == 0)
        in.device_known = 1u;
    /* The transition digest comes FROM THE CALLER and is never re-derived
     * here. Deriving it would mean taking a second tpm_baseline_snapshot, and
     * the bytes this function authorizes would then be a different snapshot
     * from the bytes the enrollment writes -- an honestly authorized
     * enrollment writing content nobody approved. A NULL digest leaves
     * transition_known clear, which refuses UNKNOWN_STATE. */
    if (transition_id) {
        uint32_t k;
        for (k = 0; k < (uint32_t)TPM_HEADLESS_ID_LEN; k++)
            in.transition_id[k] = transition_id[k];
        in.transition_known = 1u;
    }
    if (hl_counter_read(&counter) == TPM_NV_OK) {
        in.counter = counter;
        in.counter_known = 1u;
    }

    /* hl_evaluate_bindings, NOT tpm_headless_authz_evaluate: the precheck two
     * lines up already verified the signature, and evaluate() would run the
     * identical Ed25519 check a second time for every accepting boot. */
    v = hl_evaluate_bindings(&in);
    if (v != TPM_HEADLESS_OK)
        return v;

    /* CONSUME BEFORE THE CALLER MUTATES ANYTHING. An increment that fails, or
     * whose completion is unknown because the transport gave up mid-command,
     * is SPENT: the TPM may well have executed it, and re-offering the token on
     * that assumption is precisely the replay this module exists to prevent.
     * The token authorizes one ATTEMPT, so there is no retry here. */
    if (tpm_nv_increment(TPM_NV_INDEX_HEADLESS_SEQ) != TPM_NV_OK)
        return TPM_HEADLESS_CONSUME_FAILED;
    return TPM_HEADLESS_OK;
}
