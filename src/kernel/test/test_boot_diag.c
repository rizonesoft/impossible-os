/* Unit tests for boot diagnostics (TODO-14). Currently: the bootloader
 * build-identity formatter (section 10) + the shared ISO-8601 helper.
 * The section 1-3 / 4-9 cases ship with their own sections. */

#include "kernel/test/test.h"
#include "kernel/boot_version.h"
#include "kernel/boot_info.h"
#include "kernel/boot_load_status.h"
#include "kernel/panic.h"
#include "kernel/klog.h"
#include "kernel/time_iso.h"
#include "libc/string.h"

/* Off-stack restore target (struct panic_evidence is one 4 KiB page). */
static struct panic_evidence s_pe_out;

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

    /* SKIPPED (absent optional device, e.g. no NIC) is NOT degraded -- it must
     * render in the log but never trip the degraded summary/warning. */
    boot_load_status_test_restore(&empty);
    boot_load_record("network", BOOT_LOAD_CLASS_NET,     BOOT_LOAD_SKIPPED, 0u, 0u);
    boot_load_record("ahci",    BOOT_LOAD_CLASS_STORAGE, BOOT_LOAD_LOADED,  0u, 0u);
    s_blbuf[0] = 'X';
    count = boot_load_status_degraded_summary(s_blbuf, sizeof s_blbuf);
    TEST_ASSERT_EQ(count, 0u, "SKIPPED absent device is not counted degraded");
    TEST_ASSERT_EQ((uint32_t)s_blbuf[0], 0u, "SKIPPED -> empty degraded summary");
    n = boot_load_status_format(s_blbuf, sizeof s_blbuf);
    TEST_ASSERT(buf_has(s_blbuf, n, "SKIPPED"), "SKIPPED state rendered in the log");

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
        /* The dropped marker must survive a report-sized (256-byte) buffer even
         * when 64 degraded names would otherwise fill it -- it is emitted before
         * the variable-length name list (matches report_summary's sum[256]). */
        {
            char small[256];
            (void)boot_load_status_degraded_summary(small, sizeof small);
            TEST_ASSERT(buf_has(small, cstr_len(small), "dropped"),
                        "dropped marker survives the 256-byte report buffer");
        }
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

static void test_panic_evidence(void)
{
    struct panic_evidence *ev = (struct panic_evidence *)(uintptr_t)PANIC_EVIDENCE_ADDR;
    uint32_t off = (uint32_t)__builtin_offsetof(struct panic_evidence, boot_seq);

    /* Canonical IEEE CRC-32 check value: crc32("123456789") == 0xCBF43926. */
    TEST_ASSERT_EQ(panic_crc32("123456789", 9u), 0xCBF43926u, "CRC-32 check value");
    TEST_ASSERT_EQ(panic_crc32("", 0u), 0u, "CRC-32 of empty input is 0");
    TEST_ASSERT_EQ(panic_crc32("a", 1u), 0xE8B7BE43u, "CRC-32 single-byte known vector");

    /* Well-formed record built in place is restored, then the magic is cleared
     * so the same crash is never reported twice. (Built manually -- not via the
     * live collector -- to avoid mutating its first-caller-wins guard.) */
    memset(ev, 0, sizeof *ev);
    ev->version       = PANIC_EVIDENCE_VERSION;
    ev->size          = (uint32_t)sizeof *ev;
    ev->bugcheck_code = 0xABCDu;
    ev->rip           = 0x1234u;
    ev->crc32         = panic_crc32((const uint8_t *)ev + off, ev->size - off);
    ev->magic         = PANIC_EVIDENCE_MAGIC;
    TEST_ASSERT_EQ(panic_evidence_restore(&s_pe_out), 1, "valid record restored");
    TEST_ASSERT_EQ(s_pe_out.bugcheck_code, 0xABCDu, "restored bugcheck_code matches");
    TEST_ASSERT_EQ((uint32_t)s_pe_out.rip, 0x1234u, "restored rip matches");
    TEST_ASSERT_EQ((uint32_t)ev->magic, 0u, "magic cleared after a successful restore");
    TEST_ASSERT_EQ(panic_evidence_restore(&s_pe_out), 0, "second restore -> nothing (consumed)");

    /* Bad crc32 is rejected (stale 0x80000 never misread as a valid crash). */
    memset(ev, 0, sizeof *ev);
    ev->version = PANIC_EVIDENCE_VERSION;
    ev->size    = (uint32_t)sizeof *ev;
    ev->crc32   = 0x0BADBAD0u;
    ev->magic   = PANIC_EVIDENCE_MAGIC;
    TEST_ASSERT_EQ(panic_evidence_restore(&s_pe_out), 0, "bad crc32 rejected");
    TEST_ASSERT_EQ((uint32_t)ev->magic, 0u, "bad-crc record dropped");

    /* Wrong version is rejected even with a self-consistent crc. */
    memset(ev, 0, sizeof *ev);
    ev->version = PANIC_EVIDENCE_VERSION + 99u;
    ev->size    = (uint32_t)sizeof *ev;
    ev->crc32   = panic_crc32((const uint8_t *)ev + off, ev->size - off);
    ev->magic   = PANIC_EVIDENCE_MAGIC;
    TEST_ASSERT_EQ(panic_evidence_restore(&s_pe_out), 0, "wrong version rejected");

    /* Right version, WRONG size -> rejected (an old-layout record must never be
     * misread; the size gate fires before the crc check). */
    memset(ev, 0, sizeof *ev);
    ev->version = PANIC_EVIDENCE_VERSION;
    ev->size    = (uint32_t)sizeof *ev - 8u;
    ev->crc32   = panic_crc32((const uint8_t *)ev + off, ev->size - off);
    ev->magic   = PANIC_EVIDENCE_MAGIC;
    TEST_ASSERT_EQ(panic_evidence_restore(&s_pe_out), 0, "size mismatch rejected");
    TEST_ASSERT_EQ((uint32_t)ev->magic, 0u, "size-mismatch record dropped");

    /* Over-cap counts with a VALID crc -> rejected: the cross-boot page is
     * untrusted, so an in-range count is required before any consumer iterates
     * stages[]/klogs[] (else an OOB read in the artifact writer). */
    memset(ev, 0, sizeof *ev);
    ev->version     = PANIC_EVIDENCE_VERSION;
    ev->size        = (uint32_t)sizeof *ev;
    ev->stage_count = PANIC_EVIDENCE_STAGES + 1u;
    ev->crc32       = panic_crc32((const uint8_t *)ev + off, ev->size - off);
    ev->magic       = PANIC_EVIDENCE_MAGIC;
    TEST_ASSERT_EQ(panic_evidence_restore(&s_pe_out), 0, "over-cap stage_count rejected");
    TEST_ASSERT_EQ((uint32_t)ev->magic, 0u, "over-cap stage_count record dropped");
    memset(ev, 0, sizeof *ev);
    ev->version    = PANIC_EVIDENCE_VERSION;
    ev->size       = (uint32_t)sizeof *ev;
    ev->klog_count = PANIC_EVIDENCE_KLOGS + 1u;
    ev->crc32      = panic_crc32((const uint8_t *)ev + off, ev->size - off);
    ev->magic      = PANIC_EVIDENCE_MAGIC;
    TEST_ASSERT_EQ(panic_evidence_restore(&s_pe_out), 0, "over-cap klog_count rejected");

    /* Untrusted unterminated strings: a valid-crc record whose message/file
     * lack a NUL must be force-terminated on restore so no downstream reader
     * over-reads. */
    memset(ev, 0, sizeof *ev);
    ev->version = PANIC_EVIDENCE_VERSION;
    ev->size    = (uint32_t)sizeof *ev;
    memset(ev->message, 'A', sizeof ev->message);   /* no NUL anywhere */
    memset(ev->file,    'B', sizeof ev->file);
    ev->crc32   = panic_crc32((const uint8_t *)ev + off, ev->size - off);
    ev->magic   = PANIC_EVIDENCE_MAGIC;
    TEST_ASSERT_EQ(panic_evidence_restore(&s_pe_out), 1, "valid record with unterminated strings restored");
    TEST_ASSERT_EQ((uint32_t)s_pe_out.message[sizeof s_pe_out.message - 1u], 0u, "message force-terminated");
    TEST_ASSERT_EQ((uint32_t)s_pe_out.file[sizeof s_pe_out.file - 1u], 0u, "file force-terminated");

    /* No magic -> not a record. Leaves 0x80000 clean for a real panic. */
    ev->magic = 0u;
    TEST_ASSERT_EQ(panic_evidence_restore(&s_pe_out), 0, "absent magic -> no record");
}

static void test_klog_panic_snapshot(void)
{
    klog_entry_t s3[3], s2[2];
    uint32_t n3, n2;

    /* The boot ring already holds entries; klog() is suppressed under the test
     * harness so the tail cannot be staged with known content. Instead verify
     * the oldest-first index math by SELF-CONSISTENCY: snapshot(2) must equal
     * the last 2 of snapshot(3) (the oldest of the three is the one dropped),
     * compared by per-entry timestamp. Two adjacent calls read the same ring. */
    /* BACK-TO-BACK with nothing between: a TEST_ASSERT logs via klog and would
     * advance the ring, so both snapshots must be taken before any assertion. */
    n3 = klog_panic_snapshot(s3, 3u);
    n2 = klog_panic_snapshot(s2, 2u);
    TEST_ASSERT(n3 <= 3u, "snapshot returns at most max (3)");
    TEST_ASSERT(n2 <= 2u, "snapshot caps at max (2)");
    if (n3 == 3u && n2 == 2u) {
        TEST_ASSERT_EQ(s2[0].timestamp, s3[1].timestamp, "snapshot(2)[0] == snapshot(3)[1]");
        TEST_ASSERT_EQ(s2[1].timestamp, s3[2].timestamp, "snapshot(2)[1] == snapshot(3)[2] (newest)");
    }

    /* Degenerate inputs fail closed. */
    TEST_ASSERT_EQ(klog_panic_snapshot(s2, 0u), 0u, "max 0 -> 0");
    TEST_ASSERT_EQ(klog_panic_snapshot((klog_entry_t *)0, 3u), 0u, "NULL out -> 0");
}

void test_register_boot_diag(void)
{
    test_suite_register_cat("boot: panic forensic evidence",
                            test_panic_evidence, TEST_CAT_BOOT);
    test_suite_register_cat("boot: klog panic snapshot",
                            test_klog_panic_snapshot, TEST_CAT_BOOT);
    test_suite_register_cat("boot: loader identity format",
                            test_loader_identity_format, TEST_CAT_BOOT);
    test_suite_register_cat("boot: kdate_iso8601 formatter",
                            test_kdate_iso8601_fmt, TEST_CAT_BOOT);
    test_suite_register_cat("boot: load/status log (ntbtlog parity)",
                            test_boot_load_status, TEST_CAT_BOOT);
}
