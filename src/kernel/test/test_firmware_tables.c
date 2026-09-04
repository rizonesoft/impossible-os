/* ============================================================================
 * test_firmware_tables.c -- TODO-04 firmware table catalog unit tests
 *
 * Covers the catalog API (GUID/name/owner lookup, unknown-GUID negative
 * case) plus the range/checksum validator (firmware_table_validate_all).
 * Conformance-profile tests are owned by the conformance-profile section.
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/firmware_tables.h"
#include "kernel/firmware_quirks.h"
#include "kernel/boot_info.h"
#include "kernel/uefi_config.h"

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
             * says UNKNOWN_PROFILE, not VALIDATED. The firmware-region
             * oracle adds a second valid outcome: DEGRADED with
             * RANGE_UNMAPPED when the table sits in BootServicesData/Code
             * or LoaderData/Code (PMM reclaims those in Phase 0). Range
             * precedence is correct -- never reaches profile classification. */
            saw_nonzero_unknown = 1;
            if (e->status != FW_STATUS_UNKNOWN_PROFILE &&
                !(e->status == FW_STATUS_DEGRADED &&
                  e->degraded_reason == FW_DEGRADED_RANGE_UNMAPPED)) {
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
 * not collapse them all to the first SSDT. A signature-keyed lookup
 * loses every duplicate; this test guards against that regression. */
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

/* ---- Range and checksum validator (firmware_table_validate_all) -------- */

/* Synthesised ACPI SDT laid out exactly per the spec (36-byte header
 * starting with the signature, length, revision, checksum, OEM IDs, and
 * creator IDs).  Only signature and length matter for validation; the
 * checksum byte is tuned so the 8-bit byte sum is zero. */
struct test_sdt {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};

/* Build an entry pointing at `bytes` of `size` and source FW_SOURCE_ACPI_SDT
 * with status pre-set to VALIDATED so the validator can downgrade it. */
static void build_sdt_entry(struct firmware_table_entry *e,
                            const void *bytes, uint32_t size)
{
    /* Zero everything by hand so this stays libc-free. */
    uint8_t *p = (uint8_t *)e;
    for (uint32_t i = 0; i < sizeof(*e); i++) p[i] = 0;
    e->phys_addr = (uintptr_t)bytes;
    e->size = size;
    e->source = FW_SOURCE_ACPI_SDT;
    e->status = FW_STATUS_VALIDATED;
    e->degraded_reason = FW_DEGRADED_NONE;
    e->name[0] = 'X'; e->name[1] = '\0';
    e->owner[0] = 'A'; e->owner[1] = 'C'; e->owner[2] = 'P';
    e->owner[3] = 'I'; e->owner[4] = '\0';
}

static void seal_checksum(uint8_t *bytes, uint32_t len, uint32_t cs_offset)
{
    bytes[cs_offset] = 0;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum = (uint8_t)(sum + bytes[i]);
    bytes[cs_offset] = (uint8_t)(0u - sum);
}

/* Find the first CONVENTIONAL mmap descriptor with at least one frame of
 * room past its base.  Returns 0 if none exists. */
static uintptr_t conventional_addr_in_mmap(void)
{
    for (uint32_t i = 0; i < g_boot_info.mmap_count; i++) {
        const struct boot_mmap_entry *m = &g_boot_info.mmap[i];
        if (m->uefi_memory_type == UEFI_MMAP_CONVENTIONAL &&
            m->length >= 0x1000)
            return (uintptr_t)(m->base_addr + 0x100);
    }
    return 0;
}

/* ---- Synthetic-buffer test cases --------------------------------------- */

static void test_validate_acpi_sdt_clean(void)
{
    static struct test_sdt sdt;
    /* Make a clean SDT: sig "TEST", length=sizeof, revision=1. */
    sdt.signature[0] = 'T'; sdt.signature[1] = 'E';
    sdt.signature[2] = 'S'; sdt.signature[3] = 'T';
    sdt.length = sizeof(sdt);
    sdt.revision = 1;
    sdt.oem_id[0] = 'I'; sdt.oem_id[1] = 'M'; sdt.oem_id[2] = 'P';
    sdt.oem_table_id[0] = 'T'; sdt.oem_table_id[1] = 'S';
    seal_checksum((uint8_t *)&sdt, sizeof(sdt),
                  __builtin_offsetof(struct test_sdt, checksum));

    struct firmware_table_entry e;
    build_sdt_entry(&e, &sdt, sizeof(sdt));
    firmware_table_validate_one_for_test(&e, /*bypass_range=*/1);
    TEST_ASSERT_EQ(e.status, FW_STATUS_VALIDATED,
                   "well-formed SDT keeps VALIDATED status");
    TEST_ASSERT_EQ(e.degraded_reason, FW_DEGRADED_NONE,
                   "clean SDT carries no degraded reason");
}

static void test_validate_acpi_sdt_checksum_fail(void)
{
    static struct test_sdt sdt;
    sdt.signature[0] = 'B'; sdt.signature[1] = 'A';
    sdt.signature[2] = 'D'; sdt.signature[3] = 'C';
    sdt.length = sizeof(sdt);
    sdt.revision = 1;
    seal_checksum((uint8_t *)&sdt, sizeof(sdt),
                  __builtin_offsetof(struct test_sdt, checksum));
    /* Corrupt one byte AFTER sealing so the sum no longer balances. */
    ((uint8_t *)&sdt)[16] = (uint8_t)(((uint8_t *)&sdt)[16] ^ 0x55);

    struct firmware_table_entry e;
    build_sdt_entry(&e, &sdt, sizeof(sdt));
    firmware_table_validate_one_for_test(&e, /*bypass_range=*/1);
    TEST_ASSERT_EQ(e.status, FW_STATUS_DEGRADED,
                   "corrupt SDT downgrades to DEGRADED");
    TEST_ASSERT_EQ(e.degraded_reason, FW_DEGRADED_CHECKSUM_FAIL,
                   "corrupt SDT carries CHECKSUM_FAIL reason");
}

static void test_validate_acpi_sdt_length_below_header(void)
{
    static struct test_sdt sdt;
    sdt.length = sizeof(sdt);
    /* Catalog size declared smaller than the SDT header itself. */
    struct firmware_table_entry e;
    build_sdt_entry(&e, &sdt, 16);
    firmware_table_validate_one_for_test(&e, /*bypass_range=*/1);
    TEST_ASSERT_EQ(e.status, FW_STATUS_DEGRADED,
                   "size below header downgrades");
    TEST_ASSERT_EQ(e.degraded_reason, FW_DEGRADED_LENGTH_BAD,
                   "size below header reports LENGTH_BAD");
}

static void test_validate_acpi_sdt_length_mismatch(void)
{
    static struct test_sdt sdt;
    sdt.signature[0] = 'M'; sdt.signature[1] = 'I';
    sdt.signature[2] = 'S'; sdt.signature[3] = 'M';
    sdt.length = sizeof(sdt) + 8;  /* header claims 8 bytes more than catalog */
    sdt.revision = 1;
    seal_checksum((uint8_t *)&sdt, sizeof(sdt),
                  __builtin_offsetof(struct test_sdt, checksum));

    struct firmware_table_entry e;
    build_sdt_entry(&e, &sdt, sizeof(sdt));
    firmware_table_validate_one_for_test(&e, /*bypass_range=*/1);
    TEST_ASSERT_EQ(e.status, FW_STATUS_DEGRADED,
                   "header.length != catalog size downgrades");
    TEST_ASSERT_EQ(e.degraded_reason, FW_DEGRADED_LENGTH_BAD,
                   "size mismatch reports LENGTH_BAD");
}

static void test_validate_range_unmapped(void)
{
    uintptr_t bad = conventional_addr_in_mmap();
    if (bad == 0) {
        TEST_SKIP("no CONVENTIONAL mmap entry to probe");
        return;
    }
    struct firmware_table_entry e;
    build_sdt_entry(&e, (const void *)bad, 64);
    /* This test specifically exercises the range check, so do NOT bypass. */
    firmware_table_validate_one_for_test(&e, /*bypass_range=*/0);
    TEST_ASSERT_EQ(e.status, FW_STATUS_DEGRADED,
                   "phys_addr in CONVENTIONAL memory downgrades");
    TEST_ASSERT_EQ(e.degraded_reason, FW_DEGRADED_RANGE_UNMAPPED,
                   "out-of-firmware-mmap reports RANGE_UNMAPPED");
}

static void test_validate_null_phys_addr(void)
{
    struct firmware_table_entry e;
    build_sdt_entry(&e, (const void *)0, 64);
    firmware_table_validate_one_for_test(&e, /*bypass_range=*/0);
    TEST_ASSERT_EQ(e.status, FW_STATUS_DEGRADED,
                   "NULL phys_addr downgrades");
    TEST_ASSERT_EQ(e.degraded_reason, FW_DEGRADED_NULL_POINTER,
                   "NULL phys_addr reports NULL_POINTER");
}

/* Defensive cap: an oversized firmware-declared length must be
 * rejected with LENGTH_BAD before fw_sum8_is_zero scans it. Guards the
 * Codex perf-review finding that an unbounded byte-sum loop in pre-sti
 * boot could stall startup if a hostile or corrupt firmware advertises
 * a multi-MB length.
 *
 * Two-test invariant lock:
 *   1. SDT path: catalog size > FW_SDT_LENGTH_MAX must degrade with
 *      LENGTH_BAD via the size > cap branch. (Catalog size cap.)
 *   2. FPDT path: build a small buffer whose header.length claims an
 *      oversized span; buffer is sized just enough for the SDT header
 *      so a cap regression that moved the FPDT check AFTER
 *      fw_sum8_is_zero would force a kernel overread (test fails
 *      loudly) instead of silently passing. (Firmware-declared cap.) */
static void test_validate_oversized_length_capped_sdt(void)
{
    static struct test_sdt sdt;
    sdt.signature[0] = 'B'; sdt.signature[1] = 'I';
    sdt.signature[2] = 'G'; sdt.signature[3] = 'X';
    sdt.length = sizeof(sdt);
    sdt.revision = 1;
    seal_checksum((uint8_t *)&sdt, sizeof(sdt),
                  __builtin_offsetof(struct test_sdt, checksum));

    struct firmware_table_entry e;
    build_sdt_entry(&e, &sdt, 0x200000u);  /* 2 MiB > FW_SDT_LENGTH_MAX */
    firmware_table_validate_one_for_test(&e, /*bypass_range=*/1);
    TEST_ASSERT_EQ(e.status, FW_STATUS_DEGRADED,
                   "oversized SDT catalog size downgrades before any byte scan");
    TEST_ASSERT_EQ(e.degraded_reason, FW_DEGRADED_LENGTH_BAD,
                   "oversized SDT reports LENGTH_BAD");
}

/* FPDT path: a hostile firmware that advertises an oversized
 * h->length must be rejected by the cap BEFORE fw_sum8_is_zero loops
 * over the claimed span. The buffer is exactly sizeof(struct test_sdt)
 * bytes (36) so a cap regression would force the byte sum to read past
 * the end of the buffer and either fault or trip the test runner's
 * boundary detection -- in either case, the regression cannot pass
 * silently. Buffer is in BSS so it is naturally zeroed; sdt.length is
 * the only non-zero header field. */
static void test_validate_oversized_length_capped_fpdt(void)
{
    static struct test_sdt sdt;
    /* Mark the buffer as FPDT-shaped, claim length = 0x10000 which is
     * 16 * FW_FPDT_LENGTH_MAX so the cap is unambiguous. */
    sdt.signature[0] = 'F'; sdt.signature[1] = 'P';
    sdt.signature[2] = 'D'; sdt.signature[3] = 'T';
    sdt.length = 0x10000u;  /* 64 KiB -- well over 4 KiB cap */
    sdt.revision = 1;
    /* Do NOT seal_checksum: we want the cap to fire first; the test
     * proves the cap rejects BEFORE fw_sum8_is_zero would have run
     * over 64 KiB of memory past the end of this 36-byte buffer. */

    struct firmware_table_entry e;
    /* Manual entry build for FPDT cfg-table source. */
    uint8_t *p = (uint8_t *)&e;
    for (uint32_t i = 0; i < sizeof(e); i++) p[i] = 0;
    e.phys_addr = (uintptr_t)&sdt;
    e.size = 0;  /* cfg-table source: size unknown at catalog time */
    e.source = FW_SOURCE_UEFI_CFG_TABLE;
    e.status = FW_STATUS_UNKNOWN_PROFILE;
    e.degraded_reason = FW_DEGRADED_NONE;
    e.name[0] = 'F'; e.name[1] = 'P'; e.name[2] = 'D';
    e.name[3] = 'T'; e.name[4] = '\0';
    e.owner[0] = 'F'; e.owner[1] = 'P'; e.owner[2] = 'D';
    e.owner[3] = 'T'; e.owner[4] = '\0';

    firmware_table_validate_one_for_test(&e, /*bypass_range=*/1);
    TEST_ASSERT_EQ(e.status, FW_STATUS_DEGRADED,
                   "oversized FPDT firmware-declared length downgrades");
    TEST_ASSERT_EQ(e.degraded_reason, FW_DEGRADED_LENGTH_BAD,
                   "oversized FPDT reports LENGTH_BAD via cap");
}

/* Promotion contract: an UNKNOWN_PROFILE entry that passes a full
 * format-specific validator (here: ACPI SDT format) must end VALIDATED
 * so downstream consumers can distinguish "fully checked by the range
 * + checksum validator" from "only cataloged". This guards the Codex
 * re-adversarial finding about UNKNOWN -> VALIDATED promotion. */
static void test_validate_promotes_unknown_to_validated(void)
{
    static struct test_sdt sdt;
    sdt.signature[0] = 'P'; sdt.signature[1] = 'R';
    sdt.signature[2] = 'O'; sdt.signature[3] = 'M';
    sdt.length = sizeof(sdt);
    sdt.revision = 1;
    seal_checksum((uint8_t *)&sdt, sizeof(sdt),
                  __builtin_offsetof(struct test_sdt, checksum));

    struct firmware_table_entry e;
    build_sdt_entry(&e, &sdt, sizeof(sdt));
    /* Override the helper's VALIDATED default: simulate an UNKNOWN
     * cfg-table entry that catalog left for the validator to confirm. */
    e.status = FW_STATUS_UNKNOWN_PROFILE;
    e.degraded_reason = FW_DEGRADED_NONE;
    firmware_table_validate_one_for_test(&e, /*bypass_range=*/1);
    TEST_ASSERT_EQ(e.status, FW_STATUS_VALIDATED,
                   "clean full-format pass promotes UNKNOWN_PROFILE to VALIDATED");
}

/* Idempotent downgrade: a second validate pass on a DEGRADED entry must
 * not flip the status back to VALIDATED, even if the underlying buffer
 * happens to look healthy.  This is the one-way contract. */
static void test_validate_one_way_downgrade(void)
{
    static struct test_sdt sdt;
    sdt.signature[0] = 'I'; sdt.signature[1] = 'D';
    sdt.signature[2] = 'M'; sdt.signature[3] = 'P';
    sdt.length = sizeof(sdt);
    sdt.revision = 1;
    seal_checksum((uint8_t *)&sdt, sizeof(sdt),
                  __builtin_offsetof(struct test_sdt, checksum));

    /* Pre-degrade by hand and confirm the validator does not re-promote. */
    struct firmware_table_entry e;
    build_sdt_entry(&e, &sdt, sizeof(sdt));
    e.status = FW_STATUS_DEGRADED;
    e.degraded_reason = FW_DEGRADED_RANGE_UNMAPPED;
    firmware_table_validate_one_for_test(&e, /*bypass_range=*/1);
    TEST_ASSERT_EQ(e.status, FW_STATUS_DEGRADED,
                   "validator preserves prior DEGRADED status");
    TEST_ASSERT_EQ(e.degraded_reason, FW_DEGRADED_RANGE_UNMAPPED,
                   "validator preserves prior degraded reason");
}

/* ---- Live catalog invariants after firmware_tables_init ---------------- */

static void test_validate_all_clean_ovmf_zero_degraded(void)
{
    /* On any sane firmware (OVMF, VirtualBox, real hardware), the catalog
     * must have produced zero NEW CHECKSUM_FAIL or LENGTH_BAD degraded
     * entries beyond the catalog-time NULL_POINTER cases.  Those reasons
     * indicate a real platform issue worth investigating.
     *
     * RANGE_UNMAPPED is now legitimately expected on OVMF + Hyper-V +
     * bare metal: the firmware-region oracle (`fw_mmap_contains`)
     * rejects `UEFI_MMAP_BOOT_SERVICES_CODE/DATA` post-PMM-reclaim
     * because those pages may have been overwritten by the kernel
     * allocator between Phase 0 and the catalog walk in Phase 1.
     * Modern OVMF places HOB list / MAT / properties tables in
     * BootServicesData, so 5-7 catalog entries flag as RANGE_UNMAPPED
     * on a normal boot.  Skip the RANGE_UNMAPPED reason from this
     * "unexpected degradation" check; the dedicated post-reclaim
     * oracle test covers the rejection-path correctness. */
    uint32_t n = firmware_table_count();
    int saw_unexpected = 0;
    for (uint32_t i = 0; i < n; i++) {
        const struct firmware_table_entry *e = firmware_table_get(i);
        if (!e) continue;
        if (e->status != FW_STATUS_DEGRADED) continue;
        if (e->degraded_reason == FW_DEGRADED_CHECKSUM_FAIL ||
            e->degraded_reason == FW_DEGRADED_LENGTH_BAD) {
            saw_unexpected = 1;
        }
    }
    TEST_ASSERT(!saw_unexpected,
                "no checksum/length validator-found degradations on a clean firmware boot");
}

/* ---- Catalog promotion API for deferred provider oracles --------------- */

/* firmware_table_promote_to_validated is one-way: UNKNOWN_PROFILE ->
 * VALIDATED only. DEGRADED entries must stay DEGRADED; VALIDATED
 * entries must stay VALIDATED with no spurious return-true; missing
 * names must return 0. Keeps the catalog status consistent with the
 * Boot\Firmware platform decision after dtb_init succeeds. */
static void test_firmware_table_promote_unknown_to_validated(void)
{
    /* Find an UNKNOWN_PROFILE entry on the live catalog -- typically
     * MAT, RtProps, Conform, or a vendor GUID labelled "uefi-cfg". */
    uint32_t n = firmware_table_count();
    int found = 0;
    char target[FIRMWARE_TABLE_NAME_MAX];
    for (uint32_t i = 0; i < n; i++) {
        const struct firmware_table_entry *e = firmware_table_get(i);
        if (!e) continue;
        if (e->status != FW_STATUS_UNKNOWN_PROFILE) continue;
        /* Skip the placeholder name -- multiple cfg-table entries
         * share it and we want a unique-name target. */
        if (str_eq_local("uefi-cfg", e->name)) continue;
        if (e->name[0] == '\0') continue;
        for (uint32_t j = 0; j < FIRMWARE_TABLE_NAME_MAX; j++)
            target[j] = e->name[j];
        found = 1;
        break;
    }
    if (!found) {
        TEST_SKIP("no UNKNOWN_PROFILE entry with unique name available");
        return;
    }
    int promoted = firmware_table_promote_to_validated(target);
    TEST_ASSERT(promoted == 1,
                "first promote of an UNKNOWN_PROFILE entry returns 1");
    const struct firmware_table_entry *e =
        firmware_table_lookup_name(target);
    TEST_ASSERT(e != (const struct firmware_table_entry *)0,
                "promoted entry still discoverable by name");
    TEST_ASSERT_EQ(e->status, FW_STATUS_VALIDATED,
                   "status transitioned to FW_STATUS_VALIDATED");
    /* Second promote must be a no-op (already VALIDATED). */
    promoted = firmware_table_promote_to_validated(target);
    TEST_ASSERT(promoted == 0,
                "second promote of a VALIDATED entry returns 0");
}

static void test_firmware_table_promote_unknown_name_returns_zero(void)
{
    int promoted =
        firmware_table_promote_to_validated("does-not-exist-anywhere");
    TEST_ASSERT(promoted == 0,
                "promote of an absent name returns 0");
}

static void test_firmware_table_promote_null_returns_zero(void)
{
    int promoted = firmware_table_promote_to_validated((const char *)0);
    TEST_ASSERT(promoted == 0,
                "promote of NULL name returns 0 (no deref)");
}

/* ---- Conformance profile tests (UEFI 2.10 section 4.6.5) ---------------- */

static int conf_strs_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

static void test_conformance_has_profile_oor_id_returns_zero(void)
{
    /* Defensive index check on the public has_profile API.  Out-of-
     * range id must return 0 cleanly, never read past s_profile_present. */
    TEST_ASSERT_EQ(uefi_conformance_has_profile((enum uefi_conformance_profile_id)99),
                   0,
        "has_profile with OOR id returns 0");
    TEST_ASSERT_EQ(uefi_conformance_has_profile((enum uefi_conformance_profile_id)
                                                UEFI_PROFILE_ID__COUNT),
                   0,
        "has_profile with sentinel COUNT id returns 0");
}

static void test_conformance_name_returns_nonempty(void)
{
    /* Whatever the live ECPT state, the display name is never NULL
     * and never empty -- callers can print it directly. */
    const char *name = uefi_conformance_name();
    TEST_ASSERT(name != (const char *)0,
        "uefi_conformance_name() returns non-NULL");
    TEST_ASSERT(name[0] != '\0',
        "uefi_conformance_name() returns non-empty string");
}

static void test_conformance_pc_contradiction_arch_gate(void)
{
    /* On x86_64, the contradiction can only fire iff EBBR is present;
     * if EBBR is NOT present, the contradiction must be 0. */
    int has_ebbr = uefi_conformance_has_profile(UEFI_PROFILE_ID_EBBR);
    int contradiction = uefi_conformance_pc_contradiction();
    if (!has_ebbr) {
        TEST_ASSERT_EQ(contradiction, 0,
            "no EBBR -> no contradiction");
    } else {
        /* EBBR present + x86_64 build -> contradiction must fire. */
        TEST_ASSERT_EQ(contradiction, 1,
            "EBBR + x86_64 -> contradiction");
    }
}

static void test_conformance_omit_policy_default_deny(void)
{
    /* allows_omit_pc_tables() returns 1 ONLY when EBBR is present.
     * Default-deny is the safer policy when no profile matched. */
    int has_ebbr = uefi_conformance_has_profile(UEFI_PROFILE_ID_EBBR);
    int allows = uefi_conformance_allows_omit_pc_tables();
    TEST_ASSERT_EQ(allows, has_ebbr,
        "allows_omit_pc_tables() == has_profile(EBBR)");
}

static void test_conformance_level_consistent_with_presence(void)
{
    /* Backward-compat scalar level must agree with presence flags:
     *   UEFI Spec present -> FULL
     *   else EBBR present -> EBBR
     *   else (table absent OR table present-but-no-match) ->
     *     FULL (absent path) or UNKNOWN (parsed path).
     * This test validates the "single source of truth" property:
     * level cannot disagree with what has_profile() reports. */
    int level = uefi_conformance_level();
    int has_uefi = uefi_conformance_has_profile(UEFI_PROFILE_ID_UEFI_SPEC);
    int has_ebbr = uefi_conformance_has_profile(UEFI_PROFILE_ID_EBBR);

    if (has_uefi) {
        TEST_ASSERT_EQ(level, UEFI_CONFORM_FULL,
            "UEFI Spec presence -> level FULL");
    } else if (has_ebbr) {
        TEST_ASSERT_EQ(level, UEFI_CONFORM_EBBR,
            "EBBR-only presence -> level EBBR");
    } else {
        /* Either FULL (table absent) or UNKNOWN (table present but
         * no match).  Both are valid; reject the impossible levels. */
        TEST_ASSERT(level == UEFI_CONFORM_FULL || level == UEFI_CONFORM_UNKNOWN,
            "no-match -> level FULL or UNKNOWN, never EBBR");
    }
}

static void test_conformance_name_no_match_text_known(void)
{
    /* Spot-check the "no match" name strings so a future rename is
     * a single named failure rather than silent display drift. */
    const char *name = uefi_conformance_name();
    int has_uefi = uefi_conformance_has_profile(UEFI_PROFILE_ID_UEFI_SPEC);
    int has_ebbr = uefi_conformance_has_profile(UEFI_PROFILE_ID_EBBR);
    if (!has_uefi && !has_ebbr) {
        TEST_ASSERT(conf_strs_eq(name, "Full UEFI (assumed)") ||
                    conf_strs_eq(name, "unknown"),
            "no-match name is one of the documented strings");
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
    test_suite_register_cat("FW: validator clean SDT stays VALIDATED",
                            test_validate_acpi_sdt_clean,
                            TEST_CAT_BOOT);
    test_suite_register_cat("FW: validator detects checksum corruption",
                            test_validate_acpi_sdt_checksum_fail,
                            TEST_CAT_BOOT);
    test_suite_register_cat("FW: validator detects size below header",
                            test_validate_acpi_sdt_length_below_header,
                            TEST_CAT_BOOT);
    test_suite_register_cat("FW: validator detects header.length mismatch",
                            test_validate_acpi_sdt_length_mismatch,
                            TEST_CAT_BOOT);
    test_suite_register_cat("FW: validator detects range outside firmware mmap",
                            test_validate_range_unmapped,
                            TEST_CAT_BOOT);
    test_suite_register_cat("FW: validator detects NULL phys_addr",
                            test_validate_null_phys_addr,
                            TEST_CAT_BOOT);
    test_suite_register_cat("FW: validator one-way downgrade",
                            test_validate_one_way_downgrade,
                            TEST_CAT_BOOT);
    test_suite_register_cat("FW: validator promotes UNKNOWN_PROFILE on full pass",
                            test_validate_promotes_unknown_to_validated,
                            TEST_CAT_BOOT);
    test_suite_register_cat("FW: validator caps oversized SDT catalog size",
                            test_validate_oversized_length_capped_sdt,
                            TEST_CAT_BOOT);
    test_suite_register_cat("FW: validator caps oversized FPDT declared length",
                            test_validate_oversized_length_capped_fpdt,
                            TEST_CAT_BOOT);
    test_suite_register_cat("FW: validate_all clean firmware has no validator degradations",
                            test_validate_all_clean_ovmf_zero_degraded,
                            TEST_CAT_BOOT);
    test_suite_register_cat("FW: promote UNKNOWN_PROFILE -> VALIDATED",
                            test_firmware_table_promote_unknown_to_validated,
                            TEST_CAT_BOOT);
    test_suite_register_cat("FW: promote unknown name -> 0",
                            test_firmware_table_promote_unknown_name_returns_zero,
                            TEST_CAT_BOOT);
    test_suite_register_cat("FW: promote NULL name -> 0",
                            test_firmware_table_promote_null_returns_zero,
                            TEST_CAT_BOOT);
    test_suite_register_cat("Conform: has_profile OOR id -> 0",
                            test_conformance_has_profile_oor_id_returns_zero,
                            TEST_CAT_BOOT);
    test_suite_register_cat("Conform: name non-empty",
                            test_conformance_name_returns_nonempty,
                            TEST_CAT_BOOT);
    test_suite_register_cat("Conform: pc_contradiction arch-gate",
                            test_conformance_pc_contradiction_arch_gate,
                            TEST_CAT_BOOT);
    test_suite_register_cat("Conform: omit-policy default-deny",
                            test_conformance_omit_policy_default_deny,
                            TEST_CAT_BOOT);
    test_suite_register_cat("Conform: level vs has_profile consistency",
                            test_conformance_level_consistent_with_presence,
                            TEST_CAT_BOOT);
    test_suite_register_cat("Conform: no-match name is documented",
                            test_conformance_name_no_match_text_known,
                            TEST_CAT_BOOT);

    /* ---- Firmware quirks (firmware_quirks.c) ---------------------------- */
    extern void test_register_firmware_quirks(void);
    test_register_firmware_quirks();
}

/* ============================================================================
 * Firmware quirks tests
 *
 * These tests cover only PURE helpers (parse_disable, name lookup, iter,
 * is_active polarity) -- they do NOT call firmware_quirks_init() or read the
 * live active mask, because that mask is owned by the boot path on the BSP.
 * Calling init() from a test would also be a Test Code Policy violation
 * (test_*.c MUST NOT call live boot infrastructure).
 * ============================================================================ */

static void test_quirks_parse_disable_single(void)
{
    uint32_t m = firmware_quirks_parse_disable("broken_fpdt");
    TEST_ASSERT_EQ(m, FW_QUIRK_BROKEN_FPDT, "single name -> bit");

}

static void test_quirks_parse_disable_multi(void)
{
    uint32_t m = firmware_quirks_parse_disable("broken_fpdt,bogus_mat");
    TEST_ASSERT_EQ(m, FW_QUIRK_BROKEN_FPDT | FW_QUIRK_BOGUS_MAT,
                   "comma-separated -> OR'd bits");

}

static void test_quirks_parse_disable_unknown(void)
{
    uint32_t m = firmware_quirks_parse_disable("unknown_quirk,broken_fpdt");
    TEST_ASSERT_EQ(m, FW_QUIRK_BROKEN_FPDT,
                   "unknown name silently dropped, known kept");

}

static void test_quirks_parse_disable_null_empty(void)
{
    TEST_ASSERT_EQ(firmware_quirks_parse_disable(0), 0u, "NULL -> 0");
    TEST_ASSERT_EQ(firmware_quirks_parse_disable(""), 0u, "empty -> 0");

}

static void test_quirks_parse_disable_whitespace(void)
{
    uint32_t m = firmware_quirks_parse_disable("broken_fpdt , bogus_mat");
    TEST_ASSERT_EQ(m, FW_QUIRK_BROKEN_FPDT | FW_QUIRK_BOGUS_MAT,
                   "whitespace tolerated as separator");

}

static void test_quirks_name_each_bit(void)
{
    /* Exact canonical strings -- a typo in s_quirks[].name would silently
     * break boot.conf override parity AND the JSON quirks_active[] output.
     *
     * Driven from the SAME X-macro the kernel descriptor table and the
     * bootloader qmap are built from, rather than a hand-kept list. A manual
     * list here silently stopped covering new quirks the moment one was added
     * (measured: FW_QUIRK_EC_ECDT_PORTS_SWAPPED landed and neither this check
     * nor the round-trip below noticed), which is precisely the drift these
     * tests exist to catch. */
    /* INDEPENDENT oracle. These literals are written out by hand ON PURPOSE:
     * generating them from the same X-macro that feeds firmware_quirks_name()
     * would move implementation and expectation together, so an accidental
     * rename would break every boot.conf override token while every test
     * stayed green. The generated loop below then guarantees this hand-written
     * list is COMPLETE, which is the property a manual list cannot keep. */
    static const struct { uint32_t bit; const char *name; } pinned[] = {
        { FW_QUIRK_BROKEN_FPDT,           "broken_fpdt"           },
        { FW_QUIRK_BAD_MADT_CHECKSUM,     "bad_madt_checksum"     },
        { FW_QUIRK_GOP_PITCH_LIES,        "gop_pitch_lies"        },
        { FW_QUIRK_BOGUS_MAT,             "bogus_mat"             },
        { FW_QUIRK_USB_HANDOFF_BLACKLIST, "usb_handoff_blacklist" },
        { FW_QUIRK_EC_ECDT_PORTS_SWAPPED, "ec_ecdt_ports_swapped" },
    };
    uint32_t i;

    TEST_ASSERT_EQ((int)(sizeof(pinned) / sizeof(pinned[0])), FW_QUIRK_COUNT,
                   "the pinned canonical-name list covers every defined quirk");
    for (i = 0; i < sizeof(pinned) / sizeof(pinned[0]); i++)
        TEST_ASSERT(str_eq_local(firmware_quirks_name(pinned[i].bit),
                                 pinned[i].name),
                    "canonical name matches the pinned ABI string");
}

/* Round-trip parse_disable(name(bit)) == bit for every defined quirk
 * catches drift between the kernel descriptor table and the bootloader
 * qmap[] (their parallel name copies must stay in lockstep, or the
 * bootloader silently drops the override token). */
static void test_quirks_parse_round_trip_all_bits(void)
{
    /* Generated from the X-macro so "every defined quirk" stays true by
     * construction instead of by remembering to append here. */
    static const uint32_t bits[] = {
#define FW_QUIRK_DEF(id, bit, name) FW_QUIRK_##id,
#include "kernel/firmware_quirks_table.inc"
#undef FW_QUIRK_DEF
    };
    TEST_ASSERT_EQ((int)(sizeof(bits) / sizeof(bits[0])), FW_QUIRK_COUNT,
                   "the round-trip set covers every defined quirk");
    for (uint32_t i = 0; i < sizeof(bits)/sizeof(bits[0]); i++) {
        const char *n = firmware_quirks_name(bits[i]);
        TEST_ASSERT(n != 0, "name(bit) returns canonical string");
        uint32_t parsed = firmware_quirks_parse_disable(n);
        TEST_ASSERT_EQ(parsed, bits[i],
                       "parse_disable(name(bit)) round-trips to bit");
    }
}

static void test_quirks_name_invalid(void)
{
    TEST_ASSERT_EQ((uintptr_t)firmware_quirks_name(0), 0,
                   "zero bit -> NULL");
    TEST_ASSERT_EQ((uintptr_t)firmware_quirks_name(0x03), 0,
                   "non-power-of-two -> NULL");
    TEST_ASSERT_EQ((uintptr_t)firmware_quirks_name(0x800), 0,
                   "out-of-range bit -> NULL");

}

static void test_quirks_is_active_polarity(void)
{
    /* is_active rejects malformed inputs unconditionally even if the live
     * mask carries a bit -- gates on power-of-two + nonzero. */
    TEST_ASSERT_EQ(firmware_quirks_is_active(0), 0,
                   "zero bit -> 0");
    TEST_ASSERT_EQ(firmware_quirks_is_active(0x03), 0,
                   "non-power-of-two -> 0");

}

void test_register_firmware_quirks(void)
{
    test_suite_register_cat("Quirks: parse single name",
                            test_quirks_parse_disable_single, TEST_CAT_BOOT);
    test_suite_register_cat("Quirks: parse comma list",
                            test_quirks_parse_disable_multi, TEST_CAT_BOOT);
    test_suite_register_cat("Quirks: parse unknown drop",
                            test_quirks_parse_disable_unknown, TEST_CAT_BOOT);
    test_suite_register_cat("Quirks: parse NULL/empty -> 0",
                            test_quirks_parse_disable_null_empty, TEST_CAT_BOOT);
    test_suite_register_cat("Quirks: parse whitespace tolerated",
                            test_quirks_parse_disable_whitespace, TEST_CAT_BOOT);
    test_suite_register_cat("Quirks: canonical name for each bit",
                            test_quirks_name_each_bit, TEST_CAT_BOOT);
    test_suite_register_cat("Quirks: parse round-trip all bits",
                            test_quirks_parse_round_trip_all_bits, TEST_CAT_BOOT);
    test_suite_register_cat("Quirks: name rejects malformed bit",
                            test_quirks_name_invalid, TEST_CAT_BOOT);
    test_suite_register_cat("Quirks: is_active polarity guard",
                            test_quirks_is_active_polarity, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
