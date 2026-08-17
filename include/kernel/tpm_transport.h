/* ============================================================================
 * tpm_transport.h -- TPM2 command transport (TIS/FIFO + CRB)
 *
 * Kernel-side TPM 2.0 command submission per TCG PC Client Platform TPM
 * Profile (PTP) 1.05: TIS/FIFO register interface with burstCount
 * chunking, and the CRB control-area interface. Discovery via the ACPI
 * TPM2 table (start method selects the interface), with a TIS probe at
 * the fixed PTP base when the table is absent.
 *
 * Phase contract: tpm_transport_init() runs in boot Phase 1 AFTER
 * acpi_init() and boot_timing_init() (needs validated ACPI tables, UC
 * MMIO mappings, and the calibrated TSC for poll deadlines). The Phase
 * 0 tpm_init() event-log parse in tpm.c is independent and earlier.
 *
 * Concurrency contract: tpm2_submit() serializes the WHOLE transaction
 * (locality + FIFO/CRB buffers + status updates) via an ownership flag
 * under a short-hold spinlock. A second caller during an in-flight
 * transaction gets TPM_T_ERR_BUSY -- commands never interleave, and no
 * CPU spins for a TPM-duration (up to seconds) lock hold. Never call
 * from ISR context.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Interface kind (discovered at init) ---- */
#define TPM_T_IFACE_NONE 0
#define TPM_T_IFACE_TIS  1
#define TPM_T_IFACE_CRB  2

/* ---- tpm2_submit() error returns (negative; >= 0 is response length) ---- */
#define TPM_T_ERR_NODEV    (-1)  /* no TPM / transport not initialized */
#define TPM_T_ERR_ARG      (-2)  /* bad buffer / length arguments */
#define TPM_T_ERR_BUSY     (-3)  /* another transaction is in flight */
#define TPM_T_ERR_TIMEOUT  (-4)  /* poll deadline expired (transport now sticky-failed) */
#define TPM_T_ERR_IO       (-5)  /* protocol violation (Expect/dataAvail state wrong) */
#define TPM_T_ERR_RESPONSE (-6)  /* malformed response header / oversized response */
#define TPM_T_ERR_FAILED   (-7)  /* transport previously wedged (sticky) */
#define TPM_T_ERR_BUDGET   (-8)  /* bounded-sequence budget exhausted (NOT a wedge) */
#define TPM_T_ERR_SEQ      (-9)  /* submit outside, or with a stale, bounded sequence */

/* ---- TPM2 command/response constants (TPM 2.0 Part 2) ---- */
#define TPM2_ST_NO_SESSIONS      0x8001u
#define TPM2_ST_SESSIONS         0x8002u
#define TPM2_ST_RSP_COMMAND      0x00C4u  /* response to an unrecognized command tag */
#define TPM2_CC_STARTUP          0x00000144u
#define TPM2_CC_GET_CAPABILITY   0x0000017Au
#define TPM2_CC_GET_RANDOM       0x0000017Bu
#define TPM2_CC_PCR_READ         0x0000017Eu
#define TPM2_SU_CLEAR            0x0000u
#define TPM2_CAP_TPM_PROPERTIES  0x00000006u
#define TPM2_PT_MANUFACTURER     0x00000105u
#define TPM2_RC_SUCCESS          0x00000000u
#define TPM2_RC_INITIALIZE       0x00000100u
#define TPM2_RSP_HEADER_SIZE     10u

/* Hard cap on any single TPM2 response this transport will accept.
 * PTP-era TPMs use 4 KiB command/response buffers; anything larger in
 * a response header is treated as malformed (TPM_T_ERR_RESPONSE). */
#define TPM_T_MAX_RESPONSE 4096u

/* There is deliberately NO "worst-case transaction" constant here, and that
 * absence is a finding rather than an omission.
 *
 * One was added (2*TIMEOUT_B + TIMEOUT_A + 2*TIMEOUT_C) so a caller could size
 * a teardown reserve "safely above one normative transaction". It was wrong:
 * tis_submit restarts TIMEOUT_A on EVERY burst, for both the command bytes and
 * the response bytes, so with burstCount=1 a conforming device can legitimately
 * spend 2*2000 + 24*750 + 3*200 = 22600 ms on a 14-byte FlushContext. Any
 * fixed number is either below the spec (and aborts healthy hardware) or so far
 * above it that a budget derived from it cannot bound a boot at all.
 *
 * So callers must NOT try to derive a budget that a conforming TPM can never
 * exceed -- no such useful number exists. A cumulative budget is a POLICY
 * statement about how long this boot is willing to wait, and the design work is
 * in making expiry proportionate, not in making it impossible. */

/* ---- Init + state queries ---- */

/* Discover and bring up the TPM2 command transport. Degrades cleanly:
 * absent ACPI TPM2 table + dead TIS probe leaves the transport
 * unavailable and boot continues. Detects whether firmware already
 * sent TPM2_Startup and only issues Startup(CLEAR) when the TPM
 * reports TPM_RC_INITIALIZE (never perturbs firmware-owned state). */
void tpm_transport_init(void);

/* 1 when a TPM2 command transport is up and not sticky-failed. */
int tpm_transport_available(void);

/* TPM_T_IFACE_* discovered at init (NONE when unavailable). */
int tpm_transport_iface(void);

/* Submit one TPM2 command and receive its response.
 * cmd/cmd_len: fully marshaled command (header + body).
 * rsp/rsp_cap: response buffer; the response header's size field is
 * validated against BOTH rsp_cap and TPM_T_MAX_RESPONSE.
 * Returns the full response length (>= TPM2_RSP_HEADER_SIZE) on
 * success -- the caller still checks the response code via
 * tpm2_rsp_parse(). Negative TPM_T_ERR_* on transport failure. */
int tpm2_submit(const uint8_t *cmd, uint32_t cmd_len,
                uint8_t *rsp, uint32_t rsp_cap);

/* Like tpm2_submit, but if another transaction is in flight, wait up to
 * budget_ms for the transport to become free instead of returning BUSY
 * immediately. For the non-ISR CLEANUP path (e.g. FlushContext after a
 * multi-command session flow) where an immediate BUSY would abandon a resource
 * -- a leaked session handle erodes the TPM's small session pool until reboot.
 * Returns the response length / TPM rc on submit, or a negative TPM_T_ERR_*
 * (TPM_T_ERR_BUSY only when the gate never cleared within budget_ms). Never
 * call from ISR context. */
int tpm2_submit_waiting(const uint8_t *cmd, uint32_t cmd_len,
                        uint8_t *rsp, uint32_t rsp_cap, uint32_t budget_ms);

/* ---- Bounded multi-command sequences ----
 * A logical TPM operation is often several commands (the NV policy flow is
 * StartAuthSession + PolicyPCR + the NV op + FlushContext). Submitted one at a
 * time each carries its OWN per-command PTP timeouts, which reset on every FIFO
 * burst -- so against a slow-but-responsive TPM the operation has no bound at
 * all, while the whole-boot target is measured in seconds. A sequence holds the
 * transport gate once and caps the SUM of every wait inside it.
 *
 * Ownership is a TOKEN, not a CPU and not an ambient flag. A raw
 * begin/submit/end triple cannot express ownership safely: `end` after a FAILED
 * begin would release somebody else's sequence, a forgotten `end` wedges every
 * later command, and a CPU check both admits same-CPU reentrancy and rejects the
 * legitimate owner after a thread migration. So the only entry point is the
 * scoped runner below, which always releases, and every submit inside it
 * presents the generation token it was handed. */

/* Opaque bounded-sequence token. 0 is never a live sequence. */
typedef uint64_t tpm2_seq_t;

/* Body of one bounded sequence. Submit through tpm2_submit_seq(seq, ...); the
 * return value is passed straight back out of tpm2_seq_run(). */
typedef int (*tpm2_seq_fn)(tpm2_seq_t seq, void *ctx);

/* Run fn() as ONE bounded sequence holding the transport gate throughout.
 * The advertised total is work + cleanup + up to 2 * the PTP command timeout.
 * A budget expiry runs a bounded interface abort with the budget DISARMED (it
 * has to be -- the abort exists precisely because the budget is gone), and
 * that can happen once after the work budget and once after the cleanup
 * allowance. Recovery is not free, and the contract says so rather than
 * quietly excluding it; the same figure is derived at the quiesce in
 * tpm_transport.c.
 *
 * work_ms bounds the sequence's ordinary commands. cleanup_ms is a SEPARATE
 * allowance, spent only by tpm2_submit_seq_teardown(), so a mandatory teardown
 * (FlushContext) is still both possible and bounded AFTER the work budget
 * expires -- a single hard deadline would leave a started session unflushable,
 * leaking a handle out of the TPM's small session pool. Every teardown attempt
 * in one sequence DEBITS that one allowance rather than re-arming it, so the
 * total is work + cleanup regardless of how many retries a teardown needs.
 * Returns fn()'s value, or a negative TPM_T_ERR_* if the sequence never started
 * (TPM_T_ERR_BUSY when another sequence or transaction holds the transport).
 * Never call from ISR context; sequences do not nest. */
int tpm2_seq_run(uint32_t work_ms, uint32_t cleanup_ms,
                 tpm2_seq_fn fn, void *ctx);

/* Submit one command inside the live sequence named by `seq`. Returns the same
 * values as tpm2_submit, plus TPM_T_ERR_SEQ when `seq` is not the live token
 * (no sequence running, or a stale token from a finished one) and
 * TPM_T_ERR_BUDGET when the sequence's budget is already exhausted. */
int tpm2_submit_seq(tpm2_seq_t seq, const uint8_t *cmd, uint32_t cmd_len,
                    uint8_t *rsp, uint32_t rsp_cap);

/* Submit a mandatory TEARDOWN command (FlushContext) inside the sequence, on
 * the RESERVED allowance rather than on whatever the work phase left.
 *
 * Teardown gets its own budget instead of extending the shared one, because
 * extending leaked in a way that only shows up mid-operation: a flush partway
 * through a larger sequence topped the deadline up, and the ordinary commands
 * after it -- including an irreversible NV_DefineSpace -- then spent time
 * reserved for cleanup. The work budget is saved and restored around the
 * teardown. All teardowns in one sequence SHARE that single allowance -- each
 * attempt resumes it where the previous one left off rather than taking a
 * fresh reserve -- so the sequence total stays work + cleanup (plus up to two
 * bounded aborts), however many retries a teardown needs. */
int tpm2_submit_seq_teardown(tpm2_seq_t seq, const uint8_t *cmd, uint32_t cmd_len,
                             uint8_t *rsp, uint32_t rsp_cap);

/* ---- Pure marshaling helpers (unit-testable, no MMIO) ---- */

/* Big-endian field access (TPM2 wire format). */
static inline void tpm2_be16_put(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static inline void tpm2_be32_put(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static inline uint16_t tpm2_be16_get(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static inline uint32_t tpm2_be32_get(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/* Marshal TPM2_Startup(SU_CLEAR). Returns command length, 0 if cap too
 * small. */
uint32_t tpm2_build_startup(uint8_t *buf, uint32_t cap);

/* Marshal TPM2_GetCapability(TPM_PROPERTIES, PT_MANUFACTURER, 1).
 * Returns command length, 0 if cap too small. */
uint32_t tpm2_build_getcap_manufacturer(uint8_t *buf, uint32_t cap);

/* Marshal TPM2_GetRandom(bytesRequested). Returns command length, 0 if
 * cap too small or nbytes is 0. */
uint32_t tpm2_build_get_random(uint8_t *buf, uint32_t cap, uint16_t nbytes);

/* Parse a TPM2_GetRandom response: validates the header (tag/size/rc
 * via tpm2_rsp_parse, rc must be TPM2_RC_SUCCESS) and the TPM2B_DIGEST
 * payload bounds, then copies the random bytes to out. Returns the
 * byte count (> 0) or -1 on any malformed/failed response. */
int tpm2_parse_get_random(const uint8_t *rsp, uint32_t len,
                          uint8_t *out, uint32_t out_cap);

/* Marshal TPM2_PCR_Read for a SINGLE PCR (pcr_index 0..23) in the given hash
 * bank (alg = TPM_ALG_*). One PCR per call so the response TPML_DIGEST maps
 * 1:1. Returns the command length, 0 if cap too small or pcr_index > 23. */
uint32_t tpm2_build_pcr_read(uint8_t *buf, uint32_t cap, uint16_t alg,
                             uint32_t pcr_index);

/* Parse a TPM2_PCR_Read response for the SINGLE (alg, pcr_index) that was
 * requested, and bind the response to that request: the echoed pcrSelectionOut
 * must be exactly one selection naming `alg`, sizeofSelect 3, and the parser
 * keys off whether the requested PCR bit is set. If set, exactly one digest of
 * the bank's exact size (tpm_alg_digest_len_pub(alg)) is copied to out (bounded
 * by out_cap) and its length (> 0) returned. If clear, the bank is INACTIVE for
 * this PCR and 0 is returned (digest count must be 0). Any mismatch (wrong alg,
 * wrong/missing PCR bit, extra selections/digests, wrong digest size) or a
 * malformed/failed response returns -1 -- so a desynchronized TPM can never
 * have a foreign digest cached under our key. All reads are bounds-checked. */
int tpm2_parse_pcr_read(const uint8_t *rsp, uint32_t len,
                        uint16_t alg, uint32_t pcr_index,
                        uint8_t *out, uint32_t out_cap);

/* Collect up to `want` random bytes via repeated TPM2_GetRandom calls,
 * the WHOLE sequence capped by one cumulative wall-clock budget (the
 * per-command PTP timeouts reset per FIFO burst and would otherwise
 * accumulate against a slow device -- same rationale as the startup
 * probe budget). Returns bytes collected (>= 0; may be short on a
 * mid-sequence error) or a negative TPM_T_ERR_* when the FIRST call
 * fails. Boot-path callers pass a small budget_ms (e.g. 2000). */
int tpm2_get_random_bounded(uint8_t *out, uint32_t want,
                            uint32_t budget_ms);

/* Parse + validate a response header: tag, size, rc. Checks
 * len >= header, header size field >= header and <= len. Returns 0 on
 * success, -1 malformed. Out pointers may be NULL. */
int tpm2_rsp_parse(const uint8_t *rsp, uint32_t len,
                   uint16_t *out_tag, uint32_t *out_size, uint32_t *out_rc);

/* Locate the response PARAMETER area of a successful response (rc must be
 * TPM2_RC_SUCCESS). The wire layout differs by tag:
 *   ST_NO_SESSIONS: header(10) + parameters[...]                 (to size)
 *   ST_SESSIONS:    header(10) + parameterSize(4) + parameters[parameterSize]
 *                   + responseAuthArea[...]
 * A session-tagged response that is parsed at the fixed offset 10 (as the
 * non-session parsers do) would read the 4-byte parameterSize as the first
 * parameter bytes -- so every session-authorized command (NV read/write) MUST
 * locate its parameters through this helper, never at offset 10 directly.
 * Returns 0 and sets *out_off / *out_len to the parameter span (bounded inside
 * the header-declared size, with room left for the auth area on ST_SESSIONS);
 * -1 on a malformed/failed/oversize response. Out pointers may be NULL. */
int tpm2_rsp_params(const uint8_t *rsp, uint32_t len,
                    uint32_t *out_off, uint32_t *out_len);

/* ---- Test seam (kernel unit tests only) ----
 * The transport reads/writes interface registers through an io-ops
 * table so tests can substitute a fake register file (timeout, short
 * burst, oversized response, sticky-fail, reentrancy) without MMIO.
 * Offsets are relative to the mapped interface window. */
struct tpm_t_io {
    uint8_t  (*r8)(uint32_t off);
    void     (*w8)(uint32_t off, uint8_t v);
    uint32_t (*r32)(uint32_t off);
    void     (*w32)(uint32_t off, uint32_t v);
};

/* KERNEL_TESTS-gated block (release-flavor test-surface exclusion): every type and function
 * below is a test-only seam called exclusively from src/kernel/test/
 * test_tpm_*.c (already pruned entirely at KERNEL_TESTS=off); guarding the
 * declarations too keeps a release build from even seeing the prototypes. */
#ifdef KERNEL_TESTS

/* Full snapshot of the mutable transport routing state, so a test can restore
 * EVERY field it perturbed (not just the io pointer). A bare io-pointer restore
 * left iface/available/failed/fast at the test's values, which on a real-fTPM
 * host mis-routed the live transport after a TPM suite. */
struct tpm_t_test_state {
    const struct tpm_t_io *io;
    int iface;
    int available;
    int failed;
    int fast;
    int busy;
    int test_busy_ticks;
};

/* Install a fake io + interface kind; returns a FULL snapshot of the prior
 * transport state so the test can restore it via tpm_t_test_restore(). Also
 * marks the transport available and clears sticky failure. Passing io == NULL
 * sets full pre-init state (unavailable). Fast timeouts (fast != 0) shrink poll
 * deadlines to microsecond scale so timeout-path tests do not stall the suite. */
struct tpm_t_test_state tpm_t_test_install(const struct tpm_t_io *io,
                                           int iface, int fast);

/* Restore a snapshot captured by tpm_t_test_install(), putting back every
 * mutated field (io/iface/available/failed/fast/busy). Use this at test
 * teardown instead of re-installing with forced (NONE, 0) args. */
void tpm_t_test_restore(struct tpm_t_test_state st);

/* Opaque view of the live sequence budget (kernel unit tests only), so the
 * cleanup-reserve invariants are OBSERVABLE rather than asserted indirectly.
 * They could not be tested through behavior alone: proving "extend, do not
 * replace" needs a budget large enough that a replacement would still succeed,
 * which no timing-based probe can distinguish. tpm_t_test_budget_deadline()
 * returns the raw TSC deadline (0 when the budget runs in iteration mode), and
 * tpm_t_test_seq_cleanup_pending() reports whether the sequence's single
 * cleanup allowance has been armed yet by a first teardown. */
uint64_t tpm_t_test_budget_deadline(void);
int tpm_t_test_seq_cleanup_pending(void);

/* Arm the cumulative wait budget in iteration mode (kernel unit tests
 * only) -- mirrors the boot startup-probe budget so its expiry path is
 * testable. 0 disarms. */
void tpm_t_test_budget_iters(uint64_t iters);

/* 1 while a cumulative budget is armed (kernel unit tests only) --
 * lets tests prove a nested/racing caller did not clobber an active
 * sequence budget. */
int tpm_t_test_budget_active(void);

/* Make tpm2_submit_waiting() see the busy gate as held for the next N
 * acquire ticks before it can take it (kernel unit tests only) -- simulates a
 * transaction in flight on another CPU so the bounded-wait acquire path is
 * testable (BUSY-then-success) without real concurrency. 0 disarms. */
void tpm_t_test_busy_ticks(uint32_t n);

/* Snapshot of the CRB command/response buffer pointers + sizes, captured so a
 * test can RESTORE whatever real CRB mapping a prior live init established
 * (clearing them to NULL would corrupt a real CRB transport on a security-suite
 * boot that ran after a real TPM came up). */
struct tpm_t_crb_snapshot {
    volatile uint8_t *cmd;
    uint32_t cmd_size;
    volatile uint8_t *rsp;
    uint32_t rsp_size;
};

/* Point the CRB command/response buffers at test memory (kernel unit tests
 * only) so the CRB submit path can be exercised without MMIO -- the io-ops fake
 * drives the REG_REQ/REG_START handshake while these buffers stand in for the
 * mapped CRB command/response areas. Pair with tpm_t_test_install(io,
 * TPM_T_IFACE_CRB, fast). Returns the PRIOR buffer state; restore it at teardown
 * via tpm_t_test_restore_crb_buffers() rather than clearing to NULL. */
struct tpm_t_crb_snapshot tpm_t_test_install_crb_buffers(volatile uint8_t *cmd,
                                                         uint32_t cmd_size,
                                                         volatile uint8_t *rsp,
                                                         uint32_t rsp_size);

/* Restore CRB buffers captured by tpm_t_test_install_crb_buffers(). */
void tpm_t_test_restore_crb_buffers(struct tpm_t_crb_snapshot snap);

#endif /* KERNEL_TESTS */
