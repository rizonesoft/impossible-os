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
#include "kernel/crypto/sha256.h"
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
};

#define AUTHZ_MANIFEST_COUNT ((uint32_t)(sizeof s_manifest / sizeof s_manifest[0]))

/* The installed authority. NULL public area = unprovisioned = every authorized
 * write refused. Written once during Phase-1 provisioning and read-only
 * thereafter; it is not touched from an interrupt and no AP mutates it. */
static struct tpm_authz_authority s_authority;

tpm_nv_status_t tpm_authz_set_authority(const struct tpm_authz_authority *auth)
{
    if (!auth) {
        s_authority.public_area = 0;
        s_authority.public_len = 0u;
        s_authority.policy_ref = 0;
        s_authority.policy_ref_len = 0u;
        return TPM_NV_OK;
    }
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
    s_authority = *auth;
    return TPM_NV_OK;
}

int tpm_authz_provisioned(void)
{
    return (s_authority.public_area != 0 && s_authority.public_len != 0u) ? 1 : 0;
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
    uint32_t session, n, rlen = 0, rc = 0;
    tpm_nv_status_t st;
    int dlen;

    memset(nonce, AUTHZ_NONCE_FILL, sizeof nonce);
    memset(zero, 0, sizeof zero);
    authz_null_ticket(ticket);

    n = tpm2_build_start_auth_session(cmd, sizeof cmd, TPM2_SE_TRIAL,
                                      TPM_ALG_SHA256, nonce, sizeof nonce);
    if (n == 0u) { c->st = TPM_NV_BADARG; return 0; }
    if (tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, &rc) != 0) {
        c->st = st;
        return 0;
    }
    session = tpm2_parse_start_auth_session(rsp, rlen);
    if (session == 0u) {
        /* The TPM reported success, so a session may exist behind a reply this
         * parser rejected. Recover the raw handle for cleanup rather than
         * leaking it for the rest of the boot. */
        uint32_t leaked = tpm2_rsp_session_handle(rsp, rlen);
        if (leaked != 0u) {
            n = tpm2_build_flush_context(cmd, sizeof cmd, leaked);
            if (n != 0u)
                (void)tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp,
                                               &rlen, &st, &rc);
        }
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
    /* Single cleanup on every path: an abandoned trial session otherwise holds
     * a TPM session slot for the rest of the boot, and TPMs have very few. */
    n = tpm2_build_flush_context(cmd, sizeof cmd, session);
    if (n != 0u)
        (void)tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, &rc);
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

    if (SHA256_DIGEST_LEN > TPM_NV_POLICY_MAX)
        return TPM_NV_CONTRACT;

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
 *   3. write the record under the grant  -- reversible; nothing is committed yet
 *   4. read it back                      -- the bytes are on the device, verbatim
 *   5. increment the counter             -- THE COMMIT POINT, irreversible, last
 *
 * A failure anywhere before 5 leaves a record whose generation is one ahead of
 * the counter. Readers already treat that as not-current, so the machine keeps
 * using the previously committed value: the failure costs an update, never the
 * floor. The reverse order would advance the irreversible half first and leave
 * the machine committed to bytes that may not exist. */

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
    uint8_t ticket[128];
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
    if (tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, &rc) != 0)
        return st;
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
    if (tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, &rc) != 0)
        goto flush_key;
    session = tpm2_parse_start_auth_session(rsp, rlen);
    if (session == 0u) {
        uint32_t leaked = tpm2_rsp_session_handle(rsp, rlen);
        if (leaked != 0u) {
            n = tpm2_build_flush_context(cmd, sizeof cmd, leaked);
            if (n != 0u)
                (void)tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp,
                                               &rlen, &st, &rc);
        }
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
    n = tpm2_build_flush_context(cmd, sizeof cmd, key_handle);
    if (n != 0u)
        (void)tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, &rc);
    *out_session = session;
    return TPM_NV_OK;

flush_all:
    n = tpm2_build_flush_context(cmd, sizeof cmd, session);
    if (n != 0u) {
        tpm_nv_status_t ignored; uint32_t irc = 0, irl = 0;
        (void)tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &irl,
                                       &ignored, &irc);
    }
flush_key:
    if (key_handle != 0u) {
        n = tpm2_build_flush_context(cmd, sizeof cmd, key_handle);
        if (n != 0u) {
            tpm_nv_status_t ignored; uint32_t irc = 0, irl = 0;
            (void)tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &irl,
                                           &ignored, &irc);
        }
    }
    return st;
}

static int authz_write_seq(tpm2_seq_t seq, void *vctx)
{
    struct authz_write_ctx *c = (struct authz_write_ctx *)vctx;
    uint8_t cmd[1024], rsp[512];
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
    n = tpm2_build_flush_context(cmd, sizeof cmd, session);
    if (n != 0u)
        (void)tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, &rc);
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
    if (session != 0u) {
        n = tpm2_build_flush_context(cmd, sizeof cmd, session);
        if (n != 0u)
            (void)tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, &rc);
    }
    return 0;
}

static int grant_ok(const struct tpm_authz_grant *g)
{
    return g && g->approved_policy && g->approved_len == SHA256_DIGEST_LEN &&
           g->signature && g->sig_len != 0u;
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
    uint32_t        counter_index;
    uint8_t        *buf;
    uint16_t        cap;
    uint64_t        counter;
    tpm_nv_status_t st;
};

static int authz_read_seq(tpm2_seq_t seq, void *vctx)
{
    struct authz_read_ctx *c = (struct authz_read_ctx *)vctx;
    uint8_t cmd[64], rsp[512];
    struct tpm_nv_public pub;
    uint32_t n, rlen = 0, rc = 0;
    tpm_nv_status_t st;
    int name_ok = 0, got;

    st = tpm_nv_read_counter_seq(seq, c->counter_index, &c->counter);
    if (st != TPM_NV_OK) { c->st = st; return 0; }

    /* Identity BEFORE contents, in the same sequence. A Name check that runs
     * after the read has already trusted the bytes, and one that runs in its own
     * sequence has already expired by the time the read happens. */
    st = tpm_nv_read_identity_seq(seq, c->contract->nv_index, &pub, &name_ok);
    if (st != TPM_NV_OK) { c->st = st; return 0; }
    if (!name_ok) { c->st = TPM_NV_MISMATCH; return 0; }
    st = tpm_nv_identity_match(c->contract, &pub);
    if (st != TPM_NV_OK) { c->st = st; return 0; }

    /* OWNERREAD, so authHandle is the owner hierarchy. The index handle would
     * be AUTHREAD, which these contracts deliberately do not grant. */
    n = tpm2_build_nv_read(cmd, sizeof cmd, TPM_RH_OWNER,
                           c->contract->nv_index, TPM_RS_PW, c->cap, 0u);
    if (n == 0u) { c->st = TPM_NV_BADARG; return 0; }
    if (tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, &rc) != 0) {
        c->st = st;
        return 0;
    }
    got = tpm2_parse_nv_read(rsp, rlen, c->buf, c->cap);
    /* EXACT. A short read handed to the record parser would fail its length
     * check anyway, but reporting TRANSPORT here says what actually happened
     * rather than blaming the record for a truncated response. */
    c->st = (got == (int)c->cap) ? TPM_NV_OK : TPM_NV_TRANSPORT;
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
                                         uint64_t *out_counter)
{
    struct tpm_nv_identity contract;
    struct authz_read_ctx c;
    tpm_nv_status_t st;
    tpm_record_status_t rs;
    int r;

    st = tpm_authz_contract(nv_index, &contract);
    if (st != TPM_NV_OK)
        return st;
    if (contract.data_size != cap)
        return TPM_NV_BADARG;

    c.contract = &contract;
    c.counter_index = counter_index;
    c.buf = buf;
    c.cap = cap;
    c.counter = 0u;
    c.st = TPM_NV_TRANSPORT;
    r = tpm2_seq_run(TPM_NV_OP_BUDGET_MS, TPM_NV_OP_CLEANUP_BUDGET_MS,
                     authz_read_seq, &c);
    if (r != 0)
        return TPM_NV_BUSY;
    if (c.st != TPM_NV_OK)
        return c.st;

    rs = tpm_record_parse(buf, cap, want_kind, want_payload_len, out_view);
    /* A record that does not parse is CORRUPT PERSISTED STATE, not caller
     * misuse: it is an authorized-recovery question, which is exactly the
     * distinction TPM_NV_CONTRACT carries and TPM_NV_BADARG does not. */
    if (rs != TPM_RECORD_OK)
        return TPM_NV_CONTRACT;

    /* The commit check. A record one AHEAD of the counter is the legitimate
     * write-then-increment window: the transition was authorized and written
     * but never committed, so the committed value is still the old one and this
     * record is NOT the answer. Reporting MISMATCH keeps a half-finished update
     * from moving the floor in either direction. */
    if (tpm_record_counter_ok(out_view, c.counter) != TPM_RECORD_OK)
        return TPM_NV_MISMATCH;
    if (out_counter)
        *out_counter = c.counter;
    return TPM_NV_OK;
}

tpm_nv_status_t tpm_ab_floor_read(uint32_t *out_version, uint64_t *out_generation)
{
    uint8_t buf[TPM_AB_FLOOR_RECORD_LEN];
    struct tpm_record_view view;
    uint64_t counter = 0;
    tpm_nv_status_t st;

    if (!out_version)
        return TPM_NV_BADARG;
    st = authz_read_record(TPM_NV_INDEX_AB_FLOOR, TPM_NV_INDEX_AB_SEQ,
                           TPM_RECORD_KIND_AB_FLOOR,
                           (uint32_t)sizeof(struct tpm_ab_floor_payload),
                           buf, (uint16_t)sizeof buf, &view, &counter);
    if (st != TPM_NV_OK)
        return st;
    if (tpm_record_ab_floor_version(&view, out_version) != TPM_RECORD_OK)
        return TPM_NV_CONTRACT;
    if (out_generation)
        *out_generation = counter;
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

    st = tpm_ab_floor_read(&cur_version, &counter);
    if (st == TPM_NV_OK) {
        /* Local refusal BEFORE spending a transaction. This is a convenience,
         * not the boundary: the boundary is that the authority granted THIS
         * record, and a lower version reaching the TPM would fail there too. */
        if (new_version < cur_version)
            return TPM_NV_MISMATCH;
    } else if (st != TPM_NV_UNINIT && st != TPM_NV_NOTFOUND) {
        /* Anything other than "no floor yet" is a real failure and must not be
         * papered over by writing a fresh floor on top of it -- that is how a
         * rollback gets laundered into a first install. */
        return st;
    } else {
        /* No floor yet: the counter still has to be read, because the record's
         * generation must be the value the counter will hold AFTER the commit
         * and a fresh counter does not start at zero. */
        st = tpm_nv_read_counter(TPM_NV_INDEX_AB_SEQ, &counter);
        if (st == TPM_NV_UNINIT)
            counter = 0u;
        else if (st != TPM_NV_OK)
            return st;
    }

    memset(&payload, 0, sizeof payload);
    payload.security_version = new_version;
    if (tpm_record_build(record, (uint32_t)sizeof record, TPM_RECORD_KIND_AB_FLOOR,
                         counter + 1u, (const uint8_t *)&payload,
                         (uint32_t)sizeof payload) != TPM_RECORD_OK)
        return TPM_NV_BADARG;

    return tpm_authz_write_record(TPM_NV_INDEX_AB_FLOOR, TPM_NV_INDEX_AB_SEQ,
                                  record, (uint16_t)sizeof record, counter, tr);
}

tpm_nv_status_t tpm_baseline_bind_verify(const uint8_t *blob, uint32_t blob_len,
                                         uint64_t *out_generation)
{
    uint8_t buf[TPM_BASELINE_BIND_LEN];
    uint8_t digest[SHA256_DIGEST_LEN];
    struct tpm_record_view view;
    const struct tpm_baseline_bind_payload *p;
    uint64_t counter = 0;
    tpm_nv_status_t st;

    if (!blob || blob_len == 0u)
        return TPM_NV_BADARG;

    st = authz_read_record(TPM_NV_INDEX_BASELINE_BIND, TPM_NV_INDEX_BASELINE_GEN,
                           TPM_RECORD_KIND_BASELINE,
                           (uint32_t)sizeof(struct tpm_baseline_bind_payload),
                           buf, (uint16_t)sizeof buf, &view, &counter);
    /* NO bind record is a LEGACY, unauthenticated baseline. It is reported, not
     * accepted and not auto-wrapped: the blob it would wrap is owner-writable,
     * so binding it now would authenticate whatever an attacker last wrote and
     * leave the boundary worse than no boundary. Migration is section 19's. */
    if (st == TPM_NV_NOTFOUND || st == TPM_NV_UNINIT)
        return TPM_NV_NOTFOUND;
    if (st != TPM_NV_OK)
        return st;

    p = (const struct tpm_baseline_bind_payload *)view.payload;
    if (p->blob_len != blob_len)
        return TPM_NV_MISMATCH;
    sha256(blob, blob_len, digest);
    if (memcmp(digest, p->blob_digest, SHA256_DIGEST_LEN) != 0)
        return TPM_NV_MISMATCH;
    if (out_generation)
        *out_generation = counter;
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

    st = tpm_nv_read_counter(TPM_NV_INDEX_BASELINE_GEN, &counter);
    if (st == TPM_NV_UNINIT)
        counter = 0u;
    else if (st != TPM_NV_OK)
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
