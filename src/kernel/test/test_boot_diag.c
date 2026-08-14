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

/* Off-stack FIXTURE page for the evidence-lifecycle cases (section 23). Never
 * the live 0x80000 page and never the live collector: panic_evidence_take
 * mutates boot-global ownership, so exercising it from a test would lock a later
 * REAL panic out of the record. The _at() helpers run the identical publish /
 * revoke / consume / restore logic against this page instead. */
static struct panic_evidence s_pe_fixture;

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
    ev->epoch   = 1u;      /* epoch 0 is the no-record sentinel; see below */
    ev->version       = PANIC_EVIDENCE_VERSION;
    ev->size          = (uint32_t)sizeof *ev;
    ev->bugcheck_code = 0xABCDu;
    ev->rip           = 0x1234u;
    ev->crc32         = panic_crc32((const uint8_t *)ev + off, ev->size - off);
    ev->magic         = PANIC_EVIDENCE_MAGIC;
    TEST_ASSERT_EQ(panic_evidence_restore(&s_pe_out), 1, "valid record restored");
    TEST_ASSERT_EQ(s_pe_out.bugcheck_code, 0xABCDu, "restored bugcheck_code matches");
    TEST_ASSERT_EQ((uint32_t)s_pe_out.rip, 0x1234u, "restored rip matches");
    /* Restore RETAINS the page (the record is durable only after the file
     * write) so a boot that dies before emission retries; it is repeatable
     * until panic_evidence_consume() clears it. */
    TEST_ASSERT_EQ((uint32_t)ev->magic, PANIC_EVIDENCE_MAGIC, "magic retained after restore (retry)");
    TEST_ASSERT_EQ(panic_evidence_restore(&s_pe_out), 1, "restore is repeatable until consumed");
    /* consume is conditional: it must NOT erase a page record that does not
     * match the boot-restored s_prev_crash (a fresh crash from another CPU).
     * This hand-built record does not match, so consume is a safe no-op. The
     * matching-record clear is exercised by the crash_test smoke path. */
    panic_evidence_consume();
    TEST_ASSERT_EQ((uint32_t)ev->magic, PANIC_EVIDENCE_MAGIC, "consume does NOT erase a non-matching record");
    ev->magic = 0u;   /* manual cleanup of the test record */
    TEST_ASSERT_EQ(panic_evidence_restore(&s_pe_out), 0, "after manual clear -> nothing");

    /* Bad crc32 is rejected (stale 0x80000 never misread as a valid crash). */
    memset(ev, 0, sizeof *ev);
    ev->epoch   = 1u;      /* epoch 0 is the no-record sentinel; see below */
    ev->version = PANIC_EVIDENCE_VERSION;
    ev->size    = (uint32_t)sizeof *ev;
    ev->crc32   = 0x0BADBAD0u;
    ev->magic   = PANIC_EVIDENCE_MAGIC;
    TEST_ASSERT_EQ(panic_evidence_restore(&s_pe_out), 0, "bad crc32 rejected");
    TEST_ASSERT_EQ((uint32_t)ev->magic, 0u, "bad-crc record dropped");

    /* Wrong version is rejected even with a self-consistent crc. */
    memset(ev, 0, sizeof *ev);
    ev->epoch   = 1u;      /* epoch 0 is the no-record sentinel; see below */
    ev->version = PANIC_EVIDENCE_VERSION + 99u;
    ev->size    = (uint32_t)sizeof *ev;
    ev->crc32   = panic_crc32((const uint8_t *)ev + off, ev->size - off);
    ev->magic   = PANIC_EVIDENCE_MAGIC;
    TEST_ASSERT_EQ(panic_evidence_restore(&s_pe_out), 0, "wrong version rejected");

    /* Right version, WRONG size -> rejected (an old-layout record must never be
     * misread; the size gate fires before the crc check). */
    memset(ev, 0, sizeof *ev);
    ev->epoch   = 1u;      /* epoch 0 is the no-record sentinel; see below */
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
    ev->epoch   = 1u;      /* epoch 0 is the no-record sentinel; see below */
    ev->version     = PANIC_EVIDENCE_VERSION;
    ev->size        = (uint32_t)sizeof *ev;
    ev->stage_count = PANIC_EVIDENCE_STAGES + 1u;
    ev->crc32       = panic_crc32((const uint8_t *)ev + off, ev->size - off);
    ev->magic       = PANIC_EVIDENCE_MAGIC;
    TEST_ASSERT_EQ(panic_evidence_restore(&s_pe_out), 0, "over-cap stage_count rejected");
    TEST_ASSERT_EQ((uint32_t)ev->magic, 0u, "over-cap stage_count record dropped");
    memset(ev, 0, sizeof *ev);
    ev->epoch   = 1u;      /* epoch 0 is the no-record sentinel; see below */
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
    ev->epoch   = 1u;      /* epoch 0 is the no-record sentinel; see below */
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

/* Section 23: the publication epoch, and the lifecycle transitions keyed by it.
 * All over a FIXTURE page -- see s_pe_fixture. */
static void test_panic_evidence_epoch(void)
{
    volatile struct panic_evidence *fx = &s_pe_fixture;

    /* --- epoch allocation: never 0, never either observed epoch ---
     * The interesting inputs are the wrapping ones. A plain max()+1 gets
     * standing=UINT32_MAX wrong (wraps to 0), and "wrap then skip 0" then lands
     * on 1 -- which collides when the restored epoch IS 1, and a consume keyed
     * by that value would erase the newly published record instead of the old
     * one it was aiming at. */
    TEST_ASSERT_EQ(panic_evidence_next_epoch(0u, 0u), 1u, "first epoch of a clean boot is 1");
    TEST_ASSERT_EQ(panic_evidence_next_epoch(7u, 3u), 8u, "epoch advances past the standing record");
    TEST_ASSERT_EQ(panic_evidence_next_epoch(3u, 9u), 10u, "epoch advances past the restored record");
    TEST_ASSERT_EQ(panic_evidence_next_epoch(0xFFFFFFFFu, 0u), 1u, "wrap past UINT32_MAX yields a live epoch");
    TEST_ASSERT_EQ(panic_evidence_next_epoch(0xFFFFFFFFu, 1u), 2u,
                   "wrap skips the restored epoch, not just zero");
    TEST_ASSERT_EQ(panic_evidence_next_epoch(0xFFFFFFFFu, 2u), 1u,
                   "wrap skips a colliding restored epoch either way");
    {
        /* The property the three cases above are instances of, asserted
         * directly over every adversarial pair the wrap can produce. */
        uint32_t standing[4] = { 0u, 1u, 2u, 0xFFFFFFFFu };
        for (uint32_t i = 0u; i < 4u; i++) {
            for (uint32_t j = 0u; j < 4u; j++) {
                uint32_t e = panic_evidence_next_epoch(standing[i], standing[j]);
                TEST_ASSERT(e != 0u, "allocated epoch is never the no-record sentinel");
                TEST_ASSERT(e != standing[i], "allocated epoch never equals the standing epoch");
                TEST_ASSERT(e != standing[j], "allocated epoch never equals the restored epoch");
            }
        }
    }

    /* --- publish makes a record restorable, and carries its epoch --- */
    memset(&s_pe_fixture, 0, sizeof s_pe_fixture);
    s_pe_fixture.bugcheck_code = 0x5150u;
    TEST_ASSERT_EQ(panic_evidence_publish_at(fx, 0u), 0, "epoch 0 is not publishable");
    TEST_ASSERT_EQ(panic_evidence_publish_at(fx, 42u), 1, "record published at epoch 42");
    TEST_ASSERT_EQ((uint32_t)fx->magic, PANIC_EVIDENCE_MAGIC, "publication set the magic");
    TEST_ASSERT_EQ((uint32_t)fx->epoch, 42u, "publication word carries the epoch");
    TEST_ASSERT_EQ(panic_evidence_restore_at(fx, &s_pe_out), 1, "published record restores");
    TEST_ASSERT_EQ(s_pe_out.bugcheck_code, 0x5150u, "restored payload matches");
    TEST_ASSERT_EQ(s_pe_out.epoch, 42u, "restore copies the epoch out for consume");

    /* --- revoke is generation-conditional --- */
    TEST_ASSERT_EQ(panic_evidence_revoke_at(fx, 41u), 0, "revoke of a different epoch is refused");
    TEST_ASSERT_EQ((uint32_t)fx->magic, PANIC_EVIDENCE_MAGIC, "refused revoke left the record");
    TEST_ASSERT_EQ(panic_evidence_revoke_at(fx, 42u), 1, "revoke of the standing epoch succeeds");
    TEST_ASSERT_EQ((uint32_t)fx->magic, 0u, "revoked record is gone");
    TEST_ASSERT_EQ(panic_evidence_restore_at(fx, &s_pe_out), 0, "revoked record does not restore");

    /* --- THE SECTION-23 DEFECT, asserted directly ---
     * A survivable fault publishes, then revokes on its way to parking; the
     * terminal fault that follows takes the slot rather than being refused it,
     * and the next boot restores the TERMINAL record, not the survived one. */
    memset(&s_pe_fixture, 0, sizeof s_pe_fixture);
    s_pe_fixture.bugcheck_code = 0xA55Eu;                 /* survivable async fault */
    TEST_ASSERT_EQ(panic_evidence_publish_at(fx, 7u), 1, "survivable fault published");
    TEST_ASSERT_EQ(panic_evidence_revoke_at(fx, 7u), 1, "survivable fault revoked at park");
    TEST_ASSERT_EQ(panic_evidence_restore_at(fx, &s_pe_out), 0,
                   "a fault the machine SURVIVED leaves no cross-boot record");
    memset(&s_pe_fixture, 0, sizeof s_pe_fixture);
    s_pe_fixture.bugcheck_code = 0xDEADu;                 /* the fault that killed it */
    TEST_ASSERT_EQ(panic_evidence_publish_at(fx, 8u), 1, "terminal fault takes the slot");
    TEST_ASSERT_EQ(panic_evidence_restore_at(fx, &s_pe_out), 1, "terminal record restores");
    TEST_ASSERT_EQ(s_pe_out.bugcheck_code, 0xDEADu,
                   "the restored record describes the fault that killed the machine");

    /* --- consume is keyed by epoch, so a concurrent fresh crash survives ---
     * This is the case the old compare-then-clear could not express: it compared
     * boot_seq + crc and cleared the magic as a separate store, so a panic
     * publishing in the gap lost its record. */
    TEST_ASSERT_EQ(panic_evidence_publish_at(fx, 9u), 1, "republished at a newer epoch");
    panic_evidence_consume_at(fx, 8u);                    /* consume the OLD record */
    TEST_ASSERT_EQ((uint32_t)fx->magic, PANIC_EVIDENCE_MAGIC,
                   "consume of a superseded epoch does NOT erase the newer record");
    panic_evidence_consume_at(fx, 0u);
    TEST_ASSERT_EQ((uint32_t)fx->magic, PANIC_EVIDENCE_MAGIC, "consume of epoch 0 is inert");
    panic_evidence_consume_at(fx, 9u);                    /* consume the standing one */
    TEST_ASSERT_EQ((uint32_t)fx->magic, 0u, "consume of the standing epoch clears the page");
    TEST_ASSERT_EQ(panic_evidence_restore_at(fx, &s_pe_out), 0, "consumed record does not restore");

    /* --- epoch 0 is the no-record sentinel on EVERY path ---
     * A CRC-valid record carrying magic with epoch 0 must not restore. It would
     * otherwise be unconsumable -- publish refuses epoch 0 and consume is inert
     * for it -- so the same crash would be re-reported on every boot forever.
     * The page is untrusted cross-boot RAM, so this needs no bug on the writing
     * side to occur; stale contents with a plausible magic are exactly what the
     * header validation exists to reject. */
    {
        uint32_t off = (uint32_t)__builtin_offsetof(struct panic_evidence, boot_seq);
        memset(&s_pe_fixture, 0, sizeof s_pe_fixture);
        s_pe_fixture.version = PANIC_EVIDENCE_VERSION;
        s_pe_fixture.size    = (uint32_t)sizeof s_pe_fixture;
        s_pe_fixture.crc32   = panic_crc32((const uint8_t *)&s_pe_fixture + off,
                                           s_pe_fixture.size - off);
        s_pe_fixture.magic   = PANIC_EVIDENCE_MAGIC;    /* magic set, epoch 0 */
        TEST_ASSERT_EQ(panic_evidence_restore_at(fx, &s_pe_out), 0,
                       "a valid-looking record with epoch 0 is rejected");
        TEST_ASSERT_EQ((uint32_t)fx->magic, 0u,
                       "the unconsumable epoch-0 record is dropped, not left to re-report");
    }

    /* --- a rejected record is dropped through the PUBLICATION WORD ---
     * Both halves are cleared, because the drop is one CAS on the 64-bit word
     * rather than a plain store to the magic. That is what stops a rejection
     * decided about an OLD record from erasing a newer one republished
     * underneath it: the CAS names the exact record that failed validation. */
    memset(&s_pe_fixture, 0, sizeof s_pe_fixture);
    s_pe_fixture.epoch   = 21u;
    s_pe_fixture.version = PANIC_EVIDENCE_VERSION;
    s_pe_fixture.size    = (uint32_t)sizeof s_pe_fixture;
    s_pe_fixture.crc32   = 0x0BADBAD0u;                 /* deliberately wrong */
    s_pe_fixture.magic   = PANIC_EVIDENCE_MAGIC;
    TEST_ASSERT_EQ(panic_evidence_restore_at(fx, &s_pe_out), 0, "bad crc rejected on the fixture");
    TEST_ASSERT_EQ((uint32_t)fx->magic, 0u, "rejected record cleared its magic");
    TEST_ASSERT_EQ((uint32_t)fx->epoch, 0u, "rejected record cleared its epoch too");

    /* --- and the drop is GENERATION-CONDITIONAL, not an unconditional clear ---
     * The two assertions above would pass just as well if the reject path stored
     * zero over whatever happened to be on the page. What distinguishes the two
     * implementations is a drop aimed at a generation that is no longer the one
     * standing: a CAS keyed on the observed word does nothing, a blind clear
     * erases the newer record. That is the difference between a rejection
     * decided about an old record and the loss of the terminal crash that
     * replaced it, so it is asserted directly rather than inferred. */
    memset(&s_pe_fixture, 0, sizeof s_pe_fixture);
    s_pe_fixture.bugcheck_code = 0xFA7Au;
    TEST_ASSERT_EQ(panic_evidence_publish_at(fx, 31u), 1, "newer record published at epoch 31");
    TEST_ASSERT_EQ(panic_evidence_revoke_at(fx, 30u), 0,
                   "a drop aimed at the superseded generation is refused");
    TEST_ASSERT_EQ((uint32_t)fx->magic, PANIC_EVIDENCE_MAGIC,
                   "the newer record survives a stale drop");
    TEST_ASSERT_EQ((uint32_t)fx->epoch, 31u, "and keeps its own generation");
    TEST_ASSERT_EQ(panic_evidence_restore_at(fx, &s_pe_out), 1, "newer record still restores");
    TEST_ASSERT_EQ(s_pe_out.bugcheck_code, 0xFA7Au, "with its own payload intact");

    /* --- restore still RETAINS, which is what the header now promises --- */
    memset(&s_pe_fixture, 0, sizeof s_pe_fixture);
    TEST_ASSERT_EQ(panic_evidence_publish_at(fx, 11u), 1, "record for the retain check");
    TEST_ASSERT_EQ(panic_evidence_restore_at(fx, &s_pe_out), 1, "first restore succeeds");
    TEST_ASSERT_EQ(panic_evidence_restore_at(fx, &s_pe_out), 1,
                   "restore is repeatable -- it does not consume");
    TEST_ASSERT_EQ((uint32_t)fx->magic, PANIC_EVIDENCE_MAGIC, "restore retained the page");
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
    test_suite_register_cat("boot: panic evidence epoch lifecycle",
                            test_panic_evidence_epoch, TEST_CAT_BOOT);
    test_suite_register_cat("boot: klog panic snapshot",
                            test_klog_panic_snapshot, TEST_CAT_BOOT);
    test_suite_register_cat("boot: loader identity format",
                            test_loader_identity_format, TEST_CAT_BOOT);
    test_suite_register_cat("boot: kdate_iso8601 formatter",
                            test_kdate_iso8601_fmt, TEST_CAT_BOOT);
    test_suite_register_cat("boot: load/status log (ntbtlog parity)",
                            test_boot_load_status, TEST_CAT_BOOT);
}
