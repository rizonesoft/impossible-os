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
#include "kernel/boot_info.h"
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

/* ---- boot_info seed payload (boot_info seed handoff section) ----------- */

/* Local CRC-32C twin (bitwise Castagnoli) -- the kernel's entropy_crc32c
 * is static; an independent reimplementation also catches an accidental
 * polynomial change on either side of the bootloader/kernel mirror. */
static uint32_t tseed_crc32c(const uint8_t *data, uint64_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint64_t i;
    int b;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* Build a bootloader-shaped payload: 32-byte header + framed records.
 * Returns total payload length. */
static uint64_t tseed_build(uint8_t *buf, uint32_t cap,
                            uint32_t mask, uint32_t quality,
                            const uint8_t *recs, uint32_t recs_len)
{
    struct entropy_seed_header *hdr = (struct entropy_seed_header *)buf;

    memset(buf, 0, cap);
    hdr->magic          = ENTROPY_SEED_MAGIC;
    hdr->version        = ENTROPY_SEED_VERSION;
    hdr->source_mask    = mask;
    hdr->quality        = quality;
    hdr->transcript_len = recs_len;
    if (recs_len)
        memcpy(buf + sizeof(*hdr), recs, recs_len);
    return sizeof(*hdr) + recs_len;
}

/* entropy_seed_verify_fn fake: accepts an 80-byte blob whose first byte is
 * 0xAA; counter = second byte; payload_out = 0x5A fill. Everything else
 * rejected. */
static int tseed_fake_verify(const uint8_t *blob, uint32_t len,
                             uint64_t *counter_out, uint8_t payload_out[32])
{
    if (len != 80u || blob[0] != 0xAAu)
        return 0;
    *counter_out = (uint64_t)blob[1];
    memset(payload_out, 0x5A, 32);
    return 1;
}

static void test_boot_seed_payload_parse(void)
{
    uint8_t payload[512];
    uint8_t recs[256];
    uint8_t out[256];
    uint32_t out_len;
    struct entropy_seed_parse_result res;
    uint64_t plen;
    uint32_t rlen;

    /* Two framed records: 8-byte FW_RNG + 32-byte TIME. */
    static const uint8_t fw[8]  = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t tm[32];
    memset(tm, 0x77, sizeof(tm));
    rlen = entropy_frame_source(recs, sizeof(recs), 0,
                                ENTROPY_SRC_FW_RNG, fw, sizeof(fw));
    rlen = entropy_frame_source(recs, sizeof(recs), rlen,
                                ENTROPY_SRC_TIME, tm, sizeof(tm));
    TEST_ASSERT(rlen == (5u + 8u) + (5u + 32u), "fixture framing length");

    /* NULL-arg guards. */
    plen = tseed_build(payload, sizeof(payload), 0x81u, 0x2u, recs, rlen);
    TEST_ASSERT_EQ(entropy_seed_parse(NULL, plen, 0, 0, NULL,
                                      out, sizeof(out), &out_len, &res),
                   (uint32_t)ENTROPY_SEED_BAD_ARGS, "NULL payload rejected");
    TEST_ASSERT_EQ(entropy_seed_parse(payload, plen, 0, 0, NULL,
                                      NULL, sizeof(out), &out_len, &res),
                   (uint32_t)ENTROPY_SEED_BAD_ARGS, "NULL out rejected");

    /* Header gates. */
    TEST_ASSERT_EQ(entropy_seed_parse(payload, 31, 0, 0, NULL,
                                      out, sizeof(out), &out_len, &res),
                   (uint32_t)ENTROPY_SEED_TOO_SHORT, "31 bytes too short");
    payload[0] ^= 1u;
    TEST_ASSERT_EQ(entropy_seed_parse(payload, plen, 0, 0, NULL,
                                      out, sizeof(out), &out_len, &res),
                   (uint32_t)ENTROPY_SEED_BAD_MAGIC, "magic tamper rejected");
    payload[0] ^= 1u;
    payload[4] ^= 1u;  /* version word */
    TEST_ASSERT_EQ(entropy_seed_parse(payload, plen, 0, 0, NULL,
                                      out, sizeof(out), &out_len, &res),
                   (uint32_t)ENTROPY_SEED_BAD_VERSION, "version bump rejected");
    payload[4] ^= 1u;
    TEST_ASSERT_EQ(entropy_seed_parse(payload, plen + 1u, 0, 0, NULL,
                                      out, sizeof(out), &out_len, &res),
                   (uint32_t)ENTROPY_SEED_BAD_LENGTH,
                   "trailing slack rejected (exact-length contract)");

    /* CRC gates: high checksum bits, then a flipped payload bit. */
    {
        uint64_t crc = (uint64_t)tseed_crc32c(payload, plen);
        TEST_ASSERT_EQ(entropy_seed_parse(payload, plen, 1,
                                          crc | (1ull << 32), NULL,
                                          out, sizeof(out), &out_len, &res),
                       (uint32_t)ENTROPY_SEED_BAD_CRC,
                       "checksum high bits must be zero");
        payload[40] ^= 1u;
        TEST_ASSERT_EQ(entropy_seed_parse(payload, plen, 1, crc, NULL,
                                          out, sizeof(out), &out_len, &res),
                       (uint32_t)ENTROPY_SEED_BAD_CRC, "bit flip fails CRC");
        payload[40] ^= 1u;

        /* Bootloader-shaped contract test: header + records + CRC over
         * the WHOLE payload accepted end-to-end. */
        out_len = 0xFFFFFFFFu;
        TEST_ASSERT_EQ(entropy_seed_parse(payload, plen, 1, crc,
                                          NULL,
                                          out, sizeof(out), &out_len, &res),
                       (uint32_t)ENTROPY_SEED_OK,
                       "bootloader-shaped payload accepted");
        TEST_ASSERT_EQ(out_len, rlen, "all framed bytes copied");
        TEST_ASSERT_EQ(memcmp(out, recs, rlen), 0, "records copied intact");
        TEST_ASSERT_EQ(res.record_count, 2u, "two records counted");
        TEST_ASSERT_EQ(res.records_mask,
                       ENTROPY_SRC_BIT(ENTROPY_SRC_FW_RNG) |
                       ENTROPY_SRC_BIT(ENTROPY_SRC_TIME),
                       "records mask re-derived from records");
        TEST_ASSERT_EQ(res.hdr_mask, 0x81u, "advisory mask surfaced");
        TEST_ASSERT_EQ(res.hdr_quality, 0x2u, "advisory quality surfaced");
    }

    /* Record framing violations reject the WHOLE payload. */
    {
        uint8_t bad[5] = { (uint8_t)ENTROPY_SRC_COUNT, 1, 0, 0, 0 };
        plen = tseed_build(payload, sizeof(payload), 0, 0, bad, 5);
        out_len = 0xFFFFFFFFu;
        TEST_ASSERT_EQ(entropy_seed_parse(payload, plen, 0, 0, NULL,
                                          out, sizeof(out), &out_len, &res),
                       (uint32_t)ENTROPY_SEED_BAD_RECORD,
                       "src id out of range rejected");
        TEST_ASSERT_EQ(out_len, 0u, "out not consumed on bad record");

        bad[0] = (uint8_t)ENTROPY_SRC_FW_RNG;  /* rlen = 1 but no byte */
        plen = tseed_build(payload, sizeof(payload), 0, 0, bad, 5);
        TEST_ASSERT_EQ(entropy_seed_parse(payload, plen, 0, 0, NULL,
                                          out, sizeof(out), &out_len, &res),
                       (uint32_t)ENTROPY_SEED_BAD_RECORD,
                       "record length overrun rejected");

        bad[1] = 0;  /* rlen = 0 */
        plen = tseed_build(payload, sizeof(payload), 0, 0, bad, 5);
        TEST_ASSERT_EQ(entropy_seed_parse(payload, plen, 0, 0, NULL,
                                          out, sizeof(out), &out_len, &res),
                       (uint32_t)ENTROPY_SEED_BAD_RECORD,
                       "zero-length record rejected");
    }

    /* Output cap too small -> NO_FIT (never truncate). */
    plen = tseed_build(payload, sizeof(payload), 0, 0, recs, rlen);
    TEST_ASSERT_EQ(entropy_seed_parse(payload, plen, 0, 0, NULL,
                                      out, 8, &out_len, &res),
                   (uint32_t)ENTROPY_SEED_NO_FIT, "tiny out cap is NO_FIT");

    /* All-or-nothing boundary: cap fits record 1 (5+8=13 bytes) but not
     * record 2 -- the parser has already copied a prefix when it hits
     * the wall, and the contract is out_len == 0 so the caller consumes
     * NOTHING from a rejected payload. */
    out_len = 0xFFFFFFFFu;
    TEST_ASSERT_EQ(entropy_seed_parse(payload, plen, 0, 0, NULL,
                                      out, 13, &out_len, &res),
                   (uint32_t)ENTROPY_SEED_NO_FIT,
                   "second record overflow is NO_FIT");
    TEST_ASSERT_EQ(out_len, 0u, "partial copy never reaches the caller");

    /* Empty transcript (header only) parses OK with zero records. */
    plen = tseed_build(payload, sizeof(payload), 0, 0, NULL, 0);
    TEST_ASSERT_EQ(entropy_seed_parse(payload, plen, 0, 0, NULL,
                                      out, sizeof(out), &out_len, &res),
                   (uint32_t)ENTROPY_SEED_OK, "header-only payload OK");
    TEST_ASSERT_EQ(out_len, 0u, "header-only payload copies nothing");
    TEST_ASSERT_EQ(res.record_count, 0u, "header-only has zero records");
}

static void test_boot_seed_carryover_routing(void)
{
    uint8_t payload[512];
    uint8_t recs[384];
    uint8_t out[384];
    uint32_t out_len;
    struct entropy_seed_parse_result res;
    uint64_t plen;
    uint32_t rlen;
    uint8_t blob_ok[80], blob_ok2[80], blob_bad[80];

    memset(blob_ok, 0x11, sizeof(blob_ok));
    blob_ok[0] = 0xAA; blob_ok[1] = 7;     /* accepted, counter 7 */
    memset(blob_ok2, 0x22, sizeof(blob_ok2));
    blob_ok2[0] = 0xAA; blob_ok2[1] = 9;   /* accepted, counter 9 */
    memset(blob_bad, 0x33, sizeof(blob_bad));
    blob_bad[0] = 0xBB;                    /* rejected by fake verifier */

    /* src-4 ok + src-4 bad + src-4 ok2 + one FW record. */
    rlen = entropy_frame_source(recs, sizeof(recs), 0,
                                ENTROPY_SRC_SEED_FILE,
                                blob_ok, sizeof(blob_ok));
    rlen = entropy_frame_source(recs, sizeof(recs), rlen,
                                ENTROPY_SRC_SEED_FILE,
                                blob_bad, sizeof(blob_bad));
    rlen = entropy_frame_source(recs, sizeof(recs), rlen,
                                ENTROPY_SRC_SEED_FILE,
                                blob_ok2, sizeof(blob_ok2));
    {
        static const uint8_t fw[8] = { 9, 9, 9, 9, 9, 9, 9, 9 };
        rlen = entropy_frame_source(recs, sizeof(recs), rlen,
                                    ENTROPY_SRC_FW_RNG, fw, sizeof(fw));
    }
    plen = tseed_build(payload, sizeof(payload), 0, 0, recs, rlen);

    TEST_ASSERT_EQ(entropy_seed_parse(payload, plen, 0, 0,
                                      tseed_fake_verify,
                                      out, sizeof(out), &out_len, &res),
                   (uint32_t)ENTROPY_SEED_OK, "carryover payload parses");
    TEST_ASSERT_EQ(res.seed_file_ok, 2u, "two carryover blobs accepted");
    TEST_ASSERT_EQ(res.seed_file_rejected, 1u,
                   "rejected blob dropped without failing the parse");
    TEST_ASSERT_EQ(res.seed_file_counter, 9u,
                   "highest accepted counter tracked");
    TEST_ASSERT_EQ(res.record_count, 3u,
                   "accepted records = 2 carryover + 1 firmware");
    TEST_ASSERT_EQ(res.records_mask,
                   ENTROPY_SRC_BIT(ENTROPY_SRC_SEED_FILE) |
                   ENTROPY_SRC_BIT(ENTROPY_SRC_FW_RNG),
                   "mask covers seed-file + firmware only");

    /* Accepted carryover is RE-FRAMED as the 32-byte verified inner
     * payload, not the raw 80-byte blob. */
    TEST_ASSERT_EQ((uint64_t)out[0], (uint64_t)ENTROPY_SRC_SEED_FILE,
                   "first out record is src-4");
    TEST_ASSERT_EQ((uint64_t)out[1], 32u, "re-framed to inner 32 bytes");
    TEST_ASSERT_EQ((uint64_t)out[5], 0x5Au, "inner payload from verifier");

    /* Re-frame path honors the same all-or-nothing cap contract: the
     * first accepted carryover re-frame (5+32=37 bytes) fits, the second
     * does not -> NO_FIT with nothing consumed. */
    out_len = 0xFFFFFFFFu;
    TEST_ASSERT_EQ(entropy_seed_parse(payload, plen, 0, 0,
                                      tseed_fake_verify,
                                      out, 37, &out_len, &res),
                   (uint32_t)ENTROPY_SEED_NO_FIT,
                   "second re-framed carryover overflow is NO_FIT");
    TEST_ASSERT_EQ(out_len, 0u, "re-frame partial copy never consumed");

    /* No verifier wired -> every src-4 record is dropped (fail closed),
     * the rest of the payload still parses. */
    TEST_ASSERT_EQ(entropy_seed_parse(payload, plen, 0, 0, NULL,
                                      out, sizeof(out), &out_len, &res),
                   (uint32_t)ENTROPY_SEED_OK, "NULL verifier parses");
    TEST_ASSERT_EQ(res.seed_file_ok, 0u, "NULL verifier accepts nothing");
    TEST_ASSERT_EQ(res.seed_file_rejected, 3u,
                   "NULL verifier drops all carryover records");
    TEST_ASSERT_EQ(res.record_count, 1u, "firmware record survives");
}

static void test_random_seed_clone_degraded(void)
{
    /* A cloned seed file (valid-looking carryover whose machine token does
     * not match) must fail CLOSED end-to-end, not merely fail the MAC check:
     * routed through the boot-seed parser with adversarial all-HIGH advisory
     * header bits, the rejected carryover leaves no accepted record and the
     * re-derived class is DEGRADED -- the advisory header can never launder a
     * rejected clone into credit. Pure helpers only (no live VFS/NVRAM). */
    uint8_t payload[256];
    uint8_t recs[128];
    uint8_t out[128];
    uint32_t out_len = 0xFFFFFFFFu;
    struct entropy_seed_parse_result res;
    uint64_t plen;
    uint32_t rlen;
    uint8_t clone[80];

    /* First byte != 0xAA -> tseed_fake_verify rejects, modelling the
     * seed_file_early_verify BAD_MAC a wrong-token clone produces. */
    memset(clone, 0x5C, sizeof(clone));
    clone[0] = 0xBB;

    rlen = entropy_frame_source(recs, sizeof(recs), 0,
                                ENTROPY_SRC_SEED_FILE, clone, sizeof(clone));
    /* Adversarial advisory header: claim every source HIGH. */
    plen = tseed_build(payload, sizeof(payload), 0xFFu, 0xFFFFFFFFu,
                       recs, rlen);

    TEST_ASSERT_EQ(entropy_seed_parse(payload, plen, 0, 0, tseed_fake_verify,
                                      out, sizeof(out), &out_len, &res),
                   (uint32_t)ENTROPY_SEED_OK,
                   "cloned-only payload parses (rejection is not a parse error)");
    TEST_ASSERT_EQ(res.seed_file_ok, 0u, "cloned carryover not accepted");
    TEST_ASSERT_EQ(res.seed_file_rejected, 1u, "cloned carryover counted rejected");
    TEST_ASSERT_EQ(res.record_count, 0u, "no accepted records from a clone");
    TEST_ASSERT_EQ(res.records_mask, 0u, "no source credited from a clone");
    TEST_ASSERT_EQ(out_len, 0u, "nothing copied from a rejected clone");

    /* Fail closed: empty re-derived mask classifies DEGRADED regardless of
     * the all-HIGH advisory header the parser surfaced but never trusted. */
    TEST_ASSERT_EQ(res.hdr_quality, 0xFFFFFFFFu, "adversarial advisory surfaced");
    TEST_ASSERT_EQ(entropy_classify(res.records_mask, res.hdr_quality),
                   ENTROPY_CLASS_DEGRADED,
                   "clone fails closed to degraded despite HIGH advisory bits");

    /* The format layer rejects the wrong-token blob directly (anti-clone). */
    {
        static const uint8_t secret_a[SEED_FILE_SECRET_LEN] = { 0x11, 0x22 };
        static const uint8_t secret_b[SEED_FILE_SECRET_LEN] = { 0x11, 0x23 };
        uint8_t pl[SEED_FILE_PAYLOAD_LEN] = { 0xAB };
        struct seed_file_blob blob;

        seed_file_encode(&blob, 7, pl, secret_a);
        TEST_ASSERT_EQ(seed_file_accept(&blob, SEED_FILE_SIZE, secret_b, 0,
                                        NULL, NULL),
                       (uint32_t)SEED_FILE_BAD_MAC,
                       "wrong-token clone rejected at the format layer");
    }
}

static void test_boot_seed_desc_classify(void)
{
    const uint32_t vr = BOOT_PAYLOAD_FLAG_VALID | BOOT_PAYLOAD_FLAG_RESERVED;
    const uint64_t CAPS = (uint64_t)BOOT_CAP_PAYLOAD_DESCRIPTORS;

    /* VALID-only (never PMM-pinned): untouchable regardless of range --
     * the round-2 adversarial regression case. */
    TEST_ASSERT_EQ(boot_seed_desc_classify(CAPS, BOOT_PAYLOAD_FLAG_VALID,
                                           0x5000ull, 256ull),
                   (uint32_t)BOOT_SEED_DESC_NOT_RESERVED,
                   "VALID-only descriptor is not consumable");

    /* Out of the 4 GiB identity map: start above, and length crossing. */
    TEST_ASSERT_EQ(boot_seed_desc_classify(CAPS, vr, BOOT_INFO_EARLY_MAP_END,
                                           256ull),
                   (uint32_t)BOOT_SEED_DESC_OUT_OF_MAP,
                   "start at map end is out of map");
    TEST_ASSERT_EQ(boot_seed_desc_classify(CAPS, vr,
                                           BOOT_INFO_EARLY_MAP_END - 64ull,
                                           65ull),
                   (uint32_t)BOOT_SEED_DESC_OUT_OF_MAP,
                   "range crossing map end is out of map");
    TEST_ASSERT_EQ(boot_seed_desc_classify(CAPS, vr,
                                           BOOT_INFO_EARLY_MAP_END - 64ull,
                                           64ull),
                   (uint32_t)BOOT_SEED_DESC_CONSUMABLE,
                   "range ending exactly at map end passes the map gate");

    /* Length contract: below header, above cap, and both boundaries. */
    TEST_ASSERT_EQ(boot_seed_desc_classify(CAPS, vr, 0x5000ull, 31ull),
                   (uint32_t)BOOT_SEED_DESC_BAD_LENGTH,
                   "31 bytes below header size");
    TEST_ASSERT_EQ(boot_seed_desc_classify(CAPS, vr, 0x5000ull,
                                           BOOT_SEED_PAYLOAD_CAP + 1ull),
                   (uint32_t)BOOT_SEED_DESC_BAD_LENGTH,
                   "cap+1 above contract");
    TEST_ASSERT_EQ(boot_seed_desc_classify(CAPS, vr, 0x5000ull, 32ull),
                   (uint32_t)BOOT_SEED_DESC_CONSUMABLE,
                   "header-sized payload consumable");
    TEST_ASSERT_EQ(boot_seed_desc_classify(CAPS, vr, 0x5000ull,
                                           BOOT_SEED_PAYLOAD_CAP),
                   (uint32_t)BOOT_SEED_DESC_CONSUMABLE,
                   "cap-sized payload consumable");

    /* CAPABILITY gate -- checked BEFORE FLAG_RESERVED, so a descriptor
     * that would otherwise be perfectly CONSUMABLE is still refused.
     * These are the OBSERVABLE-SEAM assertions: delete the capability
     * branch in boot_seed_desc_classify and this block fails, which is
     * exactly what the equivalent TPM headless-authorization branch did
     * NOT have when deleting it left the whole suite green. */
    TEST_ASSERT_EQ(boot_seed_desc_classify(0u, vr, 0x5000ull, 256ull),
                   (uint32_t)BOOT_SEED_DESC_NO_CAPABILITY,
                   "caps clear refuses an otherwise-consumable descriptor");
    TEST_ASSERT_EQ(boot_seed_desc_classify(~(uint64_t)BOOT_CAP_PAYLOAD_DESCRIPTORS,
                                           vr, 0x5000ull, 256ull),
                   (uint32_t)BOOT_SEED_DESC_NO_CAPABILITY,
                   "every OTHER capability bit set is still a refusal");
    TEST_ASSERT_EQ(boot_seed_desc_classify(CAPS, vr, 0x5000ull, 256ull),
                   (uint32_t)BOOT_SEED_DESC_CONSUMABLE,
                   "control: same descriptor consumable once caps negotiated");
    /* A REAL handoff carries several negotiated capabilities at once, so
     * pin that the gate is a MASK TEST and not an equality check -- an
     * equality regression would pass every assertion above and then
     * reject every normal boot. */
    TEST_ASSERT_EQ(boot_seed_desc_classify(CAPS | BOOT_CAP_RUNTIME_SERVICES,
                                           vr, 0x5000ull, 256ull),
                   (uint32_t)BOOT_SEED_DESC_CONSUMABLE,
                   "caps bit alongside others is still consumable");
    /* Maximal length: the map bound is written as a subtraction against
     * BOOT_INFO_EARLY_MAP_END precisely so it cannot wrap. Pin it, or an
     * overflow-shaped rewrite could classify a wrapping descriptor as
     * BAD_LENGTH and wipe low memory through that class. */
    TEST_ASSERT_EQ(boot_seed_desc_classify(CAPS, vr, 0x5000ull, ~0ull),
                   (uint32_t)BOOT_SEED_DESC_OUT_OF_MAP,
                   "a maximal length is out-of-map, never bad-length");

    /* Precedence: caps outranks the other three refusals, so the reason
     * reported is the one a reader can act on. */
    TEST_ASSERT_EQ(boot_seed_desc_classify(0u, BOOT_PAYLOAD_FLAG_VALID,
                                           BOOT_INFO_EARLY_MAP_END, 1ull),
                   (uint32_t)BOOT_SEED_DESC_NO_CAPABILITY,
                   "caps refusal outranks NOT_RESERVED and OUT_OF_MAP");

    /* Disposition seam: what each class permits, as DATA. */
    TEST_ASSERT_EQ(boot_seed_desc_wipe_len(BOOT_SEED_DESC_NO_CAPABILITY,
                                           4096ull), 0ull,
                   "no-capability descriptor is not even wiped");
    TEST_ASSERT_EQ(boot_seed_desc_wipe_len(BOOT_SEED_DESC_NOT_RESERVED,
                                           4096ull), 0ull,
                   "unreserved descriptor is not wiped");
    TEST_ASSERT_EQ(boot_seed_desc_wipe_len(BOOT_SEED_DESC_OUT_OF_MAP,
                                           4096ull), 0ull,
                   "out-of-map descriptor is not wiped");
    TEST_ASSERT_EQ(boot_seed_desc_wipe_len(BOOT_SEED_DESC_CONSUMABLE,
                                           4096ull), 4096ull,
                   "consumable payload wipes its full length");
    /* BAD_LENGTH wipes NOTHING. The reservation pass refuses to pin a
     * descriptor failing boot_seed_length_reservable, so by Phase 1 its
     * frames may belong to the allocator -- wiping even a clamped prefix
     * would corrupt the new owner. Clamping alone was the first draft and
     * the post-commit adversarial round showed it merely traded a free of
     * unwiped memory for a permanent pin of it. */
    TEST_ASSERT_EQ(boot_seed_desc_wipe_len(BOOT_SEED_DESC_BAD_LENGTH, 31ull),
                   0ull,
                   "short bad-length payload is never pinned, so never wiped");
    TEST_ASSERT_EQ(boot_seed_desc_wipe_len(BOOT_SEED_DESC_BAD_LENGTH,
                                           BOOT_SEED_PAYLOAD_CAP + 1ull),
                   0ull,
                   "cap+1 bad-length payload is never pinned, so never wiped");
    TEST_ASSERT_EQ(boot_seed_desc_wipe_len(BOOT_SEED_DESC_BAD_LENGTH,
                                           0xFFFFFFFFull),
                   0ull,
                   "a 4 GiB claim wipes nothing rather than a clamped prefix");

    /* The shared length contract itself -- the ONE rule the Phase-0
     * reservation pass and this Phase-1 consumer both apply. If these
     * two ever disagreed, one of them would be touching memory the other
     * never reserved. */
    TEST_ASSERT_EQ((uint64_t)boot_seed_length_reservable(
                       sizeof(struct entropy_seed_header)), 1u,
                   "exactly the header size is reservable");
    TEST_ASSERT_EQ((uint64_t)boot_seed_length_reservable(
                       sizeof(struct entropy_seed_header) - 1ull), 0u,
                   "one byte below the header size is not reservable");
    TEST_ASSERT_EQ((uint64_t)boot_seed_length_reservable(BOOT_SEED_PAYLOAD_CAP),
                   1u, "exactly the contract cap is reservable");
    TEST_ASSERT_EQ((uint64_t)boot_seed_length_reservable(
                       BOOT_SEED_PAYLOAD_CAP + 1ull), 0u,
                   "cap+1 is not reservable, so it can never pin a span");
    TEST_ASSERT_EQ((uint64_t)boot_seed_length_reservable(0xFFFFFFFFull), 0u,
                   "a 4 GiB claim is not reservable");
    TEST_ASSERT_EQ((uint64_t)boot_seed_length_reservable(0ull), 0u,
                   "a zero length is not reservable");
    /* The contract and the classifier must agree, or the reservation pass
     * and the consumer would disagree about who owns the frames. */
    TEST_ASSERT_EQ(boot_seed_desc_classify(CAPS, vr, 0x5000ull,
                                           BOOT_SEED_PAYLOAD_CAP + 1ull),
                   (uint32_t)BOOT_SEED_DESC_BAD_LENGTH,
                   "classifier rejects exactly what the contract refuses to pin");

    /* The [high] design finding: a clamped WIPE must never license an
     * unclamped FREE. Only CONSUMABLE may reach the frame loop, whose
     * end address is derived from the descriptor's own length. */
    TEST_ASSERT_EQ((uint64_t)boot_seed_desc_may_free(BOOT_SEED_DESC_CONSUMABLE),
                   1u, "consumable payload may return its frames");
    TEST_ASSERT_EQ((uint64_t)boot_seed_desc_may_free(BOOT_SEED_DESC_BAD_LENGTH),
                   0u,
                   "bad-length payload never frees: the page count would "
                   "come from the field just rejected");
    TEST_ASSERT_EQ((uint64_t)boot_seed_desc_may_free(BOOT_SEED_DESC_NO_CAPABILITY),
                   0u, "no-capability payload never frees");
    TEST_ASSERT_EQ((uint64_t)boot_seed_desc_may_free(BOOT_SEED_DESC_NOT_RESERVED),
                   0u, "unreserved payload never frees");
    TEST_ASSERT_EQ((uint64_t)boot_seed_desc_may_free(BOOT_SEED_DESC_OUT_OF_MAP),
                   0u, "out-of-map payload never frees");
}

static void test_entropy_seed_zeroized(void)
{
    uint8_t buf[96];
    uint32_t i;
    uint64_t sum;

    /* Page-owning shape: wiped AND releasable. */
    memset(buf, 0xA5, sizeof(buf));
    TEST_ASSERT_EQ((uint64_t)boot_seed_release_payload(buf, sizeof(buf),
                                                       0x5000ull, 4096ull,
                                                       BOOT_PRODUCER_UEFI),
                   1u, "aligned page-certified payload is releasable");
    for (sum = 0, i = 0; i < sizeof(buf); i++)
        sum += buf[i];
    TEST_ASSERT_EQ(sum, 0u, "payload all-zero after consumption");

    /* Sub-page shape: wiped but NOT releasable. */
    memset(buf, 0xA5, sizeof(buf));
    TEST_ASSERT_EQ((uint64_t)boot_seed_release_payload(buf, sizeof(buf),
                                                       0x5010ull, 4096ull,
                                                       BOOT_PRODUCER_UEFI),
                   0u, "unaligned start never frees frames");
    for (sum = 0, i = 0; i < sizeof(buf); i++)
        sum += buf[i];
    TEST_ASSERT_EQ(sum, 0u, "unaligned payload still wiped");

    memset(buf, 0xA5, sizeof(buf));
    TEST_ASSERT_EQ((uint64_t)boot_seed_release_payload(buf, sizeof(buf),
                                                       0x5000ull, 8ull,
                                                       BOOT_PRODUCER_UEFI),
                   0u, "non-page alignment never frees frames");
    for (sum = 0, i = 0; i < sizeof(buf); i++)
        sum += buf[i];
    TEST_ASSERT_EQ(sum, 0u, "non-page-certified payload still wiped");

    /* Foreign producer: alignment is NOT ownership -- a perfectly
     * page-shaped descriptor from another producer is wipe-only. */
    memset(buf, 0xA5, sizeof(buf));
    TEST_ASSERT_EQ((uint64_t)boot_seed_release_payload(buf, sizeof(buf),
                                                       0x5000ull, 4096ull,
                                                       BOOT_PRODUCER_NONE),
                   0u, "foreign-producer payload never frees frames");
    for (sum = 0, i = 0; i < sizeof(buf); i++)
        sum += buf[i];
    TEST_ASSERT_EQ(sum, 0u, "foreign-producer payload still wiped");

    /* Guards: NULL / zero length are no-ops. */
    TEST_ASSERT_EQ((uint64_t)boot_seed_release_payload(NULL, 64,
                                                       0x5000ull, 4096ull,
                                                       BOOT_PRODUCER_UEFI),
                   0u, "NULL payload refused");
    TEST_ASSERT_EQ((uint64_t)boot_seed_release_payload(buf, 0,
                                                       0x5000ull, 4096ull,
                                                       BOOT_PRODUCER_UEFI),
                   0u, "zero length refused");
}

static void test_external_entropy_oneshot(void)
{
    uint8_t src[64], dst[64];
    uint32_t i;
    uint64_t sum;

    /* Normal path: full copy, source destroyed. */
    memset(src, 0xC7, sizeof(src));
    memset(dst, 0, sizeof(dst));
    TEST_ASSERT_EQ(entropy_external_oneshot(src, sizeof(src),
                                            dst, sizeof(dst)),
                   (uint32_t)sizeof(src), "full offering copied");
    TEST_ASSERT_EQ((uint64_t)dst[0], 0xC7u, "copy carries the bytes");
    TEST_ASSERT_EQ((uint64_t)dst[63], 0xC7u, "copy carries the last byte");
    for (sum = 0, i = 0; i < sizeof(src); i++)
        sum += src[i];
    TEST_ASSERT_EQ(sum, 0u, "source wiped after the copy (one-shot)");

    /* Cap clamp: only cap bytes copied, source STILL fully wiped. */
    memset(src, 0x3D, sizeof(src));
    memset(dst, 0, sizeof(dst));
    TEST_ASSERT_EQ(entropy_external_oneshot(src, sizeof(src), dst, 16),
                   16u, "oversize offering clamped to cap");
    TEST_ASSERT_EQ((uint64_t)dst[15], 0x3Du, "clamped copy filled");
    TEST_ASSERT_EQ((uint64_t)dst[16], 0u, "clamp writes nothing past cap");
    for (sum = 0, i = 0; i < sizeof(src); i++)
        sum += src[i];
    TEST_ASSERT_EQ(sum, 0u, "full source wiped even when clamped");

    /* Reject paths: NULL dst / zero cap still destroy the source. */
    memset(src, 0x99, sizeof(src));
    TEST_ASSERT_EQ(entropy_external_oneshot(src, sizeof(src), NULL, 64),
                   0u, "NULL dst copies nothing");
    for (sum = 0, i = 0; i < sizeof(src); i++)
        sum += src[i];
    TEST_ASSERT_EQ(sum, 0u, "rejected offering still destroyed");
    /* cap == 0: copies nothing, writes nothing, source STILL destroyed. */
    memset(src, 0x44, sizeof(src));
    memset(dst, 0xEE, sizeof(dst));
    TEST_ASSERT_EQ(entropy_external_oneshot(src, sizeof(src), dst, 0),
                   0u, "zero cap copies nothing");
    TEST_ASSERT_EQ((uint64_t)dst[0], 0xEEu, "zero cap writes nothing");
    for (sum = 0, i = 0; i < sizeof(src); i++)
        sum += src[i];
    TEST_ASSERT_EQ(sum, 0u, "zero-cap offering still destroyed");
    TEST_ASSERT_EQ(entropy_external_oneshot(NULL, 32, dst, 32),
                   0u, "NULL src refused");
    TEST_ASSERT_EQ(entropy_external_oneshot(src, 0, dst, 32),
                   0u, "zero-length offering refused");

    /* Class names are the stable diagnostics vocabulary. */
    TEST_ASSERT_EQ(strcmp(entropy_class_str(ENTROPY_CLASS_GOOD), "good"),
                   0, "class string: good");
    TEST_ASSERT_EQ(strcmp(entropy_class_str(ENTROPY_CLASS_MINIMUM),
                          "minimum"), 0, "class string: minimum");
    TEST_ASSERT_EQ(strcmp(entropy_class_str(ENTROPY_CLASS_DEGRADED),
                          "degraded"), 0, "class string: degraded");
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
    test_suite_register_cat("entropy: seed file clone degraded",
        test_random_seed_clone_degraded, TEST_CAT_SECURITY);
    test_suite_register_cat("entropy: seed file provenance gate",
        test_random_seed_provenance_gate, TEST_CAT_SECURITY);
    test_suite_register_cat("entropy: boot seed payload parse",
        test_boot_seed_payload_parse, TEST_CAT_SECURITY);
    test_suite_register_cat("entropy: boot seed carryover routing",
        test_boot_seed_carryover_routing, TEST_CAT_SECURITY);
    test_suite_register_cat("entropy: boot seed descriptor gate",
        test_boot_seed_desc_classify, TEST_CAT_SECURITY);
    test_suite_register_cat("entropy: boot seed zeroized",
        test_entropy_seed_zeroized, TEST_CAT_SECURITY);
    test_suite_register_cat("entropy: external one-shot",
        test_external_entropy_oneshot, TEST_CAT_SECURITY);
}
