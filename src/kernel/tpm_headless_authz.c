/* tpm_headless_authz.c -- one-shot signed authorization for headless enrollment.
 *
 * The security posture, the reason the device identity is the EK rather than
 * the AK, and why the token is consumed before the mutation rather than after
 * it, all live in the header banner. This file is the mechanism.
 */
#include "kernel/tpm_headless_authz.h"
#include "kernel/tpm.h"
#include "kernel/tpm_attest.h"
#include "kernel/tpm_authz.h"
#include "kernel/tpm_baseline.h"
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
static uint8_t s_authority[TPM_HEADLESS_PUBKEY_LEN];
static uint8_t s_authority_present;

int tpm_headless_authz_set_authority(const uint8_t pub[TPM_HEADLESS_PUBKEY_LEN])
{
    uint32_t i;
    if (!pub)
        return -1;
    if (s_authority_present) {
        /* Re-installing the IDENTICAL key is idempotent; a different one is
         * refused, because widening who may authorize an enrollment after the
         * fact is exactly the escalation this module must not allow. */
        return hl_eq(s_authority, pub, TPM_HEADLESS_PUBKEY_LEN) ? 0 : -2;
    }
    for (i = 0; i < TPM_HEADLESS_PUBKEY_LEN; i++)
        s_authority[i] = pub[i];
    s_authority_present = 1u;
    return 0;
}

int tpm_headless_authz_authority_present(void)
{
    return s_authority_present ? 1 : 0;
}

#ifdef KERNEL_TESTS
void tpm_headless_authz_reset_authority_for_test(void)
{
    uint32_t i;
    for (i = 0; i < TPM_HEADLESS_PUBKEY_LEN; i++)
        s_authority[i] = 0u;
    s_authority_present = 0u;
}
#endif

/* ---- the pure predicate ----------------------------------------------------- */

tpm_headless_verdict_t
tpm_headless_authz_evaluate(const struct tpm_headless_authz_inputs *in)
{
    const uint8_t *b;
    uint32_t op;

    /* A NULL input struct means nothing could be read, which is the same
     * observable state as no blob at all. Fail closed either way. */
    if (!in || !in->blob || in->blob_len == 0u)
        return TPM_HEADLESS_ABSENT;

    /* An unprovisioned machine has no escape hatch at all. Reported after the
     * absent check so an operator on a machine with no blob is told that,
     * rather than being told about a key they were never going to install. */
    if (!in->authority_present)
        return TPM_HEADLESS_NO_AUTHORITY;

    b = in->blob;
    if (in->blob_len != TPM_HEADLESS_BLOB_LEN)
        return TPM_HEADLESS_BAD_FORMAT;
    if (hl_le32(b + 0) != TPM_HEADLESS_MAGIC)
        return TPM_HEADLESS_BAD_FORMAT;
    if (hl_le16(b + 4) != (uint16_t)TPM_HEADLESS_VERSION)
        return TPM_HEADLESS_BAD_FORMAT;
    /* Reserved fields are rejected rather than ignored: they are the only
     * place a future version can put a binding, and a kernel that silently
     * drops bytes it does not understand would accept a blob whose extra
     * restrictions it never applied. */
    if (hl_le16(b + 6) != 0u || hl_le32(b + 12) != 0u)
        return TPM_HEADLESS_BAD_FORMAT;
    op = hl_le32(b + 8);
    if (op == (uint32_t)TPM_HEADLESS_OP_NONE ||
        op > (uint32_t)TPM_HEADLESS_OP_MAX)
        return TPM_HEADLESS_BAD_FORMAT;

    /* AUTHENTICITY BEFORE BINDING. Until the signature verifies, every field
     * above is attacker-chosen, so reporting which binding failed first would
     * answer questions for a party that holds no key at all. */
    if (!ci_crypto_verify(CI_SIG_ED25519, b, TPM_HEADLESS_SIGNED_LEN,
                          b + TPM_HEADLESS_SIGNED_LEN, CI_ED25519_SIG_LEN,
                          in->authority_pub, CI_ED25519_PUBKEY_LEN))
        return TPM_HEADLESS_BAD_SIGNATURE;

    /* Each binding reports whether it could not be ESTABLISHED separately from
     * whether it MISMATCHED: the first is a broken TPM and the second is the
     * wrong blob, and the operator's remedy differs. */
    if (!in->device_known)
        return TPM_HEADLESS_UNKNOWN_DEVICE;
    if (!hl_eq(b + 16, in->device_id, TPM_HEADLESS_ID_LEN))
        return TPM_HEADLESS_WRONG_DEVICE;

    if (!in->pcr_set_known)
        return TPM_HEADLESS_UNKNOWN_STATE;
    if (!hl_eq(b + 48, in->pcr_set, TPM_HEADLESS_ID_LEN))
        return TPM_HEADLESS_WRONG_PCR_SET;

    if (op != in->operation)
        return TPM_HEADLESS_WRONG_OPERATION;

    if (!in->counter_known)
        return TPM_HEADLESS_COUNTER_UNTRUSTED;
    {
        uint64_t want = hl_le64(b + 80);
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

const char *tpm_headless_verdict_label(uint8_t verdict)
{
    switch (verdict) {
    case TPM_HEADLESS_OK:              return "ok";
    case TPM_HEADLESS_ABSENT:          return "absent";
    case TPM_HEADLESS_NO_AUTHORITY:    return "no-authority";
    case TPM_HEADLESS_BAD_FORMAT:      return "bad-format";
    case TPM_HEADLESS_BAD_SIGNATURE:   return "bad-signature";
    case TPM_HEADLESS_WRONG_DEVICE:    return "wrong-device";
    case TPM_HEADLESS_WRONG_PCR_SET:   return "wrong-pcr-set";
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
static const uint8_t HL_TAG_PCR[8] = { 'I','H','A','Z','P','C','R','1' };

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

/* Measured state: SHA-256 over a canonical, FIXED-SIZE serialization of the
 * current PCR set. Fixed size on purpose -- a variable-length encoding over
 * present slots would let two different PCR sets serialize identically.
 *
 * This covers the PCR SET only. The non-PCR fields of struct tpm_baseline
 * (firmware-version hash, ABI-manifest digest, Secure Boot state) are NOT
 * covered here: binding the authorization to the whole enrolled record is a
 * separate construction with the opposite failure mode, owned by the headless
 * authorization transport and full-record binding work, which builds the
 * candidate baseline once and binds a digest over every security-relevant
 * field of it. */
static int hl_pcr_set_digest(uint8_t out[TPM_HEADLESS_ID_LEN])
{
    struct tpm_baseline b;
    struct sha256_ctx ctx;
    uint8_t hdr[3];
    uint32_t i;

    if (tpm_baseline_snapshot(TPM_ALG_SHA256, &b) != TPM_BASELINE_OK)
        return -1;
    hdr[0] = (uint8_t)(b.alg & 0xFFu);
    hdr[1] = (uint8_t)((b.alg >> 8) & 0xFFu);
    hdr[2] = b.pcr_count;
    sha256_init(&ctx);
    sha256_update(&ctx, HL_TAG_PCR, (uint32_t)sizeof HL_TAG_PCR);
    sha256_update(&ctx, hdr, 3u);
    for (i = 0; i < TPM_BASELINE_MAX_PCRS; i++) {
        uint8_t slot[2];
        static const uint8_t zero[TPM_BASELINE_DIGEST] = { 0 };
        slot[0] = b.pcrs[i].index;
        slot[1] = b.pcrs[i].present;
        sha256_update(&ctx, slot, 2u);
        /* An absent slot contributes zeros rather than stale bytes, so the
         * digest depends on what was MEASURED and not on what the snapshot
         * happened to leave in an unused entry. */
        sha256_update(&ctx, b.pcrs[i].present ? b.pcrs[i].digest : zero,
                      (uint32_t)TPM_BASELINE_DIGEST);
    }
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

tpm_headless_verdict_t tpm_headless_authz_authorize(const uint8_t *blob,
                                                    uint32_t blob_len,
                                                    uint32_t operation)
{
    struct tpm_headless_authz_inputs in;
    tpm_headless_verdict_t v;
    uint64_t counter = 0;
    uint32_t i;

    /* Cheapest checks first, and none of them touch the TPM: a boot presenting
     * no blob on a machine with no authority must not spend a transaction. */
    for (i = 0; i < (uint32_t)sizeof in; i++)
        ((uint8_t *)&in)[i] = 0u;
    in.blob = blob;
    in.blob_len = blob_len;
    in.operation = operation;
    in.authority_present = s_authority_present;
    for (i = 0; i < TPM_HEADLESS_PUBKEY_LEN; i++)
        in.authority_pub[i] = s_authority[i];
    if (!blob || blob_len == 0u)
        return TPM_HEADLESS_ABSENT;
    if (!s_authority_present)
        return TPM_HEADLESS_NO_AUTHORITY;

    if (hl_device_id(in.device_id) == 0)
        in.device_known = 1u;
    if (hl_pcr_set_digest(in.pcr_set) == 0)
        in.pcr_set_known = 1u;
    if (hl_counter_read(&counter) == TPM_NV_OK) {
        in.counter = counter;
        in.counter_known = 1u;
    }

    v = tpm_headless_authz_evaluate(&in);
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
