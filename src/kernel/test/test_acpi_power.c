/* ============================================================================
 * test_acpi_power.c -- ACPI power management unit tests
 *
 * Verifies sleep state discovery, sleep entry API, and fixed event setup.
 *
 * XREF: 02-kernel-core/TODO-26-power-management.md
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/acpi.h"

/* ---- S-state discovery ---- */

static void test_acpi_s5_always_supported(void)
{
    TEST_ASSERT(acpi_sleep_supported(5) == 1,
                "S5 (shutdown) is always supported");
}

static void test_acpi_s5_slp_typa_valid(void)
{
    uint16_t typa = acpi_get_slp_typa(5);
    TEST_ASSERT(typa != 0xFFFF,
                "S5 SLP_TYPa is valid (not 0xFFFF)");
}

static void test_acpi_invalid_state_not_supported(void)
{
    TEST_ASSERT(acpi_sleep_supported(0) == 0,
                "S0 is not a sleep state");
    TEST_ASSERT(acpi_sleep_supported(2) == 0,
                "S2 is not supported (deprecated)");
    TEST_ASSERT(acpi_sleep_supported(6) == 0,
                "S6 does not exist");
}

static void test_acpi_invalid_state_slp_typa(void)
{
    TEST_ASSERT(acpi_get_slp_typa(0) == 0xFFFF,
                "S0 SLP_TYPa is INVALID");
    TEST_ASSERT(acpi_get_slp_typa(6) == 0xFFFF,
                "S6 SLP_TYPa is INVALID");
}

static void test_acpi_sleep_states_consistent(void)
{
    /* Every supported sleep state must expose a valid SLP_TYPa.  Walk
     * S1..S5 under a single aggregated assertion so the serial log
     * emits one PASS line instead of up to five near-duplicates. */
    uint8_t s;
    int all_valid = 1;
    for (s = 1; s <= 5; s++) {
        if (acpi_sleep_supported(s) && acpi_get_slp_typa(s) == 0xFFFF) {
            all_valid = 0;
            break;
        }
    }
    TEST_ASSERT_EQ(all_valid, 1,
                   "every supported sleep state has a valid SLP_TYPa");
}

/* ---- Sleep entry API ---- */

static void test_acpi_enter_unsupported_fails(void)
{
    /* Entering an unsupported state must return -1, not crash */
    int rc = acpi_enter_sleep_state(2);  /* S2 is deprecated/absent */
    TEST_ASSERT(rc == -1,
                "acpi_enter_sleep_state(S2) returns -1 (unsupported)");
}

static void test_acpi_enter_invalid_fails(void)
{
    int rc = acpi_enter_sleep_state(6);
    TEST_ASSERT(rc == -1,
                "acpi_enter_sleep_state(S6) returns -1 (invalid)");
}

static void test_acpi_enter_s5_rejected(void)
{
    /* S5 is soft-off, not a resumable sleep state. The generic sleep API must
     * reject it (return -1 before any SLP write or storage quiesce) so the
     * S1-S4 resume tail can never run after a storage quiesce. acpi_shutdown()
     * is the only supported S5 path. */
    int rc = acpi_enter_sleep_state(5);
    TEST_ASSERT(rc == -1,
                "acpi_enter_sleep_state(S5) returns -1 (use acpi_shutdown)");
}


/* ---- Synthetic-DSDT parser tests (firmware-input hardening) --------------
 *
 * These build table images in test memory and run them through the REAL
 * parser via acpi_parse_sleep_type_test(). They cover the paths live firmware
 * never exercises: alternative AML integer encodings, an independent
 * SLP_TYPb, out-of-range values, and truncated/oversized packages.
 */

#define SDT_HDR_LEN 36u

/* Build a table image: 36-byte header + `body` bytes, signature `sig`,
 * length set to the real size, checksum fixed up so acpi_table_valid_test()
 * accepts it. Returns the total length. */
static uint32_t build_table(uint8_t *buf, const char *sig,
                            const uint8_t *body, uint32_t body_len)
{
    uint32_t total = SDT_HDR_LEN + body_len;
    uint32_t i;
    uint8_t sum = 0;

    for (i = 0; i < SDT_HDR_LEN + body_len; i++)
        buf[i] = 0;
    for (i = 0; i < 4; i++)
        buf[i] = (uint8_t)sig[i];
    buf[4] = (uint8_t)(total & 0xFF);
    buf[5] = (uint8_t)((total >> 8) & 0xFF);
    buf[6] = (uint8_t)((total >> 16) & 0xFF);
    buf[7] = (uint8_t)((total >> 24) & 0xFF);
    for (i = 0; i < body_len; i++)
        buf[SDT_HDR_LEN + i] = body[i];

    for (i = 0; i < total; i++)
        sum = (uint8_t)(sum + buf[i]);
    buf[9] = (uint8_t)(0u - sum);   /* checksum byte */
    return total;
}

static void test_acpi_parse_byteprefix(void)
{
    uint8_t buf[96];
    /* _S3_ PackageOp pkglen=6 numelem=2 BytePrefix 5 BytePrefix 5.
     * PkgLength counts its own byte + NumElements + the 4 element bytes. */
    const uint8_t body[] = { '_','S','3','_', 0x12, 0x06, 0x02,
                             0x0A, 0x05, 0x0A, 0x05 };
    uint16_t a = 0, b = 0;

    build_table(buf, "DSDT", body, (uint32_t)sizeof(body));
    TEST_ASSERT(acpi_parse_sleep_type_test(buf, '3', &a, &b) == 1,
                "well-formed _S3_ package parses");
    TEST_ASSERT_EQ(a, 5, "BytePrefix SLP_TYPa decodes to its value");
    TEST_ASSERT_EQ(b, 5, "BytePrefix SLP_TYPb decodes to its value");
}

static void test_acpi_parse_wordprefix(void)
{
    uint8_t buf[96];
    /* WordPrefix (0x0B) encoding of 3. The old decoder understood only
     * BytePrefix and returned the OPCODE byte, so this used to yield 0x0B. */
    const uint8_t body[] = { '_','S','3','_', 0x12, 0x05, 0x01,
                             0x0B, 0x03, 0x00 };
    uint16_t a = 0, b = 0;

    build_table(buf, "DSDT", body, (uint32_t)sizeof(body));
    TEST_ASSERT(acpi_parse_sleep_type_test(buf, '3', &a, &b) == 1,
                "WordPrefix-encoded _S3_ parses");
    TEST_ASSERT_EQ(a, 3, "WordPrefix decodes to its value, not its opcode");
    TEST_ASSERT_EQ(b, 0, "single-element package packs SLP_TYPb in the high byte");
}

static void test_acpi_parse_zero_one_ops(void)
{
    uint8_t buf[96];
    /* ZeroOp (0x00) for SLP_TYPa, OneOp (0x01) for SLP_TYPb */
    const uint8_t body[] = { '_','S','4','_', 0x12, 0x04, 0x02, 0x00, 0x01 };
    uint16_t a = 0xFF, b = 0xFF;

    build_table(buf, "DSDT", body, (uint32_t)sizeof(body));
    TEST_ASSERT(acpi_parse_sleep_type_test(buf, '4', &a, &b) == 1,
                "ZeroOp/OneOp encoded _S4_ parses");
    TEST_ASSERT_EQ(a, 0, "ZeroOp decodes to 0");
    TEST_ASSERT_EQ(b, 1, "OneOp decodes to 1");
}

static void test_acpi_parse_independent_typb(void)
{
    uint8_t buf[96];
    /* SLP_TYPa=2, SLP_TYPb=6 -- ACPI permits them to differ, and the PM1b
     * write must not reuse SLP_TYPa. */
    const uint8_t body[] = { '_','S','3','_', 0x12, 0x06, 0x02,
                             0x0A, 0x02, 0x0A, 0x06 };
    uint16_t a = 0, b = 0;

    build_table(buf, "DSDT", body, (uint32_t)sizeof(body));
    TEST_ASSERT(acpi_parse_sleep_type_test(buf, '3', &a, &b) == 1,
                "package with distinct A/B types parses");
    TEST_ASSERT_EQ(a, 2, "SLP_TYPa kept independent");
    TEST_ASSERT_EQ(b, 6, "SLP_TYPb kept independent of SLP_TYPa");
}

static void test_acpi_parse_wide_value_truncated_not_rejected(void)
{
    uint8_t buf[96];
    /* A value wider than the 3-bit SLP_TYP field is TRUNCATED, not rejected.
     * AcpiGetSleepTypeData takes (UINT8) of each integer and leaves the masking
     * to the register writer (pm1_write_sleep here). Rejecting instead would
     * report the state unsupported, which on S5 means no ACPI poweroff at all
     * on firmware the reference implementation handles fine. */
    const uint8_t body[] = { '_','S','3','_', 0x12, 0x04, 0x01, 0x0A, 0x09 };
    uint16_t a = 0, b = 0;

    build_table(buf, "DSDT", body, (uint32_t)sizeof(body));
    TEST_ASSERT(acpi_parse_sleep_type_test(buf, '3', &a, &b) == 1,
                "a value above 7 parses (ACPICA parity), it is not rejected");
    TEST_ASSERT_EQ(a, 9, "SLP_TYPa keeps the byte value; the writer masks it");
}

static void test_acpi_parse_truncated_package(void)
{
    uint8_t buf[96];
    /* The name and PackageOp are present but the table ENDS before the
     * integer term. The parser must report failure, not read past `length`. */
    const uint8_t body[] = { '_','S','3','_', 0x12, 0x04, 0x02 };
    uint16_t a = 0, b = 0;

    build_table(buf, "DSDT", body, (uint32_t)sizeof(body));
    TEST_ASSERT(acpi_parse_sleep_type_test(buf, '3', &a, &b) == 0,
                "package truncated at the table end fails cleanly");
    TEST_ASSERT_EQ(a, 0xFFFF, "truncated parse leaves SLP_TYPa INVALID");
}

static void test_acpi_parse_absent_object(void)
{
    uint8_t buf[96];
    const uint8_t body[] = { '_','S','3','_', 0x12, 0x04, 0x01, 0x0A, 0x01 };
    uint16_t a = 0, b = 0;

    build_table(buf, "DSDT", body, (uint32_t)sizeof(body));
    TEST_ASSERT(acpi_parse_sleep_type_test(buf, '1', &a, &b) == 0,
                "a state whose _Sx_ object is absent does not parse");
    TEST_ASSERT_EQ(a, 0xFFFF, "absent object yields INVALID, never 0");
}

static void test_acpi_parse_packed_single_element(void)
{
    uint8_t buf[96];
    /* ONE element carrying both types packed: low byte SLP_TYPa=2, next byte
     * SLP_TYPb=1 (WordPrefix 0x0102). This is the encoding the vendored
     * reference implementation accepts for Package.Count == 1
     * (AcpiGetSleepTypeData, acpica/components/hardware/hwxface.c), so it must
     * NOT be treated as a truncated two-element package. */
    const uint8_t body[] = { '_','S','3','_', 0x12, 0x05, 0x01,
                             0x0B, 0x02, 0x01 };
    uint16_t a = 0, b = 0;

    build_table(buf, "DSDT", body, (uint32_t)sizeof(body));
    TEST_ASSERT(acpi_parse_sleep_type_test(buf, '3', &a, &b) == 1,
                "single-element packed _S3_ parses");
    TEST_ASSERT_EQ(a, 2, "packed low byte is SLP_TYPa");
    TEST_ASSERT_EQ(b, 1, "packed high byte is SLP_TYPb");
}

static void test_acpi_parse_pkglength_overrun_rejected(void)
{
    uint8_t buf[96];
    /* NumElements claims 2 but PkgLength declares a package that ends after the
     * FIRST element. Before PkgLength was decoded, the second read walked past
     * the package into whatever AML followed and returned it as SLP_TYPb. */
    const uint8_t body[] = { '_','S','3','_', 0x12, 0x04, 0x02,
                             0x0A, 0x02, 0x0A, 0x06 };
    uint16_t a = 0, b = 0;

    build_table(buf, "DSDT", body, (uint32_t)sizeof(body));
    /* The package DECLARES two elements but PkgLength ends after the first, so
     * the second cannot be decoded. ACPICA fails the whole call in that case
     * rather than returning half a transition, and so does this. */
    TEST_ASSERT(acpi_parse_sleep_type_test(buf, '3', &a, &b) == 0,
                "declared-but-undecodable second element fails the object");
    TEST_ASSERT_EQ(a, 0xFFFF, "no half-published SLP_TYPa on a failed package");
    TEST_ASSERT_EQ(b, 0xFFFF, "SLP_TYPb past the declared package end is not read");
}

static void test_acpi_parse_pkglength_past_table_rejected(void)
{
    uint8_t buf[96];
    /* PkgLength declares an extent running past the end of the table. */
    const uint8_t body[] = { '_','S','3','_', 0x12, 0x3F, 0x02,
                             0x0A, 0x02, 0x0A, 0x06 };
    uint16_t a = 0, b = 0;

    build_table(buf, "DSDT", body, (uint32_t)sizeof(body));
    TEST_ASSERT(acpi_parse_sleep_type_test(buf, '3', &a, &b) == 0,
                "a PkgLength extending past the table is rejected");
    TEST_ASSERT_EQ(a, 0xFFFF, "rejected package leaves SLP_TYPa INVALID");
}

static void test_acpi_parse_packed_reserved_bytes_ignored(void)
{
    uint8_t buf[96];
    /* DWordPrefix 0x00010102 in a ONE-element package. ACPICA's case 1 takes
     * (UINT8)value and (UINT8)(value >> 8), ignoring everything above bit 15,
     * so this is a VALID A=2 / B=1 definition. Rejecting it on the reserved
     * upper bytes would hide a real _S5 and leave the machine with only the
     * emulator-specific poweroff ports. */
    const uint8_t body[] = { '_','S','5','_', 0x12, 0x07, 0x01,
                             0x0C, 0x02, 0x01, 0x01, 0x00 };
    uint16_t a = 0, b = 0;

    build_table(buf, "DSDT", body, (uint32_t)sizeof(body));
    TEST_ASSERT(acpi_parse_sleep_type_test(buf, '5', &a, &b) == 1,
                "packed value with nonzero reserved bytes is accepted");
    TEST_ASSERT_EQ(a, 2, "reserved upper bytes do not disturb SLP_TYPa");
    TEST_ASSERT_EQ(b, 1, "reserved upper bytes do not disturb SLP_TYPb");
}

static void test_acpi_parse_second_element_undecodable(void)
{
    uint8_t buf[96];
    /* Two elements declared, but the second is not an AML integer at all (0x77
     * is not a data-object opcode). ACPICA's case 2 fails the WHOLE call when
     * either element is a non-integer, so nothing may be published. */
    const uint8_t body[] = { '_','S','3','_', 0x12, 0x06, 0x02,
                             0x0A, 0x02, 0x77, 0x09 };
    uint16_t a = 0, b = 0;

    build_table(buf, "DSDT", body, (uint32_t)sizeof(body));
    TEST_ASSERT(acpi_parse_sleep_type_test(buf, '3', &a, &b) == 0,
                "a non-integer second element fails the whole object");
    TEST_ASSERT_EQ(a, 0xFFFF, "SLP_TYPa is not published when SLP_TYPb is bad");
}

/* ---- Table validation (bounded, checksummed firmware input) ---- */

static void test_acpi_table_valid_accepts_good(void)
{
    uint8_t buf[96];
    const uint8_t body[] = { 0x00 };

    build_table(buf, "DSDT", body, (uint32_t)sizeof(body));
    TEST_ASSERT(acpi_table_valid_test(buf, "DSDT") == 1,
                "well-formed checksummed DSDT validates");
}

static void test_acpi_table_valid_rejects_bad_checksum(void)
{
    uint8_t buf[96];
    const uint8_t body[] = { 0x00 };

    build_table(buf, "DSDT", body, (uint32_t)sizeof(body));
    buf[9] = (uint8_t)(buf[9] + 1);   /* corrupt the checksum byte */
    TEST_ASSERT(acpi_table_valid_test(buf, "DSDT") == 0,
                "checksum mismatch is rejected");
}

static void test_acpi_table_valid_rejects_oversized_length(void)
{
    uint8_t buf[96];
    const uint8_t body[] = { 0x00 };

    build_table(buf, "DSDT", body, (uint32_t)sizeof(body));
    /* Claim a length far past both the buffer and the 16 MiB cap. A forged
     * length like this is what used to drive the scan into unmapped memory. */
    buf[4] = 0xFF; buf[5] = 0xFF; buf[6] = 0xFF; buf[7] = 0xFF;
    TEST_ASSERT(acpi_table_valid_test(buf, "DSDT") == 0,
                "a length beyond the table-size cap is rejected");
}

static void test_acpi_table_valid_rejects_short_length(void)
{
    uint8_t buf[96];
    const uint8_t body[] = { 0x00 };

    build_table(buf, "DSDT", body, (uint32_t)sizeof(body));
    buf[4] = 8; buf[5] = 0; buf[6] = 0; buf[7] = 0;  /* shorter than the header */
    TEST_ASSERT(acpi_table_valid_test(buf, "DSDT") == 0,
                "a length that cannot cover the header is rejected");
}

static void test_acpi_table_valid_rejects_wrong_signature(void)
{
    uint8_t buf[96];
    const uint8_t body[] = { 0x00 };

    build_table(buf, "FACP", body, (uint32_t)sizeof(body));
    TEST_ASSERT(acpi_table_valid_test(buf, "DSDT") == 0,
                "a table with the wrong signature is rejected");
}

/* ---- Sleep-entry refusals ---- */

static void test_acpi_enter_s3_refused(void)
{
    /* S3 is DISCOVERED by this section but not enterable: the suspend/resume
     * machinery does not exist, and QEMU reports S3 supported, so a caller
     * must get an error rather than a machine that loses processor state. */
    TEST_ASSERT(acpi_enter_sleep_state(3) == -1,
                "S3 entry refused until the suspend pipeline exists");
}

static void test_acpi_enter_s4_refused(void)
{
    TEST_ASSERT(acpi_enter_sleep_state(4) == -1,
                "S4 entry refused until the hibernate pipeline exists");
}

static void test_acpi_s5_query_self_consistent(void)
{
    /* acpi_sleep_supported(5) must agree with acpi_get_slp_typa(5): reporting
     * S5 supported while the getter returns INVALID was the old contradiction. */
    int supported = acpi_sleep_supported(5);
    uint16_t typa = acpi_get_slp_typa(5);

    TEST_ASSERT((supported == 1) == (typa != 0xFFFF),
                "S5 support flag agrees with its parsed SLP_TYPa");
}

/* ---- Registration ---- */

void test_register_acpi_power(void)
{
    test_suite_register_cat("ACPI: S5 always supported",
                            test_acpi_s5_always_supported, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: S5 SLP_TYPa valid",
                            test_acpi_s5_slp_typa_valid, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: invalid states not supported",
                            test_acpi_invalid_state_not_supported, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: invalid state SLP_TYPa",
                            test_acpi_invalid_state_slp_typa, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: sleep state consistency",
                            test_acpi_sleep_states_consistent, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: enter unsupported state fails",
                            test_acpi_enter_unsupported_fails, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: enter invalid state fails",
                            test_acpi_enter_invalid_fails, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: generic sleep API rejects S5",
                            test_acpi_enter_s5_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: BytePrefix _Sx_ package parses",
                            test_acpi_parse_byteprefix, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: WordPrefix decodes to value not opcode",
                            test_acpi_parse_wordprefix, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: ZeroOp/OneOp integer encodings",
                            test_acpi_parse_zero_one_ops, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: SLP_TYPb independent of SLP_TYPa",
                            test_acpi_parse_independent_typb, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: wide SLP_TYP truncated not rejected",
                            test_acpi_parse_wide_value_truncated_not_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: truncated _Sx_ package fails cleanly",
                            test_acpi_parse_truncated_package, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: absent _Sx_ object yields INVALID",
                            test_acpi_parse_absent_object, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: single-element package packs both types",
                            test_acpi_parse_packed_single_element, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: element past declared PkgLength not read",
                            test_acpi_parse_pkglength_overrun_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: PkgLength past table end rejected",
                            test_acpi_parse_pkglength_past_table_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: packed reserved bytes ignored (ACPICA parity)",
                            test_acpi_parse_packed_reserved_bytes_ignored, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: bad second element fails whole object",
                            test_acpi_parse_second_element_undecodable, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: table validator accepts good DSDT",
                            test_acpi_table_valid_accepts_good, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: table validator rejects bad checksum",
                            test_acpi_table_valid_rejects_bad_checksum, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: table validator rejects oversized length",
                            test_acpi_table_valid_rejects_oversized_length, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: table validator rejects short length",
                            test_acpi_table_valid_rejects_short_length, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: table validator rejects wrong signature",
                            test_acpi_table_valid_rejects_wrong_signature, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: S3 entry refused (no suspend pipeline)",
                            test_acpi_enter_s3_refused, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: S4 entry refused (no hibernate pipeline)",
                            test_acpi_enter_s4_refused, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: S5 support flag matches parsed SLP_TYPa",
                            test_acpi_s5_query_self_consistent, TEST_CAT_BOOT);
}

#else
void test_register_acpi_power(void) {}
#endif
