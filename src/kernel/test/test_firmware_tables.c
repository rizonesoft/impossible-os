/* ============================================================================
 * test_firmware_tables.c -- TODO-04 firmware table catalog unit tests
 *
 * Covers the catalog API: GUID lookup, name lookup, owner lookup, and the
 * unknown-GUID negative case. Other TODO-04 sections own range, checksum,
 * and conformance-profile tests; this file focuses on the catalog itself.
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/firmware_tables.h"
#include "kernel/boot_info.h"

/* Local string compare so tests do not depend on libc/string.h. */
static int str_eq_local(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* ---- Catalog populated -------------------------------------------------- */

static void test_firmware_table_count_nonzero_when_cfg_table_present(void)
{
    if (g_boot_info.config_table_count == 0) {
        TEST_SKIP("no UEFI config_table entries on this platform");
        return;
    }
    uint32_t n = firmware_table_count();
    TEST_ASSERT(n >= g_boot_info.config_table_count,
                "catalog has at least one entry per config_table[] slot");
}

/* ---- GUID lookup (positive + negative) ---------------------------------- */

static void test_firmware_table_guid_lookup(void)
{
    struct boot_uefi_guid acpi20 = UEFI_GUID_ACPI_20;
    struct boot_uefi_guid acpi10 = UEFI_GUID_ACPI_10;

    const struct firmware_table_entry *e =
        firmware_table_lookup_guid(&acpi20);
    if (!e) e = firmware_table_lookup_guid(&acpi10);

    if (!e) {
        TEST_SKIP("ACPI 2.0/1.0 not advertised in config_table[]");
        return;
    }
    TEST_ASSERT_EQ(e->source, FW_SOURCE_UEFI_CFG_TABLE,
                   "ACPI lookup returns a UEFI_CFG_TABLE entry");
    TEST_ASSERT(e->phys_addr != 0,
                "ACPI catalog entry has non-zero physical address");
}

static void test_firmware_table_unknown_guid_returns_null(void)
{
    /* Synthesised GUID not in any spec; lookup must return NULL. */
    struct boot_uefi_guid bogus = { 0xDEADBEEF, 0xCAFE, 0xBABE,
        { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 } };
    const struct firmware_table_entry *e =
        firmware_table_lookup_guid(&bogus);
    TEST_ASSERT(e == (const struct firmware_table_entry *)0,
                "unknown GUID lookup returns NULL");
}

static void test_firmware_table_null_guid_returns_null(void)
{
    const struct firmware_table_entry *e =
        firmware_table_lookup_guid((const struct boot_uefi_guid *)0);
    TEST_ASSERT(e == (const struct firmware_table_entry *)0,
                "NULL guid lookup returns NULL (no deref)");
}

/* ---- Name lookup -------------------------------------------------------- */

static void test_firmware_table_name_lookup(void)
{
    /* On any UEFI platform exposing ACPI 2.0 we expect the short-name
     * label "ACPI2.0" in the catalog. Skip if neither ACPI variant was
     * found (e.g. pure DTB platform). */
    const struct firmware_table_entry *e =
        firmware_table_lookup_name("ACPI2.0");
    if (!e) e = firmware_table_lookup_name("ACPI1.0");
    if (!e) {
        TEST_SKIP("no ACPI cfg-table entry to look up by name");
        return;
    }
    TEST_ASSERT_EQ(e->source, FW_SOURCE_UEFI_CFG_TABLE,
                   "name lookup returns a UEFI_CFG_TABLE entry");
}

static void test_firmware_table_unknown_name_returns_null(void)
{
    const struct firmware_table_entry *e =
        firmware_table_lookup_name("not-a-real-table");
    TEST_ASSERT(e == (const struct firmware_table_entry *)0,
                "unknown name lookup returns NULL");
}

/* ---- Owner lookup ------------------------------------------------------- */

static void test_firmware_table_owner_count_only(void)
{
    /* count-only mode: max_out=0 with NULL out pointer must succeed. */
    uint32_t n = firmware_table_lookup_owner("ACPI",
                                              (const struct firmware_table_entry **)0,
                                              0);
    /* No assertion on n itself -- some platforms have zero ACPI entries.
     * The contract under test is "count-only call does not crash and
     * returns a sane count". */
    TEST_ASSERT(n <= firmware_table_count(),
                "owner count <= total catalog count");
}

static void test_firmware_table_owner_array_capped(void)
{
    /* Probe how many ACPI-owner entries exist; cap test only meaningful
     * when there are at least 2 (so max_out=1 forces an overrun if the
     * function ignores the cap). */
    uint32_t total = firmware_table_lookup_owner(
        "ACPI", (const struct firmware_table_entry **)0, 0);
    if (total < 2) {
        TEST_SKIP("need >= 2 ACPI-owner entries to prove cap honoured");
        return;
    }

    /* Canary pattern: pre-fill bucket[1] with a non-NULL sentinel that
     * the function MUST NOT overwrite when max_out=1. */
    const struct firmware_table_entry *canary =
        (const struct firmware_table_entry *)(uintptr_t)0xDEADBEEFCAFEBABEull;
    const struct firmware_table_entry *bucket[2];
    bucket[0] = (const struct firmware_table_entry *)0;
    bucket[1] = canary;

    uint32_t got = firmware_table_lookup_owner("ACPI", bucket, 1);

    TEST_ASSERT_EQ(got, total,
                   "lookup_owner returns full match count even when array is capped");
    TEST_ASSERT(bucket[0] != (const struct firmware_table_entry *)0,
                "first slot populated with a real entry");
    TEST_ASSERT(bucket[1] == canary,
                "max_out=1 leaves slot beyond cap untouched (no overrun)");
}

/* ---- Index API ---------------------------------------------------------- */

static void test_firmware_table_index_oob_returns_null(void)
{
    /* index == count is OOB. */
    uint32_t n = firmware_table_count();
    const struct firmware_table_entry *e = firmware_table_get(n);
    TEST_ASSERT(e == (const struct firmware_table_entry *)0,
                "firmware_table_get(count) returns NULL");
}

/* ---- Status assignment for unknown-GUID cfg-table entries -------------- */

/* Header contract: FW_STATUS_VALIDATED means a per-provider helper
 * succeeded. UEFI cfg-table entries with GUIDs the catalog cannot name
 * have NOT been validated by anyone, so they MUST sit in
 * FW_STATUS_UNKNOWN_PROFILE rather than the validated bucket. Codex
 * adversarial round-4 caught the original code marking them validated;
 * this test guards the status contract. */
static void test_firmware_table_unknown_guid_marked_unknown_profile(void)
{
    uint32_t n = firmware_table_count();
    int saw_nonzero_unknown = 0;
    int saw_null_unknown = 0;
    int unknown_violation = 0;
    int null_violation = 0;

    for (uint32_t i = 0; i < n; i++) {
        const struct firmware_table_entry *e = firmware_table_get(i);
        if (!e || e->source != FW_SOURCE_UEFI_CFG_TABLE) continue;
        /* Unknown-GUID entries are stamped with the placeholder name. */
        if (!str_eq_local("uefi-cfg", e->name)) continue;

        if (e->phys_addr == 0) {
            /* NULL VendorTable -- catalog explicitly degrades these,
             * NULL_POINTER precedence over UNKNOWN_PROFILE. */
            saw_null_unknown = 1;
            if (e->status != FW_STATUS_DEGRADED ||
                e->degraded_reason != FW_DEGRADED_NULL_POINTER) {
                null_violation = 1;
            }
        } else {
            /* Nonzero VendorTable, GUID not recognised here -- contract
             * says UNKNOWN_PROFILE, not VALIDATED. */
            saw_nonzero_unknown = 1;
            if (e->status != FW_STATUS_UNKNOWN_PROFILE) {
                unknown_violation = 1;
            }
        }
    }

    if (!saw_nonzero_unknown && !saw_null_unknown) {
        TEST_SKIP("no unknown-GUID UEFI cfg-table entries on this platform");
        return;
    }
    if (saw_nonzero_unknown) {
        TEST_ASSERT(!unknown_violation,
                    "unknown-GUID nonzero entries hold FW_STATUS_UNKNOWN_PROFILE");
    }
    if (saw_null_unknown) {
        TEST_ASSERT(!null_violation,
                    "unknown-GUID NULL-pointer entries hold DEGRADED + NULL_POINTER");
    }
}

/* ---- ACPI duplicate-signature preservation ------------------------------ */

/* Multiple SSDTs are typical ACPI shape. The catalog must store each one
 * as a distinct entry (different phys_addr, possibly different size),
 * not collapse them all to the first SSDT. Codex adversarial review
 * caught a regression where the catalog used signature-keyed lookup and
 * lost every duplicate; this test guards against the regression. */
static void test_firmware_table_acpi_duplicates_preserved(void)
{
    uint32_t n = firmware_table_count();
    uint32_t ssdt_count = 0;
    uintptr_t first_addr = 0;
    int saw_distinct = 0;

    for (uint32_t i = 0; i < n; i++) {
        const struct firmware_table_entry *e = firmware_table_get(i);
        if (!e || e->source != FW_SOURCE_ACPI_SDT) continue;
        /* "SSDT" little-endian = 0x54445353 */
        if (e->signature != 0x54445353u) continue;
        if (ssdt_count == 0) {
            first_addr = e->phys_addr;
        } else if (e->phys_addr != first_addr) {
            saw_distinct = 1;
        }
        ssdt_count++;
    }

    if (ssdt_count < 2) {
        TEST_SKIP("platform has fewer than 2 SSDTs (cannot test duplicates)");
        return;
    }
    TEST_ASSERT(saw_distinct,
                "duplicate SSDT signatures map to distinct phys_addr values");
}

/* ---- Source enum range -------------------------------------------------- */

static void test_firmware_table_source_within_known_range(void)
{
    uint32_t n = firmware_table_count();
    for (uint32_t i = 0; i < n; i++) {
        const struct firmware_table_entry *e = firmware_table_get(i);
        TEST_ASSERT(e != (const struct firmware_table_entry *)0,
                    "in-bounds index returns non-NULL entry");
        TEST_ASSERT(e->source >= FW_SOURCE_UEFI_CFG_TABLE &&
                    e->source <= FW_SOURCE_DTB,
                    "every entry has a recognised FW_SOURCE_* code");
    }
}

/* ---- Registration ------------------------------------------------------- */

void test_register_firmware_tables(void)
{
    test_suite_register_cat("FW: count nonzero (cfg-table)",
                            test_firmware_table_count_nonzero_when_cfg_table_present,
                            TEST_CAT_BOOT);
    test_suite_register_cat("FW: GUID lookup (ACPI)",
                            test_firmware_table_guid_lookup,
                            TEST_CAT_BOOT);
    test_suite_register_cat("FW: unknown GUID -> NULL",
                            test_firmware_table_unknown_guid_returns_null,
                            TEST_CAT_BOOT);
    test_suite_register_cat("FW: NULL GUID -> NULL",
                            test_firmware_table_null_guid_returns_null,
                            TEST_CAT_BOOT);
    test_suite_register_cat("FW: name lookup (ACPI)",
                            test_firmware_table_name_lookup,
                            TEST_CAT_BOOT);
    test_suite_register_cat("FW: unknown name -> NULL",
                            test_firmware_table_unknown_name_returns_null,
                            TEST_CAT_BOOT);
    test_suite_register_cat("FW: owner count-only mode",
                            test_firmware_table_owner_count_only,
                            TEST_CAT_BOOT);
    test_suite_register_cat("FW: owner array cap",
                            test_firmware_table_owner_array_capped,
                            TEST_CAT_BOOT);
    test_suite_register_cat("FW: get(count) -> NULL",
                            test_firmware_table_index_oob_returns_null,
                            TEST_CAT_BOOT);
    test_suite_register_cat("FW: unknown GUID -> UNKNOWN_PROFILE",
                            test_firmware_table_unknown_guid_marked_unknown_profile,
                            TEST_CAT_BOOT);
    test_suite_register_cat("FW: ACPI duplicate SSDTs distinct",
                            test_firmware_table_acpi_duplicates_preserved,
                            TEST_CAT_BOOT);
    test_suite_register_cat("FW: source enum range",
                            test_firmware_table_source_within_known_range,
                            TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
