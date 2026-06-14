/* Unit tests for boot diagnostics (TODO-14). Currently: the bootloader
 * build-identity formatter (section 10) + the shared ISO-8601 helper.
 * The section 1-3 / 4-9 cases ship with their own sections. */

#include "kernel/test/test.h"
#include "kernel/boot_version.h"
#include "kernel/boot_info.h"
#include "kernel/boot_load_status.h"
#include "kernel/time_iso.h"
#include "libc/string.h"

/* Off-stack fixture: the formatter buffer is 512 bytes. */
static char s_fmt[512];

/* Off-stack fixtures for the load-status log (section 11): a full pool
 * snapshot (~3 KiB) + a format buffer big enough for 64 lines. */
static struct boot_load_test_state s_blsave;
static char s_blbuf[8192];

static int buf_has(const char *hay, uint32_t hlen, const char *needle)
{
    uint32_t nl = 0, i;
    while (needle[nl]) nl++;
    if (nl == 0u || nl > hlen) return 0;
    for (i = 0; i + nl <= hlen; i++)
        if (memcmp(hay + i, needle, nl) == 0) return 1;
    return 0;
}

static uint32_t cstr_len(const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
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

static void test_boot_load_status(void)
{
    uint32_t n, count, sl;
    struct boot_load_test_state empty;

    /* Protect the live boot log; restoring an all-zero state == reset. */
    boot_load_status_test_save(&s_blsave);
    memset(&empty, 0, sizeof empty);
    boot_load_status_test_restore(&empty);

    /* Synthetic mix: LOADED + FAILED + DEGRADED + LOADED. */
    boot_load_record("ata",     BOOT_LOAD_CLASS_STORAGE, BOOT_LOAD_LOADED,   0u,      0u);
    boot_load_record("nvme",    BOOT_LOAD_CLASS_STORAGE, BOOT_LOAD_FAILED,   0x0005u, 0u);
    boot_load_record("ahci",    BOOT_LOAD_CLASS_STORAGE, BOOT_LOAD_DEGRADED, 0x0003u, 0u);
    boot_load_record("network", BOOT_LOAD_CLASS_NET,     BOOT_LOAD_LOADED,   0u,      0u);

    n = boot_load_status_format(s_blbuf, sizeof s_blbuf);
    TEST_ASSERT(n > 0u && n < sizeof s_blbuf, "format bounded");
    TEST_ASSERT(buf_has(s_blbuf, n, "4 recorded / 64 cap, 0 dropped"), "header counts");
    TEST_ASSERT(!buf_has(s_blbuf, n, "TRUNCATED"), "no truncation under capacity");
    TEST_ASSERT(buf_has(s_blbuf, n, "FAILED"), "FAILED state rendered");
    TEST_ASSERT(buf_has(s_blbuf, n, "DEGRADED"), "DEGRADED state rendered");
    TEST_ASSERT(buf_has(s_blbuf, n, "nvme"), "entry name rendered");
    TEST_ASSERT(buf_has(s_blbuf, n, "err=0x0005"), "err code hex rendered");

    /* Degraded summary: FAILED + DEGRADED counted; LOADED entries excluded. */
    count = boot_load_status_degraded_summary(s_blbuf, sizeof s_blbuf);
    sl = cstr_len(s_blbuf);
    TEST_ASSERT_EQ(count, 2u, "summary counts FAILED + DEGRADED only");
    TEST_ASSERT(buf_has(s_blbuf, sl, "2 degraded"), "summary prefix");
    TEST_ASSERT(buf_has(s_blbuf, sl, "nvme(0x0005)"), "summary names the failed entry");
    TEST_ASSERT(buf_has(s_blbuf, sl, "ahci(0x0003)"), "summary names the degraded entry");

    /* All-LOADED pool -> empty summary string, returns 0. */
    boot_load_status_test_restore(&empty);
    boot_load_record("core1", BOOT_LOAD_CLASS_CORE, BOOT_LOAD_LOADED, 0u, 0u);
    boot_load_record("core2", BOOT_LOAD_CLASS_CORE, BOOT_LOAD_LOADED, 0u, 0u);
    s_blbuf[0] = 'X';
    count = boot_load_status_degraded_summary(s_blbuf, sizeof s_blbuf);
    TEST_ASSERT_EQ(count, 0u, "all-LOADED pool -> zero degraded");
    TEST_ASSERT_EQ((uint32_t)s_blbuf[0], 0u, "all-LOADED pool -> empty summary string");

    /* begin/finish: measured-duration span with a final state. */
    boot_load_status_test_restore(&empty);
    {
        int tok = boot_load_begin("span", BOOT_LOAD_CLASS_CORE);
        TEST_ASSERT(tok >= 0, "begin returns a valid token");
        boot_load_finish(tok, BOOT_LOAD_LOADED, 0u, 0u);
        n = boot_load_status_format(s_blbuf, sizeof s_blbuf);
        TEST_ASSERT(buf_has(s_blbuf, n, "span"), "spanned entry rendered");
        TEST_ASSERT(buf_has(s_blbuf, n, "dur="), "duration field present");
    }

    /* Overflow: 70 records into a 64-slot pool -> 6 dropped, TRUNCATED, no wrap. */
    boot_load_status_test_restore(&empty);
    {
        uint32_t i;
        for (i = 0; i < 70u; i++)
            boot_load_record("flood", BOOT_LOAD_CLASS_CORE, BOOT_LOAD_FAILED, 0x00FFu, 0u);
        n = boot_load_status_format(s_blbuf, sizeof s_blbuf);
        TEST_ASSERT(buf_has(s_blbuf, n, "64 recorded / 64 cap, 6 dropped TRUNCATED"),
                    "overflow header reports dropped + TRUNCATED");
        count = boot_load_status_degraded_summary(s_blbuf, sizeof s_blbuf);
        sl = cstr_len(s_blbuf);
        TEST_ASSERT_EQ(count, 64u, "summary counts only the 64 recorded entries");
        TEST_ASSERT(buf_has(s_blbuf, sl, "dropped"), "summary notes the dropped overflow");
    }

    /* Fail-closed: NULL / zero-cap / cap-1 / tiny-cap on both pure helpers,
     * with a FAILED record present so the formatters have real content. */
    boot_load_status_test_restore(&empty);
    boot_load_record("x", BOOT_LOAD_CLASS_CORE, BOOT_LOAD_FAILED, 0x0001u, 0u);
    TEST_ASSERT_EQ(boot_load_status_format((char *)0, 64u), 0u, "format NULL buf -> 0");
    TEST_ASSERT_EQ(boot_load_status_format(s_blbuf, 0u), 0u, "format cap 0 -> 0");
    TEST_ASSERT_EQ(boot_load_status_degraded_summary((char *)0, 64u), 0u, "summary NULL buf -> 0");
    TEST_ASSERT_EQ(boot_load_status_degraded_summary(s_blbuf, 0u), 0u, "summary cap 0 -> 0");
    s_blbuf[0] = 'Z';
    TEST_ASSERT_EQ(boot_load_status_format(s_blbuf, 1u), 0u, "format cap 1 -> 0");
    TEST_ASSERT_EQ((uint32_t)s_blbuf[0], 0u, "format cap 1 NUL-terminates");
    {
        char tiny[8];
        uint32_t t = boot_load_status_format(tiny, sizeof tiny);
        TEST_ASSERT(t < sizeof tiny, "tiny-cap format stays in bounds");
        TEST_ASSERT_EQ((uint32_t)tiny[t], 0u, "tiny-cap format NUL-terminated");
        (void)boot_load_status_degraded_summary(tiny, sizeof tiny);
        TEST_ASSERT_EQ((uint32_t)tiny[cstr_len(tiny)], 0u, "tiny-cap summary NUL-terminated");
    }

    /* Capacity boundary: exactly 64 -> 0 dropped/no TRUNCATED; the 65th -> 1 dropped. */
    boot_load_status_test_restore(&empty);
    {
        uint32_t i;
        for (i = 0; i < 64u; i++)
            boot_load_record("d", BOOT_LOAD_CLASS_CORE, BOOT_LOAD_LOADED, 0u, 0u);
        n = boot_load_status_format(s_blbuf, sizeof s_blbuf);
        TEST_ASSERT(buf_has(s_blbuf, n, "64 recorded / 64 cap, 0 dropped"), "exactly 64 -> 0 dropped");
        TEST_ASSERT(!buf_has(s_blbuf, n, "TRUNCATED"), "exactly 64 -> no TRUNCATED marker");
        boot_load_record("over", BOOT_LOAD_CLASS_CORE, BOOT_LOAD_LOADED, 0u, 0u);
        n = boot_load_status_format(s_blbuf, sizeof s_blbuf);
        TEST_ASSERT(buf_has(s_blbuf, n, "64 recorded / 64 cap, 1 dropped TRUNCATED"),
                    "the 65th record -> exactly 1 dropped");
    }

    /* Pool-full token path: begin returns -1; finish(-1) is a no-op. */
    boot_load_status_test_restore(&empty);
    {
        uint32_t i, before;
        int tok;
        for (i = 0; i < 64u; i++)
            (void)boot_load_begin("b", BOOT_LOAD_CLASS_CORE);
        tok = boot_load_begin("overflow", BOOT_LOAD_CLASS_CORE);
        TEST_ASSERT(tok < 0, "begin at pool-full returns -1");
        before = boot_load_status_format(s_blbuf, sizeof s_blbuf);
        boot_load_finish(-1, BOOT_LOAD_FAILED, 0xBEEFu, 0u);   /* must be inert */
        n = boot_load_status_format(s_blbuf, sizeof s_blbuf);
        TEST_ASSERT_EQ(n, before, "finish(-1) does not mutate the log");
        TEST_ASSERT(!buf_has(s_blbuf, n, "overflow"), "pool-full begin stored nothing");
        TEST_ASSERT(!buf_has(s_blbuf, n, "beef"), "finish(-1) wrote no err code");
    }

    /* Name truncation: a 36-char name keeps its 31-char prefix, no 32nd byte. */
    boot_load_status_test_restore(&empty);
    boot_load_record("abcdefghijklmnopqrstuvwxyz0123456789", BOOT_LOAD_CLASS_CORE,
                     BOOT_LOAD_FAILED, 0x0007u, 0u);
    n = boot_load_status_format(s_blbuf, sizeof s_blbuf);
    TEST_ASSERT(buf_has(s_blbuf, n, "abcdefghijklmnopqrstuvwxyz01234"), "name keeps 31-char prefix");
    TEST_ASSERT(!buf_has(s_blbuf, n, "abcdefghijklmnopqrstuvwxyz012345"), "name truncated at 31 bytes");

    boot_load_status_test_restore(&s_blsave);   /* restore the live boot log */
}

void test_register_boot_diag(void)
{
    test_suite_register_cat("boot: loader identity format",
                            test_loader_identity_format, TEST_CAT_BOOT);
    test_suite_register_cat("boot: kdate_iso8601 formatter",
                            test_kdate_iso8601_fmt, TEST_CAT_BOOT);
    test_suite_register_cat("boot: load/status log (ntbtlog parity)",
                            test_boot_load_status, TEST_CAT_BOOT);
}
