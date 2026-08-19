/* ============================================================================
 * tpm_fake_tis.h -- shared fake-TIS transport for the TPM unit suites.
 *
 * A CC-dispatch fake that answers the commands the BASELINE wrappers issue --
 * NV_Read, NV_Write, NV_DefineSpace and PCR_Read -- from caller-supplied
 * content of arbitrary length, so a test can drive tpm_baseline_verify /
 * tpm_baseline_verify_detail / tpm_baseline_enroll end to end without a live
 * TPM. Install it with tpm_t_test_install(tpm_fake_tis_io(), TPM_T_IFACE_TIS, 1)
 * and put the previous transport back with tpm_t_test_restore().
 *
 * WHY A SECOND FAKE. test_tpm_nv.c carries its own file-static fake whose job
 * is the opposite of this one's: it generates MALFORMED and desynchronised
 * responses to prove the parser rejects them, and every one of its ~40 knobs is
 * wired into that file's tests. This fake generates only WELL-FORMED responses
 * and instead records a TRANSCRIPT -- which index, offset and length each
 * command actually asked for -- because the regression class it exists to catch
 * is a wrapper that marshals correctly but supplies the wrong arguments. A
 * CC-only fake serving one shared buffer would answer a read of the WRONG index
 * with the right bytes and stay green through exactly that bug.
 *
 * NOT a TPM simulator: no authorization is checked, no policy is evaluated, and
 * NV attributes are recorded rather than enforced. It models enough of the
 * device for the wrapper plumbing above it to be observable.
 * ========================================================================== */

#ifndef KERNEL_TEST_TPM_FAKE_TIS_H
#define KERNEL_TEST_TPM_FAKE_TIS_H

#include "kernel/types.h"
#include "kernel/tpm_transport.h"

/* Largest NV object the fake stores. The baseline blob is the biggest consumer
 * (sizeof(struct tpm_baseline)); TPM_NV_MAX_DATA is the transport's own cap. */
#define TPM_FAKE_TIS_NV_MAX   512u
#define TPM_FAKE_TIS_LOG_MAX  32u
#define TPM_FAKE_TIS_DIGEST   32u

/* One recorded request. `index` is the nvIndex for the NV commands and the PCR
 * index for PCR_Read; `sel` / `sel_count` are the raw TPML_PCR_SELECTION the
 * PCR_Read carried, so a test can assert the selection rather than trusting the
 * fake's own decode of it. */
struct tpm_fake_tis_req {
    uint32_t cc;
    uint32_t index;
    uint32_t attrs;      /* NV_DefineSpace: requested TPMA_NV attributes */
    uint16_t alg;        /* PCR_Read: hashAlg */
    uint16_t offset;     /* NV_Read / NV_Write: offset */
    uint16_t size;       /* NV_Read: requested size. NV_Write / Define: length */
    uint8_t  sel[3];     /* PCR_Read: pcrSelect bitmap */
    uint8_t  sel_count;  /* PCR_Read: TPML_PCR_SELECTION count */
};

/* Clear every knob, the NV store, the PCR table and the transcript. */
void tpm_fake_tis_reset(void);

/* The io vector to hand tpm_t_test_install(). */
const struct tpm_t_io *tpm_fake_tis_io(void);

/* ---- NV content ---- */

/* Define `nv_index` and seed it with `len` bytes (arbitrary length up to
 * TPM_FAKE_TIS_NV_MAX). A read of any OTHER index answers TPM_RC_HANDLE, which
 * is what a real TPM does for an undefined index -- so a wrapper that reads the
 * wrong index fails rather than being handed the right bytes. */
void tpm_fake_tis_nv_set(uint32_t nv_index, const uint8_t *data, uint16_t len);

/* Undefine every index: NV_Read answers TPM_RC_HANDLE (-> TPM_NV_NOTFOUND ->
 * TPM_BASELINE_NO_BASELINE). */
void tpm_fake_tis_nv_clear(void);

/* The bytes currently stored, as an NV_Write left them. Returns the stored
 * length (0 when no index is defined); copies min(stored, cap) bytes. */
uint16_t tpm_fake_tis_nv_content(uint8_t *out, uint16_t cap);

/* ---- PCR content ---- */

/* Serve `digest` (TPM_FAKE_TIS_DIGEST bytes) for `pcr_index` in the SHA-256
 * bank. An unset PCR reports the bank INACTIVE for that index (the response
 * echoes the selection bit CLEAR and carries no digest), which is how a real
 * TPM reports a PCR it did not read. */
void tpm_fake_tis_pcr_set(uint32_t pcr_index, const uint8_t *digest);
void tpm_fake_tis_pcr_clear(uint32_t pcr_index);

/* ---- Failure injection ---- */

/* Answer `cc` with `rc`. `times` < 0 fails every occurrence; `times` > 0 fails
 * only the first N, which is how a transient fault behaves.
 *
 * rc 0 DISABLES injection entirely, whatever `cc` and `times` say -- it clears
 * the record rather than arming a success. Arming one would intercept the
 * command and answer it with a bare success the handler never produced, so the
 * caller above would see its write succeed while NV never moved. */
void tpm_fake_tis_fail_cc(uint32_t cc, uint32_t rc, int times);

/* ---- Transcript ---- */

uint32_t tpm_fake_tis_log_count(void);
/* NULL when i is past the recorded set (the log saturates at
 * TPM_FAKE_TIS_LOG_MAX; tpm_fake_tis_log_overflow() reports that it did). */
const struct tpm_fake_tis_req *tpm_fake_tis_log(uint32_t i);
int      tpm_fake_tis_log_overflow(void);
uint32_t tpm_fake_tis_cc_count(uint32_t cc);

#endif /* KERNEL_TEST_TPM_FAKE_TIS_H */
