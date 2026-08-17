/* ============================================================================
 * tpm_nv.h -- TPM2 NV index storage + PCR-policy sessions (measured boot)
 *
 * The NV storage MECHANISM for measured-boot attestation baselines. The
 * baseline-enrollment layer owns the CONTENT (what gets enrolled); this module
 * owns how it is stored: define / undefine / write / read of NV indices, plus the
 * PCR-policy session machinery a baseline index is sealed to.
 *
 * Two index flavors:
 *   - Owner-authorized DATA index (OWNERREAD|OWNERWRITE): authorized with the
 *     owner-hierarchy password session (TPM_RS_PW). For generic OS data.
 *   - PCR-policy-protected BASELINE index (POLICYREAD|POLICYWRITE): read/write
 *     gated by a TPM2_PolicyPCR session over the derived baseline PCR mask
 *     (tpm_pcr_baseline_mask(), the PCR allocation table). Define/undefine still use
 *     owner auth -- owner auth is an operational bypass of any PCR policy, so it
 *     is restricted to lifecycle only, never to read/write.
 *
 * The pure marshaling helpers (tpm2_build_nv_* / tpm2_parse_nv_* / the RC
 * classifier) are MMIO-free and unit-tested through fixtures; the tpm_nv_*
 * wrappers drive the Phase-1 transport (tpm2_submit) and must NOT run in ISR
 * context (the transport serializes whole transactions and can block for a
 * TPM duration).
 *
 * Session lifetime: every path after TPM2_StartAuthSession routes through one
 * cleanup, which retries FlushContext a bounded number of times and accepts
 * only SUCCESS or "no such handle" as proof the session is gone. It does NOT
 * promise a leak can never happen -- two cases are outside its reach: a
 * CREATION whose handle cannot be recovered (the response never arrived, or it
 * arrived rc-SUCCESS but too malformed to yield a handle, so there is nothing
 * to flush) and a TEARDOWN that never gets proof within its budget.
 * Both are logged. Neither disables the transport, because a held session slot
 * degrades on its own into a definite StartAuthSession failure that every
 * caller already treats as a refusal, whereas disabling the transport would
 * also break PCR reads and attestation that open no session at all.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/tpm_transport.h"

/* ---- Permanent / reserved handles (TPM 2.0 Part 2) ---- */
#define TPM_RH_OWNER     0x40000001u  /* storage (owner) hierarchy */
#define TPM_RH_NULL      0x40000007u  /* null hierarchy / no key */
#define TPM_RS_PW        0x40000009u  /* password authorization session */

/* ---- NV / session command codes (TPM 2.0 Part 2) ---- */
#define TPM2_CC_NV_UNDEFINE_SPACE 0x00000122u
#define TPM2_CC_NV_DEFINE_SPACE   0x0000012Au
#define TPM2_CC_NV_INCREMENT      0x00000134u
#define TPM2_CC_NV_WRITE          0x00000137u
#define TPM2_CC_NV_WRITE_LOCK     0x00000138u
#define TPM2_CC_NV_READ           0x0000014Eu
#define TPM2_CC_NV_READ_PUBLIC    0x00000169u
#define TPM2_CC_START_AUTH_SESSION 0x00000176u
#define TPM2_CC_POLICY_PCR        0x0000017Fu
#define TPM2_CC_POLICY_GET_DIGEST 0x00000189u
#define TPM2_CC_FLUSH_CONTEXT     0x00000165u

/* ---- Session types (TPM_SE) ---- */
#define TPM2_SE_HMAC   0x00u
#define TPM2_SE_POLICY 0x01u
#define TPM2_SE_TRIAL  0x03u

/* TPMT_SYM_DEF "no symmetric" selector (unsalted/unbound policy session). */
#define TPM_ALG_NULL   0x0010u

/* ---- TPMA_NV attribute bits (TPM 2.0 Part 2, "Definition of (UINT32) TPMA_NV
 * Bits" -- Table 204 in rev 1.38) ----
 *
 * The WHOLE table is modelled here, not just the bits one caller happened to
 * need, because the gaps are load-bearing: bits 8-9 and 20-24 are Reserved and
 * a define that sets one is malformed, and bits 4-7 are a 4-bit TPM_NT field
 * rather than flags. The read-side numbering was previously off by one
 * (OWNERREAD/AUTHREAD/POLICYREAD at 18/19/20 instead of 17/18/19), which made
 * tpm_nv_define_data request AUTHREAD while tpm_nv_read authorized with owner
 * auth, and made the baseline define set Reserved bit 20 in place of
 * POLICYREAD. Nothing caught it because the only assertions compared the macros
 * against themselves; the wire-level fixtures in test_tpm_nv.c now carry
 * spec-literal attribute words as an independent oracle. */
#define TPMA_NV_PPWRITE       (1u << 0)
#define TPMA_NV_OWNERWRITE    (1u << 1)
#define TPMA_NV_AUTHWRITE     (1u << 2)
#define TPMA_NV_POLICYWRITE   (1u << 3)
/* bits 4-7: TPM_NT (index type) -- see TPMA_NV_TYPE below */
#define TPMA_NV_POLICY_DELETE (1u << 10)
#define TPMA_NV_WRITELOCKED   (1u << 11)  /* read-only status */
#define TPMA_NV_WRITEALL      (1u << 12)
#define TPMA_NV_WRITEDEFINE   (1u << 13)
#define TPMA_NV_WRITE_STCLEAR (1u << 14)
#define TPMA_NV_GLOBALLOCK    (1u << 15)
#define TPMA_NV_PPREAD        (1u << 16)
#define TPMA_NV_OWNERREAD     (1u << 17)
#define TPMA_NV_AUTHREAD      (1u << 18)
#define TPMA_NV_POLICYREAD    (1u << 19)
#define TPMA_NV_NO_DA         (1u << 25)
#define TPMA_NV_ORDERLY       (1u << 26)
#define TPMA_NV_CLEAR_STCLEAR (1u << 27)
#define TPMA_NV_READLOCKED    (1u << 28)  /* read-only status */
#define TPMA_NV_WRITTEN       (1u << 29)  /* read-only status: index has been written */
#define TPMA_NV_PLATFORMCREATE (1u << 30)
#define TPMA_NV_READ_STCLEAR  (1u << 31)

/* Reserved bit positions (8-9 and 20-24). A define setting any of these is
 * rejected locally rather than sent for the TPM to refuse. */
#define TPMA_NV_RESERVED_MASK 0x01F00300u

/* Attribute groups the validator reasons about. */
#define TPMA_NV_WRITE_AUTH_MASK (TPMA_NV_PPWRITE | TPMA_NV_OWNERWRITE | \
                                 TPMA_NV_AUTHWRITE | TPMA_NV_POLICYWRITE)
#define TPMA_NV_READ_AUTH_MASK  (TPMA_NV_PPREAD | TPMA_NV_OWNERREAD | \
                                 TPMA_NV_AUTHREAD | TPMA_NV_POLICYREAD)
/* TPM-maintained status bits: meaningless in a define request, so a caller
 * setting one is describing state it cannot choose. */
#define TPMA_NV_STATUS_MASK     (TPMA_NV_WRITELOCKED | TPMA_NV_READLOCKED | \
                                 TPMA_NV_WRITTEN)

/* ---- TPM_NT index type (TPMA_NV bits 4-7; Part 2 Table 202 in rev 1.38) ---- */
#define TPMA_NV_TPM_NT_SHIFT 4
#define TPMA_NV_TPM_NT_MASK  0x000000F0u
#define TPM_NT_ORDINARY 0x0u  /* ordinary data index (NV_Write / NV_Read) */
#define TPM_NT_COUNTER  0x1u  /* monotonic counter (NV_Increment; 8 bytes) */
#define TPM_NT_BITS     0x2u  /* bit field (NV_SetBits) */
#define TPM_NT_EXTEND   0x4u  /* PCR-like (NV_Extend) */
#define TPM_NT_PIN_FAIL 0x8u
#define TPM_NT_PIN_PASS 0x9u

/* Place / extract the TPM_NT field in a TPMA_NV word. */
#define TPMA_NV_TYPE(nt)      (((uint32_t)(nt) << TPMA_NV_TPM_NT_SHIFT) & TPMA_NV_TPM_NT_MASK)
#define TPMA_NV_GET_TYPE(a)   (((uint32_t)(a) & TPMA_NV_TPM_NT_MASK) >> TPMA_NV_TPM_NT_SHIFT)

/* A TPM_NT_COUNTER index holds exactly one big-endian UINT64. */
#define TPM_NV_COUNTER_SIZE 8u

/* No named attribute may fall in a Reserved position, and the TPM_NT field may
 * not collide with a flag -- the invariant the off-by-one numbering broke. */
_Static_assert((TPMA_NV_RESERVED_MASK & TPMA_NV_TPM_NT_MASK) == 0u,
               "TPMA_NV: TPM_NT field overlaps a reserved bit");
_Static_assert((TPMA_NV_RESERVED_MASK &
                (TPMA_NV_WRITE_AUTH_MASK | TPMA_NV_READ_AUTH_MASK |
                 TPMA_NV_STATUS_MASK | TPMA_NV_POLICY_DELETE | TPMA_NV_WRITEALL |
                 TPMA_NV_WRITEDEFINE | TPMA_NV_WRITE_STCLEAR | TPMA_NV_GLOBALLOCK |
                 TPMA_NV_NO_DA | TPMA_NV_ORDERLY | TPMA_NV_CLEAR_STCLEAR |
                 TPMA_NV_PLATFORMCREATE | TPMA_NV_READ_STCLEAR)) == 0u,
               "TPMA_NV: a named attribute sits in a reserved bit position");
_Static_assert((TPMA_NV_TPM_NT_MASK &
                (TPMA_NV_WRITE_AUTH_MASK | TPMA_NV_READ_AUTH_MASK)) == 0u,
               "TPMA_NV: TPM_NT field overlaps an authorization bit");
_Static_assert(TPMA_NV_TYPE(TPM_NT_PIN_PASS) == 0x90u,
               "TPMA_NV_TYPE places TPM_NT outside bits 4-7");

/* ---- Response-code formats + the NV warning/error codes we classify ----
 * The format selector is bit 7 (0x80) of the response code: 0 = format-0
 * (RC_VER1 / RC_WARN; the whole low value IS the error), 1 = format-1 (the
 * low bits carry a handle/parameter/session number that MUST be masked off
 * before comparing). The NV codes below are all format-0 and are exact-
 * compared -- running them through a format-1 mask would corrupt them and
 * hide locked / no-space / already-defined states. */
#define TPM2_RC_FMT1             0x00000080u  /* format selector bit */
#define TPM2_RC_NV_RANGE         0x00000146u
#define TPM2_RC_NV_SIZE          0x00000147u
#define TPM2_RC_NV_LOCKED        0x00000148u
#define TPM2_RC_NV_AUTHORIZATION 0x00000149u
#define TPM2_RC_NV_UNINITIALIZED 0x0000014Au
#define TPM2_RC_NV_SPACE         0x0000014Bu
#define TPM2_RC_NV_DEFINED       0x0000014Cu
/* Format-1 base errors (after masking the handle/parameter/session number). */
#define TPM2_RC_F1_ATTRIBUTES 0x00000082u  /* wrong index TYPE / illegal attrs */
#define TPM2_RC_F1_VALUE     0x00000084u
#define TPM2_RC_F1_HANDLE    0x0000008Bu
#define TPM2_RC_F1_AUTH_FAIL 0x0000008Eu
#define TPM2_RC_F1_SIZE      0x00000095u
#define TPM2_RC_F1_POLICY_FAIL 0x0000009Du

/* ---- Default OS-owned NV index handles (callers may override) ----
 * Owner-defined NV indices live in the TPM_HT_NV_INDEX space (0x01xxxxxx).
 * Section 6 selects the production handle; these defaults keep the data and
 * baseline indices distinct. */
#define TPM_NV_INDEX_OS_DATA  0x01800200u
#define TPM_NV_INDEX_BASELINE 0x01800201u

/* Wall-clock budget the session-cleanup FlushContext waits for the transport
 * busy gate to clear (matches the longest contending sequence -- the RNG
 * bounded-collect budget). Bounds the no-leak cleanup wait. */
#define TPM_NV_FLUSH_BUDGET_MS 2000u

/* Cumulative wall-clock budget for ONE logical NV operation, spanning every
 * command in it. The per-command PTP timeouts reset on each FIFO burst, so a
 * slow-but-responsive TPM can spend multiple seconds per command without any of
 * them timing out -- against a whole-boot target measured in seconds. The
 * budget belongs at this layer rather than per consumer: they all share one
 * transport, so per-consumer bounds would each stay inside their own limit and
 * still blow the boot between them.
 *
 * The cleanup reserve is NOT part of the work budget. It is claimed only when
 * the work budget has expired and a mandatory teardown (FlushContext) still has
 * to run, so an overrun cannot leave a started session unflushable -- a leaked
 * handle erodes the TPM's small session pool until reboot, which is a worse
 * outcome than the slow boot the budget exists to prevent. */
#define TPM_NV_OP_BUDGET_MS         3000u
/* The teardown allowance is a POLICY choice, not a spec-derived bound. It is
 * deliberately NOT sized to a worst case a conforming TPM can never exceed --
 * no such useful number exists here, because the PTP restarts its burst
 * deadline on every chunk (see tpm_transport.h). Two seconds is what this boot
 * is willing to spend proving a session was released, and an overrun is
 * REPORTED rather than escalated: see nv_flush(). */
#define TPM_NV_FLUSH_RETRIES        3u
#define TPM_NV_OP_CLEANUP_BUDGET_MS 2000u

/* Largest NV payload a single tpm_nv_write/read moves in one transaction.
 * A measured-boot baseline (golden PCRs + image hashes + metadata) fits well
 * under this; callers needing more chunk by `offset`. Bounds the wrapper stack
 * command/response buffers. PTP TPMs advertise a 1 KiB+ NV buffer, so 512 is
 * always within the device's MAX_NV_BUFFER_SIZE. */
#define TPM_NV_MAX_DATA 512u

/* ---- Status (one classified outcome; degraded states never wedge) ---- */
typedef enum {
    TPM_NV_OK        = 0,   /* success (data copied / index defined) */
    TPM_NV_LOCKED    = 1,   /* index read/write locked (TPM_RC_NV_LOCKED) */
    TPM_NV_NOSPACE   = 2,   /* TPM out of NV space (TPM_RC_NV_SPACE) */
    TPM_NV_DEFINED   = 3,   /* index already defined (TPM_RC_NV_DEFINED) */
    TPM_NV_UNINIT    = 4,   /* defined but never written (TPM_RC_NV_UNINITIALIZED) */
    TPM_NV_RANGE     = 5,   /* offset/size out of range (NV_RANGE/NV_SIZE/value) */
    TPM_NV_AUTH      = 6,   /* authorization / PCR policy failed */
    TPM_NV_NOTFOUND  = 7,   /* index not defined (TPM_RC_HANDLE) */
    TPM_NV_BADARG    = 8,   /* caller argument error (no transaction issued) */
    TPM_NV_TRANSPORT = 9,   /* transport failure / malformed response / no TPM */
    TPM_NV_BUSY      = 10,  /* another transaction in flight -- transient, retry */
    TPM_NV_TPMERR    = 11,  /* other classified TPM rc (not one of the above) */
    TPM_NV_ATTRS     = 12,  /* illegal attributes, or the op is wrong for this
                             * index TYPE (increment on a non-counter) */
    TPM_NV_BUDGET    = 13,  /* operation abandoned: cumulative budget exhausted.
                             * Distinct from TRANSPORT: the TPM answered, it was
                             * just slower than the boot path will wait for, and
                             * the transport stays usable. */
    TPM_NV_MISMATCH  = 14,  /* index exists with a DIFFERENT public area than the
                             * one requested -- never silently reused */
} tpm_nv_status_t;

/* Classify a raw TPM2 response code into tpm_nv_status_t. Format-first: a
 * format-0 code (bit 7 clear) is exact-compared against the NV warning/error
 * codes; a format-1 code (bit 7 set) is masked to its base error first. Never
 * returns a value outside the enum -- a degraded TPM never wedges the caller. */
tpm_nv_status_t tpm_nv_classify_rc(uint32_t rc);

/* ---- Pure marshaling seam (MMIO-free, fixture-tested) ----
 * Each returns the marshaled command length, or 0 on a bad argument / too-small
 * buffer. Auth areas use the password session (TPM_RS_PW) unless a real policy
 * session handle is supplied. */

/* TPM2_NV_DefineSpace under owner auth. attrs is the TPMA_NV bitmap; auth_policy
 * (auth_policy_len bytes, 0 for none) is the index's authPolicy; data_size is the
 * index size. */
uint32_t tpm2_build_nv_define(uint8_t *buf, uint32_t cap, uint32_t nv_index,
                              uint32_t attrs, uint16_t name_alg,
                              const uint8_t *auth_policy, uint16_t auth_policy_len,
                              uint16_t data_size);

/* Validate a define request's attributes against the TPMA_NV rules BEFORE the
 * command goes on the wire, so an illegal shape is a local, named refusal
 * instead of an opaque TPM_RC_ATTRIBUTES. Checks: no Reserved bits; TPM_NT is a
 * defined type; no TPM-maintained status bit (WRITELOCKED/READLOCKED/WRITTEN);
 * at least one read AND one write authorization bit (an index nobody may read
 * or nobody may write is a provisioning mistake, not a policy); and, for
 * COUNTER and BITS, dataSize exactly 8, with CLEAR_STCLEAR also forbidden on a
 * counter -- one that a TPM Reset can return to its pre-written state is not a
 * monotonic anchor. An EXTEND index must be exactly one name_alg digest. The
 * two PIN types are REFUSED outright: they carry authorization rules this
 * module has no consumer for and therefore does not validate, and accepting
 * them after a size check alone would pass locally and fail on real firmware. POLICY_DELETE and PLATFORMCREATE are refused outright
 * because every define this module builds is owner-authorized and both need the
 * platform hierarchy, so accepting them would produce a request that validates
 * locally and then fails on real firmware only.
 * Returns TPM_NV_OK or TPM_NV_ATTRS. Pure; no transport. */
tpm_nv_status_t tpm_nv_attrs_valid(uint32_t attrs, uint16_t data_size,
                                   uint16_t name_alg);

/* TPM2_NV_UndefineSpace under owner auth. */
uint32_t tpm2_build_nv_undefine(uint8_t *buf, uint32_t cap, uint32_t nv_index);

/* TPM2_NV_Increment: advance a TPM_NT_COUNTER index by exactly one. Two handles
 * (authHandle carrying the session, then nvIndex) and no parameters -- the same
 * handle-area shape as NV_Write without its data/offset tail. */
uint32_t tpm2_build_nv_increment(uint8_t *buf, uint32_t cap, uint32_t auth_handle,
                                 uint32_t nv_index, uint32_t session_handle);

/* TPM2_NV_WriteLock: inhibit further writes to an index. Same shape as
 * NV_Increment. Whether the lock survives a TPM Reset is a property of the
 * INDEX (WRITEDEFINE = permanent, WRITE_STCLEAR = until reset), not of this
 * command, so it is chosen at define time. */
uint32_t tpm2_build_nv_write_lock(uint8_t *buf, uint32_t cap, uint32_t auth_handle,
                                  uint32_t nv_index, uint32_t session_handle);

/* TPM2_NV_Write. session_handle = TPM_RS_PW for owner/index-password auth, or a
 * real policy session handle for a policy-protected index. */
uint32_t tpm2_build_nv_write(uint8_t *buf, uint32_t cap, uint32_t auth_handle,
                             uint32_t nv_index, uint32_t session_handle,
                             uint16_t offset, const uint8_t *data, uint16_t len);

/* TPM2_NV_Read. Same session_handle contract as write. */
uint32_t tpm2_build_nv_read(uint8_t *buf, uint32_t cap, uint32_t auth_handle,
                            uint32_t nv_index, uint32_t session_handle,
                            uint16_t size, uint16_t offset);

/* Parse a TPM2_NV_Read response: locates the parameter area through
 * tpm2_rsp_params (session-aware), bounds the TPM2B_MAX_NV_BUFFER, copies the
 * data to out. Returns the byte count (>= 0) or -1 on malformed/failed. */
int tpm2_parse_nv_read(const uint8_t *rsp, uint32_t len,
                       uint8_t *out, uint32_t out_cap);

/* TPM2_NV_ReadPublic (no auth). */
uint32_t tpm2_build_nv_read_public(uint8_t *buf, uint32_t cap, uint32_t nv_index);

/* Parse a TPM2_NV_ReadPublic response: extracts dataSize + attributes from the
 * TPMS_NV_PUBLIC, binds the public area to the requested nv_index (rejects a
 * response naming a different index), and requires exact parameter consumption
 * (TPM2B_NV_PUBLIC + TPM2B_NAME, nothing else). Returns 0 + sets outs, or -1 on
 * malformed/failed/index-mismatch. */
int tpm2_parse_nv_read_public(const uint8_t *rsp, uint32_t len, uint32_t nv_index,
                              uint16_t *out_size, uint32_t *out_attrs);

/* Longest authPolicy a TPMS_NV_PUBLIC can carry for any defined hash (SHA-512). */
#define TPM_NV_POLICY_MAX 64u

/* Every DEFINITION-bearing field of a TPMS_NV_PUBLIC. Size and attributes alone
 * do not identify an index: the authPolicy IS the access-control rule for a
 * policy-protected index and nameAlg determines its Name, so two indexes
 * agreeing on size and attributes can still be governed by entirely different
 * PCR state. Comparing only the first two would accept an index whose policy
 * someone else chose. */
struct tpm_nv_public {
    uint16_t data_size;
    uint32_t attrs;
    uint16_t name_alg;
    uint16_t policy_len;
    uint8_t  auth_policy[TPM_NV_POLICY_MAX];
};

/* Parse a TPM2_NV_ReadPublic response into the FULL public area. Applies the
 * same strict validation as tpm2_parse_nv_read_public (which it runs first), and
 * additionally requires the authPolicy to be ABSENT or exactly one nameAlg
 * digest: it refuses an oversized one rather than truncating it (a truncated
 * policy would compare equal to a different policy sharing its prefix) and
 * refuses any other length, which real hardware never produces.
 *
 * TRANSACTIONAL: `out` is written only after every check passes, so a -1 leaves
 * it byte-for-byte as the caller left it. Returns 0 + fills out, or -1. */
int tpm2_parse_nv_public_full(const uint8_t *rsp, uint32_t len, uint32_t nv_index,
                              struct tpm_nv_public *out);

/* TPM2_StartAuthSession(tpmKey=NULL, bind=NULL, sym=NULL, authHash=SHA256).
 * session_type = TPM2_SE_TRIAL (compute authPolicy) or TPM2_SE_POLICY (use it).
 * nonce_caller (>= 16 bytes recommended) is the caller nonce. */
uint32_t tpm2_build_start_auth_session(uint8_t *buf, uint32_t cap,
                                       uint8_t session_type, uint16_t auth_hash,
                                       const uint8_t *nonce_caller, uint16_t nonce_len);

/* Parse a TPM2_StartAuthSession response: returns the session handle, or 0 on
 * malformed/failed (a handle of 0 is never a valid session). Strict -- also
 * validates the nonceTPM TPM2B bounds, so a structurally-broken reply yields 0. */
uint32_t tpm2_parse_start_auth_session(const uint8_t *rsp, uint32_t len);

/* Extract the RAW session handle from any rc-success StartAuthSession response
 * with at least a 4-byte handle, WITHOUT validating the rest of the structure.
 * Returns the handle (possibly nonzero) or 0 if absent. For CLEANUP ONLY: when
 * the strict parser rejects a malformed-nonce reply that still allocated a
 * session, this recovers the handle so it can be best-effort flushed rather
 * than leaked. Never use the result to authorize -- only to FlushContext. */
uint32_t tpm2_rsp_session_handle(const uint8_t *rsp, uint32_t len);

/* TPM2_PolicyPCR(policySession, pcrDigest=empty, pcrs=<one bank+selection>).
 * The 3-byte pcr_select bitmap selects PCRs 0..23 in the `alg` bank. */
uint32_t tpm2_build_policy_pcr(uint8_t *buf, uint32_t cap, uint32_t policy_session,
                               uint16_t alg, const uint8_t pcr_select[3]);

/* TPM2_PolicyGetDigest(policySession). */
uint32_t tpm2_build_policy_get_digest(uint8_t *buf, uint32_t cap,
                                      uint32_t policy_session);

/* Parse a TPM2_PolicyGetDigest response: copies the policyDigest to out.
 * Returns the digest length (> 0) or -1 on malformed/failed. */
int tpm2_parse_policy_get_digest(const uint8_t *rsp, uint32_t len,
                                 uint8_t *out, uint32_t out_cap);

/* TPM2_FlushContext(flushHandle). The handle is a PARAMETER (not a handle-area
 * entry) per the spec, so this command carries no handle area. */
uint32_t tpm2_build_flush_context(uint8_t *buf, uint32_t cap, uint32_t handle);

/* Fill a 3-byte PCR-select bitmap (PCRs 0..23) from an arbitrary PCR mask (bit i
 * == PCR i). The shared building block for any PCR policy (baseline, seal, ...). */
void tpm_pcr_mask_to_select(uint32_t mask, uint8_t out_sel[3]);

/* Fill a 3-byte PCR-select bitmap from the derived baseline mask
 * (tpm_pcr_baseline_mask()). Thin wrapper over tpm_pcr_mask_to_select. */
void tpm_nv_baseline_pcr_select(uint8_t out_sel[3]);

/* ---- Shared policy-session seam (reused by NV policy ops + sealed secrets) ----
 * The single-cleanup PolicyPCR-session machinery, extracted so consumers do not
 * re-implement the session-handle-leak-on-error/BUSY hazard. Not ISR-safe. */

/* Validate the trailing one-session TPMS_AUTH_RESPONSE of a session-tagged
 * response (nonceTPM TPM2B + sessionAttributes + hmac TPM2B), starting at
 * auth_off and consuming the response EXACTLY to `size`. Each TPM2B is bounded
 * against the declared size and the TPM2B_HA 64-byte max, so a crafted auth area
 * claiming bytes past the response is rejected. Returns 1 if well-formed, else 0.
 * Shared by the session-command path and the CreatePrimary/Load handle parser so
 * one validator gates every session-authorized success. */
int tpm_session_auth_response_ok(const uint8_t *rsp, uint32_t size, uint32_t auth_off);

/* Submit one session-authorized TPM2 command and classify the response. On
 * TPM_NV_OK leaves rsp intact (+ *out_rlen) for the caller to parse; validates
 * the response code AND, for ST_SESSIONS successes, the response auth area.
 * *out_rc (may be NULL) receives the raw TPM response code whenever a response
 * header was parsed (success or TPM error); it is left untouched on a pre-parse
 * transport/BUSY failure, so callers pre-initialize it. Returns 0 on success
 * (with *out_st = TPM_NV_OK), -1 otherwise (*out_st set). */
int tpm_session_cmd_exec(const uint8_t *cmd, uint32_t n, uint8_t *rsp, uint32_t cap,
                         uint32_t *out_rlen, tpm_nv_status_t *out_st,
                         uint32_t *out_rc);

/* Like tpm_session_cmd_exec, but submits inside the bounded sequence `seq`
 * (tpm2_seq_run). seq == 0 submits unsequenced, which is what
 * tpm_session_cmd_exec itself is. A budget expiry surfaces as TPM_NV_BUDGET,
 * never TPM_NV_TRANSPORT, so a caller can tell "too slow for this boot" from
 * "the device misbehaved". *out_rc AND *out_rlen are both set whenever a
 * response header was PARSED, including on an error response -- a caller that
 * must judge the response ENVELOPE itself (nv_flush does, because FlushContext
 * carries no auth area for the generic check to validate) needs the received
 * length on the error path too. */
int tpm_session_cmd_exec_seq(tpm2_seq_t seq,
                             const uint8_t *cmd, uint32_t n, uint8_t *rsp,
                             uint32_t cap, uint32_t *out_rlen,
                             tpm_nv_status_t *out_st, uint32_t *out_rc);

/* Callback invoked under an open real PolicyPCR session: build + submit + parse
 * the authorized op using `session`; return its status. `seq` is the bounded
 * sequence the whole flow runs in -- submit through tpm_session_cmd_exec_seq()
 * with it, never a bare tpm2_submit, which would deadlock against the gate the
 * sequence already holds. */
typedef tpm_nv_status_t (*tpm_policy_op_fn)(tpm2_seq_t seq, uint32_t session,
                                            void *ctx);

/* Open a real POLICY session, satisfy TPM2_PolicyPCR over `sel` in bank `alg`,
 * invoke op(session, ctx), then FlushContext the session on EVERY path (op
 * error, BUSY, or a malformed-handle leak). Returns the op status (or the
 * session-setup failure). */
tpm_nv_status_t tpm_policy_session_run(uint16_t alg, const uint8_t sel[3],
                                       tpm_policy_op_fn op, void *ctx);

/* Compute the authPolicy digest for a PolicyPCR over `sel` in bank `alg` via a
 * TRIAL session (StartAuthSession(TRIAL) + PolicyPCR + PolicyGetDigest + flush).
 * Writes the 32-byte SHA-256 policy digest to out (cap >= 32). */
tpm_nv_status_t tpm_policy_pcr_digest(uint16_t alg, const uint8_t sel[3],
                                      uint8_t *out, uint32_t cap);

/* ---- High-level wrappers (Phase-1 transport; not ISR-safe) ---- */

/* Define an index with caller-chosen attributes. Validates them locally
 * (tpm_nv_attrs_valid) and, when the index already exists, reads its public
 * area back and compares dataSize + the requested attributes: an identical
 * definition is idempotent TPM_NV_OK, a DIFFERENT one is TPM_NV_MISMATCH and is
 * never silently reused. That check is the fail-closed half of the read-bit
 * correction -- a machine holding an index defined under the old, wrong
 * attributes must not have it quietly accepted as the anchor it is not.
 * Re-defining such an index means undefine + redefine, which is an authorized
 * lifecycle operation this layer deliberately does not perform. */
tpm_nv_status_t tpm_nv_define_ex(uint32_t nv_index, uint32_t attrs,
                                 uint16_t data_size,
                                 const uint8_t *auth_policy, uint16_t policy_len);

/* Define a monotonic counter index (TPM_NT_COUNTER, 8 bytes, NO_DA).
 * access_attrs supplies the read/write authorization bits ONLY; the type, size
 * and NO_DA are fixed here so a caller cannot accidentally define a "counter"
 * that is really an ordinary index. access_attrs MUST include
 * OWNERREAD|OWNERWRITE: tpm_nv_increment and tpm_nv_read_counter authorize with
 * owner auth, so a policy-only counter would be provisioned and then be
 * unusable by this module's own wrappers. A policy-authorized counter needs
 * policy-authorized wrappers and belongs with the authorization construction. */
tpm_nv_status_t tpm_nv_define_counter(uint32_t nv_index, uint32_t access_attrs);

/* Define an owner-authorized OS data index (OWNERREAD|OWNERWRITE|NO_DA). */
tpm_nv_status_t tpm_nv_define_data(uint32_t nv_index, uint16_t data_size);

/* Define the PCR-policy-protected baseline index (POLICYREAD|POLICYWRITE|NO_DA):
 * a trial session over the baseline PCR mask computes the authPolicy, then the
 * index is defined under owner auth bound to that policy. */
tpm_nv_status_t tpm_nv_define_baseline(uint32_t nv_index, uint16_t data_size);

/* Undefine an index under owner auth. */
tpm_nv_status_t tpm_nv_undefine(uint32_t nv_index);

/* Owner-auth (password) write/read for a data index. */
tpm_nv_status_t tpm_nv_write(uint32_t nv_index, uint16_t offset,
                             const uint8_t *data, uint16_t len);
tpm_nv_status_t tpm_nv_read(uint32_t nv_index, uint16_t offset,
                            uint8_t *out, uint16_t cap, uint16_t *out_len);

/* PCR-policy write/read for the baseline index: opens a real policy session,
 * satisfies PolicyPCR over the baseline mask (so the current PCR state must
 * match enrollment), runs the NV op, then flushes the session on every path. */
tpm_nv_status_t tpm_nv_policy_write(uint32_t nv_index, uint16_t offset,
                                    const uint8_t *data, uint16_t len);
tpm_nv_status_t tpm_nv_policy_read(uint32_t nv_index, uint16_t offset,
                                   uint8_t *out, uint16_t cap, uint16_t *out_len);

/* Owner-auth increment of a TPM_NT_COUNTER index by exactly one. An increment
 * against a non-counter index is TPM_NV_ATTRS and against a write-locked one is
 * TPM_NV_LOCKED -- both classified, so neither wedges the caller. Does NOT
 * retry: the completion of an abandoned increment is unknown, and a blind retry
 * could advance the counter twice. */
tpm_nv_status_t tpm_nv_increment(uint32_t nv_index);

/* Owner-auth write-lock of an index. Idempotent from the caller's side: locking
 * an already-locked index reports TPM_NV_LOCKED, which is the desired state. */
tpm_nv_status_t tpm_nv_write_lock(uint32_t nv_index);

/* Read a TPM_NT_COUNTER index as a UINT64. Requests exactly 8 bytes at offset 0
 * and decodes the big-endian value only after an exact-length read; *out is
 * untouched on any failure. A never-incremented counter reports TPM_NV_UNINIT
 * and is NOT synthesized as zero -- the first increment of a recreated index
 * may land above a previous value, so "uninitialized" and "zero" are different
 * facts and conflating them is how a rollback gets laundered. */
tpm_nv_status_t tpm_nv_read_counter(uint32_t nv_index, uint64_t *out);

#ifdef KERNEL_TESTS
/* ---- Test seam (kernel unit tests only) ----
 * Shrink the per-operation budget so the expiry / cleanup-reserve paths are
 * reachable without a test spending real seconds against a 3-second deadline.
 * Pair every set with a reset at teardown. */
void tpm_nv_test_set_op_budget(uint32_t work_ms, uint32_t cleanup_ms);
void tpm_nv_test_reset_op_budget(void);
#endif

/* Read an index's public size + attributes (no auth). */
tpm_nv_status_t tpm_nv_read_public(uint32_t nv_index, uint16_t *out_size,
                                   uint32_t *out_attrs);
