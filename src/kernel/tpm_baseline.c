/* ============================================================================
 * tpm_baseline.c -- measured-boot baseline blob (enroll / verify / rotate)
 *
 * The CONTENT layer of measured-boot attestation (see tpm_baseline.h). Pure
 * format / validate / compare / generation logic + live snapshot / enroll /
 * verify wrappers that drive tpm_pcr_get + the owner-auth NV DATA index.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/tpm.h"
#include "kernel/tpm_baseline.h"
#include "kernel/tpm_nv.h"
#include "kernel/crypto/sha256.h"
#include "kernel/smbios.h"
#include "kernel/fs/gpt.h"   /* gpt_crc32 (IEEE CRC32) */
#include "libc/string.h"
#include "kernel/tpm_pcr_alloc.h"   /* tpm_pcr_baseline_pcrs (canonical measured set) */
#include "kernel/boot_proto_descriptor.h"   /* boot_proto_abi_digest (build-time ABI identity) */

/* The baseline stores the ABI-manifest digest verbatim, so the two lengths must
 * agree; a silent mismatch would truncate or overrun the field. */
_Static_assert(BOOT_PROTO_ABI_DIGEST_LEN == TPM_BASELINE_DIGEST,
    "ABI-manifest digest must be exactly one baseline digest wide");

/* ---- Pure core ---- */

uint32_t tpm_baseline_finalize(struct tpm_baseline *b)
{
    if (!b)
        return 0;
    b->magic = TPM_BASELINE_MAGIC;
    b->version = TPM_BASELINE_VERSION;
    b->size = (uint16_t)sizeof(*b);
    b->crc32 = gpt_crc32(b, (uint32_t)__builtin_offsetof(struct tpm_baseline, crc32));
    return (uint32_t)sizeof(*b);
}

int tpm_baseline_validate(const uint8_t *blob, uint32_t len,
                          struct tpm_baseline *out)
{
    uint32_t crc, i;
    uint32_t present = 0;
    if (!out || !blob || len < sizeof(struct tpm_baseline))
        return 0;
    /* Copy the untrusted byte buffer into the ALIGNED struct before reading any
     * field -- casting a uint8_t[] to struct* and reading uint32_t fields is UB
     * and faults on strict-alignment targets (ARM64). */
    memcpy(out, blob, sizeof(struct tpm_baseline));
    if (out->magic != TPM_BASELINE_MAGIC ||
        out->version != TPM_BASELINE_VERSION ||
        out->size != (uint16_t)sizeof(struct tpm_baseline))
        return 0;
    crc = gpt_crc32(out, (uint32_t)__builtin_offsetof(struct tpm_baseline, crc32));
    if (crc != out->crc32)
        return 0;
    /* Canonical structure (CRC is integrity, NOT authenticity -- the NV blob is
     * owner-writable). A baseline MUST pin the full measured PCR set in order;
     * a blob with pcr_count=0 or every slot present=0 would otherwise "verify"
     * without checking any PCR. */
    uint8_t pcrs[TPM_BASELINE_MAX_PCRS];
    uint8_t npcr = tpm_pcr_baseline_pcrs(pcrs, TPM_BASELINE_MAX_PCRS);
    if (npcr != TPM_BASELINE_MAX_PCRS || out->pcr_count != npcr)
        return 0;
    for (i = 0; i < npcr; i++) {
        if (out->pcrs[i].index != pcrs[i])
            return 0;
        /* A verifiable baseline must pin EVERY measured PCR. Accepting a partial
         * present-set would let a crafted/degraded baseline ignore changes in
         * the unpinned PCRs and still VERIFY (false completeness on {0-7,11}).
         * On a TPM2 platform all measured PCRs live in the active SHA-256 bank,
         * so a full present-set is the normal, enrollable case. */
        if (out->pcrs[i].present != 1u)
            return 0;
        present++;
    }
    if (present != npcr)
        return 0;
    if (out->secure_boot > 1u || out->secure_boot_valid > 1u ||
        out->fw_hash_present > 1u || out->abi_manifest_present > 1u)
        return 0;
    /* A claimed-present ABI manifest must carry an actual digest. The producer
     * (boot_proto_abi_digest via tpm_baseline_snapshot) can never emit
     * present=1 with an all-zero digest -- it treats all-zero as corruption and
     * fails the snapshot -- so admitting that shape here would canonicalize a
     * blob no honest enroll could have written. Compare would usually catch it
     * against a live descriptor, but validate must not accept a representation
     * the producer cannot create. */
    if (out->abi_manifest_present) {
        uint8_t nz = 0u;
        for (i = 0; i < TPM_BASELINE_DIGEST; i++)
            nz |= out->abi_manifest[i];
        if (nz == 0u)
            return 0;
    }
    return 1;
}

tpm_baseline_verdict_t tpm_baseline_compare(const struct tpm_baseline *golden,
                                            const struct tpm_baseline *current)
{
    uint32_t i, j;
    if (!golden || !current)
        return TPM_BASELINE_CMP_BADARG;
    /* Same hash bank, or the digests are not comparable. */
    if (golden->alg != current->alg)
        return TPM_BASELINE_MISMATCH;
    /* Secure Boot state (validity included: a baseline enrolled with SB readable
     * must not silently match a boot where SB became unreadable). */
    if (golden->secure_boot_valid != current->secure_boot_valid ||
        golden->secure_boot != current->secure_boot)
        return TPM_BASELINE_MISMATCH;
    /* Firmware-version hash (when the golden carries one). */
    if (golden->fw_hash_present) {
        if (!current->fw_hash_present ||
            memcmp(golden->fw_hash, current->fw_hash, TPM_BASELINE_DIGEST) != 0)
            return TPM_BASELINE_MISMATCH;
    }
    /* Kernel-ABI manifest identity -- SYMMETRIC, unlike the fw-hash gate above,
     * and the difference is deliberate. The firmware hash comes from SMBIOS,
     * which a platform may genuinely not provide, so a golden enrolled without
     * one is a legitimate state and gating on the golden alone is correct
     * forward compatibility. The ABI manifest is a compile-time constant that
     * every correctly built kernel carries (boot_proto_abi_digest fails the
     * whole snapshot rather than reporting it absent), so `current` always has
     * it. A golden WITHOUT it can therefore only be a pre-binding baseline, and
     * a one-way gate would let that baseline keep reporting VERIFIED forever
     * while attesting nothing about the ABI. Either presence disagreement is a
     * mismatch; clearing it takes a re-enroll, which is the honest signal. */
    if (golden->abi_manifest_present != current->abi_manifest_present)
        return TPM_BASELINE_MISMATCH;
    if (golden->abi_manifest_present &&
        memcmp(golden->abi_manifest, current->abi_manifest, TPM_BASELINE_DIGEST) != 0)
        return TPM_BASELINE_MISMATCH;
    if (golden->pcr_count > TPM_BASELINE_MAX_PCRS ||
        current->pcr_count > TPM_BASELINE_MAX_PCRS)
        return TPM_BASELINE_CMP_BADARG;
    /* A golden baseline that pins NO PCR cannot be a match -- it would otherwise
     * "verify" the boot without checking any measured value. */
    {
        uint32_t k, gp = 0;
        for (k = 0; k < golden->pcr_count; k++)
            if (golden->pcrs[k].present)
                gp++;
        if (gp == 0u)
            return TPM_BASELINE_MISMATCH;
    }
    /* Every golden PCR present at enroll must equal the current digest for that
     * index in the same bank. A golden PCR that is no longer present/readable is
     * a mismatch (the measured state changed). */
    for (i = 0; i < golden->pcr_count; i++) {
        const struct tpm_baseline_pcr *g = &golden->pcrs[i];
        int found = 0;
        if (!g->present)
            continue;
        for (j = 0; j < current->pcr_count; j++) {
            const struct tpm_baseline_pcr *c = &current->pcrs[j];
            if (c->index == g->index && c->present) {
                if (memcmp(g->digest, c->digest, TPM_BASELINE_DIGEST) != 0)
                    return TPM_BASELINE_MISMATCH;
                found = 1;
                break;
            }
        }
        if (!found)
            return TPM_BASELINE_MISMATCH;
    }
    return TPM_BASELINE_MATCH;
}

uint8_t tpm_baseline_compare_pcrs(const struct tpm_baseline *golden,
                                  const struct tpm_baseline *current,
                                  uint8_t *out_status, uint8_t cap)
{
    uint8_t i, n;
    int bank_ok;

    if (!golden || !current || !out_status)
        return 0;
    if (golden->pcr_count > TPM_BASELINE_MAX_PCRS ||
        current->pcr_count > TPM_BASELINE_MAX_PCRS)
        return 0;

    n = golden->pcr_count;
    if (n > cap)
        n = cap;

    /* Digests from different banks are not comparable at all, so a bank
     * disagreement is a per-PCR mismatch for every slot rather than something
     * that could leave individual PCRs looking verified. */
    bank_ok = (golden->alg == current->alg);

    for (i = 0; i < n; i++) {
        const struct tpm_baseline_pcr *g = &golden->pcrs[i];
        uint8_t st = BOOT_INTEGRITY_MISMATCH;
        uint8_t j;

        if (!g->present) {
            /* The golden pins nothing here, so this boot's value is UNVERIFIED,
             * not wrong. Reporting it as a mismatch would invent a culprit. */
            out_status[i] = BOOT_INTEGRITY_NO_BASELINE;
            continue;
        }
        if (bank_ok) {
            for (j = 0; j < current->pcr_count; j++) {
                const struct tpm_baseline_pcr *c = &current->pcrs[j];
                if (c->index == g->index && c->present) {
                    st = (memcmp(g->digest, c->digest, TPM_BASELINE_DIGEST) == 0)
                             ? (uint8_t)BOOT_INTEGRITY_VERIFIED
                             : (uint8_t)BOOT_INTEGRITY_MISMATCH;
                    break;
                }
            }
            /* Falling out of the loop without a match leaves st MISMATCH: the
             * golden pinned this PCR and the current boot cannot produce it. */
        }
        out_status[i] = st;
    }
    return n;
}

int tpm_baseline_rotation_ok(uint32_t old_gen, uint32_t new_gen)
{
    return (new_gen > old_gen) ? 1 : 0;
}

/* ---- Live wrappers ---- */

tpm_baseline_status_t tpm_baseline_snapshot(uint16_t alg, struct tpm_baseline *out)
{
    struct boot_integrity_report rep;
    const struct smbios_system_info *si;
    uint32_t i, present = 0;

    if (!out || tpm_alg_digest_len_pub(alg) != TPM_BASELINE_DIGEST)
        return TPM_BASELINE_BADARG;
    memset(out, 0, sizeof(*out));
    out->alg = alg;

    /* Kernel-ABI manifest identity FIRST, before anything that can fail for an
     * environmental reason. Ordering is load-bearing, not cosmetic: this is a
     * check on THIS kernel's own read-only data, so it is true or false
     * independently of whether a TPM answers or a baseline exists. Running it
     * after the PCR reads (where it originally sat) let an unreadable PCR
     * return NO_TPM first and mask the corruption the fail-closed contract
     * exists to expose -- and NO_TPM is the COMMON state on a machine with no
     * TPM, so the masking was the normal case rather than a corner. */
    if (!boot_proto_abi_digest(out->abi_manifest))
        return TPM_BASELINE_SELF_CORRUPT;
    out->abi_manifest_present = 1;

    uint8_t pcrs[TPM_BASELINE_MAX_PCRS];
    uint8_t npcr = tpm_pcr_baseline_pcrs(pcrs, TPM_BASELINE_MAX_PCRS);
    if (npcr != TPM_BASELINE_MAX_PCRS)   /* blob is sized for exactly the measured set */
        return TPM_BASELINE_BADARG;
    for (i = 0; i < npcr; i++) {
        uint32_t dl = 0;
        tpm_pcr_status_t st = tpm_pcr_get(pcrs[i], alg,
                                          out->pcrs[i].digest, TPM_BASELINE_DIGEST, &dl);
        out->pcrs[i].index = pcrs[i];
        if (st == TPM_PCR_OK && dl == TPM_BASELINE_DIGEST) {
            out->pcrs[i].present = 1;
            present++;
        } else {
            out->pcrs[i].present = 0;
            memset(out->pcrs[i].digest, 0, TPM_BASELINE_DIGEST);
        }
    }
    out->pcr_count = npcr;
    /* A baseline must pin the FULL measured set; a partial snapshot (some PCR
     * unreadable/inactive) cannot form a verifiable baseline -- report it as
     * NO_TPM (degraded) rather than enrolling/comparing a partial set. */
    if (present != npcr)
        return TPM_BASELINE_NO_TPM;

    /* Secure Boot state from the honest Secure Boot reconciliation in the boot
     * integrity report. Copied out, so the two fields come from one snapshot
     * and cannot straddle a publication -- and the copy is taken here, never
     * with a publication lock held, because this runs inside the verify path
     * whose caller publishes only after it returns. */
    tpm_integrity_report_copy(&rep);
    out->secure_boot = rep.secure_boot;
    out->secure_boot_valid = rep.secure_boot_valid;

    /* Firmware-version hash: SHA-256 over the SMBIOS BIOS version string. */
    si = smbios_get_info();
    if (si) {
        uint32_t n = 0;
        while (n < (uint32_t)sizeof(si->bios_version) && si->bios_version[n] != '\0')
            n++;
        if (n != 0u) {
            sha256(si->bios_version, n, out->fw_hash);
            out->fw_hash_present = 1;
        }
    }

    return TPM_BASELINE_OK;
}

/* Map a tpm_nv_status_t to a tpm_baseline_status_t for the NV-driven wrappers. */
static tpm_baseline_status_t nv_to_baseline(tpm_nv_status_t st)
{
    switch (st) {
        case TPM_NV_OK:        return TPM_BASELINE_OK;
        case TPM_NV_NOTFOUND:
        case TPM_NV_UNINIT:    return TPM_BASELINE_NO_BASELINE;
        case TPM_NV_TRANSPORT: return TPM_BASELINE_NO_TPM;
        /* A cumulative-budget expiry belongs with NO_TPM, not TPMERR. The two
         * are handled oppositely downstream: NO_TPM means "this machine could
         * not measure at all", which deliberately leaves the integrity verdict
         * UNPUBLISHED rather than reporting a false tamper, and that is exactly
         * right for a TPM that answered too slowly for the boot's patience.
         * TPMERR would instead read as a device fault. */
        case TPM_NV_BUDGET:    return TPM_BASELINE_NO_TPM;
        /* Enumerated rather than left to default: both are genuine hard
         * failures (an illegal attribute request, or an index whose public area
         * is not the one we asked for), and naming them stops a future status
         * from inheriting this bucket by accident. */
        case TPM_NV_ATTRS:
        case TPM_NV_MISMATCH:  return TPM_BASELINE_TPMERR;
        default:               return TPM_BASELINE_TPMERR;
    }
}

tpm_baseline_status_t tpm_baseline_enroll(uint32_t nv_index, uint16_t alg)
{
    struct tpm_baseline b;
    uint8_t old_blob[sizeof(struct tpm_baseline)];
    struct tpm_baseline old;
    uint16_t got = 0;
    uint32_t gen = 1u;
    tpm_baseline_status_t st;
    tpm_nv_status_t nv;

    st = tpm_baseline_snapshot(alg, &b);
    if (st != TPM_BASELINE_OK)
        return st;

    /* Monotonic generation, FAIL CLOSED. A valid existing baseline rotates to
     * gen+1; ONLY a genuine first enroll (index NOTFOUND / never written) starts
     * at 1. A short/corrupt existing blob or any read/transport failure must NOT
     * fall through to gen=1 -- that would roll a high-generation baseline back
     * over a transient read error (content-layer anti-rollback; NV write-lock/
     * counter hardening is a tracked follow-up). */
    {
        tpm_nv_status_t rd = tpm_nv_read(nv_index, 0u, old_blob,
                                         (uint16_t)sizeof(old_blob), &got);
        if (rd == TPM_NV_OK && got == (uint16_t)sizeof(old_blob) &&
            tpm_baseline_validate(old_blob, got, &old)) {
            if (old.generation == 0xFFFFFFFFu)
                return TPM_BASELINE_TPMERR;   /* no backward wrap */
            gen = old.generation + 1u;
            if (!tpm_baseline_rotation_ok(old.generation, gen))
                return TPM_BASELINE_TPMERR;
        } else if (rd == TPM_NV_NOTFOUND || rd == TPM_NV_UNINIT) {
            gen = 1u;   /* genuine first enroll */
        } else {
            /* Index exists but is unreadable/corrupt, or a transport error. */
            return (rd == TPM_NV_OK) ? TPM_BASELINE_CORRUPT : nv_to_baseline(rd);
        }
    }
    b.generation = gen;
    if (tpm_baseline_finalize(&b) == 0u)
        return TPM_BASELINE_BADARG;

    /* Define the owner-auth DATA index (idempotent: an already-defined index
     * returns DEFINED, fine for re-enroll / rotation), then write the blob. */
    nv = tpm_nv_define_data(nv_index, (uint16_t)sizeof(b));
    if (nv != TPM_NV_OK && nv != TPM_NV_DEFINED)
        return nv_to_baseline(nv);
    nv = tpm_nv_write(nv_index, 0u, (const uint8_t *)&b, (uint16_t)sizeof(b));
    return nv_to_baseline(nv);
}

tpm_baseline_status_t tpm_baseline_verify(uint32_t nv_index, uint16_t alg,
                                          uint8_t *out_overall,
                                          uint8_t *out_pcr_status,
                                          uint8_t pcr_cap,
                                          uint8_t *out_pcr_n)
{
    uint8_t blob[sizeof(struct tpm_baseline)];
    struct tpm_baseline current;
    struct tpm_baseline golden;
    uint16_t got = 0;
    tpm_nv_status_t nv;
    tpm_baseline_status_t st;
    tpm_baseline_verdict_t v;

    /* NOT EVALUATED until a comparison actually runs. Set once, up front, so
     * every early return below leaves the caller with an honest count rather
     * than whatever happened to be in its buffer -- a stale or uninitialized
     * per-PCR array published beside a MISMATCH is the self-contradiction this
     * out-param exists to prevent. */
    if (out_pcr_n)
        *out_pcr_n = 0;

    /* THIS KERNEL's identity is checked BEFORE the NV lookup, for the same
     * reason snapshot checks it before the PCR reads: it does not depend on
     * the TPM or on a baseline existing. Behind the NV read, an absent index
     * returns NO_BASELINE first and the corruption is never reported -- and
     * "no baseline" is the normal state on a machine that has never enrolled,
     * so the masking covered the common case. Publishing MISMATCH here is
     * fail-closed: a kernel whose own read-only identity is corrupt must not
     * report anything better, whatever the NV index holds. */
    {
        uint8_t self[BOOT_PROTO_ABI_DIGEST_LEN];
        if (!boot_proto_abi_digest(self)) {
            if (out_overall)
                *out_overall = BOOT_INTEGRITY_MISMATCH;
            return TPM_BASELINE_SELF_CORRUPT;
        }
    }

    /* A detail buffer that cannot hold the whole measured set is refused before
     * any TPM work, but AFTER the self-identity check above -- the ordering is
     * load-bearing and the opposite order was a real regression. SELF_CORRUPT
     * is fail-closed and publishes MISMATCH, while the boot caller deliberately
     * does not publish a BADARG; checking capacity first would let a corrupt
     * kernel descriptor arriving with an undersized buffer return BADARG and
     * leave the corruption entirely unreported.
     *
     * Clamping instead of refusing would hand back VERIFIED beside a truncated
     * detail array whose tail then publishes as UNKNOWN -- the
     * overall-vs-per-PCR contradiction the detail exists to remove, moved one
     * layer out. The bound is knowable before the read because
     * tpm_baseline_validate only accepts a golden pinning the FULL measured set
     * in canonical order, so a validated golden always has pcr_count ==
     * TPM_BASELINE_MAX_PCRS. Declining the detail entirely (NULL) stays legal. */
    if (out_pcr_status && pcr_cap < (uint8_t)TPM_BASELINE_MAX_PCRS)
        return TPM_BASELINE_BADARG;

    nv = tpm_nv_read(nv_index, 0u, blob, (uint16_t)sizeof(blob), &got);
    if (nv != TPM_NV_OK) {
        st = nv_to_baseline(nv);
        if (st == TPM_BASELINE_NO_BASELINE && out_overall)
            *out_overall = BOOT_INTEGRITY_NO_BASELINE;
        return st;
    }
    if (got != (uint16_t)sizeof(blob) ||
        !tpm_baseline_validate(blob, got, &golden)) {
        /* A defined-but-corrupt baseline is not a silent pass: report it as a
         * mismatch (the stored golden state cannot be trusted). */
        if (out_overall)
            *out_overall = BOOT_INTEGRITY_MISMATCH;
        return TPM_BASELINE_CORRUPT;
    }

    st = tpm_baseline_snapshot(alg, &current);
    if (st != TPM_BASELINE_OK) {
        /* SELF_CORRUPT is a hard integrity failure and must be published as
         * one -- returning without touching *out_overall would leave the
         * previously computed status standing and read as a clean boot. The
         * pre-NV check above normally catches it first; this stays as the
         * belt-and-braces path for any future snapshot failure of the same
         * class. Deliberately narrow: NO_TPM/BADARG mean the current state
         * could not be measured at all, and a machine that cannot measure is
         * not a machine that failed to match, so those keep the existing
         * leave-unpublished behavior rather than reporting false tamper. */
        if (st == TPM_BASELINE_SELF_CORRUPT && out_overall)
            *out_overall = BOOT_INTEGRITY_MISMATCH;
        return st;
    }

    v = tpm_baseline_compare(&golden, &current);
    if (out_overall)
        *out_overall = (v == TPM_BASELINE_MATCH)
                           ? BOOT_INTEGRITY_VERIFIED
                           : BOOT_INTEGRITY_MISMATCH;
    /* Per-PCR detail runs on BOTH outcomes, including a MISMATCH that a scalar
     * field caused. tpm_baseline_compare stops at the first difference, so it
     * cannot supply this; compare_pcrs walks the PCRs regardless, which is what
     * lets the report say "the overall verdict is MISMATCH and every PCR
     * verified" -- the honest reading of a Secure-Boot-state or ABI-manifest
     * change, and the one that stops it looking like PCR tampering. */
    if (out_pcr_status && out_pcr_n)
        *out_pcr_n = tpm_baseline_compare_pcrs(&golden, &current,
                                               out_pcr_status, pcr_cap);
    return TPM_BASELINE_OK;
}
