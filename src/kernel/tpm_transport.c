/* ============================================================================
 * tpm_transport.c -- TPM2 command transport (TIS/FIFO + CRB)
 *
 * Discovery: ACPI TPM2 table start method selects TIS/FIFO or CRB;
 * absent table falls back to a TIS probe at the PTP fixed base. All
 * register access goes through UC MMIO mappings (vmm_map_mmio_uc) and
 * an io-ops seam so unit tests can substitute a fake register file.
 *
 * ARCH: x86-64 (TIS/CRB MMIO is PC-platform; rdtsc poll deadlines).
 * ============================================================================ */

#include "kernel/tpm_transport.h"
#include "kernel/tpm.h"          /* tpm_alg_digest_len_pub() for PCR_Read bind */
#include "kernel/acpi.h"
#include "kernel/boot_info.h"
#include "kernel/boot_timing.h"
#include "kernel/klog.h"
#include "kernel/mm/vmm.h"
#include "kernel/sched/spinlock.h"

/* ---- PTP TIS/FIFO interface (TCG PC Client Platform TPM Profile) ---- */

#define TPM_TIS_BASE_PA      0xFED40000ull  /* locality 0 register page */
#define TPM_TIS_WINDOW_SIZE  0x1000u

#define TPM_TIS_REG_ACCESS   0x000u  /* u8 */
#define TPM_TIS_REG_STS      0x018u  /* u32: sts byte | burstCount << 8 */
#define TPM_TIS_REG_FIFO     0x024u  /* u8 */
#define TPM_TIS_REG_DID_VID  0xF00u  /* u32 */

#define TPM_TIS_ACCESS_ESTABLISHMENT  0x01u
#define TPM_TIS_ACCESS_REQUEST_USE    0x02u
#define TPM_TIS_ACCESS_ACTIVE         0x20u
#define TPM_TIS_ACCESS_VALID          0x80u

#define TPM_TIS_STS_RESPONSE_RETRY  0x02u
#define TPM_TIS_STS_EXPECT          0x08u
#define TPM_TIS_STS_DATA_AVAIL      0x10u
#define TPM_TIS_STS_GO              0x20u
#define TPM_TIS_STS_COMMAND_READY   0x40u
#define TPM_TIS_STS_VALID           0x80u

/* PTP 1.05 interface timing (TIMEOUT_A/B/C/D), in milliseconds. */
#define TPM_T_TIMEOUT_A_MS  750u   /* locality grant, burstCount refresh */
#define TPM_T_TIMEOUT_B_MS  2000u  /* commandReady, command completion */
#define TPM_T_TIMEOUT_C_MS  200u   /* stsValid between protocol steps */

#define TPM_T_TIMEOUT_D_MS  30u    /* short register settle */

/* ---- CRB control area (TCG ACPI spec) -- offsets within the mapped
 * control area window. The u64 fields are unaligned; read as 2x u32. ---- */

#define TPM_CRB_REG_REQ        0x00u  /* bit0 cmdReady, bit1 goIdle */
#define TPM_CRB_REG_STS        0x04u  /* bit0 error, bit1 idle */
#define TPM_CRB_REG_CANCEL     0x08u
#define TPM_CRB_REG_START      0x0Cu  /* bit0 start */
#define TPM_CRB_REG_CMD_SIZE   0x18u
#define TPM_CRB_REG_CMD_LO     0x1Cu
#define TPM_CRB_REG_CMD_HI     0x20u
#define TPM_CRB_REG_RSP_SIZE   0x24u
#define TPM_CRB_REG_RSP_LO     0x28u
#define TPM_CRB_REG_RSP_HI     0x2Cu

#define TPM_CRB_REQ_CMD_READY  0x01u
#define TPM_CRB_REQ_GO_IDLE    0x02u
#define TPM_CRB_START_START    0x01u

/* Sanity cap for CRB command/response buffer sizes from the control
 * area -- a corrupt table must not drive a multi-MiB UC mapping. */
#define TPM_CRB_BUF_SANITY_CAP 0x10000u

/* ---- ACPI TPM2 table (TCG ACPI spec, revision 4) ---- */

#define TPM_T_ACPI_SIG_TPM2  0x324D5054u  /* "TPM2" packed LE */
#define TPM2_TABLE_MIN_SIZE  52u          /* header 36 + class 2 + rsvd 2 + ctrl 8 + method 4 */
#define TPM2_START_METHOD_ACPI      2u
#define TPM2_START_METHOD_TIS       6u
#define TPM2_START_METHOD_CRB       7u
#define TPM2_START_METHOD_CRB_ACPI  8u

/* Poll loop iteration cap per ms when the TSC frequency is unknown
 * (boot_timing reported 0); keeps deadlines bounded without a clock. */
#define TPM_T_NOFREQ_ITERS_PER_MS 50000u
/* Fast-timeout iteration cap for unit tests (whole wait, not per ms). */
#define TPM_T_FAST_TEST_ITERS 64u

/* Cumulative wall-clock budget for the boot-time startup probe. The
 * per-step PTP timeouts reset on every FIFO burst, so a slow-but-
 * present device could otherwise stretch one boot probe into tens of
 * seconds (worst case: TIMEOUT_A per byte). A healthy TPM answers
 * GetCapability in single-digit milliseconds; one that cannot finish
 * the probe inside this budget is treated as wedged for this boot. */
#define TPM_T_INIT_PROBE_BUDGET_MS 3000u

/* ---- Module state (s_state_lock guards the transition fields; the
 * in-flight transaction itself runs lock-free under s_busy ownership,
 * never holding the spinlock across MMIO polling) ---- */
static spinlock_t s_state_lock = SPINLOCK_INIT;
static int s_iface = TPM_T_IFACE_NONE;
static int s_available;        /* transport up (s_state_lock) */
static int s_busy;             /* transaction in flight (s_state_lock) */
static int s_failed;           /* sticky wedge flag (s_state_lock) */
static int s_fast_timeouts;    /* unit tests only */
static uint32_t s_test_busy_ticks; /* unit tests: report the gate busy for N waiting-acquire ticks */
static uint64_t s_tsc_per_ms;  /* 0 = unknown frequency */

static volatile uint8_t *s_tis_base;   /* mapped TIS locality 0 window */
static volatile uint8_t *s_crb_base;   /* mapped CRB control area */
static volatile uint8_t *s_crb_cmd;    /* mapped CRB command buffer */
static volatile uint8_t *s_crb_rsp;    /* mapped CRB response buffer */
static uint32_t s_crb_cmd_size;
static uint32_t s_crb_rsp_size;

/* Page-aligned underlying mappings (for unwind): the usable pointers
 * above may sit at an offset inside these windows because CRB control
 * areas and data buffers are commonly NOT page-aligned (QEMU: base +
 * 0x40 / + 0x80) while the VMM MMIO mapper requires aligned phys. */
struct tpm_t_map {
    void    *base;  /* mapped VA (page-aligned), NULL = not mapped */
    uint32_t size;  /* mapped span for vmm_unmap_mmio */
};
static struct tpm_t_map s_tis_map;
static struct tpm_t_map s_crb_ctrl_map;
static struct tpm_t_map s_crb_cmd_map;
static struct tpm_t_map s_crb_rsp_map;

/* Map [pa, pa + size) UC, tolerating a non-page-aligned pa: aligns the
 * base down, widens the span by the offset, returns the usable pointer
 * at the original offset. Records the underlying mapping in *m. */
static volatile uint8_t *tpm_t_map_uc(uint64_t pa, uint32_t size,
                                      struct tpm_t_map *m)
{
    uint32_t off = (uint32_t)(pa & 0xFFFu);
    void *base = vmm_map_mmio_uc(pa - off, off + size);
    m->base = base;
    m->size = base ? off + size : 0;
    return base ? (volatile uint8_t *)base + off : 0;
}

static void tpm_t_unmap(struct tpm_t_map *m)
{
    if (m->base)
        vmm_unmap_mmio(m->base, m->size);
    m->base = 0;
    m->size = 0;
}

/* ---- Register io seam ---- */

static uint8_t mmio_r8(uint32_t off)
{
    volatile uint8_t *base = (s_iface == TPM_T_IFACE_CRB) ? s_crb_base
                                                          : s_tis_base;
    return base[off];
}
static void mmio_w8(uint32_t off, uint8_t v)
{
    volatile uint8_t *base = (s_iface == TPM_T_IFACE_CRB) ? s_crb_base
                                                          : s_tis_base;
    base[off] = v;
}
static uint32_t mmio_r32(uint32_t off)
{
    volatile uint8_t *base = (s_iface == TPM_T_IFACE_CRB) ? s_crb_base
                                                          : s_tis_base;
    return *(volatile uint32_t *)(base + off);
}
static void mmio_w32(uint32_t off, uint32_t v)
{
    volatile uint8_t *base = (s_iface == TPM_T_IFACE_CRB) ? s_crb_base
                                                          : s_tis_base;
    *(volatile uint32_t *)(base + off) = v;
}

static const struct tpm_t_io s_mmio_io = { mmio_r8, mmio_w8,
                                           mmio_r32, mmio_w32 };
static const struct tpm_t_io *s_io = &s_mmio_io;

/* Sequence/transaction primitives (defined with tpm2_submit below). */
static int  tpm_t_check_args(const uint8_t *cmd, uint32_t cmd_len,
                             const uint8_t *rsp, uint32_t rsp_cap);
static int  tpm_t_submit_txn(const uint8_t *cmd, uint32_t cmd_len,
                             uint8_t *rsp, uint32_t rsp_cap);
static int  tpm_t_seq_begin(uint32_t budget_ms);
static void tpm_t_seq_end(void);

/* ---- Poll deadline helpers ---- */

/* ARCH: x86-64 -- raw TSC read for poll deadlines (pre-scheduler safe). */
static inline uint64_t tpm_t_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

struct tpm_t_wait {
    uint64_t deadline;   /* TSC deadline (0 = iteration mode) */
    uint64_t iters_left; /* iteration budget when no TSC frequency */
};

/* Cumulative budget shared by every wait inside one bounded sequence
 * (the boot startup probe). 0/0 = inactive. Only touched from the
 * single thread running the bounded sequence. */
static uint64_t s_budget_deadline;  /* TSC mode */
static uint64_t s_budget_iters;     /* iteration mode */
static int      s_budget_active;
/* Latched by tpm_t_wait_tick when the CUMULATIVE budget (not the per-command
 * PTP deadline) is what ended a wait. Without it a budget expiry is
 * indistinguishable from a wedged interface and sticky-fails the transport,
 * disabling the TPM for the rest of the boot over a merely slow device. */
static int      s_budget_expired;
/* Live bounded-sequence token (0 = none) and the cleanup reserve it has not
 * claimed yet. The generation only ever increases, so a token from a finished
 * sequence can never match a later one. */
static uint64_t s_seq_token;
static uint32_t s_seq_cleanup_ms;
static uint64_t s_seq_generation;   /* only ever increases; 0 stays "no sequence" */
/* ONE cleanup allowance per sequence, carried across every teardown attempt.
 * Re-arming the full reserve per attempt made the advertised bound wrong by a
 * multiple: three FlushContext retries each took a fresh 2s reserve and each
 * expiry could add another abort, so a 3s operation could hold the transport
 * for ~17s. These fields hold that allowance between attempts so the retries
 * DEBIT one deadline instead of resetting it. */
static int      s_seq_cleanup_armed;
static uint64_t s_cleanup_deadline;
static uint64_t s_cleanup_iters;
static int      s_cleanup_expired;
/* Serializes submissions made with the SAME live sequence token, and keeps the
 * gate alive until every one of them has finished. s_busy keeps unrelated
 * transactions out but cannot separate two holders of one token, and a plain
 * boolean was not enough either: the callback can return while a holder is
 * still inside a submission, and tearing the sequence down then would release
 * s_busy with MMIO still in flight. A count plus a closing flag makes shutdown
 * wait instead. */
static int      s_seq_inflight;
static int      s_seq_closing;

/* Budget arm/disarm is unlocked, and what makes that safe is the BUSY GATE,
 * not the lock: a sequence holds s_busy for its whole duration, and both locked
 * readers short-circuit on s_busy before they would look at the budget
 * (tpm_t_seq_begin tests `s_busy || s_budget_active`, tpm2_submit_waiting tests
 * s_busy first), so a non-owner never observes these fields mid-change. That
 * property is load-bearing -- do not reorder either test.
 *
 * The arm/disarm sites are therefore MORE than seq_begin/seq_end: tpm_t_quiesce
 * disarms and restores around an abort, and tpm2_submit_seq_teardown swaps the
 * work budget out for the cleanup allowance and back. An earlier version of
 * this comment named those two functions as the only ones, which is exactly the
 * sentence a maintainer would rely on to conclude the unlocked reads in
 * tpm_t_wait_tick are safe -- and it had stopped being true. */

static void tpm_t_wait_begin(struct tpm_t_wait *w, uint32_t ms)
{
    if (s_fast_timeouts) {
        w->deadline = 0;
        w->iters_left = TPM_T_FAST_TEST_ITERS;
    } else if (s_tsc_per_ms) {
        w->deadline = tpm_t_rdtsc() + (uint64_t)ms * s_tsc_per_ms;
        w->iters_left = 0;
    } else {
        w->deadline = 0;
        w->iters_left = (uint64_t)ms * TPM_T_NOFREQ_ITERS_PER_MS;
    }
}

/* Returns 1 while the wait may continue, 0 on expiry. The cumulative
 * budget (when active) caps the SUM of all waits in the sequence --
 * per-step PTP timeouts reset on every burst and would otherwise
 * accumulate unboundedly against a slow device. */
static int tpm_t_wait_tick(struct tpm_t_wait *w)
{
    __asm__ volatile ("pause");
    if (s_budget_active) {
        if (s_budget_deadline) {
            if (tpm_t_rdtsc() >= s_budget_deadline) {
                s_budget_expired = 1;
                return 0;
            }
        } else {
            if (s_budget_iters == 0) {
                s_budget_expired = 1;
                return 0;
            }
            s_budget_iters--;
        }
    }
    if (w->deadline)
        return tpm_t_rdtsc() < w->deadline;
    if (w->iters_left == 0)
        return 0;
    w->iters_left--;
    return 1;
}

/* Poll an 8-bit register until (value & mask) == want. 0 ok, -1 timeout. */
static int tpm_t_poll8(uint32_t off, uint8_t mask, uint8_t want, uint32_t ms)
{
    struct tpm_t_wait w;
    tpm_t_wait_begin(&w, ms);
    do {
        if ((s_io->r8(off) & mask) == want)
            return 0;
    } while (tpm_t_wait_tick(&w));
    return -1;
}

/* Poll a 32-bit register until (value & mask) == want. 0 ok, -1 timeout. */
static int tpm_t_poll32(uint32_t off, uint32_t mask, uint32_t want,
                        uint32_t ms)
{
    struct tpm_t_wait w;
    tpm_t_wait_begin(&w, ms);
    do {
        if ((s_io->r32(off) & mask) == want)
            return 0;
    } while (tpm_t_wait_tick(&w));
    return -1;
}

/* ---- Pure marshaling helpers ---- */

uint32_t tpm2_build_startup(uint8_t *buf, uint32_t cap)
{
    if (!buf || cap < 12u)
        return 0;
    tpm2_be16_put(buf + 0, TPM2_ST_NO_SESSIONS);
    tpm2_be32_put(buf + 2, 12u);
    tpm2_be32_put(buf + 6, TPM2_CC_STARTUP);
    tpm2_be16_put(buf + 10, TPM2_SU_CLEAR);
    return 12u;
}

uint32_t tpm2_build_getcap_manufacturer(uint8_t *buf, uint32_t cap)
{
    if (!buf || cap < 22u)
        return 0;
    tpm2_be16_put(buf + 0, TPM2_ST_NO_SESSIONS);
    tpm2_be32_put(buf + 2, 22u);
    tpm2_be32_put(buf + 6, TPM2_CC_GET_CAPABILITY);
    tpm2_be32_put(buf + 10, TPM2_CAP_TPM_PROPERTIES);
    tpm2_be32_put(buf + 14, TPM2_PT_MANUFACTURER);
    tpm2_be32_put(buf + 18, 1u);  /* propertyCount */
    return 22u;
}

uint32_t tpm2_build_get_random(uint8_t *buf, uint32_t cap, uint16_t nbytes)
{
    if (!buf || cap < 12u || nbytes == 0u)
        return 0;
    tpm2_be16_put(buf + 0, TPM2_ST_NO_SESSIONS);
    tpm2_be32_put(buf + 2, 12u);
    tpm2_be32_put(buf + 6, TPM2_CC_GET_RANDOM);
    tpm2_be16_put(buf + 10, nbytes);
    return 12u;
}

int tpm2_parse_get_random(const uint8_t *rsp, uint32_t len,
                          uint8_t *out, uint32_t out_cap)
{
    uint32_t size, rc, i;
    uint16_t n;

    if (!out || tpm2_rsp_parse(rsp, len, 0, &size, &rc) != 0 ||
        rc != TPM2_RC_SUCCESS)
        return -1;
    /* Body: TPM2B_DIGEST = u16 size + bytes, inside the header-declared
     * response size. */
    if (size < TPM2_RSP_HEADER_SIZE + 2u)
        return -1;
    n = tpm2_be16_get(rsp + TPM2_RSP_HEADER_SIZE);
    if (n == 0u || (uint32_t)n > size - TPM2_RSP_HEADER_SIZE - 2u ||
        (uint32_t)n > out_cap)
        return -1;
    for (i = 0; i < n; i++)
        out[i] = rsp[TPM2_RSP_HEADER_SIZE + 2u + i];
    return (int)n;
}

uint32_t tpm2_build_pcr_read(uint8_t *buf, uint32_t cap, uint16_t alg,
                             uint32_t pcr_index)
{
    /* header(10) + TPML_PCR_SELECTION{ count(4) + hashAlg(2) + sizeofSelect(1)
     * + pcrSelect[3] } = 20 bytes, single PCR. */
    if (!buf || cap < 20u || pcr_index > 23u)
        return 0;
    tpm2_be16_put(buf + 0, TPM2_ST_NO_SESSIONS);
    tpm2_be32_put(buf + 2, 20u);                 /* commandSize */
    tpm2_be32_put(buf + 6, TPM2_CC_PCR_READ);
    tpm2_be32_put(buf + 10, 1u);                 /* pcrSelectionIn count = 1 */
    tpm2_be16_put(buf + 14, alg);                /* hashAlg */
    buf[16] = 3u;                                /* sizeofSelect = 3 (PCR 0..23) */
    buf[17] = 0u; buf[18] = 0u; buf[19] = 0u;    /* pcrSelect bitmap */
    buf[17u + (pcr_index >> 3)] = (uint8_t)(1u << (pcr_index & 7u));
    return 20u;
}

int tpm2_parse_pcr_read(const uint8_t *rsp, uint32_t len,
                        uint16_t alg, uint32_t pcr_index,
                        uint8_t *out, uint32_t out_cap)
{
    uint32_t size, rc, off, sel_count, dig_count, i;
    uint16_t dsz, sel_alg;
    uint8_t sos, byte_idx, bit_mask, requested_set;
    uint16_t expect_len = tpm_alg_digest_len_pub(alg);

    if (!out || expect_len == 0u || pcr_index >= 24u ||
        tpm2_rsp_parse(rsp, len, 0, &size, &rc) != 0 ||
        rc != TPM2_RC_SUCCESS)
        return -1;
    /* tpm2_rsp_parse proved size >= header and size <= len; every read below
     * is bounded by `size`, so it stays inside the response buffer. */
    off = TPM2_RSP_HEADER_SIZE;
    if (size < off + 4u) return -1;            /* updateCounter */
    off += 4u;

    /* Echoed pcrSelectionOut: must be exactly the single bank+PCR we asked
     * for. A desynchronized/malformed TPM that echoes a different alg or PCR
     * (or more than one selection) must NOT have its digest cached under our
     * (alg, pcr_index) key -- bind the response to the request here. */
    if (size < off + 4u) return -1;
    sel_count = tpm2_be32_get(rsp + off);
    off += 4u;
    if (sel_count != 1u) return -1;            /* single-PCR read -> one selection */
    if (size < off + 3u) return -1;            /* hashAlg(2) + sizeofSelect(1) */
    sel_alg = tpm2_be16_get(rsp + off);
    sos = rsp[off + 2u];
    if (sel_alg != alg || sos != 3u) return -1;
    if (size < off + 3u + (uint32_t)sos) return -1;
    byte_idx = (uint8_t)(pcr_index >> 3);      /* sos==3 covers PCR 0..23 */
    bit_mask = (uint8_t)(1u << (pcr_index & 7u));
    requested_set = (uint8_t)(rsp[off + 3u + byte_idx] & bit_mask);
    off += 3u + (uint32_t)sos;

    /* TPML_DIGEST: count(4) + count*TPM2B_DIGEST{ size(2) buffer[] }. */
    if (size < off + 4u) return -1;
    dig_count = tpm2_be32_get(rsp + off);
    off += 4u;
    if (!requested_set) {
        /* TPM reports the requested PCR was not read: inactive bank. The
         * value count must agree (no stray digest). */
        return (dig_count == 0u) ? 0 : -1;
    }
    /* Requested PCR was read: exactly one digest of the bank's exact size. */
    if (dig_count != 1u) return -1;
    if (size < off + 2u) return -1;
    dsz = tpm2_be16_get(rsp + off);
    off += 2u;
    if (dsz != expect_len || (uint32_t)dsz > size - off || (uint32_t)dsz > out_cap)
        return -1;
    for (i = 0; i < dsz; i++)
        out[i] = rsp[off + i];
    return (int)dsz;
}

int tpm2_get_random_bounded(uint8_t *out, uint32_t want, uint32_t budget_ms)
{
    uint8_t cmd[16];
    uint8_t rsp[80];
    uint32_t got = 0;
    int first_err = 0;

    if (!out || want == 0u)
        return TPM_T_ERR_ARG;

    /* Own the busy gate for the WHOLE sequence: no other transaction
     * can poll under (and consume) this sequence's budget, and a
     * racing caller bounces with BUSY instead of clobbering it. */
    first_err = tpm_t_seq_begin(budget_ms);
    if (first_err != 0)
        return first_err;
    while (got < want) {
        uint32_t remaining = want - got;
        uint16_t req = (remaining > 48u) ? 48u : (uint16_t)remaining;
        uint32_t cmd_len = tpm2_build_get_random(cmd, sizeof(cmd), req);
        int n = tpm_t_check_args(cmd, cmd_len, rsp, sizeof(rsp));
        if (n == 0)
            n = tpm_t_submit_txn(cmd, cmd_len, rsp, sizeof(rsp));
        if (n < 0) {
            first_err = n;
            break;
        }
        n = tpm2_parse_get_random(rsp, (uint32_t)n, out + got,
                                  want - got);
        if (n <= 0) {
            /* A malformed/failed FIRST response is a protocol error,
             * not a zero-byte yield -- honor the negative-on-first-
             * failure contract. Later failures keep partial bytes. */
            if (got == 0u)
                first_err = TPM_T_ERR_RESPONSE;
            break;
        }
        got += (uint32_t)n;
    }
    tpm_t_seq_end();

    /* Wipe the response staging buffer -- it held seed material. */
    {
        uint32_t i;
        for (i = 0; i < sizeof(rsp); i++)
            rsp[i] = 0;
    }

    if (got == 0u && first_err < 0)
        return first_err;
    return (int)got;
}

int tpm2_rsp_parse(const uint8_t *rsp, uint32_t len,
                   uint16_t *out_tag, uint32_t *out_size, uint32_t *out_rc)
{
    uint32_t size;
    uint16_t tag;
    if (!rsp || len < TPM2_RSP_HEADER_SIZE)
        return -1;
    /* TPM 2.0 Part 1 defines exactly three response tags; anything
     * else is not a TPM response (e.g. bus garbage parsed as one). */
    tag = tpm2_be16_get(rsp + 0);
    if (tag != TPM2_ST_NO_SESSIONS && tag != TPM2_ST_SESSIONS &&
        tag != TPM2_ST_RSP_COMMAND)
        return -1;
    size = tpm2_be32_get(rsp + 2);
    if (size < TPM2_RSP_HEADER_SIZE || size > len)
        return -1;
    if (out_tag)
        *out_tag = tpm2_be16_get(rsp + 0);
    if (out_size)
        *out_size = size;
    if (out_rc)
        *out_rc = tpm2_be32_get(rsp + 6);
    return 0;
}

int tpm2_rsp_params(const uint8_t *rsp, uint32_t len,
                    uint32_t *out_off, uint32_t *out_len)
{
    uint16_t tag;
    uint32_t size, rc;

    if (tpm2_rsp_parse(rsp, len, &tag, &size, &rc) != 0 ||
        rc != TPM2_RC_SUCCESS)
        return -1;
    /* tpm2_rsp_parse proved size >= TPM2_RSP_HEADER_SIZE and size <= len. */
    if (tag == TPM2_ST_SESSIONS) {
        uint32_t psize;
        /* header(10) + parameterSize(4), then the parameter span, then the
         * response auth area. parameterSize must leave the auth area inside
         * the declared size (auth area may be empty, so <= is correct). */
        if (size < TPM2_RSP_HEADER_SIZE + 4u)
            return -1;
        psize = tpm2_be32_get(rsp + TPM2_RSP_HEADER_SIZE);
        if (psize > size - TPM2_RSP_HEADER_SIZE - 4u)
            return -1;
        if (out_off) *out_off = TPM2_RSP_HEADER_SIZE + 4u;
        if (out_len) *out_len = psize;
    } else {
        /* ST_NO_SESSIONS / ST_RSP_COMMAND: parameters run header..size. */
        if (out_off) *out_off = TPM2_RSP_HEADER_SIZE;
        if (out_len) *out_len = size - TPM2_RSP_HEADER_SIZE;
    }
    return 0;
}

/* ---- TIS transaction ---- */

/* Read the current burstCount, waiting for it to go nonzero. Returns
 * the burst (>= 1) or 0 on timeout. */
static uint32_t tis_wait_burst(uint32_t ms)
{
    struct tpm_t_wait w;
    tpm_t_wait_begin(&w, ms);
    do {
        /* burstCount is only meaningful while stsValid is set (PTP
         * status register semantics) -- ignore it otherwise. */
        uint32_t sts = s_io->r32(TPM_TIS_REG_STS);
        if (sts & TPM_TIS_STS_VALID) {
            uint32_t burst = (sts >> 8) & 0xFFFFu;
            if (burst)
                return burst;
        }
    } while (tpm_t_wait_tick(&w));
    return 0;
}

/* Response-phase burst wait: like tis_wait_burst but ALSO requires
 * stsValid + dataAvail in the same read -- a device that drops
 * dataAvail before the header-declared length is a protocol violation
 * (the FIFO would return garbage), not a wait condition. Returns the
 * burst, 0 on timeout, or sets *violation when dataAvail dropped. */
static uint32_t tis_wait_read_burst(uint32_t ms, int *violation)
{
    struct tpm_t_wait w;
    *violation = 0;
    tpm_t_wait_begin(&w, ms);
    do {
        uint32_t sts = s_io->r32(TPM_TIS_REG_STS);
        if ((sts & TPM_TIS_STS_VALID)) {
            if (!(sts & TPM_TIS_STS_DATA_AVAIL)) {
                *violation = 1;
                return 0;
            }
            if ((sts >> 8) & 0xFFFFu)
                return (sts >> 8) & 0xFFFFu;
        }
    } while (tpm_t_wait_tick(&w));
    return 0;
}

/* Wait for stsValid then return the status byte, or -1 on timeout. */
static int tis_read_status(uint32_t ms)
{
    if (tpm_t_poll32(TPM_TIS_REG_STS, TPM_TIS_STS_VALID, TPM_TIS_STS_VALID,
                     ms) != 0)
        return -1;
    return (int)(s_io->r32(TPM_TIS_REG_STS) & 0xFFu);
}

static int tis_submit(const uint8_t *cmd, uint32_t cmd_len,
                      uint8_t *rsp, uint32_t rsp_cap)
{
    uint32_t sent, got, size;
    int sts;

    /* Idle -> Ready. commandReady is write-1-to-request and reads back
     * set once the TPM is ready for a command (PTP 1.05). Require
     * stsValid in the same read: status bits are stale without it. */
    s_io->w32(TPM_TIS_REG_STS, TPM_TIS_STS_COMMAND_READY);
    if (tpm_t_poll32(TPM_TIS_REG_STS,
                     TPM_TIS_STS_VALID | TPM_TIS_STS_COMMAND_READY,
                     TPM_TIS_STS_VALID | TPM_TIS_STS_COMMAND_READY,
                     TPM_T_TIMEOUT_B_MS) != 0)
        return TPM_T_ERR_TIMEOUT;

    /* Write all bytes except the last in burstCount-sized chunks. */
    sent = 0;
    while (sent + 1u < cmd_len) {
        uint32_t burst = tis_wait_burst(TPM_T_TIMEOUT_A_MS);
        uint32_t n, i;
        if (!burst)
            return TPM_T_ERR_TIMEOUT;
        n = cmd_len - 1u - sent;
        if (n > burst)
            n = burst;
        for (i = 0; i < n; i++)
            s_io->w8(TPM_TIS_REG_FIFO, cmd[sent + i]);
        sent += n;
    }

    /* TPM must still Expect the final byte. */
    sts = tis_read_status(TPM_T_TIMEOUT_C_MS);
    if (sts < 0)
        return TPM_T_ERR_TIMEOUT;
    if (!(sts & TPM_TIS_STS_EXPECT))
        return TPM_T_ERR_IO;

    if (!tis_wait_burst(TPM_T_TIMEOUT_A_MS))
        return TPM_T_ERR_TIMEOUT;
    s_io->w8(TPM_TIS_REG_FIFO, cmd[cmd_len - 1u]);

    /* Expect must clear: the TPM saw a complete command. */
    sts = tis_read_status(TPM_T_TIMEOUT_C_MS);
    if (sts < 0)
        return TPM_T_ERR_TIMEOUT;
    if (sts & TPM_TIS_STS_EXPECT)
        return TPM_T_ERR_IO;

    /* Execute. */
    s_io->w32(TPM_TIS_REG_STS, TPM_TIS_STS_GO);
    if (tpm_t_poll32(TPM_TIS_REG_STS,
                     TPM_TIS_STS_VALID | TPM_TIS_STS_DATA_AVAIL,
                     TPM_TIS_STS_VALID | TPM_TIS_STS_DATA_AVAIL,
                     TPM_T_TIMEOUT_B_MS) != 0)
        return TPM_T_ERR_TIMEOUT;

    /* Read the 10-byte response header first, then bound the rest. */
    got = 0;
    while (got < TPM2_RSP_HEADER_SIZE) {
        int violation;
        uint32_t burst = tis_wait_read_burst(TPM_T_TIMEOUT_A_MS, &violation);
        uint32_t n, i;
        if (violation)
            return TPM_T_ERR_IO;
        if (!burst)
            return TPM_T_ERR_TIMEOUT;
        n = TPM2_RSP_HEADER_SIZE - got;
        if (n > burst)
            n = burst;
        for (i = 0; i < n; i++)
            rsp[got + i] = s_io->r8(TPM_TIS_REG_FIFO);
        got += n;
    }
    size = tpm2_be32_get(rsp + 2);
    if (size < TPM2_RSP_HEADER_SIZE || size > rsp_cap ||
        size > TPM_T_MAX_RESPONSE) {
        /* Abort the oversized/malformed response: back to Ready wipes
         * the FIFO per PTP; the caller sees a clean error. */
        s_io->w32(TPM_TIS_REG_STS, TPM_TIS_STS_COMMAND_READY);
        return TPM_T_ERR_RESPONSE;
    }
    while (got < size) {
        int violation;
        uint32_t burst = tis_wait_read_burst(TPM_T_TIMEOUT_A_MS, &violation);
        uint32_t n, i;
        if (violation)
            return TPM_T_ERR_IO;
        if (!burst)
            return TPM_T_ERR_TIMEOUT;
        n = size - got;
        if (n > burst)
            n = burst;
        for (i = 0; i < n; i++)
            rsp[got + i] = s_io->r8(TPM_TIS_REG_FIFO);
        got += n;
    }

    /* dataAvail must drop: anything more means a response longer than
     * its own header claimed. */
    sts = tis_read_status(TPM_T_TIMEOUT_C_MS);
    if (sts < 0)
        return TPM_T_ERR_TIMEOUT;
    if (sts & TPM_TIS_STS_DATA_AVAIL)
        return TPM_T_ERR_IO;

    /* Back to Idle/Ready for the next command. */
    s_io->w32(TPM_TIS_REG_STS, TPM_TIS_STS_COMMAND_READY);
    return (int)size;
}

/* ---- CRB transaction ---- */

static int crb_submit(const uint8_t *cmd, uint32_t cmd_len,
                      uint8_t *rsp, uint32_t rsp_cap)
{
    uint32_t size, i;

    if (cmd_len > s_crb_cmd_size)
        return TPM_T_ERR_ARG;

    /* Ready handshake: request cmdReady, bit self-clears when ready. */
    s_io->w32(TPM_CRB_REG_REQ, TPM_CRB_REQ_CMD_READY);
    if (tpm_t_poll32(TPM_CRB_REG_REQ, TPM_CRB_REQ_CMD_READY, 0,
                     TPM_T_TIMEOUT_C_MS) != 0)
        return TPM_T_ERR_TIMEOUT;

    for (i = 0; i < cmd_len; i++)
        s_crb_cmd[i] = cmd[i];

    s_io->w32(TPM_CRB_REG_START, TPM_CRB_START_START);
    if (tpm_t_poll32(TPM_CRB_REG_START, TPM_CRB_START_START, 0,
                     TPM_T_TIMEOUT_B_MS) != 0)
        return TPM_T_ERR_TIMEOUT;

    /* Bound the response by its own header before copying out. */
    if (s_crb_rsp_size < TPM2_RSP_HEADER_SIZE)
        return TPM_T_ERR_RESPONSE;
    for (i = 0; i < TPM2_RSP_HEADER_SIZE; i++)
        rsp[i] = s_crb_rsp[i];
    size = tpm2_be32_get(rsp + 2);
    if (size < TPM2_RSP_HEADER_SIZE || size > rsp_cap ||
        size > TPM_T_MAX_RESPONSE || size > s_crb_rsp_size)
        return TPM_T_ERR_RESPONSE;
    for (i = TPM2_RSP_HEADER_SIZE; i < size; i++)
        rsp[i] = s_crb_rsp[i];

    /* Return to idle so firmware/next caller sees a quiet interface. */
    s_io->w32(TPM_CRB_REG_REQ, TPM_CRB_REQ_GO_IDLE);
    return (int)size;
}

/* ---- Public submit (whole-transaction serialization) ---- */

/* Validate command buffers (shared by the public and owned paths). */
static int tpm_t_check_args(const uint8_t *cmd, uint32_t cmd_len,
                            const uint8_t *rsp, uint32_t rsp_cap)
{
    if (!cmd || !rsp || cmd_len < TPM2_RSP_HEADER_SIZE ||
        cmd_len > TPM_T_MAX_RESPONSE || rsp_cap < TPM2_RSP_HEADER_SIZE)
        return TPM_T_ERR_ARG;
    /* Marshaled length must match the header's size field -- a
     * mismatch would desynchronize the FIFO byte accounting. */
    if (tpm2_be32_get(cmd + 2) != cmd_len)
        return TPM_T_ERR_ARG;
    return 0;
}

/* Return the interface to a quiescent state after a command was abandoned
 * mid-flight, so the NEXT command does not inherit a half-written FIFO or an
 * in-progress CRB transfer. TIS: write-1 commandReady is the PTP abort. CRB:
 * cancel the running command, then go idle. The caller still owns the gate.
 *
 * Runs with the cumulative budget DISARMED and restored afterwards: the abort
 * exists precisely because that budget is exhausted, so leaving it armed would
 * make every poll here fail instantly and the interface would stay dirty.
 * Touching the budget unlocked is the same owner-only discipline the rest of
 * these fields use -- the gate is held, so no other transaction can observe it.
 *
 * The abort polls to the NORMATIVE PTP deadline, TIMEOUT_B, not to a shorter
 * one of our choosing. A first attempt used 200 ms on the reasoning that an
 * abort is a register handshake rather than a command; that was an invented
 * number (it happens to equal TIMEOUT_C, which governs stsValid between
 * protocol steps, not an abort), and the PTP allows CRB cancellation and a FIFO
 * commandReady abort to take up to TIMEOUT_B. Declaring failure early would
 * sticky-fail a CONFORMING TPM that merely cancels slowly -- far worse than the
 * accounting problem the short bound was meant to solve.
 *
 * So the accounting is fixed by stating it honestly instead: a sequence's true
 * worst case is work + cleanup + 2 * TPM_T_TIMEOUT_B_MS, because an abort can
 * run after the work budget and again after the cleanup reserve. The budget
 * bounds how long we WAIT ON THE TPM to make progress; it does not bound the
 * recovery that makes the interface reusable afterwards.
 *
 * Returns 0 when the interface is quiescent, -1 when the abort itself failed
 * (which IS a wedge, and is what still earns a sticky failure). */
static int tpm_t_quiesce(void)
{
    int saved_active = s_budget_active;
    int rc = 0;

    s_budget_active = 0;
    if (s_iface == TPM_T_IFACE_CRB) {
        s_io->w32(TPM_CRB_REG_CANCEL, 1u);
        if (tpm_t_poll32(TPM_CRB_REG_START, TPM_CRB_START_START, 0,
                         TPM_T_TIMEOUT_B_MS) != 0)
            rc = -1;
        s_io->w32(TPM_CRB_REG_CANCEL, 0u);
        s_io->w32(TPM_CRB_REG_REQ, TPM_CRB_REQ_GO_IDLE);
    } else {
        s_io->w32(TPM_TIS_REG_STS, TPM_TIS_STS_COMMAND_READY);
        if (tpm_t_poll32(TPM_TIS_REG_STS,
                         TPM_TIS_STS_VALID | TPM_TIS_STS_COMMAND_READY,
                         TPM_TIS_STS_VALID | TPM_TIS_STS_COMMAND_READY,
                         TPM_T_TIMEOUT_B_MS) != 0)
            rc = -1;
    }
    s_budget_active = saved_active;
    return rc;
}

/* Run one transaction. The caller MUST own the busy gate (public
 * submit or an owned sequence); sticky failure is recorded here. */
static int tpm_t_submit_txn(const uint8_t *cmd, uint32_t cmd_len,
                            uint8_t *rsp, uint32_t rsp_cap)
{
    uint64_t irqf;
    int rc;

    s_budget_expired = 0;
    rc = (s_iface == TPM_T_IFACE_CRB) ? crb_submit(cmd, cmd_len, rsp, rsp_cap)
                                      : tis_submit(cmd, cmd_len, rsp, rsp_cap);

    /* A cumulative-BUDGET expiry is a policy decision about how long this boot
     * will wait, not evidence that the device is broken -- a slow-but-responsive
     * TPM is a real machine. Poisoning the transport for it would disable every
     * unrelated later operation. Abort the abandoned command so the interface is
     * clean, then report BUDGET and leave the transport usable. Only a failed
     * abort, or an ordinary PTP deadline, still means wedged. */
    if (rc == TPM_T_ERR_TIMEOUT && s_budget_expired) {
        if (tpm_t_quiesce() == 0) {
            klog(LOG_WARN, "TPM",
                 "command budget exhausted; operation abandoned, transport kept");
            return TPM_T_ERR_BUDGET;
        }
        klog(LOG_WARN, "TPM", "budget expiry recovery failed; interface wedged");
    }

    if (rc == TPM_T_ERR_TIMEOUT) {
        /* A wedged interface stays wedged: every later command would
         * burn its full poll budget. Fail sticky (s_available stays
         * set so callers see ERR_FAILED, not ERR_NODEV); boot
         * continues. */
        spin_lock_irqsave(&s_state_lock, &irqf);
        s_failed = 1;
        spin_unlock_irqrestore(&s_state_lock, irqf);
        klog(LOG_WARN, "TPM",
             "transport timed out; marking transport failed (sticky)");
    }
    return rc;
}

/* Arm the cumulative budget for `ms`. Caller holds s_state_lock, or owns the
 * gate (the teardown path swaps it in and back out under the gate). */
static void tpm_t_budget_arm(uint32_t ms)
{
    s_budget_active = 1;
    s_budget_expired = 0;
    if (s_tsc_per_ms) {
        s_budget_deadline = tpm_t_rdtsc() + (uint64_t)ms * s_tsc_per_ms;
        s_budget_iters = 0;
    } else {
        s_budget_deadline = 0;
        s_budget_iters = (uint64_t)ms * TPM_T_NOFREQ_ITERS_PER_MS;
    }
}

/* Atomically reserve the busy gate AND the cumulative budget for a
 * multi-command sequence: while a sequence runs, no other transaction
 * can poll (so the budget is only ever consumed by its owner), and no
 * other budget can be armed. Returns 0 on success or a TPM_T_ERR_*. */
/* Is the active budget spent RIGHT NOW? The latch alone is not the truth: it
 * only records that a poll already noticed, and a deadline can pass with no
 * poll in between -- which is exactly what a teardown does, since it runs on a
 * separate allowance and then restores the work budget along with the latch
 * value it had BEFORE. Checking the deadline makes the latch a cache rather
 * than the authority, and closes the window where an ordinary command started
 * after its budget had elapsed. */
static int tpm_t_budget_spent(void)
{
    if (!s_budget_active)
        return 0;
    if (s_budget_expired)
        return 1;
    if (s_budget_deadline)
        return (tpm_t_rdtsc() >= s_budget_deadline) ? 1 : 0;
    return (s_budget_iters == 0u) ? 1 : 0;
}

static int tpm_t_seq_begin(uint32_t budget_ms)
{
    uint64_t irqf;
    spin_lock_irqsave(&s_state_lock, &irqf);
    if (!s_available) {
        spin_unlock_irqrestore(&s_state_lock, irqf);
        return TPM_T_ERR_NODEV;
    }
    if (s_failed) {
        spin_unlock_irqrestore(&s_state_lock, irqf);
        return TPM_T_ERR_FAILED;
    }
    if (s_busy || s_budget_active) {
        spin_unlock_irqrestore(&s_state_lock, irqf);
        return TPM_T_ERR_BUSY;
    }
    s_busy = 1;
    tpm_t_budget_arm(budget_ms);
    spin_unlock_irqrestore(&s_state_lock, irqf);
    return 0;
}

static void tpm_t_seq_end(void)
{
    uint64_t irqf;
    spin_lock_irqsave(&s_state_lock, &irqf);
    s_busy = 0;
    s_budget_active = 0;
    s_budget_deadline = 0;
    s_budget_iters = 0;
    s_budget_expired = 0;
    s_seq_token = 0;
    s_seq_cleanup_ms = 0;
    s_seq_cleanup_armed = 0;
    s_cleanup_deadline = 0;
    s_cleanup_iters = 0;
    s_cleanup_expired = 0;
    s_seq_inflight = 0;
    s_seq_closing = 0;
    spin_unlock_irqrestore(&s_state_lock, irqf);
}

int tpm2_seq_run(uint32_t work_ms, uint32_t cleanup_ms,
                 tpm2_seq_fn fn, void *ctx)
{
    uint64_t irqf, token;
    int rc;

    if (!fn)
        return TPM_T_ERR_ARG;

    rc = tpm_t_seq_begin(work_ms);
    if (rc != 0)
        return rc;

    /* Mint the token AFTER the gate is held, so a caller holding a stale token
     * from a finished sequence can never be mistaken for this one. */
    spin_lock_irqsave(&s_state_lock, &irqf);
    token = ++s_seq_generation;
    s_seq_token = token;
    s_seq_cleanup_ms = cleanup_ms;
    spin_unlock_irqrestore(&s_state_lock, irqf);

    rc = fn(token, ctx);

    /* Stop accepting new submissions, then WAIT for any that are still running
     * before tearing the sequence down. Clearing the gate the moment fn()
     * returned would release s_busy while a holder of the same token was still
     * driving the FIFO/CRB, and the next transaction would interleave with it.
     * Nothing in the tree hands a token to another context today, but this is
     * an exported seam and the guarantee should not depend on that staying
     * true. The wait terminates because the in-flight command carries its own
     * deadline. */
    spin_lock_irqsave(&s_state_lock, &irqf);
    s_seq_closing = 1;
    spin_unlock_irqrestore(&s_state_lock, irqf);
    for (;;) {
        int busy;
        spin_lock_irqsave(&s_state_lock, &irqf);
        busy = s_seq_inflight;
        spin_unlock_irqrestore(&s_state_lock, irqf);
        if (!busy)
            break;
        __asm__ volatile ("pause");
    }

    /* Scoped: the sequence is released on EVERY path out of fn(), including the
     * early returns its error handling is full of. That is the whole reason the
     * gate is not exposed as a raw begin/end pair. */
    tpm_t_seq_end();
    return rc;
}

/* Validate the token AND claim the sequence's single in-flight slot, atomically.
 *
 * The token check alone was not exclusion: s_busy keeps UNRELATED transactions
 * out, but two holders of the SAME token both pass it, and could then enter the
 * transaction path concurrently and interleave FIFO/CRB bytes. Every callback
 * in the tree keeps its token on the call stack, so that is latent rather than
 * live -- but the seam advertises exclusive submission, and an advertised
 * guarantee that rests on callers behaving is not a guarantee. Claiming a slot
 * under the state lock makes it one. The lock is released before any MMIO, so
 * nothing spins on it for a TPM duration.
 *
 * Returns 0 on success (caller MUST release), or a TPM_T_ERR_*. */
static int tpm_t_seq_claim(tpm2_seq_t seq)
{
    uint64_t irqf;
    int rc = 0;
    spin_lock_irqsave(&s_state_lock, &irqf);
    if (seq == 0u || seq != s_seq_token)
        rc = TPM_T_ERR_SEQ;
    else if (s_failed)
        rc = TPM_T_ERR_FAILED;
    else if (s_seq_closing)
        rc = TPM_T_ERR_SEQ;      /* the sequence is shutting down; no new work */
    else if (s_seq_inflight)
        rc = TPM_T_ERR_BUSY;
    else
        s_seq_inflight = 1;
    spin_unlock_irqrestore(&s_state_lock, irqf);
    return rc;
}

/* Capture the cleanup allowance as the current attempt left it, so the next
 * teardown resumes from there rather than starting over. */
static void tpm_t_seq_save_cleanup(void)
{
    s_cleanup_deadline = s_budget_deadline;
    s_cleanup_iters    = s_budget_iters;
    s_cleanup_expired  = s_budget_expired;
}

static void tpm_t_seq_release(void)
{
    uint64_t irqf;
    spin_lock_irqsave(&s_state_lock, &irqf);
    s_seq_inflight = 0;
    spin_unlock_irqrestore(&s_state_lock, irqf);
}

int tpm2_submit_seq(tpm2_seq_t seq, const uint8_t *cmd, uint32_t cmd_len,
                    uint8_t *rsp, uint32_t rsp_cap)
{
    int rc = tpm_t_check_args(cmd, cmd_len, rsp, rsp_cap);
    if (rc != 0)
        return rc;
    rc = tpm_t_seq_claim(seq);
    if (rc != 0)
        return rc;
    /* Refuse BEFORE touching the interface once the budget is gone: starting a
     * command we already know cannot finish would leave the TPM mid-transfer
     * for the abort path to clean up, for nothing. */
    if (tpm_t_budget_spent()) {
        tpm_t_seq_release();
        return TPM_T_ERR_BUDGET;
    }
    rc = tpm_t_submit_txn(cmd, cmd_len, rsp, rsp_cap);
    tpm_t_seq_release();
    return rc;
}

#ifdef KERNEL_TESTS
uint64_t tpm_t_test_budget_deadline(void)
{
    return s_budget_deadline;
}

int tpm_t_test_seq_cleanup_pending(void)
{
    return (s_seq_cleanup_ms != 0u && !s_seq_cleanup_armed) ? 1 : 0;
}
#endif

int tpm2_submit_seq_teardown(tpm2_seq_t seq, const uint8_t *cmd, uint32_t cmd_len,
                             uint8_t *rsp, uint32_t rsp_cap)
{
    uint64_t saved_deadline, saved_iters;
    int saved_active, saved_expired;
    int rc = tpm_t_check_args(cmd, cmd_len, rsp, rsp_cap);

    if (rc != 0)
        return rc;
    rc = tpm_t_seq_claim(seq);
    if (rc != 0)
        return rc;

    /* A teardown runs on its OWN allowance, and the work budget is saved and
     * restored around it rather than extended.
     *
     * Extending the shared deadline was the previous design and it leaked: a
     * mandatory flush in the middle of a larger operation (the trial session
     * inside a baseline define) topped up the sequence, and the ORDINARY
     * commands after it -- including an irreversible NV_DefineSpace -- then
     * spent time explicitly reserved for teardown. Separating the two budgets
     * removes the leak in one direction and the "teardown starts on the last
     * few milliseconds of a spent work budget" hole in the other, because the
     * allowance no longer depends on what the work phase left behind.
     *
     * All teardowns in one sequence SHARE that allowance (see the swap below),
     * so the total stays work + cleanup plus up to two bounded aborts,
     * regardless of how many retries a teardown needs. */
    saved_deadline = s_budget_deadline;
    saved_iters    = s_budget_iters;
    saved_active   = s_budget_active;
    saved_expired  = s_budget_expired;

    /* Swap in the sequence's ONE cleanup allowance. The first teardown arms it
     * (including a ZERO one -- a sequence created with no reserve means exactly
     * that, and skipping the arm would silently fall back to the work budget,
     * which is the quiet borrowing this split exists to stop). Every LATER
     * teardown resumes the same allowance where the previous attempt left it,
     * so N retries debit one deadline instead of taking N fresh reserves. */
    if (!s_seq_cleanup_armed) {
        tpm_t_budget_arm(s_seq_cleanup_ms);
        s_seq_cleanup_armed = 1;
    } else {
        s_budget_active   = 1;
        s_budget_deadline = s_cleanup_deadline;
        s_budget_iters    = s_cleanup_iters;
        s_budget_expired  = s_cleanup_expired;
    }

    /* PREFLIGHT, before touching the interface. Arming alone does not stop a
     * spent allowance from running: every poll reads the device BEFORE ticking
     * the budget, so an immediately-ready TPM completes the command and the
     * allowance is never consulted -- "no reserve means no teardown" would have
     * been a guarantee in name only. Checking here makes it real and keeps the
     * refusal free of any TPM state change. */
    if (tpm_t_budget_spent()) {
        tpm_t_seq_save_cleanup();
        s_budget_deadline = saved_deadline;
        s_budget_iters    = saved_iters;
        s_budget_active   = saved_active;
        s_budget_expired  = saved_expired;
        tpm_t_seq_release();
        return TPM_T_ERR_BUDGET;
    }

    rc = tpm_t_submit_txn(cmd, cmd_len, rsp, rsp_cap);

    /* Carry whatever the attempt did NOT spend forward to the next one. */
    tpm_t_seq_save_cleanup();
    s_budget_deadline = saved_deadline;
    s_budget_iters    = saved_iters;
    s_budget_active   = saved_active;
    s_budget_expired  = saved_expired;
    tpm_t_seq_release();
    return rc;
}

int tpm2_submit(const uint8_t *cmd, uint32_t cmd_len,
                uint8_t *rsp, uint32_t rsp_cap)
{
    uint64_t irqf;
    int rc;

    rc = tpm_t_check_args(cmd, cmd_len, rsp, rsp_cap);
    if (rc != 0)
        return rc;

    spin_lock_irqsave(&s_state_lock, &irqf);
    if (!s_available) {
        spin_unlock_irqrestore(&s_state_lock, irqf);
        return TPM_T_ERR_NODEV;
    }
    if (s_failed) {
        spin_unlock_irqrestore(&s_state_lock, irqf);
        return TPM_T_ERR_FAILED;
    }
    if (s_busy) {
        spin_unlock_irqrestore(&s_state_lock, irqf);
        return TPM_T_ERR_BUSY;
    }
    s_busy = 1;
    spin_unlock_irqrestore(&s_state_lock, irqf);

    rc = tpm_t_submit_txn(cmd, cmd_len, rsp, rsp_cap);

    spin_lock_irqsave(&s_state_lock, &irqf);
    s_busy = 0;
    spin_unlock_irqrestore(&s_state_lock, irqf);
    return rc;
}

/* Budget-free wait tick: like tpm_t_wait_tick but does NOT touch the sequence
 * cumulative budget (s_budget_*), since the waiting-acquire below runs OUTSIDE
 * any owned sequence and must not consume another sequence's budget. Returns 1
 * while the wait may continue, 0 on expiry. */
static int tpm_t_wait_tick_nobudget(struct tpm_t_wait *w)
{
    __asm__ volatile ("pause");
    if (w->deadline)
        return tpm_t_rdtsc() < w->deadline;
    if (w->iters_left == 0)
        return 0;
    w->iters_left--;
    return 1;
}

int tpm2_submit_waiting(const uint8_t *cmd, uint32_t cmd_len,
                        uint8_t *rsp, uint32_t rsp_cap, uint32_t budget_ms)
{
    uint64_t irqf;
    int rc, acquired = 0;
    struct tpm_t_wait w;

    rc = tpm_t_check_args(cmd, cmd_len, rsp, rsp_cap);
    if (rc != 0)
        return rc;

    /* Acquire the busy gate, waiting up to budget_ms for an in-flight
     * transaction on another CPU to finish. This is the non-ISR CLEANUP submit
     * (FlushContext after a multi-command session flow): an immediate BUSY here
     * would abandon the session handle and erode the TPM's small session pool,
     * so a brief overlapping transaction is waited out instead of bounced. */
    tpm_t_wait_begin(&w, budget_ms);
    for (;;) {
        spin_lock_irqsave(&s_state_lock, &irqf);
        if (!s_available) {
            spin_unlock_irqrestore(&s_state_lock, irqf);
            return TPM_T_ERR_NODEV;
        }
        if (s_failed) {
            spin_unlock_irqrestore(&s_state_lock, irqf);
            return TPM_T_ERR_FAILED;
        }
        if (s_test_busy_ticks != 0u) {
            /* Unit-test simulation of a gate held by another CPU. */
            s_test_busy_ticks--;
        } else if (!s_busy && !s_budget_active) {
            s_busy = 1;
            acquired = 1;
        }
        spin_unlock_irqrestore(&s_state_lock, irqf);
        if (acquired)
            break;
        if (!tpm_t_wait_tick_nobudget(&w))
            return TPM_T_ERR_BUSY;   /* deadline expired -- gate never cleared */
    }

    rc = tpm_t_submit_txn(cmd, cmd_len, rsp, rsp_cap);

    spin_lock_irqsave(&s_state_lock, &irqf);
    s_busy = 0;
    spin_unlock_irqrestore(&s_state_lock, irqf);
    return rc;
}

/* KERNEL_TESTS-gated (release-flavor test-surface exclusion): called only from
 * src/kernel/test/test_tpm_nv.c (pruned entirely at KERNEL_TESTS=off). */
#ifdef KERNEL_TESTS
void tpm_t_test_busy_ticks(uint32_t n)
{
    uint64_t irqf;
    spin_lock_irqsave(&s_state_lock, &irqf);
    s_test_busy_ticks = n;
    spin_unlock_irqrestore(&s_state_lock, irqf);
}
#endif

int tpm_transport_available(void)
{
    uint64_t irqf;
    int up;
    spin_lock_irqsave(&s_state_lock, &irqf);
    up = s_available && !s_failed;
    spin_unlock_irqrestore(&s_state_lock, irqf);
    return up;
}

int tpm_transport_iface(void)
{
    return s_iface;
}

/* ---- Startup probe (never perturb firmware-owned state) ---- */

static void tpm_t_startup_probe(void)
{
    uint8_t cmd[24];
    uint8_t rsp[64];
    uint32_t cmd_len, rc;
    int n;

    /* Own the busy gate + cumulative budget for the WHOLE probe
     * sequence (GetCapability + optional Startup + re-probe) so
     * per-burst timeout resets cannot stretch boot and no concurrent
     * submitter can consume the probe budget. */
    if (tpm_t_seq_begin(TPM_T_INIT_PROBE_BUDGET_MS) != 0) {
        klog(LOG_WARN, "TPM", "startup probe skipped (transport busy)");
        return;
    }

    cmd_len = tpm2_build_getcap_manufacturer(cmd, sizeof(cmd));
    n = tpm_t_submit_txn(cmd, cmd_len, rsp, sizeof(rsp));
    if (n < 0 || tpm2_rsp_parse(rsp, (uint32_t)n, 0, 0, &rc) != 0) {
        klog(LOG_WARN, "TPM", "startup probe failed (err=%d)", (uint64_t)n);
        tpm_t_seq_end();
        return;
    }

    if (rc == TPM2_RC_INITIALIZE) {
        /* Firmware did not send TPM2_Startup (kernel owns the TPM
         * lifecycle on this boot); send Startup(CLEAR) exactly once. */
        cmd_len = tpm2_build_startup(cmd, sizeof(cmd));
        n = tpm_t_submit_txn(cmd, cmd_len, rsp, sizeof(rsp));
        if (n < 0 || tpm2_rsp_parse(rsp, (uint32_t)n, 0, 0, &rc) != 0 ||
            rc != TPM2_RC_SUCCESS) {
            klog(LOG_WARN, "TPM", "TPM2_Startup(CLEAR) failed (rc=0x%x)",
                 (uint64_t)rc);
            tpm_t_seq_end();
            return;
        }
        klog(LOG_INFO, "TPM", "TPM2_Startup(CLEAR) sent (firmware skipped it)");
        cmd_len = tpm2_build_getcap_manufacturer(cmd, sizeof(cmd));
        n = tpm_t_submit_txn(cmd, cmd_len, rsp, sizeof(rsp));
        if (n < 0 || tpm2_rsp_parse(rsp, (uint32_t)n, 0, 0, &rc) != 0) {
            tpm_t_seq_end();
            return;
        }
    }
    tpm_t_seq_end();

    /* GetCapability payload: moreData(1) capability(4) count(4)
     * property(4) value(4) -- manufacturer is 4 ASCII chars. */
    if (rc == TPM2_RC_SUCCESS && n >= 27) {
        char vendor[5];
        vendor[0] = (char)rsp[23];
        vendor[1] = (char)rsp[24];
        vendor[2] = (char)rsp[25];
        vendor[3] = (char)rsp[26];
        vendor[4] = 0;
        klog(LOG_INFO, "TPM", "TPM2 transport up (%s, vendor %s)",
             s_iface == TPM_T_IFACE_CRB ? "CRB" : "TIS", vendor);
    } else {
        klog(LOG_INFO, "TPM", "TPM2 transport up (%s, rc=0x%x)",
             s_iface == TPM_T_IFACE_CRB ? "CRB" : "TIS", (uint64_t)rc);
    }
}

/* ---- Discovery + init ---- */

/* Read an unaligned u64 control-area field as two u32 loads. */
static uint64_t crb_read_pa(uint32_t lo_off, uint32_t hi_off)
{
    return (uint64_t)s_io->r32(lo_off) |
           ((uint64_t)s_io->r32(hi_off) << 32);
}

static int tpm_t_init_tis(uint64_t base_pa)
{
    uint32_t did_vid;

    s_tis_base = tpm_t_map_uc(base_pa, TPM_TIS_WINDOW_SIZE, &s_tis_map);
    if (!s_tis_base) {
        klog(LOG_WARN, "TPM", "TIS MMIO map failed at 0x%lx", base_pa);
        return 0;
    }
    s_iface = TPM_T_IFACE_TIS;

    did_vid = s_io->r32(TPM_TIS_REG_DID_VID);
    if (did_vid == 0u || did_vid == 0xFFFFFFFFu) {
        klog(LOG_INFO, "TPM", "no TIS device at 0x%lx (DID_VID=0x%x)",
             base_pa, (uint64_t)did_vid);
        tpm_t_unmap(&s_tis_map);
        s_tis_base = 0;
        s_iface = TPM_T_IFACE_NONE;
        return 0;
    }

    /* Take locality 0 and hold it (the kernel is the platform owner
     * after ExitBootServices; PTP allows a standing locality 0). */
    s_io->w8(TPM_TIS_REG_ACCESS, TPM_TIS_ACCESS_REQUEST_USE);
    if (tpm_t_poll8(TPM_TIS_REG_ACCESS,
                    TPM_TIS_ACCESS_VALID | TPM_TIS_ACCESS_ACTIVE,
                    TPM_TIS_ACCESS_VALID | TPM_TIS_ACCESS_ACTIVE,
                    TPM_T_TIMEOUT_A_MS) != 0) {
        klog(LOG_WARN, "TPM", "TIS locality 0 not granted");
        tpm_t_unmap(&s_tis_map);
        s_tis_base = 0;
        s_iface = TPM_T_IFACE_NONE;
        return 0;
    }

    klog(LOG_INFO, "TPM", "TIS interface at 0x%lx (DID_VID=0x%x)",
         base_pa, (uint64_t)did_vid);
    return 1;
}

static int tpm_t_init_crb(uint64_t ctrl_pa)
{
    uint64_t cmd_pa, rsp_pa;

    if (!ctrl_pa) {
        klog(LOG_WARN, "TPM", "TPM2 table CRB control area address is 0");
        return 0;
    }
    /* CRB control areas are commonly a register tail INSIDE a larger
     * MMIO page (QEMU: interface base + 0x40), so the address is not
     * page-aligned; tpm_t_map_uc handles the offset. */
    s_crb_base = tpm_t_map_uc(ctrl_pa, 0x30u, &s_crb_ctrl_map);
    if (!s_crb_base) {
        klog(LOG_WARN, "TPM", "CRB control area map failed at 0x%lx",
             ctrl_pa);
        return 0;
    }
    s_iface = TPM_T_IFACE_CRB;

    s_crb_cmd_size = s_io->r32(TPM_CRB_REG_CMD_SIZE);
    s_crb_rsp_size = s_io->r32(TPM_CRB_REG_RSP_SIZE);
    cmd_pa = crb_read_pa(TPM_CRB_REG_CMD_LO, TPM_CRB_REG_CMD_HI);
    rsp_pa = crb_read_pa(TPM_CRB_REG_RSP_LO, TPM_CRB_REG_RSP_HI);

    if (!cmd_pa || !rsp_pa ||
        s_crb_cmd_size < TPM2_RSP_HEADER_SIZE ||
        s_crb_rsp_size < TPM2_RSP_HEADER_SIZE ||
        s_crb_cmd_size > TPM_CRB_BUF_SANITY_CAP ||
        s_crb_rsp_size > TPM_CRB_BUF_SANITY_CAP) {
        klog(LOG_WARN, "TPM",
             "CRB buffers invalid (cmd=0x%lx/%u rsp=0x%lx/%u)",
             cmd_pa, (uint64_t)s_crb_cmd_size, rsp_pa,
             (uint64_t)s_crb_rsp_size);
        goto fail_unwind;
    }

    if (rsp_pa == cmd_pa) {
        /* Command and response buffers commonly share one region: map
         * the LARGER of the two sizes so response copies bounded by
         * s_crb_rsp_size never read past the mapped window. */
        uint32_t span = (s_crb_rsp_size > s_crb_cmd_size) ? s_crb_rsp_size
                                                          : s_crb_cmd_size;
        s_crb_cmd = tpm_t_map_uc(cmd_pa, span, &s_crb_cmd_map);
        s_crb_rsp = s_crb_cmd;
    } else {
        s_crb_cmd = tpm_t_map_uc(cmd_pa, s_crb_cmd_size, &s_crb_cmd_map);
        s_crb_rsp = tpm_t_map_uc(rsp_pa, s_crb_rsp_size, &s_crb_rsp_map);
    }
    if (!s_crb_cmd || !s_crb_rsp) {
        klog(LOG_WARN, "TPM", "CRB buffer map failed");
        goto fail_unwind;
    }

    klog(LOG_INFO, "TPM", "CRB interface, ctrl 0x%lx cmd %u rsp %u bytes",
         ctrl_pa, (uint64_t)s_crb_cmd_size, (uint64_t)s_crb_rsp_size);
    return 1;

fail_unwind:
    /* Unmap in reverse order and clear every static so a malformed
     * table cannot leave stray UC windows or stale pointers behind. */
    tpm_t_unmap(&s_crb_rsp_map);
    tpm_t_unmap(&s_crb_cmd_map);
    tpm_t_unmap(&s_crb_ctrl_map);
    s_crb_cmd = 0;
    s_crb_rsp = 0;
    s_crb_base = 0;
    s_crb_cmd_size = 0;
    s_crb_rsp_size = 0;
    s_iface = TPM_T_IFACE_NONE;
    return 0;
}

void tpm_transport_init(void)
{
    const uint8_t *tbl = 0;
    uint32_t tbl_size = 0;
    int up = 0;

    s_tsc_per_ms = boot_timing_tsc_freq() / 1000u;

    if (acpi_is_ready() &&
        acpi_get_raw_table(TPM_T_ACPI_SIG_TPM2, &tbl, &tbl_size)) {
        if (tbl_size < TPM2_TABLE_MIN_SIZE) {
            klog(LOG_WARN, "TPM", "TPM2 table truncated (%u bytes)",
                 (uint64_t)tbl_size);
        } else {
            /* Unaligned u64/u32 reads from the byte view -- the table
             * is plain memory, not MMIO. */
            uint64_t ctrl_pa =
                (uint64_t)tbl[40] | ((uint64_t)tbl[41] << 8) |
                ((uint64_t)tbl[42] << 16) | ((uint64_t)tbl[43] << 24) |
                ((uint64_t)tbl[44] << 32) | ((uint64_t)tbl[45] << 40) |
                ((uint64_t)tbl[46] << 48) | ((uint64_t)tbl[47] << 56);
            uint32_t method =
                (uint32_t)tbl[48] | ((uint32_t)tbl[49] << 8) |
                ((uint32_t)tbl[50] << 16) | ((uint32_t)tbl[51] << 24);

            if (method == TPM2_START_METHOD_TIS) {
                up = tpm_t_init_tis(TPM_TIS_BASE_PA);
            } else if (method == TPM2_START_METHOD_CRB) {
                up = tpm_t_init_crb(ctrl_pa);
            } else if (method == TPM2_START_METHOD_ACPI ||
                       method == TPM2_START_METHOD_CRB_ACPI) {
                /* Both need the ACPI start method object (AML); the
                 * kernel has no AML interpreter yet (owned by the
                 * ACPICA integration roadmap). */
                klog(LOG_WARN, "TPM",
                     "TPM2 start method %u needs AML; transport degraded",
                     (uint64_t)method);
            } else {
                klog(LOG_WARN, "TPM", "TPM2 start method %u unsupported",
                     (uint64_t)method);
            }
        }
    } else if (g_boot_info.tpm_available) {
        /* Firmware saw a TPM (event log exists) but no TPM2 table is
         * visible -- probe the PTP fixed TIS base before giving up. */
        up = tpm_t_init_tis(TPM_TIS_BASE_PA);
    } else {
        klog(LOG_INFO, "TPM", "no TPM2 table and no firmware TPM; "
             "transport not started");
    }

    if (!up) {
        s_iface = TPM_T_IFACE_NONE;
        return;
    }

    {
        uint64_t irqf;
        spin_lock_irqsave(&s_state_lock, &irqf);
        s_available = 1;
        s_failed = 0;
        spin_unlock_irqrestore(&s_state_lock, irqf);
    }

    tpm_t_startup_probe();
}

/* ---- Test seam ----
 * KERNEL_TESTS-gated (release-flavor test-surface exclusion): every function below replaces
 * the live callback table, CRB buffer pointers, or budget-throttle state
 * -- all called exclusively from src/kernel/test/test_tpm_*.c (already
 * pruned entirely at KERNEL_TESTS=off). Guarding the definitions here too
 * so the release map carries none of these state-mutating symbols even
 * though this TU (production TPM transport code) stays in the link. */
#ifdef KERNEL_TESTS

struct tpm_t_test_state tpm_t_test_install(const struct tpm_t_io *io,
                                           int iface, int fast)
{
    struct tpm_t_test_state old;
    uint64_t irqf;

    spin_lock_irqsave(&s_state_lock, &irqf);
    /* Capture the FULL prior state before perturbing it, so a later restore
     * puts back iface/available/failed/fast, not just the io pointer. */
    old.io = s_io;
    old.iface = s_iface;
    old.available = s_available;
    old.failed = s_failed;
    old.fast = s_fast_timeouts;
    old.busy = s_busy;
    old.test_busy_ticks = s_test_busy_ticks;
    if (io) {
        s_io = io;
        s_iface = iface;
        s_available = 1;
        s_failed = 0;
        s_fast_timeouts = fast ? 1 : 0;
    } else {
        s_io = &s_mmio_io;
        s_iface = TPM_T_IFACE_NONE;
        s_available = 0;
        s_failed = 0;
        s_fast_timeouts = 0;
    }
    s_busy = 0;
    /* Also drop any live SEQUENCE state. Clearing only s_busy left a stale
     * s_seq_token matching for a sequence whose gate had just been revoked, and
     * an armed budget belonging to it. */
    s_seq_token = 0;
    s_seq_cleanup_ms = 0;
    s_seq_cleanup_armed = 0;
    s_seq_inflight = 0;
    s_budget_active = 0;
    s_budget_expired = 0;
    s_test_busy_ticks = 0;
    spin_unlock_irqrestore(&s_state_lock, irqf);
    return old;
}

void tpm_t_test_restore(struct tpm_t_test_state st)
{
    uint64_t irqf;

    spin_lock_irqsave(&s_state_lock, &irqf);
    s_io = st.io ? st.io : &s_mmio_io;   /* a zeroed snapshot -> safe not-available */
    s_iface = st.iface;
    s_available = st.available;
    s_failed = st.failed;
    s_fast_timeouts = st.fast;
    s_busy = st.busy;
    s_test_busy_ticks = st.test_busy_ticks;
    spin_unlock_irqrestore(&s_state_lock, irqf);
}

struct tpm_t_crb_snapshot tpm_t_test_install_crb_buffers(volatile uint8_t *cmd,
                                                         uint32_t cmd_size,
                                                         volatile uint8_t *rsp,
                                                         uint32_t rsp_size)
{
    struct tpm_t_crb_snapshot prev;
    uint64_t irqf;
    spin_lock_irqsave(&s_state_lock, &irqf);
    prev.cmd = s_crb_cmd; prev.cmd_size = s_crb_cmd_size;
    prev.rsp = s_crb_rsp; prev.rsp_size = s_crb_rsp_size;
    s_crb_cmd = cmd;
    s_crb_cmd_size = cmd ? cmd_size : 0u;
    s_crb_rsp = rsp;
    s_crb_rsp_size = rsp ? rsp_size : 0u;
    spin_unlock_irqrestore(&s_state_lock, irqf);
    return prev;
}

void tpm_t_test_restore_crb_buffers(struct tpm_t_crb_snapshot snap)
{
    uint64_t irqf;
    spin_lock_irqsave(&s_state_lock, &irqf);
    s_crb_cmd = snap.cmd;
    s_crb_cmd_size = snap.cmd_size;
    s_crb_rsp = snap.rsp;
    s_crb_rsp_size = snap.rsp_size;
    spin_unlock_irqrestore(&s_state_lock, irqf);
}

void tpm_t_test_budget_iters(uint64_t iters)
{
    uint64_t irqf;
    spin_lock_irqsave(&s_state_lock, &irqf);
    s_budget_active = iters ? 1 : 0;
    s_budget_deadline = 0;
    s_budget_iters = iters;
    spin_unlock_irqrestore(&s_state_lock, irqf);
}

int tpm_t_test_budget_active(void)
{
    uint64_t irqf;
    int active;
    spin_lock_irqsave(&s_state_lock, &irqf);
    active = s_budget_active;
    spin_unlock_irqrestore(&s_state_lock, irqf);
    return active;
}

#endif /* KERNEL_TESTS */
