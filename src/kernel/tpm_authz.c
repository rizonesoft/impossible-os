/* ============================================================================
 * tpm_authz.c -- the authorization boundary for NV record writes
 *
 * See tpm_authz.h for the threat model and the write-then-increment argument.
 * This file is the orchestration: the compiled enrollment manifest, the offline
 * authority, and the one bounded sequence a whole transition runs in.
 * ========================================================================= */

#include "kernel/types.h"
#include "kernel/tpm.h"
#include "kernel/tpm_authz.h"
#include "kernel/tpm_nv.h"
#include "kernel/tpm_record.h"
#include "kernel/tpm_transport.h"
#include "kernel/tpm_budget.h"
#include "kernel/crypto/sha256.h"
#include "kernel/klog.h"
#include "libc/string.h"

/* nonceCaller for the sessions this module opens. It is not the authorization:
 * a policy session's authority comes from the assertions satisfied on it, and
 * the auth area carries an empty HMAC. The same fixed pattern is what the
 * PolicyPCR session path already uses, and diverging here would imply a
 * different security property than that path actually has. */
#define AUTHZ_NONCE_FILL 0xA5u
#define AUTHZ_NONCE_LEN  16u

/* A trial PolicyAuthorize needs an approvedPolicy to compare against the
 * session's current policyDigest, which on a fresh session is the zero digest.
 * Passing the zero digest therefore matches whether or not the TPM checks it in
 * a trial session, so the computation does not depend on that behaviour. */
#define AUTHZ_ZERO_DIGEST_LEN SHA256_DIGEST_LEN

/* The compiled enrollment manifest. THIS is the authority on index identity.
 * Nothing here is read from the device or from storage, which is the property
 * that stops a swapped stored contract from redirecting a verified read to an
 * index an attacker defined to match it.
 *
 * `attrs` is normalized (TPMA_NV_STATUS_MASK cleared) exactly as
 * struct tpm_nv_identity requires, and expect_written records that the anchor
 * is written once enrolled, which is what turns a byte-identical redefinition
 * into a detectable recreation. */
static const struct tpm_enroll_entry s_manifest[] = {
    /* The A/B floor: an ordinary DATA index, POLICY-writable and owner-readable.
     * Readable without the policy on purpose -- every boot must be able to read
     * the floor, and only an authorized update may move it. */
    { TPM_NV_INDEX_AB_FLOOR, TPM_ALG_SHA256,
      TPMA_NV_POLICYWRITE | TPMA_NV_OWNERREAD | TPMA_NV_NO_DA,
      (uint16_t)TPM_AB_FLOOR_RECORD_LEN, 1u, (uint8_t)TPM_ENROLL_POLICY_AUTHORIZE },

    /* The baseline bind record: same shape, different payload. */
    { TPM_NV_INDEX_BASELINE_BIND, TPM_ALG_SHA256,
      TPMA_NV_POLICYWRITE | TPMA_NV_OWNERREAD | TPMA_NV_NO_DA,
      (uint16_t)TPM_BASELINE_BIND_LEN, 1u, (uint8_t)TPM_ENROLL_POLICY_AUTHORIZE },

    /* The two counters. They are anchors in their own right, not bookkeeping:
     * each one's increment IS the irreversible commit point of a transition, so
     * they carry the same PolicyAuthorize boundary as the records they commit
     * and are OWNERREAD-only. Being in the manifest is what lets the transition
     * verify the counter's live identity before building a cpHash over its Name
     * -- a Name is only meaningful once the index behind it is the enrolled one.
     *
     * expect_written is 0, and that is not an oversight. A TPM_NT_COUNTER has
     * TPMA_NV_WRITTEN CLEAR until its FIRST increment, so a freshly provisioned
     * anchor legitimately reads unwritten; demanding written here would refuse
     * every machine between enrollment and its first authorized transition.
     * The recreation detector these anchors rely on is not WRITTEN anyway: it
     * is that a redefined counter cannot restart below the highest value any NV
     * counter has held over the TPM's lifetime. */
    { TPM_NV_INDEX_AB_SEQ, TPM_ALG_SHA256,
      TPMA_NV_POLICYWRITE | TPMA_NV_OWNERREAD | TPMA_NV_TYPE(TPM_NT_COUNTER) |
      TPMA_NV_NO_DA,
      (uint16_t)TPM_NV_COUNTER_SIZE, 0u, (uint8_t)TPM_ENROLL_POLICY_AUTHORIZE },

    { TPM_NV_INDEX_BASELINE_GEN, TPM_ALG_SHA256,
      TPMA_NV_POLICYWRITE | TPMA_NV_OWNERREAD | TPMA_NV_TYPE(TPM_NT_COUNTER) |
      TPMA_NV_NO_DA,
      (uint16_t)TPM_NV_COUNTER_SIZE, 0u, (uint8_t)TPM_ENROLL_POLICY_AUTHORIZE },

    /* The headless-enrollment replay anchor. OWNER-writable and
     * TPM_ENROLL_POLICY_NONE, which is the one entry here that carries no
     * PolicyAuthorize boundary -- see the handle's own comment in tpm_nv.h for
     * why owner-write is safe for a value that authorizes nothing by itself.
     * It is in the manifest for the OTHER half of the contract: without an
     * enrolled identity, a read of its value would trust whatever index
     * happens to answer at that handle. */
    { TPM_NV_INDEX_HEADLESS_SEQ, TPM_ALG_SHA256,
      TPMA_NV_OWNERWRITE | TPMA_NV_OWNERREAD | TPMA_NV_TYPE(TPM_NT_COUNTER) |
      TPMA_NV_NO_DA,
      (uint16_t)TPM_NV_COUNTER_SIZE, 0u, (uint8_t)TPM_ENROLL_POLICY_NONE },
};

#define AUTHZ_MANIFEST_COUNT ((uint32_t)(sizeof s_manifest / sizeof s_manifest[0]))

/* A relation between two headers' constants is a compile-time claim. It used to
 * be a runtime `if` that compared 32 against 64 and could never fire, which
 * reads as defensive and defends nothing. */
_Static_assert(SHA256_DIGEST_LEN <= TPM_NV_POLICY_MAX,
               "an authPolicy digest must fit the contract's policy buffer");

/* The installed authority. NULL public area = unprovisioned = every authorized
 * write refused. Written once during Phase-1 provisioning and read-only
 * thereafter; it is not touched from an interrupt and no AP mutates it. */
static struct tpm_authz_authority s_authority;

tpm_nv_status_t tpm_authz_set_authority(const struct tpm_authz_authority *auth)
{
    /* ONE-WAY. Both tpm_baseline_enroll's refusal and tpm_baseline_verify's
     * bind check key off tpm_authz_provisioned(), so a caller that could clear
     * or replace the authority could turn the entire boundary off at runtime --
     * which would make every guarantee above conditional on nobody reaching
     * this setter. An earlier revision only ASSERTED write-once in a comment.
     * Replacing a live authority is a key-rotation question with its own
     * authorization, not a plain store. */
    if (tpm_authz_provisioned())
        return TPM_NV_AUTH;
    if (!auth)
        return TPM_NV_OK;   /* already unprovisioned; nothing to clear */
    /* A non-NULL authority with nothing in it is a MISCONFIGURATION, not a
     * request to unprovision: accepting it would install an authority whose
     * Name is the hash of zero bytes and quietly make every grant check
     * meaningless. Unprovisioning has its own spelling above. */
    if (!auth->public_area || auth->public_len == 0u)
        return TPM_NV_BADARG;
    /* TPMT_PUBLIC begins type(2) || nameAlg(2) || objectAttributes(4), so a
     * public area too short to carry nameAlg cannot yield a Name at all. */
    if (auth->public_len < 8u)
        return TPM_NV_BADARG;
    if (!auth->policy_ref && auth->policy_ref_len != 0u)
        return TPM_NV_BADARG;
    /* Publish the pointer LAST, with a release store. Everything a reader needs
     * is written first, so a reader that observes a non-NULL public_area also
     * observes the length and policyRef that describe it -- a plain multi-word
     * struct assignment could be observed torn, with a live pointer beside a
     * stale length. */
    s_authority.public_len = auth->public_len;
    s_authority.policy_ref = auth->policy_ref;
    s_authority.policy_ref_len = auth->policy_ref_len;
    __atomic_store_n(&s_authority.public_area, auth->public_area, __ATOMIC_RELEASE);
    return TPM_NV_OK;
}

#ifdef KERNEL_TESTS
void tpm_authz_test_clear_authority(void)
{
    /* Test-only teardown. The production setter is one-way on purpose, so a
     * suite that installs an authority needs an explicit way back to the
     * unprovisioned state; putting it behind KERNEL_TESTS keeps that door shut
     * in a shipping kernel. */
    __atomic_store_n(&s_authority.public_area, (const uint8_t *)0, __ATOMIC_RELEASE);
    s_authority.public_len = 0u;
    s_authority.policy_ref = 0;
    s_authority.policy_ref_len = 0u;
}
#endif

int tpm_authz_provisioned(void)
{
    /* ACQUIRE against the release store below. The pointer is the publication
     * word: a reader that sees it must also see the length and policyRef that
     * were stored before it, or it would hash the wrong number of bytes into
     * the authority Name. tpm_seal.c solves the identical
     * set-once-at-boot/read-later lifecycle the same way. */
    return (__atomic_load_n(&s_authority.public_area, __ATOMIC_ACQUIRE) != 0) ? 1 : 0;
}

tpm_nv_status_t tpm_authz_authority_name(uint8_t *out, uint16_t cap,
                                         uint16_t *out_len)
{
    uint16_t name_alg;
    uint8_t digest[SHA256_DIGEST_LEN];
    uint16_t body;

    if (!out || !out_len)
        return TPM_NV_BADARG;
    if (!tpm_authz_provisioned())
        return TPM_NV_UNAVAIL;

    /* Name := nameAlg || H_nameAlg(publicArea) (TPM 2.0 Part 1, Name of a
     * transient object), wrapped in the TPM2B the wire wants. */
    name_alg = tpm2_be16_get(s_authority.public_area + 2u);
    /* Only SHA-256 is supported, and a public area naming anything else is
     * refused rather than hashed with the wrong algorithm: the resulting Name
     * would be a plausible 32 bytes that the TPM never agrees with, and the
     * only symptom would be a policy failure with nothing naming the cause.
     * TPM_ALG_NULL is refused for a different reason -- Part 1 section 26 gives
     * such an object NO Name, and PolicyAuthorize binds exactly that Name. */
    if (name_alg != TPM_ALG_SHA256)
        return TPM_NV_BADARG;

    /* The BODY only. Every wire builder adds the TPM2B size prefix itself, and
     * tpm2_parse_load_external strips it, so prefixing here would double it. */
    body = (uint16_t)(2u + SHA256_DIGEST_LEN);
    if (cap < body)
        return TPM_NV_BADARG;

    sha256(s_authority.public_area, s_authority.public_len, digest);
    tpm2_be16_put(out, name_alg);
    memcpy(out + 2, digest, SHA256_DIGEST_LEN);
    *out_len = body;
    return TPM_NV_OK;
}

const struct tpm_enroll_entry *tpm_enroll_lookup(uint32_t nv_index)
{
    uint32_t i;
    for (i = 0u; i < AUTHZ_MANIFEST_COUNT; i++)
        if (s_manifest[i].nv_index == nv_index)
            return &s_manifest[i];
    return 0;
}

/* ---- authPolicy derivation (TRIAL session; no hardcoded digest formula) ---- */

struct authz_policy_ctx {
    const uint8_t  *key_name;
    uint16_t        key_name_len;
    uint8_t        *out;
    uint32_t        cap;
    tpm_nv_status_t st;
};

/* Count of allocating commands this kernel abandoned with an UNKNOWN outcome:
 * the TPM may hold a session or a transient object whose handle nobody could
 * read. ATOMIC because the authorized paths run under a transport sequence but
 * this counter is read from outside one, and a torn read would understate a
 * leak. RELAXED is the right order and not a shortcut: this word publishes no
 * other data, so it carries no release obligation -- contrast the ACQUIRE and
 * RELEASE pair on s_authority.public_area below, which gates the length and
 * policyRef stored beside it.
 *
 * It never decreases in a shipping kernel; the reset beside the accessor is
 * KERNEL_TESTS-only, so a suite can assert a count rather than a running total.
 * IN PRODUCTION THE COUNTER HAS NO READER: the operator-facing signal is the
 * klog warning raised at the same moment, and this exists so a test can assert
 * the CLASSIFICATION -- above all that an ordinary refusal is not counted.
 * Surfacing an accumulated total in the boot-integrity report would be a
 * genuine improvement and is not what this section claims to have done. */
static uint32_t s_authz_unknown_alloc;

/* EXEC-failure accounting for a command that may have allocated a handle.
 *
 * The PARSE-failure branch of each of these commands recovers the raw handle
 * and flushes it, because the TPM reported success and the reply merely failed
 * a strict parser. The EXEC-failure branch cannot do that: either the TPM
 * refused (nothing exists) or nothing readable came back (nothing is nameable).
 * So the available action is to CLASSIFY, and to refuse to read the unknown
 * case as clean.
 *
 * `raw_rc` MUST have been reset to TPM_NV_RC_UNSET immediately before the
 * submission whose failure is being classified; a value left by an earlier
 * command in the same flow would classify this failure on somebody else's
 * evidence, and TPM2_RC_SUCCESS is 0, so a zero-initialized slot silently reads
 * as "executed successfully" rather than as "no answer".
 *
 * A pre-dispatch refusal (the cumulative budget expiring before the command
 * reached the transport) submitted nothing and is as safe as ordinary gate
 * contention, which is why the dispatch check is part of the condition rather
 * than an afterthought. Returns 1 when a handle MAY be outstanding. */
static int authz_alloc_failure_note(uint32_t raw_rc, const char *what)
{
    if (tpm_nv_outcome_is_definite_refusal(raw_rc))
        return 0;                     /* the TPM refused it; nothing allocated */
    if (!tpm2_seq_last_submit_dispatched())
        return 0;                     /* never reached the TPM at all */
    __atomic_fetch_add(&s_authz_unknown_alloc, 1u, __ATOMIC_RELAXED);
    /* Same trade the NV layer makes: an unnameable handle is a leak that
     * reports itself once the pool runs out, and disabling the transport over
     * it would break unrelated TPM use without recovering anything. */
    klog(LOG_WARN, "TPM",
         "Authorized %s outcome UNKNOWN: a TPM slot may be held until reset",
         what);
    return 1;
}

#ifdef KERNEL_TESTS
uint32_t tpm_authz_test_unknown_alloc(void)
{
    return __atomic_load_n(&s_authz_unknown_alloc, __ATOMIC_RELAXED);
}

void tpm_authz_test_reset_unknown_alloc(void)
{
    __atomic_store_n(&s_authz_unknown_alloc, 0u, __ATOMIC_RELAXED);
}
#endif

/* PARSE-failure accounting for an allocating command the TPM reported SUCCESS
 * for. The caller has already tried to recover the raw handle and flush it;
 * this is the branch where it could not, so the resource exists and is
 * unreachable.
 *
 * No dispatch check and no rc classification here, unlike the exec-failure
 * note: a parsed SUCCESS means the command reached the TPM and ran, so the
 * allocation is not in doubt -- only its handle is. Silence here was the exact
 * shape of the gap this section set out to close, one branch further along. */
static void authz_unrecoverable_handle(const char *what)
{
    __atomic_fetch_add(&s_authz_unknown_alloc, 1u, __ATOMIC_RELAXED);
    klog(LOG_WARN, "TPM",
         "Authorized %s created but its handle is UNRECOVERABLE: the slot is "
         "held until reset", what);
}

/* Marshal the NULL TPMT_TK_VERIFIED a trial PolicyAuthorize takes:
 * tag || hierarchy || empty digest. */
static void authz_null_ticket(uint8_t t[TPM2_TK_VERIFIED_NULL_LEN])
{
    tpm2_be16_put(t + 0, TPM2_ST_VERIFIED);
    tpm2_be32_put(t + 2, TPM_RH_NULL);
    tpm2_be16_put(t + 6, 0u);
}

static int authz_policy_digest_seq(tpm2_seq_t seq, void *vctx)
{
    struct authz_policy_ctx *c = (struct authz_policy_ctx *)vctx;
    uint8_t cmd[512], rsp[256], nonce[AUTHZ_NONCE_LEN];
    uint8_t zero[AUTHZ_ZERO_DIGEST_LEN];
    uint8_t ticket[TPM2_TK_VERIFIED_NULL_LEN];
    uint32_t session, n, rlen = 0, rc = TPM_NV_RC_UNSET;
    tpm_nv_status_t st;
    int dlen;

    memset(nonce, AUTHZ_NONCE_FILL, sizeof nonce);
    memset(zero, 0, sizeof zero);
    authz_null_ticket(ticket);

    n = tpm2_build_start_auth_session(cmd, sizeof cmd, TPM2_SE_TRIAL,
                                      TPM_ALG_SHA256, nonce, sizeof nonce);
    if (n == 0u) { c->st = TPM_NV_BADARG; return 0; }
    rc = TPM_NV_RC_UNSET;
    if (tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, &rc) != 0) {
        /* The trial session is the one allocating command on this flow, so a
         * failure here is the only place it can be abandoned unnamed. */
        (void)authz_alloc_failure_note(rc, "trial session");
        c->st = st;
        return 0;
    }
    session = tpm2_parse_start_auth_session(rsp, rlen);
    if (session == 0u) {
        /* The TPM reported success, so a session may exist behind a reply this
         * parser rejected. Recover the raw handle for cleanup rather than
         * leaking it for the rest of the boot. */
        uint32_t leaked = tpm2_rsp_session_handle(rsp, rlen);
        if (leaked != 0u)
            tpm_nv_flush_handle(seq, leaked);
        else
            authz_unrecoverable_handle("trial session");
        c->st = TPM_NV_TRANSPORT;
        return 0;
    }

    c->st = TPM_NV_TRANSPORT;
    n = tpm2_build_policy_authorize(cmd, sizeof cmd, session,
                                    zero, (uint16_t)sizeof zero,
                                    s_authority.policy_ref,
                                    s_authority.policy_ref_len,
                                    c->key_name, c->key_name_len,
                                    ticket, (uint32_t)sizeof ticket);
    if (n == 0u) {
        c->st = TPM_NV_BADARG;
        goto flush;
    }
    if (tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, &rc) != 0) {
        c->st = st;
        goto flush;
    }

    n = tpm2_build_policy_get_digest(cmd, sizeof cmd, session);
    if (n == 0u) {
        c->st = TPM_NV_BADARG;
        goto flush;
    }
    if (tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, &rc) != 0) {
        c->st = st;
        goto flush;
    }
    dlen = tpm2_parse_policy_get_digest(rsp, rlen, c->out, c->cap);
    /* An authPolicy of the wrong LENGTH is not a short answer to round up: it
     * would be written into a define whose index nobody could ever satisfy. */
    c->st = (dlen == (int)SHA256_DIGEST_LEN) ? TPM_NV_OK : TPM_NV_TRANSPORT;

flush:
    /* Single cleanup on every path, through the VERIFIED teardown: an abandoned
     * trial session otherwise holds a TPM session slot for the rest of the boot,
     * and a bare FlushContext on the work budget cannot even run once that
     * budget is spent. */
    tpm_nv_flush_handle(seq, session);
    return 0;
}

tpm_nv_status_t tpm_authz_contract(uint32_t nv_index, struct tpm_nv_identity *out)
{
    const struct tpm_enroll_entry *e;
    struct authz_policy_ctx c;
    uint8_t key_name[2u + SHA256_DIGEST_LEN];   /* Name BODY: nameAlg || digest */
    uint16_t key_name_len = 0;
    tpm_nv_status_t st;
    int r;

    if (!out)
        return TPM_NV_BADARG;
    e = tpm_enroll_lookup(nv_index);
    /* No manifest row means this kernel does not enroll that handle, so there
     * is no contract to derive and therefore no verified read to be had. That
     * is the answer, not an omission. */
    if (!e)
        return TPM_NV_BADARG;

    memset(out, 0, sizeof *out);
    out->nv_index       = e->nv_index;
    out->name_alg       = e->name_alg;
    out->attrs          = e->attrs;
    out->data_size      = e->data_size;
    out->expect_written = e->expect_written;
    out->policy_len     = 0u;

    if (e->policy_kind == (uint8_t)TPM_ENROLL_POLICY_NONE)
        return TPM_NV_OK;

    if (!tpm_authz_provisioned())
        return TPM_NV_UNAVAIL;

    st = tpm_authz_authority_name(key_name, (uint16_t)sizeof key_name, &key_name_len);
    if (st != TPM_NV_OK)
        return st;

    c.key_name = key_name;
    c.key_name_len = key_name_len;
    c.out = out->auth_policy;
    c.cap = TPM_NV_POLICY_MAX;
    c.st = TPM_NV_TRANSPORT;
    r = tpm2_seq_run(TPM_NV_OP_BUDGET_MS, TPM_NV_OP_CLEANUP_BUDGET_MS,
                     authz_policy_digest_seq, &c);
    if (r != 0)
        return TPM_NV_BUSY;
    if (c.st != TPM_NV_OK)
        return c.st;
    out->policy_len = (uint16_t)SHA256_DIGEST_LEN;
    return TPM_NV_OK;
}

tpm_nv_status_t tpm_authz_contract_cache_ok(const struct tpm_nv_identity *cache)
{
    struct tpm_nv_identity derived;
    tpm_nv_status_t st;
    uint16_t i;

    if (!cache)
        return TPM_NV_BADARG;
    st = tpm_authz_contract(cache->nv_index, &derived);
    if (st != TPM_NV_OK)
        return st;

    /* Field by field, not memcmp: struct padding is not part of the contract
     * and a memcmp over it would report a mismatch for two identical contracts
     * that were merely built by different code paths. */
    if (cache->nv_index != derived.nv_index) return TPM_NV_MISMATCH;
    if (cache->name_alg != derived.name_alg) return TPM_NV_MISMATCH;
    if (cache->attrs != derived.attrs) return TPM_NV_MISMATCH;
    if (cache->data_size != derived.data_size) return TPM_NV_MISMATCH;
    if (cache->expect_written != derived.expect_written) return TPM_NV_MISMATCH;
    if (cache->policy_len != derived.policy_len) return TPM_NV_MISMATCH;
    if (cache->policy_len > TPM_NV_POLICY_MAX) return TPM_NV_CONTRACT;
    for (i = 0u; i < cache->policy_len; i++)
        if (cache->auth_policy[i] != derived.auth_policy[i])
            return TPM_NV_MISMATCH;
    return TPM_NV_OK;
}

/* ---- The authorized transition ----
 *
 * Ordering is the design, not an implementation detail:
 *
 *   1. read the counter                  -- must equal what the grant assumed
 *   2. verify the index identity         -- the enrolled handle, not a lookalike
 *   3. write the record under the grant  -- OVERWRITES the sole record
 *   4. read it back                      -- the bytes are on the device, verbatim
 *   5. increment the counter             -- THE COMMIT POINT, irreversible, last
 *
 * WHAT A PRE-COMMIT FAILURE LEAVES BEHIND depends on how far it got, and the
 * four cases are not interchangeable:
 *
 *   - BEFORE the write lands (steps 1-2, command construction, or a DEFINITE
 *     refusal of the NV_Write): nothing was overwritten, so the previously
 *     committed record and its counter are intact. The update is lost; the
 *     floor is not.
 *   - AFTER a VERIFIED readback, with the increment either NOT SUBMITTED or
 *     TRUSTWORTHILY REFUSED: the record's generation is one ahead of the
 *     counter, readers treat it as not-current, and there is NO usable fallback
 *     -- step 3 overwrote the only record there is, so the previously committed
 *     bytes are gone. The machine fails closed and needs the direction-specific
 *     recovery. Two boundaries matter here. The READBACK, not the write, opens
 *     this case: NV_Write reporting success is not proof the bytes landed,
 *     which is the whole reason step 4 exists. And a REFUSED increment, not
 *     merely an unconfirmed one, keeps it: an increment whose outcome is
 *     unknown belongs to the last case below, because it may already have
 *     executed and left the pairing CURRENT -- acting on this case there would
 *     issue a second increment and drive a current pairing to counter-ahead,
 *     which is the destructive direction.
 *   - WRITE reported success but the readback FAILED or DIFFERED: the stored
 *     bytes may be the candidate, corrupt, or still the previous record. Neither
 *     the generation nor the availability of a fallback is established.
 *   - UNKNOWN outcome OF EITHER STATE-CHANGING COMMAND, the write or the
 *     increment. Unknown means ANY outcome that is neither a trustworthy
 *     refusal nor a validated success -- no response header parsed, and equally
 *     a parsed TPM2_RC_SUCCESS whose session envelope failed validation, which
 *     the executor reports as TRANSPORT. The two commands are NOT symmetric
 *     here: an unknown WRITE leaves the record unknown and the counter
 *     unattempted, while an unknown INCREMENT means the record is established
 *     and only the counter is in doubt, so the pairing may already be CURRENT.
 *     Re-read both anchors either way, and never assume the increment did not
 *     land.
 *
 * This comment used to flatten every case into "costs an update, never the
 * floor", which is false for the second; a first correction flattened them the
 * other way, which is false for the first and the last; and a second correction
 * still treated a reported write as a confirmed one. Each flattening read as
 * tidier than the truth, which is why the cases are enumerated rather than
 * summarized.
 *
 * The ordering is still the design: the reverse order would advance the
 * irreversible half first and leave the machine committed to bytes that may
 * never have been written, which is worse than any of the four above. */

struct authz_write_ctx {
    uint32_t        nv_index;
    uint32_t        counter_index;
    const uint8_t  *record;
    uint16_t        record_len;
    uint64_t        expect_counter;
    const struct tpm_authz_transition *tr;
    const struct tpm_nv_identity      *contract;
    const struct tpm_nv_identity      *counter_contract;
    const uint8_t  *key_name;
    uint16_t        key_name_len;
    tpm_nv_status_t st;
};

/* Open a policy session, satisfy PolicyCommandCode + PolicyCpHash + PolicyNV,
 * then PolicyAuthorize it with the authority's ticket. On success *out_session
 * is a session the TPM will accept for exactly the command the cpHash names.
 *
 * `cphash` MUST have been computed over the LIVE index Name: TPMA_NV_WRITTEN is
 * inside the hashed public area, so an index's Name changes on its first write
 * and an enrollment-time Name authorizes nothing afterwards. */
static tpm_nv_status_t authz_open_authorized_session(tpm2_seq_t seq,
                                                     const struct authz_write_ctx *c,
                                                     const struct tpm_authz_grant *g,
                                                     uint32_t command_code,
                                                     const uint8_t cphash[32],
                                                     const uint8_t *operand,
                                                     uint16_t operand_len,
                                                     uint32_t *out_session)
{
    uint8_t cmd[768], rsp[512], nonce[AUTHZ_NONCE_LEN];
    uint8_t ticket[TPM2_TK_VERIFIED_MAX_LEN];
    uint8_t ahash[SHA256_DIGEST_LEN];
    uint32_t ticket_len = 0, key_handle = 0;
    uint8_t key_name_tpm[2u + SHA256_DIGEST_LEN];
    uint16_t key_name_tpm_len = 0;
    uint32_t session = 0, n, rlen = 0, rc = 0;
    tpm_nv_status_t st;

    memset(nonce, AUTHZ_NONCE_FILL, sizeof nonce);
    *out_session = 0u;

    /* Load the authority's public area so the TPM can check its signature. The
     * hierarchy is named because it selects the proof value inside the ticket;
     * an unhierarchied load cannot produce one PolicyAuthorize will accept. */
    n = tpm2_build_load_external(cmd, sizeof cmd, s_authority.public_area,
                                 s_authority.public_len, TPM_RH_OWNER);
    if (n == 0u)
        return TPM_NV_BADARG;
    rc = TPM_NV_RC_UNSET;
    if (tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, &rc) != 0) {
        /* LoadExternal allocates a TRANSIENT OBJECT. The parse-failure branch
         * below recovers and flushes it; this branch has no reply to recover
         * from, so all it can do is classify and say so. */
        (void)authz_alloc_failure_note(rc, "authority key load");
        return st;
    }
    st = tpm2_parse_load_external(rsp, rlen, &key_handle,
                                  key_name_tpm, (uint16_t)sizeof key_name_tpm,
                                  &key_name_tpm_len);
    if (st != TPM_NV_OK) {
        /* The command SUCCEEDED and the TPM allocated an object; only the reply
         * failed to parse. Recover the raw handle so the slot is released --
         * returning here without flushing holds a transient slot until reboot,
         * and repeated failures exhaust the pool and disable authorization
         * entirely. */
        key_handle = tpm2_rsp_object_handle(rsp, rlen);
        if (key_handle == 0u)
            authz_unrecoverable_handle("authority key object");
        goto flush_key;
    }

    /* The TPM's Name for the loaded key must equal the one computed locally
     * from the SAME bytes. A disagreement means the compiled public area is not
     * what the TPM thinks it loaded, and continuing would bind the policy to a
     * key identity nobody chose. Both sides are Name BODIES here. */
    if (key_name_tpm_len != c->key_name_len ||
        memcmp(key_name_tpm, c->key_name, c->key_name_len) != 0) {
        st = TPM_NV_MISMATCH;
        goto flush_key;
    }

    /* aHash, NOT the approved policy. TPM2_PolicyAuthorize accepts a ticket
     * over H(approvedPolicy || policyRef); handing VerifySignature the bare
     * approved policy produces a ticket for a digest PolicyAuthorize never
     * asks about, so no grant could ever complete whichever of the two the
     * authority actually signed. */
    {
        struct sha256_ctx ah;
        sha256_init(&ah);
        sha256_update(&ah, g->approved_policy, g->approved_len);
        if (s_authority.policy_ref_len)
            sha256_update(&ah, s_authority.policy_ref, s_authority.policy_ref_len);
        sha256_final(&ah, ahash);
    }

    /* Turn the detached authority signature into a ticket. */
    n = tpm2_build_verify_signature(cmd, sizeof cmd, key_handle,
                                    ahash, g->signature, g->sig_len);
    if (n == 0u) { st = TPM_NV_BADARG; goto flush_key; }
    if (tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, &rc) != 0)
        goto flush_key;
    st = tpm2_parse_verify_signature(rsp, rlen, ticket, (uint32_t)sizeof ticket,
                                     &ticket_len);
    if (st != TPM_NV_OK)
        goto flush_key;

    n = tpm2_build_start_auth_session(cmd, sizeof cmd, TPM2_SE_POLICY,
                                      TPM_ALG_SHA256, nonce, sizeof nonce);
    if (n == 0u) { st = TPM_NV_BADARG; goto flush_key; }
    rc = TPM_NV_RC_UNSET;
    if (tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, &rc) != 0) {
        /* flush_key releases the object loaded ABOVE; it says nothing about the
         * session THIS command may have allocated, which is the gap the note
         * closes. Both matter: the object is nameable and gets flushed, the
         * session is not and gets reported. */
        (void)authz_alloc_failure_note(rc, "policy session");
        goto flush_key;
    }
    session = tpm2_parse_start_auth_session(rsp, rlen);
    if (session == 0u) {
        uint32_t leaked = tpm2_rsp_session_handle(rsp, rlen);
        if (leaked != 0u)
            tpm_nv_flush_handle(seq, leaked);
        else
            authz_unrecoverable_handle("policy session");
        st = TPM_NV_TRANSPORT;
        goto flush_key;
    }

    n = tpm2_build_policy_command_code(cmd, sizeof cmd, session, command_code);
    if (n == 0u) { st = TPM_NV_BADARG; goto flush_all; }
    if (tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, &rc) != 0)
        goto flush_all;

    n = tpm2_build_policy_cphash(cmd, sizeof cmd, session, cphash);
    if (n == 0u) { st = TPM_NV_BADARG; goto flush_all; }
    if (tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, &rc) != 0)
        goto flush_all;

    /* PolicyNV pins the generation the grant was issued against. Without it a
     * grant for one transition would satisfy the policy at any counter value,
     * and an old grant could be replayed after the counter moved on. */
    n = tpm2_build_policy_nv(cmd, sizeof cmd, c->counter_index, session,
                             operand, operand_len, 0u, TPM2_EO_EQ);
    if (n == 0u) { st = TPM_NV_BADARG; goto flush_all; }
    if (tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, &rc) != 0)
        goto flush_all;

    n = tpm2_build_policy_authorize(cmd, sizeof cmd, session,
                                    g->approved_policy, g->approved_len,
                                    s_authority.policy_ref, s_authority.policy_ref_len,
                                    c->key_name, c->key_name_len,
                                    ticket, ticket_len);
    if (n == 0u) { st = TPM_NV_BADARG; goto flush_all; }
    if (tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, &rc) != 0)
        goto flush_all;

    /* The loaded key has done its job the moment the ticket is consumed, so it
     * is flushed here rather than held for the rest of the sequence: TPMs have
     * very few transient object slots and the caller still has work to do. */
    tpm_nv_flush_handle(seq, key_handle);
    *out_session = session;
    return TPM_NV_OK;

flush_all:
    tpm_nv_flush_handle(seq, session);
flush_key:
    if (key_handle != 0u)
        tpm_nv_flush_handle(seq, key_handle);
    return st;
}

static int authz_write_seq(tpm2_seq_t seq, void *vctx)
{
    struct authz_write_ctx *c = (struct authz_write_ctx *)vctx;
    /* MEASURED FRAME, so nobody has to re-derive it: this function is 3112
     * bytes (llvm-objdump of the prologue), and the deepest authorized-write
     * chain is tpm_authz_write_record 312 + this 3112 + the authorized-session
     * helper 1544 = 4968 bytes before the transport frames.
     *
     * THE FIGURE IS LATENT, not a measurement of a shipping path: there is no
     * production caller of the authorized write at all today. The boot's only
     * enroll caller uses the UNAUTHENTICATED writer, and nothing outside the
     * tests reaches tpm_baseline_enroll_bound, tpm_ab_floor_advance or
     * tpm_authz_write_record. It is recorded now because the numbers are cheap
     * to take while the code is in hand and expensive to reconstruct later: a
     * Phase-1 caller would run on the 16 KiB BSP boot stack
     * (BSP_BOOT_STACK_SIZE, boot_hw.c) and use about a third, while a
     * KERNEL-THREAD caller gets TASK_STACK_SIZE (8 KiB) and the same chain is
     * about two thirds. Whichever arrives first, its stack is assessed then,
     * and the buffers move off the stack before a thread-context caller is
     * wired.
     *
     * rsp is sized against the RECORD SIZE THIS FUNCTION ADVERTISES, not
     * against the two record shapes that happen to be compiled in today.
     * tpm_authz_write_record accepts any record_len up to TPM_NV_MAX_DATA, and
     * the readback below is an NV_Read whose response is
     * header(10) + parameterSize(4) + TPM2B_MAX_NV_BUFFER(2 + record_len) +
     * a full one-session auth area (~69). At the maximum that is 528 bytes
     * before the auth area, so the previous 512 could not hold it: a manifest
     * grown past ~437 bytes would have surfaced as an unexplained transport
     * error rather than a named refusal. TPM_NV_MAX_DATA + 128 is the same
     * arithmetic the NV layer's own policy-op helper uses for this shape. */
    uint8_t cmd[1024], rsp[TPM_NV_MAX_RSP];
    uint8_t names[2u * (2u + SHA256_DIGEST_LEN)];
    uint8_t cnames[2u * (2u + SHA256_DIGEST_LEN)];
    uint8_t params[16u + TPM_NV_MAX_DATA];
    uint8_t cphash[SHA256_DIGEST_LEN];
    uint8_t operand[8];
    uint8_t readback[TPM_NV_MAX_DATA];
    struct tpm_nv_public pub;
    uint32_t session = 0, n, rlen = 0, rc = 0;
    uint32_t names_len, params_len, cnames_len;
    uint64_t counter = 0;
    tpm_nv_status_t st;
    int name_ok = 0, rb;
    uint16_t i;

    /* 1. The counter the grant assumed. Read INSIDE the sequence: a value read
     *    outside it could have moved before the write lands, which is exactly
     *    the interleaving the single sequence exists to prevent. */
    st = tpm_nv_read_counter_seq(seq, c->counter_index, &counter);
    if (st != TPM_NV_OK) { c->st = st; return 0; }
    if (counter != c->expect_counter) {
        /* Not a retry: the grant was signed for ONE transition and this is a
         * different one. */
        c->st = TPM_NV_MISMATCH;
        return 0;
    }

    /* 2. The live identity, and the live Name the cpHash must be built over. */
    st = tpm_nv_read_identity_seq(seq, c->nv_index, &pub, &name_ok);
    if (st != TPM_NV_OK) { c->st = st; return 0; }
    if (!name_ok) { c->st = TPM_NV_MISMATCH; return 0; }
    st = tpm_nv_identity_match(c->contract, &pub);
    if (st != TPM_NV_OK) { c->st = st; return 0; }

    /* THE LIFECYCLE CHECK, and it is not optional. tpm_nv_identity_match
     * normalizes TPMA_NV_WRITTEN away because it is not a definition property,
     * so a byte-identical destroy-and-recreate passes it: the definition IS the
     * enrolled one, because the attacker copied it. What separates the two is
     * that a freshly defined index reads back UNWRITTEN.
     *
     * Without this a read falls through to an NV_Read reporting UNINIT, and
     * UNINIT downstream means "no record yet" -- which is exactly how a
     * destroyed anchor gets laundered into a first enrollment. */
    if (c->contract->expect_written && !(pub.attrs & TPMA_NV_WRITTEN)) {
        c->st = TPM_NV_RECREATED;
        return 0;
    }

    /* cpHash covers commandCode || Name(authHandle) || Name(nvIndex) ||
     * parameters. For NV_Write both handles are the index itself, so its live
     * Name appears twice, and the parameters are the TPM2B data plus offset. */
    {
        int nl = tpm2_nv_name_compute(c->nv_index, &pub, names,
                                      (uint32_t)sizeof names / 2u);
        if (nl <= 0) { c->st = TPM_NV_TRANSPORT; return 0; }
        names_len = (uint32_t)nl;
    }
    if (names_len * 2u > (uint32_t)sizeof names) { c->st = TPM_NV_BADARG; return 0; }
    for (i = 0; i < (uint16_t)names_len; i++)
        names[names_len + i] = names[i];
    names_len *= 2u;

    params_len = 0u;
    tpm2_be16_put(params, c->record_len); params_len = 2u;
    for (i = 0; i < c->record_len; i++)
        params[params_len + i] = c->record[i];
    params_len += c->record_len;
    tpm2_be16_put(params + params_len, 0u); params_len += 2u;  /* offset */

    st = tpm2_cphash_compute(TPM2_CC_NV_WRITE, names, names_len,
                             params, params_len, cphash);
    if (st != TPM_NV_OK) { c->st = st; return 0; }

    /* The PolicyNV operand is the counter value the grant was issued against,
     * big-endian as the index stores it. */
    for (i = 0; i < 8u; i++)
        operand[i] = (uint8_t)((counter >> (56u - 8u * i)) & 0xFFu);

    st = authz_open_authorized_session(seq, c, &c->tr->write, TPM2_CC_NV_WRITE,
                                       cphash, operand, 8u, &session);
    if (st != TPM_NV_OK) { c->st = st; return 0; }

    /* 3. The write itself, under the session the authority authorized. */
    n = tpm2_build_nv_write(cmd, sizeof cmd, c->nv_index, c->nv_index, session,
                            0u, c->record, c->record_len);
    if (n == 0u) { c->st = TPM_NV_BADARG; goto flush; }
    if (tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, &rc) != 0) {
        c->st = st;
        goto flush;
    }

    /* The policy session is consumed by the command it authorized, so it is
     * flushed before the readback rather than reused. */
    tpm_nv_flush_handle(seq, session);
    session = 0u;

    /* 4. Read the bytes back before committing to them. A write the TPM
     *    reported as successful but stored differently would otherwise be
     *    committed by the increment and only discovered on the next boot. */
    n = tpm2_build_nv_read(cmd, sizeof cmd, TPM_RH_OWNER, c->nv_index, TPM_RS_PW,
                           c->record_len, 0u);
    if (n == 0u) { c->st = TPM_NV_BADARG; return 0; }
    if (tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, &rc) != 0) {
        c->st = st;
        return 0;
    }
    rb = tpm2_parse_nv_read(rsp, rlen, readback, (uint32_t)sizeof readback);
    if (rb != (int)c->record_len || memcmp(readback, c->record, c->record_len) != 0) {
        c->st = TPM_NV_MISMATCH;
        return 0;
    }

    /* 5. THE COMMIT POINT. Also authority-authorized: an increment reachable
     *    under ordinary owner auth would let an attacker advance the counter at
     *    will and manufacture the counter-ahead-of-record state deliberately.
     *
     *    The cpHash needs the COUNTER's live Name, not the record index's.
     *    NV_Increment carries counter_index in both authorization slots, so
     *    reusing the record's Name here computes a digest the TPM never agrees
     *    with and every commit is rejected -- a failure that only appears
     *    against firmware that actually executes the command. Its identity is
     *    verified against the manifest for the same reason the record index's
     *    is: a Name is only meaningful once the index behind it is the enrolled
     *    one. */
    {
        struct tpm_nv_public cpub;
        int cok = 0, cnl;
        st = tpm_nv_read_identity_seq(seq, c->counter_index, &cpub, &cok);
        if (st != TPM_NV_OK) { c->st = st; return 0; }
        if (!cok) { c->st = TPM_NV_MISMATCH; return 0; }
        st = tpm_nv_identity_match(c->counter_contract, &cpub);
        if (st != TPM_NV_OK) { c->st = st; return 0; }
        cnl = tpm2_nv_name_compute(c->counter_index, &cpub, cnames,
                                   (uint32_t)sizeof cnames / 2u);
        if (cnl <= 0) { c->st = TPM_NV_TRANSPORT; return 0; }
        cnames_len = (uint32_t)cnl;
        for (i = 0; i < (uint16_t)cnames_len; i++)
            cnames[cnames_len + i] = cnames[i];
        cnames_len *= 2u;
    }
    st = tpm2_cphash_compute(TPM2_CC_NV_INCREMENT, cnames, cnames_len, 0, 0u, cphash);
    if (st != TPM_NV_OK) { c->st = st; return 0; }
    st = authz_open_authorized_session(seq, c, &c->tr->commit, TPM2_CC_NV_INCREMENT,
                                       cphash, operand, 8u, &session);
    if (st != TPM_NV_OK) { c->st = st; return 0; }

    n = tpm2_build_nv_increment(cmd, sizeof cmd, c->counter_index,
                                c->counter_index, session);
    if (n == 0u) { c->st = TPM_NV_BADARG; goto flush; }
    if (tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, &rc) != 0) {
        c->st = st;
        goto flush;
    }
    c->st = TPM_NV_OK;

flush:
    if (session != 0u)
        tpm_nv_flush_handle(seq, session);
    return 0;
}

static int grant_ok(const struct tpm_authz_grant *g)
{
    return g && g->approved_policy && g->approved_len == SHA256_DIGEST_LEN &&
           g->signature && g->sig_len != 0u;
}

tpm_nv_status_t tpm_authz_grant_wellformed(const struct tpm_authz_transition *tr)
{
    /* Exported so a consumer whose first act is IRREVERSIBLE can check the
     * grant before it mutates anything, rather than discovering the grant was
     * malformed after the write it cannot take back. */
    if (!tr || !grant_ok(&tr->write) || !grant_ok(&tr->commit))
        return TPM_NV_BADARG;
    return TPM_NV_OK;
}

tpm_nv_status_t tpm_authz_write_record(uint32_t nv_index, uint32_t counter_index,
                                       const uint8_t *record, uint16_t record_len,
                                       uint64_t expect_counter,
                                       const struct tpm_authz_transition *tr)
{
    struct authz_write_ctx c;
    struct tpm_nv_identity contract;
    struct tpm_nv_identity counter_contract;
    uint8_t key_name[2u + SHA256_DIGEST_LEN];   /* Name BODY: nameAlg || digest */
    uint16_t key_name_len = 0;
    tpm_nv_status_t st;
    int r;

    if (!record || record_len == 0u || record_len > TPM_NV_MAX_DATA || !tr)
        return TPM_NV_BADARG;
    if (!grant_ok(&tr->write) || !grant_ok(&tr->commit))
        return TPM_NV_BADARG;
    if (nv_index == counter_index)
        return TPM_NV_BADARG;
    if (!tpm_authz_provisioned())
        return TPM_NV_UNAVAIL;

    st = tpm_authz_contract(nv_index, &contract);
    if (st != TPM_NV_OK)
        return st;
    if (contract.data_size != record_len)
        return TPM_NV_BADARG;
    /* Both contracts are derived HERE, outside the transition. tpm_authz_contract
     * opens its own trial-session sequence for a policy-bearing entry, and
     * sequences do not nest -- deriving one inside the transition would come
     * back TPM_NV_BUSY and read like TPM contention rather than a nesting
     * mistake. */
    st = tpm_authz_contract(counter_index, &counter_contract);
    if (st != TPM_NV_OK)
        return st;
    st = tpm_authz_authority_name(key_name, (uint16_t)sizeof key_name, &key_name_len);
    if (st != TPM_NV_OK)
        return st;

    c.nv_index = nv_index;
    c.counter_index = counter_index;
    c.record = record;
    c.record_len = record_len;
    c.expect_counter = expect_counter;
    c.tr = tr;
    c.contract = &contract;
    c.counter_contract = &counter_contract;
    c.key_name = key_name;
    c.key_name_len = key_name_len;
    c.st = TPM_NV_TRANSPORT;

    /* ONE sequence for the whole transition. The transport's busy gate makes
     * this the mutual exclusion: a concurrent advance is refused with BUSY
     * rather than interleaving its own read-modify-write with this one. */
    r = tpm2_seq_run(TPM_NV_OP_BUDGET_MS, TPM_NV_OP_CLEANUP_BUDGET_MS,
                     authz_write_seq, &c);
    if (r != 0)
        return TPM_NV_BUSY;
    return c.st;
}

/* ---- Verified record reads ----
 *
 * The counter and the record are read in ONE sequence. Split across two, an
 * authorized advance landing in between makes a perfectly current record look
 * skewed; that is fail-closed rather than dangerous, but it is a spurious
 * refusal of a machine that is fine, and the single sequence costs nothing. */

struct authz_read_ctx {
    const struct tpm_nv_identity *contract;
    const struct tpm_nv_identity *counter_contract;
    uint32_t        counter_index;
    uint8_t        *buf;
    uint16_t        cap;
    uint64_t        counter;
    tpm_nv_status_t st;
};

static int authz_read_seq(tpm2_seq_t seq, void *vctx)
{
    struct authz_read_ctx *c = (struct authz_read_ctx *)vctx;
    /* Sized from the API's own bound, exactly as the write path is: this reads
     * c->cap bytes and the public entry point accepts any cap up to
     * TPM_NV_MAX_DATA. The bare 512 here was the same defect the write side
     * carried, and fixing one while leaving its sibling would have closed
     * nothing -- the two grow together the day a record contract does. */
    uint8_t cmd[64], rsp[TPM_NV_MAX_RSP];
    struct tpm_nv_public pub;
    uint32_t n, rlen = 0, rc = 0;
    tpm_nv_status_t st, counter_gone = TPM_NV_OK;
    int name_ok = 0, got;

    /* THE COUNTER'S IDENTITY, BEFORE ITS VALUE. The whole commit decision rests
     * on this number, and the read path used to take it from an index it had
     * never identified -- while the WRITE path verifies the counter contract
     * before building a cpHash over its Name, for the reason its own comment
     * gives: a Name is meaningful only once the index behind it is the enrolled
     * one. This closes that asymmetry.
     *
     * WHAT IT BUYS, precisely: an index whose DEFINITION is not the enrolled
     * one -- wrong type, size, nameAlg or attributes -- can no longer supply
     * the committed generation. What it does NOT resolve is a byte-identical
     * recreate: for a counter the manifest sets expect_written 0 on purpose
     * (a TPM_NT_COUNTER reads unwritten until its first increment), so the
     * WRITTEN bit cannot separate a recreated counter from an enrolled one the
     * way it does for a record index.
     *
     * That residual ambiguity does not change what a caller must DO. A
     * TPM_NT_COUNTER restarts at or above the highest value any NV counter on
     * the TPM has ever held (TPM 2.0 Part 1 section 37.2.6.3), so a recreated
     * counter reads ABOVE a surviving record and lands in TPM_PAIRING_TORN --
     * the same verdict a genuinely torn pairing produces, and the response to
     * both is authorized recovery rather than a fresh enrollment. TORN is
     * therefore honest about being a class, not a single cause. */
    /* THE DEFERRAL COVERS ABSENCE WHEREVER IT SURFACES, and that is the whole
     * rule: a DELETED counter fails this NV_ReadPublic, while a byte-identically
     * RECREATED one passes it and fails the NV_Read below. Deferring only the
     * second left the first -- the simpler attack -- taking the legacy path.
     *
     * An absent commit counter means one of two OPPOSITE things, and the
     * counter cannot tell them apart; the RECORD can. With no record either,
     * this machine was simply never enrolled. With a record present, the commit
     * anchor behind an enrolled record is GONE, which is exactly the rollback
     * attack -- and the attacker need never increment anything to reach it.
     * Concluding here collapsed both into the legacy no-record answer, so
     * verification recommended MIGRATION, which would authenticate the current
     * owner-writable blob and launder the rollback evidence.
     *
     * Only ABSENCE is deferred. A Name that does not verify, or a definition
     * that does not match the manifest, is a definite identity failure with
     * nothing left to disambiguate, and it still returns immediately. */
    st = tpm_nv_read_identity_seq(seq, c->counter_index, &pub, &name_ok);
    if (st == TPM_NV_NOTFOUND || st == TPM_NV_UNINIT) {
        counter_gone = st;
        c->counter = 0u;
    } else if (st != TPM_NV_OK) {
        c->st = st;
        return 0;
    }

    if (counter_gone == TPM_NV_OK) {
        if (!name_ok) { c->st = TPM_NV_MISMATCH; return 0; }
        st = tpm_nv_identity_match(c->counter_contract, &pub);
        if (st != TPM_NV_OK) { c->st = st; return 0; }
        /* Manifest-driven, exactly as the record index's check is. Currently
         * inert for the two counter anchors because their expect_written is 0;
         * it is here so the rule travels with the contract rather than with
         * this call site, and it is NOT the counter recreation detector. */
        if (c->counter_contract->expect_written && !(pub.attrs & TPMA_NV_WRITTEN)) {
            c->st = TPM_NV_RECREATED;
            return 0;
        }

        st = tpm_nv_read_counter_seq(seq, c->counter_index, &c->counter);
        if (st == TPM_NV_NOTFOUND || st == TPM_NV_UNINIT) {
            counter_gone = st;
            c->counter = 0u;
        } else if (st != TPM_NV_OK) {
            c->st = st;
            return 0;
        }
    }

    name_ok = 0;
    /* Identity BEFORE contents, in the same sequence. A Name check that runs
     * after the read has already trusted the bytes, and one that runs in its own
     * sequence has already expired by the time the read happens. */
    st = tpm_nv_read_identity_seq(seq, c->contract->nv_index, &pub, &name_ok);
    if (st != TPM_NV_OK) {
        /* THE MIRROR CASE. A record that is ABSENT while its commit counter
         * holds a VALUE is a DESTROYED record, not a machine that was never
         * enrolled: a counter reads a value only once a transition committed,
         * and a transition writes the record BEFORE it increments. Reporting
         * absence here would invite the same migration the deferral above
         * exists to prevent. */
        if ((st == TPM_NV_NOTFOUND || st == TPM_NV_UNINIT) &&
            counter_gone == TPM_NV_OK)
            st = TPM_NV_RECREATED;
        c->st = st;
        return 0;
    }
    if (!name_ok) { c->st = TPM_NV_MISMATCH; return 0; }
    st = tpm_nv_identity_match(c->contract, &pub);
    if (st != TPM_NV_OK) { c->st = st; return 0; }

    /* THE LIFECYCLE CHECK, and it is not optional. tpm_nv_identity_match
     * normalizes TPMA_NV_WRITTEN away because it is not a definition property,
     * so a byte-identical destroy-and-recreate passes it: the definition IS the
     * enrolled one, because the attacker copied it. What separates the two is
     * that a freshly defined index reads back UNWRITTEN.
     *
     * Without this a read falls through to an NV_Read reporting UNINIT, and
     * UNINIT downstream means "no record yet" -- which is exactly how a
     * destroyed anchor gets laundered into a first enrollment. */
    if (c->contract->expect_written && !(pub.attrs & TPMA_NV_WRITTEN)) {
        c->st = TPM_NV_RECREATED;
        return 0;
    }

    /* OWNERREAD, so authHandle is the owner hierarchy. The index handle would
     * be AUTHREAD, which these contracts deliberately do not grant. */
    n = tpm2_build_nv_read(cmd, sizeof cmd, TPM_RH_OWNER,
                           c->contract->nv_index, TPM_RS_PW, c->cap, 0u);
    if (n == 0u) { c->st = TPM_NV_BADARG; return 0; }
    if (tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, &rc) != 0) {
        /* ABSENCE HERE IS NOT ABSENCE OF AN ENROLLMENT. The ReadPublic just
         * above proved this index exists, matches its enrolled contract and is
         * WRITTEN; a read that then answers "not found" or "never written"
         * describes a record destroyed between the two commands, or a TPM
         * answering inconsistently. Propagating it unchanged let the caller
         * collapse it into the legacy no-record result and invite migration --
         * the same laundering the counter-side deferral exists to stop, one
         * command further along. */
        if (st == TPM_NV_NOTFOUND || st == TPM_NV_UNINIT)
            st = TPM_NV_RECREATED;
        c->st = st;
        return 0;
    }
    got = tpm2_parse_nv_read(rsp, rlen, c->buf, c->cap);
    /* EXACT. A short read handed to the record parser would fail its length
     * check anyway, but reporting TRANSPORT here says what actually happened
     * rather than blaming the record for a truncated response. */
    c->st = (got == (int)c->cap) ? TPM_NV_OK : TPM_NV_TRANSPORT;

    /* NOW the deferred counter verdict can be resolved: the record is present
     * and readable, so an absent commit anchor is a LOST anchor rather than a
     * machine that was never enrolled. RECREATED, not NOTFOUND -- an identity
     * failure the boot publishes and routes to authorized recovery.
     *
     * If the record read failed instead, its own status stands: a machine
     * missing BOTH anchors is the legacy/never-enrolled case, and that is the
     * one reading that legitimately invites migration. */
    if (c->st == TPM_NV_OK && counter_gone != TPM_NV_OK)
        c->st = TPM_NV_RECREATED;
    return 0;
}

/* Read, validate and commit-check one record. `want_kind`/`want_payload_len`
 * pin what the caller asked for, so a record of the other kind served from this
 * handle is refused even though it carries a perfectly valid digest. */
static tpm_nv_status_t authz_read_record(uint32_t nv_index, uint32_t counter_index,
                                         tpm_record_kind_t want_kind,
                                         uint32_t want_payload_len,
                                         uint8_t *buf, uint16_t cap,
                                         struct tpm_record_view *out_view,
                                         uint64_t *out_counter,
                                         tpm_pairing_t *out_pairing)
{
    struct tpm_nv_identity contract, counter_contract;
    struct authz_read_ctx c;
    tpm_nv_status_t st;
    tpm_record_status_t rs;
    tpm_pairing_t pairing;
    struct tpm_boot_grant grant;
    uint32_t grant_ms, spent_ms;
    int r;

    if (out_pairing)
        *out_pairing = TPM_PAIRING_BADARG;

    /* The aggregate boot deadline, taken BEFORE any TPM work rather than just
     * before the read.
     *
     * A verified read is not one sequence. Each authorized index derives its
     * policy contract through a tpm2_seq_run of its own (tpm_authz_contract),
     * so admitting after those two derivations would let a verified read spend
     * two full uncharged sequences and still report compliance -- an aggregate
     * ledger that starts after most of the work is not an aggregate ledger.
     * The mark is taken here for the same reason: the charge must cover
     * everything the read caused, not just its last sequence.
     *
     * Exhaustion is REPORTED here rather than silently shortening the set of
     * records the boot reads. */
    st = tpm_boot_budget_admit_one(TPM_NV_VERIFIED_READ_BUDGET_MS, &grant);
    if (st != TPM_NV_OK)
        return st;
    grant_ms = grant.granted_ms;
    /* The grant is clamped to the boot remainder, so it can come back smaller
     * than the very first sequence this read must run. Refusing here spends
     * nothing that is not immediately settled back; starting anyway would spend
     * a full indivisible sequence against a remainder that could not pay for
     * it. The settle is what returns the reservation. */
    if (grant_ms < TPM_NV_POLICY_SEQ_COST_MS) {
        tpm_boot_budget_settle(&grant);
        tpm_boot_budget_note_expiry();
        return TPM_NV_BUDGET;
    }

    st = tpm_authz_contract(nv_index, &contract);
    if (st != TPM_NV_OK) {
        tpm_boot_budget_settle(&grant);
        return st;
    }
    if (contract.data_size != cap) {
        tpm_boot_budget_settle(&grant);
        return TPM_NV_BADARG;
    }

    /* Re-check the grant BETWEEN the derivations. tpm_authz_contract arms its
     * own per-operation budget and cannot be handed ours without changing a
     * contract several callers share, so the grant cannot bound a derivation
     * from the inside. Checking between them bounds the OVERSHOOT to one
     * derivation instead of two plus the read, and the charge below makes even
     * that overshoot visible to the next admission. */
    /* REFUSE BEFORE STARTING, not after overrunning. tpm_authz_contract runs a
     * tpm2_seq_run with fixed per-operation constants and cannot be handed a
     * smaller allowance without changing a contract several callers share, so
     * the sequence is INDIVISIBLE from here: once it starts it can spend its
     * full work plus cleanup regardless of what is left. Checking that the
     * whole cost still fits is what makes the aggregate a bound on latency
     * rather than an accounting of it after the fact. */
    spent_ms = tpm_boot_elapsed_ms(grant.mark_ms);
    if (spent_ms >= grant_ms ||
        (grant_ms - spent_ms) < TPM_NV_POLICY_SEQ_COST_MS) {
        tpm_boot_budget_settle(&grant);
        /* The refusal happened HERE, against the grant, not at an admission,
         * so the ledger would not otherwise record that this boot ran out. */
        tpm_boot_budget_note_expiry();
        return TPM_NV_BUDGET;
    }

    /* Derived from the compiled manifest, never read from storage -- the same
     * rule the record index's contract follows, and the reason the identity
     * check below is worth anything. */
    st = tpm_authz_contract(counter_index, &counter_contract);
    if (st != TPM_NV_OK) {
        tpm_boot_budget_settle(&grant);
        return st;
    }

    /* Same indivisibility check before the READ sequence, at the READ's price.
     * It opens no session and calls no teardown, so demanding a cleanup reserve
     * here would refuse a read that fits perfectly well. */
    spent_ms = tpm_boot_elapsed_ms(grant.mark_ms);
    if (spent_ms >= grant_ms ||
        (grant_ms - spent_ms) < TPM_NV_READ_SEQ_COST_MS) {
        tpm_boot_budget_settle(&grant);
        tpm_boot_budget_note_expiry();
        return TPM_NV_BUDGET;
    }
    grant_ms -= spent_ms;
    /* The aggregate grant bounds the whole verified read; it does NOT widen a
     * single sequence. Through the pure helper so the relation is assertable
     * rather than buried here. */
    grant_ms = tpm_boot_grant_work_ms(grant_ms);

    c.contract = &contract;
    c.counter_contract = &counter_contract;
    c.counter_index = counter_index;
    c.buf = buf;
    c.cap = cap;
    c.counter = 0u;
    c.st = TPM_NV_TRANSPORT;
    /* The cleanup reserve is passed through unclamped. It is not part of the
     * work quota and must never be traded for a tighter deadline: a mandatory
     * FlushContext that cannot run leaks a session handle out of the TPM's
     * small pool until reboot, which is worse than the overrun it would buy. */
    r = tpm2_seq_run(grant_ms, TPM_NV_OP_CLEANUP_BUDGET_MS, authz_read_seq, &c);
    /* Charge the FULL observed span -- both contract derivations, the read's
     * work, its cleanup and any abort envelope. Charging only the grant would
     * undercount exactly the operations that overran, which is the population
     * the deadline exists to bound. */
    tpm_boot_budget_settle(&grant);
    if (r != 0)
        return TPM_NV_BUSY;
    if (c.st != TPM_NV_OK)
        return c.st;

    rs = tpm_record_parse(buf, cap, want_kind, want_payload_len, out_view);
    /* A record that does not parse is CORRUPT PERSISTED STATE, not caller
     * misuse: it is an authorized-recovery question, which is exactly the
     * distinction TPM_NV_RECORD carries and TPM_NV_BADARG does not.
     *
     * RECORD and not CONTRACT, which this returned until section 29. Both are
     * corrupt persisted state, but they name DIFFERENT objects: a CONTRACT
     * failure says the index answering is not the one enrolled, and the
     * operator diagnosis built on it says exactly that. Reaching here means the
     * enrolled index passed its identity check and answered -- the bytes it
     * holds are simply not a record -- so reporting CONTRACT sent an operator
     * hunting a substituted index that is not there. */
    if (rs != TPM_RECORD_OK)
        return TPM_NV_RECORD;

    /* The commit check. The DIRECTION is reported separately for callers that
     * must act on it (recovery routing), while the status stays MISMATCH for
     * every non-current pairing: shipped strict readers classify on MISMATCH,
     * and leaking a directional status into that path would silently change
     * how they route a half-finished update. */
    pairing = tpm_record_pairing(out_view, c.counter);
    if (out_pairing)
        *out_pairing = pairing;
    if (out_counter)
        *out_counter = c.counter;
    if (pairing != TPM_PAIRING_CURRENT)
        return TPM_NV_MISMATCH;
    return TPM_NV_OK;
}

uint32_t tpm_ab_floor_read_plan(tpm_floor_read_step_t step, uint8_t *buf,
                                uint32_t cap)
{
    if (!buf)
        return 0u;

    /* Every arm below marshals with the SAME builder and the SAME arguments the
     * in-sequence path uses, so the plan is a published NAME for that byte
     * stream rather than a second implementation of it. The shared fixture in
     * the test suite asserts the two are byte-identical, which is what keeps
     * them from drifting once the loader has its own executor. */
    switch (step) {
    case TPM_FLOOR_STEP_COUNTER_PUBLIC:
        return tpm2_build_nv_read_public(buf, cap, TPM_NV_INDEX_AB_SEQ);
    case TPM_FLOOR_STEP_COUNTER_READ:
        /* OWNERREAD, so authHandle is the owner hierarchy -- these anchors do
         * not grant AUTHREAD, and a TPM that enforces the distinction refuses
         * the index handle. Exactly TPM_NV_COUNTER_SIZE: a short read would
         * invent a counter value out of partial bytes. */
        return tpm2_build_nv_read(buf, cap, TPM_RH_OWNER, TPM_NV_INDEX_AB_SEQ,
                                  TPM_RS_PW, (uint16_t)TPM_NV_COUNTER_SIZE, 0u);
    case TPM_FLOOR_STEP_RECORD_PUBLIC:
        return tpm2_build_nv_read_public(buf, cap, TPM_NV_INDEX_AB_FLOOR);
    case TPM_FLOOR_STEP_RECORD_READ:
        return tpm2_build_nv_read(buf, cap, TPM_RH_OWNER, TPM_NV_INDEX_AB_FLOOR,
                                  TPM_RS_PW, (uint16_t)TPM_AB_FLOOR_RECORD_LEN,
                                  0u);
    default:
        return 0u;
    }
}

tpm_nv_status_t tpm_ab_floor_read_view(struct tpm_ab_floor_view *out)
{
    uint8_t buf[TPM_AB_FLOOR_RECORD_LEN];
    struct tpm_record_view view = { 0 };
    uint64_t counter = 0;
    tpm_pairing_t pairing = TPM_PAIRING_BADARG;
    tpm_nv_status_t st;

    if (!out)
        return TPM_NV_BADARG;
    memset(out, 0, sizeof *out);
    out->pairing = TPM_PAIRING_BADARG;

    st = authz_read_record(TPM_NV_INDEX_AB_FLOOR, TPM_NV_INDEX_AB_SEQ,
                           TPM_RECORD_KIND_AB_FLOOR,
                           (uint32_t)sizeof(struct tpm_ab_floor_payload),
                           buf, (uint16_t)sizeof buf, &view, &counter,
                           &pairing);
    /* Anything that never reached the pairing check (transport, identity,
     * recreation, a record that would not parse) is reported as-is with no
     * pairing claim: there is nothing to pair. */
    if (st != TPM_NV_OK && st != TPM_NV_MISMATCH)
        return st;

    /* A pre-pair MISMATCH -- a counter or record identity that failed its
     * enrolled contract -- returns before tpm_record_parse ever fills `view`,
     * so there is no record generation to report and reading one would publish
     * uninitialized kernel stack under conditions the TPM's answer chooses. The
     * pairing stays BADARG, which is the caller's signal that no pairing claim
     * was made, and the generations stay zero rather than garbage. */
    if (pairing == TPM_PAIRING_BADARG)
        return (st == TPM_NV_OK) ? TPM_NV_MISMATCH : st;

    out->pairing            = pairing;
    out->committed_generation = counter;
    out->record_generation  = view.generation;

    /* THE VERSION IS PUBLISHED ONLY FOR A CURRENT PAIRING, and this is the
     * whole reason the view exists rather than an out-param on the strict read.
     * An UNCOMMITTED record is not a stale-but-usable floor that a caller may
     * fall back on: the authorized write overwrote the sole record in place
     * before the commit increment, so by the time this state is observable the
     * previously committed bytes are already gone. Publishing the candidate's
     * version here would hand a consumer -- kernel or loader -- a floor no
     * authority ever committed. */
    if (pairing != TPM_PAIRING_CURRENT)
        return TPM_NV_MISMATCH;
    /* Same split as the parse above: the record was accepted structurally and
     * its PAYLOAD is the thing that will not yield a version, which is the
     * record's problem and not the enrolled contract's. */
    if (tpm_record_ab_floor_version(&view, &out->version) != TPM_RECORD_OK)
        return TPM_NV_RECORD;
    out->version_valid = 1u;
    return TPM_NV_OK;
}

tpm_nv_status_t tpm_ab_floor_read(uint32_t *out_version, uint64_t *out_generation)
{
    struct tpm_ab_floor_view v;
    tpm_nv_status_t st;

    if (!out_version)
        return TPM_NV_BADARG;
    /* Strict wrapper: OK only for a CURRENT pairing, and every other pairing
     * collapses back to MISMATCH exactly as it did before the view existed.
     * Shipped consumers classify on MISMATCH, so the directional verdict stays
     * behind the view API rather than changing their routing underneath them. */
    st = tpm_ab_floor_read_view(&v);
    if (st != TPM_NV_OK)
        return st;
    *out_version = v.version;
    if (out_generation)
        *out_generation = v.committed_generation;
    return TPM_NV_OK;
}

tpm_nv_status_t tpm_ab_floor_advance(uint32_t new_version,
                                     const struct tpm_authz_transition *tr)
{
    uint8_t record[TPM_AB_FLOOR_RECORD_LEN];
    struct tpm_ab_floor_payload payload;
    uint32_t cur_version = 0;
    uint64_t counter = 0;
    tpm_nv_status_t st;

    if (!tr)
        return TPM_NV_BADARG;
    if (!tpm_authz_provisioned())
        return TPM_NV_UNAVAIL;

    /* An ADVANCE requires a floor to advance FROM. Every non-OK status is
     * returned as itself, including NOTFOUND and UNINIT.
     *
     * An earlier draft treated those two as "no floor yet" and wrote a fresh
     * record over them, which is the laundering this section exists to stop: a
     * destroyed anchor and an unprovisioned one are indistinguishable from
     * here, so writing a first floor on top of either hands an attacker the
     * recovery path. First provisioning is a DIFFERENT operation -- define the
     * index under its authPolicy, then make the first authorized write -- and
     * an advance may not perform it by accident.
     *
     * The version refusal below is a convenience, not the boundary: the
     * boundary is that the authority granted THIS record, and a lower version
     * reaching the TPM would fail there too. */
    st = tpm_ab_floor_read(&cur_version, &counter);
    if (st != TPM_NV_OK)
        return st;
    if (new_version < cur_version)
        return TPM_NV_MISMATCH;

    memset(&payload, 0, sizeof payload);
    payload.security_version = new_version;
    if (tpm_record_build(record, (uint32_t)sizeof record, TPM_RECORD_KIND_AB_FLOOR,
                         counter + 1u, (const uint8_t *)&payload,
                         (uint32_t)sizeof payload) != TPM_RECORD_OK)
        return TPM_NV_BADARG;

    return tpm_authz_write_record(TPM_NV_INDEX_AB_FLOOR, TPM_NV_INDEX_AB_SEQ,
                                  record, (uint16_t)sizeof record, counter, tr);
}

tpm_nv_status_t tpm_baseline_bind_view(const uint8_t *blob, uint32_t blob_len,
                                       struct tpm_baseline_bind_view *out)
{
    uint8_t buf[TPM_BASELINE_BIND_LEN];
    uint8_t digest[SHA256_DIGEST_LEN];
    struct tpm_record_view view = { 0 };
    /* A COPY, not a pointer cast over view.payload. The payload points into
     * `buf` above, a plain uint8_t array with no alignment guarantee, while
     * this struct carries 4-byte members -- a type-pun x86-64 happens to
     * tolerate and the planned ARM64 port may not. memcpy states the intent,
     * costs one 40-byte copy on a path that has just done a TPM round trip,
     * and is the only shape that stays correct if the record layout ever puts
     * the payload at an odd offset. */
    struct tpm_baseline_bind_payload p;
    uint64_t counter = 0;
    tpm_pairing_t pairing = TPM_PAIRING_BADARG;
    tpm_nv_status_t st;

    if (!blob || blob_len == 0u || !out)
        return TPM_NV_BADARG;
    memset(out, 0, sizeof *out);
    out->pairing = TPM_PAIRING_BADARG;

    st = authz_read_record(TPM_NV_INDEX_BASELINE_BIND, TPM_NV_INDEX_BASELINE_GEN,
                           TPM_RECORD_KIND_BASELINE,
                           (uint32_t)sizeof(struct tpm_baseline_bind_payload),
                           buf, (uint16_t)sizeof buf, &view, &counter,
                           &pairing);
    /* NO bind record is a LEGACY, unauthenticated baseline. It is reported, not
     * accepted and not auto-wrapped: the blob it would wrap is owner-writable,
     * so binding it now would authenticate whatever an attacker last wrote and
     * leave the boundary worse than no boundary. Migration is section 19's. */
    if (st == TPM_NV_NOTFOUND || st == TPM_NV_UNINIT)
        return TPM_NV_NOTFOUND;
    if (st != TPM_NV_OK && st != TPM_NV_MISMATCH)
        return st;

    /* Same pre-pair guard as the floor view: an identity mismatch returns
     * before the record is parsed, so `view` holds nothing and its generation
     * must not be published. */
    if (pairing == TPM_PAIRING_BADARG)
        return (st == TPM_NV_OK) ? TPM_NV_MISMATCH : st;

    out->pairing              = pairing;
    out->committed_generation = counter;
    out->record_generation    = view.generation;

    /* A non-current pairing is answered on the pairing alone. Comparing the
     * blob against an uncommitted or torn record would produce a "matches" or
     * "does not match" verdict about a record no authority committed, and a
     * caller reading only the digest result would act on it. */
    if (pairing != TPM_PAIRING_CURRENT)
        return TPM_NV_MISMATCH;

    /* The payload length was pinned at the read: authz_read_record was asked
     * for exactly sizeof(struct tpm_baseline_bind_payload) and refuses any
     * record that does not carry it, so this copy cannot over-read the view. */
    memcpy(&p, view.payload, sizeof p);
    if (p.blob_len != blob_len)
        return TPM_NV_MISMATCH;
    sha256(blob, blob_len, digest);
    if (memcmp(digest, p.blob_digest, SHA256_DIGEST_LEN) != 0)
        return TPM_NV_MISMATCH;
    out->bound = 1u;
    return TPM_NV_OK;
}

tpm_nv_status_t tpm_baseline_bind_verify(const uint8_t *blob, uint32_t blob_len,
                                         uint64_t *out_generation)
{
    struct tpm_baseline_bind_view v;
    tpm_nv_status_t st;

    /* Strict wrapper, contract-compatible with the shipped consumers: every
     * non-current pairing stays MISMATCH and the directional verdict is reached
     * only through tpm_baseline_bind_view(). */
    st = tpm_baseline_bind_view(blob, blob_len, &v);
    if (st != TPM_NV_OK)
        return st;
    if (out_generation)
        *out_generation = v.committed_generation;
    return TPM_NV_OK;
}

tpm_nv_status_t tpm_baseline_bind_write(const uint8_t *blob, uint32_t blob_len,
                                        const struct tpm_authz_transition *tr)
{
    uint8_t record[TPM_BASELINE_BIND_LEN];
    struct tpm_baseline_bind_payload payload;
    uint64_t counter = 0;
    tpm_nv_status_t st;

    if (!blob || blob_len == 0u || !tr)
        return TPM_NV_BADARG;
    if (!tpm_authz_provisioned())
        return TPM_NV_UNAVAIL;

    /* Same rule as the floor: a counter that cannot be read is not a counter at
     * zero. UNINIT is reported rather than synthesized, because the first
     * increment of a recreated index may land far above any previous value, and
     * conflating "never incremented" with "zero" is how a rollback is laundered
     * -- which is exactly what tpm_nv_read_counter's own contract says. */
    st = tpm_nv_read_counter(TPM_NV_INDEX_BASELINE_GEN, &counter);
    if (st != TPM_NV_OK)
        return st;

    memset(&payload, 0, sizeof payload);
    sha256(blob, blob_len, payload.blob_digest);
    payload.blob_len = blob_len;
    if (tpm_record_build(record, (uint32_t)sizeof record, TPM_RECORD_KIND_BASELINE,
                         counter + 1u, (const uint8_t *)&payload,
                         (uint32_t)sizeof payload) != TPM_RECORD_OK)
        return TPM_NV_BADARG;

    return tpm_authz_write_record(TPM_NV_INDEX_BASELINE_BIND,
                                  TPM_NV_INDEX_BASELINE_GEN,
                                  record, (uint16_t)sizeof record, counter, tr);
}
