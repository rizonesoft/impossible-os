/* ============================================================================
 * tpm_replay.c -- measured-boot PCR replay primitives
 *
 * tpm_pcr_extend reproduces a TPM PCR_Extend: PCR := H_bank(PCR || digest).
 * Concatenates the current PCR value and the incoming measurement digest (each
 * one bank-digest long) and hashes with the bank's algorithm. Pure + no global
 * state; the event-log walk + hardware comparison build on this.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/tpm_replay.h"
#include "kernel/tpm.h"                 /* TPM_ALG_*, tpm_alg_digest_len_pub, tpm_event* */
#include "kernel/boot_info.h"           /* g_boot_info.tpm_event_log */
#include "kernel/crypto/sha1.h"
#include "kernel/crypto/sha256.h"
#include "kernel/crypto/sha384.h"
#include "libs/monocypher/monocypher-ed25519.h"  /* crypto_sha512 for the SHA-512 bank */
#include "libc/string.h"

/* Largest bank digest is SHA-512 (64 bytes); the concat input is 2x that. */
#define TPM_REPLAY_MAX_DIGEST 64u

/* TCG EV_NO_ACTION events (e.g. the leading TCG_EfiSpecIDEvent) are log metadata
 * and are NOT measured into any PCR -- replay must skip them. */
#define TPM_EV_NO_ACTION 0x00000003u

tpm_replay_status_t tpm_pcr_extend(uint16_t alg, uint8_t *pcr, uint32_t pcr_len,
                                   const uint8_t *digest, uint32_t digest_len)
{
    uint16_t dl = tpm_alg_digest_len_pub(alg);
    uint8_t input[TPM_REPLAY_MAX_DIGEST * 2u];

    if (!pcr || !digest || dl == 0u ||
        pcr_len != (uint32_t)dl || digest_len != (uint32_t)dl)
        return TPM_REPLAY_BADARG;

    /* input = PCR_old || incoming_digest. */
    memcpy(input, pcr, dl);
    memcpy(input + dl, digest, dl);

    switch (alg) {
        case TPM_ALG_SHA1:   sha1(input,   (uint32_t)dl * 2u, pcr); break;
        case TPM_ALG_SHA256: sha256(input, (uint32_t)dl * 2u, pcr); break;
        case TPM_ALG_SHA384: sha384(input, (uint32_t)dl * 2u, pcr); break;
        case TPM_ALG_SHA512:
            crypto_sha512(pcr, input, (size_t)dl * 2u);
            break;
        default:
            /* Unreachable: tpm_alg_digest_len_pub already rejected unknown algs
             * above (dl == 0). Kept so the switch is total. */
            return TPM_REPLAY_BADARG;
    }

    /* Wipe the concat buffer -- it held PCR + measurement material. */
    for (uint32_t i = 0; i < sizeof(input); i++)
        input[i] = 0u;
    return TPM_REPLAY_OK;
}

static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

uint32_t tpm_event_bank_digest(const uint8_t *log, uint32_t log_size,
                               const struct tpm_event *e, uint16_t alg,
                               const uint8_t **out_digest)
{
    uint16_t want = tpm_alg_digest_len_pub(alg);
    uint32_t pos, d;
    if (out_digest)
        *out_digest = 0;
    if (!log || !e || !out_digest || want == 0u)
        return 0;

    /* Legacy TCG_PCR_EVENT: a single RAW 20-byte SHA-1 digest sits at
     * digests_off with NO {alg} prefix, so it must NOT be parsed as a tagged
     * EVENT2 list. The only bank a legacy entry carries is SHA-1. */
    if (e->legacy) {
        if (alg != TPM_ALG_SHA1)
            return 0;
        if (e->digests_off > log_size || log_size - e->digests_off < 20u)
            return 0;
        *out_digest = log + e->digests_off;
        return 20u;
    }

    pos = e->digests_off;
    for (d = 0; d < e->digest_count; d++) {
        uint16_t a, dl;
        /* Bound the alg(2) field, then the digest, against the log -- the list
         * came from an attacker-influenceable firmware buffer. */
        if (pos > log_size || log_size - pos < 2u)
            return 0;
        a  = rd_le16(log + pos);
        dl = tpm_alg_digest_len_pub(a);
        if (dl == 0u)                       /* unknown alg -> cannot skip safely */
            return 0;
        if (log_size - pos < (uint32_t)(2u + dl))
            return 0;
        if (a == alg) {
            *out_digest = log + pos + 2u;
            return dl;
        }
        pos += 2u + (uint32_t)dl;
    }
    return 0;                               /* bank not present in this event */
}

tpm_replay_status_t tpm_replay_pcr_from(const uint8_t *log, uint32_t log_size,
                                        const struct tpm_event *events, uint32_t n_events,
                                        uint16_t alg, uint32_t pcr_index,
                                        uint8_t *out, uint32_t out_cap)
{
    uint16_t dl = tpm_alg_digest_len_pub(alg);
    uint32_t i;
    if (!out || dl == 0u || out_cap < (uint32_t)dl || pcr_index >= 24u)
        return TPM_REPLAY_BADARG;
    if (n_events && (!events || !log))
        return TPM_REPLAY_BADARG;

    memset(out, 0, dl);                     /* all-zero PCR reset value */
    for (i = 0; i < n_events; i++) {
        const struct tpm_event *e = &events[i];
        const uint8_t *digest = 0;
        if (e->pcr_index != pcr_index)
            continue;
        if (e->event_type == TPM_EV_NO_ACTION)   /* metadata; not extended into the PCR */
            continue;
        if (tpm_event_bank_digest(log, log_size, e, alg, &digest) == (uint32_t)dl && digest)
            tpm_pcr_extend(alg, out, (uint32_t)dl, digest, (uint32_t)dl);
        /* An event with no digest in this bank is simply not extended into it. */
    }
    return TPM_REPLAY_OK;
}

tpm_replay_status_t tpm_replay_pcr(uint16_t alg, uint32_t pcr_index,
                                   uint8_t *out, uint32_t out_cap)
{
    const uint8_t *log  = (const uint8_t *)g_boot_info.tpm_event_log;
    uint32_t log_size   = g_boot_info.tpm_event_log_size;
    uint32_t n          = tpm_event_count();
    const struct tpm_event *base = (n > 0u) ? tpm_event_get(0) : 0;

    /* tpm_event_count() is 0 unless the last parse was clean (s_events is then a
     * contiguous, authoritative array), so a degraded/absent log replays to the
     * all-zero PCR -- callers gate trust on tpm_evlog_status() separately. */
    if (base)
        return tpm_replay_pcr_from(log, log_size, base, n, alg, pcr_index, out, out_cap);
    {
        uint16_t dl = tpm_alg_digest_len_pub(alg);
        if (!out || dl == 0u || out_cap < (uint32_t)dl || pcr_index >= 24u)
            return TPM_REPLAY_BADARG;
        memset(out, 0, dl);
        return TPM_REPLAY_OK;
    }
}

void tpm_replay_report_pcr(struct tpm_replay_report *r, uint32_t pcr_index, int matched)
{
    if (!r)
        return;
    r->pcr_checked++;
    if (!matched) {
        r->mismatch_count++;
        if (r->first_mismatch_pcr < 0)
            r->first_mismatch_pcr = (int16_t)pcr_index;
        r->verdict = (uint8_t)TPM_REPLAY_TAMPER;
    }
}

void tpm_replay_finalize(struct tpm_replay_report *r, int log_clean, int complete)
{
    if (!r)
        return;
    if (!log_clean) {
        /* Missing/malformed log = untrusted evidence, NOT tamper. */
        r->verdict = (uint8_t)TPM_REPLAY_UNVERIFIABLE;
        return;
    }
    /* A confirmed mismatch (TAMPER) stays; an all-match result is only VERIFIED
     * when coverage was complete. */
    if (r->verdict == (uint8_t)TPM_REPLAY_VERIFIED && !complete)
        r->verdict = (uint8_t)TPM_REPLAY_UNVERIFIABLE;
}

tpm_replay_status_t tpm_replay_verify(uint16_t alg, struct tpm_replay_report *out)
{
    /* The firmware + Secure Boot + kernel-ABI measured-boot PCRs. */
    static const uint8_t measured[] = { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 11u };
    uint16_t dl = tpm_alg_digest_len_pub(alg);
    uint32_t readable = 0, i;
    int read_failure = 0;

    if (!out || dl == 0u)
        return TPM_REPLAY_BADARG;
    out->alg                = alg;
    out->verdict            = (uint8_t)TPM_REPLAY_VERIFIED;
    out->pcr_checked        = 0;
    out->mismatch_count     = 0;
    out->first_mismatch_pcr = -1;

    /* A degraded/absent log replays to all-zero; comparing that to hardware would
     * falsely read as tamper. Gate on a clean parse BEFORE touching the TPM. */
    if (tpm_evlog_status() != TPM_EVLOG_OK) {
        tpm_replay_finalize(out, 0 /*log_clean*/, 0);
        return TPM_REPLAY_OK;
    }

    for (i = 0; i < sizeof(measured); i++) {
        uint8_t hw[64], rp[64];
        uint32_t hwlen = 0;
        tpm_pcr_status_t st = tpm_pcr_get(measured[i], alg, hw, sizeof(hw), &hwlen);
        if (st == TPM_PCR_INACTIVE)
            continue;                       /* bank genuinely absent for this PCR */
        if (st != TPM_PCR_OK || hwlen != (uint32_t)dl) {
            read_failure = 1;               /* transport/busy/bad-length -> incomplete */
            continue;
        }
        readable++;
        if (tpm_replay_pcr(alg, measured[i], rp, sizeof(rp)) != TPM_REPLAY_OK) {
            read_failure = 1;
            continue;
        }
        tpm_replay_report_pcr(out, measured[i], memcmp(rp, hw, dl) == 0 ? 1 : 0);
    }
    /* VERIFIED only when every required PCR for an active bank was actually read
     * + replayed (no read failures and at least one PCR covered). */
    tpm_replay_finalize(out, 1 /*log_clean*/, (readable > 0u && !read_failure) ? 1 : 0);
    return TPM_REPLAY_OK;
}
