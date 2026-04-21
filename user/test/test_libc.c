/* ============================================================================
 * test_libc.c -- §10 user-mode libc coverage binary
 *
 * Smoke-checks the string + formatting surface that user/include/string.h
 * and user/include/stdio.h promise. Every user binary (cmd.exe,
 * hello.exe, every test_*.exe) links against the same libc.a, so a
 * regression in strlen/strcmp/memcpy/memset/snprintf breaks ring 3
 * wholesale -- this test is the early-warning tripwire for those
 * regressions under the §3 launcher.
 *
 * Linked against the same crt0 + libc as every other user binary. No
 * kernel headers; no malloc; no live syscalls beyond what the
 * UTEST_ASSERT macro itself emits. The six assertions match the
 * libc-test-binary checklist in the 00-infrastructure user-mode
 * test framework TODO one-for-one; each compares a libc call to its
 * documented return semantics so a silent libc drift (wrong length,
 * wrong comparison sign, truncated snprintf) turns into a launcher
 * FAIL instead of propagating silently through every downstream test.
 * ============================================================================ */

#include "test.h"
#include "stdio.h"   /* snprintf */

UTEST_DEFINE_STATE();

int main(void)
{
    UTEST_BEGIN("test_libc");

    /* ---- strlen --------------------------------------------------- */
    UTEST_ASSERT(strlen("hello") == 5,
                 "strlen(\"hello\") == 5");

    /* ---- strcmp: equal + less-than -------------------------------- *
     * strcmp returns 0 for equal, negative when s1 < s2, positive
     * when s1 > s2. Assert the exact value for equality and the
     * sign for the less-than case; the magnitude of a non-zero
     * strcmp return is implementation-defined in the C standard. */
    UTEST_ASSERT(strcmp("abc", "abc") == 0,
                 "strcmp(\"abc\", \"abc\") == 0");
    UTEST_ASSERT(strcmp("abc", "abd") < 0,
                 "strcmp(\"abc\", \"abd\") < 0 (sign-negative)");

    /* ---- memcpy: round-trip --------------------------------------- *
     * Round-trip a 16-byte pattern and verify every byte lands. A
     * partial copy (wrong n, swapped dst/src) would fail memcmp at
     * the first mismatched byte. */
    const char src[16] = {0, 1, 2, 3, 4, 5, 6, 7,
                          8, 9,10,11,12,13,14,15};
    char dst[16];
    memcpy(dst, src, sizeof(dst));
    UTEST_ASSERT(memcmp(dst, src, sizeof(dst)) == 0,
                 "memcpy 16-byte round-trip preserves bytes");

    /* ---- memset: uniform fill ------------------------------------- */
    char fill[32];
    memset(fill, 0x5A, sizeof(fill));
    /* Spot-check both endpoints + the middle to catch a short write
     * or a wrong-length regression. 0x5A is 'Z', a non-zero pattern
     * that would surface if memset silently did a 0-fill. */
    UTEST_ASSERT(fill[0] == (char)0x5A &&
                 fill[15] == (char)0x5A &&
                 fill[31] == (char)0x5A,
                 "memset(buf, 0x5A, 32) fills first, middle, last byte");

    /* ---- snprintf: decimal integer ------------------------------- *
     * The spec wants `snprintf(buf, 32, "%d", 42)` -> "42". snprintf
     * returns the number of chars that WOULD have been written (per
     * C99), not including the NUL. For "42" that's 2; verify both
     * the return count AND the string content so a silent NUL-only
     * or truncated output surfaces. */
    char sbuf[32];
    int n = snprintf(sbuf, sizeof(sbuf), "%d", 42);
    UTEST_ASSERT(n == 2,
                 "snprintf(\"%d\", 42) returns 2");
    UTEST_ASSERT(strcmp(sbuf, "42") == 0,
                 "snprintf(\"%d\", 42) writes \"42\\0\"");

    UTEST_END();
    return g_fail;
}
