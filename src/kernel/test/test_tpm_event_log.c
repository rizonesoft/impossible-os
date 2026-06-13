/* test_tpm_event_log.c -- unit tests for the pure TCG event-log parser
 * (tpm_evlog_parse). The parser is side-effect-free (no globals, no klog), so
 * these tests build TCG_PCR_EVENT / TCG_PCR_EVENT2 byte fixtures in a local
 * buffer and assert the parsed metadata + structured status directly. No live
 * boot infrastructure is touched (test policy).
 *
 * XREF: 01-boot-platform/TODO-13-tpm-measured-boot-attestation.md "Harden TCG
 * Event-Log Parser".
 */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/tpm.h"

/* ---- TCG log fixture builder ---- */
static uint8_t  s_buf[1024];
static uint32_t s_len;

static void b_reset(void) { s_len = 0; }
static void b_u8(uint8_t v)  { if (s_len < sizeof(s_buf)) s_buf[s_len++] = v; }
static void b_u16(uint16_t v){ b_u8((uint8_t)v); b_u8((uint8_t)(v >> 8)); }
static void b_u32(uint32_t v){ b_u8((uint8_t)v); b_u8((uint8_t)(v >> 8));
                               b_u8((uint8_t)(v >> 16)); b_u8((uint8_t)(v >> 24)); }
static void b_zeros(uint32_t n){ uint32_t i; for (i = 0; i < n; i++) b_u8(0); }

/* TCG_PCR_EVENT: pcr(4) type(4) digest[20] data_size(4) data[]. */
static void ev_pcr(uint32_t pcr, uint32_t type, uint32_t dsize)
{
    b_u32(pcr); b_u32(type); b_zeros(20); b_u32(dsize); b_zeros(dsize);
}

/* TCG_PCR_EVENT2 with one digest of algorithm `alg`/`dsz`:
 * pcr(4) type(4) count(4) alg(2) digest[dsz] data_size(4) data[]. */
static void ev2_one(uint32_t pcr, uint32_t type, uint16_t alg, uint32_t dsz,
                    uint32_t dsize)
{
    b_u32(pcr); b_u32(type); b_u32(1); b_u16(alg); b_zeros(dsz);
    b_u32(dsize); b_zeros(dsize);
}

#define ALG_SHA256 0x000Bu

/* TPM 1.2: two TCG_PCR_EVENT entries parse cleanly with correct metadata. */
static void test_tpm_evlog_tpm12_two_events(void)
{
    b_reset();
    ev_pcr(0u, 3u /*EV_NO_ACTION*/, 0u);   /* event 0: 32 bytes */
    ev_pcr(4u, 0x0Du, 4u);                 /* event 1: 32 + 4 = 36 bytes */

    struct tpm_event ev[8];
    uint32_t count = 0, overflow = 0, fail = 0;
    tpm_evlog_status_t st = tpm_evlog_parse(s_buf, s_len, 1, ev, 8,
                                            &count, &overflow, &fail);
    TEST_ASSERT_EQ(st, TPM_EVLOG_OK, "TPM 1.2 log parses clean");
    TEST_ASSERT_EQ(count, 2u, "two events recorded");
    TEST_ASSERT_EQ(overflow, 0u, "no overflow");
    TEST_ASSERT_EQ(ev[0].pcr_index, 0u, "event 0 pcr=0");
    TEST_ASSERT_EQ(ev[1].pcr_index, 4u, "event 1 pcr=4");
    TEST_ASSERT_EQ(ev[1].payload_size, 4u, "event 1 payload size");
    TEST_ASSERT_EQ(ev[1].payload_off, 64u, "event 1 payload offset (32+32)");
}

/* TPM 2.0: spec-ID first event + one EVENT2 record; primary digest metadata. */
static void test_tpm_evlog_tpm20_event2(void)
{
    b_reset();
    ev_pcr(0u, 3u /*EV_NO_ACTION*/, 20u);             /* event 0: 32 + 20 = 52 */
    ev2_one(4u, 0x80000003u, ALG_SHA256, 32u, 0u);    /* event 1 at offset 52 */

    struct tpm_event ev[8];
    uint32_t count = 0, overflow = 0, fail = 0;
    tpm_evlog_status_t st = tpm_evlog_parse(s_buf, s_len, 2, ev, 8,
                                            &count, &overflow, &fail);
    TEST_ASSERT_EQ(st, TPM_EVLOG_OK, "TPM 2.0 crypto-agile log parses clean");
    TEST_ASSERT_EQ(count, 2u, "two events recorded");
    TEST_ASSERT_EQ(ev[1].pcr_index, 4u, "EVENT2 pcr=4");
    TEST_ASSERT_EQ(ev[1].primary_alg_id, ALG_SHA256, "EVENT2 primary alg SHA-256");
    TEST_ASSERT_EQ(ev[1].primary_digest_len, 32u, "EVENT2 digest len 32");
    /* digest bytes start after pcr(4)+type(4)+count(4)+alg(2) within event 1
     * at offset 52: 52 + 14 = 66. */
    TEST_ASSERT_EQ(ev[1].primary_digest_off, 66u, "EVENT2 primary digest offset");
}

/* A final EVENT2 whose declared data_size overruns the buffer is rejected as
 * TRUNCATED with the failing offset at the data_size field. */
static void test_tpm_evlog_truncated(void)
{
    b_reset();
    ev_pcr(0u, 3u, 20u);                              /* event 0: 52 bytes */
    /* event 1 header, but claim a data_size far past the buffer. */
    b_u32(4u); b_u32(0x80000003u); b_u32(1); b_u16(ALG_SHA256); b_zeros(32);
    b_u32(1000u);    /* data_size = 1000, buffer ends right here */

    struct tpm_event ev[8];
    uint32_t count = 0, overflow = 0, fail = 0;
    tpm_evlog_status_t st = tpm_evlog_parse(s_buf, s_len, 2, ev, 8,
                                            &count, &overflow, &fail);
    TEST_ASSERT_EQ(st, TPM_EVLOG_TRUNCATED, "oversize final event -> TRUNCATED");
    /* data_size field is at event-1 offset 52 + event2_header(8+4+2+32=46) = 98. */
    TEST_ASSERT_EQ(fail, 98u, "fail offset points at the data_size field");
}

/* More events than the output cap -> CAP_EXCEEDED + overflow flag. */
static void test_tpm_evlog_cap_exceeded(void)
{
    b_reset();
    ev_pcr(0u, 3u, 0u);   /* event 0 */
    ev_pcr(1u, 1u, 0u);   /* event 1 */
    ev_pcr(2u, 1u, 0u);   /* event 2 */

    struct tpm_event ev[2];
    uint32_t count = 0, overflow = 0, fail = 0;
    tpm_evlog_status_t st = tpm_evlog_parse(s_buf, s_len, 1, ev, 2,
                                            &count, &overflow, &fail);
    TEST_ASSERT_EQ(st, TPM_EVLOG_CAP_EXCEEDED, "3 events into cap 2 -> CAP_EXCEEDED");
    TEST_ASSERT_EQ(overflow, 1u, "overflow flag set");
}

/* An EVENT2 digest bank with an unknown algorithm id cannot be sized and is
 * rejected as UNSUPPORTED_ALG rather than guessed. */
static void test_tpm_evlog_unsupported_alg(void)
{
    b_reset();
    ev_pcr(0u, 3u, 20u);                          /* event 0 */
    ev2_one(4u, 0x80000003u, 0x0099u, 32u, 0u);   /* unknown alg 0x0099 */

    struct tpm_event ev[8];
    uint32_t count = 0, overflow = 0, fail = 0;
    tpm_evlog_status_t st = tpm_evlog_parse(s_buf, s_len, 2, ev, 8,
                                            &count, &overflow, &fail);
    TEST_ASSERT_EQ(st, TPM_EVLOG_UNSUPPORTED_ALG, "unknown alg -> UNSUPPORTED_ALG");
}

/* A buffer smaller than a TCG_PCR_EVENT header is BAD_HEADER. */
static void test_tpm_evlog_bad_header(void)
{
    b_reset();
    b_zeros(8);   /* 8 bytes < 32-byte header */

    struct tpm_event ev[4];
    uint32_t count = 0, overflow = 0, fail = 0;
    tpm_evlog_status_t st = tpm_evlog_parse(s_buf, s_len, 2, ev, 4,
                                            &count, &overflow, &fail);
    TEST_ASSERT_EQ(st, TPM_EVLOG_BAD_HEADER, "short buffer -> BAD_HEADER");
    TEST_ASSERT_EQ(count, 0u, "no events on bad header");
}

/* A valid prefix followed by a sub-header trailing remnant is TRUNCATED, not a
 * shorter clean log (TPM 1.2). */
static void test_tpm_evlog_partial_tail_tpm12(void)
{
    b_reset();
    ev_pcr(0u, 3u, 0u);    /* one clean event: 32 bytes */
    b_zeros(5u);           /* 5 dangling bytes (< 32-byte header) */

    struct tpm_event ev[8];
    uint32_t count = 0, overflow = 0, fail = 0;
    tpm_evlog_status_t st = tpm_evlog_parse(s_buf, s_len, 1, ev, 8,
                                            &count, &overflow, &fail);
    TEST_ASSERT_EQ(st, TPM_EVLOG_TRUNCATED, "TPM 1.2 partial tail -> TRUNCATED");
    TEST_ASSERT_EQ(fail, 32u, "fail offset at the dangling remnant");
}

/* Sub-header trailing remnant on a TPM 2.0 log is TRUNCATED. */
static void test_tpm_evlog_partial_tail_tpm20(void)
{
    b_reset();
    ev_pcr(0u, 3u, 20u);   /* spec-id first event: 52 bytes */
    b_zeros(5u);           /* 5 dangling bytes (< 12-byte EVENT2 header) */

    struct tpm_event ev[8];
    uint32_t count = 0, overflow = 0, fail = 0;
    tpm_evlog_status_t st = tpm_evlog_parse(s_buf, s_len, 2, ev, 8,
                                            &count, &overflow, &fail);
    TEST_ASSERT_EQ(st, TPM_EVLOG_TRUNCATED, "TPM 2.0 partial tail -> TRUNCATED");
    TEST_ASSERT_EQ(fail, 52u, "fail offset at the dangling remnant");
}

/* Count-only: out == NULL counts events without storing and without faulting. */
static void test_tpm_evlog_count_only_null_out(void)
{
    b_reset();
    ev_pcr(0u, 3u, 0u);
    ev_pcr(1u, 1u, 0u);

    uint32_t count = 0, overflow = 0, fail = 0;
    tpm_evlog_status_t st = tpm_evlog_parse(s_buf, s_len, 1,
                                            (struct tpm_event *)0, 0u,
                                            &count, &overflow, &fail);
    TEST_ASSERT_EQ(st, TPM_EVLOG_OK, "count-only (NULL out) parses clean");
    TEST_ASSERT_EQ(count, 2u, "count-only reports two events");
    TEST_ASSERT_EQ(overflow, 0u, "count-only is not an overflow");
}

void test_register_tpm_event_log(void)
{
    test_suite_register_cat("tpm-evlog: TPM 1.2 two events",
                            test_tpm_evlog_tpm12_two_events, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm-evlog: TPM 2.0 EVENT2 metadata",
                            test_tpm_evlog_tpm20_event2, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm-evlog: oversize final event truncated",
                            test_tpm_evlog_truncated, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm-evlog: cap exceeded",
                            test_tpm_evlog_cap_exceeded, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm-evlog: unsupported algorithm",
                            test_tpm_evlog_unsupported_alg, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm-evlog: bad header",
                            test_tpm_evlog_bad_header, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm-evlog: TPM 1.2 partial tail truncated",
                            test_tpm_evlog_partial_tail_tpm12, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm-evlog: TPM 2.0 partial tail truncated",
                            test_tpm_evlog_partial_tail_tpm20, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm-evlog: count-only NULL out",
                            test_tpm_evlog_count_only_null_out, TEST_CAT_SECURITY);
}

#endif /* KERNEL_TESTS */
