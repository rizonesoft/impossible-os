/* test_tpm_headless_authz.c -- the headless enrollment authorization matrix.
 *
 * Every refusal is asserted BESIDE a passing control built from the same
 * fixture, because a refusal assertion on its own passes just as happily
 * against a verifier that refuses everything -- which is the failure mode a
 * fail-closed security predicate is most likely to have.
 *
 * The signatures are real Ed25519, produced here with the same Monocypher the
 * kernel verifies with, so the accept path exercises ci_crypto_verify rather
 * than a stubbed-out yes.
 */
#include "kernel/test/test.h"
#include "kernel/tpm_headless_authz.h"
#include "kernel/tpm_enroll_gate.h"
#include "kernel/crypto/sha256.h"
#include "libs/monocypher/monocypher-ed25519.h"
#include "libc/string.h"

/* ---- fixture ---------------------------------------------------------------- */

#define HA_COUNTER 7u

static uint8_t g_pub[TPM_HEADLESS_PUBKEY_LEN];
static uint8_t g_secret[64];
static uint8_t g_device[TPM_HEADLESS_ID_LEN];
static uint8_t g_pcrs[TPM_HEADLESS_ID_LEN];

static void ha_put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void ha_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void ha_put64(uint8_t *p, uint64_t v)
{
    ha_put32(p, (uint32_t)(v & 0xFFFFFFFFu));
    ha_put32(p + 4, (uint32_t)(v >> 32));
}

static void ha_fill(uint8_t *p, uint32_t n, uint8_t seed)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        p[i] = (uint8_t)(seed + i);
}

/* Derive the fixture's key pair from a fixed seed so every run is identical.
 * crypto_ed25519_key_pair() WIPES its seed in place, so the seed is a mutable
 * local rather than a const. */
static void ha_fixture_init(void)
{
    uint8_t seed[32];
    ha_fill(seed, sizeof seed, 0x11u);
    crypto_ed25519_key_pair(g_secret, g_pub, seed);
    ha_fill(g_device, sizeof g_device, 0x40u);
    ha_fill(g_pcrs, sizeof g_pcrs, 0x80u);
}

/* Build a well-formed blob for `op` at `counter`, signed by the fixture key. */
static void ha_build(uint8_t blob[TPM_HEADLESS_BLOB_LEN], uint32_t op,
                     uint64_t counter)
{
    uint32_t i;
    for (i = 0; i < TPM_HEADLESS_BLOB_LEN; i++)
        blob[i] = 0u;
    ha_put32(blob + 0, TPM_HEADLESS_MAGIC);
    ha_put16(blob + 4, (uint16_t)TPM_HEADLESS_VERSION);
    ha_put16(blob + 6, 0u);
    ha_put32(blob + 8, op);
    ha_put32(blob + 12, 0u);
    for (i = 0; i < TPM_HEADLESS_ID_LEN; i++) blob[16 + i] = g_device[i];
    for (i = 0; i < TPM_HEADLESS_ID_LEN; i++) blob[48 + i] = g_pcrs[i];
    ha_put64(blob + 80, counter);
    crypto_ed25519_sign(blob + TPM_HEADLESS_SIGNED_LEN, g_secret,
                        blob, TPM_HEADLESS_SIGNED_LEN);
}

/* Inputs whose every live fact matches the fixture. Each test perturbs exactly
 * one field, so a failure names the binding that broke. */
static void ha_inputs(struct tpm_headless_authz_inputs *in,
                      const uint8_t *blob, uint32_t op)
{
    uint32_t i;
    for (i = 0; i < (uint32_t)sizeof *in; i++)
        ((uint8_t *)in)[i] = 0u;
    in->blob = blob;
    in->blob_len = TPM_HEADLESS_BLOB_LEN;
    in->authority_present = 1u;
    in->device_known = 1u;
    in->pcr_set_known = 1u;
    in->counter_known = 1u;
    in->counter = HA_COUNTER;
    in->operation = op;
    for (i = 0; i < TPM_HEADLESS_PUBKEY_LEN; i++) in->authority_pub[i] = g_pub[i];
    for (i = 0; i < TPM_HEADLESS_ID_LEN; i++) in->device_id[i] = g_device[i];
    for (i = 0; i < TPM_HEADLESS_ID_LEN; i++) in->pcr_set[i] = g_pcrs[i];
}

/* ---- the control: a correct authorization is ACCEPTED ----------------------- */

static void test_ha_accept(void)
{
    uint8_t blob[TPM_HEADLESS_BLOB_LEN];
    struct tpm_headless_authz_inputs in;

    ha_fixture_init();
    ha_build(blob, TPM_HEADLESS_OP_ENROLL_BASELINE, HA_COUNTER);
    ha_inputs(&in, blob, TPM_HEADLESS_OP_ENROLL_BASELINE);
    TEST_ASSERT_EQ(tpm_headless_authz_evaluate(&in), TPM_HEADLESS_OK,
                   "a correctly signed and bound authorization is accepted");
}

/* ---- the five bindings, each refused DISTINCTLY ----------------------------- */

static void test_ha_wrong_device(void)
{
    uint8_t blob[TPM_HEADLESS_BLOB_LEN];
    struct tpm_headless_authz_inputs in;

    ha_fixture_init();
    ha_build(blob, TPM_HEADLESS_OP_ENROLL_BASELINE, HA_COUNTER);
    ha_inputs(&in, blob, TPM_HEADLESS_OP_ENROLL_BASELINE);
    in.device_id[0] ^= 0xFFu;      /* the machine, not the blob, is different */
    TEST_ASSERT_EQ(tpm_headless_authz_evaluate(&in), TPM_HEADLESS_WRONG_DEVICE,
                   "a blob signed for another EK identity is wrong-device");
}

static void test_ha_wrong_pcr_set(void)
{
    uint8_t blob[TPM_HEADLESS_BLOB_LEN];
    struct tpm_headless_authz_inputs in;

    ha_fixture_init();
    ha_build(blob, TPM_HEADLESS_OP_ENROLL_BASELINE, HA_COUNTER);
    ha_inputs(&in, blob, TPM_HEADLESS_OP_ENROLL_BASELINE);
    in.pcr_set[31] ^= 0x01u;
    TEST_ASSERT_EQ(tpm_headless_authz_evaluate(&in), TPM_HEADLESS_WRONG_PCR_SET,
                   "a blob signed for another measured state is wrong-pcr-set");
}

static void test_ha_wrong_operation(void)
{
    uint8_t blob[TPM_HEADLESS_BLOB_LEN];
    struct tpm_headless_authz_inputs in;

    ha_fixture_init();
    /* Signed for a ROTATE, presented against an ENROLL request. */
    ha_build(blob, TPM_HEADLESS_OP_ROTATE_BASELINE, HA_COUNTER);
    ha_inputs(&in, blob, TPM_HEADLESS_OP_ENROLL_BASELINE);
    TEST_ASSERT_EQ(tpm_headless_authz_evaluate(&in), TPM_HEADLESS_WRONG_OPERATION,
                   "a rotation token does not authorize an enrollment");

    /* Control: the same bytes DO authorize the operation they name. */
    ha_inputs(&in, blob, TPM_HEADLESS_OP_ROTATE_BASELINE);
    TEST_ASSERT_EQ(tpm_headless_authz_evaluate(&in), TPM_HEADLESS_OK,
                   "the same token authorizes the operation it was signed for");
}

static void test_ha_stale_or_spent(void)
{
    uint8_t blob[TPM_HEADLESS_BLOB_LEN];
    struct tpm_headless_authz_inputs in;

    ha_fixture_init();
    ha_build(blob, TPM_HEADLESS_OP_ENROLL_BASELINE, HA_COUNTER);
    ha_inputs(&in, blob, TPM_HEADLESS_OP_ENROLL_BASELINE);
    /* THE REPLAY ASSERTION. The counter advanced past the value the blob names,
     * which is exactly the state a consumed token leaves behind. */
    in.counter = (uint64_t)HA_COUNTER + 1u;
    TEST_ASSERT_EQ(tpm_headless_authz_evaluate(&in), TPM_HEADLESS_STALE_OR_SPENT,
                   "the identical bytes are refused once the counter moved past");

    in.counter = HA_COUNTER;
    TEST_ASSERT_EQ(tpm_headless_authz_evaluate(&in), TPM_HEADLESS_OK,
                   "control: at the exact counter value the same bytes accept");
}

static void test_ha_not_yet_valid(void)
{
    uint8_t blob[TPM_HEADLESS_BLOB_LEN];
    struct tpm_headless_authz_inputs in;

    ha_fixture_init();
    ha_build(blob, TPM_HEADLESS_OP_ENROLL_BASELINE, HA_COUNTER);
    ha_inputs(&in, blob, TPM_HEADLESS_OP_ENROLL_BASELINE);
    in.counter = (uint64_t)HA_COUNTER - 1u;
    TEST_ASSERT_EQ(tpm_headless_authz_evaluate(&in), TPM_HEADLESS_NOT_YET_VALID,
                   "a token signed for a slot not yet reached is not-yet-valid");
}

/* ---- authenticity and shape ------------------------------------------------- */

static void test_ha_bad_signature(void)
{
    uint8_t blob[TPM_HEADLESS_BLOB_LEN];
    struct tpm_headless_authz_inputs in;

    ha_fixture_init();
    ha_build(blob, TPM_HEADLESS_OP_ENROLL_BASELINE, HA_COUNTER);
    blob[TPM_HEADLESS_SIGNED_LEN] ^= 0x01u;      /* one bit of the signature */
    ha_inputs(&in, blob, TPM_HEADLESS_OP_ENROLL_BASELINE);
    TEST_ASSERT_EQ(tpm_headless_authz_evaluate(&in), TPM_HEADLESS_BAD_SIGNATURE,
                   "a one-bit signature change is refused");

    /* A signed field edited after signing fails the same way -- proving the
     * signature covers the bindings and not merely the header. */
    ha_build(blob, TPM_HEADLESS_OP_ENROLL_BASELINE, HA_COUNTER);
    blob[80] ^= 0x01u;                            /* valid_at_counter */
    TEST_ASSERT_EQ(tpm_headless_authz_evaluate(&in), TPM_HEADLESS_BAD_SIGNATURE,
                   "editing the bound counter after signing is refused");
}

static void test_ha_wrong_authority(void)
{
    uint8_t blob[TPM_HEADLESS_BLOB_LEN];
    struct tpm_headless_authz_inputs in;
    uint8_t other_pub[TPM_HEADLESS_PUBKEY_LEN], other_secret[64], seed[32];
    uint32_t i;

    ha_fixture_init();
    ha_build(blob, TPM_HEADLESS_OP_ENROLL_BASELINE, HA_COUNTER);
    ha_inputs(&in, blob, TPM_HEADLESS_OP_ENROLL_BASELINE);

    ha_fill(seed, sizeof seed, 0x99u);
    crypto_ed25519_key_pair(other_secret, other_pub, seed);
    for (i = 0; i < TPM_HEADLESS_PUBKEY_LEN; i++)
        in.authority_pub[i] = other_pub[i];
    TEST_ASSERT_EQ(tpm_headless_authz_evaluate(&in), TPM_HEADLESS_BAD_SIGNATURE,
                   "a token from a different signer is refused");
}

static void test_ha_bad_format(void)
{
    uint8_t blob[TPM_HEADLESS_BLOB_LEN];
    struct tpm_headless_authz_inputs in;

    ha_fixture_init();
    ha_build(blob, TPM_HEADLESS_OP_ENROLL_BASELINE, HA_COUNTER);
    ha_inputs(&in, blob, TPM_HEADLESS_OP_ENROLL_BASELINE);

    in.blob_len = TPM_HEADLESS_BLOB_LEN - 1u;
    TEST_ASSERT_EQ(tpm_headless_authz_evaluate(&in), TPM_HEADLESS_BAD_FORMAT,
                   "a short blob is bad-format, never a partial parse");
    in.blob_len = TPM_HEADLESS_BLOB_LEN;

    blob[0] ^= 0xFFu;
    TEST_ASSERT_EQ(tpm_headless_authz_evaluate(&in), TPM_HEADLESS_BAD_FORMAT,
                   "a wrong magic is bad-format");
    ha_build(blob, TPM_HEADLESS_OP_ENROLL_BASELINE, HA_COUNTER);

    ha_put16(blob + 4, (uint16_t)(TPM_HEADLESS_VERSION + 1u));
    TEST_ASSERT_EQ(tpm_headless_authz_evaluate(&in), TPM_HEADLESS_BAD_FORMAT,
                   "an unknown version is refused, not best-effort parsed");
    ha_build(blob, TPM_HEADLESS_OP_ENROLL_BASELINE, HA_COUNTER);

    /* Reserved bytes are REJECTED rather than ignored: they are where a future
     * version puts a binding, and silently dropping them would accept a blob
     * whose extra restrictions this kernel never applied. */
    ha_put16(blob + 6, 1u);
    TEST_ASSERT_EQ(tpm_headless_authz_evaluate(&in), TPM_HEADLESS_BAD_FORMAT,
                   "a non-zero reserved0 is refused");
    ha_build(blob, TPM_HEADLESS_OP_ENROLL_BASELINE, HA_COUNTER);

    ha_put32(blob + 12, 1u);
    TEST_ASSERT_EQ(tpm_headless_authz_evaluate(&in), TPM_HEADLESS_BAD_FORMAT,
                   "a non-zero reserved1 is refused");
    ha_build(blob, TPM_HEADLESS_OP_ENROLL_BASELINE, HA_COUNTER);

    ha_put32(blob + 8, (uint32_t)TPM_HEADLESS_OP_MAX + 1u);
    TEST_ASSERT_EQ(tpm_headless_authz_evaluate(&in), TPM_HEADLESS_BAD_FORMAT,
                   "an out-of-range operation is refused");

    ha_build(blob, TPM_HEADLESS_OP_ENROLL_BASELINE, HA_COUNTER);
    TEST_ASSERT_EQ(tpm_headless_authz_evaluate(&in), TPM_HEADLESS_OK,
                   "control: the rebuilt blob still accepts");
}

/* ---- fail-closed defaults --------------------------------------------------- */

static void test_ha_fail_closed(void)
{
    uint8_t blob[TPM_HEADLESS_BLOB_LEN];
    struct tpm_headless_authz_inputs in;
    uint32_t i;

    ha_fixture_init();
    ha_build(blob, TPM_HEADLESS_OP_ENROLL_BASELINE, HA_COUNTER);

    TEST_ASSERT_EQ(tpm_headless_authz_evaluate(0), TPM_HEADLESS_ABSENT,
                   "a NULL input struct is a refusal, never a fault");

    ha_inputs(&in, blob, TPM_HEADLESS_OP_ENROLL_BASELINE);
    in.blob = 0;
    TEST_ASSERT_EQ(tpm_headless_authz_evaluate(&in), TPM_HEADLESS_ABSENT,
                   "presenting no blob is absent");

    ha_inputs(&in, blob, TPM_HEADLESS_OP_ENROLL_BASELINE);
    in.authority_present = 0u;
    TEST_ASSERT_EQ(tpm_headless_authz_evaluate(&in), TPM_HEADLESS_NO_AUTHORITY,
                   "an unprovisioned machine has no escape hatch at all");

    /* A ZEROED inputs struct must never authorize. This is the whole reason
     * TPM_HEADLESS_ABSENT is the zero verdict. */
    for (i = 0; i < (uint32_t)sizeof in; i++)
        ((uint8_t *)&in)[i] = 0u;
    TEST_ASSERT_EQ(tpm_headless_authz_evaluate(&in), TPM_HEADLESS_ABSENT,
                   "a zeroed inputs struct refuses");
    TEST_ASSERT_EQ((int)TPM_HEADLESS_ABSENT, 0,
                   "ABSENT must be the zero verdict so a zeroed struct is safe");
}

static void test_ha_unknown_facts(void)
{
    uint8_t blob[TPM_HEADLESS_BLOB_LEN];
    struct tpm_headless_authz_inputs in;

    ha_fixture_init();
    ha_build(blob, TPM_HEADLESS_OP_ENROLL_BASELINE, HA_COUNTER);

    /* "Could not establish" is reported separately from "did not match": the
     * first is a broken TPM and the second is the wrong blob. */
    ha_inputs(&in, blob, TPM_HEADLESS_OP_ENROLL_BASELINE);
    in.device_known = 0u;
    TEST_ASSERT_EQ(tpm_headless_authz_evaluate(&in), TPM_HEADLESS_UNKNOWN_DEVICE,
                   "an unreadable device identity is unknown, not a mismatch");

    ha_inputs(&in, blob, TPM_HEADLESS_OP_ENROLL_BASELINE);
    in.pcr_set_known = 0u;
    TEST_ASSERT_EQ(tpm_headless_authz_evaluate(&in), TPM_HEADLESS_UNKNOWN_STATE,
                   "an unreadable measured state is unknown, not a mismatch");

    ha_inputs(&in, blob, TPM_HEADLESS_OP_ENROLL_BASELINE);
    in.counter_known = 0u;
    TEST_ASSERT_EQ(tpm_headless_authz_evaluate(&in),
                   TPM_HEADLESS_COUNTER_UNTRUSTED,
                   "a counter that failed identity verification never admits");
}

/* ---- provisioning ----------------------------------------------------------- */

static void test_ha_authority_install(void)
{
    uint8_t a[TPM_HEADLESS_PUBKEY_LEN], b[TPM_HEADLESS_PUBKEY_LEN];

    ha_fill(a, sizeof a, 0x20u);
    ha_fill(b, sizeof b, 0x60u);

    tpm_headless_authz_reset_authority_for_test();
    TEST_ASSERT_EQ(tpm_headless_authz_authority_present(), 0,
                   "no authority is installed by default");
    TEST_ASSERT_EQ(tpm_headless_authz_set_authority(0), -1,
                   "a NULL key is refused");
    TEST_ASSERT_EQ(tpm_headless_authz_set_authority(a), 0,
                   "the first install succeeds");
    TEST_ASSERT_EQ(tpm_headless_authz_authority_present(), 1,
                   "the authority is reported present after install");
    TEST_ASSERT_EQ(tpm_headless_authz_set_authority(a), 0,
                   "re-installing the identical key is idempotent");
    /* THE ESCALATION THIS BLOCKS: a later caller widening who may authorize. */
    TEST_ASSERT_EQ(tpm_headless_authz_set_authority(b), -2,
                   "installing a DIFFERENT authority afterwards is refused");
    tpm_headless_authz_reset_authority_for_test();
}

/* ---- the enrollment-gate integration ---------------------------------------- */

/* Inputs with every non-operator condition satisfied, so the gate reaches its
 * confirm switch. Mirrors the admitting tuple the trusted-enrollment-provenance
 * suite already uses in test_tpm_enroll_gate.c. */
static void ha_gate_inputs(struct tpm_enroll_gate_inputs *gi)
{
    uint32_t i;
    for (i = 0; i < (uint32_t)sizeof *gi; i++)
        ((uint8_t *)gi)[i] = 0u;
    gi->tpm_enroll = 1u;
    gi->secure_boot_enabled = 1u;
    gi->whole_chain_verified = 1u;
    gi->replay_known = 1u;
    gi->replay_verdict = 0u;              /* TPM_REPLAY_VERIFIED */
    gi->sticky_present = 1u;
    gi->sticky_recovery_trigger = 1u;
    gi->confirm = TPM_CONFIRM_UNAVAILABLE;
}

static void test_ha_gate_admits_and_reports(void)
{
    struct tpm_enroll_gate_inputs gi;
    struct tpm_enroll_gate_result gr;

    /* The unchanged trusted-enrollment-provenance behaviour: no console, no
     * token, still refused. */
    ha_gate_inputs(&gi);
    gi.headless_authz = (uint8_t)TPM_HEADLESS_ABSENT;
    tpm_enroll_gate_evaluate(&gi, &gr);
    TEST_ASSERT_EQ(gr.admit, 0,
                   "a headless boot presenting no token is still refused");
    TEST_ASSERT_EQ(gr.refusal, TPM_ENROLL_REFUSE_CONFIRM_UNAVAILABLE,
                   "and the verdict is unchanged from before this existed");

    /* A consumed token admits, and is reported as ITS OWN authority. */
    ha_gate_inputs(&gi);
    gi.headless_authz = (uint8_t)TPM_HEADLESS_OK;
    tpm_enroll_gate_evaluate(&gi, &gr);
    TEST_ASSERT_EQ(gr.admit, 1, "a verified consumed token admits enrollment");
    TEST_ASSERT_EQ(gr.authority,
                   TPM_ENROLL_AUTH_HEADLESS_SIGNED_AUTHORIZATION,
                   "the headless authority is never reported as a console one");

    /* A token that was presented and did not hold reports its own refusal, so
     * the operator reissues the blob instead of attaching a keyboard. */
    ha_gate_inputs(&gi);
    gi.headless_authz = (uint8_t)TPM_HEADLESS_STALE_OR_SPENT;
    tpm_enroll_gate_evaluate(&gi, &gr);
    TEST_ASSERT_EQ(gr.admit, 0, "a spent token does not admit");
    TEST_ASSERT_EQ(gr.refusal, TPM_ENROLL_REFUSE_HEADLESS_AUTHZ,
                   "a failed token is distinguished from having none");
}

static void test_ha_gate_never_substitutes(void)
{
    struct tpm_enroll_gate_inputs gi;
    struct tpm_enroll_gate_result gr;

    /* A valid token must not paper over ANY earlier condition. Secure Boot off
     * with a good token is still boot-chain-untrusted, not an admission. */
    ha_gate_inputs(&gi);
    gi.headless_authz = (uint8_t)TPM_HEADLESS_OK;
    gi.secure_boot_enabled = 0u;
    tpm_enroll_gate_evaluate(&gi, &gr);
    TEST_ASSERT_EQ(gr.admit, 0, "a token does not substitute for Secure Boot");
    TEST_ASSERT_EQ(gr.refusal, TPM_ENROLL_REFUSE_BOOT_CHAIN_UNTRUSTED,
                   "the earlier refusal still fires first");

    ha_gate_inputs(&gi);
    gi.headless_authz = (uint8_t)TPM_HEADLESS_OK;
    gi.whole_chain_verified = 0u;
    tpm_enroll_gate_evaluate(&gi, &gr);
    TEST_ASSERT_EQ(gr.refusal, TPM_ENROLL_REFUSE_KERNEL_UNVERIFIED,
                   "a token does not substitute for a verified kernel");

    ha_gate_inputs(&gi);
    gi.headless_authz = (uint8_t)TPM_HEADLESS_OK;
    gi.sticky_recovery_trigger = 0u;
    tpm_enroll_gate_evaluate(&gi, &gr);
    TEST_ASSERT_EQ(gr.refusal, TPM_ENROLL_REFUSE_STICKY_TRIGGER_CLEAR,
                   "a token does not substitute for the NVRAM recovery ladder");
}

static void test_ha_gate_console_untouched(void)
{
    struct tpm_enroll_gate_inputs gi;
    struct tpm_enroll_gate_result gr;

    /* THE CONTROL FOR THE WHOLE FEATURE: a console machine behaves exactly as
     * it did, whatever the headless field happens to hold. */
    ha_gate_inputs(&gi);
    gi.confirm = TPM_CONFIRM_YES;
    gi.headless_authz = (uint8_t)TPM_HEADLESS_ABSENT;
    tpm_enroll_gate_evaluate(&gi, &gr);
    TEST_ASSERT_EQ(gr.admit, 1, "a keypress still admits");
    TEST_ASSERT_EQ(gr.authority,
                   TPM_ENROLL_AUTH_LOCAL_CONSOLE_ON_TRUSTED_CHAIN,
                   "and is still reported as the console authority");

    /* A declining operator is not overridden by a valid token: the console
     * answered, so the headless path is never reached. */
    ha_gate_inputs(&gi);
    gi.confirm = TPM_CONFIRM_WRONG_KEY;
    gi.headless_authz = (uint8_t)TPM_HEADLESS_OK;
    tpm_enroll_gate_evaluate(&gi, &gr);
    TEST_ASSERT_EQ(gr.admit, 0, "a token cannot override an operator's decline");
    TEST_ASSERT_EQ(gr.refusal, TPM_ENROLL_REFUSE_CONFIRM_WRONG_KEY,
                   "the decline is what gets reported");

    ha_gate_inputs(&gi);
    gi.confirm = TPM_CONFIRM_TIMEOUT;
    gi.headless_authz = (uint8_t)TPM_HEADLESS_OK;
    tpm_enroll_gate_evaluate(&gi, &gr);
    TEST_ASSERT_EQ(gr.admit, 0, "a token cannot override a console timeout");
}

/* ---- labels ----------------------------------------------------------------- */

static void test_ha_labels(void)
{
    uint8_t v;
    /* Every verdict has its own label: a log that reported two states with one
     * string would send an operator down the wrong remedy. */
    for (v = 0; v <= (uint8_t)TPM_HEADLESS_VERDICT_MAX; v++) {
        const char *s = tpm_headless_verdict_label(v);
        TEST_ASSERT(s != 0, "every verdict has a label");
        TEST_ASSERT(s[0] != '\0', "no verdict label is empty");
        /* Full-string compare, NOT a prefix: "unknown-device" and
         * "unknown-state" are legitimate labels that share the first four
         * characters with the fallthrough, and a prefix test would call them
         * failures. */
        TEST_ASSERT_NEQ(strcmp(s, "unknown"), 0,
                        "no in-range verdict falls through to unknown");
    }
    TEST_ASSERT_EQ(strcmp(tpm_headless_verdict_label(
                              (uint8_t)(TPM_HEADLESS_VERDICT_MAX + 1u)),
                          "unknown"), 0,
                   "an out-of-range verdict reads unknown");
    TEST_ASSERT_EQ(strcmp(tpm_enroll_authority_label(
                              TPM_ENROLL_AUTH_HEADLESS_SIGNED_AUTHORIZATION),
                          "headless-signed-authorization"), 0,
                   "the headless authority has its own label");
    TEST_ASSERT_EQ(strcmp(tpm_enroll_refusal_label(
                              TPM_ENROLL_REFUSE_HEADLESS_AUTHZ),
                          "headless-authz-refused"), 0,
                   "the headless refusal has its own label");
}


/* ---- the preflight, which is what keeps forged bytes off the TPM ----------- */

static void test_ha_precheck_ordering(void)
{
    uint8_t blob[TPM_HEADLESS_BLOB_LEN];
    uint32_t i;

    ha_fixture_init();
    ha_build(blob, TPM_HEADLESS_OP_ENROLL_BASELINE, HA_COUNTER);

    /* The preflight is the gate that decides whether a blob costs the TPM
     * anything. Everything it refuses is refused for FREE. */
    TEST_ASSERT_EQ(tpm_headless_authz_precheck(blob, TPM_HEADLESS_BLOB_LEN, 1u, g_pub),
                   TPM_HEADLESS_OK, "a genuinely signed blob passes the preflight");
    TEST_ASSERT_EQ(tpm_headless_authz_precheck(0, 0u, 1u, g_pub),
                   TPM_HEADLESS_ABSENT, "no blob is absent");
    TEST_ASSERT_EQ(tpm_headless_authz_precheck(blob, TPM_HEADLESS_BLOB_LEN, 0u, g_pub),
                   TPM_HEADLESS_NO_AUTHORITY, "no authority, no hatch");
    TEST_ASSERT_EQ(tpm_headless_authz_precheck(blob, TPM_HEADLESS_BLOB_LEN, 1u, 0),
                   TPM_HEADLESS_NO_AUTHORITY, "a NULL authority key is not an authority");
    TEST_ASSERT_EQ(tpm_headless_authz_precheck(blob, TPM_HEADLESS_BLOB_LEN - 1u, 1u, g_pub),
                   TPM_HEADLESS_BAD_FORMAT, "a wrong length never reaches the TPM");

    /* THE ONE THAT MATTERS: bytes an attacker invented are rejected by the
     * signature check, which needs no TPM at all. */
    blob[TPM_HEADLESS_SIGNED_LEN] ^= 0x40u;
    TEST_ASSERT_EQ(tpm_headless_authz_precheck(blob, TPM_HEADLESS_BLOB_LEN, 1u, g_pub),
                   TPM_HEADLESS_BAD_SIGNATURE,
                   "a forged signature is refused before any TPM transaction");

    /* And the preflight is the SAME code the full predicate runs first, so the
     * two can never disagree about the ordering. */
    ha_build(blob, TPM_HEADLESS_OP_ENROLL_BASELINE, HA_COUNTER);
    for (i = 0; i < 4u; i++) {
        struct tpm_headless_authz_inputs in;
        uint8_t probe[TPM_HEADLESS_BLOB_LEN];
        uint32_t k;
        for (k = 0; k < TPM_HEADLESS_BLOB_LEN; k++)
            probe[k] = blob[k];
        /* Corrupt magic, version, reserved0 and the signature in turn. */
        if (i == 0) probe[0] ^= 0xFFu;
        if (i == 1) ha_put16(probe + 4, 0x7FFFu);
        if (i == 2) ha_put16(probe + 6, 1u);
        if (i == 3) probe[TPM_HEADLESS_SIGNED_LEN + 7] ^= 0x11u;
        ha_inputs(&in, probe, TPM_HEADLESS_OP_ENROLL_BASELINE);
        TEST_ASSERT_EQ(tpm_headless_authz_evaluate(&in),
                       tpm_headless_authz_precheck(probe, TPM_HEADLESS_BLOB_LEN,
                                                   1u, g_pub),
                       "evaluate and precheck agree on every local refusal");
    }
}

/* The wire offsets the decoder reads must be the ones the struct declares.
 * Asserting them here as well as in the header is what makes the _Static_asserts
 * describe the DECODER rather than a struct nothing reads. */
static void test_ha_wire_offsets(void)
{
    TEST_ASSERT_EQ(TPM_HEADLESS_OFF_MAGIC, 0u, "magic at 0");
    TEST_ASSERT_EQ(TPM_HEADLESS_OFF_VERSION, 4u, "version at 4");
    TEST_ASSERT_EQ(TPM_HEADLESS_OFF_RESERVED0, 6u, "reserved0 at 6");
    TEST_ASSERT_EQ(TPM_HEADLESS_OFF_OPERATION, 8u, "operation at 8");
    TEST_ASSERT_EQ(TPM_HEADLESS_OFF_RESERVED1, 12u, "reserved1 at 12");
    TEST_ASSERT_EQ(TPM_HEADLESS_OFF_DEVICE_ID, 16u, "device_id at 16");
    TEST_ASSERT_EQ(TPM_HEADLESS_OFF_PCR_SET, 48u, "pcr_set at 48");
    TEST_ASSERT_EQ(TPM_HEADLESS_OFF_COUNTER, 80u, "valid_at_counter at 80");
    TEST_ASSERT_EQ(TPM_HEADLESS_OFF_SIGNATURE, TPM_HEADLESS_SIGNED_LEN,
                   "the signature begins exactly where the signed span ends");
    TEST_ASSERT_EQ(sizeof(struct tpm_headless_authz_blob), TPM_HEADLESS_BLOB_LEN,
                   "the wire struct is the wire length");
}

void test_register_tpm_headless_authz(void)
{
    test_suite_register_cat("tpm headless: accept path",
                            test_ha_accept, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: wrong device",
                            test_ha_wrong_device, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: wrong PCR set",
                            test_ha_wrong_pcr_set, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: wrong operation",
                            test_ha_wrong_operation, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: stale or spent",
                            test_ha_stale_or_spent, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: not yet valid",
                            test_ha_not_yet_valid, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: bad signature",
                            test_ha_bad_signature, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: wrong authority",
                            test_ha_wrong_authority, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: bad format",
                            test_ha_bad_format, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: fail-closed defaults",
                            test_ha_fail_closed, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: unknown facts",
                            test_ha_unknown_facts, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: authority install is one-way",
                            test_ha_authority_install, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: gate admits and reports",
                            test_ha_gate_admits_and_reports, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: gate never substitutes",
                            test_ha_gate_never_substitutes, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: console path untouched",
                            test_ha_gate_console_untouched, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: verdict labels",
                            test_ha_labels, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: preflight keeps forged bytes off the TPM",
                            test_ha_precheck_ordering, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: wire offsets match the struct",
                            test_ha_wire_offsets, TEST_CAT_SECURITY);
}
