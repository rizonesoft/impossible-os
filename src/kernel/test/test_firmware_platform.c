/* ============================================================================
 * test_firmware_platform.c -- DTB header validator + platform arbitration
 *
 * Covers the FDT magic / totalsize / version / structure-block walk in
 * dtb_init, the categorisation of /chosen, /memory, /cpu nodes, and the
 * stable name string + getters in firmware_platform.
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/dtb.h"
#include "kernel/firmware_platform.h"
#include "kernel/firmware_tables.h"

/* dtb_reset_for_test comes from kernel/dtb.h. */

/* Wrapper: bypass the firmware-region mmap oracle for the duration of
 * a single dtb_init call against a fixture buffer in kernel BSS. */
static void dtb_init_with_bypass(uintptr_t addr)
{
    firmware_table_set_mmap_bypass_for_test(1);
    dtb_reset_for_test();
    dtb_init(addr);
    firmware_table_set_mmap_bypass_for_test(0);
}

/* Big-endian uint32 store helper for fixture builders. */
static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8);
    p[3] = (uint8_t)(v >>  0);
}

/* Build a minimal valid DTB at `out` containing exactly:
 *   FDT_BEGIN_NODE ""        (root)
 *     FDT_BEGIN_NODE "chosen"
 *     FDT_END_NODE
 *     FDT_BEGIN_NODE "memory@40000000"
 *     FDT_END_NODE
 *     FDT_BEGIN_NODE "cpus"
 *       FDT_BEGIN_NODE "cpu@0"
 *       FDT_END_NODE
 *       FDT_BEGIN_NODE "cpu@1"
 *       FDT_END_NODE
 *     FDT_END_NODE
 *   FDT_END_NODE
 *   FDT_END
 *
 * Expected counts after walk: chosen=1, memory=1, cpu=2.
 *
 * Returns total size used (always <= cap). */
static uint32_t build_minimal_dtb(uint8_t *out, uint32_t cap)
{
    /* Layout: 40-byte header, then structure block immediately after.
     * Strings block is empty (no FDT_PROP entries). */
    if (cap < 256) return 0;
    for (uint32_t i = 0; i < cap; i++) out[i] = 0;

    const uint32_t hdr_size = 40;
    uint8_t *body = out + hdr_size;
    uint32_t off = 0;

    /* Helper: emit a 4-byte token. */
    #define EMIT_TOKEN(tok) do { put_be32(body + off, (tok)); off += 4; } while (0)
    /* Helper: emit BEGIN_NODE with a NUL-terminated name, padded to 4. */
    #define EMIT_BEGIN(name) do {                                        \
        EMIT_TOKEN(0x00000001u);                                         \
        const char *n = (name);                                          \
        uint32_t i = 0;                                                  \
        while (n[i]) { body[off++] = (uint8_t)n[i]; i++; }               \
        body[off++] = 0;                                                 \
        while (off & 0x3u) body[off++] = 0;                              \
    } while (0)

    EMIT_BEGIN("");                /* root */
        EMIT_BEGIN("chosen");
        EMIT_TOKEN(0x00000002u);   /* END_NODE */
        EMIT_BEGIN("memory@40000000");
        EMIT_TOKEN(0x00000002u);   /* END_NODE */
        EMIT_BEGIN("cpus");
            EMIT_BEGIN("cpu@0");
            EMIT_TOKEN(0x00000002u);
            EMIT_BEGIN("cpu@1");
            EMIT_TOKEN(0x00000002u);
        EMIT_TOKEN(0x00000002u);   /* END_NODE for cpus */
    EMIT_TOKEN(0x00000002u);       /* END_NODE for root */
    EMIT_TOKEN(0x00000009u);       /* FDT_END */

    #undef EMIT_BEGIN
    #undef EMIT_TOKEN

    uint32_t struct_size = off;
    uint32_t total       = hdr_size + struct_size;

    /* FDT header in big-endian. */
    put_be32(out +  0, 0xD00DFEEDu);     /* magic */
    put_be32(out +  4, total);           /* totalsize */
    put_be32(out +  8, hdr_size);        /* off_dt_struct */
    put_be32(out + 12, total);           /* off_dt_strings (empty: at end) */
    put_be32(out + 16, hdr_size);        /* off_mem_rsvmap (placeholder) */
    put_be32(out + 20, 17);              /* version */
    put_be32(out + 24, 16);              /* last_comp_version */
    put_be32(out + 28, 0);               /* boot_cpuid_phys */
    put_be32(out + 32, 0);               /* size_dt_strings */
    put_be32(out + 36, struct_size);     /* size_dt_struct */

    return total;
}

/* ---- DTB header validation ---------------------------------------------- */

static void test_dtb_invalid_when_phys_addr_zero(void)
{
    dtb_init_with_bypass(0);
    TEST_ASSERT(dtb_is_valid() == 0,
                "phys_addr=0 must not be reported valid");
}

static void test_dtb_invalid_on_bad_magic(void)
{
    static uint8_t buf[256];
    for (uint32_t i = 0; i < sizeof(buf); i++) buf[i] = 0;
    /* Magic deliberately wrong; rest doesn't matter. */
    put_be32(buf, 0xDEADBEEFu);
    dtb_init_with_bypass((uintptr_t)buf);
    TEST_ASSERT(dtb_is_valid() == 0,
                "bad magic must not be reported valid");
    TEST_ASSERT_EQ(dtb_total_size(), 0,
                   "invalid DTB exposes total_size = 0");
}

static void test_dtb_minimal_blob_valid(void)
{
    static uint8_t buf[1024];
    uint32_t total = build_minimal_dtb(buf, sizeof(buf));
    dtb_init_with_bypass((uintptr_t)buf);
    TEST_ASSERT(dtb_is_valid() != 0,
                "well-formed DTB is reported valid");
    TEST_ASSERT_EQ(dtb_total_size(), total,
                   "total_size matches header.totalsize");
    TEST_ASSERT_EQ(dtb_chosen_count(), 1,
                   "minimal blob has exactly one /chosen node");
    TEST_ASSERT_EQ(dtb_memory_count(), 1,
                   "minimal blob has exactly one /memory@... node");
    TEST_ASSERT_EQ(dtb_cpu_count(), 2,
                   "minimal blob has two /cpu@N nodes");
}

static void test_dtb_invalid_on_oversized_totalsize(void)
{
    static uint8_t buf[64];
    for (uint32_t i = 0; i < sizeof(buf); i++) buf[i] = 0;
    put_be32(buf +  0, 0xD00DFEEDu);
    put_be32(buf +  4, 0xFFFFFFFFu);  /* totalsize overflows 4 MiB cap */
    put_be32(buf + 20, 17);           /* version */
    put_be32(buf + 24, 16);           /* last_comp_version */
    dtb_init_with_bypass((uintptr_t)buf);
    TEST_ASSERT(dtb_is_valid() == 0,
                "totalsize > FDT_TOTALSIZE_MAX must reject");
}

static void test_dtb_invalid_on_unsupported_version(void)
{
    static uint8_t buf[1024];
    uint32_t total = build_minimal_dtb(buf, sizeof(buf));
    /* Override version to v15 (below FDT_FIRST_SUPPORTED_VERSION = 16). */
    put_be32(buf + 20, 15);
    /* keep last_comp_version at 16 -- our gate also rejects on
     * version < first_supported regardless of last_comp. */
    (void)total;
    dtb_init_with_bypass((uintptr_t)buf);
    TEST_ASSERT(dtb_is_valid() == 0,
                "version below FDT_FIRST_SUPPORTED_VERSION rejects");
}

static void test_dtb_invalid_on_truncated_struct(void)
{
    static uint8_t buf[1024];
    uint32_t total = build_minimal_dtb(buf, sizeof(buf));
    /* Lie about size_dt_struct so it overruns totalsize. */
    put_be32(buf + 36, total);  /* size_dt_struct == totalsize */
    dtb_init_with_bypass((uintptr_t)buf);
    TEST_ASSERT(dtb_is_valid() == 0,
                "off_dt_struct + size_dt_struct overrun rejects");
}

/* ---- Firmware-region mmap gate ------------------------------------------ */

/* dtb_init MUST refuse to dereference a phys_addr whose 40-byte header
 * span lies outside the firmware-bearing UEFI memory map. Without the
 * bypass-for-test toggle, kernel BSS buffers should fail the gate. */
static void test_dtb_rejects_addr_outside_firmware_mmap(void)
{
    static uint8_t buf[1024];
    (void)build_minimal_dtb(buf, sizeof(buf));
    /* Deliberately do NOT set the test bypass flag. */
    firmware_table_set_mmap_bypass_for_test(0);
    dtb_reset_for_test();
    dtb_init((uintptr_t)buf);
    TEST_ASSERT(dtb_is_valid() == 0,
                "header outside firmware mmap must not be reported valid");
}

/* ---- Inventory completeness gate (HasDTB) ------------------------------- */

/* Build a structurally valid but empty DTB: root node + FDT_END only.
 * dtb_is_valid will be true (valid header + walked structure block) but
 * memory_count and cpu_count are both zero, so HasDTB must NOT flip on. */
static uint32_t build_empty_dtb(uint8_t *out, uint32_t cap)
{
    if (cap < 64) return 0;
    for (uint32_t i = 0; i < cap; i++) out[i] = 0;
    const uint32_t hdr_size = 40;
    uint8_t *body = out + hdr_size;
    uint32_t off = 0;
    /* root: BEGIN_NODE "" + END_NODE + FDT_END */
    put_be32(body + off, 0x00000001u); off += 4;
    body[off++] = 0;                   /* empty NUL-terminated name */
    while (off & 0x3u) body[off++] = 0;
    put_be32(body + off, 0x00000002u); off += 4;
    put_be32(body + off, 0x00000009u); off += 4;
    uint32_t struct_size = off;
    uint32_t total       = hdr_size + struct_size;
    put_be32(out +  0, 0xD00DFEEDu);
    put_be32(out +  4, total);
    put_be32(out +  8, hdr_size);
    put_be32(out + 12, total);
    put_be32(out + 16, hdr_size);
    put_be32(out + 20, 17);
    put_be32(out + 24, 16);
    put_be32(out + 28, 0);
    put_be32(out + 32, 0);
    put_be32(out + 36, struct_size);
    return total;
}

static void test_dtb_empty_root_has_zero_memory_and_cpu(void)
{
    static uint8_t buf[256];
    (void)build_empty_dtb(buf, sizeof(buf));
    dtb_init_with_bypass((uintptr_t)buf);
    TEST_ASSERT(dtb_is_valid() != 0,
                "empty-root DTB still passes header + walk validation");
    TEST_ASSERT_EQ(dtb_chosen_count(), 0,
                   "empty-root DTB has zero /chosen nodes");
    TEST_ASSERT_EQ(dtb_memory_count(), 0,
                   "empty-root DTB has zero /memory nodes -- HasDTB gate must reject");
    TEST_ASSERT_EQ(dtb_cpu_count(), 0,
                   "empty-root DTB has zero /cpu@N nodes -- HasDTB gate must reject");
}

/* ---- Name prefix-collision regression ----------------------------------- */

/* "memorytest" starts with "memory" but does not match the unit-address
 * convention ("memory" or "memory@..."). The walker must NOT count it
 * as a /memory node; the trailing 't' character disqualifies it. */
static uint32_t build_memorytest_dtb(uint8_t *out, uint32_t cap)
{
    if (cap < 96) return 0;
    for (uint32_t i = 0; i < cap; i++) out[i] = 0;
    const uint32_t hdr_size = 40;
    uint8_t *body = out + hdr_size;
    uint32_t off = 0;

    #define EMIT_TOK(tok) do { put_be32(body + off, (tok)); off += 4; } while (0)
    #define EMIT_NAME(name) do {                                           \
        EMIT_TOK(0x00000001u);                                             \
        const char *n = (name);                                            \
        uint32_t i = 0;                                                    \
        while (n[i]) { body[off++] = (uint8_t)n[i]; i++; }                 \
        body[off++] = 0;                                                   \
        while (off & 0x3u) body[off++] = 0;                                \
    } while (0)

    EMIT_NAME("");           /* root */
        EMIT_NAME("memorytest");
        EMIT_TOK(0x00000002u);
    EMIT_TOK(0x00000002u);
    EMIT_TOK(0x00000009u);   /* FDT_END */
    #undef EMIT_NAME
    #undef EMIT_TOK

    uint32_t struct_size = off;
    uint32_t total       = hdr_size + struct_size;
    put_be32(out +  0, 0xD00DFEEDu);
    put_be32(out +  4, total);
    put_be32(out +  8, hdr_size);
    put_be32(out + 12, total);
    put_be32(out + 16, hdr_size);
    put_be32(out + 20, 17);
    put_be32(out + 24, 16);
    put_be32(out + 28, 0);
    put_be32(out + 32, 0);
    put_be32(out + 36, struct_size);
    return total;
}

static void test_dtb_memorytest_not_counted_as_memory(void)
{
    static uint8_t buf[256];
    (void)build_memorytest_dtb(buf, sizeof(buf));
    dtb_init_with_bypass((uintptr_t)buf);
    TEST_ASSERT(dtb_is_valid() != 0,
                "memorytest fixture passes header + walk validation");
    TEST_ASSERT_EQ(dtb_memory_count(), 0,
                   "\"memorytest\" must NOT match the /memory unit-address convention");
}

/* ---- Unbalanced structure rejection ------------------------------------- */

/* Build a DTB whose structure block has BEGIN BEGIN BEGIN FDT_END (no
 * END_NODE tokens). categorise_name fires on each BEGIN, populating
 * memory_count and cpu_count, but FDT_END at non-zero depth must
 * reject the walk so HasDTB stays false.  Without depth tracking, a
 * hostile blob could pass the inventory gate with bogus child names. */
static uint32_t build_unbalanced_dtb(uint8_t *out, uint32_t cap)
{
    if (cap < 128) return 0;
    for (uint32_t i = 0; i < cap; i++) out[i] = 0;
    const uint32_t hdr_size = 40;
    uint8_t *body = out + hdr_size;
    uint32_t off = 0;

    #define EMIT_TOK(tok) do { put_be32(body + off, (tok)); off += 4; } while (0)
    #define EMIT_NAME(name) do {                                           \
        EMIT_TOK(0x00000001u);                                             \
        const char *n = (name);                                            \
        uint32_t i = 0;                                                    \
        while (n[i]) { body[off++] = (uint8_t)n[i]; i++; }                 \
        body[off++] = 0;                                                   \
        while (off & 0x3u) body[off++] = 0;                                \
    } while (0)

    EMIT_NAME("");           /* root BEGIN, never closed */
    EMIT_NAME("memory@0");   /* memory BEGIN, never closed */
    EMIT_NAME("cpu@0");      /* cpu BEGIN, never closed */
    EMIT_TOK(0x00000009u);   /* FDT_END at depth 3 -- must reject */

    #undef EMIT_NAME
    #undef EMIT_TOK

    uint32_t struct_size = off;
    uint32_t total       = hdr_size + struct_size;
    put_be32(out +  0, 0xD00DFEEDu);
    put_be32(out +  4, total);
    put_be32(out +  8, hdr_size);
    put_be32(out + 12, total);
    put_be32(out + 16, hdr_size);
    put_be32(out + 20, 17);
    put_be32(out + 24, 16);
    put_be32(out + 28, 0);
    put_be32(out + 32, 0);
    put_be32(out + 36, struct_size);
    return total;
}

static void test_dtb_unbalanced_structure_rejects(void)
{
    static uint8_t buf[256];
    (void)build_unbalanced_dtb(buf, sizeof(buf));
    dtb_init_with_bypass((uintptr_t)buf);
    TEST_ASSERT(dtb_is_valid() == 0,
                "FDT_END at non-zero depth must reject the structure walk");
    TEST_ASSERT_EQ(dtb_total_size(), 0,
                   "unbalanced DTB exposes total_size = 0");
    /* Walker accumulated /memory@0 and /cpu@0 BEGIN_NODEs before the
     * depth gate fired at FDT_END; the accumulate-then-commit contract
     * guarantees those local counts never reach the public getters. */
    TEST_ASSERT_EQ(dtb_memory_count(), 0,
                   "rejected walk leaves dtb_memory_count() = 0");
    TEST_ASSERT_EQ(dtb_cpu_count(), 0,
                   "rejected walk leaves dtb_cpu_count() = 0");
    TEST_ASSERT_EQ(dtb_chosen_count(), 0,
                   "rejected walk leaves dtb_chosen_count() = 0");
}

/* ---- Multi-root rejection ----------------------------------------------- */

/* Build a DTB with two top-level BEGIN/END trees:
 *   BEGIN "" / END   (first root closes)
 *   BEGIN "x" / BEGIN "memory@0" / END / BEGIN "cpus" / BEGIN "cpu@0"
 *     / END / END / END                  (second "root" -- malformed)
 *   FDT_END
 * The second BEGIN at depth 0 must be rejected. Without the root_closed
 * latch, the walker would treat "x" as a second root and count its
 * memory@0 and cpus/cpu@0 children. */
static uint32_t build_multi_root_dtb(uint8_t *out, uint32_t cap)
{
    if (cap < 256) return 0;
    for (uint32_t i = 0; i < cap; i++) out[i] = 0;
    const uint32_t hdr_size = 40;
    uint8_t *body = out + hdr_size;
    uint32_t off = 0;

    #define EMIT_TOK(tok) do { put_be32(body + off, (tok)); off += 4; } while (0)
    #define EMIT_NAME(name) do {                                           \
        EMIT_TOK(0x00000001u);                                             \
        const char *n = (name);                                            \
        uint32_t i = 0;                                                    \
        while (n[i]) { body[off++] = (uint8_t)n[i]; i++; }                 \
        body[off++] = 0;                                                   \
        while (off & 0x3u) body[off++] = 0;                                \
    } while (0)

    EMIT_NAME("");                  /* root opens */
    EMIT_TOK(0x00000002u);          /* root closes */
    EMIT_NAME("x");                 /* second BEGIN at depth 0 -- reject */
        EMIT_NAME("memory@0");
        EMIT_TOK(0x00000002u);
        EMIT_NAME("cpus");
            EMIT_NAME("cpu@0");
            EMIT_TOK(0x00000002u);
        EMIT_TOK(0x00000002u);
    EMIT_TOK(0x00000002u);
    EMIT_TOK(0x00000009u);          /* FDT_END */

    #undef EMIT_NAME
    #undef EMIT_TOK

    uint32_t struct_size = off;
    uint32_t total       = hdr_size + struct_size;
    put_be32(out +  0, 0xD00DFEEDu);
    put_be32(out +  4, total);
    put_be32(out +  8, hdr_size);
    put_be32(out + 12, total);
    put_be32(out + 16, hdr_size);
    put_be32(out + 20, 17);
    put_be32(out + 24, 16);
    put_be32(out + 28, 0);
    put_be32(out + 32, 0);
    put_be32(out + 36, struct_size);
    return total;
}

static void test_dtb_multi_root_rejects(void)
{
    static uint8_t buf[320];
    (void)build_multi_root_dtb(buf, sizeof(buf));
    dtb_init_with_bypass((uintptr_t)buf);
    TEST_ASSERT(dtb_is_valid() == 0,
                "second BEGIN_NODE at depth 0 must reject the structure walk");
    TEST_ASSERT_EQ(dtb_memory_count(), 0,
                   "multi-root rejection leaves memory_count=0");
    TEST_ASSERT_EQ(dtb_cpu_count(), 0,
                   "multi-root rejection leaves cpu_count=0");
}

/* ---- Path-aware counting ------------------------------------------------ */

/* Build a balanced but misnested DTB: root -> bogus -> { memory@0,
 * cpu@0 } -> END. Token stream is well-formed (depth balances), but
 * memory@0 and cpu@0 sit at depth 2 under "bogus" instead of their
 * canonical paths (memory at depth 1; cpu@N under /cpus at depth 2).
 * The path-aware categoriser must NOT count these, so HasDTB stays
 * false even though the walker reports valid. Guards Codex re-
 * adversarial M3: balanced misnested layouts must not satisfy the
 * inventory gate. */
static uint32_t build_misnested_dtb(uint8_t *out, uint32_t cap)
{
    if (cap < 192) return 0;
    for (uint32_t i = 0; i < cap; i++) out[i] = 0;
    const uint32_t hdr_size = 40;
    uint8_t *body = out + hdr_size;
    uint32_t off = 0;

    #define EMIT_TOK(tok) do { put_be32(body + off, (tok)); off += 4; } while (0)
    #define EMIT_NAME(name) do {                                           \
        EMIT_TOK(0x00000001u);                                             \
        const char *n = (name);                                            \
        uint32_t i = 0;                                                    \
        while (n[i]) { body[off++] = (uint8_t)n[i]; i++; }                 \
        body[off++] = 0;                                                   \
        while (off & 0x3u) body[off++] = 0;                                \
    } while (0)

    EMIT_NAME("");                  /* root depth 1 */
        EMIT_NAME("bogus");         /* depth 2 */
            EMIT_NAME("memory@0");  /* depth 3, parent="bogus" */
            EMIT_TOK(0x00000002u);
            EMIT_NAME("cpu@0");     /* depth 3, parent="bogus" -- not /cpus */
            EMIT_TOK(0x00000002u);
        EMIT_TOK(0x00000002u);      /* END bogus */
    EMIT_TOK(0x00000002u);          /* END root */
    EMIT_TOK(0x00000009u);          /* FDT_END */

    #undef EMIT_NAME
    #undef EMIT_TOK

    uint32_t struct_size = off;
    uint32_t total       = hdr_size + struct_size;
    put_be32(out +  0, 0xD00DFEEDu);
    put_be32(out +  4, total);
    put_be32(out +  8, hdr_size);
    put_be32(out + 12, total);
    put_be32(out + 16, hdr_size);
    put_be32(out + 20, 17);
    put_be32(out + 24, 16);
    put_be32(out + 28, 0);
    put_be32(out + 32, 0);
    put_be32(out + 36, struct_size);
    return total;
}

static void test_dtb_misnested_memory_cpu_not_counted(void)
{
    static uint8_t buf[256];
    (void)build_misnested_dtb(buf, sizeof(buf));
    dtb_init_with_bypass((uintptr_t)buf);
    TEST_ASSERT(dtb_is_valid() != 0,
                "balanced misnested DTB still passes header + walk validation");
    TEST_ASSERT_EQ(dtb_memory_count(), 0,
                   "memory@0 under /bogus must NOT count as canonical /memory");
    TEST_ASSERT_EQ(dtb_cpu_count(), 0,
                   "cpu@0 outside /cpus must NOT count as canonical /cpus/cpu@N");
}

/* ---- Platform name table ------------------------------------------------ */

static int local_streq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void test_firmware_platform_name_strings(void)
{
    TEST_ASSERT(local_streq(firmware_platform_name(FW_PLATFORM_ACPI),
                             "ACPI"),
                "FW_PLATFORM_ACPI prints \"ACPI\"");
    TEST_ASSERT(local_streq(firmware_platform_name(FW_PLATFORM_DTB),
                             "DTB"),
                "FW_PLATFORM_DTB prints \"DTB\"");
    TEST_ASSERT(local_streq(firmware_platform_name(FW_PLATFORM_HYBRID),
                             "Hybrid"),
                "FW_PLATFORM_HYBRID prints \"Hybrid\"");
    TEST_ASSERT(local_streq(firmware_platform_name(FW_PLATFORM_UNKNOWN),
                             "Unknown"),
                "FW_PLATFORM_UNKNOWN prints \"Unknown\"");
}

/* ---- Live arbitration on QEMU OVMF -------------------------------------- */

/* On any modern UEFI firmware that exposes ACPI 2.0 + SMBIOS3 (QEMU
 * OVMF, VirtualBox EFI, real x86 hardware), firmware_platform_init
 * MUST classify the platform as ACPI. DTB-only x86 boots are not a
 * thing; if this test ever returns DTB or HYBRID on a PC, something
 * is corrupting the catalog. */
static void test_firmware_platform_acpi_on_pc(void)
{
    enum fw_platform p = firmware_platform_get();
    if (!firmware_platform_has_acpi()) {
        TEST_SKIP("no ACPI advertised -- cannot assert ACPI classification");
        return;
    }
    TEST_ASSERT(p == FW_PLATFORM_ACPI || p == FW_PLATFORM_HYBRID,
                "ACPI-bearing firmware classifies as ACPI or HYBRID");
}

static void test_firmware_platform_acpi_version_consistent(void)
{
    if (!firmware_platform_has_acpi()) {
        TEST_SKIP("no ACPI advertised");
        return;
    }
    uint32_t v = firmware_platform_acpi_version();
    TEST_ASSERT(v == 1 || v == 2,
                "AcpiVersion is 1 or 2 when HasACPI=1");
}

/* ---- Registration ------------------------------------------------------- */

void test_register_firmware_platform(void)
{
    test_suite_register_cat("DTB: phys_addr=0 invalid",
                            test_dtb_invalid_when_phys_addr_zero,
                            TEST_CAT_BOOT);
    test_suite_register_cat("DTB: bad magic invalid",
                            test_dtb_invalid_on_bad_magic,
                            TEST_CAT_BOOT);
    test_suite_register_cat("DTB: minimal blob valid + node counts",
                            test_dtb_minimal_blob_valid,
                            TEST_CAT_BOOT);
    test_suite_register_cat("DTB: oversized totalsize rejects",
                            test_dtb_invalid_on_oversized_totalsize,
                            TEST_CAT_BOOT);
    test_suite_register_cat("DTB: unsupported version rejects",
                            test_dtb_invalid_on_unsupported_version,
                            TEST_CAT_BOOT);
    test_suite_register_cat("DTB: truncated structure block rejects",
                            test_dtb_invalid_on_truncated_struct,
                            TEST_CAT_BOOT);
    test_suite_register_cat("DTB: header outside firmware mmap rejects",
                            test_dtb_rejects_addr_outside_firmware_mmap,
                            TEST_CAT_BOOT);
    test_suite_register_cat("DTB: empty-root has zero memory/cpu (HasDTB gate)",
                            test_dtb_empty_root_has_zero_memory_and_cpu,
                            TEST_CAT_BOOT);
    test_suite_register_cat("DTB: memorytest not counted as memory",
                            test_dtb_memorytest_not_counted_as_memory,
                            TEST_CAT_BOOT);
    test_suite_register_cat("DTB: unbalanced BEGIN/END rejects (depth gate)",
                            test_dtb_unbalanced_structure_rejects,
                            TEST_CAT_BOOT);
    test_suite_register_cat("DTB: misnested memory/cpu not counted (path gate)",
                            test_dtb_misnested_memory_cpu_not_counted,
                            TEST_CAT_BOOT);
    test_suite_register_cat("DTB: multi-root rejects (single-root gate)",
                            test_dtb_multi_root_rejects,
                            TEST_CAT_BOOT);
    test_suite_register_cat("FW platform: name table",
                            test_firmware_platform_name_strings,
                            TEST_CAT_BOOT);
    test_suite_register_cat("FW platform: ACPI classification on PC",
                            test_firmware_platform_acpi_on_pc,
                            TEST_CAT_BOOT);
    test_suite_register_cat("FW platform: AcpiVersion consistent",
                            test_firmware_platform_acpi_version_consistent,
                            TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
