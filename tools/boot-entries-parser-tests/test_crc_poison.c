/* Host-side regression for the boot-entries parser's poison-proof CRC table.
 *
 * WHY THIS IS A HOST TEST. The parser is reachable from src/kernel/test/
 * (test_boot_entry_parser.c #includes the same source), so a kernel-side case
 * is WRITABLE -- it was written and it does not FIT. Measured 2026-09-03: it
 * pushed __kernel_end past the 0x800000 user base and the build failed with a
 * BSS-collision refusal, headroom being 47 bytes of .text at the time. Testing
 * it here costs the kernel image nothing, which is the same reason
 * tools/boot-header-tests/ exists.
 *
 * WHAT IS UNDER TEST. BOOTX64.EFI does not zero .bss and firmware pool-poisons
 * it with 0xAF, so `if (!g_crc32_ready) crc32_init();` never rebuilt the table:
 * poison is non-zero and read as "ready". Every CRC was then computed over a
 * poisoned 1 KiB table, so a VALID boot-entry store was rejected as
 * CRC_MISMATCH and the loader fell back to a possibly different kernel. The
 * fix is a wide volatile exact-match cookie published after the table is
 * filled, plus a reset at the parser's entry.
 *
 * The third case is a CONTROL, and it is the point of the file: it reproduces
 * the OLD behaviour (poisoned table, cookie forced to READY) and asserts the
 * CRC comes out WRONG. Without it, cases 1 and 2 would pass just as happily
 * against an implementation that never had the bug, and the test would be
 * evidence of nothing. */
#include <stdio.h>
#include <string.h>

#include "../../src/boot/uefi/boot_entries_parser.c"

static int g_failures;

static void check(int ok, const char *what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_failures++;
}

/* The pattern EDK2 DEBUG builds fill uninitialised pool with. */
#define POISON_BYTE  0xAF
#define POISON_WORD  0xAFAFAFAFu

static u32 clean_crc(const char *s)
{
    crc32_reset();
    return crc32_zeroed((const u8 *)s, (u32)strlen(s), 0u);
}

static void poison_statics(void)
{
    memset((void *)g_crc32_table, POISON_BYTE, sizeof(g_crc32_table));
    g_crc32_ready = POISON_WORD;
}

int main(void)
{
    const char *SAMPLE = "impossible-os boot entries";
    u32 expected, got;

    printf("boot-entries parser: poison-proof CRC table\n");

    expected = clean_crc(SAMPLE);

    /* 1. A poisoned readiness cookie must NOT be read as "table is built".
     *    This is the shipped guarantee: the exact-match compare rejects
     *    0xAFAFAFAF, so crc32_init() runs and the CRC is correct. */
    poison_statics();
    got = crc32_zeroed((const u8 *)SAMPLE, (u32)strlen(SAMPLE), 0u);
    check(got == expected, "poisoned ready cookie still rebuilds the table");

    /* 2. crc32_reset() forces a rebuild even from a legitimately-ready state,
     *    which is what makes the parser entry reset meaningful across a warm
     *    reboot that left a valid cookie behind at the same address. */
    memset((void *)g_crc32_table, POISON_BYTE, sizeof(g_crc32_table));
    crc32_reset();
    got = crc32_zeroed((const u8 *)SAMPLE, (u32)strlen(SAMPLE), 0u);
    check(got == expected, "crc32_reset() rebuilds over a poisoned table");

    /* 3. CONTROL -- must FAIL to match, or cases 1 and 2 prove nothing.
     *    Reproduce the pre-fix state exactly: a poisoned table that the guard
     *    believes is ready. If this ever equals `expected`, the poisoned table
     *    is not actually being used and the two cases above are vacuous. */
    memset((void *)g_crc32_table, POISON_BYTE, sizeof(g_crc32_table));
    g_crc32_ready = BOOT_ENTRIES_CRC32_READY;   /* the old `!flag` guard's view */
    got = crc32_zeroed((const u8 *)SAMPLE, (u32)strlen(SAMPLE), 0u);
    check(got != expected,
          "CONTROL: a poisoned table believed ready yields a WRONG crc");

    /* 4. End to end, and it must fail if the parser-entry crc32_reset() is
     *    DELETED. The first draft of this case seeded 0xAFAFAFAF, which the
     *    exact-match guard rejects on its own, so crc32_zeroed() rebuilt the
     *    table whether or not boot_entries_parse() reset anything -- five green
     *    checks that were all blind to the load-bearing half. The re-adversarial
     *    review caught that. The seed here is instead the STALE-BUT-VALID state:
     *    a poisoned table with the cookie set to the real READY value, which is
     *    exactly what a warm reboot or a chainload from a different loader build
     *    can leave behind. Only the reset at the parser's entry can rescue it. */
    {
        static char json[] =
            "{\"schema_version\":1,\"crc32\":\"0x00000000\",\"entries\":["
            "{\"id\":\"slot-a\",\"title\":\"Slot A\",\"kind\":\"split\","
            "\"flags\":[\"active\"],\"sort_key\":\"00\","
            "\"machine_id\":\"11111111-2222-3333-4444-555555555555\","
            "\"policy_tags\":[],"
            "\"payload\":{\"kernel\":\"\\\\EFI\\\\ImpossibleOS\\\\kernel.exe\"}}"
            "]}";
        static const char HEX[] = "0123456789ABCDEF";
        u32 len = (u32)strlen(json), zero_off = 0, hdr = 0, crc, i;
        boot_entries_parse_result_t r;
        int rc;

        crc32_reset();
        if (!find_crc_field((const u8 *)json, len, &zero_off, &hdr)) {
            check(0, "fixture carries a locatable crc32 field");
            return g_failures ? 1 : 0;
        }
        crc = crc32_zeroed((const u8 *)json, len, zero_off);
        for (i = 0; i < 8u; i++)
            json[zero_off + i] = HEX[(crc >> ((7u - i) * 4u)) & 0xFu];

        /* Prove the seeded state would actually break the parse, so that a
         * pass below means the rebuild happened rather than that the poison
         * was harmless for this particular fixture. */
        memset((void *)g_crc32_table, POISON_BYTE, sizeof(g_crc32_table));
        g_crc32_ready = BOOT_ENTRIES_CRC32_READY;
        check(crc32_zeroed((const u8 *)json, len, zero_off) != crc,
              "CONTROL: the seeded stale-READY table yields a WRONG crc");

        memset((void *)g_crc32_table, POISON_BYTE, sizeof(g_crc32_table));
        g_crc32_ready = BOOT_ENTRIES_CRC32_READY;   /* stale but VALID cookie */
        rc = boot_entries_parse((const unsigned char *)json, len, 0,
                                (boot_entries_log_fn)0, &r);
        check(rc == BOOT_ENTRIES_OK,
              "valid store accepted despite a stale-READY poisoned table");
        check(r.entry_count == 1u, "the accepted store carries its one entry");
    }

    printf("boot-entries parser: %s (%d failure(s))\n",
           g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
