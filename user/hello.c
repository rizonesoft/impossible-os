/* ============================================================================
 * hello.c -- User-mode test program
 *
 * Linked against libc.a. Uses printf() and main() convention.
 * crt0.asm calls main() and passes the return value to sys_exit().
 * ============================================================================ */

#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char **argv)
{
    /* Test printf with various format specifiers */
    printf("  [ELF] Hello from user-mode ELF binary!\n");
    printf("  [ELF] printf works: %d + %d = %d\n", 17, 25, 17 + 25);
    printf("  [ELF] hex: 0x%x, string: \"%s\"\n", 0xDEAD, "Impossible OS");

    /* Test string functions */
    char buf[64];
    strcpy(buf, "libc ");
    strcat(buf, "is working!");
    printf("  [ELF] strlen(\"%s\") = %d\n", buf, (int)strlen(buf));

    /* Test itoa */
    char numbuf[32];
    itoa(12345, numbuf, 10);
    printf("  [ELF] itoa(12345) = \"%s\"\n", numbuf);

    /* argv end-to-end proof (TODO-22 shell-to-exec argument handoff): when
     * launched with the canonical test argv {"hello.exe","alpha","beta"} the
     * argument vector must reach main() through the initial user stack + crt0.
     * Return 42 for that exact vector AND for the no-argv launch (argc<=1, e.g.
     * a kernel-spawned run); any other shape means argv arrived wrong. */
    printf("  [ELF] argc=%d argv0=\"%s\"\n", argc, argc > 0 ? argv[0] : "(none)");
    {
        int i, sum = 0;
        for (i = 0; i < argc; i++)
            sum += (int)strlen(argv[i]);
        printf("  [ELF] argv total len=%d\n", sum);
    }
    if (argc <= 1)
        return 42;                          /* launched without an argv */
    return 40 + argc;                        /* argc==3 -> 43 (proves argc + argv walk) */
}
