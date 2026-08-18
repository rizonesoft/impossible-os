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
#include "kernel/tpm_authz.h"
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
int tpm_baseline_status_is_failure(tpm_baseline_status_t bs)
{
    return bs == TPM_BASELINE_CORRUPT || bs == TPM_BASELINE_SELF_CORRUPT ||
           bs == TPM_BASELINE_UNBOUND || bs == TPM_BASELINE_TORN ||
           bs == TPM_BASELINE_RELABELED || bs == TPM_BASELINE_IDENTITY;
}

tpm_baseline_status_t tpm_baseline_pairing_status(tpm_pairing_t pairing)
{
    switch (pairing) {
    case TPM_PAIRING_TORN:
    case TPM_PAIRING_UNCOMMITTED:
    case TPM_PAIRING_IMPOSSIBLE:
        /* All three are a record/counter disagreement: an authorized record
         * exists and the pairing is broken. The DIRECTION picks the repair and
         * the caller reads it from the view; this status only asserts that the
         * repair is authorized and is never a fresh enrollment. */
        return TPM_BASELINE_TORN;
    case TPM_PAIRING_CURRENT:
        /* The pairing is intact, so the record is committed and says the stored
         * blob is not the enrolled one: the relabel attack. Reported as its own
         * status rather than folded into UNBOUND, whose repair (authorized
         * migration of a legacy blob) would authenticate the attacker's
         * content. */
        return TPM_BASELINE_RELABELED;
    case TPM_PAIRING_BADARG:
    default:
        /* No pairing claim was made. This branch is reached ONLY on a
         * TPM_NV_MISMATCH, and the sole way to get one before the pairing is
         * computed is an index that failed its enrolled contract -- a wrong
         * Name, a redefined index, a counter whose public area does not match
         * the compiled manifest. That is detected tamper, so it is reported as
         * an authenticity failure the boot PUBLISHES. It used to map to TPMERR,
         * which the boot excludes from publication: the attack was caught and
         * then dropped on the floor. */
        return TPM_BASELINE_IDENTITY;
    }
}

tpm_baseline_status_t tpm_baseline_nv_status(tpm_nv_status_t st)
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
        /* BUSY is TRANSIENT and, uniquely in this enum, RETRY-SAFE: the
         * transport gate refused before anything was submitted, so nothing
         * executed and asking again cannot double-apply. It gets its own status
         * rather than sharing NO_TPM, which reads as an absent or broken TPM --
         * seal and attestation have always preserved BUSY as retryable, and this
         * layer was the one consumer turning ordinary contention into a
         * measurement the boot gave up on. The verdict still stays unpublished
         * on this attempt; the difference is that a caller can now take another
         * one. */
        case TPM_NV_BUSY:      return TPM_BASELINE_BUSY;
        /* A caller-argument error is not a TPM fault, and this enum has the
         * right value for it. */
        case TPM_NV_BADARG:    return TPM_BASELINE_BADARG;
        /* An illegal attribute request is a DEFINITION-time argument error, not
         * a claim about an attacker. Named rather than left to default so a
         * future status cannot inherit this bucket by accident. */
        case TPM_NV_ATTRS:     return TPM_BASELINE_TPMERR;
        /* MISMATCH IS CONTEXT-DEPENDENT AND STAYS OUT OF THE IDENTITY CLASS.
         *
         * On the VERIFY path a pre-pair MISMATCH does mean the index failed its
         * enrolled contract -- but that path never reaches this map: it is
         * intercepted and routed through tpm_baseline_pairing_status, whose
         * BADARG arm is the verification-specific identity mapping.
         *
         * On the WRITE path the same status is produced by two benign-to-hostile
         * conditions this map cannot tell apart: the commit counter moving
         * between the in-sequence read and the write (`tpm_authz.c`, ordinary
         * SMP contention on a healthy machine), and a post-write readback that
         * differs (a write-integrity failure). Calling either of those identity
         * tamper would make tpm_baseline_enroll_bound report an attack on a
         * contended but perfectly healthy box, which is worse than the
         * unpublished-verdict bug that motivated widening this arm. */
        case TPM_NV_MISMATCH:  return TPM_BASELINE_TPMERR;
        /* THE UNAMBIGUOUS IDENTITY CLASS, and it must be PUBLISHABLE.
         *
         * RECREATED is an anchor destroyed and recreated, which is the rollback
         * attack itself; CONTRACT is a persisted identity that is corrupt.
         * Neither has a benign producer in ANY context, which is what makes
         * them safe to classify globally where MISMATCH is not.
         *
         * They used to map to TPMERR, and boot_phase1 excludes TPMERR from
         * publication precisely because it means no integrity conclusion was
         * reached -- so the attack was detected and then dropped, leaving the
         * previous verdict standing. TPM_BASELINE_IDENTITY exists for exactly
         * this class and IS published.
         *
         * Neither may EVER reach NO_BASELINE, which this file treats as a
         * genuine first enroll: that is the laundering the identity gate exists
         * to stop. */
        case TPM_NV_RECREATED:
        case TPM_NV_CONTRACT:  return TPM_BASELINE_IDENTITY;
        /* An operation that needs an update authority when none is installed is
         * a CONFIGURATION state, not a TPM fault, and it maps to the status that
         * says exactly that. Named explicitly because this switch promises the
         * default arm is unreachable for every value the enum defines -- letting
         * a new status inherit a bucket through the default is the failure that
         * promise exists to prevent, and TPM_NV_UNAVAIL did exactly that when it
         * was added. */
        case TPM_NV_UNAVAIL:   return TPM_BASELINE_UNBOUND;
        /* The remaining hard NV failures, named so the default arm is
         * unreachable for every status the enum currently defines and a NEW one
         * cannot inherit a bucket silently. */
        case TPM_NV_LOCKED:
        case TPM_NV_NOSPACE:
        case TPM_NV_DEFINED:
        case TPM_NV_RANGE:
        case TPM_NV_AUTH:
        case TPM_NV_TPMERR:    return TPM_BASELINE_TPMERR;
        default:               return TPM_BASELINE_TPMERR;
    }
}

/* The bare owner-auth write. Kept as a separate INTERNAL entry point so the
 * authorized path can reuse it without tripping the guard that exists to stop
 * an UNauthorized caller reaching it. */
static tpm_baseline_status_t tpm_baseline_enroll_unauthenticated(uint32_t nv_index,
                                                                 uint16_t alg);

tpm_baseline_status_t tpm_baseline_enroll(uint32_t nv_index, uint16_t alg)
{
    /* REFUSE the unauthenticated path once an authority is provisioned: this
     * function writes the blob under owner auth and nothing else, so completing
     * it would leave the bind record describing the PREVIOUS blob and the very
     * next verify would report UNBOUND. tpm_baseline_enroll_bound is the
     * authorized path. */
    if (tpm_authz_provisioned())
        return TPM_BASELINE_UNBOUND;
    return tpm_baseline_enroll_unauthenticated(nv_index, alg);
}

static tpm_baseline_status_t tpm_baseline_enroll_unauthenticated(uint32_t nv_index,
                                                                 uint16_t alg)
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
            return (rd == TPM_NV_OK) ? TPM_BASELINE_CORRUPT : tpm_baseline_nv_status(rd);
        }
    }
    b.generation = gen;
    if (tpm_baseline_finalize(&b) == 0u)
        return TPM_BASELINE_BADARG;

    /* Define the owner-auth DATA index (idempotent: an already-defined index
     * returns DEFINED, fine for re-enroll / rotation), then write the blob. */
    nv = tpm_nv_define_data(nv_index, (uint16_t)sizeof(b));
    if (nv != TPM_NV_OK && nv != TPM_NV_DEFINED)
        return tpm_baseline_nv_status(nv);
    nv = tpm_nv_write(nv_index, 0u, (const uint8_t *)&b, (uint16_t)sizeof(b));
    return tpm_baseline_nv_status(nv);
}

tpm_baseline_status_t tpm_baseline_enroll_bound(uint32_t nv_index, uint16_t alg,
                                                const struct tpm_authz_transition *tr)
{
    struct tpm_baseline b;
    uint8_t blob[sizeof(struct tpm_baseline)];
    uint16_t got = 0;
    uint32_t len;
    tpm_baseline_status_t st;
    tpm_nv_status_t nv;

    if (!tr)
        return TPM_BASELINE_BADARG;
    if (!tpm_authz_provisioned())
        return TPM_BASELINE_UNBOUND;
    /* Validate the GRANT before touching NV. A non-NULL transition used to be
     * enough to reach the owner write, with the grant's structure only checked
     * later inside the bind -- so an invalid or stale grant changed the
     * baseline and was refused afterwards, which is the wrong order for an
     * operation whose first half is not undoable. */
    if (tpm_authz_grant_wellformed(tr) != TPM_NV_OK)
        return TPM_BASELINE_BADARG;

    /* Blob FIRST, bind SECOND, and the order is the safety. The bind record's
     * digest covers the blob, so a bind that lands describes bytes already on
     * the device. If the bind fails, verify reports UNBOUND or MISMATCH and the
     * machine is fail-closed against a blob nobody authorized -- which is
     * recoverable by re-binding. Binding first would authenticate a blob that
     * may never be written. */
    st = tpm_baseline_enroll_unauthenticated(nv_index, alg);
    if (st != TPM_BASELINE_OK)
        return st;

    /* Bind the bytes that are ACTUALLY ON THE DEVICE, by reading them back.
     *
     * The first version of this function took a SECOND snapshot and bound that,
     * which could never match: the stored blob carries the monotonic generation
     * the enroll assigned and a fresh snapshot's generation is zero, so every
     * successful rotation would have produced a baseline its own verifier
     * rejects. Re-deriving the bytes is the wrong instrument even where it looks
     * equivalent -- the bind record's whole claim is about what is STORED, so it
     * has to be computed from what is stored.
     *
     * The readback validates too, so a write the TPM reported as successful but
     * stored differently is caught here rather than at the next boot. */
    nv = tpm_nv_read(nv_index, 0u, blob, (uint16_t)sizeof blob, &got);
    if (nv != TPM_NV_OK)
        return tpm_baseline_nv_status(nv);
    if (got != (uint16_t)sizeof blob || !tpm_baseline_validate(blob, got, &b))
        return TPM_BASELINE_CORRUPT;
    len = (uint32_t)got;

    nv = tpm_baseline_bind_write(blob, len, tr);
    if (nv != TPM_NV_OK)
        return tpm_baseline_nv_status(nv);
    return TPM_BASELINE_OK;
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
        st = tpm_baseline_nv_status(nv);
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

    /* AUTHENTICITY, after integrity. tpm_baseline_validate proves the blob is
     * well-formed; it cannot prove anyone authorized it. The relabel attack
     * produces a blob that passes every structural check -- old vulnerable
     * content, the current generation, a correctly recomputed CRC -- so the
     * only thing that catches it is the authenticated bind record, and a
     * verifier that never consults one leaves the whole boundary unwired. */
    {
        struct tpm_baseline_bind_view bview;
        tpm_nv_status_t bnv;

        if (tpm_authz_provisioned()) {
            /* The VIEW rather than the strict verifier, because this is the one
             * consumer that must distinguish a broken pairing from an
             * unauthenticated blob: they report the same MISMATCH and demand
             * opposite operator actions. Every other caller stays on the strict
             * wrapper and its unchanged contract. */
            bnv = tpm_baseline_bind_view(blob, (uint32_t)got, &bview);
            if (bnv == TPM_NV_NOTFOUND) {
                /* No bind record on a kernel that HAS an authority: the blob is
                 * legacy and unauthenticated. Refused, and deliberately NOT
                 * auto-wrapped into a bind record -- the content is
                 * owner-writable, so binding it now would authenticate whatever
                 * an attacker last wrote and leave the boundary worse than no
                 * boundary at all. */
                if (out_overall)
                    *out_overall = BOOT_INTEGRITY_MISMATCH;
                return TPM_BASELINE_UNBOUND;
            }
            if (bnv != TPM_NV_OK) {
                /* MISMATCH splits on the pairing, exhaustively, through the
                 * PURE map so every verdict is testable without a TPM;
                 * everything else goes through the NV map. */
                tpm_baseline_status_t bst =
                    (bnv == TPM_NV_MISMATCH)
                        ? tpm_baseline_pairing_status(bview.pairing)
                        : tpm_baseline_nv_status(bnv);
                /* THE VERDICT IS WRITTEN ONLY FOR A STATUS THAT PRODUCES ONE.
                 * Setting it before classifying fabricated a tamper report out
                 * of a BUDGET, BUSY or TRANSPORT failure -- a machine that
                 * could not measure, handed to any caller reading the
                 * out-parameter as "this machine failed to match". */
                if (out_overall && tpm_baseline_status_is_failure(bst))
                    *out_overall = BOOT_INTEGRITY_MISMATCH;
                return bst;
            }
        }
        /* No authority provisioned: the binding boundary is not in force on
         * this build, so the legacy path stands unchanged. Refusing here
         * instead would report MISMATCH on every correctly-enrolled machine
         * running a kernel that simply has no key compiled in, which is a brick
         * rather than a security improvement -- the same reason Secure Boot
         * with an empty db is OFF rather than refusing every image. */
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
    /* The two out-params are INDEPENDENTLY optional, as the header says. Gating
     * the comparison on both being present meant a caller that supplied a valid
     * status buffer but declined the count got OK and a verdict with its buffer
     * untouched -- silently stale detail beside a fresh verdict. Fill the buffer
     * whenever it exists; store the count only when its own pointer exists. */
    if (out_pcr_status) {
        uint8_t written = tpm_baseline_compare_pcrs(&golden, &current,
                                                    out_pcr_status, pcr_cap);
        if (out_pcr_n)
            *out_pcr_n = written;
    }
    return TPM_BASELINE_OK;
}
