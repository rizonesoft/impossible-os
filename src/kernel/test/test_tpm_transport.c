/* ============================================================================
 * test_tpm_transport.c -- TPM2 command transport tests
 *
 * Pure-helper tests (marshaling, response parsing) plus fake-register
 * transport tests through the tpm_t_test_install() io seam: happy
 * path with burst=1 chunking, oversized response, timeout -> sticky
 * fail, busy reentrancy. No MMIO, no live boot infrastructure.
 * ============================================================================ */

#include "kernel/test/test.h"
#include "kernel/entropy.h"
#include "kernel/tpm.h"
#include "kernel/tpm_transport.h"
#include "libc/string.h"

/* ---- Fake TIS register file ----
 * Implements just enough of the PTP FIFO state machine for
 * tis_submit(): commandReady, Expect during command bytes, dataAvail
 * during response bytes, burstCount permanently 1 (worst-case
 * chunking). The canned response is set per scenario. */

#define FAKE_BUF_CAP 96u

static uint8_t  fk_cmd[FAKE_BUF_CAP];
static uint32_t fk_cmd_len;       /* bytes received so far */
static uint32_t fk_cmd_expect;    /* total command bytes (from header) */
static uint8_t  fk_rsp[FAKE_BUF_CAP];
static uint32_t fk_rsp_len;       /* canned response length */
static uint32_t fk_rsp_pos;       /* read cursor */
static int      fk_ready;         /* commandReady latched */
static int      fk_executed;      /* go received, response phase */
static uint32_t fk_cmd_total;     /* bytes received when go arrived */
static int      fk_dead;          /* all waits time out (wedge test) */
static int      fk_abort_refused; /* commandReady never asserts again: the
                                   * budget-expiry ABORT itself fails, which is
                                   * the branch that must still sticky-fail */
static int      fk_no_valid;      /* status reads lack stsValid (stale) */
static uint32_t fk_burst_delay;   /* STS reads reporting burst=0 first */
static int      fk_vary;          /* vary response payload per command */
static int      fk_one_shot;      /* die after the first completed command */
static uint16_t fk_rand_req[4];   /* GetRandom bytesRequested per command */
static uint32_t fk_rand_req_n;
static int      fk_expect_mode;   /* 0 normal, 1 never assert, 2 stuck on */
static int      fk_stuck_avail;   /* dataAvail never drops (trailing bytes) */
static int      fk_reentry_rc;    /* captured nested-submit rc */
static int      fk_reentry_armed; /* 1: nested submit; 2: nested bounded */
static int      fk_budget_after_nested;  /* budget state after the probe */

/* TIS register offsets/bits mirrored from the transport (the fake
 * implements the same PTP contract the real device does). */
#define FK_REG_STS   0x018u
#define FK_REG_FIFO  0x024u
#define FK_STS_EXPECT        0x08u
#define FK_STS_DATA_AVAIL    0x10u
#define FK_STS_GO            0x20u
#define FK_STS_COMMAND_READY 0x40u
#define FK_STS_VALID         0x80u

static uint8_t fake_r8(uint32_t off)
{
    if (off == FK_REG_FIFO && fk_executed && fk_rsp_pos < fk_rsp_len)
        return fk_rsp[fk_rsp_pos++];
    return 0;
}

static void fake_w8(uint32_t off, uint8_t v)
{
    if (off != FK_REG_FIFO || fk_executed)
        return;
    if (fk_reentry_armed) {
        /* Reentrancy probe: a nested submit (or nested bounded RNG
         * sequence) during an in-flight transaction must bounce with
         * TPM_T_ERR_BUSY without disturbing the outer caller. */
        int mode = fk_reentry_armed;
        uint8_t cmd[12], rsp[16], rnd[8];
        uint32_t n = tpm2_build_startup(cmd, sizeof(cmd));
        fk_reentry_armed = 0;
        fk_reentry_rc = (mode == 2)
            ? tpm2_get_random_bounded(rnd, sizeof(rnd), 50u)
            : tpm2_submit(cmd, n, rsp, sizeof(rsp));
        /* The outer sequence's budget must survive the nested bounce. */
        fk_budget_after_nested = tpm_t_test_budget_active();
    }
    if (fk_cmd_len < FAKE_BUF_CAP)
        fk_cmd[fk_cmd_len] = v;
    fk_cmd_len++;
    if (fk_cmd_len == 6u)
        fk_cmd_expect = tpm2_be32_get(fk_cmd + 2);
}

static uint32_t fake_r32(uint32_t off)
{
    uint8_t sts;
    if (off != FK_REG_STS || fk_dead)
        return 0;
    if (fk_no_valid) {
        /* Stale status: burstCount nonzero but stsValid clear -- the
         * transport must not trust any of it. */
        return 1u << 8;
    }
    sts = FK_STS_VALID;
    if (fk_ready && !fk_executed && fk_cmd_len == 0 && !fk_abort_refused)
        sts |= FK_STS_COMMAND_READY;
    if (fk_expect_mode == 2) {
        sts |= FK_STS_EXPECT;
    } else if (fk_expect_mode == 0 &&
               !fk_executed && fk_cmd_len > 0 && fk_cmd_len < fk_cmd_expect) {
        sts |= FK_STS_EXPECT;
    }
    if (fk_stuck_avail || (fk_executed && fk_rsp_pos < fk_rsp_len))
        sts |= FK_STS_DATA_AVAIL;
    if (fk_burst_delay) {
        /* Slow device: valid status but no burst capacity yet. */
        fk_burst_delay--;
        return (uint32_t)sts;
    }
    /* burstCount = 1: worst-case chunking on every byte. */
    return (uint32_t)sts | (1u << 8);
}

static void fake_w32(uint32_t off, uint32_t v)
{
    if (off != FK_REG_STS)
        return;
    if (v & FK_STS_COMMAND_READY) {
        fk_ready = 1;
        fk_executed = 0;
        fk_cmd_len = 0;
        fk_cmd_expect = 0;
        fk_rsp_pos = 0;
    }
    if ((v & FK_STS_GO) && fk_cmd_len >= fk_cmd_expect) {
        fk_executed = 1;
        fk_cmd_total = fk_cmd_len;
        /* Record GetRandom bytesRequested for protocol assertions. */
        if (fk_cmd_len >= 12u &&
            tpm2_be32_get(fk_cmd + 6) == TPM2_CC_GET_RANDOM &&
            fk_rand_req_n < 4u)
            fk_rand_req[fk_rand_req_n++] = tpm2_be16_get(fk_cmd + 10);
        if (fk_vary)
            fk_rsp[12] = (uint8_t)(fk_rsp[12] + 1u);
        if (fk_one_shot == 1)
            fk_one_shot = 2;  /* arm: next commandReady kills the fake */
    }
    if ((v & FK_STS_COMMAND_READY) && fk_one_shot == 2)
        fk_dead = 1;
}

static const struct tpm_t_io fk_io = { fake_r8, fake_w8,
                                       fake_r32, fake_w32 };

static void fake_reset(void)
{
    fk_cmd_len = 0;
    fk_cmd_expect = 0;
    fk_rsp_len = 0;
    fk_rsp_pos = 0;
    fk_ready = 0;
    fk_executed = 0;
    fk_cmd_total = 0;
    fk_dead = 0;
    fk_abort_refused = 0;
    fk_no_valid = 0;
    fk_burst_delay = 0;
    fk_vary = 0;
    fk_one_shot = 0;
    fk_rand_req[0] = fk_rand_req[1] = fk_rand_req[2] = fk_rand_req[3] = 0;
    fk_rand_req_n = 0;
    fk_expect_mode = 0;
    fk_stuck_avail = 0;
    fk_reentry_rc = 0;
    fk_reentry_armed = 0;
    fk_budget_after_nested = -1;
}

/* Stage a canned success response with the given rc and total size. */
static void fake_set_rsp(uint32_t rc, uint32_t size)
{
    uint32_t i;
    for (i = 0; i < FAKE_BUF_CAP; i++)
        fk_rsp[i] = 0;
    tpm2_be16_put(fk_rsp + 0, 0x8001u);
    tpm2_be32_put(fk_rsp + 2, size);
    tpm2_be32_put(fk_rsp + 6, rc);
    fk_rsp_len = (size <= FAKE_BUF_CAP) ? size : FAKE_BUF_CAP;
}

/* ---- Pure helper tests ---- */

static void test_tpm2_marshal(void)
{
    uint8_t buf[24];
    uint16_t tag;
    uint32_t size, rc;

    TEST_ASSERT_EQ(tpm2_build_startup(buf, sizeof(buf)), 12u,
                   "Startup command is 12 bytes");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 0), TPM2_ST_NO_SESSIONS,
                   "Startup tag is TPM_ST_NO_SESSIONS");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 2), 12u,
                   "Startup header size equals command length");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 6), TPM2_CC_STARTUP,
                   "Startup command code");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 10), TPM2_SU_CLEAR,
                   "Startup type SU_CLEAR");
    TEST_ASSERT_EQ(tpm2_build_startup(buf, 11u), 0u,
                   "Startup refuses an 11-byte buffer");

    TEST_ASSERT_EQ(tpm2_build_getcap_manufacturer(buf, sizeof(buf)), 22u,
                   "GetCapability command is 22 bytes");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 6), TPM2_CC_GET_CAPABILITY,
                   "GetCapability command code");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 10), TPM2_CAP_TPM_PROPERTIES,
                   "GetCapability capability field");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 14), TPM2_PT_MANUFACTURER,
                   "GetCapability property field");

    /* Response parse: valid, short, lying size field. */
    tpm2_be16_put(buf + 0, 0x8001u);
    tpm2_be32_put(buf + 2, 10u);
    tpm2_be32_put(buf + 6, TPM2_RC_SUCCESS);
    TEST_ASSERT_EQ(tpm2_rsp_parse(buf, 10u, &tag, &size, &rc), 0,
                   "minimal valid response parses");
    TEST_ASSERT_EQ((uint32_t)tag, 0x8001u, "parsed tag");
    TEST_ASSERT_EQ(size, 10u, "parsed size");
    TEST_ASSERT_EQ(rc, TPM2_RC_SUCCESS, "parsed rc");
    TEST_ASSERT_EQ(tpm2_rsp_parse(buf, 9u, 0, 0, 0), -1,
                   "9-byte response rejected");
    tpm2_be32_put(buf + 2, 11u);
    TEST_ASSERT_EQ(tpm2_rsp_parse(buf, 10u, 0, 0, 0), -1,
                   "size field beyond received length rejected");
    tpm2_be32_put(buf + 2, 9u);
    TEST_ASSERT_EQ(tpm2_rsp_parse(buf, 10u, 0, 0, 0), -1,
                   "size field below header size rejected");
    tpm2_be32_put(buf + 2, 10u);
    tpm2_be16_put(buf + 0, 0u);
    TEST_ASSERT_EQ(tpm2_rsp_parse(buf, 10u, 0, 0, 0), -1,
                   "zero tag rejected");
    tpm2_be16_put(buf + 0, 0x1234u);
    TEST_ASSERT_EQ(tpm2_rsp_parse(buf, 10u, 0, 0, 0), -1,
                   "unknown tag rejected");
    tpm2_be16_put(buf + 0, TPM2_ST_RSP_COMMAND);
    TEST_ASSERT_EQ(tpm2_rsp_parse(buf, 10u, 0, 0, 0), 0,
                   "TPM_ST_RSP_COMMAND tag accepted");
    tpm2_be16_put(buf + 0, TPM2_ST_SESSIONS);
    TEST_ASSERT_EQ(tpm2_rsp_parse(buf, 10u, 0, 0, 0), 0,
                   "TPM_ST_SESSIONS tag accepted");
}

/* ---- Fake-transport submit tests ---- */

static void test_tpm2_submit_fake(void)
{
    struct tpm_t_test_state old;
    uint8_t cmd[24], rsp[64];
    uint32_t n, rc;
    int r;

    old = tpm_t_test_install(&fk_io, TPM_T_IFACE_TIS, 1);
    fake_reset();

    /* Argument gates fire before any register traffic. */
    n = tpm2_build_startup(cmd, sizeof(cmd));
    TEST_ASSERT_EQ(tpm2_submit((const uint8_t *)0, n, rsp, sizeof(rsp)),
                   TPM_T_ERR_ARG, "NULL command rejected");
    TEST_ASSERT_EQ(tpm2_submit(cmd, 4u, rsp, sizeof(rsp)),
                   TPM_T_ERR_ARG, "short command rejected");
    cmd[5] = (uint8_t)(cmd[5] + 1u);  /* header size != cmd_len */
    TEST_ASSERT_EQ(tpm2_submit(cmd, n, rsp, sizeof(rsp)),
                   TPM_T_ERR_ARG, "header/length mismatch rejected");
    cmd[5] = (uint8_t)(cmd[5] - 1u);

    /* Happy path at burstCount=1 -- exercises per-byte chunking on
     * both the write and read sides. */
    fake_set_rsp(TPM2_RC_SUCCESS, 10u);
    r = tpm2_submit(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(r, 10, "fake submit returns 10-byte response");
    TEST_ASSERT_EQ(fk_cmd_total, n, "fake received the full command");
    TEST_ASSERT_EQ(tpm2_rsp_parse(rsp, (uint32_t)r, 0, 0, &rc), 0,
                   "fake response parses");
    TEST_ASSERT_EQ(rc, TPM2_RC_SUCCESS, "fake response rc");

    /* Payload-bearing response: 27 bytes with sentinel body bytes --
     * exercises the post-header copy loop at burstCount=1. */
    fake_reset();
    fake_set_rsp(TPM2_RC_SUCCESS, 27u);
    fk_rsp[23] = 0x51u;
    fk_rsp[26] = 0xA7u;
    r = tpm2_submit(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(r, 27, "27-byte response fully read");
    TEST_ASSERT_EQ(rsp[23], 0x51u, "payload sentinel byte 23 intact");
    TEST_ASSERT_EQ(rsp[26], 0xA7u, "payload sentinel byte 26 intact");

    /* Oversized response: header claims more than caller capacity. */
    fake_reset();
    fake_set_rsp(TPM2_RC_SUCCESS, 80u);
    r = tpm2_submit(cmd, n, rsp, 16u);
    TEST_ASSERT_EQ(r, TPM_T_ERR_RESPONSE,
                   "response beyond caller cap rejected");

    /* Header size below the 10-byte minimum. */
    fake_reset();
    fake_set_rsp(TPM2_RC_SUCCESS, 10u);
    tpm2_be32_put(fk_rsp + 2, 6u);
    r = tpm2_submit(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(r, TPM_T_ERR_RESPONSE, "header size below 10 rejected");

    /* Header size above the transport hard cap. */
    fake_reset();
    fake_set_rsp(TPM2_RC_SUCCESS, 10u);
    tpm2_be32_put(fk_rsp + 2, TPM_T_MAX_RESPONSE + 1u);
    r = tpm2_submit(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(r, TPM_T_ERR_RESPONSE,
                   "header size above TPM_T_MAX_RESPONSE rejected");

    /* Protocol violation: TPM never asserts Expect for command bytes. */
    fake_reset();
    fake_set_rsp(TPM2_RC_SUCCESS, 10u);
    fk_expect_mode = 1;
    r = tpm2_submit(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(r, TPM_T_ERR_IO, "missing Expect before final byte -> IO");

    /* Protocol violation: Expect stuck after the final byte. */
    fake_reset();
    fake_set_rsp(TPM2_RC_SUCCESS, 10u);
    fk_expect_mode = 2;
    r = tpm2_submit(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(r, TPM_T_ERR_IO, "stuck Expect after final byte -> IO");

    /* Protocol violation: dataAvail never drops after the declared
     * response length (trailing FIFO bytes). */
    fake_reset();
    fake_set_rsp(TPM2_RC_SUCCESS, 10u);
    fk_stuck_avail = 1;
    r = tpm2_submit(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(r, TPM_T_ERR_IO, "trailing dataAvail -> IO");

    /* Protocol violation: dataAvail drops BEFORE the header-declared
     * response length (truncated response must not read FIFO garbage). */
    fake_reset();
    fake_set_rsp(TPM2_RC_SUCCESS, 27u);
    fk_rsp_len = 15u;  /* device stops serving bytes early */
    r = tpm2_submit(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(r, TPM_T_ERR_IO, "dataAvail drop mid-response -> IO");

    /* Reentrancy: nested submit from inside the transaction. */
    fake_reset();
    fake_set_rsp(TPM2_RC_SUCCESS, 10u);
    fk_reentry_armed = 1;
    r = tpm2_submit(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(r, 10, "outer submit completes despite probe");
    TEST_ASSERT_EQ(fk_reentry_rc, TPM_T_ERR_BUSY,
                   "nested submit bounced with BUSY");

    /* Stale status: burstCount nonzero with stsValid clear must never
     * be trusted -- the ready poll times out instead of writing FIFO
     * bytes off stale state. Reinstall clears the sticky failure. */
    fake_reset();
    fake_set_rsp(TPM2_RC_SUCCESS, 10u);
    fk_no_valid = 1;
    r = tpm2_submit(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(r, TPM_T_ERR_TIMEOUT,
                   "stale status without stsValid -> timeout, no writes");
    TEST_ASSERT_EQ(fk_cmd_len, 0u, "no FIFO bytes written on stale status");
    tpm_t_test_install(&fk_io, TPM_T_IFACE_TIS, 1);

    /* Cumulative budget: a slow device (valid status, delayed bursts)
     * fits inside the per-step waits but must NOT fit inside a small
     * cumulative budget -- mirrors the boot startup-probe cap.
     * The expiry reports BUDGET, not TIMEOUT, and leaves the transport
     * USABLE: a device that answers within every per-command deadline and is
     * merely slower than this boot will wait for is not a wedged one, and
     * poisoning it here would disable every unrelated later TPM operation.
     * The fk_dead block below is the control for the opposite case. */
    fake_reset();
    fake_set_rsp(TPM2_RC_SUCCESS, 10u);
    fk_burst_delay = 16u;
    tpm_t_test_budget_iters(8u);
    r = tpm2_submit(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(r, TPM_T_ERR_BUDGET,
                   "slow device exceeds cumulative budget -> BUDGET");
    TEST_ASSERT_EQ(tpm_transport_available(), 1,
                   "budget expiry leaves the transport available");
    tpm_t_test_budget_iters(0u);
    tpm_t_test_install(&fk_io, TPM_T_IFACE_TIS, 1);
    fake_reset();
    fake_set_rsp(TPM2_RC_SUCCESS, 10u);
    fk_burst_delay = 16u;
    r = tpm2_submit(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(r, 10,
                   "same slow device succeeds without the budget");

    /* Budget expiry with a FAILED abort. The recovery is what decides between
     * "slow device, keep the transport" and "wedged, disable it", and only the
     * success side was covered: here commandReady never comes back, so the
     * abort itself fails and the transport must go sticky exactly as an
     * ordinary wedge does. Iteration-mode budgets make this deterministic --
     * the deadline variant would need real elapsed time. */
    fake_reset();
    fake_set_rsp(TPM2_RC_SUCCESS, 10u);
    fk_burst_delay = 16u;
    fk_abort_refused = 1;
    tpm_t_test_budget_iters(8u);
    r = tpm2_submit(cmd, n, rsp, sizeof(rsp));
    tpm_t_test_budget_iters(0u);
    TEST_ASSERT_EQ(r, TPM_T_ERR_TIMEOUT,
                   "budget expiry whose abort FAILS reports TIMEOUT, not BUDGET");
    TEST_ASSERT_EQ(tpm_transport_available(), 0,
                   "a failed abort after budget expiry sticky-fails the transport");
    r = tpm2_submit(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(r, TPM_T_ERR_FAILED, "and every later command is refused");
    tpm_t_test_install(&fk_io, TPM_T_IFACE_TIS, 1);   /* clear the sticky failure */

    /* Wedge: every wait times out -> sticky failure -> ERR_FAILED. */
    fake_reset();
    fk_dead = 1;
    r = tpm2_submit(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(r, TPM_T_ERR_TIMEOUT, "dead interface times out");
    TEST_ASSERT_EQ(tpm_transport_available(), 0,
                   "transport unavailable after wedge");
    r = tpm2_submit(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(r, TPM_T_ERR_FAILED, "sticky failure after timeout");

    /* Restore: transport returns to the real (uninitialized) state. */
    tpm_t_test_install((const struct tpm_t_io *)0, TPM_T_IFACE_NONE, 0);
    TEST_ASSERT_EQ(tpm_transport_available(), 0,
                   "transport unavailable after test restore");
    r = tpm2_submit(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(r, TPM_T_ERR_NODEV, "submit without device -> NODEV");
    /* Restore the real pre-test transport so a live-fTPM host is not left
     * unavailable for the following suites. */
    tpm_t_test_restore(old);
}

/* Stage a canned GetRandom success response carrying an n-byte
 * TPM2B_DIGEST with varied (non-stuck) payload bytes. */
static void fake_set_rsp_random(uint16_t n)
{
    uint32_t i;
    fake_set_rsp(TPM2_RC_SUCCESS, 10u + 2u + (uint32_t)n);
    tpm2_be16_put(fk_rsp + 10, n);
    for (i = 0; i < n; i++)
        fk_rsp[12u + i] = (uint8_t)(i * 7u + 3u);
}

static void test_tpm2_get_random(void)
{
    uint8_t buf[24], out[64];
    uint8_t rsp[64];
    struct tpm_t_test_state base;
    int r;

    /* Pure marshal/parse helpers. */
    TEST_ASSERT_EQ(tpm2_build_get_random(buf, sizeof(buf), 32u), 12u,
                   "GetRandom command is 12 bytes");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 6), TPM2_CC_GET_RANDOM,
                   "GetRandom command code");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 10), 32u,
                   "GetRandom bytesRequested field");
    TEST_ASSERT_EQ(tpm2_build_get_random(buf, sizeof(buf), 0u), 0u,
                   "GetRandom refuses zero bytesRequested");

    tpm2_be16_put(rsp + 0, 0x8001u);
    tpm2_be32_put(rsp + 2, 10u + 2u + 16u);
    tpm2_be32_put(rsp + 6, TPM2_RC_SUCCESS);
    tpm2_be16_put(rsp + 10, 16u);
    r = tpm2_parse_get_random(rsp, 28u, out, sizeof(out));
    TEST_ASSERT_EQ(r, 16, "valid 16-byte TPM2B parses");
    tpm2_be16_put(rsp + 10, 0u);
    TEST_ASSERT_EQ(tpm2_parse_get_random(rsp, 28u, out, sizeof(out)), -1,
                   "zero-size TPM2B rejected");
    tpm2_be16_put(rsp + 10, 17u);
    TEST_ASSERT_EQ(tpm2_parse_get_random(rsp, 28u, out, sizeof(out)), -1,
                   "TPM2B size beyond response size rejected");
    tpm2_be16_put(rsp + 10, 16u);
    TEST_ASSERT_EQ(tpm2_parse_get_random(rsp, 28u, out, 8u), -1,
                   "TPM2B size beyond out_cap rejected");
    tpm2_be32_put(rsp + 6, TPM2_RC_INITIALIZE);
    TEST_ASSERT_EQ(tpm2_parse_get_random(rsp, 28u, out, sizeof(out)), -1,
                   "nonzero rc rejected");

    /* Bounded loop protocol: 64-byte target with 32-byte canned
     * returns must request 48 (per-call cap) then 32 (remaining). */
    base = tpm_t_test_install(&fk_io, TPM_T_IFACE_TIS, 1);
    fake_reset();
    fake_set_rsp_random(32u);
    fk_vary = 1;
    r = tpm2_get_random_bounded(out, 64u, 100u);
    TEST_ASSERT_EQ(r, 64, "bounded loop collects the full 64 bytes");
    TEST_ASSERT_EQ(fk_rand_req_n, 2u, "bounded loop issued 2 commands");
    TEST_ASSERT_EQ((uint32_t)fk_rand_req[0], 48u,
                   "first request capped at 48 bytes");
    TEST_ASSERT_EQ((uint32_t)fk_rand_req[1], 32u,
                   "second request asks for the remaining 32");

    /* Short yield: one 32-byte response then a dead interface returns
     * the partial count, not an error. */
    tpm_t_test_install(&fk_io, TPM_T_IFACE_TIS, 1);
    fake_reset();
    fake_set_rsp_random(32u);
    fk_one_shot = 1;
    r = tpm2_get_random_bounded(out, 64u, 100u);
    TEST_ASSERT_EQ(r, 32, "short yield returns the partial byte count");

    /* Protocol failure on the FIRST response: bounded helper returns
     * a negative error, never a zero-byte "yield". */
    tpm_t_test_install(&fk_io, TPM_T_IFACE_TIS, 1);
    fake_reset();
    fake_set_rsp_random(32u);
    tpm2_be32_put(fk_rsp + 6, TPM2_RC_INITIALIZE);  /* rc != SUCCESS */
    r = tpm2_get_random_bounded(out, 64u, 100u);
    TEST_ASSERT_EQ(r, TPM_T_ERR_RESPONSE,
                   "failed first GetRandom response -> ERR_RESPONSE");

    /* Budget ownership: a nested bounded call during an in-flight
     * bounded sequence bounces with BUSY (never clobbers the active
     * cumulative budget) and the outer sequence still completes. */
    tpm_t_test_install(&fk_io, TPM_T_IFACE_TIS, 1);
    fake_reset();
    fake_set_rsp_random(32u);
    fk_vary = 1;
    fk_reentry_armed = 2;
    r = tpm2_get_random_bounded(out, 64u, 100u);
    TEST_ASSERT_EQ(r, 64, "outer bounded sequence completes");
    TEST_ASSERT_EQ(fk_reentry_rc, TPM_T_ERR_BUSY,
                   "nested bounded call bounced with BUSY");
    TEST_ASSERT_EQ(fk_budget_after_nested, 1,
                   "outer budget still armed after nested bounce");
    TEST_ASSERT_EQ(tpm_t_test_budget_active(), 0,
                   "budget disarmed once the outer sequence ends");
    tpm_t_test_restore(base);   /* restore live transport, not force-unavailable */
}

static void test_entropy_tpm_collect(void)
{
    uint8_t out[ENTROPY_STAGE_CAP];
    uint32_t len;
    struct tpm_t_test_state base;

    /* Happy path: 2x 32-byte varied GetRandom responses -> one staged
     * 64-byte src-2 record, HIGH credit, report flag set. */
    base = tpm_t_test_install(&fk_io, TPM_T_IFACE_TIS, 1);
    fake_reset();
    fake_set_rsp_random(32u);
    fk_vary = 1;
    entropy_staged_consume_zero();
    tpm_integrity_set_rng_available(0);
    entropy_collect_tpm();
    TEST_ASSERT_EQ((entropy_source_mask() >> ENTROPY_SRC_TPM_RNG) & 1u, 1u,
                   "TPM source mask bit set after collection");
    TEST_ASSERT_EQ(entropy_quality_get(entropy_source_quality(),
                                       ENTROPY_SRC_TPM_RNG),
                   ENTROPY_Q_HIGH, "TPM source credited HIGH");
    len = entropy_staged_drain(out, (uint32_t)sizeof(out));
    TEST_ASSERT_EQ(len, 69u, "staged record is 1+4+64 bytes");
    TEST_ASSERT_EQ(out[0], (uint8_t)ENTROPY_SRC_TPM_RNG,
                   "staged record carries TPM src id");
    TEST_ASSERT_EQ(tpm_integrity_report()->tpm_rng_available, 1u,
                   "integrity report records TPM RNG availability");

    /* Stuck output: identical halves (vary off -> same 32 bytes twice)
     * must be rejected with no staged record. */
    tpm_t_test_install(&fk_io, TPM_T_IFACE_TIS, 1);
    fake_reset();
    fake_set_rsp_random(32u);
    entropy_record_source(ENTROPY_SRC_TPM_RNG, ENTROPY_Q_NONE);
    tpm_integrity_set_rng_available(0);
    entropy_collect_tpm();
    TEST_ASSERT_EQ((entropy_source_mask() >> ENTROPY_SRC_TPM_RNG) & 1u, 0u,
                   "identical-halves output not credited");
    TEST_ASSERT_EQ(entropy_staged_drain(out, (uint32_t)sizeof(out)), 0u,
                   "identical-halves output not staged");

    /* Floor: one 16-byte response then a dead interface -> 16 < 32
     * floor, no credit. */
    tpm_t_test_install(&fk_io, TPM_T_IFACE_TIS, 1);
    fake_reset();
    fake_set_rsp_random(16u);
    fk_one_shot = 1;
    entropy_collect_tpm();
    TEST_ASSERT_EQ((entropy_source_mask() >> ENTROPY_SRC_TPM_RNG) & 1u, 0u,
                   "below-floor yield not credited");
    TEST_ASSERT_EQ(entropy_staged_drain(out, (uint32_t)sizeof(out)), 0u,
                   "below-floor yield not staged");

    /* Exact 32-byte floor: one varied 32-byte response then dead --
     * the minimum valid contribution IS credited. */
    tpm_t_test_install(&fk_io, TPM_T_IFACE_TIS, 1);
    fake_reset();
    fake_set_rsp_random(32u);
    fk_one_shot = 1;
    entropy_record_source(ENTROPY_SRC_TPM_RNG, ENTROPY_Q_NONE);
    tpm_integrity_set_rng_available(0);
    entropy_collect_tpm();
    TEST_ASSERT_EQ((entropy_source_mask() >> ENTROPY_SRC_TPM_RNG) & 1u, 1u,
                   "exactly-32-byte varied yield credited");
    len = entropy_staged_drain(out, (uint32_t)sizeof(out));
    TEST_ASSERT_EQ(len, 37u, "staged floor record is 1+4+32 bytes");

    /* Chunk replay: two identical 16-byte responses reach the 32-byte
     * floor as period-16 data -- the periodicity scan rejects it. */
    tpm_t_test_install(&fk_io, TPM_T_IFACE_TIS, 1);
    fake_reset();
    fake_set_rsp_random(16u);  /* same canned 16 bytes every call */
    entropy_record_source(ENTROPY_SRC_TPM_RNG, ENTROPY_Q_NONE);
    tpm_integrity_set_rng_available(0);
    entropy_collect_tpm();
    TEST_ASSERT_EQ((entropy_source_mask() >> ENTROPY_SRC_TPM_RNG) & 1u, 0u,
                   "replayed 16-byte chunks not credited");
    TEST_ASSERT_EQ(entropy_staged_drain(out, (uint32_t)sizeof(out)), 0u,
                   "replayed 16-byte chunks not staged");

    /* Exact 32-byte all-identical sample: rejected by the heuristic
     * even though it meets the floor (identical-halves needs 64). */
    tpm_t_test_install(&fk_io, TPM_T_IFACE_TIS, 1);
    fake_reset();
    fake_set_rsp_random(32u);
    {
        uint32_t k;
        for (k = 0; k < 32u; k++)
            fk_rsp[12u + k] = 0x5Au;
    }
    fk_one_shot = 1;
    entropy_record_source(ENTROPY_SRC_TPM_RNG, ENTROPY_Q_NONE);
    tpm_integrity_set_rng_available(0);
    entropy_collect_tpm();
    TEST_ASSERT_EQ((entropy_source_mask() >> ENTROPY_SRC_TPM_RNG) & 1u, 0u,
                   "32-byte constant-fill output not credited");
    TEST_ASSERT_EQ(entropy_staged_drain(out, (uint32_t)sizeof(out)), 0u,
                   "32-byte constant-fill output not staged");
    TEST_ASSERT_EQ(tpm_integrity_report()->tpm_rng_available, 0u,
                   "report flag stays clear on rejected output");

    /* No transport: collector is a silent no-op. */
    tpm_t_test_install((const struct tpm_t_io *)0, TPM_T_IFACE_NONE, 0);
    entropy_collect_tpm();
    TEST_ASSERT_EQ(entropy_staged_drain(out, (uint32_t)sizeof(out)), 0u,
                   "no transport stages nothing");

    /* Restore globals the collector touched. */
    entropy_record_source(ENTROPY_SRC_TPM_RNG, ENTROPY_Q_NONE);
    tpm_integrity_set_rng_available(0);
    entropy_staged_consume_zero();
    /* Restore the real pre-test transport (the no-op subcase above forced it
     * unavailable) so a live-fTPM host survives the suite. */
    tpm_t_test_restore(base);
}

static void test_tpm2_pcr_read(void)
{
    uint8_t buf[32], out[64], rsp[80];
    uint32_t i;
    int r;

    /* Marshal: SHA-256, PCR 7. */
    TEST_ASSERT_EQ(tpm2_build_pcr_read(buf, sizeof(buf), TPM_ALG_SHA256, 7u), 20u,
                   "PCR_Read command is 20 bytes");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 6), TPM2_CC_PCR_READ, "PCR_Read command code");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 10), 1u, "single pcrSelection");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 14), TPM_ALG_SHA256, "hashAlg = SHA-256");
    TEST_ASSERT_EQ(buf[16], 3u, "sizeofSelect = 3");
    TEST_ASSERT_EQ(buf[17], 0x80u, "PCR 7 bit set (1<<7) in pcrSelect[0]");
    TEST_ASSERT_EQ(tpm2_build_pcr_read(buf, sizeof(buf), TPM_ALG_SHA256, 24u), 0u,
                   "PCR index > 23 refused");

    /* Response: SHA-256 PCR 7 active, one 32-byte digest. The parser binds the
     * reply to the requested (alg, pcr_index); this fixture echoes them. */
    tpm2_be16_put(rsp + 0, 0x8001u);
    tpm2_be32_put(rsp + 6, TPM2_RC_SUCCESS);
    tpm2_be32_put(rsp + 10, 0u);              /* updateCounter */
    tpm2_be32_put(rsp + 14, 1u);              /* pcrSelection count */
    tpm2_be16_put(rsp + 18, TPM_ALG_SHA256);  /* hashAlg */
    rsp[20] = 3u;                             /* sizeofSelect */
    rsp[21] = 0x80u; rsp[22] = 0u; rsp[23] = 0u;  /* PCR 7 bit set */
    tpm2_be32_put(rsp + 24, 1u);              /* digest count */
    tpm2_be16_put(rsp + 28, 32u);             /* digest size */
    for (i = 0; i < 32u; i++) rsp[30 + i] = (uint8_t)(0xA0u + i);
    tpm2_be32_put(rsp + 2, 62u);              /* response size */
    r = tpm2_parse_pcr_read(rsp, 62u, TPM_ALG_SHA256, 7u, out, sizeof(out));
    TEST_ASSERT_EQ(r, 32, "active bank: 32-byte digest parsed");
    TEST_ASSERT_EQ(out[0], 0xA0u, "digest byte 0");
    TEST_ASSERT_EQ(out[31], (uint8_t)(0xA0u + 31u), "digest byte 31");

    /* Binding: a response echoing the WRONG hash bank must be rejected so the
     * digest is never cached under the requested (alg, pcr). */
    tpm2_be16_put(rsp + 18, TPM_ALG_SHA1);
    TEST_ASSERT_EQ(tpm2_parse_pcr_read(rsp, 62u, TPM_ALG_SHA256, 7u, out, sizeof(out)),
                   -1, "echoed wrong hashAlg rejected");
    tpm2_be16_put(rsp + 18, TPM_ALG_SHA256);

    /* Binding: requested PCR bit clear but a digest present -> desync -> -1. */
    rsp[21] = 0x40u;                          /* PCR 6 bit, not the requested 7 */
    TEST_ASSERT_EQ(tpm2_parse_pcr_read(rsp, 62u, TPM_ALG_SHA256, 7u, out, sizeof(out)),
                   -1, "digest for an unrequested PCR rejected");
    rsp[21] = 0x80u;

    /* Binding: more than one echoed selection rejected. */
    tpm2_be32_put(rsp + 14, 2u);
    TEST_ASSERT_EQ(tpm2_parse_pcr_read(rsp, 62u, TPM_ALG_SHA256, 7u, out, sizeof(out)),
                   -1, "multi-selection response rejected");
    tpm2_be32_put(rsp + 14, 1u);

    /* Binding: digest size != the bank's digest length rejected (SHA-256=32). */
    tpm2_be16_put(rsp + 28, 20u);
    TEST_ASSERT_EQ(tpm2_parse_pcr_read(rsp, 50u, TPM_ALG_SHA256, 7u, out, sizeof(out)),
                   -1, "wrong digest size for bank rejected");
    tpm2_be16_put(rsp + 28, 32u);

    /* Inactive bank: requested PCR bit clear AND digest count 0 -> 0, no error. */
    rsp[21] = 0u;
    tpm2_be32_put(rsp + 24, 0u);
    tpm2_be32_put(rsp + 2, 28u);
    TEST_ASSERT_EQ(tpm2_parse_pcr_read(rsp, 28u, TPM_ALG_SHA256, 7u, out, sizeof(out)),
                   0, "inactive bank reports 0 (not error)");
    rsp[21] = 0x80u;

    /* Malformed: digest size beyond response. */
    tpm2_be32_put(rsp + 24, 1u);
    tpm2_be16_put(rsp + 28, 33u);
    tpm2_be32_put(rsp + 2, 62u);
    TEST_ASSERT_EQ(tpm2_parse_pcr_read(rsp, 62u, TPM_ALG_SHA256, 7u, out, sizeof(out)),
                   -1, "digest size beyond response rejected");

    /* Undersized out_cap (need 32, give 16) rejected. */
    tpm2_be16_put(rsp + 28, 32u);
    TEST_ASSERT_EQ(tpm2_parse_pcr_read(rsp, 62u, TPM_ALG_SHA256, 7u, out, 16u),
                   -1, "undersized out_cap rejected");

    /* Nonzero rc rejected. */
    tpm2_be32_put(rsp + 6, TPM2_RC_INITIALIZE);
    TEST_ASSERT_EQ(tpm2_parse_pcr_read(rsp, 62u, TPM_ALG_SHA256, 7u, out, sizeof(out)),
                   -1, "nonzero rc rejected");
}

/* ---- Fake-CRB transaction seam (TODO-13 TPM tests + event-log fixtures) ----
 * The existing TIS io fake covers the FIFO interface only. crb_submit() drives
 * the CRB control-area REG_REQ/REG_START registers through the same io-ops table
 * and copies command/response bytes through the mapped s_crb_cmd/s_crb_rsp
 * buffers. This fake supplies both: an io that answers the ready/start handshake
 * plus test buffers installed via tpm_t_test_install_crb_buffers(). */

#define CF_REG_REQ   0x00u
#define CF_REG_START 0x0Cu
#define CF_REQ_CMD_READY 0x01u
#define CF_REQ_GO_IDLE   0x02u
#define CF_START_START   0x01u

static volatile uint8_t cf_cmd[64];
static volatile uint8_t cf_rsp[64];
static int      cf_executed;
static int      cf_stuck_start;   /* START never clears -> timeout */
static int      cf_ready;         /* set by a CMD_READY write to REG_REQ */
static int      cf_goidle;        /* set by a GO_IDLE write to REG_REQ */
static uint32_t cf_rsp_decl_size; /* size field the fake writes into the response */
static int      cf_cancelled;     /* a CANCEL write was observed */
static int      cf_cancel_level;  /* CANCEL modelled as a LEVEL, not an edge: a
                                   * still-asserted CANCEL blocks the next
                                   * command on real hardware, so the fake makes
                                   * readiness depend on it being cleared */
static int      cf_cancel_clears; /* CANCEL releases a stuck START (cancel works) */

static void cf_build_response(void)
{
    uint32_t i;
    for (i = 0; i < sizeof cf_rsp; i++) cf_rsp[i] = 0;
    /* ST_NO_SESSIONS success; the response's declared size drives crb_submit's
     * bounds check (the buffer itself is 64 bytes). */
    cf_rsp[0] = 0x80u; cf_rsp[1] = 0x01u;            /* tag ST_NO_SESSIONS */
    cf_rsp[2] = (uint8_t)(cf_rsp_decl_size >> 24);
    cf_rsp[3] = (uint8_t)(cf_rsp_decl_size >> 16);
    cf_rsp[4] = (uint8_t)(cf_rsp_decl_size >> 8);
    cf_rsp[5] = (uint8_t)cf_rsp_decl_size;            /* size field */
    /* rc=SUCCESS (bytes 6..9 zero); 2 payload bytes for the 12-byte happy case. */
    cf_rsp[10] = 0xA5u; cf_rsp[11] = 0x5Au;
}

static uint8_t cf_r8(uint32_t off) { (void)off; return 0; }
static void    cf_w8(uint32_t off, uint8_t v) { (void)off; (void)v; }

static uint32_t cf_r32(uint32_t off)
{
    if (off == CF_REG_REQ)
        /* cmdReady reads as SET (not ready) until crb_submit requests it -- so a
         * regression that drops the CMD_READY write times out instead of passing.
         * A CANCEL left asserted also keeps the interface unready, which is how
         * hardware behaves and what makes the deassert write load-bearing. */
        return (cf_ready && !cf_cancel_level) ? 0u : CF_REQ_CMD_READY;
    if (off == CF_REG_START)
        return (cf_stuck_start || !cf_executed) ? CF_START_START : 0u;
    return 0u;
}

#define CF_REG_CANCEL 0x08u

static void cf_w32(uint32_t off, uint32_t v)
{
    if (off == CF_REG_CANCEL) {
        cf_cancel_level = (v != 0u) ? 1 : 0;
        if (v) {
            cf_cancelled = 1;
            if (cf_cancel_clears)
                cf_stuck_start = 0;    /* the device honored the cancel */
        }
    }
    if (off == CF_REG_REQ) {
        if (v & CF_REQ_CMD_READY) cf_ready = 1;   /* ready granted on request */
        if (v & CF_REQ_GO_IDLE)   cf_goidle = 1;  /* goIdle issued */
    }
    if (off == CF_REG_START && (v & CF_START_START)) {
        cf_build_response();       /* "execute": command already in cf_cmd */
        cf_executed = 1;           /* START clears (unless cf_stuck_start) */
    }
}

static const struct tpm_t_io cf_io = { cf_r8, cf_w8, cf_r32, cf_w32 };

static void cf_reset(void)
{
    cf_executed = 0;
    cf_stuck_start = 0;
    cf_ready = 0;
    cf_goidle = 0;
    cf_rsp_decl_size = 12u;        /* header(10) + 2 payload bytes */
    cf_cancelled = 0;
    cf_cancel_level = 0;
    cf_cancel_clears = 0;
}

static void test_tpm_crb_submit_fake(void)
{
    struct tpm_t_test_state prev;
    struct tpm_t_crb_snapshot crb_snap;
    uint8_t cmd[16], rsp[64];
    int r;

    /* A well-formed 12-byte command (crb_submit copies it verbatim). */
    memset(cmd, 0, sizeof cmd);
    cmd[0] = 0x80u; cmd[1] = 0x01u;               /* tag */
    tpm2_be32_put(cmd + 2, 12u);                  /* size == cmd_len */
    tpm2_be32_put(cmd + 6, TPM2_CC_GET_RANDOM);
    cmd[10] = 0; cmd[11] = 1;

    /* Happy path: ready -> start -> response copied out. */
    cf_reset();
    prev = tpm_t_test_install(&cf_io, TPM_T_IFACE_CRB, 1);
    /* Snapshot the real CRB mapping (if any) so teardown restores it instead of
     * clearing to NULL -- a real fTPM boot must survive the test build's suite. */
    crb_snap = tpm_t_test_install_crb_buffers(cf_cmd, sizeof cf_cmd, cf_rsp, sizeof cf_rsp);
    r = tpm2_submit(cmd, 12u, rsp, sizeof rsp);
    TEST_ASSERT_EQ(r, 12, "CRB submit returns the 12-byte response");
    TEST_ASSERT(rsp[0] == 0x80u && rsp[1] == 0x01u, "CRB response tag copied");
    TEST_ASSERT(rsp[10] == 0xA5u && rsp[11] == 0x5Au, "CRB response payload copied");
    TEST_ASSERT(cf_cmd[6] == (uint8_t)(TPM2_CC_GET_RANDOM >> 24), "command reached CRB cmd buffer");
    TEST_ASSERT(cf_ready == 1, "crb_submit requested cmdReady (REG_REQ handshake)");
    TEST_ASSERT(cf_goidle == 1, "crb_submit returned to idle (goIdle) after success");

    /* Budget expiry on the CRB path, which had NO recovery coverage at all --
     * the quiesce there is a different sequence (CANCEL, wait for START to
     * clear, clear CANCEL, GO_IDLE) and could have been broken in either
     * direction without a test noticing. START stays asserted so the poll burns
     * the budget; the CANCEL is honored, so recovery succeeds and the transport
     * survives as a merely-slow device. */
    cf_reset();
    cf_stuck_start = 1;
    cf_cancel_clears = 1;
    tpm_t_test_budget_iters(8u);
    r = tpm2_submit(cmd, 12u, rsp, sizeof rsp);
    tpm_t_test_budget_iters(0u);
    TEST_ASSERT_EQ(r, TPM_T_ERR_BUDGET, "CRB budget expiry reports BUDGET");
    TEST_ASSERT_EQ(cf_cancelled, 1, "CRB recovery issued CANCEL");
    TEST_ASSERT_EQ(cf_goidle, 1, "CRB recovery returned the interface to idle");
    TEST_ASSERT_EQ(tpm_transport_available(), 1,
                   "a recovered CRB cancel leaves the transport usable");
    TEST_ASSERT_EQ(cf_cancel_level, 0,
                   "CRB recovery DEASSERTED cancel (a latched cancel blocks the bus)");
    /* Reuse WITHOUT resetting the fake: "available" is a claim about the
     * interface, and the only thing that proves it is the next command working
     * on the state recovery actually left behind. */
    cf_executed = 0;
    r = tpm2_submit(cmd, 12u, rsp, sizeof rsp);
    TEST_ASSERT_EQ(r, 12, "the next CRB command runs on the recovered interface");

    /* And when the device ignores the CANCEL, that IS a wedge. */
    cf_reset();
    cf_stuck_start = 1;
    cf_cancel_clears = 0;
    tpm_t_test_budget_iters(8u);
    r = tpm2_submit(cmd, 12u, rsp, sizeof rsp);
    tpm_t_test_budget_iters(0u);
    TEST_ASSERT_EQ(r, TPM_T_ERR_TIMEOUT, "an ignored CRB CANCEL reports TIMEOUT");
    TEST_ASSERT_EQ(cf_cancelled, 1, "CANCEL was still attempted");
    TEST_ASSERT_EQ(tpm_transport_available(), 0,
                   "an ignored CRB CANCEL sticky-fails the transport");
    tpm_t_test_install(&cf_io, TPM_T_IFACE_CRB, 1);   /* clear the sticky failure */

    /* Oversized declared response (> rsp_cap and > buffer) rejected. */
    cf_reset();
    cf_rsp_decl_size = 5000u;
    r = tpm2_submit(cmd, 12u, rsp, sizeof rsp);
    TEST_ASSERT_EQ(r, TPM_T_ERR_RESPONSE, "oversized CRB response -> ERR_RESPONSE");

    /* Response smaller than the 10-byte header rejected. */
    cf_reset();
    cf_rsp_decl_size = 4u;
    r = tpm2_submit(cmd, 12u, rsp, sizeof rsp);
    TEST_ASSERT_EQ(r, TPM_T_ERR_RESPONSE, "sub-header CRB response -> ERR_RESPONSE");

    /* A command larger than the CRB command buffer is refused before submit. */
    cf_reset();
    {
        uint8_t big[40];
        memset(big, 0, sizeof big);
        big[0] = 0x80u; big[1] = 0x01u;
        tpm2_be32_put(big + 2, 40u);
        tpm2_be32_put(big + 6, TPM2_CC_GET_RANDOM);
        (void)tpm_t_test_install_crb_buffers(cf_cmd, 16u, cf_rsp, sizeof cf_rsp);
        r = tpm2_submit(big, 40u, rsp, sizeof rsp);
        TEST_ASSERT_EQ(r, TPM_T_ERR_ARG, "command exceeding CRB cmd buffer -> ERR_ARG");
        (void)tpm_t_test_install_crb_buffers(cf_cmd, sizeof cf_cmd, cf_rsp, sizeof cf_rsp);
    }

    /* START bit never clears -> bounded timeout, no hang. Last: a TIMEOUT marks
     * the transport sticky-failed, which a reinstall (teardown below) clears. */
    cf_reset();
    cf_stuck_start = 1;
    r = tpm2_submit(cmd, 12u, rsp, sizeof rsp);
    TEST_ASSERT_EQ(r, TPM_T_ERR_TIMEOUT, "stuck CRB START -> ERR_TIMEOUT (bounded)");

    tpm_t_test_restore_crb_buffers(crb_snap);   /* restore real CRB mapping, not NULL */
    tpm_t_test_restore(prev);
}

/* ---- Degraded: transport-level no-TPM ---- */

static void test_tpm_transport_no_tpm(void)
{
    struct tpm_t_test_state prev;
    uint8_t cmd[16], rsp[32];
    int r;
    memset(cmd, 0, sizeof cmd);
    cmd[0] = 0x80u; cmd[1] = 0x01u;
    tpm2_be32_put(cmd + 2, 12u);
    tpm2_be32_put(cmd + 6, TPM2_CC_GET_RANDOM);

    /* io == NULL restores the unavailable pre-init state: a submit must report
     * ERR_NODEV (no transport), not hang or fault. */
    prev = tpm_t_test_install(0, TPM_T_IFACE_NONE, 0);
    TEST_ASSERT_EQ(tpm_transport_available(), 0, "transport reports unavailable");
    r = tpm2_submit(cmd, 12u, rsp, sizeof rsp);
    TEST_ASSERT_EQ(r, TPM_T_ERR_NODEV, "submit with no transport -> ERR_NODEV");
    tpm_t_test_restore(prev);
}

/* The test seam restores the FULL prior state, not just the io pointer. The old
 * io-pointer-only restore re-installed via (prev, NONE, 0), which forced
 * available=1 -- leaving the transport mis-routed after a TPM suite. */
static void test_tpm_t_restore_full_state(void)
{
    struct tpm_t_test_state base, snap;

    /* Baseline: force unavailable and remember the real pre-test state. */
    base = tpm_t_test_install((const struct tpm_t_io *)0, TPM_T_IFACE_NONE, 0);
    TEST_ASSERT_EQ(tpm_transport_available(), 0, "baseline transport unavailable");

    /* Install a fake (available=1), then restore the captured snapshot: the
     * transport MUST return to unavailable, not stay available. */
    snap = tpm_t_test_install(&fk_io, TPM_T_IFACE_TIS, 1);
    TEST_ASSERT_EQ(tpm_transport_available(), 1, "fake install marks available");
    tpm_t_test_restore(snap);
    TEST_ASSERT_EQ(tpm_transport_available(), 0,
                   "restore returns to unavailable (not stuck available)");

    /* Leave the original pre-test state intact for sibling suites. */
    tpm_t_test_restore(base);
}

void test_register_tpm_transport(void)
{
    test_suite_register_cat("tpm: transport marshaling",
        test_tpm2_marshal, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: test-seam full-state restore",
        test_tpm_t_restore_full_state, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: CRB submit (fake CRB)",
        test_tpm_crb_submit_fake, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: transport no-TPM degrade",
        test_tpm_transport_no_tpm, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: transport submit (fake TIS)",
        test_tpm2_submit_fake, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: GetRandom marshal + parse",
        test_tpm2_get_random, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: PCR_Read marshal + parse",
        test_tpm2_pcr_read, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: RNG entropy collection (fake TIS)",
        test_entropy_tpm_collect, TEST_CAT_SECURITY);
}
