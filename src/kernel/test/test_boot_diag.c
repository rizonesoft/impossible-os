/* Unit tests for boot diagnostics (TODO-14). Currently: the bootloader
 * build-identity formatter (section 10) + the shared ISO-8601 helper.
 * The section 1-3 / 4-9 cases ship with their own sections. */

#include "kernel/test/test.h"
#include "kernel/boot_version.h"
#include "kernel/boot_info.h"
#include "kernel/time_iso.h"
#include "libc/string.h"

/* Off-stack fixture: the formatter buffer is 512 bytes. */
static char s_fmt[512];

static int buf_has(const char *hay, uint32_t hlen, const char *needle)
{
    uint32_t nl = 0, i;
    while (needle[nl]) nl++;
    if (nl == 0u || nl > hlen) return 0;
    for (i = 0; i + nl <= hlen; i++)
        if (memcmp(hay + i, needle, nl) == 0) return 1;
    return 0;
}

static void test_loader_identity_format(void)
{
    struct boot_loader_identity id;
    uint32_t n, i;

    /* Populated identity: git_sha 0x10..0x23, a known build time, a label. */
    memset(&id, 0, sizeof id);
    for (i = 0; i < 20u; i++) id.git_sha[i] = (uint8_t)(0x10u + i);
    id.build_unix_time = 1700000000ull;            /* 2023-11-14T22:13:20Z */
    memcpy(id.build_label, "v1.0-3-gabc123", 15);  /* 14 chars + NUL */
    n = boot_loader_identity_format(&id, s_fmt, sizeof s_fmt);
    TEST_ASSERT(n > 0u && n < sizeof s_fmt, "format returns a bounded length");
    TEST_ASSERT(buf_has(s_fmt, n, "10111213"), "git_sha hex encoded (0x10,0x11,..)");
    TEST_ASSERT(buf_has(s_fmt, n, "1700000000"), "build_unix_time decimal present");
    TEST_ASSERT(buf_has(s_fmt, n, "2023-11-14T22:13:20Z"), "ISO-8601 build time present");
    TEST_ASSERT(buf_has(s_fmt, n, "v1.0-3-gabc123"), "build_label present");
    TEST_ASSERT(!buf_has(s_fmt, n, "unavailable"), "populated identity is not unavailable");

    /* Zero sentinel: all-zero sha/time/label -> "unavailable", never 000..0. */
    memset(&id, 0, sizeof id);
    n = boot_loader_identity_format(&id, s_fmt, sizeof s_fmt);
    TEST_ASSERT(n > 0u && n < sizeof s_fmt, "zero-sentinel format bounded");
    TEST_ASSERT(buf_has(s_fmt, n, "unavailable"), "zero sentinel renders unavailable");
    TEST_ASSERT(!buf_has(s_fmt, n, "0000000000000000000000000000000000000000"),
                "all-zero git_sha is NOT presented as a real commit");

    /* Zero git_sha but a real build time + label (the no-git fallback record).
     * The git_sha sentinel gates the WHOLE identity: no half-populated record
     * that disagrees with the fault-transcript renderer. */
    memset(&id, 0, sizeof id);
    id.build_unix_time = 1700000000ull;
    memcpy(id.build_label, "v9.9-fallback", 14);
    n = boot_loader_identity_format(&id, s_fmt, sizeof s_fmt);
    TEST_ASSERT(buf_has(s_fmt, n, "unavailable"), "zero sha gates whole identity");
    TEST_ASSERT(!buf_has(s_fmt, n, "1700000000"), "no build_unix_time when sha is zero");
    TEST_ASSERT(!buf_has(s_fmt, n, "v9.9-fallback"), "no build_label when sha is zero");

    /* Bounds contract: fail closed on cap 0, NUL-terminate at any cap. */
    TEST_ASSERT_EQ((uint32_t)boot_loader_identity_format(&id, s_fmt, 0u), 0u,
                   "cap 0 -> returns 0, no underflow/write");
    s_fmt[0] = 'X';
    TEST_ASSERT_EQ((uint32_t)boot_loader_identity_format(&id, s_fmt, 1u), 0u,
                   "cap 1 -> returns 0 (only the NUL fits)");
    TEST_ASSERT_EQ((uint32_t)s_fmt[0], 0u, "cap 1 NUL-terminates");
    n = boot_loader_identity_format(&id, s_fmt, sizeof s_fmt);
    TEST_ASSERT_EQ((uint32_t)s_fmt[n], 0u, "output NUL-terminated at the returned length");
}

static void test_kdate_iso8601_fmt(void)
{
    char buf[21];
    kdate_iso8601(0, buf);
    TEST_ASSERT_EQ(memcmp(buf, "1970-01-01T00:00:00Z", 21), 0, "unix 0 -> epoch ISO");
    kdate_iso8601(1700000000ull, buf);
    TEST_ASSERT_EQ(memcmp(buf, "2023-11-14T22:13:20Z", 21), 0, "known timestamp -> correct ISO");
}

void test_register_boot_diag(void)
{
    test_suite_register_cat("boot: loader identity format",
                            test_loader_identity_format, TEST_CAT_BOOT);
    test_suite_register_cat("boot: kdate_iso8601 formatter",
                            test_kdate_iso8601_fmt, TEST_CAT_BOOT);
}
