/* ============================================================================
 * tpm_replay.h -- measured-boot PCR replay primitives
 *
 * The PCR-extend operation that the replay engine walks the event log with:
 * PCR_new = H_bank(PCR_old || measurement_digest), per TPM 2.0 PCR_Extend
 * semantics (TPM 2.0 Part 1). Pure, no global state, no TPM transaction -- so
 * it is unit-testable against fixed vectors and SMP-safe by construction.
 * Dispatches on the bank algorithm to the kernel SHA-1/256/384 transforms (and
 * monocypher SHA-512). The full event-log replay + hardware-PCR comparison
 * layers on top of this.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

typedef enum {
    TPM_REPLAY_OK      = 0,  /* extend applied, pcr updated in place */
    TPM_REPLAY_BADARG  = 1,  /* NULL / wrong length / unsupported bank alg */
} tpm_replay_status_t;

/* Extend one PCR in one hash bank: pcr := H_alg(pcr || digest), both `pcr` and
 * `digest` being exactly tpm_alg_digest_len_pub(alg) bytes. `pcr` is updated in
 * place. `alg` is a TPM_ALG_* (SHA-1/256/384/512). A fresh PCR for replay starts
 * at all-zero (the TPM reset value for these banks). */
tpm_replay_status_t tpm_pcr_extend(uint16_t alg, uint8_t *pcr, uint32_t pcr_len,
                                   const uint8_t *digest, uint32_t digest_len);

struct tpm_event;  /* defined in kernel/tpm.h */

/* Extract the digest for hash bank `alg` from one parsed event's
 * TPML_DIGEST_VALUES, iterating digest_count pairs from e->digests_off with each
 * {alg(2),digest} pair bounded against log_size. Returns the digest length (>0)
 * and sets *out_digest to point into `log`; returns 0 if the bank is absent from
 * the event or the list is malformed. Pure. */
uint32_t tpm_event_bank_digest(const uint8_t *log, uint32_t log_size,
                               const struct tpm_event *e, uint16_t alg,
                               const uint8_t **out_digest);

/* Replay one PCR for one hash bank from a parsed event set: start at the all-zero
 * reset value and extend with each matching event's bank digest in log order.
 * `events[0..n_events)` must come from tpm_evlog_parse over `log`. out[0..dl)
 * receives the replayed digest (dl = the bank digest length). Pure -- no globals,
 * no TPM transaction. Events without a digest in `alg` are skipped (only banks
 * actually present in an event are extended, per TPM semantics). */
tpm_replay_status_t tpm_replay_pcr_from(const uint8_t *log, uint32_t log_size,
                                        const struct tpm_event *events, uint32_t n_events,
                                        uint16_t alg, uint32_t pcr_index,
                                        uint8_t *out, uint32_t out_cap);

/* Replay one PCR from the LIVE parsed boot event log (boot_info + tpm_event_get).
 * Thin wrapper over tpm_replay_pcr_from. A degraded/absent/non-clean log replays
 * to the all-zero PCR (no events to extend); callers gate trust on
 * tpm_evlog_status() separately. */
tpm_replay_status_t tpm_replay_pcr(uint16_t alg, uint32_t pcr_index,
                                   uint8_t *out, uint32_t out_cap);

/* Replay-vs-hardware verdict. TAMPER means the event log replays to a value the
 * TPM's actual PCR does NOT hold -- the log was modified after the TPM was
 * extended. (A legitimate different boot still replays == hardware; that vs an
 * enrolled golden baseline is a BASELINE mismatch, owned by baseline enrollment,
 * not this engine.) */
typedef enum {
    TPM_REPLAY_VERIFIED     = 0,  /* every checked PCR: replay == hardware */
    TPM_REPLAY_TAMPER       = 1,  /* a replayed PCR != the hardware PCR */
    TPM_REPLAY_UNVERIFIABLE = 2,  /* no hardware PCR was readable (no TPM/transport) */
} tpm_replay_verdict_t;

struct tpm_replay_report {
    uint16_t alg;                /* hash bank verified */
    uint8_t  verdict;            /* tpm_replay_verdict_t */
    uint8_t  pcr_checked;        /* PCRs both replayed AND read from hardware */
    uint8_t  mismatch_count;     /* PCRs where replay != hardware */
    int16_t  first_mismatch_pcr; /* lowest mismatching PCR index, or -1 */
};

/* Pure report accumulator: fold one PCR's compare outcome (matched 1/0) into the
 * report, tracking the first mismatch + counts. Init the report to
 * {alg, TPM_REPLAY_VERIFIED, 0, 0, -1} before the first call. Testable without
 * live TPM reads. */
void tpm_replay_report_pcr(struct tpm_replay_report *r, uint32_t pcr_index, int matched);

/* Pure verdict finalizer. log_clean=0 (degraded/absent event log) -> UNVERIFIABLE
 * (untrusted evidence, NOT tamper -- a missing log must never read as tampering).
 * complete=0 (a required PCR could not be read / incomplete coverage) downgrades a
 * would-be VERIFIED to UNVERIFIABLE. A confirmed mismatch (TAMPER, set by
 * tpm_replay_report_pcr) is actionable and is preserved regardless of coverage. */
void tpm_replay_finalize(struct tpm_replay_report *r, int log_clean, int complete);

/* Live: replay + hardware-compare the measured-boot PCRs (0-7 + 11) for one bank,
 * building a tamper report (any replay != hardware -> TPM_REPLAY_TAMPER, with the
 * first mismatching PCR recorded). PCRs whose bank is inactive/unreadable are
 * skipped; UNVERIFIABLE if none were readable. Returns TPM_REPLAY_BADARG on bad
 * args. Runs after the Phase-1 transport (uses tpm_pcr_get). */
tpm_replay_status_t tpm_replay_verify(uint16_t alg, struct tpm_replay_report *out);
