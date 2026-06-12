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

/* ---- TPM2 command/response constants (TPM 2.0 Part 2) ---- */
#define TPM2_ST_NO_SESSIONS      0x8001u
#define TPM2_ST_SESSIONS         0x8002u
#define TPM2_ST_RSP_COMMAND      0x00C4u  /* response to an unrecognized command tag */
#define TPM2_CC_STARTUP          0x00000144u
#define TPM2_CC_GET_CAPABILITY   0x0000017Au
#define TPM2_CC_GET_RANDOM       0x0000017Bu
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

/* Parse + validate a response header: tag, size, rc. Checks
 * len >= header, header size field >= header and <= len. Returns 0 on
 * success, -1 malformed. Out pointers may be NULL. */
int tpm2_rsp_parse(const uint8_t *rsp, uint32_t len,
                   uint16_t *out_tag, uint32_t *out_size, uint32_t *out_rc);

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

/* Install a fake io + interface kind; returns the previous io so the
 * test can restore it. Also marks the transport available and clears
 * sticky failure. Passing io == NULL restores full pre-init state
 * (unavailable). Fast timeouts (fast != 0) shrink poll deadlines to
 * microsecond scale so timeout-path tests do not stall the suite. */
const struct tpm_t_io *tpm_t_test_install(const struct tpm_t_io *io,
                                          int iface, int fast);
