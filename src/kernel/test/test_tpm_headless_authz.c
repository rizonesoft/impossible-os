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
#include "kernel/tpm.h"
#include "kernel/tpm_baseline.h"
#include "kernel/tpm_nv.h"
#include "kernel/boot_info.h"
#include "kernel/boot_headless_authz.h"
#include "kernel/test/tpm_fake_tis.h"
#include "kernel/tpm_transport.h"
#include "kernel/mm/memmap.h"
#include "kernel/kchecksum.h"
#include "kernel/mm/pmm.h"
#include "kernel/crypto/sha256.h"
#include "libs/monocypher/monocypher-ed25519.h"
#include "libc/string.h"

/* ---- fixture ---------------------------------------------------------------- */

#define HA_COUNTER 7u

static uint8_t g_pub[TPM_HEADLESS_PUBKEY_LEN];
static uint8_t g_secret[64];
static uint8_t g_device[TPM_HEADLESS_ID_LEN];
static uint8_t g_transition[TPM_HEADLESS_ID_LEN];

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
    ha_fill(g_transition, sizeof g_transition, 0x80u);
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
    for (i = 0; i < TPM_HEADLESS_ID_LEN; i++) blob[48 + i] = g_transition[i];
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
    in->transition_known = 1u;
    in->counter_known = 1u;
    in->counter = HA_COUNTER;
    in->operation = op;
    for (i = 0; i < TPM_HEADLESS_PUBKEY_LEN; i++) in->authority_pub[i] = g_pub[i];
    for (i = 0; i < TPM_HEADLESS_ID_LEN; i++) in->device_id[i] = g_device[i];
    for (i = 0; i < TPM_HEADLESS_ID_LEN; i++) in->transition_id[i] = g_transition[i];
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

static void test_ha_wrong_transition(void)
{
    uint8_t blob[TPM_HEADLESS_BLOB_LEN];
    struct tpm_headless_authz_inputs in;

    ha_fixture_init();
    ha_build(blob, TPM_HEADLESS_OP_ENROLL_BASELINE, HA_COUNTER);
    ha_inputs(&in, blob, TPM_HEADLESS_OP_ENROLL_BASELINE);
    in.transition_id[31] ^= 0x01u;
    TEST_ASSERT_EQ(tpm_headless_authz_evaluate(&in), TPM_HEADLESS_WRONG_TRANSITION,
                   "a blob signed for another transition is wrong-transition");
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
    in.transition_known = 0u;
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
    TEST_ASSERT_EQ(TPM_HEADLESS_OFF_TRANSITION, 48u, "transition_id at 48");
    TEST_ASSERT_EQ(TPM_HEADLESS_OFF_COUNTER, 80u, "valid_at_counter at 80");
    TEST_ASSERT_EQ(TPM_HEADLESS_OFF_SIGNATURE, TPM_HEADLESS_SIGNED_LEN,
                   "the signature begins exactly where the signed span ends");
    TEST_ASSERT_EQ(sizeof(struct tpm_headless_authz_blob), TPM_HEADLESS_BLOB_LEN,
                   "the wire struct is the wire length");
}


/* ---- transport + full-record binding (section 30) ---------------------------
 *
 * These cover the two halves this section added: the digest an authorization is
 * actually signed over, and the classification of the payload that carries it.
 *
 * Every refusal sits beside a control built from the same fixture, because a
 * "these differ" assertion passes just as happily against a digest function
 * that returns fresh garbage every call.
 *
 * The take path itself (boot_headless_authz_take) is deliberately NOT driven
 * from here: it dereferences a physical address through the boot identity map
 * and MUTATES the live descriptor table, which a unit test can neither
 * synthesize nor safely disturb. The decision it makes is
 * boot_headless_authz_classify, which is pure and is exhaustively covered
 * below -- the same split, for the same reason, as boot_seed_desc_classify.
 */

/* A baseline with every security-relevant field populated to a distinct,
 * non-zero pattern, so a digest that silently ignored one would be caught by
 * the perturbation tests rather than by luck. */
static void ha_baseline(struct tpm_baseline *b, uint8_t seed)
{
    uint32_t i;

    memset(b, 0, sizeof *b);
    b->magic     = TPM_BASELINE_MAGIC;
    b->version   = TPM_BASELINE_VERSION;
    b->size      = (uint16_t)sizeof *b;
    b->alg       = TPM_ALG_SHA256;
    b->pcr_count = 2u;
    b->secure_boot = 1u;
    b->secure_boot_valid = 1u;
    b->fw_hash_present = 1u;
    b->abi_manifest_present = 1u;
    ha_fill(b->fw_hash, (uint32_t)sizeof b->fw_hash, seed);
    ha_fill(b->abi_manifest, (uint32_t)sizeof b->abi_manifest,
            (uint8_t)(seed + 0x20u));
    for (i = 0; i < 2u; i++) {
        b->pcrs[i].index = (uint8_t)i;
        b->pcrs[i].present = 1u;
        ha_fill(b->pcrs[i].digest, (uint32_t)sizeof b->pcrs[i].digest,
                (uint8_t)(seed + 0x40u + i));
    }
}

/* THE POINT OF THE SECTION: the digest covers the fields a PCR-set digest did
 * not. Each perturbation touches one non-PCR field and nothing else. */
static void test_ha_canon_covers_non_pcr_fields(void)
{
    struct tpm_baseline a, b;
    uint8_t da[TPM_BASELINE_DIGEST], db[TPM_BASELINE_DIGEST];

    ha_baseline(&a, 0x10u);

    /* Control FIRST: identical content must digest identically, or every
     * "differs" assertion below is vacuous. */
    memcpy(&b, &a, sizeof b);
    TEST_ASSERT_EQ(tpm_baseline_canon_digest(&a, da), 0, "canon digest of a");
    TEST_ASSERT_EQ(tpm_baseline_canon_digest(&b, db), 0, "canon digest of its copy");
    TEST_ASSERT_EQ(memcmp(da, db, sizeof da), 0,
                   "identical baselines digest identically");

    memcpy(&b, &a, sizeof b);
    b.fw_hash[0] ^= 0xFFu;
    TEST_ASSERT_EQ(tpm_baseline_canon_digest(&b, db), 0, "canon digest, fw_hash moved");
    TEST_ASSERT_EQ(memcmp(da, db, sizeof da) != 0, 1,
                   "a different firmware-version hash is a different record");

    memcpy(&b, &a, sizeof b);
    b.abi_manifest[0] ^= 0xFFu;
    TEST_ASSERT_EQ(tpm_baseline_canon_digest(&b, db), 0, "canon digest, abi manifest moved");
    TEST_ASSERT_EQ(memcmp(da, db, sizeof da) != 0, 1,
                   "a different ABI-manifest digest is a different record");

    memcpy(&b, &a, sizeof b);
    b.secure_boot = 0u;
    TEST_ASSERT_EQ(tpm_baseline_canon_digest(&b, db), 0, "canon digest, SB state moved");
    TEST_ASSERT_EQ(memcmp(da, db, sizeof da) != 0, 1,
                   "a different Secure Boot state is a different record");

    memcpy(&b, &a, sizeof b);
    b.pcrs[0].digest[0] ^= 0xFFu;
    TEST_ASSERT_EQ(tpm_baseline_canon_digest(&b, db), 0, "canon digest, PCR moved");
    TEST_ASSERT_EQ(memcmp(da, db, sizeof da) != 0, 1,
                   "a different PCR digest is still a different record");
}

/* The three fields the digest deliberately does NOT cover. generation is the
 * load-bearing one: the enroll assigns it from what is already in NV, so an
 * offline signer cannot predict it and covering it would make every
 * authorization unsignable. */
static void test_ha_canon_excludes_derived_fields(void)
{
    struct tpm_baseline a, b;
    uint8_t da[TPM_BASELINE_DIGEST], db[TPM_BASELINE_DIGEST];

    ha_baseline(&a, 0x10u);
    memcpy(&b, &a, sizeof b);
    b.generation = 41u;
    b.crc32      = 0xDEADBEEFu;
    b.pad        = 0x5Au;
    a.generation = 7u;

    TEST_ASSERT_EQ(tpm_baseline_canon_digest(&a, da), 0, "canon digest a");
    TEST_ASSERT_EQ(tpm_baseline_canon_digest(&b, db), 0, "canon digest b");
    TEST_ASSERT_EQ(memcmp(da, db, sizeof da), 0,
                   "generation, crc32 and pad are excluded from the digest");

    /* The control that stops the assertion above from passing against a digest
     * function that ignores everything: one content byte still moves it. */
    b.fw_hash[3] ^= 0x01u;
    TEST_ASSERT_EQ(tpm_baseline_canon_digest(&b, db), 0, "canon digest b perturbed");
    TEST_ASSERT_EQ(memcmp(da, db, sizeof da) != 0, 1,
                   "a content byte still moves the digest");

    TEST_ASSERT_EQ(tpm_baseline_canon_digest((const struct tpm_baseline *)0, da), -1,
                   "a NULL baseline is refused, not hashed");
}

/* The predecessor half. A token issued for a transition out of state P must not
 * verify against a machine that is now in state Q -- that is the rollback the
 * digest alone would have permitted. */
static void test_ha_transition_binds_predecessor(void)
{
    struct tpm_baseline prev_p, prev_q, cand;
    uint8_t t_first[TPM_BASELINE_DIGEST];
    uint8_t t_p[TPM_BASELINE_DIGEST];
    uint8_t t_p2[TPM_BASELINE_DIGEST];
    uint8_t t_q[TPM_BASELINE_DIGEST];
    uint8_t t_pgen[TPM_BASELINE_DIGEST];

    ha_baseline(&cand,   0x10u);
    ha_baseline(&prev_p, 0x60u);
    ha_baseline(&prev_q, 0x90u);

    TEST_ASSERT_EQ(tpm_baseline_transition_id((const struct tpm_baseline *)0, 0u,
                                              &cand, t_first), 0,
                   "a first-enrollment transition is describable");
    TEST_ASSERT_EQ(tpm_baseline_transition_id(&prev_p, 3u, &cand, t_p), 0,
                   "a rotation out of P is describable");
    TEST_ASSERT_EQ(tpm_baseline_transition_id(&prev_p, 3u, &cand, t_p2), 0,
                   "the same rotation again");
    TEST_ASSERT_EQ(tpm_baseline_transition_id(&prev_q, 3u, &cand, t_q), 0,
                   "a rotation out of Q is describable");
    TEST_ASSERT_EQ(tpm_baseline_transition_id(&prev_p, 4u, &cand, t_pgen), 0,
                   "a rotation out of P at a later generation");

    /* Control: the same transition twice is the same digest. */
    TEST_ASSERT_EQ(memcmp(t_p, t_p2, sizeof t_p), 0,
                   "the same transition digests identically");
    TEST_ASSERT_EQ(memcmp(t_p, t_q, sizeof t_p) != 0, 1,
                   "the same candidate out of a DIFFERENT predecessor is a "
                   "different transition");
    TEST_ASSERT_EQ(memcmp(t_p, t_first, sizeof t_p) != 0, 1,
                   "a first enrollment is not the same transition as a rotation");
    TEST_ASSERT_EQ(memcmp(t_p, t_pgen, sizeof t_p) != 0, 1,
                   "the predecessor's generation is part of the transition");

    TEST_ASSERT_EQ(tpm_baseline_transition_id((const struct tpm_baseline *)0, 1u,
                                              &cand, t_p), -1,
                   "no predecessor with a non-zero generation is a contradiction");
    TEST_ASSERT_EQ(tpm_baseline_transition_id(&prev_p, 3u,
                                              (const struct tpm_baseline *)0, t_p), -1,
                   "a NULL candidate is refused");
}

/* An authorization whose blob names a different transition is refused with the
 * transition verdict specifically, not with a generic failure -- an operator
 * holding a stale token needs to know to reissue it. */
static void test_ha_non_pcr_change_refused(void)
{
    struct tpm_baseline cand, altered;
    uint8_t blob[TPM_HEADLESS_BLOB_LEN];
    struct tpm_headless_authz_inputs in;
    uint8_t signed_tid[TPM_BASELINE_DIGEST];
    uint8_t live_tid[TPM_BASELINE_DIGEST];
    uint32_t i;

    ha_fixture_init();
    ha_baseline(&cand, 0x10u);
    TEST_ASSERT_EQ(tpm_baseline_transition_id((const struct tpm_baseline *)0, 0u,
                                              &cand, signed_tid), 0,
                   "the authorized transition is describable");

    /* Sign for the candidate as approved. */
    ha_build(blob, TPM_HEADLESS_OP_ENROLL_BASELINE, HA_COUNTER);
    for (i = 0; i < TPM_HEADLESS_ID_LEN; i++)
        blob[TPM_HEADLESS_OFF_TRANSITION + i] = signed_tid[i];
    crypto_ed25519_sign(blob + TPM_HEADLESS_SIGNED_LEN, g_secret,
                        blob, TPM_HEADLESS_SIGNED_LEN);

    /* Control: the machine really is in the authorized transition. */
    ha_inputs(&in, blob, TPM_HEADLESS_OP_ENROLL_BASELINE);
    for (i = 0; i < TPM_HEADLESS_ID_LEN; i++) in.transition_id[i] = signed_tid[i];
    TEST_ASSERT_EQ(tpm_headless_authz_evaluate(&in), TPM_HEADLESS_OK,
                   "the authorized transition is accepted");

    /* Now change ONLY a non-PCR field -- the exact case a PCR-set digest could
     * not see. The PCR digests are byte-for-byte identical. */
    memcpy(&altered, &cand, sizeof altered);
    altered.fw_hash[0] ^= 0xFFu;
    TEST_ASSERT_EQ(memcmp(altered.pcrs, cand.pcrs, sizeof cand.pcrs), 0,
                   "the PCR set is unchanged, which is what makes this the case "
                   "the old digest missed");
    TEST_ASSERT_EQ(tpm_baseline_transition_id((const struct tpm_baseline *)0, 0u,
                                              &altered, live_tid), 0,
                   "the altered transition is describable");
    for (i = 0; i < TPM_HEADLESS_ID_LEN; i++) in.transition_id[i] = live_tid[i];
    TEST_ASSERT_EQ(tpm_headless_authz_evaluate(&in), TPM_HEADLESS_WRONG_TRANSITION,
                   "a record whose non-PCR fields differ from the authorized "
                   "candidate is refused");

    /* And an absent transition is a refusal, never an unbound admission. */
    ha_inputs(&in, blob, TPM_HEADLESS_OP_ENROLL_BASELINE);
    for (i = 0; i < TPM_HEADLESS_ID_LEN; i++) in.transition_id[i] = signed_tid[i];
    in.transition_known = 0u;
    TEST_ASSERT_EQ(tpm_headless_authz_evaluate(&in), TPM_HEADLESS_UNKNOWN_STATE,
                   "a transition that could not be established refuses rather "
                   "than admitting unbound");
}

/* The transport's whole refusal matrix. A payload-absent boot is the FIRST
 * case: it is the one every ordinary machine takes. */
static void test_ha_payload_classification(void)
{
    const uint32_t ok_flags = BOOT_PAYLOAD_FLAG_VALID |
                              BOOT_PAYLOAD_FLAG_RESERVED |
                              BOOT_PAYLOAD_FLAG_CHECKSUMMED;
    const uint64_t at = 0x200000ull;

    /* Control: a well-formed descriptor is usable. Asserted first so every
     * refusal below is known to be caused by its own perturbation. */
    TEST_ASSERT_EQ(boot_headless_authz_classify(BOOT_CAP_PAYLOAD_DESCRIPTORS, ok_flags, at,
                                                (uint64_t)TPM_HEADLESS_BLOB_LEN),
                   BOOT_HL_AUTHZ_USABLE,
                   "a reserved, in-map, exact-length, checksummed payload is usable");

    TEST_ASSERT_EQ(boot_headless_authz_classify(
                           BOOT_CAP_PAYLOAD_DESCRIPTORS, ok_flags & ~(uint32_t)BOOT_PAYLOAD_FLAG_RESERVED, at,
                       (uint64_t)TPM_HEADLESS_BLOB_LEN),
                   BOOT_HL_AUTHZ_NOT_RESERVED,
                   "an unreserved range is refused before anything reads it");

    TEST_ASSERT_EQ(boot_headless_authz_classify(BOOT_CAP_PAYLOAD_DESCRIPTORS, ok_flags, BOOT_INFO_EARLY_MAP_END,
                                                (uint64_t)TPM_HEADLESS_BLOB_LEN),
                   BOOT_HL_AUTHZ_OUT_OF_MAP,
                   "a payload at the map end is outside the boot identity map");
    TEST_ASSERT_EQ(boot_headless_authz_classify(BOOT_CAP_PAYLOAD_DESCRIPTORS, ok_flags,
                       BOOT_INFO_EARLY_MAP_END - (uint64_t)TPM_HEADLESS_BLOB_LEN + 1ull,
                       (uint64_t)TPM_HEADLESS_BLOB_LEN),
                   BOOT_HL_AUTHZ_OUT_OF_MAP,
                   "a payload straddling the map end is refused, not truncated");

    /* MALFORMED PAYLOAD, both directions. Short is not a weaker authorization
     * and long is not this format. */
    TEST_ASSERT_EQ(boot_headless_authz_classify(BOOT_CAP_PAYLOAD_DESCRIPTORS, ok_flags, at,
                                                (uint64_t)TPM_HEADLESS_BLOB_LEN - 1ull),
                   BOOT_HL_AUTHZ_BAD_LENGTH, "a short payload is refused");
    TEST_ASSERT_EQ(boot_headless_authz_classify(BOOT_CAP_PAYLOAD_DESCRIPTORS, ok_flags, at,
                                                (uint64_t)TPM_HEADLESS_BLOB_LEN + 1ull),
                   BOOT_HL_AUTHZ_BAD_LENGTH, "a long payload is refused");
    TEST_ASSERT_EQ(boot_headless_authz_classify(BOOT_CAP_PAYLOAD_DESCRIPTORS, ok_flags, at, 0ull),
                   BOOT_HL_AUTHZ_BAD_LENGTH, "an empty payload is refused");

    TEST_ASSERT_EQ(boot_headless_authz_classify(
                           BOOT_CAP_PAYLOAD_DESCRIPTORS, ok_flags & ~(uint32_t)BOOT_PAYLOAD_FLAG_CHECKSUMMED, at,
                       (uint64_t)TPM_HEADLESS_BLOB_LEN),
                   BOOT_HL_AUTHZ_NOT_CHECKSUMMED,
                   "a payload with nothing to detect corruption is refused");

    /* The LAST VALID range, which the straddle case above does not cover: an
     * off-by-one in the bound would reject a payload that genuinely fits. */
    TEST_ASSERT_EQ(boot_headless_authz_classify(BOOT_CAP_PAYLOAD_DESCRIPTORS, ok_flags,
                       BOOT_INFO_EARLY_MAP_END - (uint64_t)TPM_HEADLESS_BLOB_LEN,
                       (uint64_t)TPM_HEADLESS_BLOB_LEN),
                   BOOT_HL_AUTHZ_USABLE,
                   "a payload ending exactly at the map end still fits");
    /* And the overflow shape: phys_start + length would wrap, so the bound has
     * to be written as a subtraction. */
    TEST_ASSERT_EQ(boot_headless_authz_classify(BOOT_CAP_PAYLOAD_DESCRIPTORS, ok_flags, 0xFFFFFFFFFFFFFFFFull,
                                                (uint64_t)TPM_HEADLESS_BLOB_LEN),
                   BOOT_HL_AUTHZ_OUT_OF_MAP,
                   "a phys_start that would wrap is refused, not accepted");

    /* THE CAPABILITY GATE. Without the negotiated bit a RESERVED-flagged
     * descriptor names frames the reservation pass skipped, so it must be
     * refused BEFORE anything reads or wipes them -- and it outranks every
     * other check for exactly that reason. */
    TEST_ASSERT_EQ(boot_headless_authz_classify(0u, ok_flags, at,
                                                (uint64_t)TPM_HEADLESS_BLOB_LEN),
                   BOOT_HL_AUTHZ_CAP_ABSENT,
                   "an unnegotiated descriptor array is refused outright");
    TEST_ASSERT_EQ(boot_headless_authz_classify(0u, ok_flags, at, 1ull),
                   BOOT_HL_AUTHZ_CAP_ABSENT,
                   "the capability gate outranks the length check");
    TEST_ASSERT_EQ(boot_headless_authz_classify(
                       (uint32_t)~(uint32_t)BOOT_CAP_PAYLOAD_DESCRIPTORS, ok_flags,
                       at, (uint64_t)TPM_HEADLESS_BLOB_LEN),
                   BOOT_HL_AUTHZ_CAP_ABSENT,
                   "every other capability bit set is still not this one");

    /* ORDER: an unreserved descriptor is refused for being unreserved even
     * when its length is also wrong, because the earlier check is the one that
     * keeps its memory untouched. */
    TEST_ASSERT_EQ(boot_headless_authz_classify(
                           BOOT_CAP_PAYLOAD_DESCRIPTORS, ok_flags & ~(uint32_t)BOOT_PAYLOAD_FLAG_RESERVED, at, 1ull),
                   BOOT_HL_AUTHZ_NOT_RESERVED,
                   "the reserved check outranks the length check");
}

/* Every class maps to its own label, and a value cast in from outside the enum
 * names itself rather than borrowing a class it is not. */
static void test_ha_class_labels(void)
{
    TEST_ASSERT_EQ(strcmp(boot_headless_authz_class_label(BOOT_HL_AUTHZ_USABLE),
                          "usable"), 0, "usable label");
    TEST_ASSERT_EQ(strcmp(boot_headless_authz_class_label(BOOT_HL_AUTHZ_NOT_RESERVED),
                          "not-pmm-reserved"), 0, "not-reserved label");
    TEST_ASSERT_EQ(strcmp(boot_headless_authz_class_label(BOOT_HL_AUTHZ_OUT_OF_MAP),
                          "outside-boot-identity-map"), 0, "out-of-map label");
    TEST_ASSERT_EQ(strcmp(boot_headless_authz_class_label(BOOT_HL_AUTHZ_BAD_LENGTH),
                          "wrong-length"), 0, "bad-length label");
    TEST_ASSERT_EQ(strcmp(boot_headless_authz_class_label(BOOT_HL_AUTHZ_NOT_CHECKSUMMED),
                          "not-checksummed"), 0, "not-checksummed label");
    TEST_ASSERT_EQ(strcmp(boot_headless_authz_class_label(BOOT_HL_AUTHZ_CAP_ABSENT),
                          "payload-capability-absent"), 0, "cap-absent label");
    TEST_ASSERT_EQ(strcmp(boot_headless_authz_class_label((boot_hl_authz_class_t)99),
                          "unknown"), 0,
                   "a value from outside the enum names itself");
}

/* The headless entry point must carry the same refusal the console one does. Argument validation only -- no TPM is
 * touched on either of these paths. */
static void test_ha_enroll_headless_arg_guards(void)
{
    struct tpm_baseline cand;
    uint8_t tid[TPM_BASELINE_DIGEST];

    ha_baseline(&cand, 0x10u);
    memset(tid, 0x33, sizeof tid);

    TEST_ASSERT_EQ(tpm_baseline_enroll_headless(TPM_NV_INDEX_BASELINE,
                                                (const struct tpm_baseline *)0, tid),
                   TPM_BASELINE_BADARG,
                   "a NULL candidate is refused before any NV access");
    TEST_ASSERT_EQ(tpm_baseline_enroll_headless(TPM_NV_INDEX_BASELINE, &cand,
                                                (const uint8_t *)0),
                   TPM_BASELINE_BADARG,
                   "a NULL transition digest is refused before any NV access");
}


/* INDEPENDENT known-answer vectors. Every other digest test in this file
 * compares two outputs of the same implementation, so tag length, trailing-NUL
 * inclusion, byte order, struct extent or field selection could all drift while
 * staying internally self-consistent -- and every offline signing tool would
 * break silently. These three values were derived independently from the
 * published construction (tag bytes, header layout, hash order) and confirmed
 * byte-for-byte against a real run, so they pin the wire behaviour rather than
 * the code's agreement with itself.
 *
 * A change here is a FORMAT CHANGE. If one of these fails, the question is not
 * "what is the new value" -- it is whether every already-signed authorization
 * in the field has just been invalidated. */
static const uint8_t k_vec_canon_seed10[TPM_BASELINE_DIGEST] = {
    0xcd,0xff,0x87,0xae,0x9f,0x95,0x5d,0x9d,0x26,0x91,0x2d,0x9c,0x74,0xb8,0xc7,0xa5,
    0x7f,0x9e,0x12,0xf3,0xbf,0xed,0x4e,0xbd,0x8f,0x5d,0x29,0xee,0x16,0xfd,0x96,0x68
};
static const uint8_t k_vec_first_enroll[TPM_BASELINE_DIGEST] = {
    0xf1,0x56,0x28,0xe2,0xa9,0x34,0x39,0x34,0xd2,0x38,0xd7,0xb9,0x93,0x88,0x56,0x1c,
    0x8c,0xf3,0xca,0x14,0x6b,0x2f,0x5c,0xad,0x29,0x85,0x54,0x9b,0x05,0x56,0xb0,0x77
};
static const uint8_t k_vec_rotate_p60_gen3[TPM_BASELINE_DIGEST] = {
    0x8a,0xbc,0x4b,0x58,0xd5,0x58,0x53,0x1b,0x93,0x72,0xf7,0xbf,0x81,0x74,0x70,0x95,
    0x90,0xfc,0x4f,0x5d,0x9d,0x61,0xd0,0xa1,0x3f,0x93,0x22,0xfd,0x68,0x6c,0x8b,0xf9
};

static void test_ha_digest_known_answers(void)
{
    struct tpm_baseline a, p60;
    uint8_t d[TPM_BASELINE_DIGEST];

    ha_baseline(&a, 0x10u);
    ha_baseline(&p60, 0x60u);

    TEST_ASSERT_EQ(tpm_baseline_canon_digest(&a, d), 0, "canon digest computed");
    TEST_ASSERT_EQ(memcmp(d, k_vec_canon_seed10, sizeof d), 0,
                   "the canonical digest matches its independent vector");

    TEST_ASSERT_EQ(tpm_baseline_transition_id((const struct tpm_baseline *)0, 0u,
                                              &a, d), 0,
                   "first-enrollment transition computed");
    TEST_ASSERT_EQ(memcmp(d, k_vec_first_enroll, sizeof d), 0,
                   "the first-enrollment transition matches its vector");

    TEST_ASSERT_EQ(tpm_baseline_transition_id(&p60, 3u, &a, d), 0,
                   "rotation transition computed");
    TEST_ASSERT_EQ(memcmp(d, k_vec_rotate_p60_gen3, sizeof d), 0,
                   "the rotation transition matches its vector");
}

/* One perturbation per SECURITY-RELEVANT scalar, table-driven so a field added
 * to the record without a row here is visible as an omission rather than as
 * silent non-coverage. Each row moves exactly one field. */
static void test_ha_digest_every_field_covered(void)
{
    struct tpm_baseline base, b;
    uint8_t d0[TPM_BASELINE_DIGEST], d[TPM_BASELINE_DIGEST];
    uint32_t row;

    ha_baseline(&base, 0x10u);
    TEST_ASSERT_EQ(tpm_baseline_canon_digest(&base, d0), 0, "baseline digest");

    for (row = 0; row < 9u; row++) {
        const char *what = "unnamed";
        memcpy(&b, &base, sizeof b);
        switch (row) {
        case 0: b.alg = TPM_ALG_SHA1;            what = "alg"; break;
        case 1: b.pcr_count = 3u;                what = "pcr_count"; break;
        case 2: b.secure_boot ^= 1u;             what = "secure_boot"; break;
        case 3: b.secure_boot_valid ^= 1u;       what = "secure_boot_valid"; break;
        case 4: b.fw_hash_present ^= 1u;         what = "fw_hash_present"; break;
        case 5: b.abi_manifest_present ^= 1u;    what = "abi_manifest_present"; break;
        case 6: b.pcrs[0].index ^= 0x0Fu;        what = "pcr index"; break;
        case 7: b.pcrs[0].present ^= 1u;         what = "pcr present"; break;
        case 8: b.pcrs[8].digest[31] ^= 0xFFu;   what = "last PCR digest byte"; break;
        default: break;
        }
        TEST_ASSERT_EQ(tpm_baseline_canon_digest(&b, d), 0, "perturbed digest");
        TEST_ASSERT_EQ(memcmp(d, d0, sizeof d) != 0, 1, what);
    }

    /* And the padding bytes that must NOT move it, both levels. The section
     * promises every padding byte is canonicalized, and only a nested case
     * proves the nested loop runs. */
    memcpy(&b, &base, sizeof b);
    b.pad = 0x5Au;
    b.pcrs[0].pad[0] = 0x5Au;
    b.pcrs[8].pad[1] = 0xA5u;
    TEST_ASSERT_EQ(tpm_baseline_canon_digest(&b, d), 0, "padded digest");
    TEST_ASSERT_EQ(memcmp(d, d0, sizeof d), 0,
                   "top-level AND nested padding are canonicalized away");
}

/* THE NO-TPM PREFLIGHT GUARANTEE, asserted on the public wrapper rather than on
 * the helper it calls. A wiring regression that gathered EK or counter state
 * before returning a local refusal would leave every evaluate/precheck test
 * green while letting attacker-written ESP bytes cost TPM work on every boot. */
static void test_ha_authorize_spends_no_tpm_on_refusal(void)
{
    struct tpm_t_test_state prev;
    uint8_t blob[TPM_HEADLESS_BLOB_LEN];
    tpm_headless_verdict_t v_absent, v_short, v_noauth, v_badsig;
    uint32_t n_absent, n_short, n_noauth, n_badsig;

    ha_fixture_init();
    tpm_fake_tis_reset();
    prev = tpm_t_test_install(tpm_fake_tis_io(), TPM_T_IFACE_TIS, 1);

    tpm_headless_authz_reset_authority_for_test();

    v_absent = tpm_headless_authz_authorize((const uint8_t *)0, 0u,
                                            (uint32_t)TPM_HEADLESS_OP_ENROLL_BASELINE,
                                            (const uint8_t *)0);
    n_absent = tpm_fake_tis_log_count();

    ha_build(blob, TPM_HEADLESS_OP_ENROLL_BASELINE, HA_COUNTER);
    v_noauth = tpm_headless_authz_authorize(blob, TPM_HEADLESS_BLOB_LEN,
                                            (uint32_t)TPM_HEADLESS_OP_ENROLL_BASELINE,
                                            (const uint8_t *)0);
    n_noauth = tpm_fake_tis_log_count();

    TEST_ASSERT_EQ(tpm_headless_authz_set_authority(g_pub), 0,
                   "the fixture authority installs");

    v_short = tpm_headless_authz_authorize(blob, TPM_HEADLESS_BLOB_LEN - 1u,
                                           (uint32_t)TPM_HEADLESS_OP_ENROLL_BASELINE,
                                           (const uint8_t *)0);
    n_short = tpm_fake_tis_log_count();

    blob[TPM_HEADLESS_SIGNED_LEN] ^= 0x01u;    /* one bit of the signature */
    v_badsig = tpm_headless_authz_authorize(blob, TPM_HEADLESS_BLOB_LEN,
                                            (uint32_t)TPM_HEADLESS_OP_ENROLL_BASELINE,
                                            (const uint8_t *)0);
    n_badsig = tpm_fake_tis_log_count();

    tpm_headless_authz_reset_authority_for_test();
    tpm_t_test_restore(prev);

    TEST_ASSERT_EQ((int)v_absent, (int)TPM_HEADLESS_ABSENT,
                   "no blob at all is ABSENT");
    TEST_ASSERT_EQ(n_absent, 0u, "an absent blob costs the TPM nothing");
    TEST_ASSERT_EQ((int)v_noauth, (int)TPM_HEADLESS_NO_AUTHORITY,
                   "a blob with no authority installed is NO_AUTHORITY");
    TEST_ASSERT_EQ(n_noauth, 0u,
                   "a machine with no authority spends nothing on any blob");
    TEST_ASSERT_EQ((int)v_short, (int)TPM_HEADLESS_BAD_FORMAT,
                   "a blob one byte short is BAD_FORMAT");
    TEST_ASSERT_EQ(n_short, 0u, "a malformed blob costs the TPM nothing");
    TEST_ASSERT_EQ((int)v_badsig, (int)TPM_HEADLESS_BAD_SIGNATURE,
                   "a forged signature is BAD_SIGNATURE");
    TEST_ASSERT_EQ(n_badsig, 0u, "a forged blob costs the TPM nothing");
}

/* The same guarantee for the caller-side ordering helper, which is what lets
 * Phase 1 decide whether to pay for a PCR snapshot and a predecessor read. */
static void test_ha_precheck_installed_matches(void)
{
    uint8_t blob[TPM_HEADLESS_BLOB_LEN];

    ha_fixture_init();
    tpm_headless_authz_reset_authority_for_test();
    ha_build(blob, TPM_HEADLESS_OP_ENROLL_BASELINE, HA_COUNTER);

    TEST_ASSERT_EQ((int)tpm_headless_authz_precheck_installed(
                       (const uint8_t *)0, 0u),
                   (int)TPM_HEADLESS_ABSENT, "no blob is ABSENT");
    TEST_ASSERT_EQ((int)tpm_headless_authz_precheck_installed(
                       blob, TPM_HEADLESS_BLOB_LEN),
                   (int)TPM_HEADLESS_NO_AUTHORITY,
                   "no installed authority is NO_AUTHORITY");

    TEST_ASSERT_EQ(tpm_headless_authz_set_authority(g_pub), 0,
                   "the fixture authority installs");
    TEST_ASSERT_EQ((int)tpm_headless_authz_precheck_installed(
                       blob, TPM_HEADLESS_BLOB_LEN),
                   (int)TPM_HEADLESS_OK,
                   "a correctly signed blob passes the local half");
    blob[0] ^= 0xFFu;                            /* magic */
    TEST_ASSERT_EQ((int)tpm_headless_authz_precheck_installed(
                       blob, TPM_HEADLESS_BLOB_LEN),
                   (int)TPM_HEADLESS_BAD_FORMAT,
                   "a wrong magic is BAD_FORMAT");
    tpm_headless_authz_reset_authority_for_test();
}


/* THE ORDERING GUARANTEE, asserted where it actually lives. The direct
 * authorize tests above prove the authz module spends nothing on a local
 * refusal; this one proves the ENROLLMENT ORCHESTRATION does too -- deleting
 * the precheck from tpm_headless_enroll_prepare and letting the snapshot and
 * predecessor read run first turns this red, which is precisely what the
 * caller-side inlined version could not do. */
static void test_ha_prepare_spends_no_tpm_on_refusal(void)
{
    struct tpm_t_test_state prev;
    struct tpm_baseline cand;
    uint8_t blob[TPM_HEADLESS_BLOB_LEN];
    uint8_t tid[TPM_BASELINE_DIGEST];
    uint32_t op = 0u;
    tpm_headless_verdict_t v_absent, v_noauth, v_badsig;
    uint32_t n_absent, n_noauth, n_badsig;

    ha_fixture_init();
    tpm_fake_tis_reset();
    prev = tpm_t_test_install(tpm_fake_tis_io(), TPM_T_IFACE_TIS, 1);
    tpm_headless_authz_reset_authority_for_test();

    v_absent = tpm_headless_enroll_prepare((const uint8_t *)0, 0u,
                                           TPM_NV_INDEX_BASELINE, TPM_ALG_SHA256,
                                           &cand, tid, &op);
    n_absent = tpm_fake_tis_log_count();

    ha_build(blob, TPM_HEADLESS_OP_ENROLL_BASELINE, HA_COUNTER);
    v_noauth = tpm_headless_enroll_prepare(blob, TPM_HEADLESS_BLOB_LEN,
                                           TPM_NV_INDEX_BASELINE, TPM_ALG_SHA256,
                                           &cand, tid, &op);
    n_noauth = tpm_fake_tis_log_count();

    TEST_ASSERT_EQ(tpm_headless_authz_set_authority(g_pub), 0,
                   "the fixture authority installs");
    blob[TPM_HEADLESS_SIGNED_LEN] ^= 0x01u;
    v_badsig = tpm_headless_enroll_prepare(blob, TPM_HEADLESS_BLOB_LEN,
                                           TPM_NV_INDEX_BASELINE, TPM_ALG_SHA256,
                                           &cand, tid, &op);
    n_badsig = tpm_fake_tis_log_count();

    tpm_headless_authz_reset_authority_for_test();
    tpm_t_test_restore(prev);

    TEST_ASSERT_EQ((int)v_absent, (int)TPM_HEADLESS_ABSENT,
                   "no payload is ABSENT");
    TEST_ASSERT_EQ(n_absent, 0u,
                   "an absent payload costs no PCR snapshot and no NV read");
    TEST_ASSERT_EQ((int)v_noauth, (int)TPM_HEADLESS_NO_AUTHORITY,
                   "no installed authority is NO_AUTHORITY");
    TEST_ASSERT_EQ(n_noauth, 0u,
                   "a machine with no authority gathers nothing");
    TEST_ASSERT_EQ((int)v_badsig, (int)TPM_HEADLESS_BAD_SIGNATURE,
                   "a forged token is BAD_SIGNATURE");
    TEST_ASSERT_EQ(n_badsig, 0u,
                   "a forged token gathers nothing -- the whole point of the "
                   "pre-authentication ordering");
}


/* ---- the take path, driven over memory the test owns -----------------------
 *
 * The classifier assertions above prove the DECISION. These prove the take
 * ACTS on it: a refused descriptor's bytes must still be intact afterwards,
 * which no classifier-only assertion can show. Deleting the capability branch
 * from boot_headless_authz_take_from turns the first case red.
 *
 * Safe by construction: the descriptor names a static buffer this file owns,
 * translated with mm_image_virt_to_phys, so even a guard that failed open
 * would touch only this buffer.
 */
/* struct boot_info is several KiB, so the synthetic handoff is allocated rather
 * than made static: the kernel's BSS end sits directly under the 0x800000 user
 * base, and a static copy here pushed past it (BSS COLLISION at link time).
 * The payload page stays static because it is 152 bytes and its ADDRESS has to
 * survive translation to a physical one. */
static struct boot_info *s_hl_info;
static uint8_t s_hl_page[TPM_HEADLESS_BLOB_LEN] __attribute__((aligned(64)));

static int hl_build_handoff(uint32_t caps, uint32_t flags)
{
    struct boot_payload_desc *d;
    uint32_t i;

    for (i = 0; i < TPM_HEADLESS_BLOB_LEN; i++)
        s_hl_page[i] = (uint8_t)(0x31u + (i & 0x3Fu));

    if (!s_hl_info) {
        s_hl_info = (struct boot_info *)
            pmm_alloc_contiguous((sizeof(struct boot_info) + 4095u) / 4096u);
        if (!s_hl_info)
            return 0;
    }
    memset(s_hl_info, 0, sizeof *s_hl_info);
    s_hl_info->caps_present  = caps;
    s_hl_info->payload_count = 1u;
    d = &s_hl_info->payload_descriptors[0];
    (void)d;
    d->type        = (uint32_t)BOOT_PAYLOAD_HEADLESS_AUTHZ;
    d->flags       = flags;
    d->phys_start  = mm_image_virt_to_phys(s_hl_page);
    d->length      = (uint64_t)TPM_HEADLESS_BLOB_LEN;
    d->alignment   = 64ull;
    d->checksum    = (uint64_t)kcrc32c(s_hl_page, (size_t)TPM_HEADLESS_BLOB_LEN);
    d->producer_id = (uint32_t)BOOT_PRODUCER_KERNEL_TEST;
    return 1;
}

static void hl_free_handoff(void)
{
    if (s_hl_info) {
        pmm_free_contiguous((uintptr_t)s_hl_info,
                            (sizeof(struct boot_info) + 4095u) / 4096u);
        s_hl_info = (struct boot_info *)0;
    }
}

/* Is the payload page reachable the way the TAKE reaches it?
 *
 * The take dereferences `phys_start` raw, which is the Phase-1 boot identity
 * map contract every payload consumer relies on. A unit test runs much later,
 * so whether that mapping is still live is a property of the machine and not of
 * this code. Probing it (rather than assuming it) is what keeps a failure here
 * readable: an unreachable alias is a SKIP with a reason, not a mysterious
 * red assertion about CRCs. */
static int hl_phys_alias_ok(void)
{
    uint64_t phys = mm_image_virt_to_phys(s_hl_page);
    const volatile uint8_t *alias;
    uint32_t i;

    if (phys == 0u || phys >= BOOT_INFO_EARLY_MAP_END)
        return 0;
    alias = (const volatile uint8_t *)(uintptr_t)phys;
    for (i = 0; i < 8u; i++)
        if (alias[i] != s_hl_page[i])
            return 0;
    return 1;
}

static int hl_page_intact(void)
{
    uint32_t i;
    for (i = 0; i < TPM_HEADLESS_BLOB_LEN; i++)
        if (s_hl_page[i] != (uint8_t)(0x31u + (i & 0x3Fu)))
            return 0;
    return 1;
}

static void test_ha_take_refuses_without_capability(void)
{
    const uint8_t *blob = (const uint8_t *)1;   /* poisoned, must be cleared */
    const uint32_t ok_flags = BOOT_PAYLOAD_FLAG_VALID |
                              BOOT_PAYLOAD_FLAG_RESERVED |
                              BOOT_PAYLOAD_FLAG_CHECKSUMMED;
    uint32_t len;

    /* Capability ABSENT, everything else impeccable: RESERVED is set, the CRC
     * is right, the length is exact. Only the negotiated bit is missing, and
     * that alone must stop the range being read or wiped -- because the PMM
     * reservation pass skipped it for the same reason. */
    boot_headless_authz_reset_for_test();
    if (!hl_build_handoff(0u, ok_flags)) {
        TEST_SKIP("no memory for the synthetic handoff");
        return;
    }
    /* HARD PRECONDITION, not an optimization. Without a live alias the wipe
     * this test is looking for lands somewhere else, so "the page is intact"
     * would be true whether the guard fired or not -- MEASURED: with the
     * capability branch deleted the suite stayed green. A test that cannot
     * observe its guard must SKIP, not report a pass it did not earn. The
     * classifier control below runs regardless and needs no alias. */
    if (!hl_phys_alias_ok()) {
        const struct boot_payload_desc *d = &s_hl_info->payload_descriptors[0];
        TEST_ASSERT_EQ(boot_headless_authz_classify(0u, ok_flags,
                                                    d->phys_start, d->length),
                       BOOT_HL_AUTHZ_CAP_ABSENT,
                       "the take's own descriptor classifies CAP_ABSENT");
        TEST_ASSERT_EQ(boot_headless_authz_classify(
                           (uint32_t)BOOT_CAP_PAYLOAD_DESCRIPTORS, ok_flags,
                           d->phys_start, d->length),
                       BOOT_HL_AUTHZ_USABLE,
                       "and USABLE once the capability is negotiated");
        hl_free_handoff();
        TEST_SKIP("the boot identity map no longer aliases the test page, so "
                  "the take's own read/wipe cannot be observed here; an "
                  "observable payload-consumer seam is tracked with the "
                  "early-entropy seed handoff work");
        return;
    }
    len = boot_headless_authz_take_from(s_hl_info, &blob);

    TEST_ASSERT_EQ(len, 0u, "an unnegotiated handoff yields no authorization");
    TEST_ASSERT_EQ(blob == (const uint8_t *)0, 1,
                   "the output pointer is cleared, never left poisoned");
    TEST_ASSERT_EQ(hl_page_intact(), 1,
                   "the range is NEITHER read nor wiped when the capability is "
                   "absent -- those frames may already belong to the allocator");
    TEST_ASSERT_EQ((s_hl_info->payload_descriptors[0].flags &
                    (uint32_t)BOOT_PAYLOAD_FLAG_VALID) == 0u, 1,
                   "the descriptor is still retired, so it cannot be re-presented");

    /* THE CONTROL, over the descriptor the take actually saw: with the
     * capability negotiated the very same fields classify USABLE, so the
     * refusal above is caused by the capability bit and by nothing else about
     * this fixture. This is the control rather than a second take because the
     * accepting path has to dereference a physical alias, which a Phase-3 test
     * cannot rely on (the two cases below SKIP when it is absent). */
    {
        const struct boot_payload_desc *d = &s_hl_info->payload_descriptors[0];
        TEST_ASSERT_EQ(boot_headless_authz_classify(0u, ok_flags,
                                                    d->phys_start, d->length),
                       BOOT_HL_AUTHZ_CAP_ABSENT,
                       "the take's own descriptor classifies CAP_ABSENT");
        TEST_ASSERT_EQ(boot_headless_authz_classify(
                           (uint32_t)BOOT_CAP_PAYLOAD_DESCRIPTORS, ok_flags,
                           d->phys_start, d->length),
                       BOOT_HL_AUTHZ_USABLE,
                       "and USABLE once the capability is negotiated -- so the "
                       "capability is the whole difference");
    }
    hl_free_handoff();
}

static void test_ha_take_accepts_negotiated_handoff(void)
{
    const uint8_t *blob = (const uint8_t *)0;
    const uint32_t ok_flags = BOOT_PAYLOAD_FLAG_VALID |
                              BOOT_PAYLOAD_FLAG_RESERVED |
                              BOOT_PAYLOAD_FLAG_CHECKSUMMED;
    uint32_t len, again;
    uint8_t first_byte = 0u;

    /* The CONTROL for the case above: the identical descriptor with the
     * capability negotiated IS consumed, so the refusal there is caused by the
     * capability and not by the fixture being unusable. */
    boot_headless_authz_reset_for_test();
    if (!hl_build_handoff((uint32_t)BOOT_CAP_PAYLOAD_DESCRIPTORS, ok_flags)) {
        TEST_SKIP("no memory for the synthetic handoff");
        return;
    }
    if (!hl_phys_alias_ok()) {
        hl_free_handoff();
        TEST_SKIP("the boot identity map no longer aliases the test page; the "
                  "capability-refusal case above still runs and needs no alias");
        return;
    }
    len = boot_headless_authz_take_from(s_hl_info, &blob);
    if (blob)
        first_byte = blob[0];

    TEST_ASSERT_EQ(len, (uint32_t)TPM_HEADLESS_BLOB_LEN,
                   "a negotiated handoff yields the whole blob");
    TEST_ASSERT_EQ(blob != (const uint8_t *)0, 1, "the blob pointer is returned");
    TEST_ASSERT_EQ((uint32_t)first_byte, 0x31u,
                   "the returned bytes are the payload's, copied out before the "
                   "page was wiped");
    TEST_ASSERT_EQ(hl_page_intact(), 0,
                   "an accepted payload's page is wiped after the copy");
    TEST_ASSERT_EQ((s_hl_info->payload_descriptors[0].flags &
                    (uint32_t)BOOT_PAYLOAD_FLAG_VALID) == 0u, 1,
                   "an accepted descriptor is retired too");

    /* ONE-SHOT: a second take returns nothing even with the same handoff. */
    blob = (const uint8_t *)1;
    again = boot_headless_authz_take_from(s_hl_info, &blob);
    TEST_ASSERT_EQ(again, 0u, "the take is one-shot");
    TEST_ASSERT_EQ(blob == (const uint8_t *)0, 1,
                   "a second take clears the output pointer too");
    boot_headless_authz_reset_for_test();
    hl_free_handoff();
}

static void test_ha_take_refuses_corrupt_payload(void)
{
    const uint8_t *blob = (const uint8_t *)1;
    const uint32_t ok_flags = BOOT_PAYLOAD_FLAG_VALID |
                              BOOT_PAYLOAD_FLAG_RESERVED |
                              BOOT_PAYLOAD_FLAG_CHECKSUMMED;
    uint32_t len;

    /* A CRC that does not match the bytes. The range IS addressable and
     * reserved here, so unlike the capability case it may be read and wiped --
     * what must not happen is that it is handed on as an authorization. */
    boot_headless_authz_reset_for_test();
    if (!hl_build_handoff((uint32_t)BOOT_CAP_PAYLOAD_DESCRIPTORS, ok_flags)) {
        TEST_SKIP("no memory for the synthetic handoff");
        return;
    }
    if (!hl_phys_alias_ok()) {
        hl_free_handoff();
        TEST_SKIP("the boot identity map no longer aliases the test page");
        return;
    }
    s_hl_info->payload_descriptors[0].checksum ^= 0xFFull;
    len = boot_headless_authz_take_from(s_hl_info, &blob);

    TEST_ASSERT_EQ(len, 0u, "a CRC mismatch yields no authorization");
    TEST_ASSERT_EQ(blob == (const uint8_t *)0, 1, "and no pointer");
    TEST_ASSERT_EQ((s_hl_info->payload_descriptors[0].flags &
                    (uint32_t)BOOT_PAYLOAD_FLAG_VALID) == 0u, 1,
                   "a corrupt descriptor is retired rather than left live");
    boot_headless_authz_reset_for_test();
    hl_free_handoff();
}


/* A descriptor whose length is wrong must be refused WITHOUT its range being
 * read or wiped. The earlier shape assigned the payload for this class and then
 * wiped `d->length` bytes -- a field the classifier had just rejected, bounded
 * only by the 4 GiB map -- so a descriptor claiming gigabytes would have zeroed
 * gigabytes. Unlike the accept case this needs no live identity alias: the
 * whole point is that nothing is dereferenced. */
static void test_ha_take_refuses_bad_length_untouched(void)
{
    const uint8_t *blob = (const uint8_t *)1;
    const uint32_t ok_flags = BOOT_PAYLOAD_FLAG_VALID |
                              BOOT_PAYLOAD_FLAG_RESERVED |
                              BOOT_PAYLOAD_FLAG_CHECKSUMMED;
    uint32_t len;

    boot_headless_authz_reset_for_test();
    if (!hl_build_handoff((uint32_t)BOOT_CAP_PAYLOAD_DESCRIPTORS, ok_flags)) {
        TEST_SKIP("no memory for the synthetic handoff");
        return;
    }
    /* Same hard precondition as the other take cases, and MEASURED to be
     * necessary: with the payload assignment restored for this class the suite
     * stayed green, because without a live alias the wipe lands somewhere that
     * is not this page. The assertion below is correct and load-bearing where
     * the alias exists; here it must skip rather than claim a pass. */
    if (!hl_phys_alias_ok()) {
        hl_free_handoff();
        TEST_SKIP("the boot identity map no longer aliases the test page; an "
                  "observable payload-consumer seam is tracked with the "
                  "early-entropy seed handoff work");
        return;
    }
    /* A LARGE wrong length, which is the dangerous shape rather than an
     * off-by-one: if the refusal ever reads or wipes `d->length` bytes this is
     * the case that destroys memory. The address stays the test's own page. */
    s_hl_info->payload_descriptors[0].length = 0x40000000ull;   /* 1 GiB */
    len = boot_headless_authz_take_from(s_hl_info, &blob);

    TEST_ASSERT_EQ(len, 0u, "a wrong-length payload yields no authorization");
    TEST_ASSERT_EQ(blob == (const uint8_t *)0, 1, "and no pointer");
    TEST_ASSERT_EQ(hl_page_intact(), 1,
                   "a wrong-length payload is refused with its range NEVER "
                   "read or wiped");
    TEST_ASSERT_EQ((s_hl_info->payload_descriptors[0].flags &
                    (uint32_t)BOOT_PAYLOAD_FLAG_VALID) == 0u, 1,
                   "and it is still retired");
    boot_headless_authz_reset_for_test();
    hl_free_handoff();
}


void test_register_tpm_headless_authz(void)
{
    test_suite_register_cat("tpm headless: accept path",
                            test_ha_accept, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: wrong device",
                            test_ha_wrong_device, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: wrong transition",
                            test_ha_wrong_transition, TEST_CAT_SECURITY);
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
    test_suite_register_cat("tpm headless: digest covers non-PCR fields",
                            test_ha_canon_covers_non_pcr_fields, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: digest excludes derived fields",
                            test_ha_canon_excludes_derived_fields, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: transition binds the predecessor",
                            test_ha_transition_binds_predecessor, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: non-PCR change refused",
                            test_ha_non_pcr_change_refused, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: digest known answers",
                            test_ha_digest_known_answers, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: digest covers every field",
                            test_ha_digest_every_field_covered, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: authorize spends no TPM on refusal",
                            test_ha_authorize_spends_no_tpm_on_refusal, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: precheck-installed matches",
                            test_ha_precheck_installed_matches, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: prepare spends no TPM on refusal",
                            test_ha_prepare_spends_no_tpm_on_refusal, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: take refuses without capability",
                            test_ha_take_refuses_without_capability, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: take accepts negotiated handoff",
                            test_ha_take_accepts_negotiated_handoff, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: take refuses bad length untouched",
                            test_ha_take_refuses_bad_length_untouched, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: take refuses corrupt payload",
                            test_ha_take_refuses_corrupt_payload, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: payload class labels",
                            test_ha_class_labels, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: payload classification",
                            test_ha_payload_classification, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm headless: enroll-headless arg guards",
                            test_ha_enroll_headless_arg_guards, TEST_CAT_SECURITY);
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
