/* tcg_evlog.h -- strict, bounded walk of a TCG crypto-agile event log, and the
 *                identification of one boot application's own PCR measurement.
 *
 * WHY A SECOND WALKER EXISTS AT ALL. src/kernel/tpm.c already walks this log
 * for inventory: how many events, which banks, where each payload sits. That
 * parser is deliberately permissive about the log's own self-description -- it
 * declares `struct tcg_spec_id_event` (tpm.c:39) and never reads it, so it
 * never checks the "Spec ID Event03" signature, never requires the first record
 * to be EV_NO_ACTION, and sizes every EVENT2 digest from a LOCAL table instead
 * of the one the log advertises. For counting events that is harmless. For this
 * consumer it is not: a correlation that says AGREE is asserting that firmware
 * measured the very bytes we hashed, and reaching that verdict through a log we
 * only partly validated would upgrade corrupt or adversarial event data into
 * evidence about executed code. So this header validates first and walks
 * second, and it sizes digests from the log's OWN advertised bank table.
 *
 * This is intended to become the single walker, with tpm.c delegating to it;
 * that migration is tracked separately because it changes a shipped parser's
 * behaviour and carries its own regression surface. Until then, note what these
 * two are NOT: they are not each other's oracle. Two parsers agreeing on a
 * malformed log is not evidence the log is well-formed, so the tests behind
 * this header assert against independently-known expected values, and any
 * differential check against tpm.c is a consistency note, never the proof.
 *
 * WHAT VALIDATION HERE DOES NOT BUY. This header proves a log is INTERNALLY
 * well-formed: it describes its own length, advertises its banks, and every
 * record carries one digest per advertised bank. It does NOT prove the log is
 * TRUE. A TCG event log is only authenticated by replaying it into the PCRs and
 * comparing against the TPM's own values; nothing here does that. So a caller
 * that reaches a match has learned "the log claims firmware measured these
 * bytes", not "the TPM agrees it did", and any verdict built on it must be
 * worded to that strength. Binding this to a PCR replay is tracked separately.
 *
 * WHY A PURE HEADER. The log is firmware-owned and hostile by assumption; the
 * caller measures the buffer and passes the measure in, and everything here is
 * arithmetic over `log[0 .. log_size)`. That is what lets the kernel test suite
 * construct the malformed logs firmware will never hand a running loader.
 *
 * Reference: TCG PC Client Platform Firmware Profile v1.06 rev 52, section 10.2
 * (TCG_PCR_EVENT / TCG_PCR_EVENT2 / TPML_DIGEST_VALUES) and the
 * UEFI_IMAGE_LOAD_EVENT payload carried by the image-load event types. Field
 * widths follow EDK2's MdePkg/Include/IndustryStandard/UefiTcgPlatform.h, which
 * is the industry's transcription of those tables.
 *
 * Type discipline: plain C types, as in devpath_filepath.h and
 * pe_authenticode.h beside it, so the file compiles unchanged in both the
 * loader (UEFI types) and the kernel test suite (C99 stdint).
 */

#ifndef TCG_EVLOG_H
#define TCG_EVLOG_H

_Static_assert(sizeof(unsigned int) == 4,
               "tcg_evlog.h needs a 32-bit unsigned int");
_Static_assert(sizeof(unsigned long long) == 8,
               "tcg_evlog.h needs a 64-bit unsigned long long");

#define TCGL_FN static __attribute__((unused))

#define TCGL_EV_NO_ACTION                    0x00000003u
#define TCGL_EV_EFI_BOOT_SERVICES_APPLICATION 0x80000003u

#define TCGL_ALG_SHA1    0x0004u
#define TCGL_ALG_SHA256  0x000Bu
#define TCGL_ALG_SHA384  0x000Cu
#define TCGL_ALG_SHA512  0x000Du

#define TCGL_SHA256_LEN     32u
#define TCGL_LEGACY_HDR     32u   /* pcr(4) type(4) sha1(20) size(4) */
#define TCGL_SPECID_SIG_LEN 16u
#define TCGL_MAX_BANKS       8u

/* A cap on the walk, not a layout fact: the input is firmware-adjacent and the
 * caller is a boot loader, so an unbounded record count is an unbounded loop
 * over hostile data. A real platform log is far below this. */
#define TCGL_MAX_EVENTS   4096u

enum tcgl_status {
    TCGL_OK = 0,
    TCGL_ABSENT,          /* no log bytes at all -- NOT a defect, a weaker claim */
    TCGL_BAD_HEADER,      /* first record is not a well-formed TCG_PCR_EVENT */
    TCGL_NOT_SPECID,      /* first record is not the EV_NO_ACTION SpecID event */
    TCGL_BAD_SPECID,      /* SpecID payload malformed or advertises no banks */
    TCGL_BAD_SPECID_HDR,  /* SpecID record's own PCR index or digest field is wrong */
    TCGL_TRUNCATED,       /* a record runs past the end of the buffer */
    TCGL_UNKNOWN_ALG,     /* a record uses a bank the log never advertised */
    TCGL_TOO_MANY_EVENTS,
    TCGL_NO_SHA256_BANK,  /* the log advertises no SHA-256 bank at all */
    TCGL_INCOMPLETE_DIGESTS, /* a record omits a bank the log advertised */
    TCGL_DUPLICATE_ALG,   /* an algorithm declared or recorded twice */
    TCGL_NO_MATCH,        /* no image-load event for the requested device path */
    TCGL_AMBIGUOUS        /* more than one candidate -- never guess between them */
};

TCGL_FN const char *tcgl_status_name(int st)
{
    switch (st) {
    case TCGL_OK:              return "ok";
    case TCGL_ABSENT:          return "absent";
    case TCGL_BAD_HEADER:      return "bad-header";
    case TCGL_NOT_SPECID:      return "not-specid";
    case TCGL_BAD_SPECID:      return "bad-specid";
    case TCGL_BAD_SPECID_HDR:  return "bad-specid-header";
    case TCGL_TRUNCATED:       return "truncated";
    case TCGL_UNKNOWN_ALG:     return "unknown-alg";
    case TCGL_TOO_MANY_EVENTS: return "too-many-events";
    case TCGL_NO_SHA256_BANK:  return "no-sha256-bank";
    case TCGL_INCOMPLETE_DIGESTS: return "incomplete-digests";
    case TCGL_DUPLICATE_ALG:   return "duplicate-alg";
    case TCGL_NO_MATCH:        return "no-match";
    case TCGL_AMBIGUOUS:       return "ambiguous";
    default:                   return "unknown";
    }
}

TCGL_FN unsigned int tcgl_rd16(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

TCGL_FN unsigned int tcgl_rd32(const unsigned char *p)
{
    return (unsigned int)p[0]         | ((unsigned int)p[1] << 8)
         | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

TCGL_FN unsigned long long tcgl_rd64(const unsigned char *p)
{
    return (unsigned long long)tcgl_rd32(p)
         | ((unsigned long long)tcgl_rd32(p + 4) << 32);
}

struct tcgl_bank {
    unsigned int alg;    /* TCGL_ALG_* */
    unsigned int size;   /* digest length in bytes, as ADVERTISED by the log */
};

struct tcgl_log {
    const unsigned char *buf;
    unsigned long long   size;
    struct tcgl_bank     banks[TCGL_MAX_BANKS];
    unsigned int         nbanks;
    unsigned long long   first_event2;  /* offset just past the SpecID record */
};

/* What one matched image-load event says about the image it describes. */
struct tcgl_match {
    unsigned char      sha256[TCGL_SHA256_LEN];
    unsigned long long image_base;   /* ImageLocationInMemory; may be 0 legally */
    unsigned long long image_len;    /* ImageLengthInMemory */
    const unsigned char *devpath;
    unsigned long long   devpath_len;
};

/* Validate the log's self-description and record its advertised banks.
 *
 * The first record of a crypto-agile log is laid out in the LEGACY
 * TCG_PCR_EVENT shape and carries an EV_NO_ACTION event whose payload is the
 * TCG_EfiSpecIDEvent. Its signature is what positively identifies the log as
 * crypto-agile; without that check a TPM 1.2 log (EVENT records all the way
 * down) would be walked as though every record after the first were an EVENT2,
 * which reads plausible garbage rather than failing.
 */
TCGL_FN int tcgl_open(const unsigned char *log, unsigned long long log_size,
                      struct tcgl_log *out)
{
    static const char sig[TCGL_SPECID_SIG_LEN] = "Spec ID Event03";
    unsigned int i;

    if (!out)
        return TCGL_BAD_HEADER;
    out->buf = log;
    out->size = log_size;
    out->nbanks = 0;
    out->first_event2 = 0;

    /* An absent log is not a malformed one. The distinction is the whole point
     * of this status: a machine with no TPM has nothing to correlate, and the
     * honest result there is a weaker claim, never a mismatch. */
    if (!log || log_size == 0u)
        return TCGL_ABSENT;
    if (log_size < TCGL_LEGACY_HDR)
        return TCGL_BAD_HEADER;

    unsigned int ev_type = tcgl_rd32(log + 4u);
    unsigned int ev_size = tcgl_rd32(log + 28u);
    if ((log_size - TCGL_LEGACY_HDR) < (unsigned long long)ev_size)
        return TCGL_TRUNCATED;
    if (ev_type != TCGL_EV_NO_ACTION)
        return TCGL_NOT_SPECID;

    /* The SpecID record's OWN header fields are part of the contract, not
     * padding to step over. It is defined at PCR index 0 with an all-zero
     * 20-byte digest: an EV_NO_ACTION event is never extended, so a non-zero
     * digest there means the record was not produced by the rule this parser
     * is about to trust. Skipping these two made the internal-consistency
     * claim untrue -- a log violating either could still reach AGREE through
     * an otherwise well-formed record. */
    if (tcgl_rd32(log + 0u) != 0u)
        return TCGL_BAD_SPECID_HDR;
    for (i = 8u; i < 28u; i++) {
        if (log[i] != 0u)
            return TCGL_BAD_SPECID_HDR;
    }

    const unsigned char *sp = log + TCGL_LEGACY_HDR;
    /* signature(16) platformClass(4) minor(1) major(1) errata(1) uintnSize(1)
     * numberOfAlgorithms(4) = 28 bytes before the bank table. */
    if (ev_size < 28u)
        return TCGL_BAD_SPECID;
    for (i = 0; i < TCGL_SPECID_SIG_LEN; i++) {
        /* The literal is 15 chars plus its terminator, which is exactly the
         * 16-byte NUL-padded field the spec defines. */
        if (sp[i] != (unsigned char)sig[i])
            return TCGL_NOT_SPECID;
    }

    unsigned int nalg = tcgl_rd32(sp + 24u);
    if (nalg == 0u || nalg > TCGL_MAX_BANKS)
        return TCGL_BAD_SPECID;
    if ((unsigned long long)(ev_size - 28u) < (unsigned long long)nalg * 4u)
        return TCGL_BAD_SPECID;

    /* The payload must ACCOUNT FOR ITSELF exactly: 28 header bytes, the bank
     * table, a vendorInfoSize byte, and that many vendor bytes. Checking only
     * that the table FITS accepts a payload whose declared algorithm count is
     * smaller than the bytes actually present -- the reader and the writer then
     * disagree about where the table ends while every bounds check passes, and
     * a bank the log really declares becomes invisible. An event log that
     * cannot describe its own length is not one to derive executed-code
     * evidence from. */
    {
        unsigned int need = 28u + nalg * 4u;
        unsigned int vendor_len;
        if (ev_size < need + 1u)
            return TCGL_BAD_SPECID;
        vendor_len = sp[need];
        if (ev_size != need + 1u + vendor_len)
            return TCGL_BAD_SPECID;
    }

    for (i = 0; i < nalg; i++) {
        unsigned int alg = tcgl_rd16(sp + 28u + i * 4u);
        unsigned int dsz = tcgl_rd16(sp + 28u + i * 4u + 2u);
        /* A bank whose advertised size contradicts its algorithm makes every
         * later record's stride a guess, so it is refused rather than
         * normalised to the "real" length. */
        if ((alg == TCGL_ALG_SHA1   && dsz != 20u)
         || (alg == TCGL_ALG_SHA256 && dsz != 32u)
         || (alg == TCGL_ALG_SHA384 && dsz != 48u)
         || (alg == TCGL_ALG_SHA512 && dsz != 64u))
            return TCGL_BAD_SPECID;
        if (dsz == 0u || dsz > 64u)
            return TCGL_BAD_SPECID;
        /* A duplicate declaration makes "the SHA-256 bank" ambiguous and lets
         * a record satisfy a completeness check twice over with one digest. */
        {
            unsigned int k;
            for (k = 0; k < i; k++) {
                if (out->banks[k].alg == alg)
                    return TCGL_DUPLICATE_ALG;
            }
        }
        out->banks[i].alg  = alg;
        out->banks[i].size = dsz;
    }
    out->nbanks = nalg;
    out->first_event2 = (unsigned long long)TCGL_LEGACY_HDR + ev_size;
    return TCGL_OK;
}

/* Digest length for `alg` as THIS log advertised it. An algorithm the log never
 * declared has no defined stride here, and guessing one from a built-in table
 * is exactly how a walk keeps going through a log it has already lost. */
TCGL_FN int tcgl_bank_size(const struct tcgl_log *lg, unsigned int alg,
                           unsigned int *out_size)
{
    unsigned int i;
    for (i = 0; i < lg->nbanks; i++) {
        if (lg->banks[i].alg == alg) {
            *out_size = lg->banks[i].size;
            return 1;
        }
    }
    return 0;
}

/* Byte-compare two device paths. Identity is decided here and nowhere else:
 * the digest is compared only AFTER a single candidate has been selected, so a
 * second image with identical bytes loaded from a different path can never be
 * mistaken for this one. */
TCGL_FN int tcgl_dp_equal(const unsigned char *a, unsigned long long alen,
                          const unsigned char *b, unsigned long long blen)
{
    unsigned long long i;
    if (!a || !b || alen == 0u || alen != blen)
        return 0;
    for (i = 0; i < alen; i++) {
        if (a[i] != b[i])
            return 0;
    }
    return 1;
}

/* Walk every EVENT2 record and find the image-load event in `pcr` whose
 * UEFI_IMAGE_LOAD_EVENT device path equals `want_dp[0 .. want_dp_len)`.
 *
 * Requires EXACTLY ONE candidate. A machine that loaded several boot
 * applications is the normal case this guards: reporting the first match would
 * produce a confident wrong answer, so two matches is TCGL_AMBIGUOUS and the
 * caller must not report AGREE on it. `out_candidates` receives the count when
 * non-NULL, so the caller can say WHY it refused.
 */
TCGL_FN int tcgl_find_image_load(const struct tcgl_log *lg, unsigned int pcr,
                                 const unsigned char *want_dp,
                                 unsigned long long want_dp_len,
                                 struct tcgl_match *out,
                                 unsigned int *out_candidates)
{
    unsigned long long off;
    unsigned int seen = 0, found = 0, have_sha = 0;
    struct tcgl_match hit;
    unsigned int i;

    if (out_candidates)
        *out_candidates = 0;
    if (!lg || !lg->buf || lg->nbanks == 0u)
        return TCGL_BAD_HEADER;
    if (!want_dp || want_dp_len == 0u)
        return TCGL_NO_MATCH;

    for (i = 0; i < TCGL_SHA256_LEN; i++)
        hit.sha256[i] = 0u;
    hit.image_base = 0; hit.image_len = 0;
    hit.devpath = (const unsigned char *)0; hit.devpath_len = 0;

    for (off = lg->first_event2; off < lg->size; ) {
        if (++seen > TCGL_MAX_EVENTS)
            return TCGL_TOO_MANY_EVENTS;

        /* pcr(4) type(4) count(4) */
        if ((lg->size - off) < 12u)
            return TCGL_TRUNCATED;
        unsigned int rec_pcr  = tcgl_rd32(lg->buf + off);
        unsigned int rec_type = tcgl_rd32(lg->buf + off + 4u);
        unsigned int dcount   = tcgl_rd32(lg->buf + off + 8u);
        if (dcount > TCGL_MAX_BANKS)
            return TCGL_TRUNCATED;

        unsigned long long dpos = off + 12u;
        unsigned long long sha_off = 0;
        unsigned int rec_has_sha = 0;
        unsigned int seen_mask = 0;
        unsigned int d;
        for (d = 0; d < dcount; d++) {
            unsigned int dsz, bi;
            if ((lg->size - dpos) < 2u)
                return TCGL_TRUNCATED;
            unsigned int alg = tcgl_rd16(lg->buf + dpos);
            if (!tcgl_bank_size(lg, alg, &dsz))
                return TCGL_UNKNOWN_ALG;
            if ((lg->size - dpos) < (unsigned long long)(2u + dsz))
                return TCGL_TRUNCATED;
            /* Track WHICH advertised banks this record actually carries. A
             * digest list is not a free-form subset: a PCR extend applies to
             * every active bank, so a conforming record carries one digest per
             * advertised algorithm. Accepting a subset would let a record
             * present only the bank we happen to compare against, which is the
             * shape that turns a partial log into an AGREE. */
            for (bi = 0; bi < lg->nbanks; bi++) {
                if (lg->banks[bi].alg == alg) {
                    if (seen_mask & (1u << bi))
                        return TCGL_DUPLICATE_ALG;
                    seen_mask |= (1u << bi);
                    break;
                }
            }
            if (alg == TCGL_ALG_SHA256) {
                sha_off = dpos + 2u;
                rec_has_sha = 1;
            }
            dpos += 2u + dsz;
        }
        if (seen_mask != ((1u << lg->nbanks) - 1u))
            return TCGL_INCOMPLETE_DIGESTS;

        if ((lg->size - dpos) < 4u)
            return TCGL_TRUNCATED;
        unsigned int esize = tcgl_rd32(lg->buf + dpos);
        unsigned long long epos = dpos + 4u;
        if ((lg->size - epos) < (unsigned long long)esize)
            return TCGL_TRUNCATED;

        if (rec_type == TCGL_EV_EFI_BOOT_SERVICES_APPLICATION
            && rec_pcr == pcr) {
            /* UEFI_IMAGE_LOAD_EVENT: ImageLocationInMemory(8)
             * ImageLengthInMemory(8) ImageLinkTimeAddress(8)
             * LengthOfDevicePath(8) DevicePath[LengthOfDevicePath]. */
            if (esize >= 32u) {
                const unsigned char *pl = lg->buf + epos;
                unsigned long long dp_len = tcgl_rd64(pl + 24u);
                if (dp_len != 0u && dp_len <= (unsigned long long)esize - 32u
                    && tcgl_dp_equal(pl + 32u, dp_len, want_dp, want_dp_len)) {
                    found++;
                    if (found == 1u) {
                        hit.image_base  = tcgl_rd64(pl);
                        hit.image_len   = tcgl_rd64(pl + 8u);
                        hit.devpath     = pl + 32u;
                        hit.devpath_len = dp_len;
                        have_sha = rec_has_sha;
                        if (rec_has_sha) {
                            for (i = 0; i < TCGL_SHA256_LEN; i++)
                                hit.sha256[i] = lg->buf[sha_off + i];
                        }
                    }
                }
            }
        }

        off = epos + esize;
    }

    if (out_candidates)
        *out_candidates = found;
    if (found == 0u)
        return TCGL_NO_MATCH;
    if (found > 1u)
        return TCGL_AMBIGUOUS;
    /* The event exists and is unambiguous, but carries no SHA-256 bank. That is
     * a named outcome rather than a mismatch: there is a measurement, we simply
     * cannot compare it to a SHA-256 digest. */
    if (!have_sha)
        return TCGL_NO_SHA256_BANK;
    if (out)
        *out = hit;
    return TCGL_OK;
}

#endif /* TCG_EVLOG_H */
