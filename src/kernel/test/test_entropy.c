/* ============================================================================
 * test_entropy.c -- TEST_CAT_SECURITY coverage for the early entropy model
 *
 * Pure-helper tests only: classification, policy gate, quality packing,
 * transcript framing, and the conservative clamp. The global record is
 * exercised via entropy_record_source/readback (in-memory state only --
 * no live boot infrastructure). Collection paths (EFI RNG, TPM, seed
 * file) are validated by boot checkpoints, not unit tests.
 * ============================================================================ */

#include "kernel/test/test.h"
#include "kernel/entropy.h"
#include "kernel/seed_file.h"
#include "libc/string.h"

static void test_entropy_source_mask(void)
{
    /* Quality pack/unpack round-trips for every source slot. */
    uint32_t packed = 0;
    uint32_t src;

    for (src = 0; src < ENTROPY_SRC_COUNT; src++) {
        packed = entropy_quality_set(packed, (entropy_src_t)src,
                                     (src & 1) ? ENTROPY_Q_HIGH
                                               : ENTROPY_Q_LOW);
    }
    for (src = 0; src < ENTROPY_SRC_COUNT; src++) {
        entropy_quality_t want = (src & 1) ? ENTROPY_Q_HIGH : ENTROPY_Q_LOW;
        TEST_ASSERT_EQ(entropy_quality_get(packed, (entropy_src_t)src),
                       want, "2-bit quality slot round-trips");
    }

    /* Setting one slot does not disturb its neighbors. */
    packed = entropy_quality_set(packed, ENTROPY_SRC_TPM_RNG, ENTROPY_Q_NONE);
    TEST_ASSERT_EQ(entropy_quality_get(packed, ENTROPY_SRC_CPU_RNG),
                   ENTROPY_Q_HIGH, "neighbor slot survives clear");
    TEST_ASSERT_EQ(entropy_quality_get(packed, ENTROPY_SRC_ACPI_OEM0),
                   ENTROPY_Q_HIGH, "other neighbor survives clear");
}

static void test_entropy_classify(void)
{
    uint32_t mask = 0, q = 0;

    /* mask=0 -> degraded, never a fake claim. */
    TEST_ASSERT_EQ(entropy_classify(0, 0), ENTROPY_CLASS_DEGRADED,
                   "no sources classifies degraded");

    /* LOW-only sources stay degraded. */
    mask = ENTROPY_SRC_BIT(ENTROPY_SRC_JITTER) |
           ENTROPY_SRC_BIT(ENTROPY_SRC_TIME);
    q = entropy_quality_set(0, ENTROPY_SRC_JITTER, ENTROPY_Q_LOW);
    q = entropy_quality_set(q, ENTROPY_SRC_TIME, ENTROPY_Q_LOW);
    TEST_ASSERT_EQ(entropy_classify(mask, q), ENTROPY_CLASS_DEGRADED,
                   "LOW-only sources classify degraded");

    /* One HIGH -> minimum. */
    mask |= ENTROPY_SRC_BIT(ENTROPY_SRC_CPU_RNG);
    q = entropy_quality_set(q, ENTROPY_SRC_CPU_RNG, ENTROPY_Q_HIGH);
    TEST_ASSERT_EQ(entropy_classify(mask, q), ENTROPY_CLASS_MINIMUM,
                   "single HIGH source classifies minimum");

    /* Two HIGH -> good. */
    mask |= ENTROPY_SRC_BIT(ENTROPY_SRC_FW_RNG);
    q = entropy_quality_set(q, ENTROPY_SRC_FW_RNG, ENTROPY_Q_HIGH);
    TEST_ASSERT_EQ(entropy_classify(mask, q), ENTROPY_CLASS_GOOD,
                   "two HIGH sources classify good");

    /* HIGH quality bits without the mask bit do not count. */
    TEST_ASSERT_EQ(entropy_classify(0, q), ENTROPY_CLASS_DEGRADED,
                   "quality without mask bit is ignored");

    /* Descriptor-bypass guard: timing sources marked HIGH directly in the
     * packed value still never count toward classification. */
    mask = ENTROPY_SRC_BIT(ENTROPY_SRC_JITTER) |
           ENTROPY_SRC_BIT(ENTROPY_SRC_TIME);
    q = entropy_quality_set(0, ENTROPY_SRC_JITTER, ENTROPY_Q_HIGH);
    q = entropy_quality_set(q, ENTROPY_SRC_TIME, ENTROPY_Q_HIGH);
    TEST_ASSERT_EQ(entropy_classify(mask, q), ENTROPY_CLASS_DEGRADED,
                   "timing-HIGH from a descriptor never credits");

    /* All 8 sources HIGH: GOOD via the 6 non-timing sources. */
    {
        uint32_t s;
        mask = 0; q = 0;
        for (s = 0; s < ENTROPY_SRC_COUNT; s++) {
            mask |= ENTROPY_SRC_BIT(s);
            q = entropy_quality_set(q, (entropy_src_t)s, ENTROPY_Q_HIGH);
        }
        TEST_ASSERT_EQ(entropy_classify(mask, q), ENTROPY_CLASS_GOOD,
                       "all sources HIGH classifies good");
    }
}

static void test_entropy_policy(void)
{
    TEST_ASSERT_EQ(entropy_policy_ok(ENTROPY_CLASS_DEGRADED, 1), 0,
                   "release mode refuses degraded");
    TEST_ASSERT_EQ(entropy_policy_ok(ENTROPY_CLASS_MINIMUM, 1), 1,
                   "release mode accepts minimum");
    TEST_ASSERT_EQ(entropy_policy_ok(ENTROPY_CLASS_GOOD, 1), 1,
                   "release mode accepts good");
    TEST_ASSERT_EQ(entropy_policy_ok(ENTROPY_CLASS_DEGRADED, 0), 1,
                   "debug mode proceeds degraded (logged elsewhere)");
}

static void test_entropy_frame_source(void)
{
    uint8_t buf[64];
    uint8_t payload[5] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE };
    uint32_t pos;

    /* Frame = 1-byte src id + 4-byte LE length + payload. */
    pos = entropy_frame_source(buf, sizeof(buf), 0,
                               ENTROPY_SRC_TPM_RNG, payload, 5);
    TEST_ASSERT_EQ(pos, 10u, "framed record is 1+4+5 bytes");
    TEST_ASSERT_EQ(buf[0], (uint8_t)ENTROPY_SRC_TPM_RNG, "src id first");
    TEST_ASSERT_EQ(buf[1], 5u, "LE length low byte");
    TEST_ASSERT_EQ(buf[4], 0u, "LE length high byte");
    TEST_ASSERT_EQ(buf[5], 0xAA, "payload follows header");
    TEST_ASSERT_EQ(buf[9], 0xEE, "payload last byte");

    /* Second record appends after the first. */
    pos = entropy_frame_source(buf, sizeof(buf), pos,
                               ENTROPY_SRC_CPU_RNG, payload, 2);
    TEST_ASSERT_EQ(pos, 17u, "second record appends");
    TEST_ASSERT_EQ(buf[10], (uint8_t)ENTROPY_SRC_CPU_RNG,
                   "second src id at boundary");

    /* No-fit is a hard failure (returns 0), never a truncated record. */
    TEST_ASSERT_EQ(entropy_frame_source(buf, 12, 10,
                                        ENTROPY_SRC_FW_RNG, payload, 5),
                   0u, "overflow refused, not truncated");
    /* Exact fit succeeds: pos 17, payload 5 -> need 10, cap 27. */
    TEST_ASSERT_EQ(entropy_frame_source(buf, 27, 17,
                                        ENTROPY_SRC_FW_RNG, payload, 5),
                   27u, "exact-fit record accepted");
    /* pos beyond cap refused. */
    TEST_ASSERT_EQ(entropy_frame_source(buf, 12, 13,
                                        ENTROPY_SRC_FW_RNG, payload, 1),
                   0u, "pos past cap refused");
    /* Near-UINT32_MAX length must be refused by the wrap guard, with no
     * bytes written (header would otherwise land before the loop). */
    {
        uint8_t before = buf[0];
        TEST_ASSERT_EQ(entropy_frame_source(buf, sizeof(buf), 0,
                                            ENTROPY_SRC_FW_RNG, payload,
                                            0xFFFFFFFCu),
                       0u, "wrap-length refused");
        TEST_ASSERT_EQ(buf[0], before, "no bytes written on wrap refusal");
    }
    /* Invalid inputs refused. */
    TEST_ASSERT_EQ(entropy_frame_source(buf, sizeof(buf), 0,
                                        ENTROPY_SRC_COUNT, payload, 5),
                   0u, "out-of-range src refused");
    TEST_ASSERT_EQ(entropy_frame_source(buf, sizeof(buf), 0,
                                        ENTROPY_SRC_FW_RNG, payload, 0),
                   0u, "zero-length payload refused");
}

static void test_entropy_record_clamp(void)
{
    /* Snapshot the live record, exercise, restore -- the global model is
     * plain in-memory state, safe for tests. */
    uint32_t saved_mask = entropy_source_mask();
    uint32_t saved_q = entropy_source_quality();

    entropy_record_source(ENTROPY_SRC_JITTER, ENTROPY_Q_HIGH);
    TEST_ASSERT_EQ(entropy_quality_get(entropy_source_quality(),
                                       ENTROPY_SRC_JITTER),
                   ENTROPY_Q_LOW, "jitter clamped to LOW");

    entropy_record_source(ENTROPY_SRC_TIME, ENTROPY_Q_HIGH);
    TEST_ASSERT_EQ(entropy_quality_get(entropy_source_quality(),
                                       ENTROPY_SRC_TIME),
                   ENTROPY_Q_LOW, "time clamped to LOW");

    entropy_record_source(ENTROPY_SRC_CPU_RNG, ENTROPY_Q_HIGH);
    TEST_ASSERT_EQ((entropy_source_mask() >> ENTROPY_SRC_CPU_RNG) & 1u, 1u,
                   "mask bit set on record");

    /* Reserved value 3 fails conservative: records as NONE, mask clear. */
    entropy_record_source(ENTROPY_SRC_FW_RNG, (entropy_quality_t)3);
    TEST_ASSERT_EQ(entropy_quality_get(entropy_source_quality(),
                                       ENTROPY_SRC_FW_RNG),
                   ENTROPY_Q_NONE, "reserved quality records as NONE");
    TEST_ASSERT_EQ((entropy_source_mask() >> ENTROPY_SRC_FW_RNG) & 1u, 0u,
                   "reserved quality does not set the mask bit");

    /* Q_NONE retracts a previously recorded source. */
    entropy_record_source(ENTROPY_SRC_CPU_RNG, ENTROPY_Q_NONE);
    TEST_ASSERT_EQ((entropy_source_mask() >> ENTROPY_SRC_CPU_RNG) & 1u, 0u,
                   "Q_NONE retracts the mask bit");

    /* Out-of-range src is ignored entirely. */
    {
        uint32_t m0 = entropy_source_mask();
        uint32_t q0 = entropy_source_quality();
        entropy_record_source((entropy_src_t)ENTROPY_SRC_COUNT,
                              ENTROPY_Q_HIGH);
        TEST_ASSERT_EQ(entropy_source_mask(), m0,
                       "out-of-range src leaves mask unchanged");
        TEST_ASSERT_EQ(entropy_source_quality(), q0,
                       "out-of-range src leaves quality unchanged");
    }

    /* Restore: re-record saved values slot by slot. */
    {
        uint32_t src;
        for (src = 0; src < ENTROPY_SRC_COUNT; src++) {
            entropy_quality_t q =
                entropy_quality_get(saved_q, (entropy_src_t)src);
            if ((saved_mask >> src) & 1u)
                entropy_record_source((entropy_src_t)src, q);
            else
                entropy_record_source((entropy_src_t)src, ENTROPY_Q_NONE);
        }
    }
}

static void test_entropy_staged_transcript(void)
{
    uint8_t data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t out[ENTROPY_STAGE_CAP];
    uint8_t tiny[4];
    uint32_t len;

    /* Start from a clean slate (tests may run after the boot jitter
     * sampler staged real bytes; consume_zero is the supported reset). */
    entropy_staged_consume_zero();

    TEST_ASSERT_EQ(entropy_stage_source(ENTROPY_SRC_SEED_FILE, data, 8,
                                        ENTROPY_Q_HIGH), 1,
                   "staging a fitting record succeeds");

    /* Credit is recorded by the time stage_source returns (under the
     * staging lock, so a drain observing the bytes observes the credit). */
    TEST_ASSERT_EQ((entropy_source_mask() >> ENTROPY_SRC_SEED_FILE) & 1u, 1u,
                   "staged source mask bit visible after stage_source");
    TEST_ASSERT_EQ(entropy_quality_get(entropy_source_quality(),
                                       ENTROPY_SRC_SEED_FILE),
                   ENTROPY_Q_HIGH,
                   "staged source quality visible after stage_source");

    /* NULL dest and too-small cap consume nothing. */
    TEST_ASSERT_EQ(entropy_staged_drain((uint8_t *)0, sizeof(out)), 0u,
                   "drain to NULL returns 0");
    TEST_ASSERT_EQ(entropy_staged_drain(tiny, (uint32_t)sizeof(tiny)), 0u,
                   "drain with cap < staged length returns 0");

    /* Full drain returns the record and consumes it. */
    len = entropy_staged_drain(out, (uint32_t)sizeof(out));
    TEST_ASSERT_EQ(len, 13u, "drained length = 1+4+8");
    TEST_ASSERT_EQ(out[0], (uint8_t)ENTROPY_SRC_SEED_FILE,
                   "drained record carries the src id");
    TEST_ASSERT_EQ(entropy_staged_drain(out, (uint32_t)sizeof(out)), 0u,
                   "second drain finds an empty transcript");

    /* Oversized record refused; staged content stays intact. */
    {
        uint8_t big[ENTROPY_STAGE_CAP];
        uint32_t i;
        for (i = 0; i < sizeof(big); i++) big[i] = (uint8_t)i;
        TEST_ASSERT_EQ(entropy_stage_source(ENTROPY_SRC_SEED_FILE, data, 8,
                                            ENTROPY_Q_HIGH), 1,
                       "restaging after drain succeeds");
        TEST_ASSERT_EQ(entropy_stage_source(ENTROPY_SRC_JITTER, big,
                                            (uint32_t)sizeof(big),
                                            ENTROPY_Q_LOW), 0,
                       "oversized record refused");
        TEST_ASSERT_EQ(entropy_staged_overflowed(), 1,
                       "overflow flag set after refusal");
        len = entropy_staged_drain(out, (uint32_t)sizeof(out));
        TEST_ASSERT_EQ(len, 13u, "refusal leaves staged record intact");
    }

    /* consume_zero is the no-copy reset path. */
    TEST_ASSERT_EQ(entropy_stage_source(ENTROPY_SRC_SEED_FILE, data, 8,
                                        ENTROPY_Q_HIGH), 1,
                   "staging before consume_zero succeeds");
    entropy_staged_consume_zero();
    TEST_ASSERT_EQ(entropy_staged_drain(out, (uint32_t)sizeof(out)), 0u,
                   "consume_zero resets staged length");

    /* Restore the global record slot the staging test touched. */
    entropy_record_source(ENTROPY_SRC_SEED_FILE, ENTROPY_Q_NONE);
}

static void test_random_seed_format(void)
{
    /* Pure seed-file format helpers: encode/accept round-trip plus every
     * rejection class, in check order (no VFS, no NVRAM). */
    static const uint8_t secret[SEED_FILE_SECRET_LEN] = { 0x5A, 0x01, 0xFE };
    uint8_t payload[SEED_FILE_PAYLOAD_LEN];
    uint8_t payload_out[SEED_FILE_PAYLOAD_LEN];
    struct seed_file_blob blob, tampered;
    uint64_t counter_out = 0;
    uint32_t i;

    for (i = 0; i < SEED_FILE_PAYLOAD_LEN; i++)
        payload[i] = (uint8_t)(i * 7u + 3u);

    seed_file_encode(&blob, 42, payload, secret);
    TEST_ASSERT_EQ(blob.magic, SEED_FILE_MAGIC, "encode sets magic");
    TEST_ASSERT_EQ(blob.version, SEED_FILE_VERSION, "encode sets version");

    TEST_ASSERT_EQ(seed_file_accept(&blob, SEED_FILE_SIZE, secret, 41,
                                    &counter_out, payload_out),
                   (uint32_t)SEED_FILE_OK, "valid blob accepted");
    TEST_ASSERT_EQ(counter_out, 42u, "counter round-trips");
    TEST_ASSERT_EQ(memcmp(payload_out, payload, SEED_FILE_PAYLOAD_LEN), 0,
                   "payload round-trips");

    TEST_ASSERT_EQ(seed_file_accept(&blob, SEED_FILE_SIZE - 1, secret, 41,
                                    NULL, NULL),
                   (uint32_t)SEED_FILE_BAD_LEN, "short buffer rejected");
    TEST_ASSERT_EQ(seed_file_accept(NULL, SEED_FILE_SIZE, secret, 41,
                                    NULL, NULL),
                   (uint32_t)SEED_FILE_BAD_LEN, "NULL buffer rejected");

    tampered = blob;
    tampered.magic ^= 1u;
    TEST_ASSERT_EQ(seed_file_accept(&tampered, SEED_FILE_SIZE, secret, 41,
                                    NULL, NULL),
                   (uint32_t)SEED_FILE_BAD_MAGIC, "bad magic rejected");

    tampered = blob;
    tampered.version = SEED_FILE_VERSION + 1;
    TEST_ASSERT_EQ(seed_file_accept(&tampered, SEED_FILE_SIZE, secret, 41,
                                    NULL, NULL),
                   (uint32_t)SEED_FILE_BAD_VERSION, "bad version rejected");

    tampered = blob;
    tampered.payload[5] ^= 0x80u;
    TEST_ASSERT_EQ(seed_file_accept(&tampered, SEED_FILE_SIZE, secret, 41,
                                    NULL, NULL),
                   (uint32_t)SEED_FILE_BAD_MAC, "tampered payload fails MAC");

    /* Counter tamper fails the MAC -- REPLAY is unreachable for forgeries. */
    tampered = blob;
    tampered.counter = 9999;
    TEST_ASSERT_EQ(seed_file_accept(&tampered, SEED_FILE_SIZE, secret, 41,
                                    NULL, NULL),
                   (uint32_t)SEED_FILE_BAD_MAC, "tampered counter fails MAC");

    /* Anti-replay: valid MAC but counter <= last_seen. */
    TEST_ASSERT_EQ(seed_file_accept(&blob, SEED_FILE_SIZE, secret, 42,
                                    NULL, NULL),
                   (uint32_t)SEED_FILE_REPLAY, "counter == last_seen replayed");
    TEST_ASSERT_EQ(seed_file_accept(&blob, SEED_FILE_SIZE, secret, 100,
                                    NULL, NULL),
                   (uint32_t)SEED_FILE_REPLAY, "counter < last_seen replayed");

    /* Oversized input is BAD_LEN, never truncated to a valid blob. */
    {
        uint8_t big[SEED_FILE_SIZE + 1];
        memcpy(big, &blob, SEED_FILE_SIZE);
        big[SEED_FILE_SIZE] = 0;
        TEST_ASSERT_EQ(seed_file_accept(big, SEED_FILE_SIZE + 1, secret, 41,
                                        NULL, NULL),
                       (uint32_t)SEED_FILE_BAD_LEN, "oversized buffer rejected");
    }

    /* The stored MAC field itself is authenticated end to end. */
    tampered = blob;
    tampered.mac[0] ^= 1u;
    TEST_ASSERT_EQ(seed_file_accept(&tampered, SEED_FILE_SIZE, secret, 41,
                                    NULL, NULL),
                   (uint32_t)SEED_FILE_BAD_MAC, "mac[0] tamper rejected");
    tampered = blob;
    tampered.mac[SEED_FILE_MAC_LEN - 1] ^= 0x80u;
    TEST_ASSERT_EQ(seed_file_accept(&tampered, SEED_FILE_SIZE, secret, 41,
                                    NULL, NULL),
                   (uint32_t)SEED_FILE_BAD_MAC, "mac[31] tamper rejected");

    /* Rejections never write the caller's outputs (no poisoning). */
    {
        uint64_t c_sentinel = 0xA5A5A5A5A5A5A5A5ULL;
        uint8_t p_sentinel[SEED_FILE_PAYLOAD_LEN];
        uint8_t p_copy[SEED_FILE_PAYLOAD_LEN];
        memset(p_sentinel, 0xC3, sizeof(p_sentinel));
        memcpy(p_copy, p_sentinel, sizeof(p_copy));

        tampered = blob;
        tampered.payload[0] ^= 1u;  /* BAD_MAC */
        TEST_ASSERT_EQ(seed_file_accept(&tampered, SEED_FILE_SIZE, secret, 41,
                                        &c_sentinel, p_sentinel),
                       (uint32_t)SEED_FILE_BAD_MAC, "poison probe: BAD_MAC");
        TEST_ASSERT_EQ(c_sentinel, 0xA5A5A5A5A5A5A5A5ULL,
                       "BAD_MAC leaves counter_out unchanged");
        TEST_ASSERT_EQ(memcmp(p_sentinel, p_copy, sizeof(p_copy)), 0,
                       "BAD_MAC leaves payload_out unchanged");

        TEST_ASSERT_EQ(seed_file_accept(&blob, SEED_FILE_SIZE, secret, 42,
                                        &c_sentinel, p_sentinel),
                       (uint32_t)SEED_FILE_REPLAY, "poison probe: REPLAY");
        TEST_ASSERT_EQ(c_sentinel, 0xA5A5A5A5A5A5A5A5ULL,
                       "REPLAY leaves counter_out unchanged");
        TEST_ASSERT_EQ(memcmp(p_sentinel, p_copy, sizeof(p_copy)), 0,
                       "REPLAY leaves payload_out unchanged");
    }

    /* Either OK-path output pointer may be NULL independently. */
    counter_out = 0;
    TEST_ASSERT_EQ(seed_file_accept(&blob, SEED_FILE_SIZE, secret, 41,
                                    &counter_out, NULL),
                   (uint32_t)SEED_FILE_OK, "counter_out-only accept");
    TEST_ASSERT_EQ(counter_out, 42u, "counter_out-only value");
    TEST_ASSERT_EQ(seed_file_accept(&blob, SEED_FILE_SIZE, secret, 41,
                                    NULL, payload_out),
                   (uint32_t)SEED_FILE_OK, "payload_out-only accept");

    /* Counter extremes: 0 can never be fresh (last_seen starts at 0);
     * UINT64_MAX is accepted by the pure helper -- the phase3 rotation
     * guard owns the wraparound (skips rotation at exhaustion). */
    seed_file_encode(&tampered, 0, payload, secret);
    TEST_ASSERT_EQ(seed_file_accept(&tampered, SEED_FILE_SIZE, secret, 0,
                                    NULL, NULL),
                   (uint32_t)SEED_FILE_REPLAY, "counter 0 never fresh");
    seed_file_encode(&tampered, (uint64_t)0xFFFFFFFFFFFFFFFFULL,
                     payload, secret);
    TEST_ASSERT_EQ(seed_file_accept(&tampered, SEED_FILE_SIZE, secret, 41,
                                    &counter_out, NULL),
                   (uint32_t)SEED_FILE_OK, "max counter accepted (pure)");
    TEST_ASSERT_EQ(counter_out, (uint64_t)0xFFFFFFFFFFFFFFFFULL,
                   "max counter round-trips");
}

static void test_random_seed_provenance_gate(void)
{
    /* seed_file_hw_provenance: HIGH hardware class -> 1; timing-only or
     * seed-file-only credit -> 0 (no self-laundering). Boot collectors
     * populate the global record before tests run: snapshot, retract
     * every source, assert, then restore (record + Q_NONE retraction is
     * the documented pure in-memory pattern). */
    uint32_t saved_q = entropy_source_quality();
    uint32_t saved_mask = entropy_source_mask();
    uint32_t src;

    for (src = 0; src < ENTROPY_SRC_COUNT; src++)
        entropy_record_source((entropy_src_t)src, ENTROPY_Q_NONE);

    TEST_ASSERT_EQ((uint64_t)seed_file_hw_provenance(), 0u,
                   "no sources recorded: no provenance");

    entropy_record_source(ENTROPY_SRC_JITTER, ENTROPY_Q_HIGH);
    TEST_ASSERT_EQ((uint64_t)seed_file_hw_provenance(), 0u,
                   "jitter (clamped LOW) is not hardware provenance");
    entropy_record_source(ENTROPY_SRC_JITTER, ENTROPY_Q_NONE);

    entropy_record_source(ENTROPY_SRC_SEED_FILE, ENTROPY_Q_HIGH);
    TEST_ASSERT_EQ((uint64_t)seed_file_hw_provenance(), 0u,
                   "seed-file credit never proves provenance (no launder)");
    entropy_record_source(ENTROPY_SRC_SEED_FILE, ENTROPY_Q_NONE);

    entropy_record_source(ENTROPY_SRC_TPM_RNG, ENTROPY_Q_HIGH);
    TEST_ASSERT_EQ((uint64_t)seed_file_hw_provenance(), 1u,
                   "HIGH TPM RNG is hardware provenance");
    entropy_record_source(ENTROPY_SRC_TPM_RNG, ENTROPY_Q_NONE);

    entropy_record_source(ENTROPY_SRC_CPU_RNG, ENTROPY_Q_LOW);
    TEST_ASSERT_EQ((uint64_t)seed_file_hw_provenance(), 0u,
                   "LOW hardware credit is not provenance");
    entropy_record_source(ENTROPY_SRC_CPU_RNG, ENTROPY_Q_NONE);

    /* Restore the boot-time record exactly. */
    for (src = 0; src < ENTROPY_SRC_COUNT; src++) {
        entropy_quality_t q = (saved_mask & ENTROPY_SRC_BIT(src))
            ? entropy_quality_get(saved_q, (entropy_src_t)src)
            : ENTROPY_Q_NONE;
        entropy_record_source((entropy_src_t)src, q);
    }
}

static void test_random_seed_clone_rejected(void)
{
    /* A blob MACed under one machine's token secret must fail closed on a
     * machine with a different secret (anti-clone). */
    static const uint8_t secret_a[SEED_FILE_SECRET_LEN] = { 0x11, 0x22 };
    static const uint8_t secret_b[SEED_FILE_SECRET_LEN] = { 0x11, 0x23 };
    uint8_t payload[SEED_FILE_PAYLOAD_LEN] = { 0xAB };
    struct seed_file_blob blob;

    seed_file_encode(&blob, 7, payload, secret_a);
    TEST_ASSERT_EQ(seed_file_accept(&blob, SEED_FILE_SIZE, secret_a, 0,
                                    NULL, NULL),
                   (uint32_t)SEED_FILE_OK, "own-machine blob accepted");
    TEST_ASSERT_EQ(seed_file_accept(&blob, SEED_FILE_SIZE, secret_b, 0,
                                    NULL, NULL),
                   (uint32_t)SEED_FILE_BAD_MAC, "cloned blob fails closed");
}

void test_register_entropy(void)
{
    test_suite_register_cat("entropy: quality slot packing",
        test_entropy_source_mask, TEST_CAT_SECURITY);
    test_suite_register_cat("entropy: classification",
        test_entropy_classify, TEST_CAT_SECURITY);
    test_suite_register_cat("entropy: policy gate",
        test_entropy_policy, TEST_CAT_SECURITY);
    test_suite_register_cat("entropy: transcript framing",
        test_entropy_frame_source, TEST_CAT_SECURITY);
    test_suite_register_cat("entropy: record + clamp",
        test_entropy_record_clamp, TEST_CAT_SECURITY);
    test_suite_register_cat("entropy: staged transcript",
        test_entropy_staged_transcript, TEST_CAT_SECURITY);
    test_suite_register_cat("entropy: seed file format",
        test_random_seed_format, TEST_CAT_SECURITY);
    test_suite_register_cat("entropy: seed file clone rejected",
        test_random_seed_clone_rejected, TEST_CAT_SECURITY);
    test_suite_register_cat("entropy: seed file provenance gate",
        test_random_seed_provenance_gate, TEST_CAT_SECURITY);
}
