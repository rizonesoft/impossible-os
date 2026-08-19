/* ============================================================================
 * tpm.h -- TPM Measured Boot interface
 *
 * Parses the TCG event log passed from the bootloader and exposes TPM
 * availability, version, and boot event summary to the kernel.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/boot_init.h"

/* Initialize TPM subsystem -- parse event log from boot_info.
 * Returns BOOT_OK on success, BOOT_DEGRADED if no TPM. */
boot_result_t tpm_init(void);

/* Returns 1 if a TPM was detected during boot. */
int tpm_available(void);

/* Returns TPM version: 0 = none, 1 = 1.2, 2 = 2.0. */
int tpm_version(void);

/* Returns the number of measured boot events in the event log. */
uint32_t tpm_event_count(void);

/* ---- TCG event-log parse result (measured-boot event-log hardening) ----
 *
 * The parser walks the bootloader-copied TCG log once, preserving per-event
 * metadata (PCR index, type, primary digest, payload span) and returning a
 * structured status. Truncation/corruption is reported with the exact failing
 * byte offset; on any non-OK status the metadata array is marked invalid and
 * event_count stays 0 (a malformed prefix never looks like a shorter valid
 * log). All multi-byte log fields are read with byte-load helpers, so a
 * misaligned firmware log cannot fault on strict-alignment cores.
 *
 * SMP: written once by tpm_init() (Phase-1 BSP, single-threaded) and read-only
 * thereafter, so the static metadata array needs no lock. */
typedef enum {
    TPM_EVLOG_OK           = 0,  /* clean parse to end-of-buffer */
    TPM_EVLOG_NO_LOG       = 1,  /* no log present / degraded by loader caps */
    TPM_EVLOG_BAD_HEADER   = 2,  /* first TCG_PCR_EVENT header malformed/short */
    TPM_EVLOG_BAD_SPEC_ID  = 3,  /* crypto-agile spec-ID event malformed */
    TPM_EVLOG_TRUNCATED    = 4,  /* an entry runs past the buffer end */
    TPM_EVLOG_CAP_EXCEEDED = 5,  /* more events than TPM_EVENT_MAX */
    TPM_EVLOG_UNSUPPORTED_ALG = 6, /* digest bank uses an unknown hash alg */
} tpm_evlog_status_t;

/* Fixed cap on preserved events. A static array (no attacker-sized alloc);
 * overflow is reported as TPM_EVLOG_CAP_EXCEEDED, not silently dropped. */
#define TPM_EVENT_MAX 256u

/* Per-event metadata. Digest/payload bytes are NOT copied -- the event log
 * buffer is retained by the legacy tpm_event_log boot_reserved reservation, so
 * offsets into it stay valid for later replay and CEL-JSON export. */
struct tpm_event {
    uint32_t pcr_index;
    uint32_t event_type;
    uint32_t digest_count;       /* number of digests in this event's bank list */
    uint16_t primary_alg_id;     /* TPM_ALG_* of the strongest digest present */
    uint8_t  primary_digest_len; /* 20/32/48/64 */
    uint8_t  legacy;             /* 1 = legacy TCG_PCR_EVENT (a single RAW 20-byte
                                  * SHA-1 digest at digests_off, no alg prefix);
                                  * 0 = TCG_PCR_EVENT2 ({alg,digest} list). Tells
                                  * replay how to read digests_off. */
    uint32_t primary_digest_off; /* byte offset of the primary digest in the log */
    uint32_t payload_off;        /* byte offset of event_data in the log */
    uint32_t payload_size;       /* event_data length */
    uint32_t digests_off;        /* byte offset of the FIRST {alg,digest} pair in the
                                  * log (TPML_DIGEST_VALUES list start for EVENT2; the
                                  * single SHA-1 digest for legacy). digest_count pairs
                                  * follow, enabling per-bank replay digest extraction. */
};

/* Pure, side-effect-free TCG event-log parser (no globals, no klog) so it is
 * unit-testable against fixture buffers. Fills out[0..out_max) with per-event
 * metadata; *out_count = events recorded, *out_overflow = 1 if the log held
 * more than out_max events, *out_fail_offset = byte offset of the first
 * rejection. Returns the structured status (non-OK => malformed; the caller
 * must not trust the partial prefix). Out params may be NULL. */
tpm_evlog_status_t tpm_evlog_parse(const uint8_t *log, uint32_t log_size, int version,
                                   struct tpm_event *out, uint32_t out_max,
                                   uint32_t *out_count, uint32_t *out_overflow,
                                   uint32_t *out_fail_offset);

/* Structured parse status + the exact offset of the first rejection (0 when
 * status == TPM_EVLOG_OK). tpm_evlog_fail_offset() is a test-only queryable
 * accessor (guarded out of release builds, release test-surface exclusion); the same offset
 * is available to any caller via tpm_evlog_parse()'s out_fail_offset out-param
 * and is logged on the production serial path. */
tpm_evlog_status_t tpm_evlog_status(void);
#ifdef KERNEL_TESTS
uint32_t           tpm_evlog_fail_offset(void);
#endif

/* Preserved event metadata. tpm_event_get() returns NULL for out-of-range i
 * or when the last parse failed. tpm_event_overflow() is 1 when the log held
 * more than TPM_EVENT_MAX events. */
const struct tpm_event *tpm_event_get(uint32_t i);
int                     tpm_event_overflow(void);

/* Export the parsed event log as a TCG CEL-JSON subset to
 * X:\Diag\tpm-events.json. The parse runs in Phase 0 (no filesystem); call
 * this from post-mount bring-up. No-op unless the last parse was clean. */
void tpm_evlog_export_cel(void);

/* ---- Boot Integrity Verification API ----
 *
 * WHAT A VERIFIED VERDICT ACTUALLY CLAIMS, stated first because the obvious
 * reading is wrong. This facility compares the ENROLLED BASELINE against the
 * live state: the measured-set PCR digests {0-7,11} in one bank, the Secure
 * Boot state and its readability, a firmware-version hash derived from SMBIOS,
 * and the kernel-ABI manifest digest. A match means THOSE agree with what was
 * enrolled on this machine.
 *
 * It does NOT mean the kernel IMAGE was measured or content-verified. Nothing
 * in the baseline hashes kernel.exe, and the loader's own digest covers the
 * ESP FILE rather than the bytes firmware executed. Every string this API
 * renders is named so a reader cannot upgrade the claim by accident:
 * "baseline-verified", never a bare "verified".
 *
 * On that scoped footing it supports:
 *   - Trusted Boot: detect a change in the enrolled measured state
 *   - BitLocker-style FDE: seal encryption keys to PCR state
 *   - Remote attestation: prove the measured state to remote parties
 *
 * Correlating an executed image with the firmware's own PCR 4
 * EV_EFI_BOOT_SERVICES_APPLICATION measurement is a separate, tracked
 * capability and is deliberately not claimed here.
 * ---- */

/* TCG hash algorithm IDs (TCG Algorithm Registry). Public API inputs for
 * tpm_pcr_get() + the event-log metadata. */
#define TPM_ALG_SHA1    0x0004u
#define TPM_ALG_SHA256  0x000Bu
#define TPM_ALG_SHA384  0x000Cu
#define TPM_ALG_SHA512  0x000Du

/* Digest length in bytes for a TCG algorithm id; 0 = unknown/unsupported. */
static inline uint16_t tpm_alg_digest_len_pub(uint16_t alg_id)
{
    switch (alg_id) {
        case TPM_ALG_SHA1:   return 20u;
        case TPM_ALG_SHA256: return 32u;
        case TPM_ALG_SHA384: return 48u;
        case TPM_ALG_SHA512: return 64u;
        default:             return 0u;
    }
}

/* Standard PCR indices for the boot chain */
#define TPM_PCR_FIRMWARE        0   /* Platform firmware code and data */
#define TPM_PCR_FIRMWARE_CONFIG 1   /* Host platform configuration (BIOS settings) */
#define TPM_PCR_OPTION_ROMS     2   /* Option ROM code */
#define TPM_PCR_OPTION_ROM_CFG  3   /* Option ROM configuration and data */
#define TPM_PCR_MBR             4   /* IPL code (bootloader / UEFI boot app) */
#define TPM_PCR_MBR_CONFIG      5   /* IPL configuration and data */
#define TPM_PCR_STATE_TRANS     6   /* State transition and wake events */
#define TPM_PCR_SECUREBOOT      7   /* Secure Boot policy (db/dbx/KEK/PK) */

/* Boot integrity verification status.
 *
 * MISMATCH IS AN UMBRELLA INTEGRITY-FAILURE STATUS, not "a PCR differs", and a
 * consumer reading the constant alone will get that wrong. Both comments used
 * to say "PCRs", which is false in both directions: VERIFIED also covers the
 * Secure Boot state, a firmware-version hash and the ABI-manifest digest, and
 * MISMATCH covers FOUR distinct failure-provenance classes, three of which
 * never compare an ENROLLED-BASELINE PCR value (the fourth does compare PCRs,
 * just against the replayed event log rather than against a baseline):
 *
 *   1. the live state disagreed with the enrolled baseline (the only publisher
 *      that has a differing FIELD, reported as a tpm_baseline_cause_t)
 *   2. the stored baseline failed its own integrity or authenticity checks
 *      (corrupt blob, torn or relabelled bind record, unbound, wrong index)
 *   3. THIS KERNEL's own read-only identity failed validation, before any
 *      baseline was read -- enrollment refuses this today exactly as it
 *      refuses 2, but it must never be routed WITH 2, because the safety here
 *      rests entirely on that validation running
 *   4. the event-log replay disagreed with the hardware PCRs, published by
 *      tpm_integrity_set_replay_verdict independently of any baseline; the
 *      label and scope functions report that one as event-log tamper
 *
 * So do not branch on the constant alone. tpm_integrity_status_label() and
 * tpm_integrity_status_scope() encode the precedence and the wording. What the
 * boot log adds differs by class and is NOT uniformly a remedy: classes 2 and 3
 * get a NAMED DIAGNOSIS from tpm_baseline_status_repair(), class 1 gets the
 * first differing field and every differing PCR, and class 4 gets the
 * mismatching PCR. What accompanies that diagnosis VARIES: some entries are
 * diagnosis-only on purpose, some name a required repair the tree cannot yet
 * perform, and one is immediately actionable. None names a step that would
 * deterministically fail. Read tpm_baseline_status_repair()'s contract for the
 * exhaustive split before building any UX on top of it. */
#define BOOT_INTEGRITY_UNKNOWN       0  /* Not yet checked */
#define BOOT_INTEGRITY_VERIFIED      1  /* the live state matches the enrolled baseline */
#define BOOT_INTEGRITY_MISMATCH      2  /* umbrella: any of the four failures above */
#define BOOT_INTEGRITY_NO_TPM        3  /* No TPM -- cannot verify */
#define BOOT_INTEGRITY_NO_BASELINE   4  /* No golden values enrolled */
#define BOOT_INTEGRITY_NO_CRYPTO     5  /* Crypto stack not available */

/* Per-PCR verification result */
struct pcr_check {
    uint8_t  pcr_index;     /* PCR register number */
    uint8_t  status;        /* BOOT_INTEGRITY_* constant */
    uint8_t  pad[2];
};

/* Slots in boot_integrity_report.pcrs[]. Sized for the MEASURED-BOOT set, which
 * is {0..7, 11} -- the report used to carry a literal 8 (PCR 0-7), so PCR 11
 * had no slot to be reported in at all. The canonical set is the BASELINE
 * policy mask in tpm_pcr_alloc.h and the indices are filled from
 * tpm_pcr_baseline_pcrs(), never from a second hard-coded list, so the reported
 * set cannot drift from the measured one. tpm.c carries the _Static_assert
 * tying this to TPM_BASELINE_MAX_PCRS (it is the one translation unit that sees
 * both headers; tpm.h deliberately does not include tpm_baseline.h). */
#define BOOT_INTEGRITY_MAX_PCRS 9u

/* Full boot integrity report -- feeds into the "Boot Integrity" UI panel.
 *
 * PUBLICATION CONTRACT: this struct is an IMMUTABLE SNAPSHOT. It is never
 * mutated in place once published; a writer builds the next value off-lock and
 * swaps it in (see tpm_integrity_report_copy). Readers obtain it ONLY by
 * copy-out -- there is deliberately no accessor handing out a pointer into the
 * live slot, because a naked pointer cannot be held safely across a swap. */
struct boot_integrity_report {
    uint8_t  overall_status;           /* BOOT_INTEGRITY_* */
    uint8_t  pcr_count;                /* number of PCRs checked */
    uint8_t  pad[2];
    struct pcr_check pcrs[BOOT_INTEGRITY_MAX_PCRS];   /* measured set {0-7,11} */
    uint32_t event_count;              /* total measured events */
    uint8_t  tpm_version;              /* 0=none, 1=1.2, 2=2.0 */
    uint8_t  secure_boot;              /* 1 ONLY if SB state readable AND active; never
                                        * collapses "unreadable" into "off" -- gate on
                                        * secure_boot_valid before trusting this bit */
    uint8_t  tpm_rng_available;        /* 1 once TPM2_GetRandom contributed entropy */
    uint8_t  secure_boot_valid;        /* 1 if the live SB state was readable; 0 = unknown
                                        * (so secure_boot==0 means "off" only when this is 1) */
    uint8_t  replay_verdict;           /* tpm_replay_verdict_t from the Phase-1 replay-vs-
                                        * hardware check (VERIFIED/TAMPER/UNVERIFIABLE);
                                        * event-log integrity, distinct from baseline */
};

/* Assemble the FIRST boot-integrity snapshot from already-sampled inputs. PURE
 * -- no UEFI calls, no TPM, no logging, no locking, no publication -- so the
 * construction rules are testable without calling tpm_integrity_init(), which
 * is boot infrastructure a unit test may not invoke.
 *
 *   tpm_present    0 = no TPM: status NO_TPM and NO PCR is reportable.
 *   version        tpm_version field verbatim (0 none, 1 = 1.2, 2 = 2.0).
 *   events         measured-event count.
 *   sb_valid       1 if the live Secure Boot state was READABLE.
 *   sb_enabled     1 if Secure Boot is active; only meaningful when sb_valid.
 *                  An unreadable state NEVER collapses to "off".
 *   set / n        the measured-boot PCR set and its size, from
 *                  tpm_pcr_baseline_pcrs(). `n` is that function's TOTAL match
 *                  count, which may EXCEED what it wrote into `set`.
 *
 * Returns 1 when the report describes the measured set, or 0 when `n` is not
 * exactly BOOT_INTEGRITY_MAX_PCRS -- drift in EITHER direction, which is a
 * build-configuration error. On 0 the report is still fully written, fail
 * CLOSED: pcr_count 0 and status UNKNOWN, so a report that cannot represent
 * the measured set never reads as verified. `out` must be non-NULL; `set` may
 * be NULL only when tpm_present is 0. */
int tpm_integrity_build_report(struct boot_integrity_report *out,
                               int tpm_present, uint8_t version,
                               uint32_t events, int sb_valid, int sb_enabled,
                               const uint8_t *set, uint8_t n);

/* Build and publish the FIRST boot-integrity snapshot (Phase 0, on the BSP,
 * after tpm_init()). It records what Phase 0 can know -- the event count, the
 * TPM version, the live Secure Boot state -- and marks the measured-boot PCR
 * set NO_CRYPTO, because the transport and PCR cache do not exist yet.
 *
 * The golden-value comparison is NOT done here and never was: reading the
 * baseline needs the Phase-1 NV transport, so tpm_baseline_verify() runs in
 * Phase 1 and publishes its verdict through tpm_integrity_publish_baseline(). */
boot_result_t tpm_integrity_init(void);

/* Returns 1 when the live state matches the ENROLLED BASELINE (measured-set
 * PCRs, Secure Boot state and readability, firmware-version hash, ABI-manifest
 * digest); 0 when it does not, or there is no TPM, or nothing is enrolled.
 *
 * NAMED for what it answers, on purpose. The old name was tpm_integrity_verified
 * and a programmatic consumer could reasonably read it as "this boot's code was
 * verified", which it has never meant: no kernel image is measured anywhere in
 * the baseline. A caller wanting the wider claim has nothing to call yet, and
 * that is the honest state. */
int tpm_integrity_baseline_verified(void);

/* One-line statement of what the report's status covers and what it does NOT,
 * for the boot log and any operator-facing surface that renders the label. A
 * label alone cannot carry scope, and the scope is the part a reader gets
 * wrong. Pure -- safe from any context; NULL yields the unknown-status text. */
const char *tpm_integrity_status_scope(const struct boot_integrity_report *r);

/* Bytes tpm_integrity_render_mismatched_pcrs() needs: the measured set tops out
 * at PCR 11, so two digits per index, one separator each, one terminator. */
#define BOOT_INTEGRITY_PCRLIST_MAX (BOOT_INTEGRITY_MAX_PCRS * 3u + 1u)

/* Render every MISMATCH slot in `r` as a comma-separated decimal index list
 * into `out` (capacity `cap`, always NUL-terminated when cap > 0), and return
 * how the result should be READ:
 *
 *    2  differing PCRs exist but the list is NOT PROVABLY COMPLETE -- it was
 *       truncated to what fit, or some slot was never compared. `out` holds
 *       what was found, which may be empty.
 *    1  every slot was compared and `out` names ALL the differing ones
 *    0  every slot was compared and NONE differ
 *   -1  there is nothing to speak for: no slots, or slots that were never
 *       compared and no differing one found among those that were
 *
 * 0 IS THE EXCULPATORY ANSWER AND IT HAS TO BE EARNED BY EVERY SLOT. Only
 * VERIFIED and MISMATCH mean a digest was compared; NO_BASELINE does NOT (the
 * golden pins nothing there, which tpm_baseline_compare_pcrs itself calls
 * UNVERIFIED rather than wrong), and neither does UNKNOWN, NO_CRYPTO, NO_TPM
 * or anything outside the enum. Two weaker rules were tried and both lied: a
 * count-based one called a Phase-0 report (full measured set, every slot
 * NO_CRYPTO) evaluated, and an any-slot one said "no PCR differs" about eight
 * UNKNOWN slots sitting beside one VERIFIED. Truncation is the same hazard
 * from the other side -- a partial list presented as whole, or a 0 returned
 * because even the first index did not fit. Pure, no allocation, no lock -- extracted
 * from the boot path precisely so the bound and the formatting are reachable
 * by a unit test. */
int tpm_integrity_render_mismatched_pcrs(const struct boot_integrity_report *r,
                                         char *out, uint32_t cap);

/* Copy the published boot-integrity snapshot into the caller's buffer. This is
 * the ONLY reader entry point, on purpose.
 *
 * The report is published by swapping in a fresh immutable snapshot, so a
 * pointer accessor would hand out a reference into a slot the next publication
 * may reuse -- and a reader paused between LOADING that pointer and USING it
 * would then read a buffer that is already being overwritten. A refcount does
 * not close that window either: the gap sits between the load and the pin.
 * Holding the publication lock across BOTH the load and the copy is what makes
 * ACQUISITION itself safe, which is why the copy is not optional.
 *
 * Safe to call at any time (a pre-init call yields a zeroed report, which reads
 * as BOOT_INTEGRITY_UNKNOWN). No-op on a NULL argument. Not for the panic path:
 * it takes a spinlock. */
void tpm_integrity_report_copy(struct boot_integrity_report *out);

/* One-word boot-diagnostics status for the integrity report, for serial log /
 * VPD / recovery UX: "no-TPM", "event-log-tamper" (replay != hardware -- checked
 * BEFORE baseline so it is never masked by a mismatch status), "baseline-
 * mismatch", "no-baseline", "no-crypto", "baseline-verified", "unknown".
 *
 * The success label is "baseline-verified" rather than a bare "verified", and
 * that is a correctness property rather than wording: it pairs with the
 * existing "baseline-mismatch" so both name the same subject, and it cannot be
 * read as a claim that the kernel image was measured. Pair it with
 * tpm_integrity_status_scope() wherever an operator reads it. Pure -- safe to
 * call from any context. */
const char *tpm_integrity_status_label(const struct boot_integrity_report *r);

/* ---- PCR Read API (measured-boot PCR access) ----
 *
 * tpm2_pcr_read() reads ONE PCR in one hash bank via TPM2_PCR_Read on the
 * Phase-1 transport; it always issues a transaction. tpm_pcr_get() is the
 * accessor for policy consumers: the measured-boot PCRs across active banks are
 * eagerly pre-cached by tpm_pcr_cache_init() (Phase 1, single-threaded), so a
 * read of one of those hits the lock-free cache with no transaction; any other
 * valid (pcr_index, alg) falls back to an uncached tpm2_pcr_read(). Both run
 * after tpm_transport_init() (Phase 1); a Phase-0 call returns TPM_PCR_TRANSPORT.
 * An undersized out_cap is TPM_PCR_BADARG on BOTH calls (never collapsed into
 * TPM_PCR_TRANSPORT). */
typedef enum {
    TPM_PCR_OK        = 0,  /* digest copied to out, *out_len set */
    TPM_PCR_INACTIVE  = 1,  /* bank not active for this PCR (out_len 0) */
    TPM_PCR_BADARG    = 2,  /* bad pcr_index/alg/buffer */
    TPM_PCR_TRANSPORT = 3,  /* TPM transport error / no TPM / malformed response */
    TPM_PCR_BUSY      = 4,  /* another TPM transaction in flight -- transient, retry */
} tpm_pcr_status_t;

/* Read a single PCR (index 0..23) in hash bank `alg` (TPM_ALG_*). On TPM_PCR_OK
 * the digest is copied to out (bounded by out_cap) and *out_len is the length.
 * Uncached -- issues a TPM transaction every call. Returns TPM_PCR_BUSY (not
 * TPM_PCR_TRANSPORT) when another transaction is in flight, so a contended
 * caller can retry rather than treat healthy contention as a TPM fault. */
tpm_pcr_status_t tpm2_pcr_read(uint16_t alg, uint32_t pcr_index,
                               uint8_t *out, uint32_t out_cap, uint32_t *out_len);

/* PCR accessor for policy consumers. A measured-boot PCR in an active bank
 * (pre-cached by tpm_pcr_cache_init()) returns the cached digest with no TPM
 * transaction; any other valid (index, alg) falls back to an uncached
 * tpm2_pcr_read(). Same status contract as tpm2_pcr_read(). The cache is
 * read-only after Phase-1 init -- the fallback never writes it -- so this needs
 * no lock. */
tpm_pcr_status_t tpm_pcr_get(uint32_t pcr_index, uint16_t alg,
                             uint8_t *out, uint32_t out_cap, uint32_t *out_len);

/* Eagerly populate the PCR cache for the measured-boot PCRs across active hash
 * banks. Call ONCE on the BSP in Phase 1 after tpm_transport_init() (single-
 * threaded, before APs/policy consumers run); the cache is read-only after, so
 * tpm_pcr_get() needs no lock. No-op when no TPM. */
void tpm_pcr_cache_init(void);

/* Record TPM RNG availability in the report. Called by the entropy
 * TPM collector AFTER tpm_integrity_init() (the transport and RNG
 * collection run in Phase 1; the report is built in Phase 0). */
void tpm_integrity_set_rng_available(int available);

/* Publish the Phase-1 PCR-replay-vs-hardware verdict (tpm_replay_verdict_t) into
 * the integrity report. A TAMPER verdict also escalates overall_status to
 * BOOT_INTEGRITY_MISMATCH (the event log replays to a value the TPM does not
 * hold). Called from the boot path after tpm_pcr_cache_init(). */
void tpm_integrity_set_replay_verdict(uint8_t verdict);

/* Publish the Phase-1 baseline-verify verdict together with the per-PCR detail
 * it produced, in ONE publication, so the overall verdict and the per-PCR
 * statuses can never disagree.
 *
 * `pcr_status` carries BOOT_INTEGRITY_* per entry, positionally matched to the
 * measured-boot set from tpm_pcr_baseline_pcrs(); `n` is how many entries it
 * holds (0 is legal and means "this path never compared", which leaves the
 * per-PCR detail at NOT-EVALUATED rather than stale).
 *
 * A prior replay TAMPER pins overall_status at MISMATCH -- event-log tamper
 * outranks a baseline match, so no later baseline verdict may downgrade it.
 * The per-PCR detail is still refreshed in that case: pinning the overall
 * verdict is not a reason to keep reporting stale per-PCR values.
 *
 * COHERENCE: a VERIFIED status is published ONLY when the report covers EXACTLY
 * the measured set (pcr_count == BOOT_INTEGRITY_MAX_PCRS) and every one of
 * those PCRs is itself VERIFIED. A partial, truncated, oversized, absent, or
 * non-verified detail set downgrades the published verdict to UNKNOWN rather
 * than asserting a match the report cannot back. Other verdicts pass through
 * unchanged -- they assert nothing that needs backing. */
void tpm_integrity_publish_baseline(uint8_t status,
                                    const uint8_t *pcr_status, uint8_t n);

#ifdef KERNEL_TESTS
/* Republish an exact snapshot (kernel unit tests only), so a test can restore
 * the boot's real verdict at teardown after exercising the publication path.
 * Same save/restore contract as tpm_t_test_install / tpm_t_test_restore:
 * capture with tpm_integrity_report_copy(), put back with this. Without it a
 * publication test would leave the machine reporting whatever verdict its last
 * fixture published, which the UI, VPD and serial log all read.
 *
 * KERNEL_TESTS-gated for the same reason the transport seams are: it publishes
 * an arbitrary whole snapshot, which BYPASSES the tamper pin, the PCR-count
 * validation and the base-load/mutate discipline every production writer goes
 * through. That is correct for a restore and wrong for anything else, so it
 * must not exist as callable surface in a release kernel. */
void tpm_integrity_test_republish(const struct boot_integrity_report *r);

/* Make tpm_pcr_get() bypass the Phase-1 PCR cache and resolve every read
 * through the transport; returns the PREVIOUS setting so a fixture can restore
 * it (same save/restore contract as tpm_t_test_install / tpm_t_test_restore).
 *
 * A test that installs a fake TIS needs this: tpm_pcr_cache_init() populates
 * the cache from the real platform at Phase 1, so on a machine that HAS a TPM
 * the entries are valid and hold live digests and a transport-level fake is
 * never consulted -- the test would pass on a TPM-less dev host and fail on
 * bare metal. Bypassing rather than injecting digests into the cache also keeps
 * the real tpm2_pcr_read response parser in the path.
 *
 * KERNEL_TESTS-gated: it converts an eagerly-populated lock-free read into a
 * per-call TPM transaction, which is correct for a fixture and wrong for the
 * boot path, so it must not exist as callable surface in a release kernel. */
int tpm_pcr_test_cache_bypass(int on);
#endif /* KERNEL_TESTS */

/* ---- Secure Boot Variable Measurement Reconciliation (STRUCTURAL, UNAUTHENTICATED) ----
 *
 * Decodes EV_EFI_VARIABLE_* events from the parsed TCG event log and reconciles
 * the MEASURED Secure Boot variable payloads against the LIVE UEFI variables
 * (UEFI runtime GetVariable), plus structural impossible-combination checks.
 * This is a STRUCTURAL diagnostic ONLY: with no in-kernel SHA yet (SHA-256/1/384
 * are blocked on the absent kernel hash primitives) it cannot prove the logged
 * digest equals SHA-256(payload) or that the event was extended into PCR7. It
 * therefore NEVER sets BOOT_INTEGRITY_VERIFIED and is reported as unauthenticated
 * until PCR replay validates the log against hardware PCRs.
 *
 * SMP: tpm_secureboot_reconcile() runs once on the BSP in Phase 1 (single-
 * threaded), then the report is read-only -- no lock (same model as s_events). */

/* TCG PC Client event types for Secure Boot variable measurements. */
#define EV_EFI_VARIABLE_DRIVER_CONFIG  0x80000001u  /* PK/KEK/db/dbx/SecureBoot config */
#define EV_EFI_VARIABLE_AUTHORITY      0x800000E0u  /* authority that loaded an image */

/* Tracked Secure Boot variables (index into sb_reconcile_report.vars[]). */
typedef enum {
    SB_VAR_PK = 0, SB_VAR_KEK, SB_VAR_DB, SB_VAR_DBX,
    SB_VAR_SECUREBOOT, SB_VAR_SETUPMODE, SB_VAR_COUNT
} sb_var_id_t;

/* Live-variable read outcome (tri-state; never collapse unknown into "off"). */
typedef enum {
    SB_LIVE_UNREAD    = 0,  /* not looked up */
    SB_LIVE_OK        = 1,  /* read succeeded */
    SB_LIVE_NOT_FOUND = 2,  /* variable absent */
    SB_LIVE_ERROR     = 3,  /* firmware read error / unreadable */
    SB_LIVE_PATHOLOGY = 4,  /* size pathology (too large to reconcile this pass) */
} sb_live_status_t;

/* Per-variable measured-vs-live reconciliation outcome. */
typedef enum {
    SB_MATCH_NA        = 0,  /* not measured and/or not readable -> no comparison */
    SB_MATCH_EQUAL     = 1,  /* measured payload bytes == live variable bytes */
    SB_MATCH_DIFFER    = 2,  /* both present but bytes differ */
    SB_MATCH_NOMEASURE = 3,  /* live present but no measurement event */
    SB_MATCH_NOLIVE    = 4,  /* measured present but live unreadable/absent */
} sb_match_t;

/* Impossible-combination flags (bitmask). ENABLED_NO_PCR7 is set ONLY after a
 * CLEAN event-log parse proves PCR7 policy events genuinely absent -- a
 * degraded/unavailable log is reported via evlog_status, never as a false
 * impossibility. */
#define SB_IMPOSSIBLE_NONE              0x00u
#define SB_IMPOSSIBLE_ENABLED_NO_PCR7   0x01u  /* SB active, clean log, no PCR7 events */
#define SB_IMPOSSIBLE_ENABLED_SETUPMODE 0x02u  /* SB enabled while SetupMode active */
#define SB_IMPOSSIBLE_ENABLED_NO_PK     0x04u  /* SB enabled but PK absent */
#define SB_IMPOSSIBLE_STATE_UNKNOWN     0x08u  /* live SB state unreadable */

#define SB_RECONCILE_VERSION 1u

struct sb_var_reconcile {
    uint8_t var_id;       /* sb_var_id_t */
    uint8_t measured;     /* 1 if an EV_EFI_VARIABLE_* event named this var */
    uint8_t live_status;  /* sb_live_status_t */
    uint8_t match;        /* sb_match_t */
};

/* Versioned, self-describing reconciliation report (kept SEPARATE from the
 * compact boot_integrity_report so UI/entropy consumers ignore it safely). */
struct sb_reconcile_report {
    uint16_t version;          /* SB_RECONCILE_VERSION */
    uint16_t size;             /* sizeof(struct sb_reconcile_report) */
    uint8_t  ran;              /* 1 if reconcile executed (TPM present) */
    uint8_t  unauthenticated;  /* ALWAYS 1: structural only until PCR replay lands */
    uint8_t  evlog_status;     /* tpm_evlog_status_t -- distinguishes clean vs degraded log */
    uint8_t  pcr7_event_count; /* EV_EFI_VARIABLE_* events observed on PCR7 (clean log only) */
    uint8_t  impossible_flags; /* SB_IMPOSSIBLE_* bitmask */
    uint8_t  sb_enabled;       /* live: 1 if Secure Boot active (meaningful iff sb_state_valid) */
    uint8_t  sb_state_valid;   /* live: 1 if SB state readable */
    uint8_t  setup_mode;       /* live: 1 if SetupMode active */
    struct sb_var_reconcile vars[SB_VAR_COUNT];
};

/* Pure, fixture-testable TCG UEFI_VARIABLE_DATA parser. Reads the 16-byte
 * VariableName GUID, the UCS-2 variable name (UnicodeNameLength in CHAR16
 * units), and locates the VariableData span. All reads bounds-checked against
 * `size`. Returns 0 on success, -1 on malformed. out_data_off/out_data_len
 * describe VariableData within payload. out_name receives up to name_cap UCS-2
 * code units; *out_name_chars is the full name length (may exceed name_cap). */
int uefi_var_data_parse(const uint8_t *payload, uint32_t size,
                        uint8_t out_guid[16],
                        uint16_t *out_name, uint32_t name_cap, uint32_t *out_name_chars,
                        uint32_t *out_data_off, uint32_t *out_data_len);

/* Pure impossible-combination classifier (testable without firmware). */
uint8_t sb_reconcile_classify(int log_clean, int pcr7_events, int sb_state_valid,
                              int sb_enabled, int setup_mode, int pk_present);

/* Run the structural Secure Boot reconciliation. Phase 1, BSP, AFTER
 * tpm_integrity_init() + uefi_secureboot_init(). Always records the live SB
 * state; walks measured events only when the TPM/log is present + clean. */
void tpm_secureboot_reconcile(void);

/* The reconciliation report (valid after tpm_secureboot_reconcile()). */
const struct sb_reconcile_report *tpm_sb_reconcile_report(void);
