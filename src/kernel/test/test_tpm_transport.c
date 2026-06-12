/* ============================================================================
 * test_tpm_transport.c -- TPM2 command transport tests
 *
 * Pure-helper tests (marshaling, response parsing) plus fake-register
 * transport tests through the tpm_t_test_install() io seam: happy
 * path with burst=1 chunking, oversized response, timeout -> sticky
 * fail, busy reentrancy. No MMIO, no live boot infrastructure.
 * ============================================================================ */

#include "kernel/test/test.h"
#include "kernel/tpm_transport.h"

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
static int      fk_no_valid;      /* status reads lack stsValid (stale) */
static uint32_t fk_burst_delay;   /* STS reads reporting burst=0 first */
static int      fk_expect_mode;   /* 0 normal, 1 never assert, 2 stuck on */
static int      fk_stuck_avail;   /* dataAvail never drops (trailing bytes) */
static int      fk_reentry_rc;    /* captured nested-submit rc */
static int      fk_reentry_armed;

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
        /* Reentrancy probe: a nested submit during an in-flight
         * transaction must bounce with TPM_T_ERR_BUSY. */
        uint8_t cmd[12], rsp[16];
        uint32_t n = tpm2_build_startup(cmd, sizeof(cmd));
        fk_reentry_armed = 0;
        fk_reentry_rc = tpm2_submit(cmd, n, rsp, sizeof(rsp));
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
    if (fk_ready && !fk_executed && fk_cmd_len == 0)
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
    }
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
    fk_no_valid = 0;
    fk_burst_delay = 0;
    fk_expect_mode = 0;
    fk_stuck_avail = 0;
    fk_reentry_rc = 0;
    fk_reentry_armed = 0;
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
    const struct tpm_t_io *old;
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
     * cumulative budget -- mirrors the boot startup-probe cap. */
    fake_reset();
    fake_set_rsp(TPM2_RC_SUCCESS, 10u);
    fk_burst_delay = 16u;
    tpm_t_test_budget_iters(8u);
    r = tpm2_submit(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(r, TPM_T_ERR_TIMEOUT,
                   "slow device exceeds cumulative budget -> timeout");
    tpm_t_test_budget_iters(0u);
    tpm_t_test_install(&fk_io, TPM_T_IFACE_TIS, 1);
    fake_reset();
    fake_set_rsp(TPM2_RC_SUCCESS, 10u);
    fk_burst_delay = 16u;
    r = tpm2_submit(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(r, 10,
                   "same slow device succeeds without the budget");

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
    (void)old;
}

void test_register_tpm_transport(void)
{
    test_suite_register_cat("tpm: transport marshaling",
        test_tpm2_marshal, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: transport submit (fake TIS)",
        test_tpm2_submit_fake, TEST_CAT_SECURITY);
}
