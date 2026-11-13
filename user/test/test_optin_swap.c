/* ============================================================================
 * test_optin_swap.c -- opt-in fixture: replace a planned binary in place
 *
 * Runs before test_optin_victim.exe (tests/usermode.manifest order) and
 * flips the LAST byte of C:\test_optin_victim.exe, after the launcher froze
 * that entry's identity at plan time. The loader must then refuse the victim
 * as "planned binary replaced" instead of running it.
 *
 * The flip is derived from the current content (XOR 0x01), so every boot of
 * one image is a genuine mismatch: the file alternates between two states
 * and the freeze always digests the state this run found. The last byte is
 * in the section-header table, which the loader does not use, so a loader
 * that lost the identity check WOULD run the victim and print its marker.
 * Ring 3 has no seek, unlink or rename: the whole file is read and written
 * back over itself at the same length. Driven by scripts/test-identity-path.sh.
 * ============================================================================ */

#include "test.h"
#include "syscall.h"

UTEST_DEFINE_STATE();

/* sys_readfile resolves a name relative to C:\, sys_openfile a full path. */
#define VICTIM_NAME "test_optin_victim.exe"
#define VICTIM "C:\\test_optin_victim.exe"
#define VICTIM_MAX (256u * 1024u)

static unsigned char image[VICTIM_MAX];

int main(void)
{
    long n, w;
    HANDLE h;

    UTEST_BEGIN("test_optin_swap");
    n = sys_readfile(VICTIM_NAME, image, sizeof(image));
    UTEST_ASSERT(n > 0 && (unsigned long)n < sizeof(image), "victim read whole");
    if (n > 0 && (unsigned long)n < sizeof(image)) {
        image[n - 1] ^= 0x01u;
        h = sys_openfile(VICTIM, 0x02u);
        UTEST_ASSERT(h != INVALID_HANDLE_VALUE, "victim opened for write");
        if (h != INVALID_HANDLE_VALUE) {
            w = sys_writehandle(h, image, (uint32_t)n);
            UTEST_ASSERT(w == n, "victim rewritten at the same length");
            sys_closehandle(h);
        }
    }
    UTEST_END();
    return g_fail;
}
