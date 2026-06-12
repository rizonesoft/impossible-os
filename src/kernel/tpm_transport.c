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

static void tpm_t_budget_begin(uint32_t ms)
{
    s_budget_active = 1;
    if (s_tsc_per_ms) {
        s_budget_deadline = tpm_t_rdtsc() + (uint64_t)ms * s_tsc_per_ms;
        s_budget_iters = 0;
    } else {
        s_budget_deadline = 0;
        s_budget_iters = (uint64_t)ms * TPM_T_NOFREQ_ITERS_PER_MS;
    }
}

static void tpm_t_budget_end(void)
{
    s_budget_active = 0;
    s_budget_deadline = 0;
    s_budget_iters = 0;
}

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
            if (tpm_t_rdtsc() >= s_budget_deadline)
                return 0;
        } else {
            if (s_budget_iters == 0)
                return 0;
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

int tpm2_submit(const uint8_t *cmd, uint32_t cmd_len,
                uint8_t *rsp, uint32_t rsp_cap)
{
    uint64_t irqf;
    int rc;

    if (!cmd || !rsp || cmd_len < TPM2_RSP_HEADER_SIZE ||
        cmd_len > TPM_T_MAX_RESPONSE || rsp_cap < TPM2_RSP_HEADER_SIZE)
        return TPM_T_ERR_ARG;

    /* Marshaled length must match the header's size field -- a
     * mismatch would desynchronize the FIFO byte accounting. */
    if (tpm2_be32_get(cmd + 2) != cmd_len)
        return TPM_T_ERR_ARG;

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

    rc = (s_iface == TPM_T_IFACE_CRB) ? crb_submit(cmd, cmd_len, rsp, rsp_cap)
                                      : tis_submit(cmd, cmd_len, rsp, rsp_cap);

    spin_lock_irqsave(&s_state_lock, &irqf);
    s_busy = 0;
    if (rc == TPM_T_ERR_TIMEOUT) {
        /* A wedged interface stays wedged: every later command would
         * burn its full poll budget. Fail sticky (s_available stays
         * set so callers see ERR_FAILED, not ERR_NODEV); boot
         * continues. */
        s_failed = 1;
    }
    spin_unlock_irqrestore(&s_state_lock, irqf);

    if (rc == TPM_T_ERR_TIMEOUT)
        klog(LOG_WARN, "TPM",
             "transport timed out; marking transport failed (sticky)");
    return rc;
}

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

    /* Bound the WHOLE probe sequence (GetCapability + optional Startup
     * + re-probe) so per-burst timeout resets cannot stretch boot. */
    tpm_t_budget_begin(TPM_T_INIT_PROBE_BUDGET_MS);

    cmd_len = tpm2_build_getcap_manufacturer(cmd, sizeof(cmd));
    n = tpm2_submit(cmd, cmd_len, rsp, sizeof(rsp));
    if (n < 0 || tpm2_rsp_parse(rsp, (uint32_t)n, 0, 0, &rc) != 0) {
        klog(LOG_WARN, "TPM", "startup probe failed (err=%d)", (uint64_t)n);
        tpm_t_budget_end();
        return;
    }

    if (rc == TPM2_RC_INITIALIZE) {
        /* Firmware did not send TPM2_Startup (kernel owns the TPM
         * lifecycle on this boot); send Startup(CLEAR) exactly once. */
        cmd_len = tpm2_build_startup(cmd, sizeof(cmd));
        n = tpm2_submit(cmd, cmd_len, rsp, sizeof(rsp));
        if (n < 0 || tpm2_rsp_parse(rsp, (uint32_t)n, 0, 0, &rc) != 0 ||
            rc != TPM2_RC_SUCCESS) {
            klog(LOG_WARN, "TPM", "TPM2_Startup(CLEAR) failed (rc=0x%x)",
                 (uint64_t)rc);
            tpm_t_budget_end();
            return;
        }
        klog(LOG_INFO, "TPM", "TPM2_Startup(CLEAR) sent (firmware skipped it)");
        cmd_len = tpm2_build_getcap_manufacturer(cmd, sizeof(cmd));
        n = tpm2_submit(cmd, cmd_len, rsp, sizeof(rsp));
        if (n < 0 || tpm2_rsp_parse(rsp, (uint32_t)n, 0, 0, &rc) != 0) {
            tpm_t_budget_end();
            return;
        }
    }
    tpm_t_budget_end();

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

/* ---- Test seam ---- */

const struct tpm_t_io *tpm_t_test_install(const struct tpm_t_io *io,
                                          int iface, int fast)
{
    const struct tpm_t_io *old = s_io;
    uint64_t irqf;

    spin_lock_irqsave(&s_state_lock, &irqf);
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
    spin_unlock_irqrestore(&s_state_lock, irqf);
    return old;
}

void tpm_t_test_budget_iters(uint64_t iters)
{
    if (iters) {
        s_budget_active = 1;
        s_budget_deadline = 0;
        s_budget_iters = iters;
    } else {
        tpm_t_budget_end();
    }
}
