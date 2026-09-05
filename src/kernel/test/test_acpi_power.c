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
#include "kernel/boot_info.h"
#include "registry.h"

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

/* ---- Discovery: extents proven before the checksum walks them (s38) ----
 *
 * Table discovery is the FIRST reader of firmware memory, so a corrupt length
 * or entry pointer is dereferenced before any later check can reject it. These
 * exercise the two rules discovery now depends on, against synthetic inputs:
 * the derived entry count that bounds the root walk, and the memory-map
 * containment policy that bounds every read of a table body.
 *
 * Assertion messages are deliberately terse: the kernel image has a few
 * hundred bytes of .rodata headroom before it crosses USER_BASE (TODO-33 s3),
 * and test strings land in .rodata like any other.
 */
static void test_acpi_discovery_extent_guard(void)
{
    uint8_t buf[96];
    struct boot_mmap_entry map[2];
    uint32_t i;
    uint8_t sum = 0;

    /* -- Root entry count bounds the walk -- */

    /* A root whose declared length cannot even cover its header. The count is
     * (length - 36) / entry_size, so without the guard this underflows to
     * ~4 G / 8 and the walk scans hundreds of millions of entries out of
     * bounds. Zero means "walk nothing", which is the only safe answer. */
    for (i = 0; i < sizeof(buf); i++)
        buf[i] = 0;
    buf[0] = 'X'; buf[1] = 'S'; buf[2] = 'D'; buf[3] = 'T';
    buf[4] = 8;                                   /* length = 8 < 36 */
    TEST_ASSERT_EQ(acpi_root_entry_count_test(buf, 8u), 0u,
                   "short root: 0");

    /* A well-formed root: 36-byte header + four 8-byte entries. */
    buf[4] = (uint8_t)(36u + 32u);
    TEST_ASSERT_EQ(acpi_root_entry_count_test(buf, 8u), 4u,
                   "root entry count");
    /* Same length read as a 32-bit (RSDT) entry array. */
    TEST_ASSERT_EQ(acpi_root_entry_count_test(buf, 4u), 8u,
                   "rsdt entry count");
    /* A zero entry size cannot divide; it must not fault or admit a walk. */
    TEST_ASSERT_EQ(acpi_root_entry_count_test(buf, 0u), 0u,
                   "zero esize: 0");

    /* -- Memory-map containment bounds every body read -- */

    map[0].base_addr = 0x1000; map[0].length = 0x1000;
    map[0].type = 3; map[0].uefi_memory_type = UEFI_MMAP_ACPI_RECLAIM;
    map[0].attribute = 0;
    map[1].base_addr = 0x3000; map[1].length = 0x1000;
    map[1].type = 1; map[1].uefi_memory_type = UEFI_MMAP_CONVENTIONAL;
    map[1].attribute = 0;

    /* Wholly inside an ACPI-reclaim descriptor: the only accepting shape. */
    TEST_ASSERT_EQ(acpi_extent_in_map_test(map, 2u, 0, 0x1000u, 0x100u), 1,
                   "in ACPI descriptor");

    /* A declared length that runs off the end of the descriptor holding the
     * table. This is the hostile-firmware shape: the header sits in real ACPI
     * memory, so a header-only check passes, and the checksum walk then reads
     * into whatever follows. Rejected on the FULL extent, not the header. */
    TEST_ASSERT_EQ(acpi_extent_in_map_test(map, 2u, 0, 0x1F00u, 0x200u), 0,
                   "past descriptor end");

    /* An entry pointer outside every descriptor, with a COMPLETE map: that is
     * positive evidence the pointer is wrong, not missing evidence. */
    TEST_ASSERT_EQ(acpi_extent_in_map_test(map, 2u, 0, 0x9000u, 0x10u), 0,
                   "outside");

    /* Contained, but in conventional memory -- which pmm_init hands to the
     * allocator, so a table "validated" there can be overwritten afterwards. */
    TEST_ASSERT_EQ(acpi_extent_in_map_test(map, 2u, 0, 0x3000u, 0x10u), 0,
                   "non-ACPI class");

    /* A zero-length extent proves nothing and must not be read as contained. */
    TEST_ASSERT_EQ(acpi_extent_in_map_test(map, 2u, 0, 0x1000u, 0u), 0,
                   "zero-len extent");

    /* Missing evidence, not bad evidence: a truncated or absent map earns the
     * benefit of the doubt, or sleep support would drop on the machines with
     * the most descriptors. Both branches asserted so neither can regress into
     * the refusing direction unnoticed. */
    TEST_ASSERT_EQ(acpi_extent_in_map_test(map, 2u, 1, 0x9000u, 0x10u), 1,
                   "trunc admits");
    TEST_ASSERT_EQ(acpi_extent_in_map_test(map, 0u, 0, 0x9000u, 0x10u), 1,
                   "absent admits");

    /* But missing evidence never overrides evidence we DO have. A table whose
     * header sits at the end of the ACPI descriptor and whose declared length
     * runs on into the adjacent region straddles a boundary the map describes,
     * so it is refused even with the map marked truncated. Accepting it would
     * checksum straight into that neighbour -- device-register reads when the
     * neighbour is MMIO. */
    map[1].base_addr = 0x2000;
    map[1].uefi_memory_type = UEFI_MMAP_MMIO;
    TEST_ASSERT_EQ(acpi_extent_in_map_test(map, 2u, 1, 0x1F00u, 0x200u), 0,
                   "trunc crossing");
    /* The header alone still fits, which is why a header-only check passes it
     * and only the FULL extent test catches the forged length. */
    TEST_ASSERT_EQ(acpi_extent_in_map_test(map, 2u, 1, 0x1F00u, 0x24u), 1,
                   "hdr fits");

    /* But straddling an ADMISSIBLE neighbour is NOT evidence of corruption.
     * The loader coalesces adjacent descriptors only when type AND attribute
     * match, so two abutting ACPI-reclaim ranges differing only in their
     * EFI_MEMORY_* attributes stay split, and a legitimate table spanning them
     * must still be admitted under the truncated-map fallback. Refusing it
     * would fail the FADT and boot the machine single-core with no ACPI. */
    map[1].uefi_memory_type = UEFI_MMAP_ACPI_NVS;
    TEST_ASSERT_EQ(acpi_extent_in_map_test(map, 2u, 1, 0x1F00u, 0x200u), 1,
                   "ok neighbour");
    /* And on a COMPLETE map too. The span is wholly covered by a contiguous
     * run of admissible descriptors, so it is admitted on the evidence rather
     * than on the benefit of the doubt -- a table spanning two ACPI ranges the
     * loader left split is legitimate, and most machines have a complete map. */
    TEST_ASSERT_EQ(acpi_extent_in_map_test(map, 2u, 0, 0x1F00u, 0x200u), 1,
                   "span ok classes");
    /* A span reaching one byte past the covered run is refused: coverage is
     * decided on the WHOLE extent, not on where it starts. */
    TEST_ASSERT_EQ(acpi_extent_in_map_test(map, 2u, 0, 0x1F00u, 0x1101u), 0,
                   "span past run");

    /* -- A checksum that only balances PAST the declared end -- */

    /* 38 bytes whose sum is zero, but a declared length of 37. The validator
     * checksums over exactly `length`, so the 38th byte is not in the sum and
     * the table is refused. Accepting it is the bug this shape exists to
     * catch: the walk would have had to read past the declared end to make
     * the checksum balance. */
    for (i = 0; i < sizeof(buf); i++)
        buf[i] = 0;
    buf[0] = 'D'; buf[1] = 'S'; buf[2] = 'D'; buf[3] = 'T';
    buf[4] = 37;                                  /* declared length */
    buf[37] = 0x5A;                               /* one byte past it */
    for (i = 0; i < 38u; i++)
        sum = (uint8_t)(sum + buf[i]);
    buf[9] = (uint8_t)(0u - sum);                 /* balances over 38, not 37 */
    TEST_ASSERT(acpi_table_valid_test(buf, "DSDT") == 0,
                "checksum past end");

    /* Control: the SAME image balanced over its declared 37 bytes IS accepted,
     * so the refusal above is about the extent and not about the fixture. */
    buf[37] = 0;
    sum = 0;
    buf[9] = 0;
    for (i = 0; i < 37u; i++)
        sum = (uint8_t)(sum + buf[i]);
    buf[9] = (uint8_t)(0u - sum);
    TEST_ASSERT(acpi_table_valid_test(buf, "DSDT") == 1,
                "control: balances");
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

/* ---- Button action policy (section 7) ----------------------------------- */

/* Every in-range stored value survives resolution unchanged. The resolver must
 * NOT be where an unavailable action gets rewritten -- that is
 * acpi_btn_action_available()'s job, and conflating the two is exactly how a
 * configured "sleep" would turn into a shutdown. */
static void test_acpi_btn_resolve_in_range_preserved(void)
{
    uint32_t a;

    for (a = ACPI_BTN_ACTION_IGNORE; a <= ACPI_BTN_ACTION_MAX; a++)
        TEST_ASSERT_EQ(acpi_btn_resolve_action(a, 1, ACPI_BTN_DEFAULT_POWER), a,
                       "in-range action resolves to itself");
}

/* An absent value takes the caller's default, and each button has its own. */
static void test_acpi_btn_resolve_absent_uses_default(void)
{
    TEST_ASSERT_EQ(acpi_btn_resolve_action(0, 0, ACPI_BTN_DEFAULT_POWER),
                   ACPI_BTN_ACTION_SHUTDOWN,
                   "absent power-button value defaults to shutdown");
    TEST_ASSERT_EQ(acpi_btn_resolve_action(0, 0, ACPI_BTN_DEFAULT_SLEEP),
                   ACPI_BTN_ACTION_SLEEP,
                   "absent sleep-button value defaults to sleep");
    /* A present zero is the IGNORE action, not an absent value: the two must not
     * collapse, or configuring "do nothing" would silently become the default. */
    TEST_ASSERT_EQ(acpi_btn_resolve_action(0, 1, ACPI_BTN_DEFAULT_POWER),
                   ACPI_BTN_ACTION_IGNORE,
                   "present zero is the ignore action, not a missing value");
}

/* Out of range is malformed policy, so it takes the default rather than
 * disabling the button. */
static void test_acpi_btn_resolve_out_of_range_uses_default(void)
{
    TEST_ASSERT_EQ(acpi_btn_resolve_action(ACPI_BTN_ACTION_MAX + 1, 1,
                                           ACPI_BTN_DEFAULT_POWER),
                   ACPI_BTN_ACTION_SHUTDOWN,
                   "just past the last action falls back to the default");
    TEST_ASSERT_EQ(acpi_btn_resolve_action(0xFFFFFFFFu, 1,
                                           ACPI_BTN_DEFAULT_POWER),
                   ACPI_BTN_ACTION_SHUTDOWN,
                   "a wildly out-of-range value falls back to the default");
    /* The fallback must be the CALLER's, not a hard-coded shutdown: an
     * implementation that honoured the caller default only for an absent value
     * would pass every assertion above while turning a malformed
     * SleepButtonAction into an unexpected shutdown. */
    TEST_ASSERT_EQ(acpi_btn_resolve_action(ACPI_BTN_ACTION_MAX + 1, 1,
                                           ACPI_BTN_DEFAULT_SLEEP),
                   ACPI_BTN_ACTION_SLEEP,
                   "out-of-range sleep-button value uses the sleep default");
}

/* Availability is a statement about THIS tree: ignore and shutdown can happen,
 * S3/S4/lock cannot until their owning sections ship. */
static void test_acpi_btn_availability(void)
{
    TEST_ASSERT_EQ(acpi_btn_action_available(ACPI_BTN_ACTION_IGNORE), 1,
                   "doing nothing is always available");
    TEST_ASSERT_EQ(acpi_btn_action_available(ACPI_BTN_ACTION_SHUTDOWN), 1,
                   "shutdown is available");
    TEST_ASSERT_EQ(acpi_btn_action_available(ACPI_BTN_ACTION_SLEEP), 0,
                   "S3 is not available: no suspend orchestration");
    TEST_ASSERT_EQ(acpi_btn_action_available(ACPI_BTN_ACTION_HIBERNATE), 0,
                   "S4 is not available: no hibernate orchestration");
    TEST_ASSERT_EQ(acpi_btn_action_available(ACPI_BTN_ACTION_LOCK), 0,
                   "lock is not available: no session to lock");
    TEST_ASSERT_EQ(acpi_btn_action_available(ACPI_BTN_ACTION_MAX + 1), 0,
                   "an out-of-range action is never available");
}

/* The default power-button action must be one that can actually run, or the
 * fallback path would resolve to a refusal and the button would do nothing on a
 * machine with no stored policy. */
static void test_acpi_btn_power_default_is_actionable(void)
{
    TEST_ASSERT_EQ(acpi_btn_action_available(ACPI_BTN_DEFAULT_POWER), 1,
                   "the power-button default must be performable");
}

/* Edge drain: a burst collapses to one delta, and a second drain with no new
 * events reports zero. */
static void test_acpi_btn_drain_edges(void)
{
    uint64_t seen = 0;

    TEST_ASSERT_EQ(acpi_btn_drain(0, &seen), 0ull,
                   "no events yields no work");
    TEST_ASSERT_EQ(acpi_btn_drain(5, &seen), 5ull,
                   "five presses drain as five");
    TEST_ASSERT_EQ(seen, 5ull, "watermark advanced to the count");
    TEST_ASSERT_EQ(acpi_btn_drain(5, &seen), 0ull,
                   "re-draining the same count yields nothing");
}

/* Full width, uncast: a regression that truncated the delta AND the stored
 * watermark to 32 bits would pass every assertion above, then leave a
 * high-count watermark stale and report phantom work on the next drain. */
static void test_acpi_btn_drain_full_width(void)
{
    uint64_t seen = 0;

    TEST_ASSERT_EQ(acpi_btn_drain(0x100000005ull, &seen), 0x100000005ull,
                   "a delta above 2^32 is reported at full width");
    TEST_ASSERT_EQ(seen, 0x100000005ull,
                   "the watermark stores the full-width count");
    TEST_ASSERT_EQ(acpi_btn_drain(0x100000005ull, &seen), 0ull,
                   "re-draining a full-width count yields nothing");
}

/* The reason the counters were widened to 64 bits: the drain must survive a
 * wrap. With a 32-bit counter this interval would have been lost entirely. */
static void test_acpi_btn_drain_wrap(void)
{
    uint64_t seen = 0xFFFFFFFFFFFFFFFEull;

    TEST_ASSERT_EQ(acpi_btn_drain(2, &seen), 4ull,
                   "a wrapped counter still yields the true event count");
    TEST_ASSERT_EQ(seen, 2ull, "watermark follows the wrapped count");
}

/* A NULL watermark is a programming error, not a crash. */
static void test_acpi_btn_drain_null_seen(void)
{
    TEST_ASSERT_EQ(acpi_btn_drain(7, (uint64_t *)0), 0ull,
                   "a NULL watermark drains nothing");
}

/* The gate admits exactly one holder, so a press arriving during a running
 * action cannot stack a second one. */
static void test_acpi_btn_gate_single_entry(void)
{
    volatile uint32_t gate = 0;

    TEST_ASSERT_EQ(acpi_btn_gate_take(&gate), 1, "first take succeeds");
    TEST_ASSERT_EQ(acpi_btn_gate_take(&gate), 0,
                   "second take is refused while held");
    acpi_btn_gate_release(&gate);
    TEST_ASSERT_EQ(acpi_btn_gate_take(&gate), 1, "take succeeds after release");
    acpi_btn_gate_release(&gate);
    TEST_ASSERT_EQ((uint32_t)gate, 0u, "release clears the gate");
    TEST_ASSERT_EQ(acpi_btn_gate_take((volatile uint32_t *)0), 0,
                   "a NULL gate is never taken");
}

/* End-to-end over the dispatcher's whole decision: a burst of presses on one
 * button collapses to exactly ONE action, the other button stays silent, and
 * both watermarks advance. This is the same call the threaded DPC makes, so it
 * covers the drain-and-decide pass rather than its pieces. */
static void test_acpi_btn_plan_burst_collapses(void)
{
    uint64_t pwr_seen = 0;
    uint64_t slp_seen = 0;
    uint32_t pwr = 0;
    uint32_t slp = 0;

    acpi_btn_plan(4, 0, &pwr_seen, &slp_seen,
                  ACPI_BTN_ACTION_SHUTDOWN, ACPI_BTN_ACTION_SLEEP, &pwr, &slp);
    TEST_ASSERT_EQ(pwr, ACPI_BTN_ACTION_SHUTDOWN,
                   "four power presses produce one shutdown action");
    TEST_ASSERT_EQ(slp, ACPI_BTN_ACTION_NONE,
                   "a silent sleep button produces no action");
    TEST_ASSERT_EQ(pwr_seen, 4ull, "power watermark advanced past the burst");
    TEST_ASSERT_EQ(slp_seen, 0ull, "sleep watermark did not move");
}

/* A second pass with no new events must decide nothing, which is what stops the
 * DPC re-running an action every time it is woken. */
static void test_acpi_btn_plan_idle_pass(void)
{
    uint64_t pwr_seen = 0;
    uint64_t slp_seen = 0;
    uint32_t pwr = 0;
    uint32_t slp = 0;

    acpi_btn_plan(2, 3, &pwr_seen, &slp_seen,
                  ACPI_BTN_ACTION_SHUTDOWN, ACPI_BTN_ACTION_IGNORE, &pwr, &slp);
    TEST_ASSERT_EQ(pwr, ACPI_BTN_ACTION_SHUTDOWN, "first pass acts on power");
    TEST_ASSERT_EQ(slp, ACPI_BTN_ACTION_IGNORE, "first pass acts on sleep");

    acpi_btn_plan(2, 3, &pwr_seen, &slp_seen,
                  ACPI_BTN_ACTION_SHUTDOWN, ACPI_BTN_ACTION_IGNORE, &pwr, &slp);
    TEST_ASSERT_EQ(pwr, ACPI_BTN_ACTION_NONE, "second pass decides nothing");
    TEST_ASSERT_EQ(slp, ACPI_BTN_ACTION_NONE, "second pass decides nothing");
}

/* A press that lands DURING an action is recoverable exactly because the
 * watermark is published before the action runs: the count is left ahead of the
 * watermark, so the next pass still finds it. */
static void test_acpi_btn_plan_press_during_action(void)
{
    uint64_t pwr_seen = 0;
    uint64_t slp_seen = 0;
    uint32_t pwr = 0;
    uint32_t slp = 0;

    acpi_btn_plan(1, 0, &pwr_seen, &slp_seen,
                  ACPI_BTN_ACTION_SHUTDOWN, ACPI_BTN_ACTION_SLEEP, &pwr, &slp);
    TEST_ASSERT_EQ(pwr_seen, 1ull, "watermark published before the action ran");

    /* The ISR increments while the action is in flight. */
    acpi_btn_plan(2, 0, &pwr_seen, &slp_seen,
                  ACPI_BTN_ACTION_SHUTDOWN, ACPI_BTN_ACTION_SLEEP, &pwr, &slp);
    TEST_ASSERT_EQ(pwr, ACPI_BTN_ACTION_SHUTDOWN,
                   "a press during an action is found by the next pass");
}

/* Both buttons drain on every pass. Draining lazily would leave the sleep
 * watermark behind and replay its presses later. */
static void test_acpi_btn_plan_drains_both(void)
{
    uint64_t pwr_seen = 0;
    uint64_t slp_seen = 0;
    uint32_t pwr = 0;
    uint32_t slp = 0;

    acpi_btn_plan(3, 7, &pwr_seen, &slp_seen,
                  ACPI_BTN_ACTION_SHUTDOWN, ACPI_BTN_ACTION_SLEEP, &pwr, &slp);
    TEST_ASSERT_EQ(pwr_seen, 3ull, "power watermark drained");
    TEST_ASSERT_EQ(slp_seen, 7ull, "sleep watermark drained on the same pass");
}

/* NULL outputs are a caller error, not a crash, and the watermarks still
 * advance so no pass is silently repeated. */
static void test_acpi_btn_plan_null_outputs(void)
{
    uint64_t pwr_seen = 0;
    uint64_t slp_seen = 0;

    acpi_btn_plan(1, 1, &pwr_seen, &slp_seen,
                  ACPI_BTN_ACTION_SHUTDOWN, ACPI_BTN_ACTION_SLEEP,
                  (uint32_t *)0, (uint32_t *)0);
    TEST_ASSERT_EQ(pwr_seen, 1ull, "watermark advances with NULL outputs");
    TEST_ASSERT_EQ(slp_seen, 1ull, "watermark advances with NULL outputs");
}

/* ACPI_BTN_ACTION_NONE must never collide with a real action code, or a
 * no-events pass would be indistinguishable from a configured action. */
static void test_acpi_btn_none_is_not_an_action(void)
{
    TEST_ASSERT_EQ(ACPI_BTN_ACTION_NONE > ACPI_BTN_ACTION_MAX, 1,
                   "the no-action sentinel is outside the action range");
    TEST_ASSERT_EQ(acpi_btn_action_available(ACPI_BTN_ACTION_NONE), 0,
                   "the no-action sentinel is never performable");
}

/* The policy key must EXIST. The first cut of this test compared the cached
 * action against a resolve of whatever the hive held, which cannot fail: with
 * the key absent both sides evaluate to the same default, so it passed
 * identically whether or not acpi_button_policy_init ever reached the registry
 * -- the exact failure its own comment claimed it caught. Assert the key and
 * both values are present first; that is the claim with a false case. */
static void test_acpi_btn_policy_key_exists(void)
{
    HKEY hk;
    uint32_t raw = 0;

    TEST_ASSERT_EQ(RegOpenKeyEx(HKEY_LOCAL_MACHINE, ACPI_BTN_REG_PATH, 0,
                                KEY_READ, &hk),
                   ERROR_SUCCESS,
                   "the button policy key is seeded at boot");
    TEST_ASSERT_EQ(RegGetDword(hk, ACPI_BTN_REG_POWER, &raw), ERROR_SUCCESS,
                   "PowerButtonAction is present under it");
    TEST_ASSERT_EQ(raw <= ACPI_BTN_ACTION_MAX, 1,
                   "the seeded power action is a valid code");
    raw = 0;
    TEST_ASSERT_EQ(RegGetDword(hk, ACPI_BTN_REG_SLEEP, &raw), ERROR_SUCCESS,
                   "SleepButtonAction is present under it");
    TEST_ASSERT_EQ(raw <= ACPI_BTN_ACTION_MAX, 1,
                   "the seeded sleep action is a valid code");
    RegCloseKey(hk);
}

/* Both cached actions must equal what the hive holds. Meaningful now only
 * because the test above proves the values are really there: this compares the
 * cache against a live read of each value BY NAME, so a typo in either
 * production value name, or a policy_init that read only one of them, fails
 * here instead of silently ignoring user policy. */
static void test_acpi_btn_cached_matches_registry(void)
{
    HKEY hk;
    uint32_t raw = 0;
    int present = 0;

    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, ACPI_BTN_REG_PATH, 0,
                     KEY_READ, &hk) != ERROR_SUCCESS) {
        TEST_SKIP("no button policy key on this build");
        return;
    }
    present = (RegGetDword(hk, ACPI_BTN_REG_POWER, &raw) == ERROR_SUCCESS);
    TEST_ASSERT_EQ(acpi_power_button_action(),
                   acpi_btn_resolve_action(raw, present, ACPI_BTN_DEFAULT_POWER),
                   "cached power action matches the hive value");
    raw = 0;
    present = (RegGetDword(hk, ACPI_BTN_REG_SLEEP, &raw) == ERROR_SUCCESS);
    TEST_ASSERT_EQ(acpi_sleep_button_action(),
                   acpi_btn_resolve_action(raw, present, ACPI_BTN_DEFAULT_SLEEP),
                   "cached sleep action matches the hive value");
    RegCloseKey(hk);
}

/* The dispatch counter is a public diagnostic, so it gets a reader: without one
 * it is a write-only API that no regression could ever catch. It is monotonic
 * and must never be decremented by a read. */
static void test_acpi_btn_dispatch_count_monotonic(void)
{
    uint32_t a = acpi_btn_dispatch_count();
    uint32_t b = acpi_btn_dispatch_count();

    TEST_ASSERT_EQ(b >= a, 1, "the dispatch count never goes backwards");
}

/* The cached actions are always resolved values, whatever the hive held. */
static void test_acpi_btn_cached_actions_in_range(void)
{
    TEST_ASSERT_EQ(acpi_power_button_action() <= ACPI_BTN_ACTION_MAX, 1,
                   "cached power-button action is a valid code");
    TEST_ASSERT_EQ(acpi_sleep_button_action() <= ACPI_BTN_ACTION_MAX, 1,
                   "cached sleep-button action is a valid code");
}

/* ---- Registration ---- */

/* ---- S0ix firmware advertisement (section 10) ---------------------------
 *
 * acpi_fadt_s0ix_capable() is the pure half of the pair, so it can be driven
 * with synthetic tables. The wrapper acpi_s0ix_supported() reads the live
 * fadt_ptr and is therefore whatever this machine's firmware says; it is not
 * asserted to a fixed value here, only to agreeing with the pure function. */

static void test_acpi_s0ix_null_and_short_refuse(void)
{
    struct acpi_fadt f = {0};

    f.flags = ACPI_FADT_FLAG_LOW_POWER_S0;

    /* No table cannot advertise a capability. */
    TEST_ASSERT_EQ(acpi_fadt_s0ix_capable(0), 0, "null");

    /* A table too short to contain the flags dword must refuse BEFORE
     * reading it -- the bit being set in our synthetic struct is exactly the
     * trap: a length-blind read would return 1 here off a table that does
     * not actually carry the field. */
    f.header.length = ACPI_FADT_LEN_FLAGS - 1;
    TEST_ASSERT_EQ(acpi_fadt_s0ix_capable(&f), 0, "short");
}

static void test_acpi_s0ix_reads_bit21(void)
{
    struct acpi_fadt f = {0};

    f.header.length = ACPI_FADT_LEN_FLAGS;

    f.flags = 0;
    TEST_ASSERT_EQ(acpi_fadt_s0ix_capable(&f), 0, "b21 clr");

    f.flags = ACPI_FADT_FLAG_LOW_POWER_S0;
    TEST_ASSERT_EQ(acpi_fadt_s0ix_capable(&f), 1, "b21 set");

    /* Neighbouring bits must not be mistaken for it. Bit 20 is HW_REDUCED,
     * a different capability entirely, and an off-by-one shift would read it
     * instead. */
    f.flags = (1u << 20);
    TEST_ASSERT_EQ(acpi_fadt_s0ix_capable(&f), 0, "b20 no");
    f.flags = (1u << 22);
    TEST_ASSERT_EQ(acpi_fadt_s0ix_capable(&f), 0, "b22 no");

    /* Set among unrelated bits it must still be found. */
    f.flags = 0xFFFFFFFFu;
    TEST_ASSERT_EQ(acpi_fadt_s0ix_capable(&f), 1, "all bits");
}

static void test_acpi_s0ix_wrapper_matches_pure(void)
{
    const struct acpi_fadt *live = acpi_get_fadt();
    struct acpi_fadt f = {0};

    /* The wrapper must forward to its OWN evaluator over the live table. */
    TEST_ASSERT_EQ(acpi_s0ix_supported(), acpi_fadt_s0ix_capable(live),
                   "wrapper");

    /* That equality alone is weak here: no emulator this suite runs on sets
     * bit 21, so both sides are 0 and the assertion would also hold if the
     * wrapper forwarded to any other accessor that is 0 on this platform.
     * Pin the two evaluators APART on synthetic tables so "its own evaluator"
     * has teeth independent of what the host firmware reports. */
    f.header.length = ACPI_FADT_LEN_FLAGS;

    f.flags = ACPI_FADT_FLAG_LOW_POWER_S0;
    TEST_ASSERT_EQ(acpi_fadt_s0ix_capable(&f), 1, "s0ix yes");
    TEST_ASSERT_EQ(acpi_fadt_hw_reduced(&f), 0, "hwr no");

    f.flags = (1u << 20);   /* HW_REDUCED_ACPI, the adjacent bit */
    TEST_ASSERT_EQ(acpi_fadt_s0ix_capable(&f), 0, "s0ix no");
    TEST_ASSERT_EQ(acpi_fadt_hw_reduced(&f), 1, "hwr yes");
}


void test_register_acpi_power(void)
{
    test_suite_register_cat("ACPI: discovery extent guard",
                            test_acpi_discovery_extent_guard, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: button in-range action preserved",
                            test_acpi_btn_resolve_in_range_preserved, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: button absent value uses default",
                            test_acpi_btn_resolve_absent_uses_default, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: button out-of-range uses default",
                            test_acpi_btn_resolve_out_of_range_uses_default, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: button action availability",
                            test_acpi_btn_availability, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: power-button default is actionable",
                            test_acpi_btn_power_default_is_actionable, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: button edge drain collapses a burst",
                            test_acpi_btn_drain_edges, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: button drain is full width",
                            test_acpi_btn_drain_full_width, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: button plan collapses a burst",
                            test_acpi_btn_plan_burst_collapses, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: button plan idle pass decides nothing",
                            test_acpi_btn_plan_idle_pass, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: button press during action is not lost",
                            test_acpi_btn_plan_press_during_action, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: button plan drains both buttons",
                            test_acpi_btn_plan_drains_both, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: button plan tolerates NULL outputs",
                            test_acpi_btn_plan_null_outputs, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: no-action sentinel is not an action",
                            test_acpi_btn_none_is_not_an_action, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: button policy key is seeded",
                            test_acpi_btn_policy_key_exists, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: button dispatch count is monotonic",
                            test_acpi_btn_dispatch_count_monotonic, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: cached button action matches the hive",
                            test_acpi_btn_cached_matches_registry, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: button drain survives counter wrap",
                            test_acpi_btn_drain_wrap, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: button drain rejects NULL watermark",
                            test_acpi_btn_drain_null_seen, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: button gate admits one holder",
                            test_acpi_btn_gate_single_entry, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: cached button actions are valid codes",
                            test_acpi_btn_cached_actions_in_range, TEST_CAT_BOOT);
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
    test_suite_register_cat("ACPI: S0ix null/short",
                            test_acpi_s0ix_null_and_short_refuse, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: S0ix flags bit 21",
                            test_acpi_s0ix_reads_bit21, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: S0ix wrapper",
                            test_acpi_s0ix_wrapper_matches_pure, TEST_CAT_BOOT);
}

#else
void test_register_acpi_power(void) {}
#endif
