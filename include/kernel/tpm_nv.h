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
#include "kernel/crypto/sha256.h"

/* ---- Permanent / reserved handles (TPM 2.0 Part 2) ---- */
#define TPM_RH_OWNER     0x40000001u  /* storage (owner) hierarchy */
#define TPM_RH_PLATFORM  0x4000000Cu  /* platform hierarchy */
#define TPM_RH_NULL      0x40000007u  /* null hierarchy / no key */
#define TPM_RS_PW        0x40000009u  /* password authorization session */

/* ---- NV / session command codes (TPM 2.0 Part 2) ---- */
#define TPM2_CC_NV_UNDEFINE_SPACE_SPECIAL 0x0000011Fu
#define TPM2_CC_NV_UNDEFINE_SPACE 0x00000122u
#define TPM2_CC_NV_DEFINE_SPACE   0x0000012Au
#define TPM2_CC_POLICY_COMMAND_CODE 0x0000016Cu
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

/* ---- The two anti-rollback anchors, and why they are never one value ----
 *
 * BASELINE_GEN is a TPM_NT_COUNTER that advances by EXACTLY ONE per authorized
 * baseline rotation and stays bound to the blob it describes. AB_SEQ is a
 * separate TPM_NT_COUNTER that advances by exactly one per authorized A/B floor
 * transaction. Sharing one counter between them would make each consumer's
 * invariant unenforceable: a baseline rotation would silently satisfy a floor
 * advance and vice versa, so "the counter moved" would stop meaning "MY record
 * was authorized to change".
 *
 * AB_FLOOR is deliberately NOT a counter. The A/B security version may JUMP
 * (a fresh install at version 500, an upgrade skipping releases), and a TPM
 * counter can only be advanced one step at a time by TPM2_NV_Increment -- so a
 * counter cannot represent the value at all. The VALUE lives in this ordinary
 * authenticated DATA index and the UPDATE SEQUENCE lives in AB_SEQ; the record
 * content and the authorization that binds the two are owned elsewhere (the
 * authorized-record-transition work), not by this module. */
#define TPM_NV_INDEX_BASELINE_GEN 0x01800202u  /* TPM_NT_COUNTER, +1 per rotation */
#define TPM_NV_INDEX_AB_SEQ       0x01800203u  /* TPM_NT_COUNTER, +1 per floor txn */
#define TPM_NV_INDEX_AB_FLOOR     0x01800204u  /* ordinary data: the version record */

/* The baseline BIND record: a small policy-protected index that authenticates
 * the baseline blob living in the owner-writable TPM_NV_INDEX_BASELINE.
 *
 * An authPolicy is fixed at NV_DefineSpace and is hashed into the index Name,
 * so the enrolled baseline index can never ACQUIRE an authorization boundary in
 * place; the alternatives were to undefine and redefine it (opening a window
 * with NO baseline on the machine) or to copy its owner-writable contents into
 * a protected replacement (authenticating whatever an attacker last wrote).
 * Binding it from the side costs one small index and neither hazard: the blob
 * stays where it is, and a relabelled old blob fails the bind digest. */
#define TPM_NV_INDEX_BASELINE_BIND 0x01800205u  /* policy-protected bind record */

/* Bytes the A/B floor record occupies. The record LAYOUT is not this module's
 * to define; the index it lives in is, and an index cannot be defined without a
 * size. Sized to hold the paired (update sequence, security version) tuple plus
 * the digest that authorizes the transition, with room the record owner can
 * spend without an index migration. */
#define TPM_NV_AB_FLOOR_SIZE 96u

/* Owner-defined NV indices live in the TPM_HT_NV_INDEX space: the high byte of
 * the handle is TPM_HT_NV_INDEX (0x01). A handle outside it is not an NV index
 * at all, and every handle above must be DISTINCT -- two anchors sharing a
 * handle is precisely the collapse the two-index contract exists to prevent,
 * and it would be invisible at runtime because both would simply work. */
#define TPM_HT_NV_INDEX 0x01u
_Static_assert((TPM_NV_INDEX_OS_DATA >> 24) == TPM_HT_NV_INDEX &&
               (TPM_NV_INDEX_BASELINE >> 24) == TPM_HT_NV_INDEX &&
               (TPM_NV_INDEX_BASELINE_GEN >> 24) == TPM_HT_NV_INDEX &&
               (TPM_NV_INDEX_AB_SEQ >> 24) == TPM_HT_NV_INDEX &&
               (TPM_NV_INDEX_AB_FLOOR >> 24) == TPM_HT_NV_INDEX &&
               (TPM_NV_INDEX_BASELINE_BIND >> 24) == TPM_HT_NV_INDEX,
               "TPM NV index handle outside the TPM_HT_NV_INDEX space");
_Static_assert(TPM_NV_INDEX_OS_DATA != TPM_NV_INDEX_BASELINE &&
               TPM_NV_INDEX_OS_DATA != TPM_NV_INDEX_BASELINE_GEN &&
               TPM_NV_INDEX_OS_DATA != TPM_NV_INDEX_AB_SEQ &&
               TPM_NV_INDEX_OS_DATA != TPM_NV_INDEX_AB_FLOOR &&
               TPM_NV_INDEX_BASELINE != TPM_NV_INDEX_BASELINE_GEN &&
               TPM_NV_INDEX_BASELINE != TPM_NV_INDEX_AB_SEQ &&
               TPM_NV_INDEX_BASELINE != TPM_NV_INDEX_AB_FLOOR &&
               TPM_NV_INDEX_BASELINE_GEN != TPM_NV_INDEX_AB_SEQ &&
               TPM_NV_INDEX_BASELINE_GEN != TPM_NV_INDEX_AB_FLOOR &&
               TPM_NV_INDEX_AB_SEQ != TPM_NV_INDEX_AB_FLOOR &&
               TPM_NV_INDEX_BASELINE_BIND != TPM_NV_INDEX_OS_DATA &&
               TPM_NV_INDEX_BASELINE_BIND != TPM_NV_INDEX_BASELINE &&
               TPM_NV_INDEX_BASELINE_BIND != TPM_NV_INDEX_BASELINE_GEN &&
               TPM_NV_INDEX_BASELINE_BIND != TPM_NV_INDEX_AB_SEQ &&
               TPM_NV_INDEX_BASELINE_BIND != TPM_NV_INDEX_AB_FLOOR,
               "TPM NV index handles must be pairwise distinct");

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

/* Largest index this module will DEFINE. Deliberately separate from
 * TPM_NV_MAX_DATA: that one caps a single TRANSFER, and the read/write
 * wrappers chunk by offset past it, so treating it as the definition limit
 * makes an index that can be read and written in chunks impossible to
 * provision -- exactly the shape the versioned-baseline-growth work needs.
 * Bounded at the
 * PTP command-buffer scale so a define is still sanity-checked. */
#define TPM_NV_MAX_INDEX_SIZE 4096u

/* Layer 1: the record must stay definable AND readable in one transfer, or the
 * A/B consumers inherit a chunked read nobody has written. Growth past either
 * bound is a build failure here rather than a TPM_NV_ATTRS at provisioning
 * time on real firmware. */
_Static_assert(TPM_NV_AB_FLOOR_SIZE > 0u &&
               TPM_NV_AB_FLOOR_SIZE <= TPM_NV_MAX_DATA,
               "TPM_NV_AB_FLOOR_SIZE must fit one NV transfer");

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
    TPM_NV_CONTRACT  = 15,  /* the ENROLLED identity contract is itself
                             * malformed. Distinct from BADARG on purpose:
                             * BADARG means the CALLER passed something wrong
                             * and no transaction was issued, whereas this means
                             * PERSISTED state is corrupt, which is an
                             * authorized-recovery question rather than a
                             * programming error, and can be reported after a
                             * transaction has already run. */
    TPM_NV_RECREATED = 16,  /* the index is present, its Name is consistent and
                             * its DEFINITION matches the enrolled contract, but
                             * enrollment recorded it as written and the live
                             * index is not -- so it was destroyed and recreated.
                             * Never conflate this with TPM_NV_UNINIT: an
                             * uninitialized index reads as "no record yet",
                             * which downstream maps to first enrollment, and
                             * that is exactly how a destroyed anchor gets
                             * laundered into a fresh install. */
    TPM_NV_UNAVAIL   = 17,  /* the operation needs an update authority and none
                             * is provisioned. Deliberately NOT an error status:
                             * a kernel built without an authority key is
                             * CORRECTLY refusing every authorized write, the
                             * same way Secure Boot with an empty db refuses
                             * every image. A caller must not retry it, must not
                             * fall back to owner auth, and should report it as
                             * a configuration state rather than a fault. */
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

/* The transient-OBJECT twin of the above, for recovering a TPM2_LoadExternal
 * handle whose response the strict parser rejected. Type-checked to
 * TPM_HT_TRANSIENT (0x80) for the same reason the session extractor is checked
 * to the session types: FlushContext accepts both, so flushing the wrong kind
 * would evict unrelated TPM state rather than clean up this load. Never use
 * the result to authorize -- only to FlushContext. */
uint32_t tpm2_rsp_object_handle(const uint8_t *rsp, uint32_t len);

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

/* Same, for a command carrying n_sessions authorizations. A TPM2 response
 * carries exactly as many TPMS_AUTH_RESPONSE structures as the command carried
 * authorizations, so a two-authorization command (NV_UndefineSpaceSpecial: the
 * index under its own policy, then the platform hierarchy) answers with TWO --
 * and the one-session validator above rejects that success as malformed. The
 * count is a property of the COMMAND, so it is supplied by the builder's caller
 * rather than guessed from the response. n_sessions == 0 is a caller error and
 * returns 0. tpm_session_auth_response_ok is exactly this at n_sessions 1. */
int tpm_session_auth_response_n_ok(const uint8_t *rsp, uint32_t size,
                                   uint32_t auth_off, uint32_t n_sessions);

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

/* Same, for a command carrying n_sessions authorizations: the success-path
 * response-envelope check requires exactly that many TPMS_AUTH_RESPONSE
 * structures. Every other path is identical, and tpm_session_cmd_exec_seq is
 * this at n_sessions 1. Exists because a two-authorization command
 * (NV_UndefineSpaceSpecial) would otherwise have EVERY real success rejected as
 * a malformed envelope -- a failure that only appears against firmware that
 * actually executes the command. */
int tpm_session_cmd_exec_seq_n(tpm2_seq_t seq,
                               const uint8_t *cmd, uint32_t n, uint8_t *rsp,
                               uint32_t cap, uint32_t *out_rlen,
                               tpm_nv_status_t *out_st, uint32_t *out_rc,
                               uint32_t n_sessions);

/* Callback invoked under an open real PolicyPCR session: build + submit + parse
 * the authorized op using `session`; return its status. `seq` is the bounded
 * sequence the whole flow runs in -- submit through tpm_session_cmd_exec_seq()
 * with it, never a bare tpm2_submit. That misuse does NOT hang: the transport
 * sees the sequence's own busy gate and returns TPM_T_ERR_BUSY immediately,
 * surfacing as a spurious TPM_NV_BUSY that reads like TPM contention rather
 * than the caller's own mistake. (Same correction as tpm_nv_verified_op_fn; an
 * earlier revision of both comments claimed a deadlock without checking
 * tpm_transport.c, which refuses on the gate rather than blocking.) */
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

/* Define a POLICY-authorized monotonic counter: TPM_NT_COUNTER, 8 bytes, NO_DA,
 * POLICYWRITE + OWNERREAD and the supplied authPolicy.
 *
 * OWNERWRITE is deliberately absent, which is the entire point: the increment
 * of an anti-rollback anchor is the irreversible commit half of an authorized
 * transition, and an anchor an attacker can advance at will lets them
 * manufacture a counter-ahead-of-record state deliberately. OWNERREAD stays so
 * every boot can read the floor it is judged against. A separate entry point
 * rather than a mode flag on the owner-auth define, for the same reason the two
 * define hierarchies are separate builders. */
tpm_nv_status_t tpm_nv_define_counter_policy(uint32_t nv_index,
                                             const uint8_t *auth_policy,
                                             uint16_t policy_len);

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

/* In-sequence variants of the two reads above, for a caller that must hold ONE
 * bounded sequence across several reads and a write. Sequences do NOT nest, so
 * the self-sequencing wrappers are unusable there and a caller reaching for one
 * gets TPM_NV_BUSY rather than the read it asked for. Semantics are otherwise
 * identical -- the same shared parsers judge the response -- and seq == 0
 * submits unsequenced, exactly as tpm_session_cmd_exec_seq defines it. */
tpm_nv_status_t tpm_nv_read_identity_seq(tpm2_seq_t seq, uint32_t nv_index,
                                         struct tpm_nv_public *out_pub,
                                         int *out_name_ok);
tpm_nv_status_t tpm_nv_read_counter_seq(tpm2_seq_t seq, uint32_t nv_index,
                                        uint64_t *out);

/* Verified teardown of ONE handle (session or transient object) inside an open
 * sequence. Spends the sequence's separate cleanup allowance, retries a bounded
 * number of times, and accepts only proof that the handle is gone: SUCCESS, or
 * "no such handle". An unproven teardown is logged and the transport is left
 * usable rather than disabled.
 *
 * Any code that allocates a TPM handle must release it through THIS, not
 * through a bare FlushContext on the ordinary work budget: that path cannot run
 * once the work budget is spent, and discarding its result turns a transient
 * TPM_RC_RETRY into a permanent leak from a very small pool. */
void tpm_nv_flush_handle(tpm2_seq_t seq, uint32_t handle);

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

/* ============================================================================
 * Index IDENTITY and LIFECYCLE (the authorization boundary the index carries)
 *
 * Everything above authorizes an OPERATION on a handle. Nothing above proves
 * the handle still names the index that was enrolled: the handle number is a
 * value the CALLER supplies, and an attacker who can undefine and redefine an
 * index gets a fresh one, at the same handle, answering every cooperative
 * check. This block is the identity and lifecycle half.
 *
 * Two facts drive the whole design and both are easy to get backwards:
 *
 *   1. A Name is NOT stable across the index's first write. The TPM2 Name of
 *      an NV index is nameAlg || H(marshalled TPMS_NV_PUBLIC), and
 *      TPMA_NV_WRITTEN lives inside those attributes -- so the Name CHANGES the
 *      first time the index is written, and again when a lock status flips.
 *      Enrolling one Name and demanding it forever would reject the legitimate
 *      initialized index. So identity is checked at two levels: the live Name
 *      must be self-consistent with the live public area (a TPM that
 *      contradicts itself is a transport fault), and the ENROLLED contract is
 *      compared over normalized definition fields with the TPM-maintained
 *      status bits masked off, plus an EXPLICIT expected-written state.
 *
 *   2. Name equality does NOT detect recreation. An attacker who redefines the
 *      index with a byte-identical public area produces a byte-identical Name.
 *      What betrays the recreation is that a freshly defined index has
 *      TPMA_NV_WRITTEN CLEAR: an anchor that enrollment recorded as WRITTEN and
 *      that now reads back unwritten was destroyed and recreated, whatever its
 *      Name says. That is the signal, and it is why the expected-written state
 *      is part of the enrolled contract rather than a detail.
 *
 * What this block deliberately does NOT do is decide that a machine suffered a
 * legitimate TPM clear or replacement. A replaced TPM and an attacker who
 * deleted the anchor present the SAME observations, so inferring a benign clear
 * from them would hand the attacker the recovery path. The classifier reports
 * RECOVERY_REQUIRED and stops; clearing that state is an authorized transition
 * backed by evidence this module does not hold.
 * ========================================================================= */

/* nameAlg(2) + one nameAlg digest. SHA-256 is the only bank this module
 * computes, so 2 + 32. */
#define TPM_NV_NAME_MAX 34u
/* Layer 1: pin the Name length to the algorithm it is built from. Without this
 * a bank change would return a longer Name while the fixed-size buffers stayed
 * 34, and every test comparing against the same constant would still pass. */
_Static_assert(TPM_NV_NAME_MAX == 2u + SHA256_DIGEST_LEN,
               "TPM_NV_NAME_MAX must be nameAlg(2) + one SHA-256 digest");

/* Compute the TPM2 Name of an NV index: nameAlg (big-endian UINT16) followed by
 * H_nameAlg over the marshalled TPMS_NV_PUBLIC -- nvIndex(4) nameAlg(2)
 * attributes(4) authPolicy TPM2B(2 + N) dataSize(2), and NOT the TPM2B_NV_PUBLIC
 * size prefix that wraps it on the wire. Pure; no transport.
 *
 * `pub` supplies nameAlg, attributes, authPolicy and dataSize; nv_index is
 * passed separately because struct tpm_nv_public does not carry it (the parser
 * has already bound the response to the requested index by the time one exists).
 * Only TPM_ALG_SHA256 is supported -- another nameAlg is refused rather than
 * silently hashed with the wrong algorithm, which would produce a plausible
 * Name that matches nothing. Returns the Name length (34) or -1. */
int tpm2_nv_name_compute(uint32_t nv_index, const struct tpm_nv_public *pub,
                         uint8_t *out, uint32_t cap);

/* Extract the trailing TPM2B_NAME of a TPM2_NV_ReadPublic response -- the field
 * tpm2_parse_nv_read_public already bounds and then discards. Runs that strict
 * parser first, so a success here is over a fully validated response. Returns
 * the name length (0 when the TPM reported an empty name) or -1. An empty name
 * is a legal parse and a failed IDENTITY check; the two are kept separate so a
 * caller cannot mistake "the TPM told us nothing" for "the name matched". */
int tpm2_parse_nv_name(const uint8_t *rsp, uint32_t len, uint32_t nv_index,
                       uint8_t *out, uint32_t cap);

/* The enrolled identity contract for one index. Everything here is a
 * DEFINITION property: `attrs` is normalized (TPMA_NV_STATUS_MASK cleared), so
 * it survives the index being written and locked, and the mutable half is
 * carried explicitly by expect_written. */
struct tpm_nv_identity {
    uint32_t nv_index;
    uint16_t name_alg;
    uint32_t attrs;        /* normalized: TPMA_NV_STATUS_MASK bits are 0 */
    uint16_t data_size;
    uint16_t policy_len;
    uint8_t  auth_policy[TPM_NV_POLICY_MAX];
    uint8_t  expect_written; /* 1 = the anchor was written at enrollment */
};

/* Build an enrolled identity from an observed public area. Normalizes attrs and
 * records expect_written as supplied by the caller (the enroller knows whether
 * it wrote the index; the public area only says whether it is written NOW).
 * Returns TPM_NV_OK; TPM_NV_BADARG on a NULL argument or an authPolicy longer
 * than the contract can hold; and TPM_NV_ATTRS when expect_written is requested
 * for an index carrying TPMA_NV_CLEAR_STCLEAR, whose WRITTEN bit a TPM Reset
 * clears -- enrolling that combination would make an ordinary reboot
 * indistinguishable from an undefine/redefine. Pure; no transport. */
tpm_nv_status_t tpm_nv_identity_from_public(uint32_t nv_index,
                                            const struct tpm_nv_public *pub,
                                            int expect_written,
                                            struct tpm_nv_identity *out);

/* Compare an observed public area against an enrolled contract, over normalized
 * definition fields only. Returns TPM_NV_OK on a match, TPM_NV_MISMATCH on any
 * disagreement, TPM_NV_BADARG on a NULL argument, and TPM_NV_CONTRACT when
 * either side carries an authPolicy longer than TPM_NV_POLICY_MAX -- the
 * contract is exported for PERSISTENCE, so one read back from storage is
 * untrusted input, and a corrupt one is an authorized-recovery question rather
 * than API misuse. Deliberately does NOT judge
 * the written state -- that is a LIFECYCLE fact, and folding it in here would
 * report a destroyed-and-recreated anchor as a definition mismatch, which reads
 * as a provisioning error rather than an attack. Pure; no transport. */
tpm_nv_status_t tpm_nv_identity_match(const struct tpm_nv_identity *enrolled,
                                      const struct tpm_nv_public *observed);

/* What the caller learned about an anchor on this boot. Every field is an
 * OBSERVATION, never a conclusion. */
struct tpm_nv_lifecycle_obs {
    int      enrolled;       /* an enrolled contract exists for this anchor */
    int      index_present;  /* ReadPublic found the index */
    int      identity_ok;    /* observed public area matches the contract */
    int      name_ok;        /* live Name is self-consistent with the live public area */
    int      written;        /* TPMA_NV_WRITTEN is set on the live index */
    int      expect_written; /* enrollment recorded this anchor as written */
    /* An anchor that HAS a counter must not be judged without it. These are two
     * separate facts on purpose: "this anchor is counter-backed" is a property
     * of the ENROLLMENT, and "we managed to read it" is a property of this
     * BOOT. Collapsing them into one flag makes a failed counter read
     * indistinguishable from an anchor that never had a counter, and that
     * reads as ACCEPT -- missing evidence passing as evidence of safety. */
    int      counter_required; /* enrollment says this anchor is counter-backed */
    int      counter_known;  /* counter_value below was actually read */
    uint64_t counter_value;  /* live counter, when counter_known */
    uint64_t enrolled_counter; /* counter value enrollment recorded */
};

typedef enum {
    TPM_NV_LIFECYCLE_ACCEPT            = 0,
    /* Enrollment says this anchor was written; the live index is unwritten.
     * A freshly defined index is unwritten, so the anchor was destroyed and
     * recreated -- the attack the Name alone cannot see. */
    TPM_NV_LIFECYCLE_REFUSE_RECREATED  = 1,
    /* The live public area is not the enrolled definition, or the TPM's own
     * Name disagrees with the public area it just reported. */
    TPM_NV_LIFECYCLE_REFUSE_IDENTITY   = 2,
    /* The live counter is below what enrollment recorded. */
    TPM_NV_LIFECYCLE_REFUSE_ROLLBACK   = 3,
    /* The anchor is enrolled and the index is GONE. A replaced or cleared TPM
     * and an attacker who deleted the index are indistinguishable from here, so
     * this is a halt, not a diagnosis: clearing it requires an authorized
     * transition backed by evidence this module does not hold. Never treat it
     * as a benign clear, and never re-enroll over it -- that is exactly how a
     * rollback gets laundered into a fresh install. */
    TPM_NV_LIFECYCLE_RECOVERY_REQUIRED = 4,
    /* Nothing is enrolled: a first provisioning, not a verdict about an
     * existing anchor. */
    TPM_NV_LIFECYCLE_UNENROLLED        = 5,
    /* A counter-backed anchor whose counter could not be read. The index is
     * present and its identity holds, but the anti-rollback evidence is
     * MISSING, and missing evidence is not evidence of safety. Distinct from
     * RECOVERY_REQUIRED, which is about an ABSENT index: here the anchor is
     * intact and the read has to be retried or the boot refused. */
    TPM_NV_LIFECYCLE_REFUSE_INCOMPLETE = 6,
} tpm_nv_lifecycle_t;

/* Classify one anchor's observations. Pure, total, and deliberately blind to
 * everything except its argument -- it can classify observations, it cannot
 * establish their cause, and pretending otherwise is what turns the recovery
 * path into an attack surface. Returns UNENROLLED for a NULL argument, which is
 * the only answer that asserts nothing about an anchor. */
tpm_nv_lifecycle_t tpm_nv_lifecycle_classify(const struct tpm_nv_lifecycle_obs *o);

/* ---- Platform-authorized define (the ONLY path to POLICY_DELETE) ----
 *
 * Kept as a separate builder and a separate validator from the owner-auth
 * define rather than a hierarchy parameter threaded through both: the owner
 * path must not be ABLE to reach these attributes, and a shared entry point
 * with a mode flag is one wrong argument away from admitting them. */

/* Validate a define request's attributes for a PLATFORM-authorized define.
 * Identical to tpm_nv_attrs_valid except that TPMA_NV_POLICY_DELETE and
 * TPMA_NV_PLATFORMCREATE are permitted, and POLICY_DELETE additionally REQUIRES
 * an authPolicy: the whole point of the attribute is that deletion is
 * authorized by the index's own policy, so an index carrying it with no policy
 * could never be deleted by anyone. has_auth_policy is 1 when the define
 * supplies one. Returns TPM_NV_OK or TPM_NV_ATTRS. Pure; no transport. */
tpm_nv_status_t tpm_nv_attrs_valid_platform(uint32_t attrs, uint16_t data_size,
                                            uint16_t name_alg,
                                            int has_auth_policy);

/* TPM2_NV_DefineSpace under PLATFORM auth. Same wire shape as
 * tpm2_build_nv_define with TPM_RH_PLATFORM as the authHandle, validated with
 * tpm_nv_attrs_valid_platform. Returns the marshalled length or 0. */
uint32_t tpm2_build_nv_define_platform(uint8_t *buf, uint32_t cap, uint32_t nv_index,
                                       uint32_t attrs, uint16_t name_alg,
                                       const uint8_t *auth_policy,
                                       uint16_t auth_policy_len,
                                       uint16_t data_size);

/* TPM2_NV_UndefineSpaceSpecial: delete an index carrying
 * TPMA_NV_POLICY_DELETE. TWO authorized handles in order -- nvIndex, authorized
 * by a POLICY session satisfying the index's own authPolicy, then
 * TPM_RH_PLATFORM, authorized by platformAuth -- and therefore TWO
 * authorization areas and two response sessions. No parameters. Returns the
 * marshalled length or 0. */
uint32_t tpm2_build_nv_undefine_special(uint8_t *buf, uint32_t cap,
                                        uint32_t nv_index,
                                        uint32_t policy_session);

/* TPM2_PolicyCommandCode(policySession, code): binds a policy to ONE command,
 * so a delete policy authorizes deletion and nothing else. Without it the
 * index's authPolicy would authorize every policy-gated operation on the index,
 * which for a POLICY_DELETE anchor means the policy that permits deletion also
 * permits writing it. Returns the marshalled length or 0. */
uint32_t tpm2_build_policy_command_code(uint8_t *buf, uint32_t cap,
                                        uint32_t policy_session, uint32_t code);

/* Compute the authPolicy digest for a delete policy: a TRIAL session running
 * PolicyCommandCode(TPM2_CC_NV_UNDEFINE_SPACE_SPECIAL) then PolicyGetDigest,
 * flushed on every path by the same single-cleanup machinery the PCR-policy
 * digest uses. Writes 32 bytes to out (cap >= 32). */
tpm_nv_status_t tpm_nv_delete_policy_digest(uint8_t *out, uint32_t cap);

/* ---- Authorized-record-write primitives (the PolicyAuthorize construction) --
 *
 * Direct PolicySigned is NOT usable for an OFFLINE authority and this is the
 * reason these five builders exist instead. Its aHash binds nonceTPM, which the
 * TPM mints per session (TPM 2.0 Part 1 section 19.7.11: without nonceTPM the
 * assertion "may be used on any policy session on any TPM"), so an authority
 * that signs before it ever meets the TPM cannot produce one, and one that
 * omits it signs a replayable authorization.
 *
 * PolicyAuthorize inverts the problem. The index's authPolicy names only the
 * AUTHORITY KEY, permanently; the mutable half is an approved policy the
 * authority signs offline, and Part 1 section 19.7.5 specifies that a satisfied
 * PolicyAuthorize RESETS policyDigest to zero and re-extends it with the key's
 * Name, so the object's authPolicy never has to change when the approved policy
 * does. The approved policy carries PolicyCommandCode + PolicyCpHash + PolicyNV,
 * which is what pins one command, one exact set of record bytes, and one
 * counter generation. */

#define TPM2_CC_POLICY_NV         0x00000149u
#define TPM2_CC_LOAD_EXTERNAL     0x00000167u
#define TPM2_CC_POLICY_AUTHORIZE  0x0000016Au
#define TPM2_CC_POLICY_CP_HASH    0x0000016Eu
#define TPM2_CC_VERIFY_SIGNATURE  0x00000177u

/* TPMT_TK_VERIFIED tag. A "null ticket" is not zero bytes: it marshals as
 * tag(2) || hierarchy(4) || empty digest TPM2B(2) = 8 bytes, which is what a
 * TRIAL-session PolicyAuthorize takes when there is no signature to check. */
#define TPM2_ST_VERIFIED          0x8022u
#define TPM2_TK_VERIFIED_NULL_LEN 8u
/* Upper bound on a marshalled TPMT_TK_VERIFIED: tag(2) + hierarchy(4) +
 * digest TPM2B(2 + at most a 64-byte hash). A builder that trusted an unbounded
 * length could wrap its total and write past its own buffer, so the bound is
 * declared here rather than left to whatever the current parser happens to
 * produce. */
#define TPM2_TK_VERIFIED_MAX_LEN  72u

/* TPM_EO_* -- the comparison PolicyNV applies. Only the two orderings this
 * layer needs are defined; adding an unused operand set would be a wire
 * constant with no caller to keep it honest. */
#define TPM2_EO_EQ                0x0000u
/* TPM_EO assigns 0x0003 to UNSIGNED_GT and 0x0007 to UNSIGNED_GE; an earlier
 * revision of this header gave GE the GT value, which no current caller used
 * and which would have silently enforced a strict comparison for whoever
 * reached for it next. Both are defined so the pair is checkable. */
#define TPM2_EO_UNSIGNED_GT       0x0003u
#define TPM2_EO_UNSIGNED_GE       0x0007u

/* Compute cpHash for a command, per TPM 2.0 Part 1 section 18.7 equation (16):
 *
 *   cpHash := H_sessionAlg(commandCode || Name1 || Name2 || Name3 || parameters)
 *
 * `names` is the concatenation of the Names of the handles that REQUIRE
 * authorization, already marshalled (each is nameAlg || digest for an NV index),
 * in command order; `params` is the marshalled parameter area with the header
 * and the authorization area excluded.
 *
 * The Name of an NV index is nameAlg || H(nvPublicArea) and TPMA_NV_WRITTEN
 * lives INSIDE that public area, so an index's Name CHANGES on its first write.
 * A cpHash built from the enrollment-time Name therefore authorizes nothing once
 * the index is initialized -- always pass the LIVE Name.
 *
 * SHA-256 only, which is the session algorithm every policy session this module
 * opens uses. Pure; no transport. Returns TPM_NV_OK or TPM_NV_BADARG. */
tpm_nv_status_t tpm2_cphash_compute(uint32_t command_code,
                                    const uint8_t *names, uint32_t names_len,
                                    const uint8_t *params, uint32_t params_len,
                                    uint8_t out[32]);

/* TPM2_PolicyCpHash(policySession, cpHashA): pins the session to ONE command
 * with ONE exact parameter set. This is the assertion that makes an authority
 * signature cover the record BYTES rather than merely the act of writing.
 * Returns the marshalled length or 0. */
uint32_t tpm2_build_policy_cphash(uint8_t *buf, uint32_t cap,
                                  uint32_t policy_session,
                                  const uint8_t cphash[32]);

/* TPM2_PolicyNV(authHandle, nvIndex, policySession, operandB, offset, operation):
 * asserts a comparison against an NV index's CONTENTS, which is how the
 * approved policy names the exact counter generation it authorizes. authHandle
 * is the owner hierarchy (empty-password auth area, as with every other
 * owner-auth command here). Returns the marshalled length or 0. */
uint32_t tpm2_build_policy_nv(uint8_t *buf, uint32_t cap, uint32_t nv_index,
                              uint32_t policy_session,
                              const uint8_t *operand_b, uint16_t operand_len,
                              uint16_t offset, uint16_t operation);

/* TPM2_PolicyAuthorize(policySession, approvedPolicy, policyRef, keySign,
 * checkTicket): replaces the accumulated policyDigest with one derived from the
 * authority key's Name, provided the current digest equals `approvedPolicy` and
 * the ticket proves the authority signed it. Returns the marshalled length or 0.
 *
 * `key_sign` is the marshalled TPM2B_NAME of the authority key, NOT a handle:
 * the assertion binds the key's IDENTITY, so a different key that happens to be
 * loaded at the same handle authorizes nothing. */
uint32_t tpm2_build_policy_authorize(uint8_t *buf, uint32_t cap,
                                     uint32_t policy_session,
                                     const uint8_t *approved_policy,
                                     uint16_t approved_len,
                                     const uint8_t *policy_ref,
                                     uint16_t policy_ref_len,
                                     const uint8_t *key_sign, uint16_t key_sign_len,
                                     const uint8_t *ticket, uint32_t ticket_len);

/* TPM2_LoadExternal(inPrivate=empty, inPublic, hierarchy): loads the authority's
 * PUBLIC area so the TPM can verify its signature. Public-only load is what the
 * empty inPrivate selects (Part 1 section 29.3), and a hierarchy MUST be named
 * so the TPM knows which proof value to put in the verification ticket.
 *
 * The caller's public area must NOT carry nameAlg = TPM_ALG_NULL: Part 1
 * section 26 says such an object has NO Name, and PolicyAuthorize binds exactly
 * that Name. Returns the marshalled length or 0. */
uint32_t tpm2_build_load_external(uint8_t *buf, uint32_t cap,
                                  const uint8_t *in_public, uint16_t public_len,
                                  uint32_t hierarchy);

/* Parse a TPM2_LoadExternal response: the transient object handle plus the
 * TPM2B_NAME the TPM computed for it. Returns TPM_NV_OK, or TPM_NV_TRANSPORT on
 * a malformed or short reply. *out_handle is written only on success. */
tpm_nv_status_t tpm2_parse_load_external(const uint8_t *rsp, uint32_t len,
                                         uint32_t *out_handle,
                                         uint8_t *out_name, uint16_t name_cap,
                                         uint16_t *out_name_len);

/* TPM2_VerifySignature(keyHandle, digest, signature): turns a detached authority
 * signature into a TPMT_TK_VERIFIED ticket the TPM will accept from
 * PolicyAuthorize. `signature` is a marshalled TPMT_SIGNATURE.
 * Returns the marshalled length or 0. */
uint32_t tpm2_build_verify_signature(uint8_t *buf, uint32_t cap,
                                     uint32_t key_handle,
                                     const uint8_t digest[32],
                                     const uint8_t *signature, uint16_t sig_len);

/* Parse a TPM2_VerifySignature response into the raw marshalled TPMT_TK_VERIFIED
 * bytes, which is exactly what PolicyAuthorize wants back. Returns TPM_NV_OK, or
 * TPM_NV_TRANSPORT on a malformed, short, or over-long reply. */
tpm_nv_status_t tpm2_parse_verify_signature(const uint8_t *rsp, uint32_t len,
                                            uint8_t *out_ticket, uint32_t cap,
                                            uint32_t *out_len);

/* ---- The live identity gate (call BEFORE reading an index's contents) ---- */

/* Read an index's live public area together with the Name the TPM reports for
 * it, and cross-check the two: *out_name_ok is 1 only when the Name recomputed
 * from the reported public area equals the reported TPM2B_NAME. A TPM that
 * reports a Name inconsistent with its own public area has contradicted itself,
 * which is a transport-level fault rather than a policy decision, so the fact is
 * REPORTED here and judged by the caller. An EMPTY reported name is name_ok 0,
 * never a pass. out_pub may be NULL; out_name_ok may NOT -- discarding the Name
 * verdict would leave a caller reading TPM_NV_OK as though the identity held,
 * and tpm_nv_read_public already serves the public-area-only case. */
tpm_nv_status_t tpm_nv_read_identity(uint32_t nv_index,
                                     struct tpm_nv_public *out_pub,
                                     int *out_name_ok);

/* The gate itself: read the live identity and judge it against an enrolled
 * contract, WITHOUT reading a single content byte. Ordering is the whole point
 * -- a Name check that runs after the read has already trusted the bytes.
 * Returns TPM_NV_OK when the index is the enrolled one, TPM_NV_MISMATCH when it
 * is not (a failed self-consistency check, or a definition that no longer
 * matches), TPM_NV_RECREATED when the definition matches but the contract says
 * written and the live index is not, TPM_NV_CONTRACT when the ENROLLED contract
 * is itself malformed, TPM_NV_BADARG on a NULL contract, TPM_NV_NOTFOUND when
 * the index is gone, or ANY status the ReadPublic it performs can produce --
 * the executor's classifier passes its verdict through, so TPM_NV_TPMERR and
 * the other classified TPM errors are all reachable here. Do NOT write a
 * caller that treats the named values as exhaustive; they are the ones this
 * function DECIDES, not the whole set it can RETURN. It enforces the SAME
 * lifecycle check as
 * tpm_nv_verify_then; a caller switching on the status must handle RECREATED,
 * which is the one value that signals the destroy-and-recreate attack, and must
 * not fold CONTRACT into a default arm, since corrupt persisted state is an
 * authorized-recovery question rather than a device fault. out_pub (may be NULL) receives the observed public area on success so
 * a caller can feed tpm_nv_lifecycle_classify without a second read. */
tpm_nv_status_t tpm_nv_verify_identity(const struct tpm_nv_identity *enrolled,
                                       struct tpm_nv_public *out_pub);

/* Run `op` on an index whose identity was verified IN THE SAME bounded
 * transport sequence as the operation.
 *
 * tpm_nv_verify_identity above answers "is this the enrolled index RIGHT NOW",
 * and that answer EXPIRES when its sequence closes: the transport gate is
 * released, another CPU can undefine and recreate the index, and a following
 * read consumes bytes the gate never approved. Verifying and then reading in
 * two sequences is a check-then-use race, not an ordering guarantee -- so a
 * caller that reads CONTENTS after verifying uses this instead. `op` MUST
 * submit through tpm_session_cmd_exec_seq() with the supplied `seq`: a bare
 * tpm2_submit does NOT hang -- it sees the sequence's own busy gate and returns
 * TPM_T_ERR_BUSY immediately, surfacing as a spurious TPM_NV_BUSY that reads
 * like TPM contention rather than the caller's own mistake. (An earlier
 * revision of this comment claimed a deadlock; it was never checked against
 * tpm_transport.c, which refuses on the busy gate rather than blocking.)
 *
 * The LIFECYCLE state is enforced here too, not just the definition:
 * tpm_nv_identity_match normalizes TPMA_NV_WRITTEN away (it is not a definition
 * property), so an index recreated with a byte-identical public area passes
 * both the Name and the definition check. This path additionally compares the
 * live written state against the contract's expect_written and returns
 * TPM_NV_RECREATED without invoking `op`. Without that the op's NV_Read would
 * return TPM_RC_NV_UNINITIALIZED, which downstream reads as "no record yet".
 *
 * `pub` is the verified public area, so `op` need not re-read it. Returns the
 * op's status, or the verification failure WITHOUT ever invoking `op`. The
 * failures this function DECIDES are TPM_NV_MISMATCH, TPM_NV_RECREATED,
 * TPM_NV_CONTRACT (the enrolled contract is malformed) and TPM_NV_BADARG (NULL
 * contract or op). On top of those it passes through whatever the ReadPublic
 * classifier returns (NOTFOUND, TRANSPORT, BUSY, BUDGET, TPMERR, ...) and,
 * once `op` runs, whatever `op` returns -- so the full set is open by
 * construction and a caller needs a default arm. */
/* `nv_index` is the handle whose identity was just verified, and it is passed
 * explicitly so an op never has to name a handle of its own: an op that
 * hardcodes one is operating on an index this call did NOT verify, and the
 * sequence would happily serialize that mistake. Prefer
 * tpm_nv_verify_and_read() below, which owns the handle outright and removes
 * the opportunity; reach for this callback only when the operation is not a
 * plain read. */
typedef tpm_nv_status_t (*tpm_nv_verified_op_fn)(tpm2_seq_t seq,
                                                 uint32_t nv_index,
                                                 const struct tpm_nv_public *pub,
                                                 void *ctx);
tpm_nv_status_t tpm_nv_verify_then(const struct tpm_nv_identity *enrolled,
                                   tpm_nv_verified_op_fn op, void *ctx);

/* Verify the enrolled identity and read the index's contents, as ONE bounded
 * sequence, against the ENROLLED handle only.
 *
 * This is the API a content reader should use, and the reason it exists rather
 * than a documented obligation on tpm_nv_verify_then: the callback form cannot
 * stop an op from submitting against a DIFFERENT index, so a caller mix-up
 * among the anchors would read unverified bytes while the verification
 * reported success. Here the handle is the module's, not the caller's -- there
 * is no argument to get wrong.
 *
 * Owner-auth read (TPM_RS_PW), so it suits the ordinary data anchors. A
 * policy-protected index needs the policy-session form and belongs with the
 * consumer that holds the policy.
 *
 * The transfer is EXACT: a `cap` above TPM_NV_MAX_DATA is TPM_NV_BADARG rather
 * than a silent clamp, and a response whose length differs from the requested
 * `cap` is TPM_NV_TRANSPORT rather than a success. A short read reported as OK
 * would hand a record consumer truncated bytes it has no way to notice, and an
 * over-length one would copy bytes nobody asked for. *out_len is written only
 * on success and then always equals `cap`; it exists so the contract is
 * checkable, not because the length may vary.
 *
 * Returns TPM_NV_OK, any verification failure tpm_nv_verify_then can produce
 * (MISMATCH / RECREATED / CONTRACT / BADARG / NOTFOUND / transport / TPMERR),
 * TPM_NV_TRANSPORT when the response length differs from the request, or any
 * status the NV_Read itself classifies -- UNINIT for a never-written index,
 * plus RANGE / AUTH / LOCKED / ATTRS / TPMERR. The set is open: a caller needs
 * a default arm rather than an exhaustive switch. */
tpm_nv_status_t tpm_nv_verify_and_read(const struct tpm_nv_identity *enrolled,
                                       uint16_t offset, uint8_t *out,
                                       uint16_t cap, uint16_t *out_len);
